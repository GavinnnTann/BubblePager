#pragma once
#include <stdint.h>
#include <stdbool.h>

// LIR2450 (rechargeable Li-ion coin cell) monitoring.
//   Voltage: GPIO1 / ADC1_CH0 via a 100k:100k divider (÷2).
//   Charge detect: TCA6408 P6 — LOW = USB/VIN present (charging), HIGH = on battery.

struct BatteryStatus {
    uint8_t  percent;     // 0..100, from a non-linear Li-ion discharge curve
    uint16_t millivolts;  // estimated pack voltage (post-divider × 2)
    bool     charging;    // true while external power is connected
};

// Configure the ADC pin. Call once in setup() after buttons_init().
void battery_init();

// Sample the ADC (averaged) + read the charge-detect line.
BatteryStatus battery_read();

// Last value returned by battery_read(), WITHOUT touching the ADC. Lets another
// task (e.g. the heartbeat reporter) read a fresh snapshot without racing the ADC.
BatteryStatus battery_last();
