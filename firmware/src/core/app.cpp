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
  const int layout = p_.loadLayout();
  layout_ = layout == static_cast<int>(Layout::Focus) ? Layout::Focus
            : layout == static_cast<int>(Layout::Map) ? Layout::Map
                                                       : Layout::Hud;
  bootUntil_ = now + kBootMs;
  lastTick_ = now;
  screen_ = Screen::Boot;
  screenSince_ = now;
  p_.pairingChanged(paired());
  logf("app: firmware %s, id %s, %s, brightness %d, layout %s, rotation %d", kFirmware, id_,
       paired() ? "paired" : "not paired", bright_, proto::layoutName(layout_), rot_);
  applyBrightness(now);
}

// --- BLE -------------------------------------------------------------------------------------

void App::connected(uint64_t now) {
  (void)now;
  connected_ = true;
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
  logf("app: phone disconnected");
}

void App::setAuthorized(bool on) {
  if (on == authorized_) return;
  authorized_ = on;
  p_.authorizationChanged(on);
}

void App::handleHello(const Message& m, uint64_t now) {
  if (m.hasLang && m.lang != lang_) {
    lang_ = m.lang;
    p_.saveLang(static_cast<int>(lang_));
  }
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
    n = proto::buildWelcome(out, sizeof(out), kFirmware, id_, false, bright_, layout_);
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
    n = proto::buildWelcome(out, sizeof(out), kFirmware, id_, true, bright_, layout_);
    p_.send(out, n);
    setAuthorized(true);
    logf("app: paired with a new phone");
    return;
  } else if (strcmp(token_, m.token) == 0) {
    denied_ = false;
    standby_ = false;  // a new connection after bye: connected, waiting for the drive
    n = proto::buildWelcome(out, sizeof(out), kFirmware, id_, false, bright_, layout_);
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
      }
      haveV_ = true;
      lastV_ = now;
      driveActive_ = true;
      standby_ = false;
      if (m.hasPos) nav_.fix(m.lat, m.lon, m.hasHdg, m.hdg, now);
      break;
    }
    case MsgType::Chargers:
      chargers_ = m.chargers;
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
      if (m.rot >= 0 && m.rot != rot_) {
        rot_ = m.rot;
        rotChangedAt_ = now;
        rotUnsaved_ = true;
      }
      break;
    case MsgType::Dev:
      if (devSession_ && m.hasLayout) setLayout(m.layout);
      break;
    case MsgType::Bye:
      rxstats::bye++;
      standby_ = true;
      standbyAt_ = now;
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
  switch (g.type) {
    case GestureType::Tap:
      if (resetAsk_) {
        resetPairing(now);
      } else if (screen_ == Screen::Drive) {
        // hud → focus → map → hud
        setLayout(layout_ == Layout::Hud ? Layout::Focus : layout_ == Layout::Focus ? Layout::Map : Layout::Hud);
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
  toastActive_ = true;
  toastAt_ = now;
  // The phone on the line was told welcome or denied; dropping it makes it connect again and
  // send a new hello, which now pairs.
  if (connected_) p_.disconnect();
  logf("app: pairing reset");
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
  if (now < bootUntil_) return Screen::Boot;
  if (denied_) return Screen::Denied;
  if (now < pairedUntil_ && authorized_) return Screen::Paired;
  if (driveActive_) return Screen::Drive;
  if (standby_) return Screen::Standby;
  if (authorized_) return Screen::Connected;
  return Screen::Waiting;
}

void App::applyBrightness(uint64_t now) {
  float dim = 1.0f;
  if (standby_ && now - standbyAt_ > kStandbyDimMs) dim = 0.25f;
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

  const Screen s = resolveScreen(now);
  if (s != screen_) {
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
    f.toastText = Str::ResetDone;
  }
}

}  // namespace monav
