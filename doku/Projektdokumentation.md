# Projektdokumentation: Soulear-Otoskop-Bridge

Stand: 01.10.2026 · Autor: HeikoGr

> Diese Dokumentation basiert auf den Recherche-Ergebnissen in
> [Handover.md](Handover.md) (Multi-Cam-Protokoll-Übersicht) und den
> verifizierten Messungen in [../Handover.md](../Handover.md)
> (ZB-GW03-Implementierung).

---

## 1. Übersicht

Ein WLAN-Ohrreiniger der Marke **Hopefox** (Hersteller-App „Soulear",
`com.i4season.bkCamera_soulear`) überträgt ein MJPEG-Livebild per proprietary UDP.
Ziel: das Bild **ohne Hersteller-App** im Browser, VLC und Home Assistant anzeigen,
erreichbar aus dem gesamten Heimnetz.

Lösung: ein **ZB-GW03 v1.4** (Zigbee-Gateway, ESP32 + LAN8720) sitzt per WLAN am
Otoskop und stellt das Bild über Ethernet ins Heimnetz. Eine Weboberfläche dreht
das Bild automatisch mit dem Beschleunigungssensor mit.

```
[Soulear-Otoskop] <--WLAN--> [ZB-GW03 ESP32-Bridge] <--Ethernet--> [Heimnetz]
 192.168.1.1                  192.168.178.130                    http://otoskop.local/
 UDP 10005/10006               Arduino/pioarduino
```

**Zustand (Stand 30.09.2026):** Stabil, 17 fps ohne Aussetzer bei gutem Signal.
516 von 517 Bildern empfangen, längste Pause 92 ms.

---

## 2. Hardware

### 2.1 Soulear-Otoskop (Hopefox Find T)

| Eigenschaft | Wert |
|---|---|
| Chip | BK7231U-XRH-FBPRO (Beken BK7231U, Namespace `XRH`) |
| Firmware | HKV41B |
| SSID | `Soulear-6b1c9` (offen, kein Passwort) |
| IP | `192.168.1.1` (eigener DHCP-Server) |
| Client-IP | `192.168.1.10` (per DHCP) |
| Auflösung | 480×480 JPEG (~17–18 fps, 5–41 KB/Bild) |
| Header-Angabe | meldet fälschlich 640×480 in Byte 12–15 |

Die Hersteller-App `Soulear` kann über einen Serveraufruf auf `yun.simicloud.com`
gesperrt werden und erfordert eine Cloud-Lizenzprüfung. Eigene Bridge umgeht das
vollständig. Quelle: statische App-Analyse in
[rico001/open-web-soulear docs/README-statische-analyse.md](https://github.com/rico001/open-web-soulear).

### 2.2 ZB-GW03 v1.4 (aktuelle Bridge-Hardware)

Zigbee-Gateway, umgeflasht mit eigener Firmware.

| GPIO | Funktion |
|---|---|
| GPIO17 | 50-MHz-Takt für LAN8720 (intern erzeugt → WLAN-Interferenz!) |
| GPIO16 | LAN8720 Power-Enable |
| GPIO23 | Ethernet MDC |
| GPIO18 | Ethernet MDIO |
| GPIO14 | Grüne LED (active LOW) |
| GPIO15 | Rote LED (active LOW) |
| GPIO13 | Zigbee EFR32 nRESET (LOW = im Reset, spart Strom) |

**Kritisch:** Bei 100 Mbit Ethernet gehen ~2–3 % der Pakete verloren, weil WLAN-
Empfang den 50-MHz-Takt (GPIO17) stört. Lösung: **Ethernet auf 10 Mbit festsetzen**
(reicht für 3 gleichzeitige MJPEG-Zuschauer à ~3 Mbit/s).
Quelle: [syssi/esphome-zb-gw03](https://github.com/syssi/esphome-zb-gw03).

### 2.3 Empfohlene Alternative: WT32-ETH01

Das WT32-ETH01-Board hat einen **eigenen 50-MHz-Quarz** (GPIO0 INPUT), der unabhängig
vom WLAN-Empfang ist. Damit sind 100 Mbit auch bei gleichzeitigem WLAN möglich.
Unterstützt seit Multi-Platform-Erweiterung (`-DBOARD_WT32_ETH01`).
Quelle: [egnor/wt32-eth01](https://github.com/egnor/wt32-eth01).

---

## 3. Protokoll (i4season / libWifiCamera)

Das Gerät spricht das **i4season-Protokoll**, das auch von WLAN-Mikroskopen (MaxSee),
Ohrenspiegeln (AiSee, Suear) und anderen Geräten dieser Familie genutzt wird.

### 3.1 Protokoll-Header (12 Byte, little-endian)

```
Offset  Länge  Typ   Bedeutung
0       4      u32   Magic: 0xFFEEFFEE (Leitung: EE FF EE FF)
4       2      u16   ID (laufende Nummer, wird in Antwort zurückgespiegelt)
6       2      u16   Type (Befehlstyp)
8       1      u8    Unk = 0x01 bei Anfragen
9       1      u8    Err = 0x00 = OK
10      2      u16   Length (Länge der Nutzdaten in Bytes)
```

### 3.2 Befehlssequenz

| Schritt | Port | Type | Payload | Status |
|---|---|---|---|---|
| GetDeviceInfo | UDP 10005 | 0x0001 | – | ✅ verifiziert |
| START/OpenVideo | UDP 10006 | 0x0004 | 2 Byte eigener Empfangsport (LE) + `00 00` | ✅ verifiziert |
| Videodaten | → eigener Port | – | 16-Byte-Kopf + JPEG-Chunk | ✅ verifiziert |
| SetLed | UDP 10006 | 0x000A | 1 Byte: `0x00`=aus, `0x01`=an | ⚠️ dokumentiert, am Gerät ungetestet |

**Pflicht-Reihenfolge:** GetDeviceInfo **muss** vom selben Socket wie START kommen
(gleiche lokale IP+Port). Ohne diesen Schritt bestätigt das Gerät START zwar, schickt
aber kein Video.

Das erste Paket geht nach Leerlauf oft verloren → immer mehrfach senden.
Nach einem START braucht das Otoskop ~800 ms zum Anlaufen (kein sofortiger zweiter START!).

Protokollquelle: [king-cake/otoscope-windows, docs/i4season-protocol.md](https://github.com/king-cake/otoscope-windows)
– per Ghidra aus `libWifiCamera.so` reverse-engineered und am Hopefox Find T verifiziert.

### 3.3 Video-Paketkopf (16 Byte)

| Byte | Bedeutung |
|---|---|
| 0 | immer `0x01` |
| 1 | Paketnummer (8 Bit, rollt über) |
| 2 | Bildnummer (8 Bit) |
| 3 | `0x00` normal; `0x01` beim letzten Paket im Referenz-Mitschnitt |
| 4 | Anzahl Pakete im Bild |
| 5 | immer `0x01`; Bit 0 = „hat G-Sensor" laut Protokolldoku |
| 6–9 | Beschleunigungssensor (u32 LE): x=Bits 0–9, y=10–19, z=20–29; Bit 9 = Vorzeichen, Bits 0–8 = Betrag; ~128 ≙ 1 g |
| 10–11 | konstant `0x66 0x90` |
| 12–15 | `0x80 0x02 0xE0 0x01` (640/480 LE) – inkorrekt, echte Größe 480×480 |

Rollwinkel = `atan2(x, y)`. Achsen haben leichte Versätze (x ≈ −7, y ≈ +6).
Die Kamera ist im Stift um 90° verdreht eingebaut → Bilder werden immer um −90° gedreht.

---

## 4. Firmware-Architektur

### 4.1 Aufgabenverteilung (Tasks)

| Task | Kern | Priorität | Aufgabe |
|---|---|---|---|
| `videoTask` | Core 1 | 10 | UDP-Empfang, JPEG-Assembly, Lagesensor |
| `httpTask` | Core 1 | 3 | TCP accept, clientTask pro Verbindung erzeugen |
| `clientTask` | Core 1 | 3 | HTTP-Request → Response |
| `loop()` | Core 1 | 1 | ArduinoOTA, Notfall-Modus, FPS-Statistik, RSSI |

### 4.2 Bildspeicher (Frame-Objekte)

Bilder werden **nicht als zusammenhängender Block** gespeichert, sondern als Liste
von UDP-Nutzdaten (Chunks à ~1,3 KB):

- Kein großes `malloc()` → weniger Heap-Fragmentierung
- Chunks bevorzugt im **IRAM-Rest** (~44 KB, nur wortweise nutzbar) → normaler Heap
  bleibt frei; wortweises Kopieren per `volatile uint32_t*`
- Referenzzählung: HTTP-Clients halten das Bild solange, wie sie es senden

**IRAM-Falle:** `getFreeHeap()` zählt ~44 KB IRAM mit, die für `malloc()` und
Task-Stacks nicht nutzbar sind. Die Firmware nutzt daher
`heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)`.

### 4.3 Nicht-blockierendes Senden

Blockierendes `send()` schläft bei `ERR_MEM` bis zu **1 s** (lwIP-Timer). Die Firmware
sendet mit `MSG_DONTWAIT` und wiederholt bei `EAGAIN`/`EWOULDBLOCK` nach 5 ms.

### 4.4 ESP-IDF-Anpassungen (`custom_sdkconfig` in platformio.ini)

| Einstellung | Wert | Grund |
|---|---|---|
| `CONFIG_BT_ENABLED` | `n` | Bluetooth ungenutzt; spart ~14 KB IRAM |
| `CONFIG_LWIP_UDP_RECVMBOX_SIZE` | `32` (statt 6) | Ein ganzes Bild (15–30 Pakete) passt in den Puffer |

Beim ersten Build mit geändertem `custom_sdkconfig` wird ESP-IDF neu gebaut (~4 min).

### 4.5 Ethernet: 10 Mbit und Store-and-Forward

**10 Mbit (ZB-GW03):** PHY-Aushandlung auf „nur 10 Mbit Vollduplex" gesetzt (PHY-Register
ANAR Bits 5–8). Reicht für 3 Zuschauer. Umschaltbar zur Laufzeit: `POST /debug/eth10/0`.

**Store-and-Forward:** Aktiviert via `EMAC_DMA.dmaoperation_mode.tx_str_fwd = 1`.
Verhindert verstümmelte Pakete bei WLAN/DMA-Speicherbus-Konflikten.

---

## 5. HTTP-API

| Endpunkt | Methode | Beschreibung |
|---|---|---|
| `/` | GET | Livebild mit Lagekorrektur und LED-Schalter |
| `/stream` | GET | MJPEG-Stream (für VLC, Home Assistant) |
| `/snapshot` | GET | Einzelbild (JPEG) |
| `/calibrate` | GET | Lage kalibrieren (Kreis-Aufzeichnung, Vierteldrehungen, Nullpunkt) |
| `/calibration` | GET/POST | Kalibrierungsdaten als JSON |
| `/update` | GET | Status, WLAN-Modus, Firmware-Update, Neustart |
| `/update` | POST | Firmware-Update (Binärdatei, `application/octet-stream`) |
| `/status` | GET | Alle Zähler als JSON |
| `/log` | GET | Ereignisprotokoll + Absturz-Backtrace |
| `/orientation` | GET | Server-Sent Events: Lagesensor ~17×/s |
| `/led/0` | POST | Otoskop-LED aus (SetLed 0x0A, **ungetestet**) |
| `/led/1` | POST | Otoskop-LED an |
| `/sensor` | GET | Rohe Paketkopf-Mitschnitte (~7 s) |
| `/wifi/<bgn\|bg\|b>` | POST | WLAN-Modus zum Otoskop umschalten |
| `/wifi/tx/<8..84>` | POST | WLAN-Sendeleistung in 0,25 dBm |
| `/restart` | POST | Neustart |
| `/debug/<video\|wifi\|zigbee\|eth10>/<0\|1>` | POST | Diagnose-Schalter |
| `/crashtest` | POST | Absturz auslösen (für Backtrace-Test) |

**Home Assistant** (MJPEG-Kamera):
```yaml
camera:
  - platform: mjpeg
    name: Otoskop
    mjpeg_url: http://otoskop.local/stream
```

---

## 6. Konfiguration

### 6.1 Compile-Zeit ([firmware/include/config.h](../firmware/include/config.h))

Board-Auswahl per `build_flags` in `platformio.ini`:
- `-DBOARD_ZB_GW03` (Standard, 10 Mbit-Limit)
- `-DBOARD_WT32_ETH01` (100 Mbit, kein Zigbee)

Wichtige Konstanten:

| Konstante | Standard | Bedeutung |
|---|---|---|
| `MAX_STREAM_CLIENTS` | 3 | Max. gleichzeitige MJPEG-Zuschauer |
| `STALL_TIMEOUT_MS` | 200 | Stille → Handshake wiederholen |
| `HANDSHAKE_RETRY_MS` | 800 | Mindestabstand zwischen STARTs |
| `SHOW_DAMAGED_FRAMES` | 1 | Bilder mit Paketverlusten zeigen (1) oder verwerfen (0) |
| `WIFI_MODE_DEFAULT` | `"bg"` | WLAN-Modus ohne 11n (jedes Paket einzeln) |

### 6.2 Laufzeit (NVS, Namespace `otoskop`)

| Schlüssel | Inhalt | Setzen über |
|---|---|---|
| `calib` | Lage-Kalibrierung (JSON) | `/calibrate` |
| `wifimode` | `bgn`/`bg`/`b` | `/update` |
| `wifitx` | WLAN-Sendeleistung (0,25 dBm) | `POST /wifi/tx/<Wert>` |
| `eth10` | Ethernet 10 Mbit (Standard: an) | `POST /debug/eth10/<0\|1>` |

### 6.3 Geheimnisse ([firmware/include/secrets.h](../firmware/include/secrets.h))

Die Datei `secrets.h` ist in `.gitignore` und darf **nie eingecheckt** werden.
Vorlage: [firmware/include/secrets.example.h](../firmware/include/secrets.example.h)

```cpp
#define HOME_WIFI_SSID     "DeinHeimnetz"     // nur für Notfall-Modus benötigt
#define HOME_WIFI_PASSWORD "DeinPasswort"
// #define OTA_PASSWORD    "update-passwort"   // optional
```

---

## 7. Bauen und Flashen

### 7.1 VS Code (empfohlener Workflow)

Mit der Extension **`actboy168.tasks`** erscheinen alle Tasks aus
[.vscode/tasks.json](../.vscode/tasks.json) als Buttons in der Statusleiste.

Empfohlene Extensions (in [.vscode/extensions.json](../.vscode/extensions.json)):
- `actboy168.tasks` – Task-Buttons in der Statusleiste
- `ms-vscode.cpptools` – C/C++ IntelliSense
- `ms-vscode.serial-monitor` – Serial Monitor

**Zur PIO-IDE-Extension:** Die offizielle `platformio.platformio-ide`-Extension ist sehr
schwer und lädt beim ersten Mal sehr lange (großer PIO Core-Download). Da dieses Projekt
rein mit der PIO-CLI auskommt (`pio run`, `pio device monitor`), ist die Extension
**optional**. Die CLI ist schneller und zeigt dieselben Compiler-Fehlermeldungen.

### 7.2 Wichtige Build-Befehle

```bash
cd firmware

# Bauen
pio run -e zb-gw03

# Erstes Flashen (USB-UART, GPIO0 auf GND beim Einschalten)
pio run -e zb-gw03 -t upload --upload-port /dev/ttyUSB0

# OTA per HTTP
pio run -e zb-gw03-http -t upload

# OTA per espota (ArduinoOTA)
pio run -e zb-gw03-ota -t upload

# Serial Monitor
pio device monitor
```

### 7.3 OTA-Update im Browser

`http://otoskop.local/update` → `.pio/build/zb-gw03/firmware.bin` hochladen
(**nicht** `firmware.factory.bin`).

### 7.4 Backtrace auflösen

```bash
~/.platformio/packages/toolchain-xtensa-esp-elf/bin/xtensa-esp32-elf-addr2line \
  -pfiaC -e firmware/.pio/build/zb-gw03/firmware.elf \
  0x4008bf04 0x4008bec9 …
```

Die `.elf` muss zur laufenden Firmware passen (aus demselben Build).

---

## 8. Multi-Platform-Strategie

Die Firmware kompiliert für verschiedene ESP32-Boards mit Ethernet-PHY. Das Board
wird per `-DBOARD_<NAME>` in `platformio.ini` gewählt.

| Board | ETH-Takt | Max. Ethernet | LED-Polarität | Zigbee |
|---|---|---|---|---|
| ZB-GW03 v1.4 | GPIO17 OUT (intern) | **10 Mbit** (WLAN-Limit) | active LOW | EFR32, deaktiviert |
| WT32-ETH01 | GPIO0 IN (extern) | 100 Mbit | active HIGH | keins |

Für neue Hardware: WT32-ETH01 empfohlen (~8 €, kein 10-Mbit-Limit).
Für weitere Boards: Neue Sektion in `config.h` und neues `[env:...]` in `platformio.ini`.

---

## 9. Notfall-Modus

Hat Ethernet 30 s keine IP → WLAN wechselt ins Heim-WLAN (aus `secrets.h`).
- Rote LED (GPIO15) leuchtet
- Weboberfläche und OTA bleiben unter `http://otoskop.local/` erreichbar
- Kommt Ethernet 10 s zurück → automatischer Neustart in Normalbetrieb

---

## 10. Diagnose

| Quelle | Inhalt |
|---|---|
| `/log` | Ereignisse/Sekunde: fps, `nm`/`inc`/`lost`, Heap, RSSI, Zuschauer + Backtrace bei Absturz |
| `/status` | JSON: alle Zähler seit Start |
| `stalls_loss` | Aussetzer nach Paketverlust → WLAN-Signal schwach |
| `stalls_clean` | Aussetzer ohne Paketverlust → Otoskop pausiert selbst |

---

## 11. LED-Steuerung (neu, ungetestet)

Befehlstyp `0x0A` (SetLed) ist im i4season-Protokoll dokumentiert
([Fyfar/ms5-wifi-microscope](https://github.com/Fyfar/ms5-wifi-microscope),
[king-cake/otoscope-windows](https://github.com/king-cake/otoscope-windows)).

Implementiert in der Firmware:
- `POST /led/1` → LED an
- `POST /led/0` → LED aus
- LED-State in `/status` als `"led": true/false`
- Button (Lampen-Symbol) auf der Startseite (`/`)

Am Soulear-Gerät noch nicht getestet. Das Gerät könnte den Befehl ignorieren oder
anders auslegen als dokumentiert. Protokoll-Payload: 1 Byte (`0x01`=an, `0x00`=aus).

---

## 12. Offene Ideen

| Idee | Quelle/Hinweis |
|---|---|
| LED-Steuerung am Gerät testen | Befehl 0x0A, ggf. Payload-Format prüfen |
| Auflösungsabfrage (`GetCameraConfig`, 0x0D) | Vorsicht: Moduswechsel kann Encoder blockieren (bei MS5 beobachtet) |
| WT32-ETH01 in Betrieb nehmen (100 Mbit, günstiger) | Multi-Platform bereits implementiert |
| Regelmäßige Pausen des Otoskops weiter beobachten | Alle ~25 s, Ursache unklar |
| Home Assistant einrichten | `platform: mjpeg`, URL `/stream` |
| Diagnose-Endpunkte optional machen (`/crashtest`, `/sensor`) | Als Build-Flag in `config.h` |
| Multi-Cam-Proxy (Linux, mehrere Kameras gleichzeitig) | Konzept in [Handover.md](Handover.md) Abschnitt 4; andere Familien: EarFairy, JEGOAT, Xylla, MaxSee |

---

## 13. Bekannte Grenzen

- Kein PSRAM: GPIO16/17 für Ethernet belegt. Nur ~120 KB RAM + ~58 KB IRAM.
- Lagekorrektur nur im Browser (CSS-Rotation). VLC/Home Assistant erhalten rohes Bild (−90°).
- Aussetzer bei WLAN-RSSI < −70 dBm. Lösung: ZB-GW03 näher ans Otoskop stellen.
- Keine Custom-Firmware fürs Otoskop: BK7231U-Community hat keinen Kameratreiber.

---

## 14. Quellenverzeichnis

| Quelle | Relevanz |
|---|---|
| [pedrodinisf/otoscope-viewer](https://github.com/pedrodinisf/otoscope-viewer) | Dieselbe Hardware (AiSee/BK7231U/XRH), Protokoll-Cheatsheet, Testfixtures |
| [Fyfar/ms5-wifi-microscope](https://github.com/Fyfar/ms5-wifi-microscope) | Ausführlichste i4season-Protokoll-Doku (alle Typen, Auflösung, LED 0x0A) |
| [king-cake/otoscope-windows](https://github.com/king-cake/otoscope-windows) | `docs/i4season-protocol.md` (aus Ghidra-Analyse), am Find T verifiziert |
| [rico001/open-web-soulear](https://github.com/rico001/open-web-soulear) | Node.js-Proxy, statische App-Analyse (Cloud-Sperre) |
| [SeanPesce/Suear-Web-Viewer](https://github.com/SeanPesce/Suear-Web-Viewer) | MJPEG-Mirror für Suear (gleiche Familie) |
| [syssi/esphome-zb-gw03](https://github.com/syssi/esphome-zb-gw03) | Pinout ZB-GW03 v1.4 |
| [egnor/wt32-eth01](https://github.com/egnor/wt32-eth01) | Pinout WT32-ETH01, ETH_CLOCK_GPIO0_IN |
| [pioarduino/platform-espressif32](https://github.com/pioarduino/platform-espressif32) | PlatformIO-Platform mit Arduino-Core 3.x und `custom_sdkconfig` |
| [Elektroda: Taixen TXW816](https://www.elektroda.com/news/news4129331.html) | BK7231U UART/Firmware-Dump (Hintergrund) |
