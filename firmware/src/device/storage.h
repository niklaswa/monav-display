#pragma once

// NVS (Preferences): the paired phone's token, the brightness, the language, the layout and the
// picture's rotation.

#include <cstddef>
#include <cstdint>

#include "core/model.h"

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

bool loadStyle(monav::Style& out);
void saveStyle(const monav::Style& style);

/** The iPhone notification settings (cfg.ntf, cfg.ntfText); false when nothing is stored. */
bool loadNotices(monav::NoticeMode& mode, bool& text, bool& full);
void saveNotices(monav::NoticeMode mode, bool text, bool full);
uint32_t loadBestRun();  // 0 when none is stored
void saveBestRun(uint32_t ms);

}  // namespace monav::device::storage
