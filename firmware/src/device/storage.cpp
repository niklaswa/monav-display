#include "device/storage.h"

#include <Preferences.h>

#include <cstring>

namespace monav::device::storage {
namespace {

Preferences s_prefs;
constexpr const char* kNamespace = "monav";

}  // namespace

void begin() { s_prefs.begin(kNamespace, false); }

bool loadToken(char* out, size_t cap) {
  if (cap == 0) return false;
  out[0] = 0;
  if (!s_prefs.isKey("token")) return false;
  const size_t n = s_prefs.getString("token", out, cap);
  return n > 0 && out[0] != 0;
}

void saveToken(const char* token) { s_prefs.putString("token", token); }

void eraseToken() { s_prefs.remove("token"); }

int loadBrightness() { return s_prefs.isKey("bright") ? s_prefs.getUChar("bright", 80) : -1; }

void saveBrightness(int percent) {
  if (loadBrightness() != percent) s_prefs.putUChar("bright", static_cast<uint8_t>(percent));
}

int loadLang() { return s_prefs.isKey("lang") ? s_prefs.getUChar("lang", 0) : -1; }

void saveLang(int lang) { s_prefs.putUChar("lang", static_cast<uint8_t>(lang)); }

int loadLayout() { return s_prefs.isKey("layout") ? s_prefs.getUChar("layout", 0) : -1; }

void saveLayout(int layout) {
  if (loadLayout() != layout) s_prefs.putUChar("layout", static_cast<uint8_t>(layout));
}

int loadRotation() { return s_prefs.isKey("rot") ? s_prefs.getUShort("rot", 0) : -1; }

void saveRotation(int degrees) {
  if (loadRotation() != degrees) s_prefs.putUShort("rot", static_cast<uint16_t>(degrees));
}

// The look as one record: theme, surprises, name.
struct StoredStyle {
  uint8_t version;
  uint8_t theme;
  uint8_t eggs;
  char name[24];
};

bool loadStyle(monav::Style& out) {
  StoredStyle st{};
  if (!s_prefs.isKey("style") || s_prefs.getBytes("style", &st, sizeof(st)) != sizeof(st) || st.version != 1) return false;
  out.theme = static_cast<monav::Theme>(st.theme);
  out.eggs = st.eggs != 0;
  memcpy(out.name, st.name, sizeof(out.name));
  out.name[sizeof(out.name) - 1] = 0;
  out.intro = s_prefs.getBool("intro", false);
  return true;
}

void saveStyle(const monav::Style& style) {
  StoredStyle st{};
  st.version = 1;
  st.theme = static_cast<uint8_t>(style.theme);
  st.eggs = style.eggs ? 1 : 0;
  memcpy(st.name, style.name, sizeof(st.name));
  s_prefs.putBytes("style", &st, sizeof(st));
  s_prefs.putBool("intro", style.intro);
}

bool loadNotices(monav::NoticeMode& mode, bool& text, bool& full) {
  if (!s_prefs.isKey("ntf")) return false;
  const uint8_t v = s_prefs.getUChar("ntf", 0);
  const uint8_t m = v & 0x0F;
  mode = m == 2 ? monav::NoticeMode::All : m == 1 ? monav::NoticeMode::Calls : monav::NoticeMode::Off;
  text = (v & 0x10) != 0;
  full = (v & 0x20) != 0;
  return true;
}

void saveNotices(monav::NoticeMode mode, bool text, bool full) {
  s_prefs.putUChar("ntf", static_cast<uint8_t>(static_cast<uint8_t>(mode) | (text ? 0x10 : 0) | (full ? 0x20 : 0)));
}

uint32_t loadBestRun() { return s_prefs.isKey("best") ? s_prefs.getUInt("best", 0) : 0; }

void saveBestRun(uint32_t ms) { s_prefs.putUInt("best", ms); }

}  // namespace monav::device::storage
