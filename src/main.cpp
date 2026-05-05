#include "overlay_window.hpp"

#include <cstdio>
#include <objbase.h>

using namespace bluetooth_overlay;

int APIENTRY wWinMain(HINSTANCE, HINSTANCE, PWSTR cmdLine, int)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    const AppOptions options = ParseAppOptions(cmdLine);
    g_enableDebugLogging = options.consoleLogging;

    if (options.consoleLogging)
    {
        AllocConsole();
        FILE *unused = nullptr;
        freopen_s(&unused, "CONOUT$", "w", stdout);
        freopen_s(&unused, "CONOUT$", "w", stderr);
        SetConsoleTitleW(L"Bluetooth Battery Overlay Debug Console");
        DebugLog(L"Console logging enabled");
    }

    OverlayWindow app(options);
    const int exitCode = app.Run();

    if (options.consoleLogging)
    {
        DebugLog(L"Application exiting");
        FreeConsole();
    }

    CoUninitialize();
    return exitCode;
}
