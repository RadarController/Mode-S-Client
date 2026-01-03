#!/usr/bin/env python3
# youtube_sidecar.py — YouTube metrics sidecar for Mode-S Client
#
# Purpose (current):
# - Polls YouTube channel page + /live page to emit basic metrics only:
#     * youtube.stats  {live, viewers, followers}
#
# Notes:
# - We avoid Brotli by requesting identity/gzip only (WinHTTP & urllib don't reliably handle br)
# - We set minimal consent cookies to reduce consent-wall responses
# - This script intentionally DOES NOT poll or emit live chat anymore.

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
                raw_io = io.BytesIO(raw)
                raw = gzip.GzipFile(fileobj=raw_io).read()
            except Exception:
                raw = b""
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

def looks_like_consent(html: str) -> bool:
    if not html:
        return False
    h = html.lower()
    return ("consent.youtube.com" in h) or ("form" in h and "consent" in h and "consent" in h)

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

def is_live_page(html: str) -> bool:
    if not html:
        return False
    # Prefer explicit JSON booleans used by YouTube's player/microformat.
    return (
        '"isLiveNow":true' in html or
        '"isLiveContent":true' in html
    )

def parse_live(html: str) -> bool:
    """Return True if the /live page appears to represent an active live stream."""
    if not html:
        return False

    # Scheduled/upcoming streams should not be treated as live.
    if '"isUpcoming":true' in html or '"upcomingEventData"' in html:
        return False

    # Require a videoId plus explicit live markers to reduce false positives.
    vid = extract_video_id(html)
    if not vid:
        return False

    # If YouTube explicitly says not live, trust that unless we also see a true.
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

def parse_viewers(live_html: str) -> int:
    # Prefer concurrentViewers (often present in live page JSON)
    m = re.search(r'"concurrentViewers"\s*:\s*"?(\d+)"?', live_html)
    if m:
        return int(m.group(1))
    # Fallback to visible English text (may vary by locale)
    m = re.search(r'([\d,.]+)\s+watching\s+now', live_html, re.I)
    if m:
        return to_int_compact(m.group(1))
    m = re.search(r'watching\s+now[^0-9]*([\d,.]+)', live_html, re.I)
    if m:
        return to_int_compact(m.group(1))
    return 0

def parse_followers(channel_html: str) -> int:
    # YouTube uses "subscribers" wording on channel pages (locale-dependent).
    # This simple parser targets common English output.
    m = re.search(r'([\d\.]+)\s*([MK]?)\s+subscribers', channel_html, re.I)
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

def main():
    emit({"type": "youtube.boot", "ts": time.time()})
    opener = build_opener()

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

        time.sleep(POLL_INTERVAL)

if __name__ == "__main__":
    main()
