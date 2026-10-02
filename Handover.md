# Handover: Soulear-Otoskop ohne Hersteller-App

Stand: 30.09.2026, abends (ESP32-Bridge im Einsatz)

## Ziel

Das Live-Bild des WLAN-Ohrreinigers (Marke Hopefox, Hersteller-App „Soulear“) ohne die chinesische Original-App anzeigen. Das soll im Browser, in VLC oder Home Assistant funktionieren und aus dem ganzen Heimnetz erreichbar sein.

## Hardware (verifiziert)

Die Daten stammen aus der `GetDeviceInfo`-Antwort des Geräts:

| Feld | Wert |
|---|---|
| Marke / Modell | Hopefox, Modellbezeichnung vermutlich „Find T“ |
| Hersteller-App | Soulear (`com.i4season.bkCamera_soulear`) |
| Chip / Produkt | `BK7231U-XRH-FBPRO` (Beken BK7231U, Firmware-Namespace `XRH`) |
| Firmware | `HKV41B` |
| SSID | `Soulear-6b1c9` (offen, ohne Passwort) |
| IP Gerät | `192.168.1.1` (DHCP-Server im Gerät) |
| IP Client | `192.168.1.10` (per DHCP vom Gerät vergeben) |

Das Vendor-Feld liefert nur `YPC` plus Datenmüll (`ota ok`, `sys_cfgs.license =`), weil die Firmware den Puffer nicht sauber nullt. Das ist unkritisch.

## Netzwerk-Setup (nur für den Python-Viewer auf der VM)

Für die ESP32-Bridge wird das nicht gebraucht. Die VM hängt dann nur noch im Heimnetz.

- Debian-VM im Proxmox-Cluster mit zwei Interfaces:
  - `eth0`: Heimnetz, Gateway `192.168.178.1` (Fritzbox), statisch. Darüber läuft das Internet.
  - `wls16`: WLAN-Adapter in der VM, verbunden mit `Soulear-6b1c9` über DHCP (metric 600).
- Beide Default-Routen koexistieren problemlos. `192.168.1.0/24` läuft über die direkte Netzroute auf `wls16`.
- Verbinden: `nmcli device wifi connect "Soulear-6b1c9"`

## Protokoll (i4season / libWifiCamera-Familie)

Alles läuft über UDP. Header: 12 Byte, little-endian.

```
magic  u32 = 0xFFEEFFEE   (auf der Leitung: EE FF EE FF)
id     u16   laufende Nummer, wird zurückgespiegelt
type   u16   Befehlstyp
unk    u8  = 1 bei Anfragen
err    u8  = 0 = OK
length u16   Länge der Nutzdaten
```

| Schritt | Richtung | Port | Status |
|---|---|---|---|
| GetDeviceInfo (type 0x01) | Client → Gerät | UDP 10005 | ✅ verifiziert (140 Byte Antwort) |
| START / OpenVideo (type 0x04) | Client → Gerät | UDP 10006, Payload = eigener Empfangsport (u16 LE) + `00 00` | ✅ verifiziert (ACK `eeffeeff 02000400 01000000`) |
| Videodaten | Gerät → Client | kommt von UDP 10006 an den gemeldeten Port | ✅ verifiziert, 17–18 fps, 480×480 |

**Wichtig:** GetDeviceInfo muss vom selben Socket **vor** START kommen. Ohne diesen Schritt bestätigt das Gerät START zwar, schickt aber kein Video.

Jedes Videopaket hat einen 16-Byte-Header (siehe unten), danach folgen JPEG-Bytes. Ein Bild beginnt mit `FF D8` und endet mit `FF D9`, danach können Füll-Nullen kommen. Die Auflösung ist 480×480 (MJPEG). Die 15–30 Pakete eines Bildes kommen als schneller Burst.

**Wichtig:** Das erste Paket nach Leerlauf wird oft verschluckt. Anfragen deshalb immer mehrfach senden. Nach einem START braucht das Otoskop einige hundert Millisekunden zum Anlaufen. Ein erneuter START in dieser Zeit startet es wieder neu, deshalb sollten zwischen zwei START-Befehlen mindestens ~800 ms liegen.

### Video-Paketkopf (16 Byte, am Gerät verifiziert)

| Byte | Bedeutung |
|---|---|
| 0 | immer `01` |
| 1 | laufende Paketnummer (8 Bit, läuft über) |
| 2 | Bildnummer (8 Bit) |
| 3 | `00`, im Referenz-Mitschnitt `01` beim letzten Paket eines Bildes |
| 4 | Anzahl Pakete im Bild |
| 5 | immer `01` |
| 6–9 | Beschleunigungssensor, u32 little-endian: x = Bits 0–9, y = 10–19, z = 20–29; je 10 Bit Vorzeichen-Betrag (Bit 9 = Vorzeichen), ~128 = 1 g |
| 10–11 | konstant `66 90` |
| 12–15 | `80 02 e0 01` (640/480 LE), obwohl die Bilder 480×480 sind |

Rollwinkel des Stifts = `atan2(x, y)`. Die Lage steht in **jedem** Paket eines Bildes. Die Achsen haben leichte Versätze (x ≈ −7, y ≈ +6) und etwas unterschiedliche Empfindlichkeit, deshalb gibt es die Kalibrierseite der Bridge. Die Kamera ist im Stift um 90° gedreht eingebaut, das Bild wird deshalb immer um −90° gedreht. Bilder sind 5–41 KB groß, das Gerät liefert 17–18 fps.

## Dateien

| Datei | Zweck | Status |
|---|---|---|
| `probe-soulear.py` | Schickt GetDeviceInfo und dekodiert die Antwort | ✅ funktioniert |
| `soulear-viewer.py` | Python-Viewer für die VM: GetDeviceInfo + START, Bilder zusammensetzen, MJPEG auf Port 45100 | ✅ funktioniert, abgelöst durch die ESP32-Bridge |
| `firmware/` | Firmware für den ZB-GW03 (ESP32 + LAN8720): WLAN zum Otoskop, MJPEG und Weboberfläche im LAN | ✅ im Einsatz, Details in [firmware/README.md](firmware/README.md) |

Die Python-Skripte kommen nur mit der Standardbibliothek aus, die Kamera-IP kann als Argument übergeben werden (Standard `192.168.1.1`).

## ESP32-Bridge (Stand 30.09.2026)

Ein ZB-GW03 v1.4 (eigentlich Zigbee-Gateway, ESP32 ohne PSRAM + LAN8720) hängt per WLAN am Otoskop und per LAN im Heimnetz. Die ESPHome-Firmware wurde per OTA ersetzt.

- **Erreichbar:** `http://otoskop.local/` (Ethernet-IP zuletzt `192.168.178.130`), Stream unter `/stream`
- **Funktionen:** MJPEG-Stream, Snapshot, Lagekorrektur im Browser mit Kalibrierseite, Firmware-Update im Browser, Notfall-Modus über das Heim-WLAN, Absturz-Mitschnitt und Ereignisprotokoll
- **Leistung:** 17 fps ohne Aussetzer bei gutem Signal (gemessen: 516 von 517 Bildern, längste Pause 92 ms)

Die wichtigsten Erkenntnisse (ausführlich in der Firmware-README):

1. **Der WLAN-Empfang stört Ethernet.** Der ESP32 erzeugt den 50-MHz-Takt für den LAN8720 selbst (GPIO17). Bei 100 Mbit gingen 2–3 % der Ethernet-Pakete verloren, bei 10 Mbit keine. Deshalb läuft Ethernet fest mit **10 Mbit**.
2. **`getFreeHeap()` täuscht:** Es zählt ~44 KB IRAM mit, die für `malloc` und Task-Stacks nicht nutzbar sind. Die Bilddaten liegen deshalb gezielt im IRAM-Rest und werden wortweise gelesen und geschrieben.
3. **Die Arduino-Voreinstellungen sind zu knapp:** Der UDP-Puffer fasste nur 6 Pakete. Über `custom_sdkconfig` (pioarduino) ist er auf 32 erhöht, und Bluetooth ist entfernt.
4. **lwIP wartet bis zu 1 s:** Ein blockierendes `send()` schläft bei kurzem Speichermangel bis zum Timer. Die Firmware sendet deshalb nicht blockierend.
5. **WLAN-Modus b/g ohne 11n:** Ohne Paketbündelung blockiert ein fehlendes Teilpaket nicht mehr den ganzen Block.

## Nächste Schritte

1. **Home Assistant:** MJPEG-Kamera mit `http://otoskop.local/stream` einrichten. Das Bild kommt dort ungedreht an.
2. **Alltagstest** mit Drehen und wechselnden Abständen. Die Zähler in `/status` (`stalls_loss`, `packets_lost`) zeigen, wie oft das Signal einbricht. Bei häufigen Aussetzern den ZB-GW03 näher an den Einsatzort stellen.
3. **Aufräumen, optional:** Test-Schalter (`/debug/...`, `/crashtest`, `/sensor`) entfernen oder als Diagnose behalten. Die Versuchsschalter am Ende von `firmware/include/config.h` können fest in den Code übernommen werden.

## Offene Ideen

- **LED-Steuerung am Otoskop:** Ein Befehlstyp `0x0A` (SetLed) ist in der i4season-Familie dokumentiert, am Gerät aber ungetestet.
- **Auflösung:** Mit `0x0D` (GetCameraConfig) die unterstützten Modi abfragen. Vorsicht: Bei der MS5 konnte ein Moduswechsel zur Laufzeit den Encoder blockieren, bis das Gerät neu gestartet wurde.
- **Regelmäßige Pausen des Otoskops:** Etwa alle 25 s gab es kurze Pausen ohne Paketverlust. Ein Lebenszeichen-START half nicht. Seit den letzten Änderungen sind sie nicht mehr aufgetreten, beobachtet wird weiter.
- **Hardware:** Ein ESP32-Board mit eigenem 50-MHz-Quarz für Ethernet (z. B. WT32-ETH01, Takt auf GPIO0) würde 100 Mbit trotz WLAN erlauben. Für einen Stream sind 10 Mbit aber genug.
- **Custom-Firmware fürs Otoskop:** Für den BK7231U gibt es zwar Community-Firmware (OpenBeken/LibreTiny-Umfeld), aber keinen Kameratreiber. Für dieses Projekt ist das derzeit keine realistische Option.

## Quellen / Referenzen

- pedrodinisf/otoscope-viewer: exakt dieselbe Hardware (AiSee, BK7231U, XRH), macOS-only. Enthält das Protokoll-Cheatsheet und Testfixtures. https://github.com/pedrodinisf/otoscope-viewer
- Fyfar/ms5-wifi-microscope: ausführlichste Protokoll-Doku der i4season-Familie (Header, Befehlstypen, Auflösung). https://github.com/Fyfar/ms5-wifi-microscope
- SeanPesce/Suear-Web-Viewer: MJPEG-Mirror für Suear-Geräte (gleiche Familie). https://github.com/SeanPesce/Suear-Web-Viewer
- Elektroda-Thread zu Taixen-TXW816-Otoskopen: UART/Firmware-Dump anderer Hardware, nur als Hintergrund. https://www.elektroda.com/news/news4129331.html