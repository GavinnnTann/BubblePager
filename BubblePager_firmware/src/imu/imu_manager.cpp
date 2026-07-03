#include "imu_manager.h"
#include "hardware/hardware.h"
#include "config.h"
#include <Wire.h>
#include <Arduino.h>

// ── QMI8658C register map (verified against SensorLib + QMI8658C datasheet) ──
static constexpr uint8_t QMI_ADDR       = IMU_I2C_ADDR;  // 0x6B (SA0 high)

static constexpr uint8_t REG_WHO_AM_I   = 0x00;  // read: 0x05
static constexpr uint8_t REG_CTRL1      = 0x02;
static constexpr uint8_t REG_CTRL2      = 0x03;  // accel ODR + full-scale
static constexpr uint8_t REG_CTRL7      = 0x08;  // sensor enable
static constexpr uint8_t REG_CTRL8      = 0x09;  // motion / tap config
static constexpr uint8_t REG_STATUSINT  = 0x2D;  // interrupt status
static constexpr uint8_t REG_AX_L       = 0x35;  // accel XYZ, 6 bytes little-endian
static constexpr uint8_t REG_RESET      = 0x60;  // write 0xB0 to soft-reset
static constexpr uint8_t REG_RST_RESULT = 0x4D;  // 0x80 when reset complete

static constexpr uint8_t CTRL1_RUN       = 0x48; // EN.ADDR_AI | INT1_EN, INT1 active-HIGH
static constexpr uint8_t CTRL2_RUN       = 0x2C; // ±8g, LP-128Hz
static constexpr uint8_t CTRL8_HANDSHAKE = 0x80; // STATUS_INT bit7 as CTRL9 CMD_DONE

// ── I²C helpers ──────────────────────────────────────────────────────────────
static bool qmi_write(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(QMI_ADDR);
    Wire.write(reg);
    Wire.write(val);
    return Wire.endTransmission() == 0;
}

static bool qmi_read(uint8_t reg, uint8_t* buf, uint8_t len) {
    Wire.beginTransmission(QMI_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;
    Wire.requestFrom((uint8_t)QMI_ADDR, len);
    for (uint8_t i = 0; i < len; i++) {
        if (!Wire.available()) return false;
        buf[i] = Wire.read();
    }
    return true;
}

static bool qmi_read1(uint8_t reg, uint8_t* val) { return qmi_read(reg, val, 1); }

// Soft reset — chip re-initialises and auto-sets EN.ADDR_AI in CTRL1.
static bool qmi_soft_reset() {
    qmi_write(REG_RESET, 0xB0);
    delay(15);
    uint32_t t = millis();
    uint8_t rst = 0;
    while (millis() - t < 100) {
        if (qmi_read1(REG_RST_RESULT, &rst) && rst == 0x80) return true;
        delay(5);
    }
    return rst == 0x80;
}

// ── State ─────────────────────────────────────────────────────────────────────
static bool        s_ok       = false;
static ImuSettings s_cfg      = {};
static int16_t     s_prev_ax = 0, s_prev_ay = 0, s_prev_az = 0;
static bool        s_prev_valid = false;
static uint8_t     s_move_run   = 0;

bool imu_init() {
    Wire.beginTransmission(QMI_ADDR);
    if (Wire.endTransmission() != 0) {
#ifdef DEBUG_SERIAL
        Serial.printf("[IMU] no device at 0x%02X\n", QMI_ADDR);
#endif
        s_ok = false;
        return false;
    }
    if (!qmi_soft_reset()) { s_ok = false; return false; }

    uint8_t who = 0;
    if (!qmi_read1(REG_WHO_AM_I, &who) || who != 0x05) {
#ifdef DEBUG_SERIAL
        Serial.printf("[IMU] WHO_AM_I=0x%02X (expected 0x05) — init FAIL\n", who);
#endif
        s_ok = false;
        return false;
    }

    qmi_write(REG_CTRL1, CTRL1_RUN);
    qmi_write(REG_CTRL2, CTRL2_RUN);
    qmi_write(REG_CTRL8, CTRL8_HANDSHAKE);
    qmi_write(REG_CTRL7, 0x01);   // accel enable
#ifdef DEBUG_SERIAL
    Serial.println("[IMU] init OK");
#endif
    s_ok = true;
    return true;
}

void imu_configure(const ImuSettings& cfg) {
    s_cfg = cfg;
    if (!s_ok) return;
    // Accel-only run mode (motion + orientation are both read from the accel regs).
    qmi_write(REG_CTRL7, 0x00);
    qmi_write(REG_CTRL2, CTRL2_RUN);
    qmi_write(REG_CTRL8, CTRL8_HANDSHAKE);
    qmi_write(REG_CTRL7, 0x01);
#ifdef DEBUG_SERIAL
    Serial.printf("[IMU] configure: adaptive=%d flip=%d\n",
                  cfg.adaptive_timeout, cfg.orient_flip);
#endif
}

void imu_sleep_mode() {
    if (!s_ok) return;
    qmi_write(REG_CTRL7, 0x00);  // accel off
}

bool imu_read_accel(int16_t* ax, int16_t* ay, int16_t* az) {
    if (!s_ok || !ax || !ay || !az) return false;
    uint8_t buf[6];
    if (!qmi_read(REG_AX_L, buf, 6)) return false;
    *ax = (int16_t)((buf[1] << 8) | buf[0]);
    *ay = (int16_t)((buf[3] << 8) | buf[2]);
    *az = (int16_t)((buf[5] << 8) | buf[4]);
    return true;
}

bool imu_is_moving() {
    if (!s_ok || !s_cfg.adaptive_timeout) return false;
    int16_t ax, ay, az;
    if (!imu_read_accel(&ax, &ay, &az)) return false;

    // Per-axis vector delta from the previous sample — a high-pass that rejects
    // the constant gravity term (differencing the magnitude scales with gravity
    // and reads noise as motion).
    int32_t dx = (int32_t)ax - s_prev_ax;
    int32_t dy = (int32_t)ay - s_prev_ay;
    int32_t dz = (int32_t)az - s_prev_az;
    s_prev_ax = ax; s_prev_ay = ay; s_prev_az = az;
    if (!s_prev_valid) { s_prev_valid = true; return false; }

    int32_t delta2  = dx*dx + dy*dy + dz*dz;
    int32_t thresh2 = (int32_t)IMU_MOTION_THRESHOLD * IMU_MOTION_THRESHOLD;
    if (delta2 > thresh2) { if (s_move_run < 255) s_move_run++; }
    else                  { s_move_run = 0; }
    return s_move_run >= IMU_MOTION_DEBOUNCE;
}

bool imu_is_flipped(bool current_state) {
    if (!s_ok || !s_cfg.orient_flip) return false;
    int16_t ax, ay, az;
    if (!imu_read_accel(&ax, &ay, &az)) return current_state;
    // Only re-evaluate in portrait (|ax| > 2000); hold state when tilted sideways.
    int16_t ax_abs = ax < 0 ? -ax : ax;
    if (ax_abs <= 2000) return current_state;
    return ax > 500;
}
