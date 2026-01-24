**RadarController Mode-S Client** is a Windows desktop application built to power modern, data-driven ATC livestreams.  
It acts as a **real-time aggregation, automation, and overlay engine** for aviation-themed streaming — combining chat, metrics, events, and external data sources into a single, extensible client.

This project underpins the **RadarController** streaming ecosystem and the wider **StreamingATC.Live** platform.

---

## 🚦 What Is This?

The Mode-S Client is designed for **VATSIM / flight-simulation ATC streamers** who want:

- One place to handle **Twitch, TikTok, YouTube** chat
- Reliable **real-time overlays** for OBS
- A configurable **chatbot & command system**
- Live **aviation and stream metrics**
- Event-driven automation (follows, subs, gifts, alerts)
- A clean foundation for future integrations (Discord, web, APIs)

It is **not** a simple chatbot — it is a **stream control plane**.

---

## 🧠 Core Capabilities

### 💬 Multi-Platform Chat
- Twitch IRC integration
- Twitch Helix + EventSub support
- TikTok Live chat integration
- YouTube Live Chat support
- Unified internal chat model
- Per-platform metadata (badges, subs, gifts, etc.)

### 🤖 Chatbot & Commands
- JSON-driven command system
- Platform-aware responses
- Per-user & per-command throttling
- Test injection endpoint (`/api/bot/test`)
- Designed for future **live editing via UI**

### 📊 Metrics & Data
- Viewer counts
- Followers / subscribers / gifts
- Stream uptime
- Aviation-specific data (ATC context, frequency, aircraft counts, etc.)
- Exposed via internal HTTP API

### 🎥 Stream Overlays
- Built-in HTTP server (localhost)
- Overlay endpoints for OBS browser sources
- Lightweight HTML/JS overlays
- Stable fixed-height layouts for broadcast use
- Designed to avoid flicker and refresh artifacts

### 🔐 Authentication & Tokens
- Secure Twitch OAuth handling
- Silent token refresh on startup
- Automatic token persistence
- Separation of IRC and Helix/EventSub auth

### 🧩 Extensible Architecture
- Modular integrations (`integrations/`)
- Clean separation between:
  - Auth
  - Chat ingestion
  - Events
  - Bot logic
  - HTTP API
- Designed to grow without rewrites

---

## 🏗️ Architecture Overview

```
┌───────────────────────────┐
│       Mode-S Client       │
├───────────────────────────┤
│                           │
│  Integrations             │
│  ├─ Twitch (IRC, Helix)   │
│  ├─ TikTok Live           │
│  ├─ YouTube Live          │
│                           │
│  Core                     │
│  ├─ Chat Aggregator       │
│  ├─ Bot Engine            │
│  ├─ Metrics Engine        │
│  ├─ Event Router          │
│                           │
│  HTTP Server              │
│  ├─ /api/*                │
│  ├─ /overlay/*            │
│                           │
│  UI / WebView2            │
│                           │
└───────────────────────────┘
```

---

## 🖥️ Platform & Tech

- **Windows Desktop**
- **C++ (Visual Studio 2022, 2026)**
- WebView2 for embedded UI
- Built-in HTTP server
- JSON configuration & schemas
- External libraries:
  - `nlohmann::json`
  - `cpp-httplib`
  - WinHTTP / Win32 APIs

---

## 📁 Project Structure (High Level)

```
Mode-S Client/
├─ src/
│  ├─ Mode-S Client.cpp
│  ├─ AppState.*
│  ├─ HTTPServer.*
│
├─ integrations/
│  ├─ twitch/
│  ├─ tiktok/
│  ├─ youtube/
│
├─ overlay/
│  ├─ common/
│  ├─ chat.html
│  ├─ onfrequency.html
│
├─ config/
│  ├─ config.json
│  ├─ bot_commands.json
│  ├─ bot_settings.json
│
└─ assets/
```

---

## ⚙️ Configuration

All runtime configuration is file-based:

- `config.json`  
  Core app settings, ports, platform credentials

- `bot_commands.json`  
  Command definitions, triggers, responses

- `bot_settings.json`  
  Throttles, enable/disable flags, platform behaviour

No recompilation required for most behaviour changes.

---

## 🔌 OBS Integration

- Add a **Browser Source**
- Point to:
  ```
  http://localhost:<port>/overlay/...
  ```
- Fixed-height, broadcast-safe layouts
- Designed for 1080p and stacked overlays

---

## 🚧 Project Status

This is an **active, evolving project**.

Completed:
- Core chat ingestion
- Twitch auth & token refresh
- HTTP server & overlays
- JSON-based command system

In progress / planned:
- Command editing UI
- Discord integration
- Advanced aviation data overlays
- Event-driven alert system
- Packaging & installer
- Public documentation for contributors

---

## 🤝 Contributing

This project is currently **opinionated and tightly integrated** with the RadarController ecosystem, but contributions, ideas, and discussions are welcome.

It's important to note at this stage, I am not a developer. I have no idea how to code. Everything has been done with ChatGPT 5.2. Things will be wrong, things will be duplicated or in the wrong place. You might look at this and question every single decision I've made. That's ok. I'd appreciate it if you do.

If you’re interested:
- Open an issue
- Start a discussion
- Or fork and explore

---

## 📜 License

License to be finalised.  
All rights reserved for now.

---

## ✈️ About RadarController

RadarController is a VATSIM ATC streamer focused on **realistic operations, education, and high-quality production**, streaming across Twitch, TikTok, and YouTube.

The Mode-S Client exists to push ATC streaming beyond “just screen capture” — into **interactive, data-rich broadcasting**.

---

*Built for controllers.  
Built for scale.  
Built properly.*
