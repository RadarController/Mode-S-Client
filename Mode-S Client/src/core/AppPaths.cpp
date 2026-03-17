#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "core/AppPaths.h"

std::wstring GetExeDir()
{
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring p = path;
    auto pos = p.find_last_of(L"\\/");
    return (pos == std::wstring::npos) ? L"." : p.substr(0, pos);
}
