#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

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
    float cy = 0;            // 프레임 간 cy 변화율 = 낙하 속도 → 낙상 판정용
    float likelihood = 0;    // 사람일 확률 0~1
    std::string type;        // "Human", "Head", "Other" 등

    float width() const { return right - left; }
    float height() const { return bottom - top; }

    // 종횡비(가로/세로). 서 있으면 <1, 누우면 >1 경향 → 낙상 판정 단서.
    float aspectRatio() const {
        float h = height();
        // h>0이면 width()/h
        return h > 1e-6f ? width() / h : 0.0f;
    }
    bool isHuman() const { return type == "Human"; }
};

// 한 프레임(한 시점)의 감지 결과 묶음. video → core 로 넘어가는 데이터 계약.
struct DetectionFrame {
    int channel = 0;

    // 프레임을 받은 시각
    std::chrono::steady_clock::time_point received_at;
    // 프레임을 받았을 때 감지되는 객체들의 목록
    std::vector<Detection> objects;

    // 사람 객체 수 (occupancy). 요양보호사 진입 감지·재실 판단용. -> 사람 인원 수 카운트
    int humanCount() const {
        int n = 0;
        for (const auto& o : objects) { 
            if (o.isHuman()) ++n;   //type이 human이면 +1
        }
        return n;
    }
};

// 사람의 발끝(바운딩 박스 하단의 중앙)이 침대 ROI(다각형) 밖으로 나갔는지 판정.
// 침상이탈 판단
inline bool isFeetInRoi(const Detection& det, const std::vector<std::pair<float, float>>& roi) {
    if (roi.empty()) return false;

    // 발끝 좌표 (좌우중앙,하단)
    float px = (det.left + det.right) / 2.0f;
    float py = det.bottom;

    bool inside = false;
    // roi박스가 4개의 점이므로 4개의 변 (i와 j)
    for (size_t i = 0, j = roi.size() - 1; i < roi.size(); j = i++) {
        const float xi = roi[i].first, yi = roi[i].second;  // i번 꼭지점 (x,y)
        const float xj = roi[j].first, yj = roi[j].second;  // j번 꼭지점 (x,y)

        // yi,yj를 잇는 변이 py를 지나가는가
        if (((yi > py) != (yj > py)) &&
            (px < (xj - xi) * (py - yi) / (yj - yi) + xi))  // 그 선분과 만나는 x좌표보다 사람이 왼쪽에 있는가
            inside = !inside;
    }

    return inside;
}

// 침대 1개 = ROI 1개. 한 채널(=한 병실 시야)에 침대가 여러 개라서 ROI도 여러
// 개이고, 채널 안에서 roi_id로 구분한다. 침대마다 입소자가 다르므로
// resident_id를 같이 들고 다닌다 — 이게 "이 침대는 누구 자리"라는 매핑이고,
// IdentityTracker가 추적 객체에 사람 이름을 붙일 때의 앵커가 된다.
struct BedZone {
    int roi_id = 0;         // 채널 안 침대 번호 (0~DBJ_ROI_MAX_ZONES-1)
    int resident_id = 0;    // residents.resident_id (0 = 아직 입소자 미지정)
    std::vector<std::pair<float, float>> points;  // 정규화 0~1 다각형

    // 다각형이 되려면 최소 3점. 2점 이하는 그리다 만 것으로 보고 무시한다.
    bool valid() const { return points.size() >= 3; }
};

// 어느 침대에도 속하지 않음 (BedZone::roi_id 자리에 쓰는 센티널)
constexpr int kNoZone = -1;

// 사람의 발끝이 들어있는 침대의 roi_id. 어디에도 없으면 kNoZone.
// 침대 ROI끼리 겹쳐 그려졌으면 roi_id가 작은 쪽이 이긴다(std::map은 키 오름차순
// 순회 — 먼저 그린 침대 우선). 사람 하나가 두 침대에 동시에 속하면 이후의
// 이탈/귀속 판정이 매 프레임 흔들리므로, 자의적이더라도 하나로 못박는다.
inline int feetInWhichZone(const Detection& det,
                           const std::map<int, BedZone>& zones) {
    for (const auto& entry : zones) {
        if (entry.second.valid() && isFeetInRoi(det, entry.second.points))
            return entry.first;
    }
    return kNoZone;
}