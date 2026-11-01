#include "core/nav.h"

#include <cmath>

namespace monav {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kMPerDegLat = 110540.0;
constexpr double kMPerDegLon = 111320.0;

float wrap180(float a) {
  while (a > 180.0f) a -= 360.0f;
  while (a < -180.0f) a += 360.0f;
  return a;
}

}  // namespace

void geoToLocal(double lat0, double lon0, double lat, double lon, double& east, double& north) {
  east = (lon - lon0) * kMPerDegLon * std::cos(lat0 * kPi / 180.0);
  north = (lat - lat0) * kMPerDegLat;
}

void localToGeo(double lat0, double lon0, double east, double north, double& lat, double& lon) {
  lat = lat0 + north / kMPerDegLat;
  lon = lon0 + east / (kMPerDegLon * std::cos(lat0 * kPi / 180.0));
}

void Nav::fix(double lat, double lon, bool hasHdg, float hdg, uint64_t now) {
  if (!hasHdg) hdg = valid_ ? fixHdg_ : 0.0f;
  if (valid_ && lat == fixLat_ && lon == fixLon_) {
    // The same fix again (v comes 10× a second, the fix once): keep running on, but take a
    // turned heading.
    if (hasHdg && std::fabs(wrap180(hdg - fixHdg_)) > 0.05f) {
      // Running on in the new direction from the fix would move the car sideways: keep it where
      // it is drawn and let that offset ease away like a new fix's.
      const double drawnLat = lat_, drawnLon = lon_;
      corrH_ = wrap180(hdg_ - hdg);
      fixHdg_ = hdg;
      corrE_ = corrN_ = 0;
      compute();
      double e, n;
      geoToLocal(lat_, lon_, drawnLat, drawnLon, e, n);
      corrE_ = e;
      corrN_ = n;
      compute();
    }
    return;
  }
  if (!valid_) {
    valid_ = true;
    fixLat_ = lat;
    fixLon_ = lon;
    fixHdg_ = hdg;
    fixAt_ = lastTick_ = now;
    run_ = corrE_ = corrN_ = 0;
    corrH_ = 0;
    compute();
    return;
  }
  // Where the car is drawn now, as an offset from the new fix: that offset eases away.
  double e, n;
  geoToLocal(lat, lon, lat_, lon_, e, n);
  if (std::sqrt(e * e + n * n) > kSnapM) e = n = 0;
  corrE_ = e;
  corrN_ = n;
  corrH_ = wrap180(hdg_ - hdg);
  fixLat_ = lat;
  fixLon_ = lon;
  fixHdg_ = hdg;
  fixAt_ = now;
  run_ = 0;
  compute();
}

void Nav::tick(uint64_t now, float kmh, bool fresh) {
  if (!valid_) return;
  float dt = static_cast<float>(now - lastTick_) / 1000.0f;
  lastTick_ = now;
  if (dt <= 0) return;
  if (dt > 0.25f) dt = 0.25f;
  const float sinceFix = static_cast<float>(now - fixAt_) / 1000.0f;
  if (fresh && sinceFix < kMaxCoastS) run_ += kmh / 3.6 * dt;
  const double kp = std::exp(-dt / kEasePosS);
  corrE_ *= kp;
  corrN_ *= kp;
  corrH_ *= std::exp(-dt / kEaseHdgS);
  compute();
}

void Nav::compute() {
  const double h = fixHdg_ * kPi / 180.0;
  const double east = run_ * std::sin(h) + corrE_;
  const double north = run_ * std::cos(h) + corrN_;
  localToGeo(fixLat_, fixLon_, east, north, lat_, lon_);
  float hd = fixHdg_ + corrH_;
  while (hd < 0) hd += 360.0f;
  while (hd >= 360.0f) hd -= 360.0f;
  hdg_ = hd;
}

}  // namespace monav
