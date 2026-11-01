#pragma once

// Draws one frame. Depends only on a 466×466 16-bit LovyanGFX sprite and the Frame struct,
// so the very same code renders on the display and into the host preview PNGs.

#include <LovyanGFX.hpp>

#include "core/frame.h"

namespace monav::ui {

constexpr int kSize = 466;

void render(lgfx::LGFX_Sprite& canvas, const Frame& f);

/** Where the render time goes (accumulated since the last take). */
struct RenderProfile {
  uint32_t frames = 0;
  uint32_t totalUs = 0;
  uint32_t clearUs = 0;
  uint32_t copyUs = 0;   // bands into the frame
  uint32_t mapUs = 0;    // sampling the map image
  uint32_t routeUs = 0;  // the route line
  uint32_t textUs = 0;   // drawing and measuring text
  uint32_t ringUs = 0;   // the speed ring
  uint32_t cardUs = 0;   // the context card (its text included)
  uint32_t mapFrames = 0;
};
/**
 * A buffer in fast (internal) RAM of 466 × rows pixels: frames are then drawn band by band into it
 * and copied into the canvas. Without one, drawing goes straight into the canvas.
 */
void setBandBuffer(uint16_t* buffer, int rows);
/** Two band buffers, one per drawing task (with setHelper, frames are drawn on two cores). */
void setBandBuffers(uint16_t* lane0, uint16_t* lane1, int rows);
/**
 * A second drawing task: run(fn, arg) has it call fn(arg) (the odd bands of the frame) while the
 * caller draws the even ones; wait() returns once it is done.
 */
void setHelper(void (*run)(void (*fn)(void*), void* arg), void (*wait)());

/** The clock for the profile (micros() on the device); without one nothing is measured. */
void setProfileClock(uint32_t (*micros)());
RenderProfile takeRenderProfile();

}  // namespace monav::ui
