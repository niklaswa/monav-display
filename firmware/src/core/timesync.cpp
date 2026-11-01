#include "core/timesync.h"

namespace monav {
namespace {

/** How fast the offset may sink without a sample backing it: 1 ms per second (1000 ppm). */
constexpr double kDriftMsPerMs = 0.001;
/** A sample this far below the offset is a clock change on the phone, not latency. */
constexpr int64_t kJumpMs = 2000;

// Days since 1970-01-01 → civil date (Howard Hinnant's algorithm).
void civilFromDays(int64_t z, int& y, int& m, int& d) {
  z += 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const int64_t doe = z - era * 146097;
  const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const int64_t mp = (5 * doy + 2) / 153;
  d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
  m = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
  y = static_cast<int>(yoe + era * 400 + (m <= 2 ? 1 : 0));
}

int64_t daysFromCivil(int y, int m, int d) {
  y -= m <= 2 ? 1 : 0;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const int64_t yoe = y - era * 400;
  const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

/** Day number of the last Sunday of a month. */
int64_t lastSunday(int y, int m) {
  const int nextY = m == 12 ? y + 1 : y;
  const int nextM = m == 12 ? 1 : m + 1;
  const int64_t last = daysFromCivil(nextY, nextM, 1) - 1;
  const int64_t weekday = ((last % 7) + 7 + 4) % 7;  // 1970-01-01 was a Thursday (4)
  return last - weekday;
}

/** Central European Time: +60 min, +120 min from the last Sunday of March to the last of October (01:00 UTC). */
int cetOffsetMin(int64_t utcSec) {
  const int64_t day = utcSec >= 0 ? utcSec / 86400 : (utcSec - 86399) / 86400;
  int y, m, d;
  civilFromDays(day, y, m, d);
  const int64_t start = lastSunday(y, 3) * 86400 + 3600;
  const int64_t end = lastSunday(y, 10) * 86400 + 3600;
  return (utcSec >= start && utcSec < end) ? 120 : 60;
}

}  // namespace

void TimeSync::sample(int64_t phoneMs, uint64_t localMs) {
  const int64_t s = phoneMs - static_cast<int64_t>(localMs);
  if (!valid_) {
    offset_ = s;
    valid_ = true;
    lastLocal_ = localMs;
    return;
  }
  const double elapsed = static_cast<double>(localMs - lastLocal_);
  lastLocal_ = localMs;
  const int64_t decayed = offset_ - static_cast<int64_t>(elapsed * kDriftMsPerMs);
  if (s > decayed || s < offset_ - kJumpMs) {
    offset_ = s;
  } else {
    offset_ = decayed;
  }
}

ClockTime localClock(int64_t phoneMs, bool hasTz, int tzMin) {
  const int64_t utcSec = phoneMs >= 0 ? phoneMs / 1000 : (phoneMs - 999) / 1000;
  const int offset = hasTz ? tzMin : cetOffsetMin(utcSec);
  int64_t local = utcSec + static_cast<int64_t>(offset) * 60;
  int64_t secOfDay = local % 86400;
  if (secOfDay < 0) secOfDay += 86400;
  ClockTime t;
  t.hour = static_cast<int>(secOfDay / 3600);
  t.minute = static_cast<int>((secOfDay / 60) % 60);
  return t;
}

}  // namespace monav
