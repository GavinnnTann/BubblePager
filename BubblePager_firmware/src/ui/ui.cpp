#include "ui.h"
#include "config.h"
#include "net/mjpeg_player.h"   // mjpeg_thumb_buffer()
#include "display/display.h"    // display_blit() for the freeze-frame
#include <Arduino.h>
#include <lvgl.h>
#include <time.h>
#include <string.h>
#include <math.h>

// ── Screen handles ──────────────────────────────────────────────────────────
static lv_obj_t* s_idle_scr     = nullptr;
static lv_obj_t* s_incoming_scr = nullptr;
static lv_obj_t* s_black_scr     = nullptr;   // shared by PLAYING / DONE

// Idle widgets
static lv_obj_t* s_time_lbl    = nullptr;
// The breathing circle is dual-purpose (no separate WiFi icon / unread pill):
// its COLOUR is the link/server status (blue=connecting, red=failed, green=ok),
// and an unread COUNT overlays on top of it when there's pager-style unread mail.
static lv_obj_t* s_breathe      = nullptr;  // gently breathing circle below the clock
static lv_obj_t* s_unread_num   = nullptr;  // count overlaid on the breathing circle
static lv_obj_t* s_lastcap_lbl  = nullptr;  // "Last message from" caption (bottom)
static lv_obj_t* s_lastname_lbl = nullptr;  // sender name, on its own line below

// Incoming widgets
static lv_obj_t* s_pulse_ring = nullptr;
static lv_obj_t* s_sender_lbl = nullptr;
static lv_obj_t* s_queue_lbl  = nullptr;   // "+N waiting" badge

// History (freeze-frame): the thumbnail is DIRECT-BLIT to the panel (same proven
// path as video — LVGL's image/canvas widget mis-rendered the PSRAM RGB565
// buffer when used as an on-screen object). Text overlays (index badge, metadata)
// are rendered with the REAL LVGL font via an off-tree lv_canvas + lv_draw_label,
// then composited into the frame buffer pixel-by-pixel before the blit — this
// gives proper anti-aliased text while keeping the proven direct-blit image path.
static lv_obj_t*  s_text_canvas = nullptr;
static uint16_t*  s_text_buf    = nullptr;
static constexpr int TC_W = 200, TC_H = 60;   // scratch canvas size (reused for any label)

// Soft translucent rounded-rect panel (a dark "card" the photo shows faintly
// through), replacing a hard-edged solid-black box. Corners are clipped by
// distance-to-corner-centre so they read as rounded, not chopped square.
static void fb_panel(uint16_t* buf, int x, int y, int w, int h, int radius, uint8_t alpha) {
    for (int py = y; py < y + h; py++) {
        if (py < 0 || py >= 240) continue;
        for (int px = x; px < x + w; px++) {
            if (px < 0 || px >= 240) continue;
            int cx = -1, cy = -1;
            if      (px < x + radius       && py < y + radius)       { cx = x + radius;         cy = y + radius; }
            else if (px >= x + w - radius  && py < y + radius)       { cx = x + w - radius - 1;  cy = y + radius; }
            else if (px < x + radius       && py >= y + h - radius)  { cx = x + radius;          cy = y + h - radius - 1; }
            else if (px >= x + w - radius  && py >= y + h - radius)  { cx = x + w - radius - 1;  cy = y + h - radius - 1; }
            if (cx >= 0) {
                int dx = px - cx, dy = py - cy;
                if (dx * dx + dy * dy > radius * radius) continue;   // outside the rounded corner
            }
            uint16_t bg = buf[py * 240 + px];
            int r = (bg >> 11) & 0x1F, g = (bg >> 5) & 0x3F, b = bg & 0x1F;
            r -= (r * alpha) >> 8; g -= (g * alpha) >> 8; b -= (b * alpha) >> 8;   // darken toward black
            buf[py * 240 + px] = (uint16_t)((r << 11) | (g << 5) | b);
        }
    }
}

static void ensure_text_canvas() {
    if (s_text_canvas) return;
    s_text_buf = (uint16_t*)malloc(TC_W * TC_H * 2);
    // Off-tree: created with a parent so LVGL accepts it, but never added to a
    // visible screen — we only use it as a manual draw target via init_layer.
    s_text_canvas = lv_canvas_create(lv_screen_active());
    lv_canvas_set_buffer(s_text_canvas, s_text_buf, TC_W, TC_H, LV_COLOR_FORMAT_RGB565);
    lv_obj_add_flag(s_text_canvas, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_text_canvas, LV_OBJ_FLAG_IGNORE_LAYOUT);
}

// Render `text` onto the scratch canvas, then paint ONLY the glyph pixels (skip
// near-black background) onto `dst`, centred at (cx, cy). Painting glyphs only —
// not a copied box — is what gives soft/anti-aliased edges instead of a hard
// rectangle: callers draw their own panel (fb_panel) first and the letters sit on
// top of it, blending with whatever is already there.
static void composite_label(uint16_t* dst, const char* text, const lv_font_t* font, int cx, int cy) {
    ensure_text_canvas();
    lv_canvas_fill_bg(s_text_canvas, lv_color_hex(0x000000), LV_OPA_COVER);

    lv_draw_label_dsc_t dsc;
    lv_draw_label_dsc_init(&dsc);
    dsc.color = lv_color_white();
    dsc.font  = font;
    dsc.text  = text;
    dsc.align = LV_TEXT_ALIGN_CENTER;

    lv_point_t sz;
    lv_text_get_size(&sz, text, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    int w = sz.x, h = sz.y;
    if (w > TC_W) w = TC_W;
    if (h > TC_H) h = TC_H;

    lv_layer_t layer;
    lv_canvas_init_layer(s_text_canvas, &layer);
    lv_area_t coords = { 0, 0, TC_W - 1, TC_H - 1 };
    lv_draw_label(&layer, &dsc, &coords);
    lv_canvas_finish_layer(s_text_canvas, &layer);

    // lv_draw_label with LV_TEXT_ALIGN_CENTER over the full TC_W-wide area centres
    // the glyphs around TC_W/2 — read back from there, not from x=0.
    int src_x0 = (TC_W - w) / 2;
    int dst_x0 = cx - w / 2;
    int dst_y0 = cy - h / 2;
    for (int y = 0; y < h; y++) {
        int dy = dst_y0 + y; if (dy < 0 || dy >= 240) continue;
        for (int x = 0; x < w; x++) {
            int dx = dst_x0 + x; if (dx < 0 || dx >= 240) continue;
            uint16_t px = s_text_buf[y * TC_W + (src_x0 + x)];
            if (px != 0x0000) dst[dy * 240 + dx] = px;   // skip background, keep the panel/photo
        }
    }
}

// Blit the (native RGB565) frame buffer to the panel, then undo display_blit()'s
// in-place byte swap so `buf` stays in a consistent state if it's redrawn again
// (e.g. the metadata overlay re-annotates + re-blits the same freeze-frame).
static void blit_frame(uint16_t* buf) {
    display_blit(0, 0, 240, 240, buf);
    display_blit_wait();
    display_swap_rgb565(buf, 240 * 240);
}

// This toolchain's newlib has no timegm(); the standard workaround is to swap TZ
// to UTC for a single mktime() call (which otherwise interprets tm as local time)
// and restore it immediately after.
static time_t utc_mktime(struct tm* tmv) {
    char saved[32] = {};
    const char* old = getenv("TZ");
    if (old) strncpy(saved, old, sizeof(saved) - 1);
    setenv("TZ", "UTC0", 1);
    tzset();
    time_t t = mktime(tmv);
    if (old) setenv("TZ", saved, 1); else unsetenv("TZ");
    tzset();
    return t;
}

// Convert a Postgres/PostgREST UTC ISO timestamp ("2024-06-01T12:34:56.789+00:00")
// to the device's local (Singapore, SGT-8 — see time_sync_init) time, formatted
// "DD Mon, HH:MM". Falls back to an em dash on parse failure.
static void format_local_time(const char* iso_utc, char* out, size_t outlen) {
    int Y, Mo, D, H, Mi, S = 0;
    if (!iso_utc || sscanf(iso_utc, "%d-%d-%dT%d:%d:%d", &Y, &Mo, &D, &H, &Mi, &S) < 5) {
        strncpy(out, "\xe2\x80\x94", outlen - 1); out[outlen - 1] = 0;
        return;
    }
    struct tm tmv = {};
    tmv.tm_year = Y - 1900; tmv.tm_mon = Mo - 1; tmv.tm_mday = D;
    tmv.tm_hour = H; tmv.tm_min = Mi; tmv.tm_sec = S;
    time_t utc = utc_mktime(&tmv);      // parsed fields are UTC
    struct tm local;
    localtime_r(&utc, &local);          // applies the device TZ (SGT-8)
    strftime(out, outlen, "%d %b, %H:%M", &local);
}

static int  s_batt_pct   = 100;
static bool s_charging   = false;

enum class Screen : uint8_t { NONE, IDLE, INCOMING, HISTORY, PLAYING, DONE };
static Screen s_active = Screen::NONE;

// ── Idle screen ─────────────────────────────────────────────────────────────
// The breathing circle: animate SIZE only (not transform_scale / opacity, which
// force LVGL's layer-compositing path — see the incoming-ring note below).
static void breathe_cb(void* obj, int32_t v) {
    lv_obj_t* o = (lv_obj_t*)obj;
    lv_obj_set_size(o, v, v);
    lv_obj_align(o, LV_ALIGN_CENTER, 0, 29);   // hold position as it grows/shrinks
}

static void build_idle() {
    s_idle_scr = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(s_idle_scr, lv_color_black(), 0);
    lv_obj_remove_flag(s_idle_scr, LV_OBJ_FLAG_SCROLLABLE);

    // Large centred clock.
    s_time_lbl = lv_label_create(s_idle_scr);
    lv_obj_set_style_text_font(s_time_lbl, &lv_font_montserrat_44, 0);
    lv_obj_set_style_text_color(s_time_lbl, lv_color_white(), 0);
    lv_label_set_text(s_time_lbl, "--:--");
    lv_obj_align(s_time_lbl, LV_ALIGN_CENTER, 0, -26);

    // Gently breathing circle below the clock — doubles as the WiFi/server status
    // indicator (colour) and the unread-count badge (see ui_set_unread below).
    s_breathe = lv_obj_create(s_idle_scr);
    lv_obj_set_size(s_breathe, 26, 26);
    lv_obj_align(s_breathe, LV_ALIGN_CENTER, 0, 29);
    lv_obj_set_style_radius(s_breathe, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_breathe, 0, 0);
    lv_obj_set_style_bg_color(s_breathe, lv_color_hex(0x552222), 0);   // starts "no link" red
    lv_obj_clear_flag(s_breathe, LV_OBJ_FLAG_SCROLLABLE);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_breathe);
    lv_anim_set_exec_cb(&a, breathe_cb);
    lv_anim_set_values(&a, 24, 42);             // gentle grow
    lv_anim_set_duration(&a, 1600);
    lv_anim_set_playback_duration(&a, 1600);    // …and shrink back
    lv_anim_set_repeat_delay(&a, 1400);         // pause between breaths (~every few s)
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);

    // Unread count, overlaid on the SAME fixed point as the breathing circle (a
    // sibling, not a child, so its own size/position never pulses) — created
    // after the circle so it z-orders on top. Empty text = invisible, no badge.
    s_unread_num = lv_label_create(s_idle_scr);
    lv_obj_set_style_text_font(s_unread_num, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(s_unread_num, lv_color_black(), 0);
    lv_label_set_text(s_unread_num, "");
    lv_obj_align(s_unread_num, LV_ALIGN_CENTER, 0, 29);

    // "Last message from" (dim caption) with the sender name on its own line below.
    s_lastcap_lbl = lv_label_create(s_idle_scr);
    lv_obj_set_style_text_font(s_lastcap_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_lastcap_lbl, lv_color_hex(0x777777), 0);
    lv_label_set_text(s_lastcap_lbl, "");
    lv_obj_align(s_lastcap_lbl, LV_ALIGN_BOTTOM_MID, 0, -46);

    s_lastname_lbl = lv_label_create(s_idle_scr);
    lv_obj_set_style_text_font(s_lastname_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_lastname_lbl, lv_color_hex(0xdddddd), 0);
    lv_label_set_long_mode(s_lastname_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_lastname_lbl, 200);
    lv_obj_set_style_text_align(s_lastname_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_lastname_lbl, "");
    lv_obj_align(s_lastname_lbl, LV_ALIGN_BOTTOM_MID, 0, -24);
}

// ── Incoming screen (pulsing ring) ──────────────────────────────────────────
// Animate the ring's SIZE (a normal redraw) rather than transform_scale/opacity.
// transform_scale and opa<255 both force LVGL v9 to render the object through its
// layer-compositing path, which allocates a large transformed layer buffer every
// frame from internal RAM — the heaviest/most fragile path on this display and a
// hard-hang suspect. Growing the object via lv_obj_set_size just invalidates the
// old+new area and repaints, no layer buffer involved.
static void pulse_size_cb(void* obj, int32_t v) {
    lv_obj_t* o = (lv_obj_t*)obj;
    lv_obj_set_size(o, v, v);
    lv_obj_center(o);   // keep it centred as it grows
}

static void build_incoming() {
    s_incoming_scr = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(s_incoming_scr, lv_color_black(), 0);

    s_pulse_ring = lv_obj_create(s_incoming_scr);
    lv_obj_set_size(s_pulse_ring, 120, 120);
    lv_obj_center(s_pulse_ring);
    lv_obj_set_style_radius(s_pulse_ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_pulse_ring, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_pulse_ring, 5, 0);
    lv_obj_set_style_border_color(s_pulse_ring, lv_color_white(), 0);
    lv_obj_clear_flag(s_pulse_ring, LV_OBJ_FLAG_SCROLLABLE);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_pulse_ring);
    lv_anim_set_exec_cb(&a, pulse_size_cb);
    lv_anim_set_values(&a, 120, 224);   // grow within the 240 panel (no clipping)
    lv_anim_set_time(&a, 1100);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);

    s_sender_lbl = lv_label_create(s_incoming_scr);
    lv_obj_set_style_text_font(s_sender_lbl, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(s_sender_lbl, lv_color_white(), 0);
    lv_label_set_text(s_sender_lbl, "Incoming");
    lv_obj_center(s_sender_lbl);   // dead centre — inside the hollow pulsing ring

    // "+N waiting" badge (more videos queued behind this one).
    s_queue_lbl = lv_label_create(s_incoming_scr);
    lv_obj_set_style_text_font(s_queue_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_queue_lbl, lv_color_hex(0x2a5cff), 0);
    lv_label_set_text(s_queue_lbl, "");
    lv_obj_align(s_queue_lbl, LV_ALIGN_TOP_MID, 0, 30);
}

void ui_set_queue_count(int n) {
    if (!s_queue_lbl) return;
    if (n > 0) {
        char buf[24];
        snprintf(buf, sizeof(buf), "+%d waiting", n);
        lv_label_set_text(s_queue_lbl, buf);
    } else {
        lv_label_set_text(s_queue_lbl, "");
    }
}

// ── History screen (freeze-frame browsing) ──────────────────────────────────
// A black LVGL screen is loaded as a static backdrop; the thumbnail + index are
// direct-blit over it (LVGL won't repaint unless something invalidates the black
// screen). Metadata is composited into the frame too.
static void build_history() { /* no LVGL widgets — direct-blit; uses s_black_scr */ }

// Draw the "index/total" badge into the frame buffer — a tight, soft rounded pill
// sized to the text (no leftover descender padding at the bottom).
static void draw_index_badge(uint16_t* buf, int index, int total) {
    char b[16];
    snprintf(b, sizeof(b), "%d/%d", index, total);

    lv_point_t sz;
    lv_text_get_size(&sz, b, &lv_font_montserrat_16, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    int pad_x = 10, pad_y = 5;
    int w = sz.x + pad_x * 2, h = sz.y + pad_y * 2;
    int x = 120 - w / 2, y = 12;

    fb_panel(buf, x, y, w, h, h / 2, 150);          // soft rounded pill, ~59% dark
    composite_label(buf, b, &lv_font_montserrat_16, 120, y + h / 2);
}

void ui_show_history(int index, int total) {
    // Check LVGL's actual active screen, not just our Screen enum: ui_show_text()
    // also sets s_active=HISTORY (same high-level browsing state), so a text→video
    // step within HISTORY must still (re)load the black backdrop before direct-
    // blitting — otherwise the panel shows a video frame while LVGL's scene graph
    // still thinks s_text_scr is active, and a later LVGL repaint would clobber it.
    if (lv_screen_active() != s_black_scr) {
        lv_screen_load(s_black_scr);
        lv_refr_now(nullptr);
    }
    s_active = Screen::HISTORY;
    uint16_t* buf = mjpeg_thumb_buffer();
    if (buf) {
        draw_index_badge(buf, index, total);
        blit_frame(buf);
    }
    // else: no thumbnail — leave the black backdrop.
}

void ui_show_metadata(const char* sender, const char* created, int duration_sec) {
    uint16_t* buf = mjpeg_thumb_buffer();
    if (!buf) return;

    char when[24];
    format_local_time(created, when, sizeof(when));   // UTC → device-local (SGT)

    char dur[16];
    snprintf(dur, sizeof(dur), "%d s", duration_sec);

    fb_panel(buf, 25, 80, 190, 92, 18, 160);   // one soft rounded panel behind all three lines
    composite_label(buf, (sender && sender[0]) ? sender : "Unknown",
                    &lv_font_montserrat_18, 120, 100);
    composite_label(buf, when, &lv_font_montserrat_14, 120, 128);
    composite_label(buf, dur, &lv_font_montserrat_14, 120, 150);

    blit_frame(buf);
}

void ui_hide_metadata(void) { /* caller re-renders the clean frame via show_frame() */ }

// ── Incoming pulse — sender's avatar with a pulsing ring ────────────────────
// Same direct-blit + hand-composited pattern as the video progress arc: an
// LVGL widget animated on top of a direct-blit photo would get erased by
// LVGL's own dirty-rect repaint (that buffer isn't part of LVGL's scene graph
// — see gotcha #17), so the ring is alpha-blended into a COPY of the decoded
// avatar every animation frame and the whole thing is direct-blit each time.
static uint16_t* s_incoming_buf = nullptr;   // scratch copy (avatar stays pristine)

// Alpha-blend an RGB565 pixel toward white by coverage (0..1) — same technique
// as mjpeg_player.cpp's video-progress-arc blend (that one's static to its own
// translation unit, so this is a local twin rather than a shared header, since
// it's a two-line helper not worth a cross-module API).
static inline uint16_t blend_white(uint16_t bg, float cov) {
    int a = (int)(cov * 256.0f + 0.5f);
    if (a <= 0) return bg;
    if (a > 256) a = 256;
    int r = (bg >> 11) & 0x1F, g = (bg >> 5) & 0x3F, b = bg & 0x1F;
    r += ((31 - r) * a) >> 8;
    g += ((63 - g) * a) >> 8;
    b += ((31 - b) * a) >> 8;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

static void draw_pulse_ring(uint16_t* buf, float phase) {
    // A "sonar ping": ring grows outward from the photo while fading out.
    float r     = 74.0f + phase * 46.0f;      // 74 → 120 px
    float rin   = r - 5.0f, rout = r;
    float opa   = 1.0f - phase;
    int   rin2  = (int)(rin * rin), rout2 = (int)(rout * rout);
    for (int y = 0; y < 240; y++) {
        int dy = y - 120;
        int dy2 = dy * dy;
        if (dy2 > rout2) continue;
        for (int x = 0; x < 240; x++) {
            int dx = x - 120;
            int d2 = dx * dx + dy2;
            if (d2 < rin2 || d2 > rout2) continue;
            float d = sqrtf((float)d2);
            float edge = fminf(d - rin, rout - d);
            float cov = (edge < 1.0f ? edge : 1.0f) * opa;
            if (cov <= 0.02f) continue;
            int idx = y * 240 + x;
            buf[idx] = blend_white(buf[idx], cov);
        }
    }
}

void ui_enter_incoming_photo(void) {
    if (lv_screen_active() != s_black_scr) {
        lv_screen_load(s_black_scr);
        lv_refr_now(nullptr);
    }
}

void ui_render_incoming_photo(const char* sender, float phase) {
    uint16_t* src = mjpeg_thumb_buffer();
    if (!src) return;
    if (!s_incoming_buf) s_incoming_buf = (uint16_t*)heap_caps_malloc(240 * 240 * 2, MALLOC_CAP_SPIRAM);
    if (!s_incoming_buf) return;

    memcpy(s_incoming_buf, src, 240 * 240 * 2);   // start from the pristine photo each frame
    draw_pulse_ring(s_incoming_buf, phase);
    if (sender && sender[0]) {
        fb_panel(s_incoming_buf, 30, 194, 180, 30, 15, 150);
        composite_label(s_incoming_buf, sender, &lv_font_montserrat_16, 120, 209);
    }
    blit_frame(s_incoming_buf);
}

// ── Text message screen (History carousel — no photo, so no direct-blit needed) ─
// A genuine LVGL screen: real anti-aliased rounded widgets throughout, unlike the
// freeze-frame overlays which had to be hand-composited onto a direct-blit buffer.
static lv_obj_t* s_text_scr       = nullptr;
static lv_obj_t* s_text_index_lbl = nullptr;
static lv_obj_t* s_text_sender_lbl = nullptr;
static lv_obj_t* s_text_body_lbl  = nullptr;

static void build_text() {
    s_text_scr = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(s_text_scr, lv_color_black(), 0);
    lv_obj_remove_flag(s_text_scr, LV_OBJ_FLAG_SCROLLABLE);

    // "index/total" badge — matches the freeze-frame badge's look (soft rounded
    // pill), but as a real LVGL widget since there's no underlying photo here.
    s_text_index_lbl = lv_label_create(s_text_scr);
    lv_obj_set_style_text_font(s_text_index_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_text_index_lbl, lv_color_white(), 0);
    lv_obj_set_style_bg_color(s_text_index_lbl, lv_color_hex(0x1a1a1a), 0);
    lv_obj_set_style_bg_opa(s_text_index_lbl, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(s_text_index_lbl, 10, 0);
    lv_obj_set_style_pad_ver(s_text_index_lbl, 5, 0);
    lv_obj_set_style_radius(s_text_index_lbl, LV_RADIUS_CIRCLE, 0);
    lv_label_set_text(s_text_index_lbl, "");
    lv_obj_align(s_text_index_lbl, LV_ALIGN_TOP_MID, 0, 12);

    // Sender caption, above the message.
    s_text_sender_lbl = lv_label_create(s_text_scr);
    lv_obj_set_style_text_font(s_text_sender_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_text_sender_lbl, lv_color_hex(0x66aaff), 0);
    lv_label_set_text(s_text_sender_lbl, "");
    lv_obj_align(s_text_sender_lbl, LV_ALIGN_CENTER, 0, -62);

    // Message body — word-wrapped, width constrained to the round panel's safe
    // text zone (a 176px-wide column clears the bezel through the full vertical
    // span the label can occupy; see CLAUDE.md for the chord-width derivation).
    s_text_body_lbl = lv_label_create(s_text_scr);
    lv_obj_set_style_text_font(s_text_body_lbl, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(s_text_body_lbl, lv_color_white(), 0);
    lv_label_set_long_mode(s_text_body_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_text_body_lbl, 176);
    lv_obj_set_style_text_align(s_text_body_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_text_body_lbl, "");
    lv_obj_align(s_text_body_lbl, LV_ALIGN_CENTER, 0, 6);
}

void ui_show_text(const char* sender, const char* text, int index, int total) {
    char b[16];
    snprintf(b, sizeof(b), "%d/%d", index, total);
    lv_label_set_text(s_text_index_lbl, b);
    lv_label_set_text(s_text_sender_lbl, (sender && sender[0]) ? sender : "Unknown");
    lv_label_set_text(s_text_body_lbl, text ? text : "");
    lv_screen_load(s_text_scr);
    s_active = Screen::HISTORY;   // same browsing state as the freeze-frame screen
}

// ── Public API ──────────────────────────────────────────────────────────────
void ui_init(void) {
    build_idle();
    build_incoming();
    build_history();
    build_text();
    s_black_scr = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(s_black_scr, lv_color_black(), 0);
}

void ui_show_idle(void) {
    lv_screen_load(s_idle_scr);
    s_active = Screen::IDLE;
    ui_tick();
}

void ui_tick(void) {
    if (s_active != Screen::IDLE) return;

    time_t now = time(nullptr);
    struct tm tm_now;
    localtime_r(&now, &tm_now);

    char buf[16];
    strftime(buf, sizeof(buf), "%H:%M", &tm_now);
    lv_label_set_text(s_time_lbl, buf);
}

void ui_set_battery(int percent, bool charging) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    s_batt_pct = percent;
    s_charging = charging;
}

// Drives the breathing circle's colour (no separate WiFi icon — see build_idle):
// red = no link, blue = link up but still connecting to the server (SSE not
// live yet), green = fully connected.
void ui_set_status(bool wifi_ok, bool server_ok) {
    if (!s_breathe) return;
    lv_color_t c = !wifi_ok   ? lv_color_hex(0xcc3333)   // failed to connect — red
                 : !server_ok ? lv_color_hex(0x3366ff)   // connecting — blue
                              : lv_color_hex(0x33cc66);  // connected — green
    lv_obj_set_style_bg_color(s_breathe, c, 0);
}

void ui_set_unread(int n) {
    if (!s_unread_num) return;
    if (n > 0) {
        char b[8];
        snprintf(b, sizeof(b), "%d", n > 99 ? 99 : n);
        lv_label_set_text(s_unread_num, b);
    } else {
        lv_label_set_text(s_unread_num, "");
    }
}

void ui_set_last_sender(const char* name) {
    if (!s_lastcap_lbl || !s_lastname_lbl) return;
    if (name && name[0]) {
        lv_label_set_text(s_lastcap_lbl, "Last message from");
        lv_label_set_text(s_lastname_lbl, name);
    } else {
        lv_label_set_text(s_lastcap_lbl, "");
        lv_label_set_text(s_lastname_lbl, "");
    }
}

void ui_show_incoming(const char* sender) {
    if (sender && sender[0]) lv_label_set_text(s_sender_lbl, sender);
    else                     lv_label_set_text(s_sender_lbl, "Incoming");
    lv_screen_load(s_incoming_scr);
    s_active = Screen::INCOMING;
}

void ui_show_playing(void) {
    lv_screen_load(s_black_scr);
    s_active = Screen::PLAYING;
    // Force the black frame out now so no incoming-screen pixels remain under the
    // video that the caller is about to blit directly.
    lv_refr_now(nullptr);
    // Let the final LVGL flush DMA + its completion ISR finish before the caller
    // starts direct blits. Until this settles, a blit flipping the display's
    // trans-done routing (LVGL vs blit) could swallow LVGL's last flush_ready and
    // wedge the pipeline. 20 ms >> one 60-line tile DMA (~3 ms).
    delay(20);
}

void ui_show_done(void) {
    // The last video frame is still on the panel (blitted directly, outside LVGL).
    // Leave it; the caller holds it for DONE_FREEZE_MS then calls ui_show_idle().
    s_active = Screen::DONE;
}

bool ui_idle_active(void) { return s_active == Screen::IDLE; }
