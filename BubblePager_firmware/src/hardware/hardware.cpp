#include "hardware.h"
#include <driver/gpio.h>
#include <esp_sleep.h>
#include <Wire.h>

void hardware_init() {
    // I²C bus — shared by PCF85063 RTC, TCA6408 expander (buttons), CST816S touch.
    Wire.begin(RTC_SDA_PIN, RTC_SCL_PIN);
    // 100 kHz (default). 400 kHz caused byte-slipped CST816S burst reads on this
    // board — the finger-count byte came back as 0x40/0x80 (phantom "finger present"
    // → pause oscillation) and coords as garbage (long-press never held). At 100 kHz
    // the reads are clean; the touch lag that motivated 400 kHz is already solved by
    // disabling the controller's auto-sleep (touch.cpp), so speed isn't needed.
    Wire.setClock(100000);
    // Bound the per-transaction timeout so a wedged bus can't stall the render loop
    // for ~1 s (Wire "Error 263"). Ample for these 1–7 byte reads at 100 kHz.
    Wire.setTimeOut(20);
    // Backlight PWM is configured in display_init() (ledc channel 0).
}

void hardware_configure_wakeup() {
    // GPIO42 (backlight) is outside the RTC domain (RTC GPIOs are 0-21 only).
    // gpio_hold_en alone only latches during light sleep; deep sleep on a digital
    // GPIO also needs gpio_deep_sleep_hold_en() so the pad isolation latch engages
    // when the digital power domain drops. Without this the pin floats and residual
    // current keeps the backlight glowing.
    gpio_set_level((gpio_num_t)TFT_BL_PIN, 0);
    gpio_hold_en((gpio_num_t)TFT_BL_PIN);
    gpio_deep_sleep_hold_en();
}
