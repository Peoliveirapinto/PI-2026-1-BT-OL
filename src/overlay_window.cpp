#include "overlay_window.hpp"

#include <algorithm>
#include <thread>

namespace bluetooth_overlay
{

    OverlayWindow::OverlayWindow(AppOptions options) : options_(options), clickThroughEnabled_(!options.debugMode), monitor_(history_) {}

    OverlayWindow::~OverlayWindow()
    {
        StopWorker();
        DestroyTrayIcon();
        DestroyFonts();
    }

    int OverlayWindow::Run()
    {
        if (!CreateAppWindow())
        {
            return 1;
        }

        ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
        if (options_.debugMode)
        {
            ShowWindow(hwnd_, SW_SHOW);
            SetForegroundWindow(hwnd_);
        }
        UpdateLayoutAndPosition();
        StartWorker();
        RequestRefresh();

        DebugLog(options_.debugMode ? L"Debug mode enabled" : L"Overlay mode enabled");

        MSG message = {};
        while (GetMessageW(&message, nullptr, 0, 0) > 0)
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }

        StopWorker();
        return static_cast<int>(message.wParam);
    }

    DWORD OverlayWindow::BuildWindowExStyle() const
    {
        DWORD exStyle = WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TOOLWINDOW;
        if (clickThroughEnabled_)
        {
            exStyle |= WS_EX_TRANSPARENT | WS_EX_NOACTIVATE;
        }
        return exStyle;
    }

    bool OverlayWindow::CreateAppWindow()
    {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpfnWndProc = &OverlayWindow::WndProc;
        wc.lpszClassName = L"BluetoothBatteryOverlayWindow";
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;

        if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        {
            return false;
        }

        hwnd_ = CreateWindowExW(
            BuildWindowExStyle(),
            wc.lpszClassName,
            L"Bluetooth Battery Overlay",
            options_.debugMode ? WS_OVERLAPPEDWINDOW : (WS_POPUP | WS_THICKFRAME),
            0,
            0,
            kOverlayWidth,
            220,
            nullptr,
            nullptr,
            wc.hInstance,
            this);

        if (!hwnd_)
        {
            return false;
        }

        CreateFonts();
        SetLayeredWindowAttributes(hwnd_, 0, static_cast<BYTE>(kOverlayAlpha), LWA_ALPHA);
        RegisterHotKey(hwnd_, kQuitHotkeyId, MOD_CONTROL | MOD_SHIFT, 'Q');
        RegisterHotKey(hwnd_, kQuickQuitHotkeyId, MOD_CONTROL | MOD_ALT, 'Q');
        RegisterHotKey(hwnd_, kToggleClickThroughHotkeyId, MOD_CONTROL | MOD_ALT, 'T');
        CreateTrayIcon();
        return true;
    }

    void OverlayWindow::StartWorker()
    {
        stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        manualRefreshEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        workerThread_ = std::thread([this]
                                    {
            auto refreshAndPublish = [this]() {
                auto snapshot = monitor_.Refresh();
                const size_t snapshotCount = snapshot.size();

                if (g_enableDebugLogging)
                {
                    for (const auto &device : snapshot)
                    {
                        std::wstring line = L"Device: " + device.name;
                        line += L" | battery=";
                        if (device.batteryPercent.has_value())
                        {
                            line += FormatPercent(*device.batteryPercent);
                        }
                        else
                        {
                            line += L"n/a";
                        }

                        if (!device.diagnostic.empty())
                        {
                            line += L" | diag=" + device.diagnostic;
                        }
                        DebugLog(line);
                    }
                }

                {
                    std::scoped_lock lock(snapshotMutex_);
                    snapshot_ = std::move(snapshot);
                }

                if (hwnd_)
                {
                    PostMessageW(hwnd_, WM_APP_REFRESH_UI, 0, 0);
                }

                if (options_.debugMode)
                {
                    wchar_t msg[80] = {};
                    StringCchPrintfW(msg, 80, L"Refreshed %zu device(s)", snapshotCount);
                    DebugLog(msg);
                }
            };

            refreshAndPublish();

            HANDLE waitHandles[2] = {stopEvent_, manualRefreshEvent_};
            while (true)
            {
                const DWORD waitResult = WaitForMultipleObjects(2, waitHandles, FALSE, static_cast<DWORD>(kRefreshInterval.count() * 1000));
                if (waitResult == WAIT_OBJECT_0)
                {
                    break;
                }

                if (waitResult == WAIT_OBJECT_0 + 1)
                {
                    ResetEvent(manualRefreshEvent_);
                    refreshAndPublish();
                    continue;
                }

                if (waitResult == WAIT_TIMEOUT)
                {
                    refreshAndPublish();
                    continue;
                }

                break;
            } });
    }

    void OverlayWindow::StopWorker()
    {
        if (stopEvent_)
        {
            SetEvent(stopEvent_);
        }

        if (manualRefreshEvent_)
        {
            SetEvent(manualRefreshEvent_);
        }

        if (workerThread_.joinable())
        {
            workerThread_.join();
        }

        if (stopEvent_)
        {
            CloseHandle(stopEvent_);
            stopEvent_ = nullptr;
        }

        if (manualRefreshEvent_)
        {
            CloseHandle(manualRefreshEvent_);
            manualRefreshEvent_ = nullptr;
        }
    }

    void OverlayWindow::RequestRefresh()
    {
        if (manualRefreshEvent_)
        {
            SetEvent(manualRefreshEvent_);
        }
        else
        {
            PostMessageW(hwnd_, WM_APP_REFRESH_UI, 0, 0);
        }
    }

    void OverlayWindow::UpdateLayoutAndPosition()
    {
        std::vector<DeviceSnapshot> localSnapshot;
        {
            std::scoped_lock lock(snapshotMutex_);
            localSnapshot = snapshot_;
        }

        const int rowCount = static_cast<int>(std::max<size_t>(1, localSnapshot.size()));
        const int requiredHeight = kTitleHeight + (rowCount * kRowHeight) + kVerticalGap + kFooterHeight + 32;

        int x, y, width, height;

        if (!windowStateInitialized_)
        {
            width = kOverlayWidth;
            height = requiredHeight;

            RECT workArea = {};
            SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
            x = workArea.right - width - 24;
            y = workArea.top + 24;

            windowStateInitialized_ = true;
            lastWindowRect_ = { x, y, x + width, y + height };
        }
        else
        {
            x = lastWindowRect_.left;
            y = lastWindowRect_.top;
            width = lastWindowRect_.right - lastWindowRect_.left;
            
            height = lastWindowRect_.bottom - lastWindowRect_.top;
            if (height < requiredHeight) {
                height = requiredHeight;
            }
        }

        UINT flags = SWP_NOSENDCHANGING | SWP_NOZORDER | SWP_NOACTIVATE;
        if (!options_.debugMode)
        {
            flags |= SWP_SHOWWINDOW;
        }
        
        SetWindowPos(hwnd_, HWND_TOPMOST, x, y, width, height, flags);
        InvalidateRect(hwnd_, nullptr, TRUE);
    }

    void OverlayWindow::Paint(HDC hdc)
    {
        RECT client = {};
        GetClientRect(hwnd_, &client);

        HBRUSH background = CreateSolidBrush(RGB(20, 22, 28));
        FillRect(hdc, &client, background);
        DeleteObject(background);

        HPEN borderPen = CreatePen(PS_SOLID, 1, RGB(52, 58, 72));
        HBRUSH transparentBrush = static_cast<HBRUSH>(GetStockObject(NULL_BRUSH));
        HGDIOBJ oldPen = SelectObject(hdc, borderPen);
        HGDIOBJ oldBrush = SelectObject(hdc, transparentBrush);
        RoundRect(hdc, 0, 0, client.right, client.bottom, 18, 18);
        SelectObject(hdc, oldBrush);
        SelectObject(hdc, oldPen);
        DeleteObject(borderPen);

        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(244, 247, 250));

        std::vector<DeviceSnapshot> localSnapshot;
        {
            std::scoped_lock lock(snapshotMutex_);
            localSnapshot = snapshot_;
        }

        if (localSnapshot.empty())
        {
            DrawStaticContent(hdc, client, L"No connected Bluetooth LE battery devices found.", L"The overlay will update automatically when one appears.");
            return;
        }

        RECT titleRect = {kPadding, kPadding, client.right - 460, kPadding + 28};
        SelectObject(hdc, titleFont_);
        DrawTextW(hdc, L"Bluetooth battery", -1, &titleRect, DT_SINGLELINE | DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);

        RECT hintRect = {client.right - 450, kPadding + 4, client.right - kPadding, kPadding + 24};
        SetTextColor(hdc, RGB(170, 178, 190));
        SelectObject(hdc, smallFont_);
        DrawTextW(hdc, L"Ctrl+Alt+Q quit | Ctrl+Alt+T toggle pass-through", -1, &hintRect, DT_SINGLELINE | DT_RIGHT | DT_VCENTER);

        int top = kTitleHeight;
        for (const auto &snapshot : localSnapshot)
        {
            RECT row = {kPadding, top, client.right - kPadding, top + kRowHeight - 8};
            HBRUSH rowBrush = CreateSolidBrush(RGB(28, 32, 40));
            FillRect(hdc, &row, rowBrush);
            DeleteObject(rowBrush);

            HPEN rowPen = CreatePen(PS_SOLID, 1, RGB(44, 50, 62));
            HGDIOBJ oldRowPen = SelectObject(hdc, rowPen);
            HGDIOBJ oldRowBrush = SelectObject(hdc, transparentBrush);
            RoundRect(hdc, row.left, row.top, row.right, row.bottom, 12, 12);
            SelectObject(hdc, oldRowBrush);
            SelectObject(hdc, oldRowPen);
            DeleteObject(rowPen);

            RECT nameRect = {row.left + 16, row.top + 10, row.right - 100, row.top + 34};
            SetTextColor(hdc, RGB(246, 248, 252));
            SelectObject(hdc, bodyFont_);
            DrawTextW(hdc, snapshot.name.c_str(), -1, &nameRect, DT_SINGLELINE | DT_LEFT | DT_END_ELLIPSIS);

            RECT percentRect = {row.right - 92, row.top + 10, row.right - 16, row.top + 32};
            SetTextColor(hdc, RGB(160, 235, 180));
            SelectObject(hdc, titleFont_);
            if (snapshot.batteryPercent.has_value())
            {
                const std::wstring percentText = FormatPercent(*snapshot.batteryPercent);
                DrawTextW(hdc, percentText.c_str(), -1, &percentRect, DT_SINGLELINE | DT_RIGHT | DT_VCENTER);
            }
            else
            {
                DrawTextW(hdc, L"n/a", -1, &percentRect, DT_SINGLELINE | DT_RIGHT | DT_VCENTER);
            }

            int barTop = row.bottom - 18;
            RECT barBack = {row.left + 16, barTop, row.right - 16, barTop + 8};
            HBRUSH barBackground = CreateSolidBrush(RGB(40, 46, 58));
            FillRect(hdc, &barBack, barBackground);
            DeleteObject(barBackground);

            if (snapshot.batteryPercent.has_value())
            {
                const int percent = std::clamp(*snapshot.batteryPercent, 0, 100);
                const int barWidth = static_cast<int>(barBack.right - barBack.left);
                const int filledWidth = std::max(2, (barWidth * percent) / 100);
                RECT barFill = {barBack.left, barBack.top, barBack.left + filledWidth, barBack.bottom};
                COLORREF fillColor = RGB(92, 215, 140);
                if (percent < 35)
                {
                    fillColor = RGB(247, 167, 87);
                }
                if (percent < 15)
                {
                    fillColor = RGB(241, 96, 96);
                }
                HBRUSH barBrush = CreateSolidBrush(fillColor);
                FillRect(hdc, &barFill, barBrush);
                DeleteObject(barBrush);
            }

            top += kRowHeight;
        }

        RECT footerRect = {kPadding, client.bottom - kFooterHeight - 8, client.right - kPadding, client.bottom - 8};
        SetTextColor(hdc, RGB(155, 162, 174));
        SelectObject(hdc, smallFont_);
        DrawTextW(hdc, L"Health is estimated from observed drain history when the device does not expose a direct health metric.", -1, &footerRect, DT_WORDBREAK | DT_LEFT | DT_VCENTER);
    }

    void OverlayWindow::DrawStaticContent(HDC hdc, const RECT &client, const wchar_t *headline, const wchar_t *subline)
    {
        RECT head = {kPadding, kPadding + 2, client.right - kPadding, kPadding + 30};
        SelectObject(hdc, titleFont_);
        SetTextColor(hdc, RGB(246, 248, 252));
        DrawTextW(hdc, headline, -1, &head, DT_SINGLELINE | DT_LEFT | DT_VCENTER);

        RECT sub = {kPadding, kPadding + 34, client.right - kPadding, client.bottom - kPadding};
        SelectObject(hdc, bodyFont_);
        SetTextColor(hdc, RGB(179, 186, 198));
        DrawTextW(hdc, subline, -1, &sub, DT_WORDBREAK | DT_LEFT | DT_TOP);
    }

    void OverlayWindow::SetClickThroughEnabled(bool enabled)
    {
        clickThroughEnabled_ = enabled;
        if (!hwnd_)
        {
            return;
        }

        SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, static_cast<LONG_PTR>(BuildWindowExStyle()));

        UINT flags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW;
        if (clickThroughEnabled_)
        {
            flags |= SWP_NOACTIVATE;
        }

        SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, flags);
        InvalidateRect(hwnd_, nullptr, TRUE);

        DebugLog(clickThroughEnabled_ ? L"Click-through enabled" : L"Click-through disabled");
    }

    bool OverlayWindow::CreateTrayIcon()
    {
        ZeroMemory(&trayIconData_, sizeof(trayIconData_));
        trayIconData_.cbSize = sizeof(trayIconData_);
        trayIconData_.hWnd = hwnd_;
        trayIconData_.uID = kTrayIconId;
        trayIconData_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        trayIconData_.uCallbackMessage = WM_APP_TRAYICON;
        trayIconData_.hIcon = LoadIconW(nullptr, IDI_INFORMATION);
        StringCchCopyW(trayIconData_.szTip, std::size(trayIconData_.szTip), L"Bluetooth Battery Overlay");

        if (!Shell_NotifyIconW(NIM_ADD, &trayIconData_))
        {
            return false;
        }

        trayIconCreated_ = true;
        trayIconData_.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &trayIconData_);
        return true;
    }

    void OverlayWindow::DestroyTrayIcon()
    {
        if (!trayIconCreated_)
        {
            return;
        }

        Shell_NotifyIconW(NIM_DELETE, &trayIconData_);
        trayIconCreated_ = false;
    }

    void OverlayWindow::ShowTrayContextMenu()
    {
        HMENU menu = CreatePopupMenu();
        if (!menu)
        {
            return;
        }

        AppendMenuW(menu, MF_STRING, kTrayMenuReloadId, L"Reload now");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kTrayMenuExitId, L"Exit");

        POINT cursor = {};
        GetCursorPos(&cursor);

        SetForegroundWindow(hwnd_);
        const UINT selected = TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD, cursor.x, cursor.y, 0, hwnd_, nullptr);
        if (selected != 0)
        {
            PostMessageW(hwnd_, WM_COMMAND, selected, 0);
        }

        DestroyMenu(menu);
    }

    void OverlayWindow::CreateFonts()
    {
        if (!titleFont_)
        {
            titleFont_ = CreateFontW(-24, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI Semibold");
        }
        if (!bodyFont_)
        {
            bodyFont_ = CreateFontW(-18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        }
        if (!smallFont_)
        {
            smallFont_ = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        }
    }

    void OverlayWindow::DestroyFonts()
    {
        if (titleFont_)
        {
            DeleteObject(titleFont_);
            titleFont_ = nullptr;
        }
        if (bodyFont_)
        {
            DeleteObject(bodyFont_);
            bodyFont_ = nullptr;
        }
        if (smallFont_)
        {
            DeleteObject(smallFont_);
            smallFont_ = nullptr;
        }
    }

    LRESULT CALLBACK OverlayWindow::WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        OverlayWindow *self = nullptr;
        if (message == WM_NCCREATE)
        {
            auto *create = reinterpret_cast<CREATESTRUCTW *>(lParam);
            self = static_cast<OverlayWindow *>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->hwnd_ = hwnd;
        }
        else
        {
            self = reinterpret_cast<OverlayWindow *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }

        if (self)
        {
            return self->HandleMessage(message, wParam, lParam);
        }

        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    LRESULT OverlayWindow::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam)
    {
        switch (message)
        {
        case WM_CREATE:
            return 0;
        case WM_PAINT:
        {
            PAINTSTRUCT paint = {};
            HDC hdc = BeginPaint(hwnd_, &paint);
            Paint(hdc);
            EndPaint(hwnd_, &paint);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_SIZE:
            if (windowStateInitialized_ && !inInternalPosUpdate_ && wParam != SIZE_MINIMIZED)
            {
                GetWindowRect(hwnd_, &lastWindowRect_);
            }
            return 0;
        case WM_MOVE:
            if (windowStateInitialized_ && !inInternalPosUpdate_)
            {
                GetWindowRect(hwnd_, &lastWindowRect_);
            }
            return 0;
        case WM_NCHITTEST:
            if (!clickThroughEnabled_)
            {
                return DefWindowProcW(hwnd_, message, wParam, lParam);
            }
            return HTTRANSPARENT;
        case WM_HOTKEY:
            if (wParam == kQuitHotkeyId || wParam == kQuickQuitHotkeyId)
            {
                DebugLog(L"Quit hotkey pressed");
                PostQuitMessage(0);
            }
            else if (wParam == kToggleClickThroughHotkeyId)
            {
                SetClickThroughEnabled(!clickThroughEnabled_);
            }
            return 0;
        case WM_COMMAND:
            if (LOWORD(wParam) == kTrayMenuReloadId)
            {
                DebugLog(L"Manual refresh requested from tray menu");
                RequestRefresh();
                return 0;
            }
            if (LOWORD(wParam) == kTrayMenuExitId)
            {
                DebugLog(L"Exit requested from tray menu");
                PostQuitMessage(0);
                return 0;
            }
            return DefWindowProcW(hwnd_, message, wParam, lParam);
        case WM_APP_TRAYICON:
            if (LOWORD(lParam) == WM_RBUTTONUP || LOWORD(lParam) == WM_CONTEXTMENU)
            {
                ShowTrayContextMenu();
            }
            else if (LOWORD(lParam) == WM_LBUTTONDBLCLK)
            {
                SetClickThroughEnabled(false);
                ShowWindow(hwnd_, SW_SHOW);
                SetForegroundWindow(hwnd_);
            }
            return 0;
        case WM_APP_REFRESH_UI:
            UpdateLayoutAndPosition();
            return 0;
        case WM_APP_LAYOUT_UI:
            UpdateLayoutAndPosition();
            return 0;
        case WM_DESTROY:
            UnregisterHotKey(hwnd_, kQuitHotkeyId);
            UnregisterHotKey(hwnd_, kQuickQuitHotkeyId);
            UnregisterHotKey(hwnd_, kToggleClickThroughHotkeyId);
            DestroyTrayIcon();
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd_, message, wParam, lParam);
        }
    }

} // namespace bluetooth_overlay
