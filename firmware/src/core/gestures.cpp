#include "core/gestures.h"

#include <cstdlib>

namespace monav {

void Gestures::push(GestureType type, int value) {
  if (count_ == sizeof(queue_) / sizeof(queue_[0])) return;
  queue_[(head_ + count_) % (sizeof(queue_) / sizeof(queue_[0]))] = Gesture{type, value};
  count_++;
}

bool Gestures::poll(Gesture& out) {
  if (count_ == 0) return false;
  out = queue_[head_];
  head_ = (head_ + 1) % (sizeof(queue_) / sizeof(queue_[0]));
  count_--;
  return true;
}

void Gestures::down(int x, int y, uint32_t now) {
  if (!down_) {
    down_ = true;
    moved_ = swiping_ = held_ = false;
    startX_ = stepX_ = x;
    startY_ = y;
    startMs_ = now;
    return;
  }
  const int dx = x - startX_;
  const int dy = y - startY_;
  if (!moved_ && (std::abs(dx) > kSlop || std::abs(dy) > kSlop)) {
    moved_ = true;
    // Horizontal wins only when the finger clearly committed to it.
    swiping_ = !held_ && std::abs(dx) > std::abs(dy);
  }
  if (swiping_) {
    const int steps = (x - stepX_) / kStepPx;
    if (steps != 0) {
      push(GestureType::BrightnessStep, steps);
      stepX_ += steps * kStepPx;
    }
  }
}

void Gestures::up(uint32_t now) {
  if (!down_) return;
  down_ = false;
  if (swiping_) {
    push(GestureType::SwipeEnd);
  } else if (!moved_ && !held_ && now - startMs_ <= kTapMaxMs) {
    push(GestureType::Tap);
  }
}

void Gestures::update(uint32_t now) {
  if (down_ && !moved_ && !held_ && now - startMs_ >= kHoldMs) {
    held_ = true;
    push(GestureType::LongPress);
  }
}

float Gestures::holdProgress(uint32_t now) const {
  if (!down_ || moved_ || held_) return 0.0f;
  const uint32_t t = now - startMs_;
  if (t < kHoldVisibleMs) return 0.0f;
  const float p = static_cast<float>(t - kHoldVisibleMs) / static_cast<float>(kHoldMs - kHoldVisibleMs);
  return p > 1.0f ? 1.0f : p;
}

}  // namespace monav
