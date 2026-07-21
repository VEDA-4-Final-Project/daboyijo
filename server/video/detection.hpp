#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>
#include <opencv2/opencv.hpp>

// WiseAI(ONVIF) 메타데이터에서 파싱한 객체 1개.
// 좌표는 0.0~1.0 정규화 (BoundingBox 픽셀값에 tt:Transformation 적용 후).
struct Detection {
    int object_id = 0;       // tt:Object ObjectId — 프레임 간 동일 객체 추적 키
    int parent_id = 0;       // tt:Object Parent — 이 객체가 속한 부모 ObjectId (0=없음).
                             // 예: Head는 자기가 속한 Human의 id를 가리킴
    float left = 0;          // 정규화 좌표 (화면 폭 대비 0~1)
    float top = 0;
    float right = 0;
    float bottom = 0;
    float cx = 0;            // tt:CenterOfGravity 무게중심 (정규화 0~1).
    float cy = 0;            // 프레임 간 cy 변화율 = 낙하 속도 → 낙상 판정 핵심 신호
    float likelihood = 0;    // 사람일 확률 0~1
    std::string type;        // "Human", "Head", "Other" 등

    float width() const { return right - left; }
    float height() const { return bottom - top; }
    // 종횡비(가로/세로). 서 있으면 <1, 누우면 >1 경향 → 낙상 판정 단서.
    float aspectRatio() const {
        float h = height();
        return h > 1e-6f ? width() / h : 0.0f;
    }
    bool isHuman() const { return type == "Human"; }
};

// 한 프레임(한 시점)의 감지 결과 묶음. video → core 로 넘어가는 데이터 계약.
struct DetectionFrame {
    int channel = 0;
    std::chrono::steady_clock::time_point received_at;
    std::vector<Detection> objects;

    // 사람 객체 수 (occupancy). 요양보호사 진입 감지·재실 판단용.
    int humanCount() const {
        int n = 0;
        for (const auto& o : objects) {
            if (o.isHuman()) ++n;
        }
        return n;
    }
};

// 사람의 발끝(바운딩 박스 하단의 중앙)이 침대 ROI(다각형) 밖으로 나갔는지 판정.
inline bool isFeetInRoi(const Detection& det, const std::vector<std::pair<float, float>>& roi) {
    if (roi.empty()) return false;

    float px = (det.left + det.right) / 2.0f;
    float py = det.bottom;

    bool inside = false;
    for (size_t i = 0, j = roi.size() - 1; i < roi.size(); j = i++) {
        const float xi = roi[i].first, yi = roi[i].second;
        const float xj = roi[j].first, yj = roi[j].second;
        if (((yi > py) != (yj > py)) &&
            (px < (xj - xi) * (py - yi) / (yj - yi) + xi))
            inside = !inside;
    }
    
    return inside;
}