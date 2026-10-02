#!/usr/bin/env bash
# Writes firmware/compile_commands.json for VS Code IntelliSense (C/C++ extension): the
# exact compiler calls of the build, with the include paths and defines of the ESP32
# framework. Without it IntelliSense guesses and reports errors the compiler never sees.
#
# Basis is the ZB-GW03 build; main_cyd.cpp is only built for the CYD, so its entry
# (and those of the CYD's libraries) come from that build. Run again after changing
# platformio.ini or adding a source file.
set -euo pipefail
FW="$(cd "$(dirname "${BASH_SOURCE[0]}")/../firmware" && pwd)"
PIO="${PIO:-$FW/../.venv/bin/pio}"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
cd "$FW"
for env in zb-gw03 cyd; do
  "$PIO" run -s -e "$env" -t compiledb
  mv compile_commands.json "$TMP/$env.json"
done
python3 - "$TMP/zb-gw03.json" "$TMP/cyd.json" > compile_commands.json <<'EOF'
import json, os, sys
merged, seen = [], set()
for path in sys.argv[1:]:
    for e in json.load(open(path)):
        key = os.path.normpath(os.path.join(e["directory"], e["file"]))
        if key not in seen:  # first build wins (ZB-GW03 defines for the shared files)
            seen.add(key)
            merged.append(e)
json.dump(merged, sys.stdout, indent=1)
EOF
echo "firmware/compile_commands.json: $(python3 -c 'import json;print(len(json.load(open("compile_commands.json"))))' ) files"
