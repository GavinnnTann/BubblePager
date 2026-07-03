#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <time.h>

// NTP time sync + timezone for the idle clock. TelePager keeps WiFi always on
// (unlike FobBob's connect-sync-drop model), so this is a thin wrapper over the
// ESP-IDF SNTP client: the RTC/system clock are kept in UTC and the display uses
// the Singapore timezone (SGT-8, UTC+8, no DST) via the POSIX TZ env var.
//
// On every successful SNTP packet the fresh UTC time is written back to the
// PCF85063 RTC and NVS so the clock survives a power cycle.

enum class TimeSyncState : uint8_t {
    IDLE,       // no WiFi yet / SNTP not started
    SYNCING,    // SNTP started, waiting for the first packet
    OK,         // at least one packet landed
};

// Set the timezone and register the SNTP callback. Call once in setup(), early —
// before the idle clock first renders — so localtime shows Singapore time.
void time_sync_init();

// Call periodically from the main loop (e.g. the 1 Hz housekeeping). Starts SNTP
// once WiFi is up, and persists the time (RTC + NVS) whenever a packet arrives.
void time_sync_tick();

// Force an immediate re-sync (e.g. the settings "Sync now" action). No-op if
// WiFi is down. Safe to call from the main task.
void time_sync_force();

// Live state for the settings indicator.
TimeSyncState time_sync_state();

// Unix time (UTC) of the last successful sync, 0 if never. For a "synced n ago".
time_t time_sync_last();
