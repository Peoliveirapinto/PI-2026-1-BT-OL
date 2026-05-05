#pragma once

#include "bluetooth_overlay_common.hpp"

#include <unordered_map>

namespace bluetooth_overlay
{

    struct DeviceStats
    {
        int lastPercent = -1;
        uint64_t lastSampleUnixSeconds = 0;
        int64_t ewmaDrainRateMilliPercentPerHour = 0;
        int64_t baselineDrainRateMilliPercentPerHour = 0;
        int sampleCount = 0;
        bool hasSample = false;
    };

    class BatteryHistoryStore
    {
    public:
        BatteryHistoryStore();

        void UpdateAndPersist(std::vector<DeviceSnapshot> &snapshots);
        void ApplyStoredAverages(std::vector<DeviceSnapshot> &snapshots);

    private:
        void Load();
        void Save();

        std::filesystem::path storagePath_;
        std::unordered_map<std::wstring, DeviceStats> statsByDevice_;
    };

} // namespace bluetooth_overlay
