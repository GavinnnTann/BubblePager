#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>   // size_t

// MJPEG stream player. GETs /stream/<video_id>, scans the byte stream for JPEG
// SOI/EOI markers, decodes each frame to RGB565 and blits it straight to the
// GC9A01 (bypassing LVGL — gotcha #9: frame buffer lives in PSRAM).

enum class MjpegResult : uint8_t {
    OK,            // stream played to EOF
    CONNECT_FAIL,  // couldn't open /stream/<id>
    NO_FRAMES,     // connected but no decodable frame arrived
    ABORTED,       // mjpeg_abort() was called (BTN1 skip)
    NO_MEM,        // frame buffer allocation failed
};

// Allocate the PSRAM frame buffer and wire up the JPEG decoder. Call once in setup.
bool mjpeg_player_init();

// Play the stream for video_id (blocks until EOF / abort). on_first_frame, if
// non-null, is invoked exactly once the moment the first frame is decoded — the
// caller uses it to transition INCOMING → PLAYING.
MjpegResult mjpeg_play(const char* video_id, void (*on_first_frame)());

// Request the in-flight playback to stop at the next frame boundary. Safe to call
// from another task (e.g. the button poll). Cleared at the start of mjpeg_play.
void mjpeg_abort();

// Register a callback invoked repeatedly while mjpeg_play() waits on the network.
// The renderer runs in the main loop, so this lets it keep polling buttons and
// advancing haptics during playback (and call mjpeg_abort() on BTN1 skip).
void mjpeg_set_poll(void (*cb)());

// Toggle pause. While paused, mjpeg_play() stops reading the socket — TCP
// backpressure pauses the (server-paced) stream — and freezes the last frame +
// progress arc. Call from the poll callback on a screen tap. Reset each play.
void mjpeg_toggle_pause();
bool mjpeg_is_paused();

// ── Freeze-frame thumbnails (HISTORY browsing) ──────────────────────────────
// GET /thumb/<id> (single first-frame JPEG) and decode it into a 240×240 RGB565
// PSRAM buffer for instant browsing — no MJPEG stream open. Returns false on
// network/decode error. The buffer is reused across calls.
bool      mjpeg_fetch_thumb(const char* video_id);
// The decoded thumbnail buffer (240×240 RGB565, native byte order) or nullptr.
uint16_t* mjpeg_thumb_buffer();

// X-Frame-Count of the most recent mjpeg_play() stream (0 = unknown/absent,
// 1 = a static sticker — exactly one frame, not really "playing"). Valid from
// the moment headers arrive, so it's readable inside an on_first_frame callback.
uint32_t mjpeg_last_total_frames();

// Fetch /avatar/<sender_id> (sender's Telegram profile photo, server-resized to
// 240×240) into the SAME buffer as mjpeg_fetch_thumb() — they're never needed
// at once. False on any failure (no photo / privacy-blocked / network) — the
// caller falls back to the plain pulsing ring.
bool      mjpeg_fetch_avatar(const char* sender_id);

// Fetch /meta/<id> (JSON) for the long-press overlay. Fills sender + created
// (truncated to the buffers) and *duration_sec. Returns false on network/parse
// error. Blocks briefly — call from the render loop.
bool      mjpeg_fetch_meta(const char* video_id, char* sender, size_t slen,
                           char* created, size_t clen, int* duration_sec);
