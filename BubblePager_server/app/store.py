"""Local store — filesystem for the transcoded .mjpeg blobs, SQLite for metadata.

Replaces the original Supabase Storage + PostgREST backend. Everything lives
under CACHE_DIR (blobs as `<id>.mjpeg`, metadata in `telepager.db`), so a single
Docker volume mounted there captures the entire server state — no external
service to outlive the project.

SQLite work is wrapped in `asyncio.to_thread`. At this scale (one row per video,
one writer) the queries are sub-millisecond, but every caller is async and
blocking the loop mid-transcode would stall the SSE keepalives.
"""
import asyncio
import logging
import os
import sqlite3

from . import config

log = logging.getLogger("telepager.store")

# Mirrors the old Postgres table. `created_at` is filled by SQLite rather than
# the caller — transcoder.py builds the row without it, exactly as it did when
# Postgres supplied `default now()`.
_SCHEMA = """
create table if not exists videos (
    id            text primary key,
    file_id       text,
    sender_name   text,
    sender_id     integer,
    chat_id       integer,
    frames        integer,
    duration_sec  real,
    bytes         integer,
    storage_path  text,
    created_at    text not null default (strftime('%Y-%m-%dT%H:%M:%SZ', 'now'))
);
create index if not exists videos_created_at_idx on videos (created_at desc);
"""

# Columns insert_video() will accept. Anything else in the row dict is ignored
# rather than blowing up the statement. `created_at` is accepted but normally
# omitted — the transcoder lets the column default fire; only the backfill
# supplies it, to preserve the original chronology from file mtimes.
_COLUMNS = ("id", "file_id", "sender_name", "sender_id", "chat_id",
            "frames", "duration_sec", "bytes", "storage_path", "created_at")


def _connect() -> sqlite3.Connection:
    """One connection per operation — they're cheap, and it keeps the threads
    handed out by to_thread from sharing a handle across threads."""
    os.makedirs(os.path.dirname(config.DB_PATH) or ".", exist_ok=True)
    conn = sqlite3.connect(config.DB_PATH, timeout=10.0)
    conn.row_factory = sqlite3.Row
    return conn


def _run(fn):
    """Run fn(conn) in a transaction. `with conn` commits/rolls back but does
    NOT close, so the close is explicit."""
    conn = _connect()
    try:
        with conn:
            return fn(conn)
    finally:
        conn.close()


def _cache_path(video_id: str) -> str:
    os.makedirs(config.CACHE_DIR, exist_ok=True)
    return os.path.join(config.CACHE_DIR, f"{video_id}.mjpeg")


async def init() -> None:
    """Create the schema and switch on WAL. Called once from the app lifespan."""
    def _go(conn):
        conn.execute("pragma journal_mode=WAL")
        conn.executescript(_SCHEMA)

    await asyncio.to_thread(_run, _go)
    log.info("store ready: blobs in %s, db at %s",
             config.CACHE_DIR, config.DB_PATH)


async def healthy() -> bool:
    """Real liveness check for /health. The old endpoint reported whether
    Supabase *credentials were configured*, which stayed true long after the
    project itself had vanished — this actually touches the database."""
    try:
        await asyncio.to_thread(_run, lambda c: c.execute("select 1").fetchone())
        return True
    except Exception as e:
        log.error("store health check failed: %s", e)
        return False


async def aclose() -> None:
    """No-op — connections are per-operation. Kept so the lifespan teardown
    doesn't need to care which backend is in use."""
    return None


# ── Blobs ───────────────────────────────────────────────────────────────────
async def upload_mjpeg(video_id: str, data: bytes) -> str:
    """Persist the transcoded MJPEG. Returns the stored filename."""
    path = f"{video_id}.mjpeg"

    def _go():
        with open(_cache_path(video_id), "wb") as f:
            f.write(data)

    await asyncio.to_thread(_go)
    log.info("stored %s (%d bytes)", path, len(data))
    return path


async def download_mjpeg(video_id: str) -> bytes | None:
    """Read the MJPEG bytes back, or None if we no longer hold it."""
    def _go():
        cp = _cache_path(video_id)
        if not os.path.exists(cp):
            return None
        with open(cp, "rb") as f:
            return f.read()

    return await asyncio.to_thread(_go)


# ── Metadata ────────────────────────────────────────────────────────────────
async def insert_video(meta: dict) -> None:
    """Upsert one row, keyed on id (Telegram file_unique_id)."""
    cols = [c for c in _COLUMNS if c in meta]
    if not cols:
        return
    values = [meta[c] for c in cols]
    placeholders = ", ".join("?" for _ in cols)
    # id is the conflict key, so it's never part of the update.
    updates = ", ".join(f"{c}=excluded.{c}" for c in cols if c != "id")
    sql = (f"insert into videos ({', '.join(cols)}) values ({placeholders}) "
           f"on conflict(id) do update set {updates}" if updates else
           f"insert or ignore into videos ({', '.join(cols)}) values ({placeholders})")

    try:
        await asyncio.to_thread(_run, lambda c: c.execute(sql, values))
    except Exception as e:
        log.error("db insert failed for %s: %s", meta.get("id"), e)


async def get_video(video_id: str) -> dict | None:
    def _go(conn):
        row = conn.execute(
            "select * from videos where id = ?", (video_id,)).fetchone()
        return dict(row) if row else None

    try:
        return await asyncio.to_thread(_run, _go)
    except Exception as e:
        log.error("db get failed for %s: %s", video_id, e)
        return None


async def list_videos(limit: int = 20) -> list[dict]:
    def _go(conn):
        rows = conn.execute(
            "select * from videos order by created_at desc limit ?",
            (limit,)).fetchall()
        return [dict(r) for r in rows]

    try:
        return await asyncio.to_thread(_run, _go)
    except Exception as e:
        log.error("db list failed: %s", e)
        return []
