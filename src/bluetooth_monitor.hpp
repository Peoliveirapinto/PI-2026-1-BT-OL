#pragma once

#include "battery_history.hpp"

namespace bluetooth_overlay
{

    class BluetoothBatteryMonitor
    {
    public:
        explicit BluetoothBatteryMonitor(BatteryHistoryStore &history);

        std::vector<DeviceSnapshot> Refresh();

    private:
        BatteryHistoryStore &history_;
    };

} // namespace bluetooth_overlay
