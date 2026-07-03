"""FastAPI app — the endpoints the ESP32 (and Telegram) talk to.

  GET  /health          → liveness + status
  GET  /events          → SSE; the ESP32 keeps this open, receives video ids
  GET  /stream/<id>     → paced MJPEG with X-Frame-Count (Content-Length set, so
                          NO chunked encoding — chunk bytes would corrupt the
                          device's JPEG marker scan)
  POST /webhook         → Telegram webhook (webhook mode only)
"""
import asyncio
import contextlib
import logging
import re

from fastapi import FastAPI, Request, Response
from fastapi.responses import JSONResponse, StreamingResponse

from . import bot, config, device, transcoder
from . import supabase_store as store
from .events import bus
from .transcoder import split_frames

logging.basicConfig(level=logging.INFO,
                    format="%(asctime)s %(name)s %(levelname)s %(message)s")
log = logging.getLogger("telepager.server")

# Telegram file_unique_ids are URL-safe base64 chars only. Anything else is a
# probe (path traversal, injection) — reject before it reaches the filesystem or
# the Supabase Storage URL.
_VALID_ID = re.compile(r"^[A-Za-z0-9_-]{1,128}$")


@contextlib.asynccontextmanager
async def lifespan(app: FastAPI):
    if not config.BOT_TOKEN:
        log.warning("TELEGRAM_BOT_TOKEN not set — bot disabled")
    elif config.TELEGRAM_MODE == "webhook":
        await bot.register_webhook()
        await bot.set_my_commands()
    else:
        bot.start_polling()
        await bot.set_my_commands()
    log.info("TelePager server up (mode=%s, supabase=%s)",
             config.TELEGRAM_MODE, config.SUPABASE_ENABLED)
    try:
        yield
    finally:
        if config.BOT_TOKEN and config.TELEGRAM_MODE != "webhook":
            await bot.stop_polling()
        await store.aclose()


app = FastAPI(title="TelePager", lifespan=lifespan)


@app.get("/")
async def root():
    return {
        "service": "TelePager",
        "endpoints": ["/health", "/events", "/stream/<id>", "/videos", "/webhook"],
        "sse_clients": bus.client_count,
    }


@app.get("/health")
async def health():
    return {
        "status": "ok",
        "sse_clients": bus.client_count,
        "queued": bus.pending_count,
        "supabase": config.SUPABASE_ENABLED,
        "mode": config.TELEGRAM_MODE,
    }


@app.get("/events")
async def events(request: Request):
    """SSE stream. Sends `data: <video_id>\\n\\n` per new video, with a keepalive
    comment every 15 s so the device's idle guard doesn't drop the socket."""
    queue = bus.subscribe()

    async def gen():
        try:
            yield ": connected\n\n"
            while True:
                if await request.is_disconnected():
                    break
                try:
                    vid = await asyncio.wait_for(queue.get(), timeout=15.0)
                    yield f"data: {vid}\n\n"
                except asyncio.TimeoutError:
                    yield ": keepalive\n\n"
        finally:
            bus.unsubscribe(queue)

    return StreamingResponse(gen(), media_type="text/event-stream", headers={
        "Cache-Control": "no-cache",
        "Connection": "keep-alive",
        "X-Accel-Buffering": "no",     # disable proxy buffering (nginx)
    })


@app.get("/stream/{video_id}")
async def stream(video_id: str):
    if not _VALID_ID.match(video_id):
        return JSONResponse({"error": "bad id"}, status_code=400)
    data = await store.download_mjpeg(video_id)
    if not data:
        return JSONResponse({"error": "not found"}, status_code=404)

    frames = split_frames(data)
    if not frames:
        return JSONResponse({"error": "no frames"}, status_code=422)

    total_bytes = sum(len(f) for f in frames)
    delay = 1.0 / max(config.STREAM_FPS, 1)

    async def gen():
        for f in frames:
            yield f
            await asyncio.sleep(delay)

    # Content-Length is set explicitly → Starlette will NOT use chunked transfer
    # encoding, so the device receives a clean concatenation of JPEGs.
    return StreamingResponse(gen(), media_type="video/x-motion-jpeg", headers={
        "X-Frame-Count": str(len(frames)),
        "Content-Length": str(total_bytes),
        "Cache-Control": "no-cache",
    })


@app.post("/webhook")
async def webhook(request: Request):
    if config.WEBHOOK_SECRET:
        got = request.headers.get("X-Telegram-Bot-Api-Secret-Token", "")
        if got != config.WEBHOOK_SECRET:
            return Response(status_code=403)
    update = await request.json()
    # Handle in the background so Telegram gets a fast 200 (no re-delivery).
    asyncio.create_task(bot.handle_update(update))
    return {"ok": True}


_thumb_cache: dict[str, bytes] = {}


@app.get("/thumb/{video_id}")
async def thumb(video_id: str):
    """First JPEG frame of a video, cached. Powers instant freeze-frame browsing
    on the device (a single JPEG decode, not a streamed MJPEG open)."""
    if not _VALID_ID.match(video_id):
        return JSONResponse({"error": "bad id"}, status_code=400)
    data = _thumb_cache.get(video_id)
    if data is None:
        blob = await store.download_mjpeg(video_id)
        if not blob:
            return JSONResponse({"error": "not found"}, status_code=404)
        frames = split_frames(blob)
        if not frames:
            return JSONResponse({"error": "no frames"}, status_code=422)
        data = frames[0]
        if len(_thumb_cache) > 64:      # bound memory
            _thumb_cache.clear()
        _thumb_cache[video_id] = data
    return Response(content=data, media_type="image/jpeg", headers={
        "Content-Length": str(len(data)),
        "Cache-Control": "public, max-age=3600",
    })


_avatar_cache: dict[int, bytes] = {}   # only SUCCESSES are cached — see below


@app.get("/avatar/{sender_id}")
async def avatar(sender_id: int):
    """Sender's Telegram profile photo, resized to exactly 240×240 and cached —
    powers the incoming-pulse screen's photo. 404 if the user has no photo, has
    bot access blocked by privacy settings, or the fetch/resize fails; the device
    falls back to the plain pulsing ring on any non-200.

    Only a SUCCESSFUL fetch is cached. Caching a failure too (the original bug)
    poisoned the entry permanently: a user who changed their privacy setting to
    "Everybody" AFTER a first failed attempt would keep 404ing forever, since the
    cached None never got a chance to be re-tried."""
    data = _avatar_cache.get(sender_id)
    if data is None:
        raw = await bot.fetch_avatar_bytes(sender_id)
        log.info("avatar %s: getUserProfilePhotos -> %s", sender_id,
                 "got bytes" if raw else "no photo/blocked/fetch-failed")
        jpeg = await transcoder.process_avatar(raw) if raw else None
        if jpeg:
            log.info("avatar %s: resized OK (%d bytes)", sender_id, len(jpeg))
            if len(_avatar_cache) > 200:      # bound memory
                _avatar_cache.clear()
            _avatar_cache[sender_id] = jpeg
            data = jpeg
        elif raw:
            log.warning("avatar %s: got photo bytes but ffmpeg resize failed", sender_id)
    if not data:
        return JSONResponse({"error": "no avatar"}, status_code=404)
    return Response(content=data, media_type="image/jpeg", headers={
        "Content-Length": str(len(data)),
        "Cache-Control": "public, max-age=3600",
    })


@app.get("/meta/{video_id}")
async def meta(video_id: str):
    """Metadata for the long-press overlay: sender, timestamp, duration, frames."""
    if not _VALID_ID.match(video_id):
        return JSONResponse({"error": "bad id"}, status_code=400)
    row = await store.get_video(video_id)
    if not row:
        return JSONResponse({"error": "not found"}, status_code=404)
    return {
        "id":           video_id,
        "sender":       row.get("sender_name"),
        "created_at":   row.get("created_at"),
        "duration_sec": row.get("duration_sec"),
        "frames":       row.get("frames"),
    }


@app.post("/device/heartbeat")
async def device_heartbeat(request: Request):
    """Pager liveness + state (id/name/battery/rssi/uptime). Cached for /status."""
    try:
        data = await request.json()
    except Exception:
        data = {}
    device.set_heartbeat(data)
    return {"ok": True}


@app.post("/device/ack")
async def device_ack(request: Request):
    """Pager receipt after it PLAYED a video → upgrades the sender's message to
    'Played on <name>'."""
    try:
        data = await request.json()
    except Exception:
        data = {}
    vid = data.get("video_id")
    if vid:
        await bot.mark_played(vid, data.get("device_name"))
    return {"ok": True}


@app.get("/devices")
async def devices():
    """Connected/recent pagers with their last-known state."""
    return {"online": device.online_count(), "devices": device.devices()}


@app.get("/videos")
async def videos(limit: int = 20):
    """History for a future companion app."""
    return await store.list_videos(limit)


if __name__ == "__main__":
    import uvicorn
    uvicorn.run(app, host=config.SERVER_HOST, port=config.SERVER_PORT)
