#pragma once

// CST9217 capacitive touch, interrupt-driven. A small task turns the controller's reports into
// finger samples (down with position, or up) for the UI task's gesture recogniser.

#include <cstdint>

namespace monav::device::touch {

struct Sample {
  bool down;
  int16_t x;
  int16_t y;
  uint32_t ms;
};

/** I2C + controller bring-up; logs and stays off if no controller answers. */
bool begin();

/** Next finger sample, if any. */
bool poll(Sample& out);

}  // namespace monav::device::touch
