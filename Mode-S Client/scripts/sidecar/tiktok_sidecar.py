#!/usr/bin/env python
# TikTok sidecar for Mode-S Client
# - "viewers" should match TikTok website "watching now"
# - On your payload, "user_count" is the correct field
# - Robust against SignAPIError (temporary signer 500/504)

import asyncio
import json
import sys
import time
import traceback
from pathlib import Path
from typing import Any, Dict, Optional, Tuple

# ---------------- stdout pipe safety (Windows) ----------------
def emit(obj: Dict[str, Any]) -> None:
    try:
        print(json.dumps(obj, ensure_ascii=False), flush=True)
    except (BrokenPipeError, OSError):
        # When piped (findstr/PS) and consumer exits, Windows may throw OSError 22/32.
        raise SystemExit(0)


def now_ts() -> float:
    return time.time()


def log(t: str, msg: str, **extra):
    payload = {"type": t, "ts": now_ts(), "message": msg}
    payload.update(extra)
    emit(payload)


def emit_stats(
    live: bool,
    viewers: int,
    note: str = "",
    room_id: Optional[int] = None,
    **extra
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
from TikTokLive.events import ConnectEvent, DisconnectEvent, CommentEvent, RoomUserSeqEvent

# Signer error type (optional import depending on package layout)
try:
    from TikTokLive.client.errors import SignAPIError  # type: ignore
except Exception:
    SignAPIError = None  # type: ignore

emit({"type": "tiktok.boot", "ts": now_ts()})


# ---------------- helpers: data mining ----------------
def _is_int(v: Any) -> bool:
    return isinstance(v, int) and not isinstance(v, bool)


def collect_ints(obj: Any, prefix: str = "", max_depth: int = 6) -> Dict[str, int]:
    """Recursively collect int fields from dict-like or object-like payloads."""
    out: Dict[str, int] = {}
    if obj is None or max_depth <= 0:
        return out

    if isinstance(obj, dict):
        for k, v in obj.items():
            key = f"{prefix}{k}"
            if _is_int(v):
                out[key] = int(v)
            elif isinstance(v, dict):
                out.update(collect_ints(v, key + ".", max_depth - 1))
            else:
                if hasattr(v, "__dict__"):
                    out.update(collect_ints(v, key + ".", max_depth - 1))
        return out

    try:
        d = getattr(obj, "__dict__", None)
        if isinstance(d, dict):
            for k, v in d.items():
                key = f"{prefix}{k}"
                if _is_int(v):
                    out[key] = int(v)
                elif isinstance(v, dict):
                    out.update(collect_ints(v, key + ".", max_depth - 1))
                else:
                    if hasattr(v, "__dict__"):
                        out.update(collect_ints(v, key + ".", max_depth - 1))
    except Exception:
        pass

    return out


def pick_viewers_from_roominfo_candidates(candidates: Dict[str, int]) -> Tuple[Optional[str], Optional[int]]:
    """
    HARD RULE (per your request):
      - Prefer 'user_count' (matches TikTok website in your debug dump)
      - Else prefer 'stats.watch_user_count' if present and > 0
      - Else try a small safe set of likely concurrent counters
      - Never use totals/cumulative/popularity
    """
    # 1) Exact 'user_count' wins
    if "user_count" in candidates and candidates["user_count"] >= 0:
        return "user_count", candidates["user_count"]

    # Sometimes it may appear under a different path (rare); check endswith
    for k, v in candidates.items():
        if k.lower().endswith(".user_count") and v >= 0:
            return k, v

    # 2) Some payloads provide watch_user_count; only if non-zero
    for k in ("stats.watch_user_count", "watch_user_count", "stats.watchUserCount", "watchUserCount"):
        if k in candidates and candidates[k] > 0:
            return k, candidates[k]

    # 3) Safe fallbacks (concurrent-ish). Still avoid totals.
    safe_keys = [
        "stats.current_viewers",
        "stats.viewer_count",
        "viewer_count",
        "stats.userCount",
        "userCount",
        "stats.online_user",
        "online_user",
    ]
    for k in safe_keys:
        if k in candidates and candidates[k] >= 0:
            return k, candidates[k]

    # 4) Nothing suitable
    return None, None


def is_signer_failure(exc: BaseException) -> bool:
    msg = str(exc).lower()
    if "sign_not_200" in msg or "sign api" in msg or "504" in msg:
        return True
    # If SignAPIError class exists, also match it
    if SignAPIError is not None and isinstance(exc, SignAPIError):
        return True
    return False


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

    dumped_room_user_seq = False
    dumped_room_info_choice = False

    # ---------------- events ----------------
    @client.on(ConnectEvent)
    async def on_connect(event: ConnectEvent):
        nonlocal last_room_id
        last_room_id = getattr(event, "room_id", None)
        emit({"type": "tiktok.connected", "ts": now_ts()})
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

    @client.on(RoomUserSeqEvent)
    async def on_room_user_seq(event: RoomUserSeqEvent):
        # On your build, this event does NOT expose concurrent viewers.
        # Emit one-shot debug keys and ignore.
        nonlocal dumped_room_user_seq
        if not dumped_room_user_seq:
            dumped_room_user_seq = True
            try:
                emit({
                    "type": "tiktok.debug",
                    "ts": now_ts(),
                    "label": "room_user_seq.keys",
                    "keys": list((event.__dict__ or {}).keys()),
                })
            except Exception:
                pass

    # ---------------- room info polling (source of truth) ----------------
    async def poll_room_info():
        nonlocal last_viewers, dumped_room_info_choice

        while True:
            await asyncio.sleep(5)

            info = None

            # Try common async methods
            for fn_name in ("fetch_room_info", "get_room_info", "room_info"):
                try:
                    fn = getattr(client, fn_name, None)
                    if callable(fn):
                        maybe = fn()
                        info = await maybe if asyncio.iscoroutine(maybe) else maybe
                        if info is not None:
                            break
                except Exception:
                    info = None

            # Some builds stash room info on an attribute
            if info is None:
                for attr in ("room_info", "roomInfo", "room", "data"):
                    try:
                        v = getattr(client, attr, None)
                        if v is not None:
                            info = v
                            break
                    except Exception:
                        pass

            if info is None:
                emit({"type": "tiktok.warn", "ts": now_ts(), "message": "room_info_poll: no room info available"})
                continue

            candidates = collect_ints(info)
            chosen_key, chosen_value = pick_viewers_from_roominfo_candidates(candidates)

            # One-shot debug to confirm fields
            if not dumped_room_info_choice:
                dumped_room_info_choice = True
                try:
                    top_by_value = dict(sorted(candidates.items(), key=lambda kv: kv[1], reverse=True)[:30])
                    emit({
                        "type": "tiktok.debug",
                        "ts": now_ts(),
                        "label": "room_info.choice",
                        "chosen_key": chosen_key,
                        "chosen_value": chosen_value,
                        "top_by_value": top_by_value,
                    })
                except Exception:
                    pass

            if chosen_key is None or chosen_value is None:
                emit({"type": "tiktok.warn", "ts": now_ts(), "message": "room_info_poll: no suitable viewer counter found"})
                continue

            # Per your request: populate viewers using the website-like field (user_count)
            last_viewers = int(chosen_value)
            emit_stats(True, last_viewers, note="room_info_poll", room_id=last_room_id, key=chosen_key)

    asyncio.create_task(poll_room_info())

    # ---------------- connect with retry on signer failures ----------------
    attempt = 0
    while True:
        attempt += 1
        try:
            await client.connect(fetch_room_info=True)
            # If connect returns normally, stop
            emit_stats(False, 0, note="connect_returned", room_id=last_room_id)
            return 0
        except Exception as e:
            tb = traceback.format_exc()

            if is_signer_failure(e):
                # Retry with backoff (don’t exit)
                delay = min(2.0 * attempt, 10.0) + 0.5
                emit({"type": "tiktok.warn", "ts": now_ts(), "message": f"Signer failure (temporary). Retrying in {delay:.1f}s", "details": str(e)})
                await asyncio.sleep(delay)
                continue

            # Non-signer error: log and exit
            emit({"type": "tiktok.error", "ts": now_ts(), "message": "Unhandled exception", "details": tb})
            return 2


def main() -> int:
    return asyncio.run(main_async())


if __name__ == "__main__":
    raise SystemExit(main())
