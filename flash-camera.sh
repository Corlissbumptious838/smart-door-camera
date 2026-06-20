#!/usr/bin/env bash
# ===========================================================================
#  flash-camera.sh — ESP32-CAM (Türkamera) bauen + flashen
#
#  Aufruf (nachdem das Modul per USB-TTL verbunden ist):
#      ./flash-camera.sh                 # Port automatisch erkennen
#      ./flash-camera.sh /dev/cu.xyz     # Port explizit vorgeben
#
#  Erkennt den seriellen Port selbst (CH340 / CP210x). Hardcodiert nichts,
#  funktioniert daher auch mit neuen Modulen (wechselnde Portnummer).
#
#  Hinweis: Startet der Upload nicht automatisch (einfacher Adapter ohne
#  Auto-Reset), GPIO0->GND brücken, RESET drücken und Skript erneut starten.
# ===========================================================================
set -euo pipefail

# --- Projektverzeichnis relativ zum Skript bestimmen ---
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT="$DIR/doorcam"
MODUL="ESP32-CAM (Türkamera)"

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
  CANDIDATES=()
  for p in /dev/cu.usbserial-* /dev/cu.usbmodem* /dev/cu.wchusbserial* /dev/cu.SLAB_USBtoUART*; do
    [[ -e "$p" ]] && CANDIDATES+=("$p")
  done

  if [[ ${#CANDIDATES[@]} -eq 0 ]]; then
    echo "FEHLER: Kein Modul gefunden. Bitte die $MODUL per USB-TTL verbinden." >&2
    exit 1
  elif [[ ${#CANDIDATES[@]} -eq 1 ]]; then
    PORT="${CANDIDATES[0]}"
  else
    # Mehrere Ports angesteckt -> den zur Kamera passenden Adapter bevorzugen:
    # CH340 hat keine Seriennummer (macOS vergibt usbserial-<Ziffern>), CP210x -> SLAB / wch.
    PREFERRED=()
    for p in "${CANDIDATES[@]}"; do
      case "$p" in
        /dev/cu.wchusbserial*) PREFERRED+=("$p") ;;
        /dev/cu.SLAB_USBtoUART*) PREFERRED+=("$p") ;;
        /dev/cu.usbserial-[0-9]*) PREFERRED+=("$p") ;;
      esac
    done
    if [[ ${#PREFERRED[@]} -eq 1 ]]; then
      PORT="${PREFERRED[0]}"
    else
      echo "Mehrere serielle Geräte gefunden — bitte den gewünschten Port als Argument angeben:" >&2
      printf '  %s\n' "${CANDIDATES[@]}" >&2
      echo "Beispiel:  ./flash-camera.sh ${CANDIDATES[0]}" >&2
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
