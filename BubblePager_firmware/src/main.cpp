#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <time.h>
#include <sys/time.h>

#include "hardware/hardware.h"
#include "display/display.h"
#include "ui/ui.h"
#include "ui/ui_settings.h"
#include "buttons/buttons.h"
#include "touch/touch.h"
#include "haptics/haptics.h"
#include "battery/battery.h"
#include "rtc/rtc_manager.h"
#include "power/power.h"
#include "nvs/nvs_manager.h"
#include "history/history.h"
#include "imu/imu_manager.h"
#include "net/wifi_mgr.h"
#include "net/sse_client.h"
#include "net/mjpeg_player.h"
#include "net/time_sync.h"
#include "net/device_report.h"
#include "config.h"

// ── State machine (see UI Interaction Spec) ─────────────────────────────────
// HOME (clock) ⇄ HISTORY (freeze-frame browsing) → PLAYING → back. Buttons and
// touch drive the SAME transitions. SETTINGS is a modal handled via ui_settings.
enum PagerState { PS_HOME, PS_HISTORY, PS_PLAYING };
static PagerState s_state = PS_HOME;

// How a PLAYING session ended (set by the interrupt handlers, read after play).
enum Interrupt { INT_NONE = 0, INT_OLDER, INT_NEWER, INT_HOME, INT_SAME };
static volatile int s_interrupt = INT_NONE;

static QueueHandle_t s_video_queue = nullptr;
static char          s_cur_id[64]  = {};              // currently shown video id (type 'V')
static char          s_cur_type    = 'V';              // type of the currently shown entry
static volatile bool s_playing  = false;
static AppSettings   s_settings  = {};
static bool          s_flipped   = false;
static volatile bool s_settings_entry_lock = false;   // swallow the settings-opening touch
static bool          s_meta_shown = false;            // metadata overlay visible (HISTORY)
static uint16_t      s_unread_count = 0;              // pager-style unread badge on HOME
static bool           s_in_reel = false;              // true while play_unread_sequence() drives playback

// ── History entry encoding ──────────────────────────────────────────────────
// history.cpp stores opaque strings; main.cpp is the only place that knows the
// format: "<type><sender>\x01<payload>" where type is 'V' (payload=video_id) or
// 'T' (payload=message text). '\x01' (a control byte) is used as the field
// separator instead of a printable character since message text can contain
// almost anything the sender typed — '\x01' can't appear in real text messages
// and is stripped from the sender name server-side already ('|', '\n', '\r').
static void encode_entry(char* out, size_t outlen, char type, const char* sender, const char* payload) {
    snprintf(out, outlen, "%c%s\x01%s", type, sender ? sender : "", payload ? payload : "");
}

struct HistEntry { char type; char sender[48]; char payload[TEXT_MSG_MAX_CHARS + 16]; };

static bool decode_entry(const char* raw, HistEntry& e) {
    if (!raw || !raw[0]) return false;
    e.type = raw[0];
    const char* p = raw + 1;
    const char* sep = strchr(p, '\x01');
    if (!sep) {
        e.sender[0] = 0;
        strncpy(e.payload, p, sizeof(e.payload) - 1); e.payload[sizeof(e.payload) - 1] = 0;
        return true;
    }
    size_t slen = (size_t)(sep - p);
    if (slen >= sizeof(e.sender)) slen = sizeof(e.sender) - 1;
    memcpy(e.sender, p, slen); e.sender[slen] = 0;
    strncpy(e.payload, sep + 1, sizeof(e.payload) - 1); e.payload[sizeof(e.payload) - 1] = 0;
    return true;
}

// ── Touch gesture classifier ────────────────────────────────────────────────
enum Gesture { G_NONE, G_TAP, G_LONGPRESS, G_SWIPE_UP, G_SWIPE_DOWN, G_SWIPE_LEFT, G_SWIPE_RIGHT };

// Track a touch from down→up and classify on release (or fire LONG_PRESS while
// held). Error reads hold state (no phantom lift). Poll from the active context
// (main loop for HOME/HISTORY, render_poll for PLAYING) — they're exclusive.
// `tap_x` (optional) is filled with the panel-x of a G_TAP — used by the reel
// (see play_unread_sequence) to tell a left-half "previous" tap from a
// right-half "next" tap, Reels/Stories-style.
static Gesture poll_gesture(int* tap_x = nullptr) {
    static uint32_t poll_ms = 0;
    static bool     down = false;
    static int      sx = 0, sy = 0, lx = 0, ly = 0;
    static uint32_t down_ms = 0;
    static bool     moved = false, longfired = false;

    uint32_t now = millis();
    if (now - poll_ms < 20) return G_NONE;
    poll_ms = now;

    int x = 0, y = 0;
    int c = touch_read_contact(&x, &y);
    if (c < 0) return G_NONE;                  // transient I2C error → hold state
    bool pressed = (c == 1);

    if (pressed) {
        if (!down) { down = true; sx = lx = x; sy = ly = y; down_ms = now; moved = false; longfired = false; }
        else {
            lx = x; ly = y;
            if (abs(x - sx) > GEST_SWIPE_DEADZONE || abs(y - sy) > GEST_SWIPE_DEADZONE) moved = true;
            if (!moved && !longfired && (now - down_ms) >= SET_LONGPRESS_MS) { longfired = true; return G_LONGPRESS; }
        }
        return G_NONE;
    }
    if (!down) return G_NONE;
    down = false;
    if (longfired) return G_NONE;              // already delivered
    int dx = lx - sx, dy = ly - sy;
    uint32_t held = now - down_ms;
    if (abs(dy) > GEST_SWIPE_THRESHOLD && abs(dy) > abs(dx)) return dy < 0 ? G_SWIPE_UP : G_SWIPE_DOWN;
    if (abs(dx) > GEST_SWIPE_THRESHOLD && abs(dx) > abs(dy)) return dx < 0 ? G_SWIPE_LEFT : G_SWIPE_RIGHT;
    if (abs(dx) <= GEST_SWIPE_DEADZONE && abs(dy) <= GEST_SWIPE_DEADZONE && held < GEST_TAP_MAX_MS) {
        if (tap_x) *tap_x = lx;
        return G_TAP;
    }
    return G_NONE;
}

// ── Navigation ──────────────────────────────────────────────────────────────
// Drop to black before video, then confirm playback with a buzz — but only for a
// REAL (multi-frame) video/sticker. A static sticker is exactly 1 frame and
// isn't really "playing"; buzzing for it (as an unconditional start-of-play
// haptic in play_video() used to) felt like an unwanted tap-vibration for what
// is functionally just a photo. 0 (X-Frame-Count absent/unknown) still buzzes.
static void on_first_frame() {
    ui_show_playing();
    if (mjpeg_last_total_frames() != 1) haptic_tick();
}

static void go_home() {
    s_meta_shown = false;
    ui_show_idle();
    s_state = PS_HOME;
    power_reset_idle_timer();
}

// Fetch + show the current cursor's entry — a video freeze-frame or a text
// message, per its stored type. Both share the same n/N browsing.
static void show_frame() {
    char raw[HISTORY_ID_LEN];
    if (!history_current(raw, sizeof(raw))) { go_home(); return; }
    HistEntry e;
    if (!decode_entry(raw, e)) { go_home(); return; }

    s_cur_type = e.type;
    s_meta_shown = false;
    if (s_unread_count) { s_unread_count = 0; ui_set_unread(0); }   // viewing clears the badge
    int index = history_cursor_age() + 1, total = history_count();   // 1 = newest

    if (e.type == 'T') {
        s_cur_id[0] = 0;                       // no server id — nothing to play/ack
        ui_show_text(e.sender, e.payload, index, total);
    } else {
        strncpy(s_cur_id, e.payload, sizeof(s_cur_id) - 1);
        mjpeg_fetch_thumb(e.payload);           // best-effort; UI shows a placeholder on fail
        ui_show_history(index, total);
    }
    s_state = PS_HISTORY;
    power_reset_idle_timer();
}

// Long-press on a HISTORY video frame → fetch + show sender/timestamp/duration.
// No-op for text entries — the message + sender are already fully on screen.
static void show_metadata() {
    if (s_cur_type != 'V' || !s_cur_id[0]) { haptic_click(); return; }
    char sender[48] = {}, created[32] = {};
    int  dur = 0;
    if (mjpeg_fetch_meta(s_cur_id, sender, sizeof(sender), created, sizeof(created), &dur)) {
        ui_show_metadata(sender, created, dur);
        s_meta_shown = true;
        haptic_tick();
    } else {
        haptic_double();   // no metadata (offline / not found)
    }
}

static void enter_history() {
    if (history_count() == 0) { haptic_click(); return; }
    history_reset_cursor();
    show_frame();
}

static void nav_older() {
    if (s_state != PS_HISTORY) { enter_history(); return; }
    char id[HISTORY_ID_LEN];
    if (history_older(id, sizeof(id))) show_frame(); else haptic_click();   // at oldest
}
static void nav_newer() {
    if (s_state != PS_HISTORY) { enter_history(); return; }
    char id[HISTORY_ID_LEN];
    if (history_newer(id, sizeof(id))) show_frame(); else haptic_click();   // at newest
}

// Play s_cur_id. Fully interruptible (buttons/touch in render_poll set s_interrupt
// + mjpeg_abort). Decides the next screen from how it ended. Returns the interrupt
// that ended it — INT_NONE means it played to completion untouched. Callers that
// don't care (play_current) just discard it; play_unread_sequence() uses it to
// decide whether/which-direction to keep auto-advancing.
static int play_video(const char* id) {
    s_state     = PS_PLAYING;
    s_playing   = true;
    s_interrupt = INT_NONE;
    power_reset_idle_timer();
    // Confirm-buzz moved to on_first_frame() — conditional on it being a real
    // (multi-frame) video/sticker, not a static (1-frame) one.

    MjpegResult r = mjpeg_play(id, on_first_frame);
    s_playing = false;
    int intr = s_interrupt; s_interrupt = INT_NONE;

    if (r == MjpegResult::OK && intr == INT_NONE) {
        device_ack(id);                       // played to the end → Telegram "Played"
        if (history_cursor_age() == 0) go_home();   // was newest → HOME
        else                          show_frame(); // else stay on its frame
        return INT_NONE;
    }
    char tmp[HISTORY_ID_LEN];
    switch (intr) {
        case INT_HOME:  go_home(); break;
        case INT_OLDER: history_older(tmp, sizeof(tmp)); show_frame(); break;
        case INT_NEWER: history_newer(tmp, sizeof(tmp)); show_frame(); break;
        default:        show_frame(); break;  // INT_SAME / stream error → same frame
    }
    return intr;
}
// Text entries have nothing to play — a tap/hold on one just nudges the user.
static void play_current() {
    if (s_cur_type == 'V' && s_cur_id[0]) play_video(s_cur_id);
    else                                  haptic_click();
}

// Wait during a text entry's "read pause" in the reel. Returns the SAME
// Interrupt vocabulary play_video() does (INT_NONE = pause elapsed naturally),
// so play_unread_sequence() can treat text and video identically: tap visually-
// left/up = INT_OLDER, tap visually-right/down = INT_NEWER (Reels/Stories-style
// — keeps the reel rolling instead of stopping it), swipe left/right = INT_HOME
// (explicit exit). See render_poll()'s G_TAP case for why tap_x<120 means the
// visually-RIGHT half on this mount (touch x comes out mirrored).
static int wait_reel_text_pause() {
    uint32_t start = millis();
    while ((millis() - start) < UNREAD_TEXT_PAUSE_MS) {
        display_tick();
        haptics_poll();

        int tap_x = 120;
        switch (poll_gesture(&tap_x)) {
            case G_TAP:          return (tap_x < 120) ? INT_NEWER : INT_OLDER;
            case G_SWIPE_UP:     return INT_OLDER;
            case G_SWIPE_DOWN:   return INT_NEWER;
            case G_SWIPE_LEFT:
            case G_SWIPE_RIGHT:  return INT_HOME;
            case G_LONGPRESS:    return INT_SAME;   // safe fallback — just stop here
            default: break;
        }
        switch (buttons_tick()) {
            case ButtonEvent::BTN1_SHORT:
            case ButtonEvent::BTN1_HOLD: return INT_OLDER;
            case ButtonEvent::BTN2_SHORT:
            case ButtonEvent::BTN2_HOLD: return INT_NEWER;
            case ButtonEvent::PWR_SHORT: power_toggle_display(); break;
            case ButtonEvent::PWR_HOLD:  imu_sleep_mode(); power_go_to_sleep(); break;
            default: break;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return INT_NONE;
}

// ── Unread catch-up reel ─────────────────────────────────────────────────────
// Tap HOME with unread mail → play the missed items in the order they actually
// arrived: oldest first, auto-advancing forward through the backlog, ending on
// the most recent (matching how a real pager plays back missed messages).
//
// Reels/Stories-style navigation while the reel is running: tap the LEFT half
// of the screen → previous item; tap the RIGHT half → skip to next (the reel
// keeps rolling, it doesn't stop — see render_poll()'s s_in_reel branch and
// wait_reel_text_pause() above). Swipe left/right explicitly exits to HOME.
// Only a genuinely unrecognised interrupt (INT_SAME) or reaching the end stops
// the chain outright.
static void play_unread_sequence() {
    if (s_unread_count == 0) { enter_history(); return; }

    // Jump the cursor to the oldest unread entry (unread_count-1 steps older
    // than newest) by repeated relative stepping — history's cursor API has no
    // direct "seek", but HISTORY_MAX (12) keeps this trivially cheap.
    history_reset_cursor();
    char tmp[HISTORY_ID_LEN];
    for (int i = 0; i < s_unread_count - 1; i++) {
        if (!history_older(tmp, sizeof(tmp))) break;   // shouldn't happen; guards anyway
    }

    s_in_reel = true;
    for (;;) {
        char raw[HISTORY_ID_LEN];
        if (!history_current(raw, sizeof(raw))) break;
        HistEntry e;
        if (!decode_entry(raw, e)) break;

        bool was_newest = (history_cursor_age() == 0);
        s_cur_type = e.type;
        if (s_unread_count) { s_unread_count = 0; ui_set_unread(0); }   // viewing clears it

        int intr;
        if (e.type == 'T') {
            int index = history_cursor_age() + 1, total = history_count();
            s_cur_id[0] = 0;
            ui_show_text(e.sender, e.payload, index, total);
            s_state = PS_HISTORY;
            power_reset_idle_timer();
            intr = wait_reel_text_pause();
        } else {
            strncpy(s_cur_id, e.payload, sizeof(s_cur_id) - 1);
            intr = play_video(e.payload);   // for V, already steps the cursor + shows a frame
                                             // internally on INT_OLDER/INT_NEWER/INT_HOME
        }

        if (intr == INT_HOME) { go_home(); break; }
        if (intr == INT_SAME) break;   // safety fallback — leave whatever's already showing

        if (intr == INT_OLDER) {
            if (e.type == 'T' && !history_older(tmp, sizeof(tmp))) { haptic_click(); break; }
            continue;   // loop re-reads the (now older) current entry and plays/shows it
        }
        if (intr == INT_NEWER) {
            if (was_newest) { go_home(); break; }   // nothing newer than newest
            if (e.type == 'T' && !history_newer(tmp, sizeof(tmp))) break;
            continue;
        }

        // intr == INT_NONE → finished/elapsed naturally → advance forward.
        if (was_newest) { go_home(); break; }
        if (!history_newer(tmp, sizeof(tmp))) break;   // shouldn't happen; guards anyway
    }
    s_in_reel = false;
}

// Called by mjpeg_play() between network reads — keeps input live so playback is
// always interruptible (spec: no blocking waits on a full clip).
static void render_poll() {
    haptics_poll();
    static uint32_t last = 0;
    uint32_t now = millis();
    if (now - last < 25) return;               // throttle the shared I²C bus
    last = now;

    switch (buttons_tick()) {
        case ButtonEvent::BTN1_SHORT: s_interrupt = INT_OLDER; mjpeg_abort(); break;
        case ButtonEvent::BTN2_SHORT: s_interrupt = INT_NEWER; mjpeg_abort(); break;
        case ButtonEvent::PWR_SHORT:  power_toggle_display(); break;
        case ButtonEvent::PWR_HOLD:   imu_sleep_mode(); power_go_to_sleep(); break;
        default: break;
    }
    int tap_x = 120;
    switch (poll_gesture(&tap_x)) {
        case G_TAP:
            // Reels/Stories-style: in the unread reel, a tap is a hemisphere —
            // left half (visually) = previous, right half = skip to next (keeps
            // the reel rolling). Outside the reel, a tap just interrupts to this
            // frame. NOTE: touch_read_contact()'s x comes out mirrored versus
            // panel-visual x for this mount (touch.cpp's 180°-mount correction),
            // so tap_x < 120 here is the visually-RIGHT half — hence NEWER, not
            // OLDER. Re-check this mapping by hand if the mount ever changes.
            s_interrupt = s_in_reel ? (tap_x < 120 ? INT_NEWER : INT_OLDER) : INT_SAME;
            mjpeg_abort();
            break;
        case G_SWIPE_LEFT:
        case G_SWIPE_RIGHT: s_interrupt = INT_HOME; mjpeg_abort(); break;  // → HOME
        default: break;
    }
}

// ── Incoming pulse — sender's avatar (or the plain ring) + tap-to-open ───────
// New content lands here first (history_add() has already run, so it's already
// browsable even if this pulse times out). Like a real pager: tap opens the
// content immediately; any other input drops into the normal freeze-frame/text
// view; no input at all within the configured window puts the display to sleep
// and counts the item as unread — the badge is what greets the next wake, not
// the content itself.
enum WaitOutcome { WAIT_TIMEOUT, WAIT_TAP, WAIT_CANCEL };

// Shared wait loop for both the single-item and burst-collapsed pulse screens.
// has_photo/sender only matter when animating a photo (single-item, non-burst);
// pass has_photo=false for the plain-ring/burst screens (just pumps LVGL).
static WaitOutcome wait_incoming(bool has_photo, const char* sender) {
    uint32_t timeout_ms = (uint32_t)s_settings.incoming_timeout_sec * 1000UL;
    uint32_t start = millis();
    while ((millis() - start) < timeout_ms) {
        if (has_photo) {
            float phase = (float)((millis() - start) % 1400UL) / 1400.0f;
            ui_render_incoming_photo(sender, phase);
        } else {
            display_tick();   // pump the plain-ring LVGL animation
        }
        haptics_poll();

        Gesture g = poll_gesture();
        if (g == G_TAP)  return WAIT_TAP;
        if (g != G_NONE) return WAIT_CANCEL;                // any other gesture → freeze-frame

        ButtonEvent b = buttons_tick();
        if (b == ButtonEvent::PWR_HOLD) { imu_sleep_mode(); power_go_to_sleep(); }
        if (b != ButtonEvent::NONE)     return WAIT_CANCEL;  // any button → freeze-frame

        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return WAIT_TIMEOUT;
}

static void show_incoming_and_wait(const VideoMsg& msg) {
    bool has_photo = mjpeg_fetch_avatar(msg.sender_id);
    if (has_photo) ui_enter_incoming_photo();
    else           ui_show_incoming(msg.sender[0] ? msg.sender : nullptr);
    power_reset_idle_timer();

    WaitOutcome r = wait_incoming(has_photo, msg.sender);
    if (r == WAIT_TAP) {
        show_frame();                          // history cursor is already newest
        if (s_cur_type == 'V') play_current();  // video/sticker → play immediately
        // text: show_frame() already fully shows it — nothing more to do.
    } else if (r == WAIT_CANCEL) {
        show_frame();                          // drop into the normal freeze-frame/text view
    } else {
        // Timed out untouched — count as unread and sleep, pager-style.
        s_unread_count++;
        ui_set_unread(s_unread_count);
        go_home();
        if (!power_display_dimmed()) power_toggle_display();
    }
}

// Several items arrived while we weren't looking (mid-pulse, or several queued
// up before loop() got to them) — collapse into ONE "N new messages" pulse
// instead of pulsing once per item. Which one to auto-play on tap is ambiguous
// for a burst, so tap/cancel both just land on the newest freeze-frame/text
// (already the history cursor) for the user to flip through themselves.
static void show_incoming_burst_and_wait(int n) {
    char label[32];
    snprintf(label, sizeof(label), "%d new messages", n);
    ui_show_incoming(label);
    power_reset_idle_timer();

    WaitOutcome r = wait_incoming(false, nullptr);
    if (r == WAIT_TAP || r == WAIT_CANCEL) {
        show_frame();
    } else {
        s_unread_count += n;
        ui_set_unread(s_unread_count);
        go_home();
        if (!power_display_dimmed()) power_toggle_display();
    }
}

// ── Settings ────────────────────────────────────────────────────────────────
static void open_settings() {
    ui_settings_open(s_settings);
    s_settings_entry_lock = true;              // ignore the opening touch until release
    power_reset_idle_timer();
}
static void close_settings() {
    s_settings = ui_settings_values();
    nvs_save_settings(s_settings);
    haptic_set_enabled(s_settings.haptics);
    imu_configure({ s_settings.tap_sleep, s_settings.adaptive_timeout, s_settings.orient_flip });
    ui_settings_close();
    go_home();
}

// Settings drag-wheel touch (no LVGL indev). Only called while settings is active.
// A horizontal swipe (either direction) exits to HOME, matching HISTORY's swipe-
// left/right convention; a predominantly-vertical drag scrolls the wheel as before.
static void handle_settings_touch() {
    static uint32_t poll_ms   = 0;
    static bool     down      = false;
    static int      start_x   = 0, start_y = 0, last_x = 0, last_y = 0;
    static uint32_t down_ms   = 0;
    static bool     moved     = false;
    static int      last_sel  = -1;

    uint32_t now = millis();
    if (now - poll_ms < SET_TOUCH_POLL_MS) return;
    poll_ms = now;

    int x = 0, y = 0;
    int c = touch_read_contact(&x, &y);
    if (c < 0) return;
    bool press = (c == 1);

    // Swallow the finger that opened settings until it lifts.
    if (s_settings_entry_lock) { if (!press) s_settings_entry_lock = false; return; }

    if (press && !down) {
        down = true; start_x = last_x = x; start_y = last_y = y; down_ms = now; moved = false;
        last_sel = ui_settings_centered_row();
    } else if (press && down) {
        if (abs(x - start_x) > SET_MOVE_THRESH_PX || abs(y - start_y) > SET_MOVE_THRESH_PX) moved = true;
        int dy = y - last_y;
        // Only scroll on a predominantly-vertical drag — once horizontal travel
        // crosses the swipe threshold this is headed for an exit-swipe instead,
        // so don't also nudge the wheel underneath it.
        if (moved && dy != 0 && abs(x - start_x) < GEST_SWIPE_THRESHOLD) {
            int sel = ui_settings_scroll_by(dy);
            if (sel != last_sel) { last_sel = sel; haptic_click(); }
        }
        last_x = x; last_y = y;
    } else if (!press && down) {
        down = false;
        int net_dx = last_x - start_x, net_dy = last_y - start_y;
        if (abs(net_dx) > GEST_SWIPE_THRESHOLD && abs(net_dx) > abs(net_dy)) {
            haptic_tick();
            close_settings();   // horizontal swipe → exit (either direction)
        } else if (moved) {
            ui_settings_snap();
        } else {
            UiSettingsAction a = ui_settings_tap(start_y);
            haptic_click();
            if      (a == UI_SET_SYNC) time_sync_force();
            else if (a == UI_SET_EXIT) close_settings();
        }
    }
}

// ── HOME / HISTORY input (buttons + gestures) ───────────────────────────────
static void handle_home_history_input() {
    Gesture g = poll_gesture();
    ButtonEvent b = buttons_tick();

    // While the metadata overlay is up, any input just dismisses it.
    if (s_meta_shown) {
        if (g != G_NONE || b != ButtonEvent::NONE) { ui_hide_metadata(); s_meta_shown = false; }
        return;
    }

    switch (g) {
        case G_LONGPRESS:
            if (s_state == PS_HOME)         { open_settings(); haptic_tick(); }
            else if (s_state == PS_HISTORY) { show_metadata(); }
            break;
        case G_SWIPE_UP:    nav_older(); break;
        case G_SWIPE_DOWN:  nav_newer(); break;
        case G_SWIPE_LEFT:
        case G_SWIPE_RIGHT: if (s_state == PS_HISTORY) go_home(); break;
        case G_TAP:
            if      (s_state == PS_HOME)    play_unread_sequence();   // unread → catch-up reel
            else if (s_state == PS_HISTORY) play_current();
            break;
        default: break;
    }
    switch (b) {
        case ButtonEvent::BTN1_SHORT: nav_older(); break;
        case ButtonEvent::BTN2_SHORT: nav_newer(); break;
        case ButtonEvent::BTN1_HOLD:
        case ButtonEvent::BTN2_HOLD:  if (s_state == PS_HISTORY) play_current(); break;
        case ButtonEvent::PWR_SHORT:  power_toggle_display(); break;
        case ButtonEvent::PWR_HOLD:   imu_sleep_mode(); power_go_to_sleep(); break;
        default: break;
    }
}

// ── Clock restore (RTC → NVS → build floor) ─────────────────────────────────
static void restore_clock() {
    if (rtc_init()) {
        time_t t = rtc_read();
        if (t > 0) { struct timeval tv = { .tv_sec = t }; settimeofday(&tv, nullptr); return; }
    }
    time_t nv = nvs_load_time();
    if (nv > (time_t)1700000000) {
        struct timeval tv = { .tv_sec = nv };
        settimeofday(&tv, nullptr);
        rtc_write(nv);
    }
}

// ── Setup ───────────────────────────────────────────────────────────────────
void setup() {
#ifdef DEBUG_SERIAL
    Serial.begin(115200);
    Serial.setTxTimeoutMs(0);
    delay(300);
    Serial.println("[BOOT] TelePager");
#endif

    hardware_init();
    nvs_init();
    history_init();
    time_sync_init();
    restore_clock();

    if (!display_init()) {
#ifdef DEBUG_SERIAL
        Serial.println("[BOOT] display_init FAILED — halting");
#endif
        while (true) vTaskDelay(pdMS_TO_TICKS(1000));
    }
    ui_init();
    display_on();
    ui_show_idle();
    s_state = PS_HOME;

    buttons_init();
    touch_init();
    imu_init();
    battery_init();
    haptics_init();
    mjpeg_player_init();
    mjpeg_set_poll(render_poll);
    ui_settings_init();

    s_settings = nvs_load_settings();
    haptic_set_enabled(s_settings.haptics);
    imu_configure({ s_settings.tap_sleep, s_settings.adaptive_timeout, s_settings.orient_flip });

    s_video_queue = xQueueCreate(16, sizeof(VideoMsg));
    sse_client_init(s_video_queue);
    wifi_mgr_begin();

    xTaskCreatePinnedToCore(sse_client_task, "sse", 8192, nullptr, 3, nullptr, 0);
    xTaskCreatePinnedToCore(heartbeat_task, "hb",  4096, nullptr, 1, nullptr, 0);

    power_reset_idle_timer();
}

// ── Main loop (core 1, single display owner; renderer runs inline in play_video) ─
void loop() {
    display_tick();
    haptics_poll();

    uint32_t now = millis();

    if (ui_settings_active()) {
        handle_settings_touch();
        power_reset_idle_timer();
        switch (buttons_tick()) {              // power still works in settings
            case ButtonEvent::PWR_SHORT: power_toggle_display(); break;
            case ButtonEvent::PWR_HOLD:  imu_sleep_mode(); power_go_to_sleep(); break;
            default: break;
        }
    } else {
        handle_home_history_input();
    }

    // New video/text message(s) from the SSE task → snap history to newest, buzz,
    // and show the incoming pulse (sender's photo, tap-to-open). Per spec: a
    // pager-style pulse, not an auto-play and not an immediate freeze-frame.
    // Drain the WHOLE queue now (not just one message): if more than one arrived
    // — a burst, or several queued up while a previous pulse was showing —
    // collapse them into a single "N new messages" pulse instead of pulsing
    // once per item.
    VideoMsg msg;
    if (xQueueReceive(s_video_queue, &msg, 0) == pdTRUE) {
        if (ui_settings_active()) close_settings();
        VideoMsg last = msg;
        char entry[HISTORY_ID_LEN];
        int  n = 0;
        for (;;) {
            if (last.type == 'T') encode_entry(entry, sizeof(entry), 'T', last.sender, last.text);
            else                  encode_entry(entry, sizeof(entry), 'V', last.sender, last.id);
            history_add(entry);
            n++;
            if (n >= 20) break;    // sane cap — history itself only keeps HISTORY_MAX anyway
            if (xQueueReceive(s_video_queue, &last, 0) != pdTRUE) break;
        }
        ui_set_last_sender(last.sender);
        haptic_incoming();   // gentle buzz on arrival — same for video and text
        if (n == 1) show_incoming_and_wait(msg);
        else        show_incoming_burst_and_wait(n);
    }

    // IMU: motion-wake + orientation flip (HOME/HISTORY only; PLAYING blocks loop).
    static uint32_t s_last_imu = 0;
    if (!ui_settings_active() && (now - s_last_imu >= IMU_POLL_MS)) {
        s_last_imu = now;
        if (imu_is_moving()) power_reset_idle_timer();
        bool f = imu_is_flipped(s_flipped);
        if (f != s_flipped) {
            s_flipped = f;
            display_set_rotation(f ? 180 : 0);
            touch_set_flipped(f);
        }
    }

    // 1 Hz housekeeping: clock, status, NTP, settings status rows.
    static uint32_t s_last_1hz = 0;
    if (now - s_last_1hz >= 1000) {
        s_last_1hz = now;
        time_sync_tick();
        BatteryStatus bat = battery_read();
        ui_set_status(wifi_mgr_connected(), sse_client_online());
        ui_tick();

        if (ui_settings_active()) {
            ui_settings_set_battery(bat.percent, bat.charging);
            const char* txt; int st;
            switch (time_sync_state()) {
                case TimeSyncState::OK:      txt = "Synced";      st = 1; break;
                case TimeSyncState::SYNCING: txt = "Syncing";     st = 3; break;
                default:                     txt = "Tap to sync"; st = 0; break;
            }
            ui_settings_set_sync_status(txt, st);
        }
    }

    power_tick();
    vTaskDelay(pdMS_TO_TICKS(5));
}
