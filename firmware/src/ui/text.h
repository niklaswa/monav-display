#pragma once

// Text straight from the baked VLW fonts into a Surface. LovyanGFX can draw them too, but its
// anti-aliased path reads, converts and writes every pixel through generic pixel copies; this
// blits the glyph alpha into the band directly, only the rows that fall into it.

#include <cstdint>
#include <vector>

#include "ui/raster.h"

namespace monav::ui {

class VlwFont {
 public:
  /** Parses the glyph table (the bitmaps stay where `data` is: flash on the device). */
  bool load(const uint8_t* data);
  bool loaded() const { return data_ != nullptr; }

  int size() const { return size_; }
  int ascent() const { return ascent_; }
  int descent() const { return descent_; }

  /** Width of a UTF-8 string: the sum of the glyph advances. */
  int width(const char* utf8) const;
  /** Draws a UTF-8 string with its baseline at y, starting at x. Returns the width. */
  int draw(gfx::Surface& s, const char* utf8, float x, float y, uint32_t color, float alpha = 1.0f) const;

 private:
  struct Glyph {
    uint16_t code;
    uint8_t w, h, adv;
    int8_t dx;
    int16_t dy;  // baseline to the glyph's top row
    uint32_t bitmap;
  };
  const Glyph* find(uint16_t code) const;

  const uint8_t* data_ = nullptr;
  int size_ = 0, ascent_ = 0, descent_ = 0;
  int missingAdv_ = 0;
  std::vector<Glyph> glyphs_;  // sorted by code
};

/** Next code point of a UTF-8 string (BMP only); advances p. 0 at the end. */
uint16_t nextCodePoint(const char*& p);

}  // namespace monav::ui
