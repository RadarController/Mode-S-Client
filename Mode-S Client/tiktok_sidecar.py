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


emit({"type": "tiktok.boot", "ts": time.time()})


def load_config() -> dict:
    exe_dir = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
    cfg_path = os.path.join(exe_dir, "config.json")

    emit({"type": "tiktok.info", "ts": time.time(), "message": "Reading config: " + cfg_path})

    try:
        with open(cfg_path, "r", encoding="utf-8") as f:
            return json.load(f)
    except Exception:
        # Keep config failures non-fatal, but visible.
        emit({
            "type": "tiktok.warn",
            "ts": time.time(),
            "message": "Failed to read/parse config.json (using defaults)",
            "details": traceback.format_exc(),
        })
        return {}


def main() -> None:
    cfg = load_config()

    unique_id = (cfg.get("tiktok_unique_id", "") or "").replace("@", "").strip()
    emit({"type": "tiktok.config", "ts": time.time(), "unique_id": unique_id})
    if not unique_id:
        emit({"type": "tiktok.error", "ts": time.time(), "message": "tiktok_unique_id is empty"})
        return

    client = TikTokLiveClient(unique_id=unique_id)
    emit({"type": "tiktok.info", "ts": time.time(), "message": "TikTokLiveClient created"})

    # Cookies (your build requires both)
    sessionid = (cfg.get("tiktok_sessionid", "") or "").strip()
    sessionid_ss = (cfg.get("tiktok_sessionid_ss", "") or "").strip()
    tt_target_idc = (cfg.get("tiktok_tt_target_idc", "") or "").strip()

    sid = sessionid if sessionid else sessionid_ss
    if sid and tt_target_idc:
        client.web.set_session(sid, tt_target_idc)
        emit({"type": "tiktok.info", "ts": time.time(), "message": "Session set (sid + tt-target-idc)"})
    else:
        emit({
            "type": "tiktok.warn",
            "ts": time.time(),
            "message": "Missing cookies: need tiktok_sessionid (or _ss) AND tiktok_tt_target_idc",
        })

    @client.on(ConnectEvent)
    async def on_connect(event: ConnectEvent):
        emit({"type": "tiktok.connected", "ts": time.time(), "room_id": getattr(client, "room_id", None)})

    @client.on(DisconnectEvent)
    async def on_disconnect(event: DisconnectEvent):
        emit({"type": "tiktok.disconnected", "ts": time.time()})

    async def on_comment(event: CommentEvent):
        user = getattr(getattr(event, "user", None), "nickname", None) or "unknown"
        msg = getattr(event, "comment", "") or ""
        emit({"type": "tiktok.chat", "ts": time.time(), "user": str(user), "message": str(msg)})

    client.add_listener(CommentEvent, on_comment)
    emit({"type": "tiktok.info", "ts": time.time(), "message": "Registered listener: CommentEvent"})

    # Retry loop for transient signer failures
    MAX_RETRIES = 6
    BASE_DELAY = 2.0  # seconds

    def emit_sign_error(e: Exception) -> None:
        emit({
            "type": "tiktok.sign_error",
            "ts": time.time(),
            "message": "ERROR: TikTok signer service failed (temporary). Retrying…",
            "details": str(e),
        })

    for attempt in range(1, MAX_RETRIES + 1):
        try:
            emit({
                "type": "tiktok.info",
                "ts": time.time(),
                "message": f"Starting client.run() (attempt {attempt}/{MAX_RETRIES})",
            })
            client.run()
            return

        except UserOfflineError:
            emit({
                "type": "tiktok.offline",
                "ts": time.time(),
                "message": "ERROR: Channel offline",
                "unique_id": unique_id,
            })
            return

        except SignAPIError as e:
            emit_sign_error(e)

            delay = BASE_DELAY * (2 ** (attempt - 1))
            delay = min(delay, 60.0)  # cap
            delay += random.uniform(0.0, 1.5)  # jitter

            emit({"type": "tiktok.info", "ts": time.time(), "message": f"Retrying in {delay:.1f}s"})
            time.sleep(delay)

            if attempt == MAX_RETRIES:
                emit({
                    "type": "tiktok.error",
                    "ts": time.time(),
                    "message": "ERROR: TikTok signer service keeps failing. Giving up for now.",
                    "details": str(e),
                })
                return

        except Exception:
            emit({"type": "tiktok.error", "ts": time.time(), "message": traceback.format_exc()})
            return


if __name__ == "__main__":
    try:
        main()
    except Exception:
        # Last-resort catch: never crash silently.
        emit({"type": "tiktok.error", "ts": time.time(), "message": traceback.format_exc()})
        log("Unhandled exception in tiktok_sidecar.py")
