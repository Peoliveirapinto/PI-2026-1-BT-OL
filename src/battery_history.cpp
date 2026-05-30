#include "battery_history.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <fstream>

namespace bluetooth_overlay
{

    BatteryHistoryStore::BatteryHistoryStore() : storagePath_(GetStoragePath())
    {
        Load();
    }

    void BatteryHistoryStore::UpdateAndPersist(std::vector<DeviceSnapshot> &snapshots)
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
                const uint64_t MIN_SAMPLE_INTERVAL_SECONDS = 300;

                if (elapsedSeconds < MIN_SAMPLE_INTERVAL_SECONDS)
                {
                    // Ignora a medição de taxa para evitar ruídos, apenas atualiza o percentual atual
                    stats.lastPercent = percent;
                }
                else
                {
                    const double elapsedHours = std::max(1.0 / 60.0, static_cast<double>(elapsedSeconds) / 3600.0);
                    const double drainRate = static_cast<double>(stats.lastPercent - percent) / elapsedHours;
                    const int64_t drainRateMilli = static_cast<int64_t>(std::llround(drainRate * 1000.0));

                    if (drainRateMilli > 0)
                    {
                        const int64_t MIN_ACTIVE_DRAIN_MILLI = 500;
                        if (drainRateMilli >= MIN_ACTIVE_DRAIN_MILLI)
                        {
                            if (stats.baselineDrainRateMilliPercentPerHour == 0)
                            {
                                stats.baselineDrainRateMilliPercentPerHour = drainRateMilli;
                            }
                            else if (drainRateMilli < stats.baselineDrainRateMilliPercentPerHour)
                            {
                                stats.baselineDrainRateMilliPercentPerHour = drainRateMilli;
                            }
                            else
                            {
                                stats.baselineDrainRateMilliPercentPerHour = (stats.baselineDrainRateMilliPercentPerHour * 999 + drainRateMilli * 1) / 1000;
                            }
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

    void BatteryHistoryStore::ApplyStoredAverages(std::vector<DeviceSnapshot> &snapshots)
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

    void BatteryHistoryStore::Load()
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

    void BatteryHistoryStore::Save()
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

} // namespace bluetooth_overlay
