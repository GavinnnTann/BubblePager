#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Rolling history of the last N received video ids, persisted to NVS so replay /
// scrollback survives a reset. A cursor tracks the currently-selected entry for
// back/forward navigation with the buttons:
//   cursor 0        = newest video
//   cursor count-1  = oldest kept video
// A freshly received video is appended and snaps the cursor back to newest.

#define HISTORY_MAX      12    // keep the last N entries (video or text)
// Each stored entry is opaque to history.cpp — main.cpp encodes it as
// "<type><sender>\x01<payload>" ('V' + video_id, or 'T' + message text up to
// TEXT_MSG_MAX_CHARS). Sized for 1 + 46 (sender) + 1 (sep) + ~100 (text) + margin.
#define HISTORY_ID_LEN   160

// Load persisted history from NVS. Call once in setup().
void history_init();

// Append a newly received id (dedupes an immediate repeat), persist, and reset
// the cursor to newest.
void history_add(const char* id);

// Number of ids currently kept (0..HISTORY_MAX).
int  history_count();

// The id at the current cursor. false if history is empty.
bool history_current(char* out, size_t len);

// Move the cursor one step older / newer and return that id. Returns false (and
// leaves the cursor put) when already at the oldest / newest end.
bool history_older(char* out, size_t len);
bool history_newer(char* out, size_t len);

// Snap the cursor back to the newest entry (no id returned).
void history_reset_cursor();

// Cursor distance from newest (0 = newest). Handy for a "n ago" label.
int  history_cursor_age();
