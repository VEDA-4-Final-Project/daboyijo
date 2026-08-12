#ifndef VEDA_MESSAGES_HPP
#define VEDA_MESSAGES_HPP

#include <string>
#include <nlohmann/json.hpp>

// 노드 사이를 오가는 MQTT 페이로드 정의 — JSON 규격 그 자체
//
// 전송 방식과 분리한 이유 — Pi 노드들은 libmosquitto, Qt 관제 앱은 QMqttClient 로
// 붙어서 라이브러리가 다름, 주고받는 JSON 만 같으면 되므로 그 계약만 여기 모음
//
// ⚠️ 필드를 바꾸면 보내는 쪽과 받는 쪽을 같이 고칠 것
//    한쪽만 바꾸면 컴파일은 되고 런타임에 파싱이 실패함


// 웨어러블 → 중계 노드 → 브로커 (veda/wearable/data)
struct WearableData {
    std::string device_id; // 웨어러블 디바이스 id
    bool is_fall_detected;
    int heart_rate;
    int spo2;
    int steps;             // 만보기 누적 걸음 수 (BLE 에선 2바이트 lo→hi)
    long long timestamp;
};

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(WearableData, device_id, is_fall_detected, heart_rate, spo2, steps, timestamp)

// 중앙 노드 / 관제 앱 → 브로커 → 알림 노드 (veda/alarm/control)
struct AlarmCommand {
    // 대상
    std::string target_device;  // 알림 노드 id
    int         room;           // 호실 번호. 0 = 호실을 특정할 수 없음(알림 노드가
                                // 이때만 server 가 준 message 를 그대로 띄운다)

    // 이벤트
    std::string type;           // FALL | EGRESS | VITAL_ABNORMAL | CONTROL
    std::string message;        // 로그 기록용 + 알림 노드 LED 표시용

    // 오디오
    std::string audio_action;
    std::string audio_file;
    int         volume;
    bool        loop;

    // LED 매트릭스
    std::string matrix_action;  // SHOW | CLEAR | NONE
    int         matrix_passes;  // 반복 스크롤 횟수
    int         brightness;     // 0 ~ 255

    long long timestamp;
};

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(AlarmCommand,
    target_device, room,
    type, message,
    audio_action, audio_file, volume, loop,
    matrix_action, matrix_passes, brightness,
    timestamp)

#endif // VEDA_MESSAGES_HPP
