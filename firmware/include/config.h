#pragma once

// Board configuration for the Waveshare ESP32-S3-Touch-AMOLED-1.75, the only hardware this
// firmware targets. Pins and panel geometry are the ones proven in plane-radar-amoled.

#include <cstdint>

#include <driver/gpio.h>

namespace config {

// --- BOOT button (side button, active LOW) ---
constexpr gpio_num_t kBootPin = GPIO_NUM_0;
/** Holding BOOT this long forgets the paired phone (no confirmation). */
constexpr uint32_t kBootResetHoldMs = 3000;

// --- Display: CO5300 1.75" round AMOLED 466×466 (QSPI) ---
constexpr gpio_num_t kDisplayPinRst = GPIO_NUM_39;
constexpr gpio_num_t kDisplayPinCs = GPIO_NUM_12;
constexpr gpio_num_t kDisplayPinSclk = GPIO_NUM_38;
constexpr gpio_num_t kDisplayPinIo0 = GPIO_NUM_4;  // QSPI D0
constexpr gpio_num_t kDisplayPinIo1 = GPIO_NUM_5;  // QSPI D1
constexpr gpio_num_t kDisplayPinIo2 = GPIO_NUM_6;  // QSPI D2
constexpr gpio_num_t kDisplayPinIo3 = GPIO_NUM_7;  // QSPI D3
/**
 * The panel's tearing-effect line (enabled by command 0x35 in the init list). A frame push that
 * starts on its edge stays behind the panel's own refresh and never shows a seam. Taken from the
 * laby-amoled-minimap firmware; if no edge ever arrives the push stops waiting for it.
 */
constexpr gpio_num_t kDisplayPinTe = GPIO_NUM_13;

constexpr int kDisplayWidth = 466;
constexpr int kDisplayHeight = 466;
/** CO5300 RAM is wider than the visible panel; the visible area starts here. */
constexpr int kDisplayOffsetX = 6;
constexpr int kDisplayOffsetY = 0;
/**
 * The panel silently drops any write whose x or width is odd, so every frame is composited in
 * an off-screen PSRAM sprite and pushed as one full-width block.
 */

#ifndef MONAV_QSPI_HZ
/**
 * 80 MHz, as the minimap ran the same glass: measured on the board (2026-10-23) a push takes
 * 22 ms instead of 31 and the frame rate went from 10.6 to 21 fps, TE sync without a miss.
 * If the picture ever comes up scrambled, go back to 40 MHz (what plane-radar runs).
 */
#define MONAV_QSPI_HZ 80000000
#endif
constexpr uint32_t kDisplaySpiWriteHz = MONAV_QSPI_HZ;
constexpr bool kDisplayInvert = false;
constexpr bool kDisplayRgbOrder = false;

// --- Touch: CST9217 capacitive controller (I2C) ---
constexpr gpio_num_t kTouchPinSda = GPIO_NUM_15;
constexpr gpio_num_t kTouchPinScl = GPIO_NUM_14;
constexpr gpio_num_t kTouchPinInt = GPIO_NUM_11;
constexpr gpio_num_t kTouchPinRst = GPIO_NUM_40;
constexpr uint32_t kTouchI2cHz = 400000;
constexpr uint8_t kTouchI2cAddress = 0x5A;  // CST9217
/**
 * The controller reports mirrored on both axes relative to the panel (the Waveshare BSP sets
 * mirror_x and mirror_y for this module). Only the swipe direction depends on it.
 */
constexpr bool kTouchMirrorX = true;
constexpr bool kTouchMirrorY = true;
/**
 * Contact counts as ended once the controller has stopped reporting for this long: interrupts
 * arrive in bursts while a finger rests on the glass.
 */
constexpr uint32_t kTouchReleaseGapMs = 120;

// --- Frame pacing ---
constexpr uint32_t kFrameIntervalMs = 33;  // ~30 fps
constexpr uint32_t kFrameStatsIntervalMs = 10000;

}  // namespace config
