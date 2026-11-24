#pragma once

// JPEG map image → RGB565 buffer the map renderer samples (LovyanGFX's TJpgDec). Portable: the
// device decodes into its spare PSRAM map buffer, the preview into a heap buffer.

#include <cstddef>
#include <cstdint>

namespace monav::ui {

/**
 * Decodes a baseline JPEG of exactly w × h pixels into dst (mapPixels(w, h), tiled, sprite byte order).
 * `alive`, if given, is called every few dozen blocks while it runs (the device feeds its watchdog).
 */
bool decodeMap(const uint8_t* jpg, size_t len, uint16_t* dst, int w, int h, void (*alive)() = nullptr);

/**
 * Decodes the phone's picture (a baseline JPEG of Wallpaper::kSize²) into dst (row-major, sprite
 * byte order), darkened once: most behind the clock and the texts, a little towards the rim.
 */
bool decodeWallpaper(const uint8_t* jpg, size_t len, uint16_t* dst, void (*alive)() = nullptr);

/** Decodes a baseline JPEG of exactly w × h pixels into dst (row-major, sprite byte order): app icons. */
bool decodeImage(const uint8_t* jpg, size_t len, uint16_t* dst, int w, int h, void (*alive)() = nullptr);

/** The colour around the image, from its edge pixels (shown where the map runs out). */
uint16_t mapEdgeColour(const uint16_t* px, int w, int h);

}  // namespace monav::ui
