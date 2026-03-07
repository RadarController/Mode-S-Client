#pragma once

#include <windows.h>

namespace SplashScreen {

// Creates and shows the splash window. The splash is a local HTML/CSS/JS page
// hosted inside its own WebView2 instance and does not depend on the HTTP server.
bool Create(HINSTANCE hInstance, const wchar_t* displayName, const wchar_t* versionText);

// Called when the app is ready to reveal the main window.
void OnAppReady(HWND mainWindow);

// Destroys the splash window if it exists.
void Destroy();

} // namespace SplashScreen
