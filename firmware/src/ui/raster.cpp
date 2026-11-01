#include "ui/raster.h"

#include <cmath>
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <vector>

namespace monav::gfx {
namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kDeg = kPi / 180.0f;

inline float clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }

/** Coverage 0..255 from a signed distance at the pixel centre. */
inline int coverage(float sd, float alpha) {
  const float c = clamp01(0.5f - sd) * alpha;
  return static_cast<int>(c * 255.0f + 0.5f);
}

/** Smallest pixel x whose centre (x + 0.5) is at or beyond v. */
inline int firstFrom(float v) { return iceil(v - 0.5f); }

inline uint32_t div255(uint32_t v) { return (v + 128 + ((v + 128) >> 8)) >> 8; }

inline void blendRaw(uint16_t* p, uint32_t c, int a) {
  if (a <= 0) return;
  if (a >= 255) {
    *p = pack(c);
    return;
  }
  const uint32_t d = unpack(*p);
  const uint32_t ia = 255 - static_cast<uint32_t>(a);
  const uint32_t ua = static_cast<uint32_t>(a);
  const uint32_t r = div255(((c >> 16) & 0xFF) * ua + ((d >> 16) & 0xFF) * ia);
  const uint32_t g = div255(((c >> 8) & 0xFF) * ua + ((d >> 8) & 0xFF) * ia);
  const uint32_t b = div255((c & 0xFF) * ua + (d & 0xFF) * ia);
  *p = pack((r << 16) | (g << 8) | b);
}

struct Box {
  int x0, y0, x1, y1;  // inclusive-exclusive
};

Box clipBox(const Surface& s, float x0, float y0, float x1, float y1) {
  Box b{ifloor(x0), ifloor(y0), iceil(x1),
        iceil(y1)};
  if (b.x0 < 0) b.x0 = 0;
  if (b.y0 < s.oy) b.y0 = s.oy;
  if (b.x1 > s.w) b.x1 = s.w;
  if (b.y1 > s.oy + s.h) b.y1 = s.oy + s.h;
  return b;
}

/**
 * atan2 in degrees from a polynomial (error below 0.001°): libm's atan2f costs 1.5 µs on the
 * ESP32-S3, and the arcs need a few per row.
 */
inline float fastAtan2Deg(float y, float x) {
  const float ax = fabsf_(x), ay = fabsf_(y);
  const float mx = ax > ay ? ax : ay, mn = ax > ay ? ay : ax;
  if (mx == 0.0f) return 0.0f;
  const float a = mn / mx;
  const float s = a * a;
  float r = a * (0.99997726f + s * (-0.33262347f + s * (0.19354346f + s * (-0.11643287f + s * (0.05265332f + s * -0.01172120f)))));
  if (ay > ax) r = 1.57079637f - r;
  if (x < 0) r = 3.14159274f - r;
  if (y < 0) r = -r;
  return r * 57.2957795f;
}

/** Angle of (dx, dy) in degrees: 0 at 12 o'clock, clockwise, in [0, 360). */
inline float angleOf(float dx, float dy) {
  float a = fastAtan2Deg(dx, -dy);
  if (a < 0) a += 360.0f;
  return a;
}

inline float segDist(float px, float py, float ax, float ay, float bx, float by) {
  const float vx = bx - ax, vy = by - ay;
  const float wx = px - ax, wy = py - ay;
  const float l2 = vx * vx + vy * vy;
  float t = l2 > 0 ? (wx * vx + wy * vy) / l2 : 0;
  t = clamp01(t);
  const float dx = wx - vx * t, dy = wy - vy * t;
  return std::sqrt(dx * dx + dy * dy);
}

}  // namespace

uint32_t mix(uint32_t a, uint32_t b, float t) {
  t = clamp01(t);
  const float u = 1.0f - t;
  const uint32_t r = static_cast<uint32_t>(((a >> 16) & 0xFF) * u + ((b >> 16) & 0xFF) * t + 0.5f);
  const uint32_t g = static_cast<uint32_t>(((a >> 8) & 0xFF) * u + ((b >> 8) & 0xFF) * t + 0.5f);
  const uint32_t bl = static_cast<uint32_t>((a & 0xFF) * u + (b & 0xFF) * t + 0.5f);
  return (r << 16) | (g << 8) | bl;
}

uint32_t scale(uint32_t c, float k) {
  if (k >= 1.0f) return c;
  k = clamp01(k);
  const uint32_t r = static_cast<uint32_t>(((c >> 16) & 0xFF) * k + 0.5f);
  const uint32_t g = static_cast<uint32_t>(((c >> 8) & 0xFF) * k + 0.5f);
  const uint32_t b = static_cast<uint32_t>((c & 0xFF) * k + 0.5f);
  return (r << 16) | (g << 8) | b;
}

uint32_t shade(const Surface& s, uint32_t c) { return scale(c, s.dim); }

void blendPixel(Surface& s, int x, int y, uint32_t c, int a) {
  if (x < 0 || y < s.oy || x >= s.w || y >= s.oy + s.h) return;
  blendRaw(rowAt(s, y) + x, c, a);
}

void clear(Surface& s, uint32_t c) {
  const uint16_t v = pack(c);
  if (v == 0) {
    memset(s.px, 0, static_cast<size_t>(s.w) * s.h * 2);
    return;
  }
  for (int i = 0; i < s.w * s.h; i++) s.px[i] = v;
}

namespace {

/** Writes one pixel of colour c (packed `solid`) with coverage cov (0..1) times alpha. */
inline void put(uint16_t* p, uint32_t c, uint16_t solid, float cov, float alpha) {
  const float a = cov * alpha;
  if (a >= 0.998f) {
    *p = solid;
  } else if (a > 0.002f) {
    blendRaw(p, c, static_cast<int>(a * 255.0f + 0.5f));
  }
}

/** Last pixel x whose centre (x + 0.5) is at or before v. */
inline int lastTo(float v) { return ifloor(v - 0.5f); }

/**
 * One row of a shape symmetric about cx, given as half widths: beyond `out` nothing, within `full`
 * full coverage, and (for rings) within `holeFull` the hole's full coverage ends, within `hole`
 * nothing. Solid runs are plain stores; only the pixels on the edges get `exact(fx)`.
 */
template <typename Exact>
void spanRow(uint16_t* row, int w, float cx, float out, float full, float hole, float holeFull, uint32_t c,
             uint16_t solid, float alpha, Exact exact) {
  const int xa = std::max(0, firstFrom(cx - out));
  const int xb = std::min(w - 1, lastTo(cx + out));
  // Solid runs (inclusive): [sl0, sl1] and [sr0, sr1]; skipped hole (inclusive): [h0, h1].
  int sl0 = 1, sl1 = 0, sr0 = 1, sr1 = 0, h0 = 1, h1 = 0;
  if (full >= 0) {
    if (holeFull > 0) {
      sl0 = firstFrom(cx - full);
      sl1 = lastTo(cx - holeFull);
      sr0 = firstFrom(cx + holeFull);
      sr1 = lastTo(cx + full);
    } else {
      sl0 = firstFrom(cx - full);
      sl1 = lastTo(cx + full);
    }
  }
  if (hole > 0) {
    h0 = firstFrom(cx - hole) + 1;  // strictly inside: nothing to draw
    h1 = lastTo(cx + hole) - 1;
  }
  const bool opaque = alpha >= 0.998f;
  const int a8 = static_cast<int>(alpha * 255.0f + 0.5f);
  for (int x = xa; x <= xb; x++) {
    if ((x >= sl0 && x <= sl1) || (x >= sr0 && x <= sr1)) {
      const int end = (x >= sl0 && x <= sl1) ? std::min(sl1, xb) : std::min(sr1, xb);
      if (opaque) {
        for (; x <= end; x++) row[x] = solid;
      } else {
        for (; x <= end; x++) blendRaw(row + x, c, a8);
      }
      x--;
      continue;
    }
    if (x >= h0 && x <= h1) {
      x = h1;
      continue;
    }
    const float cov = exact(x + 0.5f - cx);
    if (cov > 0) put(row + x, c, solid, cov > 1 ? 1 : cov, alpha);
  }
}

}  // namespace

void fillCircle(Surface& s, float cx, float cy, float r, uint32_t c, float alpha) {
  if (r <= 0 || alpha <= 0) return;
  if (!rowsIn(s, cy - r - 1, cy + r + 1)) return;
  c = shade(s, c);
  const uint16_t solid = pack(c);
  const int y0 = std::max(s.oy, ifloor(cy - r - 1));
  const int y1 = std::min(s.oy + s.h, iceil(cy + r + 1));
  const float ro = r + 0.5f, ri = r - 0.5f;
  for (int y = y0; y < y1; y++) {
    const float fy = y + 0.5f - cy;
    const float fy2 = fy * fy;
    if (fy2 >= ro * ro) continue;
    const float out = std::sqrt(ro * ro - fy2);
    const float full = ri > fabsf_(fy) ? std::sqrt(ri * ri - fy2) : -1.0f;
    spanRow(rowAt(s, y), s.w, cx, out, full, -1, -1, c, solid, alpha,
            [&](float fx) { return 0.5f - (std::sqrt(fx * fx + fy2) - r); });
  }
}

void strokeCircle(Surface& s, float cx, float cy, float r, float width, uint32_t c, float alpha) {
  if (alpha <= 0) return;
  const float hw = width * 0.5f;
  if (!rowsIn(s, cy - r - hw - 1, cy + r + hw + 1)) return;
  c = shade(s, c);
  const uint16_t solid = pack(c);
  const float rout = r + hw, rin = r - hw;
  const int y0 = std::max(s.oy, ifloor(cy - rout - 1));
  const int y1 = std::min(s.oy + s.h, iceil(cy + rout + 1));
  for (int y = y0; y < y1; y++) {
    const float fy = y + 0.5f - cy;
    const float fy2 = fy * fy;
    const float ao = rout + 0.5f;
    if (fy2 >= ao * ao) continue;
    const float out = std::sqrt(ao * ao - fy2);
    const float af = rout - 0.5f;
    const float full = af > fabsf_(fy) ? std::sqrt(af * af - fy2) : -1.0f;
    const float ah = rin - 0.5f, ahf = rin + 0.5f;
    const float hole = ah > fabsf_(fy) ? std::sqrt(ah * ah - fy2) : -1.0f;
    float holeFull = ahf > fabsf_(fy) ? std::sqrt(ahf * ahf - fy2) : 0.0f;
    if (full >= 0 && holeFull > full) holeFull = full + 1;  // a thin ring: no solid run
    spanRow(rowAt(s, y), s.w, cx, out, full, hole, holeFull > 0 ? holeFull : 0.0001f, c, solid, alpha,
            [&](float fx) {
              const float d = std::sqrt(fx * fx + fy2);
              return fminf_(0.5f - (d - rout), 0.5f - (rin - d));
            });
  }
}

void arc(Surface& s, float cx, float cy, float r, float width, float a0, float a1, uint32_t c0, uint32_t c1,
         float alpha) {
  if (alpha <= 0) return;
  float span = a1 - a0;
  if (span <= 0) return;
  const float hw = width * 0.5f;
  if (!rowsIn(s, cy - r - hw - 1, cy + r + hw + 1)) return;
  if (span >= 360.0f) {
    strokeCircle(s, cx, cy, r, width, c0, alpha);
    return;
  }
  const uint32_t s0 = shade(s, c0), s1 = shade(s, c1);
  // Colours along the arc, 64 steps.
  uint32_t tbl[65];
  uint16_t ptbl[65];
  for (int i = 0; i <= 64; i++) {
    tbl[i] = s0 == s1 ? s0 : mix(s0, s1, i / 64.0f);
    ptbl[i] = pack(tbl[i]);
  }
  float start = std::fmod(a0, 360.0f);
  if (start < 0) start += 360.0f;
  float stop = std::fmod(start + span, 360.0f);
  auto relOf = [&](float ang) {
    float rel = ang - start;
    if (rel < 0) rel += 360.0f;
    return rel;
  };
  auto within = [&](float ang, float lo, float hi) { return ang > lo && ang < hi; };
  const bool opaque = alpha >= 0.998f;

  const float rout = r + hw, rin = r - hw;
  const int y0 = std::max(s.oy, ifloor(cy - rout - 1));
  const int y1 = std::min(s.oy + s.h, iceil(cy + rout + 1));
  for (int y = y0; y < y1; y++) {
    const float fy = y + 0.5f - cy;
    const float fy2 = fy * fy;
    const float ao = rout + 0.5f;
    if (fy2 >= ao * ao) continue;
    const float out = std::sqrt(ao * ao - fy2);
    const float af = rout - 0.5f;
    const float full = af > fabsf_(fy) ? std::sqrt(af * af - fy2) : -1.0f;
    const float ah = rin - 0.5f, ahf = rin + 0.5f;
    const float hole = ah > fabsf_(fy) ? std::sqrt(ah * ah - fy2) : -1.0f;
    const float holeFull = ahf > fabsf_(fy) ? std::sqrt(ahf * ahf - fy2) : 0.0f;
    uint16_t* row = rowAt(s, y);
    // The ring's pixels in this row, as pieces that each stay on one side of the centre line:
    // along such a piece the angle changes monotonically.
    const int xa = std::max(0, firstFrom(cx - out)), xb = std::min(s.w - 1, lastTo(cx + out));
    const int mid = firstFrom(cx);  // first pixel right of the centre line
    int pieces[4][2];
    int np = 0;
    if (hole > 0) {
      const int hl = firstFrom(cx - hole) + 1, hr = lastTo(cx + hole) - 1;
      pieces[np][0] = xa, pieces[np][1] = std::min(xb, hl - 1), np++;
      pieces[np][0] = std::max(xa, hr + 1), pieces[np][1] = xb, np++;
    } else {
      pieces[np][0] = xa, pieces[np][1] = std::min(xb, mid - 1), np++;
      pieces[np][0] = std::max(xa, mid), pieces[np][1] = xb, np++;
    }
    // Solid columns (full coverage across the ring's width) of this row, as pixel ranges.
    int sl0 = 1, sl1 = 0, sr0 = 1, sr1 = 0;
    if (full >= 0) {
      if (holeFull > 0) {
        sl0 = firstFrom(cx - full);
        sl1 = lastTo(cx - holeFull);
        sr0 = firstFrom(cx + holeFull);
        sr1 = lastTo(cx + full);
      } else {
        sl0 = firstFrom(cx - full);
        sl1 = lastTo(cx + full);
      }
    }
    const int a8 = static_cast<int>(alpha * 255.0f + 0.5f);
    for (int k = 0; k < np; k++) {
      const int p0 = pieces[k][0], p1 = pieces[k][1];
      if (p0 > p1) continue;
      const float th0 = angleOf(p0 + 0.5f - cx, fy), th1 = angleOf(p1 + 0.5f - cx, fy);
      const float lo = fminf_(th0, th1), hi = fmaxf_(th0, th1);
      // Does an end of the arc fall inside this piece (or on its ends)? Then each pixel's angle is
      // needed. Otherwise the whole piece is in or out, as its middle is.
      const bool cut = within(start, lo - 0.05f, hi + 0.05f) || within(stop, lo - 0.05f, hi + 0.05f);
      if (!cut && relOf(0.5f * (th0 + th1)) > span) continue;  // all outside
      // Colour index along the piece in 16.16 fixed point (the angle changes smoothly).
      const float k64 = 64.0f / span;
      int32_t ci = static_cast<int32_t>(relOf(th0) * k64 * 65536.0f);
      const int32_t dci = p1 > p0 ? static_cast<int32_t>((relOf(th1) - relOf(th0)) * k64 * 65536.0f / (p1 - p0)) : 0;
      for (int x = p0; x <= p1; x++, ci += dci) {
        int idx;
        if (cut) {
          const float rel = relOf(angleOf(x + 0.5f - cx, fy));
          if (rel > span) continue;
          idx = static_cast<int>(rel * k64 + 0.5f);
        } else {
          idx = (ci + 32768) >> 16;
        }
        idx = idx < 0 ? 0 : idx > 64 ? 64 : idx;
        if ((x >= sl0 && x <= sl1) || (x >= sr0 && x <= sr1)) {
          if (opaque) {
            row[x] = ptbl[idx];
          } else {
            blendRaw(row + x, tbl[idx], a8);
          }
          continue;
        }
        const float fx = x + 0.5f - cx;
        const float d = std::sqrt(fx * fx + fy2);
        const float cov = fminf_(0.5f - (d - rout), 0.5f - (rin - d));
        if (cov <= 0) continue;
        put(row + x, tbl[idx], ptbl[idx], cov > 1 ? 1 : cov, alpha);
      }
    }
  }
  // Round ends over the cut.
  fillCircle(s, cx + r * std::sin(a0 * kDeg), cy - r * std::cos(a0 * kDeg), hw, c0, alpha);
  fillCircle(s, cx + r * std::sin(a1 * kDeg), cy - r * std::cos(a1 * kDeg), hw, c1, alpha);
}

namespace {

/**
 * Where the row y = py crosses the capsule around segment A–B with radius r: [lo, hi]. The capsule
 * is convex, so it is one interval: the union of the two end discs' and the swept rectangle's.
 */
bool capsuleSpan(float ax, float ay, float bx, float by, float r, float py, float& lo, float& hi) {
  if (r <= 0) return false;
  lo = 1e30f;
  hi = -1e30f;
  auto disc = [&](float cx, float cy) {
    const float d = py - cy;
    if (d * d > r * r) return;
    const float e = std::sqrt(r * r - d * d);
    lo = fminf_(lo, cx - e);
    hi = fmaxf_(hi, cx + e);
  };
  disc(ax, ay);
  disc(bx, by);
  const float vx = bx - ax, vy = by - ay;
  const float l = std::sqrt(vx * vx + vy * vy);
  if (l > 1e-4f) {
    const float nx = -vy / l * r, ny = vx / l * r;
    const float q[4][2] = {{ax + nx, ay + ny}, {bx + nx, by + ny}, {bx - nx, by - ny}, {ax - nx, ay - ny}};
    for (int i = 0; i < 4; i++) {
      const float* p0 = q[i];
      const float* p1 = q[(i + 1) & 3];
      const float y0 = fminf_(p0[1], p1[1]), y1 = fmaxf_(p0[1], p1[1]);
      if (py < y0 || py > y1) continue;
      const float x = p1[1] == p0[1] ? fminf_(p0[0], p1[0]) : p0[0] + (p1[0] - p0[0]) * (py - p0[1]) / (p1[1] - p0[1]);
      const float x2 = p1[1] == p0[1] ? fmaxf_(p0[0], p1[0]) : x;
      lo = fminf_(lo, x);
      hi = fmaxf_(hi, x2);
    }
  }
  return hi >= lo;
}

/** 8-bit coverage of the current band: shapes add (max), then the whole is blended once. */
struct Mask {
  std::vector<uint8_t> a;
  int w = 0, h = 0, oy = 0;
  int x0 = 0, x1 = -1, y0 = 0, y1 = -1;  // touched (inclusive)

  void begin(const Surface& s) {
    if (static_cast<int>(a.size()) < s.w * s.h) a.assign(static_cast<size_t>(s.w) * s.h, 0);
    w = s.w;
    h = s.h;
    oy = s.oy;
    x0 = w;
    x1 = -1;
    y0 = oy + h;
    y1 = oy - 1;
  }

  void capsule(float ax, float ay, float bx, float by, float r) {
    const int ya = std::max(oy, ifloor(fminf_(ay, by) - r - 1));
    const int yb = std::min(oy + h, iceil(fmaxf_(ay, by) + r + 1));
    for (int y = ya; y < yb; y++) {
      const float py = y + 0.5f;
      float lo, hi;
      if (!capsuleSpan(ax, ay, bx, by, r + 0.5f, py, lo, hi)) continue;
      float ilo = 0, ihi = -1;
      const bool inner = r > 0.5f && capsuleSpan(ax, ay, bx, by, r - 0.5f, py, ilo, ihi);
      const int xa = std::max(0, firstFrom(lo)), xb = std::min(w - 1, lastTo(hi));
      if (xa > xb) continue;
      const int sa = inner ? firstFrom(ilo) : 1, sb = inner ? lastTo(ihi) : 0;
      uint8_t* row = a.data() + static_cast<size_t>(y - oy) * w;
      for (int x = xa; x <= xb; x++) {
        if (x >= sa && x <= sb) {
          const int e = std::min(sb, xb);
          for (; x <= e; x++) row[x] = 255;
          x--;
          continue;
        }
        const float sd = segDist(x + 0.5f, py, ax, ay, bx, by) - r;
        const float cov = 0.5f - sd;
        if (cov <= 0) continue;
        const uint8_t v = cov >= 1 ? 255 : static_cast<uint8_t>(cov * 255.0f + 0.5f);
        if (v > row[x]) row[x] = v;
      }
      x0 = std::min(x0, xa);
      x1 = std::max(x1, xb);
      y0 = std::min(y0, y);
      y1 = std::max(y1, y);
    }
  }

  /** Blends colour c (shaded) by the mask into s and clears what it used. */
  void flush(Surface& s, uint32_t c, float alpha) {
    if (x1 < x0) return;
    const uint16_t solid = pack(c);
    const int ia = static_cast<int>(alpha * 256.0f);
    for (int y = y0; y <= y1; y++) {
      uint8_t* m = a.data() + static_cast<size_t>(y - oy) * w;
      uint16_t* row = rowAt(s, y);
      for (int x = x0; x <= x1; x++) {
        const int v = m[x];
        if (v == 0) continue;
        m[x] = 0;
        const int cov = ia >= 256 ? v : (v * ia) >> 8;
        if (cov >= 255) {
          row[x] = solid;
        } else {
          blendRaw(row + x, c, cov);
        }
      }
    }
    x0 = w;
    x1 = -1;
    y0 = oy + h;
    y1 = oy - 1;
  }
};

Mask s_masks[2];  // one per drawing task

}  // namespace

void line(Surface& s, float x0, float y0, float x1, float y1, float width, uint32_t c, float alpha) {
  const float xy[4] = {x0, y0, x1, y1};
  polyline(s, xy, 2, width, c, alpha);
}

void polyline(Surface& s, const float* xy, int points, float width, uint32_t c, float alpha) {
  if (points < 2 || alpha <= 0) return;
  const float hw = width * 0.5f;
  float miny = xy[1], maxy = xy[1];
  for (int i = 1; i < points; i++) {
    miny = fminf_(miny, xy[2 * i + 1]);
    maxy = fmaxf_(maxy, xy[2 * i + 1]);
  }
  if (!rowsIn(s, miny - hw - 1, maxy + hw + 1)) return;
  Mask& m = s_masks[s.lane & 1];
  m.begin(s);
  for (int i = 0; i + 1 < points; i++) {
    m.capsule(xy[2 * i], xy[2 * i + 1], xy[2 * i + 2], xy[2 * i + 3], hw);
  }
  m.flush(s, shade(s, c), alpha);
}

void beginLines(const Surface& s) { s_masks[s.lane & 1].begin(s); }

void addLine(const Surface& s, float x0, float y0, float x1, float y1, float width) {
  s_masks[s.lane & 1].capsule(x0, y0, x1, y1, width * 0.5f);
}

void flushLines(Surface& s, uint32_t c, float alpha) { s_masks[s.lane & 1].flush(s, shade(s, c), alpha); }

void convex(Surface& s, const float* xy, int points, uint32_t c, float alpha) {
  if (points < 3 || alpha <= 0) return;
  c = shade(s, c);
  float area = 0;
  for (int i = 0; i < points; i++) {
    const int j = (i + 1) % points;
    area += xy[2 * i] * xy[2 * j + 1] - xy[2 * j] * xy[2 * i + 1];
  }
  const float wind = area > 0 ? 1.0f : -1.0f;
  // Outward unit normals.
  float nx[16], ny[16];
  if (points > 16) points = 16;
  for (int i = 0; i < points; i++) {
    const int j = (i + 1) % points;
    const float ex = xy[2 * j] - xy[2 * i], ey = xy[2 * j + 1] - xy[2 * i + 1];
    const float l = std::sqrt(ex * ex + ey * ey);
    nx[i] = l > 0 ? wind * ey / l : 0;
    ny[i] = l > 0 ? -wind * ex / l : 0;
  }
  float minx = xy[0], maxx = xy[0], miny = xy[1], maxy = xy[1];
  for (int i = 1; i < points; i++) {
    minx = fminf_(minx, xy[2 * i]);
    maxx = fmaxf_(maxx, xy[2 * i]);
    miny = fminf_(miny, xy[2 * i + 1]);
    maxy = fmaxf_(maxy, xy[2 * i + 1]);
  }
  const Box b = clipBox(s, minx - 1, miny - 1, maxx + 1, maxy + 1);
  for (int y = b.y0; y < b.y1; y++) {
    uint16_t* row = rowAt(s, y);
    const float py = y + 0.5f;
    for (int x = b.x0; x < b.x1; x++) {
      const float px = x + 0.5f;
      float sd = -1e9f;
      for (int i = 0; i < points; i++) {
        const float d = (px - xy[2 * i]) * nx[i] + (py - xy[2 * i + 1]) * ny[i];
        if (d > sd) sd = d;
      }
      if (sd >= 0.5f) continue;
      blendRaw(&row[x], c, coverage(sd, alpha));
    }
  }
}

namespace {

/** Signed distance to a rounded rectangle given its centre, half size and corner radius. */
inline float roundRectSd(float px, float py, float cx, float cy, float hx, float hy, float r) {
  const float qx = fabsf_(px - cx) - (hx - r);
  const float qy = fabsf_(py - cy) - (hy - r);
  const float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
  const float outside = std::sqrt(ox * ox + oy * oy);
  const float inside = fminf_(fmaxf_(qx, qy), 0.0f);
  return outside + inside - r;
}

}  // namespace

namespace {

/** Half width of a rounded rectangle (half size hx, hy, radius r) at vertical offset dy; −1 outside. */
inline float rrHalf(float dy, float hx, float hy, float r) {
  dy = fabsf_(dy);
  if (dy > hy) return -1.0f;
  const float straight = hy - r;
  if (dy <= straight) return hx;
  const float k = dy - straight;
  return (hx - r) + std::sqrt(fmaxf_(0.0f, r * r - k * k));
}

/**
 * A rounded rectangle's area (inner = false: filled) or the ring between it and an inset one
 * (stroke), row by row: spans from the row's half widths, exact distances only near the edges.
 */
void roundRectRows(Surface& s, float cx, float cy, float hx, float hy, float r, float hw, bool stroke, uint32_t c,
                   float alpha) {
  const uint16_t solid = pack(c);
  const float ohx = hx + hw, ohy = hy + hw, orr = r + hw;                  // outer edge
  const float ihx = hx - hw, ihy = hy - hw, irr = fmaxf_(0.0f, r - hw);  // inner edge (stroke)
  const int y0 = std::max(s.oy, ifloor(cy - ohy - 1));
  const int y1 = std::min(s.oy + s.h, iceil(cy + ohy + 1));
  for (int y = y0; y < y1; y++) {
    const float py = y + 0.5f;
    const float dy = fabsf_(py - cy);
    // Half widths of the level lines sd = ±0.5 of the outer (and inner) shape at this row.
    const float out = rrHalf(dy, ohx + 0.5f, ohy + 0.5f, orr + 0.5f);
    if (out < 0) continue;
    const float full = orr >= 0.5f ? rrHalf(dy, ohx - 0.5f, ohy - 0.5f, orr - 0.5f) : -1.0f;
    float hole = -1.0f, holeFull = 0.0f;
    if (stroke) {
      hole = irr >= 0.5f ? rrHalf(dy, ihx - 0.5f, ihy - 0.5f, irr - 0.5f) : -1.0f;
      holeFull = rrHalf(dy, ihx + 0.5f, ihy + 0.5f, irr + 0.5f);
      if (holeFull < 0) holeFull = 0;
      if (full >= 0 && holeFull > full) holeFull = full + 1;  // thin stroke: no solid run
    }
    spanRow(rowAt(s, y), s.w, cx, out, full, hole, stroke ? (holeFull > 0 ? holeFull : 0.0001f) : 0.0f, c, solid,
            alpha, [&](float fx) {
              float sd = roundRectSd(cx + fx, py, cx, cy, ohx, ohy, orr);
              if (stroke) sd = fmaxf_(sd, -roundRectSd(cx + fx, py, cx, cy, ihx, ihy, irr));
              return 0.5f - sd;
            });
  }
}

}  // namespace

void fillRoundRect(Surface& s, float x, float y, float w, float h, float r, uint32_t c, float alpha) {
  if (w <= 0 || h <= 0 || alpha <= 0) return;
  if (!rowsIn(s, y - 1, y + h + 1)) return;
  if (r > w * 0.5f) r = w * 0.5f;
  if (r > h * 0.5f) r = h * 0.5f;
  roundRectRows(s, x + w * 0.5f, y + h * 0.5f, w * 0.5f, h * 0.5f, r, 0.0f, false, shade(s, c), alpha);
}

void strokeRoundRect(Surface& s, float x, float y, float w, float h, float r, float width, uint32_t c,
                     float alpha) {
  if (w <= 0 || h <= 0 || alpha <= 0) return;
  if (!rowsIn(s, y - width - 1, y + h + width + 1)) return;
  if (r > w * 0.5f) r = w * 0.5f;
  if (r > h * 0.5f) r = h * 0.5f;
  roundRectRows(s, x + w * 0.5f, y + h * 0.5f, w * 0.5f, h * 0.5f, r, width * 0.5f, true, shade(s, c), alpha);
}

namespace {
std::vector<uint8_t> s_glow;
float s_glowR0 = -1, s_glowR1 = -1, s_glowRe = -1;
}  // namespace

void prepareGlow(float r0, float r1, float rEdge) {
  // Coverage by squared distance, rebuilt only when the geometry changes.
  if (s_glowR0 == r0 && s_glowR1 == r1 && s_glowRe == rEdge) return;
  const int maxD2 = static_cast<int>(rEdge * rEdge) + 1;
  s_glow.assign(static_cast<size_t>(maxD2) + 1, 0);
  for (int d2 = 0; d2 <= maxD2; d2++) {
    const float d = std::sqrt(static_cast<float>(d2));
    float t = clamp01((d - r0) / (r1 - r0));
    t = t * t * (3 - 2 * t);
    s_glow[d2] = static_cast<uint8_t>(t * 255.0f + 0.5f);
  }
  s_glowR0 = r0;
  s_glowR1 = r1;
  s_glowRe = rEdge;
}

void rimGlow(Surface& s, float cx, float cy, float r0, float r1, float rEdge, uint32_t c, float alpha) {
  if (alpha <= 0.0f) return;
  c = shade(s, c);
  prepareGlow(r0, r1, rEdge);
  const std::vector<uint8_t>& lut = s_glow;
  const int maxD2 = static_cast<int>(rEdge * rEdge) + 1;
  // Drawn onto black (right after clear), so a pixel is just the colour at its coverage:
  // a table of 256 packed colours instead of a blend per pixel.
  uint16_t tbl[256];
  for (int i = 0; i < 256; i++) tbl[i] = pack(scale(c, clamp01(alpha) * i / 255.0f));
  const int y0 = static_cast<int>(cy - rEdge), y1 = static_cast<int>(cy + rEdge) + 1;
  for (int y = y0 < s.oy ? s.oy : y0; y < (y1 > s.oy + s.h ? s.oy + s.h : y1); y++) {
    const float fy = y + 0.5f - cy;
    const float fy2 = fy * fy;
    if (fy2 > rEdge * rEdge) continue;
    const float outer = std::sqrt(rEdge * rEdge - fy2);
    const float inner = fy2 < r0 * r0 ? std::sqrt(r0 * r0 - fy2) : 0;
    uint16_t* row = rowAt(s, y);
    const int xa = static_cast<int>(cx - outer), xb = static_cast<int>(cx + outer) + 1;
    for (int x = xa < 0 ? 0 : xa; x < (xb > s.w ? s.w : xb); x++) {
      const float fx = x + 0.5f - cx;
      if (inner > 1 && fabsf_(fx) < inner - 1) {
        const int next = firstFrom(cx + inner - 1);
        if (next > x) x = next - 1;
        continue;
      }
      const int d2 = static_cast<int>(fx * fx + fy2);
      if (d2 > maxD2) continue;
      row[x] = tbl[lut[d2]];
    }
  }
}

void maskCircle(Surface& s, float cx, float cy, float r) {
  for (int y = s.oy; y < s.oy + s.h; y++) {
    uint16_t* row = rowAt(s, y);
    const float fy = y + 0.5f - cy;
    // Pixels well inside keep their colour; only the rim and the outside are touched.
    float inner = -1;
    if (fabsf_(fy) < r - 1) inner = std::sqrt((r - 1) * (r - 1) - fy * fy);
    for (int x = 0; x < s.w; x++) {
      const float fx = x + 0.5f - cx;
      if (inner > 0 && fabsf_(fx) < inner) {
        const int next = firstFrom(cx + inner);
        if (next > x) x = next - 1;
        continue;
      }
      const float sd = std::sqrt(fx * fx + fy * fy) - r;
      const float keep = clamp01(0.5f - sd);
      if (keep <= 0) {
        row[x] = 0;
      } else if (keep < 1) {
        blendRaw(&row[x], 0, static_cast<int>((1.0f - keep) * 255.0f + 0.5f));
      }
    }
  }
}

void darken(Surface& s, int x, int y, int w, int h, float k) {
  if (x < 0) {
    w += x;
    x = 0;
  }
  if (y < s.oy) {
    h -= s.oy - y;
    y = s.oy;
  }
  if (x + w > s.w) w = s.w - x;
  if (y + h > s.oy + s.h) h = s.oy + s.h - y;
  if (w <= 0 || h <= 0 || k >= 1.0f) return;
  if (k <= 0.0f) {
    for (int yy = y; yy < y + h; yy++) memset(rowAt(s, yy) + x, 0, static_cast<size_t>(w) * 2);
    return;
  }
  for (int yy = y; yy < y + h; yy++) {
    uint16_t* row = rowAt(s, yy);
    for (int xx = x; xx < x + w; xx++) {
      if (row[xx] == 0) continue;
      row[xx] = pack(scale(unpack(row[xx]), k));
    }
  }
}

void ringArc(Surface& s, float cx, float cy, float r, float width, float a0, float a1, uint32_t c0, uint32_t c1,
             float alpha) {
  // The analytic arc costs a few atan2 per row now, no per-pixel cache needed any more.
  arc(s, cx, cy, r, width, a0, a1, c0, c1, alpha);
}

}  // namespace monav::gfx
