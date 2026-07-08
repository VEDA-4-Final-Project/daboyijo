#include "system_stats.hpp"

#include <fstream>
#include <sstream>
#include <string>

double SystemStats::cpuPercent() {
    std::ifstream stat("/proc/stat");
    std::string line;
    if (!stat || !std::getline(stat, line)) {
        return -1;
    }

    // "cpu  user nice system idle iowait irq softirq steal ..."
    std::istringstream iss(line);
    std::string label;
    iss >> label;

    unsigned long long value = 0, total = 0, idle = 0;
    for (int i = 0; iss >> value; ++i) {
        total += value;
        if (i == 3 || i == 4) {  // idle + iowait
            idle += value;
        }
    }

    double percent = 0;
    if (last_total_ != 0 && total > last_total_) {
        auto d_total = total - last_total_;
        auto d_idle = idle - last_idle_;
        percent = 100.0 * (d_total - d_idle) / d_total;
    }
    last_total_ = total;
    last_idle_ = idle;
    return percent;
}

double SystemStats::socTemperature() {
    std::ifstream file("/sys/class/thermal/thermal_zone0/temp");
    long milli = 0;
    if (!file || !(file >> milli)) {
        return -1;
    }
    return milli / 1000.0;
}
