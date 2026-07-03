#pragma once
#include <stdint.h>

// Enter deep sleep immediately (backlight latched off). Wakes on the next reset
// or power cycle — TelePager has no RTC-domain wake button, so this is a manual
// "off". Persists the clock first.
void power_go_to_sleep();

// Call every loop iteration. Dims the display (backlight off, LVGL keeps running)
// after DISPLAY_TIMEOUT_SEC of inactivity so an idle pager isn't glowing all night.
// The screen restores on the next activity (video / button).
void power_tick();

// Reset the idle timer — call on any activity (incoming video, button press).
void power_reset_idle_timer();

// Toggle the backlight on/off (the power-button action). Keeps s_dimmed in sync so
// a later press always reverses it — the manual display_off() path did not, which
// left the power button able to turn the screen off but never back on.
void power_toggle_display();

// True while the display has been dimmed (idle timeout OR manual power button).
bool power_display_dimmed();
