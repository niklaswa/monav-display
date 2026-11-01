#pragma once

// The map on the glass: the image turned to the driving direction, the car low in the circle,
// optionally laid down in perspective (one scale per scanline, "Mode 7"). Sampling steps through
// the image in 16.16 fixed point with a constant step per pixel, only inside the circle.

#include <cstdint>

#include "core/mapdata.h"
#include "ui/raster.h"

namespace monav::ui {

struct MapCamera {
  float carX = 0;  // the car in map pixels
  float carY = 0;
  float hdg = 0;   // degrees from north, clockwise: screen up
  bool tilt = true;
  float cx = 233;  // where the car sits on the glass
  float cy = 318;

  // Perspective: horizon above the glass, forward stretch at the car's line.
  static constexpr float kHorizon = -380.0f;
  static constexpr float kForward = 1.3f;
};

/** Where on the glass the car would be in map pixels; false when it is off the image. */
bool carOnImage(const MapImage& m, double lat, double lon, float& mx, float& my);

/**
 * Samples the map into the circle. Rows under the overlays are darkened (`topDark` towards the
 * top edge, `bottomDark` towards the bottom), everything times `s.dim`.
 */
void drawMap(gfx::Surface& s, const MapImage& m, const MapCamera& cam, float topDark, float bottomDark);

/** Map pixel → glass. `scale` shrinks with distance (for line widths). False behind the camera. */
bool project(const MapCamera& cam, float mx, float my, float& sx, float& sy, float& scale);

/** Builds the sampler's index tables for this map's size up front (before two tasks draw). */
void prepareMap(const MapImage& m);

/** Projects the route for this frame up front (before two tasks draw bands at once). */
void prepareRoute(const MapImage& m, const RouteLine& r, const MapCamera& cam);

/** The route over the map: dark casing, blue line, starting at the car. */
void drawRoute(gfx::Surface& s, const MapImage& m, const RouteLine& r, const MapCamera& cam);

/** The car: a white-rimmed blue disc with the arrow pointing up (the driving direction). */
void drawCar(gfx::Surface& s, float cx, float cy);

}  // namespace monav::ui
