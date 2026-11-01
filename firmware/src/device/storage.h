#pragma once

// NVS (Preferences): the paired phone's token, the brightness, the language, the layout and the
// picture's rotation.

#include <cstddef>

namespace monav::device::storage {

void begin();

bool loadToken(char* out, size_t cap);
void saveToken(const char* token);
void eraseToken();

int loadBrightness();  // −1 when nothing is stored
void saveBrightness(int percent);

int loadLang();  // −1 when nothing is stored
void saveLang(int lang);

int loadLayout();  // −1 when nothing is stored
void saveLayout(int layout);

int loadRotation();  // −1 when nothing is stored, else 0..359
void saveRotation(int degrees);

}  // namespace monav::device::storage
