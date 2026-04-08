#pragma once

#include <string>

typedef struct HWND__* HWND;
class AppState;

namespace bot {

void InitializeBotStorage(AppState& state, const std::wstring& exeDir);

} // namespace bot
