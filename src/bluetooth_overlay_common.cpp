#include "bluetooth_overlay_common.hpp"

#include <cfgmgr32.h>

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
#pragma comment(lib, "cfgmgr32.lib")
#pragma comment(lib, "bluetoothapis.lib")
#pragma comment(lib, "bthprops.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

namespace bluetooth_overlay
{

    bool g_enableDebugLogging = false;

    namespace
    {
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
    } // namespace

    bool IsStartedDevNode(const SP_DEVINFO_DATA &devInfoData)
    {
        ULONG status = 0;
        ULONG problem = 0;
        if (CM_Get_DevNode_Status(&status, &problem, devInfoData.DevInst, 0) != CR_SUCCESS)
        {
            return false;
        }

        return (status & DN_STARTED) != 0;
    }

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
        if (auto byteValue = TryReadByteDeviceProperty(deviceInfoSet, devInfoData, key); byteValue.has_value() && *byteValue >= 0)
        {
            return byteValue;
        }

        if (auto dwordValue = TryReadUint32DeviceProperty(deviceInfoSet, devInfoData, key); dwordValue.has_value() && *dwordValue >= 0)
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
            if (IsSameDevPropKey(key, kBluetoothBatteryCandidateKey1) || 
                IsSameDevPropKey(key, kBluetoothBatteryCandidateKey2) ||
                IsSameDevPropKey(key, kBluetoothBatteryCandidateKey3) ||
                IsSameDevPropKey(key, kBluetoothBatteryCandidateKey4) ||
                IsSameDevPropKey(key, kBluetoothBatteryCandidateKey5))
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
        const struct Candidate {
            const DEVPROPKEY* key;
            const wchar_t* label;
        } candidates[] = {
            { &kBluetoothBatteryCandidateKey1, L"{670245F9...} pid 4" },
            { &kBluetoothBatteryCandidateKey2, L"{80497100...} pid 6" },
            { &kBluetoothBatteryCandidateKey3, L"{104EA319...} pid 2 (BBE5)" },
            { &kBluetoothBatteryCandidateKey4, L"{104EA319...} pid 2 (BB14)" },
            { &kBluetoothBatteryCandidateKey5, L"{104E17AB...} pid 2" },
        };

        std::optional<int> bestValue;
        std::wstring bestLabel;

        for (const auto& candidate : candidates) {
            if (auto value = TryReadNumericPercentProperty(deviceInfoSet, devInfoData, *candidate.key); value.has_value()) {
                if (!bestValue.has_value() || *value > *bestValue) {
                    bestValue = value;
                    bestLabel = candidate.label;
                }
            }
        }

        if (bestValue.has_value()) {
            diagnostic = std::wstring(L"windows property fallback ") + bestLabel;
            return bestValue;
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

            std::wstring name = QueryDeviceName(allDevices.value, devInfo);

            AddressBatteryMatch &match = values[*address];
            std::wstring candidate = std::to_wstring(*battery) + L"% from " + diagnostic;
            match.candidates.push_back(candidate);

            const int confidence = BatteryDiagnosticConfidence(diagnostic);
            
            // Priority:
            // 1. Higher value (since we already ignore 0 in TryReadNumericPercentProperty)
            // 2. Higher confidence source
            const bool replaceCurrent = (match.value < 0) ||
                                        (*battery > match.value) ||
                                        (confidence > match.confidence && *battery == match.value);

            if (replaceCurrent)
            {
                match.value = *battery;
                match.confidence = confidence;
                match.source = diagnostic;
                match.instanceId = instanceId;
                match.name = name;
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

    std::wstring NormalizeDeviceName(const std::wstring &name)
    {
        std::wstring n = name;
        
        // Remove common suffixes
        const std::wstring suffixes[] = { 
            L" (classic)", 
            L" (battery unavailable)", 
            L" Hands-Free AG", 
            L" Hands-Free", 
            L" Avrcp Transport",
            L" Stereo"
        };

        bool changed = true;
        while (changed) {
            changed = false;
            for (const auto& s : suffixes) {
                if (n.size() >= s.size() && n.compare(n.size() - s.size(), s.size(), s) == 0) {
                    n.resize(n.size() - s.size());
                    changed = true;
                }
            }
        }

        // Trim
        size_t first = n.find_first_not_of(L" ");
        if (std::wstring::npos == first) return n;
        size_t last = n.find_last_not_of(L" ");
        return n.substr(first, (last - first + 1));
    }

    std::vector<DeviceSnapshot> EnumerateClassicBluetoothDevices()
    {
        std::vector<DeviceSnapshot> devices;

        BLUETOOTH_DEVICE_SEARCH_PARAMS search = {};
        search.dwSize = sizeof(search);
        search.fReturnConnected = TRUE;
        search.fReturnAuthenticated = FALSE;
        search.fReturnRemembered = FALSE;
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
            if (!info.fConnected)
            {
                continue;
            }

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
                    if (hr != HRESULT_FROM_WIN32(ERROR_MORE_DATA) || valueSize < (FIELD_OFFSET(BTH_LE_GATT_CHARACTERISTIC_VALUE, Data) + 1))
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

} // namespace bluetooth_overlay
