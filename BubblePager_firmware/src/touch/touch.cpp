#include "touch.h"
#include "hardware/hardware.h"
#include <Arduino.h>
#include <Wire.h>

static constexpr uint8_t CST816S_ADDR   = 0x15;
static constexpr uint8_t REG_GESTURE    = 0x01;  // burst: gesture, finger_count, xH,xL,yH,yL
static constexpr uint8_t REG_CHIP_ID    = 0xA7;  // 0xB4=CST816S / 0xB5=T / 0xB6=D
static constexpr uint8_t REG_IRQ_CTL    = 0xFA;  // interrupt mode
static constexpr uint8_t REG_MOTION_MASK= 0xEC;  // gesture enable mask
static constexpr uint8_t REG_DIS_SLEEP  = 0xFE;  // auto-sleep disable

// touch_tapped() poll cadence during playback. 30 ms is fine to catch the down
// and up edges of a tap while keeping the shared I2C bus light.
static constexpr uint32_t TOUCH_POLL_MS = 30;
static constexpr int      TAP_MOVE_MAX  = 12;   // px travel still counted as a tap
static constexpr uint32_t TAP_MAX_MS    = 600;  // longest contact still a tap
// After a tap, refuse the next one until the finger has been CONTINUOUSLY absent
// this long. Lift-off contact bounce (present→absent→present as the finger rolls
// off) therefore can't fabricate extra taps and self-oscillate the pause.
static constexpr uint32_t TAP_REARM_MS  = 250;

static bool     s_ok        = false;   // chip probed OK at init
static uint32_t s_last_poll = 0;       // throttle: last touch_tapped() bus access
static bool     s_flip      = false;   // mirror coordinates when display is flipped

// touch_tapped() gesture state (release-based tap, like FobBob).
static bool     s_down         = false;
static int      s_start_x      = 0, s_start_y = 0;
static uint32_t s_down_ms      = 0;
static bool     s_moved        = false;
static bool     s_armed        = true; // may a new tap gesture start?
static uint32_t s_absent_since = 0;    // when the finger was first seen absent (0 = unknown)

// Tri-state contact read result.
enum Contact { CONTACT_ERR = -1, CONTACT_NONE = 0, CONTACT_DOWN = 1 };

static bool cst_read(uint8_t reg, uint8_t* buf, uint8_t len);   // defined below

// Read the finger state (and coords). Returns tri-state so callers can hold their
// gesture across a transient I2C error instead of mistaking it for a finger lift.
static int read_contact(int* x, int* y) {
    uint8_t buf[6] = {};
    if (!cst_read(REG_GESTURE, buf, 6)) return CONTACT_ERR;
    // Valid finger_count is 0 or 1. Anything else (e.g. 0x40/0x80) is a byte-slipped
    // burst read — reject it as an error rather than a phantom "finger present".
    if (buf[1] > 1) return CONTACT_ERR;
    if (buf[1] == 0) return CONTACT_NONE;
    int rx = ((int)(buf[2] & 0x0F) << 8) | buf[3];
    int ry = ((int)(buf[4] & 0x0F) << 8) | buf[5];
    // Coords must be on-panel (0–239); a wild value is another slip signature.
    if (rx > 260 || ry > 260) return CONTACT_ERR;
    if (!s_flip) { rx = 239 - rx; ry = 239 - ry; }
    if (rx < 0) rx = 0; else if (rx > 239) rx = 239;
    if (ry < 0) ry = 0; else if (ry > 239) ry = 239;
    if (x) *x = rx;
    if (y) *y = ry;
    return CONTACT_DOWN;
}

static bool cst_write(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(CST816S_ADDR);
    Wire.write(reg);
    Wire.write(val);
    return Wire.endTransmission() == 0;
}

static bool cst_read(uint8_t reg, uint8_t* buf, uint8_t len) {
    Wire.beginTransmission(CST816S_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;
    Wire.requestFrom(CST816S_ADDR, len);
    for (uint8_t i = 0; i < len; i++) {
        if (!Wire.available()) return false;
        buf[i] = Wire.read();
    }
    return true;
}

void touch_init() {
    // Hardware reset. GPIO0 is the BOOT strapping pin but safe to drive after setup.
    pinMode(TOUCH_RST_PIN, OUTPUT);
    digitalWrite(TOUCH_RST_PIN, HIGH);
    delay(5);
    digitalWrite(TOUCH_RST_PIN, LOW);
    delay(10);
    digitalWrite(TOUCH_RST_PIN, HIGH);
    delay(100);

    uint8_t id = 0;
    bool ok = cst_read(REG_CHIP_ID, &id, 1);
#ifdef DEBUG_SERIAL
    Serial.printf("[TOUCH] probe: %s chip_id=0x%02X (expect 0xB4/B5/B6)\n",
                  ok ? "ACK" : "NACK", id);
#endif
    s_ok = ok && (id == 0xB4 || id == 0xB5 || id == 0xB6);
    if (!s_ok) return;

    // REG_DIS_SLEEP = 0xFF: disable auto-sleep. Without this the chip sleeps after
    // ~5 s idle and then NAKs I2C (bus timeouts that lag playback) and returns
    // inconsistent finger counts (phantom taps that self-toggle the pause).
    cst_write(REG_IRQ_CTL,     0x11);   // motion mode: report click/swipe/long-press
    cst_write(REG_MOTION_MASK, 0x01);   // enable double-click detection
    cst_write(REG_DIS_SLEEP,   0xFF);   // keep the controller awake

    s_down         = false;
    s_moved        = false;
    s_armed        = true;
    s_absent_since = 0;
}

// Release-based tap (like FobBob): a tap = finger down → up, minimal movement,
// short contact. After firing we DISARM and only re-arm once the finger has been
// continuously absent for TAP_REARM_MS, so lift-off contact bounce can't fire a
// second tap and oscillate the pause.
bool touch_tapped() {
    if (!s_ok) return false;

    uint32_t now = millis();
    if (now - s_last_poll < TOUCH_POLL_MS) return false;
    s_last_poll = now;

    int x = 0, y = 0;
    int c = read_contact(&x, &y);
    if (c == CONTACT_ERR) return false;    // transient bus error — hold the gesture

    if (c == CONTACT_DOWN) {
        s_absent_since = 0;
        if (!s_down && s_armed) {           // start a gesture only when armed
            s_down = true; s_start_x = x; s_start_y = y; s_down_ms = now; s_moved = false;
        } else if (s_down) {
            if (abs(x - s_start_x) > TAP_MOVE_MAX || abs(y - s_start_y) > TAP_MOVE_MAX)
                s_moved = true;
        }
        return false;
    }

    // CONTACT_NONE — finger absent.
    bool tap = false;
    if (s_down) {                           // release edge → classify
        s_down = false;
        if (!s_moved && (now - s_down_ms) < TAP_MAX_MS) tap = true;
        s_armed        = false;             // disarm until a clean sustained absence
        s_absent_since = now;
    }
    if (!s_armed) {                         // re-arm after continuous absence
        if (s_absent_since == 0) s_absent_since = now;
        if (now - s_absent_since >= TAP_REARM_MS) s_armed = true;
    }
    return tap;
}

bool touch_read_point(int* x, int* y) {
    if (!s_ok) return false;
    return read_contact(x, y) == CONTACT_DOWN;
}

int touch_read_contact(int* x, int* y) {
    if (!s_ok) return CONTACT_ERR;
    return read_contact(x, y);
}

void touch_set_flipped(bool flipped) { s_flip = flipped; }
