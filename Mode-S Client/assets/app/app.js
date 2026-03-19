
const $ = (sel, root=document) => root.querySelector(sel);
const $$ = (sel, root=document) => Array.from(root.querySelectorAll(sel));

function fmtNum(n){
  if (n === null || n === undefined) return "—";
  const x = Number(n);
  if (!Number.isFinite(x)) return "—";
  return x.toLocaleString();
}

function setText(id, v){ const el = document.getElementById(id); if (el) el.textContent = v; }

function notifyNativeAppReady(){
  try{
    if (window.chrome?.webview?.postMessage) {
      window.chrome.webview.postMessage({ type: "app-ready" });
    }
  }catch(_){
    // ignore
  }
}

function trim(s){ return String(s ?? "").trim(); }

function sanitizeTikTok(input){
  return trim(input).replaceAll("@", "");
}

function sanitizeTwitch(input){
  let s = trim(input);
  if (s.startsWith("#")) s = s.slice(1);
  return s.toLowerCase();
}

function sanitizeYouTube(input){
  let s = trim(input).replaceAll("@", "");
  s = s.replace(/\s+/g, "");
  return s;
}

function sanitizePlatformValue(platform, value){
  switch(platform){
    case "tiktok": return sanitizeTikTok(value);
    case "twitch": return sanitizeTwitch(value);
    case "youtube": return sanitizeYouTube(value);
    default: return trim(value);
  }
}

function inputIdForPlatform(platform){
  switch(platform){
    case "tiktok": return "tiktokUser";
    case "twitch": return "twitchUser";
    case "youtube": return "youtubeUser";
    default: return "";
  }
}

function getPlatformInput(platform){
  const id = inputIdForPlatform(platform);
  return id ? document.getElementById(id) : null;
}

function getPlatformButton(platform, action){
  const card = document.querySelector(`.platform[data-platform="${platform}"]`);
  return card?.querySelector(`button[data-action="${action}"]`) || null;
}

function hasValidPlatformInput(platform){
  const input = getPlatformInput(platform);
  return !!sanitizePlatformValue(platform, input?.value || "");
}

function applySanitizedInputValue(platform){
  const input = getPlatformInput(platform);
  if (!input) return "";
  const cleaned = sanitizePlatformValue(platform, input.value);
  input.value = cleaned;
  return cleaned;
}

function updatePlatformInputUi(platform){
  const valid = hasValidPlatformInput(platform);
  const setBtn = getPlatformButton(platform, "set");
  const startBtn = getPlatformButton(platform, "start");

  if (setBtn) setBtn.disabled = !valid;
  if (startBtn) startBtn.disabled = !valid;

  const input = getPlatformInput(platform);
  if (input) input.classList.toggle("input--invalid", !valid && trim(input.value).length > 0);
}

function updateAllPlatformInputUi(){
  ["tiktok", "twitch", "youtube"].forEach(updatePlatformInputUi);
  updateHomeSummary();
}

function setActionBusy(button, busy, busyText){
  if (!button) return;
  if (!button.dataset.label) button.dataset.label = button.textContent;
  button.disabled = !!busy;
  button.textContent = busy ? (busyText || "Working...") : button.dataset.label;
}

async function apiGet(url){
  const r = await fetch(url, {cache:"no-store"});
  if (!r.ok) throw new Error(`${r.status} ${r.statusText}`);
  return await r.json();
}

async function apiPost(url, body){
  const r = await fetch(url, {
    method:"POST",
    headers: {"Content-Type":"application/json"},
    body: body ? JSON.stringify(body) : ""
  });

  const t = await r.text().catch(() => "");

  if (!r.ok) {
    throw new Error(`${r.status} ${r.statusText}${t ? `: ${t}` : ""}`);
  }

  if (!t) return { ok: true };

  try{
    return JSON.parse(t);
  }catch{
    return { ok: true, text: t };
  }
}

const HOME_PLATFORMS = ["tiktok", "twitch", "youtube"];
let homeSettingsCache = null;
let homeMetricsCache = null;
let homeAuthCache = {
  tiktok: null,
  twitch: null,
  youtube: null
};
let homePlatformStatusCache = {
  tiktok: { requested_state: "stopped", ts_ms: 0 },
  twitch: { requested_state: "stopped", ts_ms: 0 },
  youtube: { requested_state: "stopped", ts_ms: 0 }
};
let homeActionState = {};

function isHomePage(){
  return !!document.getElementById("homePage");
}

async function loadSettings(){
  try{
    const s = await apiGet("/api/settings");
    if (!s || s.ok !== true) throw new Error("ok=false");

    homeSettingsCache = s;

    const t = $("#tiktokUser"); if (t) t.value = sanitizeTikTok(s.tiktok_unique_id ?? "");
    const tw = $("#twitchUser"); if (tw) tw.value = sanitizeTwitch(s.twitch_login ?? "");
    const y = $("#youtubeUser"); if (y) y.value = sanitizeYouTube(s.youtube_handle ?? "");

    updateAllPlatformInputUi();
    if (isHomePage()) renderHomePlatforms();
    logLine("settings", `loaded (${s.config_path || "unknown path"})`);
    return s;
  }catch(e){
    logLine("settings", `load failed (${e.message})`);
    throw e;
  }
}

async function saveSettingsFromInputs(){
  const payload = {
    tiktok_unique_id: applySanitizedInputValue("tiktok"),
    twitch_login: applySanitizedInputValue("twitch"),
    youtube_handle: applySanitizedInputValue("youtube")
  };

  updateAllPlatformInputUi();

  // Support both endpoints (older builds used /api/settingssave)
  try{
    await apiPost("/api/settings/save", payload);
    logLine("settings", "saved");
    await loadSettings();
    return true;
  }catch(e1){
    try{
      await apiPost("/api/settingssave", payload);
      logLine("settings", "saved");
      await loadSettings();
      return true;
    }catch(e2){
      logLine("settings", `save failed (${e2.message})`);
      return false;
    }
  }
}

async function loadSettingsGlobalOnly() {
    const s = await apiGet("/api/settings");
    if (!s || s.ok !== true) throw new Error("ok=false");

    homeSettingsCache = s;

    const t = $("#tiktokUser"); if (t) t.value = sanitizeTikTok(s.tiktok_unique_id ?? "");
    const tw = $("#twitchUser"); if (tw) tw.value = sanitizeTwitch(s.twitch_login ?? "");
    const y = $("#youtubeUser"); if (y) y.value = sanitizeYouTube(s.youtube_handle ?? "");

    updateAllPlatformInputUi();
    if (isHomePage()) renderHomePlatforms();
    return s;
}

function wireTikTokCookiesPage() {
    const root = document.getElementById("tiktokCookiesPage");
    if (!root) return;

    const elSession = document.getElementById("ttSession");
    const elSessionSS = document.getElementById("ttSessionSS");
    const elTarget = document.getElementById("ttTarget");
    const elSave = document.getElementById("btnSaveTikTokCookies");
    const elStatus = document.getElementById("tiktokCookieStatus");

    const setStatus = (t) => { if (elStatus) elStatus.textContent = t || ""; };

    async function load() {
        try {
            const j = await apiGet("/api/settings/tiktok-cookies");
            if (elSession) elSession.value = j.tiktok_sessionid || "";
            if (elSessionSS) elSessionSS.value = j.tiktok_sessionid_ss || "";
            if (elTarget) elTarget.value = j.tiktok_tt_target_idc || "";
            setStatus("");
        } catch (e) {
            console.warn("Failed to load TikTok cookies", e);
            setStatus("Could not load TikTok cookies.");
        }
    }

    async function save() {
        setStatus("Saving…");
        try {
            await apiPost("/api/settings/tiktok-cookies", {
                tiktok_sessionid: elSession ? elSession.value.trim() : "",
                tiktok_sessionid_ss: elSessionSS ? elSessionSS.value.trim() : "",
                tiktok_tt_target_idc: elTarget ? elTarget.value.trim() : ""
            });
            setStatus("Saved.");
            await load();
        } catch (e) {
            console.warn("Failed to save TikTok cookies", e);
            setStatus(`Save failed (${e.message})`);
        }
    }

    elSave?.addEventListener("click", save);
    load();
}

function logLine(tag, msg){
  const log = $("#log");
  if (!log) return;
  const line = document.createElement("div");
  line.className = "log__line";
  const ts = new Date().toLocaleTimeString();
  line.innerHTML = `<span class="log__ts">[${ts}]</span><span class="log__tag">${tag}</span><span class="log__msg"></span>`;
  line.querySelector(".log__msg").textContent = msg;
  log.prepend(line);
}

function setBadge(platform, live, label){
  const b = document.getElementById(`${platform}Badge`);
  if (!b) return;
  b.textContent = label || (live ? "Live" : "Offline");
  b.classList.toggle("badge--live", !!live);

  // Also mark the platform card so CSS can tint the icon subtly when live.
  const card = document.querySelector(`.platform[data-platform="${platform}"]`);
  if (card) card.classList.toggle("platform--live", !!live);
}

function updateHomeSummary(){
  if (isHomePage()) {
    renderHomePlatforms();
    return;
  }

  const cards = $$(".platform[data-platform]");
  const linked = cards.filter(card => {
    const platform = card.dataset.platform;
    const input = getPlatformInput(platform);
    return !!sanitizePlatformValue(platform, input?.value || "");
  }).length;
  const live = cards.filter(card => card.classList.contains("platform--live")).length;

  setText("linkedAccountsCount", `${linked} / ${cards.length || 0}`);
  setText("livePlatformsCount", String(live));
}

function setState(platform, text){
  const el = document.getElementById(`${platform}State`);
  if (el) el.textContent = text;

  const dot = document.getElementById(`${platform}StateDot`);
  const card = document.querySelector(`.platform[data-platform="${platform}"]`);
  if (!card) {
    updateHomeSummary();
    return;
  }

  const normalized = String(text || "").toLowerCase();
  const live = normalized === "live";
  const connected = live || normalized === "connected";

  card.classList.toggle("platform--live", live);
  card.classList.toggle("platform--connected", connected);
  card.classList.toggle("platform--disconnected", !connected);

  if (dot) {
    dot.classList.toggle("platform__state-dot--live", live);
  }

  updateHomeSummary();
}

function setStopEnabled(platform, enabled){
  const card = document.querySelector(`.platform[data-platform="${platform}"]`);
  if (!card) return;
  const stop = card.querySelector(`button[data-action="stop"]`);
  if (stop) stop.disabled = !enabled;
}

function guessMetricsShape(m){
  // We intentionally support multiple possible shapes, since the project evolved.
  // Top-level convenience
  const viewers = m.total_viewers ?? m.viewers ?? m.viewer_count ?? m.live_viewers ?? m.concurrent_viewers;
  const followers = m.total_followers ?? m.followers ?? m.follower_count ?? m.subscribers ?? m.subscriber_count;
  // Aircraft is displayed as "assumed_now/handled_session" when EuroScope data is available.
  const aircraft = (m.euroscope && (m.euroscope.assumed_now !== undefined || m.euroscope.handled_session !== undefined))
    ? `${m.euroscope.assumed_now ?? 0}/${m.euroscope.handled_session ?? 0}`
    : (m.aircraft ?? m.aircraft_count ?? m.euroscope_aircraft ?? m.aircrafts);

  // Per-platform buckets (common patterns: m.tiktok.viewers, m.platforms.tiktok.viewers, etc.)
  const getP = (p) => (m[p] || (m.platforms && m.platforms[p]) || (m.metrics && m.metrics[p]) || null);

  return {
    viewers, followers, aircraft,
    tiktok: getP("tiktok"),
    twitch: getP("twitch"),
    youtube: getP("youtube"),
  };
}

function applyMetrics(m){
  homeMetricsCache = m;
  const s = guessMetricsShape(m);

  setText("mViewers", fmtNum(s.viewers));
  setText("mFollowers", fmtNum(s.followers));
  setText("mAircraft", (typeof s.aircraft === "string") ? s.aircraft : fmtNum(s.aircraft));

  // Twitch (top-level fields in /api/metrics)
  const tw_viewers = m.twitch_viewers ?? (s.twitch && (s.twitch.viewers ?? s.twitch.viewer_count ?? s.twitch.live_viewers));
  const tw_followers = m.twitch_followers ?? (s.twitch && (s.twitch.followers ?? s.twitch.follower_count));
  const tw_live = (m.twitch_live !== undefined) ? m.twitch_live : (s.twitch && (s.twitch.live ?? s.twitch.is_live));

  setText("twitchViewers", fmtNum(tw_viewers));
  setText("twitchFollowers", fmtNum(tw_followers));

  const twBadge = $("#twitchBadge");
  const twState = $("#twitchState");
  const isLive = (tw_live === true || tw_live === 1 || tw_live === "true");
  const hasData = (tw_viewers !== null && tw_viewers !== undefined) || (tw_followers !== null && tw_followers !== undefined);

  // IMPORTANT: toggle both label and CSS class so the badge can turn green when live.
  // (Previously we only changed text, leaving the badge blue.)
  setBadge("twitch", isLive);
  setState("twitch", isLive ? "Live" : (hasData ? "Connected" : "Disconnected"));

  

  // TikTok (top-level fields in /api/metrics)
  const tt_viewers = m.tiktok_viewers ?? (s.tiktok && (s.tiktok.viewers ?? s.tiktok.viewer_count ?? s.tiktok.live_viewers ?? s.tiktok.concurrent_viewers));
  const tt_followers = m.tiktok_followers ?? (s.tiktok && (s.tiktok.followers ?? s.tiktok.follower_count));
  const tt_live = (m.tiktok_live !== undefined) ? m.tiktok_live : (s.tiktok && (s.tiktok.live ?? s.tiktok.is_live));

  setText("tiktokViewers", fmtNum(tt_viewers));
  setText("tiktokFollowers", fmtNum(tt_followers));

  const ttBadge = $("#tiktokBadge");
  const ttState = $("#tiktokState");
  const ttIsLive = (tt_live === true || tt_live === 1 || tt_live === "true");
  const ttHasData = (tt_viewers !== null && tt_viewers !== undefined) || (tt_followers !== null && tt_followers !== undefined);

  setBadge("tiktok", ttIsLive);
  setState("tiktok", ttIsLive ? "Live" : (ttHasData ? "Connected" : "Disconnected"));
  setStopEnabled("tiktok", ttIsLive || ttHasData);

  // YouTube (top-level fields in /api/metrics)
  const yt_viewers = m.youtube_viewers ?? (s.youtube && (s.youtube.viewers ?? s.youtube.viewer_count ?? s.youtube.live_viewers ?? s.youtube.concurrent_viewers));
  const yt_followers = m.youtube_followers ?? (s.youtube && (s.youtube.followers ?? s.youtube.follower_count ?? s.youtube.subscribers ?? s.youtube.subscriber_count));
  const yt_live = (m.youtube_live !== undefined) ? m.youtube_live : (s.youtube && (s.youtube.live ?? s.youtube.is_live));

  setText("youtubeViewers", fmtNum(yt_viewers));
  setText("youtubeFollowers", fmtNum(yt_followers));

  const ytBadge = $("#youtubeBadge");
  const ytState = $("#youtubeState");
  const ytIsLive = (yt_live === true || yt_live === 1 || yt_live === "true");
  const ytHasData = (yt_viewers !== null && yt_viewers !== undefined) || (yt_followers !== null && yt_followers !== undefined);

  setBadge("youtube", ytIsLive);
  setState("youtube", ytIsLive ? "Live" : (ytHasData ? "Connected" : "Disconnected"));
  setStopEnabled("youtube", ytIsLive || ytHasData);

// status dot (best effort)
  const dot = $("#mDot");
  if (dot){
    const liveAny = Boolean(
      (s.tiktok && (s.tiktok.live || s.tiktok.is_live)) ||
      (s.twitch && (s.twitch.live || s.twitch.is_live)) ||
      (s.youtube && (s.youtube.live || s.youtube.is_live))
    );
    // Match the UI: green when any channel is live
    dot.style.background = liveAny ? "var(--green)" : "#64748b";
  }

  const applyPlatform = (name, bucket) => {
    if (!bucket) return;
    // NOTE: "connected" is not the same as "live".
    // Only consider explicit live/streaming flags as live.
    const live = Boolean(bucket.live ?? bucket.is_live ?? bucket.streaming ?? false);
    const connected = Boolean(bucket.connected ?? bucket.ok ?? bucket.running ?? false);
    setBadge(name, live, live ? "Live" : "Offline");
    setState(name, live ? "Live" : (connected ? "Connected" : "Disconnected"));

    // stats
    const v = bucket.viewers ?? bucket.viewer_count ?? bucket.live_viewers ?? bucket.concurrent_viewers;
    const f = bucket.followers ?? bucket.follower_count ?? bucket.subscribers ?? bucket.subscriber_count;
    setText(`${name}Viewers`, fmtNum(v));
    setText(`${name}Followers`, fmtNum(f));

    // stop button enabled when connected or live (best effort)
    setStopEnabled(name, live || connected);
  };

  applyPlatform("tiktok", s.tiktok);
  applyPlatform("twitch", s.twitch);
  applyPlatform("youtube", s.youtube);

  // overall status label
  const status = $("#mStatus");
  if (status){
    const anyLive = Boolean(
      (s.tiktok && (s.tiktok.live || s.tiktok.is_live)) ||
      (s.twitch && (s.twitch.live || s.twitch.is_live)) ||
      (s.youtube && (s.youtube.live || s.youtube.is_live))
    );
    status.textContent = anyLive ? "Live" : "Idle";
  }

  const build = $("#buildInfo");
  if (build){
    // if you include build fields in metrics payload, they'll show here
    const ver = m.version || (m.app && m.app.version) || window.__APP_BUILDINFO || null;
    const port = m.port || (m.http && m.http.port) || null;
    const parts = [];
    if (ver) parts.push(`${ver}`);
    if (port) parts.push(`:${port}`);
    build.textContent = parts.length ? parts.join(" ") : "—";
  }

  if (isHomePage()) {
    renderHomePlatforms();
  }
}

async function pollMetrics(){
  try{
    const m = await apiGet("/api/metrics");
    applyMetrics(m);
  }catch(e){
    // avoid spamming
    console.debug("metrics poll failed", e);
  }
}

let _logSince = 0;
async function pollLog(){
  try{
    const j = await apiGet(`/api/log?since=${_logSince}&limit=200`);
    if (!j || !j.ok) return;
    const entries = j.entries || [];
    for (const e of entries){
      const id = Number(e.id)||0;
      if (id>_logSince) _logSince = id;
      logLine("srv", e.msg);
    }
  }catch(e){
    // ignore
  }
}

function wireActions(){
  $("#btnOpenChat")?.addEventListener("click", () => {
    if (window.chrome?.webview?.postMessage) {
      window.chrome.webview.postMessage({ type: "open_chat" });
      return;
    }
    window.open("/overlay/chat.html", "_blank", "noopener");
  });

  $("#btnOpenSettings")?.addEventListener("click", () => {
    window.location.href = "/app/settings.html";
  });

  $("#btnOpenDiagnostics")?.addEventListener("click", () => {
    window.location.href = "/app/diagnostics.html";
  });

  $("#btnOpenBot")?.addEventListener("click", () => {
    window.location.href = "/app/bot.html";
  });
}


function normalizeHomePlatform(platform){
  return String(platform || "").toLowerCase();
}

function getHomeSettingValue(platform){
  const s = homeSettingsCache || {};
  switch(platform){
    case "tiktok": return sanitizeTikTok(s.tiktok_unique_id || "");
    case "twitch": return sanitizeTwitch(s.twitch_login || "");
    case "youtube": return sanitizeYouTube(s.youtube_handle || "");
    default: return "";
  }
}

function getHomeAuthState(platform, info){
  if (!info || info.ok === false) return "error";

  if (platform === "tiktok") {
    const hasIdentity = !!sanitizeTikTok(info.unique_id || getHomeSettingValue(platform));
    const hasCookies = !!info.has_sessionid && !!info.has_sessionid_ss && !!info.has_tt_target_idc;
    if (hasIdentity && hasCookies) return "connected";
    if (hasIdentity) return "needs_auth";
    return "not_configured";
  }

  const hasConfig = !!info.has_client_id && !!info.has_client_secret;
  const hasTokens = !!info.has_refresh_token || !!info.has_access_token;
  if (!hasConfig) return "missing_config";
  if (info.needs_reauth) return "needs_auth";
  if (hasTokens) return "connected";
  if (getHomeSettingValue(platform)) return "selected_only";
  return "not_configured";
}

function formatHomePrimaryIdentity(platform, value){
  if (!value) return "No account selected";
  if (platform === "tiktok") return value.startsWith("@") ? value : `@${value}`;
  if (platform === "youtube") return value.replace(/^@/, "");
  return value.startsWith("@") ? value : `@${value}`;
}

function getHomeIdentity(platform){
  const info = homeAuthCache[platform] || {};
  const selected = getHomeSettingValue(platform);

  if (platform === "tiktok") {
    const uniqueId = sanitizeTikTok(info.unique_id || selected);
    return uniqueId
      ? {
          hasIdentity: true,
          primary: formatHomePrimaryIdentity(platform, uniqueId),
          secondary: ""
        }
      : {
          hasIdentity: false,
          primary: "No account selected",
          secondary: ""
        };
  }

  if (platform === "twitch") {
    const login = sanitizeTwitch(info.connected_login || selected);
    return login
      ? {
          hasIdentity: true,
          primary: formatHomePrimaryIdentity(platform, login),
          secondary: ""
        }
      : {
          hasIdentity: false,
          primary: "No account selected",
          secondary: ""
        };
  }

  const handle = sanitizeYouTube(selected);
  const channelId = trim(info.channel_id || "");
  if (handle) {
    return {
      hasIdentity: true,
      primary: formatHomePrimaryIdentity(platform, handle),
      secondary: ""
    };
  }
  if (channelId) {
    return {
      hasIdentity: true,
      primary: channelId,
      secondary: ""
    };
  }
  return {
    hasIdentity: false,
    primary: "No account selected",
    secondary: ""
  };
}

function getHomeConfigModel(platform){
  const info = homeAuthCache[platform] || {};
  const authState = getHomeAuthState(platform, info);
  const identity = getHomeIdentity(platform);

  if (platform === "tiktok") {
    if (authState === "connected") {
      return { label: "Configured", ready: true, issue: false, tone: "ok", hasIdentity: true, needsAuth: false };
    }
    if (authState === "needs_auth") {
      return { label: "Connect required", ready: false, issue: true, tone: "warn", hasIdentity: identity.hasIdentity, needsAuth: true };
    }
    return { label: "Not configured", ready: false, issue: false, tone: "muted", hasIdentity: identity.hasIdentity, needsAuth: false };
  }

  switch(authState){
    case "connected":
      return { label: "Configured", ready: true, issue: false, tone: "ok", hasIdentity: identity.hasIdentity, needsAuth: false };
    case "needs_auth":
      return { label: "Needs re-auth", ready: false, issue: true, tone: "warn", hasIdentity: identity.hasIdentity, needsAuth: true };
    case "selected_only":
      return { label: "Account selected", ready: false, issue: true, tone: "warn", hasIdentity: identity.hasIdentity, needsAuth: true };
    case "missing_config":
      return { label: "Unavailable", ready: false, issue: true, tone: "warn", hasIdentity: identity.hasIdentity, needsAuth: false, unavailable: true };
    default:
      return { label: "Not configured", ready: false, issue: false, tone: "muted", hasIdentity: identity.hasIdentity, needsAuth: false };
  }
}

function getHomeMetricLive(platform){
  const m = homeMetricsCache || {};
  const direct = m[`${platform}_live`];
  if (direct === true || direct === 1 || direct === "true") return true;

  try {
    const shaped = guessMetricsShape(m);
    const bucket = shaped[platform];
    return Boolean(bucket && (bucket.live ?? bucket.is_live ?? bucket.streaming ?? false));
  } catch (_) {
    return false;
  }
}

function getHomeRuntimeModel(platform){
  const status = homePlatformStatusCache[platform] || {};
  const requested = String(status.requested_state || "stopped").toLowerCase();
  const running = getHomeMetricLive(platform) || requested === "running";
  return {
    label: running ? "Running" : "Stopped",
    running,
    ts_ms: Number(status.ts_ms || 0) || 0
  };
}

function getHomePrimaryAction(platform, configModel, runtimeModel){
  if (configModel.unavailable) {
    return { kind: "noop", label: "Unavailable", disabled: true };
  }
  if (!configModel.hasIdentity) {
    return { kind: "settings", label: "Open Settings", disabled: false };
  }
  if (configModel.needsAuth) {
    return { kind: "auth", label: platform === "tiktok" ? "Connect" : "Re-auth", disabled: false };
  }
  return { kind: "start", label: runtimeModel.running ? "Restart" : "Start", disabled: false };
}

function homeCardHintText(platform, configModel){
  return "";
}

function applyHomeCardTone(platform, configModel, runtimeModel){
  const card = document.querySelector(`.platform[data-platform="${platform}"]`);
  const badge = document.getElementById(`${platform}Badge`);
  if (!card || !badge) return;

  card.classList.toggle("platform--live", runtimeModel.running);
  card.classList.toggle("platform--connected", configModel.ready && !runtimeModel.running);
  card.classList.toggle("platform--issue", configModel.issue && !runtimeModel.running);
  card.classList.toggle("platform--disconnected", !configModel.hasIdentity && !runtimeModel.running);

  badge.classList.remove("badge--live", "badge--warn", "badge--muted");
  if (runtimeModel.running) badge.classList.add("badge--live");
  else if (configModel.issue) badge.classList.add("badge--warn");
  else badge.classList.add("badge--muted");
}

function renderHomePlatforms(){
  if (!isHomePage()) return;

  let selectedCount = 0;
  let runningCount = 0;
  let issueCount = 0;

  HOME_PLATFORMS.forEach((platform) => {
    const identity = getHomeIdentity(platform);
    const configModel = getHomeConfigModel(platform);
    const runtimeModel = getHomeRuntimeModel(platform);
    const primaryAction = getHomePrimaryAction(platform, configModel, runtimeModel);

    if (identity.hasIdentity) selectedCount += 1;
    if (runtimeModel.running) runningCount += 1;
    if (configModel.issue) issueCount += 1;

    setText(`${platform}IdentityPrimary`, identity.primary);
    setText(`${platform}IdentitySecondary`, identity.secondary);
    setText(`${platform}ConfigState`, configModel.label);
    setText(`${platform}RuntimeState`, runtimeModel.label);
    setText(`${platform}State`, configModel.label);
    setText(`${platform}Badge`, runtimeModel.label);

    applyHomeCardTone(platform, configModel, runtimeModel);

    const primaryBtn = getPlatformButton(platform, "primary");
    if (primaryBtn) {
      primaryBtn.textContent = primaryAction.label;
      primaryBtn.dataset.label = primaryAction.label;
      primaryBtn.disabled = !!primaryAction.disabled;
    }

    const stopBtn = getPlatformButton(platform, "stop");
    if (stopBtn) stopBtn.disabled = !runtimeModel.running;

    homeActionState[platform] = {
      primary: primaryAction,
      runtime: runtimeModel,
      config: configModel
    };
  });

  setText("linkedAccountsCount", `${selectedCount} / ${HOME_PLATFORMS.length}`);
  setText("livePlatformsCount", String(runningCount));
  setText("issuePlatformsCount", String(issueCount));
}

async function fetchHomeAuthStatuses(){
  if (!isHomePage()) return;

  const tasks = HOME_PLATFORMS.map(async (platform) => {
    const url = platform === "twitch"
      ? "/api/twitch/auth/info"
      : platform === "youtube"
        ? "/api/youtube/auth/info"
        : "/api/tiktok/auth/info";

    try {
      homeAuthCache[platform] = await apiGet(url);
    } catch (_) {
      homeAuthCache[platform] = { ok: false };
    }
  });

  await Promise.all(tasks);
  renderHomePlatforms();
}

async function pollHomePlatformStatus(){
  if (!isHomePage()) return;

  try {
    const status = await apiGet("/api/platform/status");
    HOME_PLATFORMS.forEach((platform) => {
      const node = status?.[platform];
      if (node && typeof node === "object") {
        homePlatformStatusCache[platform] = {
          requested_state: String(node.requested_state || "stopped").toLowerCase(),
          ts_ms: Number(node.ts_ms || 0) || 0
        };
      }
    });
  } catch (_) {
    // leave the previous snapshot in place
  }

  renderHomePlatforms();
}

function openConnectedAccountsPage(){
  window.location.href = "/app/accounts.html";
}

async function performHomePrimaryAction(platform, button){
  const state = homeActionState[platform];
  if (!state || !state.primary || state.primary.disabled) return;

  const action = state.primary;
  if (action.kind === "settings" || action.kind === "auth") {
    openConnectedAccountsPage();
    return;
  }
  if (action.kind !== "start") return;

  setActionBusy(button, true, state.runtime.running ? "Restarting..." : "Starting...");
  try {
    await apiPost(`/api/platform/${platform}/start`, {});
    logLine(platform, state.runtime.running ? "restart requested" : "start requested");
    await pollHomePlatformStatus();
  } catch (e) {
    logLine(platform, `${state.runtime.running ? "restart" : "start"} failed (${e.message})`);
  } finally {
    setActionBusy(button, false);
    renderHomePlatforms();
  }
}

async function performHomeStop(platform, button){
  setActionBusy(button, true, "Stopping...");
  try {
    await apiPost(`/api/platform/${platform}/stop`, {});
    logLine(platform, "stop requested");
    await pollHomePlatformStatus();
  } catch (e) {
    logLine(platform, `stop failed (${e.message})`);
  } finally {
    setActionBusy(button, false);
    renderHomePlatforms();
  }
}

function wireHomePage(){
  const root = document.getElementById("homePage");
  if (!root) return;

  root.addEventListener("click", async (ev) => {
    const btn = ev.target.closest("button[data-action]");
    if (!btn) return;

    const card = btn.closest(".platform[data-platform]");
    const action = btn.getAttribute("data-action");
    if (!card) return;

    const platform = normalizeHomePlatform(card.dataset.platform);
    if (!platform) return;

    if (action === "primary") {
      await performHomePrimaryAction(platform, btn);
      return;
    }

    if (action === "stop") {
      await performHomeStop(platform, btn);
    }
  });

  $("#btnStartAll")?.addEventListener("click", async (ev) => {
    const btn = ev.currentTarget;
    setActionBusy(btn, true, "Starting...");
    try {
      const runnable = HOME_PLATFORMS.filter((platform) => homeActionState[platform]?.primary?.kind === "start");
      if (runnable.length === 0) {
        openConnectedAccountsPage();
        return;
      }

      for (const platform of runnable) {
        try {
          await apiPost(`/api/platform/${platform}/start`, {});
          logLine("start-all", `${platform} ${homeActionState[platform]?.runtime?.running ? "restart" : "start"} requested`);
        } catch (e) {
          logLine("start-all", `${platform} failed (${e.message})`);
        }
      }

      await pollHomePlatformStatus();
    } finally {
      setActionBusy(btn, false);
      renderHomePlatforms();
    }
  });
}

function wireNativeHostMessages() {
  if (window.__rcNativeHostMessagesWired) return;
  window.__rcNativeHostMessagesWired = true;

  try {
    if (!window.chrome?.webview?.addEventListener) return;

    window.chrome.webview.addEventListener("message", async (event) => {
      const msg = event?.data || {};
      if (!msg || typeof msg !== "object") return;

      if (msg.type === "tiktok_auth_cookies") {
        try {
          await apiPost("/api/settings/tiktok-cookies", {
            tiktok_sessionid: String(msg.tiktok_sessionid || "").trim(),
            tiktok_sessionid_ss: String(msg.tiktok_sessionid_ss || "").trim(),
            tiktok_tt_target_idc: String(msg.tiktok_tt_target_idc || "").trim()
          });

          const input = document.getElementById("tiktokConnectUser");
          const uniqueId = sanitizeTikTok((input && input.value) ? input.value : (msg.tiktok_unique_id || ""));
          if (uniqueId) {
            await apiPost("/api/settings/save", { tiktok_unique_id: uniqueId });
          }

          document.dispatchEvent(new CustomEvent("rc:tiktok-auth-captured", { detail: msg }));
        } catch (e) {
          console.error("Failed to persist captured TikTok cookies", e);
        }
      }
    });
  } catch (_) {
    // no native host
  }
}


document.addEventListener("DOMContentLoaded", async () => {
    wireSmartBackButtons();
    wireSettingsHubPage();
    wireTwitchStreamInfoPage();
    wireYouTubeAuthStatus();
    wireConnectedAccountsPage();
    wireNativeHostMessages();
    loadYouTubeVodDraft();
    wireTikTokCookiesPage();
    wireOverlayTitlePage();
    wireActions();
    wireHomePage();

    try {
      await loadSettings();
    } catch (_) {
      // loadSettings already logged the failure
    }

    if (isHomePage()) {
      await fetchHomeAuthStatuses();
      await pollHomePlatformStatus();
    }

    await pollMetrics();

  requestAnimationFrame(() => {
    requestAnimationFrame(() => {
      notifyNativeAppReady();
    });
  });

  setInterval(pollMetrics, 2000);
  if (isHomePage()) {
    setInterval(fetchHomeAuthStatuses, 15000);
    setInterval(pollHomePlatformStatus, 3000);
  }
});

function sanitizeOverlayCallsign(input){
  return String(input ?? "")
    .toUpperCase()
    .replace(/[^A-Z0-9_-]/g, "")
    .trim();
}

function sanitizeOverlayRouteText(input){
  return String(input ?? "")
    .replace(/[\r\n\t]+/g, " ")
    .replace(/\s{2,}/g, " ")
    .trim()
    .slice(0, 64);
}

function wireOverlayTitlePage() {
  const elTitle = document.getElementById("overlayTitle");
  const elSub = document.getElementById("overlaySubtitle");
  const elUseSimBrief = document.getElementById("overlayUseSimBrief");
  const elManualWrap = document.getElementById("overlayManualRouteFields");
  const elManualCallsign = document.getElementById("overlayManualCallsign");
  const elManualDeparture = document.getElementById("overlayManualDeparture");
  const elManualDestination = document.getElementById("overlayManualDestination");
  const elSave = document.getElementById("btnSaveOverlayTitle");
  const elStatus = document.getElementById("overlayTitleStatus");

  if (!elTitle || !elSub || !elUseSimBrief || !elManualWrap || !elManualCallsign || !elManualDeparture || !elManualDestination || !elSave) {
    return;
  }

  const setStatus = (t) => {
    if (elStatus) elStatus.textContent = t || "";
  };

  function syncManualUi(){
    const manualMode = !elUseSimBrief.checked;
    elManualWrap.style.display = manualMode ? "block" : "none";
    [elManualCallsign, elManualDeparture, elManualDestination].forEach((el) => {
      el.disabled = !manualMode;
    });
  }

  function sanitizeManualInputs(){
    elManualCallsign.value = sanitizeOverlayCallsign(elManualCallsign.value);
    elManualDeparture.value = sanitizeOverlayRouteText(elManualDeparture.value);
    elManualDestination.value = sanitizeOverlayRouteText(elManualDestination.value);
  }

  async function load() {
    setStatus("Loading…");
    try {
      const j = await apiGet("/api/overlay/header");

      elTitle.value = (j && typeof j.title === "string") ? j.title : "";
      elSub.value = (j && typeof j.subtitle === "string") ? j.subtitle : "";
      elUseSimBrief.checked = !(j && j.use_simbrief === false);
      elManualCallsign.value = (j && typeof j.manual_callsign === "string") ? j.manual_callsign : "";
      elManualDeparture.value = (j && typeof j.manual_departure === "string") ? j.manual_departure : "";
      elManualDestination.value = (j && typeof j.manual_destination === "string") ? j.manual_destination : "";

      sanitizeManualInputs();
      syncManualUi();
      setStatus("");
    } catch (e) {
      console.warn("Failed to load overlay header", e);
      syncManualUi();
      setStatus("Could not load current header");
    }
  }

  async function save() {
    setStatus("Saving…");
    sanitizeManualInputs();

    const body = {
      title: elTitle.value || "",
      subtitle: elSub.value || "",
      use_simbrief: !!elUseSimBrief.checked,
      manual_callsign: elManualCallsign.value || "",
      manual_departure: elManualDeparture.value || "",
      manual_destination: elManualDestination.value || ""
    };

    try {
      const j = await apiPost("/api/overlay/header", body);
      if (j && j.ok === false) throw new Error(j.error || "ok=false");

      elTitle.value = (j && typeof j.title === "string") ? j.title : body.title;
      elSub.value = (j && typeof j.subtitle === "string") ? j.subtitle : body.subtitle;
      elUseSimBrief.checked = !(j && j.use_simbrief === false);
      elManualCallsign.value = (j && typeof j.manual_callsign === "string") ? j.manual_callsign : body.manual_callsign;
      elManualDeparture.value = (j && typeof j.manual_departure === "string") ? j.manual_departure : body.manual_departure;
      elManualDestination.value = (j && typeof j.manual_destination === "string") ? j.manual_destination : body.manual_destination;

      sanitizeManualInputs();
      syncManualUi();
      setStatus("Saved");
      return true;
    } catch (e) {
      console.error("Overlay header save failed", e);
      setStatus("Save failed");
      return false;
    }
  }

  elUseSimBrief.addEventListener("change", syncManualUi);
  [elManualCallsign, elManualDeparture, elManualDestination].forEach((el) => {
    el.addEventListener("input", sanitizeManualInputs);
    el.addEventListener("blur", sanitizeManualInputs);
  });

  elSave.addEventListener("click", save);

  [elTitle, elSub, elUseSimBrief, elManualCallsign, elManualDeparture, elManualDestination].forEach((el) => {
    el.addEventListener("keydown", (ev) => {
      if ((ev.ctrlKey || ev.metaKey) && ev.key === "Enter") save();
    });
  });

  syncManualUi();
  load();
}

function wireSettingsHubPage(){
  const root = document.getElementById("settingsPage");
  if(!root) return;

  $("#btnGoBot")?.addEventListener("click", () => {
    window.location.href = "/app/bot.html";
  });

  $("#btnGoOverlayTitle")?.addEventListener("click", () => {
    window.location.href = "/app/overlay_title.html";
  });

  $("#btnGoTwitchStream")?.addEventListener("click", () => {
    window.location.href = "/app/twitch_stream.html";
  });
}

// Smart back routing: if user came from Settings hub, go back there.
function wireSmartBackButtons(){
  function target(){
    try{
      const ref = document.referrer || "";
      if (ref.includes("/app/settings.html")) return "/app/settings.html";
      if (ref.includes("/app/twitch_stream.html")) return "/app/settings.html";
    }catch(_){}
    return "/app/index.html";
  }
  // bot.html uses btnBack; other pages use btnBackHome
  $("#btnBack")?.addEventListener("click", () => { window.location.href = target(); });
  $("#btnBackHome")?.addEventListener("click", () => { window.location.href = target(); });
}

function wireTwitchStreamInfoPage(){
  const root = document.getElementById("twitchStreamPage");
  if(!root) return;

    const elTitle = document.getElementById("twitchTitle");
    const elCat = document.getElementById("twitchCategory");   // display name typed/selected
    const elGameId = document.getElementById("twitchGameId");     // hidden selected id
    const elDesc = document.getElementById("twitchDescription");
    const elSave = document.getElementById("btnSaveTwitchDraft");
    const elApply = document.getElementById("btnApplyTwitch");
    const elStatus = document.getElementById("twitchStreamStatus");

  const setStatus = (t)=>{ if(elStatus) elStatus.textContent = t||""; };

  async function load(){
    try{
      const j = await apiGet("/api/twitch/streaminfo");
      if(elTitle) elTitle.value = j.title || "";
      if(elCat) elCat.value = j.category || "";
      if(elDesc) elDesc.value = j.description || "";
      setStatus("");
    }catch(e){
      console.warn("Failed to load Twitch stream info", e);
      setStatus("Could not load current draft");
    }
  }

  async function save(){
    setStatus("Saving…");
    try{
        const body = {
            title: elTitle ? elTitle.value : "",

            // Keep legacy field for older backend compatibility
            category: elCat ? elCat.value : "",

            // New fields: store both name + id (Helix needs the id)
            category_name: elCat ? elCat.value : "",
            category_id: elGameId ? elGameId.value : "",
            game_id: elGameId ? elGameId.value : "",

            description: elDesc ? elDesc.value : ""
        };
      const j = await apiPost("/api/twitch/streaminfo", body);
      if(j && j.ok === false) throw new Error(j.error || "ok=false");
      setStatus("Saved");
      return true;
    }catch(e){
      console.error(e);
      setStatus("Error saving");
      return false;
    }
  }

    async function apply() {
        console.log("Apply clicked; current values:", {
            title: document.getElementById("twitchTitle")?.value,
            category_name: document.getElementById("twitchCategory")?.value,
            category_id: document.getElementById("twitchGameId")?.value
        });
    setStatus("Updating Twitch…");
    try{
      const ok = await save();
      if(!ok) return;
      const j = await apiPost("/api/twitch/streaminfo/apply");
      if(j && j.ok === false) throw new Error(j.error || "ok=false");
      setStatus("Updated on Twitch");
    }catch(e){
      console.error(e);
      setStatus("Error updating Twitch");
    }
  }

  elSave?.addEventListener("click", save);
  elApply?.addEventListener("click", apply);

    console.log("Apply Twitch payload:", {
    title: elTitle?.value,
    category_name: elCat?.value,
    category_id: elGameId?.value
    });

  load();
}

function getYouTubeAuthUiState(data) {
  if (!data || data.ok === false) return "error";
  const hasConfig = !!data.has_client_id && !!data.has_client_secret;
  if (!hasConfig) return "missing_config";
  if (data.needs_reauth) return "reauth_required";
  if (data.has_refresh_token || data.has_access_token) return "connected";
  return "not_connected";
}

function getYouTubeAuthUiLabel(data) {
  switch (getYouTubeAuthUiState(data)) {
    case "missing_config": return "App credentials unavailable";
    case "reauth_required": return "Re-authentication required";
    case "connected": return "Connected";
    case "not_connected": return "Not connected";
    default: return "Status unavailable";
  }
}

function setYouTubeVodEditingEnabled(enabled) {
  const t = document.getElementById("youtubeVodTitle");
  const d = document.getElementById("youtubeVodDescription");
  const b = document.getElementById("btnApplyYouTubeVod");
  if (t) t.readOnly = !enabled;
  if (d) d.readOnly = !enabled;
  if (b) b.disabled = !enabled;
}

function wireYouTubeAuthStatus() {
  const nodes = document.querySelectorAll("#ytAuthStatusText");
  if (!nodes || nodes.length === 0) return;

  const setAll = (text) => nodes.forEach(n => { n.textContent = text; });
  setAll("Checking YouTube connection…");

  fetch("/api/youtube/auth/info", { cache: "no-store" })
    .then(r => r.ok ? r.json() : Promise.reject(new Error("http " + r.status)))
    .then(data => {
      const state = getYouTubeAuthUiState(data);
      if (state === "connected") {
        setAll("Connected");
        setYouTubeVodEditingEnabled(true);
        return;
      }
      if (state === "reauth_required") {
        setAll("Re-authorisation required");
        setYouTubeVodEditingEnabled(false);
        return;
      }
      if (state === "missing_config") {
        setAll("App credentials unavailable");
        setYouTubeVodEditingEnabled(false);
        return;
      }
      setAll("Not connected");
      setYouTubeVodEditingEnabled(false);
    })
    .catch(() => {
      setAll("Status unavailable");
      setYouTubeVodEditingEnabled(false);
    });
}


function openPlatformAuthPopup(startUrl, platform) {
  const absoluteUrl = /^https?:\/\//i.test(startUrl)
    ? startUrl
    : `${window.location.origin}${startUrl}`;

  let title = "Sign in";
  if (platform === "twitch") title = "Twitch sign-in";
  else if (platform === "youtube") title = "YouTube sign-in";
  else if (platform === "tiktok") title = "TikTok sign-in";

  try {
    if (window.chrome?.webview?.postMessage) {
      window.chrome.webview.postMessage({
        type: "open_auth_popup",
        platform,
        url: absoluteUrl,
        title
      });
      return true;
    }
  } catch (_) {
    // Fall through to browser fallback below.
  }

  return false;
}


function wireConnectedAccountsPage() {
  const root = document.getElementById("connectedAccountsPage");
  if (!root) return;

  const state = {
    twitch: { info: null, polling: null },
    youtube: { info: null, polling: null },
    tiktok: { info: null, polling: null }
  };

  const paths = {
    twitch: "/api/twitch/auth/info",
    youtube: "/api/youtube/auth/info",
    tiktok: "/api/tiktok/auth/info"
  };

  function setTextSafe(id, text) {
    const el = document.getElementById(id);
    if (el) el.textContent = text ?? "";
  }

  function setHidden(id, hidden) {
    const el = document.getElementById(id);
    if (el) el.hidden = !!hidden;
  }

  function setButtonBusy(btn, busy, label) {
    if (!btn) return;
    if (!btn.dataset.baseLabel) btn.dataset.baseLabel = btn.textContent || "";
    btn.disabled = !!busy;
    btn.textContent = busy ? (label || "Working…") : btn.dataset.baseLabel;
  }

  function getPrimaryButton(platform) {
    if (platform === "twitch") return document.getElementById("btnTwitchAuthPrimary");
    if (platform === "youtube") return document.getElementById("btnYouTubeAuthPrimary");
    return document.getElementById("btnTikTokAuthPrimary");
  }

  function getRefreshButton(platform) {
    if (platform === "twitch") return document.getElementById("btnTwitchAuthRefresh");
    if (platform === "youtube") return document.getElementById("btnYouTubeAuthRefresh");
    return document.getElementById("btnTikTokAuthRefresh");
  }

  function getPlatformState(platform, info) {
    if (!info || info.ok === false) return "error";

    if (platform === "tiktok") {
      const hasIdentity = !!String(info.unique_id || "").trim();
      const hasCookies = !!info.has_sessionid && !!info.has_sessionid_ss && !!info.has_tt_target_idc;
      if (!hasIdentity) return "needs_identity";
      if (hasCookies) return "connected";
      return "not_connected";
    }

    const hasConfig = !!info.has_client_id && !!info.has_client_secret;
    const hasTokens = !!info.has_refresh_token || !!info.has_access_token;
    if (!hasConfig) return "missing_config";
    if (info.needs_reauth) return "reauth_required";
    if (hasTokens) return "connected";
    return "not_connected";
  }

  function getPlatformSummary(platform, info) {
    const s = getPlatformState(platform, info);

    if (platform === "twitch") {
      if (s === "connected") return info.connected_login ? `Connected as ${info.connected_login}` : "Connected";
      if (s === "reauth_required") return "Twitch needs to be connected again.";
      if (s === "missing_config") return "Twitch app credentials are not embedded in this build.";
      if (s === "not_connected") return "Twitch is ready to connect.";
      return "Could not load Twitch status.";
    }

    if (platform === "youtube") {
      if (s === "connected") return info.channel_id ? `Connected to channel ${info.channel_id}` : "Connected";
      if (s === "reauth_required") return "YouTube needs to be connected again.";
      if (s === "missing_config") return "YouTube app credentials are not embedded in this build.";
      if (s === "not_connected") return "YouTube is ready to connect.";
      return "Could not load YouTube status.";
    }

    if (s === "connected") return info.unique_id ? `Session saved for @${info.unique_id}` : "Session saved";
    if (s === "needs_identity") return "Enter the TikTok account name you want to connect.";
    if (s === "not_connected") return "TikTok is ready to capture a session.";
    return "Could not load TikTok status.";
  }

  function getChipLabel(platform, info) {
    const s = getPlatformState(platform, info);
    switch (s) {
      case "connected": return "Connected";
      case "reauth_required": return "Reconnect needed";
      case "missing_config": return "Unavailable";
      case "needs_identity": return "Enter account";
      case "not_connected": return "Ready";
      default: return "Unavailable";
    }
  }

  function getPrimaryLabel(platform, info) {
    const s = getPlatformState(platform, info);
    if (platform === "tiktok") {
      if (s === "connected") return "Reconnect TikTok";
      return "Connect TikTok";
    }
    switch (s) {
      case "connected": return platform === "twitch" ? "Reconnect Twitch" : "Reconnect YouTube";
      case "reauth_required": return platform === "twitch" ? "Reconnect Twitch" : "Reconnect YouTube";
      case "missing_config": return "Unavailable";
      case "not_connected": return platform === "twitch" ? "Connect Twitch" : "Connect YouTube";
      default: return "Retry";
    }
  }

  function renderPlatform(platform) {
    const info = state[platform].info || {};
    const s = getPlatformState(platform, info);
    const primary = getPrimaryButton(platform);

    if (platform === "tiktok") {
      const input = document.getElementById("tiktokConnectUser");
      if (input && !String(input.value || "").trim() && info.unique_id) {
        input.value = `@${info.unique_id}`;
      }

      setTextSafe("tiktokAuthChip", getChipLabel(platform, info));
      setTextSafe("tiktokAuthSummary", getPlatformSummary(platform, info));
      setTextSafe("tiktokAuthIdentity", info.unique_id ? `@${info.unique_id}` : "—");
      setTextSafe("tiktokCookieSession", info.has_sessionid ? "Present" : "Missing");
      setTextSafe("tiktokCookieSessionSs", info.has_sessionid_ss ? "Present" : "Missing");
      setTextSafe("tiktokCookieTargetIdc", info.has_tt_target_idc ? "Present" : "Missing");
      setHidden("tiktokAuthPending", !state.tiktok.polling);

      if (primary) {
        primary.dataset.baseLabel = getPrimaryLabel(platform, info);
        primary.textContent = getPrimaryLabel(platform, info);
        const cleaned = sanitizeTikTok((input && input.value) || "");
        primary.disabled = !!state.tiktok.polling || !cleaned;
      }

      const refresh = getRefreshButton(platform);
      if (refresh) refresh.disabled = !!state.tiktok.polling;
      return;
    }

    const prefix = platform === "twitch" ? "twitch" : "youtube";
    setTextSafe(`${prefix}AuthChip`, getChipLabel(platform, info));
    setTextSafe(`${prefix}AuthSummary`, getPlatformSummary(platform, info));
    setTextSafe(`${prefix}AuthIdentity`, info.connected_login || info.channel_id || "—");
    setTextSafe(`${prefix}AuthScopes`, (info.scopes_readable || "—").trim() || "—");
    setHidden(`${prefix}AuthPending`, !state[platform].polling);

    if (primary) {
      primary.dataset.baseLabel = getPrimaryLabel(platform, info);
      primary.textContent = getPrimaryLabel(platform, info);
      primary.disabled = !!state[platform].polling || s === "missing_config";
      primary.classList.toggle("btn--primary", s !== "missing_config");
      primary.classList.toggle("btn--ghost", s === "missing_config");
    }

    const refresh = getRefreshButton(platform);
    if (refresh) refresh.disabled = !!state[platform].polling;
  }

  async function fetchInfo(platform) {
    const res = await fetch(paths[platform], { cache: "no-store" });
    if (!res.ok) throw new Error(`HTTP ${res.status}`);
    const data = await res.json();
    state[platform].info = data;
    renderPlatform(platform);
    return data;
  }

  async function saveTikTokIdentity() {
    const input = document.getElementById("tiktokConnectUser");
    const uniqueId = sanitizeTikTok((input && input.value) || "");
    if (!input) return "";
    input.value = uniqueId ? `@${uniqueId}` : "";
    if (!uniqueId) return "";
    await apiPost("/api/settings/save", { tiktok_unique_id: uniqueId });
    return uniqueId;
  }

  async function startAuth(platform) {
    const info = state[platform].info || {};
    const platformState = getPlatformState(platform, info);

    if (platform === "tiktok") {
      const uniqueId = await saveTikTokIdentity();
      if (!uniqueId) {
        renderPlatform("tiktok");
        return;
      }
    } else if (platformState === "missing_config") {
      return;
    }

    const primary = getPrimaryButton(platform);
    const startUrl = info.start_url || (platform === "twitch"
      ? "/auth/twitch/start"
      : platform === "youtube"
        ? "/auth/youtube/start"
        : "/auth/tiktok/start");

    setButtonBusy(primary, true, "Opening sign-in…");

    try {
      const openedInApp = openPlatformAuthPopup(startUrl, platform);
      if (!openedInApp) {
        window.open(startUrl, "_blank", "noopener");
      }

      setButtonBusy(primary, false);
      state[platform].polling = window.setInterval(async () => {
        try {
          const latest = await fetchInfo(platform);
          const s = getPlatformState(platform, latest);
          if (s === "connected" || s === "reauth_required" || s === "missing_config") {
            window.clearInterval(state[platform].polling);
            state[platform].polling = null;
            renderPlatform(platform);
          }
        } catch (_) {}
      }, 1500);

      renderPlatform(platform);
    } catch (e) {
      setButtonBusy(primary, false);
      throw e;
    }
  }

  document.getElementById("btnTwitchAuthRefresh")?.addEventListener("click", () => fetchInfo("twitch").catch(() => {}));
  document.getElementById("btnYouTubeAuthRefresh")?.addEventListener("click", () => fetchInfo("youtube").catch(() => {}));
  document.getElementById("btnTikTokAuthRefresh")?.addEventListener("click", () => fetchInfo("tiktok").catch(() => {}));

  document.getElementById("btnTwitchAuthPrimary")?.addEventListener("click", () => startAuth("twitch"));
  document.getElementById("btnYouTubeAuthPrimary")?.addEventListener("click", () => startAuth("youtube"));
  document.getElementById("btnTikTokAuthPrimary")?.addEventListener("click", () => startAuth("tiktok"));

  const tikTokInput = document.getElementById("tiktokConnectUser");
  tikTokInput?.addEventListener("input", () => renderPlatform("tiktok"));
  tikTokInput?.addEventListener("blur", async () => {
    await saveTikTokIdentity().catch(() => {});
    fetchInfo("tiktok").catch(() => {});
  });

  document.addEventListener("rc:tiktok-auth-captured", async () => {
    try {
      const latest = await fetchInfo("tiktok");
      if (state.tiktok.polling) {
        const s = getPlatformState("tiktok", latest);
        if (s === "connected") {
          window.clearInterval(state.tiktok.polling);
          state.tiktok.polling = null;
          renderPlatform("tiktok");
        }
      }
    } catch (_) {}
  });

  fetchInfo("twitch").catch(() => { state.twitch.info = { ok: false }; renderPlatform("twitch"); });
  fetchInfo("youtube").catch(() => { state.youtube.info = { ok: false }; renderPlatform("youtube"); });
  fetchInfo("tiktok").catch(() => { state.tiktok.info = { ok: false }; renderPlatform("tiktok"); });
}


function debounce(fn, ms){
  let t;
  return (...args) => {
    clearTimeout(t);
    t = setTimeout(() => fn(...args), ms);
  };
}

function wireTwitchCategoryLookup(){
    const input = document.getElementById("twitchCategory");
    const results = document.getElementById("twitchCategoryResults");
    const gameId = document.getElementById("twitchGameId");
    const gameIdLabel = document.getElementById("twitchGameIdLabel");
    const setGameIdLabel = () => {
        if (!gameIdLabel) return;
        gameIdLabel.textContent = (gameId.value || "—");
    };
    setGameIdLabel();

  if (!input || !results || !gameId) return;

  const hide = () => { results.style.display = "none"; results.innerHTML = ""; };
  const show = () => { results.style.display = "block"; };

    async function search(q) {
        q = (q || "").trim();

        // Reset selection if user edits text
        gameId.value = "";
        setGameIdLabel();

        if (q.length < 2) {
            hide();
            return;
        }

        try {
            const r = await fetch(`/api/twitch/categories?q=${encodeURIComponent(q)}`, { cache: "no-store" });
            if (!r.ok) throw new Error(`HTTP ${r.status}`);
            const items = await r.json();

            if (!Array.isArray(items) || items.length === 0) {
                hide();
                return;
            }

            // Escape for safe attribute injection
            const esc = (s) => String(s ?? "")
                .replace(/&/g, "&amp;")
                .replace(/"/g, "&quot;")
                .replace(/</g, "&lt;")
                .replace(/>/g, "&gt;");

            results.innerHTML = items.slice(0, 8).map(it => {
                const name = (it && (it.name || it.category_name || it.title)) ? (it.name || it.category_name || it.title) : "";
                const id = (it && (it.id || it.game_id || it.category_id || it.twitch_game_id)) ? (it.id || it.game_id || it.category_id || it.twitch_game_id) : "";

                return `
        <div class="typeahead__item" data-id="${esc(id)}" data-name="${esc(name)}">
          <div>
            <div><strong>${esc(name)}</strong></div>
            <div class="typeahead__meta">Twitch category</div>
          </div>
        </div>
      `;
            }).join("");

            show();
        } catch (e) {
            console.warn("Twitch category lookup failed:", e);
            hide();
        }
    }

  const debounced = debounce(search, 250);

  input.addEventListener("input", () => debounced(input.value));

    // Handle click on a dropdown item
    results.addEventListener("click", (ev) => {
        const el = ev.target.closest(".typeahead__item");
        if (!el) return;

        input.value = el.getAttribute("data-name") || "";
        gameId.value = el.getAttribute("data-id") || "";
        setGameIdLabel();
        hide();
    });

  // Hide dropdown when clicking outside
  document.addEventListener("click", (ev) => {
    if (ev.target === input || results.contains(ev.target)) return;
    hide();
  });
}

// Call it on DOMContentLoaded (safe)
document.addEventListener("DOMContentLoaded", () => {
  wireTwitchCategoryLookup();
});

async function loadYouTubeVodDraft() {
  const t = document.getElementById("youtubeVodTitle");
  const d = document.getElementById("youtubeVodDescription");
  if (!t || !d) return;

  try {
    const res = await fetch("/api/youtube/vod/draft", { cache: "no-store" });
    if (!res.ok) return;
    const j = await res.json();
    if (!j || !j.ok) return;

    // Only populate if empty or readonly (don't stomp user edits)
    if (!t.value) t.value = j.title || "";
    if (!d.value) d.value = j.description || "";
  } catch {}
}

async function saveYouTubeVodDraft() {
  const t = document.getElementById("youtubeVodTitle");
  const d = document.getElementById("youtubeVodDescription");
  if (!t || !d) return;

  const payload = { title: t.value || "", description: d.value || "" };
  try {
    await fetch("/api/youtube/vod/draft", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(payload)
    });
  } catch {}
}

async function applyYouTubeVod() {
  const btn = document.getElementById("btnApplyYouTubeVod");
  const nodes = document.querySelectorAll("#ytAuthStatusText");
  const setStatus = (txt) => nodes.forEach(n => n.textContent = txt);

  if (btn) btn.disabled = true;
  setStatus("Applying…");

  try {
    const res = await fetch("/api/youtube/vod/apply", { method: "POST" });
    const j = await res.json().catch(() => ({}));
    if (res.ok && j && j.ok) {
      setStatus("Connected");
    } else {
      setStatus("Apply failed");
      console.log("YouTube VOD apply failed", j);
    }
  } catch (e) {
    setStatus("Apply failed");
  } finally {
    if (btn) btn.disabled = false;
  }
}

document.addEventListener("DOMContentLoaded", () => {
  const t = document.getElementById("youtubeVodTitle");
  const d = document.getElementById("youtubeVodDescription");
  const b = document.getElementById("btnApplyYouTubeVod");
  if (t) t.addEventListener("blur", saveYouTubeVodDraft);
  if (d) d.addEventListener("blur", saveYouTubeVodDraft);
  if (b) b.addEventListener("click", (e) => { e.preventDefault(); applyYouTubeVod(); });
});
