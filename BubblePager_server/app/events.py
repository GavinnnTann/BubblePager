"""In-process SSE fan-out bus.

A module-level singleton (`bus`) so the transcoder/bot can publish and the
/events endpoint can subscribe without passing handles around. Each SSE client
gets its own asyncio.Queue; publish()/publish_text() fan a payload out to all of
them. Wire format (device parses the leading type tag — see sse_client.cpp):
    V|<video_id>|<sender>|<sender_id>     video note / sticker
    T|<sender>|<sender_id>|<text>          plain text message (text is LAST —
                                            it's the only field allowed to
                                            contain '|', everything before it
                                            is a fixed-format short field)
sender_id (the numeric Telegram user id) drives the incoming-pulse profile-photo
fetch (GET /avatar/<sender_id>) — may be empty if Telegram didn't supply one.
"""
import asyncio
import collections
import logging

log = logging.getLogger("telepager.events")


class EventBus:
    def __init__(self) -> None:
        self._subscribers: set[asyncio.Queue[str]] = set()
        # Payloads published while NO device was connected — delivered to the next
        # device that (re)connects, so videos sent to an offline pager aren't lost.
        self._pending: collections.deque[str] = collections.deque(maxlen=20)

    def subscribe(self) -> asyncio.Queue[str]:
        q: asyncio.Queue[str] = asyncio.Queue(maxsize=32)
        self._subscribers.add(q)
        # Flush the offline backlog to this (re)connecting device, oldest first.
        flushed = 0
        while self._pending:
            try:
                q.put_nowait(self._pending.popleft())
                flushed += 1
            except asyncio.QueueFull:
                break
        log.info("SSE client subscribed (now %d, flushed %d queued)",
                 len(self._subscribers), flushed)
        return q

    def unsubscribe(self, q: asyncio.Queue[str]) -> None:
        self._subscribers.discard(q)
        log.info("SSE client left (now %d)", len(self._subscribers))

    @staticmethod
    def _clean(s: str, max_len: int) -> str:
        """Strip the wire delimiters ('|' and newlines) so a field can't be
        mistaken for a field boundary or corrupt the '\\n'-joined NVS history
        blob on the device."""
        return (s.replace("|", " ").replace("\r", " ").replace("\n", " ")
                 .strip())[:max_len]

    def _send(self, payload: str) -> None:
        """Fan a wire payload out to every connected client, or hold it for the
        next (re)connect if none is online. Drops for any client whose queue is
        full (a wedged/slow device) rather than blocking."""
        if not self._subscribers:
            self._pending.append(payload)
            log.info("no device online — queued %s (%d waiting)",
                     payload, len(self._pending))
            return
        dead = 0
        for q in list(self._subscribers):
            try:
                q.put_nowait(payload)
            except asyncio.QueueFull:
                dead += 1
        log.info("published %s to %d clients (%d full)",
                 payload, len(self._subscribers), dead)

    def publish(self, video_id: str, sender_name: str | None = None,
               sender_id: int | str | None = None) -> None:
        """Fan a video note/sticker out to every connected client."""
        sender = self._clean(sender_name, 40) if sender_name else ""
        sid = str(sender_id) if sender_id else ""
        self._send(f"V|{video_id}|{sender}|{sid}")

    def publish_text(self, sender_name: str, sender_id: int | str | None,
                     text: str) -> None:
        """Fan a plain text message out to every connected client. Caller (bot.py)
        already enforces the character cap — this just sanitises delimiters."""
        sender = self._clean(sender_name or "Someone", 40)
        sid = str(sender_id) if sender_id else ""
        clean_text = self._clean(text, 200)   # matches TEXT_MSG_MAX_CHARS + margin
        self._send(f"T|{sender}|{sid}|{clean_text}")

    @property
    def client_count(self) -> int:
        return len(self._subscribers)

    @property
    def pending_count(self) -> int:
        return len(self._pending)


bus = EventBus()
