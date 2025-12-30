#!/usr/bin/env python
# tiktok_sidecar.py — Mode-S Client TikTok sidecar (TikTokLive 6.6.5, Python 3.13 safe)
#
# Key fixes:
# - Reads Debug\config.json even when launched from Debug\sidecar\
# - Never calls client.run() (avoids "bound to a different event loop" on Python 3.13)
# - Retries signer failures around await client.connect(...)
# - Emits newline-delimited JSON with flush=True for C++ pipe reader

import asyncio
import json
import sys
import time
import traceback
from pathlib import Path
from typing import Any, Dict, Optional


# Force line-buffered output for subprocess pipes
try:
    sys.stdout.reconfigure(line_buffering=True)
except Exception:
    pass


def now_ts() -> float:
    return time.time()


def emit(obj: Dict[str, Any]) -> None:
    print(json.dumps(obj, ensure_ascii=False), flush=True)


def log_info(msg: str) -> None:
    emit({"type": "tiktok.info", "ts": now_ts(), "message": msg})


def log_warn(msg: str) -> None:
    emit({"type": "tiktok.warn", "ts": now_ts(), "message": msg})


def log_err(msg: str, details: Optional[str] = None) -> None:
    payload: Dict[str, Any] = {"type": "tiktok.error", "ts": now_ts(), "message": msg}
    if details:
        payload["details"] = details
    emit(payload)


def emit_stats(live: bool, viewers: int, followers: Optional[int] = None, room_id: Optional[int] = None, note: str = "") -> None:
    payload: Dict[str, Any] = {
        "type": "tiktok.stats",
        "ts": now_ts(),
        "live": bool(live),
        "viewers": int(viewers),
    }
    if followers is not None:
        payload["followers"] = int(followers)
    if room_id is not None:
        payload["room_id"] = int(room_id)
    if note:
        payload["note"] = note
    emit(payload)


def load_config() -> Dict[str, Any]:
    """
    Prefer config.json next to the executable,
    then Debug\\config.json (dev),
    then current working directory.
    """

    # 1) config.json next to the executable (production)
    try:
        exe_dir = Path(sys.executable).resolve().parent
        p0 = exe_dir / "config.json"
        if p0.exists():
            return json.loads(p0.read_text(encoding="utf-8"))
    except Exception:
        pass

    # 2) Debug\config.json (when running from Debug\sidecar\tiktok_sidecar.py)
    try:
        p1 = Path(__file__).resolve().parent.parent / "config.json"
        if p1.exists():
            return json.loads(p1.read_text(encoding="utf-8"))
    except Exception:
        pass

    # 3) current working directory (fallback)
    try:
        p2 = Path("config.json")
        if p2.exists():
            return json.loads(p2.read_text(encoding="utf-8"))
    except Exception:
        pass

    return {}

def is_signer_failure(text: str) -> bool:
    t = (text or "").lower()
    return (
        "sign" in t
        or "signer" in t
        or "fetch_signed_websocket" in t
        or "sign_not_200" in t
        or "status code 500" in t
        or "504" in t
    )


def extract_viewers_from_room_user_seq(event: Any) -> Optional[int]:
    # common attribute names across builds
    for key in (
        "viewer_count",
        "viewerCount",
        "total_viewer_count",
        "totalViewerCount",
        "total_user",
        "totalUser",
        "online_user",
        "onlineUser",
    ):
        try:
            if hasattr(event, key):
                v = getattr(event, key)
                if isinstance(v, int):
                    return int(v)
        except Exception:
            pass

    # __dict__ fallback
    try:
        d = getattr(event, "__dict__", {}) or {}
        for key in (
            "viewer_count",
            "viewerCount",
            "total_viewer_count",
            "totalViewerCount",
            "total_user",
            "totalUser",
            "online_user",
            "onlineUser",
        ):
            v = d.get(key)
            if isinstance(v, int):
                return int(v)
    except Exception:
        pass

    return None


emit({"type": "tiktok.boot", "ts": now_ts()})

# TikTokLive imports
try:
    from TikTokLive import TikTokLiveClient  # type: ignore
    from TikTokLive.events import ConnectEvent, DisconnectEvent, CommentEvent, RoomUserSeqEvent  # type: ignore
except Exception:
    log_err("Failed to import TikTokLive", traceback.format_exc())
    raise


async def connect_once(unique_id: str, session_id: Optional[str], fetch_room_info: bool) -> None:
    """
    Connects the client once on the CURRENT asyncio loop. No client.run().
    """
    client = TikTokLiveClient(unique_id=unique_id)
    log_info("TikTokLiveClient created")

    # Apply session if possible (TikTokLive 6.6.5 does not accept session_id in __init__)
    if session_id:
        applied = False
        for fn_name in ("set_session", "set_session_id"):
            try:
                fn = getattr(client, fn_name, None)
                if callable(fn):
                    fn(session_id)
                    applied = True
                    break
            except Exception:
                pass
        # Some builds expose a web/session object
        if not applied:
            try:
                web = getattr(client, "web", None)
                if web is not None:
                    fn = getattr(web, "set_session", None)
                    if callable(fn):
                        fn(session_id)
                        applied = True
            except Exception:
                pass

        if applied:
            log_info("Session set (sid + tt-target-idc)")
        else:
            log_warn("Session id present but could not be applied via this TikTokLive build")

    last_viewers = 0
    last_followers: Optional[int] = None
    last_room_id: Optional[int] = None

    @client.on(ConnectEvent)
    async def on_connect(event: ConnectEvent):
        nonlocal last_room_id
        # Room id might exist on some builds
        try:
            rid = getattr(event, "room_id", None)
            if isinstance(rid, int):
                last_room_id = rid
        except Exception:
            pass

        emit({"type": "tiktok.connected", "ts": now_ts()})
        emit_stats(live=True, viewers=last_viewers, followers=last_followers, room_id=last_room_id, note="connected")

    @client.on(DisconnectEvent)
    async def on_disconnect(_: DisconnectEvent):
        emit({"type": "tiktok.offline", "ts": now_ts()})
        emit_stats(live=False, viewers=0, followers=last_followers, room_id=last_room_id, note="disconnected")

    @client.on(CommentEvent)
    async def on_comment(event: CommentEvent):
        try:
            user = "unknown"
            if getattr(event, "user", None) and getattr(event.user, "nickname", None):
                user = str(event.user.nickname)
            msg = str(getattr(event, "comment", "") or "")
            emit({"type": "tiktok.chat", "ts": now_ts(), "user": user, "message": msg})
        except Exception:
            pass

    @client.on(RoomUserSeqEvent)
    async def on_room_user_seq(event: RoomUserSeqEvent):
        nonlocal last_viewers
        viewers = extract_viewers_from_room_user_seq(event)

        # One-time debug if we can't find a viewer field (helps tune extraction)
        if viewers is None:
            if not getattr(on_room_user_seq, "_debugged", False):
                setattr(on_room_user_seq, "_debugged", True)
                try:
                    d = getattr(event, "__dict__", {}) or {}
                    keys = sorted([k for k in d.keys() if not str(k).startswith("_")])[:80]
                    emit({"type": "tiktok.debug", "ts": now_ts(), "label": "RoomUserSeqEvent", "keys": keys})
                except Exception:
                    pass
            return

        if viewers != last_viewers:
            last_viewers = viewers
            emit_stats(live=True, viewers=viewers, followers=last_followers, room_id=last_room_id, note="room_user_seq")

    log_info("Registered listener: CommentEvent")
    log_info("Registered RoomUserSeqEvent (class)")
    log_info(f"Starting asyncio.run(client.connect(fetch_room_info={fetch_room_info}))")

    # This is the critical call (loop-safe)
    await client.connect(fetch_room_info=fetch_room_info)


async def main_async() -> int:
    cfg = load_config()

    # Log which config path we chose
    try:
        preferred = Path(__file__).resolve().parent.parent / "config.json"
        if preferred.exists():
            log_info(f"Reading config: {str(preferred)}")
        else:
            log_info(f"Reading config: {str(Path('config.json').resolve())}")
    except Exception:
        pass

    unique_id = (cfg.get("tiktok_unique_id", "") or "").replace("@", "").strip()
    emit({"type": "tiktok.config", "ts": now_ts(), "unique_id": unique_id})
    if not unique_id:
        log_err("tiktok_unique_id is empty")
        return 1

    session_id = cfg.get("tiktok_session_id")

    MAX_RETRIES = 6
    for attempt in range(1, MAX_RETRIES + 1):
        try:
            await connect_once(unique_id=unique_id, session_id=session_id, fetch_room_info=True)
            # If connect returns, treat as ended
            emit_stats(live=False, viewers=0, note="connect_returned")
            return 0

        except Exception:
            tb = traceback.format_exc()

            if is_signer_failure(tb) and attempt < MAX_RETRIES:
                emit({
                    "type": "tiktok.sign_error",
                    "ts": now_ts(),
                    "message": "ERROR: TikTok signer service failed (temporary). Retrying…",
                    "details": tb.splitlines()[-1] if tb else "",
                })
                emit_stats(live=False, viewers=0, note="signer_retrying")

                delay = min(2.0 * attempt, 10.0) + 0.5
                log_info(f"Retrying in {delay:.1f}s")
                await asyncio.sleep(delay)
                continue

            log_err("Unhandled exception", tb)
            emit_stats(live=False, viewers=0, note="exception")
            return 2

    return 2


def main() -> int:
    return asyncio.run(main_async())


if __name__ == "__main__":
    raise SystemExit(main())