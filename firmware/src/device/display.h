#pragma once

// The panel and its frame pipeline: two full-frame sprites in PSRAM. The UI draws into one while
// a task on the other core pushes the other to the panel, started on the tearing-effect edge.

#include <LovyanGFX.hpp>

#include <cstdint>

namespace monav::device::display {

/** Brings up the panel (dark), allocates both frame buffers, starts the push task. */
bool begin();

/** The sprite to draw the next frame into (466×466, 16 bit, PSRAM). */
lgfx::LGFX_Sprite& backBuffer();

/** Hands the drawn frame to the push task and swaps buffers; waits while the previous push runs. */
void present();

/** True once a frame has reached the panel (part of a new firmware's health check). */
bool firstFramePushed();

/** Panel brightness 0–255 (CO5300 command 0x51), applied between two pushes. */
void setBrightness(uint8_t level);

/**
 * Turns the picture clockwise by this many degrees on its way to the panel (the display hangs
 * at an angle), from the next push on. Needs the DMA band push; the sprite push stays upright.
 */
void setRotation(int degrees);

struct Stats {
  uint32_t pushes = 0;
  uint32_t pushUsTotal = 0;
  uint32_t pushUsMax = 0;
  uint32_t teTimeouts = 0;
  bool teSync = false;
};
/** Push statistics since the last call (then reset). */
Stats takeStats();

}  // namespace monav::device::display
