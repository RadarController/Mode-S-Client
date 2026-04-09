#pragma once

struct AppConfig;
class AppState;
class YouTubeAuth;

namespace runtime {

void StartYouTubeRuntimeServices(YouTubeAuth& youtubeAuth, AppConfig& config, AppState& state);
void StopYouTubeRuntimeServices();

} // namespace runtime
