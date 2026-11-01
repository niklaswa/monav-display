#include "device/bench.h"

#if defined(MONAV_DEMO)

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_task_wdt.h>

#include <cmath>
#include <cstring>

#include "core/mapdata.h"
#include "device/transfers.h"
#include "ui/fonts.h"
#include "ui/maprender.h"
#include "ui/raster.h"

namespace monav::device::bench {
namespace {

template <typename F>
void timeIt(const char* name, int runs, F&& f) {
  const uint32_t t0 = micros();
  for (int i = 0; i < runs; i++) f();
  const uint32_t us = (micros() - t0) / runs;
  esp_task_wdt_reset();  // runs on the watched ui task
  Serial.printf("bench: %-34s %6u us\n", name, static_cast<unsigned>(us));
}

}  // namespace

void run() {
  constexpr int kW = 466, kRows = 60;
  auto* band = static_cast<uint16_t*>(heap_caps_malloc(kW * kRows * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  auto* frame = static_cast<uint16_t*>(heap_caps_malloc(kW * kW * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!band || !frame) return;
  // The whole canvas as one surface in PSRAM, and one band of it in internal RAM.
  gfx::Surface full{frame, kW, kW, 1.0f, 0};
  gfx::Surface b{band, kW, kRows, 1.0f, 300};
  const uint32_t white = 0xFFFFFF, grey = 0x202024;

  timeIt("clear band (internal)", 20, [&] { gfx::clear(b, 0); });
  timeIt("memcpy band -> psram", 20, [&] { memcpy(frame + 300 * kW, band, kW * kRows * 2); });
  timeIt("fillCircle r30 (band)", 20, [&] { gfx::fillCircle(b, 233, 330, 30, white); });
  timeIt("strokeCircle r25 w5 (band)", 20, [&] { gfx::strokeCircle(b, 233, 330, 25, 5, white); });
  timeIt("arc r23 w5 270deg (band)", 20, [&] { gfx::arc(b, 233, 330, 23, 5, 0, 270, white, white); });
  timeIt("fillRoundRect 300x76 (band)", 20, [&] { gfx::fillRoundRect(b, 83, 300, 300, 60, 30, grey); });
  timeIt("strokeRoundRect 300x76 (band)", 20, [&] { gfx::strokeRoundRect(b, 83, 300, 300, 60, 30, 1.5f, grey); });
  timeIt("line 20px w11 (band)", 50, [&] { gfx::line(b, 200, 320, 214, 334, 11, white); });
  timeIt("ringArc HUD track (full, psram)", 5, [&] { gfx::ringArc(full, 233, 233, 214, 14, -135, 135, grey, grey); });
  timeIt("text Label 'Langsamer' (band)", 20, [&] { ui::vlw(ui::Font::Label).draw(b, "Langsamer", 150, 340, white); });
  {
    volatile float acc = 0;
    timeIt("atan2f x1000", 3, [&] {
      for (int i = 0; i < 1000; i++) acc = acc + std::atan2(static_cast<float>(i) - 500.0f, 123.0f);
    });
    timeIt("sqrtf x1000", 3, [&] {
      for (int i = 0; i < 1000; i++) acc = acc + std::sqrt(static_cast<float>(i) + 0.5f);
    });
  }
  {
    uint16_t ptbl[65];
    for (int i = 0; i <= 64; i++) ptbl[i] = static_cast<uint16_t>(i * 1000);
    timeIt("plain store loop 14000 px", 10, [&] {
      int32_t ci = 0;
      for (int i = 0; i < 14000; i++, ci += 300) band[i] = ptbl[((ci + 32768) >> 16) & 63];
    });
  }
  timeIt("arc HUD track, one band (rows 0-60)", 10, [&] {
    gfx::Surface sb{band, kW, kRows, 1.0f, 0};
    gfx::arc(sb, 233, 233, 214, 14, -135, 135, grey, grey);
  });
  timeIt("arc HUD track, one band (rows 180-240)", 10, [&] {
    gfx::Surface sb{band, kW, kRows, 1.0f, 180};
    gfx::arc(sb, 233, 233, 214, 14, -135, 135, grey, grey);
  });
  timeIt("strokeCircle r214 w14, band 180-240", 10, [&] {
    gfx::Surface sb{band, kW, kRows, 1.0f, 180};
    gfx::strokeCircle(sb, 233, 233, 214, 14, grey);
  });
  timeIt("HUD ring track, all bands", 3, [&] {
    for (int y0 = 0; y0 < kW; y0 += kRows) {
      gfx::Surface sb{band, kW, kW - y0 < kRows ? kW - y0 : kRows, 1.0f, y0};
      gfx::arc(sb, 233, 233, 214, 14, -135, 135, grey, grey);
    }
  });
  timeIt("HUD ring fill gradient, all bands", 3, [&] {
    for (int y0 = 0; y0 < kW; y0 += kRows) {
      gfx::Surface sb{band, kW, kW - y0 < kRows ? kW - y0 : kRows, 1.0f, y0};
      gfx::arc(sb, 233, 233, 214, 14, -135, 20, 0x103010, 0x33D173);
    }
  });
  timeIt("edge ring r227 w8, all bands", 3, [&] {
    for (int y0 = 0; y0 < kW; y0 += kRows) {
      gfx::Surface sb{band, kW, kW - y0 < kRows ? kW - y0 : kRows, 1.0f, y0};
      gfx::arc(sb, 233, 233, 227, 8, -135, 20, 0x103010, 0x33D173);
    }
  });
  timeIt("text Speed '128' (full)", 5, [&] { ui::vlw(ui::Font::Speed).draw(full, "128", 100, 256, white); });

  const MapImage* m = transfers::map();
  if (m != nullptr) {
    ui::MapCamera cam;
    cam.carX = 450;
    cam.carY = 700;
    for (float hdg : {0.0f, 45.0f, 90.0f}) {
      for (bool tilt : {false, true}) {
        cam.hdg = hdg;
        cam.tilt = tilt;
        char name[48];
        snprintf(name, sizeof(name), "drawMap hdg %d %s (bands)", static_cast<int>(hdg), tilt ? "tilt" : "flat");
        timeIt(name, 3, [&] {
          for (int y0 = 0; y0 < kW; y0 += kRows) {
            gfx::Surface sb{band, kW, kW - y0 < kRows ? kW - y0 : kRows, 1.0f, y0};
            ui::drawMap(sb, *m, cam, 0, 0);
          }
        });
        snprintf(name, sizeof(name), "drawMap hdg %d %s dark (bands)", static_cast<int>(hdg), tilt ? "tilt" : "flat");
        timeIt(name, 3, [&] {
          for (int y0 = 0; y0 < kW; y0 += kRows) {
            gfx::Surface sb{band, kW, kW - y0 < kRows ? kW - y0 : kRows, 1.0f, y0};
            ui::drawMap(sb, *m, cam, 0.66f, 0.5f);
          }
        });
      }
    }
  }
  heap_caps_free(band);
  heap_caps_free(frame);
}

}  // namespace monav::device::bench

#else

namespace monav::device::bench {
void run() {}
}  // namespace monav::device::bench

#endif
