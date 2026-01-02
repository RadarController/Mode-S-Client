// assets/app/app.js
// Modern UI glue for Mode-S Client
// - Loads settings from GET /api/settings and populates fields
// - Saves settings via POST /api/settings/save (fallback /api/settingssave)
// - Start/Stop via POST /api/platform/<platform>/<action>
// - Polls metrics from GET /api/metrics and updates UI
// NOTE: This file is designed to match the IDs/structure in assets/app/index.html.

const $  = (sel, root=document) => root.querySelector(sel);
const $$ = (sel, root=document) => Array.from(root.querySelectorAll(sel));

function fmtNum(n){
  if (n === null || n === undefined) return "—";
  const x = Number(n);
  if (!Number.isFinite(x)) return "—";
  return x.toLocaleString();
}
function setText(id, v){
  const el = document.getElementById(id);
  if (el) el.textContent = (v === undefined || v === null || v === "") ? "—" : String(v);
}

async function apiGet(url){
  const r = await fetch(url, { cache: "no-store" });
  if (!r.ok) throw new Error(`${r.status} ${r.statusText}`);
  return await r.json();
}

async function apiPost(url, body){
  const r = await fetch(url, {
    method: "POST",
    headers: { "Content-Type":"application/json" },
    body: body ? JSON.stringify(body) : "{}"
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

function pickBucket(rawBucket){
  // Some builds nest under {stats:{...}} or {metrics:{...}}
  if (!rawBucket) return null;
  if (rawBucket.stats && typeof rawBucket.stats === "object") return rawBucket.stats;
  if (rawBucket.metrics && typeof rawBucket.metrics === "object") return rawBucket.metrics;
  return rawBucket;
}

function guessMetricsShape(m0){
  const m = unwrapPayload(m0) || {};

  // Top-level convenience
  const viewers = m.viewers ?? m.viewer_count ?? m.live_viewers ?? m.concurrent_viewers;
  const followers = m.followers ?? m.follower_count ?? m.subscribers ?? m.subscriber_count;
  const aircraft = m.aircraft ?? m.aircraft_count ?? m.euroscope_aircraft ?? m.aircrafts;

  // Per-platform buckets (common patterns: m.tiktok, m.platforms.tiktok, m.metrics.tiktok)
  const getP = (p) => pickBucket(m[p] || (m.platforms && m.platforms[p]) || (m.metrics && m.metrics[p]) || null);

  return {
    viewers, followers, aircraft,
    tiktok: getP("tiktok"),
    twitch: getP("twitch"),
    youtube: getP("youtube"),
    raw: m
  };
}

function isConnected(bucket){
  if (!bucket) return false;
  return Boolean(
    bucket.connected ?? bucket.running ?? bucket.is_connected ?? bucket.chat_connected ?? bucket.irc_connected ?? false
  );
}

function isLive(bucket){
  if (!bucket) return false;
  return Boolean(bucket.live ?? bucket.is_live ?? bucket.streaming ?? bucket.on_air ?? false);
}

function applyMetrics(m){
  const s = guessMetricsShape(m);

  // Overall stats: if top-level is missing, derive from per-platform where possible
  const overallViewers = s.viewers ??
    (s.tiktok && (s.tiktok.viewers ?? s.tiktok.viewer_count ?? s.tiktok.concurrent_viewers)) ??
    (s.twitch && (s.twitch.viewers ?? s.twitch.viewer_count ?? s.twitch.concurrent_viewers)) ??
    (s.youtube && (s.youtube.viewers ?? s.youtube.viewer_count ?? s.youtube.concurrent_viewers));
  const overallFollowers = s.followers ??
    (s.tiktok && (s.tiktok.followers ?? s.tiktok.follower_count)) ??
    (s.twitch && (s.twitch.followers ?? s.twitch.follower_count)) ??
    (s.youtube && (s.youtube.subscribers ?? s.youtube.subscriber_count ?? s.youtube.followers ?? s.youtube.follower_count));

  setText("mViewers", fmtNum(overallViewers));
  setText("mFollowers", fmtNum(overallFollowers));
  setText("mAircraft", fmtNum(s.aircraft));

  const dot = $("#mDot");
  if (dot){
    const anyLive = Boolean(isLive(s.tiktok) || isLive(s.twitch) || isLive(s.youtube));
    dot.style.background = anyLive ? "#3b82f6" : "#64748b";
  }

  const applyPlatform = (name, bucket) => {
    if (!bucket) return;

    // live vs connected: Twitch can be "connected" even if not live (chat polling active)
    const live = isLive(bucket);
    const connected = isConnected(bucket) || live;

    setBadge(name, live, live ? "Live" : (connected ? "Online" : "Offline"));
    setState(name, connected ? "Connected" : "Disconnected");

    // stats
    const v = bucket.viewers ?? bucket.viewer_count ?? bucket.live_viewers ?? bucket.concurrent_viewers ?? bucket.user_count;
    const f = bucket.followers ?? bucket.follower_count ?? bucket.subscribers ?? bucket.subscriber_count;
    setText(`${name}Viewers`, fmtNum(v));
    setText(`${name}Followers`, fmtNum(f));

    // enable stop when connected (not only when live)
    setStopEnabled(name, connected);
  };

  applyPlatform("tiktok", s.tiktok);
  applyPlatform("twitch", s.twitch);
  applyPlatform("youtube", s.youtube);

  const status = $("#mStatus");
  if (status){
    const anyLive = Boolean(isLive(s.tiktok) || isLive(s.twitch) || isLive(s.youtube));
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

async function pollDiag(){
  // Heartbeat + useful debug info
  try{
    const d = await apiGet("/api/chat/diag");
    if (d) logLine("diag", `chat=${d.count ?? "?"} state=${d.state_count ?? "?"}`);
  }catch(e){
    // ignore
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
  const payload = {
    tiktok_unique_id: $("#tiktokUser")?.value || "",
    twitch_login: $("#twitchUser")?.value || "",
    youtube_handle: $("#youtubeUser")?.value || "",
    ...(partial || {})
  };

  try{
    const r = await apiPost("/api/settings/save", payload);
    logLine("settings", (r && r.path) ? `saved (${r.path})` : "saved");
    await loadSettings();
    return true;
  }catch(e1){
    try{
      const r = await apiPost("/api/settingssave", payload);
      logLine("settings", (r && r.path) ? `saved (${r.path})` : "saved");
      await loadSettings();
      return true;
    }catch(e2){
      logLine("settings", `save failed (${e2.message})`);
      return false;
    }
  }
}

async function platformAction(platform, action){
  try{
    const r = await apiPost(`/api/platform/${platform}/${action}`, {});
    // Expect {ok:true,state:"started"} but tolerate anything.
    if (r && r.ok === false) {
      logLine(platform, `${action} failed (${r.error || "ok=false"})`);
    } else if (r && r.state) {
      logLine(platform, `${action} ${r.state}`);
    } else {
      logLine(platform, `${action} ok`);
    }
    // refresh metrics sooner after actions
    setTimeout(pollMetrics, 250);
    setTimeout(pollMetrics, 1000);
  }catch(e){
    logLine(platform, `${action} failed (${e.message})`);
  }
}

function wireActions(){
  $("#btnOpenChat")?.addEventListener("click", () => {
    window.open("/overlay/chat.html", "_blank", "noopener");
  });

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
  setInterval(pollDiag, 6000);
});
