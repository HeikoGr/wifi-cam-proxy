#!/usr/bin/env bash
# Sets up the build environment for the WiFi-Cam-Proxy firmware.
#
#   ./setup-build-env.sh            install PlatformIO, create secrets.h,
#                                   pre-download platform + toolchain
#   ./setup-build-env.sh --build    additionally build all boards once
#
# Tested on Debian/Ubuntu (including GitHub Codespaces) and macOS.
# The script is idempotent: running it several times does no harm.

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
[[ -z "$PY" ]] && { echo "python3 is missing (Debian/Ubuntu: sudo apt install python3 python3-venv)"; exit 1; }
info "Python: $("$PY" --version)"

# --- 2. PlatformIO in its own virtual environment (.venv, ignored via .gitignore)
if [[ ! -x "$VENV/bin/pio" ]]; then
  info "Creating $VENV and installing PlatformIO"
  "$PY" -m venv "$VENV"
  "$VENV/bin/pip" install --quiet --upgrade pip
  "$VENV/bin/pip" install --quiet platformio
else
  info "PlatformIO already installed: $("$VENV/bin/pio" --version)"
fi
PIO="$VENV/bin/pio"

# --- 3. secrets.h from the template ----------------------------------------------
if [[ ! -f "$FW/include/secrets.h" ]]; then
  cp "$FW/include/secrets.example.h" "$FW/include/secrets.h"
  warn "created firmware/include/secrets.h from the template - optionally set OTA and setup-AP passwords there"
fi

# --- 4. Pre-download platform (pioarduino) and toolchain -------------------------
# 'pkg install' downloads platform, framework and toolchain for all [env:...] in
# platformio.ini. The first real build additionally rebuilds ESP-IDF with
# custom_sdkconfig (~4 min per board).
info "Downloading platform and toolchain (~1 GB the first time)"
(cd "$FW" && "$PIO" pkg install)

# --- 5. Access to USB-UART adapters (Linux only) ---------------------------------
if [[ "$(uname)" == "Linux" ]] && ! id -nG | grep -qw dialout; then
  warn "user is not in the 'dialout' group - to flash via USB: sudo usermod -aG dialout $USER (then log in again)"
fi

# --- 6. Optional: build all boards ----------------------------------------------
if [[ $BUILD -eq 1 ]]; then
  for env in zb-gw03 wt32-eth01 cyd; do
    info "Building $env"
    (cd "$FW" && "$PIO" run -e "$env")
  done
fi

cat <<EOF

Done. Run PlatformIO with:
  source .venv/bin/activate      (then 'pio' is available directly)
  cd firmware && pio run -e zb-gw03
Or in VS Code via the tasks (Ctrl+Shift+B).
EOF
