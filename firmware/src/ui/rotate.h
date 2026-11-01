#pragma once

// Turns the finished frame on its way to the panel, so the picture stands upright however the
// display is mounted. The renderer always draws upright; the push samples the frame turned by the
// user's angle (nearest pixel), row band by row band. Only the round glass is filled: the source
// of every pixel inside the circle lies inside the frame for any angle, outside it stays black.
//
// Portable, so the host preview turns its PNGs the same way.

#include <cstdint>

namespace monav::gfx {

/** 0..359, clockwise as seen on the glass. Anything else is wrapped into that range. */
int normalizeRotation(int degrees);

struct Rotation {
  int degrees = 0;  // normalized
  int32_t cos16 = 1 << 16, sin16 = 0;  // 16.16

  explicit Rotation(int deg = 0);
  bool none() const { return degrees == 0; }
};

/**
 * Fills `rows` rows of the panel starting at panel row `y0` into `dst` (w pixels per row) from
 * the upright frame `src` (w × h), turned clockwise by `r`.
 */
void rotateRows(const uint16_t* src, int w, int h, const Rotation& r, int y0, int rows, uint16_t* dst);

/**
 * A touch on the glass at (x, y) back into the upright frame's coordinates (the inverse turn),
 * so gestures mean the same whichever way the display hangs.
 */
void unrotatePoint(const Rotation& r, int w, int h, int& x, int& y);

}  // namespace monav::gfx
