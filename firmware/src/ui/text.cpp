#include "ui/text.h"

#include <algorithm>
#include <cmath>

namespace monav::ui {
namespace {

inline uint32_t be32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

inline uint32_t div255(uint32_t v) { return (v + 128 + ((v + 128) >> 8)) >> 8; }

}  // namespace

uint16_t nextCodePoint(const char*& p) {
  const uint8_t c = static_cast<uint8_t>(*p);
  if (c == 0) return 0;
  if (c < 0x80) {
    p++;
    return c;
  }
  if ((c & 0xE0) == 0xC0 && p[1]) {
    const uint16_t cp = static_cast<uint16_t>(((c & 0x1F) << 6) | (p[1] & 0x3F));
    p += 2;
    return cp;
  }
  if ((c & 0xF0) == 0xE0 && p[1] && p[2]) {
    const uint16_t cp = static_cast<uint16_t>(((c & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F));
    p += 3;
    return cp;
  }
  // 4-byte sequences (outside the fonts anyway) and stray bytes: skip them.
  p++;
  while ((static_cast<uint8_t>(*p) & 0xC0) == 0x80) p++;
  return 0xFFFD;
}

bool VlwFont::load(const uint8_t* data) {
  if (data == nullptr) return false;
  const uint32_t count = be32(data);
  if (count == 0 || count > 4000) return false;
  size_ = static_cast<int>(be32(data + 8));
  glyphs_.clear();
  glyphs_.reserve(count);
  uint32_t bitmap = 24 + count * 28;
  int maxAscent = 0, maxDescent = 0;
  for (uint32_t i = 0; i < count; i++) {
    const uint8_t* g = data + 24 + i * 28;
    Glyph gl;
    gl.code = static_cast<uint16_t>(be32(g));
    gl.h = static_cast<uint8_t>(be32(g + 4));
    gl.w = static_cast<uint8_t>(be32(g + 8));
    gl.adv = static_cast<uint8_t>(be32(g + 12));
    gl.dy = static_cast<int16_t>(static_cast<int32_t>(be32(g + 16)));
    gl.dx = static_cast<int8_t>(static_cast<int32_t>(be32(g + 20)));
    gl.bitmap = bitmap;
    bitmap += static_cast<uint32_t>(gl.w) * gl.h;
    maxAscent = std::max(maxAscent, static_cast<int>(gl.dy));
    maxDescent = std::max(maxDescent, static_cast<int>(gl.h) - gl.dy);
    glyphs_.push_back(gl);
  }
  std::sort(glyphs_.begin(), glyphs_.end(), [](const Glyph& a, const Glyph& b) { return a.code < b.code; });
  ascent_ = maxAscent;
  descent_ = maxDescent;
  missingAdv_ = size_ * 2 / 7;
  data_ = data;
  return true;
}

const VlwFont::Glyph* VlwFont::find(uint16_t code) const {
  auto it = std::lower_bound(glyphs_.begin(), glyphs_.end(), code,
                             [](const Glyph& g, uint16_t c) { return g.code < c; });
  return it != glyphs_.end() && it->code == code ? &*it : nullptr;
}

int VlwFont::width(const char* utf8) const {
  if (!data_ || utf8 == nullptr) return 0;
  int w = 0;
  const char* p = utf8;
  while (uint16_t cp = nextCodePoint(p)) {
    const Glyph* g = find(cp);
    w += g ? g->adv : missingAdv_;
  }
  return w;
}

int VlwFont::draw(gfx::Surface& s, const char* utf8, float x, float y, uint32_t color, float alpha) const {
  if (!data_ || utf8 == nullptr) return 0;
  const uint32_t c = gfx::shade(s, color);
  const uint16_t solid = gfx::pack(c);
  const uint32_t cr = (c >> 16) & 0xFF, cg = (c >> 8) & 0xFF, cb = c & 0xFF;
  const uint32_t a256 = static_cast<uint32_t>((alpha < 0 ? 0 : alpha > 1 ? 1 : alpha) * 256.0f);
  const int baseline = gfx::iround(y);
  int pen = gfx::iround(x);
  const int start = pen;
  const char* p = utf8;
  while (uint16_t cp = nextCodePoint(p)) {
    const Glyph* g = find(cp);
    if (g == nullptr) {
      pen += missingAdv_;
      continue;
    }
    const int top = baseline - g->dy;
    const int left = pen + g->dx;
    const int r0 = std::max(0, s.oy - top), r1 = std::min<int>(g->h, s.oy + s.h - top);
    const int c0 = std::max(0, -left), c1 = std::min<int>(g->w, s.w - left);
    for (int r = r0; r < r1; r++) {
      const uint8_t* src = data_ + g->bitmap + r * g->w;
      uint16_t* row = gfx::rowAt(s, top + r) + left;
      for (int col = c0; col < c1; col++) {
        uint32_t a = src[col];
        if (a == 0) continue;
        if (a256 < 256) a = (a * a256) >> 8;
        if (a >= 255) {
          row[col] = solid;
          continue;
        }
        const uint32_t d = gfx::unpack(row[col]);
        const uint32_t ia = 255 - a;
        const uint32_t rr = div255(cr * a + ((d >> 16) & 0xFF) * ia);
        const uint32_t gg = div255(cg * a + ((d >> 8) & 0xFF) * ia);
        const uint32_t bb = div255(cb * a + (d & 0xFF) * ia);
        row[col] = gfx::pack((rr << 16) | (gg << 8) | bb);
      }
    }
    pen += g->adv;
  }
  return pen - start;
}

}  // namespace monav::ui
