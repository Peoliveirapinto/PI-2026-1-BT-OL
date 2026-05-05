#pragma once

#include "bluetooth_monitor.hpp"

#include <mutex>
#include <thread>

namespace bluetooth_overlay
{

    class OverlayWindow
    {
    public:
        explicit OverlayWindow(AppOptions options);
        ~OverlayWindow();

        int Run();

    private:
        DWORD BuildWindowExStyle() const;
        bool CreateAppWindow();
        void StartWorker();
        void StopWorker();
        void RequestRefresh();
        void UpdateLayoutAndPosition();
        void Paint(HDC hdc);
        void DrawStaticContent(HDC hdc, const RECT &client, const wchar_t *headline, const wchar_t *subline);
        void SetClickThroughEnabled(bool enabled);
        bool CreateTrayIcon();
        void DestroyTrayIcon();
        void ShowTrayContextMenu();
        void CreateFonts();
        void DestroyFonts();
        static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
        LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);

        HWND hwnd_ = nullptr;
        HANDLE stopEvent_ = nullptr;
        HANDLE manualRefreshEvent_ = nullptr;
        std::thread workerThread_;
        std::mutex snapshotMutex_;
        std::vector<DeviceSnapshot> snapshot_;
        AppOptions options_;
        bool clickThroughEnabled_ = true;
        BatteryHistoryStore history_;
        BluetoothBatteryMonitor monitor_;
        HFONT titleFont_ = nullptr;
        HFONT bodyFont_ = nullptr;
        HFONT smallFont_ = nullptr;
        NOTIFYICONDATAW trayIconData_ = {};
        bool trayIconCreated_ = false;
    };

} // namespace bluetooth_overlay
