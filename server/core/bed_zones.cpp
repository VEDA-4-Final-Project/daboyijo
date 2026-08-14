#include "bed_zones.hpp"

void BedZoneStore::update(int channel, int roi_id, bool clear,
                          std::vector<std::pair<float, float>> points) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (clear) {
        if (roi_id == kRoiIdAll) {
            channels_.erase(channel);
        } else {
            auto ch = channels_.find(channel);
            if (ch != channels_.end()) {
                ch->second.erase(roi_id);
                if (ch->second.empty()) channels_.erase(ch);
            }
        }
        return;
    }

    BedZone& zone = channels_[channel][roi_id];
    zone.roi_id = roi_id;
    zone.points = std::move(points);
    // resident_id는 건드리지 않는다 — 같은 침대를 다시 그린 것뿐인데(각도 조정 등)
    // 매핑까지 날아가면 관리자가 매번 사람을 다시 지정해야 한다.
}

void BedZoneStore::bind(int channel, int roi_id, int resident_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    BedZone& zone = channels_[channel][roi_id];
    zone.roi_id = roi_id;
    zone.resident_id = resident_id;
}

std::map<int, BedZone> BedZoneStore::zones(int channel) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = channels_.find(channel);
    return it == channels_.end() ? std::map<int, BedZone>{} : it->second;
}

bool BedZoneStore::hasZones(int channel) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = channels_.find(channel);
    if (it == channels_.end()) return false;
    for (const auto& entry : it->second) {
        if (entry.second.valid()) return true;
    }
    return false;
}
