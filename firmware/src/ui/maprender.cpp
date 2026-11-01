#include "ui/maprender.h"

#include <cmath>

#include "core/nav.h"

namespace monav::ui {
namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kDeg = kPi / 180.0f;
constexpr float kGlassR = 233.0f;
constexpr float kGlassC = 233.0f;

inline uint16_t bswap(uint16_t v) { return static_cast<uint16_t>((v >> 8) | (v << 8)); }

/** Scales a byte-swapped RGB565 pixel by f/32. */
inline uint16_t darken(uint16_t v, uint32_t f) {
  const uint32_t n = bswap(v);
  const uint32_t r = ((n >> 11) * f) >> 5;
  const uint32_t g = (((n >> 5) & 0x3F) * f) >> 5;
  const uint32_t b = ((n & 0x1F) * f) >> 5;
  return bswap(static_cast<uint16_t>((r << 11) | (g << 5) | b));
}

float smooth(float t) {
  t = t < 0 ? 0 : t > 1 ? 1 : t;
  return t * t * (3 - 2 * t);
}

struct Basis {
  float fx, fy;  // forward (driving direction) in map pixels
  float rx, ry;  // right
};

Basis basis(float hdg) {
  const float th = hdg * kDeg;
  const float s = std::sin(th), c = std::cos(th);
  return Basis{s, -c, c, s};
}

}  // namespace

bool carOnImage(const MapImage& m, double lat, double lon, float& mx, float& my) {
  if (m.px == nullptr || m.w <= 0 || m.h <= 0 || m.mpp <= 0) return false;
  double e, n;
  geoToLocal(m.lat, m.lon, lat, lon, e, n);
  mx = static_cast<float>(m.w / 2.0 + e / m.mpp);
  my = static_cast<float>(m.h / 2.0 - n / m.mpp);
  // Some picture is needed ahead of the car: off the image, or right at its edge, counts as out.
  const float margin = 24.0f;
  return mx >= margin && my >= margin && mx < m.w - margin && my < m.h - margin;
}

namespace {

/**
 * The tiled index split into a part from the row and a part from the column, looked up instead of
 * computed per pixel: index = rowPart[v] + colPart[u] (core/mapdata.h).
 */
struct TileTables {
  uint32_t w = 0;
  uint32_t row[1024];
  uint32_t col[1024];
};
TileTables s_tiles;

const TileTables& tileTables(uint32_t w, uint32_t h) {
  if (s_tiles.w != w) {
    const uint32_t tileRow = mapTilesPerRow(w) << 8;
    for (uint32_t v = 0; v < 1024; v++) s_tiles.row[v] = (v >> 4) * tileRow + ((v & 15) << 4);
    for (uint32_t u = 0; u < 1024; u++) s_tiles.col[u] = ((u >> 4) << 8) + (u & 15);
    s_tiles.w = w;
  }
  (void)h;
  return s_tiles;
}

}  // namespace

void prepareMap(const MapImage& m) { tileTables(static_cast<uint32_t>(m.w), static_cast<uint32_t>(m.h)); }

void drawMap(gfx::Surface& s, const MapImage& m, const MapCamera& cam, float topDark, float bottomDark) {
  const Basis b = basis(cam.hdg);
  const uint32_t w = static_cast<uint32_t>(m.w), h = static_cast<uint32_t>(m.h);
  const uint16_t* tex = m.px;
  const float depth = cam.cy - MapCamera::kHorizon;
  const uint32_t tileRow = mapTilesPerRow(w) << 8;  // pixels from one row of tiles to the next
  const TileTables& tt = tileTables(w, h);
  const uint32_t* rowPart = tt.row;
  const uint32_t* colPart = tt.col;

  for (int y = s.oy; y < s.oy + s.h; y++) {
    const float fyc = y + 0.5f - kGlassC;
    if (gfx::fabsf_(fyc) >= kGlassR) continue;
    const float half = std::sqrt(kGlassR * kGlassR - fyc * fyc);
    int x0 = gfx::ifloor(kGlassC - half);
    int x1 = gfx::iceil(kGlassC + half);
    if (x0 < 0) x0 = 0;
    if (x1 > s.w) x1 = s.w;
    const int n = x1 - x0;
    if (n <= 0) continue;

    // One scale per scanline: k map pixels per glass pixel, F map pixels ahead of the car.
    const float py = y + 0.5f;
    float k, ahead;
    if (cam.tilt) {
      k = depth / (py - MapCamera::kHorizon);
      ahead = MapCamera::kForward * depth * (k - 1.0f);
    } else {
      k = 1.0f;
      ahead = cam.cy - py;
    }
    const float dx0 = (x0 + 0.5f) - cam.cx;
    const float mx = cam.carX + b.fx * ahead + b.rx * k * dx0;
    const float my = cam.carY + b.fy * ahead + b.ry * k * dx0;
    int32_t u = static_cast<int32_t>(mx * 65536.0f);
    int32_t v = static_cast<int32_t>(my * 65536.0f);
    const int32_t du = static_cast<int32_t>(b.rx * k * 65536.0f);
    const int32_t dv = static_cast<int32_t>(b.ry * k * 65536.0f);

    // Darker under the overlays at the top (and, if asked, the bottom).
    float f = s.dim;
    if (topDark > 0 && py < 150.0f) f *= 1.0f - topDark * (1.0f - smooth(py / 150.0f));
    if (bottomDark > 0 && py > 340.0f) f *= 1.0f - bottomDark * smooth((py - 340.0f) / 126.0f);
    const uint32_t f32 = static_cast<uint32_t>(f * 32.0f + 0.5f);

    uint16_t* dst = gfx::rowAt(s, y) + x0;
    // Both ends on the image: the samples in between are too (they move on a straight line).
    const int32_t ue = u + du * (n - 1), ve = v + dv * (n - 1);
    const bool inside = static_cast<uint32_t>(u >> 16) < w && static_cast<uint32_t>(v >> 16) < h &&
                        static_cast<uint32_t>(ue >> 16) < w && static_cast<uint32_t>(ve >> 16) < h;
    if (inside && f32 >= 32) {
      for (int i = 0; i < n; i++) {
        dst[i] = tex[rowPart[static_cast<uint32_t>(v) >> 16] + colPart[static_cast<uint32_t>(u) >> 16]];
        u += du;
        v += dv;
      }
    } else if (f32 >= 32) {
      const uint16_t bg = m.bg;
      for (int i = 0; i < n; i++) {
        const uint32_t tu = static_cast<uint32_t>(u >> 16), tv = static_cast<uint32_t>(v >> 16);
        dst[i] = (tu < w && tv < h) ? tex[(tv >> 4) * tileRow + ((tu >> 4) << 8) + ((tv & 15) << 4) + (tu & 15)] : bg;
        u += du;
        v += dv;
      }
    } else {
      const uint16_t bg = darken(m.bg, f32);
      for (int i = 0; i < n; i++) {
        const uint32_t tu = static_cast<uint32_t>(u >> 16), tv = static_cast<uint32_t>(v >> 16);
        dst[i] = (tu < w && tv < h)
                     ? darken(tex[(tv >> 4) * tileRow + ((tu >> 4) << 8) + ((tv & 15) << 4) + (tu & 15)], f32)
                     : bg;
        u += du;
        v += dv;
      }
    }
  }
}

bool project(const MapCamera& cam, float mx, float my, float& sx, float& sy, float& scale) {
  const Basis b = basis(cam.hdg);
  const float relx = mx - cam.carX, rely = my - cam.carY;
  const float ahead = relx * b.fx + rely * b.fy;
  const float side = relx * b.rx + rely * b.ry;
  if (!cam.tilt) {
    sx = cam.cx + side;
    sy = cam.cy - ahead;
    scale = 1.0f;
    return true;
  }
  const float depth = cam.cy - MapCamera::kHorizon;
  const float k = 1.0f + ahead / (MapCamera::kForward * depth);
  if (k < 0.2f) return false;  // too far behind the camera
  sx = cam.cx + side / k;
  sy = MapCamera::kHorizon + depth / k;
  scale = 1.0f / k;
  return true;
}

namespace {

/** The route projected onto the glass, once per frame (the bands of a frame share it). */
struct ProjectedRoute {
  static constexpr int kMax = 700;
  const uint16_t* map = nullptr;
  const int16_t* pts = nullptr;
  int count = 0;
  float carX = 0, carY = 0, hdg = 0;
  bool tilt = false;
  int n = 0;
  float x[kMax], y[kMax], scale[kMax];
  bool ok[kMax];
};

ProjectedRoute s_proj;

/** Distance of point i from the line through a and b (glass pixels). */
float offLine(const ProjectedRoute& p, int a, int b, int i) {
  const float vx = p.x[b] - p.x[a], vy = p.y[b] - p.y[a];
  const float l = std::sqrt(vx * vx + vy * vy);
  if (l < 1e-3f) return std::hypot(p.x[i] - p.x[a], p.y[i] - p.y[a]);
  return gfx::fabsf_((p.x[i] - p.x[a]) * vy - (p.y[i] - p.y[a]) * vx) / l;
}

/**
 * Drops the points that lie within half a pixel of the line through their neighbours
 * (Douglas-Peucker on the glass): a straight road of thirty 10 m points becomes one segment,
 * which is what makes drawing the line cheap.
 */
void simplify(ProjectedRoute& p) {
  if (p.n < 3) return;
  static bool keep[ProjectedRoute::kMax];
  static int16_t stack[ProjectedRoute::kMax * 2];
  for (int i = 0; i < p.n; i++) keep[i] = false;
  int sp = 0;
  // Runs of points that projected (behind the camera ones split the line).
  int runStart = 0;
  for (int i = 0; i <= p.n; i++) {
    if (i < p.n && p.ok[i]) continue;
    const int a = runStart, b = i - 1;
    runStart = i + 1;
    if (b < a) continue;
    keep[a] = keep[b] = true;
    stack[sp++] = static_cast<int16_t>(a);
    stack[sp++] = static_cast<int16_t>(b);
    while (sp > 0) {
      const int hi = stack[--sp], lo = stack[--sp];
      float worst = 0;
      int at = -1;
      for (int k = lo + 1; k < hi; k++) {
        const float d = offLine(p, lo, hi, k);
        if (d > worst) {
          worst = d;
          at = k;
        }
      }
      if (at >= 0 && worst > 0.5f) {
        keep[at] = true;
        stack[sp++] = static_cast<int16_t>(lo);
        stack[sp++] = static_cast<int16_t>(at);
        stack[sp++] = static_cast<int16_t>(at);
        stack[sp++] = static_cast<int16_t>(hi);
      }
    }
  }
  int n = 0;
  for (int i = 0; i < p.n; i++) {
    if (!p.ok[i] || keep[i]) {
      p.x[n] = p.x[i];
      p.y[n] = p.y[i];
      p.scale[n] = p.scale[i];
      p.ok[n] = p.ok[i];
      n++;
    }
  }
  p.n = n;
}

void projectRoute(const MapImage& m, const RouteLine& r, const MapCamera& cam) {
  ProjectedRoute& p = s_proj;
  if (p.map == m.px && p.pts == r.pts && p.count == r.count && p.carX == cam.carX && p.carY == cam.carY &&
      p.hdg == cam.hdg && p.tilt == cam.tilt) {
    return;  // same frame, another band
  }
  p.map = m.px;
  p.pts = r.pts;
  p.count = r.count;
  p.carX = cam.carX;
  p.carY = cam.carY;
  p.hdg = cam.hdg;
  p.tilt = cam.tilt;
  p.n = 0;

  double oe, on;
  geoToLocal(m.lat, m.lon, r.lat, r.lon, oe, on);
  const float cx0 = m.w / 2.0f, cy0 = m.h / 2.0f;
  auto toMap = [&](int i, float& x, float& y) {
    x = cx0 + static_cast<float>((r.pts[2 * i] + oe) / m.mpp);
    y = cy0 - static_cast<float>((r.pts[2 * i + 1] + on) / m.mpp);
  };

  // The point of the line nearest the car: the line starts there (what is driven is cut off).
  int seg = 0;
  float best = 1e30f, bestT = 0;
  for (int i = 0; i + 1 < r.count; i++) {
    float ax, ay, bx, by;
    toMap(i, ax, ay);
    toMap(i + 1, bx, by);
    const float vx = bx - ax, vy = by - ay;
    const float l2 = vx * vx + vy * vy;
    float t = l2 > 0 ? ((cam.carX - ax) * vx + (cam.carY - ay) * vy) / l2 : 0;
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    const float dx = ax + vx * t - cam.carX, dy = ay + vy * t - cam.carY;
    const float d2 = dx * dx + dy * dy;
    if (d2 < best) {
      best = d2;
      seg = i;
      bestT = t;
    }
  }
  float ax, ay, bx, by;
  toMap(seg, ax, ay);
  toMap(seg + 1, bx, by);
  p.ok[0] = project(cam, ax + (bx - ax) * bestT, ay + (by - ay) * bestT, p.x[0], p.y[0], p.scale[0]);
  p.n = 1;
  for (int i = seg + 1; i < r.count && p.n < ProjectedRoute::kMax; i++) {
    float mx, my;
    toMap(i, mx, my);
    p.ok[p.n] = project(cam, mx, my, p.x[p.n], p.y[p.n], p.scale[p.n]);
    p.n++;
  }
  simplify(p);
}

}  // namespace

void prepareRoute(const MapImage& m, const RouteLine& r, const MapCamera& cam) {
  if (r.pts == nullptr || r.count < 2 || m.mpp <= 0) return;
  projectRoute(m, r, cam);
}

void drawRoute(gfx::Surface& s, const MapImage& m, const RouteLine& r, const MapCamera& cam) {
  if (r.pts == nullptr || r.count < 2 || m.mpp <= 0) return;
  projectRoute(m, r, cam);
  const ProjectedRoute& p = s_proj;
  const uint32_t casing = gfx::rgb(0x06, 0x22, 0x4A);
  const uint32_t line = gfx::rgb(0x2E, 0x8E, 0xFF);
  // Casing first, then the line: each pass is one shape (no seams where segments meet).
  for (int pass = 0; pass < 2; pass++) {
    gfx::beginLines(s);
    for (int i = 0; i + 1 < p.n; i++) {
      if (!p.ok[i] || !p.ok[i + 1]) continue;
      const float sc = 0.5f * (p.scale[i] + p.scale[i + 1]);
      const float core = gfx::fmaxf_(2.5f, 7.0f * sc);
      const float pad = core * 0.5f + 3.0f;
      // Only segments that reach into this band (and onto the glass).
      const float miny = gfx::fminf_(p.y[i], p.y[i + 1]) - pad, maxy = gfx::fmaxf_(p.y[i], p.y[i + 1]) + pad;
      if (!gfx::rowsIn(s, miny, maxy)) continue;
      const float minx = gfx::fminf_(p.x[i], p.x[i + 1]), maxx = gfx::fmaxf_(p.x[i], p.x[i + 1]);
      if (maxx < -pad || minx > s.w + pad) continue;
      gfx::addLine(s, p.x[i], p.y[i], p.x[i + 1], p.y[i + 1], pass == 0 ? core + 4.0f : core);
    }
    gfx::flushLines(s, pass == 0 ? casing : line);
  }
}

void drawCar(gfx::Surface& s, float cx, float cy) {
  gfx::fillCircle(s, cx, cy + 2, 21, 0x000000, 0.35f);
  gfx::fillCircle(s, cx, cy, 16.5f, 0xFFFFFF);
  gfx::fillCircle(s, cx, cy, 13.5f, gfx::rgb(0x0A, 0x84, 0xFF));
  // The arrow: two triangles meeting at the notch, as one shape (no seam down the middle).
  const float tipY = cy - 8.5f, wingY = cy + 7.0f, notchY = cy + 3.0f, wing = 7.0f;
  const float tri[2][6] = {{cx, tipY, cx + wing, wingY, cx, notchY}, {cx, tipY, cx, notchY, cx - wing, wingY}};
  auto sdTri = [](const float* t, float x, float y) {
    float area = 0;
    for (int i = 0; i < 3; i++) {
      const int j = (i + 1) % 3;
      area += t[2 * i] * t[2 * j + 1] - t[2 * j] * t[2 * i + 1];
    }
    const float wind = area > 0 ? 1.0f : -1.0f;
    float sd = -1e9f;
    for (int i = 0; i < 3; i++) {
      const int j = (i + 1) % 3;
      const float ex = t[2 * j] - t[2 * i], ey = t[2 * j + 1] - t[2 * i + 1];
      const float l = std::sqrt(ex * ex + ey * ey);
      const float d = ((x - t[2 * i]) * wind * ey - (y - t[2 * i + 1]) * wind * ex) / l;
      if (d > sd) sd = d;
    }
    return sd;
  };
  const uint32_t white = gfx::shade(s, 0xFFFFFF);
  for (int y = static_cast<int>(tipY) - 1; y <= static_cast<int>(wingY) + 1; y++) {
    for (int x = static_cast<int>(cx - wing) - 1; x <= static_cast<int>(cx + wing) + 1; x++) {
      const float sd = gfx::fminf_(sdTri(tri[0], x + 0.5f, y + 0.5f), sdTri(tri[1], x + 0.5f, y + 0.5f));
      const float c = 0.5f - sd;
      if (c <= 0) continue;
      gfx::blendPixel(s, x, y, white, c >= 1 ? 255 : static_cast<int>(c * 255));
    }
  }
}

}  // namespace monav::ui
