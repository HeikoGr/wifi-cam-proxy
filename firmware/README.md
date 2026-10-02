# Otoskop-Bridge auf ZB-GW03 v1.4

Der ZB-GW03 (ESP32 + LAN8720, eigentlich ein Zigbee-Gateway) verbindet sich per WLAN mit dem Otoskop (`Soulear-6b1c9`) und liefert das Bild über Ethernet ins Heimnetz. Im Browser gibt es eine Lagekorrektur, die das Bild mitdreht, wenn der Stift gedreht wird.

Stand 30.09.2026: Stabil bei 17 fps, auch mit Zuschauer. Aussetzer gibt es nur noch, wenn das WLAN-Signal des Stifts schwach wird (ab etwa −70 dBm).

## Bedienung

| Adresse | Zweck |
|---|---|
| `http://otoskop.local/` | Live-Bild mit Lagekorrektur und Snapshot |
| `/stream` | MJPEG für VLC oder Home Assistant (ungedreht) |
| `/snapshot` | aktuelles Einzelbild (JPEG) |
| `/calibrate` | Lage kalibrieren: Kreis-Aufzeichnung, Vierteldrehungen, Nullpunkt, Glättung |
| `/update` | Status, WLAN-Modus, Firmware-Update, Neustart |
| `/status` | alle Zähler als JSON |
| `/log` | Absturz-Mitschnitt und die letzten 40 Ereignisse, vom aktuellen und vom letzten Lauf |

LEDs: **Grün** heißt, die Firmware läuft. **Rot** heißt Notfall-Modus.

## Konfiguration

- [include/config.h](include/config.h): Pins, Zeitgrenzen, Standardwerte
- `include/secrets.h` (Vorlage [secrets.example.h](include/secrets.example.h)): Heim-WLAN für den Notfall-Modus, optional `OTA_PASSWORD`
- **NVS** (Namespace `otoskop`) speichert Laufzeit-Einstellungen. Sie überstehen Neustart und Firmware-Update:

| Schlüssel | Inhalt | ändern über |
|---|---|---|
| `calib` | Lage-Kalibrierung (JSON) | `/calibrate` → „Auf Gerät speichern“ |
| `wifimode` | `bgn`, `bg` oder `b` | `/update` → „WLAN zum Otoskop“ |
| `wifitx` | WLAN-Sendeleistung in 0,25 dBm | `POST /wifi/tx/<8..84>` |
| `eth10` | Ethernet nur 10 Mbit (Standard: an) | `POST /debug/eth10/<0\|1>` |

Die Kalibrierung sichern und zurückspielen:

```
curl http://otoskop.local/calibration > kalibrierung.json
curl -H "Content-Type: application/json" --data-binary @kalibrierung.json http://otoskop.local/calibration
```

## Bauen

```
pio run -e zb-gw03
```

Die [platformio.ini](platformio.ini) nutzt **pioarduino** (Arduino-Core 3.x auf ESP-IDF 5.5) mit `custom_sdkconfig`. Damit werden die Arduino-Bibliotheken mit eigenen ESP-IDF-Einstellungen neu gebaut:

| Einstellung | Wert | Grund |
|---|---|---|
| `CONFIG_BT_ENABLED` | `n` | Bluetooth wird nicht genutzt. Spart ~14 KB IRAM, dort liegen die Bilddaten, und RAM |
| `CONFIG_LWIP_UDP_RECVMBOX_SIZE` | `32` (statt 6) | Das Otoskop schickt die 15–30 Pakete eines Bildes als Burst. Mit 6 lief der Puffer über |

Ändert sich `custom_sdkconfig`, baut der nächste Build ESP-IDF neu. Das dauert etwa 4 Minuten, danach geht es wieder schnell.

## Firmware aktualisieren

- **Im Browser:** `http://otoskop.local/update`, dann `.pio/build/zb-gw03/firmware.bin` wählen (nicht `firmware.factory.bin`).
- **Per Kommandozeile:** `pio run -e zb-gw03-http -t upload`, das entspricht `curl --data-binary @firmware.bin http://otoskop.local/update`.
- **Per espota:** `pio run -e zb-gw03-ota -t upload`.

Ist `OTA_PASSWORD` gesetzt, gilt es für alle Wege. Bei curl gibst du es als Header `X-OTA-Password` mit. Während eines Updates gehen kurz Videopakete verloren, weil der Flash beschrieben wird. Das ist normal.

## Notfall-Modus

Hat Ethernet 30 s keine IP, wechselt das WLAN vom Otoskop ins Heim-WLAN aus `secrets.h`. Die rote LED geht an, und Weboberfläche und OTA bleiben unter `otoskop.local` erreichbar. Ist Ethernet 10 s stabil zurück, startet das Gerät neu in den Normalbetrieb. Ohne `secrets.h` ist der Notfall-Modus aus.

## Notfall per USB-UART (3,3 V)

1. Gehäuse öffnen und den Adapter an TX, RX, GND und 3V3 anschließen (TX↔RX gekreuzt).
2. GPIO0 beim Einschalten auf GND legen, damit der ESP32 im Bootloader startet.
3. Flashen mit `pio run -e zb-gw03 -t upload --upload-port /dev/ttyUSB0`.
4. Serielle Ausgabe mit `pio device monitor`.

Der Umstieg von ESPHome lief per OTA über den ESPHome-Port: `esphome.espota2.run_ota('zb-gw03.local', 3232, None, Path('.pio/build/zb-gw03/firmware.bin'))`.

## Was im Code steckt und warum

Die Maßnahmen wurden am Gerät gemessen, mit Mitschnitten auf der Heimnetz-Seite. Bei jeder steht in [src/main.cpp](src/main.cpp) ein Kommentar.

| Problem | Ursache | Lösung |
|---|---|---|
| Kein Video trotz START-Bestätigung | Das Otoskop braucht vorher GetDeviceInfo | GetDeviceInfo und START vom selben Socket |
| Absturz beim Streamen | `new` warf `bad_alloc` bei knappem Heap | Bilder mit `malloc`, bei Mangel wird das Bild verworfen |
| „Ausgelastet“, Bilder wegen Speicher verworfen | `getFreeHeap()` zählt 44 KB IRAM mit, die nicht normal nutzbar sind. Echt frei waren oft nur ~11 KB | Bilder liegen als Paketliste im **IRAM-Rest**, gelesen und geschrieben wortweise. Die Anzeige zeigt jetzt nur echten Speicher |
| Verlorene WLAN-Pakete | UDP-Puffer nur 6 Pakete, `WiFi.RSSI()` im Video-Task hat gebremst | Puffer 32, RSSI nur noch 2×/s in `loop()`, Video-Task mit Priorität 10 |
| Blockaden bei Paketverlust | 802.11n bündelt Pakete, ein fehlendes Teil hält den ganzen Block auf | WLAN-Modus `bg` (ohne 11n) |
| **Stottern: 2–3 % Ethernet-Verlust bei WLAN-Empfang** | Der ESP32 erzeugt den 50-MHz-Takt für den LAN8720 selbst (GPIO17), und der WLAN-Empfang stört ihn. Unabhängig von Sendeleistung, Puffern und Zigbee | **Ethernet nur 10 Mbit** (Aushandlung per PHY-Register). Reicht für 3 Zuschauer |
| Stottern: Sendepausen von 1 s ohne Verlust | Blockierendes `send()` schläft bei `ERR_MEM` bis zum lwIP-Timer (~1 s) | Nicht blockierend senden, Neuversuch nach 5 ms |
| Kurze Stillstände des Otoskops | Das Otoskop pausiert bei Funkeinbruch oder von selbst | Nach 200 ms Stille neu verbinden, danach frühestens alle 800 ms |

Ausprobiert und ohne Wirkung waren: Lebenszeichen-START alle 5 s, kleinere Sendeblöcke, größerer TCP-Sendepuffer (11 KB), mehr Ethernet-Sendepuffer, Store and Forward im Ethernet-Controller (ist noch an, schadet nicht). Das Zigbee-Modul wird im Reset gehalten, obwohl es die Verluste nicht verursacht hat, weil es nicht gebraucht wird und Strom spart.

## Diagnose-Werkzeuge

| Werkzeug | Zweck |
|---|---|
| `/log` | Ereignisse pro Sekunde: fps, `nm`/`inc`/`lost` (verworfen wegen Speicher, unvollständig, verlorene Pakete), Heap, RSSI, Zuschauer. Dazu Stillstände mit Ursache und bei einem Absturz der Backtrace |
| `/status` | Zähler seit dem Start, unter anderem `stalls_loss`/`stalls_clean`, `free_heap`/`min_heap`/`iram_heap`, `eth_speed` |
| `/sensor` | Paketköpfe der letzten ~7 s, diente zum Entschlüsseln des Lagesensors |
| `POST /debug/<video\|wifi\|zigbee\|eth10>/<0\|1>` | Test-Schalter: Video nicht verarbeiten, WLAN trennen, Zigbee an/aus, 10 Mbit |
| `POST /crashtest` | löst absichtlich einen Absturz aus, um den Mitschnitt zu prüfen |

Einen Backtrace aus `/log` löst du so auf. Du brauchst dafür die `firmware.elf` **genau dieser** Firmware:

```
~/.platformio/packages/toolchain-xtensa-esp-elf/bin/xtensa-esp32-elf-addr2line -pfiaC \
  -e .pio/build/zb-gw03/firmware.elf 0x4008bf04 0x4008bec9 …
```

## Grenzen

- **Kein PSRAM:** Auf WROVER-Modulen sind GPIO16/17 für PSRAM belegt, hier treiben sie den Ethernet-Chip. Für Bilder und Puffer stehen etwa 120 KB RAM und 58 KB IRAM zur Verfügung.
- **VLC und Home Assistant bekommen das Rohbild.** Die Lagekorrektur macht nur der Browser, der ESP32 kann JPEGs nicht drehen.
- **Aussetzer bei schwachem Signal:** Bei ungünstiger Drehlage des Stifts (Antenne) sinkt das Signal auf −70 dBm und weniger. Dann fehlen Pakete, und das lässt sich nur über den Aufstellort des ZB-GW03 verbessern.
