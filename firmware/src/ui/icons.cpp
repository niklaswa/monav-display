#include "ui/icons.h"

#include <cmath>

namespace monav::ui::icons {
namespace {

using gfx::Surface;

constexpr float kPi = 3.14159265358979f;
constexpr float kDeg = kPi / 180.0f;

struct P {
  float x, y;
};

P sub(P a, P b) { return {a.x - b.x, a.y - b.y}; }
P add(P a, P b) { return {a.x + b.x, a.y + b.y}; }
P mul(P a, float k) { return {a.x * k, a.y * k}; }
float len(P a) { return std::sqrt(a.x * a.x + a.y * a.y); }
P norm(P a) {
  const float l = len(a);
  return l > 0 ? P{a.x / l, a.y / l} : P{0, 0};
}

/**
 * Replaces every inner corner of a path with a rounded one (quadratic curve through the corner,
 * tangent to both legs). Returns the number of points written to `out`.
 */
int rounded(const P* in, int n, float radius, P* out, int cap) {
  int m = 0;
  out[m++] = in[0];
  for (int i = 1; i + 1 < n && m + 8 < cap; i++) {
    const P a = in[i - 1], b = in[i], c = in[i + 1];
    const P d1 = norm(sub(a, b)), d2 = norm(sub(c, b));
    const float cosT = d1.x * d2.x + d1.y * d2.y;
    const float theta = std::acos(cosT < -1 ? -1 : cosT > 1 ? 1 : cosT);
    float t = theta > 0.01f ? radius / std::tan(theta * 0.5f) : 0;
    const float lim = 0.5f * std::fmin(len(sub(a, b)), len(sub(c, b)));
    if (t > lim) t = lim;
    const P t1 = add(b, mul(d1, t)), t2 = add(b, mul(d2, t));
    constexpr int kSteps = 7;
    for (int k = 0; k <= kSteps; k++) {
      const float u = static_cast<float>(k) / kSteps;
      const float iu = 1 - u;
      out[m++] = {iu * iu * t1.x + 2 * iu * u * b.x + u * u * t2.x, iu * iu * t1.y + 2 * iu * u * b.y + u * u * t2.y};
    }
  }
  out[m++] = in[n - 1];
  return m;
}

/** Path in unit coordinates (box ±1), scaled to `size` around (cx, cy). */
void toScreen(const P* pts, int n, float cx, float cy, float size, float* xy) {
  for (int i = 0; i < n; i++) {
    xy[2 * i] = cx + pts[i].x * size;
    xy[2 * i + 1] = cy + pts[i].y * size;
  }
}

/** Rounded path ending in an arrowhead at its last point. */
void arrow(Surface& s, float cx, float cy, float size, const P* ctrl, int n, float width, float corner, uint32_t c,
           float headLen = 0.5f, float headHalf = 0.44f) {
  P path[64];
  const int m = rounded(ctrl, n, corner, path, 64);
  const P tip = path[m - 1];
  // Direction of the last leg (the last control segment, not the curve's tiny last step).
  const P dir = norm(sub(ctrl[n - 1], ctrl[n - 2]));
  const P base = sub(tip, mul(dir, headLen));
  P shaft[64];
  int k = 0;
  for (int i = 0; i < m - 1; i++) shaft[k++] = path[i];
  shaft[k++] = add(base, mul(dir, 0.08f));
  float xy[128];
  toScreen(shaft, k, cx, cy, size, xy);
  gfx::polyline(s, xy, k, width, c);
  const P nrm{-dir.y, dir.x};
  const P head[3] = {add(tip, mul(dir, 0.04f)), add(base, mul(nrm, headHalf)), sub(base, mul(nrm, headHalf))};
  float hxy[6];
  toScreen(head, 3, cx, cy, size, hxy);
  gfx::convex(s, hxy, 3, c);
}

void path(Surface& s, float cx, float cy, float size, const P* ctrl, int n, float width, float corner, uint32_t c) {
  P pts[64];
  const int m = rounded(ctrl, n, corner, pts, 64);
  float xy[128];
  toScreen(pts, m, cx, cy, size, xy);
  gfx::polyline(s, xy, m, width, c);
}

void mirror(const P* in, int n, P* out) {
  for (int i = 0; i < n; i++) out[i] = {-in[i].x, in[i].y};
}

}  // namespace

void camera(Surface& s, float cx, float cy, float size, CamKind kind, uint32_t fg, uint32_t bg) {
  if (kind == CamKind::Section) {
    // Average speed check: a span between two posts.
    const float w = size * 0.17f;
    gfx::line(s, cx - size * 0.82f, cy - size * 0.5f, cx - size * 0.82f, cy + size * 0.5f, w, fg);
    gfx::line(s, cx + size * 0.82f, cy - size * 0.5f, cx + size * 0.82f, cy + size * 0.5f, w, fg);
    const P l[2] = {{-0.15f, 0}, {-0.6f, 0}};
    const P r[2] = {{0.15f, 0}, {0.6f, 0}};
    arrow(s, cx, cy, size, l, 2, w, 0, fg, 0.3f, 0.3f);
    arrow(s, cx, cy, size, r, 2, w, 0, fg, 0.3f, 0.3f);
    return;
  }
  if (kind == CamKind::Distance) {
    // Distance check: the gap between two cars.
    const float w = size * 0.17f;
    gfx::line(s, cx - size * 0.6f, cy - size * 0.78f, cx + size * 0.6f, cy - size * 0.78f, w, fg);
    gfx::line(s, cx - size * 0.6f, cy + size * 0.78f, cx + size * 0.6f, cy + size * 0.78f, w, fg);
    const P u[2] = {{0, -0.1f}, {0, -0.55f}};
    const P d[2] = {{0, 0.1f}, {0, 0.55f}};
    arrow(s, cx, cy, size, u, 2, w, 0, fg, 0.3f, 0.3f);
    arrow(s, cx, cy, size, d, 2, w, 0, fg, 0.3f, 0.3f);
    return;
  }
  if (kind == CamKind::RedLight) {
    // A small traffic light: the camera that watches it.
    const float w = size * 0.62f, h = size * 1.7f;
    gfx::fillRoundRect(s, cx - w / 2, cy - h / 2, w, h, w / 2, fg);
    gfx::fillCircle(s, cx, cy - h * 0.29f, w * 0.3f, bg);
    gfx::fillCircle(s, cx, cy, w * 0.3f, bg);
    gfx::fillCircle(s, cx, cy + h * 0.29f, w * 0.3f, bg);
    gfx::fillCircle(s, cx, cy - h * 0.29f, w * 0.2f, fg);
    return;
  }

  float oy = 0;
  if (kind == CamKind::Mobile) {
    oy = -size * 0.2f;
    // Tripod legs.
    const float w = size * 0.13f;
    gfx::line(s, cx, cy + size * 0.35f, cx - size * 0.5f, cy + size * 0.92f, w, fg);
    gfx::line(s, cx, cy + size * 0.35f, cx + size * 0.5f, cy + size * 0.92f, w, fg);
    gfx::line(s, cx, cy + size * 0.35f, cx, cy + size * 0.92f, w, fg);
  }
  const float sz = kind == CamKind::Mobile ? size * 0.82f : size;
  const float bw = sz * 1.7f, bh = sz * 1.08f;
  const float by = cy + oy - bh * 0.42f;
  gfx::fillRoundRect(s, cx - sz * 0.32f, by - sz * 0.22f, sz * 0.64f, sz * 0.4f, sz * 0.1f, fg);
  gfx::fillRoundRect(s, cx - bw / 2, by, bw, bh, sz * 0.2f, fg);
  const float ly = by + bh * 0.52f;
  gfx::fillCircle(s, cx, ly, sz * 0.36f, bg);
  gfx::fillCircle(s, cx, ly, sz * 0.2f, fg);
}

void turn(Surface& s, float cx, float cy, float size, TurnKind kind, int exit, uint32_t fg, uint32_t muted,
          uint32_t bg) {
  const float w = size * 0.24f;
  const float corner = 0.32f;
  P m[8];
  switch (kind) {
    case TurnKind::Right:
    case TurnKind::Left: {
      const P p[3] = {{-0.32f, 0.88f}, {-0.32f, -0.12f}, {0.86f, -0.12f}};
      if (kind == TurnKind::Left) {
        mirror(p, 3, m);
        arrow(s, cx, cy, size, m, 3, w, corner, fg);
      } else {
        arrow(s, cx, cy, size, p, 3, w, corner, fg);
      }
      return;
    }
    case TurnKind::SlightRight:
    case TurnKind::SlightLeft: {
      const P p[3] = {{-0.22f, 0.88f}, {-0.22f, 0.22f}, {0.5f, -0.62f}};
      if (kind == TurnKind::SlightLeft) {
        mirror(p, 3, m);
        arrow(s, cx, cy, size, m, 3, w, corner, fg);
      } else {
        arrow(s, cx, cy, size, p, 3, w, corner, fg);
      }
      return;
    }
    case TurnKind::SharpRight:
    case TurnKind::SharpLeft: {
      const P p[3] = {{-0.5f, 0.88f}, {-0.5f, -0.5f}, {0.55f, 0.45f}};
      if (kind == TurnKind::SharpLeft) {
        mirror(p, 3, m);
        arrow(s, cx, cy, size, m, 3, w, 0.2f, fg, 0.42f, 0.38f);
      } else {
        arrow(s, cx, cy, size, p, 3, w, 0.2f, fg, 0.42f, 0.38f);
      }
      return;
    }
    case TurnKind::UTurn: {
      // Right-hand traffic turns back over the left.
      const P p[4] = {{0.38f, 0.88f}, {0.38f, -0.42f}, {-0.38f, -0.42f}, {-0.38f, 0.5f}};
      arrow(s, cx, cy, size, p, 4, w, 0.38f, fg);
      return;
    }
    case TurnKind::KeepRight:
    case TurnKind::KeepLeft: {
      P other[3] = {{0, 0.12f}, {-0.45f, -0.38f}, {-0.45f, -0.72f}};
      P main[4] = {{0, 0.88f}, {0, 0.12f}, {0.45f, -0.38f}, {0.45f, -0.86f}};
      if (kind == TurnKind::KeepLeft) {
        for (auto& q : other) q.x = -q.x;
        for (auto& q : main) q.x = -q.x;
      }
      path(s, cx, cy, size, other, 3, w * 0.8f, 0.25f, muted);
      arrow(s, cx, cy, size, main, 4, w, 0.25f, fg);
      return;
    }
    case TurnKind::Roundabout: {
      // Counter-clockwise (right-hand traffic): in from the bottom, round, out at the exit.
      const float r = 0.38f;
      const P c{0, 0.06f};
      const int n = exit <= 0 ? 2 : exit > 4 ? 4 : exit;
      // 1 right, 2 ahead, 3 left, 4 back (leaving to the lower left, clear of the way in).
      const float exitAngle = n == 4 ? -120.0f : 180.0f - 90.0f * static_cast<float>(n);
      gfx::strokeCircle(s, cx + c.x * size, cy + c.y * size, r * size, w * 0.5f, muted);
      gfx::line(s, cx, cy + 0.9f * size, cx, cy + (c.y + r) * size, w, fg);
      gfx::arc(s, cx + c.x * size, cy + c.y * size, r * size, w, exitAngle, 180.0f, fg, fg);
      const float ea = exitAngle * kDeg;
      const P dir{std::sin(ea), -std::cos(ea)};
      const P arm[2] = {add(c, mul(dir, r)), add(c, mul(dir, 0.98f))};
      arrow(s, cx, cy, size, arm, 2, w, 0, fg, 0.42f, 0.38f);
      return;
    }
    case TurnKind::Arrive: {
      // Destination pin.
      const float pr = 0.48f;
      const P tri[3] = {{-0.38f, -0.1f}, {0.38f, -0.1f}, {0, 0.9f}};
      float xy[6];
      toScreen(tri, 3, cx, cy, size, xy);
      gfx::convex(s, xy, 3, fg);
      gfx::fillCircle(s, cx, cy - 0.3f * size, pr * size, fg);
      gfx::fillCircle(s, cx, cy - 0.3f * size, 0.19f * size, bg);
      return;
    }
    case TurnKind::Straight:
    case TurnKind::Other:
    default: {
      const P p[2] = {{0, 0.88f}, {0, -0.88f}};
      arrow(s, cx, cy, size, p, 2, w, 0, fg);
      return;
    }
  }
}

void lane(Surface& s, float cx, float cy, float size, Lane l, uint32_t fg) {
  switch (l) {
    case Lane::Left:
      turn(s, cx, cy, size, TurnKind::Left, 0, fg, fg, 0);
      break;
    case Lane::Right:
      turn(s, cx, cy, size, TurnKind::Right, 0, fg, fg, 0);
      break;
    default:
      turn(s, cx, cy, size, TurnKind::Straight, 0, fg, fg, 0);
      break;
  }
}

void check(Surface& s, float cx, float cy, float size, float width, uint32_t fg, float progress) {
  if (progress <= 0) return;
  const P pts[3] = {{-0.5f, 0.02f}, {-0.14f, 0.38f}, {0.54f, -0.34f}};
  const float l1 = len(sub(pts[1], pts[0])), l2 = len(sub(pts[2], pts[1]));
  float want = (l1 + l2) * (progress > 1 ? 1 : progress);
  P out[3];
  int n = 1;
  out[0] = pts[0];
  if (want <= l1) {
    out[n++] = add(pts[0], mul(sub(pts[1], pts[0]), want / l1));
  } else {
    out[n++] = pts[1];
    want -= l1;
    out[n++] = add(pts[1], mul(sub(pts[2], pts[1]), want / l2));
  }
  float xy[6];
  toScreen(out, n, cx, cy, size, xy);
  gfx::polyline(s, xy, n, width, fg);
}

void bluetooth(Surface& s, float cx, float cy, float size, uint32_t fg) {
  const P pts[6] = {{-0.46f, -0.44f}, {0.42f, 0.42f}, {0, 0.86f}, {0, -0.86f}, {0.42f, -0.42f}, {-0.46f, 0.44f}};
  float xy[12];
  toScreen(pts, 6, cx, cy, size, xy);
  gfx::polyline(s, xy, 6, size * 0.15f, fg);
}

void bolt(Surface& s, float cx, float cy, float size, uint32_t fg) {
  // Two slanted wedges that overlap across the middle.
  const float upper[] = {cx + 0.30f * size, cy - size, cx - 0.55f * size, cy + 0.15f * size, cx + 0.10f * size,
                         cy + 0.15f * size};
  const float lower[] = {cx - 0.30f * size, cy + size, cx + 0.55f * size, cy - 0.15f * size, cx - 0.10f * size,
                         cy - 0.15f * size};
  gfx::convex(s, upper, 3, fg);
  gfx::convex(s, lower, 3, fg);
}

void sun(Surface& s, float cx, float cy, float size, uint32_t fg) {
  gfx::fillCircle(s, cx, cy, size * 0.36f, fg);
  for (int i = 0; i < 8; i++) {
    const float a = i * 45.0f * kDeg;
    const float sx = std::sin(a), sy = -std::cos(a);
    gfx::line(s, cx + sx * size * 0.62f, cy + sy * size * 0.62f, cx + sx * size * 0.9f, cy + sy * size * 0.9f,
              size * 0.14f, fg);
  }
}

void phone(Surface& s, float cx, float cy, float size, uint32_t fg) {
  const float w = size * 1.0f, h = size * 1.75f;
  gfx::strokeRoundRect(s, cx - w / 2, cy - h / 2, w, h, size * 0.22f, size * 0.13f, fg);
  gfx::line(s, cx - w * 0.16f, cy - h / 2 + size * 0.2f, cx + w * 0.16f, cy - h / 2 + size * 0.2f, size * 0.1f, fg);
}

void reset(Surface& s, float cx, float cy, float size, uint32_t fg) {
  const float r = size * 0.62f, w = size * 0.17f;
  gfx::arc(s, cx, cy, r, w, 70.0f, 330.0f, fg, fg);
  // Arrowhead at the start (70°), pointing back against the sweep.
  const float a = 70.0f * kDeg;
  const P tip{std::sin(a) * 0.62f, -std::cos(a) * 0.62f};
  const P tangent{-std::cos(a), -std::sin(a)};  // counter-clockwise
  const P base = sub(tip, mul(tangent, -0.05f));
  const P nrm{-tangent.y, tangent.x};
  const P head[3] = {add(base, mul(tangent, 0.42f)), add(base, mul(nrm, 0.3f)), sub(base, mul(nrm, 0.3f))};
  float xy[6];
  toScreen(head, 3, cx, cy, size, xy);
  gfx::convex(s, xy, 3, fg);
}

void chevrons(Surface& s, float cx, float cy, float size, bool down, uint32_t fg) {
  const float k = down ? 1.0f : -1.0f;
  for (int i = 0; i < 2; i++) {
    const float oy = (i == 0 ? -0.34f : 0.18f) * k;
    const P pts[3] = {{-0.55f, oy - 0.24f * k}, {0, oy + 0.24f * k}, {0.55f, oy - 0.24f * k}};
    float xy[6];
    toScreen(pts, 3, cx, cy, size, xy);
    gfx::polyline(s, xy, 3, size * 0.2f, fg);
  }
}

void trafficLight(Surface& s, float cx, float cy, float height, int lit, float glow) {
  const float w = height * 0.42f;
  const uint32_t lamps[3] = {gfx::rgb(0xFF, 0x45, 0x3A), gfx::rgb(0xFF, 0xD6, 0x0A), gfx::rgb(0x33, 0xD1, 0x73)};
  const uint32_t off[3] = {gfx::rgb(0x3A, 0x14, 0x12), gfx::rgb(0x36, 0x2E, 0x0C), gfx::rgb(0x0E, 0x30, 0x1C)};
  gfx::fillRoundRect(s, cx - w / 2, cy - height / 2, w, height, w / 2, gfx::rgb(0x16, 0x16, 0x18));
  gfx::strokeRoundRect(s, cx - w / 2, cy - height / 2, w, height, w / 2, 2.0f, gfx::rgb(0x34, 0x34, 0x38));
  const float r = height * 0.115f;
  for (int i = 0; i < 3; i++) {
    const float ly = cy + (i - 1) * height * 0.29f;
    if (i == lit) {
      if (glow > 0) {
        for (int k = 6; k >= 1; k--) {
          gfx::fillCircle(s, cx, ly, r + k * r * 0.32f, lamps[i], 0.07f * glow);
        }
      }
      gfx::fillCircle(s, cx, ly, r, lamps[i]);
      gfx::fillCircle(s, cx - r * 0.3f, ly - r * 0.3f, r * 0.32f, gfx::mix(lamps[i], 0xFFFFFF, 0.55f), 0.8f);
    } else {
      gfx::fillCircle(s, cx, ly, r, off[i]);
    }
  }
}

}  // namespace monav::ui::icons
