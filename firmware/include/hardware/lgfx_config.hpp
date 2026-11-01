#pragma once

// LovyanGFX device for the CO5300 AMOLED of the Waveshare ESP32-S3-Touch-AMOLED-1.75.
// Copied from plane-radar-amoled (known good on this board); only the GC9A01 variant was dropped.

#define LGFX_USE_V1
#include <LovyanGFX.hpp>

#include "config.h"

#if !defined(LGFX_USE_QSPI)
#error "LovyanGFX QSPI support missing — needs ESP-IDF >= 4.4 on an ESP32-S3"
#endif

namespace lgfx_waveshare {

/**
 * CO5300 as wired on this module: 466×466 visible pixels inside a wider panel RAM, so the column
 * window starts at kDisplayOffsetX. LovyanGFX's stock Panel_CO5300 ships the T-Watch-Ultra
 * geometry (502×410), which is why the address-window init commands are replaced here.
 */
struct Panel_CO5300_466 : public lgfx::Panel_CO5300 {
  Panel_CO5300_466() {
    _cfg.memory_width = _cfg.panel_width = config::kDisplayWidth;
    _cfg.memory_height = _cfg.panel_height = config::kDisplayHeight;
  }

  const uint8_t* getInitCommands(uint8_t listno) const override {
    constexpr uint16_t kColStart = config::kDisplayOffsetX;
    constexpr uint16_t kColEnd = kColStart + config::kDisplayWidth - 1;
    constexpr uint16_t kRowStart = config::kDisplayOffsetY;
    constexpr uint16_t kRowEnd = kRowStart + config::kDisplayHeight - 1;

    // Mirrors Waveshare's own bring-up sequence for this module (esp-bsp component
    // `waveshare__esp32_s3_touch_amoled_1_75`). The page-0x20 registers are what LovyanGFX's
    // stock CO5300 list omits; without them the panel answers commands but never latches the
    // QSPI pixel stream.
    static constexpr uint8_t list0[] = {
        0xFE, 1, 0x20,  // switch to register page 0x20
        0x19, 1, 0x10,
        0x1C, 1, 0xA0,
        0xFE, 1, 0x00,  // back to page 0
        0xC4, 1, 0x80,  // SPI mode control: QSPI pixel writes
        0x3A, 1, 0x55,  // 16 bit/pixel
        0x35, 1, 0x00,  // tearing effect line on (V-blank only)
        0x53, 1, 0x20,  // brightness control on
        0x51, 1, 0x00,  // brightness 0 until the first frame is up (no flash of old RAM)
        0x63, 1, 0xFF,  // HBM brightness max
        0x2A, 4, kColStart >> 8, kColStart & 0xFF, kColEnd >> 8, kColEnd & 0xFF,
        0x2B, 4 | 0x80, kRowStart >> 8, kRowStart & 0xFF, kRowEnd >> 8,
        kRowEnd & 0xFF, 255,  // 255 = 500 ms settle
        0x11, 0x80, 255,      // sleep out, then wait
        0x29, 0x80, 10,       // display on
        0xFF, 0xFF            // end
    };

    return listno == 0 ? list0 : nullptr;
  }
};

}  // namespace lgfx_waveshare

/** LovyanGFX device: CO5300 AMOLED on QSPI. Pin values come from config.h. */
class LGFX : public lgfx::LGFX_Device {
  lgfx::Bus_SPI _bus;
  lgfx_waveshare::Panel_CO5300_466 _panel;

 public:
  LGFX() {
    {
      auto cfg = _bus.config();
      cfg.spi_host = SPI2_HOST;
      cfg.spi_mode = 0;
      cfg.freq_write = config::kDisplaySpiWriteHz;
      cfg.freq_read = 16000000;
      cfg.pin_sclk = static_cast<int>(config::kDisplayPinSclk);
      cfg.pin_io0 = static_cast<int>(config::kDisplayPinIo0);
      cfg.pin_io1 = static_cast<int>(config::kDisplayPinIo1);
      cfg.pin_io2 = static_cast<int>(config::kDisplayPinIo2);
      cfg.pin_io3 = static_cast<int>(config::kDisplayPinIo3);
      cfg.pin_dc = -1;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      _bus.config(cfg);
      _panel.setBus(&_bus);
    }
    {
      auto cfg = _panel.config();
      cfg.pin_cs = static_cast<int>(config::kDisplayPinCs);
      cfg.pin_rst = static_cast<int>(config::kDisplayPinRst);
      cfg.panel_width = config::kDisplayWidth;
      cfg.panel_height = config::kDisplayHeight;
      cfg.memory_width = config::kDisplayWidth;
      cfg.memory_height = config::kDisplayHeight;
      cfg.offset_x = config::kDisplayOffsetX;
      cfg.offset_y = config::kDisplayOffsetY;
      cfg.invert = config::kDisplayInvert;
      cfg.rgb_order = config::kDisplayRgbOrder;
      cfg.readable = false;
      cfg.dummy_read_pixel = 0;
      cfg.dummy_read_bits = 0;
      _panel.config(cfg);
    }
    setPanel(&_panel);
  }

  bool init() {
    // CO5300 needs a far longer reset than LovyanGFX's built-in 8/64 ms pulse.
    lgfx::pinMode(config::kDisplayPinRst, lgfx::pin_mode_t::output);
    lgfx::gpio_hi(config::kDisplayPinRst);
    lgfx::delay(20);
    lgfx::gpio_lo(config::kDisplayPinRst);
    lgfx::delay(200);
    lgfx::gpio_hi(config::kDisplayPinRst);
    lgfx::delay(200);

    return lgfx::LGFX_Device::init();
  }
};
