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
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "bluetoothapis.lib")
#pragma comment(lib, "bthprops.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

namespace
{

    constexpr UINT WM_APP_REFRESH_UI = WM_APP + 1;
    constexpr UINT WM_APP_LAYOUT_UI = WM_APP + 2;
    constexpr UINT WM_APP_TRAYICON = WM_APP + 3;
    constexpr int kQuitHotkeyId = 1;
    constexpr int kQuickQuitHotkeyId = 2;
    constexpr int kToggleClickThroughHotkeyId = 3;
    constexpr UINT kTrayIconId = 1;
    constexpr UINT_PTR kTrayMenuReloadId = 2001;
    constexpr UINT_PTR kTrayMenuExitId = 2002;
    constexpr auto kRefreshInterval = std::chrono::seconds(15);
    constexpr int kOverlayAlpha = 232;
    constexpr int kOverlayWidth = 460;
    constexpr int kRowHeight = 64;
    constexpr int kTitleHeight = 44;
    constexpr int kFooterHeight = 28;
    constexpr int kPadding = 16;

    const GUID kBluetoothLeDeviceInterfaceGuid = {0x781aee18, 0x7733, 0x4ce4, {0xad, 0xd0, 0x91, 0xf4, 0x1c, 0x67, 0xb5, 0x92}};
    const DEVPROPKEY kBluetoothBatteryCandidateKey1 = {{0x670245F9, 0x6E25, 0x4179, {0x85, 0xC1, 0x98, 0x1C, 0x33, 0xB9, 0xD3, 0xB7}}, 4};
    const DEVPROPKEY kBluetoothBatteryCandidateKey2 = {{0x80497100, 0x8C73, 0x48B9, {0xAA, 0xD9, 0xCE, 0x38, 0x7E, 0x19, 0xC5, 0x6E}}, 6};

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
        std::vector<std::wstring> candidates;
    };

    struct AppOptions
    {
        bool debugMode = false;
        bool consoleLogging = false;
    };

    bool g_enableDebugLogging = false;

    void DebugLog(const std::wstring &message)
    {
        if (!g_enableDebugLogging)
        {
            return;
        }

        std::wstring line = L"[BluetoothBatteryOverlay] " + message + L"\r\n";
        OutputDebugStringW(line.c_str());

        HANDLE console = GetStdHandle(STD_OUTPUT_HANDLE);
        if (console && console != INVALID_HANDLE_VALUE)
        {
            DWORD written = 0;
            WriteConsoleW(console, line.c_str(), static_cast<DWORD>(line.size()), &written, nullptr);
        }
    }

    bool HasArgument(const std::vector<std::wstring> &args, const wchar_t *flag)
    {
        return std::find(args.begin(), args.end(), flag) != args.end();
    }

    AppOptions ParseAppOptions(PWSTR commandLine)
    {
        AppOptions options;

        int argc = 0;
        LPWSTR *argv = CommandLineToArgvW(commandLine, &argc);
        if (!argv)
        {
            return options;
        }

        std::vector<std::wstring> args;
        args.reserve(static_cast<size_t>(argc));
        for (int i = 0; i < argc; ++i)
        {
            args.push_back(argv[i]);
        }
        LocalFree(argv);

        options.debugMode = HasArgument(args, L"--debug") || HasArgument(args, L"-d");
        options.consoleLogging = HasArgument(args, L"--console") || options.debugMode;
        return options;
    }

    struct DeviceStats
    {
        int lastPercent = -1;
        uint64_t lastSampleUnixSeconds = 0;
        int64_t ewmaDrainRateMilliPercentPerHour = 0;
        int64_t baselineDrainRateMilliPercentPerHour = 0;
        int sampleCount = 0;
        bool hasSample = false;
    };

    std::wstring Utf8ToWide(const std::string &text)
    {
        if (text.empty())
        {
            return {};
        }

        int required = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
        if (required <= 0)
        {
            return {};
        }

        std::wstring wide(required, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), required);
        return wide;
    }

    std::string WideToUtf8(const std::wstring &text)
    {
        if (text.empty())
        {
            return {};
        }

        int required = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
        if (required <= 0)
        {
            return {};
        }

        std::string narrow(required, '\0');
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), narrow.data(), required, nullptr, nullptr);
        return narrow;
    }

    std::wstring JoinTwoDigits(int value)
    {
        wchar_t buffer[8] = {};
        StringCchPrintfW(buffer, 8, L"%02d", value);
        return buffer;
    }

    std::wstring FormatPercent(int value)
    {
        wchar_t buffer[16] = {};
        StringCchPrintfW(buffer, 16, L"%d%%", value);
        return buffer;
    }

    std::wstring FormatDrainRate(double milliPercentPerHour)
    {
        if (milliPercentPerHour <= 0.0)
        {
            return L"n/a";
        }

        const double percentPerHour = milliPercentPerHour / 1000.0;
        wchar_t buffer[32] = {};
        StringCchPrintfW(buffer, 32, L"%.2f%%/h", percentPerHour);
        return buffer;
    }

    std::wstring FormatRemainingTime(double minutes)
    {
        if (minutes <= 0.0 || !std::isfinite(minutes))
        {
            return L"n/a";
        }

        const int totalMinutes = static_cast<int>(std::lround(minutes));
        const int hours = totalMinutes / 60;
        const int mins = totalMinutes % 60;

        wchar_t buffer[32] = {};
        if (hours > 0)
        {
            StringCchPrintfW(buffer, 32, L"%dh %02dm", hours, mins);
        }
        else
        {
            StringCchPrintfW(buffer, 32, L"%dm", mins);
        }

        return buffer;
    }

    std::wstring FormatSystemTimeNow()
    {
        SYSTEMTIME now = {};
        GetLocalTime(&now);
        wchar_t buffer[32] = {};
        StringCchPrintfW(buffer, 32, L"%02d:%02d:%02d", now.wHour, now.wMinute, now.wSecond);
        return buffer;
    }

    std::wstring FormatHRESULT(HRESULT hr)
    {
        wchar_t buffer[24] = {};
        StringCchPrintfW(buffer, std::size(buffer), L"0x%08X", static_cast<unsigned int>(hr));
        return buffer;
    }

    int BatteryDiagnosticConfidence(const std::wstring &diagnostic)
    {
        if (diagnostic.find(L"windows property fallback") != std::wstring::npos)
        {
            return 3;
        }

        if (diagnostic.find(L"windows key-scan fallback") != std::wstring::npos)
        {
            return 1;
        }

        return 0;
    }

    bool IsShortUuid(const BTH_LE_UUID &uuid, USHORT shortUuid)
    {
        return uuid.IsShortUuid && uuid.Value.ShortUuid == shortUuid;
    }

    std::wstring GetApplicationDataDirectory()
    {
        PWSTR rawPath = nullptr;
        if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &rawPath)) || rawPath == nullptr)
        {
            return {};
        }

        std::wstring path = rawPath;
        CoTaskMemFree(rawPath);
        return path;
    }

    std::filesystem::path GetStoragePath()
    {
        std::filesystem::path base = GetApplicationDataDirectory();
        if (base.empty())
        {
            base = L".";
        }

        base /= L"BluetoothBatteryOverlay";
        std::error_code ignored;
        std::filesystem::create_directories(base, ignored);
        return base / L"device-stats.tsv";
    }

    struct DeviceInterfaceHandle
    {
        HDEVINFO value = INVALID_HANDLE_VALUE;

        ~DeviceInterfaceHandle()
        {
            if (value != INVALID_HANDLE_VALUE)
            {
                SetupDiDestroyDeviceInfoList(value);
            }
        }
    };

    std::optional<std::wstring> ExtractBluetoothAddressToken(const std::wstring &text);
    void RemoveBatteryUnavailableSuffix(std::wstring &name);

    std::wstring QueryDeviceInstanceId(HDEVINFO deviceInfoSet, SP_DEVINFO_DATA &devInfoData)
    {
        wchar_t buffer[512] = {};
        if (!SetupDiGetDeviceInstanceIdW(deviceInfoSet, &devInfoData, buffer, static_cast<DWORD>(std::size(buffer)), nullptr))
        {
            return {};
        }

        return buffer;
    }

    std::wstring QueryDeviceName(HDEVINFO deviceInfoSet, SP_DEVINFO_DATA &devInfoData)
    {
        wchar_t buffer[512] = {};
        DWORD propertyType = 0;
        if (SetupDiGetDeviceRegistryPropertyW(deviceInfoSet, &devInfoData, SPDRP_FRIENDLYNAME, &propertyType, reinterpret_cast<PBYTE>(buffer), static_cast<DWORD>(sizeof(buffer)), nullptr))
        {
            if (buffer[0] != L'\0')
            {
                return buffer;
            }
        }

        ZeroMemory(buffer, sizeof(buffer));
        if (SetupDiGetDeviceRegistryPropertyW(deviceInfoSet, &devInfoData, SPDRP_DEVICEDESC, &propertyType, reinterpret_cast<PBYTE>(buffer), static_cast<DWORD>(sizeof(buffer)), nullptr))
        {
            if (buffer[0] != L'\0')
            {
                return buffer;
            }
        }

        return L"Unknown Bluetooth device";
    }

    std::optional<int> TryReadUint32DeviceProperty(HDEVINFO deviceInfoSet, SP_DEVINFO_DATA &devInfoData, const DEVPROPKEY &key)
    {
        DEVPROPTYPE type = 0;
        DWORD value = 0;
        DWORD required = 0;
        if (!SetupDiGetDevicePropertyW(deviceInfoSet, &devInfoData, &key, &type, reinterpret_cast<PBYTE>(&value), sizeof(value), &required, 0))
        {
            return std::nullopt;
        }

        if (type != DEVPROP_TYPE_UINT32 && type != DEVPROP_TYPE_INT32)
        {
            return std::nullopt;
        }

        if (value <= 100)
        {
            return static_cast<int>(value);
        }

        return std::nullopt;
    }

    std::optional<int> TryReadByteDeviceProperty(HDEVINFO deviceInfoSet, SP_DEVINFO_DATA &devInfoData, const DEVPROPKEY &key)
    {
        DEVPROPTYPE type = 0;
        BYTE value = 0;
        DWORD required = 0;
        if (!SetupDiGetDevicePropertyW(deviceInfoSet, &devInfoData, &key, &type, &value, sizeof(value), &required, 0))
        {
            return std::nullopt;
        }

        if (type != DEVPROP_TYPE_BYTE)
        {
            return std::nullopt;
        }

        if (value <= 100)
        {
            return static_cast<int>(value);
        }

        return std::nullopt;
    }

    bool IsSameDevPropKey(const DEVPROPKEY &left, const DEVPROPKEY &right)
    {
        return IsEqualGUID(left.fmtid, right.fmtid) && left.pid == right.pid;
    }

    std::wstring FormatDevPropKey(const DEVPROPKEY &key)
    {
        wchar_t guidBuffer[64] = {};
        if (StringFromGUID2(key.fmtid, guidBuffer, static_cast<int>(std::size(guidBuffer))) <= 0)
        {
            StringCchCopyW(guidBuffer, std::size(guidBuffer), L"{unknown-guid}");
        }

        wchar_t buffer[96] = {};
        StringCchPrintfW(buffer, std::size(buffer), L"%ls pid %u", guidBuffer, key.pid);
        return buffer;
    }

    std::optional<int> TryReadNumericPercentProperty(HDEVINFO deviceInfoSet, SP_DEVINFO_DATA &devInfoData, const DEVPROPKEY &key)
    {
        if (auto byteValue = TryReadByteDeviceProperty(deviceInfoSet, devInfoData, key); byteValue.has_value() && *byteValue > 0)
        {
            return byteValue;
        }

        if (auto dwordValue = TryReadUint32DeviceProperty(deviceInfoSet, devInfoData, key); dwordValue.has_value() && *dwordValue > 0)
        {
            return dwordValue;
        }

        return std::nullopt;
    }

    std::optional<int> QueryBatteryByScanningPropertyKeys(HDEVINFO deviceInfoSet, SP_DEVINFO_DATA &devInfoData, std::wstring &diagnostic)
    {
        DWORD keyCount = 0;
        SetupDiGetDevicePropertyKeys(deviceInfoSet, &devInfoData, nullptr, 0, &keyCount, 0);
        if (keyCount == 0)
        {
            return std::nullopt;
        }

        std::vector<DEVPROPKEY> keys(keyCount);
        if (!SetupDiGetDevicePropertyKeys(deviceInfoSet, &devInfoData, keys.data(), static_cast<DWORD>(keys.size()), &keyCount, 0))
        {
            return std::nullopt;
        }

        std::optional<int> bestValue;
        std::wstring bestKey;
        for (DWORD i = 0; i < keyCount; ++i)
        {
            const DEVPROPKEY &key = keys[i];
            if (IsSameDevPropKey(key, kBluetoothBatteryCandidateKey1) || IsSameDevPropKey(key, kBluetoothBatteryCandidateKey2))
            {
                continue;
            }

            auto value = TryReadNumericPercentProperty(deviceInfoSet, devInfoData, key);
            if (!value.has_value())
            {
                continue;
            }

            if (!bestValue.has_value() || *value > *bestValue)
            {
                bestValue = value;
                bestKey = FormatDevPropKey(key);
            }
        }

        if (bestValue.has_value())
        {
            diagnostic = L"windows key-scan fallback " + bestKey;
            return bestValue;
        }

        return std::nullopt;
    }

    std::optional<int> QueryBatteryFromWindowsProperties(HDEVINFO deviceInfoSet, SP_DEVINFO_DATA &devInfoData, std::wstring &diagnostic)
    {
        if (auto value = TryReadNumericPercentProperty(deviceInfoSet, devInfoData, kBluetoothBatteryCandidateKey1); value.has_value())
        {
            diagnostic = L"windows property fallback {670245F9...} pid 4";
            return value;
        }

        if (auto value = TryReadNumericPercentProperty(deviceInfoSet, devInfoData, kBluetoothBatteryCandidateKey2); value.has_value())
        {
            diagnostic = L"windows property fallback {80497100...} pid 6";
            return value;
        }

        if (auto value = QueryBatteryByScanningPropertyKeys(deviceInfoSet, devInfoData, diagnostic); value.has_value())
        {
            return value;
        }

        return std::nullopt;
    }

    std::unordered_map<std::wstring, AddressBatteryMatch> QueryBatteryByAddressFromAllPresentDevices()
    {
        std::unordered_map<std::wstring, AddressBatteryMatch> values;

        DeviceInterfaceHandle allDevices;
        allDevices.value = SetupDiGetClassDevsW(nullptr, nullptr, nullptr, DIGCF_PRESENT | DIGCF_ALLCLASSES);
        if (allDevices.value == INVALID_HANDLE_VALUE)
        {
            return values;
        }

        for (DWORD index = 0;; ++index)
        {
            SP_DEVINFO_DATA devInfo = {};
            devInfo.cbSize = sizeof(devInfo);
            if (!SetupDiEnumDeviceInfo(allDevices.value, index, &devInfo))
            {
                break;
            }

            std::wstring instanceId = QueryDeviceInstanceId(allDevices.value, devInfo);
            auto address = ExtractBluetoothAddressToken(instanceId);
            if (!address.has_value())
            {
                continue;
            }

            std::wstring diagnostic;
            auto battery = QueryBatteryFromWindowsProperties(allDevices.value, devInfo, diagnostic);
            if (!battery.has_value())
            {
                continue;
            }

            AddressBatteryMatch &match = values[*address];
            std::wstring candidate = std::to_wstring(*battery) + L"% from " + diagnostic;
            match.candidates.push_back(candidate);

            const int confidence = BatteryDiagnosticConfidence(diagnostic);
            const bool replaceCurrent = (match.value < 0) ||
                                        (confidence > match.confidence) ||
                                        (confidence == match.confidence && *battery > match.value);

            if (replaceCurrent)
            {
                match.value = *battery;
                match.confidence = confidence;
                match.source = diagnostic;
                match.instanceId = instanceId;
            }
        }

        return values;
    }

    HANDLE OpenBluetoothLeHandle(const wchar_t *devicePath)
    {
        struct Attempt
        {
            DWORD desiredAccess;
            DWORD flags;
        };

        const Attempt attempts[] = {
            {GENERIC_READ | GENERIC_WRITE, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED},
            {GENERIC_READ, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED},
            {GENERIC_READ | GENERIC_WRITE, FILE_ATTRIBUTE_NORMAL},
            {GENERIC_READ, FILE_ATTRIBUTE_NORMAL},
            {0, FILE_ATTRIBUTE_NORMAL},
        };

        for (const auto &attempt : attempts)
        {
            HANDLE handle = CreateFileW(
                devicePath,
                attempt.desiredAccess,
                FILE_SHARE_READ | FILE_SHARE_WRITE,
                nullptr,
                OPEN_EXISTING,
                attempt.flags,
                nullptr);

            if (handle != INVALID_HANDLE_VALUE)
            {
                return handle;
            }
        }

        return INVALID_HANDLE_VALUE;
    }

    std::wstring FormatBluetoothAddress(ULONGLONG address)
    {
        wchar_t buffer[32] = {};
        StringCchPrintfW(buffer, std::size(buffer), L"BTH:%012llX", static_cast<unsigned long long>(address));
        return buffer;
    }

    std::wstring SnapshotKey(const DeviceSnapshot &snapshot)
    {
        if (!snapshot.deviceId.empty())
        {
            return snapshot.deviceId;
        }
        return snapshot.name;
    }

    std::optional<std::wstring> ExtractBluetoothAddressToken(const std::wstring &text)
    {
        constexpr size_t kAddressChars = 12;
        if (text.size() < kAddressChars)
        {
            return std::nullopt;
        }

        for (size_t i = 0; i + kAddressChars <= text.size(); ++i)
        {
            bool allHex = true;
            for (size_t j = 0; j < kAddressChars; ++j)
            {
                if (!iswxdigit(text[i + j]))
                {
                    allHex = false;
                    break;
                }
            }

            if (!allHex)
            {
                continue;
            }

            const bool hasHexBefore = (i > 0) && iswxdigit(text[i - 1]);
            const bool hasHexAfter = (i + kAddressChars < text.size()) && iswxdigit(text[i + kAddressChars]);
            if (hasHexBefore || hasHexAfter)
            {
                continue;
            }

            std::wstring token = text.substr(i, kAddressChars);
            std::transform(token.begin(), token.end(), token.begin(), [](wchar_t c)
                           { return static_cast<wchar_t>(towupper(c)); });
            return token;
        }

        return std::nullopt;
    }

    void RemoveBatteryUnavailableSuffix(std::wstring &name)
    {
        const std::wstring suffix = L" (battery unavailable)";
        if (name.size() >= suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0)
        {
            name.resize(name.size() - suffix.size());
        }
    }

    std::vector<DeviceSnapshot> EnumerateClassicBluetoothDevices()
    {
        std::vector<DeviceSnapshot> devices;

        BLUETOOTH_DEVICE_SEARCH_PARAMS search = {};
        search.dwSize = sizeof(search);
        search.fReturnAuthenticated = TRUE;
        search.fReturnRemembered = TRUE;
        search.fReturnConnected = TRUE;
        search.fReturnUnknown = FALSE;
        search.fIssueInquiry = FALSE;
        search.cTimeoutMultiplier = 0;
        search.hRadio = nullptr;

        BLUETOOTH_DEVICE_INFO info = {};
        info.dwSize = sizeof(info);

        HBLUETOOTH_DEVICE_FIND findHandle = BluetoothFindFirstDevice(&search, &info);
        if (!findHandle)
        {
            return devices;
        }

        do
        {
            DeviceSnapshot snapshot = {};
            snapshot.deviceId = FormatBluetoothAddress(info.Address.ullLong);
            if (info.szName[0] != L'\0')
            {
                snapshot.name = info.szName;
            }
            else
            {
                snapshot.name = L"Bluetooth device";
            }
            snapshot.name += L" (classic)";
            snapshot.diagnostic = L"classic bluetooth device (no public LE GATT battery path)";
            devices.push_back(std::move(snapshot));

            info.dwSize = sizeof(info);
        } while (BluetoothFindNextDevice(findHandle, &info));

        BluetoothFindDeviceClose(findHandle);
        return devices;
    }

    BatteryQueryResult QueryBatteryLevel(HANDLE deviceHandle)
    {
        BatteryQueryResult result = {};

        USHORT serviceCount = 0;
        HRESULT hr = BluetoothGATTGetServices(deviceHandle, 0, nullptr, &serviceCount, BLUETOOTH_GATT_FLAG_NONE);
        if (hr != HRESULT_FROM_WIN32(ERROR_MORE_DATA) || serviceCount == 0)
        {
            result.diagnostic = L"gatt services unavailable " + FormatHRESULT(hr);
            return result;
        }

        std::vector<BTH_LE_GATT_SERVICE> services(serviceCount);
        hr = BluetoothGATTGetServices(deviceHandle, serviceCount, services.data(), &serviceCount, BLUETOOTH_GATT_FLAG_NONE);
        if (FAILED(hr))
        {
            result.diagnostic = L"gatt service enumeration failed " + FormatHRESULT(hr);
            return result;
        }

        bool foundBatteryService = false;
        bool foundBatteryCharacteristic = false;
        HRESULT lastReadFailure = S_OK;

        for (const auto &service : services)
        {
            if (!IsShortUuid(service.ServiceUuid, 0x180F))
            {
                continue;
            }

            foundBatteryService = true;

            BTH_LE_GATT_SERVICE serviceCopy = service;
            USHORT characteristicCount = 0;
            hr = BluetoothGATTGetCharacteristics(deviceHandle, &serviceCopy, 0, nullptr, &characteristicCount, BLUETOOTH_GATT_FLAG_NONE);
            if (hr != HRESULT_FROM_WIN32(ERROR_MORE_DATA) || characteristicCount == 0)
            {
                lastReadFailure = hr;
                continue;
            }

            std::vector<BTH_LE_GATT_CHARACTERISTIC> characteristics(characteristicCount);
            hr = BluetoothGATTGetCharacteristics(deviceHandle, &serviceCopy, characteristicCount, characteristics.data(), &characteristicCount, BLUETOOTH_GATT_FLAG_NONE);
            if (FAILED(hr))
            {
                lastReadFailure = hr;
                continue;
            }

            for (const auto &characteristic : characteristics)
            {
                if (!characteristic.IsReadable || !IsShortUuid(characteristic.CharacteristicUuid, 0x2A19))
                {
                    continue;
                }

                foundBatteryCharacteristic = true;

                BTH_LE_GATT_CHARACTERISTIC characteristicCopy = characteristic;
                const ULONG readModes[] = {
                    BLUETOOTH_GATT_FLAG_FORCE_READ_FROM_CACHE,
                    BLUETOOTH_GATT_FLAG_FORCE_READ_FROM_DEVICE,
                    BLUETOOTH_GATT_FLAG_NONE,
                };

                for (ULONG readMode : readModes)
                {
                    USHORT valueSize = 0;
                    hr = BluetoothGATTGetCharacteristicValue(deviceHandle, &characteristicCopy, 0, nullptr, &valueSize, readMode);
                    if (hr != HRESULT_FROM_WIN32(ERROR_MORE_DATA) || valueSize < sizeof(BTH_LE_GATT_CHARACTERISTIC_VALUE))
                    {
                        lastReadFailure = hr;
                        continue;
                    }

                    std::vector<BYTE> valueBuffer(valueSize);
                    auto *value = reinterpret_cast<PBTH_LE_GATT_CHARACTERISTIC_VALUE>(valueBuffer.data());
                    hr = BluetoothGATTGetCharacteristicValue(deviceHandle, &characteristicCopy, valueSize, value, nullptr, readMode);
                    if (FAILED(hr) || value->DataSize == 0)
                    {
                        lastReadFailure = hr;
                        continue;
                    }

                    result.batteryPercent = static_cast<int>(value->Data[0]);
                    result.diagnostic = L"ok gatt-battery";
                    return result;
                }
            }
        }

        if (!foundBatteryService)
        {
            result.diagnostic = L"battery service 0x180F not found";
        }
        else if (!foundBatteryCharacteristic)
        {
            result.diagnostic = L"battery level characteristic 0x2A19 missing";
        }
        else
        {
            result.diagnostic = L"battery read failed " + FormatHRESULT(lastReadFailure);
        }

        return result;
    }

    class BatteryHistoryStore
    {
    public:
        BatteryHistoryStore() : storagePath_(GetStoragePath())
        {
            Load();
        }

        void UpdateAndPersist(std::vector<DeviceSnapshot> &snapshots)
        {
            const uint64_t now = static_cast<uint64_t>(std::time(nullptr));

            for (auto &snapshot : snapshots)
            {
                if (!snapshot.batteryPercent.has_value())
                {
                    continue;
                }

                DeviceStats &stats = statsByDevice_[snapshot.deviceId];
                const int percent = *snapshot.batteryPercent;

                if (!stats.hasSample)
                {
                    stats.lastPercent = percent;
                    stats.lastSampleUnixSeconds = now;
                    stats.hasSample = true;
                    continue;
                }

                if (percent < stats.lastPercent)
                {
                    const uint64_t elapsedSeconds = now > stats.lastSampleUnixSeconds ? now - stats.lastSampleUnixSeconds : 0;
                    const double elapsedHours = std::max(1.0 / 60.0, static_cast<double>(elapsedSeconds) / 3600.0);
                    const double drainRate = static_cast<double>(stats.lastPercent - percent) / elapsedHours;
                    const int64_t drainRateMilli = static_cast<int64_t>(std::llround(drainRate * 1000.0));

                    if (drainRateMilli > 0)
                    {
                        if (stats.baselineDrainRateMilliPercentPerHour == 0 || drainRateMilli < stats.baselineDrainRateMilliPercentPerHour)
                        {
                            stats.baselineDrainRateMilliPercentPerHour = drainRateMilli;
                        }

                        if (stats.ewmaDrainRateMilliPercentPerHour == 0)
                        {
                            stats.ewmaDrainRateMilliPercentPerHour = drainRateMilli;
                        }
                        else
                        {
                            stats.ewmaDrainRateMilliPercentPerHour = (stats.ewmaDrainRateMilliPercentPerHour * 85 + drainRateMilli * 15) / 100;
                        }

                        stats.sampleCount += 1;
                    }

                    stats.lastPercent = percent;
                    stats.lastSampleUnixSeconds = now;
                    stats.hasSample = true;
                }
                else if (percent > stats.lastPercent)
                {
                    stats.lastPercent = percent;
                    stats.lastSampleUnixSeconds = now;
                    stats.hasSample = true;
                }

                const double ewmaRate = static_cast<double>(stats.ewmaDrainRateMilliPercentPerHour) / 1000.0;
                const double baselineRate = static_cast<double>(stats.baselineDrainRateMilliPercentPerHour) / 1000.0;

                if (ewmaRate > 0.0)
                {
                    snapshot.remainingMinutes = (static_cast<double>(percent) * 60.0) / ewmaRate;
                    snapshot.drainRateMilliPercentPerHour = ewmaRate * 1000.0;
                }

                if (baselineRate > 0.0 && ewmaRate > 0.0)
                {
                    const double health = std::clamp((baselineRate / ewmaRate) * 100.0, 5.0, 100.0);
                    snapshot.healthPercent = health;
                }
            }

            Save();
        }

        void ApplyStoredAverages(std::vector<DeviceSnapshot> &snapshots)
        {
            for (auto &snapshot : snapshots)
            {
                auto it = statsByDevice_.find(snapshot.deviceId);
                if (it == statsByDevice_.end())
                {
                    continue;
                }

                const DeviceStats &stats = it->second;
                const double ewmaRate = static_cast<double>(stats.ewmaDrainRateMilliPercentPerHour) / 1000.0;
                const double baselineRate = static_cast<double>(stats.baselineDrainRateMilliPercentPerHour) / 1000.0;

                if (ewmaRate > 0.0 && snapshot.batteryPercent.has_value())
                {
                    snapshot.remainingMinutes = (static_cast<double>(*snapshot.batteryPercent) * 60.0) / ewmaRate;
                    snapshot.drainRateMilliPercentPerHour = ewmaRate * 1000.0;
                }

                if (baselineRate > 0.0 && ewmaRate > 0.0)
                {
                    snapshot.healthPercent = std::clamp((baselineRate / ewmaRate) * 100.0, 5.0, 100.0);
                }
            }
        }

    private:
        void Load()
        {
            std::ifstream input(storagePath_, std::ios::binary);
            if (!input.is_open())
            {
                return;
            }

            std::string line;
            while (std::getline(input, line))
            {
                if (line.empty())
                {
                    continue;
                }

                std::vector<std::string> parts;
                size_t start = 0;
                while (true)
                {
                    size_t tab = line.find('\t', start);
                    parts.push_back(line.substr(start, tab == std::string::npos ? std::string::npos : tab - start));
                    if (tab == std::string::npos)
                    {
                        break;
                    }
                    start = tab + 1;
                }

                if (parts.size() < 6)
                {
                    continue;
                }

                DeviceStats stats = {};
                stats.baselineDrainRateMilliPercentPerHour = std::strtoll(parts[1].c_str(), nullptr, 10);
                stats.ewmaDrainRateMilliPercentPerHour = std::strtoll(parts[2].c_str(), nullptr, 10);
                stats.sampleCount = std::atoi(parts[3].c_str());
                stats.lastPercent = std::atoi(parts[4].c_str());
                stats.lastSampleUnixSeconds = static_cast<uint64_t>(std::strtoull(parts[5].c_str(), nullptr, 10));
                stats.hasSample = true;
                statsByDevice_[Utf8ToWide(parts[0])] = stats;
            }
        }

        void Save()
        {
            std::ofstream output(storagePath_, std::ios::binary | std::ios::trunc);
            if (!output.is_open())
            {
                return;
            }

            for (const auto &[deviceId, stats] : statsByDevice_)
            {
                output << WideToUtf8(deviceId) << '\t'
                       << stats.baselineDrainRateMilliPercentPerHour << '\t'
                       << stats.ewmaDrainRateMilliPercentPerHour << '\t'
                       << stats.sampleCount << '\t'
                       << stats.lastPercent << '\t'
                       << stats.lastSampleUnixSeconds << '\n';
            }
        }

        std::filesystem::path storagePath_;
        std::unordered_map<std::wstring, DeviceStats> statsByDevice_;
    };

    class BluetoothBatteryMonitor
    {
    public:
        explicit BluetoothBatteryMonitor(BatteryHistoryStore &history) : history_(history) {}

        std::vector<DeviceSnapshot> Refresh()
        {
            std::vector<DeviceSnapshot> snapshots;
            DeviceInterfaceHandle deviceInfoSet;
            deviceInfoSet.value = SetupDiGetClassDevsW(&kBluetoothLeDeviceInterfaceGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
            if (deviceInfoSet.value != INVALID_HANDLE_VALUE)
            {
                SP_DEVICE_INTERFACE_DATA interfaceData = {};
                interfaceData.cbSize = sizeof(interfaceData);

                for (DWORD index = 0; SetupDiEnumDeviceInterfaces(deviceInfoSet.value, nullptr, &kBluetoothLeDeviceInterfaceGuid, index, &interfaceData); ++index)
                {
                    DWORD requiredSize = 0;
                    SetupDiGetDeviceInterfaceDetailW(deviceInfoSet.value, &interfaceData, nullptr, 0, &requiredSize, nullptr);
                    if (requiredSize == 0)
                    {
                        continue;
                    }

                    std::vector<BYTE> detailBuffer(requiredSize);
                    auto *detail = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(detailBuffer.data());
                    detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

                    SP_DEVINFO_DATA devInfo = {};
                    devInfo.cbSize = sizeof(devInfo);
                    if (!SetupDiGetDeviceInterfaceDetailW(deviceInfoSet.value, &interfaceData, detail, requiredSize, nullptr, &devInfo))
                    {
                        continue;
                    }

                    DeviceSnapshot snapshot = {};
                    snapshot.deviceId = QueryDeviceInstanceId(deviceInfoSet.value, devInfo);
                    if (snapshot.deviceId.empty())
                    {
                        snapshot.deviceId = detail->DevicePath;
                    }

                    snapshot.name = QueryDeviceName(deviceInfoSet.value, devInfo);

                    HANDLE deviceHandle = OpenBluetoothLeHandle(detail->DevicePath);
                    if (deviceHandle != INVALID_HANDLE_VALUE)
                    {
                        BatteryQueryResult batteryResult = QueryBatteryLevel(deviceHandle);
                        snapshot.batteryPercent = batteryResult.batteryPercent;
                        snapshot.diagnostic = batteryResult.diagnostic;
                        CloseHandle(deviceHandle);
                    }
                    else
                    {
                        snapshot.diagnostic = L"cannot open LE device interface";
                    }

                    if (!snapshot.batteryPercent.has_value())
                    {
                        std::wstring propertyDiagnostic;
                        auto propertyBattery = QueryBatteryFromWindowsProperties(deviceInfoSet.value, devInfo, propertyDiagnostic);
                        if (propertyBattery.has_value())
                        {
                            snapshot.batteryPercent = propertyBattery;
                            snapshot.diagnostic = propertyDiagnostic;
                        }
                        else if (snapshot.diagnostic.empty())
                        {
                            snapshot.diagnostic = L"battery value unavailable";
                        }
                        else
                        {
                            snapshot.diagnostic += L"; windows property fallback not available";
                        }
                    }

                    if (!snapshot.batteryPercent.has_value())
                    {
                        snapshot.name += L" (battery unavailable)";
                    }

                    snapshots.push_back(std::move(snapshot));
                }
            }

            std::unordered_set<std::wstring> seen;
            for (const auto &snapshot : snapshots)
            {
                seen.insert(SnapshotKey(snapshot));
            }

            auto classicDevices = EnumerateClassicBluetoothDevices();
            for (auto &classic : classicDevices)
            {
                const std::wstring key = SnapshotKey(classic);
                if (seen.find(key) != seen.end())
                {
                    continue;
                }
                seen.insert(key);
                snapshots.push_back(std::move(classic));
            }

            const auto batteryByAddress = QueryBatteryByAddressFromAllPresentDevices();
            if (g_enableDebugLogging)
            {
                for (const auto &[address, match] : batteryByAddress)
                {
                    std::wstring line = L"Address candidates " + address +
                                        L" | chosen=" + std::to_wstring(match.value) + L"%" +
                                        L" | confidence=" + std::to_wstring(match.confidence) +
                                        L" | source=" + match.source +
                                        L" | candidates=" + std::to_wstring(match.candidates.size());
                    DebugLog(line);

                    for (const auto &candidate : match.candidates)
                    {
                        DebugLog(L"  - " + candidate);
                    }
                }
            }

            for (auto &snapshot : snapshots)
            {
                if (snapshot.batteryPercent.has_value())
                {
                    continue;
                }

                auto address = ExtractBluetoothAddressToken(snapshot.deviceId);
                if (!address.has_value())
                {
                    address = ExtractBluetoothAddressToken(snapshot.name);
                }

                if (!address.has_value())
                {
                    continue;
                }

                auto it = batteryByAddress.find(*address);
                if (it == batteryByAddress.end())
                {
                    continue;
                }

                const AddressBatteryMatch &match = it->second;
                if (match.confidence >= 2)
                {
                    snapshot.batteryPercent = match.value;
                    snapshot.diagnostic = L"windows sibling fallback by bluetooth address | source=" + match.source +
                                          L" | value=" + std::to_wstring(match.value) + L"%";
                    RemoveBatteryUnavailableSuffix(snapshot.name);
                }
                else
                {
                    snapshot.diagnostic = L"weak sibling candidates for address " + *address +
                                          L" | best=" + std::to_wstring(match.value) + L"% from " + match.source +
                                          L" | candidates=" + std::to_wstring(match.candidates.size());
                }
            }

            history_.UpdateAndPersist(snapshots);
            history_.ApplyStoredAverages(snapshots);

            std::sort(snapshots.begin(), snapshots.end(), [](const DeviceSnapshot &left, const DeviceSnapshot &right)
                      { return left.name < right.name; });

            return snapshots;
        }

    private:
        BatteryHistoryStore &history_;
    };

    class OverlayWindow
    {
    public:
        explicit OverlayWindow(AppOptions options) : options_(options), clickThroughEnabled_(!options.debugMode), monitor_(history_) {}

        ~OverlayWindow()
        {
            StopWorker();
            DestroyTrayIcon();
            DestroyFonts();
        }

        int Run()
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

    private:
        DWORD BuildWindowExStyle() const
        {
            DWORD exStyle = WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TOOLWINDOW;
            if (clickThroughEnabled_)
            {
                exStyle |= WS_EX_TRANSPARENT | WS_EX_NOACTIVATE;
            }
            return exStyle;
        }

        bool CreateAppWindow()
        {
            WNDCLASSEXW wc = {};
            wc.cbSize = sizeof(wc);
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
                options_.debugMode ? (WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX) : WS_POPUP,
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

        void StartWorker()
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

        void StopWorker()
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

        void RequestRefresh()
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

        void UpdateLayoutAndPosition()
        {
            std::vector<DeviceSnapshot> localSnapshot;
            {
                std::scoped_lock lock(snapshotMutex_);
                localSnapshot = snapshot_;
            }

            const int rowCount = static_cast<int>(std::max<size_t>(1, localSnapshot.size()));
            const int height = kTitleHeight + (rowCount * kRowHeight) + kFooterHeight + (kPadding * 2);
            const int width = kOverlayWidth;

            RECT workArea = {};
            SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
            const int x = workArea.right - width - 24;
            const int y = workArea.top + 24;

            UINT flags = SWP_NOSENDCHANGING | SWP_SHOWWINDOW;
            if (clickThroughEnabled_)
            {
                flags |= SWP_NOACTIVATE;
            }
            SetWindowPos(hwnd_, HWND_TOPMOST, x, y, width, height, flags);
            InvalidateRect(hwnd_, nullptr, TRUE);
        }

        void Paint(HDC hdc)
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

            RECT titleRect = {kPadding, kPadding, client.right - kPadding, kPadding + 28};
            SelectObject(hdc, titleFont_);
            DrawTextW(hdc, L"Bluetooth battery", -1, &titleRect, DT_SINGLELINE | DT_LEFT | DT_VCENTER);

            RECT hintRect = {client.right - 190, kPadding + 4, client.right - kPadding, kPadding + 24};
            SetTextColor(hdc, RGB(170, 178, 190));
            SelectObject(hdc, smallFont_);
            if (options_.debugMode)
            {
                DrawTextW(hdc, L"Ctrl+Alt+Q quit | Ctrl+Alt+T toggle pass-through", -1, &hintRect, DT_SINGLELINE | DT_RIGHT | DT_VCENTER);
            }
            else
            {
                DrawTextW(hdc, L"Ctrl+Alt+Q quit | Ctrl+Alt+T toggle pass-through", -1, &hintRect, DT_SINGLELINE | DT_RIGHT | DT_VCENTER);
            }

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

                RECT detailRect = {row.left + 16, row.top + 34, row.right - 16, row.bottom - 12};
                SetTextColor(hdc, RGB(196, 202, 212));
                SelectObject(hdc, smallFont_);

                std::wstring detailText = L"Health ";
                if (snapshot.healthPercent.has_value())
                {
                    wchar_t buffer[32] = {};
                    StringCchPrintfW(buffer, 32, L"%.0f%%", *snapshot.healthPercent);
                    detailText += buffer;
                }
                else
                {
                    detailText += L"tracking";
                }

                detailText += L"   Est. ";
                if (snapshot.remainingMinutes.has_value())
                {
                    detailText += FormatRemainingTime(*snapshot.remainingMinutes);
                }
                else
                {
                    detailText += L"waiting for discharge history";
                }

                detailText += L"   Rate ";
                detailText += FormatDrainRate(snapshot.drainRateMilliPercentPerHour);

                if (!snapshot.diagnostic.empty() && (options_.debugMode || !snapshot.batteryPercent.has_value()))
                {
                    detailText += L"   Diag ";
                    detailText += snapshot.diagnostic;
                }

                DrawTextW(hdc, detailText.c_str(), -1, &detailRect, DT_SINGLELINE | DT_LEFT | DT_END_ELLIPSIS);

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
            DrawTextW(hdc, L"Health is estimated from observed drain history when the device does not expose a direct health metric.", -1, &footerRect, DT_SINGLELINE | DT_LEFT | DT_END_ELLIPSIS);
        }

        void DrawStaticContent(HDC hdc, const RECT &client, const wchar_t *headline, const wchar_t *subline)
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

        void SetClickThroughEnabled(bool enabled)
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

        bool CreateTrayIcon()
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

        void DestroyTrayIcon()
        {
            if (!trayIconCreated_)
            {
                return;
            }

            Shell_NotifyIconW(NIM_DELETE, &trayIconData_);
            trayIconCreated_ = false;
        }

        void ShowTrayContextMenu()
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

        void CreateFonts()
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

        void DestroyFonts()
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

        static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
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

        LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam)
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

} // namespace

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
