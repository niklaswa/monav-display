#include "ui/render.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "core/app.h"
#include "core/mapdata.h"
#include "core/nav.h"
#include "core/strings.h"
#include "ui/fonts.h"
#include "ui/maprender.h"
#include "ui/glyphs.h"
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
constexpr float kFontPx[] = {150, 196, 80, 44, 30, 52, 38, 28, 21, 30, 26, 23, 18, 104, 13, 44};
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

void chronoDrive(Ctx& x);
void stopwatchDrive(Ctx& x);

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

  if (f.layout == Layout::Clock && f.hasClock && f.theme == Theme::Stopwatch) {
    stopwatchDrive(x);
  } else if (f.layout == Layout::Clock && f.hasClock) {
    chronoDrive(x);
  } else if (mapReady) {
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

// --- Themes: the screens without a drive (0.2.7) -----------------------------------------------

/** The theme as drawn: the photo theme without a picture is the classic one. */
Theme drawnTheme(const Frame& f) {
  return f.theme == Theme::Photo && f.wallpaper == nullptr ? Theme::Classic : f.theme;
}

uint32_t accentOf(Theme theme) {
  switch (theme) {
    case Theme::Aurora:
      return rgb(0x5C, 0xF2, 0xC5);
    case Theme::Orbit:
      return rgb(0xFF, 0x9F, 0x0A);
    case Theme::Retro:
      return rgb(0xFF, 0x4F, 0xA3);
    case Theme::Photo:
      return col::white;
    case Theme::Chrono:
      return rgb(0xE5, 0x26, 0x1C);
    case Theme::Stopwatch:
      return rgb(0xF0, 0xC4, 0x1C);
    default:
      return col::green;
  }
}

/** A stable pseudo-random number in 0..1 for an index (stars, snow, sparks). */
float rnd(uint32_t i) {
  i ^= i >> 16;
  i *= 0x7FEB352Du;
  i ^= i >> 15;
  i *= 0x846CA68Bu;
  i ^= i >> 16;
  return static_cast<float>(i & 0xFFFFFF) / 16777215.0f;
}

/** Hue 0..1 at full saturation and value. */
uint32_t hue(float h) {
  h = h - std::floor(h);
  const float r = clamp01(std::fabs(h * 6 - 3) - 1);
  const float g = clamp01(2 - std::fabs(h * 6 - 2));
  const float b = clamp01(2 - std::fabs(h * 6 - 4));
  return rgb(static_cast<uint8_t>(r * 255), static_cast<uint8_t>(g * 255), static_cast<uint8_t>(b * 255));
}

/** Text with a soft dark shadow under it, for a picture behind. */
void shadowText(Ctx& x, Font font, const char* str, float px, float py, uint32_t color, float alpha = 1.0f) {
  text(x, font, str, px + 1.5f, py + 2.0f, col::black, textdatum_t::baseline_center, 0.55f * alpha);
  text(x, font, str, px, py, color, textdatum_t::baseline_center, alpha);
}

// Time-of-day and calendar moments (only with the surprises on).
bool isChristmas(const Frame& f) { return f.eggs && f.hasClock && f.clock.month == 12 && f.clock.day >= 24 && f.clock.day <= 26; }
bool isNewYear(const Frame& f) { return f.eggs && f.hasClock && f.clock.month == 1 && f.clock.day == 1 && f.clock.hour < 1; }
bool isWishTime(const Frame& f) {
  return f.eggs && f.hasClock && ((f.clock.hour == 11 && f.clock.minute == 11) || (f.clock.hour == 22 && f.clock.minute == 22));
}
bool isLeetTime(const Frame& f) { return f.eggs && f.hasClock && f.clock.hour == 13 && f.clock.minute == 37; }

/** The line under the clock: the screen's own, or what the moment calls for. */
const char* subLine(const Frame& f, Str normal) {
  if (isNewYear(f)) return tr(f.lang, Str::NewYear);
  if (isChristmas(f)) return tr(f.lang, Str::Christmas);
  if (isWishTime(f)) return tr(f.lang, Str::MakeAWish);
  if (isLeetTime(f)) return tr(f.lang, Str::Leet);
  return tr(f.lang, normal);
}

/** The clock's colour: 13:37 runs through the rainbow. */
uint32_t clockColour(const Frame& f, uint32_t normal) {
  return isLeetTime(f) ? gfx::mix(hue(f.t * 0.25f), col::white, 0.25f) : normal;
}

// --- Backgrounds -------------------------------------------------------------------------------

/**
 * Northern lights: curtains of colour over the top of the glass — a sharp lower edge, a long glow
 * upwards, brighter and darker folds that drift. Written straight into the cleared band (it is the
 * first thing drawn), a fall-off table instead of exp() per pixel.
 */
void auroraBackground(Ctx& x, float intro) {
  const Frame& f = x.f;
  constexpr int kTop = 16, kBottom = 236, kBands = 3;
  struct Band {
    float cy, r, w, phase;
    uint32_t c;
  };
  const Band bands[kBands] = {
      {563, 420, 22, 0.0f, rgb(0x2E, 0xF2, 0x9A)},
      {600, 490, 18, 2.3f, rgb(0x2F, 0xB8, 0xE8)},
      {626, 566, 16, 4.1f, rgb(0xB0, 0x4C, 0xFF)},
  };
  const int y0 = std::max(x.s.oy, kTop), y1 = std::min(x.s.oy + x.s.h, kBottom);
  if (y0 < y1) {
    static constexpr int kFall = 64;
    uint8_t fall[kFall];
    for (int i = 0; i < kFall; i++) {
      const float t = i / static_cast<float>(kFall - 1);
      fall[i] = static_cast<uint8_t>(255 * std::exp(-4.0f * t * t) * (1 - t));
    }
    // Per drawing task (the band task's stack is small): the curve in quarter pixels, the folds.
    static int16_t s_centre[2][kBands][kSize];
    static uint8_t s_fold[2][kBands][kSize];
    int16_t (&centre)[kBands][kSize] = s_centre[x.s.lane & 1];
    uint8_t (&fold)[kBands][kSize] = s_fold[x.s.lane & 1];
    const float k = intro * x.s.dim * 0.92f;
    for (int b = 0; b < kBands; b++) {
      const Band& B = bands[b];
      for (int px = 0; px < kSize; px++) {
        const float dx = px - kC;
        const float cy = B.cy - std::sqrt(B.r * B.r - dx * dx) + 10 * std::sin(px * 0.013f + f.t * 0.30f + B.phase) +
                         5 * std::sin(px * 0.031f - f.t * 0.45f + B.phase * 2);
        centre[b][px] = static_cast<int16_t>(cy * 4);
        const float curtain = 0.55f + 0.45f * std::sin(px * 0.071f + f.t * 0.6f + B.phase) *
                                          std::sin(px * 0.023f - f.t * 0.21f + B.phase);
        const float breathe = 0.8f + 0.2f * std::sin(f.t * 0.37f + B.phase * 1.7f);
        fold[b][px] = static_cast<uint8_t>(255 * clamp01(curtain * breathe * k));
      }
    }
    for (int y = y0; y < y1; y++) {
      uint16_t* row = gfx::rowAt(x.s, y);
      for (int px = 0; px < kSize; px++) {
        uint32_t r = 0, g = 0, bl = 0;
        for (int b = 0; b < kBands; b++) {
          const float d = y - centre[b][px] * 0.25f;
          // Long glow above the lower edge, a quick fade below it.
          const float t = d < 0 ? -d / (bands[b].w * 2.6f) : d / (bands[b].w * 0.45f);
          if (t >= 1) continue;
          const uint32_t v = fall[static_cast<int>(t * (kFall - 1))] * fold[b][px];  // 0..65025
          const uint32_t c = bands[b].c;
          r += ((c >> 16) & 0xFF) * v / 65025;
          g += ((c >> 8) & 0xFF) * v / 65025;
          bl += (c & 0xFF) * v / 65025;
        }
        if ((r | g | bl) == 0) continue;
        row[px] = gfx::pack(rgb(r > 255 ? 255 : r, g > 255 ? 255 : g, bl > 255 ? 255 : bl));
      }
    }
  }
  for (int i = 0; i < 28; i++) {
    const float sx = 70 + rnd(i * 3 + 1) * 326, sy = 40 + rnd(i * 3 + 2) * 150;
    if (!gfx::rowsIn(x.s, sy - 3, sy + 3)) continue;
    const float tw = 0.35f + 0.65f * (0.5f + 0.5f * std::sin(f.t * (0.6f + rnd(i) * 1.4f) + i));
    gfx::fillCircle(x.s, sx, sy, 1.1f + rnd(i * 7) * 0.8f, col::white, 0.5f * tw * intro);
  }
}

/** A watch face: minute ticks round the rim, a comet for the seconds. */
void orbitDial(Ctx& x, float intro, bool hands) {
  const Frame& f = x.f;
  const uint32_t accent = accentOf(Theme::Orbit);
  const int shown = static_cast<int>(60 * clamp01(intro) + 0.5f);
  for (int i = 0; i < shown; i++) {
    const float a = i * 6 * kDeg;
    const float sn = std::sin(a), cs = -std::cos(a);
    if (i % 5 == 0) {
      gfx::line(x.s, kC + sn * 196, kC + cs * 196, kC + sn * 212, kC + cs * 212, i % 15 == 0 ? 5.0f : 3.5f,
                i % 15 == 0 ? col::white : col::text2);
    } else {
      gfx::fillCircle(x.s, kC + sn * 207, kC + cs * 207, 1.6f, col::tick);
    }
  }
  if (!f.hasClock) return;
  const float sec = f.clock.second + f.clock.millis / 1000.0f;
  const float sa = sec * 6;
  gfx::arc(x.s, kC, kC, 224, 5, sa - 84, sa, col::black, accent, intro);
  const float sx = kC + std::sin(sa * kDeg) * 224, sy = kC - std::cos(sa * kDeg) * 224;
  gfx::fillCircle(x.s, sx, sy, 12, accent, 0.25f * intro);
  gfx::fillCircle(x.s, sx, sy, 6.5f, accent, intro);
  if (!hands) return;
  const float ha = ((f.clock.hour % 12) + f.clock.minute / 60.0f) * 30 * kDeg;
  const float ma = (f.clock.minute + sec / 60.0f) * 6 * kDeg;
  gfx::line(x.s, kC - std::sin(ha) * 18, kC + std::cos(ha) * 18, kC + std::sin(ha) * 112, kC - std::cos(ha) * 112, 11,
            col::white, intro);
  gfx::line(x.s, kC - std::sin(ma) * 22, kC + std::cos(ma) * 22, kC + std::sin(ma) * 172, kC - std::cos(ma) * 172, 6,
            col::white, intro);
  gfx::fillCircle(x.s, kC, kC, 10, col::white, intro);
  gfx::fillCircle(x.s, kC, kC, 4.5f, accent, intro);
}

constexpr float kHorizon = 318.0f;

/** Synthwave: a striped sun sinking into a grid that runs towards the viewer. */
void retroBackground(Ctx& x, float intro, float horizon = kHorizon) {
  const Frame& f = x.f;
  // Stars.
  for (int i = 0; i < 24; i++) {
    const float sx = 60 + rnd(i * 5 + 11) * 346, sy = 40 + rnd(i * 5 + 12) * (horizon - 120);
    if (!gfx::rowsIn(x.s, sy - 2, sy + 2)) continue;
    gfx::fillCircle(x.s, sx, sy, 1.2f, rgb(0xFF, 0xC8, 0xF0), 0.5f * intro);
  }
  // Haze along the horizon.
  for (int i = 0; i < 6; i++) {
    const float h = 14.0f;
    gfx::fillRoundRect(x.s, 0, horizon - (i + 1) * h, kSize, h, 0, rgb(0x5A, 0x12, 0x5E), (0.30f - i * 0.05f) * intro);
  }
  // The sun: yellow above, magenta below, the lower half cut by widening gaps.
  const float r = 92, cy = horizon - 26 + (1 - intro) * 110;
  const uint32_t top = shade(x.s, rgb(0xFF, 0xD6, 0x0A)), bottom = shade(x.s, rgb(0xFF, 0x2D, 0x8A));
  const int y0 = std::max(x.s.oy, static_cast<int>(cy - r)), y1 = std::min(x.s.oy + x.s.h, static_cast<int>(horizon));
  for (int y = y0; y < y1; y++) {
    const float dy = y + 0.5f - cy;
    if (dy * dy >= r * r) continue;
    if (dy > -12) {
      // Gaps every 13 px, growing from 2 to 8 px towards the horizon.
      const float k = (dy + 12) / (r + 12);
      const float period = 13.0f, gap = 2 + 6 * k;
      if (std::fmod(dy + 12, period) < gap) continue;
    }
    const float half = std::sqrt(r * r - dy * dy);
    const uint32_t c = gfx::mix(top, bottom, clamp01((dy + r) / (2 * r)));
    const float xl = kC - half, xr = kC + half;
    for (int px = std::max(0, static_cast<int>(xl)); px <= std::min(kSize - 1, static_cast<int>(xr)); px++) {
      const float cov = clamp01(std::min(px + 1 - xl, xr - px));
      gfx::blendPixel(x.s, px, y, c, static_cast<int>(cov * 255 * intro));
    }
  }
  // The grid.
  const uint32_t grid = rgb(0xFF, 0x3C, 0xC8);
  gfx::line(x.s, 0, horizon, kSize, horizon, 2.0f, grid, intro);
  const float phase = std::fmod(f.t * 1.4f, 1.0f);
  for (int k = 1; k <= 16; k++) {
    const float d = k - phase;
    if (d < 1.02f) continue;
    const float y = horizon + 150.0f / d;
    if (y > kSize) continue;
    gfx::line(x.s, 0, y, kSize, y, 1.6f, grid, clamp01((y - horizon) / 50.0f) * 0.85f * intro);
  }
  for (int i = -9; i <= 9; i++) {
    gfx::line(x.s, kC + i * 7.0f, horizon, kC + i * 92.0f, horizon + 300, 1.5f, grid, 0.7f * intro);
  }
}

/** The phone's picture, copied row by row (it is darkened already). */
void photoBackground(Ctx& x, float intro) {
  const Wallpaper* w = x.f.wallpaper;
  if (w == nullptr || w->px == nullptr) return;
  const float k = x.s.dim * clamp01(intro);
  const int y0 = std::max(0, x.s.oy), y1 = std::min(Wallpaper::kSize, x.s.oy + x.s.h);
  for (int y = y0; y < y1; y++) {
    uint16_t* dst = gfx::rowAt(x.s, y);
    const uint16_t* src = w->px + static_cast<size_t>(y) * Wallpaper::kSize;
    if (k >= 0.999f) {
      memcpy(dst, src, Wallpaper::kSize * 2);
    } else {
      for (int px = 0; px < Wallpaper::kSize; px++) dst[px] = gfx::pack(gfx::scale(gfx::unpack(src[px]), k));
    }
  }
}

/** Snow (Christmas) and fireworks (New Year's first hour), over any theme. */
void momentOverlay(Ctx& x) {
  const Frame& f = x.f;
  if (isChristmas(f)) {
    for (int i = 0; i < 46; i++) {
      const float speed = 18 + rnd(i * 13 + 1) * 26;
      const float y = std::fmod(rnd(i * 13 + 2) * 520 + f.t * speed, 520.0f) - 30;
      if (!gfx::rowsIn(x.s, y - 4, y + 4)) continue;
      const float sx = rnd(i * 13 + 3) * kSize + 14 * std::sin(f.t * 0.7f + i);
      gfx::fillCircle(x.s, sx, y, 1.4f + rnd(i * 13 + 4) * 1.8f, col::white, 0.75f);
    }
  }
  if (isNewYear(f)) {
    for (int b = 0; b < 4; b++) {
      const float cycle = 2.6f, ph = std::fmod(f.t + b * cycle / 4, cycle) / cycle;
      const int round = static_cast<int>((f.t + b * cycle / 4) / cycle);
      const float bx = 110 + rnd(round * 7 + b) * 246, by = 100 + rnd(round * 7 + b + 3) * 150;
      const uint32_t c = hue(rnd(round * 5 + b));
      const float rad = easeOut(ph) * 78, a = 1 - ph;
      for (int s = 0; s < 18; s++) {
        const float ang = s * (360.0f / 18) * kDeg;
        gfx::fillCircle(x.s, bx + std::sin(ang) * rad * 1.2f, by - std::cos(ang) * rad * 1.2f + ph * ph * 30, 3.4f, c, a);
      }
    }
  }
  if (isWishTime(f)) {
    for (int i = 0; i < 10; i++) {
      const float ang = (i * 36 + f.t * 20) * kDeg, rad = 150 + 12 * std::sin(f.t * 2 + i);
      const float tw = 0.5f + 0.5f * std::sin(f.t * 3.1f + i * 1.3f);
      const float sx = kC + std::sin(ang) * rad, sy = 236 - std::cos(ang) * rad * 0.62f;
      gfx::line(x.s, sx - 5 * tw, sy, sx + 5 * tw, sy, 1.6f, col::amber, tw);
      gfx::line(x.s, sx, sy - 5 * tw, sx, sy + 5 * tw, 1.6f, col::amber, tw);
    }
  }
}

/** "● Verbunden": the status line on top of the connected screen. */
void statusLine(Ctx& x, float y, uint32_t color, bool shadow = false) {
  const Frame& f = x.f;
  const char* label = tr(f.lang, Str::Connected);
  const int tw = width(x, Font::Small, label);
  const float w = tw + 18.0f;
  const float pulse = 0.6f + 0.4f * std::sin(f.t * 2 * kPi * 0.5f);
  if (shadow) text(x, Font::Small, label, kC - w / 2 + 19.5f, y + 2, col::black, textdatum_t::baseline_left, 0.55f);
  gfx::fillCircle(x.s, kC - w / 2 + 5, y - 6, 5, color, pulse);
  text(x, Font::Small, label, kC - w / 2 + 18, y, color, textdatum_t::baseline_left);
}

void clockText(Ctx& x, float baseline, uint32_t color) {
  char buf[8];
  snprintf(buf, sizeof(buf), "%02d:%02d", x.f.clock.hour, x.f.clock.minute);
  text(x, Font::Clock, buf, kC, baseline, clockColour(x.f, color));
}

/** A small pill at the foot of the glass, for a sub line over a busy background. */
void footPill(Ctx& x, float cy, const char* label, uint32_t color, float alpha = 1.0f, float backing = 0.72f) {
  const int tw = width(x, Font::Small, label);
  const float w = tw + 30.0f, h = 34.0f;
  gfx::fillRoundRect(x.s, kC - w / 2, cy - h / 2, w, h, h / 2, col::black, backing * alpha);
  text(x, Font::Small, label, kC, cy + capHeight(Font::Small) / 2, color, textdatum_t::baseline_center, alpha);
}


// --- Chrono: a sports car's dashboard chronograph -------------------------------------------------

namespace chrono {
constexpr uint32_t dial = rgb(0x0A, 0x0A, 0x0B);
constexpr uint32_t bezel = rgb(0x2E, 0x2E, 0x31);
constexpr uint32_t track = rgb(0x8A, 0x8A, 0x8F);
constexpr uint32_t red = rgb(0xE5, 0x26, 0x1C);
constexpr uint32_t lume = rgb(0xF2, 0xF0, 0xE6);
constexpr float kTopY = 146;     // small dial at 12: the date, or the limit while driving
constexpr float kTopR = 42;
constexpr float kLowY = 326;     // small dial at 6: running seconds, or the speed while driving
constexpr float kLowR = 56;
}  // namespace chrono

/** A hand from `back` behind the centre to `len`, at `deg` (0 = 12 o'clock). */
void hand(Ctx& x, float deg, float back, float len, float w, uint32_t c, float alpha = 1.0f) {
  const float a = deg * kDeg, sn = std::sin(a), cs = -std::cos(a);
  gfx::line(x.s, kC - sn * back, kC - cs * back, kC + sn * len, kC + cs * len, w, c, alpha);
}

/**
 * The face: a black dial in a thin bezel, a seconds scale 5…60, white batons, two small dials.
 * `intro` (0..1) lets the welcome draw it in.
 */
void chronoDial(Ctx& x, float intro, bool smallDials = true) {
  const Frame& f = x.f;
  gfx::fillCircle(x.s, kC, kC, 229, chrono::dial, intro);
  gfx::strokeCircle(x.s, kC, kC, 229, 3, chrono::bezel, intro);
  const int shown = static_cast<int>(60 * clamp01(intro) + 0.5f);
  for (int i = 0; i < shown; i++) {
    const float a = i * 6 * kDeg, sn = std::sin(a), cs = -std::cos(a);
    if (i % 5 == 0) {
      // Batons; a double one at 12, none at 6 (the small dial).
      if (i == 30) continue;
      if (i == 0) {
        for (float off : {-7.0f, 7.0f}) {
          gfx::line(x.s, kC + off, kC - 216, kC + off, kC - 190, 7, chrono::lume);
        }
      } else {
        gfx::line(x.s, kC + sn * 190, kC + cs * 190, kC + sn * 216, kC + cs * 216, 8, chrono::lume);
      }
      char num[4];
      snprintf(num, sizeof(num), "%d", i == 0 ? 60 : i);
      if (i != 0 && i != 30) {
        const float r = 168;
        text(x, Font::Small, num, kC + sn * r, kC + cs * r + capHeight(Font::Small) / 2, chrono::track);
      }
    } else {
      gfx::line(x.s, kC + sn * 206, kC + cs * 206, kC + sn * 217, kC + cs * 217, 2, chrono::track);
    }
  }
  (void)f;
  if (!smallDials) return;
  gfx::strokeCircle(x.s, kC, chrono::kTopY, chrono::kTopR, 2, chrono::bezel, intro);
  gfx::strokeCircle(x.s, kC, chrono::kLowY, chrono::kLowR, 2, chrono::bezel, intro);
}

/** Hour and minute hands with a lume inlay, the red sweeping seconds hand on top. */
void chronoHands(Ctx& x, float alpha, float sweep = 1.0f) {
  const Frame& f = x.f;
  if (!f.hasClock) return;
  // The welcome sweeps the hands from 12 to the time, as a dashboard sweeps its needles.
  const float k = easeOut(sweep);
  const float sec = (f.clock.second + f.clock.millis / 1000.0f) * k;
  const float minute = (f.clock.minute + f.clock.second / 60.0f) * k;
  const float hour = ((f.clock.hour % 12) + f.clock.minute / 60.0f) * k;
  hand(x, hour * 30, 22, 120, 14, chrono::lume, alpha);
  hand(x, hour * 30, -36, 110, 5, rgb(0x2A, 0x2A, 0x2C), alpha);
  hand(x, minute * 6, 22, 190, 10, chrono::lume, alpha);
  hand(x, minute * 6, -36, 180, 3.5f, rgb(0x2A, 0x2A, 0x2C), alpha);
  hand(x, sec * 6, 46, 212, 3, chrono::red, alpha);
  const float a = sec * 6 * kDeg;
  gfx::fillCircle(x.s, kC - std::sin(a) * 40, kC + std::cos(a) * 40, 7, chrono::red, alpha);
  gfx::fillCircle(x.s, kC, kC, 11, chrono::red, alpha);
  gfx::fillCircle(x.s, kC, kC, 4, chrono::dial, alpha);
}

/** Without a drive: the date at 12, running seconds at 6. */
void chronoIdle(Ctx& x, float intro) {
  const Frame& f = x.f;
  chronoDial(x, intro);
  if (f.hasClock) {
    char date[8];
    snprintf(date, sizeof(date), "%02d.%02d", f.clock.day, f.clock.month);
    text(x, Font::NumSm, date, kC, chrono::kTopY + capHeight(Font::NumSm) / 2, chrono::lume,
         textdatum_t::baseline_center, intro);
    for (int i = 0; i < 12; i++) {
      const float a = i * 30 * kDeg;
      const float r0 = i % 3 == 0 ? chrono::kLowR - 14 : chrono::kLowR - 8;
      gfx::line(x.s, kC + std::sin(a) * r0, chrono::kLowY - std::cos(a) * r0, kC + std::sin(a) * (chrono::kLowR - 3),
                chrono::kLowY - std::cos(a) * (chrono::kLowR - 3), 2, chrono::track, intro);
    }
    // The small seconds step once a second, like a mechanical movement's.
    const float a = f.clock.second * 6 * kDeg;
    gfx::line(x.s, kC, chrono::kLowY, kC + std::sin(a) * (chrono::kLowR - 10), chrono::kLowY - std::cos(a) * (chrono::kLowR - 10),
              2.5f, chrono::lume, intro);
    gfx::fillCircle(x.s, kC, chrono::kLowY, 4, chrono::lume, intro);
  }
}

/** The clock layout during a drive: the limit at 12, the speed at 6, the warnings as a chip. */
void chronoDrive(Ctx& x) {
  const Frame& f = x.f;
  if (f.alert > 0 && !f.linkLost) {
    const float pulse = 0.5f + 0.5f * std::sin(f.t * 2 * kPi * 1.4f);
    gfx::rimGlow(x.s, kC, kC, 168, 226, 233, col::red, 0.18f + 0.42f * pulse);
  }
  chronoDial(x, 1.0f);
  chronoHands(x, 1.0f);
  // Speed and limit always on top: the small dials cover the hands.
  gfx::fillCircle(x.s, kC, chrono::kTopY, chrono::kTopR - 1, chrono::dial);
  gfx::strokeCircle(x.s, kC, chrono::kTopY, chrono::kTopR, 2, chrono::bezel);
  gfx::fillCircle(x.s, kC, chrono::kLowY, chrono::kLowR - 1, chrono::dial);
  gfx::strokeCircle(x.s, kC, chrono::kLowY, chrono::kLowR, 2, chrono::bezel);
  if (f.limit > 0) {
    limitSign(x, kC, chrono::kTopY, chrono::kTopR - 6, f.limit);
  } else {
    text(x, Font::NumSm, "–", kC, chrono::kTopY + capHeight(Font::NumSm) / 2, chrono::track);
  }
  char num[8];
  snprintf(num, sizeof(num), "%ld", std::lround(f.speed < 0 ? 0 : f.speed));
  const uint32_t status = speedStatus(f.speed, f.limit);
  text(x, Font::Num, num, kC, chrono::kLowY + 10, status == col::green ? chrono::lume : status);
  text(x, Font::Tiny, tr(f.lang, Str::Kmh), kC, chrono::kLowY + 32, chrono::track);
  if (!f.linkLost && f.primary != Card::None) secondaryChip(x, f, f.primary);
}


// --- Stopwatch: a white seconds ring round a black centre, yellow hands -----------------------------

namespace stopwatch {
constexpr uint32_t ring = rgb(0xF1, 0xF1, 0xEE);
constexpr uint32_t groove = rgb(0xB4, 0xB4, 0xB2);
constexpr uint32_t ink = rgb(0x22, 0x22, 0x24);
constexpr uint32_t centre = rgb(0x0C, 0x0C, 0x0D);
constexpr uint32_t yellow = rgb(0xF0, 0xC4, 0x1C);
constexpr uint32_t steel = rgb(0x9C, 0x9C, 0xA0);
constexpr float kRingIn = 122, kRingOut = 186;
}  // namespace stopwatch

void stopwatchDial(Ctx& x, float intro) {
  using namespace stopwatch;
  gfx::fillCircle(x.s, kC, kC, 229, rgb(0x08, 0x08, 0x09), intro);
  gfx::strokeCircle(x.s, kC, kC, 229, 4, rgb(0x26, 0x26, 0x28), intro);
  // The white ring with its grooves.
  gfx::strokeCircle(x.s, kC, kC, (kRingIn + kRingOut) / 2, kRingOut - kRingIn, ring, intro);
  for (int i = 0; i < 9; i++) gfx::strokeCircle(x.s, kC, kC, kRingIn + 8 + i * 6.5f, 1.0f, groove, intro);
  // The black centre, a little relief at its edge.
  gfx::fillCircle(x.s, kC, kC, kRingIn, centre, intro);
  gfx::strokeCircle(x.s, kC, kC, kRingIn - 4, 6, rgb(0x16, 0x16, 0x18), intro);
  const int shown = static_cast<int>(60 * clamp01(intro) + 0.5f);
  for (int i = 0; i < shown; i++) {
    const float a = i * 6 * kDeg, sn = std::sin(a), cs = -std::cos(a);
    if (i % 5 == 0) {
      // Quarters in steel, the other fives in yellow, from the ring out to the rim.
      const bool quarter = i % 15 == 0;
      gfx::line(x.s, kC + sn * (kRingOut + 2), kC + cs * (kRingOut + 2), kC + sn * 218, kC + cs * 218,
                quarter ? 5.0f : 4.5f, quarter ? steel : yellow);
      // The numeral on a patch of white, so the grooves stop round it.
      char num[4];
      snprintf(num, sizeof(num), "%d", i == 0 ? 60 : i);
      const float r = (kRingIn + kRingOut) / 2, nx = kC + sn * r, ny = kC + cs * r;
      const int w = width(x, Font::NumSm, num);
      gfx::fillRoundRect(x.s, nx - w / 2.0f - 3, ny - 15, w + 6, 30, 4, ring);
      text(x, Font::NumSm, num, nx, ny + capHeight(Font::NumSm) / 2, ink);
    } else {
      gfx::line(x.s, kC + sn * 198, kC + cs * 198, kC + sn * 208, kC + cs * 208, 3, rgb(0xE8, 0xE8, 0xE8));
    }
  }
}

/** A yellow hand with a white inlay and a pointed tip. */
void stopwatchHand(Ctx& x, float deg, float tail, float len, float w, float alpha) {
  using namespace stopwatch;
  const float a = deg * kDeg, sn = std::sin(a), cs = -std::cos(a);
  const float tipBase = len - w * 1.2f;
  gfx::line(x.s, kC - sn * tail, kC - cs * tail, kC + sn * tipBase, kC + cs * tipBase, w, yellow, alpha);
  const float px = -cs, py = sn;  // across the hand
  const float tip[] = {kC + sn * tipBase + px * w / 2, kC + cs * tipBase + py * w / 2, kC + sn * len, kC + cs * len,
                       kC + sn * tipBase - px * w / 2, kC + cs * tipBase - py * w / 2};
  gfx::convex(x.s, tip, 3, yellow, alpha);
  gfx::line(x.s, kC + sn * 34, kC + cs * 34, kC + sn * (tipBase - 8), kC + cs * (tipBase - 8), w * 0.32f, col::white,
            alpha);
}

void stopwatchHands(Ctx& x, float alpha, float sweep = 1.0f) {
  using namespace stopwatch;
  const Frame& f = x.f;
  if (!f.hasClock) return;
  const float k = easeOut(sweep);
  const float sec = (f.clock.second + f.clock.millis / 1000.0f) * k;
  const float minute = (f.clock.minute + f.clock.second / 60.0f) * k;
  const float hour = ((f.clock.hour % 12) + f.clock.minute / 60.0f) * k;
  stopwatchHand(x, hour * 30, 26, 118, 15, alpha);
  stopwatchHand(x, minute * 6, 30, 176, 12, alpha);
  hand(x, sec * 6, 42, 206, 3, yellow, alpha);
  // The steel hub.
  gfx::fillCircle(x.s, kC, kC, 21, rgb(0x6E, 0x6E, 0x72), alpha);
  gfx::fillCircle(x.s, kC, kC, 18, steel, alpha);
  gfx::fillCircle(x.s, kC - 3, kC - 3, 10, rgb(0xC9, 0xC9, 0xCD), alpha);
  gfx::fillCircle(x.s, kC, kC, 4, rgb(0x5A, 0x5A, 0x5E), alpha);
}

/** The clock layout in the stopwatch's look: limit and speed in the black centre, over the hands. */
void stopwatchDrive(Ctx& x) {
  const Frame& f = x.f;
  if (f.alert > 0 && !f.linkLost) {
    const float pulse = 0.5f + 0.5f * std::sin(f.t * 2 * kPi * 1.4f);
    gfx::rimGlow(x.s, kC, kC, 168, 226, 233, col::red, 0.18f + 0.42f * pulse);
  }
  stopwatchDial(x, 1.0f);
  stopwatchHands(x, 1.0f);
  if (f.limit > 0) limitSign(x, kC, 172, 32, f.limit);
  char num[8];
  snprintf(num, sizeof(num), "%ld", std::lround(f.speed < 0 ? 0 : f.speed));
  const uint32_t status = speedStatus(f.speed, f.limit);
  const int w = width(x, Font::Num, num);
  gfx::fillRoundRect(x.s, kC - w / 2.0f - 14, 262, w + 28, 62, 16, stopwatch::centre, 0.92f);
  text(x, Font::Num, num, kC, 302, status == col::green ? col::white : status);
  text(x, Font::Tiny, tr(f.lang, Str::Kmh), kC, 318, col::text2);
  if (!f.linkLost && f.primary != Card::None) secondaryChip(x, f, f.primary);
}


// --- Charging ----------------------------------------------------------------------------------

/**
 * The car at the charger: the ring fills up to the battery's level as the percentage counts up,
 * energy runs along the filled part towards its glowing head, power and range below.
 */
void chargingScreen(Ctx& x) {
  const Frame& f = x.f;
  const float a = f.screenAge;
  const float soc = f.soc < 0 ? 0 : f.soc;
  // Fills in on arrival: the number counts along with the ring.
  const float shown = soc * easeOut(clamp01((a - 0.2f) / 1.8f));
  const uint32_t accent = soc < 20 ? col::amber : col::green;
  constexpr float r = 212, w = 18;
  gfx::strokeCircle(x.s, kC, kC, r, w, col::track);
  const float sweep = 360.0f * shown / 100.0f;
  if (sweep > 0.5f) {
    gfx::arc(x.s, kC, kC, r, w, 0, sweep, gfx::mix(col::black, accent, 0.35f), accent);
    // Energy flowing in: bright dashes riding the filled arc towards the head.
    for (int i = 0; i < 6; i++) {
      const float ph = std::fmod(f.t * 0.45f + i / 6.0f, 1.0f);
      const float at = sweep * ph;
      const float len = std::min(16.0f, sweep * 0.12f);
      if (at - len < 0) continue;
      gfx::arc(x.s, kC, kC, r, w * 0.38f, at - len, at, accent, col::white, 0.35f + 0.5f * ph);
    }
    // The head glows and breathes.
    const float head = sweep * kDeg, pulse = 0.5f + 0.5f * std::sin(f.t * 2 * kPi * 0.7f);
    const float hx = kC + std::sin(head) * r, hy = kC - std::cos(head) * r;
    gfx::fillCircle(x.s, hx, hy, 18 + 6 * pulse, accent, 0.18f + 0.12f * pulse);
    gfx::fillCircle(x.s, hx, hy, w * 0.5f + 1, col::white, 0.9f);
  }

  // "⚡ Lädt" on top.
  const char* label = tr(f.lang, Str::Charging);
  const int tw = width(x, Font::Small, label);
  const float lw = tw + 22.0f;
  const float pulse = 0.65f + 0.35f * std::sin(f.t * 2 * kPi * 0.7f);
  icons::bolt(x.s, kC - lw / 2 + 6, 134, 9, gfx::scale(accent, pulse));
  text(x, Font::Small, label, kC - lw / 2 + 22, 140, accent, textdatum_t::baseline_left);

  char num[8];
  snprintf(num, sizeof(num), "%d", static_cast<int>(shown + 0.5f));
  const int nw = width(x, Font::Speed, num);
  text(x, Font::Speed, num, kC - 14, 282, col::white);
  text(x, Font::Title, "%", kC - 14 + nw / 2.0f + 6, 282, col::text2, textdatum_t::baseline_left);

  char line[48] = "";
  char kw[16] = "", km[16] = "";
  if (f.chargeKw >= 0.5f) snprintf(kw, sizeof(kw), f.chargeKw >= 10 ? "%.0f kW" : "%.1f kW", f.chargeKw);
  if (f.rangeKm >= 0) snprintf(km, sizeof(km), "%d km", static_cast<int>(f.rangeKm + 0.5f));
  snprintf(line, sizeof(line), "%s%s%s", kw, kw[0] && km[0] ? " · " : "", km);
  if (f.lang == Lang::De) {
    for (char* c = line; *c; c++) if (*c == '.' && c[1] >= '0' && c[1] <= '9') *c = ',';
  }
  const float ta = clamp01((a - 0.6f) / 0.5f);
  text(x, Font::Body, line, kC, 330, col::text2, textdatum_t::baseline_center, ta);
}


// --- Performance intro: a jump to hyperspace, the greeting out of its flash ------------------------

void introScreen(Ctx& x) {
  const Frame& f = x.f;
  const float a = f.screenAge;  // 0 … App::kIntroMs
  // A lavender glow blooms in from the rim, the middle stays dark: the tunnel.
  const float bloom = clamp01((a - 0.35f) / 0.8f);
  if (bloom > 0) {
    uint16_t lut[256];
    const uint32_t lavender = rgb(0x70, 0x7A, 0xF4), deep = rgb(0x14, 0x16, 0x4E);
    for (int i = 0; i < 256; i++) {
      const float r = std::sqrt(i / 255.0f);  // 0 middle … 1 rim
      const float k = clamp01((r - 0.16f) / 0.84f);
      const float s = k * k * (3 - 2 * k);
      lut[i] = gfx::pack(gfx::scale(gfx::mix(deep, lavender, s), s * bloom * x.s.dim));
    }
    constexpr float kR2 = 233.0f * 233.0f;
    for (int y = x.s.oy; y < x.s.oy + x.s.h; y++) {
      uint16_t* row = gfx::rowAt(x.s, y);
      const float dy = y + 0.5f - kC;
      for (int px = 0; px < kSize; px++) {
        const float dx = px + 0.5f - kC;
        const int idx = static_cast<int>((dx * dx + dy * dy) * (255.0f / kR2));
        row[px] = lut[idx > 255 ? 255 : idx];
      }
    }
  }
  // Stars that stretch into streaks as the jump speeds up.
  const float warp = clamp01((a - 0.55f) / 1.0f);
  const float appear = clamp01(a / 0.4f);
  for (int i = 0; i < 220; i++) {
    const float ang = rnd(i * 3 + 1) * 360 * kDeg;
    const float ph = std::fmod(rnd(i * 3 + 2) + a * (0.12f + 1.5f * warp * warp) * (0.7f + 0.6f * rnd(i * 3 + 3)), 1.0f);
    const float r0 = 34 + ph * ph * 240;
    const float len = 1.5f + warp * (8 + ph * ph * 130);
    const float sn = std::sin(ang), cs = -std::cos(ang);
    const float y0 = kC + cs * r0, y1 = kC + cs * (r0 + len);
    if (!gfx::rowsIn(x.s, std::min(y0, y1) - 3, std::max(y0, y1) + 3)) continue;
    const uint32_t c = gfx::mix(rgb(0xB8, 0xC0, 0xFF), col::white, ph);
    gfx::line(x.s, kC + sn * r0, y0, kC + sn * (r0 + len), y1, 1.1f + ph * 1.9f, c, clamp01(ph * 3) * appear);
  }
  // The flash: white from the middle until it fills the glass as the intro ends.
  const float fl = clamp01((a - 2.25f) / 0.75f);
  if (fl > 0) {
    // A soft white glow growing out of the middle: full inside 40 % of its radius, fading to its
    // edge; by the end it covers the glass.
    const float R = 60 + fl * fl * 560;
    const float peak = clamp01(0.4f + fl);
    uint8_t alpha[256];
    for (int i = 0; i < 256; i++) {
      const float r = std::sqrt(i / 255.0f);  // of R
      const float k = clamp01((1.0f - r) / 0.6f);
      alpha[i] = static_cast<uint8_t>(255 * peak * k * k * (3 - 2 * k));
    }
    const uint32_t white = shade(x.s, col::white);
    const float inv = 255.0f / (R * R);
    const int y0 = std::max(x.s.oy, static_cast<int>(kC - R)), y1 = std::min(x.s.oy + x.s.h, static_cast<int>(kC + R) + 1);
    for (int y = y0; y < y1; y++) {
      uint16_t* row = gfx::rowAt(x.s, y);
      const float dy = y + 0.5f - kC;
      for (int px = 0; px < kSize; px++) {
        const float dx = px + 0.5f - kC;
        const float d2 = (dx * dx + dy * dy) * inv;
        if (d2 >= 255) continue;
        const int a = alpha[static_cast<int>(d2)];
        if (a == 0) continue;
        row[px] = gfx::pack(gfx::mix(gfx::unpack(row[px]), white, a / 255.0f));
      }
    }
  }
}

/** Connected and standby share a layout per theme; `connected` adds the status line. */
void idleScreen(Ctx& x, bool connected) {
  const Frame& f = x.f;
  const Theme theme = drawnTheme(f);
  const Str sub = connected ? Str::ConnectedSub : Str::Bye;
  const uint32_t clockCol = connected ? col::white : rgb(0xD8, 0xD8, 0xDC);
  switch (theme) {
    case Theme::Aurora:
      auroraBackground(x, connected ? 1.0f : 0.6f);
      momentOverlay(x);
      if (connected) statusLine(x, 140, accentOf(theme));
      if (f.hasClock) clockText(x, 270, clockCol);
      text(x, Font::Body, subLine(f, sub), kC, 318, col::text2);
      break;
    case Theme::Orbit:
      momentOverlay(x);
      orbitDial(x, 1.0f, true);
      // Over the hands, on a dark pill: a watch's hands may cross the texts, never hide them.
      if (connected) {
        footPill(x, 146, tr(f.lang, Str::Connected), accentOf(theme), 1.0f, 0.94f);
      }
      footPill(x, 330, subLine(f, sub), col::text2, 1.0f, 0.94f);
      break;
    case Theme::Retro:
      retroBackground(x, connected ? 1.0f : 0.65f);
      momentOverlay(x);
      if (connected) statusLine(x, 92, accentOf(theme));
      if (f.hasClock) clockText(x, 196, connected ? rgb(0x8C, 0xF6, 0xFF) : rgb(0x6A, 0xC8, 0xD8));
      footPill(x, 410, subLine(f, sub), col::white);
      break;
    case Theme::Chrono:
      chronoIdle(x, 1.0f);
      momentOverlay(x);
      chronoHands(x, 1.0f);
      break;
    case Theme::Stopwatch:
      stopwatchDial(x, 1.0f);
      momentOverlay(x);
      stopwatchHands(x, 1.0f);
      break;
    case Theme::Photo:
      photoBackground(x, connected ? 1.0f : 0.7f);
      momentOverlay(x);
      if (connected) statusLine(x, 140, col::white, true);
      if (f.hasClock) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%02d:%02d", f.clock.hour, f.clock.minute);
        shadowText(x, Font::Clock, buf, kC, 270, clockColour(f, col::white));
      }
      shadowText(x, Font::Body, subLine(f, sub), kC, 318, rgb(0xE6, 0xE6, 0xEA));
      break;
    default:
      quietRing(x);
      momentOverlay(x);
      if (connected) {
        statusLine(x, 140, col::green);
        if (f.hasClock) {
          clockText(x, 270, clockCol);
        } else {
          gfx::fillCircle(x.s, kC, 220, 40, col::green);
          icons::check(x.s, kC, 220, 22, 7.0f, col::black);
        }
        text(x, Font::Body, subLine(f, sub), kC, 318, col::text2);
      } else {
        if (f.hasClock) clockText(x, 262, clockCol);
        text(x, Font::Body, subLine(f, sub), kC, 312, col::text2);
      }
      break;
  }
}

void connectedScreen(Ctx& x) { idleScreen(x, true); }

void standbyScreen(Ctx& x) { idleScreen(x, false); }

/** Getting into the car: a greeting by the time of day (and name), in the theme's look. */
void welcomeScreen(Ctx& x) {
  const Frame& f = x.f;
  const Theme theme = drawnTheme(f);
  const uint32_t accent = accentOf(theme);
  const float a = f.screenAge;
  const float intro = easeOut(a / 1.1f);

  float base = 0;  // shifts the text block per theme
  switch (theme) {
    case Theme::Aurora:
      auroraBackground(x, intro);
      break;
    case Theme::Orbit:
      orbitDial(x, intro, false);
      break;
    case Theme::Retro:
      retroBackground(x, intro, 352);
      base = -78;
      break;
    case Theme::Photo:
      photoBackground(x, intro);
      break;
    case Theme::Chrono:
      chronoDial(x, intro, false);
      chronoHands(x, intro, clamp01((a - 0.2f) / 1.3f));
      break;
    case Theme::Stopwatch:
      stopwatchDial(x, intro);
      stopwatchHands(x, intro, clamp01((a - 0.2f) / 1.3f));
      break;
    default: {
      // The ring sweeps round in green, then settles to the quiet one.
      quietRing(x);
      const float sweep = easeOut(a / 0.9f);
      const float fadeRing = 1 - clamp01((a - 1.4f) / 0.8f);
      if (sweep > 0.01f && fadeRing > 0) {
        gfx::ringArc(x.s, kC, kC, kRingR, kRingW, kA0, kA0 + kSweep * sweep, gfx::mix(col::black, accent, 0.3f), accent,
                     fadeRing);
      }
      break;
    }
  }
  momentOverlay(x);

  int hour = f.hasClock ? f.clock.hour : 12;
  const Str greet = hour >= 4 && hour < 11 ? Str::GoodMorning : hour >= 11 && hour < 18 ? Str::GoodDay : Str::GoodEvening;
  const bool photo = theme == Theme::Photo;
  auto draw = [&](Font font, const char* s, float y, uint32_t c, float delay) {
    const float t = clamp01((a - delay) / 0.45f);
    if (t <= 0) return;
    const float yy = y + (1 - easeOut(t)) * 14;
    if (photo) {
      shadowText(x, font, s, kC, yy, c, t);
    } else {
      text(x, font, s, kC, yy, c, textdatum_t::baseline_center, t);
    }
  };

  if (f.hasClock && theme != Theme::Retro && theme != Theme::Chrono && theme != Theme::Stopwatch) {
    char clock[8];
    snprintf(clock, sizeof(clock), "%02d:%02d", f.clock.hour, f.clock.minute);
    draw(Font::NumSm, clock, 132, photo ? col::white : col::text2, 0.1f);
  }
  char line[48];
  if (theme == Theme::Stopwatch) {
    // Inside the black centre, over the hands.
    gfx::fillCircle(x.s, kC, kC + 6, 92, stopwatch::centre, 0.85f * clamp01((a - 0.9f) / 0.4f));
    draw(Font::Title, tr(f.lang, greet), f.name[0] ? 222 : 240, col::white, 1.0f);
    if (f.name[0]) {
      char name[32];
      fit(x, Font::Title, f.name, 180, name, sizeof(name));
      draw(Font::Title, name, 258, accent, 1.15f);
    }
    draw(Font::Small, tr(f.lang, Str::HaveAGoodDrive), f.name[0] ? 290 : 274, col::text2, 1.3f);
    return;
  }
  if (theme == Theme::Chrono) {
    // A window in the lower half of the dial, like a date window: the greeting sits on its own
    // black plate between the 40 and the 20, so the sweeping hands pass under it, not through it
    // (0.2.12; before, the hands crossed the big letters).
    const bool named = f.name[0] != 0;
    const float top = named ? 252 : 262, h = named ? 108 : 92, w = 248;
    const float plate = clamp01((a - 0.15f) / 0.4f);
    if (plate > 0) {
      gfx::fillRoundRect(x.s, kC - w / 2, top, w, h, 22, chrono::dial, plate);
      gfx::strokeRoundRect(x.s, kC - w / 2, top, w, h, 22, 2, chrono::bezel, plate);
    }
    if (named) {
      snprintf(line, sizeof(line), "%s,", tr(f.lang, greet));
      draw(Font::Title, line, 286, chrono::lume, 0.25f);
      char name[32];
      fit(x, Font::Title, f.name, 220, name, sizeof(name));
      draw(Font::Title, name, 318, chrono::red, 0.45f);
      draw(Font::Small, tr(f.lang, Str::HaveAGoodDrive), 346, chrono::track, 0.7f);
    } else {
      draw(Font::Title, tr(f.lang, greet), 298, chrono::lume, 0.25f);
      draw(Font::Small, tr(f.lang, Str::HaveAGoodDrive), 332, chrono::track, 0.7f);
    }
    return;
  }
  if (f.name[0] != 0) {
    snprintf(line, sizeof(line), "%s,", tr(f.lang, greet));
    draw(Font::Hero, line, 232 + base, col::white, 0.25f);
    char name[32];
    fit(x, Font::Hero, f.name, 340, name, sizeof(name));
    draw(Font::Hero, name, 284 + base, accent, 0.45f);
  } else {
    draw(Font::Hero, tr(f.lang, greet), 258 + base, col::white, 0.25f);
  }
  const float subY = theme == Theme::Retro ? 0 : (f.name[0] != 0 ? 338 : 310);
  if (theme == Theme::Retro) {
    const float t = clamp01((a - 0.7f) / 0.45f);
    if (t > 0) footPill(x, 418, tr(f.lang, Str::HaveAGoodDrive), col::white, t);
  } else {
    draw(Font::Body, tr(f.lang, Str::HaveAGoodDrive), subY, photo ? rgb(0xE6, 0xE6, 0xEA) : col::text2, 0.7f);
  }
}

// --- Surprises over the screen ---------------------------------------------------------------

/** Light speed: the glass goes dark, stars streak out of the middle. */
void warpOverlay(Ctx& x) {
  const Frame& f = x.f;
  const float a = f.eggAge;
  const float in = clamp01(a / 0.5f), out = clamp01((5.2f - a) / 0.6f);
  const float k = in * out;
  gfx::darken(x.s, 0, 0, kSize, kSize, 1 - 0.9f * k);
  const float speed = 0.25f + 1.6f * easeOut(clamp01(a / 2.2f));
  for (int i = 0; i < 90; i++) {
    const float ang = rnd(i * 2 + 1) * 360 * kDeg;
    const float ph = std::fmod(rnd(i * 2 + 2) + a * speed * (0.6f + rnd(i) * 0.8f), 1.0f);
    const float r0 = 12 + ph * ph * 250;
    const float len = 6 + ph * ph * 90 * speed;
    const float sn = std::sin(ang), cs = -std::cos(ang);
    const float y0 = kC + cs * r0, y1 = kC + cs * (r0 + len);
    if (!gfx::rowsIn(x.s, std::min(y0, y1) - 2, std::max(y0, y1) + 2)) continue;
    const uint32_t c = gfx::mix(rgb(0x9C, 0xC8, 0xFF), col::white, ph);
    gfx::line(x.s, kC + sn * r0, y0, kC + sn * (r0 + len), y1, 1.2f + ph * 1.8f, c, clamp01(ph * 2.5f) * k);
  }
  const float ta = clamp01((a - 0.9f) / 0.5f) * out;
  if (ta > 0) {
    text(x, Font::Title, tr(f.lang, Str::Warp), kC, 246, col::white, textdatum_t::baseline_center, ta);
    text(x, Font::Small, tr(f.lang, Str::WarpSub), kC, 280, rgb(0x9C, 0xC8, 0xFF), textdatum_t::baseline_center, ta);
  }
}

/** 88 mph: lightning round the rim, a blue glow, and the time jump named at the foot. */
void timeTravelOverlay(Ctx& x) {
  const Frame& f = x.f;
  const float a = f.eggAge;
  const float k = clamp01(a / 0.2f) * clamp01((4.2f - a) / 0.5f);
  // Lightning: jagged lines from the rim inwards, a new set every 90 ms.
  const int round = static_cast<int>(a / 0.09f);
  for (int b = 0; b < 3; b++) {
    if (rnd(round * 11 + b) < 0.35f) continue;
    const float ang = rnd(round * 11 + b + 5) * 360;
    float pts[16];
    float r = 230;
    for (int p = 0; p < 8; p++) {
      const float aa = (ang + (rnd(round * 31 + b * 8 + p) - 0.5f) * 10) * kDeg;
      pts[p * 2] = kC + std::sin(aa) * r;
      pts[p * 2 + 1] = kC - std::cos(aa) * r;
      r -= 9 + rnd(round * 17 + p) * 7;
    }
    gfx::polyline(x.s, pts, 8, 5.0f, rgb(0x4A, 0x8C, 0xFF), 0.35f * k);
    gfx::polyline(x.s, pts, 8, 1.8f, col::white, k);
  }
  const int tw = width(x, Font::Small, tr(f.lang, Str::TimeTravel));
  const float w = tw + 30.0f;
  gfx::fillRoundRect(x.s, kC - w / 2, 428 - 16, w, 32, 16, rgb(0x0A, 0x2A, 0x6A), 0.9f * k);
  text(x, Font::Small, tr(f.lang, Str::TimeTravel), kC, 428 + capHeight(Font::Small) / 2, col::white,
       textdatum_t::baseline_center, k);
}

/** 42 km/h: the answer, with a few sparks. */
void answerOverlay(Ctx& x) {
  const Frame& f = x.f;
  const float a = f.eggAge;
  const float k = clamp01(a / 0.3f) * clamp01((4.5f - a) / 0.5f);
  for (int i = 0; i < 12; i++) {
    const float ph = std::fmod(a * 0.5f + rnd(i * 3 + 1), 1.0f);
    const float ang = rnd(i * 3 + 2) * 360 * kDeg;
    const float r = 60 + ph * 70;
    gfx::fillCircle(x.s, kC + std::sin(ang) * r, 220 - std::cos(ang) * r * 0.7f, 2.2f, col::amber, (1 - ph) * k);
  }
  const char* label = tr(f.lang, Str::Answer);
  const int tw = width(x, Font::Small, label);
  const float w = tw + 30.0f;
  gfx::fillRoundRect(x.s, kC - w / 2, 428 - 16, w, 32, 16, rgb(0x3A, 0x2E, 0x04), 0.92f * k);
  text(x, Font::Small, label, kC, 428 + capHeight(Font::Small) / 2, col::amber, textdatum_t::baseline_center, k);
}

/** Seconds with one decimal, the decimal sign of the language: "4,3 s" / "4.3 s". */
void seconds(char* out, size_t cap, float s, Lang lang) {
  const int tenths = static_cast<int>(s * 10 + 0.5f);
  snprintf(out, cap, lang == Lang::En ? "%d.%d s" : "%d,%d s", tenths / 10, tenths % 10);
}

/**
 * 0–100 km/h: while a launch runs, its time ticks at the foot of the glass; at 100 the result
 * stays there a few seconds, with a flash round the rim (gold for a new best).
 */
void launchOverlay(Ctx& x) {
  const Frame& f = x.f;
  constexpr float cy = 402, h = 52;  // in place of the chip below the card
  char time[16];
  if (f.launchMs > 0) {
    const float a = f.launchAge;
    const uint32_t accent = f.launchBest ? rgb(0xFF, 0xC8, 0x2E) : col::green;
    // The flash: the whole rim lights up and fades.
    const float flash = 1 - clamp01(a / 1.2f);
    if (flash > 0) gfx::strokeCircle(x.s, kC, kC, 226, 10, accent, flash * 0.9f);
    const float k = clamp01(a / 0.25f) * clamp01((App::kRunShowMs / 1000.0f - a) / 0.5f);
    seconds(time, sizeof(time), f.launchMs / 1000.0f, f.lang);
    const char* label = tr(f.lang, f.launchBest ? Str::NewBest : Str::ZeroToHundred);
    const float w = std::max(width(x, Font::Title, time), width(x, Font::Small, label)) + 40.0f;
    gfx::fillRoundRect(x.s, kC - w / 2, cy - h / 2, w, h, 18, rgb(0x10, 0x10, 0x12), 0.94f * k);
    gfx::strokeRoundRect(x.s, kC - w / 2, cy - h / 2, w, h, 18, 1.5f, accent, k);
    text(x, Font::Small, label, kC, cy - 6, accent, textdatum_t::baseline_center, k);
    text(x, Font::Title, time, kC, cy + 20, col::white, textdatum_t::baseline_center, k);
    return;
  }
  if (f.launchLive >= 0) {
    seconds(time, sizeof(time), f.launchLive, f.lang);
    const char* label = tr(f.lang, Str::ZeroToHundred);
    const float w = std::max(width(x, Font::Title, time), width(x, Font::Small, label)) + 40.0f;
    gfx::fillRoundRect(x.s, kC - w / 2, cy - h / 2, w, h, 18, rgb(0x10, 0x10, 0x12), 0.94f);
    // How far to 100, along the pill's lower edge.
    const float p = clamp01(f.speed / 100.0f);
    gfx::fillRoundRect(x.s, kC - w / 2 + 10, cy + h / 2 - 5, (w - 20) * p, 3, 1.5f, col::green);
    text(x, Font::Small, label, kC, cy - 6, col::text2);
    text(x, Font::Title, time, kC, cy + 20, col::white);
  }
}

void eggOverlay(Ctx& x) {
  switch (x.f.egg) {
    case Egg::Warp:
      warpOverlay(x);
      break;
    case Egg::TimeTravel:
      if (x.f.screen == Screen::Drive) timeTravelOverlay(x);
      break;
    case Egg::Answer:
      if (x.f.screen == Screen::Drive) answerOverlay(x);
      break;
    default:
      break;
  }
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

// --- The iPhone's notifications (0.2.11) -------------------------------------------------------

/** Seconds the full notification shows the icon alone before the rest comes in. */
constexpr float kNoticeIconOnly = 0.9f;

float easeInOut(float t) {
  t = clamp01(t);
  return t < 0.5f ? 4 * t * t * t : 1 - std::pow(-2 * t + 2, 3.0f) / 2;
}

/** A colour for an app without an icon (yet): the same app always gets the same one. */
uint32_t appColour(const char* app) {
  static constexpr uint32_t kColours[] = {rgb(0x0A, 0x84, 0xFF), rgb(0x5E, 0x5C, 0xE6), rgb(0xFF, 0x9F, 0x0A),
                                          rgb(0xFF, 0x37, 0x5F), rgb(0x30, 0xB0, 0xC7), rgb(0xBF, 0x5A, 0xF2),
                                          rgb(0x34, 0xC7, 0x59)};
  uint32_t h = 2166136261u;
  for (const char* p = app; p != nullptr && *p != 0; p++) h = (h ^ static_cast<uint8_t>(*p)) * 16777619u;
  return kColours[h % (sizeof(kColours) / sizeof(kColours[0]))];
}

/** Coverage (0..1) of a rounded square of half side `half` and corner radius r at (dx, dy) from its centre. */
inline float squircleCover(float dx, float dy, float half, float r) {
  const float qx = gfx::fabsf_(dx) - (half - r), qy = gfx::fabsf_(dy) - (half - r);
  const float ox = gfx::fmaxf_(qx, 0), oy = gfx::fmaxf_(qy, 0);
  const float d = std::sqrt(ox * ox + oy * oy) + gfx::fminf_(gfx::fmaxf_(qx, qy), 0) - r;
  return clamp01(0.5f - d);
}

/**
 * The app's icon as iOS shows it: the square artwork with continuous-looking round corners, `size`
 * pixels wide around (cx, cy). Without an icon: a rounded square in the app's colour with a glyph.
 */
void appIcon(Ctx& x, const Frame& f, float cx, float cy, float size, float alpha) {
  const Frame::Notice& n = f.notice;
  const float half = size / 2, r = size * 0.2237f;
  if (!gfx::rowsIn(x.s, cy - half - 1, cy + half + 1) || alpha <= 0.003f) return;
  const AppIcon* icon = f.noticeIcon;
  if (icon == nullptr || icon->px == nullptr) {
    const uint32_t c = n.call || n.missed ? col::green : appColour(n.app);
    gfx::fillRoundRect(x.s, cx - half, cy - half, size, size, r, c, alpha);
    if (n.call || n.missed) {
      icons::glyph(x.s, glyphs::kCall, cx, cy, size * 0.62f, col::white, alpha);
    } else {
      // The app's first letter.
      char letter[5] = {0};
      const char* name = n.appName[0] != 0 ? n.appName : n.title;
      const char* p = name;
      if (vlw(Font::Hero).loaded() && *p != 0) {
        const char* start = p;
        nextCodePoint(p);
        const size_t len = static_cast<size_t>(p - start) < sizeof(letter) - 1 ? p - start : sizeof(letter) - 1;
        memcpy(letter, start, len);
        if (letter[0] >= 'a' && letter[0] <= 'z') letter[0] = static_cast<char>(letter[0] - 32);
      }
      const Font font = size >= 80 ? Font::Hero : Font::Small;
      text(x, font, letter, cx, cy + capHeight(font) / 2, col::white, textdatum_t::baseline_center, alpha);
    }
    return;
  }
  constexpr int kN = AppIcon::kSize;
  const float step = kN / size;  // source pixels per glass pixel
  const int y0 = std::max(gfx::ifloor(cy - half), x.s.oy), y1 = std::min(gfx::iceil(cy + half), x.s.oy + x.s.h);
  const int x0 = std::max(gfx::ifloor(cx - half), 0), x1 = std::min(gfx::iceil(cx + half), kSize);
  const int box = step >= 2 ? gfx::iround(step) : 1;  // small: average a box of source pixels
  for (int y = y0; y < y1; y++) {
    uint16_t* row = gfx::rowAt(x.s, y);
    const float py = y + 0.5f - cy;
    const float sy = (py + half) * step - 0.5f;
    for (int px = x0; px < x1; px++) {
      const float dx = px + 0.5f - cx;
      const float cover = squircleCover(dx, py, half, r) * alpha;
      if (cover <= 0.004f) continue;
      const float sx = (dx + half) * step - 0.5f;
      uint32_t rr = 0, gg = 0, bb = 0;
      if (box > 1) {
        const int bx = std::min(std::max(gfx::iround(sx - box / 2.0f + 0.5f), 0), kN - box);
        const int by = std::min(std::max(gfx::iround(sy - box / 2.0f + 0.5f), 0), kN - box);
        for (int j = 0; j < box; j++) {
          const uint16_t* src = icon->px + (by + j) * kN + bx;
          for (int i = 0; i < box; i++) {
            const uint32_t c = gfx::unpack(src[i]);
            rr += c >> 16;
            gg += (c >> 8) & 0xFF;
            bb += c & 0xFF;
          }
        }
        const uint32_t k = static_cast<uint32_t>(box * box);
        rr /= k;
        gg /= k;
        bb /= k;
      } else {
        // Bilinear.
        const float fx = gfx::fminf_(gfx::fmaxf_(sx, 0), kN - 1.001f), fy = gfx::fminf_(gfx::fmaxf_(sy, 0), kN - 1.001f);
        const int ix = static_cast<int>(fx), iy = static_cast<int>(fy);
        const float tx = fx - ix, ty = fy - iy;
        const uint16_t* a = icon->px + iy * kN + ix;
        const uint32_t c00 = gfx::unpack(a[0]), c10 = gfx::unpack(a[1]), c01 = gfx::unpack(a[kN]),
                       c11 = gfx::unpack(a[kN + 1]);
        auto ch = [&](int shift) {
          const float v00 = (c00 >> shift) & 0xFF, v10 = (c10 >> shift) & 0xFF, v01 = (c01 >> shift) & 0xFF,
                      v11 = (c11 >> shift) & 0xFF;
          return static_cast<uint32_t>((v00 * (1 - tx) + v10 * tx) * (1 - ty) + (v01 * (1 - tx) + v11 * tx) * ty + 0.5f);
        };
        rr = ch(16);
        gg = ch(8);
        bb = ch(0);
      }
      const uint32_t c = gfx::shade(x.s, rgb(static_cast<uint8_t>(rr), static_cast<uint8_t>(gg), static_cast<uint8_t>(bb)));
      if (cover >= 0.996f) {
        row[px] = gfx::pack(c);
      } else {
        gfx::blendPixel(x.s, px, y, c, static_cast<int>(cover * 255 + 0.5f));
      }
    }
  }
}

/** Breaks `src` into at most `maxLines` lines no wider than widths[i]; the last one ends in "…" if cut. */
int wrapLines(Ctx& x, Font font, const char* src, const float* widths, int maxLines, char (*out)[96]) {
  int lines = 0;
  const char* p = src;
  while (*p == ' ') p++;
  while (*p != 0 && lines < maxLines) {
    // Take words while they fit.
    size_t best = 0;
    size_t i = 0;
    char buf[96];
    for (;;) {
      size_t j = i;
      while (p[j] != 0 && p[j] != ' ') j++;
      if (j >= sizeof(buf)) break;
      memcpy(buf, p, j);
      buf[j] = 0;
      if (width(x, font, buf) > widths[lines]) break;
      best = j;
      if (p[j] == 0) break;
      i = j + 1;
    }
    const bool last = lines == maxLines - 1;
    if (best == 0 || (last && p[best] != 0)) {
      // A word longer than the line, or the last line with more to come: cut with an ellipsis.
      fit(x, font, p, widths[lines], out[lines], sizeof(out[0]));
      if (last && p[best] != 0 && best > 0) {
        char rest[200];
        snprintf(rest, sizeof(rest), "%.*s …", static_cast<int>(best), p);
        if (width(x, font, rest) <= widths[lines]) snprintf(out[lines], sizeof(out[0]), "%s", rest);
      }
      lines++;
      if (best == 0) break;
      p += best;
    } else {
      memcpy(out[lines], p, best);
      out[lines][best] = 0;
      lines++;
      p += best;
    }
    while (*p == ' ') p++;
  }
  return lines;
}

/** Half the width of the glass at height y, minus a margin. */
float chordHalf(float y, float margin) {
  const float dy = y - kC;
  const float r = kC - margin;
  return dy * dy >= r * r ? 0 : std::sqrt(r * r - dy * dy);
}

/**
 * Over the whole glass, the way the iPhone's StandBy does it: first only the app's icon, large in
 * the middle; then it shrinks up to the top and the sender and the text come in under it. A call
 * gets its two buttons.
 */
void noticeFull(Ctx& x) {
  const Frame::Notice& n = x.f.notice;
  const float in = easeOut(n.age / 0.28f);
  const float a = in * (1 - n.out);
  // Black behind it, as StandBy; a drive's map stays faintly there.
  gfx::darken(x.s, 0, 0, kSize, kSize, 1.0f - (x.f.screen == Screen::Drive && !n.call ? 0.8f : 0.94f) * a);
  if (a <= 0.003f) return;

  const bool call = n.call;
  // The icon alone: springs up from small, large in the middle ...
  const float pop = 0.55f + 0.45f * easeOutBack(n.age / 0.5f);
  const float bigSize = 192.0f * pop, bigCy = kC - 8;
  // ... then moves up and shrinks; the rest follows it in.
  const float m = n.age < kNoticeIconOnly ? 0 : easeInOut((n.age - kNoticeIconOnly) / 0.5f);
  const float smallSize = call ? 96.0f : 104.0f, smallCy = call ? 112.0f : 118.0f;
  const float iconSize = bigSize + (smallSize - bigSize) * m;
  const float iconCy = bigCy + (smallCy - bigCy) * m;
  appIcon(x, x.f, kC, iconCy, iconSize, a);

  // The app's name under the icon, wherever the icon is.
  char line[96];
  float y = iconCy + iconSize / 2 + 30;
  fit(x, Font::Small, n.appName, chordHalf(y, 40) * 2, line, sizeof(line));
  text(x, Font::Small, line, kC, y, col::text2, textdatum_t::baseline_center, a);

  const float rest = easeOut((n.age - kNoticeIconOnly - 0.4f) / 0.35f);  // once the icon is up
  const float ra = a * rest;
  if (ra <= 0.003f) return;
  const float lift = (1 - rest) * 14;

  // The sender, large for a call.
  y = smallCy + smallSize / 2 + 30 + (call ? 52 : 38) + lift;
  Font titleFont = call ? Font::Hero : Font::Title;
  if (width(x, titleFont, n.title) > chordHalf(y, 44) * 2) titleFont = Font::Title;
  fit(x, titleFont, n.title, chordHalf(y, 44) * 2, line, sizeof(line));
  text(x, titleFont, line, kC, y, col::white, textdatum_t::baseline_center, ra);

  if (call) {
    // Decline left, accept right; the accept button rings.
    const float by = kCallButtonY + lift;
    const float ring = std::fmod(n.age, 1.4f) / 1.4f;
    gfx::strokeCircle(x.s, kC + kCallButtonDx, by, kCallButtonR + 4 + 16 * ring, 3, col::green,
                      a * (1 - ring) * 0.7f);
    gfx::fillCircle(x.s, kC - kCallButtonDx, by, kCallButtonR, col::red, ra);
    gfx::fillCircle(x.s, kC + kCallButtonDx, by, kCallButtonR, col::green, ra);
    icons::glyph(x.s, glyphs::kCallEnd, kC - kCallButtonDx, by, 44, col::white, ra);
    icons::glyph(x.s, glyphs::kCall, kC + kCallButtonDx, by, 40, col::white, ra);
    const float ly = by + kCallButtonR + 22;
    fit(x, Font::Small, n.negative, 110, line, sizeof(line));
    text(x, Font::Small, line, kC - kCallButtonDx, ly, col::text2, textdatum_t::baseline_center, ra);
    fit(x, Font::Small, n.positive, 110, line, sizeof(line));
    text(x, Font::Small, line, kC + kCallButtonDx, ly, col::text2, textdatum_t::baseline_center, ra);
    return;
  }

  if (n.body[0] != 0) {
    char lines[3][96];
    float widths[3];
    const float first = y + 34;  // (lift is in y already)
    for (int i = 0; i < 3; i++) widths[i] = chordHalf(first + i * 28 + 6, 40) * 2;
    const int count = wrapLines(x, Font::Body, n.body, widths, 3, lines);
    for (int i = 0; i < count; i++) {
      text(x, Font::Body, lines[i], kC, first + i * 28, rgb(0xD1, 0xD1, 0xD6), textdatum_t::baseline_center, ra);
    }
  }
  if (n.waiting > 0) {
    char more[8];
    snprintf(more, sizeof(more), "+%d", n.waiting);
    chip(x, 428, more, col::text2, 0, nullptr, ra);
  }
  // Its time runs down round the rim.
  if (n.left > 0.003f) gfx::arc(x.s, kC, kC, 229, 3, 0.0f, 360.0f * n.left, col::tick, col::tick, ra);
}

/** While driving: a pill low on the glass, the icon small, app and sender — the speed stays. */
void noticeCompact(Ctx& x) {
  const Frame::Notice& n = x.f.notice;
  const float in = easeOut(n.age / 0.3f);
  const float a = in * (1 - n.out);
  if (a <= 0.003f) return;
  const float cy = (x.mapReady ? 176.0f : kChipCy) + (1 - in) * 14;
  const float h = 50, iconSize = 34;
  char app[64], title[96];
  const float textMax = 196;
  fit(x, Font::Tiny, n.appName, textMax, app, sizeof(app));
  fit(x, Font::Small, n.title, textMax, title, sizeof(title));
  const float tw = std::max(width(x, Font::Tiny, app), width(x, Font::Small, title));
  const float w = 8 + iconSize + 10 + tw + 18;
  const float x0 = kC - w / 2;
  if (!gfx::rowsIn(x.s, cy - h / 2 - 1, cy + h / 2 + 1)) return;
  gfx::fillRoundRect(x.s, x0, cy - h / 2, w, h, h / 2, rgb(0x22, 0x22, 0x25), a);
  appIcon(x, x.f, x0 + 8 + iconSize / 2, cy, iconSize, a);
  const float tx = x0 + 8 + iconSize + 10;
  text(x, Font::Tiny, app, tx, cy - 6, col::text2, textdatum_t::baseline_left, a);
  text(x, Font::Small, title, tx, cy + 15, col::white, textdatum_t::baseline_left, a);
}

/** Everything of one frame that falls into the band x.s. */
void renderBand(Ctx& x) {
  const Frame& f = x.f;
  // Every screen except the HUD fades in.
  // (The welcome brings its own entry animation.)
  const float fade = f.screen == Screen::Drive || f.screen == Screen::Boot || f.screen == Screen::Welcome ||
                             f.screen == Screen::Charging || f.screen == Screen::Intro
                         ? 1.0f
                         : clamp01(f.screenAge / 0.3f);
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
    case Screen::Welcome:
      welcomeScreen(x);
      break;
    case Screen::Charging:
      chargingScreen(x);
      break;
    case Screen::Intro:
      introScreen(x);
      break;
  }
  // Out of the intro's flash: the white fades off the greeting.
  if (f.afterIntro && f.screenAge < 0.7f) {
    gfx::fillRoundRect(x.s, 0, 0, kSize, kSize, 0, col::white, 1 - easeOut(f.screenAge / 0.7f));
  }
  x.s.dim = 1.0f;
  if (x.f.egg != Egg::None) eggOverlay(x);
  if (f.screen == Screen::Drive && !f.linkLost && (f.launchMs > 0 || f.launchLive >= 0)) launchOverlay(x);
  if (f.notice.style == Frame::Notice::Style::Full) {
    noticeFull(x);
  } else if (f.notice.style == Frame::Notice::Style::Compact) {
    noticeCompact(x);
  }

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
