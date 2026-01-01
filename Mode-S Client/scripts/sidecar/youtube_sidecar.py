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
from typing import Any, Dict, Optional, Tuple, List
from urllib.request import Request, urlopen
from urllib.error import URLError, HTTPError


def emit(obj: Dict[str, Any]) -> None:
    try:
        print(json.dumps(obj, ensure_ascii=False), flush=True)
    except (BrokenPipeError, OSError):
        raise SystemExit(0)


def now_ts() -> float:
    return time.time()


def find_config_candidates() -> List[Path]:
    # Match TikTok sidecar behavior, but also allow an explicit config path arg.
    candidates: List[Path] = []
    if len(sys.argv) > 1:
        try:
            candidates.append(Path(sys.argv[1]).expanduser())
        except Exception:
            pass

    candidates.extend([
        Path(sys.executable).resolve().parent / "config.json",
        Path(__file__).resolve().parent.parent / "config.json",
        Path("config.json"),
    ])
    # De-duplicate while preserving order
    out: List[Path] = []
    seen = set()
    for p in candidates:
        try:
            rp = p.resolve()
        except Exception:
            rp = p
        key = str(rp)
        if key not in seen:
            seen.add(key)
            out.append(p)
    return out

# ---------------- config ----------------
def load_config() -> Tuple[Dict[str, Any], Optional[Path]]:
    """Load config.json using the same discovery approach as tiktok_sidecar.py,
    plus a couple of extra common locations used by the app.

    Returns: (cfg_dict, path_used_or_None)
    """
    # 1) Explicit path argument takes priority (useful when launched by the app).
    arg_path: Optional[Path] = None
    if len(sys.argv) > 1:
        try:
            p = Path(sys.argv[1])
            arg_path = p if p.is_absolute() else (Path.cwd() / p)
        except Exception:
            arg_path = None

    candidates: List[Path] = []
    if arg_path:
        candidates.append(arg_path)

    # 2) TikTok-style candidates (copied)
    candidates += [
        Path(sys.executable).resolve().parent / "config.json",
        Path(__file__).resolve().parent.parent / "config.json",
        Path("config.json"),
    ]

    # 3) Additional common locations (to avoid cwd surprises)
    # project root (scripts/sidecar -> scripts -> <root>)
    try:
        candidates.append(Path(__file__).resolve().parents[2] / "config.json")
    except Exception:
        pass

    # next to the script itself (scripts/sidecar/config.json)
    try:
        candidates.append(Path(__file__).resolve().parent / "config.json")
    except Exception:
        pass

    # Next to the launched program (sometimes the exe sets argv[0])
    try:
        candidates.append(Path(sys.argv[0]).resolve().parent / "config.json")
    except Exception:
        pass

    # De-dupe while preserving order
    try:
        candidates = unique_candidates(candidates)
    except Exception:
        # Fallback de-dupe
        seen = set()
        deduped = []
        for p in candidates:
            sp = str(p)
            if sp not in seen:
                seen.add(sp)
                deduped.append(p)
        candidates = deduped

    for p in candidates:
        try:
            if p.exists():
                return json.loads(p.read_text(encoding="utf-8")), p
        except Exception:
            continue

    return {}, None

def emit_debug_config(cfg_path: Optional[Path], cfg: Dict[str, Any]) -> None:
    # Always emit so we can diagnose missing_handle quickly
    emit({
        "type": "youtube.debug",
        "ts": time.time(),
        "config_path": str(cfg_path.resolve()) if cfg_path else None,
        "cwd": str(Path.cwd()),
        "youtube_handle": cfg.get("youtube_handle", ""),
    })



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

    # concurrent viewers often appears as: "1,234 watching now"
    m = re.search(r'([0-9][0-9,\.]*)\s+watching\s+now', html, re.IGNORECASE)
    if m:
        n = m.group(1).replace(",", "")
        try:
            return True, int(float(n)), "watching_now_text"
        except Exception:
            return True, 0, "watching_now_parse_fail"

    # Secondary: look for JSON flag patterns
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
        cfg, cfg_path = load_config()
        emit_debug_config(cfg_path, cfg)
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
