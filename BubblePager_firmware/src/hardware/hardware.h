#pragma once
#include <Arduino.h>

// ── Display (GC9A01 — I80 8-bit parallel, via LCD_CAM peripheral) ──────────
// Interface: I80, NOT SPI. Data bus D0–D7 must be consecutive GPIOs from TFT_D0.
constexpr int TFT_D0_PIN   = 10;  // I80 D0
constexpr int TFT_D1_PIN   = 11;  // I80 D1
constexpr int TFT_D2_PIN   = 12;  // I80 D2
constexpr int TFT_D3_PIN   = 13;  // I80 D3
constexpr int TFT_D4_PIN   = 14;  // I80 D4
constexpr int TFT_D5_PIN   = 15;  // I80 D5
constexpr int TFT_D6_PIN   = 16;  // I80 D6
constexpr int TFT_D7_PIN   = 17;  // I80 D7
constexpr int TFT_WR_PIN   = 3;   // I80 WRB (write strobe, active LOW)
constexpr int TFT_RS_PIN   = 18;  // I80 RS  (0 = command, 1 = data)
constexpr int TFT_CS_PIN   = 2;   // I80 CS  (chip select, active LOW)
constexpr int TFT_RST_PIN  = 21;  // Display reset (active LOW)
constexpr int TFT_BL_PIN   = 42;  // Backlight — NPN Q2, IO42 HIGH = on

// ── PCF85063 RTC + TCA6408 expander — shared I²C (SIIC bus) ────────────────
constexpr int RTC_SDA_PIN  = 8;   // SIIC_SDA_IO8
constexpr int RTC_SCL_PIN  = 9;   // SIIC_SCL_IO9

// ── Shared-I²C device addresses (single source of truth) ───────────────────
constexpr int TCA6408_I2C_ADDR = 0x20;  // buttons expander
constexpr int RTC_I2C_ADDR     = 0x51;  // PCF85063 RTC
constexpr int TOUCH_I2C_ADDR   = 0x15;  // CST816S capacitive touch
constexpr int IMU_I2C_ADDR     = 0x6B;  // QMI8658 accelerometer/IMU

// ── CST816S capacitive touch (unused in TelePager; buttons drive the UX) ───
constexpr int TOUCH_RST_PIN   = 0;   // CST816 RST — GPIO0 (safe to drive after boot)

// ── Battery monitoring ───────────────────────────────────────────────────
constexpr int BAT_ADC_PIN     = 1;   // GPIO1 / ADC1_CH0 — battery via 100k:100k divider (÷2)

// ── Haptics ──────────────────────────────────────────────────────────────
constexpr int HAPTIC_PIN      = 38;  // vibration motor — 2N2222 low-side switch

// ── TCA6408 I/O expander — buttons ─────────────────────────────────────────
// INT (GPIO45) is left unwired: it is outside the RTC domain and its pulses are
// sub-ms through a slow expander — unusable for wake. Poll the port instead.
constexpr int TCA6408_INT_PIN = 45;

// Port-bit assignments (TCA6408A P0–P7), all active LOW.
constexpr int BTN1_BIT       = 3;    // TCA6408 P3 — SW_UP   (skip / return to idle)
constexpr int BTN_PWR_BIT    = 4;    // TCA6408 P4 — SW_Power (display sleep toggle)
constexpr int BTN2_BIT       = 5;    // TCA6408 P5 — SW_DOWN (replay last video)
constexpr int CHARGE_DET_BIT = 6;    // TCA6408 P6 — LOW = USB/VIN present (charging)

// ── Peripheral initialisation ─────────────────────────────────────────────
void hardware_init();

// Latch GPIO42 (backlight) LOW across deep sleep. GPIO42 is outside the RTC
// domain, so without gpio_hold_en + gpio_deep_sleep_hold_en it floats and the
// backlight glows faintly in sleep (gotcha #5).
void hardware_configure_wakeup();
