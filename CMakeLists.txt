# =============================================================================
#  P2PMove — trasferimento esami DICOM PACS→PACS tramite C-MOVE delegato
#  Stack: C++20, DCMTK (DICOM), Dear ImGui + GLFW + OpenGL3 (GUI), SQLite (persistenza)
# =============================================================================
cmake_minimum_required(VERSION 3.24)

# Versione indipendente: DCMTK e GLFW statici, dizionario DICOM integrato,
# nessuna dipendenza da Homebrew a run-time (vedi scripts/make_release.sh)
option(P2P_STANDALONE "Applicazione indipendente (librerie statiche)" OFF)
if(P2P_STANDALONE AND APPLE AND NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET)
  # gira su macOS 13 Ventura e successivi (deve precedere project())
  set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "macOS minimo")
endif()

project(P2PMove VERSION 1.1.2 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)   # per clangd / IntelliSense

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

if(APPLE AND NOT P2P_STANDALONE)
  # Homebrew su Apple Silicon installa in /opt/homebrew
  list(APPEND CMAKE_PREFIX_PATH /opt/homebrew /usr/local)
  # Nessun CMAKE_OSX_DEPLOYMENT_TARGET: le librerie Homebrew sono compilate per la
  # versione di macOS installata; fissarne una più vecchia produce solo avvisi del linker.
endif()

include(FetchContent)

# --- dipendenze ----------------------------------------------------------------
if(P2P_STANDALONE)
  # DCMTK statico compilato da scripts/build_dcmtk_static.sh
  set(P2P_DCMTK_PREFIX "${CMAKE_SOURCE_DIR}/third_party/dcmtk")
  if(NOT EXISTS "${P2P_DCMTK_PREFIX}/lib/cmake/dcmtk/DCMTKConfig.cmake")
    message(FATAL_ERROR "DCMTK statico mancante: eseguire prima ./scripts/build_dcmtk_static.sh")
  endif()
  find_package(DCMTK REQUIRED CONFIG PATHS "${P2P_DCMTK_PREFIX}/lib/cmake/dcmtk" NO_DEFAULT_PATH)
else()
  find_package(DCMTK REQUIRED CONFIG)    # brew install dcmtk
endif()
find_package(SQLite3 REQUIRED)           # incluso nell'SDK macOS (libreria di sistema)
find_package(OpenGL REQUIRED)
find_package(Threads REQUIRED)

if(NOT P2P_STANDALONE)
  find_package(glfw3 3.3 QUIET)          # brew install glfw
endif()
if(NOT glfw3_FOUND)
  message(STATUS "GLFW: lo scarico e lo compilo come libreria statica")
  set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(GLFW_INSTALL        OFF CACHE BOOL "" FORCE)
  set(GLFW_LIBRARY_TYPE   STATIC CACHE STRING "" FORCE)
  set(GLFW_BUILD_WAYLAND  OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG        3.4
    GIT_SHALLOW    TRUE)
  FetchContent_MakeAvailable(glfw)
endif()

# --- Dear ImGui (non ha un CMakeLists: compiliamo i sorgenti necessari) -------
FetchContent_Declare(imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG        v1.91.5
  GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(imgui)

add_library(imgui STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/imgui_demo.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
  ${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp)
target_include_directories(imgui PUBLIC
  ${imgui_SOURCE_DIR}
  ${imgui_SOURCE_DIR}/backends)
target_link_libraries(imgui PUBLIC glfw OpenGL::GL)
if(APPLE)
  target_compile_definitions(imgui PUBLIC GL_SILENCE_DEPRECATION)
endif()

# --- libreria core (senza GUI: riutilizzabile per una futura versione CLI) ----
add_library(p2pcore STATIC
  src/core/Types.cpp
  src/core/Logger.cpp
  src/core/DicomClient.cpp
  src/core/Database.cpp
  src/core/TransferEngine.cpp)
target_include_directories(p2pcore PUBLIC src ${DCMTK_INCLUDE_DIRS})
target_link_libraries(p2pcore PUBLIC
  DCMTK::dcmnet DCMTK::dcmdata DCMTK::oflog DCMTK::ofstd
  SQLite::SQLite3 Threads::Threads)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(p2pcore PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()
target_compile_definitions(p2pcore PUBLIC P2P_VERSION="${PROJECT_VERSION}")
if(P2P_STANDALONE)
  target_compile_definitions(p2pcore PUBLIC P2P_STANDALONE=1)
endif()

# --- applicazione ---------------------------------------------------------------
option(P2P_MACOS_BUNDLE "Crea P2PMove.app su macOS" ON)

set(APP_SOURCES src/main.cpp src/ui/App.cpp)
if(APPLE AND P2P_MACOS_BUNDLE)
  set(APP_ICON ${CMAKE_SOURCE_DIR}/cmake/P2PMove.icns)
  set_source_files_properties(${APP_ICON} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
  add_executable(p2pmove MACOSX_BUNDLE ${APP_SOURCES} ${APP_ICON})
  set_target_properties(p2pmove PROPERTIES
    OUTPUT_NAME "P2PMove"
    MACOSX_BUNDLE_INFO_PLIST       ${CMAKE_SOURCE_DIR}/cmake/Info.plist.in
    MACOSX_BUNDLE_BUNDLE_NAME      "P2PMove"
    MACOSX_BUNDLE_GUI_IDENTIFIER   "local.p2pmove.app"
    MACOSX_BUNDLE_ICON_FILE        "P2PMove.icns"
    MACOSX_BUNDLE_BUNDLE_VERSION   ${PROJECT_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION})
else()
  add_executable(p2pmove ${APP_SOURCES})
endif()
target_link_libraries(p2pmove PRIVATE p2pcore imgui)

# --- strumento da riga di comando (stesso motore, nessuna GUI) ----------------
add_executable(p2pmove-cli src/cli/main_cli.cpp)
target_link_libraries(p2pmove-cli PRIVATE p2pcore)

# --- test automatici (senza rete né PACS): ctest --preset debug ---------------
option(P2P_BUILD_TESTS "Compila p2pmove-tests" ON)
if(P2P_BUILD_TESTS)
  enable_testing()
  add_executable(p2pmove-tests tests/test_main.cpp)
  target_link_libraries(p2pmove-tests PRIVATE p2pcore)
  add_test(NAME p2pmove-tests COMMAND p2pmove-tests)
endif()

# nella versione indipendente anche la CLI viaggia dentro l'app:
#   /Applications/P2PMove.app/Contents/MacOS/p2pmove-cli
if(APPLE AND P2P_MACOS_BUNDLE AND P2P_STANDALONE)
  add_custom_command(TARGET p2pmove POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:p2pmove-cli> $<TARGET_FILE_DIR:p2pmove>/p2pmove-cli
    COMMENT "Copio p2pmove-cli nel bundle")
  add_dependencies(p2pmove p2pmove-cli)
endif()
