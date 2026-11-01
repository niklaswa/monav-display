#include "core/protocol.h"

#include <ArduinoJson.h>

#include <cmath>
#include <cstdio>
#include <cstring>

namespace monav::proto {

// --- Framing ---------------------------------------------------------------------------------

void Reassembler::reset() {
  len_ = 0;
  active_ = false;
  nextSeq_ = 0;
  buf_[0] = 0;
}

bool Reassembler::feed(const uint8_t* data, size_t len) {
  if (data == nullptr || len == 0) return false;
  const uint8_t hdr = data[0];
  const uint8_t seq = hdr & kSeqMask;
  const uint8_t* payload = data + 1;
  const size_t n = len - 1;

  if (hdr & kFirst) {
    if (active_) dropped_++;  // a new message started before the last one ended
    active_ = true;
    len_ = 0;
  } else if (!active_) {
    return false;  // continuation without a start: nothing to attach it to
  } else if (seq != nextSeq_) {
    active_ = false;  // gap: what was collected is incomplete
    len_ = 0;
    dropped_++;
    return false;
  }

  if (len_ + n > kMaxMessage) {
    active_ = false;
    len_ = 0;
    dropped_++;
    return false;
  }
  memcpy(buf_ + len_, payload, n);
  len_ += n;
  nextSeq_ = (seq + 1) & kSeqMask;

  if (hdr & kLast) {
    active_ = false;
    buf_[len_] = 0;
    return true;
  }
  return false;
}

// --- Parsing ---------------------------------------------------------------------------------

namespace {

void copyStr(char* dst, size_t cap, const char* src) {
  if (cap == 0) return;
  if (src == nullptr) {
    dst[0] = 0;
    return;
  }
  size_t i = 0;
  for (; i + 1 < cap && src[i]; i++) dst[i] = src[i];
  // Cut short: do not leave half a UTF-8 sequence at the end.
  if (src[i] != 0) {
    while (i > 0 && (static_cast<uint8_t>(src[i]) & 0xC0) == 0x80) i--;
  }
  dst[i] = 0;
}

bool eq(const char* a, const char* b) { return a != nullptr && strcmp(a, b) == 0; }

int64_t asTime(JsonVariantConst v) {
  if (v.is<int64_t>()) return v.as<int64_t>();
  if (v.is<double>()) return static_cast<int64_t>(std::llround(v.as<double>()));
  return 0;
}

float asFloat(JsonVariantConst v, float fallback = 0) {
  if (v.is<float>() || v.is<int>() || v.is<double>()) return v.as<float>();
  return fallback;
}

int asInt(JsonVariantConst v, int fallback) {
  if (v.is<int>()) return v.as<int>();
  if (v.is<float>() || v.is<double>()) return static_cast<int>(std::lround(v.as<double>()));
  return fallback;
}

LightState lightState(const char* s) {
  if (eq(s, "red")) return LightState::Red;
  if (eq(s, "green")) return LightState::Green;
  if (eq(s, "uncertain")) return LightState::Uncertain;
  return LightState::Unknown;
}

Lane lane(const char* s) {
  if (eq(s, "left")) return Lane::Left;
  if (eq(s, "right")) return Lane::Right;
  if (eq(s, "straight")) return Lane::Straight;
  return Lane::All;
}

CamKind camKind(const char* s) {
  if (eq(s, "fixed")) return CamKind::Fixed;
  if (eq(s, "mobile")) return CamKind::Mobile;
  if (eq(s, "redlight")) return CamKind::RedLight;
  if (eq(s, "section")) return CamKind::Section;
  if (eq(s, "distance")) return CamKind::Distance;
  return CamKind::Other;
}

TurnKind turnKind(const char* s) {
  static const struct {
    const char* name;
    TurnKind kind;
  } kTable[] = {
      {"straight", TurnKind::Straight},       {"slightLeft", TurnKind::SlightLeft},
      {"left", TurnKind::Left},               {"sharpLeft", TurnKind::SharpLeft},
      {"slightRight", TurnKind::SlightRight}, {"right", TurnKind::Right},
      {"sharpRight", TurnKind::SharpRight},   {"uTurn", TurnKind::UTurn},
      {"keepLeft", TurnKind::KeepLeft},       {"keepRight", TurnKind::KeepRight},
      {"roundabout", TurnKind::Roundabout},   {"arrive", TurnKind::Arrive},
  };
  for (const auto& e : kTable) {
    if (eq(s, e.name)) return e.kind;
  }
  return TurnKind::Other;
}

void parseScene(JsonObjectConst doc, Scene& sc) {
  sc.at = asTime(doc["at"]);

  JsonObjectConst light = doc["light"];
  if (!light.isNull()) {
    sc.light.present = true;
    sc.light.st = lightState(light["st"]);
    sc.light.from = asTime(light["from"]);
    sc.light.to = asTime(light["to"]);
    sc.light.d = asFloat(light["d"]);
    sc.light.adv = asInt(light["adv"], -1);
    sc.light.lane = lane(light["turn"]);
  }

  JsonObjectConst cam = doc["cam"];
  if (!cam.isNull()) {
    sc.cam.present = true;
    sc.cam.kind = camKind(cam["k"]);
    sc.cam.d = asFloat(cam["d"]);
    sc.cam.vmax = asInt(cam["vmax"], 0);
    copyStr(sc.cam.label, sizeof(sc.cam.label), cam["label"]);
  }

  JsonObjectConst sec = doc["sec"];
  if (!sec.isNull()) {
    sc.sec.present = true;
    sc.sec.vmax = asInt(sec["vmax"], 0);
    sc.sec.avg = asFloat(sec["avg"]);
  }

  JsonObjectConst turn = doc["turn"];
  if (!turn.isNull()) {
    sc.turn.present = true;
    sc.turn.kind = turnKind(turn["k"]);
    sc.turn.d = asFloat(turn["d"]);
    copyStr(sc.turn.road, sizeof(sc.turn.road), turn["road"]);
    sc.turn.exit = asInt(turn["exit"], 0);
    sc.turn.eta = asTime(turn["eta"]);
  }
}

void parsePlace(JsonObjectConst obj, Message& out) {
  JsonVariantConst lat = obj["lat"], lon = obj["lon"];
  if ((lat.is<double>() || lat.is<int>()) && (lon.is<double>() || lon.is<int>())) {
    out.lat = lat.as<double>();
    out.lon = lon.as<double>();
    out.hasPos = std::isfinite(out.lat) && std::isfinite(out.lon) && std::fabs(out.lat) <= 90.0 &&
                 std::fabs(out.lon) <= 180.0;
  }
}

}  // namespace

bool parse(const char* json, size_t len, Message& out) {
  out = Message{};
  JsonDocument doc;
  if (deserializeJson(doc, json, len) != DeserializationError::Ok) return false;
  JsonObjectConst obj = doc.as<JsonObjectConst>();
  if (obj.isNull()) return false;

  const char* t = obj["t"];
  if (!obj["now"].isNull()) {
    out.hasNow = true;
    out.now = asTime(obj["now"]);
  }
  if (obj["tz"].is<int>()) {
    out.hasTz = true;
    out.tzMin = obj["tz"].as<int>();
  }

  if (eq(t, "hello")) {
    out.type = MsgType::Hello;
    out.proto = asInt(obj["proto"], 0);
    copyStr(out.token, sizeof(out.token), obj["token"]);
    const char* lang = obj["lang"];
    if (lang != nullptr) {
      out.hasLang = true;
      out.lang = (strncmp(lang, "en", 2) == 0) ? Lang::En : Lang::De;
    }
  } else if (eq(t, "v")) {
    out.type = MsgType::Vehicle;
    out.kmh = asFloat(obj["kmh"]);
    if (!std::isfinite(out.kmh) || out.kmh < 0) out.kmh = 0;
    out.lim = asInt(obj["lim"], 0);
    if (out.lim < 0) out.lim = 0;
    parsePlace(obj, out);
    if (!obj["hdg"].isNull()) {
      out.hdg = asFloat(obj["hdg"]);
      out.hasHdg = std::isfinite(out.hdg);
    }
  } else if (eq(t, "s")) {
    out.type = MsgType::Scene;
    parseScene(obj, out.scene);
  } else if (eq(t, "cfg")) {
    out.type = MsgType::Config;
    out.bright = asInt(obj["bright"], -1);
    if (obj["tilt"].is<bool>()) {
      out.hasTilt = true;
      out.tilt = obj["tilt"].as<bool>();
    }
    if (!obj["rot"].isNull()) {
      out.rot = asInt(obj["rot"], 0) % 360;
      if (out.rot < 0) out.rot += 360;
    }
  } else if (eq(t, "chg")) {
    out.type = MsgType::Chargers;
    for (JsonArrayConst st : obj["st"].as<JsonArrayConst>()) {
      if (out.chargers.count >= Chargers::kMax) break;
      if (st.size() < 3) continue;
      Charger c;
      c.lat = st[0].as<double>();
      c.lon = st[1].as<double>();
      if (!std::isfinite(c.lat) || !std::isfinite(c.lon) || std::fabs(c.lat) > 90.0 || std::fabs(c.lon) > 180.0) continue;
      const int free = asInt(st[2], 0);
      c.free = static_cast<int16_t>(free < 0 ? 0 : free > 999 ? 999 : free);
      if (st.size() >= 4) {
        const int total = asInt(st[3], -1);
        c.total = static_cast<int16_t>(total < c.free ? -1 : total > 999 ? 999 : total);
      }
      out.chargers.st[out.chargers.count++] = c;
    }
  } else if (eq(t, "bye")) {
    out.type = MsgType::Bye;
  } else if (eq(t, "ota") || eq(t, "map") || eq(t, "route")) {
    out.type = eq(t, "ota") ? MsgType::Ota : eq(t, "map") ? MsgType::Map : MsgType::Route;
    const char* op = obj["op"];
    out.op = eq(op, "begin")   ? TransferOp::Begin
             : eq(op, "end")   ? TransferOp::End
             : eq(op, "abort") ? TransferOp::Abort
             : eq(op, "clear") ? TransferOp::Clear
                               : TransferOp::None;
    out.id = asInt(obj["id"], 0);
    const int64_t size = asTime(obj["size"]);
    out.size = size > 0 && size < 0x7FFFFFFF ? static_cast<uint32_t>(size) : 0;
    copyStr(out.sha256, sizeof(out.sha256), obj["sha256"]);
    copyStr(out.version, sizeof(out.version), obj["version"]);
    out.w = asInt(obj["w"], 0);
    out.h = asInt(obj["h"], 0);
    out.mpp = asFloat(obj["mpp"]);
    copyStr(out.attr, sizeof(out.attr), obj["attr"]);
    parsePlace(obj, out);
  } else if (eq(t, "dev")) {
    out.type = MsgType::Dev;
    const char* layout = obj["layout"];
    if (layout != nullptr) {
      out.hasLayout = true;
      out.layout = eq(layout, "map") ? Layout::Map : eq(layout, "focus") ? Layout::Focus : Layout::Hud;
    }
  } else {
    out.type = MsgType::Unknown;
  }
  return true;
}

// --- Building --------------------------------------------------------------------------------

namespace {
size_t clampLen(int n, size_t cap) { return n < 0 ? 0 : static_cast<size_t>(n) < cap ? static_cast<size_t>(n) : cap - 1; }
}  // namespace

const char* layoutName(Layout layout) {
  switch (layout) {
    case Layout::Focus:
      return "focus";
    case Layout::Map:
      return "map";
    default:
      return "hud";
  }
}

size_t buildWelcome(char* out, size_t cap, const char* fw, const char* id, bool fresh, int bright, Layout layout) {
  const int n = snprintf(out, cap,
                         "{\"t\":\"welcome\",\"fw\":\"%s\",\"proto\":%d,\"id\":\"%s\",\"fresh\":%s,"
                         "\"bright\":%d,\"layout\":\"%s\"}",
                         fw, kVersion, id, fresh ? "true" : "false", bright, layoutName(layout));
  return clampLen(n, cap);
}

size_t buildDenied(char* out, size_t cap, const char* id) {
  const int n = snprintf(out, cap, "{\"t\":\"denied\",\"id\":\"%s\"}", id);
  return n < 0 ? 0 : static_cast<size_t>(n) < cap ? static_cast<size_t>(n) : cap - 1;
}

size_t buildStatus(char* out, size_t cap, int bright, const Layout* layout) {
  int n;
  if (bright >= 0 && layout != nullptr) {
    n = snprintf(out, cap, "{\"t\":\"status\",\"bright\":%d,\"layout\":\"%s\"}", bright, layoutName(*layout));
  } else if (layout != nullptr) {
    n = snprintf(out, cap, "{\"t\":\"status\",\"layout\":\"%s\"}", layoutName(*layout));
  } else {
    n = snprintf(out, cap, "{\"t\":\"status\",\"bright\":%d}", bright);
  }
  return clampLen(n, cap);
}

size_t buildBulkGot(char* out, size_t cap, int id, uint32_t got) {
  return clampLen(snprintf(out, cap, "{\"t\":\"bulk\",\"id\":%d,\"got\":%lu}", id, static_cast<unsigned long>(got)),
                  cap);
}

size_t buildBulkFrom(char* out, size_t cap, int id, uint32_t from) {
  return clampLen(
      snprintf(out, cap, "{\"t\":\"bulk\",\"id\":%d,\"from\":%lu}", id, static_cast<unsigned long>(from)), cap);
}

size_t buildTransferState(char* out, size_t cap, const char* kind, const char* state, int id, const char* err) {
  int n;
  if (err != nullptr) {
    n = snprintf(out, cap, "{\"t\":\"%s\",\"st\":\"%s\",\"id\":%d,\"err\":\"%s\"}", kind, state, id, err);
  } else {
    n = snprintf(out, cap, "{\"t\":\"%s\",\"st\":\"%s\",\"id\":%d}", kind, state, id);
  }
  return clampLen(n, cap);
}

size_t buildInfo(char* out, size_t cap, const char* fw, const char* id, bool paired) {
  const int n = snprintf(out, cap, "{\"fw\":\"%s\",\"proto\":%d,\"id\":\"%s\",\"paired\":%s}", fw,
                         kVersion, id, paired ? "true" : "false");
  return n < 0 ? 0 : static_cast<size_t>(n) < cap ? static_cast<size_t>(n) : cap - 1;
}

}  // namespace monav::proto
