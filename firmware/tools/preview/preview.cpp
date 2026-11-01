// Host preview: drives the real App (core/) with scripted phone messages through the real
// framing and JSON parser, renders frames with the real renderer (ui/) into a 466×466 16-bit
// LovyanGFX sprite, masks it to the round glass and writes PNGs plus an overview grid.
//
// Build and run: tools/preview/render.sh   (writes Display/firmware/preview/*.png)

#include <LovyanGFX.hpp>

#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#include <ArduinoJson.h>

#include "core/app.h"
#include "core/mapdata.h"
#include "core/nav.h"
#include "core/protocol.h"
#include "png.h"
#include "ui/fonts.h"
#include "ui/icons.h"
#include "ui/mapdecode.h"
#include "ui/raster.h"
#include "ui/render.h"
#include "ui/rotate.h"

using namespace monav;

namespace {

// 2026-10-23 04:13:00 UTC = 06:13 in Germany (CEST), like the Monav mockups.
constexpr int64_t kPhone0 = 1792728780000LL;
constexpr int kSize = ui::kSize;
constexpr int kBandRows = 60;  // as on the device
int g_bandMismatch = 0;

bool g_verbose = false;

class FakePlatform : public Platform {
 public:
  std::string token;
  int bright = -1;
  int rot = -1;
  int lang = -1;
  int layout = -1;
  std::vector<std::string> sent;

  void send(const char* json, size_t len) override {
    sent.emplace_back(json, len);
    if (g_verbose) printf("    display -> phone: %.*s\n", static_cast<int>(len), json);
  }
  void disconnect() override {
    if (g_verbose) printf("    display drops the connection\n");
  }
  void setBrightness(int, float) override {}
  bool loadToken(char* out, size_t cap) override {
    snprintf(out, cap, "%s", token.c_str());
    return !token.empty();
  }
  void saveToken(const char* t) override { token = t; }
  void eraseToken() override { token.clear(); }
  int loadBrightness() override { return bright; }
  void saveBrightness(int b) override { bright = b; }
  int loadRotation() override { return rot; }
  void saveRotation(int d) override { rot = d; }
  int loadLang() override { return lang; }
  void saveLang(int l) override { lang = l; }
  int loadLayout() override { return layout; }
  void saveLayout(int l) override { layout = l; }
  void authorizationChanged(bool) override {}
  void pairingChanged(bool) override {}
  void log(const char* line) override {
    if (g_verbose) printf("    %s\n", line);
  }
};

constexpr const char* kToken = "9f0c4e1a7b2d4c8e9a1f3b5d7c9e0a2b";
constexpr const char* kOtherToken = "11112222333344445555666677778888";

/** One simulated display + phone. */
struct Sim {
  FakePlatform p;
  App app;
  uint64_t now = 5000;
  proto::Reassembler inbox;
  uint8_t seq = 0;
  bool driving = false;
  float kmh = 0;
  int lim = 0;
  uint64_t nextV = 0;
  // The phone's fix: advanced once a second along hdg, sent with every v (as Monav does).
  bool hasPos = false;
  double lat = 0, lon = 0;
  float hdg = 0;
  uint64_t nextFix = 0;
  const MapImage* map = nullptr;
  const RouteLine* route = nullptr;
  Frame::Update update;

  explicit Sim(bool paired, const char* id = "4F2A") : app(p, id) {
    if (paired) p.token = kToken;
    app.begin(now);
  }

  int64_t phone() const { return kPhone0 + static_cast<int64_t>(now) - 5000; }

  /** Sends one JSON message the way the phone would: chunked, through the reassembler. */
  void send(const std::string& json) {
    proto::fragment(json.data(), json.size(), 182, seq, [&](const uint8_t* chunk, size_t len) {
      if (inbox.feed(chunk, len)) {
        Message m;
        if (proto::parse(inbox.message(), inbox.length(), m)) {
          app.message(m, now);
        } else {
          printf("    !! JSON did not parse: %s\n", inbox.message());
        }
      }
    });
  }

  void sendf(const char* fmt, ...) {
    char buf[1100];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    send(buf);
  }

  void connect(const char* token = kToken, const char* lang = "de") {
    app.connected(now);
    sendf("{\"t\":\"hello\",\"proto\":1,\"token\":\"%s\",\"lang\":\"%s\",\"now\":%lld}", token, lang,
          static_cast<long long>(phone() - 25));
  }

  void vehicle() {
    char lim_[24] = "", pos[96] = "";
    if (lim > 0) snprintf(lim_, sizeof(lim_), ",\"lim\":%d", lim);
    if (hasPos) snprintf(pos, sizeof(pos), ",\"lat\":%.6f,\"lon\":%.6f,\"hdg\":%.1f", lat, lon, hdg);
    sendf("{\"t\":\"v\",\"now\":%lld,\"kmh\":%.1f%s%s}", static_cast<long long>(phone() - 30), kmh, lim_, pos);
  }

  /** Puts the car at a pixel of a map image, heading hdg. */
  void place(const MapImage& m, float px, float py, float heading) {
    localToGeo(m.lat, m.lon, (px - m.w / 2.0) * m.mpp, (m.h / 2.0 - py) * m.mpp, lat, lon);
    hdg = heading;
    hasPos = true;
  }

  void drive(float speed, int limit) {
    kmh = speed;
    lim = limit;
    driving = true;
    vehicle();
    nextV = now + 100;
  }

  /** Advance the clock in frame steps; while driving, `v` arrives 10 times a second. */
  void step(uint32_t ms) {
    const uint64_t end = now + ms;
    while (now < end) {
      now += 33;
      if (now > end) now = end;
      if (driving && hasPos && now >= nextFix) {
        if (nextFix != 0) {
          const double th = hdg * 3.14159265358979 / 180.0;
          localToGeo(lat, lon, kmh / 3.6 * std::sin(th), kmh / 3.6 * std::cos(th), lat, lon);
        }
        nextFix = now + 1000;
      }
      if (driving && now >= nextV) {
        vehicle();
        nextV = now + 100;
      }
      app.tick(now);
    }
  }
};

// --- Output ----------------------------------------------------------------------------------

struct Shot {
  std::string name;
  std::string title;
  std::vector<uint8_t> rgb;
};

lgfx::LGFX_Sprite* g_canvas = nullptr;
std::vector<Shot> g_shots;
std::string g_outDir;

void toRgb(lgfx::LGFX_Sprite& s, std::vector<uint8_t>& out) {
  out.resize(static_cast<size_t>(s.width()) * s.height() * 3);
  const uint16_t* px = static_cast<const uint16_t*>(s.getBuffer());
  for (int i = 0; i < s.width() * s.height(); i++) {
    const uint32_t c = gfx::unpack(px[i]);
    out[i * 3] = c >> 16;
    out[i * 3 + 1] = c >> 8;
    out[i * 3 + 2] = c;
  }
}

void shoot(Sim& sim, const char* name, const char* title) {
  Frame f;
  sim.app.frame(f, sim.now);
  f.map = sim.map;
  f.route = sim.route;
  f.update = sim.update;
  // The device draws band by band through internal RAM; drawn straight into the canvas, the
  // frame must come out the same.
  static std::vector<uint16_t> band0(kSize * kBandRows), band1(kSize * kBandRows);
  ui::setBandBuffers(nullptr, nullptr, 0);
  ui::render(*g_canvas, f);
  std::vector<uint16_t> direct(static_cast<const uint16_t*>(g_canvas->getBuffer()),
                               static_cast<const uint16_t*>(g_canvas->getBuffer()) + kSize * kSize);
  // Two drawing "tasks" like on the device (here one after the other): every other band each.
  ui::setBandBuffers(band0.data(), band1.data(), kBandRows);
  ui::setHelper([](void (*fn)(void*), void* arg) { fn(arg); }, [] {});
  const auto t0 = std::chrono::steady_clock::now();
  constexpr int kRuns = 10;
  for (int i = 0; i < kRuns; i++) ui::render(*g_canvas, f);
  const double ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / kRuns;
  int differ = 0;
  const uint16_t* banded = static_cast<const uint16_t*>(g_canvas->getBuffer());
  for (int i = 0; i < kSize * kSize; i++) differ += banded[i] != direct[i];
  if (differ > 0) {
    printf("  !! %s: %d pixels differ between banded and direct drawing\n", name, differ);
    g_bandMismatch++;
  }
  // Turned the way the device's push turns it.
  const gfx::Rotation rot(sim.app.rotation());
  if (!rot.none()) {
    std::vector<uint16_t> upright(banded, banded + kSize * kSize);
    gfx::rotateRows(upright.data(), kSize, kSize, rot, 0, kSize, static_cast<uint16_t*>(g_canvas->getBuffer()));
  }
  gfx::Surface s{static_cast<uint16_t*>(g_canvas->getBuffer()), kSize, kSize, 1.0f};
  gfx::maskCircle(s, kSize / 2.0f, kSize / 2.0f, kSize / 2.0f);
  Shot shot{name, title, {}};
  toRgb(*g_canvas, shot.rgb);
  const std::string path = g_outDir + "/" + name + ".png";
  png::write(path.c_str(), shot.rgb.data(), kSize, kSize);
  printf("  %-28s %-36s host render %.2f ms\n", (std::string(name) + ".png").c_str(), title, ms);
  g_shots.push_back(std::move(shot));
}

/** All shots on one sheet: a grid of glasses with their titles. */
void overview() {
  const int cols = 5;
  const int cell = 500, labelH = 56, margin = 40, headH = 110;
  const int rows = static_cast<int>((g_shots.size() + cols - 1) / cols);
  const int W = margin * 2 + cols * cell;
  const int H = headH + margin + rows * (cell + labelH) + margin;
  // The sheet colour as RGB565 can hold it, so the label strips (drawn in a 16-bit sprite) match.
  const uint32_t bg = gfx::unpack(gfx::pack(0x0C0C0E));
  std::vector<uint8_t> sheet(static_cast<size_t>(W) * H * 3, 0);
  for (size_t i = 0; i < sheet.size(); i += 3) {
    sheet[i] = bg >> 16;
    sheet[i + 1] = bg >> 8;
    sheet[i + 2] = bg;
  }

  lgfx::LGFX_Sprite label;
  label.setColorDepth(16);
  auto blitText = [&](const char* str, ui::Font font, int x, int y, int w, int h, uint32_t color, bool center) {
    label.createSprite(w, h);
    label.fillScreen(static_cast<uint32_t>(bg));
    ui::useFont(label, font);
    label.setTextColor(static_cast<uint32_t>(color));
    label.setTextDatum(center ? lgfx::textdatum_t::middle_center : lgfx::textdatum_t::middle_left);
    label.drawString(str, center ? w / 2 : 0, h / 2);
    std::vector<uint8_t> rgb;
    toRgb(label, rgb);
    for (int yy = 0; yy < h; yy++) {
      for (int xx = 0; xx < w; xx++) {
        const size_t d = (static_cast<size_t>(y + yy) * W + x + xx) * 3;
        const size_t s = (static_cast<size_t>(yy) * w + xx) * 3;
        sheet[d] = rgb[s];
        sheet[d + 1] = rgb[s + 1];
        sheet[d + 2] = rgb[s + 2];
      }
    }
    label.deleteSprite();
  };

  blitText("Monav Display \xc2\xb7 Vorschau", ui::Font::Title, margin, 30, 900, 44, 0xFFFFFF, false);
  blitText("Waveshare ESP32-S3-Touch-AMOLED-1.75 \xc2\xb7 466 \xc3\x97 466 \xc2\xb7 rendered by the firmware's own UI code",
           ui::Font::Body, margin, 72, 1400, 30, 0x98989F, false);

  for (size_t i = 0; i < g_shots.size(); i++) {
    const int c = static_cast<int>(i % cols), r = static_cast<int>(i / cols);
    const int x0 = margin + c * cell + (cell - kSize) / 2;
    const int y0 = headH + margin + r * (cell + labelH);
    // Bezel ring around the glass.
    for (int yy = -8; yy < kSize + 8; yy++) {
      for (int xx = -8; xx < kSize + 8; xx++) {
        const float dx = xx + 0.5f - kSize / 2.0f, dy = yy + 0.5f - kSize / 2.0f;
        const float d = std::sqrt(dx * dx + dy * dy);
        if (d > kSize / 2.0f + 0.5f && d < kSize / 2.0f + 7.5f) {
          const size_t o = (static_cast<size_t>(y0 + yy) * W + x0 + xx) * 3;
          sheet[o] = sheet[o + 1] = 0x22;
          sheet[o + 2] = 0x25;
        }
      }
    }
    const Shot& s = g_shots[i];
    for (int yy = 0; yy < kSize; yy++) {
      for (int xx = 0; xx < kSize; xx++) {
        const float dx = xx + 0.5f - kSize / 2.0f, dy = yy + 0.5f - kSize / 2.0f;
        if (dx * dx + dy * dy > (kSize / 2.0f + 0.5f) * (kSize / 2.0f + 0.5f)) continue;
        const size_t d = (static_cast<size_t>(y0 + yy) * W + x0 + xx) * 3;
        const size_t o = (static_cast<size_t>(yy) * kSize + xx) * 3;
        sheet[d] = s.rgb[o];
        sheet[d + 1] = s.rgb[o + 1];
        sheet[d + 2] = s.rgb[o + 2];
      }
    }
    blitText(s.title.c_str(), ui::Font::Small, margin + c * cell, y0 + kSize + 14, cell, 30, 0xC8C8CC, true);
  }
  const std::string path = g_outDir + "/overview.png";
  png::write(path.c_str(), sheet.data(), W, H);
  printf("  overview.png (%d x %d)\n", W, H);
}

// --- Scenarios -------------------------------------------------------------------------------

std::string light(const Sim& s, const char* st, int fromMs, int toMs, int d, int adv, const char* lane) {
  char buf[256];
  std::string extra;
  if (adv >= 0) extra += ",\"adv\":" + std::to_string(adv);
  if (lane) extra += std::string(",\"turn\":\"") + lane + "\"";
  if (toMs != 0) {
    snprintf(buf, sizeof(buf), "\"light\":{\"st\":\"%s\",\"from\":%lld,\"to\":%lld,\"d\":%d%s}", st,
             static_cast<long long>(s.phone() + fromMs), static_cast<long long>(s.phone() + toMs), d, extra.c_str());
  } else {
    snprintf(buf, sizeof(buf), "\"light\":{\"st\":\"%s\",\"d\":%d%s}", st, d, extra.c_str());
  }
  return buf;
}

void scene(Sim& s, const std::string& parts) {
  s.sendf("{\"t\":\"s\",\"at\":%lld%s%s}", static_cast<long long>(s.phone() - 40), parts.empty() ? "" : ",",
          parts.c_str());
}

// --- Maps ------------------------------------------------------------------------------------

/** A map image as the phone sends it (tools/preview/maps/sample-*.jpg + .json), decoded. */
struct Sample {
  std::string name;
  std::vector<uint8_t> jpg;
  std::vector<uint16_t> px;
  MapImage img;
  // From the .json: a drive along real streets (map pixels) and the car's place on it.
  std::vector<std::pair<float, float>> route;
  float carX = 512, carY = 512, carHdg = 0;
};
std::vector<Sample> g_samples;
std::string g_mapDir;

bool readFile(const std::string& path, std::vector<uint8_t>& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  return true;
}

/** A stand-in when no samples are there: dark blocks, a street grid, a river. */
void syntheticMap(Sample& s) {
  s.name = "synthetic";
  s.px.assign(mapPixels(1024, 1024), 0);
  for (int y = 0; y < 1024; y++) {
    for (int x = 0; x < 1024; x++) {
      uint32_t c = gfx::rgb(0x2E, 0x2C, 0x48);  // blocks
      if (x % 112 < 9 || y % 96 < 9) c = gfx::rgb(0x4A, 0x4E, 0x62);  // streets
      if (std::abs(x - 300 - static_cast<int>(40 * std::sin(y / 90.0))) < 14) c = gfx::rgb(0x1C, 0x2A, 0x6A);
      s.px[mapIndex(x, y, 1024)] = gfx::pack(c);
    }
  }
  s.img = MapImage{s.px.data(), 1024, 1024, 51.2211, 6.7841, 1.6f, 1, gfx::pack(gfx::rgb(0x2E, 0x2C, 0x48))};
}

void loadSamples() {
  for (const char* name : {"sample-berliner-allee", "sample-koenigsallee", "sample-a46"}) {
    Sample smp;
    smp.name = name;
    std::vector<uint8_t> meta;
    if (!readFile(g_mapDir + "/" + name + ".jpg", smp.jpg) || !readFile(g_mapDir + "/" + name + ".json", meta)) {
      continue;
    }
    JsonDocument doc;
    if (deserializeJson(doc, meta.data(), meta.size())) continue;
    const int w = doc["w"] | 1024, h = doc["h"] | 1024;
    smp.px.assign(mapPixels(w, h), 0);
    if (!ui::decodeMap(smp.jpg.data(), smp.jpg.size(), smp.px.data(), w, h)) {
      printf("  !! could not decode %s.jpg\n", name);
      continue;
    }
    smp.img = MapImage{smp.px.data(), w, h, doc["lat"].as<double>(), doc["lon"].as<double>(), doc["mpp"].as<float>(),
                       static_cast<uint8_t>(g_samples.size() + 1), ui::mapEdgeColour(smp.px.data(), w, h)};
    snprintf(smp.img.attr, sizeof(smp.img.attr), "%s", doc["attr"] | "");
    for (JsonArrayConst pt : doc["route"].as<JsonArrayConst>()) smp.route.emplace_back(pt[0].as<float>(), pt[1].as<float>());
    if (doc["car"].size() == 3) {
      smp.carX = doc["car"][0].as<float>();
      smp.carY = doc["car"][1].as<float>();
      smp.carHdg = doc["car"][2].as<float>();
    }
    g_samples.push_back(std::move(smp));
  }
  if (g_samples.empty()) {
    Sample smp;
    syntheticMap(smp);
    g_samples.push_back(std::move(smp));
    printf("  (no map samples in %s - using a synthetic map)\n", g_mapDir.c_str());
  }
  for (Sample& smp : g_samples) smp.img.px = smp.px.data();  // after the moves
}

const Sample& sampleNamed(const char* name) {
  for (const Sample& smp : g_samples) {
    if (smp.name == name) return smp;
  }
  return g_samples.front();
}

/** A route through map pixels, every ~10 m, in metres around the image centre (as Monav sends it). */
struct Route {
  std::vector<int16_t> pts;
  RouteLine line;
};

Route makeRoute(const MapImage& m, const std::vector<std::pair<float, float>>& p) {
  Route r;
  if (p.empty()) return r;
  for (size_t i = 0; i + 1 < p.size(); i++) {
    const float dx = (p[i + 1].first - p[i].first) * m.mpp, dy = (p[i + 1].second - p[i].second) * m.mpp;
    const int steps = std::max(1, static_cast<int>(std::sqrt(dx * dx + dy * dy) / 10.0f));
    for (int k = 0; k < steps; k++) {
      const float t = static_cast<float>(k) / steps;
      const float px = p[i].first + (p[i + 1].first - p[i].first) * t;
      const float py = p[i].second + (p[i + 1].second - p[i].second) * t;
      r.pts.push_back(static_cast<int16_t>(std::lround((px - m.w / 2.0f) * m.mpp)));
      r.pts.push_back(static_cast<int16_t>(std::lround((m.h / 2.0f - py) * m.mpp)));
    }
  }
  r.pts.push_back(static_cast<int16_t>(std::lround((p.back().first - m.w / 2.0f) * m.mpp)));
  r.pts.push_back(static_cast<int16_t>(std::lround((m.h / 2.0f - p.back().second) * m.mpp)));
  r.line = RouteLine{r.pts.data(), static_cast<int>(r.pts.size() / 2), m.lat, m.lon};
  return r;
}

/** A drive in the map layout: connected, driving, two taps (hud → focus → map). */
void mapDrive(Sim& s, const MapImage& m, float px, float py, float hdg, float kmh, int lim, const char* lang = "de") {
  s.step(2600);
  s.connect(kToken, lang);
  s.map = &m;
  s.place(m, px, py, hdg);
  s.drive(kmh, lim);
  s.step(400);
  s.app.gesture(Gesture{GestureType::Tap}, s.now);
  s.app.gesture(Gesture{GestureType::Tap}, s.now);
}

/** mapDrive at the sample's car, on its route. */
void sampleDrive(Sim& s, const Sample& smp, float kmh, int lim, const char* lang = "de") {
  mapDrive(s, smp.img, smp.carX, smp.carY, smp.carHdg, kmh, lim, lang);
}

void runAll() {
  {
    Sim s(false);
    s.step(1150);
    shoot(s, "01-boot", "Start");
  }
  {
    Sim s(false);
    s.step(2600);
    shoot(s, "02-waiting-pairable", "Warten \xc2\xb7 bereit zum Koppeln");
  }
  {
    Sim s(true);
    s.step(3100);
    shoot(s, "03-waiting", "Warten \xc2\xb7 gekoppelt");
  }
  {
    Sim s(false);
    s.step(2600);
    s.connect();
    s.step(1100);
    shoot(s, "04-paired", "Gekoppelt (neu)");
  }
  {
    Sim s(true);
    s.step(2600);
    s.connect(kOtherToken);
    s.step(600);
    shoot(s, "05-denied", "Anderes iPhone");
  }
  {
    Sim s(true);
    s.step(2600);
    s.connect();
    s.step(2000);
    shoot(s, "06-connected", "Verbunden");
  }
  {
    // Camera close, 58 in a 50: the ring pulses red; a light behind it as a chip.
    Sim s(true);
    s.step(2600);
    s.connect();
    s.drive(58, 50);
    s.step(500);
    scene(s, "\"cam\":{\"k\":\"fixed\",\"d\":180,\"vmax\":50,\"label\":\"Fester Blitzer\"}," +
                 light(s, "red", -20000, 21000, 260, 0, nullptr));
    s.step(160);
    shoot(s, "07-camera-too-fast", "Blitzer \xc2\xb7 zu schnell");
  }
  {
    Sim s(true);
    s.step(2600);
    s.connect();
    s.drive(52, 50);
    s.step(500);
    scene(s, light(s, "red", -20000, 14000, 190, 45, nullptr));
    s.step(300);
    shoot(s, "08-light-red-slower", "Ampel rot \xc2\xb7 langsamer");
  }
  {
    Sim s(true);
    s.step(2600);
    s.connect();
    s.drive(46, 50);
    s.step(500);
    scene(s, light(s, "green", -6000, 22000, 320, -1, "straight"));
    s.step(300);
    shoot(s, "09-light-green", "Ampel gr\xc3\xbcn \xc2\xb7 Tempo halten");
  }
  {
    Sim s(true);
    s.step(2600);
    s.connect();
    s.drive(48, 50);
    s.step(500);
    scene(s, light(s, "red", -9000, 21000, 260, 0, "left"));
    s.step(300);
    shoot(s, "10-light-coast", "Ampel rot \xc2\xb7 ausrollen");
  }
  {
    Sim s(true);
    s.step(2600);
    s.connect();
    s.drive(0, 50);
    s.step(500);
    scene(s, light(s, "red", -26000, 12000, 8, -1, nullptr));
    s.step(300);
    shoot(s, "11-light-standing", "Halt an Rot");
  }
  {
    Sim s(true);
    s.step(2600);
    s.connect();
    s.drive(46, 50);
    s.step(500);
    char parts[300];
    snprintf(parts, sizeof(parts),
             "\"turn\":{\"k\":\"left\",\"d\":300,\"road\":\"Herzogstra\xc3\x9f"
             "e\",\"eta\":%lld}",
             static_cast<long long>(s.phone() + 18 * 60000));
    scene(s, parts);
    s.step(300);
    shoot(s, "12-turn", "Navigation \xc2\xb7 links abbiegen");
    // The display hung at an angle: the app turns the picture back upright.
    s.send("{\"t\":\"cfg\",\"rot\":-25}");
    s.step(100);
    shoot(s, "34-rotated", "Gedreht (\xe2\x88\x92" "25\xc2\xb0)");
  }
  {
    Sim s(true);
    s.step(2600);
    s.connect();
    s.drive(38, 50);
    s.step(500);
    scene(s, "\"turn\":{\"k\":\"roundabout\",\"d\":180,\"road\":\"Richtung K\xc3\xb6ln\",\"exit\":2}," +
                 light(s, "green", -4000, 9000, 520, -1, nullptr));
    s.step(300);
    shoot(s, "13-roundabout", "Kreisverkehr \xc2\xb7 2. Ausfahrt");
  }
  {
    Sim s(true);
    s.step(2600);
    s.connect();
    s.drive(78, 80);
    s.step(500);
    scene(s, "\"sec\":{\"vmax\":80,\"avg\":76.5},\"turn\":{\"k\":\"keepRight\",\"d\":2400,\"road\":\"A 46\"}");
    s.step(300);
    shoot(s, "14-section", "Abschnittskontrolle");
  }
  {
    Sim s(true);
    s.step(2600);
    s.connect();
    s.drive(128, 130);
    s.step(500);
    scene(s, "\"cam\":{\"k\":\"mobile\",\"d\":820,\"vmax\":100,\"label\":\"Mobiler Blitzer\"}");
    s.step(300);
    shoot(s, "15-highway-mobile", "Autobahn \xc2\xb7 mobiler Blitzer");
  }
  {
    Sim s(true);
    s.step(2600);
    s.connect();
    s.drive(46, 50);
    s.step(500);
    s.app.gesture(Gesture{GestureType::Tap}, s.now);
    s.step(300);
    shoot(s, "16-focus", "Fokus (Tippen)");
  }
  {
    Sim s(true);
    s.step(2600);
    s.connect();
    s.drive(54, 0);
    s.step(600);
    shoot(s, "17-no-limit", "Ohne Tempolimit");
  }
  {
    Sim s(true);
    s.step(2600);
    s.connect();
    s.drive(52, 50);
    s.step(500);
    scene(s, light(s, "red", -20000, 14000, 190, 45, nullptr));
    s.step(300);
    s.driving = false;
    s.step(4200);
    shoot(s, "18-link-lost", "Verbindung unterbrochen");
  }
  {
    Sim s(true);
    s.step(2600);
    s.connect();
    s.drive(30, 50);
    s.step(500);
    s.driving = false;
    s.send("{\"t\":\"bye\"}");
    s.step(1500);
    shoot(s, "19-standby", "Standby (bye)");
  }
  {
    Sim s(true);
    s.step(2600);
    s.connect();
    s.drive(46, 50);
    s.step(500);
    s.app.gesture(Gesture{GestureType::LongPress}, s.now);
    s.step(1400);
    shoot(s, "20-reset-ask", "3 s halten \xe2\x86\x92 zur\xc3\xbc" "cksetzen?");
  }
  {
    Sim s(true);
    s.step(2600);
    s.connect();
    s.drive(46, 50);
    s.step(500);
    scene(s, light(s, "green", -6000, 22000, 320, -1, nullptr));
    s.app.gesture(Gesture{GestureType::BrightnessStep, -3}, s.now);
    s.step(200);
    shoot(s, "21-brightness", "Wischen \xe2\x86\x92 Helligkeit");
  }
  {
    Sim s(true);
    s.step(2600);
    s.connect(kToken, "en");
    s.drive(58, 50);
    s.step(500);
    scene(s, "\"cam\":{\"k\":\"redlight\",\"d\":340,\"vmax\":50,\"label\":\"Red-light camera\"}," +
                 light(s, "red", -20000, 14000, 190, 45, nullptr));
    s.step(300);
    shoot(s, "22-english-camera", "English \xc2\xb7 camera");
  }
  {
    Sim s(true);
    s.step(2600);
    s.connect(kToken, "en");
    s.drive(52, 50);
    s.step(500);
    scene(s, light(s, "uncertain", -20000, 9000, 230, 45, nullptr));
    s.step(300);
    shoot(s, "23-english-light", "English \xc2\xb7 light");
  }
  {
    Sim s(true);
    s.step(2600);
    s.connect(kToken, "en");
    s.step(400);
    s.app.disconnected(s.now);
    s.step(1200);
    shoot(s, "24-english-waiting", "English \xc2\xb7 waiting");
  }
  // --- Map layout ---
  {
    const Sample& smp = sampleNamed("sample-berliner-allee");
    const MapImage& m = smp.img;
    Route route = makeRoute(m, smp.route);
    Sim s(true);
    sampleDrive(s, smp, 48, 50);
    s.route = &route.line;
    char parts[200];
    snprintf(parts, sizeof(parts), "\"turn\":{\"k\":\"right\",\"d\":430,\"road\":\"Bahnstra\xc3\x9f" "e\",\"eta\":%lld}",
             static_cast<long long>(s.phone() + 9 * 60000));
    scene(s, parts);
    s.step(1500);
    shoot(s, "25-map-tilt", "Karte \xc2\xb7 geneigt, Route");
  }
  {
    const Sample& smp = sampleNamed("sample-berliner-allee");
    const MapImage& m = smp.img;
    Route route = makeRoute(m, smp.route);
    Sim s(true);
    sampleDrive(s, smp, 48, 50);
    s.route = &route.line;
    s.send("{\"t\":\"cfg\",\"tilt\":false}");
    scene(s, light(s, "red", -20000, 14000, 210, 40, nullptr));
    s.step(1500);
    shoot(s, "26-map-flat", "Karte \xc2\xb7 flach, Ampel");
  }
  // Charging stations from a plugin: free places only (the default), then free of all.
  auto chargers = [](Sim& s, const MapImage& m, bool totals) {
    const float pts[][4] = {{440, 680, 3, 6}, {478, 596, 0, 4}, {548, 522, 12, 16}, {612, 652, 1, 2}};
    std::string st;
    for (const auto& p : pts) {
      double lat, lon;
      localToGeo(m.lat, m.lon, (p[0] - m.w / 2.0) * m.mpp, (m.h / 2.0 - p[1]) * m.mpp, lat, lon);
      char one[80];
      if (totals) {
        snprintf(one, sizeof(one), "%s[%.6f,%.6f,%d,%d]", st.empty() ? "" : ",", lat, lon, (int)p[2], (int)p[3]);
      } else {
        snprintf(one, sizeof(one), "%s[%.6f,%.6f,%d]", st.empty() ? "" : ",", lat, lon, (int)p[2]);
      }
      st += one;
    }
    s.send("{\"t\":\"chg\",\"st\":[" + st + "]}");
  };
  {
    const Sample& smp = sampleNamed("sample-berliner-allee");
    const MapImage& m = smp.img;
    Route route = makeRoute(m, smp.route);
    Sim s(true);
    sampleDrive(s, smp, 48, 50);
    s.route = &route.line;
    chargers(s, m, false);
    s.step(1500);
    shoot(s, "35-map-chargers", "Karte \xc2\xb7 Lades\xc3\xa4ulen, frei");
  }
  {
    const Sample& smp = sampleNamed("sample-berliner-allee");
    const MapImage& m = smp.img;
    Sim s(true);
    sampleDrive(s, smp, 48, 50);
    s.send("{\"t\":\"cfg\",\"tilt\":false}");
    chargers(s, m, true);
    s.step(1500);
    shoot(s, "36-map-chargers-total", "Karte \xc2\xb7 Lades\xc3\xa4ulen, frei/alle");
  }
  {
    const Sample& smp = sampleNamed("sample-koenigsallee");
    const MapImage& m = smp.img;
    Route route = makeRoute(m, smp.route);
    Sim s(true);
    sampleDrive(s, smp, 37, 30);
    s.route = &route.line;
    scene(s, "\"cam\":{\"k\":\"fixed\",\"d\":160,\"vmax\":30,\"label\":\"Fester Blitzer\"}");
    s.step(1500);
    shoot(s, "27-map-camera", "Karte \xc2\xb7 Blitzer, zu schnell");
  }
  {
    const Sample& smp = sampleNamed("sample-a46");
    const MapImage& m = smp.img;
    Route route = makeRoute(m, smp.route);
    Sim s(true);
    sampleDrive(s, smp, 118, 120);
    s.route = &route.line;
    scene(s, "\"turn\":{\"k\":\"keepRight\",\"d\":1800,\"road\":\"Richtung K\xc3\xb6ln\"}");
    s.step(1500);
    shoot(s, "28-map-motorway", "Karte \xc2\xb7 Autobahn");
  }
  {
    const Sample& smp = sampleNamed("sample-berliner-allee");
    Sim s(true);
    sampleDrive(s, smp, 52, 50, "en");
    scene(s, light(s, "green", -8000, 16000, 280, -1, "straight"));
    s.step(1500);
    shoot(s, "29-map-english", "English \xc2\xb7 map");
  }
  {
    Sim s(true);
    sampleDrive(s, sampleNamed("sample-berliner-allee"), 48, 50);
    s.map = nullptr;  // nothing received yet
    scene(s, light(s, "red", -20000, 14000, 210, 40, nullptr));
    s.step(1500);
    shoot(s, "30-map-loading", "Karte l\xc3\xa4" "dt (noch kein Bild)");
  }
  {
    Sim s(true);
    s.step(2600);
    s.connect();
    s.step(500);
    s.update.phase = Frame::Update::Phase::Receiving;
    s.update.progress = 0.42f;
    snprintf(s.update.version, sizeof(s.update.version), "0.2.1");
    shoot(s, "31-update", "Update \xc3\xbc" "ber Bluetooth");
    s.update.phase = Frame::Update::Phase::Restarting;
    s.update.progress = 1.0f;
    shoot(s, "32-update-restart", "Update \xc2\xb7 Neustart");
    s.update.phase = Frame::Update::Phase::Failed;
    snprintf(s.update.error, sizeof(s.update.error), "SHA-256 stimmt nicht");
    shoot(s, "33-update-failed", "Update \xc2\xb7 fehlgeschlagen");
  }
}

/** Every icon at the size the cards draw it (preview/icons.png), for checking the glyphs. */
void iconSheet() {
  lgfx::LGFX_Sprite& c = *g_canvas;
  gfx::Surface s{static_cast<uint16_t*>(c.getBuffer()), kSize, kSize, 1.0f};
  gfx::clear(s, 0);
  const TurnKind turns[] = {TurnKind::Straight,  TurnKind::SlightLeft, TurnKind::Left,       TurnKind::SharpLeft,
                            TurnKind::SlightRight, TurnKind::Right,    TurnKind::SharpRight, TurnKind::UTurn,
                            TurnKind::KeepLeft,  TurnKind::KeepRight,  TurnKind::Roundabout, TurnKind::Arrive};
  const uint32_t blue = gfx::rgb(0x0A, 0x84, 0xFF), red = gfx::rgb(0xFF, 0x45, 0x3A);
  int i = 0;
  for (TurnKind k : turns) {
    const float cx = 60 + (i % 6) * 69, cy = 70 + (i / 6) * 72;
    gfx::fillCircle(s, cx, cy, 29, blue);
    ui::icons::turn(s, cx, cy, 19, k, 2, 0xFFFFFF, gfx::mix(blue, 0xFFFFFF, 0.4f), blue);
    i++;
  }
  for (int e = 1; e <= 4; e++) {
    const float cx = 60 + (e - 1) * 69, cy = 70 + 2 * 72;
    gfx::fillCircle(s, cx, cy, 29, blue);
    ui::icons::turn(s, cx, cy, 19, TurnKind::Roundabout, e, 0xFFFFFF, gfx::mix(blue, 0xFFFFFF, 0.4f), blue);
  }
  const CamKind cams[] = {CamKind::Fixed, CamKind::Mobile, CamKind::RedLight, CamKind::Section, CamKind::Distance,
                          CamKind::Other};
  i = 0;
  for (CamKind k : cams) {
    const float cx = 60 + i * 69, cy = 70 + 3 * 72;
    gfx::fillCircle(s, cx, cy, 29, red);
    ui::icons::camera(s, cx, cy, 15.5f, k, 0xFFFFFF, red);
    i++;
  }
  ui::icons::bluetooth(s, 60, 380, 20, 0xFFFFFF);
  ui::icons::sun(s, 129, 380, 20, 0xFFFFFF);
  ui::icons::phone(s, 198, 380, 20, 0xFFFFFF);
  ui::icons::reset(s, 267, 380, 20, 0xFFFFFF);
  ui::icons::chevrons(s, 336, 380, 14, true, 0xFFFFFF);
  ui::icons::check(s, 405, 380, 18, 5, 0xFFFFFF);
  std::vector<uint8_t> rgb;
  toRgb(c, rgb);
  png::write((g_outDir + "/icons.png").c_str(), rgb.data(), kSize, kSize);
  printf("  icons.png\n");
}

bool loadFonts(const std::string& dir, std::vector<std::vector<uint8_t>>& store) {
  constexpr int n = static_cast<int>(ui::Font::Count);
  const uint8_t* ptrs[n];
  store.resize(n);
  for (int i = 0; i < n; i++) {
    const std::string path = dir + "/" + ui::kFontFiles[i] + ".vlw";
    std::ifstream in(path, std::ios::binary);
    if (!in) {
      fprintf(stderr, "missing font %s\n", path.c_str());
      return false;
    }
    store[i].assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    ptrs[i] = store[i].data();
  }
  return ui::initFonts(ptrs);
}

}  // namespace

int runChecks();  // checks.cpp

int main(int argc, char** argv) {
  if (argc < 3) {
    fprintf(stderr, "usage: %s <fonts dir> <output dir> [-v]\n", argv[0]);
    return 2;
  }
  g_outDir = argv[2];
  g_verbose = argc > 3 && strcmp(argv[3], "-v") == 0;
  if (runChecks() != 0) return 1;
  std::vector<std::vector<uint8_t>> fonts;
  if (!loadFonts(argv[1], fonts)) return 1;
  g_mapDir = argc > 4 ? argv[4] : "tools/preview/maps";

  lgfx::LGFX_Sprite canvas;
  canvas.setColorDepth(16);
  if (!canvas.createSprite(kSize, kSize)) {
    fprintf(stderr, "no canvas\n");
    return 1;
  }
  canvas.setTextWrap(false);
  g_canvas = &canvas;

  printf("Rendering previews to %s\n", g_outDir.c_str());
  loadSamples();
  runAll();
  overview();
  ui::setBandBuffers(nullptr, nullptr, 0);
  iconSheet();
  if (g_bandMismatch) {
    printf("%d scenarios differ between banded and direct drawing\n", g_bandMismatch);
    return 1;
  }
  return 0;
}
