// =============================================================================
//  Types.cpp
// =============================================================================
#include "Types.h"

namespace p2p {

const char* pacsTypeName(int type) {
    switch (static_cast<PacsType>(type)) {
        case PacsType::Dcm4chee2:   return "dcm4chee 2.18";
        case PacsType::Dcm4cheeArc: return "dcm4chee-arc (light)";
        case PacsType::Orthanc:     return "Orthanc";
        case PacsType::Horos:       return "Horos";
        default:                    return "Altro";
    }
}

const char* pacsTypeHint(int type) {
    switch (static_cast<PacsType>(type)) {
        case PacsType::Dcm4chee2:
            return "dcm4chee 2.18: la destinazione deve essere censita in 'AE Management' "
                   "(web console JMX/AE) con AE Title, host e porta. Il servizio "
                   "MoveScu deve poter raggiungere B. Per default accetta qualsiasi "
                   "Calling AE (CallingAETitles=ANY).";
        case PacsType::Dcm4cheeArc:
            return "dcm4chee-arc: la destinazione va aggiunta come 'AE' remoto "
                   "(Configurazione > AE list). Per default accetta qualsiasi Calling AE "
                   "salvo restrizioni su 'Accepted Calling AE Titles'. Su studi grandi "
                   "il C-MOVE può non inviare risposte 'pending': alzare il timeout C-MOVE.";
        case PacsType::Orthanc:
            return "Orthanc: B deve essere in \"DicomModalities\". Il C-MOVE da un AE "
                   "non censito richiede \"DicomAlwaysAllowMove\": true oppure l'AE di "
                   "questa app in DicomModalities. Per Orthanc la sonda di routing può "
                   "dare esito indeterminato: usare il 'Test reale'.";
        case PacsType::Horos:
            return "Horos: attivare il Listener (Preferenze > Listener) e aggiungere B "
                   "in Preferenze > Locations. Verificare che il Q/R SCP accetti C-MOVE. "
                   "Horos spesso non invia conteggi intermedi: la barra immagini avanza "
                   "solo a fine studio.";
        default:
            return "Il nodo deve conoscere l'AE Title, l'IP e la porta di B nella sua "
                   "tabella dei nodi remoti.";
    }
}

bool NodeConfig::valid(std::string* why, bool needHost) const {
    auto fail = [&](const char* m) { if (why) *why = m; return false; };
    if (aet.empty())        return fail("AE Title mancante");
    if (aet.size() > 16)    return fail("AE Title oltre 16 caratteri");
    if (needHost) {
        if (host.empty())                 return fail("host mancante");
        if (port <= 0 || port > 65535)    return fail("porta non valida");
    }
    if (why) why->clear();
    return true;
}

void AppConfig::sanitize() {
    auto cl = [](int& v, int lo, int hi) { v = v < lo ? lo : v > hi ? hi : v; };
    cl(net.connectTimeoutSec, 1, 300);
    cl(net.acseTimeoutSec, 1, 300);
    cl(net.findTimeoutSec, 5, 3600);
    cl(net.moveTimeoutSec, 30, 86400);
    cl(net.maxPdu, 4096, 131072);
    cl(xfer.workers, 1, 16);
    cl(xfer.maxRetries, 0, 10);
    cl(xfer.retryDelaySec, 0, 600);
}

const char* statusCode(StudyStatus s) {
    switch (s) {
        case StudyStatus::Pending: return "PENDING";
        case StudyStatus::Running: return "RUNNING";
        case StudyStatus::Done:    return "DONE";
        case StudyStatus::Partial: return "PARTIAL";
        case StudyStatus::Failed:  return "FAILED";
        case StudyStatus::Skipped: return "SKIPPED";
        default:                   return "PENDING";
    }
}

const char* statusLabel(StudyStatus s) {
    switch (s) {
        case StudyStatus::Pending: return "In coda";
        case StudyStatus::Running: return "In corso";
        case StudyStatus::Done:    return "Completato";
        case StudyStatus::Partial: return "Parziale";
        case StudyStatus::Failed:  return "Fallito";
        case StudyStatus::Skipped: return "Già su B";
        default:                   return "?";
    }
}

StudyStatus statusFromCode(const std::string& c) {
    for (int i = 0; i < static_cast<int>(StudyStatus::Count); ++i)
        if (c == statusCode(static_cast<StudyStatus>(i))) return static_cast<StudyStatus>(i);
    return StudyStatus::Pending;
}

} // namespace p2p
