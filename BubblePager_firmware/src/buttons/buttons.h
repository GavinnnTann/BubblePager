#pragma once
#include <stdint.h>

enum class ButtonEvent : uint8_t {
    NONE,
    BTN1_SHORT,      // BTN1 (older): released before the hold threshold
    BTN2_SHORT,      // BTN2 (newer): released before the hold threshold
    BTN1_HOLD,       // BTN1 held >= BTN_HOLD_MS (fires once) → play
    BTN2_HOLD,       // BTN2 held >= BTN_HOLD_MS (fires once) → play
    PWR_SHORT,       // Power tapped → sleep / wake (backlight)
    PWR_HOLD,        // Power held >= BTN_PWR_HOLD_MS → shutdown (deep sleep)
};

// Call once in setup() after hardware_init().
void buttons_init();

// Call every loop iteration. Returns one pending event (SHORT on release, HOLD
// once at the threshold while still held) or NONE.
ButtonEvent buttons_tick();

// Read the raw TCA6408A input port byte. Used by boot_determine_mode() to
// identify which signal (Touch_INT, IMU_INT1, etc.) caused a GPIO45 wakeup.
// Returns 0xFF on I²C error.
uint8_t buttons_read_port();
