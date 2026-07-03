#pragma once
#include "nvs/nvs_manager.h"   // AppSettings

// Center-locked settings wheel (ported from FobBob): a fixed blue focus frame sits
// at the viewport centre and the option rows scroll under it. NO LVGL input device
// is used — main feeds CST816 drag deltas via ui_settings_scroll_by()/_snap()/_tap()
// so scrolling is 1:1 with the finger and never fights LVGL momentum.
//
// Rows: Battery(status) · Sync(status+action) · Haptics · Motion wake · Auto 180 ·
//       Tap sleep · Exit

// What activating the focused row asks the caller to do.
enum UiSettingsAction { UI_SET_NONE = 0, UI_SET_SYNC, UI_SET_EXIT };

// Build the screen once (after ui_init).
void ui_settings_init();

// Show the wheel seeded from `cur`. Becomes the active LVGL screen.
void ui_settings_open(const AppSettings& cur);

bool ui_settings_active();
void ui_settings_close();   // caller has switched away — mark inactive

// ── Manual wheel driving (main feeds CST816 gestures) ───────────────────────
// Scroll the list by dy pixels (finger delta). Returns the new centred row index.
int  ui_settings_scroll_by(int dy);
// The row currently nearest the centre focus slot.
int  ui_settings_centered_row();
// Animate the nearest row into the centre slot (call on finger release after a drag).
void ui_settings_snap();
// Tap at panel-y: an off-centre row scrolls into focus (UI_SET_NONE); the centred
// row toggles/activates and may return an action (UI_SET_SYNC / UI_SET_EXIT).
UiSettingsAction ui_settings_tap(int y);

// ── Live status rows ────────────────────────────────────────────────────────
void ui_settings_set_battery(int pct, bool charging);
// state: 0 neutral, 1 ok (green), 2 fail (red), 3 busy (blue).
void ui_settings_set_sync_status(const char* text, int state);

// The current (edited) toggle values.
AppSettings ui_settings_values();
