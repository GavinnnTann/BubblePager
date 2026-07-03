#include "haptics.h"
#include "hardware/hardware.h"
#include "config.h"
#include <Arduino.h>

// LEDC channel 0 is the display backlight (see display.cpp) — use channel 2 here.
// PWM freq / duty + pattern durations are tunable in config.h.
static constexpr uint8_t  HAPTIC_LEDC_CH = 2;

// A pattern is a list of alternating ON,OFF,ON,… durations in ms (even index = on).
static const uint16_t *s_seq   = nullptr;
static uint8_t         s_seq_n = 0;
static uint8_t         s_seq_i = 0;
static uint32_t        s_seq_t = 0;
static bool            s_enabled = true;

static void motor_set(bool on) {
    ledcWrite(HAPTIC_LEDC_CH, on ? HAPTIC_DUTY : 0);
}

static void play(const uint16_t *seq, uint8_t n) {
    s_seq = seq; s_seq_n = n; s_seq_i = 0; s_seq_t = 0;  // poll() starts it on the next call
}

void haptics_init() {
    ledcSetup(HAPTIC_LEDC_CH, HAPTIC_PWM_HZ, 8);
    ledcAttachPin(HAPTIC_PIN, HAPTIC_LEDC_CH);
    motor_set(false);
}

void haptics_poll() {
    if (!s_seq) return;
    if ((int32_t)(millis() - s_seq_t) < 0) return;     // current segment still running
    if (s_seq_i >= s_seq_n) { motor_set(false); s_seq = nullptr; return; }
    motor_set((s_seq_i & 1) == 0);                     // even index → on, odd → off
    s_seq_t = millis() + s_seq[s_seq_i];
    s_seq_i++;
}

static const uint16_t PAT_TICK[]   = { HAPTIC_TICK_MS };    // single confirmation pulse
static const uint16_t PAT_CLICK[]  = { HAPTIC_CLICK_MS };   // very short detent pulse
static const uint16_t PAT_DOUBLE[] = { HAPTIC_DOUBLE_ON_MS, HAPTIC_DOUBLE_GAP_MS, HAPTIC_DOUBLE_ON_MS };
// Incoming-video alert: [ON, OFF] x3 (last OFF omitted). Even index = on.
static const uint16_t PAT_INCOMING[] = {
    HAPTIC_INCOMING_ON_MS, HAPTIC_INCOMING_OFF_MS,
    HAPTIC_INCOMING_ON_MS, HAPTIC_INCOMING_OFF_MS,
    HAPTIC_INCOMING_ON_MS,
};

void haptic_incoming() { if (s_enabled) play(PAT_INCOMING, 5); }
void haptic_tick()   { if (s_enabled) play(PAT_TICK,   1); }
void haptic_click()  { if (s_enabled) play(PAT_CLICK,  1); }
void haptic_double() { if (s_enabled) play(PAT_DOUBLE, 3); }

void haptic_blocking(uint16_t ms) {
    if (!s_enabled) return;
    motor_set(true);
    delay(ms);
    motor_set(false);
}

void haptic_set_enabled(bool en) {
    s_enabled = en;
    if (!en) { motor_set(false); s_seq = nullptr; }   // cut any in-flight pattern
}

bool haptic_enabled() { return s_enabled; }
