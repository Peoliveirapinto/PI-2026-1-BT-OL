#include "bluetooth_monitor.hpp"

#include <algorithm>
#include <utility>

namespace bluetooth_overlay
{

    BluetoothBatteryMonitor::BluetoothBatteryMonitor(BatteryHistoryStore &history) : history_(history) {}

    std::vector<DeviceSnapshot> BluetoothBatteryMonitor::Refresh()
    {
        std::vector<DeviceSnapshot> snapshots;
        HDEVINFO deviceInfoSet = SetupDiGetClassDevsW(&kBluetoothLeDeviceInterfaceGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
        if (deviceInfoSet != INVALID_HANDLE_VALUE)
        {
            SP_DEVICE_INTERFACE_DATA interfaceData = {};
            interfaceData.cbSize = sizeof(interfaceData);

            for (DWORD index = 0; SetupDiEnumDeviceInterfaces(deviceInfoSet, nullptr, &kBluetoothLeDeviceInterfaceGuid, index, &interfaceData); ++index)
            {
                DWORD requiredSize = 0;
                SetupDiGetDeviceInterfaceDetailW(deviceInfoSet, &interfaceData, nullptr, 0, &requiredSize, nullptr);
                if (requiredSize == 0)
                {
                    continue;
                }

                std::vector<BYTE> detailBuffer(requiredSize);
                auto *detail = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(detailBuffer.data());
                detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

                SP_DEVINFO_DATA devInfo = {};
                devInfo.cbSize = sizeof(devInfo);
                if (!SetupDiGetDeviceInterfaceDetailW(deviceInfoSet, &interfaceData, detail, requiredSize, nullptr, &devInfo))
                {
                    continue;
                }

                DeviceSnapshot snapshot = {};
                snapshot.deviceId = QueryDeviceInstanceId(deviceInfoSet, devInfo);
                if (snapshot.deviceId.empty())
                {
                    snapshot.deviceId = detail->DevicePath;
                }

                snapshot.name = QueryDeviceName(deviceInfoSet, devInfo);

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
                    auto propertyBattery = QueryBatteryFromWindowsProperties(deviceInfoSet, devInfo, propertyDiagnostic);
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

            SetupDiDestroyDeviceInfoList(deviceInfoSet);
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

} // namespace bluetooth_overlay
