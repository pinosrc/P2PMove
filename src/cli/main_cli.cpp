// =============================================================================
//  p2pmove-cli — stesso motore della GUI, da riga di comando
//
//  Usa la configurazione salvata dalla GUI (database in Application Support).
//  Utile per test, automazioni notturne (launchd/cron) e diagnosi.
//
//    p2pmove-cli echo                      C-ECHO verso A e B
//    p2pmove-cli probe                     A conosce B come destinazione C-MOVE?
//    p2pmove-cli test                      trasferisce 1 immagine reale A→B
//    p2pmove-cli scan  AAAA-MM-GG AAAA-MM-GG
//    p2pmove-cli move  AAAA-MM-GG AAAA-MM-GG [worker]
//    p2pmove-cli status AAAA-MM-GG AAAA-MM-GG
//    p2pmove-cli set chiave valore         es. set a.host 192.0.2.10
//
//  Opzione globale: --db PERCORSO  (database alternativo)
// =============================================================================
#include "core/DateUtil.h"
#include "core/DicomClient.h"
#include "core/Logger.h"
#include "core/TransferEngine.h"

#include "dcmtk/config/osconfig.h"
#include "dcmtk/oflog/oflog.h"

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <glob.h>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using namespace p2p;
namespace fs = std::filesystem;

namespace {

std::atomic<bool> gInterrupt{false};

std::string defaultDb() {
    const char* home = std::getenv("HOME");
    fs::path base = home ? fs::path(home) : fs::current_path();
#if defined(__APPLE__)
    base = base / "Library" / "Application Support" / "P2PMove";
#else
    base = base / ".local" / "share" / "p2pmove";
#endif
    std::error_code ec;
    fs::create_directories(base, ec);
    return (base / "p2pmove.db").string();
}

void ensureDicomDictionary() {
#if defined(P2P_STANDALONE)
    return;   // versione indipendente: dizionario compilato dentro DCMTK
#endif
    if (std::getenv("DCMDICTPATH")) return;
    for (const char* p : {"/opt/homebrew/share/dcmtk*/dicom.dic", "/usr/local/share/dcmtk*/dicom.dic",
                          "/usr/share/libdcmtk*/dicom.dic"}) {
        glob_t g{};
        if (glob(p, 0, nullptr, &g) == 0 && g.gl_pathc > 0) {
            setenv("DCMDICTPATH", g.gl_pathv[g.gl_pathc - 1], 0);
            globfree(&g);
            return;
        }
        globfree(&g);
    }
}

// chiavi scritte da Database::saveConfig (set non deve creare impostazioni inesistenti)
bool isKnownSetting(const std::string& key) {
    static const char* nodeFields[] = {"name", "aet", "host", "port", "type"};
    for (const char* n : {"local", "a", "b"})
        for (const char* f : nodeFields)
            if (key == std::string(n) + "." + f) return true;
    for (const char* k : {"net.connect", "net.acse", "net.find", "net.move", "net.pdu",
                          "x.workers", "x.retries", "x.delay", "x.verify", "x.skip",
                          "x.failed", "x.daybyday", "log.anonymize"})
        if (key == k) return true;
    return false;
}

int usage() {
    std::cerr << "Uso: p2pmove-cli [--db file] echo|probe|test|scan|move|status|set ...\n";
    return 2;
}

void printConfig(const AppConfig& c) {
    std::printf("Locale %s | A %s@%s:%d | B %s@%s:%d\n", c.local.aet.c_str(), c.a.aet.c_str(),
                c.a.host.c_str(), c.a.port, c.b.aet.c_str(), c.b.host.c_str(), c.b.port);
}

} // namespace

int main(int argc, char** argv) {
    std::signal(SIGPIPE, SIG_IGN);
    std::signal(SIGINT, [](int) { gInterrupt = true; });
    ensureDicomDictionary();
    OFLog::configure(OFLogger::WARN_LOG_LEVEL);

    std::vector<std::string> args(argv + 1, argv + argc);
    std::string dbPath = defaultDb();
    if (args.size() >= 2 && args[0] == "--db") { dbPath = args[1]; args.erase(args.begin(), args.begin() + 2); }
    if (args.empty()) return usage();

    Database db;
    std::string err;
    if (!db.open(dbPath, err)) { std::cerr << "DB: " << err << "\n"; return 1; }
    Logger::get().openFile(fs::path(dbPath).parent_path().string() + "/p2pmove.log");
    AppConfig cfg = db.loadConfig();
    const std::string& cmd = args[0];
    DicomClient client(cfg.local, cfg.net);

    if (cmd == "set" && args.size() == 3) {
        if (!isKnownSetting(args[1])) {
            std::cerr << "chiave sconosciuta: " << args[1] << "\n";
            return 2;
        }
        db.setSetting(args[1], args[2]);
        printConfig(db.loadConfig());
        return 0;
    }
    printConfig(cfg);

    if (cmd == "echo") {
        auto a = client.echo(cfg.a);
        auto b = client.echo(cfg.b);
        std::printf("A: %s\nB: %s\n", a.message.c_str(), b.message.c_str());
        return a.ok && b.ok ? 0 : 1;
    }
    if (cmd == "probe") {
        auto r = client.probeMoveDestination(cfg.a, cfg.b.aet);
        std::printf("%s\n", r.message.c_str());
        return r.verdict == ProbeResult::Verdict::Ok ? 0 : 1;
    }
    if (cmd == "test") {
        auto r = client.testRealTransfer(cfg.a, cfg.b, [](const std::string& s) {
            std::printf("  - %s\n", s.c_str());
        });
        std::printf("%s\n", r.message.c_str());
        return r.verdict == ProbeResult::Verdict::Ok ? 0 : 1;
    }
    if ((cmd == "scan" || cmd == "move" || cmd == "status") && args.size() >= 3) {
        Ymd f, t;
        if (!Ymd::parse(args[1], f) || !Ymd::parse(args[2], t)) { std::cerr << "date non valide\n"; return 2; }

        if (cmd == "status") {
            auto c = db.countByStatus(cfg.route(), f.dicom(), t.dicom());
            for (int i = 0; i < int(StudyStatus::Count); ++i)
                std::printf("%-12s %d\n", statusLabel(StudyStatus(i)), c[i]);
            return 0;
        }
        if (cmd == "scan") {
            ScanJob scan;
            if (!scan.start(cfg, &db, args[1], args[2], "", err)) { std::cerr << err << "\n"; return 1; }
            while (scan.running()) {
                if (gInterrupt) scan.stop();
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
            auto s = scan.state();
            std::printf("Studi trovati %d (nuovi %d), intervalli in errore %d %s\n", s.found, s.added,
                        s.dayErrors, s.error.c_str());
            return s.ok ? 0 : 1;
        }
        // move: solo qui gli studi RUNNING sono davvero orfani (la GUI li azzera al proprio avvio)
        db.resetRunning();
        if (args.size() >= 4) cfg.xfer.workers = std::max(1, std::atoi(args[3].c_str()));
        TransferEngine eng;
        if (!eng.start(cfg, &db, args[1], args[2], err)) { std::cerr << err << "\n"; return 1; }
        while (eng.running()) {
            if (gInterrupt) eng.stop();
            auto s = eng.snapshot();
            std::printf("\r  studi %d/%d  ok %d  parz %d  fall %d  saltati %d  img %ld  %.1f img/s   ",
                        s.processed, s.total, s.done, s.partial, s.failed, s.skipped, s.imagesDone,
                        s.imagesPerSec);
            std::fflush(stdout);
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        eng.join();
        auto s = eng.snapshot();
        std::printf("\nFine: %d completati, %d parziali, %d falliti, %d già su B, %ld immagini in %.1f s\n",
                    s.done, s.partial, s.failed, s.skipped, s.imagesDone, s.elapsedSec);
        for (const auto& r : s.recent)
            std::printf("  [%s] %s %s — %s\n", statusLabel(r.status), r.studyDate.c_str(),
                        r.patient.c_str(), r.message.c_str());
        return (s.failed == 0 && s.partial == 0) ? 0 : 1;
    }
    return usage();
}
