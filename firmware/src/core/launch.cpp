#include "core/launch.h"

namespace monav {
namespace {

/** When the speed crossed `v` between two samples (linear). */
int64_t crossing(int64_t t0, float v0, int64_t t1, float v1, float v) {
  if (v1 <= v0) return t1;
  const double f = (static_cast<double>(v) - v0) / (static_cast<double>(v1) - v0);
  return t0 + static_cast<int64_t>((t1 - t0) * (f < 0 ? 0 : f > 1 ? 1 : f) + 0.5);
}

}  // namespace

void LaunchTimer::reset() {
  phase_ = Phase::Idle;
  haveLast_ = false;
}

uint32_t LaunchTimer::sample(int64_t t, float kmh) {
  uint32_t result = 0;
  // Time going backwards (a new connection, a clock change): start over.
  if (haveLast_ && t < lastT_) reset();
  switch (phase_) {
    case Phase::Idle:
      if (kmh < kStillKmh) {
        phase_ = Phase::Standing;
        stillSince_ = t;
      }
      break;
    case Phase::Standing:
      if (kmh < kStillKmh) break;
      if (haveLast_ && lastT_ - stillSince_ >= kStillMs) {
        phase_ = Phase::Running;
        start_ = crossing(lastT_, lastKmh_, t, kmh, kStillKmh);
        peak_ = kmh;
      } else {
        phase_ = Phase::Idle;  // only rolled for a moment
      }
      break;
    case Phase::Running:
      if (kmh >= kTargetKmh) {
        const int64_t at = crossing(lastT_, lastKmh_, t, kmh, kTargetKmh);
        result = static_cast<uint32_t>(at - start_ > 0 ? at - start_ : 1);
        phase_ = Phase::Idle;
      } else if (kmh < peak_ - kDropKmh || t - start_ > kMaxMs) {
        phase_ = kmh < kStillKmh ? Phase::Standing : Phase::Idle;
        stillSince_ = t;
      } else if (kmh > peak_) {
        peak_ = kmh;
      }
      break;
  }
  lastT_ = t;
  lastKmh_ = kmh;
  haveLast_ = true;
  return result;
}

}  // namespace monav
