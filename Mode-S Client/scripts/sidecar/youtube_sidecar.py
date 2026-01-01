#!/usr/bin/env python
# YouTube sidecar for Mode-S Client
#
# Emits:
#   {"type":"youtube.stats","ts":<unix>,"live":bool,"viewers":int,"followers":int,"note":str}
#
# Data sources (web-scrape; no API key):
#   - https://www.youtube.com/@<handle>/live  (live + concurrent viewers if live)
#   - https://www.youtube.com/@<handle>       (subscriber count)
#
# Notes:
#   - UK/EU often returns a consent interstitial; we send a SOCS cookie to bypass.
#   - We emit stats every poll (even if unchanged) so downstream always refreshes fields.

import json
import sys
import time
import re
from pathlib import Path
from typing import Any, Dict, Optional, Tuple
from urllib.request import Request, urlopen
from urllib.error import URLError, HTTPError


def emit(obj: Dict[str, Any]) -> None:
    try:
        print(json.dumps(obj, ensure_ascii=False), flush=True)
    except (BrokenPipeError, OSError):
        raise SystemExit(0)


def now_ts() -> float:
    return time.time()


def _to_int_from_compact(s: str) -> int:
    """Parse numbers like '1,234' or compact '1.2K'/'3.4M'."""
    if not s:
        return 0
    t = s.strip().replace(",", "").replace(" ", "")
    try:
        return int(t)
    except ValueError:
        pass

    m = re.match(r"^(\d+(?:\.\d+)?)([KkMm])$", t)
    if not m:
        return 0
    n = float(m.group(1))
    suf = m.group(2).upper()
    if suf == "K":
        n *= 1_000
    elif suf == "M":
        n *= 1_000_000
    return int(n)



def load_config() -> Dict[str, Any]:
    candidates = [
        Path(sys.executable).resolve().parent / "config.json",
        Path(__file__).resolve().parent.parent / "config.json",
        Path("config.json"),
    ]
    for p in candidates:
        if p.exists():
            try:
                return json.loads(p.read_text(encoding="utf-8"))
            except Exception:
                return {}
    return {}

    print(json.dumps({
    "type": "youtube.debug",
    "config_path": config_path,
    "youtube_handle": handle
}), flush=True)

def sanitize_handle(s: str) -> str:
    s = (s or "").strip()
    if s.startswith("@"):
        s = s[1:]
    return s.strip().lower()


# Consent cookie (helps bypass consent.youtube.com interstitial in many regions).
# If this stops working in future, we can move it to config.json.
SOCS_COOKIE = "SOCS=CAESEwgDEgk0ODE3Nzk3MjQaAmVuIAEaBgiA_LyaBg"

UA = (
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
    "AppleWebKit/537.36 (KHTML, like Gecko) "
    "Chrome/120.0.0.0 Safari/537.36"
)


def http_get(url: str) -> Tuple[int, str]:
    req = Request(
        url,
        headers={
            "User-Agent": UA,
            "Accept": "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8",
            "Accept-Language": "en-GB,en;q=0.9",
            "Referer": "https://www.youtube.com/",
            "Cookie": SOCS_COOKIE,
        },
        method="GET",
    )
    try:
        with urlopen(req, timeout=15) as resp:
            status = getattr(resp, "status", 200)
            data = resp.read()
            # YouTube pages are UTF-8
            return int(status), data.decode("utf-8", errors="ignore")
    except HTTPError as e:
        try:
            body = e.read().decode("utf-8", errors="ignore")
        except Exception:
            body = ""
        return int(e.code), body
    except URLError:
        return 0, ""
    except Exception:
        return 0, ""


def looks_like_consent_page(html: str) -> bool:
    h = html.lower()
    return (
        "consent.youtube.com" in h
        or "before you continue to youtube" in h
        or ("we use cookies" in h and "youtube" in h)
    )


def parse_abbrev_number(s: str) -> Optional[int]:
    s = s.strip()
    if not s:
        return None
    # Remove commas/spaces
    s = s.replace(",", "").replace(" ", "")
    m = re.match(r"^([0-9]+(?:\.[0-9]+)?)([KMB])?$", s, re.IGNORECASE)
    if not m:
        # maybe plain int
        if s.isdigit():
            return int(s)
        return None
    num = float(m.group(1))
    suf = (m.group(2) or "").upper()
    mult = 1
    if suf == "K":
        mult = 1_000
    elif suf == "M":
        mult = 1_000_000
    elif suf == "B":
        mult = 1_000_000_000
    return int(num * mult)


def extract_followers(html: str) -> Optional[int]:
    # Prefer structured JSON snippet: "subscriberCountText":{"simpleText":"5.6M subscribers"}
    m = re.search(r'"subscriberCountText"\s*:\s*\{\s*"simpleText"\s*:\s*"([^"]+)"', html)
    if m:
        txt = m.group(1)
        # e.g. "5.6M subscribers"
        num = txt.split(" ")[0].strip()
        v = parse_abbrev_number(num)
        if v is not None:
            return v

    # Fallback: plain text occurrences
    m = re.search(r'([0-9][0-9\.,]*\s*[KMB]?)\s+subscribers', html, re.IGNORECASE)
    if m:
        num = m.group(1).strip()
        # normalize "1,234" -> "1234"
        num = num.replace(",", "")
        v = parse_abbrev_number(num)
        if v is not None:
            return v

    return None


def extract_live_and_viewers(html: str) -> Tuple[bool, int, str]:
    # If consent page, treat as offline (caller may log note)
    if looks_like_consent_page(html):
        return False, 0, "consent_interstitial"

    # YouTube embeds markers that usually indicate live content.
    # We keep this tolerant because HTML changes.
    if "LIVE_STREAM_OFFLINE" in html:
        return False, 0, "live_offline_marker"

    # 1) Best signal: embedded JSON field (most reliable when present)
    # Examples:
    #   "concurrentViewers":"1234"
    #   "concurrentViewers":1234
    m = re.search(r'"concurrentViewers"\s*:\s*"(?P<n>\d+)"', html)
    if m:
        return True, int(m.group("n")), "concurrentViewers_json"

    m = re.search(r'"concurrentViewers"\s*:\s*(?P<n>\d+)', html)
    if m:
        return True, int(m.group("n")), "concurrentViewers_json_num"

    # 2) UI text (varies by layout/locale)
    # Common: "1,234 watching now"
    m = re.search(r'([0-9][0-9,\.]*)\s+watching\s+now', html, re.IGNORECASE)
    if m:
        n = _to_int_from_compact(m.group(1))
        return True, n, "watching_now_text" if n else "watching_now_parse_fail"

    # Alternate: "watching now ... 1,234"
    m = re.search(r'watching\s+now[^0-9]*([0-9][0-9,\.]*)', html, re.IGNORECASE)
    if m:
        n = _to_int_from_compact(m.group(1))
        return True, n, "watching_now_text_alt" if n else "watching_now_parse_fail_alt"

    # 3) Secondary: look for JSON flag patterns that indicate LIVE
    if '"isLiveContent":true' in html or '"isLive":true' in html:
        return True, 0, "isLive_marker"

    return False, 0, "no_live_markers"

def emit_stats(live: bool, viewers: int, followers: int, note: str = "") -> None:
    emit(
        {
            "type": "youtube.stats",
            "ts": now_ts(),
            "live": bool(live),
            "viewers": int(viewers),
            "followers": int(followers),
            "note": note,
        }
    )


def main() -> int:
    emit({"type": "youtube.boot", "ts": now_ts()})

    last_followers = 0
    last_live = False
    last_viewers = 0

    while True:
        cfg = load_config()
        handle = sanitize_handle(cfg.get("youtube_handle", ""))

        if not handle:
            # still emit so downstream rewrites values
            emit_stats(False, 0, 0, note="missing_handle")
            time.sleep(5)
            continue

        # Followers/subscribers (channel page)
        followers = last_followers
        st, html = http_get(f"https://www.youtube.com/@{handle}")
        if st == 200 and html:
            v = extract_followers(html)
            if v is not None:
                followers = v
                last_followers = v
        # Live + viewers
        live = False
        viewers = 0
        st, html = http_get(f"https://www.youtube.com/@{handle}/live")
        note = "fetch_fail"
        if st == 200 and html:
            live, viewers, note = extract_live_and_viewers(html)
            if live:
                last_live = True
                last_viewers = viewers
            else:
                last_live = False
                last_viewers = 0
        else:
            # don't keep stale live/viewers
            live = False
            viewers = 0

        emit_stats(live, viewers, followers, note=note)

        time.sleep(15)

if __name__ == "__main__":
    raise SystemExit(main())
