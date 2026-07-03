FROM python:3.12-slim

# FFmpeg is required for the MP4 → MJPEG transcode.
RUN apt-get update \
    && apt-get install -y --no-install-recommends ffmpeg \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /srv

COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

COPY app ./app

# The ESP32 connects to this port; expose/map it on the host.
ENV SERVER_PORT=8080
EXPOSE 8080

# uvicorn reads host/port; SERVER_PORT is echoed via the shell so a platform can
# override it (Railway/Render set $PORT — pass -e SERVER_PORT=$PORT there).
CMD ["sh", "-c", "uvicorn app.stream_server:app --host 0.0.0.0 --port ${SERVER_PORT}"]
