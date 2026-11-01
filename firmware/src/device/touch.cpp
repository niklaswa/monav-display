#include "device/touch.h"

#include <Arduino.h>
#include <TouchDrvCSTXXX.hpp>
#include <Wire.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include "config.h"

namespace monav::device::touch {
namespace {

TouchDrvCSTXXX s_touch;
QueueHandle_t s_samples = nullptr;
volatile bool s_irq = false;

void IRAM_ATTR onTouchIsr() { s_irq = true; }

bool takeIrq() {
  noInterrupts();
  const bool pending = s_irq;
  s_irq = false;
  interrupts();
  return pending;
}

void push(bool down, int16_t x, int16_t y) {
  Sample s{down, x, y, millis()};
  if (xQueueSend(s_samples, &s, 0) != pdTRUE && !down) {
    // The queue is full of moves: make room so the lift is never lost.
    Sample dropped;
    xQueueReceive(s_samples, &dropped, 0);
    xQueueSend(s_samples, &s, 0);
  }
}

/**
 * The controller reports in bursts while a finger rests on the glass and goes quiet when it
 * lifts (sometimes without a final "no touch" report), so a gap in the interrupts ends a contact.
 * Coordinates are only read right after an interrupt: without fresh data the read is stale.
 */
void touchTask(void*) {
  bool down = false;
  uint32_t lastSeen = 0;
  int16_t lastX = 0, lastY = 0;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(10));
    const uint32_t now = millis();
    if (takeIrq()) {
      int16_t x[2] = {0}, y[2] = {0};
      const uint8_t n = s_touch.getPoint(x, y, 1);
      if (n > 0) {
        // Every report goes out, moved or not: a resting finger is what the hold timer needs.
        down = true;
        lastSeen = now;
        lastX = x[0];
        lastY = y[0];
        push(true, lastX, lastY);
        continue;
      }
      if (down) {
        down = false;
        push(false, lastX, lastY);
      }
      continue;
    }
    if (down && now - lastSeen >= config::kTouchReleaseGapMs) {
      down = false;
      push(false, lastX, lastY);
    }
  }
}

}  // namespace

bool begin() {
  s_samples = xQueueCreate(24, sizeof(Sample));
  s_touch.setPins(static_cast<int>(config::kTouchPinRst), static_cast<int>(config::kTouchPinInt));
  Wire.begin(static_cast<int>(config::kTouchPinSda), static_cast<int>(config::kTouchPinScl),
             static_cast<uint32_t>(config::kTouchI2cHz));
  if (!s_touch.begin(Wire, config::kTouchI2cAddress, static_cast<int>(config::kTouchPinSda),
                     static_cast<int>(config::kTouchPinScl))) {
    Serial.println("touch: no controller found - BOOT button only");
    return false;
  }
  s_touch.setMaxCoordinates(config::kDisplayWidth, config::kDisplayHeight);
  s_touch.setMirrorXY(config::kTouchMirrorX, config::kTouchMirrorY);
  Serial.printf("touch: %s ready\n", s_touch.getModelName());

  pinMode(config::kTouchPinInt, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(static_cast<uint8_t>(config::kTouchPinInt)), onTouchIsr, FALLING);
  xTaskCreatePinnedToCore(touchTask, "touch", 4096, nullptr, 4, nullptr, 1);
  return true;
}

bool poll(Sample& out) { return s_samples != nullptr && xQueueReceive(s_samples, &out, 0) == pdTRUE; }

}  // namespace monav::device::touch
