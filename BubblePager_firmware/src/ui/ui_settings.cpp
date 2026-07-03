#include "ui_settings.h"
#include "config.h"
#include <lvgl.h>
#include <Arduino.h>

// Row identities. Value-bearing rows are contiguous 0..SET_INCOMING.
enum { SET_BATTERY = 0, SET_SYNC, SET_HAPTIC, SET_ADAPT, SET_FLIP, SET_TAP,
       SET_INCOMING, SET_EXIT, SET_COUNT };

static const uint16_t INCOMING_PRESETS[] = { 3, 5, 8, 12, 20 };
static constexpr int  INCOMING_PRESET_COUNT = 5;

// Geometry (see config.h). Centring maths: with SET_PAD_Y top/bottom padding the
// scroll-top that centres row i is exactly i*SET_PITCH, so the centred row is
// round(scroll_top / SET_PITCH).
static constexpr int SET_PITCH      = SET_ROW_H + SET_ROW_GAP;
static constexpr int SET_PAD_Y      = (SET_VIEW_H - SET_ROW_H) / 2;
static constexpr int SET_SCROLL_MAX = SET_PITCH * (SET_COUNT - 1);

static lv_obj_t* s_scr             = nullptr;
static lv_obj_t* s_list            = nullptr;
static lv_obj_t* s_focus           = nullptr;   // fixed blue frame at the centre slot
static lv_obj_t* s_rows[SET_COUNT]   = {};
static lv_obj_t* s_labels[SET_COUNT] = {};
static lv_obj_t* s_values[SET_COUNT] = {};      // null for the Exit row

static AppSettings s_vals   = {};
static int         s_sel    = SET_SYNC;
static bool        s_active = false;

static const char* ON_OFF(bool b) { return b ? "ON" : "OFF"; }

static void refresh_row(int i) {
    if (i < 0 || i >= SET_COUNT || !s_values[i]) return;
    switch (i) {
        case SET_HAPTIC: lv_label_set_text(s_values[i], ON_OFF(s_vals.haptics));          break;
        case SET_ADAPT:  lv_label_set_text(s_values[i], ON_OFF(s_vals.adaptive_timeout)); break;
        case SET_FLIP:   lv_label_set_text(s_values[i], ON_OFF(s_vals.orient_flip));      break;
        case SET_TAP:    lv_label_set_text(s_values[i], ON_OFF(s_vals.tap_sleep));        break;
        case SET_INCOMING: {
            char b[8]; snprintf(b, sizeof(b), "%us", s_vals.incoming_timeout_sec);
            lv_label_set_text(s_values[i], b);
            break;
        }
        default: break;   // BATTERY + SYNC are driven by their setters
    }
}

// Focused row reads black-on-blue (the fixed focus frame); off-centre rows dim.
// Battery + Sync own their value colour (status), so only their name is re-coloured.
static void update_highlight() {
    for (int i = 0; i < SET_COUNT; i++) {
        if (!s_rows[i]) continue;
        bool sel = (i == s_sel);
        lv_obj_set_style_text_color(s_labels[i],
            sel ? lv_color_black() : lv_color_hex(0xBDBDBD), 0);
        if (s_values[i] && i != SET_BATTERY && i != SET_SYNC)
            lv_obj_set_style_text_color(s_values[i],
                sel ? lv_color_black() : lv_color_hex(0x9E9E9E), 0);
    }
}

void ui_settings_init() {
    if (s_scr) return;
    s_scr = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(s_scr, lv_color_black(), 0);
    lv_obj_remove_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* title = lv_label_create(s_scr);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 22);
    lv_label_set_text(title, "Settings");

    // Fixed focus frame at the centre slot (created before the list → sits behind).
    s_focus = lv_obj_create(s_scr);
    lv_obj_remove_flag(s_focus, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_focus, SET_ROW_W, SET_ROW_H);
    lv_obj_align(s_focus, LV_ALIGN_CENTER, 0, 18);
    lv_obj_set_style_radius(s_focus, 8, 0);
    lv_obj_set_style_border_width(s_focus, 0, 0);
    lv_obj_set_style_bg_color(s_focus, lv_color_hex(0x2196F3), 0);
    lv_obj_set_style_bg_opa(s_focus, LV_OPA_COVER, 0);

    // Scroll viewport. Scrolling is driven by the ui_settings_* API, not an indev.
    s_list = lv_obj_create(s_scr);
    lv_obj_set_size(s_list, SET_VIEW_W, SET_VIEW_H);
    lv_obj_align(s_list, LV_ALIGN_CENTER, 0, 18);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_list, 0, 0);
    lv_obj_set_style_pad_all(s_list, 0, 0);
    lv_obj_set_style_pad_top(s_list, SET_PAD_Y, 0);
    lv_obj_set_style_pad_bottom(s_list, SET_PAD_Y, 0);
    lv_obj_set_style_pad_row(s_list, SET_ROW_GAP, 0);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
    // Keep SCROLLABLE (needed for a real scroll range); no LVGL indev is registered,
    // so it never auto-scrolls — the ui_settings_* API owns the scroll position.

    static const char* names[SET_COUNT] = {
        "Battery", "Sync", "Haptics", "Motion wake", "Auto 180", "Tap sleep",
        "Incoming", "Exit",
    };
    for (int i = 0; i < SET_COUNT; i++) {
        lv_obj_t* row = lv_obj_create(s_list);
        lv_obj_set_size(row, SET_ROW_W, SET_ROW_H);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);   // focus frame provides highlight
        lv_obj_set_style_radius(row, 8, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_left(row, 14, 0);
        lv_obj_set_style_pad_right(row, 14, 0);
        lv_obj_set_style_pad_ver(row, 0, 0);
        lv_obj_set_scrollbar_mode(row, LV_SCROLLBAR_MODE_OFF);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        s_rows[i] = row;

        lv_obj_t* lbl = lv_label_create(row);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);
        lv_label_set_text(lbl, names[i]);
        s_labels[i] = lbl;

        if (i != SET_EXIT) {
            lv_obj_t* val = lv_label_create(row);
            lv_obj_set_style_text_font(val, &lv_font_montserrat_16, 0);
            lv_obj_align(val, LV_ALIGN_RIGHT_MID, 0, 0);
            lv_label_set_text(val, "");
            s_values[i] = val;
        }
    }
}

// Scroll so row `sel` lands in the centre slot; update focus + highlight.
static void center_on(int sel, bool anim) {
    if (!s_list) return;
    if (sel < 0) sel = 0;
    if (sel >= SET_COUNT) sel = SET_COUNT - 1;
    s_sel = sel;
    lv_obj_update_layout(s_list);
    lv_obj_scroll_to_y(s_list, SET_PITCH * sel, anim ? LV_ANIM_ON : LV_ANIM_OFF);
    update_highlight();
}

static int centered_from_scroll() {
    lv_obj_update_layout(s_list);
    int st = lv_obj_get_scroll_top(s_list);
    if (st < 0) st = 0;
    int sel = (st + SET_PITCH / 2) / SET_PITCH;
    if (sel < 0) sel = 0;
    if (sel >= SET_COUNT) sel = SET_COUNT - 1;
    return sel;
}

void ui_settings_open(const AppSettings& cur) {
    ui_settings_init();
    s_vals = cur;
    for (int i = 0; i < SET_COUNT; i++) refresh_row(i);
    lv_label_set_text(s_values[SET_SYNC], LV_SYMBOL_WIFI " …");
    lv_screen_load(s_scr);
    s_active = true;
    center_on(SET_SYNC, false);
}

bool ui_settings_active() { return s_active; }
void ui_settings_close()  { s_active = false; }

int ui_settings_centered_row() {
    if (!s_list) return s_sel;
    return centered_from_scroll();
}

int ui_settings_scroll_by(int dy) {
    if (!s_list) return s_sel;
    // scroll_by(+dy) moves content DOWN (reduces scroll_top). Clamp scroll_top to
    // [0, SET_SCROLL_MAX] so the first/last row travels no further than the centre.
    lv_obj_update_layout(s_list);
    int st   = lv_obj_get_scroll_top(s_list);
    int want = st - dy;
    if (want < 0) want = 0;
    if (want > SET_SCROLL_MAX) want = SET_SCROLL_MAX;
    int applied = st - want;
    if (applied) lv_obj_scroll_by(s_list, 0, applied, LV_ANIM_OFF);
    int sel = centered_from_scroll();
    if (sel != s_sel) { s_sel = sel; update_highlight(); }
    return s_sel;
}

void ui_settings_snap() {
    if (!s_list) return;
    center_on(centered_from_scroll(), true);
}

// Row whose vertical centre is nearest panel-y, within the viewport. -1 if none.
static int row_at(int y) {
    if (!s_scr || !s_list) return -1;
    lv_obj_update_layout(s_scr);
    lv_area_t va;
    lv_obj_get_coords(s_list, &va);
    if (y < va.y1 || y > va.y2) return -1;
    int best = -1, best_d = 1 << 30;
    for (int i = 0; i < SET_COUNT; i++) {
        if (!s_rows[i]) continue;
        lv_area_t a;
        lv_obj_get_coords(s_rows[i], &a);
        int cy = (a.y1 + a.y2) / 2;
        if (cy < va.y1 || cy > va.y2) continue;
        int d = (cy > y) ? (cy - y) : (y - cy);
        if (d < best_d) { best_d = d; best = i; }
    }
    return best;
}

// Toggle/activate the centred row.
static UiSettingsAction select_focused() {
    switch (s_sel) {
        case SET_SYNC:   return UI_SET_SYNC;
        case SET_HAPTIC: s_vals.haptics          = !s_vals.haptics;          refresh_row(SET_HAPTIC); break;
        case SET_ADAPT:  s_vals.adaptive_timeout = !s_vals.adaptive_timeout; refresh_row(SET_ADAPT);  break;
        case SET_FLIP:   s_vals.orient_flip      = !s_vals.orient_flip;      refresh_row(SET_FLIP);   break;
        case SET_TAP:    s_vals.tap_sleep        = !s_vals.tap_sleep;        refresh_row(SET_TAP);    break;
        case SET_INCOMING: {
            int idx = 0;
            for (int k = 0; k < INCOMING_PRESET_COUNT; k++)
                if (INCOMING_PRESETS[k] == s_vals.incoming_timeout_sec) { idx = k; break; }
            idx = (idx + 1) % INCOMING_PRESET_COUNT;
            s_vals.incoming_timeout_sec = INCOMING_PRESETS[idx];
            refresh_row(SET_INCOMING);
            break;
        }
        case SET_EXIT:   return UI_SET_EXIT;
        case SET_BATTERY: default: break;   // status only
    }
    return UI_SET_NONE;
}

UiSettingsAction ui_settings_tap(int y) {
    int r = row_at(y);
    if (r < 0) return UI_SET_NONE;
    if (r == s_sel) return select_focused();   // already centred → activate
    center_on(r, true);                         // off-centre → scroll into focus
    return UI_SET_NONE;
}

void ui_settings_set_battery(int pct, bool charging) {
    if (!s_values[SET_BATTERY]) return;
    if (pct < 0) pct = 0; if (pct > 100) pct = 100;
    const char* icon = charging          ? LV_SYMBOL_CHARGE :
                       pct >= 80          ? LV_SYMBOL_BATTERY_FULL :
                       pct >= 55          ? LV_SYMBOL_BATTERY_3 :
                       pct >= 30          ? LV_SYMBOL_BATTERY_2 :
                       pct >= 10          ? LV_SYMBOL_BATTERY_1 :
                                            LV_SYMBOL_BATTERY_EMPTY;
    char buf[24];
    snprintf(buf, sizeof(buf), "%s %d%%", icon, pct);
    lv_label_set_text(s_values[SET_BATTERY], buf);
    lv_color_t col = charging ? lv_color_hex(0x4CAF50) :
                     pct < 10 ? lv_color_hex(0xF44336) :
                     pct < 30 ? lv_color_hex(0xFF8800) :
                                lv_color_hex(0xE0E0E0);
    lv_obj_set_style_text_color(s_values[SET_BATTERY], col, 0);
}

void ui_settings_set_sync_status(const char* text, int state) {
    if (!s_values[SET_SYNC] || !text) return;
    char buf[40];
    snprintf(buf, sizeof(buf), LV_SYMBOL_WIFI " %s", text);
    lv_label_set_text(s_values[SET_SYNC], buf);
    lv_color_t col = state == 1 ? lv_color_hex(0x4CAF50) :   // ok
                     state == 2 ? lv_color_hex(0xF44336) :   // fail
                     state == 3 ? lv_color_hex(0x2196F3) :   // busy
                                  lv_color_hex(0x9E9E9E);    // neutral
    lv_obj_set_style_text_color(s_values[SET_SYNC], col, 0);
}

AppSettings ui_settings_values() { return s_vals; }
