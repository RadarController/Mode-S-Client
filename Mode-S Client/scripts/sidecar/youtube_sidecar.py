#!/usr/bin/env python3
# youtube_sidecar.py — config loading IDENTICAL to tiktok_sidecar.py

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

POLL_INTERVAL = 15

SOCS_COOKIE = "SOCS=CAESEwgDEgk0ODE3Nzk3MjQaAmVuIAEaBgiA_LyaBg"

BASE_HEADERS = {
    "User-Agent": (
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
        "AppleWebKit/537.36 (KHTML, like Gecko) "
        "Chrome/120.0.0.0 Safari/537.36"
    ),
    "Accept": "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8",
    "Accept-Language": "en-GB,en;q=0.9",
    "Referer": "https://www.youtube.com/",
    "Accept-Encoding": "gzip",
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
        if p.exists():
            return json.loads(p.read_text(encoding="utf-8"))
    return {}
# -------------------------------------------------------------

def build_opener():
    cj = http.cookiejar.CookieJar()
    ctx = ssl.create_default_context()
    https_handler = urllib.request.HTTPSHandler(context=ctx)
    return urllib.request.build_opener(
        https_handler,
        urllib.request.HTTPCookieProcessor(cj)
    )

def decode_body(resp, raw: bytes) -> str:
    enc = resp.headers.get("Content-Encoding", "")
    if enc and "gzip" in enc.lower():
        try:
            raw = gzip.GzipFile(fileobj=io.BytesIO(raw)).read()
        except Exception:
            pass
    return raw.decode("utf-8", errors="ignore")

def http_get(opener, url: str):
    req = urllib.request.Request(url, method="GET")
    for k, v in BASE_HEADERS.items():
        req.add_header(k, v)
    req.add_header("Cookie", SOCS_COOKIE)

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
    h = html.lower() if html else ""
    return (
        "consent.youtube.com" in h or
        "before you continue to youtube" in h or
        ("we use cookies" in h and "youtube" in h)
    )

def parse_live(html: str) -> bool:
    return (
        "isLiveContent\":true" in html or
        "\"isLive\":true" in html
    )

def to_int_compact(s: str) -> int:
    s = (s or "").replace(",", "").strip()
    try:
        return int(s)
    except ValueError:
        pass
    m = re.match(r"^(\d+(?:\.\d+)?)([KkMm])$", s)
    if not m:
        return 0
    n = float(m.group(1))
    return int(n * (1_000 if m.group(2).upper() == "K" else 1_000_000))

def parse_viewers(html: str) -> int:
    m = re.search(r'"concurrentViewers"\s*:\s*"(\d+)"', html)
    if m:
        return int(m.group(1))
    m = re.search(r'"concurrentViewers"\s*:\s*(\d+)', html)
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
    n = float(m.group(1))
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
