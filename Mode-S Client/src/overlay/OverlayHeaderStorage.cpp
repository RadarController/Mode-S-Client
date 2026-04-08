#include "overlay/OverlayHeaderStorage.h"

#include <filesystem>

#include "AppState.h"
#include "core/StringUtil.h"
#include "log/UiLog.h"

namespace overlay {

void InitializeOverlayHeaderStorage(AppState& state, const std::wstring& exeDir)
{
    try {
        const std::filesystem::path hdrPath = std::filesystem::path(exeDir) / "overlay_header.json";
        state.set_overlay_header_storage_path(ToUtf8(hdrPath.wstring()));
        if (state.load_overlay_header_from_disk()) {
            LogLine(L"OVERLAY: loaded header settings from overlay_header.json");
        }
        else {
            LogLine(L"OVERLAY: no overlay_header.json found (or empty/invalid) - using defaults");
        }
    }
    catch (...) {
        LogLine(L"OVERLAY: failed to set/load overlay header settings path");
    }
}

} // namespace overlay
