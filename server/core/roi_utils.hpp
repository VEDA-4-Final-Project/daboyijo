#pragma once
#include <vector>
#include <utility>
#include "detection.hpp"

namespace RoiUtils {
    inline bool pointInPolygon(float px, float py, const std::vector<std::pair<float, float>>& poly) {
        bool inside = false;
        for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
            const float xi = poly[i].first, yi = poly[i].second;
            const float xj = poly[j].first, yj = poly[j].second;
            if (((yi > py) != (yj > py)) && (px < (xj - xi) * (py - yi) / (yj - yi) + xi))
                inside = !inside;
        }
        return inside;
    }

    // 두 모듈이 공통으로 호출할 '발끝 기준 In/Out 판정' 함수
    inline bool isFootInBed(const Detection& d, const std::vector<std::pair<float, float>>& bed_roi) {
        if (bed_roi.size() < 3) return false;
        const float foot_x = (d.left + d.right) / 2.0f;
        const float foot_y = d.bottom;
        return pointInPolygon(foot_x, foot_y, bed_roi);
    }
}