#!/bin/bash
# Builds the real firmware sources (camera protocols, probes, frame store, sniffer) for the host (with
# AddressSanitizer and UBSan) against small stand-ins for the ESP32 APIs, and runs the tests.
set -eo pipefail
cd "$(dirname "$0")"
FW=../../firmware
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
CXX=${CXX:-g++}
FLAGS="-std=gnu++17 -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined -DBOARD_ZB_GW03 -Istub -I$FW/include"

echo "== JHCMD (MAX-VIEW frame): packet order, losses, no memory, LED messages"
$CXX $FLAGS $FW/src/cam_jhcmd.cpp $FW/src/cam_probe.cpp jhcmd_stubs.cpp jhcmd_test.cpp -o "$OUT/jhcmd_test"
"$OUT/jhcmd_test" data/max-view-frame.bin | grep -v "diag:"

echo
echo "== JHCMD as on the CYD (JPEG_COLLAPSE_FILL: fill bytes cut for JPEGDEC)"
$CXX $FLAGS -DJPEG_COLLAPSE_FILL=1 $FW/src/cam_jhcmd.cpp $FW/src/cam_probe.cpp jhcmd_stubs.cpp jhcmd_test.cpp -o "$OUT/jhcmd_cyd_test"
"$OUT/jhcmd_cyd_test" data/max-view-frame.bin | grep -v "diag:"

echo
echo "== Protocol probes (\"automatic\") against fake cameras on 127.0.0.x"
$CXX $FLAGS -pthread $FW/src/cam_i4season.cpp $FW/src/cam_jhcmd.cpp $FW/src/cam_probe.cpp jhcmd_stubs.cpp probe_test.cpp -o "$OUT/probe_test"
"$OUT/probe_test" | grep -v "diag:"

echo
echo "== Frame store: memory rules of allocChunk (no sanitizers: the heap budget is read from mallinfo)"
$CXX -std=gnu++17 -g -O1 -DBOARD_ZB_GW03 -Istub -I$FW/include $FW/src/frame.cpp frame_test.cpp -o "$OUT/frame_test"
"$OUT/frame_test"

echo
echo "== Sniffer: 802.11 data frames -> UDP/TCP/ICMP lines"
$CXX $FLAGS $FW/src/sniffer.cpp sniffer_stubs.cpp sniffer_test.cpp -o "$OUT/sniffer_test"
"$OUT/sniffer_test"
