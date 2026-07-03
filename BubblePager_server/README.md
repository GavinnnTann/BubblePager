# BubblePager server

Bridges Telegram video-notes to the ESP32 pager. It receives a `video_note`,
transcodes it to a 240×240 MJPEG with FFmpeg, stores it in Supabase, and pushes
the id to the device over SSE. The device then pulls the paced MJPEG stream.

```
Telegram ──video_note──▶ bot.py ──▶ transcoder.py ──▶ Supabase Storage (.mjpeg)
                                            │           Supabase Postgres (metadata)
                                            └──▶ events.bus.publish(id)
                                                       │
ESP32 ◀── SSE /events (data: <id>) ◀───────────────────┘
ESP32 ── GET /stream/<id> ──▶ paced JPEG frames + X-Frame-Count header
```

## Why it can't run on your PC
It's a small always-on web service: it holds the SSE socket to the device open
and must be reachable whenever someone sends a video. If your computer sleeps,
the pager goes dark. Run it somewhere always-on (see **Deploy**). Supabase (already
cloud) holds the files + metadata, so the box itself stays stateless/disposable.

## Endpoints
| Method | Path            | Purpose                                             |
|--------|-----------------|-----------------------------------------------------|
| GET    | `/health`       | liveness + SSE client count                         |
| GET    | `/events`       | SSE — device keeps this open, gets `data: <id>`     |
| GET    | `/stream/<id>`  | paced MJPEG, `X-Frame-Count` header, `Content-Length` set |
| POST   | `/webhook`      | Telegram webhook (webhook mode only)                |
| GET    | `/videos`       | recent history (JSON) for a future companion app    |

## Setup
1. **Bot token** — talk to [@BotFather](https://t.me/BotFather), `/newbot`, copy the token.
2. **Supabase** — create a project, then run [`schema.sql`](schema.sql) in the SQL
   editor (creates the `videos` bucket + `videos` table). Copy the Project URL and
   the **service-role** key (Settings → API).
3. `cp .env.example .env` and fill in `TELEGRAM_BOT_TOKEN`, `SUPABASE_URL`,
   `SUPABASE_SERVICE_ROLE_KEY`.

## Run locally (polling mode — no public URL needed)
```bash
pip install -r requirements.txt          # needs ffmpeg on PATH
python -m app.stream_server              # serves on :8080, polls Telegram
```
Point the firmware's `config.h` at your machine: `SERVER_HOST "192.168.x.x"`,
`SERVER_PORT 8080`. Send a video-note to the bot and watch it appear.

> Local run is for testing only — see above re: always-on.

## Deploy (always-on)
Any host that runs a container and lets the ESP32 reach a plain-HTTP port works.
Because the firmware uses plain HTTP (not TLS), a **raw VPS is simplest** — a
managed PaaS that forces HTTPS-only on :443 would require adding TLS to the
firmware.

**Recommended: a small VPS (Hetzner / DigitalOcean / Oracle free tier).**
```bash
# on the VPS, with Docker installed:
git clone <repo> && cd TelePager_server
cp .env.example .env      # fill it in; set TELEGRAM_MODE=polling
docker build -t bubblepager .
docker run -d --restart unless-stopped --env-file .env -p 8080:8080 bubblepager
```
Then set the firmware `SERVER_HOST` to the VPS IP/hostname, `SERVER_PORT 8080`.
Polling mode needs no inbound Telegram access, so you don't even need a domain —
just the open `:8080` for the device.

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
- The MP4 is deleted right after transcoding; only the `.mjpeg` is kept (Supabase
  Storage + a local cache under `storage/` for fast replays).
- Without Supabase keys the server still runs in **local-disk mode** (files under
  `storage/`, no DB) — handy for a first smoke test.
