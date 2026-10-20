#!/usr/bin/env bash
# =============================================================================
#  laboratorio_pacs.sh — due PACS di prova sul Mac (dcmqrscp di DCMTK) per
#  provare P2PMove senza toccare i PACS dello studio.
#
#    ./scripts/laboratorio_pacs.sh start  /cartella/con/file/dicom
#    ./scripts/laboratorio_pacs.sh stop
#
#  LABA (porta 11201) = sorgente, conosce LABB come destinazione C-MOVE
#  LABB (porta 11202) = destinazione
#  Poi in P2PMove: A = LABA / 127.0.0.1 / 11201, B = LABB / 127.0.0.1 / 11202
# =============================================================================
set -euo pipefail
LAB="${TMPDIR:-/tmp}/p2pmove-lab"
cmd="${1:-}"

stop() {
  if [ -f "$LAB/pids" ]; then
    while read -r p; do kill "$p" 2>/dev/null || true; done < "$LAB/pids"
    rm -f "$LAB/pids"
  fi
  echo "Laboratorio fermato."
}

case "$cmd" in
  start)
    SRC="${2:?indicare la cartella con i file DICOM da caricare su LABA}"
    stop >/dev/null
    mkdir -p "$LAB/dbA" "$LAB/dbB"
    cat > "$LAB/a.cfg" <<CFG
NetworkTCPPort = 11201
MaxPDUSize = 16384
MaxAssociations = 16
HostTable BEGIN
labb = (LABB, localhost, 11202)
HostTable END
VendorTable BEGIN
VendorTable END
AETable BEGIN
LABA $LAB/dbA RW (500, 4096mb) ANY
AETable END
CFG
    sed -e 's/11201/11202/' -e 's/^labb = .*//' -e 's/LABA/LABB/' -e 's#dbA#dbB#' "$LAB/a.cfg" > "$LAB/b.cfg"
    dcmqrscp -c "$LAB/a.cfg" -dhl > "$LAB/a.log" 2>&1 & echo $! >> "$LAB/pids"
    dcmqrscp -c "$LAB/b.cfg" -dhl > "$LAB/b.log" 2>&1 & echo $! >> "$LAB/pids"
    sleep 1
    echo "Carico i file DICOM di $SRC su LABA..."
    storescu -aec LABA +sd +r -nh 127.0.0.1 11201 "$SRC" || true
    echo "LABA: $(ls "$LAB/dbA" | grep -c . ) file indicizzati. Log in $LAB"
    ;;
  stop) stop ;;
  *) echo "Uso: $0 start CARTELLA_DICOM | stop"; exit 2 ;;
esac
