#!/usr/bin/env python3
# youtube_sidecar.py — YouTube metrics + live chat sidecar for Mode-S Client
#
# - Config loading IDENTICAL to tiktok_sidecar.py
# - Emits:
#     * youtube.stats  {live, viewers, followers}
#     * youtube.chat   {user, message, ts_ms, id}
#
# Notes:
# - We avoid Brotli by requesting identity/gzip only (WinHTTP & urllib don't reliably handle br)
# - We set minimal consent cookies to reduce consent-wall responses

import json
import sys
import time
import re
import ssl
import gzip
import io
from pathlib import Path
import urllib.request
import urllib.error
import http.cookiejar

POLL_INTERVAL = 15  # seconds (stats refresh)

SOCS_COOKIE = "SOCS=CAESEwgDEgk0ODE3Nzk3MjQaAmVuIAEaBgiA_LyOBg"
CONSENT_COOKIE = "CONSENT=YES+1"
BASE_COOKIE = f"{SOCS_COOKIE}; {CONSENT_COOKIE}"

BASE_HEADERS = {
    "User-Agent": (
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
        "AppleWebKit/537.36 (KHTML, like Gecko) "
        "Chrome/120.0.0.0 Safari/537.36"
    ),
    "Accept": "text/html,application/json;q=0.9,*/*;q=0.8",
    "Accept-Language": "en-GB,en;q=0.9",
    "Referer": "https://www.youtube.com/",
    "Accept-Encoding": "gzip, identity",
    "Connection": "close",
}

def emit(obj):
    print(json.dumps(obj, ensure_ascii=False), flush=True)

def sanitize_handle(h):
    h = (h or "").strip()
    if h.startswith("@"):
        h = h[1:]
    return h

# ---------------- config (IDENTICAL TO TIKTOK) ----------------
def load_config():
    candidates = [
        Path(sys.executable).resolve().parent / "config.json",
        Path(__file__).resolve().parent.parent / "config.json",
        Path("config.json"),
    ]
    for p in candidates:
        try:
            if p.exists():
                return json.loads(p.read_text(encoding="utf-8"))
        except Exception:
            pass
    return {}

def build_opener():
    cj = http.cookiejar.CookieJar()
    https_handler = urllib.request.HTTPSHandler(context=ssl.create_default_context())
    opener = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(cj), https_handler)
    return opener

def decode_body(resp, raw: bytes) -> str:
    enc = ""
    try:
        enc = (resp.headers.get("Content-Encoding") or "").lower()
    except Exception:
        enc = ""
    if enc == "gzip":
        try:
            raw = gzip.decompress(raw)
        except Exception:
            # sometimes partial; fallback to GzipFile
            try:
                raw = io.BytesIO(raw)
                raw = gzip.GzipFile(fileobj=raw).read()
            except Exception:
                raw = b""
    # (We do not support br here; request identity/gzip to avoid it)
    try:
        return raw.decode("utf-8", errors="replace")
    except Exception:
        try:
            return raw.decode("latin-1", errors="replace")
        except Exception:
            return ""

def http_get(opener, url: str):
    req = urllib.request.Request(url, method="GET")
    for k, v in BASE_HEADERS.items():
        req.add_header(k, v)
    req.add_header("Cookie", BASE_COOKIE)

    try:
        resp = opener.open(req, timeout=15)
        raw = resp.read()
        return resp.getcode(), decode_body(resp, raw), resp.geturl()
    except urllib.error.HTTPError as e:
        try:
            raw = e.read()
            html = decode_body(e, raw)
        except Exception:
            html = ""
        return e.code or 0, html, e.geturl()
    except Exception:
        return 0, "", url

def http_post_json(opener, url: str, payload: dict, extra_headers: dict):
    data = json.dumps(payload).encode("utf-8")
    req = urllib.request.Request(url, data=data, method="POST")
    for k, v in BASE_HEADERS.items():
        req.add_header(k, v)
    req.add_header("Cookie", BASE_COOKIE)
    req.add_header("Content-Type", "application/json")
    for k, v in (extra_headers or {}).items():
        req.add_header(k, v)

    try:
        resp = opener.open(req, timeout=15)
        raw = resp.read()
        return resp.getcode(), decode_body(resp, raw)
    except urllib.error.HTTPError as e:
        try:
            raw = e.read()
            body = decode_body(e, raw)
        except Exception:
            body = ""
        return e.code or 0, body
    except Exception:
        return 0, ""

def looks_like_consent(html: str) -> bool:
    if not html:
        return False
    h = html.lower()
    return ("consent.youtube.com" in h) or ("form" in h and "consent" in h and "consent" in h)

def is_live_page(html: str) -> bool:
    if not html:
        return False

    # IMPORTANT:
    # The channel /live page can still contain a "videoId" even when the
    # channel is *not* live (YouTube often shows a featured upload/replay).
    # Heuristics like a raw "LIVE" token or presence of live_chat markup
    # can produce false positives.
    #
    # Prefer explicit JSON booleans used by YouTube's player/microformat.
    h = html
    return (
        '"isLiveNow":true' in h or
        '"isLiveContent":true' in h
    )


def parse_live(html: str) -> bool:
    """Return True if the /live page appears to represent an active live stream."""
    if not html:
        return False

    # YouTube may show scheduled/upcoming streams on /live. Treat those as not-live.
    # (The UI should not show "Live" for upcoming.)
    if '"isUpcoming":true' in html or '"upcomingEventData"' in html:
        return False

    # Prefer presence of a videoId plus explicit "is live" markers.
    vid = extract_video_id(html)
    if not vid:
        return False

    # If YouTube explicitly says not live, trust that (unless we also see a true).
    if ('"isLiveNow":false' in html and '"isLiveNow":true' not in html) and (
        '"isLiveContent":true' not in html
    ):
        return False

    return is_live_page(html)

def to_int_compact(s: str) -> int:
    s = (s or "").strip().replace(",", "")
    m = re.match(r"^([0-9]*\.?[0-9]+)\s*([kKmM])?$", s)
    if not m:
        try:
            return int(float(s))
        except Exception:
            return 0
    n = float(m.group(1))
    suf = (m.group(2) or "").upper()
    if suf == "K":
        n *= 1_000
    elif suf == "M":
        n *= 1_000_000
    return int(n)

def parse_viewers(html: str) -> int:
    # Prefer concurrentViewers (often present in live page JSON)
    m = re.search(r'"concurrentViewers"\s*:\s*"?(\d+)"?', html)
    if m:
        return int(m.group(1))
    m = re.search(r'([\d,.]+)\s+watching\s+now', html, re.I)
    if m:
        return to_int_compact(m.group(1))
    m = re.search(r'watching\s+now[^0-9]*([\d,.]+)', html, re.I)
    if m:
        return to_int_compact(m.group(1))
    return 0

def parse_followers(html: str) -> int:
    m = re.search(r'([\d\.]+)\s*([MK]?)\s+subscribers', html, re.I)
    if not m:
        return 0
    try:
        n = float(m.group(1))
    except Exception:
        return 0
    if m.group(2).upper() == "K":
        n *= 1_000
    elif m.group(2).upper() == "M":
        n *= 1_000_000
    return int(n)

# ---------------- YouTube chat scraping ----------------

def extract_video_id(live_html: str) -> str:
    # Best: "videoId":"XXXXXXXXXXX"
    m = re.search(r'"videoId"\s*:\s*"([a-zA-Z0-9_-]{11})"', live_html)
    if m:
        return m.group(1)
    # Fallback: watch?v=
    m = re.search(r'watch\?v=([a-zA-Z0-9_-]{11})', live_html)
    if m:
        return m.group(1)
    return ""

def extract_brace_json_after(text: str, marker: str) -> str:
    i = text.find(marker)
    if i < 0:
        return ""
    # find first '{' after marker
    j = text.find("{", i)
    if j < 0:
        return ""
    depth = 0
    in_str = False
    quote = ""
    esc = False
    for k in range(j, len(text)):
        c = text[k]
        if in_str:
            if esc:
                esc = False
                continue
            if c == "\\":  # escape
                esc = True
                continue
            if c == quote:
                in_str = False
                quote = ""
            continue
        else:
            if c == '"' or c == "'":
                in_str = True
                quote = c
                continue
            if c == "{":
                depth += 1
            elif c == "}":
                depth -= 1
                if depth == 0:
                    return text[j : k + 1]
    return ""

def extract_ytcfg(chat_html: str):
    api_key = ""
    client_ver = ""
    visitor = ""

    cfg = extract_brace_json_after(chat_html, "ytcfg.set(")
    if cfg:
        try:
            j = json.loads(cfg)
            api_key = j.get("INNERTUBE_API_KEY", "") or ""
            client_ver = j.get("INNERTUBE_CLIENT_VERSION", "") or ""
            visitor = j.get("VISITOR_DATA", "") or ""
        except Exception:
            pass

    # crude fallbacks
    if not api_key:
        m = re.search(r'"INNERTUBE_API_KEY"\s*:\s*"([^"]+)"', chat_html)
        if m: api_key = m.group(1)
    if not client_ver:
        m = re.search(r'"INNERTUBE_CLIENT_VERSION"\s*:\s*"([^"]+)"', chat_html)
        if m: client_ver = m.group(1)
    if not visitor:
        m = re.search(r'"VISITOR_DATA"\s*:\s*"([^"]+)"', chat_html)
        if m: visitor = m.group(1)

    return api_key, client_ver, visitor

def extract_initial_continuation(chat_html: str) -> str:
    # Usually many continuations; pick the first
    m = re.search(r'"continuation"\s*:\s*"([^"]+)"', chat_html)
    return m.group(1) if m else ""

def walk_for_chat_renderers(node, out_msgs):
    if isinstance(node, dict):
        if "liveChatTextMessageRenderer" in node and isinstance(node["liveChatTextMessageRenderer"], dict):
            r = node["liveChatTextMessageRenderer"]
            mid = r.get("id") or ""
            user = ""
            msg = ""

            try:
                user = (r.get("authorName", {}) or {}).get("simpleText", "") or ""
            except Exception:
                user = ""

            # message runs (text + emoji shortcuts)
            try:
                runs = ((r.get("message", {}) or {}).get("runs")) or []
                parts = []
                for run in runs:
                    if isinstance(run, dict) and "text" in run:
                        parts.append(str(run.get("text") or ""))
                    elif isinstance(run, dict) and "emoji" in run and isinstance(run["emoji"], dict):
                        e = run["emoji"]
                        sc = e.get("shortcuts") or []
                        if isinstance(sc, list) and sc:
                            parts.append(str(sc[0]))
                        else:
                            eid = e.get("emojiId")
                            parts.append(f":{eid}:" if eid else "�")
                msg = "".join(parts).strip()
            except Exception:
                msg = ""

            if user and msg:
                out_msgs.append({"id": mid, "user": user, "message": msg})


        # Super Chat (paid message)
        if "liveChatPaidMessageRenderer" in node and isinstance(node["liveChatPaidMessageRenderer"], dict):
            r = node["liveChatPaidMessageRenderer"]
            mid = r.get("id") or ""
            user = ""
            amount = ""
            msg = ""
            try:
                user = (r.get("authorName", {}) or {}).get("simpleText", "") or ""
                amount = (r.get("purchaseAmountText", {}) or {}).get("simpleText", "") or ""
            except Exception:
                pass

            # message runs (text + emoji shortcuts)
            try:
                runs = ((r.get("message", {}) or {}).get("runs")) or []
                parts = []
                for run in runs:
                    if isinstance(run, dict) and "text" in run:
                        parts.append(str(run.get("text") or ""))
                    elif isinstance(run, dict) and "emoji" in run and isinstance(run["emoji"], dict):
                        e = run["emoji"]
                        sc = e.get("shortcuts") or []
                        if isinstance(sc, list) and sc:
                            parts.append(str(sc[0]))
                        else:
                            eid = e.get("emojiId")
                            parts.append(f":{eid}:" if eid else "�")
                msg = "".join(parts).strip()
            except Exception:
                msg = ""

            label = f"[SUPERCHAT {amount}] " if amount else "[SUPERCHAT] "
            out_msgs.append({"id": mid, "user": user or "YouTube", "message": (label + msg).strip()})

        # Super Sticker (paid sticker)
        if "liveChatPaidStickerRenderer" in node and isinstance(node["liveChatPaidStickerRenderer"], dict):
            r = node["liveChatPaidStickerRenderer"]
            mid = r.get("id") or ""
            user = ""
            amount = ""
            try:
                user = (r.get("authorName", {}) or {}).get("simpleText", "") or ""
                amount = (r.get("purchaseAmountText", {}) or {}).get("simpleText", "") or ""
            except Exception:
                pass
            label = f"[SUPERSTICKER {amount}]" if amount else "[SUPERSTICKER]"
            out_msgs.append({"id": mid, "user": user or "YouTube", "message": label})

        # Membership (new member / milestone)
        if "liveChatMembershipItemRenderer" in node and isinstance(node["liveChatMembershipItemRenderer"], dict):
            r = node["liveChatMembershipItemRenderer"]
            mid = r.get("id") or ""
            user = ""
            msg = ""
            try:
                user = (r.get("authorName", {}) or {}).get("simpleText", "") or ""
                msg = (r.get("headerSubtext", {}) or {}).get("simpleText", "") or ""
            except Exception:
                pass
            out_msgs.append({"id": mid, "user": user or "YouTube", "message": f"[MEMBERSHIP] {msg or 'became a member'}".strip()})

        for v in node.values():
            walk_for_chat_renderers(v, out_msgs)
    elif isinstance(node, list):
        for v in node:
            walk_for_chat_renderers(v, out_msgs)

def extract_next_continuation_and_timeout(j) -> tuple[str, int]:
    cont = ""
    timeout_ms = 1500
    try:
        cc = j.get("continuationContents", {}) or {}
        lc = cc.get("liveChatContinuation", {}) or {}
        conts = lc.get("continuations", []) or []
        if conts:
            c0 = conts[0] or {}
            t = c0.get("timedContinuationData") or c0.get("invalidationContinuationData") or {}
            cont = t.get("continuation") or ""
            timeout_ms = int(t.get("timeoutMs") or timeout_ms)
    except Exception:
        pass
    timeout_ms = max(250, min(10000, timeout_ms))
    return cont, timeout_ms

def ensure_chat_session(opener, video_id: str, state: dict):
    # state keys: api_key, client_ver, visitor, continuation
    chat_url = f"https://www.youtube.com/live_chat?is_popout=1&v={video_id}"
    code, html, _ = http_get(opener, chat_url)
    if code != 200 or not html:
        return False
    api_key, client_ver, visitor = extract_ytcfg(html)
    cont = extract_initial_continuation(html)
    if not api_key or not cont:
        return False
    state["api_key"] = api_key
    state["client_ver"] = client_ver or "2.20250101.00.00"
    state["visitor"] = visitor or ""
    state["continuation"] = cont
    return True

def poll_chat_once(opener, state: dict):
    api_key = state.get("api_key", "")
    client_ver = state.get("client_ver", "")
    visitor = state.get("visitor", "")
    continuation = state.get("continuation", "")
    if not api_key or not continuation:
        return False, [], 1500

    payload = {
        "context": {
            "client": {
                "clientName": "WEB",
                "clientVersion": client_ver or "2.20250101.00.00",
            }
        },
        "continuation": continuation,
    }

    url = f"https://www.youtube.com/youtubei/v1/live_chat/get_live_chat?key={api_key}"
    headers = {
        "X-Youtube-Client-Name": "1",
        "X-Youtube-Client-Version": client_ver or "2.20250101.00.00",
        "Origin": "https://www.youtube.com",
    }
    if visitor:
        headers["X-Goog-Visitor-Id"] = visitor

    code, body = http_post_json(opener, url, payload, headers)
    if code != 200 or not body:
        return False, [], 2500

    try:
        j = json.loads(body)
    except Exception:
        return False, [], 2500

    msgs = []
    try:
        walk_for_chat_renderers(j, msgs)
    except Exception:
        msgs = []

    next_cont, timeout_ms = extract_next_continuation_and_timeout(j)
    if next_cont:
        state["continuation"] = next_cont

    return True, msgs, timeout_ms

def main():
    emit({"type": "youtube.boot", "ts": time.time()})
    opener = build_opener()

    # chat state
    chat_state = {"video_id": "", "api_key": "", "client_ver": "", "visitor": "", "continuation": ""}
    seen_ids = set()

    while True:
        cfg = load_config()
        handle = sanitize_handle(cfg.get("youtube_handle", ""))

        if not handle:
            emit({
                "type": "youtube.stats",
                "ts": time.time(),
                "live": False,
                "viewers": 0,
                "followers": 0,
                "note": "missing_handle",
            })
            time.sleep(POLL_INTERVAL)
            continue

        channel_url = f"https://www.youtube.com/@{handle}"
        live_url = f"{channel_url}/live"

        _, channel_html, _ = http_get(opener, channel_url)
        _, live_html, _ = http_get(opener, live_url)

        consent = looks_like_consent(channel_html) or looks_like_consent(live_html)

        live = parse_live(live_html)
        viewers = parse_viewers(live_html) if live else 0

        # Followers: primary = channel page, fallback = live page
        followers = parse_followers(channel_html)
        if followers == 0:
            followers = parse_followers(live_html)

        emit({
            "type": "youtube.stats",
            "ts": time.time(),
            "live": live,
            "viewers": viewers,
            "followers": followers,
            "note": ("consent" if consent else None),
        })

        # Chat polling: run for up to (POLL_INTERVAL - small slack) seconds between stats refreshes
        if live:
            video_id = extract_video_id(live_html)
            if video_id and video_id != chat_state.get("video_id"):
                # Stream changed -> reset
                chat_state = {"video_id": video_id, "api_key": "", "client_ver": "", "visitor": "", "continuation": ""}
                seen_ids.clear()

            if video_id and (not chat_state.get("api_key") or not chat_state.get("continuation")):
                ok = ensure_chat_session(opener, video_id, chat_state)
                if not ok:
                    # don't spam; try again next stats tick
                    time.sleep(POLL_INTERVAL)
                    continue

            end = time.time() + max(1.0, POLL_INTERVAL - 1.0)
            while time.time() < end and chat_state.get("api_key") and chat_state.get("continuation"):
                ok, msgs, sleep_ms = poll_chat_once(opener, chat_state)
                if ok:
                    now_ms = int(time.time() * 1000)
                    for m in msgs:
                        mid = m.get("id") or ""
                        # Dedupe: prefer message id when present, otherwise hash user+msg+time bucket
                        key = mid or f"{m.get('user','')}|{m.get('message','')}|{now_ms//2000}"
                        if key in seen_ids:
                            continue
                        seen_ids.add(key)
                        # bound memory
                        if len(seen_ids) > 5000:
                            # drop ~half
                            seen_ids = set(list(seen_ids)[-2500:])

                        # NOTE: Chat is handled by the native integration now (C++), so we do NOT emit youtube.chat here.
                        # Emitting chat from both sources causes duplicates in /api/chat (one with runs, one without).
                        pass

#                        emit({
#                            "type": "youtube.chat",
#                            "ts": time.time(),
#                            "ts_ms": now_ms,
#                            "id": mid,
#                            "user": m.get("user", ""),
#                            "message": m.get("message", ""),
#                        })

                # Sleep based on server hint (or backoff on error)
                time.sleep(max(0.25, min(10.0, sleep_ms / 1000.0)))
        else:
            # not live -> reset chat session
            chat_state = {"video_id": "", "api_key": "", "client_ver": "", "visitor": "", "continuation": ""}
            seen_ids.clear()

        # Next stats tick
        time.sleep(POLL_INTERVAL)

if __name__ == "__main__":
    main()