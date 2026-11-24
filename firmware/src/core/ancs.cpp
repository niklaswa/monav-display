#include "core/ancs.h"

#include <cstdio>
#include <cstring>

namespace monav::ancs {
namespace {

// Command and attribute ids (ANCS specification).
constexpr uint8_t kCmdGetNotificationAttributes = 0;
constexpr uint8_t kCmdGetAppAttributes = 1;
constexpr uint8_t kCmdPerformNotificationAction = 2;

constexpr uint8_t kAttrAppIdentifier = 0;
constexpr uint8_t kAttrTitle = 1;
constexpr uint8_t kAttrSubtitle = 2;
constexpr uint8_t kAttrMessage = 3;
constexpr uint8_t kAttrPositiveLabel = 6;
constexpr uint8_t kAttrNegativeLabel = 7;
constexpr int kNotificationAttrs = 6;  // the ones asked for in buildGetNotification

constexpr uint8_t kAppAttrDisplayName = 0;

void putU32(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
  p[2] = static_cast<uint8_t>(v >> 16);
  p[3] = static_cast<uint8_t>(v >> 24);
}

uint32_t getU32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 |
         static_cast<uint32_t>(p[3]) << 24;
}

/** Characters the text fonts have beyond ASCII and Latin-1/Extended-A (scripts/build_fonts.py). */
bool inFont(uint32_t cp) {
  if (cp >= 0x20 && cp <= 0x7E) return true;
  if (cp >= 0xA0 && cp <= 0x17F) return cp != 0xAD;
  switch (cp) {
    case 0x2013: case 0x2014: case 0x2018: case 0x2019: case 0x201A: case 0x201C: case 0x201D:
    case 0x201E: case 0x2022: case 0x2026: case 0x2039: case 0x203A: case 0x2190: case 0x2191:
    case 0x2192: case 0x2193: case 0x2212: case 0x2713: case 0x2715:
      return true;
    default:
      return false;
  }
}

/** A stand-in the fonts have for a character they do not; 0: drop it, ' ': white space. */
uint32_t substitute(uint32_t cp) {
  if (cp < 0x20 || (cp >= 0x7F && cp < 0xA0)) return ' ';
  if ((cp >= 0x2000 && cp <= 0x200A) || cp == 0x2028 || cp == 0x2029 || cp == 0x202F || cp == 0x205F ||
      cp == 0x3000) {
    return ' ';
  }
  if (cp >= 0x2010 && cp <= 0x2012) return '-';
  if (cp == 0x2015) return 0x2014;
  if (cp == 0x2032 || cp == 0x02BC) return 0x2019;
  if (cp == 0x2033) return '"';
  return 0;  // zero-width and direction marks, variation selectors, emoji, other scripts
}

size_t putUtf8(uint32_t cp, char* out) {
  if (cp < 0x80) {
    out[0] = static_cast<char>(cp);
    return 1;
  }
  if (cp < 0x800) {
    out[0] = static_cast<char>(0xC0 | (cp >> 6));
    out[1] = static_cast<char>(0x80 | (cp & 0x3F));
    return 2;
  }
  out[0] = static_cast<char>(0xE0 | (cp >> 12));
  out[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
  out[2] = static_cast<char>(0x80 | (cp & 0x3F));
  return 3;
}

}  // namespace

bool parseSource(const uint8_t* data, size_t len, SourceEvent& out) {
  if (data == nullptr || len < 8 || data[0] > 2) return false;
  out.event = static_cast<EventId>(data[0]);
  out.flags = data[1];
  out.category = static_cast<Category>(data[2] <= 11 ? data[2] : 0);
  out.count = data[3];
  out.uid = getU32(data + 4);
  return true;
}

size_t buildGetNotification(uint8_t* out, size_t cap, uint32_t uid) {
  if (cap < 21) return 0;
  size_t n = 0;
  out[n++] = kCmdGetNotificationAttributes;
  putU32(out + n, uid);
  n += 4;
  out[n++] = kAttrAppIdentifier;
  const struct {
    uint8_t attr;
    uint16_t max;
  } sized[] = {{kAttrTitle, kTitleMax}, {kAttrSubtitle, kSubtitleMax}, {kAttrMessage, kMessageMax}};
  for (const auto& a : sized) {
    out[n++] = a.attr;
    out[n++] = static_cast<uint8_t>(a.max);
    out[n++] = static_cast<uint8_t>(a.max >> 8);
  }
  out[n++] = kAttrPositiveLabel;
  out[n++] = kAttrNegativeLabel;
  return n;
}

size_t buildGetApp(uint8_t* out, size_t cap, const char* app) {
  const size_t len = app ? strlen(app) : 0;
  if (len == 0 || cap < len + 3) return 0;
  out[0] = kCmdGetAppAttributes;
  memcpy(out + 1, app, len);
  out[1 + len] = 0;
  out[2 + len] = kAppAttrDisplayName;
  return len + 3;
}

size_t buildAction(uint8_t* out, size_t cap, uint32_t uid, bool positive) {
  if (cap < 6) return 0;
  out[0] = kCmdPerformNotificationAction;
  putU32(out + 1, uid);
  out[5] = positive ? 0 : 1;
  return 6;
}

// --- Response --------------------------------------------------------------------------------

void Response::beginNotification(uint32_t uid) {
  active_ = true;
  command_ = kCmdGetNotificationAttributes;
  uid_ = uid;
  len_ = 0;
  notice_ = Notice{};
  notice_.uid = uid;
}

void Response::beginApp(const char* app) {
  active_ = true;
  command_ = kCmdGetAppAttributes;
  snprintf(appId_, sizeof(appId_), "%s", app ? app : "");
  len_ = 0;
  appName_[0] = 0;
}

Response::Result Response::feed(const uint8_t* data, size_t len) {
  if (!active_) return Result::Error;
  if (len_ + len > sizeof(buf_)) {
    active_ = false;
    return Result::Error;
  }
  memcpy(buf_ + len_, data, len);
  len_ += len;
  // The first packet must answer the command that was written.
  if (len_ >= 1 && buf_[0] != command_) {
    active_ = false;
    return Result::Error;
  }
  if (!parse()) return Result::More;
  active_ = false;
  return Result::Done;
}

void Response::store(uint8_t attr, const uint8_t* value, size_t len) {
  const char* v = reinterpret_cast<const char*>(value);
  if (command_ == kCmdGetAppAttributes) {
    if (attr == kAppAttrDisplayName) cleanText(v, len, appName_, sizeof(appName_));
    return;
  }
  switch (attr) {
    case kAttrAppIdentifier: {
      // A bundle id is plain ASCII: copied as it is.
      const size_t n = len < sizeof(notice_.app) - 1 ? len : sizeof(notice_.app) - 1;
      memcpy(notice_.app, v, n);
      notice_.app[n] = 0;
      break;
    }
    case kAttrTitle:
      cleanText(v, len, notice_.title, sizeof(notice_.title));
      break;
    case kAttrSubtitle:
      cleanText(v, len, notice_.subtitle, sizeof(notice_.subtitle));
      break;
    case kAttrMessage:
      cleanText(v, len, notice_.message, sizeof(notice_.message));
      break;
    case kAttrPositiveLabel:
      cleanText(v, len, notice_.positive, sizeof(notice_.positive));
      break;
    case kAttrNegativeLabel:
      cleanText(v, len, notice_.negative, sizeof(notice_.negative));
      break;
    default:
      break;
  }
}

/** True once the whole response is in (and stored). */
bool Response::parse() {
  size_t p = 1;
  int expected = 0;
  if (command_ == kCmdGetNotificationAttributes) {
    if (len_ < 5) return false;
    if (getU32(buf_ + 1) != uid_) return false;  // not ours: wait (a stale answer is dropped by the caller)
    p = 5;
    expected = kNotificationAttrs;
  } else {
    // The app identifier, NUL-terminated, then the attributes.
    while (p < len_ && buf_[p] != 0) p++;
    if (p >= len_) return false;
    p++;
    expected = 1;
  }
  // Each attribute: id, length (uint16 little endian), value. Parse only once all are in.
  size_t q = p;
  for (int i = 0; i < expected; i++) {
    if (q + 3 > len_) return false;
    const size_t vlen = static_cast<size_t>(buf_[q + 1]) | static_cast<size_t>(buf_[q + 2]) << 8;
    if (q + 3 + vlen > len_) return false;
    q += 3 + vlen;
  }
  for (int i = 0; i < expected; i++) {
    const uint8_t attr = buf_[p];
    const size_t vlen = static_cast<size_t>(buf_[p + 1]) | static_cast<size_t>(buf_[p + 2]) << 8;
    store(attr, buf_ + p + 3, vlen);
    p += 3 + vlen;
  }
  return true;
}

// --- Text ------------------------------------------------------------------------------------

void cleanText(const char* in, size_t len, char* out, size_t cap) {
  if (cap == 0) return;
  out[0] = 0;
  if (in == nullptr) return;
  size_t n = 0;
  bool space = true;  // drops leading white space and collapses runs
  size_t i = 0;
  while (i < len && in[i] != 0) {
    const uint8_t c = static_cast<uint8_t>(in[i]);
    uint32_t cp;
    size_t step;
    if (c < 0x80) {
      cp = c;
      step = 1;
    } else if ((c & 0xE0) == 0xC0) {
      cp = c & 0x1F;
      step = 2;
    } else if ((c & 0xF0) == 0xE0) {
      cp = c & 0x0F;
      step = 3;
    } else if ((c & 0xF8) == 0xF0) {
      cp = c & 0x07;
      step = 4;
    } else {
      i++;  // a stray continuation byte
      continue;
    }
    if (i + step > len) break;
    bool ok = true;
    for (size_t k = 1; k < step; k++) {
      const uint8_t cc = static_cast<uint8_t>(in[i + k]);
      if ((cc & 0xC0) != 0x80) {
        ok = false;
        break;
      }
      cp = (cp << 6) | (cc & 0x3F);
    }
    if (!ok) {
      i++;
      continue;
    }
    i += step;
    if (!inFont(cp)) cp = substitute(cp);
    if (cp == 0) continue;
    if (cp == ' ') {
      if (space) continue;
      space = true;
    } else {
      space = false;
    }
    char enc[3];
    const size_t m = putUtf8(cp, enc);
    if (n + m >= cap) break;
    memcpy(out + n, enc, m);
    n += m;
  }
  while (n > 0 && out[n - 1] == ' ') n--;
  out[n] = 0;
}

}  // namespace monav::ancs
