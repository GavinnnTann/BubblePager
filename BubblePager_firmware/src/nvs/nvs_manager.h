#pragma once
#include <Arduino.h>
#include <stdint.h>
#include <time.h>

// Minimal NVS-backed persistence for TelePager. Uses the Preferences (NVS)
// library — RTC_DATA_ATTR is unreliable here because native USB CDC re-enumerates
// on every reset and clears it (gotcha #8).

bool nvs_init();

// Video-id history lives in src/history (its own NVS namespace). This module now
// only persists the clock fallback.

// Persist last-known unix time so the clock survives a power cycle when the RTC
// oscillator has stopped. 0 = never stored.
void   nvs_save_time(time_t t);
time_t nvs_load_time();

// ── User settings (settings page) ───────────────────────────────────────────
// Mirrors the settings-page toggles. Persisted so choices survive a reset.
struct AppSettings {
    bool haptics;          // master haptic enable
    bool tap_sleep;        // double-tap screen → sleep/dim
    bool adaptive_timeout; // IMU motion resets the idle timer
    bool orient_flip;      // IMU auto 180° rotation
    uint16_t incoming_timeout_sec;   // how long the incoming pulse waits for a
                                     // tap before the display sleeps (pager-style
                                     // "unread" instead of showing the content)
};

// Load settings (returns sensible defaults — all-on except tap_sleep — if unset).
AppSettings nvs_load_settings();
// Persist the current settings.
void        nvs_save_settings(const AppSettings& s);
