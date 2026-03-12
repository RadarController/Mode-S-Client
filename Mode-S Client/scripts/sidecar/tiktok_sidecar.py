#!/usr/bin/env python
# TikTok sidecar for Mode-S Client
# Hardened viewer-count polling for long-running sessions.

# --- ensure bundled deps are importable when shipped with embedded python ---
import os
import sys
from pathlib import Path as _Path

_here = _Path(__file__).resolve().parent
_bundled = _here / "site-packages"
if _bundled.exists():
    sys.path.insert(0, str(_bundled))

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
        raise SystemExit(0)


def now_ts() -> float:
    return time.time()


# ---------------- TikTok event aggregation ----------------
GIFT_AGG_WINDOW_S = 2.0
_gift_pending = {}

# ---------------- viewer polling hardening ----------------
ROOM_INFO_POLL_INTERVAL_S = 5.0
ROOM_INFO_FETCH_TIMEOUT_S = 8.0
ROOM_INFO_STALE_WARN_S = 20.0
ROOM_INFO_FAILURE_RESET_S = 45.0
MAX_CONSECUTIVE_ROOM_INFO_FAILURES = 3


def emit_event(event_type: str, user: str, message: str, ts_ms: int) -> None:
    emit({
        "type": "tiktok.event",
        "event_type": event_type,
        "user": user,
        "message": message,
        "ts_ms": ts_ms,
        "ts": ts_ms / 1000.0,
    })


def _finalize_gift(key):
    g = _gift_pending.pop(key, None)
    if not g:
        return
    user = g.get("user") or "unknown"
    name = g.get("gift_name") or "Gift"
    count = int(g.get("count") or 1)
    ts_ms = int(g.get("last_ts_ms") or int(now_ts() * 1000))
    emit_event("gift", user, f"sent {name} x{count}", ts_ms)


async def gift_aggregator_loop():
    while True:
        await asyncio.sleep(0.5)
        now = now_ts()
        to_close = []
        for key, g in list(_gift_pending.items()):
            last = float(g.get("last_ts", 0.0))
            if now - last >= GIFT_AGG_WINDOW_S:
                to_close.append(key)
        for key in to_close:
            _finalize_gift(key)


def emit_stats(
    live: bool,
    viewers: int,
    note: str = "",
    room_id: Optional[int] = None,
):
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
from TikTokLive.events import (
    ConnectEvent, DisconnectEvent, CommentEvent, FollowEvent,
    GiftEvent, LikeEvent, ShareEvent, SubscribeEvent
)
from TikTokLive.client.errors import UserOfflineError, WebcastBlocked200Error

try:
    from TikTokLive.client.errors import SignAPIError  # type: ignore
except Exception:
    SignAPIError = None  # type: ignore


# ---------------- helpers ----------------
def extract_user_count(obj: Any) -> Optional[int]:
    if obj is None:
        return None

    try:
        v = getattr(obj, "user_count", None)
        if isinstance(v, int) and v >= 0:
            return v
    except Exception:
        pass

    if isinstance(obj, dict):
        v = obj.get("user_count")
        if isinstance(v, int) and v >= 0:
            return v

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
    res = fn(*args)
    return await res if asyncio.iscoroutine(res) else res


def candidate_fetchers(client: Any) -> Tuple[Tuple[str, Callable[..., Any]], ...]:
    cands: list[Tuple[str, Callable[..., Any]]] = []

    for name in ("fetch_room_info", "get_room_info", "room_info", "fetchRoomInfo", "getRoomInfo"):
        fn = getattr(client, name, None)
        if callable(fn):
            cands.append((f"client.{name}", fn))

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
    for label, fn in candidate_fetchers(client):
        try:
            sig = None
            try:
                sig = inspect.signature(fn)
            except Exception:
                sig = None

            if sig is not None:
                params = list(sig.parameters.values())
                wants_arg = any(
                    p.kind in (p.POSITIONAL_ONLY, p.POSITIONAL_OR_KEYWORD) and p.default is p.empty
                    for p in params[1:]
                )
                if wants_arg and room_id is not None:
                    info = await call_maybe_async(fn, room_id)
                else:
                    info = await call_maybe_async(fn)
            else:
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
            continue
        except Exception:
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

    sessionid = (cfg.get("tiktok_sessionid") or "").strip()
    sessionid_ss = (cfg.get("tiktok_sessionid_ss") or "").strip()

    try:
        if sessionid and hasattr(client, "web") and hasattr(client.web, "set_session_id"):
            client.web.set_session_id(sessionid)
            emit({"type": "tiktok.info", "ts": now_ts(), "message": "sessionid set"})
        if sessionid_ss and hasattr(client, "web") and hasattr(client.web, "set_session_id_ss"):
            client.web.set_session_id_ss(sessionid_ss)
            emit({"type": "tiktok.info", "ts": now_ts(), "message": "sessionid_ss set"})
    except Exception as e:
        emit({"type": "tiktok.warn", "ts": now_ts(), "message": f"failed to set session cookies: {e}"})

    asyncio.create_task(gift_aggregator_loop())

    last_room_id: Optional[int] = None
    last_viewers: int = 0
    dumped_missing_user_count = False
    last_viewers_ok_ts: float = 0.0
    consecutive_room_info_failures: int = 0

    async def poll_room_info():
        nonlocal last_viewers, dumped_missing_user_count, last_room_id
        nonlocal last_viewers_ok_ts, consecutive_room_info_failures

        while True:
            try:
                if last_room_id is None:
                    await asyncio.sleep(1.0)
                    continue

                await asyncio.sleep(ROOM_INFO_POLL_INTERVAL_S)

                try:
                    info, used = await asyncio.wait_for(
                        fetch_fresh_room_info(client, last_room_id),
                        timeout=ROOM_INFO_FETCH_TIMEOUT_S,
                    )
                except asyncio.TimeoutError:
                    consecutive_room_info_failures += 1
                    emit({
                        "type": "tiktok.warn",
                        "ts": now_ts(),
                        "message": f"room_info_poll timeout after {ROOM_INFO_FETCH_TIMEOUT_S:.1f}s",
                        "failures": consecutive_room_info_failures,
                    })
                    if last_viewers_ok_ts > 0 and (now_ts() - last_viewers_ok_ts) >= ROOM_INFO_STALE_WARN_S:
                        emit_stats(True, last_viewers, note="room_info_poll_timeout_stale", room_id=last_room_id)
                    continue

                if info is None:
                    consecutive_room_info_failures += 1
                    emit({
                        "type": "tiktok.warn",
                        "ts": now_ts(),
                        "message": "room_info_poll: no fresh room info method available",
                        "failures": consecutive_room_info_failures,
                    })
                    emit_stats(True, last_viewers, note="room_info_poll_no_fresh_info", room_id=last_room_id)
                    continue

                viewers = extract_user_count(info)

                if viewers is None:
                    consecutive_room_info_failures += 1
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
                    emit_stats(True, last_viewers, note=f"room_info_poll_missing_user_count:{used}", room_id=last_room_id)
                    continue

                consecutive_room_info_failures = 0
                last_viewers = int(viewers)
                last_viewers_ok_ts = now_ts()
                emit_stats(True, last_viewers, note=f"room_info_poll:{used}", room_id=last_room_id)

            except asyncio.CancelledError:
                raise
            except Exception:
                consecutive_room_info_failures += 1
                emit({
                    "type": "tiktok.error",
                    "ts": now_ts(),
                    "message": "room_info_poll crashed; continuing",
                    "details": traceback.format_exc(),
                    "failures": consecutive_room_info_failures,
                })
                await asyncio.sleep(1.0)
                continue

    asyncio.create_task(poll_room_info())

    async def _call_maybe_await(fn: Callable[..., Any], *args: Any, **kwargs: Any) -> bool:
        try:
            r = fn(*args, **kwargs)
            if inspect.isawaitable(r):
                await r
            return True
        except Exception as e:
            emit({"type": "tiktok.warn", "ts": now_ts(), "message": f"send_chat error: {e}"})
            return False

    async def _try_send_chat(text: str) -> bool:
        if hasattr(client, "send_message"):
            return await _call_maybe_await(getattr(client, "send_message"), text)

        if hasattr(client, "web"):
            web = getattr(client, "web")
            for name in ("send_message", "send_chat", "sendMessage", "sendChat"):
                if hasattr(web, name):
                    return await _call_maybe_await(getattr(web, name), text)

        emit({"type": "tiktok.warn", "ts": now_ts(), "message": "sending chat not supported by this TikTokLive build"})
        return False

    async def stdin_loop():
        while True:
            line = await asyncio.to_thread(sys.stdin.readline)
            if not line:
                await asyncio.sleep(0.05)
                continue
            line = line.strip()
            if not line:
                continue

            try:
                cmd = json.loads(line)
            except Exception:
                continue

            if cmd.get("op") == "send_chat":
                text = str(cmd.get("text") or "").strip()
                if not text:
                    continue
                ok = await _try_send_chat(text)
                emit({"type": "tiktok.send_result", "ts": now_ts(), "ok": bool(ok), "text": text})

    asyncio.create_task(stdin_loop())

    @client.on(ConnectEvent)
    async def on_connect(event: ConnectEvent):
        nonlocal last_room_id, last_viewers, last_viewers_ok_ts, consecutive_room_info_failures
        last_room_id = getattr(event, "room_id", None)
        consecutive_room_info_failures = 0

        # Best-effort seed from the connect event itself if it carries room/stats info.
        seeded_viewers = None
        for attr in ("room_info", "room", "stats", "data"):
            try:
                seeded_viewers = extract_user_count(getattr(event, attr, None))
                if seeded_viewers is not None:
                    break
            except Exception:
                pass

        if seeded_viewers is not None:
            last_viewers = int(seeded_viewers)
            last_viewers_ok_ts = now_ts()

        emit({"type": "tiktok.connected", "ts": now_ts()})
        emit_stats(True, last_viewers, note="connected", room_id=last_room_id)

    @client.on(DisconnectEvent)
    async def on_disconnect(_: DisconnectEvent):
        nonlocal consecutive_room_info_failures, last_room_id
        consecutive_room_info_failures = 0
        last_room_id = None
        emit({"type": "tiktok.offline", "ts": now_ts()})
        emit_stats(False, 0, note="disconnected", room_id=last_room_id)

    @client.on(CommentEvent)
    async def on_comment(event: CommentEvent):
        try:
            user_info = getattr(event, "user_info", None)
            user = (
                getattr(user_info, "nick_name", None)
                or getattr(user_info, "username", None)
                or "unknown"
            )
            message = (
                getattr(event, "content", None)
                or getattr(event, "comment", None)
                or getattr(event, "text", None)
                or ""
            )
            emit({
                "type": "tiktok.chat",
                "ts": now_ts(),
                "user": str(user),
                "message": str(message),
            })
        except Exception as ex:
            log("tiktok.error", f"comment handler failed: {ex}")

    @client.on(FollowEvent)
    async def on_follow(event: FollowEvent):
        try:
            user = getattr(event.user, "nickname", None) or getattr(event.user, "unique_id", None) or "unknown"
        except Exception:
            user = "unknown"
        ts_ms = int(now_ts() * 1000)
        emit_event("follow", user, "followed", ts_ms)

    @client.on(ShareEvent)
    async def on_like(event: ShareEvent):
        return

    @client.on(LikeEvent)
    async def on_like(event: LikeEvent):
        return

    @client.on(GiftEvent)
    async def on_gift(event: GiftEvent):
        try:
            user_obj = getattr(event, "user", None)
            user = getattr(user_obj, "nickname", None) or getattr(user_obj, "unique_id", None) or "unknown"
            u_id = getattr(user_obj, "unique_id", None) or user
        except Exception:
            user = "unknown"
            u_id = "unknown"

        gift = getattr(event, "gift", None)
        gift_id = getattr(gift, "id", None) or getattr(event, "gift_id", None) or "unknown"
        gift_name = getattr(gift, "name", None) or getattr(event, "gift_name", None) or "Gift"

        rep = int(getattr(event, "repeat_count", None) or getattr(event, "repeatCount", None) or 1)
        ts = now_ts()
        ts_ms = int(ts * 1000)

        key = (str(u_id), str(gift_id))
        g = _gift_pending.get(key)
        if not g:
            g = {"count": 0, "last_ts": ts, "last_ts_ms": ts_ms, "gift_name": str(gift_name), "user": str(user)}
            _gift_pending[key] = g

        g["count"] = int(g.get("count", 0)) + rep
        g["last_ts"] = ts
        g["last_ts_ms"] = ts_ms
        g["gift_name"] = str(gift_name)
        g["user"] = str(user)

        if bool(getattr(event, "repeat_end", False) or getattr(event, "repeatEnd", False)):
            _finalize_gift(key)

    attempt = 0
    while True:
        attempt += 1
        try:
            await client.connect(fetch_room_info=True)
            emit_stats(False, 0, note="connect_returned", room_id=last_room_id)
            return 0

        except UserOfflineError as e:
            emit({
                "type": "tiktok.info",
                "ts": now_ts(),
                "message": f"User is offline: {e}",
            })
            return 0

        except WebcastBlocked200Error as e:
            emit({
                "type": "tiktok.error",
                "ts": now_ts(),
                "message": f"TikTok blocked this device/session: {e}",
            })
            return 3

        except asyncio.CancelledError:
            emit({
                "type": "tiktok.info",
                "ts": now_ts(),
                "message": "TikTok sidecar cancelled",
            })
            raise

        except KeyboardInterrupt:
            emit({
                "type": "tiktok.info",
                "ts": now_ts(),
                "message": "TikTok sidecar stopped",
            })
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


def main():
    try:
        return asyncio.run(main_async())
    except KeyboardInterrupt:
        emit({
            "type": "tiktok.info",
            "ts": now_ts(),
            "message": "TikTok sidecar stopped by user",
        })
        return 0

if __name__ == "__main__":
    raise SystemExit(main())
