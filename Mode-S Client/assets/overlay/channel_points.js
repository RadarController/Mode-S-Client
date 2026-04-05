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
  const logoShell = card ? card.querySelector('.cp-logoShell') : null;

  const queue = [];
  const seen = new Map();
  let playing = false;
  let lastPollOk = 0;
  let lastPollErr = '';
  let currentAudio = null;

  const isDebug = new URLSearchParams(location.search).has('debug');
  if (isDebug) document.body.classList.add('debug');

  const currentSurface = location.pathname.toLowerCase().includes('portrait')
    ? 'portrait'
    : 'landscape';

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

  function shouldRenderEvent(e) {
    const action = (e && typeof e.app_action === 'object' && !Array.isArray(e.app_action))
      ? e.app_action
      : {};

    if (action.overlay_enabled === false) return false;

    const surface = String(action.overlay_surface || 'both').toLowerCase();
    if (!surface || surface === 'both') return true;
    return surface === currentSurface;
  }

  function getAction(e) {
    return (e && typeof e.app_action === 'object' && !Array.isArray(e.app_action))
      ? e.app_action
      : {};
  }

  function renderTemplate(template, e) {
    const input = String(e?.user_input || '').trim();
    const costText = formatCost(e?.cost);

    const values = {
      user: String(e?.user || 'Unknown').trim(),
      reward: String(e?.reward_title || e?.message || 'Channel Points').trim(),
      input,
      cost: String(e?.cost ?? '').trim(),
      cost_text: costText,
      prompt: String(e?.prompt || '').trim()
    };

    return String(template || '')
      .replace(/\{(user|reward|input|cost|cost_text|prompt)\}/gi, (match, key) => {
        const value = values[String(key || '').toLowerCase()];
        return value != null ? String(value) : '';
      })
      .replace(/\s{2,}/g, ' ')
      .trim();
  }

  function buildTitle(e) {
    const action = getAction(e);
    const template = String(action.alert_text || '').trim();
    if (template) {
      const rendered = renderTemplate(template, e);
      if (rendered) return rendered;
    }
    return String(e.reward_title || e.message || 'Channel Points').trim();
  }

  function buildRedeemer(e) {
    const user = String(e.user || 'Unknown').trim();
    return `Redeemed by ${user}`;
  }

  function resolveAssetUrl(rawValue, defaultBase) {
    const raw = String(rawValue || '').trim();
    if (!raw) return '';

    if (/^https?:\/\//i.test(raw)) return raw;
    if (raw.startsWith('/')) return raw;

    return `${defaultBase}/${raw}`;
  }

  function resolveSoundUrl(soundFile) {
    return resolveAssetUrl(soundFile, '/assets/sounds');
  }

  function resolveImageUrl(imageFile) {
    return resolveAssetUrl(imageFile, '/assets/channel-points');
  }

  function resolveVideoUrl(videoFile) {
    return resolveAssetUrl(videoFile, '/assets/channel-points');
  }

  function ensureMediaLayer() {
    if (!card) return null;

    let wrap = document.getElementById('cpMediaWrap');
    let image = document.getElementById('cpMediaImage');
    let video = document.getElementById('cpMediaVideo');
    let shade = document.getElementById('cpMediaShade');

    if (wrap && image && video && shade) {
      return { wrap, image, video, shade };
    }

    wrap = document.createElement('div');
    wrap.id = 'cpMediaWrap';
    Object.assign(wrap.style, {
      position: 'absolute',
      inset: '0',
      borderRadius: 'inherit',
      overflow: 'hidden',
      display: 'none',
      zIndex: '1',
      pointerEvents: 'none'
    });

    image = document.createElement('img');
    image.id = 'cpMediaImage';
    image.alt = '';
    Object.assign(image.style, {
      position: 'absolute',
      inset: '0',
      width: '100%',
      height: '100%',
      objectFit: 'cover',
      objectPosition: 'center',
      display: 'none'
    });

    video = document.createElement('video');
    video.id = 'cpMediaVideo';
    video.muted = true;
    video.loop = true;
    video.autoplay = true;
    video.playsInline = true;
    video.setAttribute('playsinline', '');
    video.setAttribute('muted', '');
    video.setAttribute('autoplay', '');
    Object.assign(video.style, {
      position: 'absolute',
      inset: '0',
      width: '100%',
      height: '100%',
      objectFit: 'cover',
      objectPosition: 'center',
      display: 'none'
    });

    shade = document.createElement('div');
    shade.id = 'cpMediaShade';
    Object.assign(shade.style, {
      position: 'absolute',
      inset: '0',
      background: 'linear-gradient(180deg, rgba(6,8,12,0.16) 0%, rgba(6,8,12,0.42) 55%, rgba(6,8,12,0.70) 100%)',
      display: 'none'
    });

    wrap.appendChild(image);
    wrap.appendChild(video);
    wrap.appendChild(shade);
    card.appendChild(wrap);

    return { wrap, image, video, shade };
  }

  const media = ensureMediaLayer();

  function stopCurrentAudio() {
    if (!currentAudio) return;
    try {
      currentAudio.pause();
      currentAudio.currentTime = 0;
    } catch {}
    currentAudio = null;
  }

  function stopVideoPlayback() {
    if (!media) return;
    try {
      media.video.pause();
    } catch {}
    try {
      media.video.removeAttribute('src');
      media.video.load();
    } catch {}
    media.video.style.display = 'none';
  }

  function clearMedia() {
    if (!media) return;

    stopVideoPlayback();

    media.image.style.display = 'none';
    media.image.removeAttribute('src');

    media.shade.style.display = 'none';
    media.wrap.style.display = 'none';

    if (logoShell) logoShell.style.display = '';
  }

  async function applyMediaForEvent(e) {
    if (!media) return;

    const action = getAction(e);
    const videoUrl = action.video_enabled ? resolveVideoUrl(action.video_file) : '';
    const imageUrl = action.image_enabled ? resolveImageUrl(action.image_file) : '';

    clearMedia();

    if (videoUrl) {
      try {
        if (logoShell) logoShell.style.display = 'none';
        media.wrap.style.display = 'block';
        media.shade.style.display = 'block';
        media.image.style.display = 'none';
        media.image.removeAttribute('src');

        media.video.src = videoUrl;
        media.video.style.display = 'block';
        media.video.load();
        await media.video.play();
        return;
      } catch (err) {
        console.debug('[channel_points] video playback failed', videoUrl, err);
        stopVideoPlayback();
      }
    }

    if (imageUrl) {
      if (logoShell) logoShell.style.display = 'none';
      media.wrap.style.display = 'block';
      media.shade.style.display = 'block';
      media.image.src = imageUrl;
      media.image.style.display = 'block';
      return;
    }

    clearMedia();
  }

  async function playSoundForEvent(e) {
    const action = getAction(e);
    if (!action.sound_enabled) return;

    const url = resolveSoundUrl(action.sound_file);
    if (!url) return;

    try {
      stopCurrentAudio();
      currentAudio = new Audio(url);
      currentAudio.preload = 'auto';
      currentAudio.currentTime = 0;
      await currentAudio.play();
    } catch (err) {
      console.debug('[channel_points] sound playback failed', url, err);
      currentAudio = null;
    }
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
      clearMedia();
      card.hidden = true;
      return;
    }

    playing = true;
    try {
      render(next);
      await applyMediaForEvent(next);
      await playSoundForEvent(next);

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
      clearMedia();
      card.hidden = true;

      await sleep(CONFIG.gapMs);
    } catch (err) {
      console.error('[channel_points] playNext crashed', next, err);
      try {
        clearAnim();
        clearMedia();
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
        if (!shouldRenderEvent(e)) continue;
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
        prompt: '',
        ts_ms: Date.now(),
        app_action: {
          overlay_enabled: true,
          overlay_surface: 'both',
          alert_text: '{user} redeemed {reward}',
          sound_enabled: false,
          sound_file: '',
          image_enabled: false,
          image_file: '',
          video_enabled: false,
          video_file: ''
        }
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