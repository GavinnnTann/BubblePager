# BubblePager — Setup Guide

Get a BubblePager running end to end: someone sends a Telegram video-note to your
bot, and it plays on the ESP32-S3 round display.

```
Telegram ──video_note──▶  Server (FastAPI + FFmpeg)  ──▶ storage/ (.mjpeg + SQLite)
                                     │
                          SSE push + paced MJPEG
                                     ▼
                          ESP32-S3 + GC9A01 (the pager)
```

Two folders:
- `BubblePager_firmware/` — the ESP32 firmware (PlatformIO / C++)
- `BubblePager_server/`   — the Python bridge (FastAPI)

Do the steps in order: **Bot → Server → Firmware → Verify.**

---

## 0. Prerequisites

| For | Install |
|-----|---------|
| Firmware | [VS Code](https://code.visualstudio.com/) + [PlatformIO extension](https://platformio.org/install/ide?install=vscode) |
| Server (local) | [Python 3.11+](https://www.python.org/downloads/) and [FFmpeg](https://www.gyan.dev/ffmpeg/builds/) on PATH |
| Server (deploy) | [Docker](https://docs.docker.com/get-docker/) |
| Accounts | A [Telegram](https://telegram.org/) account. That's all — the server keeps its own data. |

Windows FFmpeg: `winget install -e --id Gyan.FFmpeg` (restart the terminal after).

---

## 1. Telegram bot

1. In Telegram, message [@BotFather](https://t.me/BotFather) → `/newbot`.
2. Give it a name and a username (must end in `bot`).
3. Copy the **token** it gives you (looks like `123456789:AAExampleTokenABC...`).
   You'll paste it into the server `.env` in step 2.

That's it — the device only *receives*, so no other bot config is required.

---

## 2. Server

### 2a. Configure (all deployment options share this)

```bash
cd BubblePager_server
cp .env.example .env      # then edit .env
```

Fill in `.env`:

| Var | Value |
|-----|-------|
| `TELEGRAM_BOT_TOKEN` | from step 1 |
| `SERVER_PORT` | port the ESP32 connects to (e.g. `8090`) |
| `TELEGRAM_MODE` | `polling` (default; no public URL needed) |

Leave the rest at defaults. Whatever `SERVER_PORT` you pick, the firmware
`SERVER_PORT` (step 3) **must match**.

Transcoded videos and their metadata (SQLite) both land in
`BubblePager_server/storage/` — no external database to set up, and one
directory to back up or move.

### 2b. Option A — Laptop / desktop (our current dev setup)

Quickest for testing. **Downside: the pager only works while the machine is awake
and on the same network** — see 2c/2d for always-on.

```bash
pip install -r requirements.txt
python -m app.stream_server
```

You should see `Telegram polling started` with no errors. Then find the
machine's LAN IP (the firmware points here):

- Windows: `ipconfig` → IPv4 Address
- macOS/Linux: `ipconfig getifaddr en0` / `hostname -I`

Sanity check in a browser: `http://localhost:<port>/health` → `{"status":"ok",...}`.

### 2c. Option B — Home server or Raspberry Pi (always-on, home)

Any always-on box on your home Wi-Fi — a Pi, a NAS, a spare mini-PC, a VM on a
home hypervisor — is the simplest 24/7 host.

```bash
cd BubblePager_server            # with your filled-in .env present
docker compose up -d --build
```

Compose bind-mounts `./storage` into the container, so the `.mjpeg` files and
the SQLite database live on the host and survive rebuilds. Moving an existing
library over is one command:

```bash
rsync -avz storage/ user@homeserver:~/BubblePager_server/storage/
```

Point the firmware `SERVER_HOST` at the host's LAN IP (give it a DHCP reservation
so it doesn't change).

> **Home-only by default.** The Pi's LAN IP (`192.168.x.x`) only works while the
> pager is on the *same* home network. To use the pager **away from home** you
> need to solve two separate reachability problems:
> 1. **Expose the Pi to the internet** — forward the port (`8090`) on your home
>    router to the Pi's LAN IP, then point the firmware `SERVER_HOST` at your home
>    **public IP** (use a dynamic-DNS name, e.g. DuckDNS, since home IPs change).
> 2. **Give the pager its own internet** — it's a Wi-Fi station, so it must join a
>    network wherever it goes: tether it to a **phone hotspot**, or wire up a
>    **cellular module** (the firmware would need extending for that). Update
>    `WIFI_SSID`/`WIFI_PASS` to whatever network it will use.
>
> If you plan to use it out and about, **Option C is simpler** — a public VM needs
> no port forwarding, so only problem #2 (the pager's own connectivity) remains.

### 2d. Option C — Oracle Cloud Free Tier (always-on, public)

A free `Always Free` VM gives you a public host anywhere.

```bash
# on the VM (Ubuntu), Docker installed:
cd BubblePager_server            # with .env
docker compose up -d --build
```

Then open the port so the pager can reach it:
- Oracle console → VCN → **Security List** → add an ingress rule for TCP `8090`.
- On the VM: `sudo iptables -I INPUT -p tcp --dport 8090 -j ACCEPT` (persist it).

Point the firmware `SERVER_HOST` at the VM's **public IP**.

> **Reachable anywhere — no port forwarding needed.** Because the VM already has a
> public IP, the pager can reach it from any network; you don't touch a home
> router. The only remaining requirement for **using the pager away from home** is
> giving *the pager itself* internet: it's a Wi-Fi station, so tether it to a
> **phone hotspot** or wire up a **cellular module** (firmware extension needed),
> and set `WIFI_SSID`/`WIFI_PASS` to that network.

> All three use the same image — only *where* it runs and the IP the firmware
> targets differ. The firmware speaks plain HTTP, so use a raw VM/Pi with an open
> port (a PaaS that forces HTTPS-only would need TLS added to the firmware).
> If you make the server publicly reachable, see **Security** below.

---

## 3. Firmware

```bash
cd BubblePager_firmware
cp include/config.example.h include/config.h    # first time only
```

Edit `include/config.h`:

```c
#define WIFI_SSID     "your_wifi_name"
#define WIFI_PASS     "your_wifi_password"
#define SERVER_HOST   "172.20.10.7"   // the server IP from step 2 (LAN or public)
#define SERVER_PORT   8090            // must match SERVER_PORT in the server .env
#define DEVICE_NAME   "Kitchen"       // shown in /devices and "Played on <name>"
```

The ESP32 and the server must be on the **same network** (unless the server is on
a public IP). Build + flash with the board plugged in over USB:

```bash
pio run -t upload           # or use the PlatformIO ▶ Upload button in VS Code
```

To watch the device logs, uncomment `-D DEBUG_SERIAL` in `platformio.ini`,
reflash, then `pio device monitor` (115200 baud).

### Taking it mobile

The two `WIFI_*` lines are what tie the pager to a network, and they're the knob
you turn to use it away from home. Whatever network the pager will live on, its
`WIFI_SSID`/`WIFI_PASS` must match it, **and** `SERVER_HOST` must be an address
reachable *from that network*:

| Where the pager lives | `WIFI_*` → | `SERVER_HOST` → |
|-----------------------|-----------|-----------------|
| Home, same LAN as the server | your home Wi-Fi | server's LAN IP (`192.168.x.x`) |
| Out and about, **public** server (Option C) | a phone hotspot / cellular link | the VM's **public IP** |
| Out and about, **home** server (Option B) | a phone hotspot / cellular link | your home **public IP / DDNS** (port-forwarded — see 2c) |

Two hard constraints to keep in mind:

- **The pager needs its own internet everywhere it goes.** It's a Wi-Fi station
  with no SIM — so a **phone hotspot** is the quick option, or wire up a
  **cellular module** for a self-contained device (that needs a firmware
  extension; it's not built in yet).
- **`SERVER_HOST` is compiled in.** Changing networks means re-editing `config.h`
  and reflashing. If you'll roam between networks often, prefer a public server
  (Option C) with a stable IP/hostname so only the `WIFI_*` lines ever change —
  or move these values into on-device settings later so no reflash is needed.

---

## 4. Verify end to end

1. Server running (step 2), device flashed and powered (step 3).
2. Device shows the **clock face**; the status dot turns **green** once it reaches
   the server (`http://<host>:<port>/health` shows `"sse_clients": 1`).
3. Record a **round video-note** in Telegram (tap the mic icon → tap again to
   switch to video) and send it to your bot.
4. Server log:
   ```
   video_note from <you> (id=...)
   processed <id> — N frames, X.Xs, #### bytes
   published <id> to 1 clients
   ```
5. The device buzzes, shows the pulsing ring, then plays the bubble with the
   depleting progress arc. 🎉

**On-device controls:** BTN1/BTN2 scroll video history · tap screen = pause ·
power button = dim.

---

## Troubleshooting

| Symptom | Likely cause / fix |
|---------|--------------------|
| Server: `401 Unauthorized` on Telegram | `TELEGRAM_BOT_TOKEN` still the placeholder / wrong |
| Server: `error while attempting to bind ... 8090` | Port in use — pick another `SERVER_PORT` (and match it in `config.h`) |
| Server: transcode fails / `ffmpeg not found` | FFmpeg not on PATH — install it, restart the terminal |
| `/health` shows `sse_clients: 0` | Device never reached the server — check Wi-Fi, `SERVER_HOST`/IP, same network |
| Server logs `published` but device never plays | Network blocks device-to-device traffic (common on **phone hotspots** — use a real router) |
| Device stuck on clock, dot not green | Wrong `SERVER_HOST`/port, or server not running |
| `not found` on `/stream` | The `.mjpeg` isn't in `storage/` — transcode failed, or the id is stale (check the `processed <id>` line in the log) |
| `/health` shows `"db": false` | `storage/` isn't writable — check the bind-mount permissions under Docker |

---

## Security (before making the server public)

Fine on a home LAN as-is. If you expose it on a public IP:
- **Rotate the Telegram bot token** (`/revoke` in @BotFather) if it's ever been
  shared or committed.
- The endpoints are currently **unauthenticated** — anyone who can reach the port
  can read metadata and spoof device heartbeats. Add a shared `DEVICE_TOKEN`
  header check (ask before deploying publicly) if that matters to you.

See each subproject's README for deeper details:
[firmware](BubblePager_firmware/CLAUDE.md) · [server](BubblePager_server/README.md).
