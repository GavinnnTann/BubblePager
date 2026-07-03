#pragma once
#include <Arduino.h>
#include <lvgl.h>

// Initialise I80 bus, GC9A01 panel, LVGL, and backlight PWM. Call once in setup().
bool display_init();

// Drive LVGL tick + timer handler. Call every loop() iteration.
void display_tick();

// Backlight control (ledc channel 0).
void display_on();                      // restore BACKLIGHT_DUTY
void display_off();                     // duty 0, LVGL keeps running
void display_set_brightness(uint8_t duty);  // 0–255

// Send GC9A01 SLPIN (0x10) then blank backlight. Call before deep sleep so the
// panel's internal regulators shut down and eliminate the faint glow on dark screens.
// Safe to call even if display_init() was never called (guards nullptr internally).
void display_sleep();

// Returns the LVGL display handle — use this to create screens.
lv_display_t* display_get();

// ── Direct panel blit (MJPEG playback — bypasses LVGL) ─────────────────────
// Push a block of RGB565 pixels straight to the GC9A01 GRAM at (x,y,w,h).
// `pixels` is w*h RGB565 samples in native little-endian (this call swaps bytes
// in place before DMA, so the buffer is modified). Blocks until the DMA of the
// previous blit has completed, then kicks this one asynchronously. Do NOT
// interleave with LVGL rendering — call only while no LVGL screen is flushing
// (i.e. during the PLAYING state). Returns false if the display isn't ready.
bool display_blit(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t* pixels);

// Block until any in-flight display_blit() DMA has finished. Call before reusing
// or freeing a buffer previously passed to display_blit().
void display_blit_wait();

// Swap RGB565 byte order in place — the same operation display_blit() performs
// before DMA. Exposed so a caller that blits the SAME buffer more than once (e.g.
// a freeze-frame blitted, then annotated and blitted again for an overlay) can
// undo the swap between blits and keep the buffer in a consistent native-order
// state. Without this, blitting the same buffer twice double-swaps it back to
// native order while the panel still expects swapped bytes — the picture inverts.
void display_swap_rgb565(uint16_t* pixels, uint32_t count);

// Rotate the LVGL display. degrees must be 0 or 180.
// Redraws the current screen; safe to call from the main task at any time.
void display_set_rotation(int degrees);
