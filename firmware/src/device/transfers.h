#pragma once

// Binary transfers (PROTOCOL.md "Binary transfers"): firmware updates, map images, the route and
// the picture for the photo theme (kept in the flash's data partition, decoded at boot).
//
// The control messages (ota/map/route) and the bulk writes arrive on the NimBLE host task, which
// only bookkeeps and copies: map and route bytes go straight into PSRAM, firmware bytes into a
// ring buffer. A worker task writes the firmware to flash (acknowledging only what is written),
// checks and activates it, and decodes map images into the spare map buffer. The UI task swaps
// a decoded map (and a new route) in at the start of a frame.

#include <cstddef>
#include <cstdint>

#include "core/frame.h"
#include "core/mapdata.h"
#include "core/model.h"

namespace monav::device::transfers {

/** Allocates the two map buffers (PSRAM) and the firmware ring, starts the worker. */
bool begin();

/** The connection was welcomed / is gone: transfers are only accepted in between. */
void setAuthorized(bool authorized);

// --- NimBLE host task ---

/** An ota / map / route control message. */
void control(const Message& m);
/** One write on `bulk`. */
void onBulk(const uint8_t* data, size_t len);
/** The phone went away: a running firmware update is abandoned. */
void onDisconnect();

// --- UI task ---

/** Start of a frame: takes a freshly decoded map / new route in (and tells the phone). */
void frameBegin(uint64_t nowMs);
const MapImage* map();
const RouteLine* route();
/** The phone's picture for the photo theme (nullptr when none is stored). */
const Wallpaper* wallpaper();
/** A picture is stored in flash (any task). */
bool hasWallpaper();
/** An app's icon for the notifications, nullptr until the phone sent it (UI task). */
const AppIcon* icon(const char* app);
/** An icon for this app is cached (any task). */
bool hasIcon(const char* app);
/** The firmware update for the screen (phase None when there is none). */
Frame::Update update();

/** Demo / tests: decode a map image held in memory as if it had been sent. */
bool loadLocalMap(const uint8_t* jpg, size_t len, int w, int h, double lat, double lon, float mpp);

}  // namespace monav::device::transfers
