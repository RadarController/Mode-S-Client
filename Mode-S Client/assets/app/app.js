
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
  if (!r.ok) throw new Error(`${r.status} ${r.statusText}`);
  const text = await r.text();
  try { return JSON.parse(text); } catch { return { ok:true, raw:text }; }
}

async function loadSettings(){
  try{
    const j = await apiGet("/api/settings");
    if (!j || !j.ok) return;
    if ($("#tiktokUser"))  $("#tiktokUser").value  = j.tiktok_unique_id || "";
    if ($("#twitchUser"))  $("#twitchUser").value  = j.twitch_login || "";
    if ($("#youtubeUser")) $("#youtubeUser").value = j.youtube_handle || "";
    logLine("settings", "loaded");
  }catch(e){
    logLine("settings", `load failed (${e.message})`);
  }
}

async function saveSettings(){
  const payload = {
    tiktok_unique_id: $("#tiktokUser")?.value || "",
    twitch_login: $("#twitchUser")?.value || "",
    youtube_handle: $("#youtubeUser")?.value || ""
  };
  try{
    // support both endpoints
    try{
      await apiPost("/api/settings/save", payload);
    }catch(_){
      await apiPost("/api/settingssave", payload);
    }
    logLine("settings", "saved");
    await loadSettings();
    return true;
  }catch(e){
    logLine("settings", `save failed (${e.message})`);
    return false;
  }
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

function guessMetricsShape(m){
  // We intentionally support multiple possible shapes, since the project evolved.
  // Top-level convenience
  const viewers = m.viewers ?? m.viewer_count ?? m.live_viewers ?? m.concurrent_viewers;
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
      (s.tiktok && (s.tiktok.live || s.tiktok.is_live)) ||
      (s.twitch && (s.twitch.live || s.twitch.is_live)) ||
      (s.youtube && (s.youtube.live || s.youtube.is_live))
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

    // stop button enabled when "live" (best effort)
    setStopEnabled(name, live);
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
    const ver = m.version || (m.app && m.app.version) || null;
    const port = m.port || (m.http && m.http.port) || null;
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
    window.open("/overlay/chat.html", "_blank", "noopener");
  });

  $("#btnSave")?.addEventListener("click", async () => {
    await saveSettings();
  });

  $$(".platform").forEach(card => {
    const platform = card.dataset.platform;
    card.addEventListener("click", async (ev) => {
      const btn = ev.target.closest("button[data-action]");
      if (!btn) return;

      const action = btn.getAttribute("data-action");
      
if (action === "set"){
  const ok = await saveSettings();
  if (ok) logLine(platform, "config saved");
  return;
}
if (action === "start"){
        try{
          await apiPost(`/api/platform/${platform}/start`);
          logLine(platform, "start requested");
        }catch(e){
          logLine(platform, `start failed (${e.message})`);
        }
        return;
      }

      if (action === "stop"){
        try{
          await apiPost(`/api/platform/${platform}/stop`);
          logLine(platform, "stop requested");
        }catch(e){
          logLine(platform, `stop failed (${e.message})`);
        }
        return;
      }
    });
  });
}

wireActions();
loadSettings();

pollMetrics();
setInterval(pollMetrics, 2000);
pollLog();
setInterval(pollLog, 1000);
