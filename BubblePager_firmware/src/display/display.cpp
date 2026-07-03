#include "display.h"
#include "hardware/hardware.h"
#include "config.h"
#include <driver/ledc.h>

#include <lvgl.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>             // vTaskDelay
#include <freertos/semphr.h>           // blit-done signalling
#include "draw/sw/lv_draw_sw_utils.h"  // lv_draw_sw_rgb565_swap

// ── Constants ────────────────────────────────────────────────────────────────

static constexpr uint16_t LCD_W       = 240;
static constexpr uint16_t LCD_H       = 240;
// 60 rows per tile (N16R16 chip has ample DRAM; 60×240×2 = 28800 bytes per buffer)
static constexpr uint16_t BUF_LINES   = 60;
// 2 bytes/pixel (RGB565). NOT sizeof(lv_color_t) — that is 3 bytes in LVGL v9.
static constexpr size_t   BUF_BYTES   = LCD_W * BUF_LINES * sizeof(uint16_t);
// GC9A01 I80 write-cycle minimum is 66 ns → max ~15 MHz.
// 10 MHz (100 ns cycle) gives comfortable margin.
static constexpr uint32_t I80_CLK_HZ  = 10000000;
// Full-frame size used for max_transfer_bytes — required when any LVGL animation
// triggers layer compositing (e.g. LV_SCR_LOAD_ANIM_FADE_IN). Even with ANIM_NONE
// this guards against future animation use.
static constexpr size_t   FULL_FRAME_BYTES = LCD_W * LCD_H * sizeof(uint16_t);

// ── State ─────────────────────────────────────────────────────────────────────

static esp_lcd_panel_io_handle_t s_io        = nullptr;
static lv_display_t*             s_disp      = nullptr;
static lv_color_t*               s_buf1      = nullptr;
static lv_color_t*               s_buf2      = nullptr;

// Direct-blit (MJPEG) completion path. When s_blit_active is set the trans-done
// ISR gives s_blit_done instead of signalling LVGL — during PLAYING no LVGL
// flush is in flight, so the same callback serves both cleanly.
static SemaphoreHandle_t         s_blit_done   = nullptr;
static volatile bool             s_blit_active = false;
static volatile bool             s_blit_pending = false;

// ── Helpers ───────────────────────────────────────────────────────────────────

static inline void io_cmd(uint8_t cmd) {
    esp_lcd_panel_io_tx_param(s_io, cmd, nullptr, 0);
}

static inline void io_data(uint8_t cmd, const uint8_t* d, size_t len) {
    esp_lcd_panel_io_tx_param(s_io, cmd, d, len);
}

// ── GC9A01 init sequence ──────────────────────────────────────────────────────
// Derived from Waveshare & community GC9A01 examples (I80 / parallel mode).
// The sequence is identical to SPI; only the physical bus differs.

static void gc9a01_reset() {
    gpio_set_level((gpio_num_t)TFT_RST_PIN, 1);  // start HIGH (released)
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level((gpio_num_t)TFT_RST_PIN, 0);  // assert reset
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level((gpio_num_t)TFT_RST_PIN, 1);  // release reset
    vTaskDelay(pdMS_TO_TICKS(120));
}

static void gc9a01_init_regs() {
    io_cmd(0xEF);
    io_data(0xEB, (const uint8_t[]){0x14}, 1);
    io_cmd(0xFE);
    io_cmd(0xEF);
    io_data(0xEB, (const uint8_t[]){0x14}, 1);
    io_data(0x84, (const uint8_t[]){0x40}, 1);
    io_data(0x85, (const uint8_t[]){0xFF}, 1);
    io_data(0x86, (const uint8_t[]){0xFF}, 1);
    io_data(0x87, (const uint8_t[]){0xFF}, 1);
    io_data(0x88, (const uint8_t[]){0x0A}, 1);
    io_data(0x89, (const uint8_t[]){0x21}, 1);
    io_data(0x8A, (const uint8_t[]){0x00}, 1);
    io_data(0x8B, (const uint8_t[]){0x80}, 1);
    io_data(0x8C, (const uint8_t[]){0x01}, 1);
    io_data(0x8D, (const uint8_t[]){0x01}, 1);
    io_data(0x8E, (const uint8_t[]){0xFF}, 1);
    io_data(0x8F, (const uint8_t[]){0xFF}, 1);
    io_data(0xB6, (const uint8_t[]){0x00, 0x20}, 2);
    // MADCTL 0xC8 = MY|MX|BGR.
    //   BGR (0x08): panel is wired BGR — without this red/blue are swapped, which
    //               made the blue code render amber and the arc run amber→blue→blue
    //               instead of blue→amber→red.
    //   MY|MX (0xC0): rotate the image 180° (display physically mounted upside down).
    io_data(0x36, (const uint8_t[]){0xC8}, 1); // MADCTL — 180° flip + BGR color order
    io_data(0x3A, (const uint8_t[]){0x05}, 1); // COLMOD — RGB565
    io_data(0x90, (const uint8_t[]){0x08, 0x08, 0x08, 0x08}, 4);
    io_data(0xBD, (const uint8_t[]){0x06}, 1);
    io_data(0xBC, (const uint8_t[]){0x00}, 1);
    io_data(0xFF, (const uint8_t[]){0x60, 0x01, 0x04}, 3);
    io_data(0xC3, (const uint8_t[]){0x13}, 1);
    io_data(0xC4, (const uint8_t[]){0x13}, 1);
    io_data(0xC9, (const uint8_t[]){0x22}, 1);
    io_data(0xBE, (const uint8_t[]){0x11}, 1);
    io_data(0xE1, (const uint8_t[]){0x10, 0x0E}, 2);
    io_data(0xDF, (const uint8_t[]){0x21, 0x0C, 0x02}, 3);
    io_data(0xF0, (const uint8_t[]){0x45, 0x09, 0x08, 0x08, 0x26, 0x2A}, 6);
    io_data(0xF1, (const uint8_t[]){0x43, 0x70, 0x72, 0x36, 0x37, 0x6F}, 6);
    io_data(0xF2, (const uint8_t[]){0x45, 0x09, 0x08, 0x08, 0x26, 0x2A}, 6);
    io_data(0xF3, (const uint8_t[]){0x43, 0x70, 0x72, 0x36, 0x37, 0x6F}, 6);
    io_data(0xED, (const uint8_t[]){0x1B, 0x0B}, 2);
    io_data(0xAE, (const uint8_t[]){0x77}, 1);
    io_data(0xCD, (const uint8_t[]){0x63}, 1);
    io_data(0x70, (const uint8_t[]){0x07, 0x07, 0x04, 0x0E, 0x0F, 0x09, 0x07, 0x08, 0x03}, 9);
    io_data(0xE8, (const uint8_t[]){0x34}, 1);
    io_data(0x62, (const uint8_t[]){0x18, 0x0D, 0x71, 0xED, 0x70, 0x70,
                                    0x18, 0x0F, 0x71, 0xEF, 0x70, 0x70}, 12);
    io_data(0x63, (const uint8_t[]){0x18, 0x11, 0x71, 0xF1, 0x70, 0x70,
                                    0x18, 0x13, 0x71, 0xF3, 0x70, 0x70}, 12);
    io_data(0x64, (const uint8_t[]){0x28, 0x29, 0xF1, 0x01, 0xF1, 0x00, 0x07}, 7);
    io_data(0x66, (const uint8_t[]){0x3C, 0x00, 0xCD, 0x67, 0x45, 0x45, 0x10, 0x00, 0x00, 0x00}, 10);
    io_data(0x67, (const uint8_t[]){0x00, 0x3C, 0x00, 0x00, 0x00, 0x01, 0x54, 0x10, 0x32, 0x98}, 10);
    io_data(0x74, (const uint8_t[]){0x10, 0x85, 0x80, 0x00, 0x00, 0x4E, 0x00}, 7);
    io_data(0x98, (const uint8_t[]){0x3E, 0x07}, 2);
    io_cmd(0x35);   // tearing effect line ON
    io_cmd(0x21);   // display inversion ON (required for correct GC9A01 colors)

    io_cmd(0x11);   // sleep out
    vTaskDelay(pdMS_TO_TICKS(120));

    io_cmd(0x29);   // display on
    vTaskDelay(pdMS_TO_TICKS(20));
}

// ── Flush ISR + callback ──────────────────────────────────────────────────────
// Pattern from Espressif's official i80_controller / esp_lvgl_port examples:
//   flush_cb            → swap bytes, send CASET/RASET, kick async RAMWR, RETURN
//   on_color_trans_done → calls lv_display_flush_ready() directly from the ISR
//
// The esp_lcd i80 driver invokes on_color_trans_done ONLY when a tx_color (RAMWR)
// transaction completes — NOT for tx_param (CASET/RASET). So it fires exactly once
// per flush and the buffer is never reused before the DMA finishes.
//
// Earlier this code blocked in flush_cb on a binary semaphore taken with
// portMAX_DELAY. Any single missed/late give from the ISR (queue pressure, a
// transaction that errors before the callback runs) wedges flush_cb forever — the
// loop stops calling lv_timer_handler and the panel freezes mid-frame: the tiles
// already DMA'd stay correct while the rest of GRAM keeps its power-on noise. That
// is the classic "frozen with a glitch band across the middle" failure. Signalling
// flush_ready directly from the ISR removes that deadlock class entirely and is the
// canonical approach; on a dropped frame LVGL simply re-flushes next cycle.
//
// Software byte-swap (lv_draw_sw_rgb565_swap) replaces hardware swap_color_bytes=1
// to match Espressif's reference — hardware swap interacts badly with certain
// tx_param/tx_color sequencing in ESP-IDF v4.4.

// Fires once per tx_color (RAMWR) completion. Signal LVGL directly from the ISR —
// this is the canonical Espressif esp_lcd + LVGL i80 pattern. on_color_trans_done
// is wired by the driver to fire ONLY for color transactions, not tx_param, so it
// fires exactly once per flush by construction. No blocking semaphore is needed,
// which removes the off-by-one deadlock class that leaves a half-painted frame
// on screen (correct tiles + unwritten GRAM noise bands) when a single callback
// is missed or an oversized transfer errors out before the callback runs.
static bool IRAM_ATTR on_color_trans_done(esp_lcd_panel_io_handle_t,
                                           esp_lcd_panel_io_event_data_t*,
                                           void*) {
    // s_disp is a file-static set during display_init() and is valid by the time
    // any flush (and therefore this callback) can run.
    if (s_blit_active) {
        // Direct MJPEG blit completed — wake display_blit_wait().
        BaseType_t hp_task_woken = pdFALSE;
        if (s_blit_done) xSemaphoreGiveFromISR(s_blit_done, &hp_task_woken);
        return hp_task_woken == pdTRUE;
    }
    if (s_disp) lv_display_flush_ready(s_disp);
    return false;
}

static void lvgl_flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
    uint32_t w = (uint32_t)(area->x2 - area->x1 + 1);
    uint32_t h = (uint32_t)(area->y2 - area->y1 + 1);

    // Swap RGB565 bytes in-place before sending to display (GC9A01 is big-endian).
    // This matches the official Espressif I80 example pattern.
    lv_draw_sw_rgb565_swap(px_map, w * h);

    // Column address set (CASET 0x2A)
    const uint8_t col[4] = {
        (uint8_t)(area->x1 >> 8), (uint8_t)(area->x1 & 0xFF),
        (uint8_t)(area->x2 >> 8), (uint8_t)(area->x2 & 0xFF)
    };
    esp_lcd_panel_io_tx_param(s_io, 0x2A, col, 4);

    // Row address set (RASET 0x2B)
    const uint8_t row[4] = {
        (uint8_t)(area->y1 >> 8), (uint8_t)(area->y1 & 0xFF),
        (uint8_t)(area->y2 >> 8), (uint8_t)(area->y2 & 0xFF)
    };
    esp_lcd_panel_io_tx_param(s_io, 0x2B, row, 4);

    // Memory write (RAMWR 0x2C) — async DMA. on_color_trans_done calls
    // lv_display_flush_ready() from the ISR when the DMA completes; do NOT block
    // here and do NOT call flush_ready here.
    // 2 bytes/pixel (RGB565), NOT sizeof(lv_color_t): in LVGL v9 lv_color_t is a
    // 24-bit struct (3 bytes), but the RGB565 frame buffer is 2 bytes/pixel. Using
    // sizeof(lv_color_t) streamed 1.5x the data into the CASET/RASET window, which
    // wrapped and produced byte-misaligned noise bands while the loop ran normally.
    esp_lcd_panel_io_tx_color(s_io, 0x2C, px_map, w * h * sizeof(uint16_t));
}

// ── Public API ────────────────────────────────────────────────────────────────

bool display_init() {
    // Hardware reset
    gpio_config_t rst_cfg = {
        .pin_bit_mask = (1ULL << TFT_RST_PIN),
        .mode         = GPIO_MODE_OUTPUT,
    };
    gpio_config(&rst_cfg);
    gc9a01_reset();

    // Release the deep-sleep pad hold before ledc takes over. Both gpio_hold_en and
    // gpio_deep_sleep_hold_en are set in hardware_configure_wakeup(); gpio_hold_dis
    // clears the per-pin latch. gpio_deep_sleep_hold_dis() clears the global enable.
    gpio_deep_sleep_hold_dis();
    gpio_hold_dis((gpio_num_t)TFT_BL_PIN);
    gpio_reset_pin((gpio_num_t)TFT_BL_PIN);
    gpio_set_direction((gpio_num_t)TFT_BL_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)TFT_BL_PIN, 0);
    ledcSetup(0, 5000, 8);
    ledcAttachPin(TFT_BL_PIN, 0);
    ledcWrite(0, 0);   // off during init

    // ── I80 bus ───────────────────────────────────────────────────────────────
    esp_lcd_i80_bus_handle_t bus = nullptr;
    esp_lcd_i80_bus_config_t bus_cfg = {
        .dc_gpio_num      = TFT_RS_PIN,
        .wr_gpio_num      = TFT_WR_PIN,
        .clk_src          = LCD_CLK_SRC_PLL160M,
        .data_gpio_nums   = {
            TFT_D0_PIN, TFT_D1_PIN, TFT_D2_PIN, TFT_D3_PIN,
            TFT_D4_PIN, TFT_D5_PIN, TFT_D6_PIN, TFT_D7_PIN,
        },
        .bus_width        = 8,
        // Must be >= largest single DMA transfer. LV_SCR_LOAD_ANIM_FADE_IN uses
        // LVGL layer compositing and attempts a full-frame flush (115200 bytes);
        // using ANIM_NONE avoids that, but set the max to the full frame for safety.
        .max_transfer_bytes = FULL_FRAME_BYTES,
    };
    if (esp_lcd_new_i80_bus(&bus_cfg, &bus) != ESP_OK) {
#ifdef DEBUG_SERIAL
        Serial.println("[DISP] esp_lcd_new_i80_bus FAILED");
#endif
        return false;
    }

    // ── Panel IO ──────────────────────────────────────────────────────────────
    esp_lcd_panel_io_i80_config_t io_cfg = {
        .cs_gpio_num        = TFT_CS_PIN,
        .pclk_hz            = I80_CLK_HZ,
        .trans_queue_depth  = 10,
        .on_color_trans_done = on_color_trans_done,
        .user_ctx           = nullptr,
        .lcd_cmd_bits       = 8,
        .lcd_param_bits     = 8,
        .dc_levels = {
            .dc_idle_level  = 0,
            .dc_cmd_level   = 0,
            .dc_dummy_level = 0,
            .dc_data_level  = 1,
        },
        // swap_color_bytes removed — byte swap is done in software via
        // lv_draw_sw_rgb565_swap() in the flush callback (official Espressif pattern)
        .flags = { 0 },
    };
    if (esp_lcd_new_panel_io_i80(bus, &io_cfg, &s_io) != ESP_OK) {
#ifdef DEBUG_SERIAL
        Serial.println("[DISP] esp_lcd_new_panel_io_i80 FAILED");
#endif
        return false;
    }

    // ── GC9A01 register init ─────────────────────────────────────────────────
    gc9a01_init_regs();

    // Binary semaphore for direct-blit DMA completion (MJPEG playback).
    s_blit_done = xSemaphoreCreateBinary();

    // ── LVGL init ─────────────────────────────────────────────────────────────
    lv_init();

    // Tick source. LV_TICK_CUSTOM was REMOVED in LVGL v9 — the platformio.ini
    // LV_TICK_CUSTOM_* flags are silently inert, so LVGL's clock stayed frozen at 0
    // and its display-refresh timer never fired (one frame drawn, then never again
    // despite labels changing). v9 requires registering the tick getter at runtime.
    lv_tick_set_cb([]() -> uint32_t { return (uint32_t)millis(); });

    // Allocate draw buffers — two buffers of BUF_LINES rows each
    s_buf1 = (lv_color_t*)heap_caps_malloc(BUF_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    s_buf2 = (lv_color_t*)heap_caps_malloc(BUF_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!s_buf1 || !s_buf2) {
#ifdef DEBUG_SERIAL
        Serial.println("[DISP] draw buffer alloc FAILED");
#endif
        return false;
    }

    s_disp = lv_display_create(LCD_W, LCD_H);
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(s_disp, lvgl_flush_cb);
    lv_display_set_buffers(s_disp, s_buf1, s_buf2, BUF_BYTES,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);

#ifdef DEBUG_SERIAL
    Serial.println("[DISP] init OK");
#endif
    return true;
}

void display_tick() {
    // Tick source is registered in display_init() via lv_tick_set_cb(millis),
    // so lv_tick_inc() is not needed here — just service LVGL's timers.
    lv_timer_handler();
}

void display_on()  { ledcWrite(0, BACKLIGHT_DUTY); }
void display_off() { ledcWrite(0, 0); }
void display_set_brightness(uint8_t duty) { ledcWrite(0, duty); }

void display_sleep() {
    if (s_io) {
        io_cmd(0x10);  // GC9A01 SLPIN — shuts down panel regulators, kills faint glow
        vTaskDelay(pdMS_TO_TICKS(5));  // datasheet: 5 ms before power-down
    }
    ledcWrite(0, 0);
}

lv_display_t* display_get() {
    return s_disp;
}

void display_blit_wait() {
    if (!s_blit_pending) return;
    // Wait for the trans-done ISR to release the semaphore. Timeout guards against
    // a lost interrupt so playback can't wedge forever (matches the flush_cb no-
    // deadlock rule); on timeout we simply proceed and let the next blit overwrite.
    xSemaphoreTake(s_blit_done, pdMS_TO_TICKS(200));
    s_blit_pending = false;
    s_blit_active  = false;
}

void display_swap_rgb565(uint16_t* pixels, uint32_t count) {
    lv_draw_sw_rgb565_swap(pixels, count);
}

bool display_blit(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t* pixels) {
    if (!s_io || !pixels || w <= 0 || h <= 0) return false;

    // Serialise with the previous blit's DMA before touching the panel window.
    display_blit_wait();

    // GC9A01 is big-endian; swap RGB565 bytes in place (same as the flush cb).
    lv_draw_sw_rgb565_swap(pixels, (uint32_t)w * h);

    const int16_t x2 = x + w - 1;
    const int16_t y2 = y + h - 1;
    const uint8_t col[4] = {
        (uint8_t)(x >> 8),  (uint8_t)(x & 0xFF),
        (uint8_t)(x2 >> 8), (uint8_t)(x2 & 0xFF)
    };
    esp_lcd_panel_io_tx_param(s_io, 0x2A, col, 4);   // CASET
    const uint8_t row[4] = {
        (uint8_t)(y >> 8),  (uint8_t)(y & 0xFF),
        (uint8_t)(y2 >> 8), (uint8_t)(y2 & 0xFF)
    };
    esp_lcd_panel_io_tx_param(s_io, 0x2B, row, 4);   // RASET

    // Async RAMWR — on_color_trans_done gives s_blit_done when the DMA finishes.
    s_blit_active  = true;
    s_blit_pending = true;
    esp_lcd_panel_io_tx_color(s_io, 0x2C, pixels, (size_t)w * h * sizeof(uint16_t));
    return true;
}

void display_set_rotation(int degrees) {
    if (!s_disp || !s_io) return;
    // Drive rotation via MADCTL (0x36) rather than LVGL software rotation.
    // The init sequence sets 0xC8 (MY|MX|BGR = 180° flip + BGR) to compensate
    // for the panel being physically mounted upside-down on the PCB.
    // Stacking LVGL software-180° on top of hardware-180° = 360° = no visible change,
    // so we toggle the hardware register instead and keep LVGL at 0°.
    //
    //  degrees==0   (normal, face-up):  0xC8 — hardware 180°, panel mounts upside-down
    //  degrees==180 (device flipped):   0x08 — BGR only, hardware 0° cancels mount flip
    const uint8_t madctl = (degrees == 0) ? 0xC8 : 0x08;
    io_data(0x36, &madctl, 1);
    lv_obj_invalidate(lv_screen_active());  // schedule full LVGL redraw
}
