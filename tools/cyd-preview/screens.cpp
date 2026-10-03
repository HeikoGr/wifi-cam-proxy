// Draws the CYD screens with the real display code (firmware/src/main_cyd.cpp) into an
// in-memory canvas and writes them as PPM files: live image (1:1 and fit), menu, camera
// choice, waiting screen. Run by screenshots.py.
//
//   screens <frame.jpg> <rotation> <battery %> <camera SSID> <led: none|switch|dim> <out dir>
#include "../../firmware/src/main_cyd.cpp"

#include <string>
#include <vector>

// --- What main_cyd.cpp needs from the rest of the firmware ----------------------------
VideoStats stats;
CamTelemetry telemetry;
portMUX_TYPE infoMux = 0;
std::atomic<int> ledRequest{-1};
std::atomic<int> ledLevel{100};
void CamTelemetry::reset() {}
uint8_t *allocChunk(size_t len, size_t) { return (uint8_t *)malloc(len); }
void copyToChunk(uint8_t *d, const uint8_t *s, size_t n) { memcpy(d, s, n); }
void copyFromChunk(uint8_t *d, const uint8_t *c, size_t off, size_t n) { memcpy(d, c + off, n); }
unsigned heapFree() { return 0; }
unsigned heapMin() { return 0; }
uint32_t getFrame(Frame &out) { return 0; }
void crumb(const char *, ...) {}
void crashlogInit() {}
void cpuLoadUpdate() {}
int cpuLoad(int) { return -1; }
void cameraBegin() {}
void cameraLoop() {}
void cameraOnWifiGotIp() {}
bool cameraSelect(const char *, const char *, CamProto) { return true; }
void cameraRequestScan() {}
const char *cameraStateKey() { return "connected"; }
static int rotation = 0;
int cameraImageRotation() { return rotation; }
static const char *current = "";
void cameraCurrentSsid(char *out, size_t len) { snprintf(out, len, "%s", current); }
int cameraNetworks(ScanEntry *out, int max) {
  static const ScanEntry nets[] = {
      {"Soulear-6b1c9", -48, true, CamProto::I4season},
      {"MAXVIEW-7762", -55, true, CamProto::Jhcmd},
      {"wifi_camera_MS5_1A2B", -63, true, CamProto::I4season},
      {"HomeNetwork", -58, false, CamProto::None},
      {"Guest", -74, true, CamProto::None},
  };
  int n = 0;
  for (auto &e : nets)
    if (n < max) out[n++] = e;
  return n;
}
const char *protoKey(CamProto p) { return p == CamProto::I4season ? "i4season" : p == CamProto::Jhcmd ? "jhcmd" : p == CamProto::Auto ? "auto" : ""; }
int protoChoiceCount() { return 3; }
CamProto protoChoice(int i) { return i == 1 ? CamProto::I4season : i == 2 ? CamProto::Jhcmd : CamProto::Auto; }

// --- Screens ---------------------------------------------------------------------------
static std::string outDir;

static void save(const char *name) {  // in UI orientation (320x240)
  lcd.setRotation(UI_ROT);
  int w = lcd.width(), h = lcd.height();
  std::string path = outDir + "/" + name + ".ppm";
  FILE *f = fopen(path.c_str(), "wb");
  fprintf(f, "P6\n%d %d\n255\n", w, h);
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++) {
      auto p = lcd.readPixelRGB(x, y);
      uint8_t b[3] = {p.R8(), p.G8(), p.B8()};
      fwrite(b, 1, 3, f);
    }
  fclose(f);
  printf("%s\n", path.c_str());
}

// On the CYD the JHCMD session cuts runs of JPEG fill bytes (FF FF ...) in the scan
// data to one FF, JPEGDEC stops at them (JPEG_COLLAPSE_FILL). A /snapshot of the bridge
// still has them: do the same here.
static void collapseFill(std::vector<uint8_t> &j) {
  for (size_t i = 2; i + 4 < j.size(); i += 2 + (j[i + 2] << 8 | j[i + 3])) {
    if (j[i] != 0xFF) return;
    if (j[i + 1] != 0xDA) continue;
    size_t from = i + 2 + (j[i + 2] << 8 | j[i + 3]), to = from;
    for (size_t k = from; k < j.size(); k++)
      if (!(j[k] == 0xFF && k + 1 < j.size() && j[k + 1] == 0xFF)) j[to++] = j[k];
    j.resize(to);
    return;
  }
}

static void live(const Frame &f, bool full, const char *name) {
  showLive();
  zoomLevel = full ? Z_1TO1 : Z_FIT;
  shownFps = 17;
  lastFrameAt = millis();
  overlayShown[0] = 0;
  if (!drawFrame(f)) {
    fprintf(stderr, "could not decode the frame\n");
    exit(1);
  }
  drawOverlay();
  save(name);
}

int main(int argc, char **argv) {
  if (argc != 7) {
    fprintf(stderr, "usage: %s <frame.jpg> <rotation> <battery %%> <camera SSID> <led: none|switch|dim> <out dir>\n", argv[0]);
    return 1;
  }
  FILE *in = fopen(argv[1], "rb");
  if (!in) return perror(argv[1]), 1;
  std::vector<uint8_t> jpg;
  for (int c; (c = fgetc(in)) != EOF;) jpg.push_back(c);
  fclose(in);
  rotation = atoi(argv[2]);
  telemetry.battery = atoi(argv[3]);
  current = argv[4];
  telemetry.ledSupported = strcmp(argv[5], "none") != 0;
  telemetry.ledDimmable = !strcmp(argv[5], "dim");
  telemetry.led = 0;
  outDir = argv[6];
  collapseFill(jpg);

  // The frame as the camera delivers it: a list of UDP payloads
  Frame f = Frame::create();
  for (size_t i = 0; i < jpg.size(); i += 1400) f.append(&jpg[i], std::min<size_t>(1400, jpg.size() - i));

  lcd.init();
  lcd.setRotation(UI_ROT);
  jpeg = new JPEGDEC;
  live(f, true, "cyd-live");
  live(f, false, "cyd-live-fit");
  zoomLevel = Z_1TO1;  // the default
  showMenu();
  save("cyd-menu");
  showChoose();
  save("cyd-cameras");
  drawStatus("Looking for camera...");
  save("cyd-waiting");
  return 0;
}
