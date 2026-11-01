#pragma once

// Anti-aliased shapes drawn straight into a LovyanGFX 16-bit sprite buffer.
//
// LovyanGFX has smooth circles and wide lines but no smooth arcs or polygons, and its per-pixel
// calls are slow on a PSRAM sprite. These primitives work on the raw buffer (RGB565, byte-swapped
// the way LovyanGFX stores it) and compute coverage from signed distances, so every edge is
// anti-aliased the same way on the device and in the host preview.
//
// Angles are in degrees, 0 at 12 o'clock, growing clockwise.

#include <cstdint>

namespace monav::gfx {

// Inline rounding and min/max: libm's floorf/ceilf/fminf/fmaxf are real calls on the ESP32-S3
// (no single instruction), and the span loops call them several times per row.
inline int ifloor(float v) {
  const int i = static_cast<int>(v);
  return i > v ? i - 1 : i;
}
inline int iceil(float v) {
  const int i = static_cast<int>(v);
  return i < v ? i + 1 : i;
}
inline int iround(float v) { return ifloor(v + 0.5f); }
inline float fminf_(float a, float b) { return a < b ? a : b; }
inline float fmaxf_(float a, float b) { return a > b ? a : b; }
inline float fabsf_(float a) { return a < 0 ? -a : a; }

constexpr uint32_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | b;
}

/** Linear mix: t = 0 → a, t = 1 → b. */
uint32_t mix(uint32_t a, uint32_t b, float t);
/** Brightness multiply (0..1). */
uint32_t scale(uint32_t c, float k);

/**
 * What is drawn into: a band of rows of the 466×466 canvas (or all of it). Coordinates are always
 * canvas coordinates; `oy` is the canvas row the band's first row stands for, `h` its row count.
 * Drawing into internal RAM band by band is several times faster than into the PSRAM frame.
 */
struct Surface {
  uint16_t* px = nullptr;
  int w = 0;
  int h = 0;
  /** Multiplies every colour drawn (the HUD dims while the link is lost). */
  float dim = 1.0f;
  int oy = 0;  // canvas row of px[0]
  /** Which of the two drawing tasks owns this band (each has its own scratch buffers). */
  uint8_t lane = 0;
};

/** The row of canvas line y inside the band (y must be within it). */
inline uint16_t* rowAt(const Surface& s, int y) { return s.px + (y - s.oy) * s.w; }
inline bool rowsIn(const Surface& s, float top, float bottom) { return bottom >= s.oy && top < s.oy + s.h; }

inline uint16_t pack(uint32_t c) {
  const uint16_t v = static_cast<uint16_t>(((c >> 8) & 0xF800) | ((c >> 5) & 0x07E0) | ((c >> 3) & 0x001F));
  return static_cast<uint16_t>((v >> 8) | (v << 8));
}

inline uint32_t unpack(uint16_t s) {
  const uint16_t v = static_cast<uint16_t>((s >> 8) | (s << 8));
  const uint32_t r = (v >> 11) & 0x1F, g = (v >> 5) & 0x3F, b = v & 0x1F;
  return ((r << 3 | r >> 2) << 16) | ((g << 2 | g >> 4) << 8) | (b << 3 | b >> 2);
}

/** The colour as it will be drawn on `s` (dim applied). */
uint32_t shade(const Surface& s, uint32_t c);

/** Blend colour `c` (already shaded) over pixel (x, y) with coverage a (0..255). */
void blendPixel(Surface& s, int x, int y, uint32_t c, int a);

void clear(Surface& s, uint32_t c = 0);

void fillCircle(Surface& s, float cx, float cy, float r, uint32_t c, float alpha = 1.0f);
void strokeCircle(Surface& s, float cx, float cy, float r, float width, uint32_t c, float alpha = 1.0f);

/**
 * Arc along radius r with the given stroke width and round caps, from a0 to a1 (degrees,
 * clockwise, a1 > a0). The colour runs from c0 at a0 to c1 at a1.
 */
void arc(Surface& s, float cx, float cy, float r, float width, float a0, float a1, uint32_t c0, uint32_t c1,
         float alpha = 1.0f);

/** Thick line with round caps. */
void line(Surface& s, float x0, float y0, float x1, float y1, float width, uint32_t c, float alpha = 1.0f);
/** Thick polyline with round joins and caps (xy = x0, y0, x1, y1, ...). */
void polyline(Surface& s, const float* xy, int points, float width, uint32_t c, float alpha = 1.0f);
/**
 * Many thick lines as one shape (their union, anti-aliased once): begin, add, flush with the colour.
 * Used for the route; nothing else may draw lines in between.
 */
void beginLines(const Surface& s);
void addLine(const Surface& s, float x0, float y0, float x1, float y1, float width);
void flushLines(Surface& s, uint32_t c, float alpha = 1.0f);

/** Filled convex polygon (either winding). */
void convex(Surface& s, const float* xy, int points, uint32_t c, float alpha = 1.0f);

void fillRoundRect(Surface& s, float x, float y, float w, float h, float r, uint32_t c, float alpha = 1.0f);
void strokeRoundRect(Surface& s, float x, float y, float w, float h, float r, float width, uint32_t c,
                     float alpha = 1.0f);

/**
 * A soft band inside the glass edge: transparent at radius r0, `alpha` at r1 and beyond
 * (up to the edge at rEdge). Used for the red pulse when a camera is close. Writes the pixels
 * instead of blending them: draw it first, onto the cleared (black) frame.
 */
void rimGlow(Surface& s, float cx, float cy, float r0, float r1, float rEdge, uint32_t c, float alpha);
/** Builds rimGlow's table up front (before two tasks draw bands at once). */
void prepareGlow(float r0, float r1, float rEdge);

/** Black outside the circle (the preview's glass mask). */
void maskCircle(Surface& s, float cx, float cy, float r);

/** Multiply every pixel inside the rectangle by k (dim a region after drawing it). */
void darken(Surface& s, int x, int y, int w, int h, float k);

/** Same as arc() (kept for the call sites that draw the big rings). */
void ringArc(Surface& s, float cx, float cy, float r, float width, float a0, float a1, uint32_t c0, uint32_t c1,
             float alpha = 1.0f);

}  // namespace monav::gfx
