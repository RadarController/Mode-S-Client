#pragma once

class AppState;
class YouTubeAuth;

namespace runtime {

void StartYouTubeRuntimeServices(YouTubeAuth& youtubeAuth, AppState& state);
void StopYouTubeRuntimeServices();

} // namespace runtime
