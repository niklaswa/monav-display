// Host checks of the protocol and the app logic (run by tools/preview/render.sh before the
// previews are drawn). Framing edge cases, parsing, pairing, time sync and the timeouts.

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <cmath>

#include "core/app.h"
#include "core/bulk.h"
#include "core/mapdata.h"
#include "core/nav.h"
#include "core/protocol.h"
#include "core/timesync.h"
#include "ui/maprender.h"
#include "ui/rotate.h"

using namespace monav;

namespace {

int g_failed = 0;
int g_passed = 0;

#define CHECK(cond)                                                    \
  do {                                                                 \
    if (cond) {                                                        \
      g_passed++;                                                      \
    } else {                                                           \
      g_failed++;                                                      \
      printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
    }                                                                  \
  } while (0)

std::vector<uint8_t> chunk(uint8_t hdr, const char* payload) {
  std::vector<uint8_t> v{hdr};
  v.insert(v.end(), payload, payload + strlen(payload));
  return v;
}

bool feed(proto::Reassembler& r, const std::vector<uint8_t>& c) { return r.feed(c.data(), c.size()); }

void framing() {
  using proto::kFirst;
  using proto::kLast;
  proto::Reassembler r;

  CHECK(feed(r, chunk(kFirst | kLast | 5, "{\"t\":\"bye\"}")));
  CHECK(std::string(r.message()) == "{\"t\":\"bye\"}");

  // Three chunks in order.
  CHECK(!feed(r, chunk(kFirst | 10, "{\"t\":")));
  CHECK(!feed(r, chunk(11, "\"v\",")));
  CHECK(feed(r, chunk(kLast | 12, "\"kmh\":5}")));
  CHECK(std::string(r.message()) == "{\"t\":\"v\",\"kmh\":5}");

  // A gap drops the message; the next FIRST starts clean.
  CHECK(!feed(r, chunk(kFirst | 20, "{\"t\":")));
  CHECK(!feed(r, chunk(kLast | 22, "\"bye\"}")));
  CHECK(feed(r, chunk(kFirst | kLast | 23, "{}")));
  CHECK(std::string(r.message()) == "{}");

  // A FIRST in the middle drops what was collected.
  CHECK(!feed(r, chunk(kFirst | 30, "{\"t\":\"lost")));
  CHECK(feed(r, chunk(kFirst | kLast | 31, "{\"t\":\"bye\"}")));
  CHECK(std::string(r.message()) == "{\"t\":\"bye\"}");

  // Continuation without a start is ignored.
  CHECK(!feed(r, chunk(kLast | 40, "x\"}")));

  // Sequence numbers wrap at 64.
  CHECK(!feed(r, chunk(kFirst | 62, "{\"a\"")));
  CHECK(!feed(r, chunk(63, ":")));
  CHECK(!feed(r, chunk(0, "1")));
  CHECK(feed(r, chunk(kLast | 1, "}")));
  CHECK(std::string(r.message()) == "{\"a\":1}");

  // More than 1024 bytes is dropped.
  std::string big(600, 'x');
  CHECK(!feed(r, chunk(kFirst | 0, big.c_str())));
  CHECK(!feed(r, chunk(kLast | 1, big.c_str())));

  // fragment() → Reassembler round trip at the smallest chunk size and an MTU-sized one.
  for (size_t size : {size_t{20}, size_t{182}}) {
    std::string msg = "{\"t\":\"s\",\"turn\":{\"k\":\"left\",\"road\":\"" + std::string(300, 'r') + "\"}}";
    uint8_t seq = 60;
    proto::Reassembler rr;
    int done = 0;
    size_t chunks = 0;
    bool sized = true;
    proto::fragment(msg.data(), msg.size(), size, seq, [&](const uint8_t* d, size_t n) {
      chunks++;
      sized = sized && n <= size;
      if (rr.feed(d, n)) done++;
    });
    CHECK(sized);
    CHECK(done == 1);
    CHECK(std::string(rr.message()) == msg);
    CHECK(chunks == (msg.size() + size - 2) / (size - 1));
  }
}

void parsing() {
  Message m;
  const char* hello = "{\"t\":\"hello\",\"proto\":1,\"token\":\"abc\",\"lang\":\"en\",\"now\":1761220000123,\"x\":1}";
  CHECK(proto::parse(hello, strlen(hello), m));
  CHECK(m.type == MsgType::Hello && m.proto == 1 && std::string(m.token) == "abc");
  CHECK(m.hasLang && m.lang == Lang::En && m.hasNow && m.now == 1761220000123LL);

  const char* v = "{\"t\":\"v\",\"now\":1761220000123,\"kmh\":87.4}";
  CHECK(proto::parse(v, strlen(v), m));
  CHECK(m.type == MsgType::Vehicle && m.lim == 0 && m.kmh > 87.3f && m.kmh < 87.5f);

  const char* s =
      "{\"t\":\"s\",\"at\":1761220000123,"
      "\"light\":{\"st\":\"red\",\"from\":1761219990000,\"to\":1761220012000,\"d\":220,\"adv\":48,\"turn\":\"left\"},"
      "\"cam\":{\"k\":\"fixed\",\"d\":340,\"vmax\":50,\"label\":\"Fester Blitzer\"},"
      "\"sec\":{\"vmax\":80,\"avg\":76.5},"
      "\"turn\":{\"k\":\"roundabout\",\"d\":300,\"road\":\"Herzogstra\xc3\x9f" "e\",\"exit\":2,\"eta\":1761220900000},"
      "\"future\":{\"x\":1}}";
  CHECK(proto::parse(s, strlen(s), m));
  CHECK(m.type == MsgType::Scene && m.scene.at == 1761220000123LL);
  CHECK(m.scene.light.present && m.scene.light.st == LightState::Red && m.scene.light.adv == 48);
  CHECK(m.scene.light.lane == Lane::Left && m.scene.light.to == 1761220012000LL);
  CHECK(m.scene.cam.present && m.scene.cam.kind == CamKind::Fixed && m.scene.cam.vmax == 50);
  CHECK(m.scene.sec.present && m.scene.sec.vmax == 80);
  CHECK(m.scene.turn.present && m.scene.turn.kind == TurnKind::Roundabout && m.scene.turn.exit == 2);
  CHECK(std::string(m.scene.turn.road) == "Herzogstra\xc3\x9f" "e");

  const char* s2 = "{\"t\":\"s\",\"at\":1,\"light\":{\"st\":\"green\",\"d\":90}}";
  CHECK(proto::parse(s2, strlen(s2), m));
  CHECK(m.scene.light.adv == -1 && m.scene.light.to == 0 && m.scene.light.lane == Lane::All);
  CHECK(!m.scene.cam.present && !m.scene.turn.present);

  const char* unknown = "{\"t\":\"wat\",\"a\":1}";
  CHECK(proto::parse(unknown, strlen(unknown), m) && m.type == MsgType::Unknown);
  CHECK(!proto::parse("{nope", 5, m));

  char out[128];
  proto::buildWelcome(out, sizeof(out), "0.1.0", "4F2A", true, 80, Layout::Hud);
  CHECK(std::string(out) ==
        "{\"t\":\"welcome\",\"fw\":\"0.1.0\",\"proto\":1,\"id\":\"4F2A\",\"fresh\":true,\"bright\":80,\"layout\":\"hud\"}");
  proto::buildDenied(out, sizeof(out), "4F2A");
  CHECK(std::string(out) == "{\"t\":\"denied\",\"id\":\"4F2A\"}");
  proto::buildStatus(out, sizeof(out), 60);
  CHECK(std::string(out) == "{\"t\":\"status\",\"bright\":60}");
  proto::buildInfo(out, sizeof(out), "0.1.0", "4F2A", true);
  CHECK(std::string(out) == "{\"fw\":\"0.1.0\",\"proto\":1,\"id\":\"4F2A\",\"paired\":true}");
}

void timeSync() {
  TimeSync t;
  // True offset 1000; latencies 40, 15, 90 ms: the offset follows the fastest message.
  t.sample(1000 + 100 - 40, 100);
  t.sample(1000 + 200 - 15, 200);
  CHECK(t.offset() == 985);
  t.sample(1000 + 300 - 90, 300);
  CHECK(t.offset() >= 984 && t.offset() <= 985);  // a late message barely moves it
  // Without better samples it sinks by at most ~1 ms per second.
  t.sample(1000 + 10300 - 90, 10300);
  CHECK(t.offset() >= 974 && t.offset() <= 976);
  // A clock jump on the phone is taken at once.
  t.sample(50000 + 10400, 10400);
  CHECK(t.offset() == 50000);
  t.sample(1000 + 10500, 10500);  // and back down by much more than latency could explain
  CHECK(t.offset() == 1000);

  // 2026-10-23 04:13 UTC is 06:13 in Germany (summer time), 2026-12-01 04:13 UTC is 05:13.
  ClockTime c = localClock(1792728780000LL, false, 0);
  CHECK(c.hour == 6 && c.minute == 13);
  c = localClock(1796098380000LL, false, 0);
  CHECK(c.hour == 5 && c.minute == 13);
  c = localClock(1792728780000LL, true, -240);
  CHECK(c.hour == 0 && c.minute == 13);
}

class TestPlatform : public Platform {
 public:
  std::string token;
  std::vector<std::string> sent;
  int disconnects = 0;
  int bright = -1;
  int rot = -1;
  int layout = -1;
  bool paired = false;
  bool authorized = false;
  void send(const char* json, size_t len) override { sent.emplace_back(json, len); }
  void disconnect() override { disconnects++; }
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
  int loadLang() override { return -1; }
  void saveLang(int) override {}
  int loadLayout() override { return layout; }
  void saveLayout(int l) override { layout = l; }
  void authorizationChanged(bool a) override { authorized = a; }
  void pairingChanged(bool p) override { paired = p; }
  void log(const char*) override {}
};

Message parsed(const std::string& json) {
  Message m;
  proto::parse(json.data(), json.size(), m);
  return m;
}

void app() {
  {
    // Fresh pairing; everything before welcome is ignored.
    TestPlatform p;
    App a(p, "4F2A");
    uint64_t now = 1000;
    a.begin(now);
    now += 2000;
    a.tick(now);
    CHECK(a.screen() == Screen::Waiting && !a.paired());
    a.connected(now);
    a.message(parsed("{\"t\":\"v\",\"now\":5,\"kmh\":50,\"lim\":50}"), now);
    a.tick(now);
    CHECK(a.screen() == Screen::Waiting);
    a.message(parsed("{\"t\":\"hello\",\"proto\":1,\"token\":\"T1\",\"lang\":\"de\",\"now\":5}"), now);
    CHECK(p.token == "T1" && p.paired);
    CHECK(!p.sent.empty() && p.sent.back().find("\"fresh\":true") != std::string::npos);
    a.tick(now);
    CHECK(a.screen() == Screen::Paired);
    a.message(parsed("{\"t\":\"v\",\"now\":5,\"kmh\":50,\"lim\":50}"), now);
    now += 3000;
    a.tick(now);
    CHECK(a.screen() == Screen::Drive);
    // No v for > 3 s: link lost, still the HUD; after 20 more seconds the drive is over.
    now += 3500;
    a.tick(now);
    Frame f;
    a.frame(f, now);
    CHECK(f.screen == Screen::Drive && f.linkLost);
    now += 20500;
    a.tick(now);
    CHECK(a.screen() == Screen::Connected);
    // bye: standby.
    a.message(parsed("{\"t\":\"bye\"}"), now);
    a.tick(now);
    CHECK(a.screen() == Screen::Standby);
    // The phone goes away: standby stays (the clock runs on); it comes back: connected.
    a.disconnected(now);
    now += 1000;
    a.tick(now);
    CHECK(a.screen() == Screen::Standby);
    a.connected(now);
    a.message(parsed("{\"t\":\"hello\",\"proto\":1,\"token\":\"T1\",\"now\":5}"), now);
    a.tick(now);
    CHECK(a.screen() == Screen::Connected);
  }
  {
    // Same phone again: welcome without fresh. Another phone: denied, and ignored.
    TestPlatform p;
    p.token = "T1";
    App a(p, "4F2A");
    uint64_t now = 1000;
    a.begin(now);
    now += 2000;
    a.connected(now);
    a.message(parsed("{\"t\":\"hello\",\"proto\":1,\"token\":\"T1\",\"lang\":\"de\",\"now\":5}"), now);
    CHECK(p.sent.back().find("\"fresh\":false") != std::string::npos);
    a.disconnected(now);
    a.connected(now);
    a.message(parsed("{\"t\":\"hello\",\"proto\":1,\"token\":\"T2\",\"lang\":\"de\",\"now\":5}"), now);
    CHECK(p.sent.back() == "{\"t\":\"denied\",\"id\":\"4F2A\"}");
    a.message(parsed("{\"t\":\"v\",\"now\":5,\"kmh\":50,\"lim\":50}"), now);
    a.tick(now);
    CHECK(a.screen() == Screen::Denied && p.token == "T1");
    // 3 s hold asks, a tap confirms: forgotten, and the phone is dropped so it says hello again.
    a.gesture(Gesture{GestureType::LongPress}, now);
    a.tick(now);
    Frame f;
    a.frame(f, now);
    CHECK(f.resetAsk);
    a.gesture(Gesture{GestureType::Tap}, now);
    CHECK(p.token.empty() && p.disconnects == 1 && !p.paired);
    a.disconnected(now);
    a.connected(now);
    a.message(parsed("{\"t\":\"hello\",\"proto\":1,\"token\":\"T2\",\"lang\":\"en\",\"now\":5}"), now);
    CHECK(p.token == "T2" && p.sent.back().find("\"fresh\":true") != std::string::npos);
  }
  {
    // The question times out; BOOT resets without asking. Brightness: swipe → status, cfg → stored.
    TestPlatform p;
    p.token = "T1";
    App a(p, "4F2A");
    uint64_t now = 1000;
    a.begin(now);
    a.connected(now);
    a.message(parsed("{\"t\":\"hello\",\"proto\":1,\"token\":\"T1\",\"now\":5}"), now);
    a.gesture(Gesture{GestureType::LongPress}, now);
    now += 5200;
    a.tick(now);
    Frame f;
    a.frame(f, now);
    CHECK(!f.resetAsk && p.token == "T1");
    a.gesture(Gesture{GestureType::BrightnessStep, -2}, now);
    a.gesture(Gesture{GestureType::SwipeEnd}, now);
    CHECK(a.brightness() == 60 && p.bright == 60);
    CHECK(p.sent.back() == "{\"t\":\"status\",\"bright\":60}");
    a.message(parsed("{\"t\":\"cfg\",\"bright\":3}"), now);
    CHECK(a.brightness() == 5 && p.bright == 5);
    // Rotation: applied at once, stored once the slider stops; wraps into 0..359.
    a.message(parsed("{\"t\":\"cfg\",\"rot\":-30}"), now);
    CHECK(a.rotation() == 330 && p.rot == -1);
    a.message(parsed("{\"t\":\"cfg\",\"rot\":725}"), now);
    a.tick(now + 400);
    CHECK(a.rotation() == 5 && p.rot == -1);
    a.tick(now + App::kRotationSaveMs + 10);
    CHECK(p.rot == 5);
    a.message(parsed("{\"t\":\"cfg\",\"bright\":40}"), now + App::kRotationSaveMs + 20);
    CHECK(a.rotation() == 5);
    a.bootButtonHeld(now);
    CHECK(p.token.empty());
  }
  {
    // Distances run down with the speed; the light counts down on the phone's clock.
    TestPlatform p;
    p.token = "T1";
    App a(p, "4F2A");
    uint64_t now = 1000;
    const int64_t phone0 = 1792728780000LL;
    auto phone = [&] { return phone0 + static_cast<int64_t>(now); };
    a.begin(now);
    a.connected(now);
    char buf[300];
    snprintf(buf, sizeof(buf), "{\"t\":\"hello\",\"proto\":1,\"token\":\"T1\",\"now\":%lld}",
             static_cast<long long>(phone()));
    a.message(parsed(buf), now);
    snprintf(buf, sizeof(buf), "{\"t\":\"v\",\"now\":%lld,\"kmh\":36,\"lim\":50}", static_cast<long long>(phone()));
    a.message(parsed(buf), now);
    snprintf(buf, sizeof(buf),
             "{\"t\":\"s\",\"at\":%lld,\"light\":{\"st\":\"red\",\"from\":%lld,\"to\":%lld,\"d\":200}}",
             static_cast<long long>(phone()), static_cast<long long>(phone() - 10000),
             static_cast<long long>(phone() + 10000));
    a.message(parsed(buf), now);
    for (int i = 0; i < 60; i++) {  // 2 s at 36 km/h = 10 m/s
      now += 33;
      if (i % 3 == 0) {
        snprintf(buf, sizeof(buf), "{\"t\":\"v\",\"now\":%lld,\"kmh\":36,\"lim\":50}", static_cast<long long>(phone()));
        a.message(parsed(buf), now);
      }
      a.tick(now);
    }
    Frame f;
    a.frame(f, now);
    CHECK(f.lightD > 178 && f.lightD < 182);
    CHECK(f.lightLeft > 7.9f && f.lightLeft < 8.1f);
    CHECK(f.lightPhase > 0.39f && f.lightPhase < 0.41f);
    CHECK(f.primary == Card::Light);
  }
}

void bulkTransfers() {
  // Chunk header: id, then the offset little endian.
  const uint8_t w[] = {7, 0x00, 0x40, 0x01, 0x00, 'a', 'b'};
  bulk::Chunk ch;
  CHECK(bulk::parseChunk(w, sizeof(w), ch) && ch.id == 7 && ch.offset == 0x14000 && ch.len == 2);
  CHECK(!bulk::parseChunk(w, 4, ch));

  bulk::Slot slot;
  CHECK(slot.offer(0, 10) == bulk::Slot::Verdict::Drop);  // nothing announced
  slot.begin(7, 40000);
  uint32_t ack = 0;
  CHECK(slot.offer(0, 10000) == bulk::Slot::Verdict::Take && slot.next() == 10000);
  CHECK(!slot.stored(10000, ack));
  CHECK(slot.offer(10000, 10000) == bulk::Slot::Verdict::Take);
  CHECK(slot.stored(20000, ack) && ack == 16384);  // a 16 KiB boundary crossed
  CHECK(!slot.stored(20000, ack));                 // ... only once
  // A gap: one request for the rest, then silence until the bytes flow again.
  CHECK(slot.offer(25000, 500) == bulk::Slot::Verdict::AskFrom && slot.next() == 20000);
  CHECK(slot.offer(25500, 500) == bulk::Slot::Verdict::Drop);
  CHECK(slot.offer(10000, 500) == bulk::Slot::Verdict::Drop);  // a repeat of what is in
  CHECK(slot.offer(20000, 15000) == bulk::Slot::Verdict::Take);
  CHECK(slot.offer(36000, 10) == bulk::Slot::Verdict::AskFrom);  // a new gap asks again
  CHECK(slot.offer(35000, 6000) == bulk::Slot::Verdict::Drop);   // beyond the size
  CHECK(slot.offer(35000, 5000) == bulk::Slot::Verdict::Take && slot.received());
  CHECK(slot.stored(35000, ack) && ack == 32768);
  CHECK(slot.stored(40000, ack) && ack == 40000);  // and once more when the last byte is in
  CHECK(!slot.stored(40000, ack));
}

void parsing02() {
  Message m;
  const char* v = "{\"t\":\"v\",\"now\":1761220000123,\"kmh\":48.2,\"lim\":50,\"lat\":51.214231,\"lon\":6.782815,\"hdg\":87.5}";
  CHECK(proto::parse(v, strlen(v), m));
  // ArduinoJson parses numbers with few digits in float: 6.782815 comes back 6.7828145 (3 cm).
  // Below 16.78° that is at most ~1e-6° (~10 cm), far under a map pixel.
  CHECK(m.hasPos && std::fabs(m.lat - 51.214231) < 1e-9 && std::fabs(m.lon - 6.782815) < 2e-6);
  CHECK(m.hasHdg && std::fabs(m.hdg - 87.5f) < 1e-4f);
  const char* bad = "{\"t\":\"v\",\"kmh\":5,\"lat\":951.2,\"lon\":6.7}";
  CHECK(proto::parse(bad, strlen(bad), m) && !m.hasPos);
  const char* cfg = "{\"t\":\"cfg\",\"tilt\":false}";
  CHECK(proto::parse(cfg, strlen(cfg), m) && m.hasTilt && !m.tilt && m.bright == -1);
  CHECK(m.rot == -1);
  const char* chg = "{\"t\":\"chg\",\"st\":[[51.2142,6.7828,3],[51.2150,6.7801,0,4],[95,6,1],[51.2,6.7,-2,1],[51.2,6.7]]}";
  CHECK(proto::parse(chg, strlen(chg), m) && m.type == MsgType::Chargers && m.chargers.count == 3);
  CHECK(m.chargers.st[0].free == 3 && m.chargers.st[0].total == -1);
  CHECK(m.chargers.st[1].free == 0 && m.chargers.st[1].total == 4);
  CHECK(m.chargers.st[2].free == 0 && m.chargers.st[2].total == 1);
  const char* noChg = "{\"t\":\"chg\",\"st\":[]}";
  CHECK(proto::parse(noChg, strlen(noChg), m) && m.type == MsgType::Chargers && m.chargers.count == 0);
  {
    // The push's rotation: turned 90° clockwise, the frame's top shows on the right; a touch
    // there maps back to the top. The corners outside the glass stay black.
    constexpr int n = 466;
    std::vector<uint16_t> src(n * n), dst(n * n, 0xFFFF);
    for (int i = 0; i < n * n; i++) src[i] = static_cast<uint16_t>((static_cast<uint32_t>(i) * 2654435761u) >> 16);
    const gfx::Rotation r90(90);
    gfx::rotateRows(src.data(), n, n, r90, 0, n, dst.data());
    const int pts[][2] = {{233, 233}, {400, 233}, {233, 50}, {100, 300}, {300, 440}};
    for (const auto& q : pts) CHECK(dst[q[1] * n + q[0]] == src[(n - 1 - q[0]) * n + q[1]]);
    CHECK(dst[0] == 0 && dst[n - 1] == 0 && dst[n * n - 1] == 0);
    int x = 400, y = 233;
    gfx::unrotatePoint(r90, n, n, x, y);
    CHECK(x == 233 && y == 65);
    std::vector<uint16_t> back(n * n);
    gfx::rotateRows(src.data(), n, n, gfx::Rotation(0), 0, n, back.data());
    CHECK(back[233 * n + 233] == src[233 * n + 233] && back[50 * n + 233] == src[50 * n + 233]);
    CHECK(gfx::Rotation(-90).degrees == 270 && gfx::Rotation(720).degrees == 0);
  }
  const char* ota = "{\"t\":\"ota\",\"op\":\"begin\",\"id\":7,\"size\":1360624,\"sha256\":\"9f0c\",\"version\":\"0.2.1\"}";
  CHECK(proto::parse(ota, strlen(ota), m) && m.type == MsgType::Ota && m.op == TransferOp::Begin);
  CHECK(m.id == 7 && m.size == 1360624 && std::string(m.sha256) == "9f0c" && std::string(m.version) == "0.2.1");
  const char* map = "{\"t\":\"map\",\"op\":\"begin\",\"id\":8,\"size\":81234,\"w\":1024,\"h\":1024,\"lat\":51.2142,\"lon\":6.7828,\"mpp\":1.2}";
  CHECK(proto::parse(map, strlen(map), m) && m.type == MsgType::Map && m.w == 1024 && m.h == 1024);
  CHECK(m.hasPos && std::fabs(m.mpp - 1.2f) < 1e-5f && m.size == 81234);
  const char* clr = "{\"t\":\"route\",\"op\":\"clear\"}";
  CHECK(proto::parse(clr, strlen(clr), m) && m.type == MsgType::Route && m.op == TransferOp::Clear);

  char out[128];
  const Layout lm = Layout::Map;
  proto::buildStatus(out, sizeof(out), -1, &lm);
  CHECK(std::string(out) == "{\"t\":\"status\",\"layout\":\"map\"}");
  proto::buildBulkGot(out, sizeof(out), 7, 49152);
  CHECK(std::string(out) == "{\"t\":\"bulk\",\"id\":7,\"got\":49152}");
  proto::buildBulkFrom(out, sizeof(out), 7, 32768);
  CHECK(std::string(out) == "{\"t\":\"bulk\",\"id\":7,\"from\":32768}");
  proto::buildTransferState(out, sizeof(out), "ota", "error", 7, "sha256 mismatch");
  CHECK(std::string(out) == "{\"t\":\"ota\",\"st\":\"error\",\"id\":7,\"err\":\"sha256 mismatch\"}");
  proto::buildTransferState(out, sizeof(out), "map", "ready", 8);
  CHECK(std::string(out) == "{\"t\":\"map\",\"st\":\"ready\",\"id\":8}");
}

void nav() {
  Nav n;
  const double lat0 = 51.2211, lon0 = 6.7841;
  uint64_t now = 1000;
  n.fix(lat0, lon0, true, 90.0f, now);
  CHECK(n.valid() && n.lat() == lat0 && n.lon() == lon0 && n.hdg() == 90.0f);
  // Repeats of the fix: it runs on with the speed (36 km/h east = 10 m/s).
  for (int i = 0; i < 10; i++) {
    now += 100;
    n.fix(lat0, lon0, true, 90.0f, now);
    n.tick(now, 36.0f, true);
  }
  double e, nn;
  geoToLocal(lat0, lon0, n.lat(), n.lon(), e, nn);
  CHECK(e > 9.5 && e < 10.5 && std::fabs(nn) < 0.01);
  // The next fix says 12 m east: no jump - the drawn place eases over.
  double lat1, lon1;
  localToGeo(lat0, lon0, 12.0, 0.0, lat1, lon1);
  const double drawnLat = n.lat(), drawnLon = n.lon();
  n.fix(lat1, lon1, true, 90.0f, now);
  CHECK(std::fabs(n.lat() - drawnLat) < 1e-9 && std::fabs(n.lon() - drawnLon) < 1e-9);
  for (int i = 0; i < 20; i++) {
    now += 100;
    n.tick(now, 36.0f, true);
  }
  geoToLocal(lat0, lon0, n.lat(), n.lon(), e, nn);
  CHECK(e > 12.0 + 20.0 - 0.2 && e < 12.0 + 20.0 + 0.1);  // 12 m + 2 s × 10 m/s, the −2 m eased away
  // Without new fixes it stops running on 2.5 s after the last one.
  for (int i = 0; i < 10; i++) {
    now += 100;
    n.tick(now, 36.0f, true);
  }
  const double before = n.lon();
  for (int i = 0; i < 20; i++) {
    now += 100;
    n.tick(now, 36.0f, true);
  }
  CHECK(std::fabs(n.lon() - before) < 1e-6);
  // A jump of a kilometre is taken at once; the heading still turns smoothly (90° → 0°).
  double lat2, lon2;
  localToGeo(lat0, lon0, 0.0, 1000.0, lat2, lon2);
  n.fix(lat2, lon2, true, 0.0f, now);
  CHECK(std::fabs(n.lat() - lat2) < 1e-9 && std::fabs(n.hdg() - 90.0f) < 1e-3f);
  for (int i = 0; i < 30; i++) {
    now += 100;
    n.tick(now, 0.0f, true);
  }
  CHECK(n.hdg() < 1.0f || n.hdg() > 359.0f);
  // A turned heading on a repeated fix eases in too.
  n.fix(lat2, lon2, true, 90.0f, now);
  CHECK(n.hdg() < 1.0f || n.hdg() > 359.0f);
  for (int i = 0; i < 10; i++) {
    now += 100;
    n.tick(now, 0.0f, true);
  }
  CHECK(n.hdg() > 80.0f && n.hdg() < 90.0f);
}

void mapGeometry() {
  MapImage m;
  static uint16_t px[16];
  m.px = px;
  m.w = 1024;
  m.h = 1024;
  m.lat = 51.2211;
  m.lon = 6.7841;
  m.mpp = 1.6f;
  float mx, my;
  CHECK(ui::carOnImage(m, m.lat, m.lon, mx, my) && mx == 512.0f && my == 512.0f);
  double lat, lon;
  localToGeo(m.lat, m.lon, 160.0, -320.0, lat, lon);  // 100 px east, 200 px south
  CHECK(ui::carOnImage(m, lat, lon, mx, my) && std::fabs(mx - 612.0f) < 0.05f && std::fabs(my - 712.0f) < 0.05f);
  localToGeo(m.lat, m.lon, 900.0, 0.0, lat, lon);  // off the image
  CHECK(!ui::carOnImage(m, lat, lon, mx, my));
  CHECK(mapIndex(0, 0, 1024) == 0 && mapIndex(15, 15, 1024) == 255 && mapIndex(16, 0, 1024) == 256);
  CHECK(mapIndex(0, 16, 1024) == 65 * 256);  // a row of tiles holds 64 + 1 spare tiles
  // Ahead of the car (heading east: +x on the image) is up on the glass, flat and tilted.
  for (bool tilt : {false, true}) {
    ui::MapCamera cam;
    cam.carX = 500;
    cam.carY = 500;
    cam.hdg = 90;
    cam.tilt = tilt;
    float sx, sy, sc;
    CHECK(ui::project(cam, 600, 500, sx, sy, sc) && sy < cam.cy - 10 && std::fabs(sx - cam.cx) < 0.01f);
    CHECK(ui::project(cam, 500, 600, sx, sy, sc) && sx > cam.cx + 10);  // south = right when driving east
    CHECK(ui::project(cam, 500, 500, sx, sy, sc) && std::fabs(sx - cam.cx) < 0.01f && std::fabs(sy - cam.cy) < 0.01f);
  }
}

void app02() {
  {
    // welcome names the layout; taps cycle hud → focus → map → hud and each change is reported.
    TestPlatform p;
    p.token = "T1";
    App a(p, "4F2A");
    uint64_t now = 1000;
    a.begin(now);
    a.connected(now);
    CHECK(!p.authorized);
    a.message(parsed("{\"t\":\"hello\",\"proto\":1,\"token\":\"T1\",\"now\":5}"), now);
    CHECK(p.authorized && p.sent.back().find("\"layout\":\"hud\"") != std::string::npos);
    a.message(parsed("{\"t\":\"v\",\"now\":5,\"kmh\":30,\"lim\":50,\"lat\":51.2,\"lon\":6.7,\"hdg\":10}"), now);
    now += 2500;
    a.message(parsed("{\"t\":\"v\",\"now\":5,\"kmh\":30,\"lim\":50,\"lat\":51.2,\"lon\":6.7,\"hdg\":10}"), now);
    a.tick(now);
    CHECK(a.screen() == Screen::Drive);
    a.gesture(Gesture{GestureType::Tap}, now);
    CHECK(a.layout() == Layout::Focus && p.sent.back() == "{\"t\":\"status\",\"layout\":\"focus\"}");
    a.gesture(Gesture{GestureType::Tap}, now);
    CHECK(a.layout() == Layout::Map && p.layout == static_cast<int>(Layout::Map));
    a.tick(now);
    Frame f;
    a.frame(f, now);
    CHECK(f.layout == Layout::Map && f.hasPose && std::fabs(f.lat - 51.2) < 1e-6);
    a.message(parsed("{\"t\":\"cfg\",\"tilt\":false}"), now);
    a.frame(f, now);
    CHECK(!f.tilt);
    a.gesture(Gesture{GestureType::Tap}, now);
    CHECK(a.layout() == Layout::Hud);
    a.disconnected(now);
    CHECK(!p.authorized);
  }
#ifdef MONAV_DEV_TOKEN
  {
    // Development builds: the test client's token gets a welcome without touching the pairing,
    // and may set the layout.
    TestPlatform p;
    p.token = "T1";
    App a(p, "4F2A");
    uint64_t now = 1000;
    a.begin(now);
    a.connected(now);
    a.message(parsed(std::string("{\"t\":\"hello\",\"proto\":1,\"token\":\"") + MONAV_DEV_TOKEN + "\",\"now\":5}"), now);
    CHECK(p.authorized && p.token == "T1" && p.sent.back().find("\"welcome\"") != std::string::npos);
    a.message(parsed("{\"t\":\"dev\",\"layout\":\"map\"}"), now);
    CHECK(a.layout() == Layout::Map);
    // An ordinary phone may not.
    a.disconnected(now);
    a.connected(now);
    a.message(parsed("{\"t\":\"hello\",\"proto\":1,\"token\":\"T1\",\"now\":5}"), now);
    a.message(parsed("{\"t\":\"dev\",\"layout\":\"hud\"}"), now);
    CHECK(a.layout() == Layout::Map);
  }
#endif
}

}  // namespace

int runChecks() {
  framing();
  parsing();
  parsing02();
  bulkTransfers();
  timeSync();
  nav();
  mapGeometry();
  app();
  app02();
  printf("Checks: %d passed, %d failed\n", g_passed, g_failed);
  return g_failed;
}
