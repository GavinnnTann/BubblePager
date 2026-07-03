#include "wifi_mgr.h"
#include "config.h"
#include <WiFi.h>

static uint32_t s_last_retry_ms = 0;
static bool     s_begun         = false;
static uint32_t s_retry_count   = 0;

void wifi_mgr_begin() {
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.setSleep(false);          // keep the radio hot for low-latency SSE/streaming
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    s_begun         = true;
    s_last_retry_ms = millis();
#ifdef DEBUG_SERIAL
    Serial.printf("[WIFI] connecting to '%s'\n", WIFI_SSID);
#endif
}

bool wifi_mgr_connected() {
    return WiFi.status() == WL_CONNECTED;
}

void wifi_mgr_tick() {
    if (!s_begun) return;
    if (wifi_mgr_connected()) { s_retry_count = 0; return; }
    if ((millis() - s_last_retry_ms) < WIFI_RECONNECT_MS) return;
    s_last_retry_ms = millis();
    s_retry_count++;

    // Don't tear down a connection that's mid-handshake. setAutoReconnect(true) is
    // already retrying under us; a WiFi.disconnect()+begin() every 5 s just aborts
    // the association before it completes (the old code's reconnect thrash). Nudge
    // with WiFi.reconnect() normally, and only do a full begin() every 6th attempt
    // (~30 s) in case the driver has fully given up (e.g. AP was power-cycled).
#ifdef DEBUG_SERIAL
    wl_status_t st = WiFi.status();
    Serial.printf("[WIFI] link down (status=%d, rssi=%d) — reconnect #%u\n",
                  (int)st, (int)WiFi.RSSI(), (unsigned)s_retry_count);
#endif
    if (s_retry_count % 6 == 0) {
        WiFi.disconnect(false, true);   // clear + re-issue association
        WiFi.begin(WIFI_SSID, WIFI_PASS);
    } else {
        WiFi.reconnect();
    }
}

bool wifi_mgr_wait(uint32_t timeout_ms) {
    uint32_t start = millis();
    while (!wifi_mgr_connected() && (millis() - start) < timeout_ms) {
        delay(100);
    }
    return wifi_mgr_connected();
}
