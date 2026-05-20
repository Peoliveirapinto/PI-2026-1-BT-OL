#pragma once

#include <windows.h>
#include <setupapi.h>
#include <bluetoothapis.h>
#include <bluetoothleapis.h>
#include <bthledef.h>
#include <shlobj.h>
#include <shellapi.h>
#include <strsafe.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace bluetooth_overlay
{

    inline constexpr UINT WM_APP_REFRESH_UI = WM_APP + 1;
    inline constexpr UINT WM_APP_LAYOUT_UI = WM_APP + 2;
    inline constexpr UINT WM_APP_TRAYICON = WM_APP + 3;
    inline constexpr int kQuitHotkeyId = 1;
    inline constexpr int kQuickQuitHotkeyId = 2;
    inline constexpr int kToggleClickThroughHotkeyId = 3;
    inline constexpr UINT kTrayIconId = 1;
    inline constexpr UINT_PTR kTrayMenuReloadId = 2001;
    inline constexpr UINT_PTR kTrayMenuExitId = 2002;
    inline constexpr auto kRefreshInterval = std::chrono::seconds(15);
    inline constexpr int kOverlayAlpha = 232;
    inline constexpr int kOverlayWidth = 700;
    inline constexpr int kRowHeight = 80;
    inline constexpr int kTitleHeight = 64;
    inline constexpr int kFooterHeight = 60;
    inline constexpr int kPadding = 16;
    inline constexpr int kVerticalGap = 64;

    inline const GUID kBluetoothLeDeviceInterfaceGuid = {0x781aee18, 0x7733, 0x4ce4, {0xad, 0xd0, 0x91, 0xf4, 0x1c, 0x67, 0xb5, 0x92}};
    inline const DEVPROPKEY kBluetoothBatteryCandidateKey1 = {{0x670245F9, 0x6E25, 0x4179, {0x85, 0xC1, 0x98, 0x1C, 0x33, 0xB9, 0xD3, 0xB7}}, 4};
    inline const DEVPROPKEY kBluetoothBatteryCandidateKey2 = {{0x80497100, 0x8C73, 0x48B9, {0xAA, 0xD9, 0xCE, 0x38, 0x7E, 0x19, 0xC5, 0x6E}}, 6};
    inline const DEVPROPKEY kBluetoothBatteryCandidateKey3 = {{0x104EA319, 0x6EE2, 0x4701, {0xBD, 0x47, 0x8D, 0xDB, 0xF4, 0x25, 0xBB, 0xE5}}, 2};
    inline const DEVPROPKEY kBluetoothBatteryCandidateKey4 = {{0x104EA319, 0x6EE2, 0x4701, {0xBD, 0x47, 0x8D, 0xDB, 0xF4, 0x25, 0xBB, 0x14}}, 2};
    inline const DEVPROPKEY kBluetoothBatteryCandidateKey5 = {{0x104E17AB, 0x059A, 0x4876, {0xBD, 0x14, 0xBA, 0xF2, 0x8C, 0xA0, 0xA2, 0x2A}}, 2};

    struct DeviceSnapshot
    {
        std::wstring deviceId;
        std::wstring name;
        std::optional<int> batteryPercent;
        std::wstring diagnostic;
        std::optional<double> healthPercent;
        std::optional<double> remainingMinutes;
        double drainRateMilliPercentPerHour = 0.0;
    };

    struct BatteryQueryResult
    {
        std::optional<int> batteryPercent;
        std::wstring diagnostic;
    };

    struct AddressBatteryMatch
    {
        int value = -1;
        int confidence = 0;
        std::wstring source;
        std::wstring instanceId;
        std::wstring name;
        std::vector<std::wstring> candidates;
    };

    struct AppOptions
    {
        bool debugMode = false;
        bool consoleLogging = false;
    };

    extern bool g_enableDebugLogging;

    void DebugLog(const std::wstring &message);
    bool HasArgument(const std::vector<std::wstring> &args, const wchar_t *flag);
    AppOptions ParseAppOptions(PWSTR commandLine);
    std::wstring Utf8ToWide(const std::string &text);
    std::string WideToUtf8(const std::wstring &text);
    std::wstring JoinTwoDigits(int value);
    std::wstring FormatPercent(int value);
    std::wstring FormatDrainRate(double milliPercentPerHour);
    std::wstring FormatRemainingTime(double minutes);
    std::wstring FormatSystemTimeNow();
    std::wstring FormatHRESULT(HRESULT hr);
    int BatteryDiagnosticConfidence(const std::wstring &diagnostic);
    bool IsShortUuid(const BTH_LE_UUID &uuid, USHORT shortUuid);
    std::wstring GetApplicationDataDirectory();
    std::filesystem::path GetStoragePath();
    std::wstring QueryDeviceInstanceId(HDEVINFO deviceInfoSet, SP_DEVINFO_DATA &devInfoData);
    std::wstring QueryDeviceName(HDEVINFO deviceInfoSet, SP_DEVINFO_DATA &devInfoData);
    std::optional<int> TryReadUint32DeviceProperty(HDEVINFO deviceInfoSet, SP_DEVINFO_DATA &devInfoData, const DEVPROPKEY &key);
    std::optional<int> TryReadByteDeviceProperty(HDEVINFO deviceInfoSet, SP_DEVINFO_DATA &devInfoData, const DEVPROPKEY &key);
    bool IsSameDevPropKey(const DEVPROPKEY &left, const DEVPROPKEY &right);
    std::wstring FormatDevPropKey(const DEVPROPKEY &key);
    std::optional<int> TryReadNumericPercentProperty(HDEVINFO deviceInfoSet, SP_DEVINFO_DATA &devInfoData, const DEVPROPKEY &key);
    std::optional<int> QueryBatteryByScanningPropertyKeys(HDEVINFO deviceInfoSet, SP_DEVINFO_DATA &devInfoData, std::wstring &diagnostic);
    std::optional<int> QueryBatteryFromWindowsProperties(HDEVINFO deviceInfoSet, SP_DEVINFO_DATA &devInfoData, std::wstring &diagnostic);
    std::unordered_map<std::wstring, AddressBatteryMatch> QueryBatteryByAddressFromAllPresentDevices();
    bool IsStartedDevNode(const SP_DEVINFO_DATA &devInfoData);
    HANDLE OpenBluetoothLeHandle(const wchar_t *devicePath);
    std::wstring FormatBluetoothAddress(ULONGLONG address);
    std::wstring SnapshotKey(const DeviceSnapshot &snapshot);
    std::optional<std::wstring> ExtractBluetoothAddressToken(const std::wstring &text);
    void RemoveBatteryUnavailableSuffix(std::wstring &name);
    std::wstring NormalizeDeviceName(const std::wstring &name);
    std::vector<DeviceSnapshot> EnumerateClassicBluetoothDevices();
    BatteryQueryResult QueryBatteryLevel(HANDLE deviceHandle);

} // namespace bluetooth_overlay
