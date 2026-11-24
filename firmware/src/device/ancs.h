#pragma once

// The iPhone's notifications over Bluetooth (ANCS, core/ancs.h for the wire format). The display
// stays the peripheral Monav connects to; on that same connection it bonds with the iPhone (the
// iPhone asks once whether to pair), finds the Apple Notification Center Service in the iPhone's
// GATT database and subscribes to it. Everything Bluetooth runs on the NimBLE host task; the UI
// task switches it on and off and picks up the notifications from a queue.

#include <cstdint>

#include "core/ancs.h"

namespace monav::device::ancs {

struct Event {
  enum Kind : uint8_t { Added, Modified, Removed, State } kind;
  monav::ancs::Notice notice;  // Added, Modified
  uint32_t uid;                // Removed
  char state[12];              // State: off, pairing, on, stale, denied, blocked
};

/** After ble::begin(): listens to the connection's events. */
void begin();

/** UI task: bond and subscribe on the current connection (and the next), or stop. */
void setEnabled(bool on);

/** UI task: the next notification or state change, if any. */
bool poll(Event& out);

/** UI task: accept (positive) or decline a call. */
void action(uint32_t uid, bool positive);

/** Forgets every bonded iPhone (the pairing reset). */
void forgetBonds();

}  // namespace monav::device::ancs
