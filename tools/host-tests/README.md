# Host tests for the camera code

Checks the camera code without a camera or a board. The real firmware sources
([cam_jhcmd.cpp](../../firmware/src/cam_jhcmd.cpp), [sniffer.cpp](../../firmware/src/sniffer.cpp))
are compiled on the host with AddressSanitizer and UBSan, against small stand-ins for the
ESP32 APIs (`stub/`, `*_stubs.cpp`: frame store, Wi-Fi, sockets are real loopback UDP).

```bash
tools/host-tests/run.sh      # needs g++ (or CXX=clang++); exit code != 0 on a failure
```

| Test | What it checks |
|---|---|
| `jhcmd_test.cpp` | Feeds a real MAX-VIEW frame (`data/max-view-frame.bin`, 24 UDP packets, captured with `/camdiag/raw`) to the JHCMD session over UDP port 10900: in order, shuffled packets, packet 0 last, a lost packet (damaged frame), no memory at packet 5 (the next frame must come through). The published JPEG must be byte-identical to the original. Also the camera's messages: light button (`JHCMD 10 20`, `FDWN`), reply to INIT2 (device name, must not change the LED). |
| `frame_test.cpp` | `allocChunk` with a short heap: a 70 KB frame gets the memory of the stored frame if nobody else holds it, and is refused while a viewer is sending the stored one; `latestFrameSeq()` holds no reference. The word-wise IRAM copies (`copyToChunk`, `copyFromChunk`) against `memcpy` for every offset, length and target alignment (`fakeIram` in `stub/esp_memory_utils.h`). Built without sanitizers, the heap budget is read from `mallinfo2`. |
| `sniffer_test.cpp` | Builds synthetic 802.11 QoS data frames (UDP, TCP ACK, TCP with data, ICMP, traffic that is not the camera's) and checks the recording's lines. |

A new capture for another camera: `curl -s http://otoskop.local/camdiag/raw` once to request
it and a second time to download it; then adapt `jhcmd_test.cpp` (the frame layout is
camera specific).

The tests only cover the parsing and bookkeeping logic. Wi-Fi reception, timing and the
real devices stay a matter for the board.
