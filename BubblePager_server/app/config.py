"""Environment-backed settings. Loads .env once at import."""
import os
from dotenv import load_dotenv

load_dotenv()


def _req(name: str) -> str:
    v = os.getenv(name, "").strip()
    if not v:
        raise RuntimeError(f"Missing required env var: {name}")
    return v


# Telegram
BOT_TOKEN      = os.getenv("TELEGRAM_BOT_TOKEN", "").strip()
TELEGRAM_MODE  = os.getenv("TELEGRAM_MODE", "polling").strip().lower()   # polling|webhook
PUBLIC_URL     = os.getenv("PUBLIC_URL", "").strip().rstrip("/")
WEBHOOK_SECRET = os.getenv("WEBHOOK_SECRET", "").strip()
TG_API         = f"https://api.telegram.org/bot{BOT_TOKEN}"
TG_FILE_API    = f"https://api.telegram.org/file/bot{BOT_TOKEN}"

# Web server
SERVER_HOST = os.getenv("SERVER_HOST", "0.0.0.0").strip()
SERVER_PORT = int(os.getenv("SERVER_PORT", "8080"))

# Transcode / playback
FFMPEG_BIN   = os.getenv("FFMPEG_BIN", "ffmpeg").strip()
VIDEO_SIZE   = int(os.getenv("VIDEO_SIZE", "240"))
JPEG_QUALITY = int(os.getenv("JPEG_QUALITY", "5"))
STREAM_FPS   = int(os.getenv("STREAM_FPS", "12"))

# Storage — blobs and the SQLite file both live under CACHE_DIR so a single
# Docker volume mounted there holds all persistent state.
CACHE_DIR = os.getenv("CACHE_DIR", "storage").strip()
DB_PATH   = os.getenv("DB_PATH", os.path.join(CACHE_DIR, "telepager.db")).strip()
