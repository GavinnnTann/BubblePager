#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// LVGL screens for the pager state machine: IDLE (clock), INCOMING (pulsing
// ring), PLAYING (black — video blits directly, no LVGL), DONE (freeze/fade).
// All calls must run on the main task (the one pumping display_tick()).

void ui_init(void);

// ── IDLE ────────────────────────────────────────────────────────────────────
void ui_show_idle(void);
// 1 Hz refresh of the idle clock / battery ring / status dot. No-op off idle.
void ui_tick(void);
// Push sensor/link state shown on the idle screen.
void ui_set_battery(int percent, bool charging);
void ui_set_status(bool wifi_ok, bool server_ok);
// Set the "Last message from <name>" line on the idle screen. Empty/null hides it.
void ui_set_last_sender(const char* name);
// Unread-message badge on the idle screen ("N new"), pager-style. n<=0 hides it.
void ui_set_unread(int n);

// ── INCOMING ────────────────────────────────────────────────────────────────
// Pulsing white ring, like Telegram's incoming video-note ring. Optional sender
// name (may be null / empty).
void ui_show_incoming(const char* sender);

// Show "+N waiting" on the incoming screen when more videos are queued behind this
// one (n<=0 hides it).
void ui_set_queue_count(int n);

// ── HISTORY (freeze-frame browsing) ─────────────────────────────────────────
// Show the decoded thumbnail (mjpeg_thumb_buffer()) with an "index/total" badge.
// Call mjpeg_fetch_thumb(id) first to populate the buffer.
void ui_show_history(int index, int total);

// Long-press metadata overlay on the HISTORY screen. `created` is the raw ISO
// timestamp (shown to the minute). Hidden when the frame changes.
void ui_show_metadata(const char* sender, const char* created, int duration_sec);
void ui_hide_metadata(void);

// Text-message entry in the History carousel — a plain LVGL screen (no photo, so
// no direct-blit needed): sender caption, word-wrapped body constrained to the
// round display's safe text zone, and the same "index/total" badge as freeze-frame
// browsing. Interleaves with video entries via the same older/newer navigation.
void ui_show_text(const char* sender, const char* text, int index, int total);

// ── Incoming pulse — sender's avatar with a pulsing ring ────────────────────
// Call ui_enter_incoming_photo() once (switches to the direct-blit backdrop),
// then ui_render_incoming_photo() repeatedly (~30ms) with phase cycling 0→1→0
// to animate a "sonar ping" ring around the avatar (mjpeg_thumb_buffer(), already
// populated by mjpeg_fetch_avatar()). Caller decides how long to animate for and
// what a tap/timeout does next; this only draws.
void ui_enter_incoming_photo(void);
void ui_render_incoming_photo(const char* sender, float phase);

// ── PLAYING ─────────────────────────────────────────────────────────────────
// Black screen flushed once so nothing from LVGL bleeds under the video. After
// this returns the caller stops pumping LVGL and blits frames via display_blit().
void ui_show_playing(void);

// ── DONE ────────────────────────────────────────────────────────────────────
// Called after the last frame. The frame is already on the panel; this just
// fades back to IDLE after DONE_FREEZE_MS (handled by the caller's timing).
void ui_show_done(void);

// True while IDLE is the active screen (gates ui_tick work).
bool ui_idle_active(void);

#ifdef __cplusplus
}
#endif
