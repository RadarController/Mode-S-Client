#!/usr/bin/env python
# TikTok sidecar for Mode-S Client
# FINAL SIMPLIFIED VERSION
#
# Viewers logic:
#   - viewers == room_info.user_count ONLY
#   - No totals, no popularity, no heuristics
#   - Matches TikTok website "watching now"

import asyncio
import json
import sys
import time
import traceback
import inspect
from pathlib import Path
from typing import Any, Dict, Optional, Callable, Tuple

# ---------------- stdout pipe safety (Windows) ----------------
def emit(obj: Dict[str, Any]) -> None:
    try:
        print(json.dumps(obj, ensure_ascii=False), flush=True)
    except (BrokenPipeError, OSError):
        # Downstream pipe closed (findstr / PowerShell)
        raise SystemExit(0)


def now_ts() -> float:
    return time.time()


def emit_stats(
    live: bool,
    viewers: int,
    note: str = "",
    room_id: Optional[int] = None,
):
    # IMPORTANT: Always emit a fresh stats object so downstream rewrites the value each poll.
    payload = {
        "type": "tiktok.stats",
        "ts": now_ts(),
        "live": bool(live),
        "viewers": int(viewers),
        "note": note,
    }
    if room_id is not None:
        payload["room_id"] = int(room_id)
    emit(payload)


def log(t: str, msg: str, **extra):
    payload = {"type": t, "ts": now_ts(), "message": msg}
    payload.update(extra)
    emit(payload)


# ---------------- config ----------------
def load_config() -> Dict[str, Any]:
    candidates = [
        Path(sys.executable).resolve().parent / "config.json",
        Path(__file__).resolve().parent.parent / "config.json",
        Path("config.json"),
    ]
    for p in candidates:
        if p.exists():
            return json.loads(p.read_text(encoding="utf-8"))
    return {}


# ---------------- TikTokLive ----------------
from TikTokLive import TikTokLiveClient
from TikTokLive.events import ConnectEvent, DisconnectEvent, CommentEvent

try:
    from TikTokLive.client.errors import SignAPIError  # type: ignore
except Exception:
    SignAPIError = None  # type: ignore


emit({"type": "tiktok.boot", "ts": now_ts()})


# ---------------- helpers ----------------
def extract_user_count(obj: Any) -> Optional[int]:
    """
    Extract ONLY user_count from room info.
    This matches the TikTok website viewer number on this build.
    """
    if obj is None:
        return None

    # Direct attribute
    try:
        v = getattr(obj, "user_count", None)
        if isinstance(v, int) and v >= 0:
            return v
    except Exception:
        pass

    # Dict
    if isinstance(obj, dict):
        v = obj.get("user_count")
        if isinstance(v, int) and v >= 0:
            return v

    # Nested common containers
    for name in ("stats", "data", "room", "room_info", "roomInfo"):
        try:
            nested = getattr(obj, name, None)
            v = extract_user_count(nested)
            if v is not None:
                return v
        except Exception:
            pass

    return None


def is_signer_failure(exc: BaseException) -> bool:
    msg = str(exc).lower()
    if "sign_not_200" in msg or "sign api" in msg or "504" in msg:
        return True
    if SignAPIError is not None and isinstance(exc, SignAPIError):
        return True
    return False


async def call_maybe_async(fn: Callable[..., Any], *args: Any) -> Any:
    """
    Call fn with args, supporting both sync and async callables.
    """
    res = fn(*args)
    return await res if asyncio.iscoroutine(res) else res


def candidate_fetchers(client: Any) -> Tuple[Tuple[str, Callable[..., Any]], ...]:
    """
    Return a list of (label, callable) fetchers that are likely to perform a *fresh* room info fetch.
    We try public APIs first, then common internal ones used across TikTokLive versions.
    """
    cands: list[Tuple[str, Callable[..., Any]]] = []

    # Public-ish names seen across versions
    for name in ("fetch_room_info", "get_room_info", "room_info", "fetchRoomInfo", "getRoomInfo"):
        fn = getattr(client, name, None)
        if callable(fn):
            cands.append((f"client.{name}", fn))

    # Internal/common objects that sometimes exist
    for obj_name in ("_web", "web", "_client", "client", "_room", "room"):
        obj = getattr(client, obj_name, None)
        if obj is None:
            continue
        for name in ("fetch_room_info", "get_room_info", "room_info", "fetchRoomInfo", "getRoomInfo"):
            fn = getattr(obj, name, None)
            if callable(fn):
                cands.append((f"{obj_name}.{name}", fn))

    return tuple(cands)


async def fetch_fresh_room_info(client: Any, room_id: Optional[int]) -> Tuple[Optional[Any], str]:
    """
    Try hard to fetch a fresh room-info object every poll.
    Returns (info, label_used). If nothing worked: (None, "none").
    """
    for label, fn in candidate_fetchers(client):
        try:
            # Some versions require room_id; others accept no args.
            sig = None
            try:
                sig = inspect.signature(fn)
            except Exception:
                sig = None

            if sig is not None:
                # If it looks like it expects at least 1 positional arg (beyond self),
                # try with room_id first when available.
                params = list(sig.parameters.values())
                wants_arg = any(
                    p.kind in (p.POSITIONAL_ONLY, p.POSITIONAL_OR_KEYWORD) and p.default is p.empty
                    for p in params[1:]  # skip self
                )
                if wants_arg and room_id is not None:
                    info = await call_maybe_async(fn, room_id)
                else:
                    info = await call_maybe_async(fn)
            else:
                # No signature info; try (room_id) then ().
                if room_id is not None:
                    try:
                        info = await call_maybe_async(fn, room_id)
                    except TypeError:
                        info = await call_maybe_async(fn)
                else:
                    info = await call_maybe_async(fn)

            if info is not None:
                return info, label
        except TypeError:
            # Wrong arity; keep trying other candidates
            continue
        except Exception:
            # Fetch failed; try next candidate
            continue

    return None, "none"


# ---------------- main ----------------
async def main_async() -> int:
    cfg = load_config()
    unique_id = (cfg.get("tiktok_unique_id") or "").replace("@", "").strip()

    emit({"type": "tiktok.config", "ts": now_ts(), "unique_id": unique_id})

    if not unique_id:
        log("tiktok.error", "tiktok_unique_id missing")
        return 1

    client = TikTokLiveClient(unique_id=unique_id)
    log("tiktok.info", "TikTokLiveClient created")

    last_room_id: Optional[int] = None
    last_viewers: int = 0
    dumped_missing_user_count = False

    # ---------------- events ----------------
    @client.on(ConnectEvent)
    async def on_connect(event: ConnectEvent):
        nonlocal last_room_id
        last_room_id = getattr(event, "room_id", None)
        emit({"type": "tiktok.connected", "ts": now_ts()})
        # Emit immediately (may be 0 until first poll refreshes)
        emit_stats(True, last_viewers, note="connected", room_id=last_room_id)

    @client.on(DisconnectEvent)
    async def on_disconnect(_: DisconnectEvent):
        emit({"type": "tiktok.offline", "ts": now_ts()})
        emit_stats(False, 0, note="disconnected", room_id=last_room_id)

    @client.on(CommentEvent)
    async def on_comment(event: CommentEvent):
        try:
            emit({
                "type": "tiktok.chat",
                "ts": now_ts(),
                "user": event.user.nickname if event.user else "unknown",
                "message": event.comment,
            })
        except Exception:
            pass

    # ---------------- room info polling (SOURCE OF TRUTH) ----------------
    async def poll_room_info():
        nonlocal last_viewers, dumped_missing_user_count

        while True:
            await asyncio.sleep(5)

            # Always try to fetch *fresh* room info each poll
            info, used = await fetch_fresh_room_info(client, last_room_id)

            if info is None:
                # Do NOT fall back to cached attributes (they can be stale).
                emit({
                    "type": "tiktok.warn",
                    "ts": now_ts(),
                    "message": "room_info_poll: no fresh room info method available",
                })
                # Still emit stats so downstream sees ongoing polls (viewers stays as last known).
                emit_stats(True, last_viewers, note="room_info_poll_no_fresh_info", room_id=last_room_id)
                continue

            viewers = extract_user_count(info)

            if viewers is None:
                if not dumped_missing_user_count:
                    dumped_missing_user_count = True
                    try:
                        emit({
                            "type": "tiktok.debug",
                            "ts": now_ts(),
                            "label": "user_count_missing",
                            "fetcher": used,
                            "keys": list((getattr(info, "__dict__", {}) or {}).keys()),
                        })
                    except Exception:
                        pass
                # Still emit stats each poll (keep last_viewers) so field is rewritten.
                emit_stats(True, last_viewers, note=f"room_info_poll_missing_user_count:{used}", room_id=last_room_id)
                continue

            last_viewers = int(viewers)

            # IMPORTANT: emit every poll, not only when it changes
            emit_stats(True, last_viewers, note=f"room_info_poll:{used}", room_id=last_room_id)

    asyncio.create_task(poll_room_info())

    # ---------------- connect with retry on signer failures ----------------
    attempt = 0
    while True:
        attempt += 1
        try:
            await client.connect(fetch_room_info=True)
            emit_stats(False, 0, note="connect_returned", room_id=last_room_id)
            return 0
        except Exception as e:
            if is_signer_failure(e):
                delay = min(2.0 * attempt, 10.0) + 0.5
                emit({
                    "type": "tiktok.warn",
                    "ts": now_ts(),
                    "message": f"Signer failure (temporary). Retrying in {delay:.1f}s",
                })
                await asyncio.sleep(delay)
                continue

            emit({
                "type": "tiktok.error",
                "ts": now_ts(),
                "message": "Unhandled exception",
                "details": traceback.format_exc(),
            })
            return 2


def main() -> int:
    return asyncio.run(main_async())


if __name__ == "__main__":
    raise SystemExit(main())
