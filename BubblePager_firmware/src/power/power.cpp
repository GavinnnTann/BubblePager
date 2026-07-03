#include "power.h"
#include "hardware/hardware.h"
#include "display/display.h"
#include "nvs/nvs_manager.h"
#include "config.h"
#include <esp_sleep.h>
#include <time.h>

static uint32_t s_last_activity_ms = 0;
static bool     s_dimmed           = false;

void power_go_to_sleep() {
    time_t now = time(nullptr);
    if (now > 1000000000) nvs_save_time(now);

    display_sleep();
    hardware_configure_wakeup();
    esp_deep_sleep_start();
}

void power_reset_idle_timer() {
    s_last_activity_ms = millis();
    if (s_dimmed) {
        display_on();
        s_dimmed = false;
    }
}

void power_toggle_display() {
    if (s_dimmed) {
        display_on();
        s_dimmed = false;
        s_last_activity_ms = millis();   // restart the idle countdown on wake
    } else {
        display_off();
        s_dimmed = true;                 // keep state in sync so the next press wakes
    }
}

void power_tick() {
#if DISPLAY_TIMEOUT_SEC > 0
    if (s_dimmed) return;
    if ((millis() - s_last_activity_ms) >= (uint32_t)DISPLAY_TIMEOUT_SEC * 1000UL) {
        display_off();          // backlight off; LVGL keeps running underneath
        s_dimmed = true;
    }
#endif
}

bool power_display_dimmed() { return s_dimmed; }
