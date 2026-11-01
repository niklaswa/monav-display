#pragma once

// Development only (build flag MONAV_DEMO, env amoled175-demo): a scripted drive along a route on
// an embedded MapKit sample, without a phone. It goes through the real app, and the map and route
// go through the real transfer path (control message + bulk writes), so the frame times measured
// in each layout are the real ones. Without MONAV_DEMO these are empty.

#include <cstdint>

#include "core/app.h"

namespace monav::device::demo {

void begin(App& app, uint64_t now);
void step(App& app, uint64_t now);

}  // namespace monav::device::demo
