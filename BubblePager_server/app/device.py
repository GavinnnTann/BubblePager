"""Multi-device registry + play receipts.

Each pager POSTs a periodic heartbeat (id/name/battery/signal) to /device/heartbeat
and a receipt to /device/ack once it has actually PLAYED a video. We track the
latest heartbeat per device (for /status and /devices) and map video_id → the
Telegram status message so the bot can upgrade "Delivered" to "Played on <name>",
accumulating every device that plays it.
"""
import time

# A device counts as online if it has beaten within this window (heartbeat ~30 s).
DEVICE_ONLINE_SEC = 90
MAX_DEVICES = 200                 # cap the registry — /device/heartbeat is unauthenticated

_devices: dict[str, dict] = {}   # device_id -> {name,battery,charging,rssi,uptime,ts}
_msg_map: dict[str, dict] = {}   # video_id -> {chat_id,message_id,seen:set,ts}


def _prune_devices() -> None:
    """Drop long-stale entries, then hard-cap by evicting the oldest. Bounds
    memory against a flood of spoofed heartbeats with unique ids."""
    now = time.time()
    stale = [k for k, d in _devices.items() if now - d["ts"] > DEVICE_ONLINE_SEC * 20]
    for k in stale:
        _devices.pop(k, None)
    while len(_devices) > MAX_DEVICES:
        oldest = min(_devices, key=lambda k: _devices[k]["ts"])
        _devices.pop(oldest, None)


def set_heartbeat(data: dict) -> str:
    did = str(data.get("id") or "unknown")[:64]   # clamp id length
    _devices[did] = {
        "name":     (data.get("name") or did),
        "battery":  data.get("battery"),
        "charging": bool(data.get("charging")),
        "rssi":     data.get("rssi"),
        "uptime":   data.get("uptime"),
        "ts":       time.time(),
    }
    _prune_devices()
    return did


def devices(only_online: bool = False) -> list[dict]:
    now = time.time()
    out = []
    for did, d in _devices.items():
        age = now - d["ts"]
        online = age < DEVICE_ONLINE_SEC
        if only_online and not online:
            continue
        out.append({**d, "id": did, "age": age, "online": online})
    return sorted(out, key=lambda x: x["ts"], reverse=True)


def online_count() -> int:
    return sum(1 for d in devices() if d["online"])


# ── Play receipts ───────────────────────────────────────────────────────────
def remember_message(video_id: str, chat_id, message_id) -> None:
    """Record which Telegram status message tracks this video (for played acks)."""
    if not (chat_id and message_id):
        return
    _msg_map[video_id] = {"chat_id": chat_id, "message_id": message_id,
                          "seen": set(), "ts": time.time()}
    while len(_msg_map) > 100:            # bound memory
        _msg_map.pop(next(iter(_msg_map)))


def record_play(video_id: str, device_name: str | None):
    """Add a device to the 'seen' set for a video. Returns
    (chat_id, message_id, sorted_names) so the bot can (re)edit the message, or
    None if the video isn't tracked. Kept (not popped) so multiple devices
    accumulate."""
    e = _msg_map.get(video_id)
    if not e:
        return None
    if device_name:
        e["seen"].add(device_name)
    return e["chat_id"], e["message_id"], sorted(e["seen"])
