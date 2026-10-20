// =============================================================================
//  TransferEngine.h — ricerca studi su A (ScanJob) e trasferimento C-MOVE (TransferEngine)
//
//  Modello di concorrenza:
//   - la UI (thread principale) legge solo "istantanee" (snapshot) protette da mutex;
//   - ogni worker apre le proprie associazioni DICOM, quindi N worker = N C-MOVE
//     in parallelo verso il PACS A, che a sua volta apre N associazioni C-STORE verso B;
//   - lo stato di ogni studio viene scritto subito su SQLite: in caso di chiusura o
//     crash nulla va perso e gli studi "RUNNING" tornano "PENDING" al riavvio.
// =============================================================================
#pragma once

#include "Database.h"
#include "DicomClient.h"
#include "Types.h"

#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace p2p {

// -----------------------------------------------------------------------------
// Valutazione degli esiti (pura, senza rete: testabile)
// -----------------------------------------------------------------------------
struct MoveOutcome {
    StudyStatus status = StudyStatus::Failed;
    std::string message;
};

// esito di un C-MOVE (studio intero o per serie) rispetto alle immagini attese
MoveOutcome evaluateMove(const MoveResult& mr, int expected, bool nothingToDo, bool seriesMode,
                         size_t seriesCount);
// aggiusta l'esito dopo il conteggio delle immagini su B (onB < 0 = verifica non riuscita)
void applyVerification(MoveOutcome& o, int onB, int expected, const std::string& err);

// -----------------------------------------------------------------------------
// Ricerca degli studi su A in un intervallo di date
// -----------------------------------------------------------------------------
class ScanJob {
public:
    struct State {
        bool        running    = false;
        bool        finished   = false;
        bool        ok         = false;
        int         daysTotal  = 0;
        int         daysDone   = 0;
        int         found      = 0;
        int         added      = 0;
        int         dayErrors  = 0;
        std::string current;
        std::string error;
    };

    ~ScanJob();
    bool  start(const AppConfig& cfg, Database* db, const std::string& fromIso,
                const std::string& toIso, const std::string& modality, std::string& err);
    void  stop() { stop_ = true; }
    State state() const;
    bool  running() const;

private:
    void run(AppConfig cfg, Database* db, long fromDays, long toDays, std::string modality);

    std::thread        th_;
    std::atomic<bool>  stop_{false};
    mutable std::mutex mtx_;
    State              st_;
};

// -----------------------------------------------------------------------------
// Trasferimento
// -----------------------------------------------------------------------------
struct WorkerView {
    int         id        = 0;
    bool        busy      = false;
    std::string phase;           // "C-MOVE", "Verifica su B", "Attesa ritentativo"...
    std::string patient;
    std::string studyDate;
    std::string description;
    int         attempt   = 0;
    int         expected  = -1;
    int         completed = 0;
    int         failed    = 0;
    int         remaining = -1;
    double      seconds   = 0;   // durata dello studio corrente
};

struct ResultView {
    std::string time;
    std::string patient;
    std::string studyDate;
    std::string description;
    StudyStatus status = StudyStatus::Pending;
    int         completed = 0;
    int         failed    = 0;
    int         expected  = -1;
    int         onB       = -1;
    int         attempts  = 0;
    double      seconds   = 0;
    std::string message;
};

struct EngineSnapshot {
    bool   running        = false;
    bool   paused         = false;
    bool   stopping       = false;
    int    total          = 0;   // studi in coda all'avvio
    int    processed      = 0;
    int    done           = 0;
    int    partial        = 0;
    int    failed         = 0;
    int    skipped        = 0;
    long   imagesDone     = 0;
    long   imagesExpected = 0;   // somma dei conteggi noti su A
    double elapsedSec     = 0;
    double imagesPerSec   = 0;
    double etaSec         = -1;
    std::vector<WorkerView> workers;
    std::vector<ResultView> recent;   // ultimi risultati (più recenti in testa)
};

class TransferEngine {
public:
    ~TransferEngine();

    // carica la coda dal DB e avvia i worker
    bool start(const AppConfig& cfg, Database* db, const std::string& fromIso,
               const std::string& toIso, std::string& err);
    void pause(bool p) { paused_ = p; }
    void stop();             // termina dopo gli studi in corso (invia C-CANCEL se possibile)
    void join();
    bool running() const { return running_; }
    EngineSnapshot snapshot() const;

private:
    void workerLoop(int wid);
    void processStudy(int wid, StudyRow study);
    void setWorker(int wid, const std::function<void(WorkerView&)>& fn);
    void pushResult(ResultView r);

    AppConfig                cfg_;
    Database*                db_ = nullptr;
    std::string              route_;
    std::vector<StudyRow>    queue_;
    std::atomic<size_t>      next_{0};
    std::vector<std::thread> threads_;
    std::thread              supervisor_;

    std::atomic<bool> running_{false};
    std::atomic<bool> paused_{false};
    std::atomic<bool> stop_{false};
    std::atomic<int>  activeWorkers_{0};

    std::atomic<int>  processed_{0}, done_{0}, partial_{0}, failed_{0}, skipped_{0};
    std::atomic<long> imagesDone_{0};
    std::atomic<long> imagesExpected_{0};
    std::chrono::steady_clock::time_point t0_;
    std::atomic<double> finalElapsed_{0};

    mutable std::mutex       mtx_;
    std::vector<WorkerView>  workers_;
    std::vector<std::chrono::steady_clock::time_point> workerStart_;
    std::deque<ResultView>   recent_;
};

} // namespace p2p
