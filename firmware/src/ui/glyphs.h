#pragma once

// Vector symbols baked into alpha masks (scripts/build_glyphs.py writes glyphs.cpp): shapes with
// real curves that the primitives of ui/raster.h would only approximate.

#include <cstdint>

namespace monav::ui::glyphs {

constexpr int kSize = 96;

/** Phone handset, picking up (Material "call"). */
extern const uint8_t kCall[kSize * kSize];
/** Phone handset, hanging up (Material "call_end"). */
extern const uint8_t kCallEnd[kSize * kSize];

}  // namespace monav::ui::glyphs
