#pragma once

// Turns raw finger samples into the display's gestures: tap, 3 s hold, horizontal swipe
// (brightness steps). Portable, so the host build can exercise it too.

#include <cstdint>

namespace monav {

enum class GestureType : uint8_t {
  Tap,
  LongPress,       // fired once, while the finger is still down
  BrightnessStep,  // value = +1 / −1 per step of horizontal travel
  SwipeEnd,        // the finger lifted after at least one brightness step
};

struct Gesture {
  GestureType type;
  int value = 0;
};

class Gestures {
 public:
  static constexpr int kSlop = 22;              // px a tap may wander
  static constexpr uint32_t kTapMaxMs = 450;
  static constexpr uint32_t kHoldMs = 3000;
  static constexpr uint32_t kHoldVisibleMs = 450;  // the hold ring appears after this
  static constexpr int kStepPx = 34;            // horizontal travel per brightness step

  /** Finger on the glass at (x, y), in panel coordinates. Call for every report. */
  void down(int x, int y, uint32_t now);
  /** Finger lifted. */
  void up(uint32_t now);
  /** Call every frame: fires the long press while the finger rests. */
  void update(uint32_t now);
  /** Next gesture, if any. */
  bool poll(Gesture& out);

  /** 0 when not holding still; rises to 1 at kHoldMs (for the hold ring). */
  float holdProgress(uint32_t now) const;
  bool isDown() const { return down_; }

 private:
  void push(GestureType type, int value = 0);

  bool down_ = false;
  bool moved_ = false;
  bool swiping_ = false;
  bool held_ = false;
  int startX_ = 0, startY_ = 0;
  int stepX_ = 0;
  uint32_t startMs_ = 0;

  Gesture queue_[8];
  uint8_t head_ = 0, count_ = 0;
};

}  // namespace monav
