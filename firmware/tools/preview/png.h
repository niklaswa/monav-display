#pragma once

// Minimal PNG writer (8-bit RGB, no filtering) on top of zlib. Host only.

#include <zlib.h>

#include <cstdint>
#include <cstdio>
#include <vector>

namespace png {

inline void put32(std::vector<uint8_t>& v, uint32_t x) {
  v.push_back(x >> 24);
  v.push_back(x >> 16);
  v.push_back(x >> 8);
  v.push_back(x);
}

inline void chunk(FILE* f, const char* type, const std::vector<uint8_t>& data) {
  std::vector<uint8_t> buf;
  put32(buf, static_cast<uint32_t>(data.size()));
  buf.insert(buf.end(), type, type + 4);
  buf.insert(buf.end(), data.begin(), data.end());
  const uint32_t crc = crc32(0, buf.data() + 4, static_cast<uInt>(buf.size() - 4));
  put32(buf, crc);
  fwrite(buf.data(), 1, buf.size(), f);
}

/** rgb: width*height*3 bytes, rows top to bottom. */
inline bool write(const char* path, const uint8_t* rgb, int width, int height) {
  FILE* f = fopen(path, "wb");
  if (!f) return false;
  static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  fwrite(sig, 1, 8, f);
  std::vector<uint8_t> ihdr;
  put32(ihdr, width);
  put32(ihdr, height);
  ihdr.push_back(8);  // bit depth
  ihdr.push_back(2);  // colour type RGB
  ihdr.push_back(0);
  ihdr.push_back(0);
  ihdr.push_back(0);
  chunk(f, "IHDR", ihdr);
  std::vector<uint8_t> raw;
  raw.reserve(static_cast<size_t>(height) * (width * 3 + 1));
  for (int y = 0; y < height; y++) {
    raw.push_back(0);
    raw.insert(raw.end(), rgb + static_cast<size_t>(y) * width * 3, rgb + static_cast<size_t>(y + 1) * width * 3);
  }
  uLongf len = compressBound(static_cast<uLong>(raw.size()));
  std::vector<uint8_t> z(len);
  compress2(z.data(), &len, raw.data(), static_cast<uLong>(raw.size()), 9);
  z.resize(len);
  chunk(f, "IDAT", z);
  chunk(f, "IEND", {});
  fclose(f);
  return true;
}

}  // namespace png
