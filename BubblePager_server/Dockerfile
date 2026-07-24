FROM python:3.12-slim

# FFmpeg is required for the MP4 → MJPEG transcode.
RUN apt-get update \
    && apt-get install -y --no-install-recommends ffmpeg \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /srv

COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

COPY app ./app

# Blobs + SQLite default to a mounted volume rather than a path inside the
# image, so nothing persistent is written to the container's writable layer.
ENV CACHE_DIR=/data \
    DB_PATH=/data/telepager.db
VOLUME ["/data"]

# The ESP32 connects to this port; expose/map it on the host.
ENV SERVER_PORT=8090
EXPOSE 8090

# uvicorn reads host/port; SERVER_PORT is echoed via the shell so a platform can
# override it (Railway/Render set $PORT — pass -e SERVER_PORT=$PORT there).
CMD ["sh", "-c", "uvicorn app.stream_server:app --host 0.0.0.0 --port ${SERVER_PORT}"]
