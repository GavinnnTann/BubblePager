#include "nvs_manager.h"
#include <Preferences.h>

static Preferences s_prefs;
static bool        s_ok = false;

static constexpr const char* NS       = "telepager";
static constexpr const char* KEY_TIME = "last_time";
static constexpr const char* KEY_SET  = "settings";   // packed AppSettings bitfield
static constexpr const char* KEY_INTO = "in_timeout";  // incoming-pulse timeout (sec)

bool nvs_init() {
    s_ok = s_prefs.begin(NS, false);   // read/write
    return s_ok;
}

void nvs_save_time(time_t t) {
    if (!s_ok || t <= 0) return;
    s_prefs.putULong64(KEY_TIME, (uint64_t)t);
}

time_t nvs_load_time() {
    if (!s_ok) return 0;
    return (time_t)s_prefs.getULong64(KEY_TIME, 0);
}

// Pack the four bools into one byte. Bit 7 marks "stored" so an unset key (0)
// falls through to defaults rather than reading as all-off.
static constexpr uint8_t SET_PRESENT = 0x80;
static constexpr uint8_t SET_HAPTICS = 0x01;
static constexpr uint8_t SET_TAPSLP  = 0x02;
static constexpr uint8_t SET_ADAPT   = 0x04;
static constexpr uint8_t SET_FLIP    = 0x08;

AppSettings nvs_load_settings() {
    // Defaults: haptics + motion-wake + auto-flip on, tap-to-sleep off, 5s pulse.
    AppSettings d = { true, false, true, true, 5 };
    if (!s_ok) return d;
    uint8_t v = s_prefs.getUChar(KEY_SET, 0);
    if (!(v & SET_PRESENT)) return d;
    d.haptics          = (bool)(v & SET_HAPTICS);
    d.tap_sleep        = (bool)(v & SET_TAPSLP);
    d.adaptive_timeout = (bool)(v & SET_ADAPT);
    d.orient_flip      = (bool)(v & SET_FLIP);
    d.incoming_timeout_sec = s_prefs.getUShort(KEY_INTO, 5);
    return d;
}

void nvs_save_settings(const AppSettings& s) {
    if (!s_ok) return;
    uint8_t v = SET_PRESENT;
    if (s.haptics)          v |= SET_HAPTICS;
    if (s.tap_sleep)        v |= SET_TAPSLP;
    if (s.adaptive_timeout) v |= SET_ADAPT;
    if (s.orient_flip)      v |= SET_FLIP;
    s_prefs.putUChar(KEY_SET, v);
    s_prefs.putUShort(KEY_INTO, s.incoming_timeout_sec);
}
