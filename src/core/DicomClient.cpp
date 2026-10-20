// =============================================================================
//  DicomClient.cpp — implementazione DCMTK
// =============================================================================
#include "DicomClient.h"
#include "DateUtil.h"
#include "Logger.h"

#include "dcmtk/config/osconfig.h"
#include "dcmtk/dcmdata/dctk.h"
#include "dcmtk/dcmnet/diutil.h"
#include "dcmtk/dcmnet/scu.h"

#include <algorithm>
#include <chrono>
#include <map>

namespace p2p {

namespace {

using Clock = std::chrono::steady_clock;

long elapsedMs(Clock::time_point t0) {
    return static_cast<long>(
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count());
}

std::string getStr(DcmItem& ds, const DcmTagKey& tag) {
    OFString v;
    if (ds.findAndGetOFStringArray(tag, v).good()) return v.c_str();
    return {};
}

int getInt(DcmItem& ds, const DcmTagKey& tag) {
    OFString v;
    if (ds.findAndGetOFString(tag, v).good() && !v.empty()) {
        try { return std::stoi(v.c_str()); } catch (...) {}
    }
    return -1;
}

// "COGNOME^NOME" -> "COGNOME NOME"
std::string prettyName(std::string pn) {
    std::replace(pn.begin(), pn.end(), '^', ' ');
    while (!pn.empty() && pn.back() == ' ') pn.pop_back();
    return pn;
}

// DcmSCU con gestione personalizzata delle risposte C-MOVE:
// aggiorna l'avanzamento e invia C-CANCEL quando richiesto dall'utente.
class MoveSCU : public DcmSCU {
public:
    ProgressFn               onProgress;
    const std::atomic<bool>* cancel     = nullptr;
    bool                     cancelSent = false;
    MoveProgress             last;
    int                      responses  = 0;

protected:
    OFCondition handleMOVEResponse(const T_ASC_PresentationContextID presID,
                                   RetrieveResponse* r, OFBool& waitForNextResponse) override {
        if (r == nullptr) return DIMSE_NULLKEY;
        ++responses;
        last.status    = r->m_status;
        last.remaining = r->m_numberOfRemainingSubops;
        last.completed = r->m_numberOfCompletedSubops;
        last.failed    = r->m_numberOfFailedSubops;
        last.warning   = r->m_numberOfWarningSubops;

        waitForNextResponse =
            (r->m_status == dimse::Pending || r->m_status == dimse::PendingWarning);

        if (onProgress) onProgress(last);

        if (waitForNextResponse && cancel && cancel->load() && !cancelSent) {
            logWarn("Invio C-CANCEL al PACS sorgente");
            sendCANCELRequest(presID);
            cancelSent = true;
        }
        return EC_Normal;
    }
};

// Chiude l'associazione in ogni caso all'uscita dallo scope
struct AssocGuard {
    DcmSCU& scu;
    ~AssocGuard() {
        if (scu.isConnected()) scu.releaseAssociation();
    }
};

} // namespace

// -----------------------------------------------------------------------------
DicomClient::DicomClient(NodeConfig local, NetSettings net)
    : local_(std::move(local)), net_(net) {}

std::string DicomClient::statusText(uint16_t s) {
    char hex[8];
    std::snprintf(hex, sizeof hex, "%04X", s);
    std::string h = std::string("0x") + hex;
    switch (s) {
        case dimse::Success:        return h + " Successo";
        case dimse::Pending:        return h + " In corso";
        case dimse::PendingWarning: return h + " In corso (avviso)";
        case dimse::Cancel:         return h + " Annullato";
        case dimse::SubOpsWarning:  return h + " Avviso: alcune sotto-operazioni fallite";
        case dimse::OutOfResources1:return h + " Risorse esaurite (numero di corrispondenze)";
        case dimse::OutOfResources2:return h + " Risorse esaurite (sotto-operazioni)";
        case dimse::DestUnknown:    return h + " Destinazione C-MOVE sconosciuta al PACS sorgente";
        case dimse::IdentifierError:return h + " Identificatore non conforme alla SOP Class";
        case dimse::NoStatus:       return "nessuna risposta";
        default:
            if ((s & 0xF000) == 0xC000) return h + " Impossibile elaborare la richiesta";
            return h;
    }
}

bool DicomClient::connect(DcmSCU& scu, const NodeConfig& peer, const char* abstractSyntax,
                          int dimseTimeoutSec, std::string& err) const {
    scu.setAETitle(local_.aet.c_str());
    scu.setPeerHostName(peer.host.c_str());
    scu.setPeerPort(static_cast<Uint16>(peer.port));
    scu.setPeerAETitle(peer.aet.c_str());
    scu.setMaxReceivePDULength(static_cast<Uint32>(net_.maxPdu));
    scu.setConnectionTimeout(net_.connectTimeoutSec);
    scu.setACSETimeout(static_cast<Uint32>(net_.acseTimeoutSec));
    scu.setDIMSEBlockingMode(DIMSE_NONBLOCKING);
    scu.setDIMSETimeout(static_cast<Uint32>(dimseTimeoutSec));

    OFList<OFString> ts;
    ts.push_back(UID_LittleEndianExplicitTransferSyntax);
    ts.push_back(UID_LittleEndianImplicitTransferSyntax);
    scu.addPresentationContext(abstractSyntax, ts);

    OFCondition c = scu.initNetwork();
    if (c.bad()) {
        err = cat("initNetwork: ", c.text());
        return false;
    }
    c = scu.negotiateAssociation();
    if (c.bad()) {
        err = cat("associazione con ", peer.aet, "@", peer.host, ":", peer.port,
                  " fallita: ", c.text());
        return false;
    }
    if (scu.findPresentationContextID(abstractSyntax, "") == 0) {
        err = cat(peer.aet, " ha rifiutato il servizio richiesto (", abstractSyntax, ")");
        scu.releaseAssociation();
        return false;
    }
    return true;
}

// -----------------------------------------------------------------------------
EchoResult DicomClient::echo(const NodeConfig& peer) const {
    EchoResult res;
    const auto t0 = Clock::now();
    DcmSCU scu;
    AssocGuard guard{scu};
    std::string err;
    if (!connect(scu, peer, UID_VerificationSOPClass, net_.findTimeoutSec, err)) {
        res.message = err;
        res.ms      = elapsedMs(t0);
        return res;
    }
    OFCondition c = scu.sendECHORequest(0);
    res.ms = elapsedMs(t0);
    if (c.good()) {
        res.ok      = true;
        res.message = cat("C-ECHO riuscito (", res.ms, " ms)");
    } else {
        res.message = cat("C-ECHO fallito: ", c.text());
    }
    return res;
}

// -----------------------------------------------------------------------------
bool DicomClient::cfind(const NodeConfig& peer, DcmDataset& keys,
                        std::vector<std::unique_ptr<DcmDataset>>& out,
                        std::string& err) const {
    DcmSCU scu;
    AssocGuard guard{scu};
    const char* sop = UID_FINDStudyRootQueryRetrieveInformationModel;
    if (!connect(scu, peer, sop, net_.findTimeoutSec, err)) return false;

    const T_ASC_PresentationContextID pc = scu.findPresentationContextID(sop, "");
    OFList<QRResponse*> responses;
    OFCondition c = scu.sendFINDRequest(pc, &keys, &responses);

    uint16_t finalStatus = dimse::NoStatus;
    for (OFListIterator(QRResponse*) it = responses.begin(); it != responses.end(); ++it) {
        QRResponse* r = *it;
        if (r->m_dataset) out.push_back(std::make_unique<DcmDataset>(*r->m_dataset));
        finalStatus = r->m_status;
        delete r;
    }
    if (c.bad()) {
        err = cat("C-FIND su ", peer.aet, ": ", c.text());
        return false;
    }
    if (finalStatus != dimse::Success && finalStatus != dimse::Pending &&
        finalStatus != dimse::PendingWarning) {
        err = cat("C-FIND su ", peer.aet, ": ", statusText(finalStatus));
        return false;
    }
    return true;
}

bool DicomClient::findStudies(const NodeConfig& peer, const std::string& dateRange,
                              const std::string& modality, std::vector<StudyInfo>& out,
                              std::string& err) const {
    DcmDataset keys;
    keys.putAndInsertString(DCM_QueryRetrieveLevel, "STUDY");
    keys.putAndInsertString(DCM_SpecificCharacterSet, "");
    keys.putAndInsertString(DCM_StudyInstanceUID, "");
    keys.putAndInsertString(DCM_StudyDate, dateRange.c_str());
    keys.putAndInsertString(DCM_StudyTime, "");
    keys.putAndInsertString(DCM_PatientName, "");
    keys.putAndInsertString(DCM_PatientID, "");
    keys.putAndInsertString(DCM_AccessionNumber, "");
    keys.putAndInsertString(DCM_StudyDescription, "");
    keys.putAndInsertString(DCM_ModalitiesInStudy, modality.c_str());
    keys.putAndInsertString(DCM_NumberOfStudyRelatedInstances, "");
    keys.putAndInsertString(DCM_NumberOfStudyRelatedSeries, "");

    std::vector<std::unique_ptr<DcmDataset>> rs;
    if (!cfind(peer, keys, rs, err)) return false;

    for (auto& ds : rs) {
        ds->convertToUTF8();   // nomi con accenti (ISO_IR 100 ecc.) -> UTF-8 per ImGui
        StudyInfo s;
        s.uid          = getStr(*ds, DCM_StudyInstanceUID);
        if (s.uid.empty()) continue;
        s.patientId    = getStr(*ds, DCM_PatientID);
        s.patientName  = prettyName(getStr(*ds, DCM_PatientName));
        s.studyDate    = getStr(*ds, DCM_StudyDate);
        s.studyTime    = getStr(*ds, DCM_StudyTime);
        s.accession    = getStr(*ds, DCM_AccessionNumber);
        s.description  = getStr(*ds, DCM_StudyDescription);
        s.modalities   = getStr(*ds, DCM_ModalitiesInStudy);
        s.numInstances = getInt(*ds, DCM_NumberOfStudyRelatedInstances);
        s.numSeries    = getInt(*ds, DCM_NumberOfStudyRelatedSeries);
        out.push_back(std::move(s));
    }
    return true;
}

bool DicomClient::findSeries(const NodeConfig& peer, const std::string& studyUid,
                             std::vector<SeriesInfo>& out, std::string& err) const {
    DcmDataset keys;
    keys.putAndInsertString(DCM_QueryRetrieveLevel, "SERIES");
    keys.putAndInsertString(DCM_StudyInstanceUID, studyUid.c_str());
    keys.putAndInsertString(DCM_SeriesInstanceUID, "");
    keys.putAndInsertString(DCM_Modality, "");
    keys.putAndInsertString(DCM_SeriesDescription, "");
    keys.putAndInsertString(DCM_NumberOfSeriesRelatedInstances, "");

    std::vector<std::unique_ptr<DcmDataset>> rs;
    if (!cfind(peer, keys, rs, err)) return false;
    for (auto& ds : rs) {
        SeriesInfo s;
        s.uid          = getStr(*ds, DCM_SeriesInstanceUID);
        s.modality     = getStr(*ds, DCM_Modality);
        s.description  = getStr(*ds, DCM_SeriesDescription);
        s.numInstances = getInt(*ds, DCM_NumberOfSeriesRelatedInstances);
        if (!s.uid.empty()) out.push_back(std::move(s));
    }
    return true;
}

bool DicomClient::findInstances(const NodeConfig& peer, const std::string& studyUid,
                                const std::string& seriesUid,
                                std::vector<std::string>& sopUids, std::string& err) const {
    DcmDataset keys;
    keys.putAndInsertString(DCM_QueryRetrieveLevel, "IMAGE");
    keys.putAndInsertString(DCM_StudyInstanceUID, studyUid.c_str());
    keys.putAndInsertString(DCM_SeriesInstanceUID, seriesUid.c_str());
    keys.putAndInsertString(DCM_SOPInstanceUID, "");

    std::vector<std::unique_ptr<DcmDataset>> rs;
    if (!cfind(peer, keys, rs, err)) return false;
    for (auto& ds : rs) {
        std::string u = getStr(*ds, DCM_SOPInstanceUID);
        if (!u.empty()) sopUids.push_back(std::move(u));
    }
    return true;
}

int DicomClient::countStudyInstances(const NodeConfig& peer, const std::string& studyUid,
                                     std::string& err) const {
    // 1) attributo NumberOfStudyRelatedInstances a livello STUDY
    DcmDataset keys;
    keys.putAndInsertString(DCM_QueryRetrieveLevel, "STUDY");
    keys.putAndInsertString(DCM_StudyInstanceUID, studyUid.c_str());
    keys.putAndInsertString(DCM_NumberOfStudyRelatedInstances, "");
    std::vector<std::unique_ptr<DcmDataset>> rs;
    if (!cfind(peer, keys, rs, err)) return -1;
    if (rs.empty()) return 0;                       // studio assente
    const int n = getInt(*rs.front(), DCM_NumberOfStudyRelatedInstances);
    if (n >= 0) return n;

    // 2) somma di NumberOfSeriesRelatedInstances
    std::vector<SeriesInfo> series;
    if (!findSeries(peer, studyUid, series, err)) return -1;
    int  sum      = 0;
    bool complete = true;
    for (const auto& s : series) {
        if (s.numInstances < 0) { complete = false; break; }
        sum += s.numInstances;
    }
    if (complete) return sum;

    // 3) conteggio esplicito a livello IMAGE (più lento, ma universale)
    sum = 0;
    for (const auto& s : series) {
        std::vector<std::string> sops;
        if (!findInstances(peer, studyUid, s.uid, sops, err)) return -1;
        sum += static_cast<int>(sops.size());
    }
    return sum;
}

// -----------------------------------------------------------------------------
MoveResult DicomClient::cmove(const NodeConfig& src, const std::string& destAet,
                              DcmDataset& keys, const ProgressFn& onProgress,
                              const std::atomic<bool>* cancel) const {
    MoveResult res;
    const auto t0 = Clock::now();
    MoveSCU scu;
    scu.onProgress = onProgress;
    scu.cancel     = cancel;
    AssocGuard guard{scu};

    const char* sop = UID_MOVEStudyRootQueryRetrieveInformationModel;
    if (!connect(scu, src, sop, net_.moveTimeoutSec, res.error)) {
        res.ms = elapsedMs(t0);
        return res;
    }
    res.associationOk = true;

    const T_ASC_PresentationContextID pc = scu.findPresentationContextID(sop, "");
    OFCondition c = scu.sendMOVERequest(pc, destAet.c_str(), &keys, nullptr);

    res.ms        = elapsedMs(t0);
    res.responses = scu.responses;
    res.status    = scu.last.status;
    res.completed = scu.last.completed;
    res.failed    = scu.last.failed;
    res.warning   = scu.last.warning;
    res.cancelled = scu.cancelSent || res.status == dimse::Cancel;
    if (c.bad()) {
        res.error = cat("C-MOVE: ", c.text());
        if (c == DIMSE_NODATAAVAILABLE)
            res.error += cat(" (nessuna risposta entro ", net_.moveTimeoutSec,
                             " s: aumentare il timeout C-MOVE)");
    } else if (res.status != dimse::Success && res.status != dimse::SubOpsWarning) {
        res.error = statusText(res.status);
    }
    return res;
}

MoveResult DicomClient::moveStudy(const NodeConfig& src, const std::string& destAet,
                                  const std::string& studyUid, const ProgressFn& onProgress,
                                  const std::atomic<bool>* cancel) const {
    DcmDataset keys;
    keys.putAndInsertString(DCM_QueryRetrieveLevel, "STUDY");
    keys.putAndInsertString(DCM_StudyInstanceUID, studyUid.c_str());
    return cmove(src, destAet, keys, onProgress, cancel);
}

MoveResult DicomClient::moveSeries(const NodeConfig& src, const std::string& destAet,
                                   const std::string& studyUid, const std::string& seriesUid,
                                   const ProgressFn& onProgress,
                                   const std::atomic<bool>* cancel) const {
    DcmDataset keys;
    keys.putAndInsertString(DCM_QueryRetrieveLevel, "SERIES");
    keys.putAndInsertString(DCM_StudyInstanceUID, studyUid.c_str());
    keys.putAndInsertString(DCM_SeriesInstanceUID, seriesUid.c_str());
    return cmove(src, destAet, keys, onProgress, cancel);
}

bool DicomClient::findIncompleteSeries(const NodeConfig& src, const NodeConfig& dst,
                                       const std::string& studyUid,
                                       std::vector<std::string>& out, int& missingImages,
                                       std::string& err) const {
    out.clear();
    missingImages = 0;
    std::vector<SeriesInfo> onA, onB;
    if (!findSeries(src, studyUid, onA, err)) return false;
    if (!findSeries(dst, studyUid, onB, err)) return false;

    // conteggio di una serie: attributo NumberOfSeriesRelatedInstances o C-FIND IMAGE
    auto count = [&](const NodeConfig& node, const SeriesInfo& se, bool& ok) {
        if (se.numInstances >= 0) return se.numInstances;
        std::vector<std::string> sops;
        ok = findInstances(node, studyUid, se.uid, sops, err) && ok;
        return static_cast<int>(sops.size());
    };
    std::map<std::string, const SeriesInfo*> bByUid;
    for (const auto& se : onB) bByUid[se.uid] = &se;

    bool ok = true;
    for (const auto& se : onA) {
        const int nA = count(src, se, ok);
        auto it = bByUid.find(se.uid);
        const int nB = it == bByUid.end() ? 0 : count(dst, *it->second, ok);
        if (nB < nA) {
            out.push_back(se.uid);
            missingImages += nA - nB;
        }
    }
    return ok;
}

MoveResult DicomClient::moveInstance(const NodeConfig& src, const std::string& destAet,
                                     const std::string& studyUid, const std::string& seriesUid,
                                     const std::string& sopUid) const {
    DcmDataset keys;
    keys.putAndInsertString(DCM_QueryRetrieveLevel, "IMAGE");
    keys.putAndInsertString(DCM_StudyInstanceUID, studyUid.c_str());
    keys.putAndInsertString(DCM_SeriesInstanceUID, seriesUid.c_str());
    keys.putAndInsertString(DCM_SOPInstanceUID, sopUid.c_str());
    return cmove(src, destAet, keys, nullptr, nullptr);
}

// -----------------------------------------------------------------------------
ProbeResult DicomClient::probeMoveDestination(const NodeConfig& src,
                                              const std::string& destAet) const {
    ProbeResult pr;
    char uid[100];
    dcmGenerateUniqueIdentifier(uid, SITE_STUDY_UID_ROOT);   // UID certamente inesistente

    DcmDataset keys;
    keys.putAndInsertString(DCM_QueryRetrieveLevel, "STUDY");
    keys.putAndInsertString(DCM_StudyInstanceUID, uid);
    MoveResult r = cmove(src, destAet, keys, nullptr, nullptr);

    if (!r.associationOk) {
        pr.verdict = ProbeResult::Verdict::Error;
        pr.message = r.error;
    } else if (r.status == dimse::DestUnknown) {
        pr.verdict = ProbeResult::Verdict::DestUnknown;
        pr.message = cat(src.aet, " NON conosce la destinazione '", destAet,
                         "': aggiungerla alla tabella dei nodi remoti di A.");
    } else if (r.status == dimse::Success || r.status == dimse::SubOpsWarning) {
        // Molti PACS (dcm4chee) validano la destinazione prima della ricerca, altri
        // (es. dcmqrscp) solo se trovano immagini: il successo non è una prova definitiva.
        pr.verdict = ProbeResult::Verdict::Indeterminate;
        pr.message = cat(src.aet, " non ha rifiutato '", destAet,
                         "' come destinazione (nessun dato inviato): routing probabilmente "
                         "corretto. Alcuni PACS verificano la destinazione solo quando ci sono "
                         "immagini da inviare: conferma definitiva con il Test reale.");
    } else {
        pr.verdict = ProbeResult::Verdict::Indeterminate;
        pr.message = cat("Risposta non conclusiva: ", statusText(r.status),
                         r.error.empty() ? "" : " — ", r.error,
                         ". Alcuni PACS rifiutano subito un UID inesistente: usare il Test reale.");
    }
    return pr;
}

ProbeResult DicomClient::testRealTransfer(const NodeConfig& src, const NodeConfig& dst,
                                          const StepFn& step) const {
    ProbeResult pr;
    auto say = [&](const std::string& s) { if (step) step(s); logInfo("[Test reale] ", s); };

    // 1) cerca uno studio recente su A (7, 30, 365 giorni)
    std::vector<StudyInfo> studies;
    std::string err;
    const Ymd today = Ymd::today();
    for (int days : {7, 30, 365}) {
        const std::string range = today.addDays(-days).dicom() + "-" + today.dicom();
        say(cat("Cerco uno studio su ", src.aet, " negli ultimi ", days, " giorni..."));
        studies.clear();
        if (!findStudies(src, range, "", studies, err)) {
            pr.message = err;
            return pr;
        }
        if (!studies.empty()) break;
    }
    if (studies.empty()) {
        pr.verdict = ProbeResult::Verdict::Indeterminate;
        pr.message = "Nessuno studio trovato su A nell'ultimo anno: impossibile eseguire il test.";
        return pr;
    }
    // preferisce lo studio più piccolo con conteggio noto
    std::sort(studies.begin(), studies.end(), [](const StudyInfo& x, const StudyInfo& y) {
        auto key = [](const StudyInfo& s) { return s.numInstances > 0 ? s.numInstances : 1 << 30; };
        return key(x) < key(y);
    });
    const StudyInfo& st = studies.front();

    // 2) serie più piccola
    std::vector<SeriesInfo> series;
    if (!findSeries(src, st.uid, series, err) || series.empty()) {
        pr.message = err.empty() ? "Lo studio non contiene serie" : err;
        return pr;
    }
    std::sort(series.begin(), series.end(), [](const SeriesInfo& x, const SeriesInfo& y) {
        auto key = [](const SeriesInfo& s) { return s.numInstances > 0 ? s.numInstances : 1 << 30; };
        return key(x) < key(y);
    });
    const SeriesInfo& se = series.front();

    // 3) prima immagine della serie
    std::vector<std::string> sops;
    if (!findInstances(src, st.uid, se.uid, sops, err) || sops.empty()) {
        pr.message = err.empty() ? "La serie non contiene immagini" : err;
        return pr;
    }
    const std::string& sop = sops.front();
    say(cat("Immagine scelta: studio del ", dicomDateToDisplay(st.studyDate), ", serie ",
            se.modality, " '", se.description, "'"));

    // 4) C-MOVE della singola immagine
    say(cat("C-MOVE di 1 immagine da ", src.aet, " verso ", dst.aet, "..."));
    MoveResult mr = moveInstance(src, dst.aet, st.uid, se.uid, sop);
    if (!mr.associationOk) {
        pr.message = mr.error;
        return pr;
    }
    if (mr.status == dimse::DestUnknown) {
        pr.verdict = ProbeResult::Verdict::DestUnknown;
        pr.message = statusText(mr.status);
        return pr;
    }
    if (mr.status != dimse::Success || mr.completed < 1) {
        pr.message = cat("C-MOVE non riuscito: ", statusText(mr.status),
                         " (completate ", mr.completed, ", fallite ", mr.failed,
                         "). A conosce B ma non riesce a inviare: controllare rete/porta/AE di B.");
        return pr;
    }

    // 5) verifica presenza su B
    say(cat("Verifico la presenza dell'immagine su ", dst.aet, "..."));
    DcmDataset keys;
    keys.putAndInsertString(DCM_QueryRetrieveLevel, "IMAGE");
    keys.putAndInsertString(DCM_StudyInstanceUID, st.uid.c_str());
    keys.putAndInsertString(DCM_SeriesInstanceUID, se.uid.c_str());
    keys.putAndInsertString(DCM_SOPInstanceUID, sop.c_str());
    std::vector<std::unique_ptr<DcmDataset>> rs;
    if (!cfind(dst, keys, rs, err)) {
        pr.verdict = ProbeResult::Verdict::Indeterminate;
        pr.message = cat("A ha inviato l'immagine (completed=1) ma la verifica su B è fallita: ", err);
        return pr;
    }
    if (rs.empty()) {
        pr.verdict = ProbeResult::Verdict::Indeterminate;
        pr.message = "A dichiara l'invio riuscito ma B non restituisce l'immagine al C-FIND "
                     "(indicizzazione ritardata? riprovare tra qualche secondo).";
        return pr;
    }
    pr.verdict = ProbeResult::Verdict::Ok;
    pr.message = cat("A e B si vedono: 1 immagine trasferita e ritrovata su ", dst.aet,
                     " in ", mr.ms, " ms.");
    return pr;
}

} // namespace p2p
