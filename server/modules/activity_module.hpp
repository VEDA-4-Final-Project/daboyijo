#pragma once

#include <map>
#include <mutex>
#include <string>

#include "veda_messages.hpp"   // WearableData (노드들과 공유하는 JSON 규격)

class Database;

// [일일 리포트] 웨어러블 만보기 → 활동량 기록.
//
// 리포트의 "시간별 활동시간" 을 만드는 유일한 원천이다. 릴레이가 올리는
// WearableData 를 전부 받아 1분 단위로 눌러 담고, 분이 바뀔 때마다
// activity_minute 에 한 줄씩 flush 한다.
//
// ── 왜 매 패킷 저장하지 않는가 ─────────────────────────────
// 릴레이는 초 단위로 발행한다. 그대로 넣으면 4명 기준 하루 34만 행이 되고,
// 시간별 그래프를 그릴 때마다 그걸 전부 훑어야 한다. 1분으로 누르면 하루
// 5760 행이라 조회가 편하고, "몇 분 동안 움직였나" 라는 질문에도 바로 답이 된다.
//
// ── 왜 MqttMasterManager 안에 넣지 않았나 ──────────────────
// 그쪽은 알람 판정(래치·히스테리시스)이 본업이라 관심사가 다르고, 담당자가
// 따로 있어 병합 충돌이 난다. ARCHITECTURE.md 의 "새 기능은 자기 모듈 파일에서"
// 규칙대로 별도 모듈로 두고, main.cpp 에서 콜백 한 줄로 배선한다.
class ActivityModule {
public:
    explicit ActivityModule(Database& db) : db_(db) {}

    // 웨어러블 패킷 1개 도착. mosquitto 네트워크 스레드에서 불린다.
    // 분이 바뀌는 순간 직전 1분치를 DB 에 쓴다(그 안에서만 DB 접근).
    void onWearableData(const WearableData& data);

    // 서버 종료 직전 1회. 아직 flush 되지 않은 마지막 1분치를 저장한다.
    // 이게 없으면 종료 직전 1분은 통째로 사라진다.
    void flushAll();

private:
    // 기기 1대의 누적 상태.
    struct Acc {
        // 직전 패킷의 누적 걸음 수. 분 경계와 무관하게 이어진다 —
        // 분이 바뀌었다고 초기화하면 그 경계에서 걸음이 한 번 누락된다.
        int  prev_steps = -1;          // -1 = 첫 패킷(아직 차분 불가)

        long long minute = -1;         // 지금 모으는 중인 분 (epoch 초, 0초 정렬)
        int  steps_delta = 0;          // 그 분 동안 늘어난 걸음 합
        int  last_cum    = 0;          // 그 분의 마지막 누적값 원본
        long hr_sum = 0;  int hr_n = 0;      // 0(측정실패)은 빼고 평균
        long spo2_sum = 0; int spo2_n = 0;

        int  resident_id = -1;         // device_id → 입소자. 분이 바뀔 때 갱신
    };

    // 모은 1분치를 DB 에 쓰고 카운터를 비운다. mutex_ 를 쥔 채 부를 것.
    void flushLocked(const std::string& device_id, Acc& a);

    std::mutex mutex_;                 // acc_ 보호 (지금은 MQTT 스레드 하나뿐이지만
                                       //  flushAll 이 메인 스레드에서 들어온다)
    std::map<std::string, Acc> acc_;   // device_id → 누적 상태
    Database& db_;
};
