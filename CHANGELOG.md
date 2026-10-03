# Changelog

All notable changes to this project. Format: [Keep a Changelog](https://keepachangelog.com/).
Releases are CI builds (`build-<run>-<sha>`); run numbers have gaps where releases were purged.
Entries up to the first build were reconstructed from the commit history.

## [Unreleased]

### Added
- MAX-VIEW: the zoom and photo buttons of the camera are reported (`/led` returns `key` and `seq`).
- Web UI: zoom steps 1x, 2x, 4x and a freeze button; the camera's zoom and photo buttons control them, and the image rotation stays fixed while frozen.
- Soulear otoscope: its button (press counter in the status push) freezes the image like the photo button of the MAX-VIEW.

## build-6-1ddc315 - 2026-10-02

### Added
- Web UI: footer with GitHub link and the commit of the build.

### Changed
- CI: Node 24 actions, `ubuntu-24.04` pinned.

## build-5-f3df25a - 2026-10-02

### Changed
- Rescue mode: web UI reduced to repair functions, shorter home Wi-Fi attempt.
- Updated screenshots.

## build-4-6aaebce - 2026-10-02

### Changed
- CI: all boards are built sequentially in one job.

## build-3-e60e6ba - 2026-10-02

### Changed
- CI: PlatformIO cache is shared across boards.

## build-1-1db7be1 - 2026-10-02

First published build.

### Added
- Bridge for the Soulear (i4season) otoscope on an ESP32 with Ethernet (ZB-GW03): MJPEG stream,
  snapshot, web UI, firmware update, project documentation.
- WT32-ETH01 as a second board.
- CYD (ESP32-2432S028R) as a camera display with touch menu, protocol choice and statistics.
- Multiple cameras, battery level and LED switch, setup access point.
- MaxSee / JoyHonest (`JHCMD`) protocol with MAX-VIEW support: LED dimming and button
  feedback, battery in 10 % steps, status polling like the vendor app.
- Protocol table; "automatic" asks the camera with a short probe per protocol instead of
  guessing; automatic scan can be switched off.
- `/cameras`: protocol choice and reconnect per network.
- Image rotation per camera model.
- Web UI: modern layout, 2x zoom, optional round crop, separate Settings, Status and Update
  pages, switches, VLC link, displayed frames per second, factory reset button.
- CPU load per core in `/status`, on the Status page and in the serial statistics.
- Commit of the build in `/status`, on the Status page and at boot.
- `/camdiag`, `/camdiag/raw` and `/camdiag/send`: camera diagnostics with a ring buffer.
- Sniffer for the vendor app's camera traffic.
- Host tests for the JHCMD session, protocol probes, frame store, JPEG crop and sniffer.
- UI preview and CYD preview tools, screenshots in the docs.
- Installation and build guide, MIT license, VS Code tasks, build environment script.

### Changed
- All texts translated to English; hostname changed from `otoskop.local` to `wifi-cam.local`.
- Stream: limited to 10 fps, then 5 fps per viewer on the ZB-GW03, finally only for images
  above 640x480.
- 720p: only one stream viewer, rest of a given-up frame is skipped; less heap per viewer, stale
  streams end, no orientation stream without a sensor.
- Frame store gives up the stored frame when a new one does not fit; raw capture is freed after
  30 s; sniffer frees its recording when it stops.
- JHCMD: init only when connecting, heartbeat every 3 s like the app; MAX-VIEW is detected by
  `192.168.29.1`.
- CYD: display SPI at 80 MHz, decoding and display transfer overlap, decoder on core 1, `-O2`;
  1:1 crops of 720p frames are drawn completely; flicker-free battery/fps overlay; menus laid
  out from the display size; heap floor kept for the Wi-Fi driver.
- Live view: round crop without the black frame.
- Source split into `http.cpp` and `network.cpp`, routes as a table; NVS access via
  `settings.h`.
- Serial output with CRLF line endings.

### Removed
- Early diagnostics, the unused i4season START keepalive and the CYD orientation correction.

### Fixed
- `/status` buffer overread, unescaped SSIDs and a race in the sniffer.
- HTTP request paths up to 159 characters.
- Live view on Safari (`/live` sent as `application/octet-stream`).
- JHCMD: runs of JPEG fill bytes are cut to one `FF` on the CYD (JPEGDEC stopped there).
- CYD: damaged frames are dropped.
