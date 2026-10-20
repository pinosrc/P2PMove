// =============================================================================
//  main.cpp — avvio di P2PMove: GLFW + OpenGL3 + Dear ImGui
// =============================================================================
#include "core/Logger.h"
#include "ui/App.h"

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#if defined(__APPLE__)
#include <OpenGL/gl3.h>
#else
#include <GL/gl.h>
#endif

#include "dcmtk/config/osconfig.h"
#include "dcmtk/dcmdata/dcdict.h"
#include "dcmtk/oflog/oflog.h"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <glob.h>
#include <string>

namespace fs = std::filesystem;

namespace {

// Cartella dati dell'applicazione (DB, log, CSV, layout)
std::string dataDirectory() {
    const char* home = std::getenv("HOME");
    fs::path base = home ? fs::path(home) : fs::current_path();
#if defined(__APPLE__)
    base = base / "Library" / "Application Support" / "P2PMove";
#else
    base = base / ".local" / "share" / "p2pmove";
#endif
    std::error_code ec;
    fs::create_directories(base, ec);
    return base.string();
}

// DCMTK di Homebrew usa un dizionario DICOM esterno (dicom.dic). Se la variabile
// DCMDICTPATH non è impostata e il percorso compilato non esiste (es. dopo un
// aggiornamento di versione) le query falliscono con "no data dictionary loaded".
// Il dizionario viene caricato al primo utilizzo, quindi basta impostare la
// variabile prima di qualunque chiamata DCMTK.
void ensureDicomDictionary() {
#if defined(P2P_STANDALONE)
    return;   // versione indipendente: dizionario compilato dentro DCMTK
#endif
    if (std::getenv("DCMDICTPATH")) return;
    const char* patterns[] = {
        "/opt/homebrew/share/dcmtk*/dicom.dic",   // Homebrew Apple Silicon
        "/usr/local/share/dcmtk*/dicom.dic",      // Homebrew Intel / build manuale
        "/usr/share/libdcmtk*/dicom.dic",         // Debian/Ubuntu
    };
    for (const char* p : patterns) {
        glob_t g{};
        if (glob(p, 0, nullptr, &g) == 0 && g.gl_pathc > 0) {
            setenv("DCMDICTPATH", g.gl_pathv[g.gl_pathc - 1], 0);   // versione più recente
            globfree(&g);
            return;
        }
        globfree(&g);
    }
}

void applyStyle() {
    ImGuiStyle& st = ImGui::GetStyle();
    ImGui::StyleColorsDark();
    st.WindowRounding = 0;
    st.FrameRounding  = 4;
    st.GrabRounding   = 4;
    st.TabRounding    = 4;
    st.FramePadding   = ImVec2(8, 5);
    st.ItemSpacing    = ImVec2(8, 7);
    st.CellPadding    = ImVec2(6, 4);
    ImVec4* c = st.Colors;
    c[ImGuiCol_WindowBg]      = ImVec4(0.105f, 0.11f, 0.125f, 1.f);
    c[ImGuiCol_ChildBg]       = ImVec4(0.09f, 0.095f, 0.11f, 1.f);
    c[ImGuiCol_FrameBg]       = ImVec4(0.17f, 0.18f, 0.21f, 1.f);
    c[ImGuiCol_Header]        = ImVec4(0.20f, 0.33f, 0.52f, 0.55f);
    c[ImGuiCol_Button]        = ImVec4(0.20f, 0.36f, 0.60f, 0.85f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.26f, 0.44f, 0.72f, 1.f);
    c[ImGuiCol_PlotHistogram] = ImVec4(0.27f, 0.62f, 0.95f, 1.f);
}

// Carattere con lettere accentate, frecce e simboli; nitido su schermi Retina
void loadFonts(float scale) {
    ImGuiIO& io = ImGui::GetIO();
    static const ImWchar ranges[] = {
        0x0020, 0x00FF,   // Latino di base + Latino-1 (à è é ì ò ù)
        0x0100, 0x017F,   // Latino esteso-A
        0x2000, 0x206F,   // punteggiatura (— … •)
        0x2190, 0x21FF,   // frecce (→)
        0x25A0, 0x25FF,   // forme geometriche (●)
        0,
    };
    const char* candidates[] = {
        "/System/Library/Fonts/Supplemental/Arial Unicode.ttf",
        "/System/Library/Fonts/Supplemental/Arial.ttf",
        "/Library/Fonts/Arial Unicode.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    };
    const float size = 15.0f;
    for (const char* f : candidates) {
        if (fs::exists(f) && io.Fonts->AddFontFromFileTTF(f, size * scale, nullptr, ranges)) {
            io.FontGlobalScale = 1.0f / scale;
            return;
        }
    }
    ImFontConfig cfg;
    cfg.SizePixels = 13.0f * scale;
    io.Fonts->AddFontDefault(&cfg);
    io.FontGlobalScale = 1.0f / scale;
}

void glfwError(int code, const char* desc) {
    std::fprintf(stderr, "GLFW %d: %s\n", code, desc);
}

} // namespace

int main(int, char**) {
    // un peer che chiude il socket non deve terminare il processo
    std::signal(SIGPIPE, SIG_IGN);
    ensureDicomDictionary();
    OFLog::configure(OFLogger::WARN_LOG_LEVEL);   // log DCMTK: solo avvisi ed errori

    const std::string dataDir = dataDirectory();
    p2p::App app;
    std::string err;
    if (!app.init(dataDir, err)) {
        std::fprintf(stderr, "Impossibile inizializzare il database: %s\n", err.c_str());
        return 1;
    }
    if (!dcmDataDict.isDictionaryLoaded())
        p2p::logError("Dizionario DICOM non caricato: impostare DCMDICTPATH al file dicom.dic di DCMTK");

    glfwSetErrorCallback(glfwError);
    if (!glfwInit()) return 1;

#if defined(__APPLE__)
    const char* glsl = "#version 150";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#else
    const char* glsl = "#version 130";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
#endif

    GLFWwindow* window = glfwCreateWindow(1440, 920, "P2PMove — trasferimento PACS → PACS",
                                          nullptr, nullptr);
    if (!window) { glfwTerminate(); return 1; }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    static const std::string iniPath = dataDir + "/imgui.ini";
    io.IniFilename = iniPath.c_str();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    float xs = 1.f, ys = 1.f;
    glfwGetWindowContentScale(window, &xs, &ys);
    applyStyle();
    loadFonts(xs > 0 ? xs : 1.f);

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl);

    while (true) {
        // a riposo ridisegna 4 volte al secondo, durante le operazioni 20
        glfwWaitEventsTimeout(app.busy() ? 0.05 : 0.25);

        if (glfwWindowShouldClose(window)) {
            if (app.confirmClose()) break;
            glfwSetWindowShouldClose(window, GLFW_FALSE);
        }
        if (app.readyToExit()) break;

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        app.frame();
        ImGui::Render();

        int w = 0, h = 0;
        glfwGetFramebufferSize(window, &w, &h);
        glViewport(0, 0, w, h);
        glClearColor(0.1f, 0.1f, 0.12f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    p2p::logInfo("P2PMove chiuso");
    // thread ancora attivi (worker abbandonati, test di rete): il distruttore di App
    // li attenderebbe fino ai timeout di rete
    if (app.needsFastExit()) app.fastExit();
    return 0;
}
