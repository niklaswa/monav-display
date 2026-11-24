#include "core/app.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "core/protocol.h"
#include "core/rxstats.h"

namespace monav {
namespace {

float approach(float value, float target, float dt, float tau) {
  if (tau <= 0) return target;
  return value + (target - value) * (1.0f - std::exp(-dt / tau));
}

int clampBright(int b) { return b < 5 ? 5 : b > 100 ? 100 : b; }

/** Ring scale for a limit: the limit sits at the top of the 270° ring. */
float scaleFor(int lim) { return lim > 0 ? 2.0f * static_cast<float>(lim) : 160.0f; }

}  // namespace

App::App(Platform& platform, const char* id) : p_(platform) {
  snprintf(id_, sizeof(id_), "%s", id);
}

void App::logf(const char* fmt, ...) {
  char line[160];
  va_list args;
  va_start(args, fmt);
  vsnprintf(line, sizeof(line), fmt, args);
  va_end(args);
  p_.log(line);
}

void App::begin(uint64_t now) {
  if (!p_.loadToken(token_, sizeof(token_))) token_[0] = 0;
  const int b = p_.loadBrightness();
  bright_ = b < 0 ? 80 : clampBright(b);
  const int lang = p_.loadLang();
  lang_ = lang == static_cast<int>(Lang::En) ? Lang::En : Lang::De;
  const int rot = p_.loadRotation();
  rot_ = rot < 0 ? 0 : rot % 360;
  bestRunMs_ = p_.loadBestRun();
  Style stored;
  if (p_.loadStyle(stored)) {
    style_ = stored;
    if (static_cast<int>(style_.theme) >= static_cast<int>(Theme::Count)) style_.theme = Theme::Classic;
    style_.name[sizeof(style_.name) - 1] = 0;
  }
  const int layout = p_.loadLayout();
  layout_ = layout == static_cast<int>(Layout::Focus)   ? Layout::Focus
            : layout == static_cast<int>(Layout::Map)   ? Layout::Map
            : layout == static_cast<int>(Layout::Clock) ? Layout::Clock
                                                         : Layout::Hud;
  NoticeMode mode;
  bool text = false, full = false;
  if (p_.loadNotices(mode, text, full)) {
    noticeMode_ = mode;
    noticeText_ = text;
    noticeAlwaysFull_ = full;
  }
  bootIntro_ = style_.intro;
  bootIntroPending_ = bootIntro_;
  bootUntil_ = now + (bootIntro_ ? kIntroMs : kBootMs);
  lastTick_ = now;
  screen_ = Screen::Boot;
  screenSince_ = now;
  p_.pairingChanged(paired());
  logf("app: firmware %s, id %s, %s, brightness %d, layout %s, rotation %d, theme %s", kFirmware, id_,
       paired() ? "paired" : "not paired", bright_, proto::layoutName(layout_), rot_, proto::themeName(style_.theme));
  applyBrightness(now);
}

// --- BLE -------------------------------------------------------------------------------------

void App::connected(uint64_t now) {
  (void)now;
  connected_ = true;
  iconAskedCount_ = 0;
  setAuthorized(false);
  denied_ = false;
  devSession_ = false;
  logf("app: phone connected");
}

void App::disconnected(uint64_t now) {
  connected_ = false;
  setAuthorized(false);
  denied_ = false;
  devSession_ = false;
  disconnectedAt_ = now;
  // The notifications came over this connection: a ringing call cannot be answered any more.
  noticeQueued_ = 0;
  if (noticeOn_ && noticeUntil_ != 0) hideNotice(now);
  snprintf(noticeLinkState_, sizeof(noticeLinkState_), "off");
  logf("app: phone disconnected");
}

void App::setAuthorized(bool on) {
  if (on == authorized_) return;
  authorized_ = on;
  p_.authorizationChanged(on);
  updateNoticeLink();
}

void App::setLang(Lang lang) {
  if (lang == lang_) return;
  lang_ = lang;
  p_.saveLang(static_cast<int>(lang_));
}

void App::handleHello(const Message& m, uint64_t now) {
  if (m.hasLang) setLang(m.lang);
  if (m.hasTz) {
    hasTz_ = true;
    tzMin_ = m.tzMin;
  }
  if (m.hasNow) {
    time_.reset();
    time_.sample(m.now, now);
  }

  char out[200];
  size_t n;
#ifdef MONAV_DEV_TOKEN
  // Development builds only: a test client (tools/ble) gets in without touching the pairing.
  if (m.token[0] != 0 && strcmp(m.token, MONAV_DEV_TOKEN) == 0) {
    denied_ = false;
    devSession_ = true;
    standby_ = false;
    setAuthorized(true);
    n = proto::buildWelcome(out, sizeof(out), kFirmware, id_, false, bright_, layout_, p_.hasWallpaper());
    if (!driveActive_) startWelcome(now);
    logf("app: dev client (not paired, pairing untouched)");
    p_.send(out, n);
    return;
  }
#endif
  if (m.token[0] == 0) {
    setAuthorized(false);
    denied_ = true;
    n = proto::buildDenied(out, sizeof(out), id_);
    logf("app: hello without token: denied");
  } else if (!paired()) {
    snprintf(token_, sizeof(token_), "%s", m.token);
    p_.saveToken(token_);
    p_.pairingChanged(true);
    denied_ = false;
    pairedUntil_ = now + kPairedMs;
    // Authorized only after the welcome is out: from then on bulk transfers are accepted.
    n = proto::buildWelcome(out, sizeof(out), kFirmware, id_, true, bright_, layout_, p_.hasWallpaper());
    p_.send(out, n);
    setAuthorized(true);
    logf("app: paired with a new phone");
    return;
  } else if (strcmp(token_, m.token) == 0) {
    denied_ = false;
    standby_ = false;  // a new connection after bye: connected, waiting for the drive
    // Getting into the car: a greeting — not when the link only dropped for a moment mid-drive.
    if (!driveActive_) startWelcome(now);
    n = proto::buildWelcome(out, sizeof(out), kFirmware, id_, false, bright_, layout_, p_.hasWallpaper());
    p_.send(out, n);
    setAuthorized(true);
    logf("app: welcome back");
    return;
  } else {
    setAuthorized(false);
    denied_ = true;
    n = proto::buildDenied(out, sizeof(out), id_);
    logf("app: hello from another phone: denied");
  }
  if (m.proto != proto::kVersion) logf("app: phone speaks protocol %d, display %d", m.proto, proto::kVersion);
  p_.send(out, n);
}

void App::message(const Message& m, uint64_t now) {
  if (m.type == MsgType::Hello) {
    handleHello(m, now);
    return;
  }
  // Until welcome, everything else from this connection is ignored.
  if (!authorized_) return;

  switch (m.type) {
    case MsgType::Vehicle: {
      rxstats::vehicle++;
      if (m.hasPos) rxstats::vehicleWithPos++;
      if (m.hasNow) time_.sample(m.now, now);
      kmh_ = m.kmh;
      if (m.lim != lim_) lim_ = m.lim;
      if (!haveV_) shownKmh_ = kmh_;
      if (!driveActive_) {
        // A drive (re)starts: the ring scale snaps instead of sweeping in from the last one.
        scaleMax_ = scaleFor(lim_);
        timeTravelDone_ = false;
        answerDone_ = false;
        answerSince_ = 0;
      }
      haveV_ = true;
      lastV_ = now;
      if (launchOn_) {
        // On the phone's clock when it is sent: the speed's own timestamps, no radio jitter.
        const bool phoneTime = m.hasNow;
        if (phoneTime != launchPhoneTime_) launch_.reset();
        launchPhoneTime_ = phoneTime;
        const uint32_t ms = launch_.sample(phoneTime ? m.now : static_cast<int64_t>(now), m.kmh);
        if (ms > 0) {
          runMs_ = ms;
          runAt_ = now;
          runBest_ = bestRunMs_ == 0 || ms < bestRunMs_;
          if (runBest_) {
            bestRunMs_ = ms;
            p_.saveBestRun(ms);
          }
          char out[64];
          const size_t n = proto::buildRun(out, sizeof(out), ms, runBest_);
          p_.send(out, n);
          logf("app: 0-100 in %u ms%s", static_cast<unsigned>(ms), runBest_ ? " (best)" : "");
        }
      }
      driveActive_ = true;
      standby_ = false;
      if (m.hasPos) nav_.fix(m.lat, m.lon, m.hasHdg, m.hdg, now);
      break;
    }
    case MsgType::Chargers:
      chargers_ = m.chargers;
      break;
    case MsgType::Battery:
      if (m.batOn != charging_) logf("app: charging %s", m.batOn ? "started" : "ended");
      charging_ = m.batOn;
      chargeAt_ = now;
      if (m.soc >= 0) soc_ = m.soc;
      chargeKw_ = m.kw;
      if (m.km >= 0) rangeKm_ = m.km;
      break;
    case MsgType::Scene: {
      rxstats::scene++;
      scene_ = m.scene;
      haveScene_ = true;
      // Distances refer to Scene::at; the scene is already that old when it arrives.
      double age = 0;
      if (time_.valid() && scene_.at > 0) {
        age = static_cast<double>(time_.phoneNow(now) - scene_.at) / 1000.0;
        if (age < 0) age = 0;
        if (age > 10) age = 10;
      }
      const bool fresh = haveV_ && now - lastV_ <= kVehicleStaleMs;
      sceneOdo_ = odo_ - (fresh ? kmh_ / 3.6 * age : 0.0);
      break;
    }
    case MsgType::Config:
      rxstats::config++;
      if (m.bright >= 0) {
        const int b = clampBright(m.bright);
        if (b != bright_) {
          bright_ = b;
          p_.saveBrightness(bright_);
        }
      }
      if (m.hasTz) {
        hasTz_ = true;
        tzMin_ = m.tzMin;
      }
      if (m.hasTilt) tilt_ = m.tilt;
      if (m.hasLang) setLang(m.lang);
      if (m.rot >= 0 && m.rot != rot_) {
        rot_ = m.rot;
        rotChangedAt_ = now;
        rotUnsaved_ = true;
      }
      if (m.hasLayout) setLayout(m.layout);
      if ((m.hasNotices && m.notices != noticeMode_) || (m.hasNoticeText && m.noticeText != noticeText_) ||
          (m.hasNoticeFull && m.noticeFull != noticeAlwaysFull_)) {
        if (m.hasNotices) noticeMode_ = m.notices;
        if (m.hasNoticeText) noticeText_ = m.noticeText;
        if (m.hasNoticeFull) noticeAlwaysFull_ = m.noticeFull;
        p_.saveNotices(noticeMode_, noticeText_, noticeAlwaysFull_);
        logf("app: iPhone notifications %s%s%s",
             noticeMode_ == NoticeMode::All ? "all" : noticeMode_ == NoticeMode::Calls ? "calls" : "off",
             noticeText_ ? ", with text" : "", noticeAlwaysFull_ ? ", always large" : "");
        if (noticeMode_ == NoticeMode::Off) {
          noticeQueued_ = 0;
          if (noticeOn_ && noticeUntil_ != 0) hideNotice(now);
        }
        updateNoticeLink();
      }
      if (m.hasLaunch && m.launch != launchOn_) {
        launchOn_ = m.launch;
        launch_.reset();
      }
      if (m.hasTheme || m.hasEggs || m.hasName || m.hasIntro) {
        Style next = style_;
        if (m.hasIntro) next.intro = m.intro;
        if (m.hasTheme) next.theme = m.theme;
        if (m.hasEggs) next.eggs = m.eggs;
        if (m.hasName) snprintf(next.name, sizeof(next.name), "%s", m.name);
        if (next.theme != style_.theme || next.eggs != style_.eggs || next.intro != style_.intro ||
            strcmp(next.name, style_.name) != 0) {
          style_ = next;
          p_.saveStyle(style_);
          logf("app: theme %s, surprises %s", proto::themeName(style_.theme), style_.eggs ? "on" : "off");
        }
        if (!style_.eggs) egg_ = Egg::None;
      }
      break;
    case MsgType::Dev:
      if (devSession_ && m.hasLayout) setLayout(m.layout);
      break;
    case MsgType::Bye:
      rxstats::bye++;
      standby_ = true;
      standbyAt_ = now;
      welcomeUntil_ = introUntil_ = 0;
      launch_.reset();
      driveActive_ = false;
      haveV_ = false;
      nav_.reset();
      chargers_ = Chargers{};
      logf("app: bye");
      break;
    default:
      break;
  }
}

// --- Input -----------------------------------------------------------------------------------

void App::gesture(const Gesture& g, uint64_t now) {
  // A notification on the glass takes the tap: the call's buttons, or away with it.
  if (g.type == GestureType::Tap && !resetAsk_ && noticeOn_ && noticeUntil_ != 0) {
    if (noticeCur_.isCall() && noticeFull_ && g.x >= 0) {
      const float hit = kCallButtonR * 1.6f;
      for (int side = -1; side <= 1; side += 2) {
        const float dx = g.x - (233.0f + side * kCallButtonDx), dy = g.y - kCallButtonY;
        if (dx * dx + dy * dy <= hit * hit) {
          p_.noticeAction(noticeCur_.uid, side > 0);
          logf("app: call %s on the glass", side > 0 ? "accepted" : "declined");
          break;
        }
      }
    }
    hideNotice(now);
    return;
  }
  switch (g.type) {
    case GestureType::Tap:
      if (resetAsk_) {
        resetPairing(now);
      } else if (screen_ == Screen::Drive) {
        // hud → focus → map → clock → hud
        setLayout(layout_ == Layout::Hud     ? Layout::Focus
                  : layout_ == Layout::Focus ? Layout::Map
                  : layout_ == Layout::Map   ? Layout::Clock
                                             : Layout::Hud);
      } else if (style_.eggs && (screen_ == Screen::Connected || screen_ == Screen::Standby ||
                                 screen_ == Screen::Welcome)) {
        // A secret: seven quick taps on the glass while parked.
        taps_ = now - lastTapAt_ <= kTapGapMs ? taps_ + 1 : 1;
        lastTapAt_ = now;
        if (taps_ >= kWarpTaps) {
          taps_ = 0;
          startEgg(Egg::Warp, now);
        }
      }
      break;
    case GestureType::LongPress:
      if (!resetAsk_) {
        resetAsk_ = true;
        resetAskAt_ = now;
      }
      break;
    case GestureType::BrightnessStep:
      bright_ = clampBright(bright_ + g.value * 10);
      brightnessShowing_ = true;
      brightnessShownAt_ = now;
      break;
    case GestureType::SwipeEnd:
      brightnessShownAt_ = now;
      p_.saveBrightness(bright_);
      sendStatus();
      break;
  }
}

void App::bootButtonHeld(uint64_t now) {
  logf("app: BOOT held - forgetting the paired phone");
  resetPairing(now);
}

void App::resetPairing(uint64_t now) {
  token_[0] = 0;
  p_.eraseToken();
  p_.pairingChanged(false);
  resetAsk_ = false;
  driveActive_ = false;
  standby_ = false;
  haveV_ = false;
  haveScene_ = false;
  authorized_ = false;
  denied_ = false;
  toast(Str::ResetDone, now);
  // The phone on the line was told welcome or denied; dropping it makes it connect again and
  // send a new hello, which now pairs.
  if (connected_) p_.disconnect();
  logf("app: pairing reset");
}

void App::toast(Str text, uint64_t now) {
  toastText_ = text;
  toastActive_ = true;
  toastAt_ = now;
}

// --- The iPhone's notifications ---------------------------------------------------------------

void App::updateNoticeLink() {
  const bool on = authorized_ && !devSession_ && noticeMode_ != NoticeMode::Off;
  if (on == noticeLinkOn_) return;
  noticeLinkOn_ = on;
  p_.setNoticeLink(on);
}

void App::noticeLink(const char* state, uint64_t now) {
  if (state == nullptr || strcmp(state, noticeLinkState_) == 0) return;
  const bool wasOn = strcmp(noticeLinkState_, "on") == 0;
  snprintf(noticeLinkState_, sizeof(noticeLinkState_), "%s", state);
  logf("app: iPhone notifications: %s", state);
  // The iPhone asks to pair: the glass says where to look. Once it works, that it does.
  if (strcmp(state, "pairing") == 0) {
    toast(Str::NoticePair, now);
  } else if (strcmp(state, "on") == 0 && !wasOn && toastActive_ && toastText_ == Str::NoticePair) {
    toast(Str::NoticesOn, now);
  }
  if (!authorized_) return;
  char out[48];
  const size_t n = proto::buildNoticeState(out, sizeof(out), state);
  p_.send(out, n);
}

bool App::noticeWanted(const ancs::Notice& n) const {
  if (noticeMode_ == NoticeMode::Off || (n.flags & ancs::flag::kPreExisting) != 0) return false;
  if (noticeMode_ == NoticeMode::Calls) {
    return n.category == ancs::Category::IncomingCall || n.category == ancs::Category::MissedCall;
  }
  return true;
}

void App::askIcon(const char* app) {
  if (!authorized_ || app == nullptr || app[0] == 0 || p_.hasIcon(app)) return;
  for (int i = 0; i < iconAskedCount_; i++) {
    if (strcmp(iconAsked_[i], app) == 0) return;
  }
  if (iconAskedCount_ >= kIconAsks) return;
  snprintf(iconAsked_[iconAskedCount_++], sizeof(iconAsked_[0]), "%s", app);
  char out[112];
  const size_t n = proto::buildIconRequest(out, sizeof(out), app);
  if (n > 0) p_.send(out, n);
}

void App::showNotice(const ancs::Notice& n, uint64_t now) {
  noticeCur_ = n;
  noticeOn_ = true;
  noticeAt_ = now;
  noticeUntil_ = now + (n.isCall() ? kCallMaxMs : kNoticeMs);
  noticeHideAt_ = 0;
  noticeFull_ = noticeFull(now);
}

void App::hideNotice(uint64_t now) {
  if (!noticeOn_ || noticeUntil_ == 0) return;
  noticeUntil_ = 0;
  noticeHideAt_ = now;
}

void App::notice(const ancs::Notice& n, bool modified, uint64_t now) {
  if (modified) {
    // A changed notification is updated where it is, not shown again.
    if (noticeOn_ && noticeUntil_ != 0 && noticeCur_.uid == n.uid) noticeCur_ = n;
    for (int i = 0; i < noticeQueued_; i++) {
      if (noticeQueue_[i].uid == n.uid) noticeQueue_[i] = n;
    }
    return;
  }
  if (!noticeWanted(n)) return;
  logf("app: notification %u (category %d) from %s", static_cast<unsigned>(n.uid), static_cast<int>(n.category),
       n.app);
  askIcon(n.app);
  const bool visible = noticeOn_ && noticeUntil_ != 0;
  if (!noticeOn_) {
    showNotice(n, now);
    return;
  }
  if (n.isCall()) {
    // A call comes first; what it pushes aside waits for it.
    if (visible && !noticeCur_.isCall() && noticeQueued_ < kNoticeQueue) {
      for (int i = noticeQueued_; i > 0; i--) noticeQueue_[i] = noticeQueue_[i - 1];
      noticeQueue_[0] = noticeCur_;
      noticeQueued_++;
    }
    showNotice(n, now);
    return;
  }
  if (noticeQueued_ == kNoticeQueue) {
    for (int i = 1; i < kNoticeQueue; i++) noticeQueue_[i - 1] = noticeQueue_[i];
    noticeQueued_--;
  }
  noticeQueue_[noticeQueued_++] = n;
}

void App::noticeRemoved(uint32_t uid, uint64_t now) {
  if (noticeOn_ && noticeCur_.uid == uid) hideNotice(now);
  int k = 0;
  for (int i = 0; i < noticeQueued_; i++) {
    if (noticeQueue_[i].uid != uid) noticeQueue_[k++] = noticeQueue_[i];
  }
  noticeQueued_ = k;
}

void App::tickNotices(uint64_t now) {
  if (!noticeOn_) return;
  if (noticeUntil_ != 0) {
    // More waiting: each goes sooner. A call stays until it ends.
    if (noticeQueued_ > 0 && !noticeCur_.isCall() && noticeUntil_ > noticeAt_ + kNoticeBusyMs) {
      noticeUntil_ = noticeAt_ + kNoticeBusyMs;
    }
    if (now >= noticeUntil_) hideNotice(now);
    return;
  }
  if (now - noticeHideAt_ < kNoticeFadeMs) return;
  noticeOn_ = false;
  if (noticeQueued_ > 0) {
    const ancs::Notice next = noticeQueue_[0];
    for (int i = 1; i < noticeQueued_; i++) noticeQueue_[i - 1] = noticeQueue_[i];
    noticeQueued_--;
    showNotice(next, now);
  }
}

bool App::noticeFull(uint64_t now) const {
  (void)now;
  if (noticeCur_.isCall() || noticeAlwaysFull_) return true;
  return screen_ != Screen::Drive || kmh_ < kNoticeFullKmh;
}

void App::startWelcome(uint64_t now) {
  // The intro already played at power-up: the greeting comes out of that flash (connected while
  // it ran), or straight away (connected just after) — never the jump twice in a row.
  if (bootIntroPending_ && style_.intro) {
    bootIntroPending_ = false;
    if (now < bootUntil_) {
      introUntil_ = 0;
      welcomeUntil_ = bootUntil_ + kWelcomeMs;
      return;
    }
    if (now - bootUntil_ < kIntroRepeatMs) {
      introUntil_ = 0;
      welcomeUntil_ = now + kWelcomeMs;
      return;
    }
  }
  bootIntroPending_ = false;
  // With the intro: the hyperspace jump first, the greeting out of its flash.
  introUntil_ = style_.intro ? now + kIntroMs : 0;
  welcomeUntil_ = (style_.intro ? introUntil_ : now) + kWelcomeMs;
}

// --- Surprises ----------------------------------------------------------------------------------

namespace {
uint32_t eggMs(Egg egg) {
  switch (egg) {
    case Egg::Warp:
      return 5200;
    case Egg::TimeTravel:
      return 4200;
    case Egg::Answer:
      return 4500;
    default:
      return 0;
  }
}
}  // namespace

void App::startEgg(Egg egg, uint64_t now) {
  if (!style_.eggs) return;
  egg_ = egg;
  eggAt_ = now;
  logf("app: surprise %d", static_cast<int>(egg));
}

void App::tickEggs(uint64_t now, bool fresh) {
  if (egg_ != Egg::None && now - eggAt_ > eggMs(egg_)) egg_ = Egg::None;
  if (!style_.eggs || !driveActive_ || !fresh) {
    answerSince_ = 0;
    return;
  }
  const long kmh = std::lround(kmh_);
  // 88 mph is 141.6 km/h.
  if (!timeTravelDone_ && kmh >= 141 && kmh <= 143) {
    timeTravelDone_ = true;
    startEgg(Egg::TimeTravel, now);
  }
  if (kmh == 42) {
    if (answerSince_ == 0) answerSince_ = now;
    if (!answerDone_ && now - answerSince_ >= kAnswerHoldMs) {
      answerDone_ = true;
      startEgg(Egg::Answer, now);
    }
  } else {
    answerSince_ = 0;
  }
}

void App::sendStatus() {
  if (!authorized_) return;
  char out[48];
  const size_t n = proto::buildStatus(out, sizeof(out), bright_);
  p_.send(out, n);
}

void App::setLayout(Layout layout) {
  if (layout == layout_) return;
  layout_ = layout;
  p_.saveLayout(static_cast<int>(layout_));
  logf("app: layout %s", proto::layoutName(layout_));
  if (!authorized_) return;
  char out[48];
  const size_t n = proto::buildStatus(out, sizeof(out), -1, &layout_);
  p_.send(out, n);
}

// --- Per frame -------------------------------------------------------------------------------

bool App::linkLost(uint64_t now) const {
  if (!driveActive_) return false;
  return !connected_ || now - lastV_ > kVehicleStaleMs;
}

uint64_t App::lostSince() const {
  const uint64_t stale = lastV_ + kVehicleStaleMs;
  if (!connected_ && disconnectedAt_ < stale) return disconnectedAt_;
  return stale;
}

Screen App::resolveScreen(uint64_t now) const {
  if (now < bootUntil_) return bootIntro_ ? Screen::Intro : Screen::Boot;
  if (denied_) return Screen::Denied;
  if (now < pairedUntil_ && authorized_) return Screen::Paired;
  // The greeting stays up while the car stands; driving off ends it.
  if (now < introUntil_ && authorized_ && !(driveActive_ && kmh_ >= 10)) return Screen::Intro;
  if (now < welcomeUntil_ && authorized_ && !(driveActive_ && kmh_ >= 10)) return Screen::Welcome;
  // At the charger the phone may still send `v` (at 0): charging wins until the car moves.
  if (charging_ && soc_ >= 0 && !(driveActive_ && kmh_ >= 3)) return Screen::Charging;
  if (driveActive_) return Screen::Drive;
  if (standby_) return Screen::Standby;
  if (authorized_) return Screen::Connected;
  return Screen::Waiting;
}

void App::applyBrightness(uint64_t now) {
  float dim = 1.0f;
  if (standby_ && now - standbyAt_ > kStandbyDimMs && !noticeOn_) dim = 0.25f;
  // Ease the dim factor so standby fades rather than steps.
  const float dt = 1.0f / 30.0f;
  dim_ = std::fabs(dim - dim_) < 0.01f ? dim : approach(dim_, dim, dt, 0.6f);
  if (bright_ != appliedBright_ || std::fabs(dim_ - appliedDim_) > 0.004f) {
    appliedBright_ = bright_;
    appliedDim_ = dim_;
    p_.setBrightness(bright_, dim_);
  }
}

void App::tick(uint64_t now) {
  float dt = static_cast<float>(now - lastTick_) / 1000.0f;
  if (dt < 0) dt = 0;
  if (dt > 0.25f) dt = 0.25f;
  lastTick_ = now;
  t_ += dt;

  // Saved once the angle has stopped moving: the app sends every step of its slider.
  if (rotUnsaved_ && now - rotChangedAt_ >= kRotationSaveMs) {
    rotUnsaved_ = false;
    p_.saveRotation(rot_);
    logf("app: rotation %d saved", rot_);
  }

  // Speed: ease towards the latest value; the odometer runs on the latest value itself.
  shownKmh_ = approach(shownKmh_, kmh_, dt, 0.2f);
  if (std::fabs(shownKmh_ - kmh_) < 0.05f) shownKmh_ = kmh_;
  scaleMax_ = approach(scaleMax_, scaleFor(lim_), dt, 0.35f);
  const bool fresh = haveV_ && now - lastV_ <= kVehicleStaleMs;
  if (fresh) odo_ += kmh_ / 3.6 * dt;
  nav_.tick(now, kmh_, fresh);
  tickEggs(now, fresh);
  if (charging_ && (now - chargeAt_ > kChargeStaleMs || (!connected_ && now - chargeAt_ > 3000))) charging_ = false;
  // Driving off ends the greeting for good (it does not come back at the next stop).
  if (welcomeUntil_ != 0 && driveActive_ && kmh_ >= 10) welcomeUntil_ = introUntil_ = 0;

  // Link lost long enough: the drive is over as far as the display can tell.
  if (driveActive_ && linkLost(now) && now - lostSince() > kLinkLostMs) {
    driveActive_ = false;
    haveV_ = false;
    nav_.reset();
    logf("app: link lost for %u s - back to %s", static_cast<unsigned>(kLinkLostMs / 1000),
         connected_ ? "connected" : "waiting");
  }

  if (resetAsk_ && now - resetAskAt_ > kResetAskMs) resetAsk_ = false;
  if (brightnessShowing_ && now - brightnessShownAt_ > kBrightnessShowMs + 400) brightnessShowing_ = false;
  if (toastActive_ && now - toastAt_ > 2600) toastActive_ = false;
  tickNotices(now);

  const Screen s = resolveScreen(now);
  if (s != screen_) {
    // Driving off cuts the intro short: no flash to come out of then.
    introEndedAt_ = screen_ == Screen::Intro && s != Screen::Drive ? now : 0;
    screen_ = s;
    screenSince_ = now;
  }

  applyBrightness(now);
}

float App::runDown(float d) const {
  const float driven = static_cast<float>(odo_ - sceneOdo_);
  const float left = d - (driven > 0 ? driven : 0);
  return left > 0 ? left : 0;
}

void App::frame(Frame& f, uint64_t now) const {
  f = Frame{};
  f.screen = screen_;
  f.lang = lang_;
  f.t = t_;
  f.screenAge = static_cast<float>(now - screenSince_) / 1000.0f;
  snprintf(f.id, sizeof(f.id), "%s", id_);
  f.paired = paired();
  f.brightness = bright_;
  f.theme = style_.theme;
  f.eggs = style_.eggs;
  snprintf(f.name, sizeof(f.name), "%s", style_.name);
  // The screen right after an intro that ran to its flash comes out of the white.
  f.afterIntro = introEndedAt_ != 0 && screenSince_ == introEndedAt_;
  if (egg_ != Egg::None) {
    f.egg = egg_;
    f.eggAge = static_cast<float>(now - eggAt_) / 1000.0f;
  }
  f.soc = soc_;
  f.chargeKw = chargeKw_;
  f.rangeKm = rangeKm_;
  f.launchBestMs = bestRunMs_;
  if (runMs_ > 0 && now - runAt_ < kRunShowMs) {
    f.launchMs = runMs_;
    f.launchAge = static_cast<float>(now - runAt_) / 1000.0f;
    f.launchBest = runBest_;
  } else if (launchOn_ && launch_.phase() == LaunchTimer::Phase::Running && shownKmh_ >= kRunLiveKmh &&
             linkFresh(now)) {
    const int64_t clock = launchPhoneTime_ ? (time_.valid() ? time_.phoneNow(now) : 0) : static_cast<int64_t>(now);
    if (clock > 0) {
      const float s = static_cast<float>(clock - launch_.start()) / 1000.0f;
      f.launchLive = s < 0 ? 0 : s;
    }
  }

  if (time_.valid()) {
    f.hasClock = true;
    f.clock = localClock(time_.phoneNow(now), hasTz_, tzMin_);
  }

  // Drive HUD
  f.layout = layout_;
  f.focus = layout_ == Layout::Focus;
  f.tilt = tilt_;
  if (nav_.valid()) {
    f.hasPose = true;
    f.lat = nav_.lat();
    f.lon = nav_.lon();
    f.hdg = nav_.hdg();
  }
  f.chargers = chargers_;
  f.speed = shownKmh_;
  f.limit = lim_;
  f.scaleMax = scaleMax_;
  f.linkLost = linkLost(now);

  if (haveScene_) {
    f.scene = scene_;
    const int64_t phoneNow = time_.phoneNow(now);
    const Scene& sc = scene_;
    if (sc.light.present) {
      f.lightD = runDown(sc.light.d);
      if (sc.light.to > 0 && time_.valid()) {
        const float left = static_cast<float>(sc.light.to - phoneNow) / 1000.0f;
        f.lightLeft = left > 0 ? left : 0;
        if (sc.light.from > 0 && sc.light.to > sc.light.from) {
          const float span = static_cast<float>(sc.light.to - sc.light.from) / 1000.0f;
          float frac = f.lightLeft / span;
          f.lightPhase = frac < 0 ? 0 : frac > 1 ? 1 : frac;
        }
      }
    }
    if (sc.cam.present) f.camD = runDown(sc.cam.d);
    if (sc.turn.present) {
      f.turnD = runDown(sc.turn.d);
      if (sc.turn.eta > 0) {
        f.hasEta = true;
        f.eta = localClock(sc.turn.eta, hasTz_, tzMin_);
      }
    }

    Card cards[4];
    int n = 0;
    if (sc.cam.present && f.camD <= kCameraNearM) cards[n++] = Card::Camera;
    if (sc.sec.present) cards[n++] = Card::Section;
    if (sc.light.present && f.lightD <= kLightNearM) cards[n++] = Card::Light;
    if (sc.turn.present) cards[n++] = Card::Turn;
    f.primary = n > 0 ? cards[0] : Card::None;
    f.secondary = n > 1 ? cards[1] : Card::None;

    // Camera close and the car too fast for it: the ring pulses.
    const int vmax = sc.cam.vmax > 0 ? sc.cam.vmax : lim_;
    if (sc.cam.present && f.camD <= kCameraAlertM && vmax > 0 && std::lround(shownKmh_) > vmax) {
      f.alert = 1.0f;
    }
  }

  // Overlays
  f.hold = resetAsk_ ? 0 : hold_;
  f.resetAsk = resetAsk_;
  if (resetAsk_) {
    const float left = 1.0f - static_cast<float>(now - resetAskAt_) / static_cast<float>(kResetAskMs);
    f.resetLeft = left < 0 ? 0 : left;
  }
  if (brightnessShowing_) {
    const float since = static_cast<float>(now - brightnessShownAt_);
    f.brightnessShow = since < kBrightnessShowMs ? 1.0f : 1.0f - (since - kBrightnessShowMs) / 400.0f;
    if (f.brightnessShow < 0) f.brightnessShow = 0;
  }
  if (toastActive_) {
    const float since = static_cast<float>(now - toastAt_);
    f.toast = since < 2200 ? 1.0f : 1.0f - (since - 2200) / 400.0f;
    if (f.toast < 0) f.toast = 0;
    f.toastText = toastText_;
  }

  if (noticeOn_) {
    const ancs::Notice& n = noticeCur_;
    Frame::Notice& v = f.notice;
    v.style = noticeFull_ ? Frame::Notice::Style::Full : Frame::Notice::Style::Compact;
    v.age = static_cast<float>(now - noticeAt_) / 1000.0f;
    if (noticeUntil_ == 0) {
      const float out = static_cast<float>(now - noticeHideAt_) / static_cast<float>(kNoticeFadeMs);
      v.out = out > 1 ? 1 : out;
    }
    v.call = n.isCall();
    v.missed = n.category == ancs::Category::MissedCall;
    if (!v.call && noticeUntil_ > noticeAt_) {
      const float left = static_cast<float>(noticeUntil_ - now) / static_cast<float>(noticeUntil_ - noticeAt_);
      v.left = left < 0 ? 0 : left > 1 ? 1 : left;
    } else if (!v.call) {
      v.left = 0;
    }
    snprintf(v.app, sizeof(v.app), "%s", n.app);
    // The line above the title: what kind of thing it is, from which app.
    const bool phoneApp = strcmp(n.app, "com.apple.mobilephone") == 0;
    if (v.call || v.missed) {
      const char* kind = tr(lang_, v.call ? Str::IncomingCall : Str::MissedCall);
      if (n.appName[0] != 0 && !phoneApp) {
        snprintf(v.appName, sizeof(v.appName), "%s \xc2\xb7 %s", kind, n.appName);
      } else {
        snprintf(v.appName, sizeof(v.appName), "%s", kind);
      }
    } else if (n.appName[0] != 0) {
      snprintf(v.appName, sizeof(v.appName), "%s", n.appName);
    } else {
      snprintf(v.appName, sizeof(v.appName), "%s", tr(lang_, Str::Notification));
    }
    snprintf(v.title, sizeof(v.title), "%s", n.title[0] != 0 ? n.title : n.appName);
    if (!v.call) {
      const char* body = noticeText_ && n.message[0] != 0 ? n.message : n.subtitle;
      snprintf(v.body, sizeof(v.body), "%s", body);
    }
    snprintf(v.positive, sizeof(v.positive), "%s", n.positive[0] != 0 ? n.positive : tr(lang_, Str::Accept));
    snprintf(v.negative, sizeof(v.negative), "%s", n.negative[0] != 0 ? n.negative : tr(lang_, Str::Decline));
    v.waiting = noticeQueued_;
  }
}

}  // namespace monav
