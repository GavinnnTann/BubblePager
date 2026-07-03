#pragma once
#include <stdint.h>
#include <stdbool.h>

// WiFi station manager. Connects to the SSID/pass from config.h and keeps the
// link up — the SSE listener and MJPEG player depend on a live connection.

// Begin association (non-blocking). Call once in setup().
void wifi_mgr_begin();

// Call periodically (e.g. from the SSE task loop). Re-issues a reconnect if the
// link has dropped, rate-limited to WIFI_RECONNECT_MS. Cheap when connected.
void wifi_mgr_tick();

// True once associated with an IP.
bool wifi_mgr_connected();

// Block until connected or timeout_ms elapses. Returns the connection state.
bool wifi_mgr_wait(uint32_t timeout_ms);
