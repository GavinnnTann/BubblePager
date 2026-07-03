#pragma once
#include <stdint.h>

// Vibration-motor haptics — 2N2222 low-side switch on HAPTIC_PIN, driven via LEDC.
//
// Patterns are non-blocking: haptic_tick()/haptic_double() just queue a sequence;
// haptics_poll() (call every main-loop iteration) advances it off millis(). Use
// haptic_blocking() only for events that immediately deep-sleep or restart, where
// the poll loop will never run again.

void haptics_init();
void haptics_poll();   // advance the active pattern; call once per loop iteration

// Master enable. When disabled, all haptic_*() calls become no-ops (and any
// in-flight pattern is cut). Seed from NVS at boot; toggled live by the settings
// page. Default enabled.
void haptic_set_enabled(bool en);
bool haptic_enabled();

void haptic_incoming();// incoming-video buzz: [100ms ON, 50ms OFF] x3 (PATTERN_INCOMING)
void haptic_tick();    // one short crisp pulse — confirmations (verify OK, tap, plug-in)
void haptic_click();   // very short pulse — scroll/nav detents (kept distinct so fast
                       // scrolling reads as separate clicks, not one continuous buzz)
void haptic_double();  // two pulses — rejection / alert (verify fail)

// Synchronous pulse for terminal events (sleep / restart) that won't poll again.
void haptic_blocking(uint16_t ms);
