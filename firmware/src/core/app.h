#pragma once

// The display's behaviour: pairing, the screens and when they change, local countdowns and
// distances, brightness. Pure C++ — the firmware feeds it BLE events, touch gestures and the
// clock; the host preview feeds it scripted messages. Not thread-safe: one task drives it.

#include <cstddef>
#include <cstdint>

#include "core/frame.h"
#include "core/gestures.h"
#include "core/model.h"
#include "core/nav.h"
#include "core/timesync.h"

namespace monav {

/** What the app needs from the hardware around it. */
class Platform {
 public:
  virtual ~Platform() = default;
  /** Send one display → phone message over `outbox` (the platform chunks it). */
  virtual void send(const char* json, size_t len) = 0;
  /** Drop the current BLE connection (after a pairing reset). */
  virtual void disconnect() = 0;
  /** Panel brightness: the user's percentage (5–100) times a dim factor (0–1). */
  virtual void setBrightness(int percent, float dim) = 0;

  virtual bool loadToken(char* out, size_t cap) = 0;
  virtual void saveToken(const char* token) = 0;
  virtual void eraseToken() = 0;
  /** −1 when nothing is stored. */
  virtual int loadBrightness() = 0;
  virtual void saveBrightness(int percent) = 0;
  /** −1 when nothing is stored, else the Lang value. */
  virtual int loadLang() = 0;
  virtual void saveLang(int lang) = 0;
  /** −1 when nothing is stored, else the Layout value. */
  virtual int loadLayout() = 0;
  virtual void saveLayout(int layout) = 0;
  /** −1 when nothing is stored, else degrees clockwise (0..359). */
  virtual int loadRotation() = 0;
  virtual void saveRotation(int degrees) = 0;
  /** The connection was welcomed (true) or is not any more (false): gates the bulk transfers. */
  virtual void authorizationChanged(bool authorized) = 0;
  /** The pairing changed: update the `info` characteristic. */
  virtual void pairingChanged(bool paired) = 0;
  virtual void log(const char* line) = 0;
};

class App {
 public:
  /** The firmware version: the one place it is written (the app's bundling script reads it here). */
  static constexpr const char* kFirmware = "0.2.6";

  static constexpr uint32_t kBootMs = 1800;
  static constexpr uint32_t kPairedMs = 2600;
  static constexpr uint32_t kVehicleStaleMs = 3000;   // no `v` this long: link lost
  static constexpr uint32_t kLinkLostMs = 20000;      // then back to waiting
  static constexpr uint32_t kStandbyDimMs = 60000;    // standby dims after this
  static constexpr uint32_t kResetAskMs = 5000;
  static constexpr uint32_t kBrightnessShowMs = 1400;
  static constexpr uint32_t kRotationSaveMs = 1500;   // the app's slider sends while it moves
  static constexpr float kCameraNearM = 1000;
  static constexpr float kCameraAlertM = 400;
  static constexpr float kLightNearM = 400;

  App(Platform& platform, const char* id);

  void begin(uint64_t now);

  // BLE
  void connected(uint64_t now);
  void disconnected(uint64_t now);
  void message(const Message& m, uint64_t now);

  // Input
  void gesture(const Gesture& g, uint64_t now);
  void setHold(float progress) { hold_ = progress; }
  void bootButtonHeld(uint64_t now);

  /** Once per frame, before frame(): easing, odometer, timeouts, brightness. */
  void tick(uint64_t now);
  void frame(Frame& out, uint64_t now) const;

  bool paired() const { return token_[0] != 0; }
  bool authorized() const { return authorized_; }
  Screen screen() const { return screen_; }
  int brightness() const { return bright_; }
  Layout layout() const { return layout_; }
  bool tilt() const { return tilt_; }
  /** Degrees clockwise the picture is turned on the glass (0..359). */
  int rotation() const { return rot_; }
  bool linkFresh(uint64_t now) const { return haveV_ && now - lastV_ <= kVehicleStaleMs; }

 private:
  Screen resolveScreen(uint64_t now) const;
  bool linkLost(uint64_t now) const;
  uint64_t lostSince() const;
  void handleHello(const Message& m, uint64_t now);
  void applyBrightness(uint64_t now);
  void sendStatus();
  void setLayout(Layout layout);
  void setLang(Lang lang);
  void setAuthorized(bool on);
  void resetPairing(uint64_t now);
  float runDown(float d) const;
  void logf(const char* fmt, ...);

  Platform& p_;
  char id_[5];
  char token_[68] = {0};
  Lang lang_ = Lang::De;
  bool hasTz_ = false;
  int tzMin_ = 0;
  TimeSync time_;

  uint64_t bootUntil_ = 0;
  uint64_t lastTick_ = 0;
  float t_ = 0;

  // Connection
  bool connected_ = false;
  bool authorized_ = false;  // welcome sent on this connection
  bool denied_ = false;      // denied sent on this connection
  uint64_t disconnectedAt_ = 0;
  uint64_t pairedUntil_ = 0;

  // Drive
  bool driveActive_ = false;
  bool standby_ = false;
  uint64_t standbyAt_ = 0;
  bool haveV_ = false;
  uint64_t lastV_ = 0;
  float kmh_ = 0;
  int lim_ = 0;
  float shownKmh_ = 0;
  float scaleMax_ = 100;
  Layout layout_ = Layout::Hud;
  bool tilt_ = true;
  int rot_ = 0;
  uint64_t rotChangedAt_ = 0;
  bool rotUnsaved_ = false;
  bool devSession_ = false;
  Nav nav_;

  // Scene and the distance run down since it arrived
  bool haveScene_ = false;
  Scene scene_;
  Chargers chargers_;
  double odo_ = 0;       // metres driven, by the latest speed
  double sceneOdo_ = 0;  // odometer reading at Scene::at

  // Screen tracking
  Screen screen_ = Screen::Boot;
  uint64_t screenSince_ = 0;

  // Overlays
  float hold_ = 0;
  bool resetAsk_ = false;
  uint64_t resetAskAt_ = 0;
  uint64_t brightnessShownAt_ = 0;
  bool brightnessShowing_ = false;
  uint64_t toastAt_ = 0;
  bool toastActive_ = false;

  // Brightness
  int bright_ = 80;
  int appliedBright_ = -1;
  float appliedDim_ = -1;
  float dim_ = 1;
};

}  // namespace monav
