# Host tests for the camera code

Checks the camera code without a camera or a board. The real firmware sources
([cam_jhcmd.cpp](../../firmware/src/cam_jhcmd.cpp), [cam_i4season.cpp](../../firmware/src/cam_i4season.cpp),
[cam_probe.cpp](../../firmware/src/cam_probe.cpp), [frame.cpp](../../firmware/src/frame.cpp),
[sniffer.cpp](../../firmware/src/sniffer.cpp)) are compiled on the host with AddressSanitizer and UBSan
(the frame store without), against small stand-ins for the
ESP32 APIs (`stub/`, `*_stubs.cpp`: frame store, Wi-Fi, sockets are real loopback UDP).

```bash
tools/host-tests/run.sh      # needs g++ (or CXX=clang++); exit code != 0 on a failure
```

| Test | What it checks |
|---|---|
| `jhcmd_test.cpp` | Feeds a real MAX-VIEW frame (`data/max-view-frame.bin`, 24 UDP packets, captured with `/camdiag/raw`) to the JHCMD session over UDP port 10900: in order, shuffled packets, packet 0 last, a lost packet (damaged frame), no memory at packet 5 (the next frame must come through). The published JPEG must be byte-identical to the original. Also the camera's messages: light button (`JHCMD 10 20`, `FDWN`), reply to INIT2 (device name, must not change the LED). |
| `jhcmd_test.cpp` again with `-DJPEG_COLLAPSE_FILL=1` | As on the CYD: the published JPEG must equal the original with every run of fill bytes `FF` cut to one. |
| `probe_test.cpp` | The protocol probes of "automatic" against fake cameras on `127.0.0.x` (threads with loopback UDP): each camera answers its own probe and not the other one, a lost first request is covered by the second, a wrong answer on the port does not count, no camera gives up after ~400 ms. |
| `jpeg_crop_test.cpp` | The CYD's decode geometry ([jpeg_crop.h](../../firmware/include/jpeg_crop.h): 1:1 crop with skipping the rows above it, fit, MCU groups, DMA ping-pong) against the real JPEGDEC: every visible pixel must equal a full decode. Images from `make_test_jpegs.py`: the MAX-VIEW frame and, with Pillow (`.venv`), five generated formats. Needs JPEGDEC from a CYD build (`pio run -e cyd`), else skipped. |
| `frame_test.cpp` | `allocChunk` with a short heap: a 70 KB frame gets the memory of the stored frame if nobody else holds it, and is refused while a viewer is sending the stored one; `latestFrameSeq()` holds no reference. The word-wise IRAM copies (`copyToChunk`, `copyFromChunk`) against `memcpy` for every offset, length and target alignment (`fakeIram` in `stub/esp_memory_utils.h`). Built without sanitizers, the heap budget is read from `mallinfo2`. |
| `sniffer_test.cpp` | Builds synthetic 802.11 QoS data frames (UDP, TCP ACK, TCP with data, ICMP, traffic that is not the camera's) and checks the recording's lines. |

A new capture for another camera: `curl -s http://otoskop.local/camdiag/raw` once to request
it and a second time to download it; then adapt `jhcmd_test.cpp` (the frame layout is
camera specific).

The tests only cover the parsing and bookkeeping logic. Wi-Fi reception, timing and the
real devices stay a matter for the board.
