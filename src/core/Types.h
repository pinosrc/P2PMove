// =============================================================================
//  P2PMove — trasferimento esami DICOM PACS→PACS tramite C-MOVE delegato
//  Types.h : strutture dati condivise tra core e interfaccia
// =============================================================================
#pragma once

#include <cstdint>
#include <string>

namespace p2p {

// Tipologia del PACS: usata solo per mostrare suggerimenti di configurazione,
// il protocollo DIMSE è identico per tutti.
enum class PacsType : int { Dcm4chee2 = 0, Dcm4cheeArc, Orthanc, Horos, Altro, Count };

const char* pacsTypeName(int type);
const char* pacsTypeHint(int type);   // suggerimenti per far funzionare il C-MOVE

// Un nodo DICOM (Application Entity)
struct NodeConfig {
    std::string name;            // etichetta libera (es. "PACS_A")
    std::string aet;             // AE Title (max 16 caratteri)
    std::string host;            // IP o hostname
    int port = 11112;
    int type = static_cast<int>(PacsType::Altro);

    // true se i campi sono utilizzabili; in caso contrario 'why' spiega il motivo
    bool valid(std::string* why = nullptr, bool needHost = true) const;
};

// Parametri di rete DIMSE
struct NetSettings {
    int connectTimeoutSec = 10;    // apertura TCP
    int acseTimeoutSec    = 30;    // negoziazione associazione
    int findTimeoutSec    = 120;   // attesa tra risposte C-FIND
    int moveTimeoutSec    = 3600;  // attesa tra risposte C-MOVE (alcuni PACS non inviano "pending")
    int maxPdu            = 116794;
};

// Parametri del trasferimento
struct TransferSettings {
    int  workers          = 1;     // 1 = sincrono, >1 = asincrono (associazioni parallele)
    int  maxRetries       = 2;     // tentativi automatici aggiuntivi nella stessa sessione
    int  retryDelaySec    = 10;
    bool verifyOnB        = true;  // dopo il C-MOVE conta le immagini su B (C-FIND)
    bool skipIfPresentOnB = false; // prima del C-MOVE salta gli studi già completi su B
    bool includeFailed    = true;  // rimette in coda anche FALLITI/PARZIALI
    bool dayByDayQuery    = true;  // C-FIND su A un giorno alla volta (evita limiti sui risultati)
};

struct AppConfig {
    NodeConfig       local;        // questa applicazione (solo AE Title)
    NodeConfig       a;            // PACS sorgente
    NodeConfig       b;            // PACS destinazione
    NetSettings      net;
    TransferSettings xfer;
    bool             logAnonymize = false;   // nel registro: UID dello studio al posto del nome paziente

    // chiave che identifica la coppia sorgente→destinazione nel database
    std::string route() const { return a.aet + ">" + b.aet; }

    // riporta nei limiti consentiti i parametri numerici (valori da DB/CLI non fidati)
    void sanitize();
};

// Dati di uno studio restituiti dal C-FIND
struct StudyInfo {
    std::string uid;
    std::string patientId;
    std::string patientName;
    std::string studyDate;     // AAAAMMGG
    std::string studyTime;
    std::string accession;
    std::string description;
    std::string modalities;    // es. "CT\SR"
    int numInstances = -1;     // -1 = non comunicato dal PACS
    int numSeries    = -1;
};

// Stati di uno studio nella coda persistente
enum class StudyStatus : int { Pending = 0, Running, Done, Partial, Failed, Skipped, Count };

const char*  statusCode(StudyStatus s);   // valore salvato nel DB ("PENDING", ...)
const char*  statusLabel(StudyStatus s);  // etichetta italiana per l'interfaccia
StudyStatus  statusFromCode(const std::string& code);

// Riga della tabella "studies"
struct StudyRow : StudyInfo {
    int         instancesB = -1;
    StudyStatus status     = StudyStatus::Pending;
    int         attempts   = 0;
    int         completed  = 0;
    int         failed     = 0;
    int         warning    = 0;
    int         lastDimse  = 0;
    std::string lastError;
    std::string updatedAt;
};

} // namespace p2p
