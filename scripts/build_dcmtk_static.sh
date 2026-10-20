#!/usr/bin/env bash
# =============================================================================
#  build_dcmtk_static.sh — compila DCMTK come librerie STATICHE per la versione
#  indipendente di P2PMove (nessuna dipendenza da Homebrew a run-time).
#
#  - solo i moduli necessari: ofstd, oflog, oficonv, dcmdata, dcmtls (senza OpenSSL), dcmnet
#  - dizionario DICOM e tabelle dei set di caratteri COMPILATI DENTRO
#    (niente dicom.dic esterno, niente DCMDICTPATH)
#  - nessuna libreria esterna (OpenSSL, PNG, TIFF, XML...) salvo zlib di sistema
#
#  Risultato: third_party/dcmtk (header, .a e DCMTKConfig.cmake)
#  Durata: ~3-5 minuti su Mac M3. Si esegue una volta sola.
# =============================================================================
set -euo pipefail

VERSION="${DCMTK_VERSION:-3.6.9}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/third_party/src/dcmtk-$VERSION"
BUILD="$ROOT/third_party/build/dcmtk-$VERSION"
PREFIX="$ROOT/third_party/dcmtk"

if [ -f "$PREFIX/lib/cmake/dcmtk/DCMTKConfig.cmake" ] && [ "${1:-}" != "--force" ]; then
  echo "DCMTK statico già presente in $PREFIX (usa --force per ricompilare)"
  exit 0
fi

# Commit atteso per ogni versione: un tag git può essere spostato, l'hash del commit no.
# Per un'altra versione indicare DCMTK_VERSION e DCMTK_COMMIT (hash completo del tag).
case "$VERSION" in
  3.6.9) EXPECTED_COMMIT="${DCMTK_COMMIT:-ac002900cab167509881e5b837cdef5dcb07cd37}" ;;
  *)     EXPECTED_COMMIT="${DCMTK_COMMIT:-}" ;;
esac
if [ -z "$EXPECTED_COMMIT" ]; then
  echo "✘ DCMTK $VERSION: nessun commit noto. Impostare DCMTK_COMMIT=<hash del tag DCMTK-$VERSION>" >&2
  exit 1
fi

mkdir -p "$ROOT/third_party/src"
if [ ! -d "$SRC" ]; then
  echo "→ Scarico DCMTK $VERSION..."
  git -c advice.detachedHead=false clone --quiet --depth 1 --branch "DCMTK-$VERSION" https://github.com/DCMTK/dcmtk.git "$SRC"
fi

# i sorgenti (appena scaricati o già presenti) devono essere esattamente quel commit,
# senza modifiche ai file tracciati
ACTUAL_COMMIT="$(git -C "$SRC" rev-parse HEAD)"
if [ "$ACTUAL_COMMIT" != "$EXPECTED_COMMIT" ]; then
  echo "✘ DCMTK $VERSION: commit $ACTUAL_COMMIT diverso dall'atteso $EXPECTED_COMMIT." >&2
  echo "  Sorgenti non affidabili: eliminare $SRC e rieseguire." >&2
  exit 1
fi
if ! git -C "$SRC" diff --quiet HEAD; then
  echo "✘ DCMTK $VERSION: file tracciati modificati in $SRC. Eliminarlo e rieseguire." >&2
  exit 1
fi
echo "✔ DCMTK $VERSION verificato (commit ${EXPECTED_COMMIT:0:12})"

JOBS="$( (sysctl -n hw.ncpu 2>/dev/null || nproc) )"

# stesso minimo di macOS dell'app (CMakeLists.txt): senza, le librerie risultano compilate
# per l'SDK del Mac di build e l'app potrebbe non partire su macOS 13
OSX_ARGS=()
if [ "$(uname -s)" = "Darwin" ]; then
  OSX_ARGS=(-DCMAKE_OSX_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET:-13.0}")
fi
echo "→ Configuro DCMTK $VERSION (statico, dizionario integrato)..."
cmake -S "$SRC" -B "$BUILD" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  "${OSX_ARGS[@]}" \
  -DCMAKE_INSTALL_PREFIX="$PREFIX" \
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
  -DBUILD_SHARED_LIBS=OFF \
  -DBUILD_APPS=OFF \
  -DDCMTK_MODULES="ofstd;oflog;oficonv;dcmdata;dcmtls;dcmnet" \
  -DDCMTK_DEFAULT_DICT=builtin \
  -DDCMTK_USE_DCMDICTPATH=OFF \
  -DDCMTK_ENABLE_BUILTIN_OFICONV_DATA=ON \
  -DDCMTK_ENABLE_CHARSET_CONVERSION=oficonv \
  -DDCMTK_ENABLE_STL=ON \
  -DDCMTK_WITH_OPENSSL=OFF \
  -DDCMTK_WITH_PNG=OFF \
  -DDCMTK_WITH_TIFF=OFF \
  -DDCMTK_WITH_XML=OFF \
  -DDCMTK_WITH_ZLIB=ON \
  -DDCMTK_WITH_SNDFILE=OFF \
  -DDCMTK_WITH_ICONV=OFF \
  -DDCMTK_WITH_ICU=OFF \
  -DDCMTK_WITH_WRAP=OFF \
  -DDCMTK_WITH_OPENJPEG=OFF \
  -DDCMTK_WITH_DOXYGEN=OFF \
  > "$ROOT/third_party/dcmtk-configure.log"

echo "→ Compilo con $JOBS processi..."
cmake --build "$BUILD" --parallel "$JOBS" > "$ROOT/third_party/dcmtk-build.log"
cmake --install "$BUILD" > /dev/null
echo "✔ DCMTK statico installato in $PREFIX"
