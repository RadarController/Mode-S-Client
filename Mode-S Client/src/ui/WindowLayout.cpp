#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "ui/WindowLayout.h"

RECT CenteredWindowRect(int width, int height)
{
    RECT wa{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);

    RECT rc{ 0, 0, width, height };
    AdjustWindowRectEx(&rc, WS_OVERLAPPEDWINDOW, FALSE, 0);

    const int windowWidth = rc.right - rc.left;
    const int windowHeight = rc.bottom - rc.top;

    const int x = wa.left + ((wa.right - wa.left) - windowWidth) / 2;
    const int y = wa.top + ((wa.bottom - wa.top) - windowHeight) / 2;

    return RECT{ x, y, x + windowWidth, y + windowHeight };
}