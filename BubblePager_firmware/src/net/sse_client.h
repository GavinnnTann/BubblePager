#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

// Persistent SSE listener (Task A). Holds an HTTP GET /events connection open and
// posts each received message onto the queue. Reconnects with exponential backoff
// on drop (gotcha #10). Never renders.

// Wire format (server: app/events.py) — a leading type tag distinguishes content:
//   data: V|<video_id>|<sender>|<sender_id>   video note / sticker
//   data: T|<sender>|<sender_id>|<text>       plain text (text is LAST — the
//                                             only field allowed to contain
//                                             '|'; everything before it is a
//                                             fixed-format short field)
// sender_id (numeric Telegram user id, may be empty) drives the incoming-pulse
// profile-photo fetch (GET /avatar/<sender_id>).
struct VideoMsg {
    char type;          // 'V' or 'T'
    char id[64];         // video id (type 'V')
    char sender[48];
    char sender_id[16];  // decimal Telegram user id, "" if unknown
    char text[200];      // message body (type 'T') — TEXT_MSG_MAX_CHARS + margin
};

// Register the queue that received video ids are posted to. Call before the task.
void sse_client_init(QueueHandle_t video_queue);

// FreeRTOS task entry point — loops forever. Pin to core 0 in main.
void sse_client_task(void* arg);

// True while the /events stream is currently connected (drives the UI status dot).
bool sse_client_online();
