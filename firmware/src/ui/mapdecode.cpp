#include "ui/mapdecode.h"

#include <lgfx/utility/lgfx_tjpgd.h>

#include <cstdlib>
#include <cstring>

#include "core/mapdata.h"
#include "ui/raster.h"

namespace monav::ui {
namespace {

struct Decode {
  const uint8_t* src;
  size_t len;
  size_t pos;
  uint16_t* dst;
  uint32_t w, h;
  void (*alive)();
  uint32_t blocks;
};

uint32_t input(void* dev, uint8_t* buf, uint32_t n) {
  Decode& d = *static_cast<Decode*>(dev);
  if (d.pos + n > d.len) n = static_cast<uint32_t>(d.len - d.pos);
  if (buf != nullptr) memcpy(buf, d.src + d.pos, n);
  d.pos += n;
  return n;
}

/** One decoded block (RGB888, left..right × top..bottom inclusive) into the tiled buffer. */
uint32_t output(void* dev, void* bitmap, JRECT* r) {
  Decode& d = *static_cast<Decode*>(dev);
  if (d.alive != nullptr && (++d.blocks & 63) == 0) d.alive();
  const uint8_t* rgb = static_cast<const uint8_t*>(bitmap);
  for (uint32_t y = r->top; y <= r->bottom; y++) {
    for (uint32_t x = r->left; x <= r->right; x++, rgb += 3) {
      if (x >= d.w || y >= d.h) continue;
      // Darkened once here (82 %), so the speed and the card stand out without darkening every
      // frame: the per-frame darkening cost a quarter of the map's drawing time.
      const uint32_t r = rgb[0] * 210u >> 8, g = rgb[1] * 210u >> 8, b = rgb[2] * 210u >> 8;
      const uint16_t v = static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
      d.dst[mapIndex(x, y, d.w)] = static_cast<uint16_t>((v >> 8) | (v << 8));
    }
  }
  return 1;
}

}  // namespace

bool decodeMap(const uint8_t* jpg, size_t len, uint16_t* dst, int w, int h, void (*alive)()) {
  if (jpg == nullptr || dst == nullptr || len < 4 || w <= 0 || h <= 0) return false;
  constexpr size_t kPool = 3900;  // TJpgDec work area, as LovyanGFX's own drawJpg uses
  void* pool = malloc(kPool);
  if (pool == nullptr) return false;
  Decode d{jpg, len, 0, dst, static_cast<uint32_t>(w), static_cast<uint32_t>(h), alive, 0};
  lgfxJdec dec;
  bool ok = lgfx_jd_prepare(&dec, input, pool, kPool, &d) == JDR_OK && dec.width == w && dec.height == h;
  if (ok) ok = lgfx_jd_decomp(&dec, output, 0) == JDR_OK;
  free(pool);
  return ok;
}

uint16_t mapEdgeColour(const uint16_t* px, int w, int h) {
  if (px == nullptr || w < 2 || h < 2) return 0;
  uint32_t r = 0, g = 0, b = 0, n = 0;
  auto add = [&](int x, int y) {
    const uint32_t c = gfx::unpack(px[mapIndex(x, y, w)]);
    r += (c >> 16) & 0xFF;
    g += (c >> 8) & 0xFF;
    b += c & 0xFF;
    n++;
  };
  for (int i = 0; i < 64; i++) {
    const int x = (w - 1) * i / 63, y = (h - 1) * i / 63;
    add(x, 0);
    add(x, h - 1);
    add(0, y);
    add(w - 1, y);
  }
  return gfx::pack(gfx::rgb(r / n, g / n, b / n));
}

}  // namespace monav::ui
