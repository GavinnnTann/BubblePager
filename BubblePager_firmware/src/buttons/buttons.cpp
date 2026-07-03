#include "buttons.h"
#include "hardware/hardware.h"
#include "config.h"
#include <Wire.h>

static const uint32_t DEBOUNCE_MS = 30;
static constexpr uint8_t TCA6408_ADDR = 0x20;

struct Btn {
    bool     raw;
    bool     deb;
    uint32_t change_ms;
    uint32_t down_ms;
    bool     hold_fired;
};
static Btn s_b1 = {}, s_b2 = {}, s_bp = {};

// Read TCA6408 input port register (0x00). Returns 0xFF on I²C error (all released).
static uint8_t tca_read_port() {
    Wire.beginTransmission(TCA6408_ADDR);
    Wire.write(0x00);
    if (Wire.endTransmission(false) != 0) return 0xFF;
    Wire.requestFrom(TCA6408_ADDR, (uint8_t)1);
    return Wire.available() ? Wire.read() : 0xFF;
}

static void debounce(Btn& b, bool raw) {
    if (raw != b.raw) { b.raw = raw; b.change_ms = millis(); }
    if ((millis() - b.change_ms) >= DEBOUNCE_MS) b.deb = raw;
}

void buttons_init() {
    // All TCA6408 pins as inputs (config register 0x03 = 0xFF).
    Wire.beginTransmission(TCA6408_ADDR);
    Wire.write(0x03);
    Wire.write(0xFF);
    Wire.endTransmission();

    uint8_t port = tca_read_port();
    bool b1 = !(port & (1 << BTN1_BIT));
    bool b2 = !(port & (1 << BTN2_BIT));
    bool bp = !(port & (1 << BTN_PWR_BIT));
    s_b1 = { b1, b1, millis(), 0, false };
    s_b2 = { b2, b2, millis(), 0, false };
    s_bp = { bp, bp, millis(), 0, false };
}

uint8_t buttons_read_port() { return tca_read_port(); }

// Classify one button from its previous debounced state. Returns 0 none, 1 short
// (release before hold), 2 hold (fired once at the threshold while still held).
static int classify(Btn& b, bool prev_deb, uint32_t hold_ms) {
    uint32_t now = millis();
    if (b.deb && !prev_deb) {                 // press edge
        b.down_ms = now; b.hold_fired = false;
        return 0;
    }
    if (b.deb && prev_deb) {                   // held
        if (!b.hold_fired && (now - b.down_ms) >= hold_ms) { b.hold_fired = true; return 2; }
        return 0;
    }
    if (!b.deb && prev_deb) {                  // release edge
        return b.hold_fired ? 0 : 1;
    }
    return 0;
}

ButtonEvent buttons_tick() {
    bool p1 = s_b1.deb, p2 = s_b2.deb, pp = s_bp.deb;

    uint8_t port = tca_read_port();
    debounce(s_b1, !(port & (1 << BTN1_BIT)));
    debounce(s_b2, !(port & (1 << BTN2_BIT)));
    debounce(s_bp, !(port & (1 << BTN_PWR_BIT)));

    int r;
    r = classify(s_bp, pp, BTN_PWR_HOLD_MS);
    if (r == 1) return ButtonEvent::PWR_SHORT;
    if (r == 2) return ButtonEvent::PWR_HOLD;
    r = classify(s_b1, p1, BTN_HOLD_MS);
    if (r == 1) return ButtonEvent::BTN1_SHORT;
    if (r == 2) return ButtonEvent::BTN1_HOLD;
    r = classify(s_b2, p2, BTN_HOLD_MS);
    if (r == 1) return ButtonEvent::BTN2_SHORT;
    if (r == 2) return ButtonEvent::BTN2_HOLD;
    return ButtonEvent::NONE;
}
