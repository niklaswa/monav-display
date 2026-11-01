#include "device/display.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <atomic>
#include <cstring>

#include "config.h"
#include "hardware/lgfx_config.hpp"
#include "ui/rotate.h"

#ifndef MONAV_PUSH_DMA
/**
 * 1: copy 16-row bands into two internal DMA buffers and push them with pushImageDMA, so core 0
 *    is free while the bytes go out (measured: push 42 → 27 ms, map layout +2 fps, HUD +4 fps).
 * 0: LGFX_Sprite::pushSprite straight from PSRAM (what plane-radar runs).
 */
#define MONAV_PUSH_DMA 1
#endif

namespace monav::device::display {
namespace {

LGFX s_tft;
LGFX_Sprite s_bufA(&s_tft);
LGFX_Sprite s_bufB(&s_tft);
LGFX_Sprite* const s_buf[2] = {&s_bufA, &s_bufB};
int s_draw = 0;
LGFX_Sprite* s_pending = nullptr;

SemaphoreHandle_t s_frameReady = nullptr;  // a frame waits to be pushed
SemaphoreHandle_t s_frameDone = nullptr;   // the push task is idle (its buffer is free)
SemaphoreHandle_t s_te = nullptr;          // tearing-effect edge
bool s_teSync = true;
int s_teMisses = 0;

std::atomic<int> s_brightWanted{-1};
std::atomic<int> s_rotation{0};
int s_brightApplied = -1;
bool s_firstFrame = true;
std::atomic<bool> s_pushedOnce{false};

portMUX_TYPE s_statsLock = portMUX_INITIALIZER_UNLOCKED;
Stats s_stats;

#if MONAV_PUSH_DMA
constexpr int kBandRows = 16;  // two of these in internal RAM: 2 × 15 kB
uint16_t* s_bands[2] = {nullptr, nullptr};
#endif

void IRAM_ATTR onTearingEffect() {
  BaseType_t woken = pdFALSE;
  xSemaphoreGiveFromISR(s_te, &woken);
  if (woken == pdTRUE) portYIELD_FROM_ISR();
}

/** Waits for the panel to start a refresh. Gives up on the line after repeated silence. */
bool waitForRefresh() {
  if (!s_teSync || s_te == nullptr) return false;
  xSemaphoreTake(s_te, 0);  // an edge from while we were busy is stale
  if (xSemaphoreTake(s_te, pdMS_TO_TICKS(25)) == pdTRUE) {
    s_teMisses = 0;
    return true;
  }
  if (++s_teMisses >= 8) {
    s_teSync = false;
    Serial.println("display: no tearing-effect signal on the TE pin - pushing unsynchronised");
  }
  return false;
}

void pushFrame(LGFX_Sprite& spr, const gfx::Rotation& rot) {
#if MONAV_PUSH_DMA
  if (s_bands[0] && s_bands[1]) {
    const uint16_t* src = static_cast<const uint16_t*>(spr.getBuffer());
    s_tft.startWrite();
    int i = 0;
    for (int y = 0; y < config::kDisplayHeight; y += kBandRows, i++) {
      const int rows = (config::kDisplayHeight - y) < kBandRows ? (config::kDisplayHeight - y) : kBandRows;
      uint16_t* band = s_bands[i & 1];
      // Turned while the other band's DMA runs, like the copy it replaces.
      if (rot.none()) {
        memcpy(band, src + y * config::kDisplayWidth, static_cast<size_t>(rows) * config::kDisplayWidth * 2);
      } else {
        gfx::rotateRows(src, config::kDisplayWidth, config::kDisplayHeight, rot, y, rows, band);
      }
      s_tft.pushImageDMA(0, y, config::kDisplayWidth, rows, reinterpret_cast<const lgfx::swap565_t*>(band));
    }
    s_tft.endWrite();
    return;
  }
#endif
  spr.pushSprite(&s_tft, 0, 0);
}

void pushTask(void*) {
  for (;;) {
    xSemaphoreTake(s_frameReady, portMAX_DELAY);
    LGFX_Sprite* spr = s_pending;
    static gfx::Rotation rot;
    if (s_rotation.load() != rot.degrees) rot = gfx::Rotation(s_rotation.load());
    const bool synced = waitForRefresh();
    const uint32_t t0 = micros();
    pushFrame(*spr, rot);
    const uint32_t us = micros() - t0;

    // Brightness only after the first frame is on the glass (the panel starts dark).
    const int want = s_brightWanted.load();
    if (want >= 0 && want != s_brightApplied) {
      s_tft.setBrightness(static_cast<uint8_t>(want));
      s_brightApplied = want;
    }
    if (s_firstFrame) {
      s_firstFrame = false;
      s_pushedOnce.store(true);
      Serial.printf("display: first frame pushed in %u us\n", static_cast<unsigned>(us));
    }

    portENTER_CRITICAL(&s_statsLock);
    s_stats.pushes++;
    s_stats.pushUsTotal += us;
    if (us > s_stats.pushUsMax) s_stats.pushUsMax = us;
    if (!synced && s_teSync) s_stats.teTimeouts++;
    s_stats.teSync = s_teSync;
    portEXIT_CRITICAL(&s_statsLock);

    xSemaphoreGive(s_frameDone);
  }
}

}  // namespace

bool begin() {
  if (!s_tft.init()) {
    Serial.println("display: panel init failed");
    return false;
  }
  s_tft.setRotation(0);
  s_tft.setBrightness(0);

  for (LGFX_Sprite* b : s_buf) {
    b->setColorDepth(16);
    b->setPsram(true);
    if (!b->createSprite(config::kDisplayWidth, config::kDisplayHeight)) {
      Serial.println("display: no PSRAM for the frame buffers");
      return false;
    }
    b->setTextWrap(false);
    b->fillScreen(0);
  }

#if MONAV_PUSH_DMA
  for (auto& band : s_bands) {
    band = static_cast<uint16_t*>(
        heap_caps_aligned_alloc(64, kBandRows * config::kDisplayWidth * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
  }
  if (!s_bands[0] || !s_bands[1]) Serial.println("display: no DMA bands - falling back to pushSprite");
#endif

  s_frameReady = xSemaphoreCreateBinary();
  s_frameDone = xSemaphoreCreateBinary();
  s_te = xSemaphoreCreateBinary();
  xSemaphoreGive(s_frameDone);

  pinMode(config::kDisplayPinTe, INPUT);
  attachInterrupt(digitalPinToInterrupt(config::kDisplayPinTe), onTearingEffect, RISING);

  // Core 0, next to the BLE host (which has the higher priority and preempts it).
  xTaskCreatePinnedToCore(pushTask, "push", 6144, nullptr, 2, nullptr, 0);
  Serial.printf("display: CO5300 466x466, QSPI %u MHz, %s push\n",
                static_cast<unsigned>(config::kDisplaySpiWriteHz / 1000000), MONAV_PUSH_DMA ? "DMA band" : "sprite");
  return true;
}

lgfx::LGFX_Sprite& backBuffer() { return *s_buf[s_draw]; }

void present() {
  xSemaphoreTake(s_frameDone, portMAX_DELAY);  // the other buffer is free again
  s_pending = s_buf[s_draw];
  s_draw ^= 1;
  xSemaphoreGive(s_frameReady);
}

void setBrightness(uint8_t level) { s_brightWanted.store(level); }

void setRotation(int degrees) { s_rotation.store(gfx::normalizeRotation(degrees)); }

bool firstFramePushed() { return s_pushedOnce.load(); }

Stats takeStats() {
  portENTER_CRITICAL(&s_statsLock);
  Stats s = s_stats;
  s_stats = Stats{};
  s_stats.teSync = s.teSync;
  portEXIT_CRITICAL(&s_statsLock);
  return s;
}

}  // namespace monav::device::display
