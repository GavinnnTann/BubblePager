#include "device_report.h"
#include "wifi_mgr.h"
#include "battery/battery.h"
#include "config.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// Stable device id from the eFuse MAC (hex), cached on first use.
static const char* device_id() {
    static char id[13] = {};
    if (!id[0]) {
        uint64_t mac = ESP.getEfuseMac();
        snprintf(id, sizeof(id), "%012llX", (unsigned long long)mac);
    }
    return id;
}

// Minimal HTTP/1.1 POST of a JSON body. Fire-and-forget: we drain the response
// briefly so the socket closes cleanly, but ignore its content. Returns false on
// connect failure. Short timeouts so a bad server can't stall the caller.
static bool http_post_json(const char* path, const String& body) {
    if (!wifi_mgr_connected()) return false;
    WiFiClient c;
    if (!c.connect(SERVER_HOST, SERVER_PORT, 3000)) return false;
    c.printf("POST %s HTTP/1.1\r\n"
             "Host: %s\r\n"
             "Content-Type: application/json\r\n"
             "Content-Length: %u\r\n"
             "Connection: close\r\n\r\n",
             path, SERVER_HOST, (unsigned)body.length());
    c.print(body);

    uint32_t t = millis();
    while (c.connected() && (millis() - t) < 2000) {
        while (c.available()) c.read();
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    c.stop();
    return true;
}

static String json_escape(const char* s) {
    String out;
    for (const char* p = s; p && *p; p++) {
        char ch = *p;
        if (ch == '"' || ch == '\\') { out += '\\'; out += ch; }
        else if (ch >= 0x20)         { out += ch; }
    }
    return out;
}

void heartbeat_task(void*) {
    // Small initial delay so WiFi + first battery read settle.
    vTaskDelay(pdMS_TO_TICKS(4000));
    for (;;) {
        if (wifi_mgr_connected()) {
            BatteryStatus b = battery_last();   // cached — no ADC race with the main task
            String body = String("{\"id\":\"") + device_id() +
                          "\",\"name\":\"" + json_escape(DEVICE_NAME) +
                          "\",\"battery\":" + (int)b.percent +
                          ",\"charging\":" + (b.charging ? "true" : "false") +
                          ",\"rssi\":" + (int)WiFi.RSSI() +
                          ",\"uptime\":" + (unsigned long)(millis() / 1000) + "}";
            http_post_json("/device/heartbeat", body);
        }
        vTaskDelay(pdMS_TO_TICKS(HEARTBEAT_MS));
    }
}

void device_ack(const char* video_id) {
    if (!video_id || !video_id[0]) return;
    String body = String("{\"video_id\":\"") + json_escape(video_id) +
                  "\",\"device_name\":\"" + json_escape(DEVICE_NAME) + "\"}";
    http_post_json("/device/ack", body);
}
