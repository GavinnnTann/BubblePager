#include "time_sync.h"
#include "wifi_mgr.h"
#include "rtc/rtc_manager.h"
#include "nvs/nvs_manager.h"
#include "config.h"

#include <Arduino.h>
#include <time.h>
#include <esp_sntp.h>

// Singapore: UTC+8, no DST. POSIX TZ sign is inverted, so "+8" is written "-8".
static constexpr const char* TZ_SINGAPORE = "SGT-8";
// A plausible-time floor: anything below this is an unset clock, not a real sync.
static constexpr time_t NTP_SANITY_EPOCH  = 1700000000;  // 2023-11-14
// Re-sync no more often than this once we have a good time.
static constexpr uint32_t SYNC_INTERVAL_MS = 60UL * 60UL * 1000UL;  // 1 h

static volatile bool s_packet   = false;   // set by SNTP task on a real packet
static bool          s_started  = false;   // configTime() issued (SNTP running)
static TimeSyncState s_state    = TimeSyncState::IDLE;
static time_t        s_last     = 0;       // last good sync (UTC)
static uint32_t      s_last_ms  = 0;       // millis() of last good sync

// Runs in the SNTP task context — do NOT touch I2C (RTC) here; just flag it and
// let time_sync_tick() persist on the main task (single I2C owner).
static void on_ntp_sync(struct timeval*) { s_packet = true; }

void time_sync_init() {
    setenv("TZ", TZ_SINGAPORE, 1);
    tzset();
    sntp_set_time_sync_notification_cb(on_ntp_sync);
}

static void start_sntp() {
    // Use configTzTime (NOT configTime): configTime(0,0,…) internally resets TZ to
    // UTC, clobbering our SGT-8 setenv and making the clock read 8 h behind. The
    // epoch stays UTC regardless (time() is always UTC); TZ only shifts the display.
    configTzTime(TZ_SINGAPORE, "pool.ntp.org", "time.google.com");
    s_started = true;
    s_state   = TimeSyncState::SYNCING;
}

void time_sync_tick() {
    if (!wifi_mgr_connected()) return;

    if (!s_started) start_sntp();

    if (s_packet) {
        s_packet = false;
        time_t now = time(nullptr);
        if (now > NTP_SANITY_EPOCH) {
            rtc_write(now);          // refresh the crystal-backed RTC (UTC)
            nvs_save_time(now);      // bound rollback on a future power loss
            s_last    = now;
            s_last_ms = millis();
            s_state   = TimeSyncState::OK;
#ifdef DEBUG_SERIAL
            Serial.printf("[NTP] sync OK: unix=%lld\n", (long long)now);
#endif
        }
    }
}

void time_sync_force() {
    if (!wifi_mgr_connected()) return;
    if (!s_started) { start_sntp(); return; }
    // Already running — kick an immediate re-poll.
    s_state = TimeSyncState::SYNCING;
    sntp_restart();
}

TimeSyncState time_sync_state() { return s_state; }
time_t        time_sync_last()  { return s_last; }
