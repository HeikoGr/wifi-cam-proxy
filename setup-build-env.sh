#!/usr/bin/env bash
# Richtet die Build-Umgebung für die WiFi-Cam-Proxy-Firmware ein.
#
#   ./setup-build-env.sh            PlatformIO installieren, secrets.h anlegen,
#                                   Plattform + Toolchain vorladen
#   ./setup-build-env.sh --build    zusätzlich einmal alle Boards bauen
#
# Getestet auf Debian/Ubuntu (auch GitHub Codespaces) und macOS.
# Das Skript ist idempotent: mehrfach ausführen schadet nicht.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FW="$ROOT/firmware"
VENV="$ROOT/.venv"
BUILD=0
[[ "${1:-}" == "--build" ]] && BUILD=1

info() { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m!!\033[0m  %s\n' "$*"; }

# --- 1. Python ------------------------------------------------------------------
PY="$(command -v python3 || true)"
[[ -z "$PY" ]] && { echo "python3 fehlt (Debian/Ubuntu: sudo apt install python3 python3-venv)"; exit 1; }
info "Python: $("$PY" --version)"

# --- 2. PlatformIO in eigener virtueller Umgebung (.venv, per .gitignore ignoriert)
if [[ ! -x "$VENV/bin/pio" ]]; then
  info "Lege $VENV an und installiere PlatformIO"
  "$PY" -m venv "$VENV"
  "$VENV/bin/pip" install --quiet --upgrade pip
  "$VENV/bin/pip" install --quiet platformio
else
  info "PlatformIO schon installiert: $("$VENV/bin/pio" --version)"
fi
PIO="$VENV/bin/pio"

# --- 3. secrets.h aus der Vorlage ----------------------------------------------
if [[ ! -f "$FW/include/secrets.h" ]]; then
  cp "$FW/include/secrets.example.h" "$FW/include/secrets.h"
  warn "firmware/include/secrets.h aus der Vorlage angelegt - Heim-WLAN für den Notfall-Modus eintragen"
fi

# --- 4. Plattform (pioarduino) und Toolchain vorladen ---------------------------
# 'pkg install' lädt Plattform, Framework und Toolchain für alle [env:...] in
# platformio.ini. Der erste echte Build baut zusätzlich ESP-IDF mit
# custom_sdkconfig neu (~4 min pro Board).
info "Lade Plattform und Toolchain (beim ersten Mal ~1 GB)"
(cd "$FW" && "$PIO" pkg install)

# --- 5. Zugriff auf USB-UART-Adapter (nur Linux) --------------------------------
if [[ "$(uname)" == "Linux" ]] && ! id -nG | grep -qw dialout; then
  warn "Benutzer ist nicht in der Gruppe 'dialout' - zum Flashen per USB: sudo usermod -aG dialout $USER (danach neu anmelden)"
fi

# --- 6. Optional: alle Boards bauen ----------------------------------------------
if [[ $BUILD -eq 1 ]]; then
  for env in zb-gw03 wt32-eth01; do
    info "Baue $env"
    (cd "$FW" && "$PIO" run -e "$env")
  done
fi

cat <<EOF

Fertig. PlatformIO aufrufen mit:
  source .venv/bin/activate      (danach steht 'pio' direkt zur Verfügung)
  cd firmware && pio run -e zb-gw03
Oder in VS Code über die Tasks (Strg+Umschalt+B).
EOF
