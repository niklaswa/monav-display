#include "ui/render.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "core/mapdata.h"
#include "core/nav.h"
#include "core/strings.h"
#include "ui/fonts.h"
#include "ui/maprender.h"
#include "ui/icons.h"
#include "ui/raster.h"

namespace monav::ui {
namespace {

using gfx::rgb;
using lgfx::textdatum_t;

constexpr float kPi = 3.14159265358979f;
constexpr float kDeg = kPi / 180.0f;

// --- Geometry --------------------------------------------------------------------------------

constexpr float kC = 233.0f;      // centre of the glass
constexpr float kRingR = 214.0f;  // speedometer ring (mid line)
constexpr float kRingW = 14.0f;
constexpr float kA0 = -135.0f;  // ring start (bottom left)
constexpr float kA1 = 135.0f;   // ring end (bottom right)
constexpr float kSweep = kA1 - kA0;

// Context card: a pill in the lower third, its round ends hold an icon and a sign. The HUD's is
// bigger; the map layout's sits lower, under the car.
struct CardGeom {
  float w, h, x, cy, y;
  float disc;       // icon disc / sign radius in the round ends
  float capL, capR; // centres of the left / right disc
  float textX;      // card text starts here
  float textR;      // ... and ends here when the right disc is used
  float textRFree;  // ... or here when it is not
  float line1;      // baseline of the first line
  float line2;      // baseline of the second line
};

constexpr CardGeom makeCard(float w, float h, float cy, float disc, float inset, float line1, float line2) {
  return CardGeom{w,
                  h,
                  kC - w / 2,
                  cy,
                  cy - h / 2,
                  disc,
                  kC - w / 2 + inset,
                  kC + w / 2 - inset,
                  kC - w / 2 + inset + disc + 13.0f,
                  kC + w / 2 - inset - disc - 11.0f,
                  kC + w / 2 - 30.0f,
                  cy + line1,
                  cy + line2};
}

constexpr CardGeom kHudCard = makeCard(320, 88, 332, 29, 40, 2, 29);
constexpr CardGeom kMapCard = makeCard(300, 76, 386, 26, 36, 1, 25);

constexpr float kChipCy = 404.0f;  // the small chip below the card
constexpr float kChipH = 32.0f;

// --- Colours ---------------------------------------------------------------------------------

namespace col {
constexpr uint32_t black = 0x000000;
constexpr uint32_t white = 0xFFFFFF;
constexpr uint32_t text2 = rgb(0x98, 0x98, 0x9F);  // secondary text
constexpr uint32_t text3 = rgb(0x63, 0x63, 0x66);
constexpr uint32_t tick = rgb(0x3A, 0x3A, 0x3C);
constexpr uint32_t track = rgb(0x1C, 0x1C, 0x1E);
constexpr uint32_t faintTrack = rgb(0x12, 0x12, 0x14);
constexpr uint32_t card = rgb(0x15, 0x15, 0x17);
constexpr uint32_t cardEdge = rgb(0x24, 0x24, 0x28);
constexpr uint32_t chip = rgb(0x1C, 0x1C, 0x1E);
constexpr uint32_t disc = rgb(0x2C, 0x2C, 0x2E);
constexpr uint32_t green = rgb(0x33, 0xD1, 0x73);
constexpr uint32_t red = rgb(0xFF, 0x45, 0x3A);
constexpr uint32_t amber = rgb(0xFF, 0xD6, 0x0A);
constexpr uint32_t blue = rgb(0x0A, 0x84, 0xFF);
constexpr uint32_t signRed = rgb(0xE2, 0x1F, 0x26);
}  // namespace col

// Pixel size of each font (scripts/build_fonts.py); Inter's cap height is 0.727 em.
constexpr float kFontPx[] = {150, 196, 80, 44, 30, 52, 38, 28, 21, 30, 26, 23, 18, 104, 13};
static_assert(sizeof(kFontPx) / sizeof(kFontPx[0]) == static_cast<int>(Font::Count), "font sizes out of sync");
float capHeight(Font f) { return kFontPx[static_cast<int>(f)] * 0.727f; }

float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
float easeOut(float t) {
  t = clamp01(t);
  return 1 - (1 - t) * (1 - t) * (1 - t);
}
float easeOutBack(float t) {
  t = clamp01(t);
  const float c1 = 1.70158f, c3 = c1 + 1;
  return 1 + c3 * std::pow(t - 1, 3.0f) + c1 * std::pow(t - 1, 2.0f);
}

// --- Drawing context -------------------------------------------------------------------------

struct Ctx {
  gfx::Surface s;
  const Frame& f;
  float alpha = 1.0f;  // fade of the whole screen (entry animation)
  bool mapReady = false;  // map layout with a picture under the car
  float mapX = 0, mapY = 0;
  CardGeom G = kHudCard;  // the card being drawn (the map layout's is smaller)
};

uint32_t (*g_clock)() = nullptr;
RenderProfile g_prof;
inline uint32_t clockUs() { return g_clock ? g_clock() : 0; }


/** Draws text; y is the baseline unless the datum says otherwise. Returns the width. */
int text(Ctx& x, Font font, const char* str, float px, float py, uint32_t color,
         textdatum_t datum = textdatum_t::baseline_center, float alpha = 1.0f) {
  if (str == nullptr || str[0] == 0) return 0;
  const uint32_t t0 = clockUs();
  const VlwFont& f = vlw(font);
  const int w = f.width(str);
  // Only bands the text reaches into draw it (all texts here sit on a baseline).
  if (gfx::rowsIn(x.s, py - f.ascent() - 1, py + f.descent() + 1)) {
    float left = px;
    if (datum == textdatum_t::baseline_center) {
      left = px - w / 2.0f;
    } else if (datum == textdatum_t::baseline_right) {
      left = px - w;
    }
    f.draw(x.s, str, left, py, color, alpha);
  }
  g_prof.textUs += clockUs() - t0;
  return w;
}

int width(Ctx& x, Font font, const char* str) {
  (void)x;
  return vlw(font).width(str);
}

/** Copies `src` into `out`, shortened with an ellipsis until it fits `maxW` pixels. */
void fit(Ctx& x, Font font, const char* src, float maxW, char* out, size_t cap) {
  snprintf(out, cap, "%s", src ? src : "");
  if (width(x, font, out) <= maxW) return;
  size_t n = strlen(out);
  while (n > 0) {
    // Drop one UTF-8 character (and trailing spaces).
    do {
      n--;
    } while (n > 0 && (static_cast<uint8_t>(out[n]) & 0xC0) == 0x80);
    while (n > 0 && out[n - 1] == ' ') n--;
    if (n + 4 > cap) continue;
    memcpy(out + n, "…", 3);
    out[n + 3] = 0;
    if (width(x, font, out) <= maxW) return;
  }
  out[0] = 0;
}

/** Number and unit of a distance: "340" "m", "1,2" "km". */
void distance(float m, Lang lang, char* num, size_t cap, const char** unit) {
  if (m < 995.0f) {
    const int v = m < 100.0f ? static_cast<int>(std::lround(m / 5.0f) * 5) : static_cast<int>(std::lround(m / 10.0f) * 10);
    snprintf(num, cap, "%d", v);
    *unit = "m";
  } else if (m < 9950.0f) {
    snprintf(num, cap, "%.1f", m / 1000.0f);
    if (lang == Lang::De) {
      for (char* p = num; *p; p++) {
        if (*p == '.') *p = ',';
      }
    }
    *unit = "km";
  } else {
    snprintf(num, cap, "%d", static_cast<int>(std::lround(m / 1000.0f)));
    *unit = "km";
  }
}

/** Number with its unit set smaller after it, left-aligned at x. Returns the total width. */
int valueWithUnit(Ctx& x, float px, float baseline, const char* num, const char* unit, uint32_t color,
                  Font numFont = Font::Num) {
  const int w = text(x, numFont, num, px, baseline, color, textdatum_t::baseline_left);
  if (unit == nullptr || unit[0] == 0) return w;
  const int u = text(x, Font::Body, unit, px + w + 5, baseline, col::text2, textdatum_t::baseline_left);
  return w + 5 + u;
}

// --- Pieces ----------------------------------------------------------------------------------

/** German speed-limit sign (Zeichen 274): white disc, red ring, black digits. */
void limitSign(Ctx& x, float cx, float cy, float r, int value) {
  gfx::fillCircle(x.s, cx, cy, r, col::white);
  gfx::strokeCircle(x.s, cx, cy, r * 0.82f, r * 0.2f, col::signRed);
  char buf[8];
  snprintf(buf, sizeof(buf), "%d", value);
  const float inner = r * 1.3f;  // usable width inside the red ring
  const Font order[] = {Font::SignLg, Font::SignMd, Font::SignSm, Font::SignXs};
  Font font = Font::SignXs;
  for (Font f : order) {
    if (capHeight(f) <= r * 0.73f && width(x, f, buf) <= inner) {
      font = f;
      break;
    }
  }
  text(x, font, buf, cx + 0.5f, cy + capHeight(font) / 2, col::black);
}

uint32_t speedStatus(float speed, int limit) {
  if (limit <= 0) return col::white;
  const long sp = std::lround(speed);
  if (sp > limit + 5) return col::red;
  if (sp > limit) return col::amber;
  return col::green;
}

/** The speedometer ring: track, fill up to the speed, ticks, the limit mark. */
void speedRingImpl(Ctx& x, const Frame& f);
void speedRing(Ctx& x, const Frame& f) {
  const uint32_t t0 = clockUs();
  speedRingImpl(x, f);
  g_prof.ringUs += clockUs() - t0;
}
void speedRingImpl(Ctx& x, const Frame& f) {
  const uint32_t status = speedStatus(f.speed, f.limit);
  gfx::ringArc(x.s, kC, kC, kRingR, kRingW, kA0, kA1, col::track, col::track);

  // Ticks every 10 (or 20) km/h, just inside the ring.
  const float step = f.scaleMax <= 140 ? 10.0f : 20.0f;
  const float tickR = kRingR - kRingW / 2 - 9;
  for (float v = step; v < f.scaleMax - step * 0.4f; v += step) {
    const float a = (kA0 + kSweep * v / f.scaleMax) * kDeg;
    const bool major = std::fmod(v + 0.5f, 50.0f) < 1.0f;
    gfx::fillCircle(x.s, kC + tickR * std::sin(a), kC - tickR * std::cos(a), major ? 2.2f : 1.5f,
                    major ? col::text3 : col::tick);
  }

  const float frac = clamp01(f.speed / f.scaleMax);
  if (frac > 0.003f) {
    const float tip = kA0 + kSweep * frac;
    gfx::ringArc(x.s, kC, kC, kRingR, kRingW, kA0, tip, gfx::mix(col::black, status, 0.22f), status);
  }

  if (f.limit > 0) {
    // The limit mark crosses the ring; with the scale at twice the limit it sits at the top.
    const float a = (kA0 + kSweep * clamp01(f.limit / f.scaleMax)) * kDeg;
    const float sx = std::sin(a), sy = -std::cos(a);
    const float r0 = kRingR - kRingW / 2 - 5, r1 = kRingR + kRingW / 2 + 4;
    gfx::line(x.s, kC + r0 * sx, kC + r0 * sy, kC + r1 * sx, kC + r1 * sy, 9.0f, col::black);
    gfx::line(x.s, kC + r0 * sx, kC + r0 * sy, kC + r1 * sx, kC + r1 * sy, 4.0f, col::white);
  }
}

/** The faint ring outline on the quiet screens, so the glass reads as the same instrument. */
void quietRing(Ctx& x) { gfx::ringArc(x.s, kC, kC, kRingR, kRingW, kA0, kA1, col::faintTrack, col::faintTrack); }

void card(Ctx& x, uint32_t edge = col::cardEdge, float edgeW = 1.5f) {
  // The edge as a slightly bigger pill under the card: two solid fills are much cheaper than a
  // stroke around it.
  gfx::fillRoundRect(x.s, x.G.x - edgeW / 2, x.G.y - edgeW / 2, x.G.w + edgeW, x.G.h + edgeW, (x.G.h + edgeW) / 2, edge);
  gfx::fillRoundRect(x.s, x.G.x + edgeW / 2, x.G.y + edgeW / 2, x.G.w - edgeW, x.G.h - edgeW, (x.G.h - edgeW) / 2, col::card);
}

const char* camLabel(const Frame& f) {
  if (f.scene.cam.label[0]) return f.scene.cam.label;
  switch (f.scene.cam.kind) {
    case CamKind::Fixed:
      return tr(f.lang, Str::CamFixed);
    case CamKind::Mobile:
      return tr(f.lang, Str::CamMobile);
    case CamKind::RedLight:
      return tr(f.lang, Str::CamRedLight);
    case CamKind::Section:
      return tr(f.lang, Str::CamSection);
    case CamKind::Distance:
      return tr(f.lang, Str::CamDistance);
    default:
      return tr(f.lang, Str::CamOther);
  }
}

uint32_t lightColor(LightState st) {
  switch (st) {
    case LightState::Red:
      return col::red;
    case LightState::Green:
      return col::green;
    case LightState::Uncertain:
      return col::amber;
    default:
      return col::text2;
  }
}

Str lightLabel(LightState st) {
  switch (st) {
    case LightState::Red:
      return Str::LightRed;
    case LightState::Green:
      return Str::LightGreen;
    case LightState::Uncertain:
      return Str::LightUncertain;
    default:
      return Str::LightUnknown;
  }
}

uint32_t sectionColor(const Section& sec) {
  if (sec.vmax <= 0) return col::white;
  if (sec.avg > sec.vmax + 0.5f) return col::red;
  if (sec.avg > sec.vmax - 2.0f) return col::amber;
  return col::green;
}

void cameraCard(Ctx& x, const Frame& f) {
  const Camera& cam = f.scene.cam;
  const bool alert = f.alert > 0;
  if (alert) {
    const float pulse = 0.5f + 0.5f * std::sin(f.t * 2 * kPi * 1.4f);
    card(x, gfx::mix(col::cardEdge, col::red, 0.45f + 0.55f * pulse), 2.5f);
  } else {
    card(x);
  }
  gfx::fillCircle(x.s, x.G.capL, x.G.cy, x.G.disc, col::red);
  icons::camera(x.s, x.G.capL, x.G.cy, x.G.disc * 0.534f, cam.kind, col::white, col::red);

  const float right = cam.vmax > 0 ? x.G.textR : x.G.textRFree;
  char num[12];
  const char* unit;
  distance(f.camD, f.lang, num, sizeof(num), &unit);
  valueWithUnit(x, x.G.textX, x.G.line1, num, unit, col::white);
  char label[64];
  fit(x, Font::Small, camLabel(f), right - x.G.textX, label, sizeof(label));
  text(x, Font::Small, label, x.G.textX, x.G.line2, col::text2, textdatum_t::baseline_left);

  if (cam.vmax > 0) limitSign(x, x.G.capR, x.G.cy, x.G.disc, cam.vmax);
}

void sectionCard(Ctx& x, const Frame& f) {
  const Section& sec = f.scene.sec;
  const uint32_t c = sectionColor(sec);
  card(x);
  gfx::fillCircle(x.s, x.G.capL, x.G.cy, x.G.disc, col::disc);
  gfx::strokeCircle(x.s, x.G.capL, x.G.cy, x.G.disc - 1.5f, 3.0f, c);
  icons::camera(x.s, x.G.capL, x.G.cy, x.G.disc * 0.517f, CamKind::Section, col::white, col::disc);

  char num[16];
  snprintf(num, sizeof(num), "Ø %d", static_cast<int>(std::lround(sec.avg)));
  valueWithUnit(x, x.G.textX, x.G.line1, num, nullptr, c);
  const float right = sec.vmax > 0 ? x.G.textR : x.G.textRFree;
  char label[64];
  fit(x, Font::Small, tr(f.lang, Str::Section), right - x.G.textX, label, sizeof(label));
  text(x, Font::Small, label, x.G.textX, x.G.line2, col::text2, textdatum_t::baseline_left);
  if (sec.vmax > 0) limitSign(x, x.G.capR, x.G.cy, x.G.disc, sec.vmax);
}

/** The light's phase as a ring that empties towards the change, the seconds inside. */
void lightRing(Ctx& x, const Frame& f, float cx, float cy, float r, float w, Font font) {
  const Light& l = f.scene.light;
  const uint32_t c = lightColor(l.st);
  if (f.lightLeft >= 0) {
    gfx::strokeCircle(x.s, cx, cy, r, w, gfx::mix(col::black, c, 0.25f));
    const float phase = f.lightPhase >= 0 ? f.lightPhase : 1.0f;
    if (phase > 0.004f) gfx::arc(x.s, cx, cy, r, w, 0.0f, 360.0f * phase, c, c);
    char num[8];
    const int secs = static_cast<int>(std::ceil(f.lightLeft - 0.05f));
    snprintf(num, sizeof(num), "%d", secs);
    if (secs >= 100) font = Font::Small;
    text(x, font, num, cx, cy + capHeight(font) / 2, c);
  } else {
    for (int k = 4; k >= 1; k--) gfx::fillCircle(x.s, cx, cy, r * 0.42f + k * 3.0f, c, 0.08f);
    gfx::fillCircle(x.s, cx, cy, r * 0.42f, c);
  }
}

void lightCard(Ctx& x, const Frame& f) {
  const Light& l = f.scene.light;
  card(x);
  lightRing(x, f, x.G.capL, x.G.cy, x.G.disc - 3.0f, 5, Font::NumSm);

  // What to do: the first line and the badge in the right end.
  const bool standing = f.speed < 5.0f;
  Str action;
  uint32_t actionColor;
  enum { None, Speed, Coast } badge = None;
  if (standing) {
    action = lightLabel(l.st);
    actionColor = lightColor(l.st);
  } else if (l.adv > 0) {
    const float diff = static_cast<float>(l.adv) - f.speed;
    action = diff < -2.5f ? Str::Slower : diff > 2.5f ? Str::Faster : Str::KeepSpeed;
    actionColor = action == Str::Slower ? col::amber : col::green;
    badge = Speed;
  } else if (l.adv == 0) {
    action = Str::Coast;
    actionColor = col::amber;
    badge = Coast;
  } else if (l.st == LightState::Unknown) {
    action = Str::LightUnknown;
    actionColor = col::text2;
  } else {
    // The current speed is right: the green headline says it all, no badge.
    action = Str::KeepSpeed;
    actionColor = col::green;
  }

  const float right = badge != None ? x.G.textR : x.G.textRFree;
  char line[64];
  Font f1 = Font::Label;
  if (width(x, f1, tr(f.lang, action)) > right - x.G.textX) f1 = Font::Body;
  fit(x, f1, tr(f.lang, action), right - x.G.textX, line, sizeof(line));
  text(x, f1, line, x.G.textX, x.G.line1 - 3, actionColor, textdatum_t::baseline_left);

  // Second line: "Rot · 220 m" (or "bis Grün · 8 m" while standing), lane arrow first.
  float tx = x.G.textX;
  if (l.lane != Lane::All) {
    icons::lane(x.s, tx + 7, x.G.line2 - 6, 7.5f, l.lane, col::text2);
    tx += 20;
  }
  char num[12];
  const char* unit;
  distance(f.lightD, f.lang, num, sizeof(num), &unit);
  char src[64];
  if (standing) {
    snprintf(src, sizeof(src), "%s · %s %s",
             tr(f.lang, l.st == LightState::Red ? Str::UntilGreen : Str::UntilRed), num, unit);
  } else {
    snprintf(src, sizeof(src), "%s · %s %s", tr(f.lang, lightLabel(l.st)), num, unit);
  }
  fit(x, Font::Small, src, right - tx, line, sizeof(line));
  text(x, Font::Small, line, tx, x.G.line2, col::text2, textdatum_t::baseline_left);

  switch (badge) {
    case Speed: {
      // The speed that catches green, as a green-ringed badge.
      gfx::fillCircle(x.s, x.G.capR, x.G.cy, x.G.disc, rgb(0x0C, 0x24, 0x16));
      gfx::strokeCircle(x.s, x.G.capR, x.G.cy, x.G.disc - 2.5f, 5.0f, col::green);
      char adv[8];
      snprintf(adv, sizeof(adv), "%d", l.adv);
      const Font font = width(x, Font::SignMd, adv) <= 38 ? Font::SignMd : Font::SignSm;
      text(x, font, adv, x.G.capR + 0.5f, x.G.cy + capHeight(font) / 2, col::white);
      break;
    }
    case Coast:
      gfx::fillCircle(x.s, x.G.capR, x.G.cy, x.G.disc, rgb(0x2E, 0x26, 0x05));
      gfx::strokeCircle(x.s, x.G.capR, x.G.cy, x.G.disc - 2.5f, 5.0f, col::amber);
      icons::chevrons(x.s, x.G.capR, x.G.cy + 1, x.G.disc * 0.41f, true, col::amber);
      break;
    default:
      break;
  }
}

void turnCard(Ctx& x, const Frame& f) {
  const Turn& t = f.scene.turn;
  card(x);
  gfx::fillCircle(x.s, x.G.capL, x.G.cy, x.G.disc, col::blue);
  icons::turn(x.s, x.G.capL, x.G.cy, x.G.disc * 0.655f, t.kind, t.exit, col::white, gfx::mix(col::blue, col::white, 0.4f), col::blue);
  if (t.kind == TurnKind::Roundabout && t.exit > 0) {
    // Exit number as a badge on the disc.
    const float bx = x.G.capL + x.G.disc * 0.72f, by = x.G.cy - x.G.disc * 0.69f;
    gfx::fillCircle(x.s, bx, by, 12.5f, col::card);
    gfx::fillCircle(x.s, bx, by, 10.5f, col::white);
    char n[4];
    snprintf(n, sizeof(n), "%d", t.exit);
    text(x, Font::Small, n, bx + 0.5f, by + capHeight(Font::Small) / 2, col::black);
  }

  const float right = x.G.textRFree;
  char num[12];
  const char* unit;
  distance(f.turnD, f.lang, num, sizeof(num), &unit);
  valueWithUnit(x, x.G.textX, x.G.line1, num, unit, col::white);

  char src[96];
  const char* road = t.road[0] ? t.road : nullptr;
  if (road) {
    // A roundabout's exit is the badge on the arrow; the line is the road.
    snprintf(src, sizeof(src), "%s", road);
  } else if (t.kind == TurnKind::Roundabout && t.exit > 0) {
    snprintf(src, sizeof(src), tr(f.lang, Str::ExitFmt), t.exit);
  } else if (t.kind == TurnKind::Arrive) {
    snprintf(src, sizeof(src), "%s", tr(f.lang, Str::Destination));
  } else if (t.kind == TurnKind::Roundabout) {
    snprintf(src, sizeof(src), "%s", tr(f.lang, Str::Roundabout));
  } else {
    src[0] = 0;
  }
  char line[96];
  fit(x, Font::Small, src, right - x.G.textX, line, sizeof(line));
  text(x, Font::Small, line, x.G.textX, x.G.line2, col::text2, textdatum_t::baseline_left);
}

/** Small pill below the card: an icon and a short text. */
void chip(Ctx& x, float cy, const char* label, uint32_t textColor, int iconW,
          void (*icon)(Ctx&, const Frame&, float, float), float alpha = 1.0f) {
  const int tw = width(x, Font::Small, label);
  const float pad = 14;
  const float gap = iconW > 0 ? 8.0f : 0.0f;
  const float w = pad * 2 + iconW + gap + tw;
  const float x0 = kC - w / 2;
  gfx::fillRoundRect(x.s, x0, cy - kChipH / 2, w, kChipH, kChipH / 2, col::chip, alpha);
  if (icon) icon(x, x.f, x0 + pad + iconW / 2.0f, cy);
  text(x, Font::Small, label, x0 + pad + iconW + gap, cy + capHeight(Font::Small) / 2, textColor,
       textdatum_t::baseline_left, alpha);
}

void secondaryChip(Ctx& x, const Frame& f, Card which) {
  char label[48];
  char num[12];
  const char* unit;
  switch (which) {
    case Card::Camera:
      distance(f.camD, f.lang, num, sizeof(num), &unit);
      snprintf(label, sizeof(label), "%s %s", num, unit);
      chip(x, kChipCy, label, col::white, 20, [](Ctx& c, const Frame& fr, float cx, float cy) {
        icons::camera(c.s, cx, cy, 9.5f, fr.scene.cam.kind, col::red, col::chip);
      });
      break;
    case Card::Section: {
      snprintf(label, sizeof(label), "Ø %d %s", static_cast<int>(std::lround(f.scene.sec.avg)),
               tr(f.lang, Str::Kmh));
      chip(x, kChipCy, label, sectionColor(f.scene.sec), 0, nullptr);
      break;
    }
    case Card::Light:
      if (f.lightLeft >= 0) {
        snprintf(label, sizeof(label), "%d s", static_cast<int>(std::ceil(f.lightLeft - 0.05f)));
      } else {
        distance(f.lightD, f.lang, num, sizeof(num), &unit);
        snprintf(label, sizeof(label), "%s %s", num, unit);
      }
      chip(x, kChipCy, label, lightColor(f.scene.light.st), 14, [](Ctx& c, const Frame& fr, float cx, float cy) {
        gfx::fillCircle(c.s, cx, cy, 6.5f, lightColor(fr.scene.light.st));
      });
      break;
    case Card::Turn:
      distance(f.turnD, f.lang, num, sizeof(num), &unit);
      snprintf(label, sizeof(label), "%s %s", num, unit);
      chip(x, kChipCy, label, col::white, 18, [](Ctx& c, const Frame& fr, float cx, float cy) {
        icons::turn(c.s, cx, cy, 9.0f, fr.scene.turn.kind, fr.scene.turn.exit, col::white, col::text3, col::chip);
      });
      break;
    default:
      break;
  }
}

// --- Screens ---------------------------------------------------------------------------------

void primaryCardImpl(Ctx& x, const Frame& f);
void primaryCard(Ctx& x, const Frame& f) {
  const uint32_t t0 = clockUs();
  primaryCardImpl(x, f);
  g_prof.cardUs += clockUs() - t0;
}
void primaryCardImpl(Ctx& x, const Frame& f) {
  switch (f.primary) {
    case Card::Camera:
      cameraCard(x, f);
      break;
    case Card::Section:
      sectionCard(x, f);
      break;
    case Card::Light:
      lightCard(x, f);
      break;
    case Card::Turn:
      turnCard(x, f);
      break;
    default:
      break;
  }
}

/** A small arc going round: something is on its way. */
void spinnerIcon(Ctx& c, const Frame& fr, float cx, float cy) {
  const float a = std::fmod(fr.t * 300.0f, 360.0f);
  gfx::strokeCircle(c.s, cx, cy, 6.5f, 2.2f, col::tick);
  gfx::arc(c.s, cx, cy, 6.5f, 2.2f, a, a + 110.0f, col::white, col::white);
}

/** The map layout's thin speed ring along the edge of the glass, over the map. */
void edgeRingImpl(Ctx& x, const Frame& f);
void edgeRing(Ctx& x, const Frame& f) {
  const uint32_t t0 = clockUs();
  edgeRingImpl(x, f);
  g_prof.ringUs += clockUs() - t0;
}
void edgeRingImpl(Ctx& x, const Frame& f) {
  constexpr float r = 227.0f, w = 8.0f;
  uint32_t status = speedStatus(f.speed, f.limit);
  float alpha = 1.0f;
  if (f.alert > 0) {
    // Camera close and too fast: the ring turns red and pulses (in the same pass).
    status = col::red;
    alpha = 0.55f + 0.45f * (0.5f + 0.5f * std::sin(f.t * 2 * kPi * 1.4f));
  }
  const float frac = f.alert > 0 ? 1.0f : clamp01(f.speed / f.scaleMax);
  if (frac > 0.003f) {
    gfx::ringArc(x.s, kC, kC, r, w, kA0, kA0 + kSweep * frac, gfx::mix(col::black, status, f.alert > 0 ? 1.0f : 0.3f),
                 status, alpha);
  }
  if (f.limit > 0) {
    const float a = (kA0 + kSweep * clamp01(f.limit / f.scaleMax)) * kDeg;
    const float sx = std::sin(a), sy = -std::cos(a);
    gfx::line(x.s, kC + (r - 7) * sx, kC + (r - 7) * sy, kC + (r + 5) * sx, kC + (r + 5) * sy, 3.5f, col::white);
  }
}

/** The map layout's speed and limit, side by side at the top over the darkened map. */
void mapSpeed(Ctx& x, const Frame& f, const char* num, uint32_t numColor) {
  constexpr float base = 116.0f, signR = 31.0f, gap = 14.0f;
  const int w = width(x, Font::SpeedMd, num);
  float numCx = kC;
  if (f.limit > 0) {
    const float group = 2 * signR + gap + w;
    const float x0 = kC - group / 2;
    limitSign(x, x0 + signR, base - capHeight(Font::SpeedMd) / 2, signR, f.limit);
    numCx = x0 + 2 * signR + gap + w / 2.0f;
  }
  text(x, Font::SpeedMd, num, numCx, base, numColor);
  text(x, Font::Small, tr(f.lang, Str::Kmh), numCx, base + 24, col::text2);
}

/**
 * Charging stations as pins over the map: a pill with a bolt and the free places (or free/all),
 * green while there is a free one, grey when full. Farther pins first, so nearer ones lie on top;
 * none under the speed at the top.
 */
void chargerPins(Ctx& x, const Frame& f, const MapCamera& cam) {
  const Chargers& cs = f.chargers;
  if (cs.count == 0) return;
  const MapImage& m = *f.map;
  struct Pin {
    float x, y;
    int i;
  } pins[Chargers::kMax];
  int n = 0;
  for (int i = 0; i < cs.count; i++) {
    double e, no;
    geoToLocal(m.lat, m.lon, cs.st[i].lat, cs.st[i].lon, e, no);
    const float px = m.w / 2.0f + static_cast<float>(e / m.mpp);
    const float py = m.h / 2.0f - static_cast<float>(no / m.mpp);
    float sx, sy, scale;
    if (!project(cam, px, py, sx, sy, scale) || scale < 0.45f) continue;
    const float dx = sx - kC, dy = sy - kC;
    if (sy < 178 || dx * dx + dy * dy > 205.0f * 205.0f) continue;
    Pin p{sx, sy, i};
    int k = n++;
    while (k > 0 && pins[k - 1].y > p.y) {
      pins[k] = pins[k - 1];
      k--;
    }
    pins[k] = p;
  }
  constexpr float h = 26, pad = 8, boltW = 11, gap = 3, lift = 22;
  for (int k = 0; k < n; k++) {
    const Charger& c = cs.st[pins[k].i];
    const float px = pins[k].x, py = pins[k].y;
    if (!gfx::rowsIn(x.s, py - lift - h, py + 2)) continue;
    char label[12];
    if (c.total >= 0) {
      snprintf(label, sizeof(label), "%d/%d", c.free, c.total);
    } else {
      snprintf(label, sizeof(label), "%d", c.free);
    }
    const bool free = c.free > 0;
    const uint32_t fill = free ? col::green : col::disc;
    const uint32_t fg = free ? col::black : col::white;
    const int tw = width(x, Font::Small, label);
    const float w = pad + boltW + gap + tw + pad;
    const float left = px - w / 2, cy = py - lift;
    // Dark rim for contrast on a bright map, then the pill and its point.
    gfx::fillRoundRect(x.s, left - 1.5f, cy - h / 2 - 1.5f, w + 3, h + 3, h / 2 + 1.5f, col::black, 0.55f);
    const float tip[] = {px - 6, cy + h / 2 - 1, px + 6, cy + h / 2 - 1, px, py};
    gfx::convex(x.s, tip, 3, fill);
    gfx::fillRoundRect(x.s, left, cy - h / 2, w, h, h / 2, fill);
    icons::bolt(x.s, left + pad + boltW / 2, cy, 7.5f, fg);
    text(x, Font::Small, label, left + pad + boltW + gap, cy + capHeight(Font::Small) / 2, fg,
         textdatum_t::baseline_left);
  }
}

void mapScreen(Ctx& x, float mx, float my, const char* num, uint32_t numColor) {
  const Frame& f = x.f;
  MapCamera cam;
  cam.carX = mx;
  cam.carY = my;
  cam.hdg = f.hdg;
  cam.tilt = f.tilt;
  const uint32_t t0 = clockUs();
  drawMap(x.s, *f.map, cam, 0.0f, 0.0f);
  const uint32_t t1 = clockUs();
  if (f.route != nullptr) drawRoute(x.s, *f.map, *f.route, cam);
  chargerPins(x, f, cam);
  g_prof.mapUs += t1 - t0;
  g_prof.routeUs += clockUs() - t1;
  if (x.s.oy == 0) g_prof.mapFrames++;  // once per frame, not per band
  drawCar(x.s, cam.cx, cam.cy);
  edgeRing(x, f);
  mapSpeed(x, f, num, numColor);
  x.G = kMapCard;
  primaryCard(x, f);
  x.G = kHudCard;
  // Credited to whoever drew the picture: Apple (MapKit) unless the phone names another.
  const char* attr = f.map->attr[0] != 0 ? f.map->attr : tr(f.lang, Str::Attribution);
  text(x, Font::Tiny, attr, kC, 446, col::text2, textdatum_t::baseline_center, 0.8f);
}

void driveScreen(Ctx& x) {
  const Frame& f = x.f;
  if (f.linkLost) x.s.dim = 0.32f;

  const long sp = std::lround(f.speed < 0 ? 0 : f.speed);
  char num[8];
  snprintf(num, sizeof(num), "%ld", sp);
  const uint32_t status = speedStatus(f.speed, f.limit);
  const uint32_t numColor = status == col::green ? col::white : status;

  // The map layout needs a picture under the car; until there is one, the HUD and a chip say so.
  const bool mapLayout = f.layout == Layout::Map;
  const bool mapReady = x.mapReady;

  if (mapReady) {
    mapScreen(x, x.mapX, x.mapY, num, numColor);
  } else {
    // Camera close and too fast: a red pulse glows in from the rim, under the ring.
    if (f.alert > 0 && !f.linkLost) {
      const float pulse = 0.5f + 0.5f * std::sin(f.t * 2 * kPi * 1.4f);
      gfx::rimGlow(x.s, kC, kC, 168, 226, 233, col::red, 0.18f + 0.42f * pulse);
    }
    speedRing(x, f);

    if (f.focus) {
      if (f.limit > 0) limitSign(x, kC, 96, 56, f.limit);
      const float base = f.limit > 0 ? 322.0f : 300.0f;
      text(x, Font::SpeedXL, num, kC, base, numColor);
      text(x, Font::Body, tr(f.lang, Str::Kmh), kC, base + 42, col::text2);
    } else {
      if (f.limit > 0) limitSign(x, kC, 82, 44, f.limit);
      const float base = 256.0f;
      // Centred under the sign; three digits push the group left just enough to keep "km/h" off the ring.
      const int w = width(x, Font::Speed, num);
      const int uw = width(x, Font::Body, tr(f.lang, Str::Kmh));
      const float overflow = kC + w / 2.0f + 8 + uw - (kC + 178.0f);
      const float cx = overflow > 0 ? kC - overflow : kC;
      text(x, Font::Speed, num, cx, base, numColor);
      text(x, Font::Body, tr(f.lang, Str::Kmh), cx + w / 2.0f + 8, base, col::text2, textdatum_t::baseline_left);

      primaryCard(x, f);
      if (!f.linkLost) {
        if (mapLayout) {
          chip(x, kChipCy, tr(f.lang, Str::MapLoading), col::text2, 16, spinnerIcon);
        } else if (f.secondary != Card::None) {
          secondaryChip(x, f, f.secondary);
        } else if (f.primary == Card::Turn && f.hasEta) {
          char eta[16], label[40];
          snprintf(eta, sizeof(eta), "%02d:%02d", f.eta.hour, f.eta.minute);
          snprintf(label, sizeof(label), tr(f.lang, Str::ArrivalFmt), eta);
          chip(x, kChipCy, label, col::text2, 0, nullptr);
        }
      }
    }
  }

  if (f.linkLost) {
    x.s.dim = 1.0f;
    const float cy = mapReady ? 176.0f : f.focus ? 404.0f : kChipCy;
    chip(x, cy, tr(f.lang, Str::LinkLost), col::white, 12, [](Ctx& c, const Frame& fr, float cx, float cyy) {
      const float pulse = 0.55f + 0.45f * std::sin(fr.t * 2 * kPi * 0.8f);
      gfx::fillCircle(c.s, cx, cyy, 5.5f, col::amber, pulse);
    });
  }
}

/** Firmware update: the progress around the glass, what is happening in the middle. */
void updateScreen(Ctx& x) {
  const Frame& f = x.f;
  const Frame::Update& u = f.update;
  using Phase = Frame::Update::Phase;
  const bool failed = u.phase == Phase::Failed;
  const uint32_t accent = failed ? col::red : u.phase == Phase::Restarting ? col::green : col::blue;
  gfx::ringArc(x.s, kC, kC, kRingR, kRingW, kA0, kA1, col::track, col::track);
  const float p = failed ? 1.0f : clamp01(u.progress);
  if (p > 0.003f) gfx::ringArc(x.s, kC, kC, kRingR, kRingW, kA0, kA0 + kSweep * p, gfx::mix(col::black, accent, 0.35f), accent);

  if (failed) {
    gfx::fillCircle(x.s, kC, 150, 40, rgb(0x34, 0x10, 0x0E));
    gfx::line(x.s, kC - 13, 137, kC + 13, 163, 6, col::red);
    gfx::line(x.s, kC + 13, 137, kC - 13, 163, 6, col::red);
    text(x, Font::Title, tr(f.lang, Str::UpdateFailed), kC, 240, col::white);
    char err[48];
    fit(x, Font::Small, u.error, 300, err, sizeof(err));
    text(x, Font::Small, err, kC, 272, col::text2);
    text(x, Font::Small, tr(f.lang, Str::KeepsRunning), kC, 312, col::text2);
    return;
  }
  if (u.phase == Phase::Restarting) {
    gfx::fillCircle(x.s, kC, 186, 46, col::green);
    icons::check(x.s, kC, 186, 26, 8.0f, col::black);
    text(x, Font::Title, tr(f.lang, Str::Restarting), kC, 280, col::white);
    if (u.version[0]) text(x, Font::Body, u.version, kC, 314, col::text2);
    return;
  }
  char title[48];
  snprintf(title, sizeof(title), "%s %s", tr(f.lang, Str::UpdateTitle), u.version);
  text(x, Font::Body, title, kC, 150, col::text2);
  char pct[8];
  snprintf(pct, sizeof(pct), "%d", static_cast<int>(p * 100.0f + 0.5f) > 100 ? 100 : static_cast<int>(p * 100.0f));
  const int w = text(x, Font::SpeedMd, pct, kC - 10, 262, col::white);
  text(x, Font::Title, "%", kC - 10 + w / 2.0f + 6, 262, col::text2, textdatum_t::baseline_left);
  text(x, Font::Body, tr(f.lang, u.phase == Phase::Verifying ? Str::Verifying : Str::DoNotPowerOff), kC, 316,
       u.phase == Phase::Verifying ? col::text2 : col::amber);
}

void idPill(Ctx& x, float cy, float alpha) {
  const Frame& f = x.f;
  const int tw = width(x, Font::NumSm, f.id);
  const float w = tw + 40.0f;
  gfx::fillRoundRect(x.s, kC - w / 2, cy - 22, w, 44, 22, col::chip, alpha);
  text(x, Font::NumSm, f.id, kC, cy + capHeight(Font::NumSm) / 2, col::white, textdatum_t::baseline_center, alpha);
}

void waitingScreen(Ctx& x) {
  const Frame& f = x.f;
  quietRing(x);
  const float cy = 158;
  // Rings travelling outwards: the display is calling.
  for (int i = 0; i < 3; i++) {
    float ph = std::fmod(f.t / 2.4f + i / 3.0f, 1.0f);
    const float r = 46 + ph * 74;
    const float a = (1 - ph) * (1 - ph) * 0.55f;
    gfx::strokeCircle(x.s, kC, cy, r, 2.5f, col::blue, a);
  }
  gfx::fillCircle(x.s, kC, cy, 42, col::blue);
  icons::bluetooth(x.s, kC, cy, 21, col::white);

  if (f.paired) {
    text(x, Font::Title, tr(f.lang, Str::Waiting), kC, 290, col::white);
    idPill(x, 346, 1.0f);
  } else {
    text(x, Font::Title, tr(f.lang, Str::ReadyToPair), kC, 284, col::white);
    text(x, Font::Body, tr(f.lang, Str::PairHint), kC, 318, col::text2);
    idPill(x, 370, 1.0f);
  }
}

void pairedScreen(Ctx& x) {
  const Frame& f = x.f;
  quietRing(x);
  const float a = f.screenAge;
  const float cy = 176;
  // Burst ring.
  if (a > 0.15f && a < 1.2f) {
    const float p = clamp01((a - 0.15f) / 1.0f);
    gfx::strokeCircle(x.s, kC, cy, 58 + easeOut(p) * 80, 3.0f, col::green, (1 - p) * 0.7f);
  }
  const float r = 58 * easeOutBack(a / 0.42f);
  if (r > 1) gfx::fillCircle(x.s, kC, cy, r, col::green);
  icons::check(x.s, kC, cy, 30, 9.0f, col::black, clamp01((a - 0.3f) / 0.35f));
  const float ta = clamp01((a - 0.45f) / 0.4f);
  text(x, Font::Title, tr(f.lang, Str::Paired), kC, 296 + (1 - ta) * 10, col::white, textdatum_t::baseline_center, ta);
  text(x, Font::Body, tr(f.lang, Str::PairedSub), kC, 332 + (1 - ta) * 10, col::text2, textdatum_t::baseline_center,
       ta);
}

void deniedScreen(Ctx& x) {
  const Frame& f = x.f;
  quietRing(x);
  const float cy = 140;
  gfx::fillCircle(x.s, kC, cy, 46, rgb(0x34, 0x10, 0x0E));
  gfx::strokeCircle(x.s, kC, cy, 44.5f, 3.0f, col::red);
  icons::phone(x.s, kC - 3, cy + 1, 21, col::white);
  // Small "not this one" badge.
  gfx::fillCircle(x.s, kC + 30, cy + 28, 15, col::black);
  gfx::fillCircle(x.s, kC + 30, cy + 28, 12, col::red);
  gfx::line(x.s, kC + 25, cy + 28, kC + 35, cy + 28, 3.5f, col::white);

  text(x, Font::Title, tr(f.lang, Str::Denied1), kC, 240, col::white);
  text(x, Font::Title, tr(f.lang, Str::Denied2), kC, 276, col::white);
  text(x, Font::Body, tr(f.lang, Str::DeniedHint), kC, 318, col::text2);
  idPill(x, 372, 1.0f);
}

void clockText(Ctx& x, float baseline, uint32_t color) {
  char buf[8];
  snprintf(buf, sizeof(buf), "%02d:%02d", x.f.clock.hour, x.f.clock.minute);
  text(x, Font::Clock, buf, kC, baseline, color);
}

void connectedScreen(Ctx& x) {
  const Frame& f = x.f;
  quietRing(x);
  // Status line on top: green dot + "Verbunden".
  const char* label = tr(f.lang, Str::Connected);
  const int tw = width(x, Font::Small, label);
  const float w = tw + 18.0f;
  const float pulse = 0.6f + 0.4f * std::sin(f.t * 2 * kPi * 0.5f);
  gfx::fillCircle(x.s, kC - w / 2 + 5, 140 - 6, 5, col::green, pulse);
  text(x, Font::Small, label, kC - w / 2 + 18, 140, col::green, textdatum_t::baseline_left);
  if (f.hasClock) {
    clockText(x, 270, col::white);
  } else {
    gfx::fillCircle(x.s, kC, 220, 40, col::green);
    icons::check(x.s, kC, 220, 22, 7.0f, col::black);
  }
  text(x, Font::Body, tr(f.lang, Str::ConnectedSub), kC, 318, col::text2);
}

void standbyScreen(Ctx& x) {
  const Frame& f = x.f;
  quietRing(x);
  if (f.hasClock) clockText(x, 262, rgb(0xD8, 0xD8, 0xDC));
  text(x, Font::Body, tr(f.lang, Str::Bye), kC, 312, col::text2);
}

void bootScreen(Ctx& x) {
  const Frame& f = x.f;
  const float a = f.screenAge;
  const int lit = a < 0.35f ? 0 : a < 0.7f ? 1 : 2;
  const float glow = lit == 2 ? easeOut((a - 0.7f) / 0.5f) : 0.6f;
  icons::trafficLight(x.s, kC, 186, 150, lit, glow);
  const float ta = clamp01((a - 0.75f) / 0.5f);
  text(x, Font::Title, "Monav", kC, 318 + (1 - ta) * 8, col::white, textdatum_t::baseline_center, ta);
  text(x, Font::Small, "Display", kC, 346 + (1 - ta) * 8, col::text2, textdatum_t::baseline_center, ta);
}

// --- Overlays --------------------------------------------------------------------------------

void holdOverlay(Ctx& x) {
  const float p = x.f.hold;
  gfx::strokeCircle(x.s, kC, kC, 226, 6, rgb(0x2C, 0x2C, 0x2E));
  if (p > 0.003f) gfx::arc(x.s, kC, kC, 226, 6, 0.0f, 360.0f * p, col::white, col::white);
}

void resetOverlay(Ctx& x) {
  const Frame& f = x.f;
  gfx::darken(x.s, 0, 0, kSize, kSize, 0.0f);
  gfx::fillCircle(x.s, kC, 150, 44, rgb(0x33, 0x2B, 0x06));
  icons::reset(x.s, kC, 150, 26, col::amber);
  text(x, Font::Title, tr(f.lang, Str::ResetQuestion), kC, 256, col::white);
  text(x, Font::Body, tr(f.lang, Str::ResetConfirm), kC, 294, col::text2);
  // The question times out: the ring runs down.
  gfx::strokeCircle(x.s, kC, kC, 224, 5, rgb(0x2C, 0x2C, 0x2E));
  if (f.resetLeft > 0.003f) gfx::arc(x.s, kC, kC, 224, 5, 0.0f, 360.0f * f.resetLeft, col::amber, col::amber);
}

void brightnessOverlay(Ctx& x) {
  const Frame& f = x.f;
  const float a = f.brightnessShow;
  // Fade the screen behind it, then the level as the ring and the number in the middle.
  gfx::darken(x.s, 0, 0, kSize, kSize, 1.0f - a);
  gfx::ringArc(x.s, kC, kC, kRingR, kRingW, kA0, kA1, col::track, col::track, a);
  const float frac = clamp01((f.brightness - 5) / 95.0f);
  gfx::ringArc(x.s, kC, kC, kRingR, kRingW, kA0, kA0 + kSweep * (0.02f + 0.98f * frac), rgb(0x60, 0x60, 0x64),
               col::white, a);
  icons::sun(x.s, kC, 168, 26, gfx::scale(col::white, a));
  char buf[8];
  snprintf(buf, sizeof(buf), "%d", f.brightness);
  const int w = text(x, Font::Num, buf, kC - 8, 268, col::white, textdatum_t::baseline_center, a);
  text(x, Font::Body, "%", kC - 8 + w / 2.0f + 4, 268, col::text2, textdatum_t::baseline_left, a);
  text(x, Font::Body, tr(f.lang, Str::Brightness), kC, 306, col::text2, textdatum_t::baseline_center, a);
}

void toastOverlay(Ctx& x) {
  const Frame& f = x.f;
  chip(x, kChipCy + 4, tr(f.lang, f.toastText), col::white, 16,
       [](Ctx& c, const Frame& fr, float cx, float cy) {
         gfx::fillCircle(c.s, cx, cy, 8.5f, col::green, fr.toast);
         icons::check(c.s, cx, cy, 6, 2.2f, col::black);
       },
       f.toast);
}

/** Everything of one frame that falls into the band x.s. */
void renderBand(Ctx& x) {
  const Frame& f = x.f;
  // Every screen except the HUD fades in.
  const float fade = f.screen == Screen::Drive || f.screen == Screen::Boot ? 1.0f : clamp01(f.screenAge / 0.3f);
  x.s.dim = fade;

  if (f.update.phase != Frame::Update::Phase::None) {
    x.s.dim = 1.0f;
    updateScreen(x);
    return;
  }

  switch (f.screen) {
    case Screen::Boot:
      bootScreen(x);
      break;
    case Screen::Waiting:
      waitingScreen(x);
      break;
    case Screen::Paired:
      pairedScreen(x);
      break;
    case Screen::Denied:
      deniedScreen(x);
      break;
    case Screen::Connected:
      connectedScreen(x);
      break;
    case Screen::Drive:
      driveScreen(x);
      break;
    case Screen::Standby:
      standbyScreen(x);
      break;
  }
  x.s.dim = 1.0f;

  if (f.resetAsk) {
    resetOverlay(x);
  } else if (f.hold > 0) {
    holdOverlay(x);
  }
  if (f.brightnessShow > 0) brightnessOverlay(x);
  if (f.toast > 0 && f.screen != Screen::Drive) toastOverlay(x);
}

uint16_t* g_bands[2] = {nullptr, nullptr};
int g_bandRows = 0;
void (*g_helperRun)(void (*)(void*), void*) = nullptr;
void (*g_helperWait)() = nullptr;

/**
 * One drawing task's part of a frame: it takes the next band not yet taken until none is left, so
 * the two tasks share the work however busy their cores are with other things.
 */
struct LaneJob {
  const Frame* f;
  uint16_t* frame;
  int w, h;
  bool mapReady;
  float mapX, mapY;
  uint8_t lane;
  std::atomic<int>* next;
};

void renderLane(const LaneJob& j) {
  uint16_t* band = g_bands[j.lane];
  for (;;) {
    const int index = j.next->fetch_add(1);
    const int y0 = index * g_bandRows;
    if (y0 >= j.h) break;
    const int rows = j.h - y0 < g_bandRows ? j.h - y0 : g_bandRows;
    Ctx x{gfx::Surface{band, j.w, rows, 1.0f, y0, j.lane}, *j.f};
    x.mapReady = j.mapReady;
    x.mapX = j.mapX;
    x.mapY = j.mapY;
    const uint32_t c0 = j.lane == 0 ? clockUs() : 0;
    gfx::clear(x.s, 0);
    if (j.lane == 0) g_prof.clearUs += clockUs() - c0;
    renderBand(x);
    const uint32_t c1 = j.lane == 0 ? clockUs() : 0;
    memcpy(j.frame + static_cast<size_t>(y0) * j.w, band, static_cast<size_t>(rows) * j.w * 2);
    if (j.lane == 0) g_prof.copyUs += clockUs() - c1;
  }
}

void renderLaneEntry(void* arg) { renderLane(*static_cast<const LaneJob*>(arg)); }

}  // namespace

void setBandBuffer(uint16_t* buffer, int rows) { setBandBuffers(buffer, nullptr, rows); }

void setBandBuffers(uint16_t* lane0, uint16_t* lane1, int rows) {
  g_bands[0] = lane0;
  g_bands[1] = lane1;
  g_bandRows = rows;
}

void setHelper(void (*run)(void (*fn)(void*), void* arg), void (*wait)()) {
  g_helperRun = run;
  g_helperWait = wait;
}

void setProfileClock(uint32_t (*micros)()) { g_clock = micros; }

RenderProfile takeRenderProfile() {
  const RenderProfile p = g_prof;
  g_prof = RenderProfile{};
  return p;
}

void render(lgfx::LGFX_Sprite& canvas, const Frame& f) {
  const uint32_t start = clockUs();
  float mapX = 0, mapY = 0;
  const bool mapReady = f.update.phase == Frame::Update::Phase::None && f.screen == Screen::Drive &&
                        f.layout == Layout::Map && f.map != nullptr && f.hasPose &&
                        carOnImage(*f.map, f.lat, f.lon, mapX, mapY);
  uint16_t* frame = static_cast<uint16_t*>(canvas.getBuffer());
  const int w = canvas.width(), h = canvas.height();

  // What both drawing tasks share is worked out before they start.
  gfx::prepareGlow(168, 226, 233);
  if (mapReady) prepareMap(*f.map);
  if (mapReady && f.route != nullptr) {
    MapCamera cam;
    cam.carX = mapX;
    cam.carY = mapY;
    cam.hdg = f.hdg;
    cam.tilt = f.tilt;
    prepareRoute(*f.map, *f.route, cam);
  }

  if (g_bands[0] == nullptr || g_bandRows <= 0) {
    // No band buffer: straight into the canvas.
    Ctx x{gfx::Surface{frame, w, h, 1.0f, 0, 0}, f};
    x.mapReady = mapReady;
    x.mapX = mapX;
    x.mapY = mapY;
    gfx::clear(x.s, 0);
    renderBand(x);
  } else if (g_bands[1] != nullptr && g_helperRun != nullptr) {
    // Two tasks (one per core) draw the bands through internal RAM, whichever is free next.
    std::atomic<int> next{0};
    LaneJob helper{&f, frame, w, h, mapReady, mapX, mapY, 1, &next};
    LaneJob mine{&f, frame, w, h, mapReady, mapX, mapY, 0, &next};
    g_helperRun(renderLaneEntry, &helper);
    renderLane(mine);
    if (g_helperWait) g_helperWait();
  } else {
    // Band by band through internal RAM: every blend there is cheap, and each finished band goes
    // into the frame in one sequential copy.
    std::atomic<int> next{0};
    LaneJob mine{&f, frame, w, h, mapReady, mapX, mapY, 0, &next};
    renderLane(mine);
  }
  g_prof.totalUs += clockUs() - start;
  g_prof.frames++;
}

}  // namespace monav::ui
