# WiFi-Cam-Proxy

Billige WLAN-Otoskope, Ohrreiniger-Kameras und WLAN-Mikroskope spannen ein eigenes WLAN auf und
sprechen proprietäre, aber unverschlüsselte UDP-Protokolle. Zum Ansehen braucht man normalerweise
die App des Herstellers, teils mit Cloud-Lizenzprüfung.

Dieses Projekt ersetzt die App durch einen kleinen **ESP32 mit Ethernet**. Er verbindet sich per
WLAN mit der Kamera und stellt das Bild über Ethernet im Heimnetz bereit:

```
[WLAN-Kamera] <--WLAN--> [ESP32 + LAN8720] <--Ethernet--> [Heimnetz]
 Otoskop/Mikroskop          dieser Proxy                  http://otoskop.local/
```

- **MJPEG-Stream** für Browser, VLC und Home Assistant (`/stream`), Einzelbild (`/snapshot`)
- **Kameras werden automatisch erkannt**, und zwar am WLAN-Namen (SSID). Sind mehrere in
  Reichweite, wählst du in der Weboberfläche eine aus (`/cameras`).
- **Lagekorrektur** im Browser für Otoskope mit Lagesensor: Das Bild dreht sich mit, wenn der
  Stift gedreht wird. Dazu gibt es eine Kalibrierseite.
- **Vergrößerung 2×** im Browser (Knopf oder Doppelklick, Ausschnitt per Ziehen verschieben).
  Das Bild kommt quadratisch an, rund beschneiden lässt es sich optional.
- **Akkustand und LED** der Kamera, soweit das Protokoll sie kennt
- Firmware-Update im Browser, Absturz-Backtrace in `/status`
- **Notfall-Modus** ohne Ethernet: Heim-WLAN oder eigener Access Point `WiFi-Cam-XXXX` mit
  Einrichtungsseite (Captive Portal, wie beim Arduino-WiFiManager)

## Unterstützte Kameras

| Familie | Erkennung (SSID beginnt mit) | Geräte | Bild | Lage | Akku | LED | Stand |
|---|---|---|---|---|---|---|---|
| **i4season** | `Soulear`, `SUEAR`, `i4season`, `inskam`, `Yanxuan`, `wifi_camera_`, `MAX-VIEW`¹ | Hopefox Find T (Soulear-App), MS5-Mikroskop, vermutlich MAX-VIEW-Mikroskope | ✅ | ✅ (wenn vorhanden) | ✅² | ✅² | Soulear am Gerät erprobt |
| **MaxSee / JoyHonest** (`JHCMD`) | `Maxsee`, `JH-` | ältere WLAN-Mikroskope (Kamera auf `192.168.29.1`) | ✅² | – | – | – | nach Doku umgesetzt, ungetestet |

¹ Vermutung: Die MAX-VIEW-App stammt von i4season (`com.i4season.maxview`). Heißt das WLAN anders,
lässt sich die Kamera unter `/cameras` trotzdem auswählen, mit dem Protokoll „automatisch“.
² Nach Protokolldoku umgesetzt, am eigenen Gerät noch nicht geprüft.

Andere Familien wie EarFairy (RTSP), JEGOAT, Xylla und iTiMO finden ihr WLAN über Bluetooth LE
oder brauchen RTSP. Sie sind im ESP32 bisher nicht umgesetzt. Mehr dazu in
[doku/Handover.md](doku/Handover.md).

**Mikroskope mit 720p:** Ein ESP32 ohne PSRAM hat für Bilder nur gut 100 KB RAM. Bilder über 48 KB
werden nur angenommen, solange genug Speicher frei bleibt. Sonst werden sie verworfen und unter
`/status` als `drop_nomem` gezählt. Bei 1280×720 kann das häufig passieren.

## Hardware

| Board | Ethernet-Takt | Ethernet | Anmerkung |
|---|---|---|---|
| **ZB-GW03 v1.4** (Zigbee-Gateway, umgeflasht) | GPIO17, vom ESP32 erzeugt | 10 Mbit | WLAN-Empfang stört den Takt bei 100 Mbit. 10 Mbit reichen für 3 Zuschauer. Läuft zuverlässig. |
| **WT32-ETH01** | GPIO0, eigener Quarz | 100 Mbit | Konfiguration nach Datenblatt, am Gerät noch nicht getestet |
| **CYD ESP32-2432S028R** („Cheap Yellow Display“) | – | kein Ethernet | zeigt das Bild direkt auf dem 2,8"-Display (Touch-Menü), siehe unten. Ungetestet |

Pins und Board-Auswahl stehen in [firmware/include/config.h](firmware/include/config.h).

### CYD als Kamera-Display

Mit `pio run -e cyd -t upload` wird das CYD zum eigenständigen Anzeigegerät: Es sucht die
Kamera wie die Bridge und zeigt den mittleren 320×240-Ausschnitt in voller Auflösung (Zoom 1:1).
Alternativ zeigt es das ganze Bild verkleinert (480×480 → 240×240). Ist der Prozessor langsamer
als die Kamera, fallen Bilder von selbst weg. Angezeigt wird immer das neueste Bild. Ein Tipp aufs
Bild öffnet das Menü mit LED, Lagekorrektur, „Lage = oben“, Zoom, Kamerawahl und Helligkeit. Die Lagekorrektur dreht in 90°-Schritten, weil für beliebige Winkel ohne
PSRAM der Bildpuffer fehlt. 720p-Mikroskope scheitern wie bei der Bridge am RAM. Code:
[firmware/src/main_cyd.cpp](firmware/src/main_cyd.cpp).

## Schnellstart

```bash
./setup-build-env.sh              # PlatformIO in .venv, secrets.h anlegen, Toolchain laden
source .venv/bin/activate
cd firmware
pio run -e zb-gw03                # bauen (erster Build ~5 min: ESP-IDF wird neu gebaut)
pio run -e zb-gw03 -t upload      # erstes Flashen per USB-UART (GPIO0 beim Einschalten auf GND)
pio run -e zb-gw03-http -t upload # danach übers LAN
```

In `firmware/include/secrets.h` kannst du optional ein OTA-Passwort und das Passwort des
Einrichtungs-APs setzen (Standard `wificam-setup`). Das Heim-WLAN für den Notfall-Modus stellst du
im Gerät unter `/wifi-setup` ein. Danach erreichst du das Gerät unter **http://otoskop.local/**.

| Adresse | Zweck |
|---|---|
| `/` | Livebild, Kamera-Info, Akku, LED, Lagekorrektur |
| `/cameras` | gefundene Kameras, Auswahl, neu suchen |
| `/stream`, `/snapshot` | MJPEG und Einzelbild (ungedreht) |
| `/calibrate` | Lagesensor kalibrieren |
| `/update` | Status, WLAN-Modus, Firmware-Update |
| `/wifi-setup` | Heim-WLAN für den Notfall-Modus |
| `/status`, `/cameras.json` | Zähler, letzter Absturz, Kamera-Zustand |

Details zu Bedienung, Diagnose und den Messungen hinter den Einstellungen findest du in
[firmware/README.md](firmware/README.md).

## Aufbau

| Datei | Inhalt |
|---|---|
| [firmware/src/camera.cpp](firmware/src/camera.cpp) | WLAN-Scan, SSID-Muster, Auswahl, Video-Task |
| [firmware/src/cam_i4season.cpp](firmware/src/cam_i4season.cpp) | i4season-Protokoll (Soulear, MS5, …) |
| [firmware/src/cam_jhcmd.cpp](firmware/src/cam_jhcmd.cpp) | MaxSee/JoyHonest-Protokoll |
| [firmware/src/frame.cpp](firmware/src/frame.cpp) | Bildspeicher (Paketliste im IRAM-Rest) |
| [firmware/src/rescue.cpp](firmware/src/rescue.cpp) | Notfall-Modus: Heim-WLAN oder eigener Access Point |
| [firmware/src/main.cpp](firmware/src/main.cpp) | HTTP-Server, Ethernet, OTA |
| [firmware/src/main_cyd.cpp](firmware/src/main_cyd.cpp) | statt main.cpp auf dem CYD: Display, Touch-Menü |
| [firmware/include/web_ui.h](firmware/include/web_ui.h) | Weboberfläche |
| [soulear-viewer.py](soulear-viewer.py), [probe-soulear.py](probe-soulear.py) | Python-Werkzeuge für den PC (nur Standardbibliothek) |

**Speicher:** Der Code aller Protokolle liegt im Flash und läuft direkt von dort. RAM belegt nur
die Sitzung der gerade verbundenen Kamera. Sie wird beim Verbinden angelegt und beim Wechsel wieder
freigegeben. Ein weiteres Protokoll kostet deshalb Flash, aber kein RAM.

**Neues Protokoll:** Lege eine Klasse von `CamSession` ab ([firmware/include/camera.h](firmware/include/camera.h))
an, ergänze sie in `CamProto` und im Video-Task und trage die SSID-Muster in `SSID_PATTERNS` ein.

## Reverse-Engineering-Projekte

Ohne die Vorarbeit dieser Projekte gäbe es diesen Proxy nicht. Die Protokolle hier sind eigene
Nachimplementierungen der Beschreibungen, kopiert wurde kein Code.

### i4season / Soulear (Otoskope)

- **[king-cake/otoscope-windows](https://github.com/king-cake/otoscope-windows)**: Die
  [i4season-Protokollbeschreibung](https://github.com/king-cake/otoscope-windows/blob/master/docs/i4season-protocol.md)
  stammt per Ghidra aus `libWifiCamera.so` der Soulear-App und wurde an einer Hopefox Find T geprüft.
  Grundlage für Handshake, Lagesensor, Akku und LED.
- **[king-cake/otoscope](https://github.com/king-cake/otoscope)**: Fork der Android-App mit Soulear-Support
- **[pedrodinisf/otoscope-viewer](https://github.com/pedrodinisf/otoscope-viewer)**: dieselbe Hardware
  (AiSee, BK7231U), Protokoll-Cheatsheet und Testdaten
- **[rico001/open-web-soulear](https://github.com/rico001/open-web-soulear)**: Node.js-Proxy und
  statische Analyse der Soulear-App (Cloud-Lizenzprüfung, SSID-Präfixe)
- **[SeanPesce/Suear-Web-Viewer](https://github.com/SeanPesce/Suear-Web-Viewer)**: MJPEG-Viewer für
  Suear-Geräte (gleiche Bibliothek), Vorarbeit zum Paketformat
- **[The-Dorkknight/earscope-app](https://github.com/The-Dorkknight/earscope-app)**: Ear-Scope-App mit
  [Protokoll-Erklärung](https://github.com/The-Dorkknight/earscope-app/blob/main/HOW_IT_WORKS.md)

### Mikroskope

- **[Fyfar/ms5-wifi-microscope](https://github.com/Fyfar/ms5-wifi-microscope)**: MS5-Mikroskop
  (i4season, 1280×720), ausführliche Protokolldoku inkl. Auflösungsbefehlen
- **[czietz/wifimicroscope](https://github.com/czietz/wifimicroscope)** und der Artikel
  [Reverse-engineering a Wifi microscope](https://www.chzsoft.de/site/hardware/reverse-engineering-a-wifi-microscope/)
  (CHZ-Soft): MaxSee/JoyHonest-Protokoll (`JHCMD`, UDP 20000/10900)
- [loehnertj/maxsee_viewer](https://github.com/loehnertj/maxsee_viewer),
  [slofo82/MaxSee_wifiMicroscope](https://github.com/slofo82/MaxSee_wifiMicroscope),
  [fbetancourt-dev/microscope-viewer](https://github.com/fbetancourt-dev/microscope-viewer):
  weitere MaxSee/JoyHonest-Viewer
- [Hackaday: Reverse Engineering a Wifi Microscope MS5](https://hackaday.io/project/206057-reverse-engineering-a-wifi-microscope-ms5)

### Weitere Kamera-Familien

- **[rbeilvert/otoscope](https://github.com/rbeilvert/otoscope)**: Android-App mit Multi-Vendor-Ansatz
  (EarFairy, JEGOAT, Xylla, iTiMO), Vorbild für die Erkennung per SSID bzw. BLE
- [raunak51299/Endoscope-Hacking](https://github.com/raunak51299/Endoscope-Hacking): iTiMO-Endoskop
- [mplough: Rewriting the video stream from a wi-fi borescope](https://mplough.github.io/2019/12/14/borescope.html),
  [n8henrie: Reverse Engineering My WiFi Endoscope](https://n8henrie.com/2019/02/reverse-engineering-my-wifi-endoscope-part-4/)

### Hardware

- [syssi/esphome-zb-gw03](https://github.com/syssi/esphome-zb-gw03): Pinbelegung ZB-GW03
- [egnor/wt32-eth01](https://github.com/egnor/wt32-eth01): Pinbelegung WT32-ETH01
- [lovyan03/LovyanGFX](https://github.com/lovyan03/LovyanGFX) und [bitbank2/JPEGDEC](https://github.com/bitbank2/JPEGDEC):
  Display/Touch und JPEG-Dekoder für das CYD
- [pioarduino/platform-espressif32](https://github.com/pioarduino/platform-espressif32): Arduino-Core 3.x
  mit `custom_sdkconfig` für PlatformIO

Eine vollständige Linksammlung mit Protokoll-Übersicht aller bekannten Familien steht in
[doku/Handover.md](doku/Handover.md).

## Lizenz und Hinweise

Unabhängige Interoperabilitäts-Forschung an eigener Hardware, ohne Verbindung zu den Herstellern.
Mehrere der verlinkten Projekte stehen unter der GPL. Wer von dort Code übernimmt statt nur
Protokollwissen, muss das berücksichtigen.
