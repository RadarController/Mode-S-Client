// /assets/mergedChatFeed.js
// Shared merge logic for chat + EventSub events.
// Consumers provide small hooks (fetchChat, extractChatItems, normalizeChatItem, normalizeEventItem, remember, addLine).
(function () {
  "use strict";

  function buildEventIndex(eventNorms) {
    const map = new Map();
    for (const e of (eventNorms || [])) {
      const p = (e.platform ? e.platform : "").toLowerCase();
      const u = (e.user ? e.user : "").toLowerCase();
      const t = (e.text ? e.text : "").toLowerCase();
      const key = p + "|" + u + "|" + t;
      const arr = map.get(key) || [];
      arr.push(Number(e.ts) || 0);
      map.set(key, arr);
    }
    for (const [k, arr] of map.entries()) arr.sort((a, b) => a - b);
    return map;
  }

  function hasNearTimestamp(sortedArr, ts, windowMs) {
    if (!sortedArr || !sortedArr.length) return false;
    const lo = ts - windowMs, hi = ts + windowMs;
    let l = 0, r = sortedArr.length - 1;
    while (l <= r) {
      const mid = (l + r) >> 1;
      const v = sortedArr[mid];
      if (v < lo) l = mid + 1;
      else if (v > hi) r = mid - 1;
      else return true;
    }
    return false;
  }

  function isChatDuplicateOfEvent(chatNorm, eventIndex, windowMs) {
    if (!chatNorm || chatNorm.is_event) return false;
    if ((chatNorm.platform || "").toLowerCase() !== "twitch") return false; // only suppress for Twitch
    const p = (chatNorm.platform ? chatNorm.platform : "").toLowerCase();
    const u = (chatNorm.user ? chatNorm.user : "").toLowerCase();
    const t = (chatNorm.text ? chatNorm.text : "").toLowerCase();
    const key = p + "|" + u + "|" + t;
    const arr = eventIndex.get(key);
    if (!arr) return false;
    const ts = Number(chatNorm.ts) || 0;
    return hasNearTimestamp(arr, ts, windowMs);
  }

  async function safeCall(fn, fallback) {
    try { return await fn(); } catch (_) { return fallback; }
  }

  async function tick(cfg) {
    const {
      fetchChat,
      fetchEvents,
      extractChatItems,
      normalizeChatItem,
      normalizeEventItem,
      remember,
      addLine,
      onError,
      maxItems = 200,
      dedupeWindowMs = 5000
    } = cfg;

    try {
      const [chatJson, eventsJson] = await Promise.all([
        safeCall(fetchChat, null),
        safeCall(fetchEvents, null)
      ]);

      const chatItemsRaw = extractChatItems ? (extractChatItems(chatJson) || []) : [];
      const eventsRaw = (eventsJson && eventsJson.events) ? eventsJson.events : (eventsJson || []);

      // Normalize events first (needed for duplicate suppression)
      const eventNorms = [];
      for (const e of eventsRaw) {
        const n = normalizeEventItem(e);
        if (!n || !n.text) continue;
        n.is_event = true;
        eventNorms.push(n);
      }
      const eventIndex = buildEventIndex(eventNorms);

      const merged = [];

      for (const raw of chatItemsRaw) {
        const n = normalizeChatItem(raw);
        if (!n || !n.text) continue;
        n.is_event = !!n.is_event; // preserve if caller sets it
        if (isChatDuplicateOfEvent(n, eventIndex, dedupeWindowMs)) continue;
        merged.push(n);
      }

      for (const n of eventNorms) merged.push(n);

      merged.sort((a, b) => (Number(a.ts) || 0) - (Number(b.ts) || 0));

      const slice = merged.slice(-maxItems);
      for (const n of slice) {
        if (!n.id) {
          // Best-effort stable id
          n.id = `${n.is_event ? "ev" : "ch"}:${(n.platform || "")}:${(n.user || "")}:${(n.ts || 0)}:${(n.text || "")}`.slice(0, 512);
        }
        if (remember(n.id)) addLine(n);
      }
    } catch (e) {
      if (onError) onError(e);
    }
  }

  function start(cfg) {
    if (!cfg) throw new Error("MergedChatFeed.start: missing cfg");
    const intervalMs = cfg.intervalMs || 1000;
    const run = () => tick(cfg);
    // run immediately
    run();
    return setInterval(run, intervalMs);
  }

  window.MergedChatFeed = { start };
})();
