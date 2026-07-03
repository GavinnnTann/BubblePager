#pragma once
#include <Arduino.h>
#include <time.h>

// Returns false if the oscillator-stop flag is set (first power-on or coin cell died).
// Must be called before rtc_read(); Wire.begin() is called internally.
bool   rtc_init();

// Read current time from PCF85063. Returns 0 if oscillator was stopped.
time_t rtc_read();

// Write unix time to PCF85063 and clear the oscillator-stop flag.
void   rtc_write(time_t t);

// True only when the RTC has valid time (oscillator never stopped since last rtc_write).
bool   rtc_is_valid();
