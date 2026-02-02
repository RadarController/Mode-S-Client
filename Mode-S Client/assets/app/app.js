
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

async function saveSettingsFromInputs(){
  const payload = {
    tiktok_unique_id: $("#tiktokUser")?.value || "",
    twitch_login: $("#twitchUser")?.value || "",
    youtube_handle: $("#youtubeUser")?.value || ""
  };

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
    const ver = m.version || (m.app && m.app.version) || window.__APP_BUILDINFO || null;
    const port = m.port || (m.http && m.http.port) || null;
    const parts = [];
    if (ver) parts.push(`${ver}`);
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
        // In-app (WebView2): ask the native host to open the floating chat window.
        if (window.chrome?.webview?.postMessage) {
            window.chrome.webview.postMessage({ type: "open_chat" });
            return;
        }

        // Fallback if /app is opened in a normal browser:
        window.open("/overlay/chat.html", "_blank", "noopener");
    });

   $("#btnOpenSettings")?.addEventListener("click", () => {
     window.location.href = "/app/settings.html";
   });

  $("#btnOpenBot")?.addEventListener("click", () => {
    // Navigate within the same WebView/app window.
    window.location.href = "/app/bot.html";
  });

  $("#btnStartAll")?.addEventListener("click", async () => {
    logLine("start-all", "starting all platforms");
    const platforms = ["tiktok", "twitch", "youtube"];
    for (const p of platforms) {
      try {
        await apiPost(`/api/platform/${p}/start`, {});
        logLine("start-all", `${p} start requested`);
      } catch (e) {
        logLine("start-all", `${p} start failed (${e.message})`);
      }
    }
    logLine("start-all", "done");
  });


  $("#btnSave")?.addEventListener("click", async () => {
    await saveSettingsFromInputs();
  });

  $$(".platform").forEach(card => {
    const platform = card.dataset.platform;
    card.addEventListener("click", async (ev) => {
      const btn = ev.target.closest("button[data-action]");
      if (!btn) return;

      const action = btn.getAttribute("data-action");
      if (action === "set"){
        // For now: SET just saves the current usernames via /api/settings/save.
        await saveSettingsFromInputs();
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

document.addEventListener("DOMContentLoaded", async () => {
  wireSmartBackButtons();
  wireSettingsHubPage();
  wireTwitchStreamInfoPage();
  wireTwitchOAuthPage();
  wireOverlayTitlePage();
  wireActions();
  await loadSettings();
  await pollMetrics();
  setInterval(pollMetrics, 2000);
  await pollLog();
  setInterval(pollLog, 1000);
});

function wireOverlayTitlePage() {
    // overlay_title.html has these ids; if they exist, we are on that page.
    const elTitle = document.getElementById("overlayTitle");
    const elSub = document.getElementById("overlaySubtitle");
    const elSave = document.getElementById("btnSaveOverlayTitle");
    const elStatus = document.getElementById("overlayTitleStatus");

    if (!elTitle || !elSub || !elSave) return;

    const setStatus = (t) => { if (elStatus) elStatus.textContent = t || ""; };

    async function load() {
        setStatus("Loading…");
        try {
            const j = await apiGet("/api/overlay/header");
            // Expecting: { title: "...", subtitle: "..." }
            elTitle.value = (j && typeof j.title === "string") ? j.title : "";
            elSub.value = (j && typeof j.subtitle === "string") ? j.subtitle : "";
            setStatus("");
        } catch (e) {
            console.warn("Failed to load overlay header", e);
            setStatus("Could not load current header");
        }
    }

    async function save() {
        setStatus("Saving…");
        const body = { title: elTitle.value || "", subtitle: elSub.value || "" };

        // Prefer the same route the overlay reads from.
        // If your backend expects a different POST route, this fallback keeps it resilient.
        try {
            const j = await apiPost("/api/overlay/header", body);
            if (j && j.ok === false) throw new Error(j.error || "ok=false");
            setStatus("Saved");
            return true;
        } catch (e1) {
            try {
                // Fallback for older builds if you named it differently
                const j = await apiPost("/api/overlay/header/save", body);
                if (j && j.ok === false) throw new Error(j.error || "ok=false");
                setStatus("Saved");
                return true;
            } catch (e2) {
                console.error("Overlay header save failed", e1, e2);
                setStatus("Error saving");
                return false;
            }
        }
    }

    elSave.addEventListener("click", save);

    // Optional QoL: Ctrl+Enter to save from an input
    [elTitle, elSub].forEach(el => {
        el.addEventListener("keydown", (ev) => {
            if ((ev.ctrlKey || ev.metaKey) && ev.key === "Enter") save();
        });
    });

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
    window.location.href = "/app/stream_details.html";
  });
}

// Smart back routing: if user came from Settings hub, go back there.
function wireSmartBackButtons(){
  function target(){
    try{
      const ref = document.referrer || "";
      if (ref.includes("/app/settings.html")) return "/app/settings.html";
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
        // YouTube (Phase 2): mirror stream fields into the YouTube VOD inputs on the same page.
    // UI-only for now (no backend apply yet).
    const ytVodTitle = document.getElementById("youtubeVodTitle");
    const ytVodDesc  = document.getElementById("youtubeVodDescription");

    const mirrorToYouTubeVod = () => {
      const title = elTitle ? (elTitle.value || "") : "";
      const desc  = elDesc  ? (elDesc.value  || "") : "";
      if (ytVodTitle) ytVodTitle.value = title;
      if (ytVodDesc)  ytVodDesc.value  = desc;
    };

    elTitle?.addEventListener("input", mirrorToYouTubeVod);
    elDesc?.addEventListener("input", mirrorToYouTubeVod);
const setStatus = (t)=>{ if(elStatus) elStatus.textContent = t||""; };

  async function load(){
    try{
      const j = await apiGet("/api/twitch/streaminfo");
      if(elTitle) elTitle.value = j.title || "";
      if(elCat) elCat.value = j.category || "";
      if(elDesc) elDesc.value = j.description || "";
      // Keep YouTube VOD fields in sync with loaded draft
      mirrorToYouTubeVod();
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
      mirrorToYouTubeVod();
      setStatus("Saved");
      return true;
    }catch(e){
      console.error(e);
      setStatus("Error saving");
      return false;
    }
  }

    async function apply() {    setStatus("Updating stream details…");
    try{
      const ok = await save();
      if(!ok) return;
      const j = await apiPost("/api/twitch/streaminfo/apply");
      if(j && j.ok === false) throw new Error(j.error || "ok=false");
      setStatus("Twitch: Updated • YouTube: Not enabled yet");
    }catch(e){
      console.error(e);
      setStatus("Error updating Twitch");
    }
  }

  elSave?.addEventListener("click", save);
  elApply?.addEventListener("click", apply);
  load();
}


function wireTwitchOAuthPage(){
  const root = document.getElementById("twitchOAuthPage");
  if(!root) return;

  const elScopes = document.getElementById("twitchOAuthScopes");
  const elStart = document.getElementById("btnTwitchOAuthStart");
  const elCopy = document.getElementById("btnTwitchOAuthCopy");
  const elStatus = document.getElementById("twitchOAuthStatus");

  const setStatus = (t)=>{ if(elStatus) elStatus.textContent = t || ""; };

  async function load(){
    try{
      const j = await apiGet("/api/twitch/auth/info");
      if(elScopes) elScopes.textContent = (j.scopes_readable || "").trim();

      if(j.oauth_routes_wired === false){
        setStatus("OAuth routes are not wired in this build.");
        elStart && (elStart.disabled = true);
        elCopy && (elCopy.disabled = true);
        return;
      }

      const startUrl = j.start_url || "/auth/twitch/start";
      const abs = `${window.location.origin}${startUrl}`;

      elStart?.addEventListener("click", () => {
        window.open(startUrl, "_blank", "noopener");
        setStatus("Opened OAuth flow in a new tab.");
      });

      elCopy?.addEventListener("click", async () => {
        try{
          await navigator.clipboard.writeText(abs);
          setStatus("Copied start URL.");
        }catch(e){
          // Fallback
          const ta = document.createElement("textarea");
          ta.value = abs;
          document.body.appendChild(ta);
          ta.select();
          document.execCommand("copy");
          document.body.removeChild(ta);
          setStatus("Copied start URL.");
        }
      });

      setStatus("");
    }catch(e){
      console.warn("Failed to load Twitch OAuth info", e);
      setStatus("Could not load OAuth info.");
    }
  }

  load();
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