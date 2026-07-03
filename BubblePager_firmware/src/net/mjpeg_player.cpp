#include "mjpeg_player.h"
#include "wifi_mgr.h"
#include "display/display.h"
#include "config.h"
#include <WiFi.h>
#include <WiFiClient.h>
#include <TJpg_Decoder.h>
#include <ArduinoJson.h>
#include <math.h>

// PSRAM frame accumulator — one JPEG frame at a time (q=5 ≈ 10-20 KB, cap 48 KB).
static uint8_t*       s_frame     = nullptr;
static size_t         s_frame_len = 0;
// Freeze-frame thumbnail: 240×240 RGB565 in PSRAM, decoded from /thumb/<id>.
static uint16_t*      s_thumb     = nullptr;
static volatile bool  s_abort     = false;
static volatile bool  s_paused    = false;
static volatile bool  s_pause_dirty = false;   // pause state changed → redraw overlay
static void         (*s_poll)()   = nullptr;
// X-Frame-Count of the most recent mjpeg_play() (0 = unknown/absent). Lets a
// caller tell a real (multi-frame) video/sticker apart from a static (1-frame)
// sticker AFTER headers arrive but before/as the first frame decodes — e.g. to
// skip a "playback started" haptic for something that isn't really playing.
static uint32_t       s_last_total_frames = 0;

// ── Progress arc, composited INTO the video (Telegram-style depleting ring) ──
// The arc is painted into each MCU block as it is decoded, so every frame that
// reaches the panel already contains it. That removes the flicker of the old
// approach (dots blitted OVER the video strobed, because the next full frame
// erased them). Solid white band near the panel edge, like the battery ring.
// Band radii (px). Outer sits at the panel edge (radius 120) so the arc is flush.
static constexpr float ARC_RIN     = 110.0f;
static constexpr float ARC_ROUT    = 120.0f;
static constexpr float ARC_FEATHER = 5.0f;     // rounded end-cap softness (degrees)
static constexpr int   ARC_GIN2    = 108 * 108; // guard band (radial feather ±)
static constexpr int   ARC_GOUT2   = 122 * 122;
static volatile bool   s_arc_on    = false;     // draw the arc this playback?
static volatile float  s_arc_deg   = 0.0f;      // remaining span, degrees clockwise from top

static inline float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

// Alpha-blend an RGB565 pixel toward white by coverage (0..1). Interpreting the
// bits directly is valid: TJpgDec output is native RGB565 and display_blit() swaps
// the whole buffer uniformly afterwards, so a blended pixel stays consistent.
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

// TJpg decode callback: composite the progress arc into the RGB565 MCU block, then
// push it straight to the panel. Waiting on the DMA is required because TJpgDec
// reuses `bitmap` for the next block and display_blit() kicks the transfer async.
// The arc is anti-aliased (soft radial edges + rounded end caps) so it reads as a
// smooth ring, not a stack of rectangles.
static bool jpeg_blit_cb(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bitmap) {
    if (y >= 240) return true;                 // off-panel guard
    if (s_arc_on) {
        for (int r = 0; r < h; r++) {
            int py = y + r; if (py < 0 || py >= 240) continue;
            int dy = py - 120;
            for (int c = 0; c < w; c++) {
                int px = x + c; if (px < 0 || px >= 240) continue;
                int dx = px - 120;
                int d2 = dx * dx + dy * dy;
                if (d2 < ARC_GIN2 || d2 > ARC_GOUT2) continue;   // outside the band
                float d = sqrtf((float)d2);
                // Radial coverage: 1 inside the band, fading over ~1 px at each edge.
                float cov_r = clampf(fminf(d - ARC_RIN, ARC_ROUT - d) + 0.5f, 0.0f, 1.0f);
                if (cov_r <= 0.0f) continue;

                // Angle clockwise from the top (12 o'clock); y grows downward.
                float fromTop = atan2f((float)dy, (float)dx) * (180.0f / (float)M_PI) + 90.0f;
                if (fromTop < 0) fromTop += 360.0f;

                float cov_a;
                if (s_arc_deg >= 359.0f) {
                    cov_a = 1.0f;               // full ring — no seam notch at the top
                } else {
                    if (fromTop > s_arc_deg) continue;
                    // Rounded caps: fade over ARC_FEATHER° at BOTH ends of the span.
                    float da = fminf(fromTop, s_arc_deg - fromTop);
                    cov_a = da < ARC_FEATHER ? da / ARC_FEATHER : 1.0f;
                }

                float cov = cov_r * cov_a;
                if (cov <= 0.02f) continue;
                int idx = r * w + c;
                bitmap[idx] = blend_white(bitmap[idx], cov);
            }
        }
    }
    display_blit(x, y, (int16_t)w, (int16_t)h, bitmap);
    display_blit_wait();
    return !s_abort;                           // false aborts the decode early
}

bool mjpeg_player_init() {
    if (!s_frame) {
        s_frame = (uint8_t*)heap_caps_malloc(MJPEG_FRAME_MAX_BYTES, MALLOC_CAP_SPIRAM);
    }
    TJpgDec.setJpgScale(1);
    TJpgDec.setSwapBytes(false);   // display_blit() handles the RGB565 byte swap
    TJpgDec.setCallback(jpeg_blit_cb);
    return s_frame != nullptr;
}

void mjpeg_abort() { s_abort = true; }
void mjpeg_set_poll(void (*cb)()) { s_poll = cb; }
void mjpeg_toggle_pause() { s_paused = !s_paused; s_pause_dirty = true; }
bool mjpeg_is_paused() { return s_paused; }

// Set the arc span shown while the NEXT frame is composited (progress before this
// frame). total==0 (no X-Frame-Count header) → no arc.
static void set_arc(uint32_t played, uint32_t total) {
    if (total == 0) { s_arc_on = false; return; }
    float rem = 1.0f - (float)played / (float)total;
    if (rem < 0) rem = 0;
    s_arc_deg = rem * 360.0f;
    s_arc_on  = true;
}

// Two vertical bars in the centre — a pause glyph. Drawn once when paused; the
// next frame overwrites it on resume.
static void draw_pause_glyph() {
    static uint16_t bar[10 * 44];
    for (int i = 0; i < 10 * 44; i++) bar[i] = 0xFFFF;
    display_blit(108, 98, 10, 44, bar); display_blit_wait();
    display_blit(122, 98, 10, 44, bar); display_blit_wait();
}

// Decode one complete JPEG in s_frame[0..s_frame_len) to the panel.
static bool decode_frame() {
    if (s_frame_len < 4) return false;
    return TJpgDec.drawJpg(0, 0, s_frame, s_frame_len) == JDR_OK;
}

// ── Freeze-frame thumbnails ──────────────────────────────────────────────────
// TJpg callback for thumbnails: write each MCU block into the s_thumb RGB565
// buffer (native order) instead of the panel — the UI shows it via an lv_image.
static bool thumb_cb(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bitmap) {
    if (!s_thumb) return false;
    for (int r = 0; r < h; r++) {
        int py = y + r; if (py < 0 || py >= 240) continue;
        for (int c = 0; c < w; c++) {
            int px = x + c; if (px < 0 || px >= 240) continue;
            s_thumb[py * 240 + px] = bitmap[r * w + c];
        }
    }
    return true;
}

// GET `path` and read the response body into s_frame. Returns the body length
// (bounded by MJPEG_FRAME_MAX_BYTES), 0 on failure.
static size_t http_get_body(const char* path) {
    if (!wifi_mgr_connected() || !s_frame) return 0;
    WiFiClient client;
    if (!client.connect(SERVER_HOST, SERVER_PORT, STREAM_CONNECT_TIMEOUT_MS)) return 0;
    client.printf("GET %s HTTP/1.1\r\nHost: " SERVER_HOST "\r\nConnection: close\r\n\r\n", path);

    long content_len = -1;
    client.setTimeout(STREAM_CONNECT_TIMEOUT_MS);
    while (client.connected()) {                    // headers
        String line = client.readStringUntil('\n');
        if (line.length() == 0) break;
        line.trim();
        if (line.isEmpty()) break;
        int colon = line.indexOf(':');
        if (colon > 0) {
            String key = line.substring(0, colon); key.toLowerCase();
            if (key == "content-length") content_len = line.substring(colon + 1).toInt();
        }
    }

    size_t len = 0;
    uint32_t last = millis();
    while (client.connected() || client.available()) {
        int n = client.available();
        if (n <= 0) {
            if ((millis() - last) > STREAM_CONNECT_TIMEOUT_MS) break;
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }
        size_t room = MJPEG_FRAME_MAX_BYTES - len;
        if ((size_t)n > room) n = (int)room;
        int got = client.read(s_frame + len, n);
        if (got <= 0) continue;
        len += got;
        last = millis();
        if (content_len > 0 && len >= (size_t)content_len) break;
        if (len >= MJPEG_FRAME_MAX_BYTES) break;
    }
    client.stop();
    return len;
}

// Shared by mjpeg_fetch_thumb() and mjpeg_fetch_avatar(): GET `path`, decode the
// single JPEG response into s_thumb (240×240 RGB565). Both are "one still image"
// fetches — only the URL and the debug tag differ.
static bool fetch_and_decode_still(const char* path, const char* dbg_tag) {
    if (!s_frame) return false;
    if (!s_thumb) s_thumb = (uint16_t*)heap_caps_malloc(240 * 240 * 2, MALLOC_CAP_SPIRAM);
    if (!s_thumb) return false;

    // heap_caps_malloc does NOT zero memory — a failed fetch/decode below must not
    // leave the buffer showing whatever garbage was already in PSRAM (that's what
    // read as full-screen colour static: the decode was silently failing and the
    // caller blitted the uninitialized buffer anyway).
    memset(s_thumb, 0, 240 * 240 * 2);

    size_t len = http_get_body(path);
#ifdef DEBUG_SERIAL
    Serial.printf("[%s] %s len=%u first=%02X %02X\n", dbg_tag, path, (unsigned)len,
                  len > 0 ? s_frame[0] : 0, len > 1 ? s_frame[1] : 0);
#endif
    if (len < 4) return false;

    s_arc_on = false;                     // no progress arc on a still frame
    TJpgDec.setCallback(thumb_cb);
    bool ok = TJpgDec.drawJpg(0, 0, s_frame, len) == JDR_OK;
    TJpgDec.setCallback(jpeg_blit_cb);    // restore the video callback
#ifdef DEBUG_SERIAL
    Serial.printf("[%s] decode %s\n", dbg_tag, ok ? "OK" : "FAILED");
#endif
    if (!ok) memset(s_thumb, 0, 240 * 240 * 2);   // decode failed → stay black, not noise
    return ok;
}

bool mjpeg_fetch_thumb(const char* video_id) {
    char path[96];
    snprintf(path, sizeof(path), "/thumb/%s", video_id);
    return fetch_and_decode_still(path, "THUMB");
}

bool mjpeg_fetch_avatar(const char* sender_id) {
    if (!sender_id || !sender_id[0]) return false;
    char path[64];
    snprintf(path, sizeof(path), "/avatar/%s", sender_id);
    return fetch_and_decode_still(path, "AVATAR");
}

uint16_t* mjpeg_thumb_buffer() { return s_thumb; }

uint32_t mjpeg_last_total_frames() { return s_last_total_frames; }

bool mjpeg_fetch_meta(const char* video_id, char* sender, size_t slen,
                      char* created, size_t clen, int* duration_sec) {
    if (!s_frame) return false;
    char path[96];
    snprintf(path, sizeof(path), "/meta/%s", video_id);
    size_t len = http_get_body(path);
    if (len < 2) return false;

    JsonDocument doc;
    if (deserializeJson(doc, (const char*)s_frame, len)) return false;
    if (sender)   { const char* s = doc["sender"] | "";       strncpy(sender, s, slen - 1);  sender[slen - 1] = 0; }
    if (created)  { const char* c = doc["created_at"] | "";   strncpy(created, c, clen - 1); created[clen - 1] = 0; }
    if (duration_sec) *duration_sec = (int)lroundf((float)(doc["duration_sec"] | 0.0f));
    return true;
}

// Read HTTP status line + headers, returning the value of X-Frame-Count (0 if
// absent). Leaves the client positioned at the start of the response body.
static uint32_t read_headers(WiFiClient& client) {
    uint32_t total = 0;
    client.setTimeout(STREAM_CONNECT_TIMEOUT_MS);
    while (client.connected()) {
        String line = client.readStringUntil('\n');
        if (line.length() == 0) break;              // read timed out
        line.trim();                                // drop trailing '\r'
        if (line.isEmpty()) break;                  // blank line → end of headers
        int colon = line.indexOf(':');
        if (colon > 0) {
            String key = line.substring(0, colon);
            key.toLowerCase();
            if (key == "x-frame-count") {
                total = (uint32_t)line.substring(colon + 1).toInt();
            }
        }
    }
    return total;
}

MjpegResult mjpeg_play(const char* video_id, void (*on_first_frame)()) {
    s_abort = false;
    s_paused = false;
    s_pause_dirty = false;
    if (!s_frame) return MjpegResult::NO_MEM;
    if (!wifi_mgr_connected()) return MjpegResult::CONNECT_FAIL;

    WiFiClient client;
#ifdef DEBUG_SERIAL
    Serial.printf("[MJPEG] connecting %s:%d /stream/%s\n", SERVER_HOST, SERVER_PORT, video_id);
#endif
    if (!client.connect(SERVER_HOST, SERVER_PORT, STREAM_CONNECT_TIMEOUT_MS)) {
#ifdef DEBUG_SERIAL
        Serial.println("[MJPEG] connect FAILED");
#endif
        return MjpegResult::CONNECT_FAIL;
    }

    client.printf(
        "GET /stream/%s HTTP/1.1\r\n"
        "Host: " SERVER_HOST "\r\n"
        "Accept: multipart/x-mixed-replace, image/jpeg\r\n"
        "Connection: close\r\n"
        "\r\n", video_id);

    uint32_t total_frames = read_headers(client);   // 0 = unknown → no arc
    s_last_total_frames = total_frames;
#ifdef DEBUG_SERIAL
    Serial.printf("[MJPEG] headers done, X-Frame-Count=%u\n", total_frames);
#endif

    // Frame-marker state machine. We ignore multipart boundaries entirely and key
    // off JPEG SOI (FF D8) / EOI (FF D9) as CLAUDE.md specifies.
    bool     in_frame   = false;
    bool     prev_ff    = false;
    uint32_t frames     = 0;
    bool     first_sent = false;
    uint32_t last_rx    = millis();
    // Frame-PROGRESS watchdog, distinct from the raw-byte stall (last_rx) above.
    // A misbehaving server can keep the socket open and trickle bytes forever
    // (SSE keepalives, chunk framing, a non-MJPEG error page, or frames that never
    // assemble a complete FF D8..FF D9). last_rx resets on every byte, so it would
    // never fire — the UI would hang on the frozen incoming ring with no escape.
    // Bail out if no *decoded frame* has arrived within STREAM_CONNECT_TIMEOUT_MS,
    // whether that's the first frame (never left INCOMING) or a mid-stream stall.
    uint32_t last_frame = millis();

    static uint8_t rx[1460];

    while (client.connected() || client.available()) {
        if (s_abort) { client.stop(); return MjpegResult::ABORTED; }

        if (s_poll) s_poll();   // keep buttons + haptics + touch alive during playback

        // Paused: stop draining the socket (TCP backpressure pauses the server-
        // paced stream) and hold the last frame. Don't count paused time as a stall.
        if (s_paused) {
            if (s_pause_dirty) { draw_pause_glyph(); s_pause_dirty = false; }
            last_rx = millis();
            last_frame = millis();
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (s_pause_dirty) s_pause_dirty = false;   // just resumed

        // No decoded frame in too long → give up so we always return to idle
        // instead of freezing on the incoming ring (independent of byte activity).
        if ((millis() - last_frame) > STREAM_CONNECT_TIMEOUT_MS) {
#ifdef DEBUG_SERIAL
            Serial.printf("[MJPEG] frame watchdog: no frame for %d ms (frames=%u)\n",
                          STREAM_CONNECT_TIMEOUT_MS, frames);
#endif
            break;
        }

        int n = client.available();
        if (n <= 0) {
            if ((millis() - last_rx) > STREAM_CONNECT_TIMEOUT_MS) break;   // stalled
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }
        if (n > (int)sizeof(rx)) n = sizeof(rx);
        int got = client.read(rx, n);
        if (got <= 0) continue;
        last_rx = millis();

        for (int i = 0; i < got; i++) {
            uint8_t b = rx[i];

            if (!in_frame) {
                if (prev_ff && b == 0xD8) {          // SOI
                    in_frame    = true;
                    s_frame_len = 0;
                    s_frame[s_frame_len++] = 0xFF;
                    s_frame[s_frame_len++] = 0xD8;
                    prev_ff = false;
                    continue;
                }
                prev_ff = (b == 0xFF);
                continue;
            }

            if (s_frame_len < MJPEG_FRAME_MAX_BYTES) s_frame[s_frame_len++] = b;

            if (prev_ff && b == 0xD9) {              // EOI → frame complete
                in_frame = false;
                prev_ff  = false;
                if (s_frame_len <= MJPEG_FRAME_MAX_BYTES) {
                    // Switch to the black PLAYING screen BEFORE blitting the first
                    // frame — on_first_frame() flushes LVGL, so it must run before
                    // decode_frame() paints the panel (or it would erase it).
                    if (!first_sent) {
                        first_sent = true;
#ifdef DEBUG_SERIAL
                        Serial.printf("[MJPEG] first frame assembled, len=%u\n",
                                      (unsigned)s_frame_len);
#endif
                        if (on_first_frame) on_first_frame();
                    }
                    set_arc(frames, total_frames);   // arc composited into this frame
                    if (decode_frame()) {
                        frames++;
                        last_frame = millis();   // reset frame-progress watchdog
                    }
#ifdef DEBUG_SERIAL
                    else Serial.printf("[MJPEG] decode FAILED (len=%u)\n",
                                       (unsigned)s_frame_len);
#endif
                }
                if (s_abort) { client.stop(); return MjpegResult::ABORTED; }
                continue;
            }
            prev_ff = (b == 0xFF);
        }
    }

    client.stop();
    if (s_abort)       return MjpegResult::ABORTED;
    return frames > 0 ? MjpegResult::OK : MjpegResult::NO_FRAMES;
}
