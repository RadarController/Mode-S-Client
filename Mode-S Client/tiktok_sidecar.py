# tiktok_sidecar.py
import json
import os
import random
import sys
import time
import traceback

from TikTokLive import TikTokLiveClient
from TikTokLive.events import CommentEvent, ConnectEvent, DisconnectEvent
from TikTokLive.client.errors import UserOfflineError, SignAPIError


# --- Output helpers ---------------------------------------------------------
# Option A: stdout is *only* newline-delimited JSON events.
# Any human-readable diagnostics should go to stderr.

def emit(obj: dict) -> None:
    sys.stdout.write(json.dumps(obj, ensure_ascii=False) + "\n")
    sys.stdout.flush()


def log(msg: str) -> None:
    sys.stderr.write(str(msg) + "\n")
    sys.stderr.flush()


def now_ts() -> float:
    return time.time()


def safe_int(v, default=0) -> int:
    try:
        if v is None:
            return default
        # Some values come as strings
        return int(v)
    except Exception:
        return default


def get_attr(obj, *names, default=None):
    for n in names:
        if hasattr(obj, n):
            return getattr(obj, n)
    return default


def extract_viewers_from_event(event) -> int | None:
    """
    TikTokLive event schemas vary by version.
    Try a handful of common attribute names used across releases.
    """
    # Most common older name: viewCount (camel)
    v = get_attr(event, "viewCount", "viewerCount", "viewer_count", "view_count", "count", default=None)
    if v is not None:
        return safe_int(v, None)

    # Sometimes nested under event.data or event.room
    data = get_attr(event, "data", "room", "roomInfo", default=None)
    if data is not None:
        v2 = get_attr(data, "viewCount", "viewerCount", "viewer_count", "view_count", "count", default=None)
        if v2 is not None:
            return safe_int(v2, None)

    return None


def extract_followers_from_room_info(room_info) -> int | None:
    """
    Best-effort: try to pull follower count from room_info.
    Not guaranteed to exist (depends on TikTokLive version and what it fetches).
    """
    try:
        # Sometimes: room_info.owner.stats.followerCount / follower_count
        owner = get_attr(room_info, "owner", "user", "host", default=None)
        if owner is None:
            return None

        stats = get_attr(owner, "stats", "statistics", default=None)
        if stats is None:
            return None

        f = get_attr(stats, "followerCount", "follower_count", "followers", default=None)
        if f is None:
            return None

        return safe_int(f, None)
    except Exception:
        return None


def emit_stats(*, live: bool, viewers: int, followers: int | None, room_id=None, note: str | None = None) -> None:
    payload = {
        "type": "tiktok.stats",
        "ts": now_ts(),
        "live": bool(live),
        "viewers": safe_int(viewers, 0),
        "room_id": room_id,
    }
    if followers is not None:
        payload["followers"] = safe_int(followers, 0)
    if note:
        payload["note"] = note
    emit(payload)


emit({"type": "tiktok.boot", "ts": now_ts()})


def load_config() -> dict:
    exe_dir = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
    cfg_path = os.path.join(exe_dir, "config.json")

    emit({"type": "tiktok.info", "ts": now_ts(), "message": "Reading config: " + cfg_path})

    try:
        with open(cfg_path, "r", encoding="utf-8") as f:
            return json.load(f)
    except Exception:
        # Keep config failures non-fatal, but visible.
        emit({
            "type": "tiktok.warn",
            "ts": now_ts(),
            "message": "Failed to read/parse config.json (using defaults)",
            "details": traceback.format_exc(),
        })
        return {}


def main() -> None:
    cfg = load_config()

    unique_id = (cfg.get("tiktok_unique_id", "") or "").replace("@", "").strip()
    emit({"type": "tiktok.config", "ts": now_ts(), "unique_id": unique_id})
    if not unique_id:
        emit({"type": "tiktok.error", "ts": now_ts(), "message": "tiktok_unique_id is empty"})
        # Ensure metrics reset if misconfigured
        emit_stats(live=False, viewers=0, followers=None, room_id=None, note="missing_unique_id")
        return

    client = TikTokLiveClient(unique_id=unique_id)
    emit({"type": "tiktok.info", "ts": now_ts(), "message": "TikTokLiveClient created"})

    # Cookies (your build requires both)
    sessionid = (cfg.get("tiktok_sessionid", "") or "").strip()
    sessionid_ss = (cfg.get("tiktok_sessionid_ss", "") or "").strip()
    tt_target_idc = (cfg.get("tiktok_tt_target_idc", "") or "").strip()

    sid = sessionid if sessionid else sessionid_ss
    if sid and tt_target_idc:
        client.web.set_session(sid, tt_target_idc)
        emit({"type": "tiktok.info", "ts": now_ts(), "message": "Session set (sid + tt-target-idc)"})
    else:
        emit({
            "type": "tiktok.warn",
            "ts": now_ts(),
            "message": "Missing cookies: need tiktok_sessionid (or _ss) AND tiktok_tt_target_idc",
        })

    # Keep last-known stats to avoid spamming duplicates
    last_live = False
    last_viewers = -1
    last_followers = None

    def maybe_emit(live: bool, viewers: int, followers: int | None, note: str | None = None) -> None:
        nonlocal last_live, last_viewers, last_followers
        # Emit when anything changes
        if live != last_live or viewers != last_viewers or followers != last_followers or note is not None:
            rid = getattr(client, "room_id", None)
            emit_stats(live=live, viewers=viewers, followers=followers, room_id=rid, note=note)
            last_live = live
            last_viewers = viewers
            last_followers = followers

    @client.on(ConnectEvent)
    async def on_connect(event: ConnectEvent):
        rid = getattr(client, "room_id", None)
        emit({"type": "tiktok.connected", "ts": now_ts(), "room_id": rid})

        # Best-effort: fetch follower count from room_info if available
        followers = None
        room_info = getattr(client, "room_info", None)
        if room_info is not None:
            followers = extract_followers_from_room_info(room_info)

        # When connected, we are live; viewers may arrive shortly via viewer update events.
        maybe_emit(live=True, viewers=max(0, last_viewers), followers=followers, note="connected")

    @client.on(DisconnectEvent)
    async def on_disconnect(event: DisconnectEvent):
        emit({"type": "tiktok.disconnected", "ts": now_ts()})
        # Reset metrics so /api/metrics doesn't show stale viewers when disconnected
        maybe_emit(live=False, viewers=0, followers=last_followers, note="disconnected")

    async def on_comment(event: CommentEvent):
        user = getattr(getattr(event, "user", None), "nickname", None) or "unknown"
        msg = getattr(event, "comment", "") or ""
        emit({"type": "tiktok.chat", "ts": now_ts(), "user": str(user), "message": str(msg)})

    client.add_listener(CommentEvent, on_comment)
    emit({"type": "tiktok.info", "ts": now_ts(), "message": "Registered listener: CommentEvent"})

    # --- Viewer count updates ------------------------------------------------
    #
    # TikTokLive has changed event names/types over time. We support multiple variants:
    # - Newer versions may expose a class in TikTokLive.events
    # - Older versions used string event names (e.g. "viewer_count_update")
    #
    # We do best-effort extraction of the viewer count from the event payload.

    viewer_event_bound = False

    # Try modern class-based events first
    try:
        from TikTokLive.events import ViewerCountUpdateEvent  # type: ignore

        @client.on(ViewerCountUpdateEvent)  # type: ignore
        async def on_viewer_count_update(event: ViewerCountUpdateEvent):  # type: ignore
            viewers = extract_viewers_from_event(event)
            if viewers is not None:
                # We are definitely live if we're receiving viewer updates
                maybe_emit(live=True, viewers=viewers, followers=last_followers)

        viewer_event_bound = True
        emit({"type": "tiktok.info", "ts": now_ts(), "message": "Registered ViewerCountUpdateEvent (class)"})
    except Exception:
        viewer_event_bound = False

    # Try alternate/older type locations
    if not viewer_event_bound:
        try:
            from TikTokLive.types.events import ViewerCountUpdateEvent  # type: ignore

            @client.on("viewer_count_update")  # older API supported string names
            async def on_viewer_count_update_str(event: ViewerCountUpdateEvent):  # type: ignore
                viewers = extract_viewers_from_event(event)
                if viewers is not None:
                    maybe_emit(live=True, viewers=viewers, followers=last_followers)

            viewer_event_bound = True
            emit({"type": "tiktok.info", "ts": now_ts(), "message": "Registered viewer_count_update (string/type)"})
        except Exception:
            viewer_event_bound = False

    # As a last resort, listen to "all events" and sniff viewer count fields.
    # This is intentionally noisy, but we only emit when values change.
    if not viewer_event_bound:
        try:
            from TikTokLive.events import WebsocketResponseEvent  # type: ignore

            @client.on(WebsocketResponseEvent)  # type: ignore
            async def on_any_event(event: WebsocketResponseEvent):  # type: ignore
                viewers = extract_viewers_from_event(event)
                if viewers is not None:
                    maybe_emit(live=True, viewers=viewers, followers=last_followers)

            emit({"type": "tiktok.warn", "ts": now_ts(), "message": "Viewer event not found; using WebsocketResponseEvent sniffing"})
        except Exception:
            # If we can't bind any viewer event, at least ensure we reset on disconnect/offline.
            emit({"type": "tiktok.warn", "ts": now_ts(), "message": "No viewer update event available in this TikTokLive build"})

    # Retry loop for transient signer failures
    MAX_RETRIES = 6
    BASE_DELAY = 2.0  # seconds

    def emit_sign_error(e: Exception) -> None:
        emit({
            "type": "tiktok.sign_error",
            "ts": now_ts(),
            "message": "ERROR: TikTok signer service failed (temporary). Retrying…",
            "details": str(e),
        })

    for attempt in range(1, MAX_RETRIES + 1):
        try:
            emit({
                "type": "tiktok.info",
                "ts": now_ts(),
                "message": f"Starting client.run() (attempt {attempt}/{MAX_RETRIES})",
            })

            # NOTE: We keep the call as client.run() because your current build uses it successfully.
            # If you later want richer stats via room_info, we can switch to:
            #   client.run(fetch_room_info=True)
            # depending on the TikTokLive version you have installed.
            client.run()

            # If run() returns, connection ended — ensure reset
            maybe_emit(live=False, viewers=0, followers=last_followers, note="run_returned")
            return

        except UserOfflineError:
            emit({
                "type": "tiktok.offline",
                "ts": now_ts(),
                "message": "ERROR: Channel offline",
                "unique_id": unique_id,
            })
            # Reset metrics so /api/metrics doesn't show stale viewers when offline
            maybe_emit(live=False, viewers=0, followers=last_followers, note="offline")
            return

        except SignAPIError as e:
            emit_sign_error(e)

            # Also reset viewers/live while we're retrying so metrics don't stick "live"
            maybe_emit(live=False, viewers=0, followers=last_followers, note="signer_retrying")

            delay = BASE_DELAY * (2 ** (attempt - 1))
            delay = min(delay, 60.0)  # cap
            delay += random.uniform(0.0, 1.5)  # jitter

            emit({"type": "tiktok.info", "ts": now_ts(), "message": f"Retrying in {delay:.1f}s"})
            time.sleep(delay)

            if attempt == MAX_RETRIES:
                emit({
                    "type": "tiktok.error",
                    "ts": now_ts(),
                    "message": "ERROR: TikTok signer service keeps failing. Giving up for now.",
                    "details": str(e),
                })
                maybe_emit(live=False, viewers=0, followers=last_followers, note="signer_give_up")
                return

        except Exception:
            emit({"type": "tiktok.error", "ts": now_ts(), "message": traceback.format_exc()})
            maybe_emit(live=False, viewers=0, followers=last_followers, note="exception")
            return


if __name__ == "__main__":
    try:
        main()
    except Exception:
        # Last-resort catch: never crash silently.
        emit({"type": "tiktok.error", "ts": now_ts(), "message": traceback.format_exc()})
        # Ensure reset so metrics don't remain stale
        emit_stats(live=False, viewers=0, followers=None, room_id=None, note="unhandled_exception")
        log("Unhandled exception in tiktok_sidecar.py")