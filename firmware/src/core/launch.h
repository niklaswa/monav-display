#pragma once

// The 0–100 km/h timer. Every start from standing is timed; a run counts when the car stood still
// for a second, then reached 100 km/h without slowing down on the way. The times come from the
// phone's clock in the `v` messages and are interpolated between two of them at both ends, so
// 10 messages a second give about a twentieth of a second. Portable (firmware + host checks).

#include <cstdint>

namespace monav {

class LaunchTimer {
 public:
  enum class Phase : uint8_t { Idle, Standing, Running };

  static constexpr float kStillKmh = 1.0f;      // below: standing; the run starts as it is crossed
  static constexpr float kTargetKmh = 100.0f;
  static constexpr int64_t kStillMs = 1000;     // standing at least this long before a run
  static constexpr float kDropKmh = 4.0f;       // slower than the run's peak by this: no run
  static constexpr int64_t kMaxMs = 30000;      // longer: not a launch

  /** One speed at time `t` (ms, one clock throughout). Returns the run's time in ms once 100 is reached, else 0. */
  uint32_t sample(int64_t t, float kmh);
  void reset();

  Phase phase() const { return phase_; }
  /** When the running run began (same clock as sample()). */
  int64_t start() const { return start_; }

 private:
  Phase phase_ = Phase::Idle;
  int64_t stillSince_ = 0;
  int64_t start_ = 0;
  int64_t lastT_ = 0;
  float lastKmh_ = 0;
  float peak_ = 0;
  bool haveLast_ = false;
};

}  // namespace monav
