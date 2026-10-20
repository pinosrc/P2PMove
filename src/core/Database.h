// =============================================================================
//  Database.h — persistenza SQLite: configurazione, coda studi, storico tentativi
//
//  Una sola connessione protetta da mutex: SQLite è abbondantemente veloce per
//  il volume di scritture di un trasferimento (qualche riga al secondo).
// =============================================================================
#pragma once

#include "Types.h"

#include <array>
#include <map>
#include <mutex>
#include <string>
#include <vector>

struct sqlite3;

namespace p2p {

struct StudyFilter {
    std::string route;
    std::string dateFrom;   // AAAAMMGG (vuoto = nessun limite)
    std::string dateTo;
    int         status = -1;   // -1 = tutti, altrimenti StudyStatus
    std::string text;          // filtro libero su nome/ID/descrizione
    int         limit  = 20000;
};

struct AttemptRecord {
    std::string studyUid;
    std::string startedAt;
    int         attemptNo  = 0;
    int         dimse      = 0;
    int         completed  = 0;
    int         failed     = 0;
    int         warning    = 0;
    int         instancesB = -1;
    std::string outcome;
    std::string message;
    long        durationMs = 0;
};

using StatusCounts = std::array<int, static_cast<int>(StudyStatus::Count)>;

class Database {
public:
    Database() = default;
    ~Database();
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    bool open(const std::string& path, std::string& err);
    void close();

    // --- impostazioni chiave/valore -----------------------------------------
    void        setSetting(const std::string& key, const std::string& value);
    std::string getSetting(const std::string& key, const std::string& def = "") const;
    void        saveConfig(const AppConfig& cfg);
    AppConfig   loadConfig() const;

    // --- coda studi ---------------------------------------------------------
    // inserisce/aggiorna gli studi trovati su A; restituisce quanti erano nuovi
    int  upsertStudies(const std::string& route, const std::vector<StudyInfo>& studies);
    // all'avvio: gli studi rimasti "RUNNING" (crash/chiusura) tornano "PENDING"
    int  resetRunning();
    // rimette in coda FAILED/PARTIAL nell'intervallo
    int  requeueFailed(const std::string& route, const std::string& from, const std::string& to);

    std::vector<StudyRow> listStudies(const StudyFilter& f) const;
    StatusCounts          countByStatus(const std::string& route, const std::string& from,
                                        const std::string& to) const;
    // studi da trasferire nell'intervallo, ordinati per data
    std::vector<StudyRow> queueFor(const std::string& route, const std::string& from,
                                   const std::string& to, bool includeFailed) const;

    void markRunning(const std::string& route, const std::string& uid);
    void setInstancesA(const std::string& route, const std::string& uid, int n);
    void saveOutcome(const std::string& route, const std::string& uid, StudyStatus st,
                     int attempts, int completed, int failed, int warning, int dimse,
                     int instancesB, const std::string& error);
    void addAttempt(const std::string& route, const AttemptRecord& a);
    std::vector<AttemptRecord> attemptsFor(const std::string& route,
                                           const std::string& uid) const;

    // esporta in CSV gli studi filtrati
    bool exportCsv(const StudyFilter& f, const std::string& path, std::string& err) const;

private:
    bool exec(const char* sql, std::string* err = nullptr) const;
    void setSettingLocked(const std::string& key, const std::string& value);   // mtx_ già preso

    sqlite3*           db_ = nullptr;
    mutable std::mutex mtx_;
};

} // namespace p2p
