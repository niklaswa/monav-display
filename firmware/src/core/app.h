#pragma once

// The display's behaviour: pairing, the screens and when they change, local countdowns and
// distances, brightness. Pure C++ — the firmware feeds it BLE events, touch gestures and the
// clock; the host preview feeds it scripted messages. Not thread-safe: one task drives it.

#include <cstddef>
#include <cstdint>

#include "core/ancs.h"
#include "core/frame.h"
#include "core/gestures.h"
#include "core/launch.h"
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
  /** The look (theme, surprises, name); false when nothing is stored. */
  virtual bool loadStyle(Style& out) {
    (void)out;
    return false;
  }
  virtual void saveStyle(const Style& style) { (void)style; }
  /** The best 0–100 km/h time in ms; 0 when none is stored. */
  virtual uint32_t loadBestRun() { return 0; }
  virtual void saveBestRun(uint32_t ms) { (void)ms; }
  /** A picture from the phone is stored (reported in `welcome`). */
  virtual bool hasWallpaper() { return false; }
  /** The notification settings (cfg.ntf, cfg.ntfText); false when nothing is stored. */
  virtual bool loadNotices(NoticeMode& mode, bool& text, bool& full) {
    (void)mode;
    (void)text;
    (void)full;
    return false;
  }
  virtual void saveNotices(NoticeMode mode, bool text, bool full) {
    (void)mode;
    (void)text;
    (void)full;
  }
  /**
   * Whether the display should bond with the connected iPhone and subscribe to its notifications.
   * True only while the paired phone is welcomed and the phone wants notifications.
   */
  virtual void setNoticeLink(bool on) { (void)on; }
  /** Accept (positive) or decline a call on the iPhone. */
  virtual void noticeAction(uint32_t uid, bool positive) {
    (void)uid;
    (void)positive;
  }
  /** An icon for this app is in the display's cache. */
  virtual bool hasIcon(const char* app) {
    (void)app;
    return false;
  }
  /** The connection was welcomed (true) or is not any more (false): gates the bulk transfers. */
  virtual void authorizationChanged(bool authorized) = 0;
  /** The pairing changed: update the `info` characteristic. */
  virtual void pairingChanged(bool paired) = 0;
  virtual void log(const char* line) = 0;
};

class App {
 public:
  /** The firmware version: the one place it is written (the app's bundling script reads it here). */
  static constexpr const char* kFirmware = "0.2.12";

  static constexpr uint32_t kBootMs = 1800;
  static constexpr uint32_t kPairedMs = 2600;
  static constexpr uint32_t kVehicleStaleMs = 3000;   // no `v` this long: link lost
  static constexpr uint32_t kLinkLostMs = 20000;      // then back to waiting
  static constexpr uint32_t kStandbyDimMs = 60000;    // standby dims after this
  static constexpr uint32_t kResetAskMs = 5000;
  static constexpr uint32_t kBrightnessShowMs = 1400;
  static constexpr uint32_t kRotationSaveMs = 1500;   // the app's slider sends while it moves
  static constexpr uint32_t kWelcomeMs = 3800;     // the greeting when the phone connects
  static constexpr uint32_t kIntroMs = 3000;       // the performance intro before it (ends at the flash's peak)
  static constexpr uint32_t kIntroRepeatMs = 20000;  // a connection this soon after the boot intro skips it
  static constexpr uint32_t kTapGapMs = 700;       // taps further apart start counting anew
  static constexpr int kWarpTaps = 7;
  static constexpr uint32_t kAnswerHoldMs = 2500;  // 42 km/h this long
  static constexpr uint32_t kChargeStaleMs = 15000;  // no `bat` this long: not charging any more
  static constexpr uint32_t kRunShowMs = 8000;    // a timed run stays this long
  static constexpr float kRunLiveKmh = 15;        // the live timer from this speed on
  static constexpr uint32_t kNoticeMs = 8000;        // a notification stays this long (the icon alone first)
  static constexpr uint32_t kNoticeBusyMs = 4500;    // ... or this long with more waiting
  static constexpr uint32_t kCallMaxMs = 60000;      // a ringing call, unless the iPhone ends it first
  static constexpr uint32_t kNoticeFadeMs = 300;
  static constexpr float kNoticeFullKmh = 5;         // slower than this (parked): the whole glass
  static constexpr int kNoticeQueue = 4;
  static constexpr int kIconAsks = 12;               // icons asked for on one connection
  static constexpr float kCameraNearM = 1000;
  static constexpr float kCameraAlertM = 400;
  static constexpr float kLightNearM = 400;

  App(Platform& platform, const char* id);

  void begin(uint64_t now);

  // BLE
  void connected(uint64_t now);
  void disconnected(uint64_t now);
  void message(const Message& m, uint64_t now);

  // The iPhone's notifications (device/ancs)
  /** A notification arrived (or one changed: `modified`). */
  void notice(const ancs::Notice& n, bool modified, uint64_t now);
  /** The iPhone took a notification back (a call was answered or hung up there). */
  void noticeRemoved(uint32_t uid, uint64_t now);
  /** How the link to the iPhone's notifications stands: off, pairing, on, stale, denied, blocked. */
  void noticeLink(const char* state, uint64_t now);
  NoticeMode notices() const { return noticeMode_; }
  bool noticeShowing() const { return noticeOn_; }

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
  const Style& style() const { return style_; }
  Egg egg() const { return egg_; }
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
  void startEgg(Egg egg, uint64_t now);
  void startWelcome(uint64_t now);
  void tickEggs(uint64_t now, bool fresh);
  bool noticeWanted(const ancs::Notice& n) const;
  void showNotice(const ancs::Notice& n, uint64_t now);
  void hideNotice(uint64_t now);
  void tickNotices(uint64_t now);
  void askIcon(const char* app);
  void updateNoticeLink();
  bool noticeFull(uint64_t now) const;
  void toast(Str text, uint64_t now);
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

  // Look and surprises
  Style style_;
  uint64_t welcomeUntil_ = 0;
  uint64_t introUntil_ = 0;
  uint64_t introEndedAt_ = 0;
  // With the intro on, it replaces the traffic light at power-up; the first connection after
  // that does not play it a second time.
  bool bootIntro_ = false;
  bool bootIntroPending_ = false;
  Egg egg_ = Egg::None;
  uint64_t eggAt_ = 0;
  int taps_ = 0;
  uint64_t lastTapAt_ = 0;
  bool timeTravelDone_ = false;  // once per drive
  bool answerDone_ = false;
  uint64_t answerSince_ = 0;

  // Charging
  bool charging_ = false;
  uint64_t chargeAt_ = 0;  // last `bat`
  float soc_ = -1, chargeKw_ = -1, rangeKm_ = -1;

  // 0–100 km/h
  LaunchTimer launch_;
  bool launchOn_ = false;
  bool launchPhoneTime_ = false;  // the timer runs on the phone's clock (v carries `now`)
  uint32_t bestRunMs_ = 0;
  uint32_t runMs_ = 0;
  uint64_t runAt_ = 0;
  bool runBest_ = false;

  // The iPhone's notifications
  NoticeMode noticeMode_ = NoticeMode::Off;
  bool noticeText_ = false;
  bool noticeAlwaysFull_ = false;  // the whole glass while driving too (cfg.ntfFull)
  bool noticeLinkOn_ = false;
  char noticeLinkState_[12] = "off";
  bool noticeOn_ = false;         // one is shown (or fading out)
  ancs::Notice noticeCur_;
  uint64_t noticeAt_ = 0;         // when it appeared
  uint64_t noticeUntil_ = 0;      // when it starts to fade (0: hiding)
  uint64_t noticeHideAt_ = 0;     // when the fade began
  bool noticeFull_ = false;       // over the whole glass (decided when it appears, kept until it goes)
  ancs::Notice noticeQueue_[kNoticeQueue];
  int noticeQueued_ = 0;
  char iconAsked_[kIconAsks][64] = {};
  int iconAskedCount_ = 0;

  // Overlays
  float hold_ = 0;
  bool resetAsk_ = false;
  uint64_t resetAskAt_ = 0;
  uint64_t brightnessShownAt_ = 0;
  bool brightnessShowing_ = false;
  uint64_t toastAt_ = 0;
  bool toastActive_ = false;
  Str toastText_ = Str::ResetDone;

  // Brightness
  int bright_ = 80;
  int appliedBright_ = -1;
  float appliedDim_ = -1;
  float dim_ = 1;
};

}  // namespace monav
