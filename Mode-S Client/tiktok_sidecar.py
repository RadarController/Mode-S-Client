import json
import os
import sys
import time
import traceback

from TikTokLive import TikTokLiveClient
from TikTokLive.events import CommentEvent, ConnectEvent, DisconnectEvent

print('{"type":"tiktok.boot","ts":0}', flush=True)

def emit(obj):
    sys.stdout.write(json.dumps(obj, ensure_ascii=False) + "\n")
    sys.stdout.flush()

def load_config():
    exe_dir = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
    cfg_path = os.path.join(exe_dir, "config.json")
    emit({"type": "tiktok.info", "ts": time.time(), "message": "Reading config: " + cfg_path})
    try:
        with open(cfg_path, "r", encoding="utf-8") as f:
            return json.load(f)
    except Exception:
        return {}

def main():
    try:
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
            emit({"type": "tiktok.warn", "ts": time.time(),
                  "message": "Missing cookies: need tiktok_sessionid (or _ss) AND tiktok_tt_target_idc"})

        @client.on(ConnectEvent)
        async def on_connect(event: ConnectEvent):
            emit({"type": "tiktok.connected", "ts": time.time(), "room_id": getattr(client, "room_id", None)})

        @client.on(DisconnectEvent)
        async def on_disconnect(event: DisconnectEvent):
            emit({"type": "tiktok.disconnected", "ts": time.time()})

        async def on_comment(event: CommentEvent):
            # v6.6.5 docs show event.user.nickname and event.comment :contentReference[oaicite:1]{index=1}
            user = getattr(getattr(event, "user", None), "nickname", None) or "unknown"
            msg = getattr(event, "comment", "") or ""
            emit({"type": "tiktok.chat", "ts": time.time(), "user": str(user), "message": str(msg)})

        client.add_listener(CommentEvent, on_comment)
        emit({"type": "tiktok.info", "ts": time.time(), "message": "Registered listener: CommentEvent"})

        emit({"type": "tiktok.info", "ts": time.time(), "message": "Starting client.run()"})
        client.run()

    except Exception:
        emit({"type": "tiktok.error", "ts": time.time(), "message": traceback.format_exc()})

if __name__ == "__main__":
    main()
