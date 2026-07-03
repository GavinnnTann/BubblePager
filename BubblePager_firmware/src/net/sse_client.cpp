#include "sse_client.h"
#include "wifi_mgr.h"
#include "config.h"
#include <WiFi.h>
#include <WiFiClient.h>

static QueueHandle_t   s_queue   = nullptr;
static volatile bool   s_online  = false;

void sse_client_init(QueueHandle_t video_queue) {
    s_queue = video_queue;
}

bool sse_client_online() { return s_online; }

// Parse one SSE data line and post it to the queue. Wire format (see events.py):
//   V|<video_id>|<sender>|<sender_id>   video note / sticker
//   T|<sender>|<sender_id>|<text>       plain text (text is LAST — the only
//                                       field allowed to contain '|'; the first
//                                       3 pipes are structural, the rest of the
//                                       string is kept verbatim as the text)
// Ignores empty/malformed lines and comment/keepalive lines (which never start
// with "data:" so they never reach here — see run_once()).
static void emit_message(const String& value) {
    String v = value;
    v.trim();
    if (v.isEmpty()) return;

    int p1 = v.indexOf('|');
    if (p1 < 0) return;                       // no type tag — malformed, drop
    String type = v.substring(0, p1);
    String rest = v.substring(p1 + 1);
    int p2 = rest.indexOf('|');
    String f2   = (p2 >= 0) ? rest.substring(0, p2) : rest;      // sender (T) / video_id (V)
    String tail = (p2 >= 0) ? rest.substring(p2 + 1) : String();

    VideoMsg msg = {};
    if (type == "T") {
        int p3 = tail.indexOf('|');
        String sid  = (p3 >= 0) ? tail.substring(0, p3) : tail;
        String text = (p3 >= 0) ? tail.substring(p3 + 1) : String();
        String sender = f2; sender.trim(); sid.trim();
        if (text.length() >= sizeof(msg.text)) text.remove(sizeof(msg.text) - 1);
        msg.type = 'T';
        strncpy(msg.sender, sender.c_str(), sizeof(msg.sender) - 1);
        strncpy(msg.sender_id, sid.c_str(), sizeof(msg.sender_id) - 1);
        strncpy(msg.text, text.c_str(), sizeof(msg.text) - 1);
    } else {                                  // "V" (or unknown → treat as video)
        String id = f2; id.trim();
        if (id.isEmpty()) return;
        int p3 = tail.indexOf('|');
        String sender = (p3 >= 0) ? tail.substring(0, p3) : tail;
        String sid    = (p3 >= 0) ? tail.substring(p3 + 1) : String();
        sender.trim(); sid.trim();
        msg.type = 'V';
        strncpy(msg.id, id.c_str(), sizeof(msg.id) - 1);
        strncpy(msg.sender, sender.c_str(), sizeof(msg.sender) - 1);
        strncpy(msg.sender_id, sid.c_str(), sizeof(msg.sender_id) - 1);
    }

    if (s_queue) xQueueSend(s_queue, &msg, 0);
#ifdef DEBUG_SERIAL
    Serial.printf("[SSE] type=%c id='%s' sender='%s' sender_id='%s' text='%s'\n",
                  msg.type, msg.id, msg.sender, msg.sender_id, msg.text);
#endif
}

// One connection attempt. Returns when the stream drops so the caller can back off.
static void run_once() {
    if (!wifi_mgr_connected()) return;

    WiFiClient client;
    client.setTimeout(SSE_CONNECT_TIMEOUT_MS / 1000);
    if (!client.connect(SERVER_HOST, SERVER_PORT, SSE_CONNECT_TIMEOUT_MS)) {
#ifdef DEBUG_SERIAL
        Serial.println("[SSE] connect failed");
#endif
        return;
    }

    client.print(
        "GET /events HTTP/1.1\r\n"
        "Host: " SERVER_HOST "\r\n"
        "Accept: text/event-stream\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: keep-alive\r\n"
        "\r\n");

    s_online = true;
#ifdef DEBUG_SERIAL
    Serial.println("[SSE] connected to /events");
#endif

    String line;
    line.reserve(96);
    uint32_t last_rx = millis();

    while (client.connected() && wifi_mgr_connected()) {
        while (client.available()) {
            char c = (char)client.read();
            last_rx = millis();
            if (c == '\r') continue;
            if (c == '\n') {
                // Blank line = event boundary; "data:" line carries the id. The
                // HTTP status/headers arrive as plain lines first — those never
                // start with "data:" so they are harmless to skip.
                if (line.startsWith("data:")) emit_message(line.substring(5));
                line = "";
            } else if (line.length() < 200) {
                line += c;
            }
        }
        // Idle guard: if the server sends nothing for a long time and the socket
        // has silently died, drop and let the backoff reconnect.
        if ((millis() - last_rx) > (SSE_BACKOFF_MAX_MS * 2)) break;
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    s_online = false;
    client.stop();
#ifdef DEBUG_SERIAL
    Serial.println("[SSE] disconnected");
#endif
}

void sse_client_task(void* arg) {
    (void)arg;
    uint32_t backoff = SSE_BACKOFF_START_MS;
    for (;;) {
        if (!wifi_mgr_connected()) {
            wifi_mgr_tick();
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        run_once();

        // Exponential backoff, reset on a clean (long-lived) connection is handled
        // implicitly: a connection that lasted a while still backs off briefly,
        // which is fine. Cap per gotcha #10.
        vTaskDelay(pdMS_TO_TICKS(backoff));
        backoff = (backoff * 2 > SSE_BACKOFF_MAX_MS) ? SSE_BACKOFF_MAX_MS : backoff * 2;
        if (wifi_mgr_connected() && backoff > SSE_BACKOFF_START_MS * 4) {
            // Give a healthy network a fresh short backoff next drop.
            backoff = SSE_BACKOFF_START_MS;
        }
    }
}
