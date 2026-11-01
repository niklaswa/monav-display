#include "device/demo.h"

#if defined(MONAV_DEMO)

#include <Arduino.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "core/nav.h"
#include "core/protocol.h"
#include "device/bench.h"
#include "device/transfers.h"

#ifndef MONAV_DEV_TOKEN
#error "the demo signs in with MONAV_DEV_TOKEN"
#endif

extern "C" {
extern const uint8_t _binary_tools_preview_maps_sample_berliner_allee_jpg_start[] asm(
    "_binary_tools_preview_maps_sample_berliner_allee_jpg_start");
extern const uint8_t _binary_tools_preview_maps_sample_berliner_allee_jpg_end[] asm(
    "_binary_tools_preview_maps_sample_berliner_allee_jpg_end");
}

namespace monav::device::demo {
namespace {

// tools/preview/maps/sample-berliner-allee.json
constexpr double kMapLat = 51.2211, kMapLon = 6.7841;
constexpr float kMapMpp = 1.6009712f;
constexpr const char* kMapAttr = "\u00a9 OpenStreetMap";
// sample-berliner-allee.json's route: Corneliusstraße and Berliner Allee north, Bahnstraße, Oststraße.
constexpr float kPath[][2] = {{455.4, 970}, {456.2, 778.9}, {452.3, 761.4}, {426.9, 729.9}, {423.3, 722}, {426.8, 605.7}, {518.9, 606.2}, {522.5, 596}, {579.9, 474.5}};
constexpr int kPathN = sizeof(kPath) / sizeof(kPath[0]);
constexpr float kKmh = 45.0f;
constexpr uint32_t kLoopMs = 90000;

uint64_t s_start = 0, s_nextV = 0, s_nextFix = 0, s_nextScene = 0;
int s_phase = -1;
double s_fixLat = 0, s_fixLon = 0;
float s_fixHdg = 0;
std::vector<int16_t> s_route;
uint8_t s_seq = 0;

void send(App& app, const char* json, uint64_t now) {
  Message m;
  if (proto::parse(json, strlen(json), m)) {
    if (m.type == MsgType::Ota || m.type == MsgType::Map || m.type == MsgType::Route) {
      transfers::control(m);
    } else {
      app.message(m, now);
    }
  }
}

/** Sends `data` on the bulk path the way the phone does: chunks of MTU − 3. */
void bulk(uint8_t id, const uint8_t* data, size_t len) {
  uint8_t chunk[512];
  const size_t payload = 509 - 5;
  for (size_t off = 0; off < len; off += payload) {
    const size_t n = len - off < payload ? len - off : payload;
    chunk[0] = id;
    chunk[1] = off & 0xFF;
    chunk[2] = (off >> 8) & 0xFF;
    chunk[3] = (off >> 16) & 0xFF;
    chunk[4] = (off >> 24) & 0xFF;
    memcpy(chunk + 5, data + off, n);
    transfers::onBulk(chunk, n + 5);
  }
}

float pathLength() {
  float total = 0;
  for (int i = 0; i + 1 < kPathN; i++) total += std::hypot(kPath[i + 1][0] - kPath[i][0], kPath[i + 1][1] - kPath[i][1]);
  return total * kMapMpp;
}

/** Position (map px) and heading at `metres` along the path. */
void along(float metres, float& x, float& y, float& hdg) {
  float left = metres / kMapMpp;
  for (int i = 0; i + 1 < kPathN; i++) {
    const float dx = kPath[i + 1][0] - kPath[i][0], dy = kPath[i + 1][1] - kPath[i][1];
    const float l = std::hypot(dx, dy);
    hdg = std::atan2(dx, -dy) * 180.0f / 3.14159265f;
    if (hdg < 0) hdg += 360.0f;
    if (left <= l || i + 2 == kPathN) {
      const float t = left / l > 1 ? 1 : left / l;
      x = kPath[i][0] + dx * t;
      y = kPath[i][1] + dy * t;
      return;
    }
    left -= l;
  }
}

void pxToGeo(float x, float y, double& lat, double& lon) {
  localToGeo(kMapLat, kMapLon, (x - 512.0) * kMapMpp, (512.0 - y) * kMapMpp, lat, lon);
}

}  // namespace

void begin(App& app, uint64_t now) {
  s_start = now + 2500;  // after the boot screen
  app.connected(now);
  char buf[256];
  snprintf(buf, sizeof(buf), "{\"t\":\"hello\",\"proto\":1,\"token\":\"%s\",\"lang\":\"de\",\"now\":%llu}",
           MONAV_DEV_TOKEN, static_cast<unsigned long long>(1792728780000ULL + now));
  send(app, buf, now);

  // The map image, as Monav sends it.
  const size_t jpgLen = _binary_tools_preview_maps_sample_berliner_allee_jpg_end -
                        _binary_tools_preview_maps_sample_berliner_allee_jpg_start;
  snprintf(buf, sizeof(buf),
           "{\"t\":\"map\",\"op\":\"begin\",\"id\":1,\"size\":%u,\"w\":1024,\"h\":1024,\"lat\":%.6f,\"lon\":%.6f,"
           "\"mpp\":%.7f,\"attr\":\"%s\"}",
           static_cast<unsigned>(jpgLen), kMapLat, kMapLon, kMapMpp, kMapAttr);
  send(app, buf, now);
  bulk(1, _binary_tools_preview_maps_sample_berliner_allee_jpg_start, jpgLen);

  // The route, every ~10 m, around the image centre.
  for (int i = 0; i + 1 < kPathN; i++) {
    const float dx = kPath[i + 1][0] - kPath[i][0], dy = kPath[i + 1][1] - kPath[i][1];
    const int steps = static_cast<int>(std::hypot(dx, dy) * kMapMpp / 10.0f) + 1;
    for (int k = 0; k < steps; k++) {
      const float x = kPath[i][0] + dx * k / steps, y = kPath[i][1] + dy * k / steps;
      s_route.push_back(static_cast<int16_t>(std::lround((x - 512) * kMapMpp)));
      s_route.push_back(static_cast<int16_t>(std::lround((512 - y) * kMapMpp)));
    }
  }
  snprintf(buf, sizeof(buf), "{\"t\":\"route\",\"op\":\"begin\",\"id\":2,\"size\":%u,\"lat\":%.6f,\"lon\":%.6f}",
           static_cast<unsigned>(s_route.size() * 2), kMapLat, kMapLon);
  send(app, buf, now);
  bulk(2, reinterpret_cast<const uint8_t*>(s_route.data()), s_route.size() * 2);
  Serial.printf("demo: map %u bytes and route %u points sent through the bulk path\n", static_cast<unsigned>(jpgLen),
                static_cast<unsigned>(s_route.size() / 2));
}

void step(App& app, uint64_t now) {
  if (now < s_start) return;
  static bool benched = false;
  if (!benched && transfers::map() != nullptr) {
    benched = true;
    bench::run();
  }
  const uint32_t t = static_cast<uint32_t>((now - s_start) % kLoopMs);
  // 0–15 s HUD, 15–25 s focus, 25–55 s map in perspective, 55–75 s map flat, 75–90 s HUD.
  const int phase = t < 15000 ? 0 : t < 25000 ? 1 : t < 55000 ? 2 : t < 75000 ? 3 : 4;
  if (phase != s_phase) {
    s_phase = phase;
    static const char* const kLayout[] = {"hud", "focus", "map", "map", "hud"};
    char buf[96];
    snprintf(buf, sizeof(buf), "{\"t\":\"dev\",\"layout\":\"%s\"}", kLayout[phase]);
    send(app, buf, now);
    send(app, phase == 3 ? "{\"t\":\"cfg\",\"tilt\":false}" : "{\"t\":\"cfg\",\"tilt\":true}", now);
    Serial.printf("demo: %s%s\n", kLayout[phase], phase == 3 ? " (flat)" : phase == 2 ? " (tilted)" : "");
  }

  const float total = pathLength();
  const float metres = std::fmod(kKmh / 3.6f * (now - s_start) / 1000.0f, total * 0.95f);
  // Fixes once a second (like Monav), v ten times a second repeating the last fix.
  if (now >= s_nextFix) {
    float x, y;
    along(metres, x, y, s_fixHdg);
    pxToGeo(x, y, s_fixLat, s_fixLon);
    s_nextFix = now + 1000;
  }
  if (now >= s_nextV) {
    char buf[200];
    snprintf(buf, sizeof(buf),
             "{\"t\":\"v\",\"now\":%llu,\"kmh\":%.1f,\"lim\":50,\"lat\":%.6f,\"lon\":%.6f,\"hdg\":%.1f}",
             static_cast<unsigned long long>(1792728780000ULL + now), kKmh, s_fixLat, s_fixLon, s_fixHdg);
    send(app, buf, now);
    s_nextV = now + 100;
  }
  if (now >= s_nextScene) {
    // The corner into Bahnstraße: a traffic light there, the turn after it.
    const float corner = std::hypot(kPath[2][0] - kPath[0][0], kPath[2][1] - kPath[0][1]) * kMapMpp;
    const float toCorner = corner - metres;
    char buf[400];
    const unsigned long long phone = 1792728780000ULL + now;
    if (toCorner > 5) {
      snprintf(buf, sizeof(buf),
               "{\"t\":\"s\",\"at\":%llu,\"light\":{\"st\":\"red\",\"from\":%llu,\"to\":%llu,\"d\":%d,\"adv\":38},"
               "\"turn\":{\"k\":\"right\",\"d\":%d,\"road\":\"Bahnstra\xc3\x9f" "e\"}}",
               phone, phone - 12000, phone + 15000, static_cast<int>(toCorner), static_cast<int>(toCorner));
    } else {
      snprintf(buf, sizeof(buf),
               "{\"t\":\"s\",\"at\":%llu,\"cam\":{\"k\":\"fixed\",\"d\":%d,\"vmax\":30,\"label\":\"Fester Blitzer\"}}",
               phone, static_cast<int>(300 + toCorner));
    }
    send(app, buf, now);
    s_nextScene = now + 2000;
  }
}

}  // namespace monav::device::demo

#else  // !MONAV_DEMO

namespace monav::device::demo {
void begin(App&, uint64_t) {}
void step(App&, uint64_t) {}
}  // namespace monav::device::demo

#endif
