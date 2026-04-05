(() => {
  const CONFIG = {
    pollMs: 450,
    queueMax: 25,
    dedupeMax: 1500,
    holdMs: 3600,
    gapMs: 260,
    endpoint: '/api/twitch/channelpoints/live'
  };

  const $ = (sel) => document.querySelector(sel);

  const card = $('#card');
  const rewardTitle = $('#rewardTitle');
  const redeemer = $('#redeemer');
  const userInputRow = $('#userInputRow');
  const userInput = $('#userInput');
  const costBadge = $('#costBadge');
  const debugText = $('#debugText');

  const queue = [];
  const seen = new Map();
  let playing = false;
  let lastPollOk = 0;
  let lastPollErr = '';

  const isDebug = new URLSearchParams(location.search).has('debug');
  if (isDebug) document.body.classList.add('debug');

  const LAST_TS_KEY = 'channel_points_overlay_last_ts:' + location.pathname;
  const BOOT_MS = Date.now();

  function loadLastTs() {
    try {
      const raw = localStorage.getItem(LAST_TS_KEY);
      const n = Number(raw || 0);
      return Number.isFinite(n) ? n : 0;
    } catch {
      return 0;
    }
  }

  function saveLastTs(ts) {
    try {
      localStorage.setItem(LAST_TS_KEY, String(ts || 0));
    } catch {}
  }

  let lastAcceptedTs = loadLastTs();
  if (!lastAcceptedTs) {
    lastAcceptedTs = BOOT_MS;
    saveLastTs(lastAcceptedTs);
  }

  function updateDebug() {
    if (!isDebug) return;
    const age = lastPollOk ? Math.max(0, Math.floor((Date.now() - lastPollOk) / 1000)) + 's' : '—';
    debugText.textContent = `q=${queue.length} • last_ok=${age} • cutoff=${lastAcceptedTs}${lastPollErr ? ' • err=' + lastPollErr : ''}`;
  }

  function eventKey(e) {
    return String(e.redemption_id || e.id || '');
  }

  function pruneSeen() {
    if (seen.size <= CONFIG.dedupeMax) return;
    const entries = [...seen.entries()].sort((a, b) => a[1] - b[1]);
    const removeCount = Math.ceil(entries.length * 0.25);
    for (let i = 0; i < removeCount; i++) seen.delete(entries[i][0]);
  }

  function shouldAcceptEvent(e) {
    const ts = Number(e?.ts_ms || 0);
    if (ts > 0 && ts <= lastAcceptedTs) return false;
    return true;
  }

  function markAcceptedEvent(e) {
    const ts = Number(e?.ts_ms || 0);
    if (ts > lastAcceptedTs) {
      lastAcceptedTs = ts;
      saveLastTs(lastAcceptedTs);
    }
  }

  function formatCost(n) {
    const v = Number(n || 0);
    if (!v) return '';
    return `${v.toLocaleString()} point${v === 1 ? '' : 's'}`;
  }

  function buildTitle(e) {
    return String(e.reward_title || e.message || 'Channel Points').trim();
  }

  function buildRedeemer(e) {
    const user = String(e.user || 'Unknown').trim();
    return `Redeemed by ${user}`;
  }

  function render(a) {
    rewardTitle.textContent = buildTitle(a);
    redeemer.textContent = buildRedeemer(a);

    const input = String(a.user_input || '').trim();
    if (input) {
      userInput.textContent = input;
      userInputRow.hidden = false;
    } else {
      userInput.textContent = '';
      userInputRow.hidden = true;
    }

    const cost = formatCost(a.cost);
    costBadge.textContent = cost || 'Twitch Channel Points';
  }

  function clearAnim() {
    card.classList.remove('enter', 'hold', 'exit');
    void card.offsetWidth;
  }

  function sleep(ms) {
    return new Promise((resolve) => setTimeout(resolve, ms));
  }

  async function playNext() {
    const next = queue.shift();
    if (!next) {
      playing = false;
      card.hidden = true;
      return;
    }

    playing = true;
    try {
      render(next);
      card.hidden = false;
      clearAnim();
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
    } catch (err) {
      console.error('[channel_points] playNext crashed', next, err);
      try {
        clearAnim();
        card.hidden = true;
      } catch {}
    } finally {
      playing = false;
    }

    if (queue.length) playNext();
  }

  async function fetchJson(url) {
    const r = await fetch(url, { cache: 'no-store' });
    if (!r.ok) throw new Error(`${r.status} ${r.statusText}`);
    return await r.json();
  }

  async function pollOnce() {
    try {
      const data = await fetchJson(CONFIG.endpoint);
      const events = Array.isArray(data?.events) ? data.events : [];

      for (const e of events) {
        if (!shouldAcceptEvent(e)) continue;
        const key = eventKey(e);
        if (!key) continue;
        if (seen.has(key)) continue;

        seen.set(key, Date.now());
        queue.push(e);
        markAcceptedEvent(e);
      }

      if (queue.length > CONFIG.queueMax) {
        queue.splice(0, queue.length - CONFIG.queueMax);
      }

      pruneSeen();
      lastPollOk = Date.now();
      lastPollErr = '';
    } catch (err) {
      lastPollErr = (err?.message || String(err)).slice(0, 80);
      console.debug('[channel_points] poll failed', lastPollErr);
    }

    updateDebug();
    if (!playing) playNext();
  }

  if (isDebug) {
    window.__channelPointsTest = (e) => {
      const fake = Object.assign({
        redemption_id: 'manual-' + Date.now(),
        reward_title: 'Test Reward',
        user: 'RadarController',
        user_input: '',
        cost: 1,
        ts_ms: Date.now()
      }, e || {});
      const key = eventKey(fake);
      if (!seen.has(key)) seen.set(key, Date.now());
      queue.push(fake);
      if (!playing) playNext();
      updateDebug();
      console.debug('[channel_points] injected test event', fake);
    };
  }

  setInterval(pollOnce, CONFIG.pollMs);
  pollOnce();
})();