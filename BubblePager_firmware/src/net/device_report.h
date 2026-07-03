#pragma once

// Device → server reporting over plain HTTP POST (JSON):
//   • heartbeat_task — periodic POST /device/heartbeat with id/name/battery/rssi,
//     so the server's /status and /devices know this pager is alive + its state.
//   • device_ack     — POST /device/ack after a video finishes PLAYING, so the
//     sender's Telegram message upgrades to "Played on <name>".
//
// The stable device id is the eFuse MAC; the friendly name is DEVICE_NAME.

// FreeRTOS task entry — loops forever posting a heartbeat every HEARTBEAT_MS.
void heartbeat_task(void* arg);

// Report that `video_id` was played (best-effort, quick blocking POST). Safe to
// call from the render loop between videos.
void device_ack(const char* video_id);
