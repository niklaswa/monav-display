#pragma once

// The phone's clock on the display (PROTOCOL.md, "Time").
//
// offset = phone time − own clock. A message's phone time was taken before it arrived, so every
// sample is the true offset minus that message's latency: the fastest message gives the largest
// sample. The offset therefore follows the largest samples at once and only drifts down slowly
// (clock drift between the two crystals), so one late message cannot shift the countdowns.

#include <cstdint>

namespace monav {

class TimeSync {
 public:
  /** Phone time `phoneMs` arrived at own clock `localMs`. */
  void sample(int64_t phoneMs, uint64_t localMs);
  void reset() { valid_ = false; }

  bool valid() const { return valid_; }
  /** Phone time now, estimated from the own clock. 0 while no sample arrived. */
  int64_t phoneNow(uint64_t localMs) const { return valid_ ? static_cast<int64_t>(localMs) + offset_ : 0; }
  /** Own clock at phone time `phoneMs`. */
  int64_t toLocal(int64_t phoneMs) const { return phoneMs - offset_; }
  int64_t offset() const { return offset_; }

 private:
  bool valid_ = false;
  int64_t offset_ = 0;
  uint64_t lastLocal_ = 0;
};

/** Hours and minutes of a phone time in the phone's time zone. */
struct ClockTime {
  int hour = 0;
  int minute = 0;
};

/**
 * Local wall-clock time. `tzMin` is the phone's UTC offset in minutes when it sent one
 * (`hello.tz`, an extension); otherwise Central European Time with EU summer time is assumed.
 */
ClockTime localClock(int64_t phoneMs, bool hasTz, int tzMin);

}  // namespace monav
