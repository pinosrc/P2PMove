// =============================================================================
//  App.cpp — interfaccia grafica
// =============================================================================
#include "App.h"

#include "core/DateUtil.h"
#include "core/DicomClient.h"
#include "core/Logger.h"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cstdio>
#include <cstdlib>

namespace p2p {

namespace {

// --- palette ---------------------------------------------------------------
const ImVec4 kOk    {0.30f, 0.78f, 0.42f, 1.f};
const ImVec4 kWarn  {0.96f, 0.72f, 0.20f, 1.f};
const ImVec4 kErr   {0.93f, 0.33f, 0.31f, 1.f};
const ImVec4 kRun   {0.35f, 0.62f, 0.98f, 1.f};
const ImVec4 kMuted {0.60f, 0.62f, 0.66f, 1.f};

ImVec4 statusColor(StudyStatus s) {
    switch (s) {
        case StudyStatus::Done:    return kOk;
        case StudyStatus::Skipped: return ImVec4(0.45f, 0.75f, 0.70f, 1.f);
        case StudyStatus::Partial: return kWarn;
        case StudyStatus::Failed:  return kErr;
        case StudyStatus::Running: return kRun;
        default:                   return kMuted;
    }
}

void dot(const ImVec4& col) {
    const float r = ImGui::GetTextLineHeight() * 0.32f;
    ImVec2 p = ImGui::GetCursorScreenPos();
    p.x += r + 2;
    p.y += ImGui::GetTextLineHeight() * 0.5f + ImGui::GetStyle().FramePadding.y;
    ImGui::GetWindowDrawList()->AddCircleFilled(p, r, ImGui::GetColorU32(col));
    ImGui::Dummy(ImVec2(r * 2 + 6, ImGui::GetFrameHeight()));
}

void helpMarker(const char* text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 36.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

std::string fmtDuration(double sec) {
    if (sec < 0) return "—";
    const long s = static_cast<long>(sec);
    char b[32];
    if (s >= 3600) std::snprintf(b, sizeof b, "%ld h %02ld min", s / 3600, (s % 3600) / 60);
    else if (s >= 60) std::snprintf(b, sizeof b, "%ld min %02ld s", s / 60, s % 60);
    else std::snprintf(b, sizeof b, "%ld s", s);
    return b;
}

std::string intOrDash(int v) { return v < 0 ? "—" : std::to_string(v); }

double now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

} // namespace

// =============================================================================
bool App::TestSlot::busy() const {
    return fut.valid() && fut.wait_for(std::chrono::seconds(0)) != std::future_status::ready;
}

App::App() {
    const Ymd t = Ymd::today();
    fromIso_ = t.addDays(-7).iso();
    toIso_   = t.iso();
}

App::~App() {
    scan_.stop();
    engine_.stop();
    engine_.join();
}

bool App::init(const std::string& dataDir, std::string& err) {
    dataDir_ = dataDir;
    Logger::get().openFile(dataDir + "/p2pmove.log");
    if (!db_.open(dataDir + "/p2pmove.db", err)) return false;
    cfg_ = db_.loadConfig();
    const int reset = db_.resetRunning();
    if (reset > 0) logWarn(reset, " studi interrotti alla chiusura precedente sono tornati in coda");
    fromIso_  = db_.getSetting("ui.from", fromIso_);
    toIso_    = db_.getSetting("ui.to", toIso_);
    modality_ = db_.getSetting("ui.modality", "");
    logInfo("P2PMove avviato — dati in ", dataDir);
    return true;
}

bool App::busy() const {
    return scan_.running() || engine_.running() || testsBusy();
}

void App::fastExit() {
    scan_.stop();
    engine_.stop();
    // gli studi ancora in corso tornano in coda, come dopo un crash
    const int n = db_.resetRunning();
    logWarn("Chiusura immediata: ", n, " studi rimessi in coda");
    std::fflush(nullptr);
    std::_Exit(0);
}

bool App::confirmClose() {
    if (!engine_.running() && !scan_.running()) return true;
    closeRequested_ = true;
    return false;
}

void App::saveConfig() {
    db_.saveConfig(cfg_);
    cfgDirty_ = false;
    reloadNeeded_ = true;
    flash("Configurazione salvata");
}

void App::flash(const std::string& msg, bool error) {
    flashMsg_  = msg;
    flashErr_  = error;
    flashTime_ = now();
    if (error) logError(msg); else logInfo(msg);
}

std::string App::rangeFromDicom() const { Ymd d; return Ymd::parse(fromIso_, d) ? d.dicom() : ""; }
std::string App::rangeToDicom()   const { Ymd d; return Ymd::parse(toIso_, d)   ? d.dicom() : ""; }

void App::reloadStudies() {
    StudyFilter f;
    f.route    = cfg_.route();
    f.dateFrom = rangeFromDicom();
    f.dateTo   = rangeToDicom();
    f.status   = statusFilter_;
    f.text     = textFilter_;
    rows_      = db_.listStudies(f);
    counts_    = db_.countByStatus(f.route, f.dateFrom, f.dateTo);
    lastReload_   = now();
    reloadNeeded_ = false;
}

// =============================================================================
//  Fotogramma
// =============================================================================
void App::frame() {
    // ricarica automatica dell'elenco quando terminano ricerca o trasferimento
    const bool scanRun = scan_.running(), engRun = engine_.running();
    if ((scanWasRunning_ && !scanRun) || (engineWasRunning_ && !engRun)) reloadNeeded_ = true;
    scanWasRunning_ = scanRun;
    engineWasRunning_ = engRun;
    if ((scanRun || engRun) && now() - lastReload_ > 2.0) reloadNeeded_ = true;
    if (reloadNeeded_) reloadStudies();

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("P2PMove", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);

    ImGui::TextColored(kRun, "P2PMove");
    ImGui::SameLine(0, 4);
    ImGui::TextDisabled("v%s", P2P_VERSION);
    ImGui::SameLine();
    ImGui::TextDisabled("trasferimento DICOM PACS → PACS tramite C-MOVE delegato");
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - 330 + ImGui::GetCursorPosX());
    ImGui::Text("%s  →  %s", cfg_.a.aet.c_str(), cfg_.b.aet.c_str());

    const float statusH = ImGui::GetFrameHeightWithSpacing() + 4;
    ImGui::BeginChild("main", ImVec2(0, -statusH));
    if (ImGui::BeginTabBar("tabs")) {
        if (ImGui::BeginTabItem("  Nodi e test  "))  { drawNodesTab();    ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("  Ricerca e coda  ")) { drawSearchTab(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("  Trasferimento  ")) { drawTransferTab(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("  Registro  "))     { drawLogTab();      ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
    ImGui::EndChild();
    drawStatusBar();
    drawDetailPopup();
    drawClosePopup();
    ImGui::End();
}

// =============================================================================
//  Tab 1 — Nodi e test
// =============================================================================
bool App::drawNodeEditor(const char* id, NodeConfig& n, bool withHost) {
    bool changed = false;
    ImGui::PushID(id);
    ImGui::PushItemWidth(-90);
    changed |= ImGui::InputText("Nome", &n.name);
    if (ImGui::InputText("AE Title", &n.aet, ImGuiInputTextFlags_CharsUppercase)) {
        if (n.aet.size() > 16) n.aet.resize(16);
        changed = true;
    }
    if (withHost) {
        changed |= ImGui::InputText("Host / IP", &n.host);
        changed |= ImGui::InputInt("Porta", &n.port, 0);
        int t = n.type;
        if (ImGui::BeginCombo("Tipo", pacsTypeName(t))) {
            for (int i = 0; i < static_cast<int>(PacsType::Count); ++i)
                if (ImGui::Selectable(pacsTypeName(i), i == t)) { n.type = i; changed = true; }
            ImGui::EndCombo();
        }
    }
    ImGui::PopItemWidth();
    std::string why;
    if (!n.valid(&why, withHost)) ImGui::TextColored(kErr, "%s", why.c_str());
    ImGui::PopID();
    return changed;
}

void App::runTest(TestSlot& slot, TestFn fn) {
    if (slot.busy()) return;
    {
        std::lock_guard<std::mutex> lk(slot.m);
        slot.state = TRun;
        slot.text  = "in corso...";
        slot.steps.clear();
    }
    slot.fut = std::async(std::launch::async, [&slot, fn] {
        std::pair<int, std::string> r;
        try {
            r = fn(slot);
        } catch (const std::exception& e) {
            r = {TErr, std::string("eccezione: ") + e.what()};
        }
        std::lock_guard<std::mutex> lk(slot.m);
        slot.state = r.first;
        slot.text  = r.second;
    });
}

void App::drawTestRow(const char* button, const char* help, TestSlot& slot, const TestFn& fn,
                      bool enabled) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::BeginDisabled(!enabled || slot.busy());
    if (ImGui::Button(button, ImVec2(-FLT_MIN, 0))) runTest(slot, fn);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", help);

    ImGui::TableNextColumn();
    std::lock_guard<std::mutex> lk(slot.m);
    const ImVec4 col = slot.state == TOk ? kOk : slot.state == TWarn ? kWarn
                     : slot.state == TErr ? kErr : slot.state == TRun ? kRun : kMuted;
    dot(col);
    ImGui::TableNextColumn();
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(slot.state == TNone ? "non eseguito" : slot.text.c_str());
    for (const auto& s : slot.steps) ImGui::TextDisabled("  • %s", s.c_str());
    ImGui::PopTextWrapPos();
}

void App::drawNodesTab() {
    ImGui::Spacing();
    if (ImGui::BeginTable("nodes", 3, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_PadOuterX)) {
        ImGui::TableSetupColumn("Questa applicazione (SCU)");
        ImGui::TableSetupColumn("PACS A — sorgente");
        ImGui::TableSetupColumn("PACS B — destinazione");
        ImGui::TableHeadersRow();
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        cfgDirty_ |= drawNodeEditor("local", cfg_.local, false);
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("Con il C-MOVE delegato l'app non riceve immagini: serve solo il "
                            "suo AE Title (Calling AE). Nessuna porta in ascolto.");
        ImGui::PopTextWrapPos();
        ImGui::TableNextColumn();
        cfgDirty_ |= drawNodeEditor("a", cfg_.a, true);
        ImGui::TableNextColumn();
        cfgDirty_ |= drawNodeEditor("b", cfg_.b, true);
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("L'AE Title di B deve coincidere con quello configurato nella "
                            "tabella dei nodi remoti di A (è la Move Destination).");
        ImGui::PopTextWrapPos();
        ImGui::EndTable();
    }

    if (ImGui::CollapsingHeader("Parametri di rete")) {
        ImGui::PushItemWidth(140);
        cfgDirty_ |= ImGui::InputInt("Timeout connessione TCP (s)", &cfg_.net.connectTimeoutSec);
        cfgDirty_ |= ImGui::InputInt("Timeout associazione ACSE (s)", &cfg_.net.acseTimeoutSec);
        cfgDirty_ |= ImGui::InputInt("Timeout risposte C-FIND (s)", &cfg_.net.findTimeoutSec);
        cfgDirty_ |= ImGui::InputInt("Timeout risposte C-MOVE (s)", &cfg_.net.moveTimeoutSec);
        helpMarker("Tempo massimo di silenzio tra due risposte C-MOVE. Alcuni PACS "
                   "(dcm4chee-arc senza 'pending', Horos) rispondono solo a fine studio: per "
                   "studi TC/RM grandi servono valori alti (3600 s).");
        cfgDirty_ |= ImGui::InputInt("Max PDU (byte)", &cfg_.net.maxPdu, 0);
        ImGui::PopItemWidth();
        cfg_.sanitize();
    }

    ImGui::Spacing();
    ImGui::BeginDisabled(!cfgDirty_);
    if (ImGui::Button("Salva configurazione")) saveConfig();
    ImGui::EndDisabled();
    if (cfgDirty_) { ImGui::SameLine(); ImGui::TextColored(kWarn, "modifiche non salvate"); }

    ImGui::SeparatorText("Suggerimenti di configurazione lato server");
    ImGui::PushTextWrapPos(0);
    ImGui::TextColored(kMuted, "A (%s): %s", pacsTypeName(cfg_.a.type), pacsTypeHint(cfg_.a.type));
    ImGui::TextColored(kMuted, "B (%s): deve accettare associazioni C-STORE dall'AE di A "
                       "(%s). %s", pacsTypeName(cfg_.b.type), cfg_.a.aet.c_str(),
                       cfg_.b.type == int(PacsType::Orthanc)
                           ? "Orthanc accetta C-STORE da AE sconosciuti se DicomAlwaysAllowStore è true (default)."
                           : "");
    ImGui::PopTextWrapPos();

    ImGui::SeparatorText("Test di connettività");
    const AppConfig c = cfg_;   // copia per i thread
    const bool aOk = c.a.valid() && c.local.valid(nullptr, false);
    const bool bOk = c.b.valid() && c.local.valid(nullptr, false);

    if (ImGui::BeginTable("tests", 3, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("btn", ImGuiTableColumnFlags_WidthFixed, 260);
        ImGui::TableSetupColumn("dot", ImGuiTableColumnFlags_WidthFixed, 22);
        ImGui::TableSetupColumn("esito", ImGuiTableColumnFlags_WidthStretch);

        drawTestRow("1. C-ECHO app → A", "Verifica che A sia raggiungibile e accetti l'AE di questa app.",
            echoA_, [c](TestSlot&) {
                auto r = DicomClient(c.local, c.net).echo(c.a);
                logInfo("C-ECHO ", c.a.aet, ": ", r.message);
                return std::make_pair(r.ok ? int(TOk) : int(TErr), r.message);
            }, aOk);

        drawTestRow("2. C-ECHO app → B", "Verifica che B sia raggiungibile (serve per la verifica a fine studio).",
            echoB_, [c](TestSlot&) {
                auto r = DicomClient(c.local, c.net).echo(c.b);
                logInfo("C-ECHO ", c.b.aet, ": ", r.message);
                return std::make_pair(r.ok ? int(TOk) : int(TErr), r.message);
            }, bOk);

        drawTestRow("3. Routing: A conosce B?",
            "Invia ad A un C-MOVE verso B per uno studio inesistente: nessun dato viene "
            "trasferito. Rosso (0xA801) = destinazione non configurata su A; giallo = "
            "nessun rifiuto, da confermare con il Test reale.",
            probe_, [c](TestSlot&) {
                auto r = DicomClient(c.local, c.net).probeMoveDestination(c.a, c.b.aet);
                logInfo("Sonda routing: ", r.message);
                const int st = r.verdict == ProbeResult::Verdict::Ok ? int(TOk)
                             : r.verdict == ProbeResult::Verdict::Indeterminate ? int(TWarn)
                             : int(TErr);
                return std::make_pair(st, r.message);
            }, aOk && c.b.valid());

        drawTestRow("4. Test reale A → B (1 immagine)",
            "Sceglie un'immagine recente su A, la trasferisce su B con C-MOVE e la "
            "ricerca su B con C-FIND. È la prova definitiva che A e B si vedono.",
            real_, [c](TestSlot& slot) {
                auto r = DicomClient(c.local, c.net).testRealTransfer(
                    c.a, c.b, [&slot](const std::string& s) {
                        std::lock_guard<std::mutex> lk(slot.m);
                        slot.steps.push_back(s);
                    });
                const int st = r.verdict == ProbeResult::Verdict::Ok ? int(TOk)
                             : r.verdict == ProbeResult::Verdict::Indeterminate ? int(TWarn)
                             : int(TErr);
                return std::make_pair(st, r.message);
            }, aOk && bOk);
        ImGui::EndTable();
    }
    if (cfgDirty_) ImGui::TextColored(kWarn, "I test usano i valori a video anche se non salvati.");
}

// =============================================================================
//  Tab 2 — Ricerca e coda
// =============================================================================
bool App::drawDateRange() {
    bool changed = false;
    Ymd d;
    auto dateField = [&](const char* label, const char* id, std::string& v) {
        const bool ok = Ymd::parse(v, d);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        ImGui::SameLine();
        if (!ok) ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.45f, 0.12f, 0.12f, 1.f));
        ImGui::SetNextItemWidth(120);
        if (ImGui::InputTextWithHint(id, "AAAA-MM-GG", &v)) changed = true;
        if (!ok) ImGui::PopStyleColor();
    };
    dateField("Dal", "##dal", fromIso_);
    ImGui::SameLine();
    dateField("al", "##al", toIso_);
    ImGui::SameLine();
    const Ymd t = Ymd::today();
    auto quick = [&](const char* label, Ymd a, Ymd b) {
        ImGui::SameLine();
        if (ImGui::SmallButton(label)) { fromIso_ = a.iso(); toIso_ = b.iso(); changed = true; }
    };
    ImGui::TextDisabled("|");
    quick("Oggi", t, t);
    quick("7 gg", t.addDays(-6), t);
    quick("30 gg", t.addDays(-29), t);
    quick("Anno corrente", Ymd{t.y, 1, 1}, t);
    quick("Anno scorso", Ymd{t.y - 1, 1, 1}, Ymd{t.y - 1, 12, 31});
    if (changed) {
        db_.setSetting("ui.from", fromIso_);
        db_.setSetting("ui.to", toIso_);
        reloadNeeded_ = true;
    }
    return changed;
}

void App::drawSearchTab() {
    ImGui::Spacing();
    drawDateRange();

    ImGui::SetNextItemWidth(120);
    if (ImGui::InputTextWithHint("Modalità", "es. CT, MR", &modality_,
                                 ImGuiInputTextFlags_CharsUppercase))
        db_.setSetting("ui.modality", modality_);
    helpMarker("Filtro facoltativo su ModalitiesInStudy. Non tutti i PACS lo supportano: "
               "lasciare vuoto per tutte le modalità.");
    ImGui::SameLine(0, 24);
    cfgDirty_ |= ImGui::Checkbox("Interroga un giorno alla volta", &cfg_.xfer.dayByDayQuery);
    helpMarker("Consigliato: evita i limiti sul numero massimo di risultati dei C-FIND "
               "(dcm4chee, Orthanc LimitFindResults) e rende la ricerca riprendibile.");

    const ScanJob::State ss = scan_.state();
    if (!ss.running) {
        if (ImGui::Button("Cerca studi su A", ImVec2(200, 0))) {
            std::string err;
            if (cfgDirty_) saveConfig();
            if (!scan_.start(cfg_, &db_, fromIso_, toIso_, modality_, err)) flash(err, true);
        }
    } else {
        if (ImGui::Button("Interrompi ricerca", ImVec2(200, 0))) scan_.stop();
    }
    if (ss.running || ss.finished) {
        ImGui::SameLine();
        const float f = ss.daysTotal > 0 ? float(ss.daysDone) / float(ss.daysTotal) : 0.f;
        char ov[160];
        std::snprintf(ov, sizeof ov, "%s %d/%d — %d studi trovati, %d nuovi%s", ss.running ? "Ricerca" : "Ricerca terminata",
                      ss.daysDone, ss.daysTotal, ss.found, ss.added,
                      ss.dayErrors ? " — CON ERRORI (vedi Registro)" : "");
        if (ss.dayErrors) ImGui::PushStyleColor(ImGuiCol_PlotHistogram, kErr);
        ImGui::ProgressBar(f, ImVec2(-FLT_MIN, 0), ov);
        if (ss.dayErrors) ImGui::PopStyleColor();
    }

    // --- riepilogo per stato ---------------------------------------------------
    ImGui::SeparatorText("Studi nell'intervallo (coda persistente)");
    int total = 0;
    for (int v : counts_) total += v;
    ImGui::Text("Totale: %d", total);
    for (int i = 0; i < static_cast<int>(StudyStatus::Count); ++i) {
        ImGui::SameLine(0, 18);
        ImGui::TextColored(statusColor(StudyStatus(i)), "%s: %d", statusLabel(StudyStatus(i)), counts_[i]);
    }

    // --- filtri e azioni -------------------------------------------------------
    ImGui::SetNextItemWidth(150);
    const char* cur = statusFilter_ < 0 ? "Tutti gli stati" : statusLabel(StudyStatus(statusFilter_));
    if (ImGui::BeginCombo("##stato", cur)) {
        if (ImGui::Selectable("Tutti gli stati", statusFilter_ < 0)) { statusFilter_ = -1; reloadNeeded_ = true; }
        for (int i = 0; i < static_cast<int>(StudyStatus::Count); ++i)
            if (ImGui::Selectable(statusLabel(StudyStatus(i)), statusFilter_ == i)) {
                statusFilter_ = i; reloadNeeded_ = true;
            }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(260);
    if (ImGui::InputTextWithHint("##txt", "cerca paziente, ID, accession, descrizione", &textFilter_))
        reloadNeeded_ = true;
    ImGui::SameLine();
    if (ImGui::Button("Aggiorna")) reloadNeeded_ = true;
    ImGui::SameLine();
    ImGui::BeginDisabled(engine_.running());
    if (ImGui::Button("Rimetti in coda falliti/parziali")) {
        const int n = db_.requeueFailed(cfg_.route(), rangeFromDicom(), rangeToDicom());
        flash(cat(n, " studi rimessi in coda"));
        reloadNeeded_ = true;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Esporta CSV")) {
        StudyFilter f;
        f.route = cfg_.route(); f.dateFrom = rangeFromDicom(); f.dateTo = rangeToDicom();
        f.status = statusFilter_; f.text = textFilter_;
        const std::string path = dataDir_ + "/studi_" + rangeFromDicom() + "_" + rangeToDicom() + ".csv";
        std::string err;
        if (db_.exportCsv(f, path, err)) flash("CSV esportato: " + path);
        else flash(err, true);
    }

    // --- tabella studi ---------------------------------------------------------
    const ImGuiTableFlags tf = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                               ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable |
                               ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_Hideable;
    if (ImGui::BeginTable("studies", 10, tf, ImVec2(0, ImGui::GetContentRegionAvail().y))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Data", 0, 80);
        ImGui::TableSetupColumn("Paziente", 0, 200);
        ImGui::TableSetupColumn("ID paziente", 0, 100);
        ImGui::TableSetupColumn("Descrizione", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Mod.", 0, 60);
        ImGui::TableSetupColumn("Img A", 0, 55);
        ImGui::TableSetupColumn("Img B", 0, 55);
        ImGui::TableSetupColumn("Stato", 0, 90);
        ImGui::TableSetupColumn("Tent.", 0, 40);
        ImGui::TableSetupColumn("Ultimo esito", 0, 280);
        ImGui::TableHeadersRow();

        ImGuiListClipper clip;
        clip.Begin(static_cast<int>(rows_.size()));
        while (clip.Step()) {
            for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
                const StudyRow& r = rows_[i];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::PushID(i);
                if (ImGui::Selectable(dicomDateToDisplay(r.studyDate).c_str(), false,
                                      ImGuiSelectableFlags_SpanAllColumns |
                                      ImGuiSelectableFlags_AllowDoubleClick)) {
                    detail_ = r;
                    detailAttempts_ = db_.attemptsFor(cfg_.route(), r.uid);
                    openDetail_ = true;
                }
                ImGui::PopID();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(r.patientName.c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(r.patientId.c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(r.description.c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(r.modalities.c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(intOrDash(r.numInstances).c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(intOrDash(r.instancesB).c_str());
                ImGui::TableNextColumn(); ImGui::TextColored(statusColor(r.status), "%s", statusLabel(r.status));
                ImGui::TableNextColumn(); ImGui::Text("%d", r.attempts);
                ImGui::TableNextColumn(); ImGui::TextUnformatted(r.lastError.c_str());
            }
        }
        ImGui::EndTable();
    }
    if (rows_.size() >= 20000) ImGui::TextColored(kWarn, "Mostrati i primi 20000 studi: restringere il filtro.");
}

void App::drawDetailPopup() {
    if (openDetail_) { ImGui::OpenPopup("Dettaglio studio"); openDetail_ = false; }
    ImGui::SetNextWindowSize(ImVec2(820, 460), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Dettaglio studio", nullptr)) {
        const StudyRow& r = detail_;
        ImGui::Text("%s  (%s)", r.patientName.c_str(), r.patientId.c_str());
        ImGui::TextDisabled("%s %s — %s — %s", dicomDateToDisplay(r.studyDate).c_str(),
                            r.studyTime.substr(0, std::min<size_t>(6, r.studyTime.size())).c_str(),
                            r.modalities.c_str(), r.description.c_str());
        ImGui::Text("Accession: %s", r.accession.c_str());
        ImGui::Text("StudyInstanceUID: %s", r.uid.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Copia")) ImGui::SetClipboardText(r.uid.c_str());
        ImGui::Text("Immagini su A: %s   su B: %s   Stato: ", intOrDash(r.numInstances).c_str(),
                    intOrDash(r.instancesB).c_str());
        ImGui::SameLine();
        ImGui::TextColored(statusColor(r.status), "%s", statusLabel(r.status));

        ImGui::SeparatorText("Tentativi");
        if (ImGui::BeginTable("att", 7, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_ScrollY, ImVec2(0, -ImGui::GetFrameHeightWithSpacing() - 6))) {
            ImGui::TableSetupColumn("#");
            ImGui::TableSetupColumn("Inizio");
            ImGui::TableSetupColumn("DIMSE");
            ImGui::TableSetupColumn("Ok/Fall.");
            ImGui::TableSetupColumn("Su B");
            ImGui::TableSetupColumn("Durata");
            ImGui::TableSetupColumn("Messaggio", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();
            for (const auto& a : detailAttempts_) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::Text("%d", a.attemptNo);
                ImGui::TableNextColumn(); ImGui::TextUnformatted(a.startedAt.c_str());
                ImGui::TableNextColumn(); ImGui::Text("%04X", a.dimse);
                ImGui::TableNextColumn(); ImGui::Text("%d / %d", a.completed, a.failed);
                ImGui::TableNextColumn(); ImGui::TextUnformatted(intOrDash(a.instancesB).c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(fmtDuration(a.durationMs / 1000.0).c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(a.message.c_str());
            }
            ImGui::EndTable();
        }
        if (ImGui::Button("Chiudi", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

// =============================================================================
//  Tab 3 — Trasferimento
// =============================================================================
void App::drawTransferTab() {
    const EngineSnapshot s = engine_.snapshot();
    const bool running = s.running;
    ImGui::Spacing();

    ImGui::BeginDisabled(running);
    drawDateRange();
    ImGui::SeparatorText("Modalità");
    int mode = cfg_.xfer.workers > 1 ? 1 : 0;
    if (ImGui::RadioButton("Sincrona — uno studio alla volta", mode == 0)) { cfg_.xfer.workers = 1; cfgDirty_ = true; }
    ImGui::SameLine(0, 30);
    if (ImGui::RadioButton("Asincrona — più C-MOVE in parallelo", mode == 1)) {
        cfg_.xfer.workers = std::max(2, cfg_.xfer.workers == 1 ? 4 : cfg_.xfer.workers);
        cfgDirty_ = true;
    }
    if (cfg_.xfer.workers > 1) {
        ImGui::SetNextItemWidth(250);
        cfgDirty_ |= ImGui::SliderInt("Associazioni parallele", &cfg_.xfer.workers, 2, 8);
        helpMarker("Ogni worker apre un'associazione C-MOVE verso A e A apre una connessione "
                   "C-STORE verso B. 3–4 è un buon compromesso: oltre, il collo di bottiglia "
                   "diventa il disco/CPU dei PACS. Verificare il numero massimo di associazioni "
                   "accettate da A e B.");
    }
    ImGui::SeparatorText("Controlli");
    cfgDirty_ |= ImGui::Checkbox("Verifica su B a fine studio (C-FIND conteggio immagini)", &cfg_.xfer.verifyOnB);
    cfgDirty_ |= ImGui::Checkbox("Salta gli studi già completi su B", &cfg_.xfer.skipIfPresentOnB);
    cfgDirty_ |= ImGui::Checkbox("Includi studi falliti/parziali delle sessioni precedenti", &cfg_.xfer.includeFailed);
    ImGui::SetNextItemWidth(100);
    cfgDirty_ |= ImGui::InputInt("Ritentativi automatici per studio", &cfg_.xfer.maxRetries);
    ImGui::SameLine(0, 30);
    ImGui::SetNextItemWidth(100);
    cfgDirty_ |= ImGui::InputInt("Attesa tra ritentativi (s)", &cfg_.xfer.retryDelaySec);
    cfg_.xfer.maxRetries    = std::clamp(cfg_.xfer.maxRetries, 0, 10);
    cfg_.xfer.retryDelaySec = std::clamp(cfg_.xfer.retryDelaySec, 0, 600);
    ImGui::EndDisabled();

    ImGui::Spacing();
    const int queued = counts_[int(StudyStatus::Pending)] +
                       (cfg_.xfer.includeFailed ? counts_[int(StudyStatus::Failed)] +
                                                  counts_[int(StudyStatus::Partial)] : 0);
    if (!running) {
        ImGui::BeginDisabled(queued == 0);
        if (ImGui::Button("Avvia trasferimento", ImVec2(220, 32))) {
            if (cfgDirty_) saveConfig();
            std::string err;
            if (!engine_.start(cfg_, &db_, fromIso_, toIso_, err)) flash(err, true);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (queued == 0) ImGui::TextColored(kMuted, "Nessuno studio in coda: eseguire prima 'Cerca studi su A'.");
        else             ImGui::Text("%d studi da trasferire nell'intervallo.", queued);
    } else {
        if (!s.paused) { if (ImGui::Button("Pausa", ImVec2(120, 32))) engine_.pause(true); }
        else           { if (ImGui::Button("Riprendi", ImVec2(120, 32))) engine_.pause(false); }
        ImGui::SameLine();
        ImGui::BeginDisabled(s.stopping);
        if (ImGui::Button("Ferma", ImVec2(120, 32))) engine_.stop();
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (s.stopping)    ImGui::TextColored(kWarn, "Arresto in corso: attendo la fine degli studi in corso (C-CANCEL inviato se possibile)...");
        else if (s.paused) ImGui::TextColored(kWarn, "In pausa: gli studi già avviati terminano, i successivi attendono.");
    }

    if (s.total == 0) return;

    // --- avanzamento globale --------------------------------------------------
    ImGui::SeparatorText("Avanzamento");
    char ov[200];
    std::snprintf(ov, sizeof ov, "Studi %d / %d", s.processed, s.total);
    ImGui::ProgressBar(s.total ? float(s.processed) / float(s.total) : 0.f, ImVec2(-FLT_MIN, 0), ov);
    if (s.imagesExpected > 0) {
        std::snprintf(ov, sizeof ov, "Immagini %ld / %ld", s.imagesDone, s.imagesExpected);
        ImGui::ProgressBar(std::min(1.f, float(s.imagesDone) / float(s.imagesExpected)),
                           ImVec2(-FLT_MIN, 0), ov);
    }
    ImGui::TextColored(kOk, "Completati %d", s.done);
    ImGui::SameLine(0, 20); ImGui::TextColored(statusColor(StudyStatus::Skipped), "Già su B %d", s.skipped);
    ImGui::SameLine(0, 20); ImGui::TextColored(kWarn, "Parziali %d", s.partial);
    ImGui::SameLine(0, 20); ImGui::TextColored(kErr, "Falliti %d", s.failed);
    ImGui::SameLine(0, 40);
    ImGui::Text("Tempo %s   Velocità %.1f img/s   Fine stimata tra %s",
                fmtDuration(s.elapsedSec).c_str(), s.imagesPerSec, fmtDuration(s.etaSec).c_str());

    // --- worker ----------------------------------------------------------------
    ImGui::SeparatorText("Associazioni attive");
    if (ImGui::BeginTable("workers", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                        ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("#", 0, 24);
        ImGui::TableSetupColumn("Fase", 0, 130);
        ImGui::TableSetupColumn("Studio", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Tent.", 0, 40);
        ImGui::TableSetupColumn("Avanzamento", 0, 260);
        ImGui::TableSetupColumn("Durata", 0, 90);
        ImGui::TableHeadersRow();
        for (const auto& w : s.workers) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%d", w.id);
            ImGui::TableNextColumn();
            ImGui::TextColored(w.busy ? kRun : kMuted, "%s", w.phase.empty() ? "—" : w.phase.c_str());
            ImGui::TableNextColumn();
            if (w.busy) ImGui::Text("%s  %s  %s", dicomDateToDisplay(w.studyDate).c_str(),
                                    w.patient.c_str(), w.description.c_str());
            ImGui::TableNextColumn(); if (w.busy) ImGui::Text("%d", w.attempt);
            ImGui::TableNextColumn();
            if (w.busy) {
                // totale: preferisce completate+fallite+rimanenti (dalle risposte pending)
                int tot = w.remaining >= 0 ? w.completed + w.failed + w.remaining : w.expected;
                if (tot <= 0) tot = w.expected;
                if (tot > 0) {
                    std::snprintf(ov, sizeof ov, "%d / %d%s", w.completed, tot,
                                  w.failed ? " (errori)" : "");
                    ImGui::ProgressBar(std::min(1.f, float(w.completed) / float(tot)), ImVec2(-FLT_MIN, 0), ov);
                } else {
                    ImGui::ProgressBar(-1.0f * float(ImGui::GetTime()), ImVec2(-FLT_MIN, 0), "in attesa di risposta");
                }
            }
            ImGui::TableNextColumn(); if (w.busy) ImGui::TextUnformatted(fmtDuration(w.seconds).c_str());
        }
        ImGui::EndTable();
    }

    // --- risultati recenti -----------------------------------------------------
    ImGui::SeparatorText("Dettaglio trasferimenti (più recenti in alto)");
    if (ImGui::BeginTable("recent", 8, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                          ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit |
                          ImGuiTableFlags_Resizable, ImVec2(0, ImGui::GetContentRegionAvail().y))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Ora", 0, 70);
        ImGui::TableSetupColumn("Data", 0, 80);
        ImGui::TableSetupColumn("Paziente", 0, 200);
        ImGui::TableSetupColumn("Esito", 0, 90);
        ImGui::TableSetupColumn("Img A/B", 0, 80);
        ImGui::TableSetupColumn("Tent.", 0, 40);
        ImGui::TableSetupColumn("Durata", 0, 80);
        ImGui::TableSetupColumn("Messaggio", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        ImGuiListClipper clip;
        clip.Begin(static_cast<int>(s.recent.size()));
        while (clip.Step()) {
            for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
                const ResultView& r = s.recent[i];
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(r.time.c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(dicomDateToDisplay(r.studyDate).c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(r.patient.c_str());
                ImGui::TableNextColumn(); ImGui::TextColored(statusColor(r.status), "%s", statusLabel(r.status));
                ImGui::TableNextColumn(); ImGui::Text("%s / %s", intOrDash(r.expected).c_str(), intOrDash(r.onB).c_str());
                ImGui::TableNextColumn(); ImGui::Text("%d", r.attempts);
                ImGui::TableNextColumn(); ImGui::TextUnformatted(fmtDuration(r.seconds).c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(r.message.c_str());
            }
        }
        ImGui::EndTable();
    }
}

// =============================================================================
//  Tab 4 — Registro
// =============================================================================
void App::drawLogTab() {
    ImGui::Spacing();
    const char* levels[] = {"Debug", "Info", "Avvisi", "Errori"};
    ImGui::SetNextItemWidth(120);
    ImGui::Combo("Livello minimo", &logMinLevel_, levels, 4);
    ImGui::SameLine();
    ImGui::Checkbox("Scorrimento automatico", &logAutoScroll_);
    ImGui::SameLine();
    if (ImGui::Button("Pulisci")) Logger::get().clear();
    ImGui::SameLine();
    ImGui::TextDisabled("File: %s", Logger::get().filePath().c_str());
    if (ImGui::Checkbox("Non registrare i nomi dei pazienti", &cfg_.logAnonymize))
        db_.setSetting("log.anonymize", cfg_.logAnonymize ? "1" : "0");
    helpMarker("Nel registro compare l'UID dello studio al posto del nome. Vale dal "
               "prossimo avvio del trasferimento. Il registro è ruotato automaticamente "
               "(5 file da 5 MB).");

    ImGui::BeginChild("log", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
    const auto entries = Logger::get().snapshot();
    for (const auto& e : entries) {
        if (static_cast<int>(e.level) < logMinLevel_) continue;
        const ImVec4 col = e.level == LogLevel::Error ? kErr : e.level == LogLevel::Warn ? kWarn
                         : e.level == LogLevel::Debug ? kMuted : ImGui::GetStyleColorVec4(ImGuiCol_Text);
        ImGui::TextColored(kMuted, "%s", e.time.c_str() + 11);
        ImGui::SameLine();
        ImGui::TextColored(col, "%s", e.text.c_str());
    }
    const size_t gen = Logger::get().generation();
    if (logAutoScroll_ && gen != logGen_) ImGui::SetScrollHereY(1.0f);
    logGen_ = gen;
    ImGui::EndChild();
}

// =============================================================================
//  Barra di stato e chiusura
// =============================================================================
void App::drawStatusBar() {
    ImGui::Separator();
    const EngineSnapshot s = engine_.snapshot();
    if (s.running) {
        ImGui::TextColored(kRun, "● Trasferimento %s: %d/%d studi", s.paused ? "in pausa" : "in corso",
                           s.processed, s.total);
    } else if (scan_.running()) {
        ImGui::TextColored(kRun, "● Ricerca su A in corso");
    } else {
        ImGui::TextColored(kMuted, "● Inattivo");
    }
    if (now() - flashTime_ < 8.0) {
        ImGui::SameLine(0, 30);
        ImGui::TextColored(flashErr_ ? kErr : kOk, "%s", flashMsg_.c_str());
    }
}

void App::drawClosePopup() {
    if (closeRequested_) { ImGui::OpenPopup("Chiudere?"); closeRequested_ = false; }
    if (ImGui::BeginPopupModal("Chiudere?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Un trasferimento o una ricerca è in corso.");
        ImGui::TextUnformatted("Fermare e chiudere? Gli studi interrotti torneranno in coda.");
        if (ImGui::Button("Ferma e chiudi", ImVec2(160, 0))) {
            scan_.stop();
            engine_.stop();
            ImGui::CloseCurrentPopup();
            // la chiusura effettiva avviene in main() quando i worker hanno terminato
            closeRequested_ = false;
            flash("Arresto in corso, chiusura appena terminati gli studi attivi...");
            forceClose_ = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Chiudi comunque", ImVec2(160, 0))) {
            // senza attendere i C-MOVE in corso (un PACS silenzioso può trattenerli a lungo)
            abandon_ = true;
            ImGui::CloseCurrentPopup();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Esce subito. Gli studi in corso tornano in coda; A potrebbe\n"
                              "completare comunque l'invio già avviato verso B.");
        ImGui::SameLine();
        if (ImGui::Button("Annulla", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

} // namespace p2p
