#pragma once
#include <stdbool.h>

// Minimal CST816S capacitive-touch reader (I²C 0x15, shared bus with RTC +
// TCA6408). TelePager only needs a single gesture: a screen tap to toggle
// pause during playback — so this exposes just tap-edge detection, no coords.

void touch_init();

// Returns true exactly once per finger-down edge (debounced). Poll it from the
// same task that owns the I²C bus (the render loop). Used for tap-to-pause during
// PLAYING (LVGL is not pumped then, so the indev below is idle).
bool touch_tapped();

// Read the current contact point. Returns true while a finger is down, writing
// panel coordinates (0–239) to *x/*y. Unthrottled.
bool touch_read_point(int* x, int* y);

// Tri-state contact read: 1 = finger down (x/y written), 0 = no finger,
// -1 = I2C error. Lets a gesture handler hold its state across a transient error
// instead of mistaking it for a finger lift.
int touch_read_contact(int* x, int* y);

// Mirror touch coordinates when the display is flipped 180° (keeps taps aligned
// with what the user sees). Call from the orientation-flip handler.
void touch_set_flipped(bool flipped);
