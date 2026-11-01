// Monav Display — firmware for the round display in the car that shows what the Monav iPhone
// app computes. The phone sends state over BLE (../PROTOCOL.md); everything on the glass is
// drawn here.
//
// Tasks:
//   ui     core 1   app logic + drawing into the back buffer (this file)
//   push   core 0   pushes finished frames to the panel on the tearing-effect edge
//   touch  core 1   CST9217 reports → finger samples
//   nimble core 0   BLE host: reassembles + parses writes, queues them for the ui task,
//                   takes bulk transfer bytes
//   bulk   core 0   writes firmware updates to flash, decodes map images

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_ota_ops.h>
#include <esp_task_wdt.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/semphr.h>

#include <cmath>
#include <cstdio>

#include "config.h"
#include "core/app.h"
#include "core/gestures.h"
#include "core/protocol.h"
#include "core/rxstats.h"
#include "device/ble_link.h"
#include "device/display.h"
#include "device/storage.h"
#include "device/demo.h"
#include "device/touch.h"
#include "device/transfers.h"
#include "ui/fonts.h"
#include "ui/render.h"
#include "ui/rotate.h"

// --- Embedded fonts (board_build.embed_files) ------------------------------------------------

#define MONAV_FONT(name)                                                                         \
  extern const uint8_t _binary_data_fonts_##name##_vlw_start[] asm("_binary_data_fonts_" #name \
                                                                   "_vlw_start");
extern "C" {
MONAV_FONT(speed)
MONAV_FONT(speed_xl)
MONAV_FONT(speed_md)
MONAV_FONT(num)
MONAV_FONT(num_sm)
MONAV_FONT(sign_lg)
MONAV_FONT(sign_md)
MONAV_FONT(sign_sm)
MONAV_FONT(sign_xs)
MONAV_FONT(title)
MONAV_FONT(label)
MONAV_FONT(body)
MONAV_FONT(small)
MONAV_FONT(clock)
MONAV_FONT(tiny)
}

namespace {

using namespace monav;

// In ui::Font order (ui/fonts.h, kFontFiles).
const uint8_t* const kFonts[static_cast<int>(ui::Font::Count)] = {
    _binary_data_fonts_speed_vlw_start,
    _binary_data_fonts_speed_xl_vlw_start,
    _binary_data_fonts_speed_md_vlw_start,
    _binary_data_fonts_num_vlw_start,
    _binary_data_fonts_num_sm_vlw_start,
    _binary_data_fonts_sign_lg_vlw_start,
    _binary_data_fonts_sign_md_vlw_start,
    _binary_data_fonts_sign_sm_vlw_start,
    _binary_data_fonts_sign_xs_vlw_start,
    _binary_data_fonts_title_vlw_start,
    _binary_data_fonts_label_vlw_start,
    _binary_data_fonts_body_vlw_start,
    _binary_data_fonts_small_vlw_start,
    _binary_data_fonts_clock_vlw_start,
    _binary_data_fonts_tiny_vlw_start,
};

uint64_t nowMs() { return static_cast<uint64_t>(esp_timer_get_time() / 1000); }
uint32_t microsNow() { return static_cast<uint32_t>(esp_timer_get_time()); }

/** The display's 4-hex-digit id: the last two bytes of the Bluetooth address. */
void deviceId(char* out, size_t cap) {
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_BT);
  snprintf(out, cap, "%02X%02X", mac[4], mac[5]);
}

char s_id[5] = "0000";

class DevicePlatform : public Platform {
 public:
  void send(const char* json, size_t len) override {
    Serial.printf("ble: -> %.*s\n", static_cast<int>(len), json);
    device::ble::send(json, len);
  }
  void disconnect() override { device::ble::disconnect(); }
  void setBrightness(int percent, float dim) override {
    // Perceived brightness grows much faster than the panel's level at the dark end.
    const float p = static_cast<float>(percent) / 100.0f;
    float level = (4.0f + std::pow(p, 1.5f) * 251.0f) * dim;
    if (level < 2) level = 2;
    if (level > 255) level = 255;
    device::display::setBrightness(static_cast<uint8_t>(level + 0.5f));
  }
  bool loadToken(char* out, size_t cap) override { return device::storage::loadToken(out, cap); }
  void saveToken(const char* token) override { device::storage::saveToken(token); }
  void eraseToken() override { device::storage::eraseToken(); }
  int loadBrightness() override { return device::storage::loadBrightness(); }
  void saveBrightness(int percent) override { device::storage::saveBrightness(percent); }
  int loadLang() override { return device::storage::loadLang(); }
  void saveLang(int lang) override { device::storage::saveLang(lang); }
  int loadLayout() override { return device::storage::loadLayout(); }
  void saveLayout(int layout) override { device::storage::saveLayout(layout); }
  int loadRotation() override { return device::storage::loadRotation(); }
  void saveRotation(int degrees) override { device::storage::saveRotation(degrees); }
  void authorizationChanged(bool authorized) override { device::transfers::setAuthorized(authorized); }
  void pairingChanged(bool paired) override {
    char info[96];
    proto::buildInfo(info, sizeof(info), App::kFirmware, s_id, paired);
    device::ble::setInfo(info);
  }
  void log(const char* line) override { Serial.println(line); }
};

DevicePlatform s_platform;
App* s_app = nullptr;
bool s_displayOk = false;
Gestures s_gestures;
Frame s_frame;  // big: kept off the task stack

/** BOOT held for 3 s forgets the paired phone, no questions asked. Returns hold progress. */
float pollBootButton(uint64_t now) {
  static uint64_t pressedAt = 0;
  static bool fired = false;
  const bool down = digitalRead(config::kBootPin) == LOW;
  if (!down) {
    pressedAt = 0;
    fired = false;
    return 0;
  }
  if (pressedAt == 0) pressedAt = now;
  const uint64_t held = now - pressedAt;
  if (!fired && held >= config::kBootResetHoldMs) {
    fired = true;
    s_app->bootButtonHeld(now);
  }
  if (fired || held < 300) return 0;
  return static_cast<float>(held - 300) / static_cast<float>(config::kBootResetHoldMs - 300);
}

// --- Firmware probation (bootloader app rollback) ----------------------------------------------

/**
 * A firmware that arrived over the air starts on probation: it confirms itself once it has drawn,
 * advertises and has run for kHealthyAfterMs. Until then a crash or hang sends the next boot back
 * to the previous firmware (the bootloader marks this one aborted).
 */
constexpr uint64_t kHealthyAfterMs = 15000;

const char* resetReasonName(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:
      return "power on";
    case ESP_RST_SW:
      return "restart";
    case ESP_RST_PANIC:
      return "crash";
    case ESP_RST_INT_WDT:
      return "interrupt watchdog";
    case ESP_RST_TASK_WDT:
      return "task watchdog";
    case ESP_RST_WDT:
      return "watchdog";
    case ESP_RST_BROWNOUT:
      return "brownout";
    default:
      return "other";
  }
}
bool s_pendingVerify = false;

void checkProbation() {
  const esp_partition_t* running = esp_ota_get_running_partition();
  esp_ota_img_states_t state;
  if (running != nullptr && esp_ota_get_state_partition(running, &state) == ESP_OK) {
    s_pendingVerify = state == ESP_OTA_IMG_PENDING_VERIFY;
    Serial.printf("ota: running %s (%s)\n", running->label,
                  s_pendingVerify               ? "on probation"
                  : state == ESP_OTA_IMG_VALID  ? "confirmed"
                  : state == ESP_OTA_IMG_UNDEFINED ? "flashed over USB"
                                                : "state unknown");
  } else if (running != nullptr) {
    Serial.printf("ota: running %s (flashed over USB)\n", running->label);
  }
}

void confirmIfHealthy(uint64_t now) {
  if (!s_pendingVerify) return;
  const bool ble = device::ble::advertising();
#if defined(MONAV_DEMO)
  const bool bleOk = true;  // the demo has no BLE
#else
  const bool bleOk = ble;
#endif
  if (now < kHealthyAfterMs || !device::display::firstFramePushed() || !bleOk) return;
  const esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
  s_pendingVerify = false;
  Serial.printf("ota: firmware %s confirmed (%s)\n", App::kFirmware, esp_err_to_name(err));
}

// --- The second drawing task (core 0) ---------------------------------------------------------

SemaphoreHandle_t s_bandGo = nullptr, s_bandDone = nullptr;
void (*s_bandFn)(void*) = nullptr;
void* s_bandArg = nullptr;

void bandTask(void*) {
  for (;;) {
    xSemaphoreTake(s_bandGo, portMAX_DELAY);
    s_bandFn(s_bandArg);
    xSemaphoreGive(s_bandDone);
  }
}

void bandRun(void (*fn)(void*), void* arg) {
  s_bandFn = fn;
  s_bandArg = arg;
  xSemaphoreGive(s_bandGo);
}

void bandWait() { xSemaphoreTake(s_bandDone, portMAX_DELAY); }

const char* screenName(const Frame& f) {
  if (f.update.phase != Frame::Update::Phase::None) return "update";
  switch (f.screen) {
    case Screen::Drive:
      return proto::layoutName(f.layout);
    case Screen::Waiting:
      return "waiting";
    case Screen::Connected:
      return "connected";
    case Screen::Standby:
      return "standby";
    default:
      return "other";
  }
}

void uiTask(void*) {
  // The task watchdog watches this loop: it waits for the second drawing task and the push every
  // frame, so a hang anywhere in drawing or pushing stops it too. 5 s without a frame resets the
  // display (on probation: back to the previous firmware).
  esp_task_wdt_add(nullptr);
  uint32_t frames = 0, drawUsTotal = 0, drawUsMax = 0, frameUsMax = 0;
  uint64_t statsAt = nowMs();
  const char* statsScreen = "";
  const uint32_t frameUs = config::kFrameIntervalMs * 1000;

  for (;;) {
    esp_task_wdt_reset();
    const uint32_t frameStart = micros();
    const uint64_t now = nowMs();

#if defined(MONAV_DEMO)
    device::demo::step(*s_app, now);
#endif
    // BLE: connection events and messages, in the order they happened.
    device::ble::Event ev;
    while (device::ble::poll(ev)) {
      switch (ev.kind) {
        case device::ble::Event::Connected:
          s_app->connected(now);
          break;
        case device::ble::Event::Disconnected:
          s_app->disconnected(now);
          break;
        case device::ble::Event::Received:
          s_app->message(ev.msg, now);
          break;
      }
    }

    // Touch → gestures, in the upright picture's coordinates (a swipe across the turned picture
    // is still a swipe across it).
    const gfx::Rotation rot(s_app->rotation());
    device::touch::Sample t;
    while (device::touch::poll(t)) {
      if (t.down) {
        int x = t.x, y = t.y;
        gfx::unrotatePoint(rot, config::kDisplayWidth, config::kDisplayHeight, x, y);
        s_gestures.down(x, y, t.ms);
      } else {
        s_gestures.up(t.ms);
      }
    }
    const uint32_t ms = millis();
    s_gestures.update(ms);
    Gesture g;
    while (s_gestures.poll(g)) s_app->gesture(g, now);
    const float bootHold = pollBootButton(now);
    const float touchHold = s_gestures.holdProgress(ms);
    s_app->setHold(bootHold > touchHold ? bootHold : touchHold);

    // Draw.
    device::transfers::frameBegin(now);
    s_app->tick(now);
    uint32_t drawUs = 0;
    if (s_displayOk) {
      s_app->frame(s_frame, now);
      s_frame.map = device::transfers::map();
      s_frame.route = device::transfers::route();
      s_frame.update = device::transfers::update();
      device::display::setRotation(s_app->rotation());
      lgfx::LGFX_Sprite& canvas = device::display::backBuffer();
      const uint32_t drawStart = micros();
      ui::render(canvas, s_frame);
      drawUs = micros() - drawStart;
      device::display::present();
    }
    confirmIfHealthy(now);
#if defined(MONAV_CRASH_TEST)
    // Rollback test build: dies before it could confirm itself.
    if (now > 6000) {
      Serial.println("crash test: abort() on purpose");
      abort();
    }
#endif
#if defined(MONAV_HANG_TEST)
    // Rollback test build: hangs before it could confirm itself (the watchdog must catch it).
    if (now > 6000) {
      Serial.println("hang test: the ui task stops here on purpose");
      for (;;) {
      }
    }
#endif

    const uint32_t loopUs = micros() - frameStart;
    frames++;
    drawUsTotal += drawUs;
    if (drawUs > drawUsMax) drawUsMax = drawUs;
    if (loopUs > frameUsMax) frameUsMax = loopUs;
    const char* screen = screenName(s_frame);
    if (now - statsAt >= config::kFrameStatsIntervalMs || (screen != statsScreen && now - statsAt >= 3000)) {
      const auto push = device::display::takeStats();
      const auto prof = ui::takeRenderProfile();
      const float secs = static_cast<float>(now - statsAt) / 1000.0f;
      const float n = static_cast<float>(prof.frames ? prof.frames : 1);
      Serial.printf(
          "frame [%s]: %.1f fps, draw %.1f ms avg / %.1f max (clear %.1f, copy %.1f, map %.1f, route %.1f, text %.1f, "
          "ring %.1f, card %.1f), "
          "push %.1f ms avg / %.1f max, loop max %.1f ms, TE %s (%u misses), heap %u kB, psram %u kB, mtu %u, "
          "interval %.2f ms\n",
          statsScreen[0] ? statsScreen : screen, frames / secs, drawUsTotal / 1000.0f / (frames ? frames : 1),
          drawUsMax / 1000.0f, prof.clearUs / 1000.0f / n, prof.copyUs / 1000.0f / n,
          prof.mapFrames ? prof.mapUs / 1000.0f / prof.mapFrames : 0.0f,
          prof.mapFrames ? prof.routeUs / 1000.0f / prof.mapFrames : 0.0f, prof.textUs / 1000.0f / n,
          prof.ringUs / 1000.0f / n, prof.cardUs / 1000.0f / n,
          push.pushes ? push.pushUsTotal / 1000.0f / push.pushes : 0.0f, push.pushUsMax / 1000.0f,
          frameUsMax / 1000.0f, push.teSync ? "sync" : "off", static_cast<unsigned>(push.teTimeouts),
          static_cast<unsigned>(ESP.getFreeHeap() / 1024), static_cast<unsigned>(ESP.getFreePsram() / 1024),
          static_cast<unsigned>(device::ble::mtu()), device::ble::connIntervalUs() / 1000.0f);
      Serial.printf("rx: v %u (with position %u), scene %u, cfg %u, bye %u, map begin %u (rejected %u), route %u, "
                    "bulk %u bytes\n",
                    static_cast<unsigned>(rxstats::vehicle.exchange(0)), static_cast<unsigned>(rxstats::vehicleWithPos.exchange(0)),
                    static_cast<unsigned>(rxstats::scene.exchange(0)), static_cast<unsigned>(rxstats::config.exchange(0)),
                    static_cast<unsigned>(rxstats::bye.exchange(0)), static_cast<unsigned>(rxstats::mapBegin.exchange(0)),
                    static_cast<unsigned>(rxstats::mapRejected.exchange(0)), static_cast<unsigned>(rxstats::routeBegin.exchange(0)),
                    static_cast<unsigned>(rxstats::bulkBytes.exchange(0)));
      frames = drawUsTotal = drawUsMax = frameUsMax = 0;
      statsAt = now;
    }
    statsScreen = screen;

    // Pace to the frame interval; a frame that ran long is followed straight away (a short
    // yield only), not by another full interval.
    const uint32_t spent = micros() - frameStart;
    if (spent + 1000 < frameUs) {
      vTaskDelay(pdMS_TO_TICKS((frameUs - spent) / 1000));
    } else {
      vTaskDelay(1);
    }
  }
}

}  // namespace

// Arduino's start-up would confirm an over-the-air firmware at once; this one confirms itself
// only once it is healthy (confirmIfHealthy).
extern "C" bool verifyRollbackLater() { return true; }

void setup() {
  // USB serial without a host reading it (in the car) must never hold anything up: room for bursts,
  // and a write gives up after 5 ms instead of waiting HWCDC's 100 ms for a reader. (Nothing calls
  // Serial.flush(): without a host it never returns.)
  Serial.setTxBufferSize(4096);
  Serial.setTxTimeoutMs(5);
  Serial.begin(115200);
  delay(50);
  Serial.printf("\nMonav Display %s (protocol %d)\n", App::kFirmware, proto::kVersion);
  checkProbation();
  // Core 0 draws half of every frame, pushes the previous one (the SPI wait is a busy loop) and
  // writes flash or decodes maps: under load it is never idle for long, so its idle task is no
  // health signal - it starved the task watchdog in the map layout and during updates. The
  // watchdog watches the tasks whose progress matters instead: ui (which waits for drawing and
  // pushing every frame) and the transfer worker.
  disableCore0WDT();

  pinMode(config::kBootPin, INPUT_PULLUP);
  device::storage::begin();
  deviceId(s_id, sizeof(s_id));

  if (!ui::initFonts(kFonts)) Serial.println("fonts: some VLW fonts failed to load");
  ui::setProfileClock(microsNow);
  // Frames are drawn band by band through internal RAM (many times faster per pixel than the
  // PSRAM frame) and copied into the frame one band at a time.
  // Two drawing tasks, one per core, draw every other band each.
  constexpr int kBandRows = 48;  // two of these in internal RAM (2 × 45 kB), next to BLE
  constexpr size_t kBandBytes = static_cast<size_t>(config::kDisplayWidth) * kBandRows * 2;
  auto* band0 = static_cast<uint16_t*>(heap_caps_malloc(kBandBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  auto* band1 = static_cast<uint16_t*>(heap_caps_malloc(kBandBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  if (band0 != nullptr && band1 != nullptr) {
    ui::setBandBuffers(band0, band1, kBandRows);
    s_bandGo = xSemaphoreCreateBinary();
    s_bandDone = xSemaphoreCreateBinary();
    if (xTaskCreatePinnedToCore(bandTask, "band", 8 * 1024, nullptr, 2, nullptr, 0) == pdPASS) {
      ui::setHelper(bandRun, bandWait);
    } else {
      Serial.println("display: no memory for the second drawing task - drawing on one core");
    }
  } else if (band0 != nullptr) {
    ui::setBandBuffer(band0, kBandRows);
    Serial.println("display: one band buffer only - drawing on one core");
  } else {
    Serial.println("display: no internal RAM for the band buffer - drawing straight into PSRAM");
  }
  s_displayOk = device::display::begin();
  if (!s_displayOk) Serial.println("display: bring-up failed - BLE keeps running, nothing is drawn");
  if (!device::transfers::begin()) Serial.println("transfers: map buffers missing");
  device::touch::begin();
#if defined(MONAV_DEMO)
  Serial.println("demo: scripted drive, BLE off");
#else
  device::ble::begin(s_id);
#endif

  static App app(s_platform, s_id);
  s_app = &app;
  app.begin(nowMs());
#if defined(MONAV_DEMO)
  device::demo::begin(app, nowMs());
#endif

  // Text is blitted straight from the font data (ui/text.h), no glyph buffers on the stack.
  if (xTaskCreatePinnedToCore(uiTask, "ui", 16 * 1024, nullptr, 3, nullptr, 1) != pdPASS) {
    Serial.println("ui: no memory for the drawing task");
  }
  // Late in the start: the first lines get lost while a USB host enumerates the port.
  Serial.printf("boot: reset reason %s\n", resetReasonName(esp_reset_reason()));
  Serial.printf("memory: %u kB internal free (largest block %u kB), %u kB PSRAM free\n",
                static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024),
                static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
}

void loop() {
  // Everything runs in its own task.
  vTaskDelete(nullptr);
}
