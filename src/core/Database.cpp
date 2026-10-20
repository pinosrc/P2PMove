// =============================================================================
//  Database.cpp
// =============================================================================
#include "Database.h"
#include "DateUtil.h"
#include "Logger.h"

#include <sqlite3.h>

#include <fstream>

namespace p2p {

namespace {

const char* kSchema = R"SQL(
PRAGMA journal_mode=WAL;
PRAGMA synchronous=NORMAL;
CREATE TABLE IF NOT EXISTS settings(
    key   TEXT PRIMARY KEY,
    value TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS studies(
    route        TEXT NOT NULL,
    study_uid    TEXT NOT NULL,
    patient_id   TEXT, patient_name TEXT,
    study_date   TEXT, study_time   TEXT,
    accession    TEXT, description  TEXT, modalities TEXT,
    instances_a  INTEGER DEFAULT -1,
    instances_b  INTEGER DEFAULT -1,
    status       TEXT NOT NULL DEFAULT 'PENDING',
    attempts     INTEGER DEFAULT 0,
    completed    INTEGER DEFAULT 0,
    failed       INTEGER DEFAULT 0,
    warning      INTEGER DEFAULT 0,
    last_dimse   INTEGER DEFAULT 0,
    last_error   TEXT DEFAULT '',
    found_at     TEXT,
    updated_at   TEXT,
    PRIMARY KEY(route, study_uid)
);
CREATE INDEX IF NOT EXISTS ix_studies_date   ON studies(route, study_date);
CREATE INDEX IF NOT EXISTS ix_studies_status ON studies(route, status);
CREATE TABLE IF NOT EXISTS attempts(
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    route       TEXT NOT NULL,
    study_uid   TEXT NOT NULL,
    attempt_no  INTEGER,
    started_at  TEXT,
    dimse       INTEGER,
    completed   INTEGER, failed INTEGER, warning INTEGER,
    instances_b INTEGER,
    outcome     TEXT,
    message     TEXT,
    duration_ms INTEGER
);
CREATE INDEX IF NOT EXISTS ix_attempts_study ON attempts(route, study_uid);
)SQL";

// Prepared statement con distruzione automatica
class Stmt {
public:
    Stmt(sqlite3* db, const char* sql) {
        if (sqlite3_prepare_v2(db, sql, -1, &st_, nullptr) != SQLITE_OK) {
            logError("SQLite prepare: ", sqlite3_errmsg(db), " — ", sql);
            st_ = nullptr;
        }
    }
    ~Stmt() { if (st_) sqlite3_finalize(st_); }
    explicit operator bool() const { return st_ != nullptr; }

    Stmt& bind(int i, const std::string& v) { sqlite3_bind_text(st_, i, v.c_str(), -1, SQLITE_TRANSIENT); return *this; }
    Stmt& bind(int i, int v)                { sqlite3_bind_int(st_, i, v); return *this; }
    Stmt& bind(int i, long v)               { sqlite3_bind_int64(st_, i, v); return *this; }
    bool  step()                            { return st_ && sqlite3_step(st_) == SQLITE_ROW; }
    bool  run()                             { if (!st_) return false; int rc = sqlite3_step(st_); sqlite3_reset(st_); return rc == SQLITE_DONE; }
    void  reset()                           { sqlite3_reset(st_); sqlite3_clear_bindings(st_); }

    std::string text(int c) const {
        auto* p = sqlite3_column_text(st_, c);
        return p ? reinterpret_cast<const char*>(p) : "";
    }
    int  integer(int c) const { return sqlite3_column_int(st_, c); }
    long int64(int c)   const { return static_cast<long>(sqlite3_column_int64(st_, c)); }

private:
    sqlite3_stmt* st_ = nullptr;
};

// condizione su intervallo date (AAAAMMGG ordinabile come testo)
std::string dateWhere(const std::string& from, const std::string& to) {
    std::string w;
    if (!from.empty()) w += " AND study_date >= ?" ;
    if (!to.empty())   w += " AND study_date <= ?" ;
    return w;
}

const char* kStudyCols =
    "study_uid, patient_id, patient_name, study_date, study_time, accession, description, "
    "modalities, instances_a, instances_b, status, attempts, completed, failed, warning, "
    "last_dimse, last_error, updated_at";

StudyRow readStudy(const Stmt& s) {
    StudyRow r;
    r.uid          = s.text(0);
    r.patientId    = s.text(1);
    r.patientName  = s.text(2);
    r.studyDate    = s.text(3);
    r.studyTime    = s.text(4);
    r.accession    = s.text(5);
    r.description  = s.text(6);
    r.modalities   = s.text(7);
    r.numInstances = s.integer(8);
    r.instancesB   = s.integer(9);
    r.status       = statusFromCode(s.text(10));
    r.attempts     = s.integer(11);
    r.completed    = s.integer(12);
    r.failed       = s.integer(13);
    r.warning      = s.integer(14);
    r.lastDimse    = s.integer(15);
    r.lastError    = s.text(16);
    r.updatedAt    = s.text(17);
    return r;
}

// virgolette raddoppiate; le celle che Excel/Calc leggerebbero come formula (= + - @)
// ricevono un apostrofo iniziale
std::string csvEscape(const std::string& v) {
    std::string o = "\"";
    if (!v.empty() && std::string("=+-@\t\r").find(v[0]) != std::string::npos) o += '\'';
    for (char c : v) { if (c == '"') o += '"'; o += c; }
    return o + "\"";
}

// testo utente per LIKE ... ESCAPE '\': % e _ valgono come caratteri normali
std::string likeEscape(const std::string& v) {
    std::string o;
    for (char c : v) {
        if (c == '\\' || c == '%' || c == '_') o += '\\';
        o += c;
    }
    return o;
}

} // namespace

// -----------------------------------------------------------------------------
Database::~Database() { close(); }

bool Database::open(const std::string& path, std::string& err) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (sqlite3_open_v2(path.c_str(), &db_,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                        nullptr) != SQLITE_OK) {
        err = db_ ? sqlite3_errmsg(db_) : "sqlite3_open fallito";
        if (db_) { sqlite3_close(db_); db_ = nullptr; }   // l'handle va chiuso anche se open fallisce
        return false;
    }
    sqlite3_busy_timeout(db_, 5000);
    return exec(kSchema, &err);
}

void Database::close() {
    std::lock_guard<std::mutex> lk(mtx_);
    if (db_) { sqlite3_close(db_); db_ = nullptr; }
}

bool Database::exec(const char* sql, std::string* err) const {
    char* msg = nullptr;
    if (sqlite3_exec(db_, sql, nullptr, nullptr, &msg) != SQLITE_OK) {
        if (err) *err = msg ? msg : "errore SQLite";
        logError("SQLite: ", msg ? msg : "?");
        sqlite3_free(msg);
        return false;
    }
    return true;
}

// --- impostazioni -------------------------------------------------------------
void Database::setSetting(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lk(mtx_);
    setSettingLocked(key, value);
}

void Database::setSettingLocked(const std::string& key, const std::string& value) {
    Stmt s(db_, "INSERT INTO settings(key,value) VALUES(?,?) "
                "ON CONFLICT(key) DO UPDATE SET value=excluded.value");
    s.bind(1, key).bind(2, value).run();
}

std::string Database::getSetting(const std::string& key, const std::string& def) const {
    std::lock_guard<std::mutex> lk(mtx_);
    Stmt s(db_, "SELECT value FROM settings WHERE key=?");
    s.bind(1, key);
    return s.step() ? s.text(0) : def;
}

void Database::saveConfig(const AppConfig& c) {
    // un solo lock per l'intera transazione: altrimenti il BEGIN può incrociarsi con
    // quello di upsertStudies (ricerca in corso) e le due transazioni si fondono
    std::lock_guard<std::mutex> lk(mtx_);
    auto setSetting = [this](const std::string& k, const std::string& v) { setSettingLocked(k, v); };
    auto node = [&](const std::string& p, const NodeConfig& n) {
        setSetting(p + ".name", n.name);
        setSetting(p + ".aet",  n.aet);
        setSetting(p + ".host", n.host);
        setSetting(p + ".port", std::to_string(n.port));
        setSetting(p + ".type", std::to_string(n.type));
    };
    exec("BEGIN");
    node("local", c.local);
    node("a", c.a);
    node("b", c.b);
    setSetting("net.connect", std::to_string(c.net.connectTimeoutSec));
    setSetting("net.acse",    std::to_string(c.net.acseTimeoutSec));
    setSetting("net.find",    std::to_string(c.net.findTimeoutSec));
    setSetting("net.move",    std::to_string(c.net.moveTimeoutSec));
    setSetting("net.pdu",     std::to_string(c.net.maxPdu));
    setSetting("x.workers",   std::to_string(c.xfer.workers));
    setSetting("x.retries",   std::to_string(c.xfer.maxRetries));
    setSetting("x.delay",     std::to_string(c.xfer.retryDelaySec));
    setSetting("x.verify",    c.xfer.verifyOnB ? "1" : "0");
    setSetting("x.skip",      c.xfer.skipIfPresentOnB ? "1" : "0");
    setSetting("x.failed",    c.xfer.includeFailed ? "1" : "0");
    setSetting("x.daybyday",  c.xfer.dayByDayQuery ? "1" : "0");
    setSetting("log.anonymize", c.logAnonymize ? "1" : "0");
    exec("COMMIT");
}

AppConfig Database::loadConfig() const {
    auto i = [&](const std::string& k, int d) {
        try { return std::stoi(getSetting(k, std::to_string(d))); } catch (...) { return d; }
    };
    auto b = [&](const std::string& k, bool d) { return getSetting(k, d ? "1" : "0") == "1"; };
    auto node = [&](const std::string& p, NodeConfig def) {
        NodeConfig n;
        n.name = getSetting(p + ".name", def.name);
        n.aet  = getSetting(p + ".aet",  def.aet);
        n.host = getSetting(p + ".host", def.host);
        n.port = i(p + ".port", def.port);
        n.type = i(p + ".type", def.type);
        return n;
    };
    AppConfig c;
    // valori iniziali di esempio (da impostare in "Nodi")
    c.local = node("local", {"Questa app", "P2PMOVE", "", 0, int(PacsType::Altro)});
    c.a     = node("a", {"Sorgente", "PACS_A", "192.0.2.10", 11112, int(PacsType::Dcm4chee2)});
    c.b     = node("b", {"Destinazione", "PACS_B", "192.0.2.20", 11112, int(PacsType::Dcm4cheeArc)});
    c.net.connectTimeoutSec = i("net.connect", c.net.connectTimeoutSec);
    c.net.acseTimeoutSec    = i("net.acse",    c.net.acseTimeoutSec);
    c.net.findTimeoutSec    = i("net.find",    c.net.findTimeoutSec);
    c.net.moveTimeoutSec    = i("net.move",    c.net.moveTimeoutSec);
    c.net.maxPdu            = i("net.pdu",     c.net.maxPdu);
    c.xfer.workers          = i("x.workers",   c.xfer.workers);
    c.xfer.maxRetries       = i("x.retries",   c.xfer.maxRetries);
    c.xfer.retryDelaySec    = i("x.delay",     c.xfer.retryDelaySec);
    c.xfer.verifyOnB        = b("x.verify",    c.xfer.verifyOnB);
    c.xfer.skipIfPresentOnB = b("x.skip",      c.xfer.skipIfPresentOnB);
    c.xfer.includeFailed    = b("x.failed",    c.xfer.includeFailed);
    c.xfer.dayByDayQuery    = b("x.daybyday",  c.xfer.dayByDayQuery);
    c.logAnonymize          = b("log.anonymize", c.logAnonymize);
    c.sanitize();
    return c;
}

// --- coda studi -----------------------------------------------------------------
int Database::upsertStudies(const std::string& route, const std::vector<StudyInfo>& v) {
    std::lock_guard<std::mutex> lk(mtx_);
    const std::string now = nowTimestamp();
    int added = 0;
    exec("BEGIN");
    Stmt exists(db_, "SELECT 1 FROM studies WHERE route=? AND study_uid=?");
    // Se uno studio già COMPLETATO ha ora più immagini su A di quante ne abbiamo
    // verificate su B (immagini aggiunte dopo il trasferimento), torna in coda.
    Stmt up(db_,
        "INSERT INTO studies(route,study_uid,patient_id,patient_name,study_date,study_time,"
        "accession,description,modalities,instances_a,status,found_at,updated_at) "
        "VALUES(?,?,?,?,?,?,?,?,?,?,'PENDING',?,?) "
        "ON CONFLICT(route,study_uid) DO UPDATE SET "
        " patient_id=excluded.patient_id, patient_name=excluded.patient_name,"
        " study_date=excluded.study_date, study_time=excluded.study_time,"
        " accession=excluded.accession, description=excluded.description,"
        " modalities=excluded.modalities,"
        " status = CASE WHEN studies.status IN ('DONE','SKIPPED') AND excluded.instances_a > 0"
        "               AND studies.instances_b >= 0 AND excluded.instances_a > studies.instances_b"
        "          THEN 'PENDING' ELSE studies.status END,"
        " instances_a = CASE WHEN excluded.instances_a >= 0 THEN excluded.instances_a"
        "               ELSE studies.instances_a END,"
        " updated_at=excluded.updated_at");
    for (const auto& s : v) {
        exists.reset();
        exists.bind(1, route).bind(2, s.uid);
        if (!exists.step()) ++added;
        up.reset();
        up.bind(1, route).bind(2, s.uid).bind(3, s.patientId).bind(4, s.patientName)
          .bind(5, s.studyDate).bind(6, s.studyTime).bind(7, s.accession)
          .bind(8, s.description).bind(9, s.modalities).bind(10, s.numInstances)
          .bind(11, now).bind(12, now);
        up.run();
    }
    exec("COMMIT");
    return added;
}

int Database::resetRunning() {
    std::lock_guard<std::mutex> lk(mtx_);
    exec("UPDATE studies SET status='PENDING', last_error='interrotto (chiusura app)' "
         "WHERE status='RUNNING'");
    return sqlite3_changes(db_);
}

int Database::requeueFailed(const std::string& route, const std::string& from,
                            const std::string& to) {
    std::lock_guard<std::mutex> lk(mtx_);
    std::string sql = "UPDATE studies SET status='PENDING' WHERE route=? "
                      "AND status IN ('FAILED','PARTIAL')" + dateWhere(from, to);
    Stmt s(db_, sql.c_str());
    int i = 1;
    s.bind(i++, route);
    if (!from.empty()) s.bind(i++, from);
    if (!to.empty())   s.bind(i++, to);
    s.run();
    return sqlite3_changes(db_);
}

std::vector<StudyRow> Database::listStudies(const StudyFilter& f) const {
    std::lock_guard<std::mutex> lk(mtx_);
    std::string sql = std::string("SELECT ") + kStudyCols + " FROM studies WHERE route=?" +
                      dateWhere(f.dateFrom, f.dateTo);
    if (f.status >= 0) sql += " AND status=?";
    if (!f.text.empty())
        sql += " AND (patient_name LIKE ? ESCAPE '\\' OR patient_id LIKE ? ESCAPE '\\'"
               " OR description LIKE ? ESCAPE '\\' OR accession LIKE ? ESCAPE '\\')";
    sql += " ORDER BY study_date DESC, study_time DESC LIMIT ?";
    Stmt s(db_, sql.c_str());
    int i = 1;
    s.bind(i++, f.route);
    if (!f.dateFrom.empty()) s.bind(i++, f.dateFrom);
    if (!f.dateTo.empty())   s.bind(i++, f.dateTo);
    if (f.status >= 0)       s.bind(i++, std::string(statusCode(static_cast<StudyStatus>(f.status))));
    if (!f.text.empty()) {
        const std::string like = "%" + likeEscape(f.text) + "%";
        for (int k = 0; k < 4; ++k) s.bind(i++, like);
    }
    s.bind(i++, f.limit);
    std::vector<StudyRow> out;
    while (s.step()) out.push_back(readStudy(s));
    return out;
}

StatusCounts Database::countByStatus(const std::string& route, const std::string& from,
                                     const std::string& to) const {
    std::lock_guard<std::mutex> lk(mtx_);
    StatusCounts c{};
    std::string sql = "SELECT status, COUNT(*) FROM studies WHERE route=?" +
                      dateWhere(from, to) + " GROUP BY status";
    Stmt s(db_, sql.c_str());
    int i = 1;
    s.bind(i++, route);
    if (!from.empty()) s.bind(i++, from);
    if (!to.empty())   s.bind(i++, to);
    while (s.step()) c[static_cast<int>(statusFromCode(s.text(0)))] += s.integer(1);
    return c;
}

std::vector<StudyRow> Database::queueFor(const std::string& route, const std::string& from,
                                         const std::string& to, bool includeFailed) const {
    std::lock_guard<std::mutex> lk(mtx_);
    std::string sql = std::string("SELECT ") + kStudyCols + " FROM studies WHERE route=?" +
                      dateWhere(from, to) +
                      (includeFailed ? " AND status IN ('PENDING','FAILED','PARTIAL')"
                                     : " AND status='PENDING'") +
                      " ORDER BY study_date, study_time";
    Stmt s(db_, sql.c_str());
    int i = 1;
    s.bind(i++, route);
    if (!from.empty()) s.bind(i++, from);
    if (!to.empty())   s.bind(i++, to);
    std::vector<StudyRow> out;
    while (s.step()) out.push_back(readStudy(s));
    return out;
}

void Database::markRunning(const std::string& route, const std::string& uid) {
    std::lock_guard<std::mutex> lk(mtx_);
    Stmt s(db_, "UPDATE studies SET status='RUNNING', updated_at=? WHERE route=? AND study_uid=?");
    s.bind(1, nowTimestamp()).bind(2, route).bind(3, uid).run();
}

void Database::setInstancesA(const std::string& route, const std::string& uid, int n) {
    std::lock_guard<std::mutex> lk(mtx_);
    Stmt s(db_, "UPDATE studies SET instances_a=? WHERE route=? AND study_uid=?");
    s.bind(1, n).bind(2, route).bind(3, uid).run();
}

void Database::saveOutcome(const std::string& route, const std::string& uid, StudyStatus st,
                           int attempts, int completed, int failed, int warning, int dimse,
                           int instancesB, const std::string& error) {
    std::lock_guard<std::mutex> lk(mtx_);
    Stmt s(db_,
        "UPDATE studies SET status=?, attempts=attempts+?, completed=?, failed=?, warning=?,"
        " last_dimse=?, instances_b=CASE WHEN ?>=0 THEN ? ELSE instances_b END,"
        " last_error=?, updated_at=? WHERE route=? AND study_uid=?");
    s.bind(1, std::string(statusCode(st))).bind(2, attempts).bind(3, completed)
     .bind(4, failed).bind(5, warning).bind(6, dimse).bind(7, instancesB).bind(8, instancesB)
     .bind(9, error).bind(10, nowTimestamp()).bind(11, route).bind(12, uid).run();
}

void Database::addAttempt(const std::string& route, const AttemptRecord& a) {
    std::lock_guard<std::mutex> lk(mtx_);
    Stmt s(db_,
        "INSERT INTO attempts(route,study_uid,attempt_no,started_at,dimse,completed,failed,"
        "warning,instances_b,outcome,message,duration_ms) VALUES(?,?,?,?,?,?,?,?,?,?,?,?)");
    s.bind(1, route).bind(2, a.studyUid).bind(3, a.attemptNo).bind(4, a.startedAt)
     .bind(5, a.dimse).bind(6, a.completed).bind(7, a.failed).bind(8, a.warning)
     .bind(9, a.instancesB).bind(10, a.outcome).bind(11, a.message).bind(12, a.durationMs).run();
}

std::vector<AttemptRecord> Database::attemptsFor(const std::string& route,
                                                 const std::string& uid) const {
    std::lock_guard<std::mutex> lk(mtx_);
    Stmt s(db_,
        "SELECT study_uid,started_at,attempt_no,dimse,completed,failed,warning,instances_b,"
        "outcome,message,duration_ms FROM attempts WHERE route=? AND study_uid=? ORDER BY id");
    s.bind(1, route).bind(2, uid);
    std::vector<AttemptRecord> out;
    while (s.step()) {
        AttemptRecord a;
        a.studyUid   = s.text(0);
        a.startedAt  = s.text(1);
        a.attemptNo  = s.integer(2);
        a.dimse      = s.integer(3);
        a.completed  = s.integer(4);
        a.failed     = s.integer(5);
        a.warning    = s.integer(6);
        a.instancesB = s.integer(7);
        a.outcome    = s.text(8);
        a.message    = s.text(9);
        a.durationMs = s.int64(10);
        out.push_back(std::move(a));
    }
    return out;
}

bool Database::exportCsv(const StudyFilter& f, const std::string& path, std::string& err) const {
    StudyFilter all = f;
    all.limit = 1000000;
    const auto rows = listStudies(all);
    std::ofstream o(path);
    if (!o) { err = "impossibile scrivere " + path; return false; }
    o << "\xEF\xBB\xBF";   // BOM UTF-8 per Excel
    // il nome del paziente non viene esportato (il CSV circola più del database)
    o << "Data;Ora;PatientID;Accession;Descrizione;Modalita;ImgA;ImgB;Stato;"
         "Tentativi;Completate;Fallite;DIMSE;Errore;StudyInstanceUID;Aggiornato\n";
    for (const auto& r : rows) {
        char dimse[8];
        std::snprintf(dimse, sizeof dimse, "%04X", r.lastDimse);
        o << r.studyDate << ';' << r.studyTime << ';' << csvEscape(r.patientId) << ';'
          << csvEscape(r.accession) << ';' << csvEscape(r.description) << ';' << r.modalities << ';' << r.numInstances << ';'
          << r.instancesB << ';' << statusLabel(r.status) << ';' << r.attempts << ';'
          << r.completed << ';' << r.failed << ';' << dimse << ';' << csvEscape(r.lastError)
          << ';' << r.uid << ';' << r.updatedAt << '\n';
    }
    return true;
}

} // namespace p2p
