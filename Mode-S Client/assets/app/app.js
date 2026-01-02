// assets/app/app.js
// Modern UI glue for Mode-S Client
// - Loads settings from GET /api/settings and populates fields
// - Saves settings via POST /api/settings/save (fallback /api/settingssave)
// - Start/Stop via POST /api/platform/<platform>/<action> (with a few fallbacks)
// - Polls metrics from GET /api/metrics and updates UI (supports multiple payload shapes)

const $ = (sel, root=document) => root.querySelector(sel);
const $$ = (sel, root=document) => Array.from(root.querySelectorAll(sel));

function fmtNum(n){
  if (n === null || n === undefined) return "—";
  const x = Number(n);
  if (!Number.isFinite(x)) return "—";
  return x.toLocaleString();
}
function setText(id, v){ const el = document.getElementById(id); if (el) el.textContent = v; }

async function apiGet(url){
  const r = await fetch(url, { cache: "no-store" });
  if (!r.ok) throw new Error(`${r.status} ${r.statusText}`);
  return await r.json();
}

async function apiPost(url, body){
  const r = await fetch(url, {
    method: "POST",
    headers: { "Content-Type":"application/json" },
    body: body ? JSON.stringify(body) : ""
  });
  if (!r.ok) throw new Error(`${r.status} ${r.statusText}`);
  const text = await r.text();
  try { return JSON.parse(text); } catch { return { ok:true, raw:text }; }
}

function logLine(tag, msg){
  const log = $("#log");
  const ts = new Date().toLocaleTimeString();
  const line = document.createElement("div");
  line.className = "log__line";
  line.innerHTML = `<span class="log__ts">[${ts}]</span><span class="log__tag">${tag}</span><span class="log__msg"></span>`;
  line.querySelector(".log__msg").textContent = msg;
  if (log) log.prepend(line);
  else console.log(`[${ts}]${tag} ${msg}`);
}

function setBadge(platform, live, label){
  const b = document.getElementById(`${platform}Badge`);
  if (!b) return;
  b.textContent = label || (live ? "Live" : "Offline");
  b.classList.toggle("badge--live", !!live);
}

function setState(platform, text){
  const el = document.getElementById(`${platform}State`);
  if (el) el.textContent = text;
}

function setStopEnabled(platform, enabled){
  const card = document.querySelector(`.platform[data-platform="${platform}"]`);
  if (!card) return;
  const stop = card.querySelector(`button[data-action="stop"]`);
  if (stop) stop.disabled = !enabled;
}

function unwrapPayload(m){
  // Support {ok:true, data:{...}} as well as plain objects
  if (m && typeof m === "object" && "data" in m && (m.ok === true || m.ok === undefined)) {
    if (m.data && typeof m.data === "object") return m.data;
  }
  return m;
}

function guessMetricsShape(m0){
  const m = unwrapPayload(m0) || {};

  // Top-level convenience
  const viewers = m.viewers ?? m.viewer_count ?? m.live_viewers ?? m.concurrent_viewers
               ?? (m.tiktok && (m.tiktok.viewers ?? m.tiktok.viewer_count ?? m.tiktok.concurrent_viewers))
               ?? (m.platforms && m.platforms.tiktok && (m.platforms.tiktok.viewers ?? m.platforms.tiktok.viewer_count));
  const followers = m.followers ?? m.follower_count ?? m.subscribers ?? m.subscriber_count;
  const aircraft = m.aircraft ?? m.aircraft_count ?? m.euroscope_aircraft ?? m.aircrafts;

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
  const s = guessMetricsShape(m);

  setText("mViewers", fmtNum(s.viewers));
  setText("mFollowers", fmtNum(s.followers));
  setText("mAircraft", fmtNum(s.aircraft));

  // status dot (best effort)
  const dot = $("#mDot");
  if (dot){
    const liveAny = Boolean(
      (s.tiktok && (s.tiktok.live || s.tiktok.is_live || s.tiktok.streaming)) ||
      (s.twitch && (s.twitch.live || s.twitch.is_live || s.twitch.streaming)) ||
      (s.youtube && (s.youtube.live || s.youtube.is_live || s.youtube.streaming))
    );
    dot.style.background = liveAny ? "#3b82f6" : "#64748b";
  }

  const applyPlatform = (name, bucket) => {
    if (!bucket) return;
    const live = Boolean(bucket.live ?? bucket.is_live ?? bucket.streaming ?? bucket.connected ?? false);
    setBadge(name, live, live ? "Live" : "Offline");
    setState(name, live ? "Connected" : "Disconnected");

    // stats
    const v = bucket.viewers ?? bucket.viewer_count ?? bucket.live_viewers ?? bucket.concurrent_viewers;
    const f = bucket.followers ?? bucket.follower_count ?? bucket.subscribers ?? bucket.subscriber_count;
    setText(`${name}Viewers`, fmtNum(v));
    setText(`${name}Followers`, fmtNum(f));

    setStopEnabled(name, live);
  };

  applyPlatform("tiktok", s.tiktok);
  applyPlatform("twitch", s.twitch);
  applyPlatform("youtube", s.youtube);

  const status = $("#mStatus");
  if (status){
    const anyLive = Boolean(
      (s.tiktok && (s.tiktok.live || s.tiktok.is_live || s.tiktok.streaming)) ||
      (s.twitch && (s.twitch.live || s.twitch.is_live || s.twitch.streaming)) ||
      (s.youtube && (s.youtube.live || s.youtube.is_live || s.youtube.streaming))
    );
    status.textContent = anyLive ? "Live" : "Idle";
  }

  const build = $("#buildInfo");
  if (build){
    const mm = unwrapPayload(m) || {};
    const ver = mm.version || (mm.app && mm.app.version) || null;
    const port = mm.port || (mm.http && mm.http.port) || null;
    const parts = [];
    if (ver) parts.push(`v${ver}`);
    if (port) parts.push(`:${port}`);
    build.textContent = parts.length ? parts.join(" ") : "—";
  }
}

async function pollMetrics(){
  try{
    const m = await apiGet("/api/metrics");
    applyMetrics(m);
  }catch(e){
    console.debug("metrics poll failed", e);
  }
}

async function loadSettings(){
  try{
    const s = await apiGet("/api/settings");
    if (!s || s.ok !== true) throw new Error("ok=false");
    const t = $("#tiktokUser"); if (t) t.value = s.tiktok_unique_id ?? "";
    const tw = $("#twitchUser"); if (tw) tw.value = s.twitch_login ?? "";
    const y = $("#youtubeUser"); if (y) y.value = s.youtube_handle ?? "";
    logLine("settings", `loaded (${s.config_path || "unknown path"})`);
  }catch(e){
    logLine("settings", `load failed (${e.message})`);
  }
}

async function saveSettings(partial){
  // partial can include one or more fields; we still include current values for completeness
  const payload = {
    tiktok_unique_id: $("#tiktokUser")?.value || "",
    twitch_login: $("#twitchUser")?.value || "",
    youtube_handle: $("#youtubeUser")?.value || "",
    ...(partial || {})
  };

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

async function postWithFallbacks(urls, body){
  let lastErr = null;
  for (const u of urls){
    try { return await apiPost(u, body); }
    catch(e){ lastErr = e; }
  }
  throw lastErr || new Error("request failed");
}

async function platformAction(platform, action){
  // Best-effort fallbacks to support older route naming if present.
  const urls = [
    `/api/platform/${platform}/${action}`,
    `/api/${platform}/${action}`,
    `/api/${platform}${action}` // e.g. /api/tiktokstart
  ];
  try{
    await postWithFallbacks(urls, {});
    logLine(platform, `${action} requested`);
  }catch(e){
    logLine(platform, `${action} failed (${e.message})`);
  }
}

function wireActions(){
  // Open Chat
  $("#btnOpenChat")?.addEventListener("click", () => {
    window.open("/overlay/chat.html", "_blank", "noopener");
  });

  // Save Settings (top bar)
  $("#btnSave")?.addEventListener("click", async () => {
    await saveSettings();
  });

  // Platform card actions (event delegation)
  $$(".platform").forEach(card => {
    const platform = card.dataset.platform;
    card.addEventListener("click", async (ev) => {
      const btn = ev.target.closest("button[data-action]");
      if (!btn) return;

      const action = btn.getAttribute("data-action");

      if (action === "set"){
        // Save only this platform field (but payload includes all fields too)
        const input = card.querySelector("input");
        const val = input?.value || "";
        if (platform === "tiktok") await saveSettings({ tiktok_unique_id: val });
        else if (platform === "twitch") await saveSettings({ twitch_login: val });
        else if (platform === "youtube") await saveSettings({ youtube_handle: val });
        else await saveSettings();
        return;
      }

      if (action === "start"){ await platformAction(platform, "start"); return; }
      if (action === "stop"){ await platformAction(platform, "stop"); return; }
    });
  });
}

document.addEventListener("DOMContentLoaded", async () => {
  wireActions();
  await loadSettings();
  await pollMetrics();
  setInterval(pollMetrics, 2000);
});
