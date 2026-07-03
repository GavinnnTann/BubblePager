#include "battery.h"
#include "hardware/hardware.h"
#include "buttons/buttons.h"
#include <Arduino.h>

// LIR2450 open-circuit voltage → state-of-charge. Li-ion is markedly non-linear:
// a long flat plateau through the mid-range and steep knees at both ends, so a
// straight voltage→percent scale would read wildly wrong. This piecewise table
// (charged 4.20 V → empty ~3.27 V) is linearly interpolated between points.
struct VPoint { uint16_t mv; uint8_t pct; };
static const VPoint CURVE[] = {
    {4200, 100}, {4150, 95}, {4110, 90}, {4080, 85}, {4020, 80}, {3980, 75},
    {3950,  70}, {3910, 65}, {3870, 60}, {3850, 55}, {3840, 50}, {3820, 45},
    {3800,  40}, {3790, 35}, {3770, 30}, {3750, 25}, {3730, 20}, {3710, 15},
    {3690,  10}, {3610,  5}, {3270,  0},
};
static const int CURVE_N = sizeof(CURVE) / sizeof(CURVE[0]);

static uint8_t pct_from_mv(uint16_t mv) {
    if (mv >= CURVE[0].mv)            return 100;
    if (mv <= CURVE[CURVE_N - 1].mv) return 0;
    for (int i = 0; i < CURVE_N - 1; i++) {
        uint16_t hi = CURVE[i].mv, lo = CURVE[i + 1].mv;
        if (mv <= hi && mv >= lo) {
            int span = hi - lo;                       // > 0
            int dp   = CURVE[i].pct - CURVE[i + 1].pct;
            return (uint8_t)(CURVE[i + 1].pct + (long)(mv - lo) * dp / span);
        }
    }
    return 0;
}

// Two-stage smoothing so the displayed % never jumps on a noisy reading or a
// momentary load sag:
//   1. Per-call hardware average of N ADC samples (kills sample noise).
//   2. An exponential moving average across calls (kills slower fluctuations):
//      filtered += (raw - filtered) >> EMA_SHIFT, i.e. α = 1/2^EMA_SHIFT.
//      EMA_SHIFT=3 → α≈0.125, a gentle ~8-reading time constant.
// The EMA runs on VOLTAGE (not percent) so the non-linear curve is applied once,
// to an already-stable input. s_filt_mv = -1 until seeded on the first read.
static constexpr int EMA_SHIFT = 3;
static int32_t s_filt_mv = -1;
static BatteryStatus s_last = { 0, 0, false };   // cached snapshot for battery_last()

void battery_init() {
    analogReadResolution(12);
    analogSetPinAttenuation(BAT_ADC_PIN, ADC_11db);  // full-scale ~3.3 V at the pin (2.1 V at 4.2 V pack)
    s_filt_mv = -1;                                  // re-seed the filter on next read
}

BatteryStatus battery_read() {
    // Stage 1: average a handful of samples to settle ADC noise.
    uint32_t sum = 0;
    const int N = 8;
    for (int i = 0; i < N; i++) sum += analogReadMilliVolts(BAT_ADC_PIN);
    int32_t raw_vbat = (int32_t)(sum / N) * 2;   // two equal 100k resistors → ÷2 divider

    // Stage 2: exponential moving average across calls (seed on first read so the
    // very first display value is correct rather than ramping up from zero).
    if (s_filt_mv < 0) s_filt_mv = raw_vbat;
    else               s_filt_mv += (raw_vbat - s_filt_mv) >> EMA_SHIFT;

    BatteryStatus s;
    s.millivolts = (uint16_t)s_filt_mv;
    s.percent    = pct_from_mv((uint16_t)s_filt_mv);
    // P6 LOW = USB/VIN present (charging); HIGH = running on the cell.
    s.charging   = (buttons_read_port() & (1u << CHARGE_DET_BIT)) == 0;
    s_last = s;
    return s;
}

BatteryStatus battery_last() { return s_last; }
