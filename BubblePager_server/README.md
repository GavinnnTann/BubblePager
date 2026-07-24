# BubblePager server

Bridges Telegram video-notes to the ESP32 pager. It receives a `video_note`,
transcodes it to a 240×240 MJPEG with FFmpeg, stores it on disk, and pushes the
id to the device over SSE. The device then pulls the paced MJPEG stream.

```
Telegram ──video_note──▶ bot.py ──▶ transcoder.py ──▶ storage/<id>.mjpeg
                                            │           storage/telepager.db (metadata)
                                            └──▶ events.bus.publish(id)
                                                       │
ESP32 ◀── SSE /events (data: <id>) ◀───────────────────┘
ESP32 ── GET /stream/<id> ──▶ paced JPEG frames + X-Frame-Count header
```

## Why it can't run on your laptop
It's a small always-on web service: it holds the SSE socket to the device open
and must be reachable whenever someone sends a video. If your machine sleeps,
the pager goes dark. Run it somewhere always-on (see **Deploy**).

## Storage
Everything persistent lives under `CACHE_DIR` (`storage/` by default):

- `<video_id>.mjpeg` — the transcoded blobs, served by `/stream` and `/thumb`
- `telepager.db` — SQLite, one `videos` row per item, backing `/meta` and `/videos`

The schema is created automatically on startup by [`app/store.py`](app/store.py);
there is no migration step. Keeping the blobs and the DB in one directory means a
single bind mount (or one `rsync`) captures the entire server state.

> Earlier versions kept files in Supabase Storage with metadata in Supabase
> Postgres. That's gone — a deleted Supabase project took the metadata with it
> and broke ingest, while the blobs survived in the local cache the whole time.
> Self-hosted has no such failure mode. If you're migrating from that setup,
> `python -m app.backfill` reconstructs rows from `.mjpeg` files already on disk
> (frames/bytes/duration from the blob, `created_at` from the file mtime; sender
> is unrecoverable).

## Endpoints
| Method | Path                | Purpose                                             |
|--------|---------------------|-----------------------------------------------------|
| GET    | `/health`           | liveness, SSE client count, real DB check           |
| GET    | `/events`           | SSE — device keeps this open, gets `data: <id>`     |
| GET    | `/stream/<id>`      | paced MJPEG, `X-Frame-Count` header, `Content-Length` set |
| GET    | `/thumb/<id>`       | first JPEG frame (cached) — freeze-frame browsing   |
| GET    | `/meta/<id>`        | sender / created_at / duration / frames             |
| GET    | `/avatar/<user_id>` | sender's Telegram photo, 240×240 (cached)           |
| POST   | `/device/heartbeat` | pager battery / rssi / uptime                       |
| POST   | `/device/ack`       | play receipt → "👁 Played on <name>"                |
| GET    | `/devices`          | connected pagers + last-known state                 |
| POST   | `/webhook`          | Telegram webhook (webhook mode only)                |
| GET    | `/videos`           | recent history (JSON) for a future companion app    |

## Setup
1. **Bot token** — talk to [@BotFather](https://t.me/BotFather), `/newbot`, copy the token.
2. `cp .env.example .env` and fill in `TELEGRAM_BOT_TOKEN`. Everything else has a
   working default.

That's it — no external accounts.

## Run locally (polling mode — no public URL needed)
```bash
pip install -r requirements.txt          # needs ffmpeg on PATH
python -m app.stream_server              # serves on :8090, polls Telegram
```
Point the firmware's `config.h` at your machine: `SERVER_HOST "192.168.x.x"`,
`SERVER_PORT 8090`. Send a video-note to the bot and watch it appear.

> Local run is for testing only — see above re: always-on.

## Deploy (always-on)
Any host that runs a container and lets the ESP32 reach a plain-HTTP port works.
Because the firmware uses plain HTTP (not TLS), a **plain host on your LAN or a
raw VPS is simplest** — a managed PaaS that forces HTTPS-only on :443 would
require adding TLS to the firmware.

```bash
# on the host, with Docker installed:
git clone <repo> && cd BubblePager_server
cp .env.example .env      # fill it in; set TELEGRAM_MODE=polling
docker compose up -d --build
```

`docker-compose.yml` bind-mounts `./storage` to `/data` in the container, so the
blobs and SQLite file stay on the host — inspectable, rsync-able, and untouched
by a rebuild. Migrating an existing library is just:

```bash
rsync -avz storage/ user@host:~/BubblePager_server/storage/
```

Then set the firmware `SERVER_HOST` to the host's LAN IP (give it a DHCP
reservation) and `SERVER_PORT` to match `.env`. Polling mode needs no inbound
Telegram access, so no domain or certificate is required.

> **LAN-only by default.** A private LAN IP works only while the pager is on the
> same network. To use it away from home you'd need to forward the port on your
> router plus dynamic DNS — and note the API has **no authentication**: anyone who
> reaches the port can read `/videos`, pull `/stream/<id>`, and POST fake
> heartbeats. Add a shared-secret header before exposing it.

**Webhook mode** (optional, if you have a public HTTPS domain): set
`TELEGRAM_MODE=webhook` and `PUBLIC_URL=https://your-domain`; the app registers
the webhook on startup.

## Notes / gotchas
- `/stream` sets `Content-Length` on purpose so the response is **not** chunked —
  HTTP chunk-size bytes would otherwise land inside the JPEG stream and corrupt
  the device's SOI/EOI scan.
- `/stream` paces frames at `STREAM_FPS`; this is what makes real-time playback
  and the device's tap-to-pause (TCP backpressure) work. Keep `STREAM_FPS` equal
  to the transcode `-r`.
- The MP4 is deleted right after transcoding; only the `.mjpeg` is kept.
- SQLite runs in WAL mode and every call goes through `asyncio.to_thread`, so a
  transcode never blocks the event loop and stalls the SSE keepalives.
- `/health` genuinely queries the database. The previous version reported whether
  *credentials were configured*, which stayed `true` long after the backing
  project had ceased to exist.
