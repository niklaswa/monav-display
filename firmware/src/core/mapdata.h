#pragma once

// A decoded map image and the route line, as the renderer reads them. The buffers belong to
// whoever filled them (the device's map store, or the host preview).

#include <cstdint>

namespace monav {

/**
 * Map pixels are stored in 16×16 tiles (row-major tiles, row-major inside a tile): turned to any
 * heading, a row of the glass then walks through a few tiles that stay in the cache instead of
 * jumping a whole image row (2 kB) per pixel through PSRAM. Each row of tiles has one spare tile
 * at its end: without it a row of tiles of a 1024-pixel image is exactly 32 KiB - the size of the
 * data cache - and tiles above one another would all compete for the same cache sets.
 */
constexpr int kMapTile = 16;
inline uint32_t mapTilesPerRow(uint32_t w) { return (w + kMapTile - 1) / kMapTile + 1; }
inline uint32_t mapIndex(uint32_t x, uint32_t y, uint32_t w) {
  return (((y >> 4) * mapTilesPerRow(w) + (x >> 4)) << 8) | ((y & 15) << 4) | (x & 15);
}
inline uint32_t mapPixels(uint32_t w, uint32_t h) {
  return mapTilesPerRow(w) * ((h + kMapTile - 1) / kMapTile) * kMapTile * kMapTile;
}

struct MapImage {
  const uint16_t* px = nullptr;  // RGB565, byte-swapped as LovyanGFX sprites store it, tiled (mapIndex)
  int w = 0;
  int h = 0;
  double lat = 0;  // the centre pixel
  double lon = 0;
  float mpp = 1;  // metres per pixel
  uint8_t id = 0;
  uint16_t bg = 0;  // colour around the image (from its edge), same format as px
  /** Whom the picture is credited to, shown under the map; empty: Apple Maps (what Monav sends). */
  char attr[40] = {0};
};

struct RouteLine {
  const int16_t* pts = nullptr;  // pairs: metres east, metres north of (lat, lon)
  int count = 0;                 // points (pairs)
  double lat = 0;
  double lon = 0;
};

}  // namespace monav
