"""Reconstruct metadata rows for .mjpeg blobs already sitting in CACHE_DIR.

The original rows lived in Supabase Postgres and went away with the project, so
sender/chat are unrecoverable — but frame count, byte size and duration are all
derivable from the blob itself, and the file mtime is a good stand-in for
created_at (it preserves the ordering /videos sorts by). That's enough for the
device's long-press overlay to show something instead of 404ing.

Idempotent, and deliberately non-destructive: a video that already has a row is
skipped, because whatever is in there — a real sender name from a live
transcode — beats anything this can reconstruct.

    python -m app.backfill                              # on the host
    docker compose exec bubblepager python -m app.backfill   # in the container
"""
import asyncio
import datetime as dt
import glob
import logging
import os

from . import config, store
from .transcoder import split_frames

logging.basicConfig(level=logging.INFO, format="%(message)s")
log = logging.getLogger("telepager.backfill")


async def main() -> None:
    await store.init()

    paths = sorted(glob.glob(os.path.join(config.CACHE_DIR, "*.mjpeg")))
    if not paths:
        log.info("no .mjpeg files in %s — nothing to do", config.CACHE_DIR)
        return

    added = skipped = failed = 0
    for path in paths:
        video_id = os.path.splitext(os.path.basename(path))[0]

        if await store.get_video(video_id):
            skipped += 1
            continue

        try:
            with open(path, "rb") as f:
                blob = f.read()
            frames = split_frames(blob)
            if not frames:
                log.warning("  %s — no JPEG frames, skipping", video_id)
                failed += 1
                continue

            mtime = dt.datetime.fromtimestamp(
                os.path.getmtime(path), dt.timezone.utc)

            await store.insert_video({
                "id":           video_id,
                "frames":       len(frames),
                "duration_sec": round(len(frames) / max(config.STREAM_FPS, 1), 2),
                "bytes":        len(blob),
                "storage_path": f"{video_id}.mjpeg",
                "created_at":   mtime.strftime("%Y-%m-%dT%H:%M:%SZ"),
            })
            added += 1
        except Exception as e:
            log.error("  %s — %s", video_id, e)
            failed += 1

    log.info("backfill done: %d added, %d already present, %d failed",
             added, skipped, failed)


if __name__ == "__main__":
    asyncio.run(main())
