"""Media → MJPEG transcode (FFmpeg), then upload to Storage + record + notify.

Three source kinds share this one pipeline:
  "video" — video_note (MP4) or a video sticker (WEBM/VP9). FFmpeg auto-detects
            the container, so the SAME ffmpeg command handles both unchanged.
  "image" — a static sticker (WEBP, usually with alpha). Transcoded to a single
            240×240 JPEG frame — which IS a valid 1-frame "MJPEG" (split_frames()
            finds exactly one SOI/EOI pair), so the rest of the pipeline (upload,
            DB row, /stream, /thumb, device playback) needs no changes at all:
            the device just plays a "video" that happens to have one frame, holds
            it for DONE_FREEZE_MS, and returns to idle.
Alpha (stickers can be transparent) is flattened to black by the yuv420p pixel
format conversion — acceptable since the whole device UI is black-background.
"""
import asyncio
import logging
import os
import tempfile

from . import config
from . import store
from .events import bus

log = logging.getLogger("telepager.transcoder")

SOI = b"\xff\xd8"
EOI = b"\xff\xd9"


def split_frames(data: bytes) -> list[bytes]:
    """Split a raw MJPEG byte blob into individual JPEG frames by SOI/EOI."""
    frames: list[bytes] = []
    i = 0
    n = len(data)
    while True:
        start = data.find(SOI, i)
        if start < 0:
            break
        end = data.find(EOI, start + 2)
        if end < 0:
            break
        frames.append(data[start:end + 2])
        i = end + 2
    return frames


async def _run_ffmpeg(src_path: str, *, fit: str = "cover") -> bytes:
    """Transcode a video (MP4 video_note or WEBM/VP9 video sticker — FFmpeg
    auto-detects the container) to a 240×240 MJPEG blob on stdout.

    fit="cover"   scale to FILL the square + centre-crop the overflow. Used for
                  video_note: Telegram already frames those as a well-centred
                  round face-cam bubble, so filling the circle looks right.
    fit="contain" scale to FIT within the square (no overflow) + pad the
                  remainder with black. Used for stickers: they come in
                  arbitrary non-square aspect ratios, and cropping would chop
                  off part of the artwork. The black padding is invisible
                  against the round bezel / the rest of the UI's black bg.
    """
    s = config.VIDEO_SIZE
    if fit == "contain":
        vf = (f"scale={s}:{s}:force_original_aspect_ratio=decrease,"
              f"pad={s}:{s}:(ow-iw)/2:(oh-ih)/2:color=black")
    else:
        vf = f"scale={s}:{s}:force_original_aspect_ratio=increase,crop={s}:{s}"
    args = [
        config.FFMPEG_BIN, "-hide_banner", "-loglevel", "error",
        "-i", src_path,
        "-vf", vf,
        "-r", str(config.STREAM_FPS),
        "-q:v", str(config.JPEG_QUALITY),
        "-vcodec", "mjpeg",
        "-f", "mjpeg", "pipe:1",
    ]
    proc = await asyncio.create_subprocess_exec(
        *args, stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE,
    )
    out, err = await proc.communicate()
    if proc.returncode != 0:
        raise RuntimeError(f"ffmpeg failed ({proc.returncode}): {err.decode()[:300]}")
    return out


async def _run_ffmpeg_static(image_path: str) -> bytes:
    """Transcode a static sticker (WEBP, possibly with alpha) to a SINGLE
    240×240 JPEG frame on stdout — a trivially valid 1-frame MJPEG. Always
    "contain" (scale to fit + pad black): stickers are arbitrary aspect ratios,
    so cropping would chop off part of the artwork — see _run_ffmpeg()'s fit
    docstring for the full rationale."""
    s = config.VIDEO_SIZE
    vf = (f"scale={s}:{s}:force_original_aspect_ratio=decrease,"
          f"pad={s}:{s}:(ow-iw)/2:(oh-ih)/2:color=black,format=yuv420p")
    args = [
        config.FFMPEG_BIN, "-hide_banner", "-loglevel", "error",
        "-i", image_path,
        "-vf", vf,
        "-frames:v", "1",
        "-q:v", str(config.JPEG_QUALITY),
        "-vcodec", "mjpeg",
        "-f", "mjpeg", "pipe:1",
    ]
    proc = await asyncio.create_subprocess_exec(
        *args, stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE,
    )
    out, err = await proc.communicate()
    if proc.returncode != 0:
        raise RuntimeError(f"ffmpeg (static) failed ({proc.returncode}): {err.decode()[:300]}")
    return out


async def process_avatar(raw_bytes: bytes) -> bytes | None:
    """Resize a Telegram profile photo to an exact 240×240 JPEG so the device's
    decode path (which blits 1:1 onto a fixed 240×240 canvas, no client-side
    resize) never has to special-case a smaller/odd-sized source. Telegram
    profile photos are already square, but "cover" crop is used defensively in
    case a given size isn't exactly square."""
    tmp = tempfile.NamedTemporaryFile(suffix=".jpg", dir=config.CACHE_DIR, delete=False)
    tmp.write(raw_bytes)
    tmp.close()
    try:
        return await _run_ffmpeg_static_cover(tmp.name)
    except Exception as e:
        log.warning("avatar resize failed: %s", e)
        return None
    finally:
        os.unlink(tmp.name)


async def _run_ffmpeg_static_cover(image_path: str) -> bytes:
    """Like _run_ffmpeg_static() but "cover" (fill + crop) — used only for
    avatars, which are expected square already; a stray non-square source just
    gets center-cropped rather than padded, which looks more like a normal
    avatar crop than a letterboxed sticker would."""
    s = config.VIDEO_SIZE
    vf = f"scale={s}:{s}:force_original_aspect_ratio=increase,crop={s}:{s},format=yuv420p"
    args = [
        config.FFMPEG_BIN, "-hide_banner", "-loglevel", "error",
        "-i", image_path,
        "-vf", vf,
        "-frames:v", "1",
        "-q:v", str(config.JPEG_QUALITY),
        "-vcodec", "mjpeg",
        "-f", "mjpeg", "pipe:1",
    ]
    proc = await asyncio.create_subprocess_exec(
        *args, stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE,
    )
    out, err = await proc.communicate()
    if proc.returncode != 0:
        raise RuntimeError(f"ffmpeg (avatar) failed ({proc.returncode}): {err.decode()[:300]}")
    return out


# Suffix hints ffmpeg's demuxer via the file extension; harmless if it guesses
# from content instead, but avoids ambiguity for a raw WEBP/WEBM byte stream.
_SUFFIX = {"video": ".mp4", "image": ".webp"}


async def process(src_bytes: bytes, meta: dict, *, kind: str = "video",
                  fit: str = "cover") -> dict | None:
    """Full pipeline for one item — a video note, a video sticker, or a static
    sticker (`kind`: "video" | "image"). `fit` picks the crop behaviour for
    "video" (see _run_ffmpeg()) — "image" always uses "contain" regardless.
    `meta` carries id/file_id/sender/chat. Returns the stored row (or None)."""
    video_id = meta["id"]

    tmp = tempfile.NamedTemporaryFile(
        suffix=_SUFFIX.get(kind, ".mp4"), dir=config.CACHE_DIR, delete=False)
    tmp.write(src_bytes)
    tmp.close()
    try:
        mjpeg = await (_run_ffmpeg_static(tmp.name) if kind == "image"
                       else _run_ffmpeg(tmp.name, fit=fit))
    except Exception as e:
        log.error("transcode %s (%s) failed: %s", video_id, kind, e)
        os.unlink(tmp.name)
        return None
    os.unlink(tmp.name)   # purge the source immediately (gotcha: temp only)

    frames = split_frames(mjpeg)
    if not frames:
        log.error("transcode %s produced no frames", video_id)
        return None

    await store.upload_mjpeg(video_id, mjpeg)

    row = {
        "id":          video_id,
        "file_id":     meta.get("file_id"),
        "sender_name": meta.get("sender_name"),
        "sender_id":   meta.get("sender_id"),
        "chat_id":     meta.get("chat_id"),
        "frames":      len(frames),
        "duration_sec": round(len(frames) / max(config.STREAM_FPS, 1), 2),
        "bytes":       len(mjpeg),
        "storage_path": f"{video_id}.mjpeg",
    }
    await store.insert_video(row)

    log.info("processed %s — %d frames, %.1fs, %d bytes",
             video_id, len(frames), row["duration_sec"], len(mjpeg))

    # Notify all connected devices last, once the file is actually retrievable.
    # Sender name + id ride along so the device can show "Last message from
    # <name>" and fetch /avatar/<sender_id> for the incoming pulse screen.
    bus.publish(video_id, meta.get("sender_name"), meta.get("sender_id"))
    return row
