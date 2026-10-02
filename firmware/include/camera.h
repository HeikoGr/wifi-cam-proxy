#pragma once

// Kamera-Schicht: Erkennung (WLAN-Scan nach SSID-Mustern), Verbindung, Protokolle.
//
// Speicher: Der Code aller Protokolle liegt im Flash und wird von dort ausgeführt
// (XIP über den Cache), er kostet also kein RAM. RAM belegen nur Puffer und Zustand,
// und die gibt es nur für die gerade verbundene Kamera: die Sitzung des aktiven
// Protokolls wird beim Verbinden mit new angelegt und beim Wechsel wieder gelöscht.
// Ein Nachladen von Code aus dem Flash ist deshalb nicht nötig.

#include <Arduino.h>

#include <atomic>

#include "config.h"
#include "frame.h"

// --- Protokolle -----------------------------------------------------------------
enum class CamProto : uint8_t {
  None = 0,
  I4season,  // Soulear/Hopefox-Otoskope, MS5/MAX-VIEW-Mikroskope (UDP 10005/10006)
  Jhcmd,     // MaxSee/JoyHonest-Mikroskope (UDP 20000/10900, "JHCMD")
  Auto = 0xFF,
};
const char *protoKey(CamProto p);   // "i4season", "jhcmd", "auto", ""
const char *protoName(CamProto p);  // für die Weboberfläche
CamProto protoFromKey(const char *key);
// Erkennt die Kamera-Familie am SSID-Namen (Tabelle in camera.cpp), None = unbekannt
CamProto protoForSsid(const char *ssid);

// Eine verbundene Kamera. Läuft vollständig im Video-Task.
class CamSession {
 public:
  virtual ~CamSession() = default;
  // Empfängt höchstens ein Paket, kehrt spätestens nach ~200 ms zurück
  virtual void poll(uint8_t *pkt, size_t cap) = 0;
};
CamSession *createI4seasonSession(uint32_t camIp);
CamSession *createJhcmdSession(uint32_t camIp);

// --- Zustand, den Protokolle und Weboberfläche teilen ---------------------------
struct VideoStats {
  std::atomic<uint32_t> framesTotal{0};
  std::atomic<uint32_t> framesDropped{0};   // Summe der drei folgenden
  std::atomic<uint32_t> dropNoMem{0};       // kein Heap für das fertige Bild
  std::atomic<uint32_t> dropTooBig{0};      // größer als MAX_FRAME_BYTES
  std::atomic<uint32_t> dropIncomplete{0};  // Paket(e) im Bild verloren
  std::atomic<uint32_t> packetsLost{0};     // Lücken in der Sequenznummer
  std::atomic<uint32_t> framesDamaged{0};   // trotz Paketverlust angezeigt
  std::atomic<uint32_t> maxFrameBytes{0};
  std::atomic<uint32_t> handshakes{0};
  std::atomic<uint32_t> keepalives{0};
  // Stillstände: mit Paketverlust in den 2 s davor (Funk) oder ohne (Kamera pausiert
  // von sich aus). Von den sauberen die letzten Zeitpunkte (s seit Start).
  std::atomic<uint32_t> stallsLoss{0};
  std::atomic<uint32_t> stallsClean{0};
  static const int CLEAN_STALL_TIMES = 8;
  uint32_t cleanStallAt[CLEAN_STALL_TIMES];
};
extern VideoStats stats;

// Telemetrie der Kamera, -1 = unbekannt
struct CamTelemetry {
  std::atomic<bool> hasOrientation{false};  // Lagesensor liefert Werte
  std::atomic<int16_t> accX{0}, accY{0}, accZ{0};
  std::atomic<uint32_t> accSeq{0};
  std::atomic<int8_t> battery{-1};          // Akku in %
  std::atomic<int8_t> charging{-1};         // 1 = lädt (Bedeutung unsicher)
  std::atomic<int8_t> led{-1};              // letzter von der Kamera bestätigter Zustand
  std::atomic<bool> ledSupported{false};
  std::atomic<uint16_t> width{0}, height{0};  // laut Video-Kopf (Soulear meldet falsch 640x480)
  char vendor[33] = "", product[33] = "", firmware[17] = "";  // unter infoMux
  void reset();
};
extern CamTelemetry telemetry;
extern portMUX_TYPE infoMux;

// LED-Wunsch aus der Weboberfläche (-1 = nichts, 0/1); die Sitzung sendet ihn
extern std::atomic<int> ledRequest;

// Aus main.cpp
extern volatile bool rescueMode;
extern std::atomic<bool> updating;
// Darf die Sitzung jetzt Befehle an die Kamera schicken?
bool cameraLinkUp();

// --- Kameraverwaltung (camera.cpp) ----------------------------------------------
void cameraBegin();  // in setup(): NVS laden, Video-Task starten, erste Verbindung
void cameraLoop();   // in loop(): Scan, Auswahl, Wiederverbinden
void cameraOnWifiGotIp();
void cameraRestartWifi();  // nach Änderung des WLAN-Modus neu verbinden
// Auswahl aus der Weboberfläche. Leere SSID = Vorgabe löschen, wieder automatisch
bool cameraSelect(const char *ssid, const char *pass, CamProto proto);
void cameraRequestScan();
// JSON für /cameras in out schreiben, Rückgabe = Länge
size_t cameraJson(char *out, size_t len);
const char *cameraSsid();          // verbundene/gewählte Kamera ("" = keine)
CamProto cameraProto();            // Protokoll der aktiven Sitzung
