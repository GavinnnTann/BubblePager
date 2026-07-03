"""Telegram bot — receives video_note updates, downloads, hands off to transcode.

Supports both long-polling (getUpdates loop) and webhook delivery; the FastAPI
app picks the mode from config and either starts the polling task or registers
the webhook on startup.
"""
import asyncio
import logging

import httpx

from . import config
from . import device
from . import transcoder
from .events import bus

log = logging.getLogger("telepager.bot")

# Hard cap on a text message forwarded to the pager. The round 240×240 display's
# safe text zone (word-wrapped, centred, clear of the circular bezel) comfortably
# fits ~100 characters at a readable size — over that gets REJECTED (not
# truncated), so the sender knows to shorten it rather than getting a silently
# clipped message.
TEXT_MSG_MAX_CHARS = 100


def _ago(sec: float) -> str:
    sec = int(sec)
    if sec < 10:   return "just now"
    if sec < 60:   return f"{sec}s ago"
    if sec < 3600: return f"{sec // 60}m ago"
    return f"{sec // 3600}h ago"

_poll_task: asyncio.Task | None = None
_stop = asyncio.Event()


def _client() -> httpx.AsyncClient:
    return httpx.AsyncClient(timeout=60.0)


# ── Sender feedback (one status message, edited as state changes) ────────────
async def send_message(chat_id, text: str, reply_to=None) -> int | None:
    """Send a message; return its message_id (for later edits) or None. reply_to
    threads the status onto the original video (matters in busy group chats)."""
    if not chat_id:
        return None
    params = {"chat_id": chat_id, "text": text}
    if reply_to:
        params["reply_to_message_id"] = reply_to
        params["allow_sending_without_reply"] = "true"   # don't fail if it's gone
    try:
        async with _client() as c:
            r = await c.get(f"{config.TG_API}/sendMessage", params=params)
            j = r.json()
            if j.get("ok"):
                return j["result"]["message_id"]
            log.warning("sendMessage not ok: %s", j)
    except Exception as e:
        log.warning("sendMessage failed: %s", e)
    return None


async def set_my_commands() -> None:
    """Register the bot's command menu (the '/' autocomplete list in Telegram)."""
    cmds = [
        {"command": "status",  "description": "Pager status, queue & battery"},
        {"command": "devices", "description": "List connected pagers"},
        {"command": "help",    "description": "How to use TelePager"},
    ]
    try:
        async with _client() as c:
            r = await c.post(f"{config.TG_API}/setMyCommands", json={"commands": cmds})
            log.info("setMyCommands → %s", r.json())
    except Exception as e:
        log.warning("setMyCommands failed: %s", e)


async def mark_played(video_id: str, device_name: str | None = None) -> None:
    """A pager reported it PLAYED this video → upgrade the status message. Multiple
    devices accumulate: '👁 Played on Kitchen, Bedroom'."""
    r = device.record_play(video_id, device_name)
    if not r:
        return
    chat_id, message_id, names = r
    who = ", ".join(names) if names else "your pager"
    await edit_message(chat_id, message_id, f"👁 Played on {who}")


async def edit_message(chat_id, message_id, text: str) -> None:
    """Edit the status message in place. Falls back to a new message if we never
    got a message_id (so the user still gets the final state)."""
    if not chat_id:
        return
    if not message_id:
        await send_message(chat_id, text)
        return
    try:
        async with _client() as c:
            await c.get(f"{config.TG_API}/editMessageText",
                        params={"chat_id": chat_id, "message_id": message_id,
                                "text": text})
    except Exception as e:
        log.warning("editMessageText failed: %s", e)


async def fetch_avatar_bytes(sender_id: int) -> bytes | None:
    """Telegram getUserProfilePhotos → pick a mid-size photo → getFile → download
    the raw bytes. Returns None if the user has no photo, has privacy settings
    blocking bot access, or any step fails — all treated as "no avatar" by the
    caller (stream_server.py's /avatar falls back gracefully; the device falls
    back to the plain pulsing ring)."""
    try:
        async with _client() as c:
            r = await c.get(f"{config.TG_API}/getUserProfilePhotos",
                            params={"user_id": sender_id, "limit": 1})
            j = r.json()
            if not j.get("ok"):
                log.warning("getUserProfilePhotos(%s) not ok: %s", sender_id, j)
                return None
            photos = j.get("result", {}).get("photos")
            if not photos:
                log.info("getUserProfilePhotos(%s): user has no photo (total_count=%s)",
                         sender_id, j.get("result", {}).get("total_count"))
                return None
            sizes = photos[0]   # one profile photo, offered small→large
            photo = sizes[min(1, len(sizes) - 1)]   # a modest mid-size, not the largest
            r2 = await c.get(f"{config.TG_API}/getFile", params={"file_id": photo["file_id"]})
            j2 = r2.json()
            if not j2.get("ok"):
                log.warning("getFile(%s) not ok: %s", photo["file_id"], j2)
                return None
            file_path = j2["result"]["file_path"]
            r3 = await c.get(f"{config.TG_FILE_API}/{file_path}")
            if r3.status_code != 200:
                log.warning("avatar download %s -> %s", file_path, r3.status_code)
                return None
            return r3.content
    except Exception as e:
        log.warning("fetch_avatar_bytes(%s) failed: %s", sender_id, e)
        return None


async def _download_file(file_id: str) -> bytes | None:
    """Telegram getFile → resolve file_path → download the bytes."""
    async with _client() as c:
        r = await c.get(f"{config.TG_API}/getFile", params={"file_id": file_id})
        j = r.json()
        if not j.get("ok"):
            log.error("getFile failed: %s", j)
            return None
        file_path = j["result"]["file_path"]
        r2 = await c.get(f"{config.TG_FILE_API}/{file_path}")
        if r2.status_code != 200:
            log.error("file download failed: %s", r2.status_code)
            return None
        return r2.content


def _extract_video_note(message: dict) -> dict | None:
    """Return meta dict for a video_note message, or None if it isn't one."""
    vn = message.get("video_note")
    if not vn:
        return None
    frm = message.get("from", {}) or {}
    name = " ".join(x for x in (frm.get("first_name"), frm.get("last_name")) if x)
    return {
        "id":          vn["file_unique_id"],
        "file_id":     vn["file_id"],
        "sender_name": name or frm.get("username") or "Someone",
        "sender_id":   frm.get("id"),
        "chat_id":     (message.get("chat", {}) or {}).get("id"),
    }


def _extract_sticker(message: dict) -> dict | None:
    """Return meta dict for a sticker message (static WEBP or video WEBM), or
    None if it isn't one. `is_animated` (.tgs Lottie) callers must check and
    decline — FFmpeg can't decode that vector format."""
    st = message.get("sticker")
    if not st:
        return None
    frm = message.get("from", {}) or {}
    name = " ".join(x for x in (frm.get("first_name"), frm.get("last_name")) if x)
    return {
        "id":           st["file_unique_id"],
        "file_id":      st["file_id"],
        "sender_name":  name or frm.get("username") or "Someone",
        "sender_id":    frm.get("id"),
        "chat_id":      (message.get("chat", {}) or {}).get("id"),
        "is_animated":  bool(st.get("is_animated")),   # .tgs — unsupported
        "is_video":     bool(st.get("is_video")),       # .webm — reuses the video path
    }


async def _run_pipeline(meta: dict, kind: str = "video", fit: str = "cover") -> str:
    """Download → transcode → publish. Returns a result tag; may raise on a
    transient error (caught + retried by the caller)."""
    data = await _download_file(meta["file_id"])
    if not data:
        return "fetch_fail"
    row = await transcoder.process(data, meta, kind=kind, fit=fit)
    if not row:
        return "process_fail"
    # process() published to connected devices (or queued it if none were online).
    return "delivered" if bus.client_count > 0 else "queued"


async def _handle_media(meta: dict, message: dict, kind: str, label: str,
                        *, fit: str = "cover") -> None:
    """Shared pipeline for anything that goes through transcoder.process(): video
    notes, video stickers, and static stickers. One status message, edited as the
    pipeline progresses, threaded onto the original message (unambiguous in a
    group chat). Retries once on a transient error.

    fit="cover" (default) fills the circle + crops overflow — right for
    video_note, which Telegram already frames as a centred round bubble.
    fit="contain" scales to fit + pads black — right for stickers, which come in
    arbitrary aspect ratios that cropping would chop into."""
    log.info("%s from %s (id=%s)", label, meta["sender_name"], meta["id"])
    chat_id = meta.get("chat_id")
    status_id = await send_message(chat_id, "📨 Got it — sending to your pager…",
                                   reply_to=message.get("message_id"))

    result = None
    for attempt in range(2):
        try:
            result = await _run_pipeline(meta, kind, fit)
            break
        except Exception as e:
            log.warning("pipeline attempt %d failed: %s", attempt + 1, e)
            if attempt == 0:
                await edit_message(chat_id, status_id, "⏳ Hit a snag — retrying…")
                await asyncio.sleep(1.5)

    if result is None:
        await edit_message(chat_id, status_id,
                           "⚠️ Server error — couldn't deliver. Please resend.")
    elif result == "fetch_fail":
        await edit_message(chat_id, status_id,
                           "⚠️ Couldn't fetch that from Telegram. Try again.")
    elif result == "process_fail":
        await edit_message(chat_id, status_id,
                           "⚠️ Couldn't process that. Try again.")
    elif result == "delivered":
        device.remember_message(meta["id"], chat_id, status_id)
        await edit_message(chat_id, status_id, "✅ Sent to your pager!")
    elif result == "queued":
        device.remember_message(meta["id"], chat_id, status_id)
        await edit_message(
            chat_id, status_id,
            f"📴 Pager offline — queued ({bus.pending_count} waiting). "
            "It'll play when the pager reconnects.")


async def _handle_command(text: str, chat_id) -> None:
    """Text commands: /status (summary), /devices (per-pager state)."""
    cmd = text.split()[0].lower().lstrip("/").split("@")[0]
    if cmd == "status":
        lines = ["📟 TelePager",
                 f"Pagers online: {device.online_count()}",
                 f"In queue: {bus.pending_count}"]
        for d in device.devices(only_online=True)[:3]:
            batt = f"{d['battery']}%" if d.get("battery") is not None else "?"
            chg = "⚡" if d.get("charging") else ""
            lines.append(f"• {d['name']}: {batt}{chg}")
        await send_message(chat_id, "\n".join(lines))
    elif cmd == "devices":
        ds = device.devices()
        if not ds:
            await send_message(chat_id, "No pagers have checked in yet.")
            return
        lines = ["📟 Pagers:"]
        for d in ds:
            dot = "🟢" if d["online"] else "⚪"
            batt = f"{d['battery']}%" if d.get("battery") is not None else "?"
            chg = "⚡" if d.get("charging") else ""
            seen = "online" if d["online"] else _ago(d["age"])
            lines.append(f"{dot} {d['name']} — {batt}{chg} · {seen}")
        await send_message(chat_id, "\n".join(lines))
    elif cmd in ("start", "help"):
        await send_message(
            chat_id,
            "Send me a round video note, a sticker, or a short text and I'll "
            "show it on your pager(s). (Animated .tgs stickers aren't "
            "supported.)\n/status — quick check\n/devices — list pagers")
    else:
        await send_message(chat_id, "Unknown command. Try /status or /devices.")


def _sender_name(message: dict) -> str:
    frm = message.get("from", {}) or {}
    name = " ".join(x for x in (frm.get("first_name"), frm.get("last_name")) if x)
    return name or frm.get("username") or "Someone"


async def _handle_text_message(message: dict, text: str) -> None:
    """Plain text (not a command, not a video note) → forward to the pager(s) as
    a text screen. Hard-rejects over the character cap rather than truncating, so
    the sender knows to shorten it and resend."""
    chat_id = (message.get("chat") or {}).get("id")
    reply_to = message.get("message_id")

    if len(text) > TEXT_MSG_MAX_CHARS:
        await send_message(
            chat_id,
            f"⚠️ Message too long ({len(text)}/{TEXT_MSG_MAX_CHARS} chars). "
            "Shorten it and resend.",
            reply_to=reply_to)
        return

    sender = _sender_name(message)
    sender_id = (message.get("from") or {}).get("id")
    bus.publish_text(sender, sender_id, text)
    log.info("text message from %s (%d chars)", sender, len(text))

    online = device.online_count() > 0
    status = ("✅ Sent to your pager!" if online else
              f"📴 Pager offline — queued ({bus.pending_count} waiting). "
              "It'll show when the pager reconnects.")
    await send_message(chat_id, status, reply_to=reply_to)


async def handle_update(update: dict) -> None:
    message = update.get("message") or update.get("channel_post")
    if not message:
        return

    text = (message.get("text") or "").strip()
    if text.startswith("/"):
        await _handle_command(text, (message.get("chat") or {}).get("id"))
        return

    meta = _extract_video_note(message)
    if meta:
        await _handle_media(meta, message, "video", "video_note")
        return

    sticker = _extract_sticker(message)
    if sticker:
        if sticker["is_animated"]:
            # .tgs is gzipped Lottie (vector JSON) — FFmpeg can't decode it, and
            # rendering Lottie server-side is real added complexity for a rare
            # case. Decline clearly rather than silently drop it.
            await send_message(
                sticker["chat_id"],
                "🎭 Animated stickers aren't supported yet — try a video note, "
                "a static sticker, or a video sticker!",
                reply_to=message.get("message_id"))
            return
        kind = "video" if sticker["is_video"] else "image"
        # "contain": stickers are arbitrary aspect ratios — don't crop the artwork.
        await _handle_media(sticker, message, kind, "sticker", fit="contain")
        return

    if text:
        await _handle_text_message(message, text)
    # else: no video note, no sticker, no text — ignore (photos, etc.)


# ── Polling mode ────────────────────────────────────────────────────────────
async def _poll_loop() -> None:
    offset = 0
    log.info("Telegram polling started")
    async with _client() as c:
        # Drop any webhook so getUpdates is allowed.
        try:
            await c.get(f"{config.TG_API}/deleteWebhook")
        except Exception:
            pass
        while not _stop.is_set():
            try:
                r = await c.get(
                    f"{config.TG_API}/getUpdates",
                    params={"offset": offset, "timeout": 25,
                            "allowed_updates": '["message","channel_post"]'},
                    timeout=35.0,
                )
                j = r.json()
                if not j.get("ok"):
                    await asyncio.sleep(3)
                    continue
                for upd in j["result"]:
                    offset = upd["update_id"] + 1
                    try:
                        await handle_update(upd)
                    except Exception as e:
                        log.exception("update handling failed: %s", e)
            except Exception as e:
                log.warning("getUpdates error: %s", e)
                await asyncio.sleep(3)
    log.info("Telegram polling stopped")


def start_polling() -> None:
    global _poll_task
    _stop.clear()
    _poll_task = asyncio.create_task(_poll_loop())


async def stop_polling() -> None:
    _stop.set()
    if _poll_task:
        await _poll_task


# ── Webhook mode ────────────────────────────────────────────────────────────
async def register_webhook() -> None:
    if not config.PUBLIC_URL:
        raise RuntimeError("webhook mode needs PUBLIC_URL")
    url = f"{config.PUBLIC_URL}/webhook"
    params = {"url": url, "allowed_updates": '["message","channel_post"]'}
    if config.WEBHOOK_SECRET:
        params["secret_token"] = config.WEBHOOK_SECRET
    async with _client() as c:
        r = await c.get(f"{config.TG_API}/setWebhook", params=params)
        log.info("setWebhook %s → %s", url, r.json())
