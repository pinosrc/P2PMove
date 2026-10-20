// =============================================================================
//  DicomClient.h — operazioni DIMSE lato SCU basate su DCMTK (DcmSCU)
//
//  Ogni metodo apre e chiude la propria associazione: la classe è quindi
//  utilizzabile da più thread contemporaneamente (un'istanza DcmSCU per chiamata).
// =============================================================================
#pragma once

#include "Types.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class DcmDataset;
class DcmSCU;
class OFCondition;

namespace p2p {

// Codici di stato DIMSE usati dall'applicazione (PS3.4 Annex C)
namespace dimse {
constexpr uint16_t Success         = 0x0000;
constexpr uint16_t Pending         = 0xFF00;
constexpr uint16_t PendingWarning  = 0xFF01;
constexpr uint16_t Cancel          = 0xFE00;
constexpr uint16_t SubOpsWarning   = 0xB000;
constexpr uint16_t OutOfResources1 = 0xA701;
constexpr uint16_t OutOfResources2 = 0xA702;
constexpr uint16_t DestUnknown     = 0xA801;
constexpr uint16_t IdentifierError = 0xA900;
constexpr uint16_t NoStatus        = 0xFFFF;   // nessuna risposta ricevuta (interno)
} // namespace dimse

struct EchoResult {
    bool        ok = false;
    std::string message;
    long        ms = 0;
};

struct MoveProgress {
    uint16_t status    = dimse::NoStatus;
    int      remaining = -1;
    int      completed = 0;
    int      failed    = 0;
    int      warning   = 0;
};

struct MoveResult {
    bool        associationOk = false;   // associazione stabilita e richiesta inviata
    bool        cancelled     = false;
    uint16_t    status        = dimse::NoStatus;
    int         completed     = 0;
    int         failed        = 0;
    int         warning       = 0;
    int         responses     = 0;
    std::string error;
    long        ms            = 0;
};

struct ProbeResult {
    enum class Verdict { Ok, DestUnknown, Indeterminate, Error } verdict = Verdict::Error;
    std::string message;
};

struct SeriesInfo {
    std::string uid;
    std::string modality;
    std::string description;
    int         numInstances = -1;
};

using ProgressFn = std::function<void(const MoveProgress&)>;
using StepFn     = std::function<void(const std::string&)>;

class DicomClient {
public:
    DicomClient(NodeConfig local, NetSettings net);

    // C-ECHO verso un nodo
    EchoResult echo(const NodeConfig& peer) const;

    // C-FIND a livello STUDY; dateRange in formato DICOM ("AAAAMMGG" o "AAAAMMGG-AAAAMMGG")
    bool findStudies(const NodeConfig& peer, const std::string& dateRange,
                     const std::string& modality, std::vector<StudyInfo>& out,
                     std::string& err) const;

    bool findSeries(const NodeConfig& peer, const std::string& studyUid,
                    std::vector<SeriesInfo>& out, std::string& err) const;

    bool findInstances(const NodeConfig& peer, const std::string& studyUid,
                       const std::string& seriesUid, std::vector<std::string>& sopUids,
                       std::string& err) const;

    // Numero di immagini di uno studio su un nodo:
    //   >=0 conteggio, 0 = studio assente, -1 = impossibile determinarlo (err valorizzato)
    int countStudyInstances(const NodeConfig& peer, const std::string& studyUid,
                            std::string& err) const;

    // C-MOVE di uno studio intero da 'src' verso 'destAet'
    MoveResult moveStudy(const NodeConfig& src, const std::string& destAet,
                         const std::string& studyUid, const ProgressFn& onProgress,
                         const std::atomic<bool>* cancel) const;

    // C-MOVE di una sola serie (usato nei ritentativi degli studi parziali)
    MoveResult moveSeries(const NodeConfig& src, const std::string& destAet,
                          const std::string& studyUid, const std::string& seriesUid,
                          const ProgressFn& onProgress, const std::atomic<bool>* cancel) const;

    // Serie di uno studio presenti su 'src' con meno immagini su 'dst' (o assenti).
    // 'missingImages' = totale immagini mancanti stimate.
    bool findIncompleteSeries(const NodeConfig& src, const NodeConfig& dst,
                              const std::string& studyUid, std::vector<std::string>& series,
                              int& missingImages, std::string& err) const;

    // C-MOVE di una singola immagine (usato dal test reale)
    MoveResult moveInstance(const NodeConfig& src, const std::string& destAet,
                            const std::string& studyUid, const std::string& seriesUid,
                            const std::string& sopUid) const;

    // Sonda: C-MOVE di uno StudyInstanceUID inesistente. Se A risponde 0xA801
    // non conosce la destinazione; se risponde 0x0000 la conosce (nessun dato inviato).
    ProbeResult probeMoveDestination(const NodeConfig& src, const std::string& destAet) const;

    // Test definitivo: sposta UNA immagine reale da A a B e la ricerca su B.
    ProbeResult testRealTransfer(const NodeConfig& src, const NodeConfig& dst,
                                 const StepFn& step) const;

    static std::string statusText(uint16_t status);

private:
    bool connect(DcmSCU& scu, const NodeConfig& peer, const char* abstractSyntax,
                 int dimseTimeoutSec, std::string& err) const;
    bool cfind(const NodeConfig& peer, DcmDataset& keys,
               std::vector<std::unique_ptr<DcmDataset>>& out, std::string& err) const;
    MoveResult cmove(const NodeConfig& src, const std::string& destAet, DcmDataset& keys,
                     const ProgressFn& onProgress, const std::atomic<bool>* cancel) const;

    NodeConfig  local_;
    NetSettings net_;
};

} // namespace p2p
