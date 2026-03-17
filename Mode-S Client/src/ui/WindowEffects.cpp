#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "ui/WindowEffects.h"

// --------------------------- Translucent window helper ----------------------
// Prefer Acrylic blur so the background is translucent but text remains readable.
// Falls back to layered alpha if unavailable (Windows versions without the API).
typedef enum _ACCENT_STATE {
    ACCENT_DISABLED = 0,
    ACCENT_ENABLE_GRADIENT = 1,
    ACCENT_ENABLE_TRANSPARENTGRADIENT = 2,
    ACCENT_ENABLE_BLURBEHIND = 3,
    ACCENT_ENABLE_ACRYLICBLURBEHIND = 4,
    ACCENT_ENABLE_HOSTBACKDROP = 5
} ACCENT_STATE;

typedef struct _ACCENT_POLICY {
    int AccentState;
    int AccentFlags;
    int GradientColor; // ARGB
    int AnimationId;
} ACCENT_POLICY;

typedef struct _WINDOWCOMPOSITIONATTRIBDATA {
    int Attrib;
    PVOID pvData;
    SIZE_T cbData;
} WINDOWCOMPOSITIONATTRIBDATA;

bool TryEnableAcrylic(HWND hwnd, BYTE bgOpacity /*0..255*/)
{
    HMODULE hUser = GetModuleHandleW(L"user32.dll");
    if (!hUser) return false;

    using Fn = BOOL(WINAPI*)(HWND, WINDOWCOMPOSITIONATTRIBDATA*);
    auto p = (Fn)GetProcAddress(hUser, "SetWindowCompositionAttribute");
    if (!p) return false;

    const int WCA_ACCENT_POLICY = 19;

    ACCENT_POLICY policy{};
    policy.AccentState = ACCENT_ENABLE_ACRYLICBLURBEHIND;
    policy.AccentFlags = 2;

    // Dark tint; bgOpacity controls tint alpha (NOT child controls).
    policy.GradientColor = (bgOpacity << 24) | 0x202020;

    WINDOWCOMPOSITIONATTRIBDATA data{};
    data.Attrib = WCA_ACCENT_POLICY;
    data.pvData = &policy;
    data.cbData = sizeof(policy);

    return p(hwnd, &data) != 0;
}