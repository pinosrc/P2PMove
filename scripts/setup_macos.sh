#!/usr/bin/env bash
# =============================================================================
#  setup_macos.sh — prepara l'ambiente di sviluppo C++ di P2PMove su Mac Apple Silicon
#  Idempotente: si può rilanciare senza danni.
# =============================================================================
set -euo pipefail

ok()   { printf "\033[32m✔ %s\033[0m\n" "$*"; }
info() { printf "\033[36m→ %s\033[0m\n" "$*"; }
warn() { printf "\033[33m! %s\033[0m\n" "$*"; }

# 1) Command Line Tools di Xcode (compilatore Apple Clang, SDK macOS, git, lldb)
if xcode-select -p >/dev/null 2>&1; then
  ok "Command Line Tools presenti: $(xcode-select -p)"
else
  info "Installo i Command Line Tools di Xcode (si apre una finestra di conferma)..."
  xcode-select --install || true
  warn "Rilancia questo script al termine dell'installazione."
  exit 0
fi

# 2) Homebrew
if ! command -v brew >/dev/null 2>&1; then
  info "Installo Homebrew..."
  /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
  eval "$(/opt/homebrew/bin/brew shellenv)"
  grep -q 'brew shellenv' "$HOME/.zprofile" 2>/dev/null || \
    echo 'eval "$(/opt/homebrew/bin/brew shellenv)"' >> "$HOME/.zprofile"
fi
ok "Homebrew: $(brew --version | head -1)"

# 3) Strumenti di build e librerie
info "Installo/aggiorno cmake, ninja, dcmtk, glfw..."
brew install cmake ninja dcmtk glfw
ok "cmake $(cmake --version | head -1 | awk '{print $3}'), ninja $(ninja --version), dcmtk $(brew list --versions dcmtk | awk '{print $2}')"

# 4) IDE (facoltativo): Visual Studio Code + estensioni
if ! command -v code >/dev/null 2>&1 && [ ! -d "/Applications/Visual Studio Code.app" ]; then
  read -r -p "Installare Visual Studio Code? [s/N] " r
  if [[ "${r:-n}" =~ ^[sS]$ ]]; then brew install --cask visual-studio-code; fi
fi
CODE_BIN="$(command -v code || echo "/Applications/Visual Studio Code.app/Contents/Resources/app/bin/code")"
if [ -x "$CODE_BIN" ]; then
  for ext in ms-vscode.cmake-tools llvm-vs-code-extensions.vscode-clangd vadimcn.vscode-lldb xaver.clang-format; do
    "$CODE_BIN" --install-extension "$ext" --force >/dev/null && ok "estensione $ext"
  done
fi

# 5) Verifica rapida della toolchain
info "Verifica: compilo un programma DCMTK di prova..."
TMP="$(mktemp -d)"
cat > "$TMP/t.cpp" <<'CPP'
#include "dcmtk/config/osconfig.h"
#include "dcmtk/dcmnet/scu.h"
int main() { DcmSCU scu; scu.setAETitle("TEST"); return 0; }
CPP
cat > "$TMP/CMakeLists.txt" <<'CM'
cmake_minimum_required(VERSION 3.24)
project(t CXX)
set(CMAKE_CXX_STANDARD 20)
list(APPEND CMAKE_PREFIX_PATH /opt/homebrew)
find_package(DCMTK REQUIRED CONFIG)
add_executable(t t.cpp)
target_include_directories(t PRIVATE ${DCMTK_INCLUDE_DIRS})
target_link_libraries(t PRIVATE DCMTK::dcmnet)
CM
cmake -S "$TMP" -B "$TMP/b" -G Ninja >/dev/null && cmake --build "$TMP/b" >/dev/null && "$TMP/b/t" \
  && ok "Toolchain C++ + DCMTK funzionante" || warn "Verifica fallita: vedere l'output di cmake in $TMP"
rm -rf "$TMP"

echo
ok "Ambiente pronto. Compilazione:"
echo "   cmake --preset debug && cmake --build --preset debug"
echo "   open build/debug/P2PMove.app"
