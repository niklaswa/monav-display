#include "device/storage.h"

#include <Preferences.h>

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

}  // namespace monav::device::storage
