// =============================================================================
//  p2pmove-tests — test senza rete né PACS (eseguire con: ctest --preset debug)
//  Nessun framework: CHECK conta i fallimenti e il processo esce con codice != 0.
// =============================================================================
#include "core/Database.h"
#include "core/DateUtil.h"
#include "core/DicomClient.h"
#include "core/Logger.h"
#include "core/TransferEngine.h"
#include "core/Types.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

using namespace p2p;
namespace fs = std::filesystem;

static int gFailures = 0;
static int gChecks   = 0;

#define CHECK(cond)                                                                      \
    do {                                                                                 \
        ++gChecks;                                                                       \
        if (!(cond)) {                                                                   \
            ++gFailures;                                                                 \
            std::fprintf(stderr, "FALLITO %s:%d  %s\n", __FILE__, __LINE__, #cond);      \
        }                                                                                \
    } while (0)

namespace {

// cartella temporanea per test, ripulita all'uscita
struct TempDir {
    fs::path path;
    TempDir() {
        path = fs::temp_directory_path() / ("p2pmove-test-" + std::to_string(std::rand()));
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~TempDir() { std::error_code ec; fs::remove_all(path, ec); }
    std::string file(const std::string& n) const { return (path / n).string(); }
};

StudyInfo study(const std::string& uid, const std::string& date, const std::string& name,
                int n = 10) {
    StudyInfo s;
    s.uid          = uid;
    s.studyDate    = date;
    s.patientName  = name;
    s.patientId    = "ID-" + uid;
    s.numInstances = n;
    return s;
}

std::string slurp(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// ----------------------------------------------------------------------------
void testDates() {
    Ymd d;
    CHECK(Ymd::parse("2024-02-29", d) && d.y == 2024 && d.m == 2 && d.d == 29);
    CHECK(Ymd::parse("20240229", d));
    CHECK(!Ymd::parse("2023-02-29", d));       // anno non bisestile
    CHECK(!Ymd::parse("2024-04-31", d));
    CHECK(!Ymd::parse("2024-01-1x", d));       // testo finale
    CHECK(!Ymd::parse(" 024-01-01", d));       // spazio iniziale
    CHECK(!Ymd::parse("+024-01-01", d));       // segno
    CHECK(!Ymd::parse("2024/01/01", d));
    CHECK(!Ymd::parse("2024-1-01", d));
    CHECK(!Ymd::parse("", d));
    CHECK(!Ymd::parse("1899-12-31", d));

    Ymd a{2024, 12, 31};
    CHECK(a.addDays(1).iso() == "2025-01-01");
    CHECK(a.addDays(-366).iso() == "2023-12-31");
    for (long z : {-1000L, 0L, 19000L, 25000L}) CHECK(Ymd::fromDays(z).toDays() == z);
    CHECK(dicomDateToDisplay("20240131") == "31/01/2024");
}

void testTypes() {
    NodeConfig n{"x", "AE", "host", 104, 0};
    CHECK(n.valid());
    n.aet = std::string(17, 'A');
    CHECK(!n.valid());
    n.aet = "AE"; n.port = 70000;
    CHECK(!n.valid());
    n.port = 104; n.host.clear();
    CHECK(!n.valid());
    CHECK(n.valid(nullptr, false));

    for (int i = 0; i < int(StudyStatus::Count); ++i)
        CHECK(int(statusFromCode(statusCode(StudyStatus(i)))) == i);

    AppConfig c;
    c.net.maxPdu = 0;
    c.net.moveTimeoutSec = -5;
    c.net.connectTimeoutSec = 99999;
    c.xfer.workers = 500;
    c.xfer.maxRetries = -3;
    c.xfer.retryDelaySec = 100000;
    c.sanitize();
    CHECK(c.net.maxPdu == 4096);
    CHECK(c.net.moveTimeoutSec == 30);
    CHECK(c.net.connectTimeoutSec == 300);
    CHECK(c.xfer.workers == 16);
    CHECK(c.xfer.maxRetries == 0);
    CHECK(c.xfer.retryDelaySec == 600);
}

void testDatabase() {
    TempDir td;
    Database db;
    std::string err;
    CHECK(!db.open("/nonexistent-dir-p2pmove/x.db", err));
    CHECK(!err.empty());

    CHECK(db.open(td.file("t.db"), err));
    const std::string R = "A>B";

    // inserimento e conteggio dei nuovi
    CHECK(db.upsertStudies(R, {study("1", "20260101", "ROSSI MARIO"),
                               study("2", "20260102", "BIANCHI ANNA"),
                               study("3", "20260103", "100% LATTE")}) == 3);
    CHECK(db.upsertStudies(R, {study("1", "20260101", "ROSSI MARIO")}) == 0);

    // filtro testo: % e _ sono caratteri normali
    StudyFilter f;
    f.route = R;
    f.text  = "%";
    CHECK(db.listStudies(f).size() == 1);      // solo "100% LATTE"
    f.text  = "_";
    CHECK(db.listStudies(f).empty());
    f.text  = "rossi";
    CHECK(db.listStudies(f).size() == 1);
    f.text.clear();
    CHECK(db.listStudies(f).size() == 3);

    // studio completato con più immagini su A di quante verificate su B torna in coda
    db.saveOutcome(R, "1", StudyStatus::Done, 1, 10, 0, 0, 0, 10, "ok");
    db.upsertStudies(R, {study("1", "20260101", "ROSSI MARIO", 10)});
    CHECK(db.countByStatus(R, "", "")[int(StudyStatus::Done)] == 1);
    db.upsertStudies(R, {study("1", "20260101", "ROSSI MARIO", 14)});
    CHECK(db.countByStatus(R, "", "")[int(StudyStatus::Pending)] == 3);

    // RUNNING azzerato, FAILED/PARTIAL rimessi in coda
    db.markRunning(R, "2");
    CHECK(db.resetRunning() == 1);
    db.saveOutcome(R, "2", StudyStatus::Failed, 1, 0, 0, 0, 0, -1, "x");
    db.saveOutcome(R, "3", StudyStatus::Partial, 1, 5, 2, 0, 0, 5, "y");
    CHECK(db.queueFor(R, "", "", false).size() == 1);   // solo lo studio 1
    CHECK(db.queueFor(R, "", "", true).size() == 3);
    CHECK(db.requeueFailed(R, "", "") == 2);
    CHECK(db.queueFor(R, "", "", false).size() == 3);

    // CSV: celle che sembrano formule vengono neutralizzate
    StudyInfo formula = study("4", "20260104", "VERDI GIULIA");
    formula.description = "=CMD|' /C calc'!A0";
    db.upsertStudies(R, {formula});
    const std::string csv = td.file("o.csv");
    f.text.clear();
    CHECK(db.exportCsv(f, csv, err));
    const std::string body = slurp(csv);
    CHECK(body.find("\"'=CMD") != std::string::npos);
    CHECK(body.find("\"=CMD") == std::string::npos);
    CHECK(body.rfind("\xEF\xBB\xBF", 0) == 0);           // BOM per Excel

    // CSV: nessun nome di paziente, né nell'intestazione né nelle righe
    CHECK(body.find("Paziente") == std::string::npos);
    for (const char* name : {"ROSSI", "BIANCHI", "LATTE", "VERDI"})
        CHECK(body.find(name) == std::string::npos);
    CHECK(body.find("ID-1") != std::string::npos);        // il PatientID resta
    CHECK(body.find("20260101") != std::string::npos);
}

void testConfigRoundTrip() {
    TempDir td;
    Database db;
    std::string err;
    CHECK(db.open(td.file("c.db"), err));

    AppConfig c = db.loadConfig();
    c.a.aet = "SRC";  c.a.host = "10.1.1.1"; c.a.port = 4242;
    c.b.aet = "DST";
    c.net.moveTimeoutSec = 7200;
    c.xfer.workers = 4;
    c.xfer.skipIfPresentOnB = true;
    c.logAnonymize = true;
    db.saveConfig(c);

    const AppConfig l = db.loadConfig();
    CHECK(l.a.aet == "SRC" && l.a.host == "10.1.1.1" && l.a.port == 4242);
    CHECK(l.b.aet == "DST");
    CHECK(l.net.moveTimeoutSec == 7200);
    CHECK(l.xfer.workers == 4 && l.xfer.skipIfPresentOnB);
    CHECK(l.logAnonymize);
    CHECK(l.route() == "SRC>DST");

    // valori fuori limite scritti direttamente (come farebbe la CLI) vengono riportati nei limiti
    db.setSetting("net.pdu", "0");
    db.setSetting("x.workers", "9999");
    db.setSetting("x.retries", "abc");   // non numerico: default
    const AppConfig s = db.loadConfig();
    CHECK(s.net.maxPdu == 4096);
    CHECK(s.xfer.workers == 16);
    CHECK(s.xfer.maxRetries == TransferSettings{}.maxRetries);
}

void testLoggerRotation() {
    TempDir td;
    const std::string path = td.file("test.log");
    Logger& lg = Logger::get();
    lg.setRotation(2000, 3);
    lg.openFile(path);
    for (int i = 0; i < 200; ++i) lg.write(LogLevel::Info, std::string(60, 'x') + std::to_string(i));
    CHECK(fs::exists(path));
    CHECK(fs::exists(path + ".1"));
    CHECK(fs::exists(path + ".2"));
    CHECK(!fs::exists(path + ".3"));                    // al massimo 3 file in tutto
    CHECK(fs::file_size(path) <= 2000 + 200);
    // l'ultima riga è nel file corrente
    CHECK(slurp(path).find("x199") != std::string::npos);
    lg.setRotation(5 * 1024 * 1024, 5);
}

void testOutcomes() {
    MoveResult ok;
    ok.associationOk = true; ok.status = dimse::Success; ok.completed = 10; ok.responses = 5;
    CHECK(evaluateMove(ok, 10, false, false, 0).status == StudyStatus::Done);

    MoveResult noAssoc;
    noAssoc.error = "rifiutata";
    auto o = evaluateMove(noAssoc, 10, false, false, 0);
    CHECK(o.status == StudyStatus::Failed && o.message == "rifiutata");

    MoveResult cancelled = ok;
    cancelled.cancelled = true;
    CHECK(evaluateMove(cancelled, 10, false, false, 0).status == StudyStatus::Pending);

    MoveResult warn = ok;
    warn.status = dimse::SubOpsWarning; warn.failed = 2; warn.completed = 8;
    CHECK(evaluateMove(warn, 10, false, false, 0).status == StudyStatus::Partial);

    MoveResult allFail = warn;
    allFail.completed = 0; allFail.failed = 10;
    CHECK(evaluateMove(allFail, 10, false, false, 0).status == StudyStatus::Failed);

    // association caduta a metà: stato pending, errore, ma immagini già inviate -> parziale
    MoveResult dropped;
    dropped.associationOk = true; dropped.status = dimse::Pending; dropped.completed = 120;
    dropped.error = "C-MOVE: Peer aborted Association";
    o = evaluateMove(dropped, 500, false, false, 0);
    CHECK(o.status == StudyStatus::Partial);
    CHECK(o.message.find("120 immagini già inviate") != std::string::npos);

    // stessa caduta senza nessuna immagine inviata -> fallito
    dropped.completed = 0;
    CHECK(evaluateMove(dropped, 500, false, false, 0).status == StudyStatus::Failed);

    // nessuna serie mancante su B
    MoveResult none = ok;
    none.completed = 0;
    CHECK(evaluateMove(none, 10, true, false, 0).message == "nessuna serie mancante su B");

    // verifica su B
    MoveOutcome v{StudyStatus::Done, "x"};
    applyVerification(v, 7, 10, "");
    CHECK(v.status == StudyStatus::Partial);
    v = {StudyStatus::Partial, "x"};
    applyVerification(v, 10, 10, "");
    CHECK(v.status == StudyStatus::Done);
    v = {StudyStatus::Done, "x"};
    applyVerification(v, -1, 10, "timeout");
    CHECK(v.status == StudyStatus::Done && v.message.find("timeout") != std::string::npos);
    v = {StudyStatus::Done, "x"};
    applyVerification(v, 0, -1, "");
    CHECK(v.status == StudyStatus::Failed);
}

} // namespace

int main() {
    testDates();
    testTypes();
    testDatabase();
    testConfigRoundTrip();
    testLoggerRotation();
    testOutcomes();
    std::printf("%d verifiche, %d fallite\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
