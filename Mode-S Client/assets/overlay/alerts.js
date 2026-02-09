/*
  ATC Alerts Overlay (Twitch + TikTok)

  Served via: http://localhost:17845/overlay/alerts.html
  Polls:
    - /api/twitch/eventsub/events
    - /api/tiktok/events  (also tries /api/TikTok/events)

  Notes:
  - Client-side de-dupe (uses event.id if present, otherwise hashes payload)
  - Queues alerts and plays them one-by-one
  - Set ?debug=1 on the overlay URL to show the debug banner
*/

(() => {
    const CONFIG = {
        pollMs: 450,
        queueMax: 25,
        dedupeMax: 1500,
        // enter (320) + hold (3600) + exit (420) + gap (260)
        holdMs: 3600,
        gapMs: 260,
        endpoints: [
            { url: '/api/twitch/eventsub/events' },
            { url: '/api/tiktok/events' },
            { url: '/api/TikTok/events', optional: true },
            // Optional until YouTube events are fully stable.
            { url: '/api/youtube/events', optional: true },
        ],
    };

    const $ = (sel) => document.querySelector(sel);

    // DOM
    const stage = $('#stage');
    const card = $('#card');
    const scan = $('#scan');
    const elIcon = $('#platformIcon');
    const elCallsign = $('#callsign');
    const elUser = $('#user');
    const elType = $('#type');
    const elMsg = $('#msg');
    const elTime = $('#time');
    const debugBanner = $('#debug');
    const debugText = $('#debugText');

    const queue = [];
    const seen = new Map();
    let playing = false;
    let lastPollOk = 0;
    let lastPollErr = '';

    const isDebug = new URLSearchParams(location.search).has('debug');
    if (isDebug) document.body.classList.add('debug');

    function nowMs() { return Date.now(); }

    // ---- NEW: prevent replaying buffered/old events on first load ----
    // We persist a per-platform "last seen ts_ms" so page refreshes don't replay historical API buffers.
    const LAST_TS_KEY = 'atc_alerts_last_ts_v1';
    const BOOT_MS = nowMs(); // local boot time, used to initialize cutoffs on first run

    function loadLastTs() {
        try {
            const raw = localStorage.getItem(LAST_TS_KEY);
            if (!raw) return {};
            const obj = JSON.parse(raw);
            return (obj && typeof obj === 'object') ? obj : {};
        } catch {
            return {};
        }
    }

    function saveLastTs(obj) {
        try {
            localStorage.setItem(LAST_TS_KEY, JSON.stringify(obj));
        } catch {
            // ignore (OBS/webview privacy mode, storage disabled, etc.)
        }
    }

    const lastTsByPlatform = loadLastTs();

    // If this is the first time the overlay is ever opened (no stored state),
    // initialize cutoffs to "now" so we don't replay whatever the server currently buffers.
    if (!('twitch' in lastTsByPlatform) && !('tiktok' in lastTsByPlatform) && !('youtube' in lastTsByPlatform)) {
        lastTsByPlatform.twitch = BOOT_MS;
        lastTsByPlatform.tiktok = BOOT_MS;
        lastTsByPlatform.youtube = BOOT_MS;
        saveLastTs(lastTsByPlatform);
    }

    function shouldAcceptEvent(e) {
        const p = normalizePlatform(e?.platform);
        const ts = Number(e?.ts_ms || 0);

        // If the event has a timestamp, enforce monotonic "only newer than last processed".
        if (ts > 0) {
            const cutoff = Number(lastTsByPlatform[p] || 0);
            if (cutoff && ts <= cutoff) return false;
            return true;
        }

        // If ts_ms is missing/0, fall back to ID/hash de-dupe only (can't reliably gate by time).
        return true;
    }

    function markAcceptedEvent(e) {
        const p = normalizePlatform(e?.platform);
        const ts = Number(e?.ts_ms || 0);
        if (ts > 0) {
            const prev = Number(lastTsByPlatform[p] || 0);
            if (ts > prev) {
                lastTsByPlatform[p] = ts;
                saveLastTs(lastTsByPlatform);
            }
        }
    }
    // ---- /NEW ----

    function hash32(s) {
        let h = 0x811c9dc5;
        for (let i = 0; i < s.length; i++) {
            h ^= s.charCodeAt(i);
            h = (h * 0x01000193) >>> 0;
        }
        return h >>> 0;
    }

    function normalizePlatform(p) {
        return String(p || '').toLowerCase();
    }

    function eventKey(e) {
        if (!e) return '';
        if (e.id) return `id:${e.id}`;
        const p = normalizePlatform(e.platform);
        const t = String(e.type || '').toLowerCase();
        const u = String(e.user || '');
        const m = String(e.message || '');
        const ts = String(e.ts_ms || '');
        return `k:${p}|${t}|${u}|${m}|${ts}`;
    }

    function pruneSeen() {
        if (seen.size <= CONFIG.dedupeMax) return;
        const entries = [...seen.entries()].sort((a, b) => a[1] - b[1]);
        const removeCount = Math.ceil(entries.length * 0.25);
        for (let i = 0; i < removeCount; i++) seen.delete(entries[i][0]);
    }

    function shortTime(ts) {
        try {
            const d = new Date(Number(ts));
            const hh = String(d.getUTCHours()).padStart(2, '0');
            const mm = String(d.getUTCMinutes()).padStart(2, '0');
            const ss = String(d.getUTCSeconds()).padStart(2, '0');
            return `${hh}:${mm}:${ss}Z`;
        } catch {
            return '';
        }
    }

    function callsignFromUser(user) {
        const raw = String(user || '').trim();
        if (!raw) return 'UNKNOWN';
        const ascii = raw.normalize('NFKD').replace(/[\u0300-\u036f]/g, '');
        const clean = ascii.replace(/[^a-zA-Z0-9]/g, '');
        const up = (clean || ascii || raw).toUpperCase();
        if (/^[A-Z]+$/.test(up) && up.length <= 6) {
            const num = (Math.abs(hash32(raw)) % 9000) + 1000;
            return `${up}${num}`;
        }
        return up.slice(0, 12);
    }

    function setPlatformClass(platform) {
        card.classList.remove('twitch', 'tiktok', 'youtube');
        if (platform === 'twitch') card.classList.add('twitch');
        if (platform === 'tiktok') card.classList.add('tiktok');
        if (platform === 'youtube') card.classList.add('youtube');
    }

    function setPlatformIcon(platform) {
        const svgTwitch = `
      <svg viewBox="0 0 24 24" aria-hidden="true">
        <path fill="currentColor" d="M3 2h18v12l-5 5h-5l-3 3H6v-3H3V2zm16 2H5v14h3v3l3-3h5l3-3V4z"/>
        <path fill="currentColor" d="M15 7h2v6h-2V7zm-5 0h2v6h-2V7z"/>
      </svg>`;

        const svgTikTok = `
      <svg viewBox="0 0 24 24" aria-hidden="true">
        <path fill="currentColor" d="M14 3c1 2.6 2.9 4.5 5.6 5.1v3.1c-2.1-.1-4-1-5.6-2.3v6.5c0 3.2-2.6 5.8-5.8 5.8S2.4 18.6 2.4 15.4s2.6-5.8 5.8-5.8c.4 0 .8 0 1.2.1v3.3c-.4-.1-.8-.2-1.2-.2-1.4 0-2.6 1.2-2.6 2.6s1.2 2.6 2.6 2.6 2.6-1.2 2.6-2.6V3H14z"/>
      </svg>`;

        // Simple YouTube play badge (kept monochrome to match the overlay style).
        const svgYouTube = `
      <svg viewBox="0 0 24 24" aria-hidden="true">
        <path fill="currentColor" d="M21.6 7.2a3.1 3.1 0 0 0-2.2-2.2C17.5 4.5 12 4.5 12 4.5s-5.5 0-7.4.5A3.1 3.1 0 0 0 2.4 7.2 32.5 32.5 0 0 0 2 12a32.5 32.5 0 0 0 .4 4.8 3.1 3.1 0 0 0 2.2 2.2c1.9.5 7.4.5 7.4.5s5.5 0 7.4-.5a3.1 3.1 0 0 0 2.2-2.2A32.5 32.5 0 0 0 22 12a32.5 32.5 0 0 0-.4-4.8z"/>
        <path fill="#0b0f14" d="M10 15.5V8.5L16 12l-6 3.5z"/>
      </svg>`;

        if (platform === 'youtube') elIcon.innerHTML = svgYouTube;
        else if (platform === 'tiktok') elIcon.innerHTML = svgTikTok;
        else elIcon.innerHTML = svgTwitch;
    }

    function mapEvent(e) {
        const platform = normalizePlatform(e.platform);
        const type = String(e.type || '').toLowerCase();

        let kind = 'EVENT';
        let message = '';

        if (platform === 'twitch' && type === 'channel.follow') {
            kind = 'HOLDING';
            message = 'Enter the hold, delay undetermined.';
        } else if (platform === 'twitch' && type === 'channel.subscribe') {
            kind = 'HOLDING CANCELLED';
            message = 'Your hold is cancelled, expect vectors!';
        } else if (platform === 'twitch' && type === 'channel.subscription.message') {
            kind = 'RESUB';
            const months = Number(e.cumulative_months || e.months || 0);
            const txt = String(e.resub_message || '').trim();
            if (months > 0) {
                message = `resubbed for ${months} months in a row!`;
            } else {
                message = 'resubbed!';
            }
            if (txt) message += ` ${txt}`;
        } else if (platform === 'twitch' && (type === 'channel.subscription.gift' || type === 'channel.subscription.gifted' || type.includes('gift'))) {
            kind = 'HOLD EMPTIED';
            message = 'No delay, expect vectors!';
        }
        else if (platform === 'twitch' && type === 'channel.cheer') {
            const bits = Number(e.bits || e.total_bits || 0);
            kind = 'DELAY';
            message = bits > 0
                ? `${bits} minutes of delay added.`
                : 'The delay has increased.';
        } else if (platform === 'tiktok' && type === 'follow') {
            kind = 'HOLDING';
            message = 'Enter the hold, delay undetermined.';
        } else if (platform === 'tiktok' && type === 'gift') {
            kind = 'DESCEND';
            message = 'Expected delay has been reduced.';
        } else if (platform === 'youtube' && type === 'subscribe') {
            kind = 'HOLDING';
            message = 'Enter the hold, delay undetermined.';
        } else if (platform === 'youtube' && type === 'membership') {
            kind = 'HOLDING CANCELLED';
            message = 'Your hold is cancelled, expect vectors!';
        } else {
            kind = (e.type || 'EVENT').toString();
            message = (e.message || '').toString();
        }

        // Keep generic “followed/subscribed” out of the cinematic line; it reads better as the ATC phrase.
        const rawMsg = String(e.message || '').trim();
        if (rawMsg && !(platform === 'twitch' && type === 'channel.subscription.message')) {
            const low = rawMsg.toLowerCase();
            if (low !== 'followed' && low !== 'subscribed' && message !== rawMsg) {
                message = rawMsg;
            }
        }

        return {
            platform,
            kind,
            callsign: callsignFromUser(e.user),
            user: String(e.user || ''),
            message: message || '',
            ts_ms: Number(e.ts_ms || 0),
        };
    }

    function enqueue(rawEvent) {
        const a = mapEvent(rawEvent);
        if (!a.user && !a.callsign) return;
        queue.push(a);
        while (queue.length > CONFIG.queueMax) queue.shift();
    }

    async function fetchJson(url) {
        const r = await fetch(url, { cache: 'no-store' });
        if (!r.ok) throw new Error(`${r.status} ${r.statusText}`);
        return await r.json();
    }

    function updateDebug() {
        if (!isDebug) return;
        const age = lastPollOk ? `${Math.max(0, Math.floor((nowMs() - lastPollOk) / 1000))}s` : '—';
        const q = queue.length;

        // show persisted cutoffs too (helpful to validate the “no replay on load” fix)
        const cutT = Number(lastTsByPlatform.twitch || 0);
        const cutK = Number(lastTsByPlatform.tiktok || 0);
        const cutY = Number(lastTsByPlatform.youtube || 0);
        const cuts = `cutoff(tw:${shortTime(cutT)} tk:${shortTime(cutK)} yt:${shortTime(cutY)})`;

        debugText.textContent = `q=${q} • last_ok=${age} • ${cuts}${lastPollErr ? ` • err=${lastPollErr}` : ''}`;
    }

    async function pollOnce() {
        let anyOk = false;
        let errMsg = '';

        for (const ep of CONFIG.endpoints) {
            try {
                const data = await fetchJson(ep.url);
                const events = Array.isArray(data?.events)
                    ? data.events
                    : (Array.isArray(data?.events?.events) ? data.events.events : []);

                for (const e of events) {
                    if (!shouldAcceptEvent(e)) continue;

                    const key = eventKey(e);
                    if (!key) continue;
                    if (seen.has(key)) continue;

                    seen.set(key, nowMs());
                    enqueue(e);
                    markAcceptedEvent(e);
                }

                pruneSeen();
                anyOk = true;
            } catch (err) {
                if (!ep.optional) {
                    errMsg = (err?.message || String(err)).slice(0, 80);
                    console.debug('[alerts] poll failed', ep.url, errMsg);
                }
            }
        }

        if (anyOk) {
            lastPollOk = nowMs();
            lastPollErr = '';
        } else if (errMsg) {
            lastPollErr = errMsg;
        }

        updateDebug();
        if (!playing) playNext();
    }

    function renderAlert(a) {
        setPlatformClass(a.platform);
        setPlatformIcon(a.platform);

        elCallsign.textContent = a.callsign || 'UNKNOWN';
        elUser.textContent = a.user || 'UNKNOWN';
        elType.textContent = a.kind || 'EVENT';
        elMsg.textContent = a.message || '';
        elTime.textContent = shortTime(a.ts_ms) || '';

        stage.setAttribute('aria-label', `${a.platform} ${a.kind} ${a.user}`);
    }

    function clearAnimClasses() {
        card.classList.remove('enter', 'hold', 'exit');
        // force reflow so re-adding classes restarts keyframes reliably
        void card.offsetWidth;
    }

    async function playNext() {
        const next = queue.shift();
        if (!next) {
            playing = false;
            card.hidden = true;
            return;
        }

        playing = true;
        renderAlert(next);

        card.hidden = false;
        clearAnimClasses();
        card.classList.add('enter');

        await sleep(320);
        card.classList.remove('enter');
        card.classList.add('hold');

        await sleep(CONFIG.holdMs);
        card.classList.remove('hold');
        card.classList.add('exit');

        await sleep(420);
        card.classList.remove('exit');
        card.hidden = true;

        await sleep(CONFIG.gapMs);
        playNext();
    }

    function sleep(ms) { return new Promise((r) => setTimeout(r, ms)); }

    // Kick off
    setInterval(pollOnce, CONFIG.pollMs);
    pollOnce();
})();