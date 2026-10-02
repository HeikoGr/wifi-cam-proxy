#!/bin/bash
# Builds the CYD screen renderer (screens.cpp) for the host: the real display code
# (firmware/src/main_cyd.cpp) with LovyanGFX and JPEGDEC from the cyd build's libdeps.
#   tools/cyd-preview/build.sh <out dir>    -> <out dir>/screens
set -eo pipefail
cd "$(dirname "$0")"
OUT=${1:?out dir}
L=../../firmware/.pio/libdeps/cyd
if [ ! -f "$L/LovyanGFX/src/lgfx/v1/lgfx_v1.cpp" ] || [ ! -f "$L/JPEGDEC/src/JPEGDEC.cpp" ]; then
  echo "LovyanGFX/JPEGDEC not downloaded yet: cd firmware && pio run -e cyd" >&2
  exit 1
fi
mkdir -p "$OUT"
CXX=${CXX:-g++}
CC=${CC:-gcc}
# NO_SIMD: JPEGDEC as on the ESP32 (its SSE path on x86 garbled 4:2:0 at 1:1)
DEFS="-DLGFX_LINUX_FB -DBOARD_CYD -D__LINUX__ -DNO_SIMD"
INC="-Istub -I../host-tests/stub -I../../firmware/include -I$L/LovyanGFX/src -I$L/JPEGDEC/src"
OBJS=()
for c in $(find "$L/LovyanGFX/src" -name '*.c'); do  # fonts and utilities, C
  o="$OUT/$(basename "$c" .c).o"
  [ "$o" -nt "$c" ] || $CC -O1 $DEFS -I$L/LovyanGFX/src -c "$c" -o "$o"
  OBJS+=("$o")
done
for c in "$L/LovyanGFX/src/lgfx/v1/lgfx_v1.cpp" "$L/JPEGDEC/src/JPEGDEC.cpp"; do
  o="$OUT/$(basename "$c" .cpp).o"
  [ "$o" -nt "$c" ] || $CXX -std=gnu++17 -O1 $DEFS $INC -c "$c" -o "$o"
  OBJS+=("$o")
done
$CXX -std=gnu++17 -O1 $DEFS $INC screens.cpp "${OBJS[@]}" -o "$OUT/screens" -lpthread
