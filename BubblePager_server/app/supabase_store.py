"""Supabase Storage (transcoded .mjpeg) + Postgres (metadata) access via httpx.

Server-side only — uses the service-role key. If Supabase isn't configured the
module degrades to a local-disk cache (dev mode) so the pipeline still works
before keys are supplied.
"""
import logging
import os

import httpx

from . import config

log = logging.getLogger("telepager.store")

_client: httpx.AsyncClient | None = None


def _http() -> httpx.AsyncClient:
    global _client
    if _client is None:
        _client = httpx.AsyncClient(timeout=30.0)
    return _client


async def aclose() -> None:
    global _client
    if _client is not None:
        await _client.aclose()
        _client = None


def _headers(extra: dict | None = None) -> dict:
    h = {
        "apikey": config.SUPABASE_KEY,
        "Authorization": f"Bearer {config.SUPABASE_KEY}",
    }
    if extra:
        h.update(extra)
    return h


def _cache_path(video_id: str) -> str:
    os.makedirs(config.CACHE_DIR, exist_ok=True)
    return os.path.join(config.CACHE_DIR, f"{video_id}.mjpeg")


# ── Storage ─────────────────────────────────────────────────────────────────
async def upload_mjpeg(video_id: str, data: bytes) -> str:
    """Upload the transcoded MJPEG. Always writes the local cache too. Returns
    the storage path (bucket-relative)."""
    path = f"{video_id}.mjpeg"
    with open(_cache_path(video_id), "wb") as f:
        f.write(data)

    if not config.SUPABASE_ENABLED:
        log.warning("Supabase disabled — kept %s in local cache only", path)
        return path

    url = f"{config.SUPABASE_URL}/storage/v1/object/{config.SUPABASE_BUCKET}/{path}"
    r = await _http().post(
        url,
        headers=_headers({"Content-Type": "image/jpeg", "x-upsert": "true"}),
        content=data,
    )
    if r.status_code >= 300:
        log.error("Storage upload failed %s: %s", r.status_code, r.text[:200])
        r.raise_for_status()
    log.info("uploaded %s (%d bytes) to Supabase Storage", path, len(data))
    return path


async def download_mjpeg(video_id: str) -> bytes | None:
    """Fetch the MJPEG bytes — local cache first, then Supabase Storage."""
    cp = _cache_path(video_id)
    if os.path.exists(cp):
        with open(cp, "rb") as f:
            return f.read()

    if not config.SUPABASE_ENABLED:
        return None

    path = f"{video_id}.mjpeg"
    url = f"{config.SUPABASE_URL}/storage/v1/object/{config.SUPABASE_BUCKET}/{path}"
    r = await _http().get(url, headers=_headers())
    if r.status_code == 200:
        with open(cp, "wb") as f:      # warm the cache for next replay
            f.write(r.content)
        return r.content
    log.warning("Storage download %s → %s", path, r.status_code)
    return None


# ── Postgres (PostgREST) ────────────────────────────────────────────────────
async def insert_video(meta: dict) -> None:
    """Upsert a metadata row keyed on id (Telegram file_unique_id)."""
    if not config.SUPABASE_ENABLED:
        return
    url = f"{config.SUPABASE_URL}/rest/v1/{config.SUPABASE_TABLE}"
    r = await _http().post(
        url,
        headers=_headers({
            "Content-Type": "application/json",
            "Prefer": "resolution=merge-duplicates,return=minimal",
        }),
        json=[meta],
    )
    if r.status_code >= 300:
        log.error("DB insert failed %s: %s", r.status_code, r.text[:200])


async def get_video(video_id: str) -> dict | None:
    if not config.SUPABASE_ENABLED:
        return None
    url = f"{config.SUPABASE_URL}/rest/v1/{config.SUPABASE_TABLE}"
    r = await _http().get(
        url, headers=_headers(),
        params={"id": f"eq.{video_id}", "select": "*", "limit": 1},
    )
    if r.status_code == 200 and r.json():
        return r.json()[0]
    return None


async def list_videos(limit: int = 20) -> list[dict]:
    if not config.SUPABASE_ENABLED:
        return []
    url = f"{config.SUPABASE_URL}/rest/v1/{config.SUPABASE_TABLE}"
    r = await _http().get(
        url, headers=_headers(),
        params={"select": "*", "order": "created_at.desc", "limit": str(limit)},
    )
    return r.json() if r.status_code == 200 else []
