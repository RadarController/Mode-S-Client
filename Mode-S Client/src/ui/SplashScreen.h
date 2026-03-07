#pragma once

#include <windows.h>

namespace SplashScreen {

// Creates and shows the splash window.
// displayName and versionText are currently accepted for compatibility with
// Mode-S Client.cpp, even though the new image-based splash does not need to
// render them as text.
bool Create(HINSTANCE hInstance, const wchar_t* displayName, const wchar_t* versionText);

// Called when the app is ready to reveal the main window.
void OnAppReady(HWND mainWindow);

// Destroys the splash window if it exists.
void Destroy();

} // namespace SplashScreen
