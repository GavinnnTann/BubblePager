#pragma once
#include <stdint.h>
#include <stdbool.h>

// QMI8658C driver — accelerometer only (gyro disabled for power). Ported from
// FobBob. Drives the TelePager settings features: motion-wake (adaptive display
// timeout) and orientation flip. The IMU sits on the shared I²C bus (0x6B).

// Full init: soft-reset, verify WHO_AM_I, configure accel ODR/scale. Returns
// false if the sensor is absent or the bus faults.
bool imu_init();

// Which accel-derived features are active. Seed from NVS, re-apply on any change.
struct ImuSettings {
    bool tap_sleep;         // double-tap the SCREEN → sleep (handled by touch, not IMU)
    bool adaptive_timeout;  // motion resets the display idle timer
    bool orient_flip;       // upside-down → display 180° rotation
};
void imu_configure(const ImuSettings& cfg);

// Low-power (accel off) before deep sleep.
void imu_sleep_mode();

// Raw accel (LSB; 1 g ≈ 4096 LSB at ±8 g). Returns false on I²C error.
bool imu_read_accel(int16_t* ax, int16_t* ay, int16_t* az);

// True when the vector delta since the last call exceeds the motion threshold
// (with debounce). No-op / false when adaptive_timeout is disabled.
bool imu_is_moving();

// True when the device appears upside-down. Pass the current flip state so it is
// held while the device is tilted sideways (prevents spurious un-flips). No-op /
// returns current_state when orient_flip is disabled.
bool imu_is_flipped(bool current_state);
