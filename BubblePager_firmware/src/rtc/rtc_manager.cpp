#include "rtc_manager.h"
#include "hardware/hardware.h"
#include <Wire.h>
#include <sys/time.h>

static constexpr uint8_t PCF85063_ADDR = 0x51;

// PCF85063 register map (NOT the PCF8563's, which shares I²C address 0x51 but
// puts time at 0x02 — using that map here corrupts the Offset/RAM registers
// and reads shifted garbage):
//   0x00 Control_1 (bit5 = STOP)   0x01 Control_2   0x02 Offset   0x03 RAM
//   0x04 Seconds (bit7 = OS flag)  0x05 Minutes     0x06 Hours
//   0x07 Days    0x08 Weekdays    0x09 Months      0x0A Years
static constexpr uint8_t REG_CTRL1   = 0x00;
static constexpr uint8_t REG_OFFSET  = 0x02;
static constexpr uint8_t REG_SECONDS = 0x04;

static bool s_valid = false;

static uint8_t bcd2dec(uint8_t b) { return (b >> 4) * 10 + (b & 0x0F); }
static uint8_t dec2bcd(uint8_t d) { return ((d / 10) << 4) | (d % 10); }

bool rtc_init() {
    Wire.begin(RTC_SDA_PIN, RTC_SCL_PIN);

    // Read seconds register — bit 7 is the oscillator-stop (OS) flag.
    // OS=1 means the RTC lost power and time is invalid.
    Wire.beginTransmission(PCF85063_ADDR);
    Wire.write(REG_SECONDS);
    if (Wire.endTransmission(false) != 0) return false;  // chip not responding

    Wire.requestFrom(PCF85063_ADDR, (uint8_t)1);
    if (!Wire.available()) return false;

    s_valid = !(Wire.read() & 0x80);
    return s_valid;
}

bool rtc_is_valid() { return s_valid; }

time_t rtc_read() {
    if (!s_valid) return 0;

    Wire.beginTransmission(PCF85063_ADDR);
    Wire.write(REG_SECONDS);  // start at seconds register (0x04)
    Wire.endTransmission(false);
    Wire.requestFrom(PCF85063_ADDR, (uint8_t)7);
    if (Wire.available() < 7) return 0;

    uint8_t sec  = bcd2dec(Wire.read() & 0x7F);  // mask OS flag
    uint8_t min  = bcd2dec(Wire.read() & 0x7F);
    uint8_t hour = bcd2dec(Wire.read() & 0x3F);
    uint8_t day  = bcd2dec(Wire.read() & 0x3F);
    Wire.read();                                  // weekday — not used
    uint8_t mon  = bcd2dec(Wire.read() & 0x1F);
    uint8_t year = bcd2dec(Wire.read());          // 00–99, offset from 2000

    struct tm t = {};
    t.tm_sec   = sec;
    t.tm_min   = min;
    t.tm_hour  = hour;
    t.tm_mday  = day;
    t.tm_mon   = mon - 1;       // tm_mon is 0-based
    t.tm_year  = 100 + year;    // years since 1900; 2024 → 124
    t.tm_isdst = 0;

    // mktime() interprets as local time — use timegm equivalent via UTC offset trick
    // ESP-IDF doesn't ship timegm, so adjust manually: set TZ=UTC, call mktime
    setenv("TZ", "UTC0", 1);
    tzset();
    time_t result = mktime(&t);
    // Restore TZ if needed (device always uses UTC so this is a no-op in practice)
    return result;
}

void rtc_write(time_t t) {
    struct tm* tm = gmtime(&t);

    // Halt oscillator before writing to prevent partial-update corruption
    Wire.beginTransmission(PCF85063_ADDR);
    Wire.write(REG_CTRL1);
    Wire.write(0x20);   // STOP bit (bit 5)
    Wire.endTransmission();

    // Clear the Offset calibration register. Earlier firmware (PCF8563-style
    // register map) wrote BCD seconds here, detuning the oscillator by up to
    // ~±240 ppm — reset it to factory-neutral.
    Wire.beginTransmission(PCF85063_ADDR);
    Wire.write(REG_OFFSET);
    Wire.write(0x00);
    Wire.endTransmission();

    // Write 7 time registers starting at Seconds (0x04).
    // Writing seconds with bit 7 = 0 also clears the OS flag.
    Wire.beginTransmission(PCF85063_ADDR);
    Wire.write(REG_SECONDS);
    Wire.write(dec2bcd((uint8_t)tm->tm_sec));
    Wire.write(dec2bcd((uint8_t)tm->tm_min));
    Wire.write(dec2bcd((uint8_t)tm->tm_hour));
    Wire.write(dec2bcd((uint8_t)tm->tm_mday));
    Wire.write((uint8_t)tm->tm_wday);
    Wire.write(dec2bcd((uint8_t)(tm->tm_mon + 1)));   // 1-based month
    Wire.write(dec2bcd((uint8_t)(tm->tm_year - 100))); // offset from 2000
    Wire.endTransmission();

    // Restart oscillator
    Wire.beginTransmission(PCF85063_ADDR);
    Wire.write(REG_CTRL1);
    Wire.write(0x00);
    Wire.endTransmission();

    s_valid = true;
}
