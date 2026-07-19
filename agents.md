# AI Agents and AI-Assisted Development

## 1. Why this file exists

This repository was built primarily through AI-assisted development. The existing README explicitly states that the original author was not a software developer and that the implementation was produced with ChatGPT 5.2.

That history is relevant to anyone taking over the project. AI made it possible to build a broad, working prototype, but the resulting code must not be assumed to have received the same design review, test coverage or security review as a conventionally maintained production application.

This file explains:

- how AI has been used;
- what that means for the present codebase;
- how future AI agents should work in the repository;
- which decisions must remain subject to human review.

It does not contain hidden prompts, model transcripts or a complete development history. Those artefacts are not present in the repository.

---

## 2. How AI has been used

### Confirmed use

The repository's README records that ChatGPT 5.2 was used to create the project. Based on that statement, AI should be treated as the primary implementation assistant for the existing code.

AI has evidently been used for work including:

- generating C++ integration code;
- creating Win32 and WebView2 application structure;
- adding platform API clients;
- creating HTML, CSS and JavaScript control surfaces and overlays;
- breaking parts of the original implementation into modules;
- writing comments and diagnostics;
- implementing JSON persistence and HTTP routes;
- iterating on simulator and streaming features.

The precise model, prompt and human review level for each individual file cannot be established from the repository alone.

### Reasonable inference from the code

The code contains several characteristics commonly produced by iterative, prompt-driven development:

- very detailed explanatory comments in some files and little documentation in others;
- multiple abstractions for related concerns;
- old implementation paths remaining after newer paths were added;
- comments and diagnostics that refer to superseded configuration mechanisms;
- a large route file that accumulated many unrelated features;
- partially implemented interfaces and placeholder components;
- duplicated or unusual ownership members that no longer appear to be wired.

These observations do not mean the code is unusable. They mean that the current behaviour must be traced and tested rather than inferred from filenames, comments or class names alone.

---

## 3. Current AI-era artefacts to understand

The following examples are known at the time of this handover:

### Active implementation versus remnants

- The active YouTube implementation uses native C++ OAuth, channel-statistics, live-status and live-chat services.
- `YouTubeSidecar.*` and `youtube_sidecar.py` remain in the project but were not found in the active runtime wiring.
- `AppRuntime` has a member named `youtube` whose type is `TikTokSidecar`; it appears to be an unused remnant.

An AI agent must not extend an apparently relevant class until it has traced whether that class is actually constructed, started and consumed.

### Stubbed functionality

`ObsWsClient` is a stub. Its `connect()` method always returns false and `set_text()` is a no-op. Although `ObsMetricsPublisher` calls it, there is no functioning OBS WebSocket update path. The operational OBS integration is currently the local browser-source overlays.

An AI agent must not describe this feature as working simply because the class and worker exist.

### Monolithic HTTP implementation

`HttpServer.cpp` contains many route groups, external API helpers, file-persistence helpers, SimBrief polling and SimConnect exposure. This makes broad edits risky because apparently unrelated features share one translation unit.

Future changes should extract one coherent route or service group at a time and preserve route behaviour during the move.

### Stale names and configuration models

Some diagnostics refer to credentials being read directly from `config.json`, while the current design expects OAuth application credentials through `EmbeddedOAuthConfig.local.h` and user tokens through `config.json`.

AI-generated comments are documentation, not proof. Follow the actual call path.

---

## 4. Rules for future AI agents

These rules apply to ChatGPT, Codex, Claude and any other automated coding agent used on the repository.

### 4.1 Read before editing

Before making changes, read at least:

1. `architecture.md`;
2. `toberemoved.md`;
3. `README.md`;
4. `Mode-S Client/Mode-S Client.vcxproj`;
5. the entry point in `src/Mode-S Client.cpp`;
6. `src/app/AppRuntime.h`;
7. `src/app/AppBootstrap.cpp`;
8. `src/app/AppShutdown.cpp`;
9. every caller and consumer of the component being changed.

Do not treat code search results as a substitute for reading the relevant implementation.

### 4.2 Trace the active path

For each proposed change, identify:

- who owns the object;
- who starts it;
- which thread invokes it;
- where its output is stored;
- which API/UI/overlay consumes that output;
- how it is stopped;
- whether another older implementation exists.

A useful change description should be able to state this path in plain language.

### 4.3 Keep changes small

Prefer one coherent change per pull request. Examples:

- make the SimBrief ID configurable;
- replace embedded branding with a brand configuration object;
- extract Twitch routes from `HttpServer.cpp`;
- implement the OBS WebSocket client;
- remove the unused YouTube sidecar.

Do not combine a takeover cleanup, large refactor, dependency upgrade and new feature into one patch.

### 4.4 Do not rewrite working areas without evidence

Broad AI rewrites can remove subtle platform behaviour, event normalization, rate limiting or shutdown handling. Preserve interfaces and behaviour unless the task explicitly calls for a breaking change.

When refactoring:

- move code before redesigning it;
- keep endpoint paths and JSON response shapes stable;
- retain existing failure handling until tests prove a replacement;
- compare before/after behaviour with recorded fixtures where possible.

### 4.5 Preserve runtime lifetimes

Long-running services are owned by `AppRuntime`. Startup receives short-lived dependency views. Never capture those temporary wrapper objects by reference in asynchronous callbacks.

Every added worker must have:

- a long-lived owner;
- a stop flag or cancellation mechanism;
- a deterministic join/stop path;
- an entry in `AppShutdown` where appropriate.

### 4.6 Respect the loopback security boundary

The local HTTP server currently binds to `127.0.0.1`. Do not change it to `0.0.0.0`, a LAN address or an internet-facing listener without implementing a security design first.

Many routes can:

- modify account settings;
- start or stop platform clients;
- update stream information;
- manage rewards;
- send messages;
- invoke simulator automation.

Exposing those routes would require authentication, authorization, CSRF protection, input limits, secret handling and a threat review.

### 4.7 Never commit credentials

Do not commit:

- `config.json`;
- `EmbeddedOAuthConfig.local.h`;
- OAuth client secrets;
- access or refresh tokens;
- TikTok cookies;
- account-specific channel IDs where they are intended to remain private;
- OBS passwords;
- local browser/session data;
- copied production logs containing tokens or personal data.

Use redacted fixtures or obvious placeholders in examples.

Before proposing a commit, search the diff for terms such as:

```text
client_secret
access_token
refresh_token
sessionid
Authorization:
Bearer 
oauth:
```

### 4.8 Do not inherit the original operator's identity

New work must not introduce or preserve a personal account, pilot ID, social handle, reward ID or brand string as a default.

Account identifiers belong in local configuration. Brand values should be configurable or placed in one clearly documented branding module.

Use `toberemoved.md` as the current audit list and update it as items are resolved.

### 4.9 Be explicit about incomplete validation

An AI agent must state exactly what was tested. Acceptable statements include:

- project compiled in Debug x64;
- project compiled in Release x64;
- local `/app` and selected API routes were smoke-tested;
- Twitch OAuth was not tested because no test account was available;
- simulator integration was not tested because MSFS/Fenix was unavailable.

Do not claim that a change is working merely because it is syntactically plausible or because a file was successfully edited.

### 4.10 Update documentation with architecture changes

A change must update `architecture.md` when it alters:

- service ownership;
- startup or shutdown;
- local ports or route groups;
- persistence locations;
- authentication flows;
- platform data flow;
- thread behaviour;
- build prerequisites;
- the status of a stub or legacy component.

Update `toberemoved.md` when a takeover item is resolved, replaced or newly discovered.

---

## 5. Recommended AI workflow

### Step 1: Establish scope

Restate the requested outcome and identify the minimum files required. Check for unrelated working-tree or branch changes before editing.

### Step 2: Map the implementation

Use code search to find declarations, constructors, calls, routes, configuration keys and UI consumers. Read complete files around the relevant sections.

### Step 3: Identify risks

Consider:

- account ownership;
- secret exposure;
- thread lifetime;
- shutdown hangs;
- API rate limits;
- platform permission scopes;
- backward compatibility of local JSON;
- overlay response shape;
- build portability.

### Step 4: Implement narrowly

Patch only the intended behaviour. Avoid opportunistic formatting or unrelated renaming.

### Step 5: Validate in layers

Where the development environment permits:

1. restore packages;
2. compile Debug x64;
3. compile Release x64;
4. run the client with a clean test configuration;
5. test the affected local API route;
6. test the associated UI/overlay;
7. test the external platform or simulator with a non-production account/session;
8. verify clean shutdown.

### Step 6: Review the diff

Check for:

- accidental secrets;
- unrelated changes;
- duplicated implementations;
- new hardcoded account data;
- missing error handling;
- missing shutdown wiring;
- stale comments and documentation.

### Step 7: Publish as a reviewable change

Use a dedicated branch and draft pull request. The pull request should explain:

- what changed;
- why it changed;
- the runtime path affected;
- what was tested;
- what could not be tested;
- any migration or credential action required.

---

## 6. Human review requirements

AI should assist, not act as the final authority, for the following areas.

### Authentication and account transfer

A human owner must create, control and revoke Twitch, Google/YouTube and TikTok credentials. AI must not decide to transfer or retain another person's tokens.

### Platform terms and policy

Platform API scopes, automated chat behaviour, scraping/session-cookie use, monetization events and simulator-triggered rewards can have policy implications. A human maintainer must confirm current platform terms before deployment.

### Simulator automation safety

The Fenix failure feature can change the simulated aircraft state in response to public support events. A human operator must approve:

- which events count as credits;
- conversion thresholds;
- eligible failures;
- immediate versus armed behaviour;
- cooldowns;
- maximum frequency;
- the panic-stop procedure.

### Security and distribution

A human reviewer must approve any change that:

- exposes the HTTP server beyond loopback;
- changes secret storage;
- adds telemetry or remote services;
- creates an installer or auto-updater;
- signs and distributes binaries.

### Licensing

The repository currently says that licensing is not finalised. A human rights holder must select and apply a licence before a formal transfer, open-source release or third-party binary distribution.

---

## 7. Areas suitable for future AI-assisted improvement

AI can be useful for the following, provided each change is reviewed and tested:

- breaking `HttpServer.cpp` into route modules;
- creating configuration schemas and migration helpers;
- replacing machine-specific build paths with MSBuild properties;
- adding unit tests for normalization, persistence and bot throttling;
- adding recorded JSON fixtures for platform events;
- implementing a genuine OBS WebSocket transport;
- removing confirmed dead YouTube sidecar code;
- consolidating branding into configuration;
- adding secure Windows credential storage;
- adding CI for dependency restore and compilation;
- producing an installer and clean first-run setup flow.

The preferred use of AI is incremental modernization around known behaviour, not wholesale regeneration of the application.

---

## 8. Definition of done for AI-generated changes

An AI-assisted change is not complete until:

- its active call path has been traced;
- no personal credentials or account identifiers were introduced;
- the implementation has an owner and shutdown path where required;
- relevant builds/tests have been run or explicitly marked unavailable;
- the diff contains no unrelated edits;
- user-facing or architectural documentation is updated;
- a human can understand the change without reading the AI conversation that produced it.