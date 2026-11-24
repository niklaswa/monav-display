#pragma once

// Everything one frame shows. The app fills it every frame from its state; the renderer draws
// it and depends on nothing else, which is what lets the host build render the same screens to
// PNG.

#include <cstdint>

#include "core/model.h"
#include "core/strings.h"
#include "core/timesync.h"

namespace monav {

enum class Screen : uint8_t { Boot, Waiting, Paired, Denied, Connected, Drive, Standby, Welcome, Charging, Intro };

struct MapImage;   // core/mapdata.h
struct RouteLine;
struct Wallpaper;
struct AppIcon;

/** A surprise playing over the screen (only while the phone leaves them on, `cfg.eggs`). */
enum class Egg : uint8_t {
  None,
  Warp,      // seven quick taps on a screen without a drive
  TimeTravel,  // the speed passes 88 mph (141–142 km/h)
  Answer,    // 42 km/h held for a few seconds
};

/** Where the call's two buttons sit on the full notification (renderer and touch agree on it). */
constexpr float kCallButtonDx = 84;  // left (decline) and right (accept) of the centre
constexpr float kCallButtonY = 368;
constexpr float kCallButtonR = 38;

/** What the bottom of the drive HUD shows, in priority order. */
enum class Card : uint8_t { None, Camera, Section, Light, Turn };

struct Frame {
  Screen screen = Screen::Boot;
  Lang lang = Lang::De;
  float t = 0;          // seconds since boot: looping animations
  float screenAge = 0;  // seconds since this screen appeared: entry animations

  char id[5] = "0000";
  bool paired = false;

  bool hasClock = false;
  ClockTime clock;

  // --- Look (0.2.7) ---
  Theme theme = Theme::Classic;
  bool eggs = true;
  char name[24] = {0};
  /** The phone's picture for the photo theme; filled in by the caller, nullptr when none. */
  const Wallpaper* wallpaper = nullptr;
  Egg egg = Egg::None;
  float eggAge = 0;  // seconds since the surprise began
  bool afterIntro = false;  // this screen follows the performance intro: it comes out of its flash

  // --- Charging (0.2.7, `bat`) ---
  float soc = -1;  // percent
  float chargeKw = -1;
  float rangeKm = -1;

  // --- 0–100 km/h (0.2.7) ---
  float launchLive = -1;   // seconds into a launch while it runs (shown once it is clearly one); −1: none
  uint32_t launchMs = 0;   // the last run's time while it is shown; 0: none
  float launchAge = 0;     // seconds since that run ended
  bool launchBest = false; // it beat the best so far
  uint32_t launchBestMs = 0;

  // --- Drive HUD ---
  Layout layout = Layout::Hud;
  bool focus = false;     // speed + limit only, bigger (layout == Focus)
  bool tilt = true;       // map in perspective
  float speed = 0;        // km/h, eased towards the latest `v`
  int limit = 0;          // 0 = none known
  float scaleMax = 100;   // km/h at the end of the ring (eased when the limit changes)
  bool linkLost = false;  // no `v` for > 3 s or disconnected mid-drive
  float alert = 0;        // 0..1: camera close and too fast — the ring pulses red

  Card primary = Card::None;
  Card secondary = Card::None;
  Scene scene;            // the parts as last received
  float lightD = 0;       // distances run down since Scene::at
  float camD = 0;
  float turnD = 0;
  float lightLeft = -1;   // seconds until the light's phase ends; −1 = not in the forecast
  float lightPhase = -1;  // fraction of the phase still to go (1 → 0); −1 = unknown
  bool hasEta = false;
  ClockTime eta;

  // --- Map (layout == Map) ---
  bool hasPose = false;   // the car's place from `v`, run on and eased (core/nav)
  double lat = 0, lon = 0;
  float hdg = 0;
  /** Filled in by the caller (the map buffers belong to the device / preview, not the app). */
  const MapImage* map = nullptr;
  const RouteLine* route = nullptr;
  /** Charging stations as pins on the map (`chg`). */
  Chargers chargers;

  // --- Firmware update (filled in by the caller from the OTA writer; takes over the glass) ---
  struct Update {
    enum class Phase : uint8_t { None, Receiving, Verifying, Restarting, Failed };
    Phase phase = Phase::None;
    float progress = 0;  // 0..1
    char version[24] = {0};
    char error[40] = {0};
  } update;

  // --- An iPhone notification (0.2.11, ANCS) ---
  struct Notice {
    /** Full: over the whole glass, the icon large (parked, and calls). Compact: a pill while driving. */
    enum class Style : uint8_t { None, Full, Compact };
    Style style = Style::None;
    float age = 0;    // seconds since it appeared
    float out = 0;    // 0 → 1 while it fades away
    float left = -1;  // share of its time still to go (1 → 0); −1: until the iPhone ends it (a call)
    bool call = false;    // ringing: accept and decline on the glass
    bool missed = false;  // a missed call
    char app[64] = {0};      // bundle identifier (for the icon)
    char appName[32] = {0};  // shown above the title; empty: none
    char title[64] = {0};
    char body[192] = {0};    // the message, when the phone allows the text; else empty
    char positive[24] = {0};
    char negative[24] = {0};
    int waiting = 0;  // more notifications queued behind this one
  } notice;
  /** The notification's app icon; filled in by the caller, nullptr when there is none (yet). */
  const AppIcon* noticeIcon = nullptr;

  // --- Overlays ---
  float hold = 0;           // finger resting: 0..1 towards the 3 s reset question
  bool resetAsk = false;    // "Kopplung zurücksetzen? Tippen zum Bestätigen"
  float resetLeft = 0;      // 1 → 0 over the question's timeout
  float brightnessShow = 0; // brightness overlay opacity (while swiping)
  int brightness = 80;      // 5..100
  float toast = 0;          // opacity of a short message at the bottom
  Str toastText = Str::ResetDone;
};

}  // namespace monav
