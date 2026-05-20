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

                if (!IsStartedDevNode(devInfo))
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

                if (!snapshot.batteryPercent.has_value() || *snapshot.batteryPercent == 0)
                {
                    std::wstring propertyDiagnostic;
                    auto propertyBattery = QueryBatteryFromWindowsProperties(deviceInfoSet, devInfo, propertyDiagnostic);
                    
                    // Only use fallback if it gave us a value, and if we previously had 0, only if the fallback is > 0
                    bool useFallback = propertyBattery.has_value();
                    if (useFallback && snapshot.batteryPercent.has_value() && *snapshot.batteryPercent == 0) {
                        if (*propertyBattery == 0) useFallback = false;
                    }

                    if (useFallback)
                    {
                        snapshot.batteryPercent = propertyBattery;
                        snapshot.diagnostic = propertyDiagnostic;
                    }
                    else if (!snapshot.batteryPercent.has_value())
                    {
                        if (snapshot.diagnostic.empty())
                        {
                            snapshot.diagnostic = L"battery value unavailable";
                        }
                        else
                        {
                            snapshot.diagnostic += L"; windows property fallback not available";
                        }
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

        std::unordered_set<std::wstring> usedAddresses;

        for (auto &snapshot : snapshots)
        {
            auto address = ExtractBluetoothAddressToken(snapshot.deviceId);
            if (!address.has_value())
            {
                address = ExtractBluetoothAddressToken(snapshot.name);
            }

            if (address.has_value())
            {
                usedAddresses.insert(*address);
            }

            // If we have a value and it's > 0, we are happy. 
            // If it's 0, we might want to check the address-based fallback to see if there's a better one.
            if (snapshot.batteryPercent.has_value() && *snapshot.batteryPercent > 0)
            {
                continue;
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
            // Overwrite if we have no value, OR if our current value is 0 and the match is > 0
            bool shouldOverwrite = !snapshot.batteryPercent.has_value();
            if (!shouldOverwrite && *snapshot.batteryPercent == 0 && match.value > 0) {
                shouldOverwrite = true;
            }

            if (shouldOverwrite && match.confidence >= 1)
            {
                snapshot.batteryPercent = match.value;
                snapshot.diagnostic = L"windows sibling fallback by bluetooth address | source=" + match.source +
                                      L" | value=" + std::to_wstring(match.value) + L"%";
                RemoveBatteryUnavailableSuffix(snapshot.name);
            }
            else if (!snapshot.batteryPercent.has_value())
            {
                snapshot.diagnostic = L"weak sibling candidates for address " + *address +
                                      L" | best=" + std::to_wstring(match.value) + L"% from " + match.source +
                                      L" | candidates=" + std::to_wstring(match.candidates.size());
            }
        }

        for (const auto &[address, match] : batteryByAddress)
        {
            if (usedAddresses.find(address) == usedAddresses.end() && match.value > 0)
            {
                bool merged = false;
                std::wstring normMatch = NormalizeDeviceName(match.name);
                for (auto &snapshot : snapshots) {
                    std::wstring normSnap = NormalizeDeviceName(snapshot.name);
                    if (normSnap == normMatch) {
                        if (!snapshot.batteryPercent.has_value() || *snapshot.batteryPercent == 0) {
                            snapshot.batteryPercent = match.value;
                            snapshot.diagnostic = L"windows property fallback by name match | " + match.source;
                            RemoveBatteryUnavailableSuffix(snapshot.name);
                            merged = true;
                            break;
                        }
                    }
                }

                if (!merged) {
                    DeviceSnapshot newSnap = {};
                    newSnap.deviceId = match.instanceId;
                    newSnap.name = match.name;
                    newSnap.batteryPercent = match.value;
                    newSnap.diagnostic = L"windows property device | " + match.source;
                    snapshots.push_back(std::move(newSnap));
                }
            }
        }

        // Final deduplication and name cleaning
        std::vector<DeviceSnapshot> finalSnapshots;
        std::unordered_map<std::wstring, size_t> nameToIndex;

        for (auto &snap : snapshots) {
            std::wstring norm = NormalizeDeviceName(snap.name);
            auto it = nameToIndex.find(norm);
            if (it != nameToIndex.end()) {
                auto &existing = finalSnapshots[it->second];
                // Merge if existing has no battery or 0 battery and new one has > 0
                if ((!existing.batteryPercent.has_value() || *existing.batteryPercent == 0) && 
                    (snap.batteryPercent.has_value() && *snap.batteryPercent > 0)) {
                    existing.batteryPercent = snap.batteryPercent;
                    existing.diagnostic = snap.diagnostic;
                }
            } else {
                nameToIndex[norm] = finalSnapshots.size();
                // Clean name before adding
                snap.name = norm;
                finalSnapshots.push_back(std::move(snap));
            }
        }

        snapshots = std::move(finalSnapshots);

        history_.UpdateAndPersist(snapshots);
        history_.ApplyStoredAverages(snapshots);

        std::sort(snapshots.begin(), snapshots.end(), [](const DeviceSnapshot &left, const DeviceSnapshot &right)
                  { return left.name < right.name; });

        return snapshots;
    }

} // namespace bluetooth_overlay
