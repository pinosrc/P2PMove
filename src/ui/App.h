// =============================================================================
//  App.h — interfaccia grafica (Dear ImGui) di P2PMove
// =============================================================================
#pragma once

#include "core/Database.h"
#include "core/TransferEngine.h"
#include "core/Types.h"

#include <functional>
#include <future>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace p2p {

class App {
public:
    App();
    ~App();

    bool init(const std::string& dataDir, std::string& err);
    void frame();                         // disegna un fotogramma
    bool busy() const;                    // ricerca/trasferimento/test in corso
    bool confirmClose();                  // false = chiusura sospesa (popup di conferma)
    // l'utente ha confermato "Ferma e chiudi" e i worker hanno terminato
    bool readyToExit() const {
        return abandon_ || (forceClose_ && !engine_.running() && !scan_.running());
    }
    // true se all'uscita ci sono ancora thread che non conviene attendere (worker dopo
    // "Chiudi comunque", test di rete in corso): main() chiama allora fastExit()
    bool needsFastExit() const { return abandon_ || testsBusy(); }
    // rimette in coda gli studi RUNNING e termina il processo senza attendere i thread
    [[noreturn]] void fastExit();

private:
    // esito dei test asincroni sui nodi
    enum TestState { TNone = 0, TOk, TWarn, TErr, TRun };
    struct TestSlot {
        std::future<void>        fut;
        mutable std::mutex       m;
        int                      state = TNone;
        std::string              text;
        std::vector<std::string> steps;
        bool busy() const;
    };
    using TestFn = std::function<std::pair<int, std::string>(TestSlot&)>;

    bool testsBusy() const { return echoA_.busy() || echoB_.busy() || probe_.busy() || real_.busy(); }
    void runTest(TestSlot& slot, TestFn fn);
    void drawTestRow(const char* button, const char* help, TestSlot& slot, const TestFn& fn,
                     bool enabled);

    void drawNodesTab();
    void drawSearchTab();
    void drawTransferTab();
    void drawLogTab();
    void drawStatusBar();
    void drawDetailPopup();
    void drawClosePopup();
    bool drawNodeEditor(const char* id, NodeConfig& n, bool withHost);
    bool drawDateRange();

    void reloadStudies();
    void saveConfig();
    void flash(const std::string& msg, bool error = false);
    std::string rangeFromDicom() const;
    std::string rangeToDicom() const;

    std::string    dataDir_;
    Database       db_;
    AppConfig      cfg_;
    bool           cfgDirty_ = false;
    ScanJob        scan_;
    TransferEngine engine_;

    TestSlot echoA_, echoB_, probe_, real_;

    // intervallo di date (condiviso tra ricerca e trasferimento)
    std::string fromIso_, toIso_, modality_;

    // elenco studi
    std::vector<StudyRow> rows_;
    StatusCounts          counts_{};
    int                   statusFilter_ = -1;
    std::string           textFilter_;
    bool                  reloadNeeded_ = true;
    double                lastReload_   = 0;
    bool                  scanWasRunning_   = false;
    bool                  engineWasRunning_ = false;

    // dettaglio studio
    bool                       openDetail_ = false;
    StudyRow                   detail_;
    std::vector<AttemptRecord> detailAttempts_;

    // messaggio temporaneo nella barra di stato
    std::string flashMsg_;
    bool        flashErr_  = false;
    double      flashTime_ = -100;

    bool   closeRequested_ = false;
    bool   forceClose_     = false;
    bool   abandon_        = false;   // "Chiudi comunque": non attendere i worker
    int    logMinLevel_    = 1;
    bool   logAutoScroll_  = true;
    size_t logGen_         = 0;
};

} // namespace p2p
