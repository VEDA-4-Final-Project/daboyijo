#ifndef VEDA_MESSAGES_HPP
#define VEDA_MESSAGES_HPP

#include <string>
#include <nlohmann/json.hpp>

// 노드 사이를 오가는 MQTT 페이로드 정의 — 즉 JSON 규격 그 자체.
//
// 전송 방식과 떼어놓은 이유: 라즈베리파이 노드들(중계·중앙·알림)은 libmosquitto 로
// 붙고, 윈도우 Qt 관제 앱은 Qt MQTT(QMqttClient) 로 붙는다. 라이브러리는 서로
// 달라도 주고받는 JSON 은 반드시 같아야 하므로, 그 계약만 여기 모아둔다.
// 이 헤더는 mosquitto 도 Qt 도 필요로 하지 않아 양쪽에서 그대로 include 된다.
//
// ⚠️ 필드를 바꾸면 보내는 쪽과 받는 쪽을 같이 고쳐야 한다. 한쪽만 바꾸면
//    컴파일은 멀쩡히 되고 런타임에 파싱이 실패한다.


// 웨어러블 → 중계 노드 → 브로커 (veda/wearable/data)
struct WearableData {
    std::string device_id; // 웨어러블 디바이스 id
    bool is_fall_detected;
    double temperature;
    int heart_rate;
    int spo2;
    long long timestamp;
};

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(WearableData, device_id, is_fall_detected, temperature,heart_rate, spo2, timestamp)

// 중앙 노드 / 관제 앱 → 브로커 → 알림 노드 (veda/alarm/control)
struct AlarmCommand {
    // 대상
    std::string target_device;  // 알림 노드 id
    int room;           // 호실

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
