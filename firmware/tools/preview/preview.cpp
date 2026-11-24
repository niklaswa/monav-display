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
#include <tuple>
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
  Style style;
  bool haveStyle = false;
  bool wallpaper = false;
  bool loadStyle(Style& out) override {
    if (haveStyle) out = style;
    return haveStyle;
  }
  void saveStyle(const Style& s) override {
    style = s;
    haveStyle = true;
  }
  bool hasWallpaper() override { return wallpaper; }
  bool icon = false;
  bool hasIcon(const char*) override { return icon; }
  std::vector<std::pair<uint32_t, bool>> actions;
  void noticeAction(uint32_t uid, bool positive) override { actions.emplace_back(uid, positive); }
  bool noticeLinkOn = false;
  void setNoticeLink(bool on) override { noticeLinkOn = on; }
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
  const Wallpaper* wallpaper = nullptr;
  const AppIcon* icon = nullptr;
  int64_t shift = 0;  // moves the phone's clock (a date, a time of day)
  Frame::Update update;

  explicit Sim(bool paired, const char* id = "4F2A", bool intro = false) : app(p, id) {
    if (paired) p.token = kToken;
    if (intro) {
      p.haveStyle = true;
      p.style.intro = true;
    }
    app.begin(now);
  }

  int64_t phone() const { return kPhone0 + shift + static_cast<int64_t>(now) - 5000; }

  /** The phone's clock at a local (German) date and time; call before connect(). */
  void at(int year, int month, int day, int hour, int minute, int second = 0);

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

void Sim::at(int year, int month, int day, int hour, int minute, int second) {
  // Days since 1970 (Howard Hinnant), German time: CEST from April to September is close enough
  // for the dates the shots use.
  const int y = year - (month <= 2 ? 1 : 0);
  const int era = y / 400;
  const int yoe = y - era * 400;
  const int doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const int64_t days = static_cast<int64_t>(era) * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468;
  const int offsetMin = month > 3 && month < 10 ? 120 : 60;
  const int64_t ms = ((days * 86400) + hour * 3600 + minute * 60 + second - offsetMin * 60) * 1000LL;
  shift = ms - (kPhone0 + static_cast<int64_t>(now) - 5000);
}

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
  f.wallpaper = sim.wallpaper;
  f.noticeIcon = strcmp(f.notice.app, "app.example.chat") == 0 ? sim.icon : nullptr;  // only the made-up chat app has one
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

std::vector<uint16_t> g_wallPx;
Wallpaper g_wall;
bool g_haveWall = false;

void loadWallpaper() {
  std::vector<uint8_t> jpg;
  if (!readFile(g_mapDir + "/sample-wallpaper.jpg", jpg)) return;
  g_wallPx.assign(static_cast<size_t>(Wallpaper::kSize) * Wallpaper::kSize, 0);
  if (!ui::decodeWallpaper(jpg.data(), jpg.size(), g_wallPx.data())) {
    printf("  !! could not decode sample-wallpaper.jpg\n");
    return;
  }
  g_wall.px = g_wallPx.data();
  g_haveWall = true;
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

// --- The iPhone's notifications --------------------------------------------------------------

std::vector<uint16_t> g_iconPx;
AppIcon g_icon;

/** A made-up messenger icon: a green gradient with a white speech bubble (no real app's artwork). */
void makeIcon() {
  constexpr int n = AppIcon::kSize;
  g_iconPx.assign(n * n, 0);
  for (int y = 0; y < n; y++) {
    for (int x = 0; x < n; x++) {
      const float t = static_cast<float>(y) / n;
      g_iconPx[y * n + x] = gfx::pack(gfx::mix(gfx::rgb(0x5B, 0xE5, 0x6E), gfx::rgb(0x12, 0xA8, 0x3E), t));
    }
  }
  gfx::Surface s{g_iconPx.data(), n, n, 1.0f, 0, 0};
  gfx::fillRoundRect(s, n * 0.2f, n * 0.24f, n * 0.6f, n * 0.44f, n * 0.2f, 0xFFFFFF);
  const float tail[6] = {n * 0.3f, n * 0.6f, n * 0.26f, n * 0.8f, n * 0.46f, n * 0.64f};
  gfx::convex(s, tail, 3, 0xFFFFFF);
  g_icon.px = g_iconPx.data();
}

ancs::Notice makeNotice(uint32_t uid, ancs::Category cat, const char* app, const char* appName, const char* title,
                        const char* message) {
  ancs::Notice n;
  n.uid = uid;
  n.category = cat;
  snprintf(n.app, sizeof(n.app), "%s", app);
  snprintf(n.appName, sizeof(n.appName), "%s", appName);
  snprintf(n.title, sizeof(n.title), "%s", title);
  snprintf(n.message, sizeof(n.message), "%s", message);
  return n;
}

void noticeShots() {
  const char* kMsg = "Bin in 10 Minuten da \xe2\x80\x93 soll ich noch Br\xc3\xb6tchen mitbringen? Und denk an den Schl\xc3\xbcssel f\xc3\xbcr die Garage.";
  auto parked = [](Sim& s, bool text) {
    s.step(2600);
    s.connect();
    s.sendf("{\"t\":\"cfg\",\"ntf\":\"all\",\"ntfText\":%s}", text ? "true" : "false");
    s.step(5000);  // past the greeting
  };
  {
    Sim s(true);
    s.icon = &g_icon;
    s.p.icon = true;
    parked(s, true);
    s.app.notice(makeNotice(7, ancs::Category::Social, "app.example.chat", "Chat", "Anna", kMsg), false, s.now);
    s.step(120);
    shoot(s, "47-notice-pop", "Mitteilung \xc2\xb7 Icon springt auf");
    s.step(500);
    shoot(s, "48-notice-icon", "Mitteilung \xc2\xb7 erst nur das Icon");
    s.step(600);
    shoot(s, "49-notice-move", "Mitteilung \xc2\xb7 Icon wandert hoch");
    s.step(1400);
    shoot(s, "50-notice", "Mitteilung \xc2\xb7 mit Text");
  }
  {
    Sim s(true);
    parked(s, false);
    s.app.notice(makeNotice(8, ancs::Category::Email, "app.example.mail", "Post", "Deutsche Bahn", kMsg), false, s.now);
    s.step(2600);
    shoot(s, "51-notice-noicon", "Mitteilung \xc2\xb7 ohne Icon, ohne Text");
  }
  {
    Sim s(true);
    parked(s, false);
    ancs::Notice call = makeNotice(9, ancs::Category::IncomingCall, "com.apple.mobilephone", "Telefon", "Mama", "");
    call.flags = ancs::flag::kPositiveAction | ancs::flag::kNegativeAction;
    s.app.notice(call, false, s.now);
    s.step(2400);
    shoot(s, "52-notice-call", "Anruf \xc2\xb7 annehmen / ablehnen");
    s.app.gesture(Gesture{GestureType::Tap, 0, static_cast<int>(233 + kCallButtonDx), static_cast<int>(kCallButtonY)}, s.now);
    if (s.p.actions.size() != 1 || !s.p.actions[0].second) printf("  !! tapping accept did not accept the call\n");
  }
  {
    Sim s(true);
    s.icon = &g_icon;
    s.p.icon = true;
    parked(s, true);
    s.drive(87, 100);
    s.step(1500);
    s.app.notice(makeNotice(10, ancs::Category::Social, "app.example.chat", "Chat", "Anna", kMsg), false, s.now);
    s.step(900);
    shoot(s, "53-notice-drive", "Mitteilung w\xc3\xa4hrend der Fahrt");
  }
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
    s.step(4400);
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
    s.step(App::kWelcomeMs);  // past the greeting
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
  // --- Themes, welcome, surprises (0.2.7) ---
  // A themed display: connected after `cfg` set the look; `welcome` shoots the greeting instead.
  auto themed = [](const char* theme, const char* name, bool welcome, Sim& s) {
    s.step(2600);
    s.connect();
    char cfg[160];
    snprintf(cfg, sizeof(cfg), "{\"t\":\"cfg\",\"theme\":\"%s\",\"name\":\"%s\"}", theme, name);
    s.send(cfg);
    s.step(welcome ? 1900 : 4400);
  };
  {
    Sim s(true);
    themed("classic", "", true, s);
    shoot(s, "37-welcome-classic", "Begr\xc3\xbc\xc3\x9fung \xc2\xb7 Klassisch");
  }
  {
    Sim s(true);
    s.at(2026, 11, 14, 18, 42, 20);
    themed("aurora", "Niklas", true, s);
    shoot(s, "38-welcome-aurora", "Begr\xc3\xbc\xc3\x9fung \xc2\xb7 Polarlicht, Name");
  }
  {
    Sim s(true);
    s.at(2026, 11, 14, 18, 44, 0);
    themed("aurora", "", false, s);
    shoot(s, "39-aurora", "Polarlicht \xc2\xb7 verbunden");
  }
  {
    Sim s(true);
    s.at(2026, 11, 14, 10, 9, 34);
    themed("orbit", "Niklas", true, s);
    shoot(s, "40-welcome-orbit", "Begr\xc3\xbc\xc3\x9fung \xc2\xb7 Orbit");
  }
  {
    Sim s(true);
    s.at(2026, 11, 14, 10, 9, 34);
    themed("orbit", "", false, s);
    shoot(s, "41-orbit", "Orbit \xc2\xb7 verbunden");
  }
  {
    Sim s(true);
    s.at(2026, 11, 14, 21, 5, 0);
    themed("retro", "Niklas", true, s);
    s.step(600);
    shoot(s, "42-welcome-retro", "Begr\xc3\xbc\xc3\x9fung \xc2\xb7 Retro");
  }
  {
    Sim s(true);
    s.at(2026, 11, 14, 21, 7, 0);
    themed("retro", "", false, s);
    shoot(s, "43-retro", "Retro \xc2\xb7 verbunden");
  }
  {
    Sim s(true);
    s.wallpaper = g_haveWall ? &g_wall : nullptr;
    s.p.wallpaper = g_haveWall;
    s.at(2026, 11, 14, 17, 30, 0);
    themed("photo", "Niklas", true, s);
    shoot(s, "44-welcome-photo", "Begr\xc3\xbc\xc3\x9fung \xc2\xb7 eigenes Bild");
  }
  {
    Sim s(true);
    s.wallpaper = g_haveWall ? &g_wall : nullptr;
    s.p.wallpaper = g_haveWall;
    s.at(2026, 11, 14, 17, 32, 0);
    themed("photo", "", false, s);
    shoot(s, "45-photo", "Eigenes Bild \xc2\xb7 verbunden");
  }
  {
    Sim s(true);
    s.at(2026, 11, 14, 17, 32, 0);
    themed("photo", "", false, s);
    shoot(s, "46-photo-missing", "Eigenes Bild fehlt \xe2\x86\x92 klassisch");
  }
  {
    // Seven quick taps while parked.
    Sim s(true);
    themed("classic", "", false, s);
    for (int i = 0; i < 7; i++) {
      s.app.gesture(Gesture{GestureType::Tap}, s.now);
      s.step(250);
    }
    s.step(1700);
    shoot(s, "47-egg-warp", "\xc3\x9c" "berraschung \xc2\xb7 7\xc3\x97 tippen");
  }
  {
    Sim s(true);
    themed("classic", "", false, s);
    s.drive(120, 0);
    s.step(1000);
    s.drive(142, 0);
    s.step(900);
    shoot(s, "48-egg-88mph", "\xc3\x9c" "berraschung \xc2\xb7 88 mph");
  }
  {
    Sim s(true);
    themed("classic", "", false, s);
    s.drive(42, 50);
    s.step(3600);
    shoot(s, "49-egg-42", "\xc3\x9c" "berraschung \xc2\xb7 42 km/h");
  }
  {
    Sim s(true);
    s.at(2026, 12, 24, 17, 12, 0);
    themed("classic", "", false, s);
    shoot(s, "50-christmas", "24.\xe2\x80\x93" "26. Dezember");
  }
  {
    Sim s(true);
    s.at(2027, 1, 1, 0, 14, 0);
    themed("retro", "", false, s);
    s.step(1300);
    shoot(s, "51-new-year", "Neujahr, erste Stunde");
  }
  {
    Sim s(true);
    s.at(2026, 11, 14, 11, 11, 0);
    themed("aurora", "", false, s);
    shoot(s, "52-wish", "11:11 \xc2\xb7 W\xc3\xbcnsch dir was");
  }
  {
    Sim s(true);
    s.at(2026, 11, 14, 13, 37, 0);
    themed("classic", "", false, s);
    shoot(s, "53-leet", "13:37");
  }
  {
    Sim s(true);
    s.at(2026, 11, 14, 22, 30, 0);
    themed("orbit", "", false, s);
    s.send("{\"t\":\"bye\"}");
    s.step(1000);
    shoot(s, "54-orbit-standby", "Orbit \xc2\xb7 Bis bald");
  }
  {
    Sim s(true);
    s.at(2026, 11, 14, 10, 9, 34);
    themed("chrono", "", false, s);
    shoot(s, "55-chrono", "Chrono \xc2\xb7 verbunden");
  }
  {
    Sim s(true);
    s.at(2026, 11, 14, 19, 52, 12);
    themed("chrono", "Niklas", true, s);
    shoot(s, "56-welcome-chrono", "Begr\xc3\xbc\xc3\x9fung \xc2\xb7 Chrono");
  }
  {
    // The clock layout set from the app: the chronograph stays during the drive.
    Sim s(true);
    s.at(2026, 11, 14, 8, 21, 47);
    themed("chrono", "", false, s);
    s.send("{\"t\":\"cfg\",\"layout\":\"clock\"}");
    s.drive(53, 50);
    s.step(800);
    scene(s, "\"cam\":{\"k\":\"fixed\",\"d\":320,\"vmax\":50,\"label\":\"Fester Blitzer\"}");
    s.step(500);
    shoot(s, "57-clock-drive", "Uhr w\xc3\xa4hrend der Fahrt \xc2\xb7 Blitzer");
  }
  {
    Sim s(true);
    s.at(2026, 11, 14, 16, 4, 5);
    themed("classic", "", false, s);
    s.send("{\"t\":\"cfg\",\"layout\":\"clock\"}");
    s.drive(118, 120);
    s.step(1300);
    shoot(s, "58-clock-drive-motorway", "Uhr w\xc3\xa4hrend der Fahrt \xc2\xb7 120");
  }
  {
    Sim s(true);
    s.at(2026, 11, 14, 10, 9, 34);
    themed("stopwatch", "", false, s);
    shoot(s, "59-stopwatch", "Stoppuhr \xc2\xb7 verbunden");
  }
  {
    Sim s(true);
    s.at(2026, 11, 14, 19, 52, 12);
    themed("stopwatch", "Niklas", true, s);
    s.step(900);
    shoot(s, "60-welcome-stopwatch", "Begr\xc3\xbc\xc3\x9fung \xc2\xb7 Stoppuhr");
  }
  {
    Sim s(true);
    s.at(2026, 11, 14, 8, 21, 47);
    themed("stopwatch", "", false, s);
    s.send("{\"t\":\"cfg\",\"layout\":\"clock\"}");
    s.drive(57, 50);
    s.step(1300);
    shoot(s, "61-stopwatch-drive", "Stoppuhr w\xc3\xa4hrend der Fahrt");
  }
  // 0–100 km/h: standing, then evenly to 100 in 4.3 s.
  auto launchRun = [](Sim& s, uint32_t untilMs) {
    s.step(2600);
    s.connect();
    s.send("{\"t\":\"cfg\",\"launch\":true}");
    s.step(App::kWelcomeMs);
    s.drive(0, 0);
    s.step(1600);
    for (uint32_t t = 0; t < untilMs; t += 33) {
      s.kmh = std::min(118.0f, t * 100.0f / 4300.0f);
      s.step(33);
    }
  };
  // The performance intro: stars, the tunnel, the jump, the flash — then the greeting.
  for (auto [name, title, ms] : {std::make_tuple("67-intro-stars", "Performance-Intro \xc2\xb7 Sterne", 500),
                                 std::make_tuple("68-intro-tunnel", "Performance-Intro \xc2\xb7 Tunnel", 1100),
                                 std::make_tuple("69-intro-jump", "Performance-Intro \xc2\xb7 Sprung", 1900),
                                 std::make_tuple("70-intro-flash", "Performance-Intro \xc2\xb7 Blitz", 2750),
                                 std::make_tuple("71-intro-welcome", "Performance-Intro \xc2\xb7 Begr\xc3\xbc\xc3\x9fung", 3350)}) {
    Sim s(true);
    s.at(2026, 11, 16, 18, 5, 0);
    s.step(2600);
    s.connect();
    s.send("{\"t\":\"cfg\",\"intro\":true,\"name\":\"Niklas\"}");
    // The intro is set after this hello: the next connection plays it.
    s.app.disconnected(s.now);
    s.step(500);
    s.connect();
    s.step(ms);
    shoot(s, name, title);
  }
  {
    // Powered up with the intro on: the jump instead of the traffic light, then waiting.
    Sim s(true, "4F2A", true);
    s.step(1700);
    shoot(s, "72-boot-intro", "Start mit Performance-Intro");
    s.step(1500);
    shoot(s, "73-boot-intro-waiting", "Nach dem Intro \xc2\xb7 warten");
  }
  // At the charger: the ring fills in, then the energy runs.
  for (auto [name, title, soc, kw, ms] : {std::make_tuple("64-charging-in", "Laden \xc2\xb7 f\xc3\xbcllt sich", 64.0, 142.0, 900),
                                          std::make_tuple("65-charging", "Laden \xc2\xb7 Supercharger", 64.0, 142.0, 3200),
                                          std::make_tuple("66-charging-low", "Laden \xc2\xb7 Akku leer, AC", 12.0, 10.8, 3200)}) {
    Sim s(true);
    s.step(2600);
    s.connect();
    s.step(App::kWelcomeMs);
    char bat[96];
    snprintf(bat, sizeof(bat), "{\"t\":\"bat\",\"soc\":%.1f,\"kw\":%.1f,\"km\":%d}", soc, kw, static_cast<int>(soc * 5.1));
    s.send(bat);
    s.step(ms);
    shoot(s, name, title);
  }
  {
    Sim s(true);
    launchRun(s, 2700);
    shoot(s, "62-launch-live", "0\xe2\x80\x93" "100 \xc2\xb7 l\xc3\xa4uft");
  }
  {
    Sim s(true);
    launchRun(s, 4900);
    shoot(s, "63-launch-best", "0\xe2\x80\x93" "100 \xc2\xb7 neue Bestzeit");
  }
}


// --- Showreel (-reel): every design animated, frame by frame ----------------------------------------

int g_reelFrame = 0;
FILE* g_reelCaptions = nullptr;

/** Films `ms` of the sim at 30 frames a second; `each` runs before every frame (t in seconds). */
void film(Sim& sim, uint32_t ms, const char* title, const std::function<void(Sim&, float)>& each = nullptr) {
  fprintf(g_reelCaptions, "%d\t%s\n", g_reelFrame, title);
  ui::setBandBuffers(nullptr, nullptr, 0);
  std::vector<uint8_t> rgb;
  for (uint32_t t = 0; t < ms; t += 33) {
    if (each) each(sim, t / 1000.0f);
    sim.step(33);
    Frame f;
    sim.app.frame(f, sim.now);
    f.map = sim.map;
    f.route = sim.route;
    f.wallpaper = sim.wallpaper;
    f.noticeIcon = strcmp(f.notice.app, "app.example.chat") == 0 ? sim.icon : nullptr;  // only the made-up chat app has one
    f.update = sim.update;
    ui::render(*g_canvas, f);
    gfx::Surface sf{static_cast<uint16_t*>(g_canvas->getBuffer()), kSize, kSize, 1.0f};
    gfx::maskCircle(sf, kSize / 2.0f, kSize / 2.0f, kSize / 2.0f);
    toRgb(*g_canvas, rgb);
    char path[512];
    snprintf(path, sizeof(path), "%s/frame_%05d.png", g_outDir.c_str(), g_reelFrame++);
    png::write(path, rgb.data(), kSize, kSize);
  }
}

void reel() {
  const std::string captions = g_outDir + "/captions.tsv";
  g_reelCaptions = fopen(captions.c_str(), "w");
  if (g_reelCaptions == nullptr) return;
  auto look = [](Sim& s, const char* theme, const char* name) {
    s.step(2600);
    s.connect();
    char cfg[160];
    snprintf(cfg, sizeof(cfg), "{\"t\":\"cfg\",\"theme\":\"%s\",\"name\":\"%s\"}", theme, name);
    s.send(cfg);
  };
  {
    Sim s(true);
    film(s, 2600, "Start");
  }
  {
    Sim s(true);
    s.at(2026, 11, 14, 7, 42, 10);
    look(s, "classic", "");
    film(s, 5200, "Klassisch \xc2\xb7 Begr\xc3\xbc\xc3\x9fung beim Einsteigen");
  }
  {
    Sim s(true);
    s.at(2026, 11, 14, 18, 42, 20);
    look(s, "aurora", "Niklas");
    film(s, 5600, "Polarlicht");
  }
  {
    Sim s(true);
    s.at(2026, 11, 14, 10, 9, 30);
    look(s, "orbit", "Niklas");
    film(s, 5200, "Orbit");
  }
  {
    Sim s(true);
    s.at(2026, 11, 14, 21, 5, 0);
    look(s, "retro", "Niklas");
    film(s, 5600, "Retro");
  }
  {
    Sim s(true);
    s.wallpaper = g_haveWall ? &g_wall : nullptr;
    s.p.wallpaper = g_haveWall;
    s.at(2026, 11, 14, 17, 30, 0);
    look(s, "photo", "Niklas");
    film(s, 5200, "Eigenes Bild");
  }
  {
    Sim s(true);
    s.at(2026, 11, 14, 19, 52, 12);
    look(s, "chrono", "Niklas");
    film(s, 5600, "Chrono");
  }
  {
    Sim s(true);
    s.at(2026, 11, 14, 8, 21, 5);
    look(s, "stopwatch", "Niklas");
    film(s, 5600, "Stoppuhr");
  }
  {
    Sim s(true);
    s.at(2026, 11, 14, 8, 30, 0);
    s.step(2600);
    s.connect();
    s.drive(20, 50);
    s.step(300);
    scene(s, light(s, "red", -20000, 9000, 230, 0, nullptr));
    film(s, 4500, "Fahrt \xc2\xb7 Ampel", [](Sim& sim, float t) {
      sim.kmh = std::max(0.0f, 38 - t * 9);
    });
  }
  {
    const Sample& smp = sampleNamed("sample-berliner-allee");
    Route route = makeRoute(smp.img, smp.route);
    Sim s(true);
    s.at(2026, 11, 14, 8, 31, 0);
    sampleDrive(s, smp, 46, 50);
    s.route = &route.line;
    scene(s, "\"turn\":{\"k\":\"right\",\"d\":430,\"road\":\"Bahnstra\xc3\x9f" "e\"}");
    film(s, 4500, "Fahrt \xc2\xb7 Karte");
  }
  {
    Sim s(true);
    s.at(2026, 11, 14, 8, 33, 40);
    look(s, "chrono", "");
    s.send("{\"t\":\"cfg\",\"layout\":\"clock\"}");
    s.drive(30, 50);
    s.step(400);
    scene(s, "\"cam\":{\"k\":\"fixed\",\"d\":420,\"vmax\":50,\"label\":\"Fester Blitzer\"}");
    film(s, 5000, "Uhr w\xc3\xa4hrend der Fahrt \xc2\xb7 Chrono", [](Sim& sim, float t) {
      sim.kmh = 30 + t * 6;
    });
  }
  {
    Sim s(true);
    s.at(2026, 11, 14, 8, 35, 10);
    look(s, "stopwatch", "");
    s.send("{\"t\":\"cfg\",\"layout\":\"clock\"}");
    s.drive(70, 100);
    s.step(400);
    film(s, 4500, "Uhr w\xc3\xa4hrend der Fahrt \xc2\xb7 Stoppuhr", [](Sim& sim, float t) {
      sim.kmh = 70 + t * 7;
    });
  }
  {
    Sim s(true);
    s.at(2026, 11, 16, 18, 5, 0);
    look(s, "classic", "Niklas");
    s.send("{\"t\":\"cfg\",\"intro\":true}");
    s.app.disconnected(s.now);
    s.step(400);
    s.connect();
    film(s, 6200, "Performance-Intro");
  }
  {
    Sim s(true);
    look(s, "classic", "");
    s.step(4400);
    s.send("{\"t\":\"bat\",\"soc\":71,\"kw\":146,\"km\":362}");
    film(s, 6000, "Laden", [](Sim& sim, float t) {
      static int last = -1;
      if (t < 0.05f) last = -1;
      const int sec = static_cast<int>(t);
      if (sec != last) {
        last = sec;
        char bat[96];
        snprintf(bat, sizeof(bat), "{\"t\":\"bat\",\"soc\":%.1f,\"kw\":%.0f,\"km\":%d}", 71 + t * 0.4, 146 - t * 2,
                 static_cast<int>(362 + t * 2));
        sim.send(bat);
      }
    });
  }
  {
    Sim s(true);
    look(s, "classic", "");
    s.send("{\"t\":\"cfg\",\"launch\":true}");
    s.step(4400);
    s.drive(0, 0);
    s.step(1600);
    film(s, 9500, "0\xe2\x80\x93" "100 km/h", [](Sim& sim, float t) {
      sim.kmh = t < 0.6f ? 0.0f : std::min(122.0f, (t - 0.6f) * 100.0f / 4.3f);
    });
  }
  {
    Sim s(true);
    look(s, "classic", "");
    s.step(4400);
    film(s, 6600, "\xc3\x9c" "berraschung \xc2\xb7 7\xc3\x97 aufs Glas tippen", [](Sim& sim, float t) {
      const int i = static_cast<int>(t / 0.2f);
      static int last = -1;
      if (t < 0.05f) last = -1;
      if (t < 1.4f && i != last) {
        last = i;
        sim.app.gesture(Gesture{GestureType::Tap}, sim.now);
      }
    });
  }
  {
    Sim s(true);
    look(s, "classic", "");
    s.step(4400);
    s.drive(128, 0);
    film(s, 5000, "\xc3\x9c" "berraschung \xc2\xb7 88 mph", [](Sim& sim, float t) {
      sim.kmh = std::min(142.0f, 128 + t * 14);
    });
  }
  {
    Sim s(true);
    look(s, "classic", "");
    s.step(4400);
    s.drive(42, 50);
    film(s, 6500, "\xc3\x9c" "berraschung \xc2\xb7 42 km/h");
  }
  {
    Sim s(true);
    s.at(2026, 12, 24, 17, 12, 0);
    look(s, "classic", "");
    s.step(4400);
    film(s, 4000, "24.\xe2\x80\x93" "26. Dezember");
  }
  {
    Sim s(true);
    s.at(2027, 1, 1, 0, 0, 30);
    look(s, "retro", "");
    s.step(4400);
    film(s, 4500, "Neujahr");
  }
  {
    Sim s(true);
    s.at(2026, 11, 14, 11, 11, 0);
    look(s, "aurora", "");
    s.step(4400);
    film(s, 3500, "11:11");
  }
  fclose(g_reelCaptions);
  printf("  %d frames\n", g_reelFrame);
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

/** The iPhone's notifications in one drive: a pill while moving, StandBy-like at the red light, a call. */
void noticeReel() {
  const std::string captions = g_outDir + "/captions.tsv";
  g_reelCaptions = fopen(captions.c_str(), "w");
  if (g_reelCaptions == nullptr) return;
  const Sample& smp = sampleNamed("sample-berliner-allee");
  Route route = makeRoute(smp.img, smp.route);
  Sim s(true);
  s.icon = &g_icon;
  s.p.icon = true;
  s.at(2026, 11, 14, 8, 31, 0);
  sampleDrive(s, smp, 46, 50, "en");
  s.route = &route.line;
  s.sendf("{\"t\":\"cfg\",\"ntf\":\"all\",\"ntfText\":true}");
  scene(s, "\"turn\":{\"k\":\"right\",\"d\":430,\"road\":\"Bahnstra\xc3\x9f" "e\"}");
  s.step(3000);
  film(s, 1800, "Driving \xc2\xb7 map");
  s.app.notice(makeNotice(21, ancs::Category::Social, "app.example.chat", "Chat", "Anna",
                          "Ten minutes away \xe2\x80\x93 want me to grab some coffee on the way?"),
               false, s.now);
  film(s, 4200, "While moving \xc2\xb7 small, the speed stays");
  film(s, 3000, "Red light", [](Sim& sim, float t) { sim.kmh = std::max(0.0f, 46 - t * 17); });
  s.kmh = 0;
  s.app.notice(makeNotice(22, ancs::Category::Social, "app.example.chat", "Chat", "Jonas",
                          "Are we still on for dinner at seven? I'll bring dessert."),
               false, s.now);
  film(s, 6200, "Stopped \xc2\xb7 like StandBy: the icon first, then the text");
  ancs::Notice call = makeNotice(23, ancs::Category::IncomingCall, "com.apple.mobilephone", "Phone", "Mom", "");
  call.flags = ancs::flag::kPositiveAction | ancs::flag::kNegativeAction;
  s.app.notice(call, false, s.now);
  film(s, 3600, "Call \xc2\xb7 answer or decline on the glass");
  s.app.gesture(Gesture{GestureType::Tap, 0, static_cast<int>(233 + kCallButtonDx), static_cast<int>(kCallButtonY)}, s.now);
  s.app.noticeRemoved(23, s.now);
  film(s, 1200, "Answered");
  film(s, 2400, "Green \xc2\xb7 off again", [](Sim& sim, float t) { sim.kmh = std::min(46.0f, t * 16); });
  fclose(g_reelCaptions);
  printf("  %d frames\n", g_reelFrame);
}

/**
 * The store promo (-promo [de|en]): 15 s for the App Store videos, one continuous drive. The chronograph
 * greets on getting in, stays on during the first metres with the speed in its dials, then the HUD with
 * a message arriving, then a fixed camera ahead.
 */
void promoReel(const char* lang) {
  const bool en = strcmp(lang, "en") == 0;
  const std::string captions = g_outDir + "/captions.tsv";
  g_reelCaptions = fopen(captions.c_str(), "w");
  if (g_reelCaptions == nullptr) return;
  Sim s(true);
  s.icon = &g_icon;
  s.p.icon = true;
  s.at(2026, 11, 14, 7, 41, 52);
  s.step(2600);
  s.connect(kToken, lang);
  s.send("{\"t\":\"cfg\",\"theme\":\"chrono\",\"name\":\"\",\"ntf\":\"all\",\"ntfText\":true}");
  film(s, 3600, "Welcome");
  s.send("{\"t\":\"cfg\",\"layout\":\"clock\"}");
  s.drive(0, 50);
  film(s, 3900, "Clock while driving", [](Sim& sim, float t) { sim.kmh = std::min(47.0f, t * 15.0f); });
  s.send("{\"t\":\"cfg\",\"layout\":\"hud\"}");
  film(s, 700, "HUD");
  s.app.notice(makeNotice(31, ancs::Category::Social, "app.example.chat", "Chat", "Anna",
                          en ? "Ten minutes away \xe2\x80\x93 want me to grab some coffee on the way?"
                             : "Bin in zehn Minuten da \xe2\x80\x93 soll ich Kaffee mitbringen?"),
               false, s.now);
  film(s, 3400, "Message");
  s.drive(54, 50);
  s.step(200);
  scene(s, en ? "\"cam\":{\"k\":\"fixed\",\"d\":260,\"vmax\":50,\"label\":\"Fixed camera\"}"
              : "\"cam\":{\"k\":\"fixed\",\"d\":260,\"vmax\":50,\"label\":\"Fester Blitzer\"}");
  film(s, 3400, "Camera", [](Sim& sim, float t) { sim.kmh = std::max(48.0f, 54.0f - t * 3.0f); });
  fclose(g_reelCaptions);
  printf("  %d frames\n", g_reelFrame);
}

/** Single glasses for the store screenshots in either language (-stills [de|en]): store-*.png. */
void storeStills(const char* lang) {
  const bool en = strcmp(lang, "en") == 0;
  {
    Sim s(true);
    s.step(2600);
    s.connect(kToken, lang);
    s.drive(46, 50);
    s.step(500);
    scene(s, light(s, "green", -6000, 22000, 320, -1, "straight"));
    s.step(300);
    shoot(s, "store-light", "light");
  }
  {
    const Sample& smp = sampleNamed("sample-a46");
    Route route = makeRoute(smp.img, smp.route);
    Sim s(true);
    sampleDrive(s, smp, 118, 120, lang);
    s.route = &route.line;
    scene(s, en ? "\"turn\":{\"k\":\"keepRight\",\"d\":1800,\"road\":\"Cologne\"}"
                : "\"turn\":{\"k\":\"keepRight\",\"d\":1800,\"road\":\"Richtung K\xc3\xb6ln\"}");
    s.step(1500);
    shoot(s, "store-map", "map");
  }
  {
    const Sample& smp = sampleNamed("sample-berliner-allee");
    const MapImage& m = smp.img;
    Sim s(true);
    sampleDrive(s, smp, 48, 50, lang);
    s.send("{\"t\":\"cfg\",\"tilt\":false}");
    const float pts[][4] = {{440, 680, 3, 6}, {478, 596, 0, 4}, {548, 522, 12, 16}, {612, 652, 1, 2}};
    std::string st;
    for (const auto& p : pts) {
      double lat, lon;
      localToGeo(m.lat, m.lon, (p[0] - m.w / 2.0) * m.mpp, (m.h / 2.0 - p[1]) * m.mpp, lat, lon);
      char one[80];
      snprintf(one, sizeof(one), "%s[%.6f,%.6f,%d,%d]", st.empty() ? "" : ",", lat, lon, (int)p[2], (int)p[3]);
      st += one;
    }
    s.send("{\"t\":\"chg\",\"st\":[" + st + "]}");
    s.step(1500);
    shoot(s, "store-chargers", "chargers");
  }
  {
    Sim s(true);
    s.step(2600);
    s.connect(kToken, lang);
    s.step(App::kWelcomeMs);
    s.send("{\"t\":\"bat\",\"soc\":64.0,\"kw\":142.0,\"km\":326}");
    s.step(3200);
    shoot(s, "store-charging", "charging");
  }
  {
    Sim s(true);
    s.at(2026, 11, 14, 10, 9, 34);
    s.step(2600);
    s.connect(kToken, lang);
    s.send("{\"t\":\"cfg\",\"theme\":\"stopwatch\",\"name\":\"\"}");
    s.step(4400);
    shoot(s, "store-stopwatch", "stopwatch");
  }
  {
    Sim s(true);
    s.step(2600);
    s.connect(kToken, lang);
    s.drive(38, 50);
    s.step(500);
    scene(s, std::string(en ? "\"turn\":{\"k\":\"roundabout\",\"d\":180,\"road\":\"Cologne\",\"exit\":2},"
                            : "\"turn\":{\"k\":\"roundabout\",\"d\":180,\"road\":\"Richtung K\xc3\xb6ln\",\"exit\":2},") +
                 light(s, "green", -4000, 9000, 520, -1, nullptr));
    s.step(300);
    shoot(s, "store-roundabout", "roundabout");
  }
  {
    Sim s(true);
    s.at(2026, 11, 14, 18, 44, 20);
    s.step(2600);
    s.connect(kToken, lang);
    s.send("{\"t\":\"cfg\",\"theme\":\"aurora\",\"name\":\"\"}");
    s.step(4400);
    shoot(s, "store-aurora", "aurora");
  }
  {
    const Sample& smp = sampleNamed("sample-berliner-allee");
    Route route = makeRoute(smp.img, smp.route);
    Sim s(true);
    sampleDrive(s, smp, 46, 50, lang);
    s.route = &route.line;
    scene(s, "\"turn\":{\"k\":\"right\",\"d\":430,\"road\":\"Bahnstra\xc3\x9f" "e\"}");
    s.step(1500);
    shoot(s, "store-city", "city map");
  }
}

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

  loadSamples();
  loadWallpaper();
  makeIcon();
  if (argc > 5 && strcmp(argv[5], "-notices") == 0) {
    makeIcon();
    printf("Filming the notifications to %s\n", g_outDir.c_str());
    noticeReel();
    return 0;
  }
  if (argc > 5 && strcmp(argv[5], "-stills") == 0) {
    storeStills(argc > 6 ? argv[6] : "de");
    return 0;
  }
  if (argc > 5 && strcmp(argv[5], "-promo") == 0) {
    printf("Filming the store promo to %s\n", g_outDir.c_str());
    promoReel(argc > 6 ? argv[6] : "de");
    return 0;
  }
  if (argc > 5 && strcmp(argv[5], "-reel") == 0) {
    printf("Filming the showreel to %s\n", g_outDir.c_str());
    reel();
    return 0;
  }
  printf("Rendering previews to %s\n", g_outDir.c_str());
  runAll();
  noticeShots();
  overview();
  ui::setBandBuffers(nullptr, nullptr, 0);
  iconSheet();
  if (g_bandMismatch) {
    printf("%d scenarios differ between banded and direct drawing\n", g_bandMismatch);
    return 1;
  }
  return 0;
}
