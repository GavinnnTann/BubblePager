# BubblePager — Controls & Navigation

Every action works from **both** the physical buttons and the touchscreen, so you
can drive it whichever way is closer to hand. This page is the full reference; the
[cheat sheet](#cheat-sheet) at the bottom is the 20-second version.

## Screens

| Screen | What you see |
|--------|--------------|
| **Home** | Clock, a breathing circle, Wi-Fi status, the last sender, and an unread badge (`N new`) if you've missed messages |
| **History** | A frozen preview of a past message with an `n/N` position counter — the browsing view |
| **Playing** | A video-note / sticker playing full-screen with a depleting progress ring |
| **Settings** | A touch scroll-wheel of options (opened from Home) |

The flow is **Home → History (browse) → Playing**. A newly arrived message does
**not** auto-play — it lands on a freeze-frame preview you choose to open, like
unread mail.

## Buttons

Three buttons: **UP** (BTN1), **DOWN** (BTN2), and **Power**.

| Button | On Home / History | While Playing |
|--------|-------------------|---------------|
| **UP** — tap | Go to the **older** message (from Home, enters History) | Stop and jump to the **older** message |
| **DOWN** — tap | Go to the **newer** message | Stop and jump to the **newer** message |
| **UP / DOWN — hold** | **Play** the previewed message | — |
| **Power** — tap | Sleep / wake the screen (display off, still receiving) | Same |
| **Power** — hold | Shut down (deep sleep) | Same |

> If **Auto 180** is on (Settings), UP/DOWN swap when the device is flipped, so
> they always match what you see.

## Touch gestures

| Gesture | On Home | On History | While Playing |
|---------|---------|------------|---------------|
| **Tap** | — | **Play** the previewed message | Stop, back to the freeze-frame |
| **Long-press** | Open **Settings** | Show **sender / time / duration** overlay | — |
| **Swipe ↑ / ↓** | — | Older / newer message | — |
| **Swipe ← / →** | — | Back to Home | Stop, back to Home |

Tap and long-press are distinguished by how long you hold; a swipe is any clear
drag. (Thresholds are tunable in `config.h`.)

## Receiving a message

When something arrives, the device buzzes and shows an **incoming pulse** (the
sender's Telegram photo, or a plain ring if their privacy settings hide it). For a
few seconds you can:

- **Tap** → open it right now (video/sticker **plays**, text shows in full).
- **Any button/swipe** → dismiss to the freeze-frame preview (browse it like any
  past message).
- **Do nothing** → the screen sleeps and the message becomes **unread**. Home then
  shows a `N new` badge — just like a real pager, not a scrolling feed.

How long it waits is the **Incoming** setting (default 5 s).

## Catch-up reel (unread)

Got a `N new` badge? **Tap the screen on Home** to play everything you missed, in
the order it arrived — Stories/Reels style:

| Gesture during the reel | Action |
|-------------------------|--------|
| Tap **left half** of screen | Previous item |
| Tap **right half** of screen | Skip to next |
| Swipe ← / → | Exit to Home |
| UP / DOWN button | Previous / next item |

The reel keeps rolling as you skip; viewing anything clears the unread badge.

## Browsing history

The device keeps your last **12** messages (videos, stickers, and texts,
interleaved in one timeline). From Home, tap **UP/DOWN** (or swipe ↑/↓ in History)
to move through them; the `n/N` counter shows where you are (`1` = newest).
**Long-press** any preview for the sender, timestamp, and duration.

> Text messages show in full immediately (no "play"); videos and stickers show a
> freeze-frame you play with a tap or a button-hold. Replaying an old video needs
> the server to still have it cached.

## Settings

**Long-press on Home** opens the settings wheel. It's **touch-driven**:

- **Drag up / down** to scroll the wheel; the centered row is selected.
- **Tap** the centered row to toggle it (or trigger it, for actions).
- **Swipe left / right** (or scroll to **Exit** and tap) to save and leave.
- The **Power** button still sleeps/wakes here.

| Option | What it does |
|--------|--------------|
| **Battery** | Shows battery % and charging state (info only) |
| **Sync** | Force a clock re-sync over Wi-Fi (NTP) |
| **Haptics** | Vibration feedback ON / OFF |
| **Motion wake** | Movement keeps the screen awake / resets the idle timer |
| **Auto 180** | Auto-rotate the display 180° when flipped |
| **Tap sleep** | Double-tap to sleep the screen |
| **Incoming** | How long the incoming pulse waits before marking unread (3 / 5 / 8 / 12 / 20 s) |
| **Exit** | Save and return to Home |

## Cheat sheet

```
HOME        UP/DOWN → browse history     tap (with unread) → catch-up reel
            long-press → Settings        Power → sleep · Power-hold → off

HISTORY     tap or hold → play           swipe ↑↓ / UP·DOWN → older/newer
            long-press → info            swipe ←→ → Home

PLAYING     tap → stop (freeze)          UP/DOWN → stop + older/newer
            swipe ←→ → stop (Home)

INCOMING    tap → open now               ignore → unread badge
```

See also: [`SETUP.md`](SETUP.md) to get it running, and the main
[`README.md`](README.md) for what BubblePager is.
