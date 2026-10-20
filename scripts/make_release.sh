#!/usr/bin/env bash
# =============================================================================
#  make_release.sh — crea la versione INDIPENDENTE di P2PMove per macOS
#
#    ./scripts/make_release.sh             build + verifica + DMG in dist/
#    ./scripts/make_release.sh --install   come sopra e copia in /Applications
#
#  Risultato:
#    dist/P2PMove-<versione>.dmg   (trascina P2PMove in Applicazioni)
#    dist/P2PMove.app              (bundle pronto)
#
#  L'app contiene DCMTK e GLFW compilati staticamente, il dizionario DICOM e le
#  tabelle dei set di caratteri: sul Mac di destinazione NON servono Homebrew,
#  dcmtk, glfw né Xcode. Usa solo librerie di sistema (SQLite, zlib, OpenGL, Cocoa).
# =============================================================================
set -euo pipefail

ok()   { printf "\033[32m✔ %s\033[0m\n" "$*"; }
info() { printf "\033[36m→ %s\033[0m\n" "$*"; }
fail() { printf "\033[31m✘ %s\033[0m\n" "$*"; exit 1; }

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
VERSION="$(sed -nE 's/^project\(P2PMove VERSION ([0-9.]+).*/\1/p' CMakeLists.txt)"
APP_BUILD="$ROOT/build/standalone/P2PMove.app"
DIST="$ROOT/dist"

for t in cmake ninja git codesign hdiutil otool; do
  command -v "$t" >/dev/null || fail "manca '$t' (eseguire scripts/setup_macos.sh)"
done

# 1) DCMTK statico (una volta sola)
info "DCMTK statico..."
bash "$ROOT/scripts/build_dcmtk_static.sh"

# 2) build Release indipendente
info "Compilo P2PMove $VERSION (Release, statico)..."
cmake --preset standalone >/dev/null
cmake --build --preset standalone

[ -d "$APP_BUILD" ] || fail "bundle non trovato: $APP_BUILD"

# 3) verifica: nessuna libreria fuori dal sistema
info "Verifico le dipendenze dinamiche..."
for bin in "$APP_BUILD/Contents/MacOS/P2PMove" "$APP_BUILD/Contents/MacOS/p2pmove-cli"; do
  bad="$(otool -L "$bin" | tail -n +2 | awk '{print $1}' | grep -vE '^(/usr/lib/|/System/Library/)' || true)"
  if [ -n "$bad" ]; then
    echo "$bad"
    fail "$(basename "$bin") dipende da librerie esterne al sistema"
  fi
done
ok "Solo librerie di sistema:"
otool -L "$APP_BUILD/Contents/MacOS/P2PMove" | tail -n +2 | awk '{print "    " $1}'

# 4) copia pulita in dist/ e firma ad-hoc (necessaria su Apple Silicon)
rm -rf "$DIST/P2PMove.app" "$DIST/dmg"
mkdir -p "$DIST"
ditto "$APP_BUILD" "$DIST/P2PMove.app"
xattr -cr "$DIST/P2PMove.app"
# l'eseguibile ausiliario va firmato prima del bundle che lo contiene
codesign --force --sign - --timestamp=none "$DIST/P2PMove.app/Contents/MacOS/p2pmove-cli"
codesign --force --sign - --timestamp=none "$DIST/P2PMove.app"
codesign --verify --deep --strict "$DIST/P2PMove.app" && ok "Firma ad-hoc valida"

# 5) immagine disco con collegamento ad Applicazioni
info "Creo il DMG..."
mkdir -p "$DIST/dmg"
ditto "$DIST/P2PMove.app" "$DIST/dmg/P2PMove.app"
ln -s /Applications "$DIST/dmg/Applications"
cat > "$DIST/dmg/LEGGIMI.txt" <<TXT
P2PMove $VERSION — trasferimento DICOM PACS → PACS (C-MOVE delegato)

Installazione: trascinare P2PMove nella cartella Applicazioni.

Primo avvio su un Mac diverso da quello di compilazione:
  se macOS dice che lo sviluppatore non può essere verificato, aprire
  Impostazioni di Sistema → Privacy e sicurezza → "Apri comunque",
  oppure da Terminale:  xattr -dr com.apple.quarantine /Applications/P2PMove.app

Al primo collegamento ai PACS consentire l'accesso alla "Rete locale".

Dati (configurazione, coda, registro):
  ~/Library/Application Support/P2PMove/

Riga di comando inclusa:
  /Applications/P2PMove.app/Contents/MacOS/p2pmove-cli echo
TXT
DMG="$DIST/P2PMove-$VERSION.dmg"
rm -f "$DMG"
hdiutil create -quiet -volname "P2PMove $VERSION" -srcfolder "$DIST/dmg" -ov -format UDZO "$DMG"
rm -rf "$DIST/dmg"
ok "DMG: $DMG ($(du -h "$DMG" | cut -f1))"

# 6) installazione facoltativa
if [ "${1:-}" = "--install" ]; then
  info "Installo in /Applications..."
  rm -rf "/Applications/P2PMove.app"
  ditto "$DIST/P2PMove.app" "/Applications/P2PMove.app"
  ok "Installata: /Applications/P2PMove.app"
  open "/Applications/P2PMove.app"
fi
