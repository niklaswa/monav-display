#include "ui/fonts.h"

namespace monav::ui {
namespace {

constexpr int kCount = static_cast<int>(Font::Count);
lgfx::VLWfont s_fonts[kCount];
VlwFont s_vlw[kCount];
lgfx::PointerWrapper s_data[kCount];
bool s_ready[kCount] = {false};

}  // namespace

const char* const kFontFiles[kCount] = {
    "speed", "speed_xl", "speed_md", "num", "num_sm", "sign_lg", "sign_md", "sign_sm", "sign_xs", "title", "label", "body", "small", "clock", "tiny",
};

bool initFonts(const uint8_t* const data[kCount]) {
  bool ok = true;
  for (int i = 0; i < kCount; i++) {
    if (data[i] == nullptr) {
      ok = false;
      continue;
    }
    s_data[i].set(data[i]);
    s_ready[i] = s_fonts[i].loadFont(&s_data[i]);
    ok = ok && s_ready[i] && s_vlw[i].load(data[i]);
  }
  return ok;
}

void useFont(lgfx::LGFXBase& g, Font f) {
  const int i = static_cast<int>(f);
  if (i >= 0 && i < kCount && s_ready[i]) {
    g.setFont(&s_fonts[i]);
  } else {
    g.setFont(&lgfx::fonts::Font2);
  }
  g.setTextSize(1);
}

const VlwFont& vlw(Font f) { return s_vlw[static_cast<int>(f)]; }

}  // namespace monav::ui
