#pragma once

// The car's place for the map: fixes from `v` (lat/lon/hdg, about once a second, repeated in
// between), run on along the heading with the speed, and eased towards each new fix so the map
// glides instead of stepping.

#include <cstdint>

namespace monav {

/** The protocol's flat projection around (lat0, lon0): metres east and north. */
void geoToLocal(double lat0, double lon0, double lat, double lon, double& east, double& north);
void localToGeo(double lat0, double lon0, double east, double north, double& lat, double& lon);

class Nav {
 public:
  static constexpr float kEasePosS = 0.6f;    // time constant of the position correction
  static constexpr float kEaseHdgS = 0.45f;   // ... and of the heading
  static constexpr float kMaxCoastS = 2.5f;   // run on at most this long past the last fix
  static constexpr float kSnapM = 150.0f;     // a jump bigger than this is taken at once

  /** A `v` with a place. Repeats of the last fix only refresh it. */
  void fix(double lat, double lon, bool hasHdg, float hdg, uint64_t now);
  /** Per frame: run on with the speed (when the link is fresh), ease the corrections. */
  void tick(uint64_t now, float kmh, bool fresh);
  void reset() { valid_ = false; }

  bool valid() const { return valid_; }
  /** The place and heading to draw. */
  double lat() const { return lat_; }
  double lon() const { return lon_; }
  float hdg() const { return hdg_; }

 private:
  void compute();

  bool valid_ = false;
  double fixLat_ = 0, fixLon_ = 0;
  float fixHdg_ = 0;
  uint64_t fixAt_ = 0;
  uint64_t lastTick_ = 0;
  double run_ = 0;  // metres along fixHdg_ since the fix
  double corrE_ = 0, corrN_ = 0;
  float corrH_ = 0;
  double lat_ = 0, lon_ = 0;
  float hdg_ = 0;
};

}  // namespace monav
