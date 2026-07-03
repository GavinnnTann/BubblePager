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

# Supabase
SUPABASE_URL     = os.getenv("SUPABASE_URL", "").strip().rstrip("/")
SUPABASE_KEY     = os.getenv("SUPABASE_SERVICE_ROLE_KEY", "").strip()
SUPABASE_BUCKET  = os.getenv("SUPABASE_BUCKET", "videos").strip()
SUPABASE_TABLE   = os.getenv("SUPABASE_TABLE", "videos").strip()
SUPABASE_ENABLED = bool(SUPABASE_URL and SUPABASE_KEY)

# Web server
SERVER_HOST = os.getenv("SERVER_HOST", "0.0.0.0").strip()
SERVER_PORT = int(os.getenv("SERVER_PORT", "8080"))

# Transcode / playback
FFMPEG_BIN   = os.getenv("FFMPEG_BIN", "ffmpeg").strip()
VIDEO_SIZE   = int(os.getenv("VIDEO_SIZE", "240"))
JPEG_QUALITY = int(os.getenv("JPEG_QUALITY", "5"))
STREAM_FPS   = int(os.getenv("STREAM_FPS", "12"))

CACHE_DIR = os.getenv("CACHE_DIR", "storage").strip()
