// =============================================================================
//  TransferEngine.cpp
// =============================================================================
#include "TransferEngine.h"
#include "DateUtil.h"
#include "Logger.h"

#include <algorithm>
#include <map>

namespace p2p {

using Clock = std::chrono::steady_clock;

namespace {
double secondsSince(Clock::time_point t) {
    return std::chrono::duration<double>(Clock::now() - t).count();
}

WorkerView freshWorker(int id, const char* phase) {
    WorkerView w;
    w.id    = id;
    w.phase = phase;
    return w;
}

// attesa interrompibile
void sleepInterruptible(int seconds, const std::atomic<bool>& stop) {
    for (int i = 0; i < seconds * 10 && !stop; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
}
} // namespace

// =============================================================================
//  Valutazione degli esiti (funzioni pure, coperte da tests/)
// =============================================================================
MoveOutcome evaluateMove(const MoveResult& mr, int expected, bool nothingToDo, bool seriesMode,
                         size_t seriesCount) {
    MoveOutcome o;
    if (!mr.associationOk) {
        o.status  = StudyStatus::Failed;
        o.message = mr.error;
    } else if (mr.cancelled) {
        o.status  = StudyStatus::Pending;   // interrotto dall'utente: resta in coda
        o.message = "annullato dall'utente";
    } else if (mr.status == dimse::Success && mr.failed == 0) {
        o.status  = StudyStatus::Done;
        o.message = cat(mr.completed, " immagini inviate");
        if (nothingToDo)
            o.message = "nessuna serie mancante su B";
        else if (mr.completed == 0 && mr.responses <= 1 && expected > 0)
            o.message = "A ha risposto 'successo' senza sotto-operazioni";
        else if (seriesMode)
            o.message = cat(mr.completed, " immagini inviate (", seriesCount, " serie)");
    } else if ((mr.status == dimse::SubOpsWarning || mr.failed > 0) && mr.completed == 0) {
        o.status  = StudyStatus::Failed;   // nessuna immagine arrivata: B irraggiungibile?
        o.message = cat("nessuna immagine inviata, ", mr.failed, " fallite (",
                        DicomClient::statusText(mr.status), "): A non riesce a inviare a B");
    } else if (mr.status == dimse::SubOpsWarning || mr.failed > 0) {
        o.status  = StudyStatus::Partial;
        o.message = cat(mr.failed, " immagini fallite su ", mr.completed + mr.failed,
                        " (", DicomClient::statusText(mr.status), ")");
    } else {
        // associazione caduta o stato anomalo: se qualche immagine è già arrivata lo
        // studio è parziale (verifica su B e ritentativo delle sole serie mancanti)
        o.status  = mr.completed > 0 ? StudyStatus::Partial : StudyStatus::Failed;
        o.message = mr.error.empty() ? DicomClient::statusText(mr.status) : mr.error;
        if (mr.completed > 0) o.message += cat(" (", mr.completed, " immagini già inviate)");
    }
    return o;
}

void applyVerification(MoveOutcome& o, int onB, int expected, const std::string& err) {
    if (onB < 0) {
        o.message += cat(" — verifica su B non riuscita: ", err);
    } else if (expected > 0 && onB < expected) {
        o.status  = StudyStatus::Partial;
        o.message = cat("su B ", onB, " immagini su ", expected, " attese");
    } else if (expected > 0 && onB >= expected) {
        o.status  = StudyStatus::Done;   // B completo (anche se A ha segnalato avvisi)
        o.message = cat(onB, "/", expected, " immagini verificate su B");
    } else if (onB == 0) {
        o.status  = StudyStatus::Failed;
        o.message = "studio non trovato su B dopo il C-MOVE";
    }
}

// =============================================================================
//  ScanJob
// =============================================================================
ScanJob::~ScanJob() {
    stop_ = true;
    if (th_.joinable()) th_.join();
}

bool ScanJob::running() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return st_.running;
}

ScanJob::State ScanJob::state() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return st_;
}

bool ScanJob::start(const AppConfig& cfg, Database* db, const std::string& fromIso,
                    const std::string& toIso, const std::string& modality, std::string& err) {
    if (running()) { err = "ricerca già in corso"; return false; }
    Ymd from, to;
    if (!Ymd::parse(fromIso, from) || !Ymd::parse(toIso, to)) { err = "date non valide"; return false; }
    if (!(from <= to)) { err = "la data iniziale è successiva a quella finale"; return false; }
    if (!cfg.a.valid(&err)) { err = "Nodo A: " + err; return false; }
    if (!cfg.local.valid(&err, false)) { err = "AE locale: " + err; return false; }

    if (th_.joinable()) th_.join();
    stop_ = false;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        st_ = State{};
        st_.running   = true;
        st_.daysTotal = cfg.xfer.dayByDayQuery ? int(to.toDays() - from.toDays() + 1) : 1;
    }
    th_ = std::thread(&ScanJob::run, this, cfg, db, from.toDays(), to.toDays(), modality);
    return true;
}

void ScanJob::run(AppConfig cfg, Database* db, long fromDays, long toDays, std::string modality) {
    DicomClient client(cfg.local, cfg.net);
    const std::string route = cfg.route();
    logInfo("Ricerca studi su ", cfg.a.aet, " dal ", Ymd::fromDays(fromDays).iso(), " al ",
            Ymd::fromDays(toDays).iso(), modality.empty() ? "" : " modalità " + modality);

    // intervalli da interrogare: un giorno alla volta oppure l'intero range
    std::vector<std::pair<long, long>> chunks;
    if (cfg.xfer.dayByDayQuery)
        for (long d = fromDays; d <= toDays; ++d) chunks.emplace_back(d, d);
    else
        chunks.emplace_back(fromDays, toDays);

    std::string lastErr;
    for (const auto& [d0, d1] : chunks) {
        if (stop_) break;
        const Ymd y0 = Ymd::fromDays(d0), y1 = Ymd::fromDays(d1);
        const std::string range = d0 == d1 ? y0.dicom() : y0.dicom() + "-" + y1.dicom();
        {
            std::lock_guard<std::mutex> lk(mtx_);
            st_.current = d0 == d1 ? y0.iso() : y0.iso() + " → " + y1.iso();
        }
        std::vector<StudyInfo> studies;
        std::string err;
        bool ok = false;
        for (int attempt = 0; attempt < 3 && !ok && !stop_; ++attempt) {   // 3 tentativi per giorno
            studies.clear();
            ok = client.findStudies(cfg.a, range, modality, studies, err);
            if (!ok) {
                logWarn("C-FIND ", range, " tentativo ", attempt + 1, ": ", err);
                sleepInterruptible(2, stop_);
            }
        }
        int added = 0;
        if (ok) {
            added = db->upsertStudies(route, studies);
            if (!studies.empty())
                logInfo("  ", range, ": ", studies.size(), " studi (", added, " nuovi)");
        } else {
            lastErr = err;
            logError("C-FIND ", range, " fallito definitivamente: ", err);
        }
        std::lock_guard<std::mutex> lk(mtx_);
        st_.daysDone++;
        st_.found += int(studies.size());
        st_.added += added;
        if (!ok) st_.dayErrors++;
    }

    std::lock_guard<std::mutex> lk(mtx_);
    st_.running  = false;
    st_.finished = true;
    st_.ok       = st_.dayErrors == 0 && !stop_;
    st_.error    = stop_ ? "interrotta dall'utente" : lastErr;
    st_.current.clear();
    logInfo("Ricerca terminata: ", st_.found, " studi trovati, ", st_.added, " nuovi, ",
            st_.dayErrors, " intervalli in errore");
}

// =============================================================================
//  TransferEngine
// =============================================================================
TransferEngine::~TransferEngine() {
    stop();
    join();
}

bool TransferEngine::start(const AppConfig& cfg, Database* db, const std::string& fromIso,
                           const std::string& toIso, std::string& err) {
    if (running_) { err = "trasferimento già in corso"; return false; }
    Ymd from, to;
    if (!Ymd::parse(fromIso, from) || !Ymd::parse(toIso, to)) { err = "date non valide"; return false; }
    if (!cfg.a.valid(&err)) { err = "Nodo A: " + err; return false; }
    if (!cfg.b.valid(&err)) { err = "Nodo B: " + err; return false; }
    if (!cfg.local.valid(&err, false)) { err = "AE locale: " + err; return false; }
    join();

    cfg_   = cfg;
    db_    = db;
    route_ = cfg.route();
    queue_ = db->queueFor(route_, from.dicom(), to.dicom(), cfg.xfer.includeFailed);
    if (queue_.empty()) {
        err = "nessuno studio da trasferire nell'intervallo (eseguire prima la ricerca su A)";
        return false;
    }

    const int nWorkers = std::clamp(cfg.xfer.workers, 1, 16);
    next_ = 0;
    processed_ = done_ = partial_ = failed_ = skipped_ = 0;
    imagesDone_ = 0;
    long expected = 0;
    for (const auto& s : queue_) if (s.numInstances > 0) expected += s.numInstances;
    imagesExpected_ = expected;
    stop_   = false;
    paused_ = false;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        workers_.assign(nWorkers, WorkerView{});
        workerStart_.assign(nWorkers, Clock::now());
        for (int i = 0; i < nWorkers; ++i) workers_[i].id = i + 1;
        recent_.clear();
    }
    t0_       = Clock::now();
    running_  = true;
    activeWorkers_ = nWorkers;

    logInfo("Avvio trasferimento ", cfg.a.aet, " → ", cfg.b.aet, ": ", queue_.size(),
            " studi, modalità ", nWorkers == 1 ? "sincrona" : "asincrona", " (", nWorkers,
            " worker)");

    for (int i = 0; i < nWorkers; ++i) threads_.emplace_back(&TransferEngine::workerLoop, this, i);
    supervisor_ = std::thread([this] {
        for (auto& t : threads_) if (t.joinable()) t.join();
        finalElapsed_ = secondsSince(t0_);
        running_ = false;
        logInfo("Trasferimento terminato: ", done_.load(), " completati, ", partial_.load(),
                " parziali, ", failed_.load(), " falliti, ", skipped_.load(), " già presenti su B");
    });
    return true;
}

void TransferEngine::stop() {
    if (running_ && !stop_) logWarn("Arresto richiesto: termino gli studi in corso");
    stop_   = true;
    paused_ = false;
}

void TransferEngine::join() {
    if (supervisor_.joinable()) supervisor_.join();
    threads_.clear();
}

void TransferEngine::setWorker(int wid, const std::function<void(WorkerView&)>& fn) {
    std::lock_guard<std::mutex> lk(mtx_);
    fn(workers_[wid]);
}

void TransferEngine::pushResult(ResultView r) {
    std::lock_guard<std::mutex> lk(mtx_);
    recent_.push_front(std::move(r));
    if (recent_.size() > 500) recent_.pop_back();
}

EngineSnapshot TransferEngine::snapshot() const {
    EngineSnapshot s;
    s.running        = running_;
    s.paused         = paused_;
    s.stopping       = stop_ && running_;
    s.total          = int(queue_.size());
    s.processed      = processed_;
    s.done           = done_;
    s.partial        = partial_;
    s.failed         = failed_;
    s.skipped        = skipped_;
    s.imagesDone     = imagesDone_;
    s.imagesExpected = imagesExpected_;
    s.elapsedSec     = running_ ? secondsSince(t0_) : finalElapsed_.load();
    if (s.elapsedSec > 1) s.imagesPerSec = double(s.imagesDone) / s.elapsedSec;
    if (s.running) {
        // stima: preferisce le immagini se i conteggi sono noti, altrimenti gli studi
        if (s.imagesExpected > 0 && s.imagesPerSec > 0)
            s.etaSec = std::max(0.0, double(s.imagesExpected - s.imagesDone) / s.imagesPerSec);
        else if (s.processed > 0)
            s.etaSec = s.elapsedSec / s.processed * (s.total - s.processed);
    }
    std::lock_guard<std::mutex> lk(mtx_);
    s.workers = workers_;
    for (size_t i = 0; i < s.workers.size(); ++i)
        if (s.workers[i].busy) s.workers[i].seconds = secondsSince(workerStart_[i]);
    s.recent.assign(recent_.begin(), recent_.end());
    return s;
}

void TransferEngine::workerLoop(int wid) {
    while (!stop_) {
        while (paused_ && !stop_) {
            setWorker(wid, [](WorkerView& w) { w.phase = "In pausa"; });
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        if (stop_) break;
        const size_t idx = next_++;
        if (idx >= queue_.size()) break;
        processStudy(wid, queue_[idx]);
        ++processed_;
    }
    setWorker(wid, [](WorkerView& w) { w = freshWorker(w.id, "Terminato"); });
    --activeWorkers_;
}

void TransferEngine::processStudy(int wid, StudyRow st) {
    DicomClient client(cfg_.local, cfg_.net);
    const auto  tStudy = Clock::now();
    {
        std::lock_guard<std::mutex> lk(mtx_);
        workerStart_[wid] = tStudy;
        WorkerView& w = workers_[wid];
        w = freshWorker(w.id, "");
        w.busy        = true;
        w.patient     = st.patientName.empty() ? st.patientId : st.patientName;
        w.studyDate   = st.studyDate;
        w.description = st.description;
        w.expected    = st.numInstances;
        w.phase       = "Preparazione";
    }
    db_->markRunning(route_, st.uid);
    std::string err;

    // conteggio su A se il PACS non l'ha fornito nella ricerca (serve per la verifica)
    if (st.numInstances < 0 && (cfg_.xfer.verifyOnB || cfg_.xfer.skipIfPresentOnB)) {
        setWorker(wid, [](WorkerView& w) { w.phase = "Conteggio su A"; });
        const int n = client.countStudyInstances(cfg_.a, st.uid, err);
        if (n >= 0) {
            st.numInstances = n;
            imagesExpected_ += n;   // il totale cresce man mano che i conteggi diventano noti
            db_->setInstancesA(route_, st.uid, n);
            setWorker(wid, [n](WorkerView& w) { w.expected = n; });
        }
    }

    ResultView rv;
    rv.patient     = st.patientName.empty() ? st.patientId : st.patientName;
    rv.studyDate   = st.studyDate;
    rv.description = st.description;
    rv.expected    = st.numInstances;

    auto finish = [&](StudyStatus status, int attempts, int completed, int failed, int warning,
                      int dimseStatus, int onB, const std::string& msg) {
        db_->saveOutcome(route_, st.uid, status, attempts, completed, failed, warning,
                         dimseStatus, onB, msg);
        rv.time      = nowTimestamp().substr(11);
        rv.status    = status;
        rv.completed = completed;
        rv.failed    = failed;
        rv.onB       = onB;
        rv.attempts  = attempts;
        rv.seconds   = secondsSince(tStudy);
        rv.message   = msg;
        switch (status) {
            case StudyStatus::Done:    ++done_;    break;
            case StudyStatus::Partial: ++partial_; break;
            case StudyStatus::Failed:  ++failed_;  break;
            case StudyStatus::Skipped: ++skipped_; break;
            default: break;
        }
        pushResult(rv);
        setWorker(wid, [](WorkerView& w) { w = freshWorker(w.id, "Libero"); });
    };

    // già presente su B?
    if (cfg_.xfer.skipIfPresentOnB && st.numInstances > 0) {
        setWorker(wid, [](WorkerView& w) { w.phase = "Controllo su B"; });
        const int onB = client.countStudyInstances(cfg_.b, st.uid, err);
        if (onB >= st.numInstances) {
            imagesDone_ += st.numInstances;
            finish(StudyStatus::Skipped, 0, 0, 0, 0, 0, onB,
                   cat("già presente su B (", onB, " immagini)"));
            return;
        }
    }

    StudyStatus status = StudyStatus::Failed;
    std::string message;
    MoveResult  mr;
    int         onB      = -1;
    int         attempts = 0;
    const int   maxAttempts = 1 + std::max(0, cfg_.xfer.maxRetries);
    // immagini già conteggiate nella barra globale per questo studio: un ritentativo
    // rinvia tutto lo studio, quindi si somma solo l'eccedenza rispetto al massimo visto
    int counted = 0;
    // uno studio rimasto PARZIALE (da questa o da una sessione precedente) viene
    // ritentato inviando solo le serie incomplete su B, non l'intero studio
    bool retryMissingSeries = (st.status == StudyStatus::Partial) && cfg_.xfer.verifyOnB;

    for (int attempt = 1; attempt <= maxAttempts && !stop_; ++attempt) {
        attempts = attempt;
        const auto tAttempt = Clock::now();

        std::vector<std::string> seriesToMove;
        int  missing     = 0;
        bool seriesMode  = false;
        bool nothingToDo = false;
        if (retryMissingSeries) {
            setWorker(wid, [attempt](WorkerView& w) { w.phase = "Analisi serie su B"; w.attempt = attempt; });
            std::string e2;
            if (client.findIncompleteSeries(cfg_.a, cfg_.b, st.uid, seriesToMove, missing, e2)) {
                seriesMode  = !seriesToMove.empty();
                nothingToDo = seriesToMove.empty();
                if (seriesMode)
                    logInfo("Studio ", st.uid, ": ritento solo ", seriesToMove.size(),
                            " serie incomplete (~", missing, " immagini)");
            } else {
                logWarn("Analisi serie non riuscita (", e2, "): ritento lo studio intero");
            }
        }

        setWorker(wid, [attempt, seriesMode, missing](WorkerView& w) {
            w.phase = seriesMode ? "C-MOVE serie" : "C-MOVE"; w.attempt = attempt;
            w.completed = 0; w.failed = 0; w.remaining = -1;
            if (seriesMode) w.expected = missing;
        });

        if (nothingToDo) {
            // B risulta già completo: basta la verifica finale
            mr = MoveResult{};
            mr.associationOk = true;
            mr.status        = dimse::Success;
        } else if (!seriesMode) {
            // l'avanzamento in immagini arriva dalle risposte "pending" del PACS A;
            // un ritentativo rinvia tutto lo studio: si conta solo l'eccedenza
            auto onProgress = [&](const MoveProgress& p) {
                const int c = p.completed;
                if (c > counted) { imagesDone_ += (c - counted); counted = c; }
                setWorker(wid, [&](WorkerView& w) {
                    w.completed = p.completed; w.failed = p.failed; w.remaining = p.remaining;
                });
            };
            mr = client.moveStudy(cfg_.a, cfg_.b.aet, st.uid, onProgress, &stop_);
            if (mr.completed > counted) { imagesDone_ += (mr.completed - counted); counted = mr.completed; }
        } else {
            // C-MOVE per serie; i risultati vengono sommati in un unico MoveResult
            MoveResult agg;
            agg.associationOk = true;
            agg.status        = dimse::Success;
            int base = 0;   // immagini completate nelle serie precedenti di questo tentativo
            for (const auto& seUid : seriesToMove) {
                if (stop_) { agg.cancelled = true; break; }
                int seen = 0;
                auto onProgress = [&](const MoveProgress& p) {
                    if (p.completed > seen) {
                        int add = p.completed - seen;
                        if (st.numInstances > 0) add = std::min(add, std::max(0, st.numInstances - counted));
                        imagesDone_ += add;
                        counted += add;
                        seen = p.completed;
                    }
                    setWorker(wid, [&](WorkerView& w) {
                        w.completed = base + p.completed; w.failed = agg.failed + p.failed;
                    });
                };
                MoveResult r = client.moveSeries(cfg_.a, cfg_.b.aet, st.uid, seUid, onProgress, &stop_);
                if (r.completed > seen) {
                    int add = r.completed - seen;
                    if (st.numInstances > 0) add = std::min(add, std::max(0, st.numInstances - counted));
                    imagesDone_ += add;
                    counted += add;
                }
                base          += r.completed;
                agg.completed += r.completed;
                agg.failed    += r.failed;
                agg.warning   += r.warning;
                agg.responses += r.responses;
                agg.cancelled  = agg.cancelled || r.cancelled;
                if (!r.associationOk) { agg.associationOk = agg.completed > 0; agg.error = r.error; agg.status = r.status; }
                else if (r.status != dimse::Success) { agg.status = r.status; if (!r.error.empty()) agg.error = r.error; }
                if (!r.associationOk || r.cancelled || r.status == dimse::DestUnknown) break;
            }
            mr = agg;
        }

        // --- valutazione dell'esito ------------------------------------------
        {
            const MoveOutcome o = evaluateMove(mr, st.numInstances, nothingToDo, seriesMode,
                                               seriesToMove.size());
            status  = o.status;
            message = o.message;
        }

        // --- verifica su B ---------------------------------------------------
        if (cfg_.xfer.verifyOnB && (status == StudyStatus::Done || status == StudyStatus::Partial)) {
            setWorker(wid, [](WorkerView& w) { w.phase = "Verifica su B"; });
            onB = client.countStudyInstances(cfg_.b, st.uid, err);
            MoveOutcome o{status, message};
            applyVerification(o, onB, st.numInstances, err);
            status  = o.status;
            message = o.message;
        }

        AttemptRecord ar;
        ar.studyUid   = st.uid;
        ar.attemptNo  = attempt;
        ar.startedAt  = nowTimestamp();
        ar.dimse      = mr.status;
        ar.completed  = mr.completed;
        ar.failed     = mr.failed;
        ar.warning    = mr.warning;
        ar.instancesB = onB;
        ar.outcome    = statusCode(status);
        ar.message    = message;
        ar.durationMs = static_cast<long>(secondsSince(tAttempt) * 1000);
        db_->addAttempt(route_, ar);

        if (status == StudyStatus::Done || status == StudyStatus::Pending) break;
        // il prossimo tentativo di uno studio parziale invia solo le serie mancanti
        retryMissingSeries = (status == StudyStatus::Partial) && cfg_.xfer.verifyOnB;
        // destinazione sconosciuta: inutile ritentare
        if (mr.status == dimse::DestUnknown) break;

        if (attempt < maxAttempts && !stop_) {
            logWarn("Studio ", st.uid, " tentativo ", attempt, ": ", message,
                    " — nuovo tentativo tra ", cfg_.xfer.retryDelaySec, " s");
            setWorker(wid, [](WorkerView& w) { w.phase = "Attesa ritentativo"; });
            sleepInterruptible(cfg_.xfer.retryDelaySec, stop_);
        }
    }

    // "Ferma" premuto prima del primo tentativo (es. durante conteggio/controllo su B):
    // lo studio non è fallito, resta in coda
    if (attempts == 0) {
        status  = StudyStatus::Pending;
        message = "annullato dall'utente";
    }

    // il registro su disco non deve contenere nomi di pazienti se l'opzione è attiva
    const std::string who = cfg_.logAnonymize ? "studio " + st.uid : rv.patient;
    if (status == StudyStatus::Done)
        logInfo("OK  ", dicomDateToDisplay(st.studyDate), " ", who, " — ", message);
    else if (status == StudyStatus::Pending)
        logWarn("--  ", dicomDateToDisplay(st.studyDate), " ", who, " — ", message);
    else
        logError(statusLabel(status), "  ", dicomDateToDisplay(st.studyDate), " ", who,
                 " — ", message);

    finish(status, attempts, mr.completed, mr.failed, mr.warning, mr.status, onB, message);
}

} // namespace p2p
