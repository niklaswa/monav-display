#pragma once

// The baked Inter VLW fonts (scripts/build_fonts.py). Each is parsed once into its own
// VLWfont object, so switching fonts while drawing a frame costs nothing.

#include <LovyanGFX.hpp>

#include <cstdint>

#include "ui/text.h"

namespace monav::ui {

enum class Font : uint8_t {
  Speed,    // 150 px bold digits (tabular)
  SpeedXL,  // 196 px bold digits (tabular), focus layout
  SpeedMd,  // 80 px bold digits (tabular), map layout
  Num,      // 44 px bold digits, distances
  NumSm,    // 30 px bold digits, countdowns, the id
  SignLg,   // 52 px bold digits, limit sign (focus)
  SignMd,   // 38 px
  SignSm,   // 28 px
  SignXs,   // 21 px
  Title,    // 30 px semibold text
  Label,    // 26 px semibold text, the card headline
  Body,     // 23 px medium text
  Small,    // 18 px semibold text
  Clock,    // 104 px light digits
  Tiny,     // 13 px medium text (map attribution)
  Count,
};

/** File names of the fonts, in Font order (data/fonts/<name>.vlw). */
extern const char* const kFontFiles[static_cast<int>(Font::Count)];

/** Hand over the VLW bytes of every font (embedded on the device, read from disk on the host). */
bool initFonts(const uint8_t* const data[static_cast<int>(Font::Count)]);

/** LovyanGFX drawing with the font (the preview's labels). */
void useFont(lgfx::LGFXBase& g, Font f);

/** The font for drawing into a Surface (ui/text.h): what the renderer uses. */
const VlwFont& vlw(Font f);

}  // namespace monav::ui
