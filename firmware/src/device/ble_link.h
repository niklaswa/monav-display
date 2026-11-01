#pragma once

// The BLE side of PROTOCOL.md: a NimBLE peripheral with the Monav service (inbox, outbox,
// info), one connection at a time, no bonding. Writes are reassembled and parsed on the NimBLE
// host task and handed to the UI task through a queue, in order, with the connection events.

#include <cstddef>
#include <cstdint>

#include "core/model.h"

namespace monav::device::ble {

struct Event {
  enum Kind : uint8_t { Connected, Disconnected, Received } kind;
  Message msg;  // Received only
};

/** Starts the stack and advertising as "Monav Display <id>". */
void begin(const char* id);

/** Next event for the UI task, if any. */
bool poll(Event& out);

/** One display → phone message, chunked to the MTU, notified on `outbox`. */
void send(const char* json, size_t len);

/** Drops the current connection (advertising restarts on its own). */
void disconnect();

/** New value of the `info` characteristic. */
void setInfo(const char* json);

/** Diagnostics. */
uint16_t mtu();
/** Connection interval of the current connection in µs (0 without one). */
int connIntervalUs();
/** Advertising was started (part of the health check that confirms a new firmware). */
bool advertising();
uint32_t droppedMessages();

}  // namespace monav::device::ble
