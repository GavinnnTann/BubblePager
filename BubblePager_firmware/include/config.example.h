#pragma once

// ── TelePager configuration ─────────────────────────────────────────────────
// Secrets live here (WiFi + server). This file is gitignored — commit
// config.example.h instead. XSTR() stringifies SERVER_PORT into SERVER_URL.

#define XSTR_(x) #x
#define XSTR(x)  XSTR_(x)

// ── WiFi ────────────────────────────────────────────────────────────────────
#define WIFI_SSID     "your_ssid"
#define WIFI_PASS     "your_password"

// ── Server (Python FastAPI service) ─────────────────────────────────────────
#define SERVER_HOST   "192.168.1.100"      // LAN IP or VPS hostname
#define SERVER_PORT   8080
#define SERVER_URL    "http://" SERVER_HOST ":" XSTR(SERVER_PORT)

// SSE reconnect backoff (ms) — exponential 1s → cap. Server can restart; the
// ESP32 must silently recover (gotcha #10).
#define SSE_BACKOFF_START_MS   1000
#define SSE_BACKOFF_MAX_MS    30000
#define SSE_CONNECT_TIMEOUT_MS 8000    // give up a single /events dial after this

// MJPEG stream
#define STREAM_CONNECT_TIMEOUT_MS 8000
#define MJPEG_FRAME_MAX_BYTES  (48 * 1024)   // PSRAM frame accumulator (q=5 ≈ 10-20 KB)

// ── Device identity + heartbeat (server /status, /devices, played receipts) ──
#define DEVICE_NAME     "TelePager"   // shown in /devices and "Played on <name>"
#define HEARTBEAT_MS    30000         // POST battery/signal to /device/heartbeat this often

// ── WiFi connect ────────────────────────────────────────────────────────────
#define WIFI_CONNECT_TIMEOUT_MS  15000   // initial association timeout
#define WIFI_RECONNECT_MS         5000   // retry cadence once dropped

// ── Display / power ─────────────────────────────────────────────────────────
#define BACKLIGHT_DUTY            200    // 0-255 ledc duty for normal brightness

// Idle display auto-off. The pager is passive, so the screen dims out after a
// while and wakes on a video or a button. 0 disables the timeout.
#define DISPLAY_TIMEOUT_SEC      120     // dim the backlight after 2 min idle
#define DISPLAY_TIMEOUT_MIN        5
#define DISPLAY_TIMEOUT_MAX     3600

// DONE state: freeze on the last video frame before fading back to idle.
#define DONE_FREEZE_MS           2000

// Unread catch-up reel (tap HOME with unread mail → auto-play oldest→newest):
// how long a TEXT entry in the sequence is shown before auto-advancing (videos
// advance on their own natural playback-finished signal, this is the text
// equivalent — any input during the pause stops the sequence immediately).
#define UNREAD_TEXT_PAUSE_MS     3000

// ── Buttons ─────────────────────────────────────────────────────────────────
#define BTN_CHORD_MS             5000    // both buttons held (reserved for future use)

// ── Haptics (vibration motor — strength + pattern feel) ─────────────────────
#define HAPTIC_PWM_HZ           20000    // PWM freq — inaudible; full duty = steady-on
#define HAPTIC_DUTY               255    // 0-255 motor strength
#define HAPTIC_TICK_MS            40     // confirmation pulse (e.g. done)
#define HAPTIC_CLICK_MS           18     // short nav/detent pulse
#define HAPTIC_DOUBLE_ON_MS       40     // alert pattern: each pulse length
#define HAPTIC_DOUBLE_GAP_MS      70     // alert pattern: gap between pulses
#define HAPTIC_SLEEP_MS           50     // blocking "goodbye" pulse on deep sleep

// Incoming-video buzz: [100ms ON, 50ms OFF] x3 (CLAUDE.md PATTERN_INCOMING).
#define HAPTIC_INCOMING_ON_MS    100
#define HAPTIC_INCOMING_OFF_MS    50
#define HAPTIC_INCOMING_REPS       3

// ── Text messages (History carousel text screen) ─────────────────────────────
// Must match TEXT_MSG_MAX_CHARS in the server's bot.py — the server REJECTS (does
// not truncate) anything over this, so the firmware never has to defensively trim
// what it renders. VideoMsg.text/history entries just need headroom past it.
#define TEXT_MSG_MAX_CHARS 100

// ── IMU (QMI8658 — motion wake + orientation flip, settings page) ────────────
#define IMU_MOTION_THRESHOLD     600     // per-axis vector delta (LSB @ ±8g) = motion
#define IMU_MOTION_DEBOUNCE        3     // consecutive above-thresh polls before "moving"
#define IMU_POLL_MS              500     // how often the idle loop samples the IMU

// ── Settings wheel geometry (center-locked picker, FobBob-style) ─────────────
#define SET_ROW_W                196     // row pill width (px)
#define SET_ROW_H                 40     // row height (px)
#define SET_ROW_GAP                6     // vertical gap between rows (px)
#define SET_VIEW_W               240     // scroll viewport width (px)
#define SET_VIEW_H               200     // scroll viewport height (px)
// Touch feel (manual drag wheel — main feeds CST816 deltas to ui_settings_*).
#define SET_TOUCH_POLL_MS         20     // touch sample cadence while a UI screen is up
#define SET_MOVE_THRESH_PX         8     // finger travel that turns a tap into a drag
#define SET_LONGPRESS_MS         500     // hold that opens settings / long-press gesture
#define SET_DOUBLETAP_MS         400     // idle double-tap window (tap-to-sleep)

// ── Gesture + button interaction (HOME/HISTORY/PLAYING state machine) ────────
#define GEST_SWIPE_DEADZONE       10     // px travel still treated as a stationary tap
#define GEST_SWIPE_THRESHOLD      40     // px travel that commits a swipe
#define GEST_TAP_MAX_MS          400     // longest stationary contact still a tap
#define BTN_HOLD_MS              500     // button hold → play the shown video
#define BTN_PWR_HOLD_MS         1000     // power hold → shutdown (deep sleep)
