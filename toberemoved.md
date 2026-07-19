# Items to Remove or Replace Before Handover

## 1. Scope

This is the takeover audit requested for Mode-S Client. It records:

- values hardcoded to the original RadarController/StreamingATC identity;
- external applications, tokens and account state that belong to the original operator;
- machine-specific build settings;
- stream-specific defaults that a successor should consciously adopt, change or remove;
- legacy and incomplete code that should not be presented as operational.

This file is intentionally an action register. Items should be checked off or removed from this document as they are resolved.

No committed live OAuth secret was found during the audit. The repository deliberately excludes the local credential header and `config.json`. That does **not** remove the need to revoke the original credentials and authorize replacement accounts.

---

## 2. Priority 0: account ownership and credentials

These items must be resolved before another person runs the application with real accounts.

### 2.1 SimBrief account is hardcoded

- [ ] Replace the hardcoded SimBrief pilot ID `11686`.

Location:

```text
Mode-S Client/src/http/HttpServer.cpp
```

`StartSimBriefWorker()` declares the ID twice: once in the function scope and once inside the worker-thread lambda. The worker requests:

```text
www.simbrief.com/api/xml.fetcher.php?userid=11686&json=1
```

Until changed, every installation will continue to fetch the original operator's latest SimBrief plan.

Recommended fix:

- add `simbrief_pilot_id` to `AppConfig` and `config.example.json`;
- do not start the worker when it is unset or invalid;
- expose the setting through the local UI;
- remove both hardcoded declarations;
- avoid logging the full returned plan where it could expose personal flight data.

### 2.2 Twitch developer application and user authorization

- [ ] Revoke or retire the original Twitch developer application credentials.
- [ ] Create a Twitch developer application controlled by the new maintainer.
- [ ] Configure the replacement OAuth redirect URI used by the local callback route.
- [ ] Create a new ignored `EmbeddedOAuthConfig.local.h` containing the replacement app credentials.
- [ ] Delete the original local `config.json` rather than transferring its Twitch tokens.
- [ ] Authorize the replacement Twitch account through the application.
- [ ] Set a replacement `twitch_login`.

Relevant tracked code:

```text
Mode-S Client/src/oauth/EmbeddedOAuthConfig.h
Mode-S Client/integrations/twitch/TwitchAuth.*
Mode-S Client/integrations/twitch/TwitchIrcWsClient.*
Mode-S Client/integrations/twitch/TwitchHelixService.*
Mode-S Client/integrations/twitch/TwitchEventSubWsClient.*
Mode-S Client/src/runtime/TwitchRuntimeCoordinator.cpp
Mode-S Client/src/http/HttpServerOptionsBuilder.cpp
```

Relevant local/ignored data:

```text
Mode-S Client/src/oauth/EmbeddedOAuthConfig.local.h
config.json -> twitch_login
config.json -> twitch.user_access_token
config.json -> twitch.user_refresh_token
```

The tracked header uses macros named `RC_TWITCH_CLIENT_ID` and `RC_TWITCH_CLIENT_SECRET`. The values default to empty and the real local header is ignored. The `RC_` prefix is branding residue and should eventually be renamed to a neutral project prefix, but renaming it is secondary to replacing the credentials.

The current Twitch authorization requests broad channel-management scopes, including chat, followers, subscriptions, bits, Channel Points and broadcast management. The new maintainer should review and reduce the scopes to those actually required.

No hardcoded Twitch broadcaster numeric ID was found in the tracked code. The runtime resolves the configured login through Twitch APIs.

### 2.3 Twitch Channel Points rewards and mappings

- [ ] Remove or recreate rewards owned by the original Twitch channel.
- [ ] Clear locally persisted reward action mappings before connecting a replacement channel.
- [ ] Replace any reward-specific images and sounds.
- [ ] Verify that pending/history data does not contain original channel redemption IDs.

Reward IDs are channel-specific even where they are not hardcoded into source code. Runtime mappings are persisted through application state/config and should not be copied blindly to another broadcaster.

The repository ignores these potentially account-specific asset directories:

```text
Mode-S Client/assets/channel-points/
Mode-S Client/assets/sounds/
```

They are not covered by the committed-file text audit. Inspect and remove any local copies before handing over a build directory or development machine.

### 2.4 Google/YouTube developer application and channel authorization

- [ ] Revoke or retire the original Google OAuth client.
- [ ] Create a Google Cloud project/OAuth client controlled by the new maintainer.
- [ ] Configure the local YouTube OAuth redirect URI.
- [ ] populate the ignored local OAuth header with the replacement client ID and secret.
- [ ] Delete the original YouTube tokens/channel state from local `config.json`.
- [ ] Authorize the replacement YouTube channel.
- [ ] Set a replacement `youtube_handle`.

Relevant tracked code:

```text
Mode-S Client/src/oauth/EmbeddedOAuthConfig.h
Mode-S Client/integrations/youtube/YouTubeAuth.*
Mode-S Client/integrations/youtube/YouTubeChannelStatsService.*
Mode-S Client/integrations/youtube/YouTubeLiveStatusService.*
Mode-S Client/integrations/youtube/YouTubeLiveChatService.*
Mode-S Client/src/runtime/YouTubeRuntimeCoordinator.cpp
Mode-S Client/src/http/HttpServerOptionsBuilder.cpp
Mode-S Client/src/http/HttpServer.cpp
```

Relevant local/ignored data:

```text
Mode-S Client/src/oauth/EmbeddedOAuthConfig.local.h
config.json -> youtube.access_token
config.json -> youtube.refresh_token
config.json -> youtube.channel_id
config.json -> youtube.expires_at_unix
config.json -> youtube.scope
config.json -> youtube_handle
```

The tracked macros `RC_YOUTUBE_CLIENT_ID` and `RC_YOUTUBE_CLIENT_SECRET` should eventually receive neutral names.

### 2.5 TikTok account and session cookies

- [ ] Remove the original TikTok unique ID from local configuration.
- [ ] Remove the original `sessionid`, `sessionid_ss` and `tt_target_idc` values.
- [ ] Connect only a replacement account that the new maintainer is authorized to use.
- [ ] Review the current TikTokLive/session-cookie approach against current TikTok and library requirements.

Relevant tracked code:

```text
Mode-S Client/scripts/sidecar/tiktok_sidecar.py
Mode-S Client/integrations/tiktok/TikTokSidecar.*
Mode-S Client/integrations/tiktok/TikTokFollowersService.*
Mode-S Client/src/runtime/TikTokRuntimeCoordinator.cpp
```

Relevant local/ignored data:

```text
config.json -> tiktok_unique_id
config.json -> tiktok_sessionid
config.json -> tiktok_sessionid_ss
config.json -> tiktok_tt_target_idc
```

TikTok cookies are credentials. Do not transfer them to a successor or include them in a packaged application.

### 2.6 Local operational state

- [ ] Do not hand over the original runtime data files without reviewing and sanitizing them.

Potential local state includes:

```text
config.json
bot_commands.json
bot_settings.json
overlay_header.json
twitch_streaminfo.json
Fenix failure metadata JSON
generated logs and diagnostics
WebView2 profile/cache data
```

The source-controlled `bot_commands.json` is a default file and is covered separately below. Locally modified copies may contain URLs, names or policies not present in Git.

---

## 3. Priority 1: hardcoded RadarController and StreamingATC identity

### 3.1 Native application identity

- [ ] Replace the hardcoded application display name:

```text
Mode-S Client/src/Mode-S Client.cpp
```

Current value:

```text
StreamingATC.Live Mode-S Client
```

This value is used for the splash, window title and visible application identity.

- [ ] Review the native window class name `StreamHubWindow`. It is not personally identifying, but it reflects an older product name and may be renamed during a wider branding cleanup.

### 3.2 Bot identity and default content

- [ ] Make the synthetic bot display name configurable.

Location:

```text
Mode-S Client/src/bot/BotCommandDispatcher.cpp
```

Current value:

```text
StreamingATC.Bot
```

It is used both to ignore the application's own messages and to label synthetic bot messages. This is functional coupling, not merely cosmetic text. A replacement must use the same configured identity consistently for loop prevention and overlay attribution.

- [ ] Replace the default `about` response in:

```text
Mode-S Client/bot_commands.json
```

Current response:

```text
StreamingATC.Live Mode-S Client
```

- [ ] Review every locally edited bot response for original Discord links, social handles, schedules, policies and names.

- [ ] Replace the example channel in the comment in:

```text
Mode-S Client/src/bot/BotReplyRouter.h
```

The `radarcontroller` value there is documentation-only, but it should not remain in a neutral handover.

### 3.3 Embedded control-surface pages

Explicit RadarController references were found in the following application assets:

- [ ] `Mode-S Client/assets/app/index.html`
- [ ] `Mode-S Client/assets/app/settings.html`
- [ ] `Mode-S Client/assets/app/accounts.html`
- [ ] `Mode-S Client/assets/app/bot.html`
- [ ] `Mode-S Client/assets/app/channel_points.js`
- [ ] `Mode-S Client/assets/app/diagnostics.html`
- [ ] `Mode-S Client/assets/app/overlay_title.html`
- [ ] `Mode-S Client/assets/app/stream_details.html`
- [ ] `Mode-S Client/assets/app/tiktok_cookies.html`
- [ ] `Mode-S Client/assets/app/twitch_rewards.html`
- [ ] `Mode-S Client/assets/app/twitch_stream.html`
- [ ] `Mode-S Client/assets/app/splash/index.html`

Examples include page titles, logo alternative text, visible brand names and debug/test data. `channel_points.js`, for example, injects `RadarController` as the user in its manual debug event.

The pages should use one configurable or generated brand source rather than repeating product identity in each file.

### 3.4 Overlay pages and stream-specific templates

Explicit RadarController/StreamingATC references were found in:

- [ ] `Mode-S Client/assets/overlay/landscape/26_starting.html`
- [ ] `Mode-S Client/assets/overlay/landscape/26_post_stream.html`
- [ ] `Mode-S Client/assets/overlay/landscape/26_footer_blighty.html`
- [ ] `Mode-S Client/assets/overlay/landscape/26_webcam.html`
- [ ] `Mode-S Client/assets/overlay/portrait/26_starting.html`
- [ ] `Mode-S Client/assets/overlay/portrait/26_post_stream.html`
- [ ] `Mode-S Client/assets/overlay/landscape/channel_points.html`
- [ ] `Mode-S Client/assets/overlay/portrait/channel_points_portrait.html`
- [ ] `Mode-S Client/assets/overlay/channel_points.js`

Known content includes:

- `@radarcontroller`;
- `twitch.tv/radarcontroller`;
- StreamingATC.Live branding;
- RadarController names and logos;
- Blighty-specific naming in `26_footer_blighty.html`.

The `26_*` naming and layout set appear to be specific to the original 2026 stream package. A successor may keep the layout, but it should be renamed and reviewed as a complete visual package rather than performing text replacement only.

### 3.5 Icons, logos and compiled resources

- [ ] Replace `/assets/icons/RClogo.svg` wherever used by the embedded UI.
- [ ] Replace `Mode-S Client/assets/icons/radarcontrollermodesclient.ico`.
- [ ] Update the icon reference in `Mode-S Client/Mode-S Client.vcxproj`.
- [ ] Update corresponding entries in `Mode-S Client/Mode-S Client.vcxproj.filters`.
- [ ] Review `Mode-S Client/ui/Mode-S Client.rc` and resource headers for names or assets tied to the original brand.
- [ ] Visually inspect every binary image in `assets/icons/` and the overlay folders. Text search cannot detect words baked into PNG, JPG, ICO or other binary assets.

### 3.6 Splash implementation

- [ ] Review branding in:

```text
Mode-S Client/src/ui/SplashScreen.cpp
Mode-S Client/assets/app/splash/index.html
```

The native code receives the product name from `Mode-S Client.cpp`, while the HTML splash also contains StreamingATC branding. Both must be updated together.

### 3.7 README and repository metadata

- [ ] Rewrite `README.md` for a neutral project or the successor's identity.
- [ ] Remove the RadarController biography and claims about the wider StreamingATC.Live ecosystem where they are no longer applicable.
- [ ] Update screenshots, links, contribution text and project status.
- [ ] Resolve the licence. The current README says the licence is to be finalised and that all rights are reserved.
- [ ] Review the GitHub repository name, description, topics, social preview and owner after the code transfer decision is made.

A code handover and a legal right to redistribute are separate matters. A clear licence or written transfer arrangement is required.

---

## 4. Priority 1: build and machine-specific dependencies

### 4.1 Hardcoded SimConnect SDK drive paths

- [ ] Remove the original development-machine paths from:

```text
Mode-S Client/Mode-S Client.vcxproj
```

Current include path in x64 configurations:

```text
G:\MSFS 2024 SDK\SimConnect SDK\include
```

Current library path in x64 configurations:

```text
G:\MSFS 2024 SDK\SimConnect SDK\lib
```

Recommended replacement:

- define an MSBuild property such as `$(SIMCONNECT_SDK)`;
- read it from an untracked `.props` file or environment variable;
- provide a documented example property sheet;
- fail with a clear build message when the SDK is unavailable;
- verify Debug x64 and Release x64 on a clean machine.

### 4.2 Python/TikTok packaging

- [ ] Confirm how Python is supplied to the replacement installation.
- [ ] Confirm that the required `TikTokLive` dependencies are included and licensed for distribution.
- [ ] Remove cached packages, cookies and local interpreter paths from any handover archive.

The Visual Studio post-build copies the sidecar directory, but the application still needs a usable Python executable and compatible package set.

### 4.3 Version-generation tooling

- [ ] Verify that `tools/gen_version.ps1` is present in a fresh clone and remains tracked despite the broad `tools/` ignore rule.
- [ ] Document PowerShell execution requirements for a clean build.

---

## 5. Priority 2: stream-specific behaviour to review

These items are not necessarily wrong, but they reflect the original stream and should not silently become defaults for another operator.

### 5.1 OBS text input names

- [ ] Review these hardcoded OBS input names in `src/runtime/ObsMetricsPublisher.cpp`:

```text
TOTAL_VIEWER_COUNT
TOTAL_FOLLOWER_COUNT
```

They are tied to a particular OBS scene collection naming convention. More importantly, the current `ObsWsClient` is a no-op stub, so changing the names alone will not make the feature work.

Recommended outcome: either remove the publisher until implemented or create configurable OBS connection/input settings with a real WebSocket transport.

### 5.2 Fenix monetization-to-failure policy

- [ ] Review which Twitch, TikTok and YouTube events create failure credits.
- [ ] Review bits, gift and membership/subscription conversion thresholds.
- [ ] Review eligible failures, weights, cooldowns and per-session limits.
- [ ] Review the hardcoded 60% immediate / 40% armed selection split.
- [ ] Keep automation disabled by default until the successor approves the policy.
- [ ] Remove original failure metadata and regenerate it against the successor's Fenix installation.

This feature embodies stream rules, not generic application infrastructure.

### 5.3 Overlay and UI timing/style defaults

- [ ] Review Channel Points polling, queue, hold and animation timings.
- [ ] Review overlay dimensions, fixed layout assumptions and typography.
- [ ] Review the default Inter font and external Google Fonts request.
- [ ] Replace original headers, titles, callsign examples and social prompts.

### 5.4 Default bot commands

- [ ] Decide whether `!help`, `!about`, `!discord` and `!bacon` belong in a neutral default installation.
- [ ] Add `!metar` to the documented command list if the feature is retained.
- [ ] Replace the placeholder Discord response with either configuration or no default command.

### 5.5 Local port and paths

The loopback port `17845` is hardcoded in multiple places. It is not RadarController-specific, but a distributable application should ideally make it configurable from one source. If changed, update:

- native WebView URL;
- HTTP server options;
- logs and documentation;
- OBS browser-source instructions;
- OAuth redirect URIs.

Do not expose the listener beyond loopback without adding authentication and a security design.

---

## 6. Priority 2: legacy, misleading or incomplete components

### 6.1 Unused YouTube sidecar path

- [ ] Confirm and then remove or document:

```text
Mode-S Client/integrations/youtube/YouTubeSidecar.h
Mode-S Client/integrations/youtube/YouTubeSidecar.cpp
Mode-S Client/scripts/sidecar/youtube_sidecar.py
```

The files are included in the project/package, but no active startup or consumer reference was found. The active YouTube implementation is native C++.

### 6.2 Suspicious `AppRuntime::youtube` member

- [ ] Confirm and remove or rename the following member in `src/app/AppRuntime.h`:

```cpp
TikTokSidecar youtube;
```

It is not included in the current bootstrap dependency object and appears to be a remnant of the older sidecar path.

### 6.3 OBS WebSocket stub

- [ ] Do not advertise direct OBS text-input updates as working.
- [ ] Either implement `ObsWsClient` or remove the no-op publisher path.

Current behaviour:

```text
ObsWsClient::connect() -> always false
ObsWsClient::set_text() -> no operation
```

OBS browser-source overlays remain functional independently of this stub.

### 6.4 Monolithic HTTP server and stale storage names

- [ ] Extract route groups from `HttpServer.cpp` incrementally.
- [ ] Move SimBrief polling and related external API logic into a dedicated service.
- [ ] Rename or migrate `twitch_streaminfo.json`, which currently stores some YouTube VOD draft data.
- [ ] Correct diagnostics that still describe obsolete credential locations.

These are maintainability items, not immediate account-transfer blockers.

---

## 7. External services that are not tied to the original account

The audit did not find original-owner credentials or identifiers in these integrations:

| Integration | Current dependency | Takeover assessment |
|---|---|---|
| Local HTTP/WebView | `127.0.0.1:17845` | Generic local infrastructure |
| EuroScope ingest | Local JSON producer with `ts_ms` | No VATSIM account authentication found |
| AviationWeather METAR | Public `aviationweather.gov` endpoint | No personal API key found |
| SimConnect | Local MSFS connection | Machine/SDK dependent, not account dependent |
| Fenix EFB API | `localhost:8083` | Local simulator dependency, not RadarController account dependent |
| WebView2 | Microsoft runtime/NuGet package | Generic dependency |
| OpenSSL/cpp-httplib | Build/runtime libraries | Generic dependency |

No VATSIM OAuth token, VATSIM account ID or direct VATSIM API integration was found in the inspected code. EuroScope data is accepted from a local producer without authenticating to VATSIM.

Twitch, TikTok and YouTube integration code is reusable, but all application registrations, channel identities, user tokens, cookies, reward IDs and local state must belong to the replacement operator.

---

## 8. Search inventory used for the audit

The following searches should return no unintended original-owner references after cleanup:

```powershell
git grep -n -i "radarcontroller"
git grep -n -i "streamingatc"
git grep -n -i "streamingatc.live"
git grep -n -i "@radarcontroller"
git grep -n -i "twitch.tv/radarcontroller"
git grep -n "11686"
git grep -n "G:\\MSFS 2024 SDK"
git grep -n -i "RClogo"
git grep -n -i "radarcontrollermodesclient"
git grep -n "RC_TWITCH"
git grep -n "RC_YOUTUBE"
git grep -n "StreamingATC.Bot"
```

Also search for likely secret and identity fields:

```powershell
git grep -n -i "client_secret"
git grep -n -i "access_token"
git grep -n -i "refresh_token"
git grep -n -i "sessionid"
git grep -n -i "channel_id"
git grep -n -i "broadcaster_user_id"
git grep -n -i "reward_id"
```

Expected code references to field names are not secrets. Inspect the actual values and the Git history before deciding that a result is safe.

Text search is insufficient for:

- binary icons and images;
- audio files;
- ignored local asset directories;
- build output;
- WebView profile data;
- deleted secrets retained in Git history;
- repository settings and release attachments.

A final handover must include a visual asset review and a secret-history scan.

---

## 9. Recommended removal order

1. **Revoke credentials first.** Revoke original Twitch/Google tokens and remove TikTok cookies from local machines.
2. **Start from clean local state.** Do not transfer the original `config.json`, WebView profile or runtime JSON files.
3. **Parameterize account values.** Fix the SimBrief ID and any discovered reward/account IDs.
4. **Make the build portable.** Remove the `G:` SimConnect paths and document prerequisites.
5. **Centralize product identity.** Introduce one neutral brand configuration/source.
6. **Replace UI, overlays and binary assets.** Include social URLs and debug fixtures.
7. **Remove confirmed dead paths and label stubs accurately.** Begin with the YouTube sidecar and OBS client after verification.
8. **Resolve licensing and repository ownership.** Do this before third-party distribution.
9. **Run all text and secret searches again.** Review the complete diff and Git history.
10. **Build and smoke-test with replacement accounts.** Test one platform at a time and keep Fenix automation disabled initially.

---

## 10. Completion criteria

The takeover cleanup is complete when:

- no original account credential remains active;
- no hardcoded original account ID controls runtime data;
- a clean clone can build without the original machine's drive layout;
- the application can start with an empty configuration;
- a successor can register and authorize their own platform applications;
- UI, overlays, icons, bot identity and documentation contain no unintended original branding;
- account-specific reward and stream policy data has been cleared or consciously recreated;
- incomplete features are removed, implemented or clearly labelled;
- the repository has a clear licence and transfer position;
- the searches in this document have been reviewed with no unexplained results.