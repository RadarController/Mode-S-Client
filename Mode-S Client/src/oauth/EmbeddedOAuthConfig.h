#pragma once

// Local overrides should define these macros before this file's constants are declared.
// Keep real secrets out of this tracked file.
#ifdef __has_include
#  if __has_include("EmbeddedOAuthConfig.local.h")
#    include "EmbeddedOAuthConfig.local.h"
#  endif
#endif

#ifndef RC_TWITCH_CLIENT_ID
#  define RC_TWITCH_CLIENT_ID ""
#endif

#ifndef RC_TWITCH_CLIENT_SECRET
#  define RC_TWITCH_CLIENT_SECRET ""
#endif

#ifndef RC_YOUTUBE_CLIENT_ID
#  define RC_YOUTUBE_CLIENT_ID ""
#endif

#ifndef RC_YOUTUBE_CLIENT_SECRET
#  define RC_YOUTUBE_CLIENT_SECRET ""
#endif

#include <string>

namespace EmbeddedOAuthConfig
{
    inline const std::string& TwitchClientId()
    {
        static const std::string v = RC_TWITCH_CLIENT_ID;
        return v;
    }

    inline const std::string& TwitchClientSecret()
    {
        static const std::string v = RC_TWITCH_CLIENT_SECRET;
        return v;
    }

    inline const std::string& YouTubeClientId()
    {
        static const std::string v = RC_YOUTUBE_CLIENT_ID;
        return v;
    }

    inline const std::string& YouTubeClientSecret()
    {
        static const std::string v = RC_YOUTUBE_CLIENT_SECRET;
        return v;
    }

    inline bool HasTwitchCredentials()
    {
        return !TwitchClientId().empty() && !TwitchClientSecret().empty();
    }

    inline bool HasYouTubeCredentials()
    {
        return !YouTubeClientId().empty() && !YouTubeClientSecret().empty();
    }
}