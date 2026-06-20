#!/usr/bin/env bash
# ===========================================================================
#  flash-display.sh — ESP32-S3 (Büro-Display) bauen + flashen
#
#  Aufruf (nachdem das Modul per USB verbunden ist):
#      ./flash-display.sh                 # Port automatisch erkennen
#      ./flash-display.sh /dev/cu.xyz     # Port explizit vorgeben
#
#  Erkennt den seriellen Port selbst (FTDI / nativer USB). Hardcodiert nichts,
#  funktioniert daher auch mit neuen Modulen (andere Seriennummer).
# ===========================================================================
set -euo pipefail

# --- Projektverzeichnis relativ zum Skript bestimmen ---
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT="$DIR/officedisplay"
MODUL="ESP32-S3 (Display)"

if [[ ! -f "$PROJECT/platformio.ini" ]]; then
  echo "FEHLER: Projekt nicht gefunden: $PROJECT" >&2
  exit 1
fi

# --- PlatformIO vorhanden? ---
if ! command -v pio >/dev/null 2>&1; then
  echo "FEHLER: 'pio' (PlatformIO) nicht gefunden." >&2
  echo "  Installieren mit:  brew install platformio" >&2
  exit 1
fi

# --- Seriellen Port bestimmen ---
PORT="${1:-}"
if [[ -z "$PORT" ]]; then
  # Alle in Frage kommenden USB-Seriell-Ports einsammeln (Bluetooth/debug-console
  # haben andere Namen und werden dadurch nicht erfasst).
  CANDIDATES=()
  for p in /dev/cu.usbserial-* /dev/cu.usbmodem* /dev/cu.wchusbserial* /dev/cu.SLAB_USBtoUART*; do
    [[ -e "$p" ]] && CANDIDATES+=("$p")
  done

  if [[ ${#CANDIDATES[@]} -eq 0 ]]; then
    echo "FEHLER: Kein Modul gefunden. Bitte den $MODUL per USB verbinden." >&2
    exit 1
  elif [[ ${#CANDIDATES[@]} -eq 1 ]]; then
    PORT="${CANDIDATES[0]}"
  else
    # Mehrere Ports angesteckt -> den zum S3 passenden bevorzugen:
    # FTDI (Seriennummer beginnt mit Buchstaben, z.B. usbserial-A50...) oder nativer USB (usbmodem).
    PREFERRED=()
    for p in "${CANDIDATES[@]}"; do
      case "$p" in
        /dev/cu.usbmodem*) PREFERRED+=("$p") ;;
        /dev/cu.usbserial-[A-Za-z]*) PREFERRED+=("$p") ;;
      esac
    done
    if [[ ${#PREFERRED[@]} -eq 1 ]]; then
      PORT="${PREFERRED[0]}"
    else
      echo "Mehrere serielle Geräte gefunden — bitte den gewünschten Port als Argument angeben:" >&2
      printf '  %s\n' "${CANDIDATES[@]}" >&2
      echo "Beispiel:  ./flash-display.sh ${CANDIDATES[0]}" >&2
      exit 1
    fi
  fi
fi

echo "Modul:   $MODUL"
echo "Projekt: $PROJECT"
echo "Port:    $PORT"
echo "-----------------------------------------------------------"

# --- Bauen + Flashen ---
pio run -d "$PROJECT" -t upload --upload-port "$PORT"

echo "-----------------------------------------------------------"
echo "Flash erfolgreich."

# --- Optional: seriellen Monitor öffnen ---
printf "Seriellen Monitor öffnen? [j/N] "
read -r answer || answer=""
case "$answer" in
  j|J|y|Y) exec pio device monitor -d "$PROJECT" -p "$PORT" -b 115200 ;;
  *) echo "Fertig." ;;
esac
