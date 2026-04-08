#pragma once

#include <string>

class AppState;

namespace overlay {

void InitializeOverlayHeaderStorage(AppState& state, const std::wstring& exeDir);

} // namespace overlay
