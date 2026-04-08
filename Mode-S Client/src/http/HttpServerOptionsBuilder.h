#pragma once

#include <functional>
#include <string>

#include "http/HttpServer.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "app/AppBootstrap.h"

namespace httpoptions {

HttpServer::Options BuildHttpServerOptions(
    AppBootstrap::Dependencies& deps,
    const std::wstring& exeDir,
    const std::function<void(const std::string&)>& restartTwitchHelixPoller);

} // namespace httpoptions
