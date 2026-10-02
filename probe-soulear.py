#!/usr/bin/env python3
# probe-soulear.py
# Sends the i4season command "GetDeviceInfo" to the otoscope
# and prints what the device reveals about itself.
# Basis: protocol documentation from Fyfar/ms5-wifi-microscope
# and pedrodinisf/otoscope-viewer.

import socket
import struct
import sys

# --- Configuration -----------------------------------------------------
# IP of the otoscope: normally the default gateway in the Soulear Wi-Fi.
# If "ip route" shows something else, change it here or pass it as an argument.
CAMERA_IP = sys.argv[1] if len(sys.argv) > 1 else "192.168.1.1"
CMD_PORT = 10005      # command channel of i4season devices
TIMEOUT_S = 1.0       # seconds to wait per attempt
RETRIES = 5           # the first packet after idle is often lost

# --- Build the command ---------------------------------------------------
# 12-byte header, little-endian:
#   magic  (4 bytes) = 0xFFEEFFEE  -> on the wire: EE FF EE FF
#   id     (2 bytes) = running number, the device sends it back
#   type   (2 bytes) = 0x0001 -> GetDeviceInfo
#   unk    (1 byte)  = 1 in requests
#   err    (1 byte)  = 0
#   length (2 bytes) = 0, because we send no payload
MAGIC = 0xFFEEFFEE
MSG_ID = 0
TYPE_GET_DEVICE_INFO = 0x01

request = struct.pack("<IHHBBH", MAGIC, MSG_ID, TYPE_GET_DEVICE_INFO, 1, 0, 0)
print(f"Sending GetDeviceInfo to {CAMERA_IP}:{CMD_PORT} -> {request.hex(' ')}")

# --- Send and wait for a reply -------------------------------------------
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.settimeout(TIMEOUT_S)

reply = None
for attempt in range(1, RETRIES + 1):
    sock.sendto(request, (CAMERA_IP, CMD_PORT))
    try:
        reply, addr = sock.recvfrom(2048)
        print(f"Reply on attempt {attempt} from {addr[0]}:{addr[1]}, {len(reply)} bytes")
        break
    except socket.timeout:
        print(f"Attempt {attempt}: no reply")

sock.close()

if reply is None:
    print("\nNo reply. Possible reasons:")
    print(" - wrong IP (see 'ip route')")
    print(" - the device speaks a different protocol (then we need a capture)")
    sys.exit(1)

# --- Evaluate the reply ---------------------------------------------------
# Show the raw content first, so we see something even if it deviates
print("\nRaw data (hex):")
print(reply.hex(" "))

# Split the header (same format as the request)
magic, msg_id, msg_type, unk, err, length = struct.unpack("<IHHBBH", reply[:12])
print(f"\nHeader: magic=0x{magic:08X} id={msg_id} type=0x{msg_type:02X} err={err} length={length}")

if magic != MAGIC:
    print("Magic does not match -> different protocol than expected.")
    sys.exit(1)

# Payload per documentation:
#   1 byte unknown, 32 bytes vendor, 32 bytes product,
#   16 bytes firmware, 32 bytes SSID (each padded with zero bytes)
data = reply[12:]

def text(raw: bytes) -> str:
    """Cut off zero bytes and decode as text."""
    return raw.split(b"\x00", 1)[0].decode("ascii", errors="replace")

if len(data) >= 113:
    print("\nDevice info:")
    print(f"  Vendor:   {text(data[1:33])}")
    print(f"  Product:  {text(data[33:65])}")
    print(f"  Firmware: {text(data[65:81])}")
    print(f"  SSID:     {text(data[81:113])}")
else:
    print("Reply shorter than expected, please look at the raw data above.")
