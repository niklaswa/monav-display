#include "ui/rotate.h"

#include <cmath>
#include <cstring>

namespace monav::gfx {

namespace {

// A hair inside the last pixel centre, so a rounded source index never leaves the frame.
constexpr float kInset = 0.1f;

}  // namespace

int normalizeRotation(int degrees) {
  int d = degrees % 360;
  return d < 0 ? d + 360 : d;
}

Rotation::Rotation(int deg) : degrees(normalizeRotation(deg)) {
  const double a = degrees * M_PI / 180.0;
  cos16 = static_cast<int32_t>(std::lround(std::cos(a) * 65536.0));
  sin16 = static_cast<int32_t>(std::lround(std::sin(a) * 65536.0));
}

// The pixel at (x, y) on the glass shows the upright frame's pixel at
//   sx = c + dx·cos + dy·sin,  sy = c − dx·sin + dy·cos   (dx, dy from the centre c)
// which is the frame turned clockwise by the angle. Along a row both step by a constant.
void rotateRows(const uint16_t* src, int w, int h, const Rotation& r, int y0, int rows, uint16_t* dst) {
  const float cx = (w - 1) * 0.5f, cy = (h - 1) * 0.5f;
  const float rad = (w < h ? w : h) * 0.5f - 0.5f - kInset;
  const float c = r.cos16 / 65536.0f, s = r.sin16 / 65536.0f;
  for (int row = 0; row < rows; row++) {
    uint16_t* out = dst + row * w;
    const float dy = static_cast<float>(y0 + row) - cy;
    const float span2 = rad * rad - dy * dy;
    if (span2 < 0) {
      memset(out, 0, static_cast<size_t>(w) * 2);
      continue;
    }
    const float half = std::sqrt(span2);
    int x0 = static_cast<int>(std::ceil(cx - half));
    int x1 = static_cast<int>(std::floor(cx + half));
    if (x0 < 0) x0 = 0;
    if (x1 > w - 1) x1 = w - 1;
    memset(out, 0, static_cast<size_t>(x0) * 2);
    memset(out + x1 + 1, 0, static_cast<size_t>(w - 1 - x1) * 2);

    const float dx = static_cast<float>(x0) - cx;
    // +0.5 so the shift rounds to the nearest pixel (the values are never negative).
    int32_t sx = static_cast<int32_t>((cx + dx * c + dy * s + 0.5f) * 65536.0f);
    int32_t sy = static_cast<int32_t>((cy - dx * s + dy * c + 0.5f) * 65536.0f);
    const int32_t stepX = r.cos16, stepY = -r.sin16;
    uint16_t* o = out + x0;
    for (int x = x0; x <= x1; x++) {
      *o++ = src[(sy >> 16) * w + (sx >> 16)];
      sx += stepX;
      sy += stepY;
    }
  }
}

void unrotatePoint(const Rotation& r, int w, int h, int& x, int& y) {
  if (r.none()) return;
  const float cx = (w - 1) * 0.5f, cy = (h - 1) * 0.5f;
  const float c = r.cos16 / 65536.0f, s = r.sin16 / 65536.0f;
  const float dx = x - cx, dy = y - cy;
  x = static_cast<int>(std::lround(cx + dx * c + dy * s));
  y = static_cast<int>(std::lround(cy - dx * s + dy * c));
}

}  // namespace monav::gfx
