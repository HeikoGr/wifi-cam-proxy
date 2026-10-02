// Protocol probe for "automatic": a short UDP request/answer (see CamProtocol::probe)

#include <Arduino.h>
#include <lwip/sockets.h>

#include "camera.h"

bool probeUdp(uint32_t camIp, uint16_t port, uint16_t localPort, const uint8_t *req, size_t len,
              bool (*accept)(const uint8_t *data, int n), uint32_t waitMs) {
  int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s < 0) return false;
  // The session binds the same fixed port right after (JHCMD: 20000)
  int one = 1;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  sockaddr_in local = {};
  local.sin_family = AF_INET;
  local.sin_addr.s_addr = htonl(INADDR_ANY);
  local.sin_port = htons(localPort);
  if (bind(s, (sockaddr *)&local, sizeof(local)) != 0) {
    close(s);
    return false;
  }
  timeval tv = {0, 50 * 1000};
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  sockaddr_in to = {};
  to.sin_family = AF_INET;
  to.sin_addr.s_addr = camIp;
  to.sin_port = htons(port);

  bool ok = false;
  int sent = 0;
  uint32_t t0 = millis();
  while (!ok && millis() - t0 < waitMs) {
    if (sent == 0 || (sent == 1 && millis() - t0 >= waitMs / 2)) {
      sendto(s, req, len, 0, (sockaddr *)&to, sizeof(to));
      sent++;
    }
    uint8_t buf[256];
    sockaddr_in from = {};
    socklen_t flen = sizeof(from);
    int n = recvfrom(s, buf, sizeof(buf), 0, (sockaddr *)&from, &flen);
    ok = n > 0 && from.sin_addr.s_addr == camIp && accept(buf, n);
  }
  close(s);
  return ok;
}
