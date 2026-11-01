#pragma once

// Icons drawn from anti-aliased primitives (ui/raster.h). `size` is the half extent of the
// icon's box in pixels: an icon drawn at (cx, cy, 20) fits a 40 × 40 square.

#include <cstdint>

#include "core/model.h"
#include "ui/raster.h"

namespace monav::ui::icons {

/** Speed camera; `bg` is the colour behind it (the lens is cut out of the body). */
void camera(gfx::Surface& s, float cx, float cy, float size, CamKind kind, uint32_t fg, uint32_t bg);

/** Manoeuvre arrow. For roundabouts the exit decides where the arm leaves the circle. */
void turn(gfx::Surface& s, float cx, float cy, float size, TurnKind kind, int exit, uint32_t fg, uint32_t muted,
          uint32_t bg);

/** Lane arrow of a traffic light (light.turn). */
void lane(gfx::Surface& s, float cx, float cy, float size, Lane lane, uint32_t fg);

/** Check mark, drawn up to `progress` (0..1) of its length. */
void check(gfx::Surface& s, float cx, float cy, float size, float width, uint32_t fg, float progress = 1.0f);

/** Bluetooth rune. */
void bluetooth(gfx::Surface& s, float cx, float cy, float size, uint32_t fg);

/** Lightning bolt (charging station). */
void bolt(gfx::Surface& s, float cx, float cy, float size, uint32_t fg);

/** Sun (brightness). */
void sun(gfx::Surface& s, float cx, float cy, float size, uint32_t fg);

/** Phone outline (another iPhone). */
void phone(gfx::Surface& s, float cx, float cy, float size, uint32_t fg);

/** Circular arrow (reset). */
void reset(gfx::Surface& s, float cx, float cy, float size, uint32_t fg);

/** Two chevrons pointing down (coast / slow down) or up (speed up). */
void chevrons(gfx::Surface& s, float cx, float cy, float size, bool down, uint32_t fg);

/** Traffic light housing with its three lamps; `lit` 0 red, 1 amber, 2 green, −1 none. */
void trafficLight(gfx::Surface& s, float cx, float cy, float height, int lit, float glow);

}  // namespace monav::ui::icons
