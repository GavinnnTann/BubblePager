#include "history.h"
#include <Arduino.h>
#include <Preferences.h>
#include <string.h>

// In-RAM ring, oldest-first: s_ids[0] = oldest kept, s_ids[count-1] = newest.
// Newest-first cursor maps to array index (count-1-cursor).
static char s_ids[HISTORY_MAX][HISTORY_ID_LEN];
static int  s_count  = 0;
static int  s_cursor = 0;

static constexpr const char* NS  = "tp_hist";
static constexpr const char* KEY = "ids";   // '\n'-joined, oldest-first

static void persist() {
    String blob;
    for (int i = 0; i < s_count; i++) {
        if (i) blob += '\n';
        blob += s_ids[i];
    }
    Preferences p;
    if (p.begin(NS, false)) { p.putString(KEY, blob); p.end(); }
}

void history_init() {
    s_count  = 0;
    s_cursor = 0;
    Preferences p;
    if (!p.begin(NS, true)) return;          // read-only; ok if namespace absent
    String blob = p.getString(KEY, "");
    p.end();

    int start = 0;
    while (start < (int)blob.length() && s_count < HISTORY_MAX) {
        int nl = blob.indexOf('\n', start);
        String tok = (nl < 0) ? blob.substring(start) : blob.substring(start, nl);
        if (tok.length() > 0) {
            strncpy(s_ids[s_count], tok.c_str(), HISTORY_ID_LEN - 1);
            s_ids[s_count][HISTORY_ID_LEN - 1] = '\0';
            s_count++;
        }
        if (nl < 0) break;
        start = nl + 1;
    }
}

void history_add(const char* id) {
    if (!id || !id[0]) return;

    // Dedupe an immediate repeat (Telegram can resend the same note).
    if (s_count > 0 && strncmp(s_ids[s_count - 1], id, HISTORY_ID_LEN) == 0) {
        s_cursor = 0;
        return;
    }

    if (s_count == HISTORY_MAX) {
        // Drop the oldest — shift everything down one slot.
        memmove(&s_ids[0], &s_ids[1], (HISTORY_MAX - 1) * HISTORY_ID_LEN);
        s_count = HISTORY_MAX - 1;
    }
    strncpy(s_ids[s_count], id, HISTORY_ID_LEN - 1);
    s_ids[s_count][HISTORY_ID_LEN - 1] = '\0';
    s_count++;
    s_cursor = 0;                             // snap to newest
    persist();
}

int history_count() { return s_count; }

bool history_current(char* out, size_t len) {
    if (s_count == 0 || !out || len == 0) return false;
    int idx = s_count - 1 - s_cursor;         // newest-first cursor → array index
    if (idx < 0) idx = 0;
    strncpy(out, s_ids[idx], len - 1);
    out[len - 1] = '\0';
    return true;
}

bool history_older(char* out, size_t len) {
    if (s_cursor >= s_count - 1) return false; // already at oldest
    s_cursor++;
    return history_current(out, len);
}

bool history_newer(char* out, size_t len) {
    if (s_cursor <= 0) return false;           // already at newest
    s_cursor--;
    return history_current(out, len);
}

void history_reset_cursor() { s_cursor = 0; }

int history_cursor_age() { return s_cursor; }
