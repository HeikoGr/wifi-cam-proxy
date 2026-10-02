#!/usr/bin/env python3
# probe_soulear.py
# Schickt den i4season-Befehl "GetDeviceInfo" an das Otoskop
# und gibt aus, was das Gerät über sich selbst verrät.
# Grundlage: Protokoll-Doku aus Fyfar/ms5-wifi-microscope
# und pedrodinisf/otoscope-viewer.

import socket
import struct
import sys

# --- Konfiguration ---------------------------------------------------
# IP des Otoskops: normalerweise das Default-Gateway im Soulear-WLAN.
# Falls "ip route" etwas anderes zeigt, hier anpassen oder als Argument übergeben.
CAMERA_IP = sys.argv[1] if len(sys.argv) > 1 else "192.168.1.1"
CMD_PORT = 10005      # Befehlskanal der i4season-Geräte
TIMEOUT_S = 1.0       # Sekunden warten pro Versuch
RETRIES = 5           # Das erste Paket nach Leerlauf geht oft verloren

# --- Befehl zusammenbauen -------------------------------------------
# 12-Byte-Header, little-endian:
#   magic  (4 Byte) = 0xFFEEFFEE  -> auf der Leitung: EE FF EE FF
#   id     (2 Byte) = laufende Nummer, das Gerät schickt sie zurück
#   type   (2 Byte) = 0x0001 -> GetDeviceInfo
#   unk    (1 Byte) = 1 bei Anfragen
#   err    (1 Byte) = 0
#   length (2 Byte) = 0, weil wir keine Nutzdaten mitschicken
MAGIC = 0xFFEEFFEE
MSG_ID = 0
TYPE_GET_DEVICE_INFO = 0x01

request = struct.pack("<IHHBBH", MAGIC, MSG_ID, TYPE_GET_DEVICE_INFO, 1, 0, 0)
print(f"Sende GetDeviceInfo an {CAMERA_IP}:{CMD_PORT} -> {request.hex(' ')}")

# --- Senden und auf Antwort warten ----------------------------------
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.settimeout(TIMEOUT_S)

reply = None
for attempt in range(1, RETRIES + 1):
    sock.sendto(request, (CAMERA_IP, CMD_PORT))
    try:
        reply, addr = sock.recvfrom(2048)
        print(f"Antwort bei Versuch {attempt} von {addr[0]}:{addr[1]}, {len(reply)} Bytes")
        break
    except socket.timeout:
        print(f"Versuch {attempt}: keine Antwort")

sock.close()

if reply is None:
    print("\nKeine Antwort. Mögliche Gründe:")
    print(" - falsche IP (siehe 'ip route')")
    print(" - Gerät spricht ein anderes Protokoll (dann brauchen wir einen Mitschnitt)")
    sys.exit(1)

# --- Antwort auswerten ----------------------------------------------
# Erst den Rohinhalt zeigen, damit wir auch bei Abweichungen etwas sehen
print("\nRohdaten (hex):")
print(reply.hex(" "))

# Header zerlegen (gleiches Format wie die Anfrage)
magic, msg_id, msg_type, unk, err, length = struct.unpack("<IHHBBH", reply[:12])
print(f"\nHeader: magic=0x{magic:08X} id={msg_id} type=0x{msg_type:02X} err={err} length={length}")

if magic != MAGIC:
    print("Magic passt nicht -> anderes Protokoll als erwartet.")
    sys.exit(1)

# Nutzdaten laut Doku:
#   1 Byte unbekannt, 32 Byte Hersteller, 32 Byte Produkt,
#   16 Byte Firmware, 32 Byte SSID (jeweils mit Nullbytes aufgefüllt)
data = reply[12:]

def text(raw: bytes) -> str:
    """Nullbytes abschneiden und als Text dekodieren."""
    return raw.split(b"\x00", 1)[0].decode("ascii", errors="replace")

if len(data) >= 113:
    print("\nGeräteinfo:")
    print(f"  Hersteller: {text(data[1:33])}")
    print(f"  Produkt:    {text(data[33:65])}")
    print(f"  Firmware:   {text(data[65:81])}")
    print(f"  SSID:       {text(data[81:113])}")
else:
    print("Antwort kürzer als erwartet, bitte Rohdaten oben ansehen.")
