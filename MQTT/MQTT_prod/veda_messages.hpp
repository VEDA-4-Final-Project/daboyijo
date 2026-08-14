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

    // 관제 앱 "테스트" 버튼이 보낸, 잠깐 보여주기만 하는 명령인지.
    // 실제 낙상/침상이탈/생체이상 알람과 관제 앱 "테스트"는 둘 다 matrix_action=SHOW,
    // audio_action=PLAY 라 그 두 필드만으로는 구분이 안 된다 — 알림 노드가 "테스트라서
    // 보여준 뒤 원래 밝기·음량으로 되돌려야 하는지"를 판단하려면 이 필드가 꼭 필요하다.
    // 기본값 false — 이 필드를 모르는(재빌드 전) 발신자가 보낸 명령도 "실제 알람"으로
    // 안전하게 취급된다.
    bool        is_test = false;
};

// AlarmCommand 만 매크로 대신 직접 쓴다 — is_test 는 "없으면 false" 로 관대하게 읽어야
// 한다(구버전 발신자가 아직 이 필드를 안 보내도 파싱이 실패하면 안 됨). 매크로가 만드는
// from_json 은 모든 필드에 j.at() 을 써서 하나라도 빠지면 예외를 던지므로 못 쓴다.
// 나머지 필드는 기존과 똑같이 필수로 둔다(하나라도 빠지면 여전히 파싱 실패 — 의도한 대로).
inline void to_json(nlohmann::json& j, const AlarmCommand& c) {
    j = nlohmann::json{
        {"target_device", c.target_device}, {"room", c.room},
        {"type", c.type}, {"message", c.message},
        {"audio_action", c.audio_action}, {"audio_file", c.audio_file},
        {"volume", c.volume}, {"loop", c.loop},
        {"matrix_action", c.matrix_action}, {"matrix_passes", c.matrix_passes},
        {"brightness", c.brightness},
        {"timestamp", c.timestamp},
        {"is_test", c.is_test},
    };
}
inline void from_json(const nlohmann::json& j, AlarmCommand& c) {
    j.at("target_device").get_to(c.target_device);
    j.at("room").get_to(c.room);
    j.at("type").get_to(c.type);
    j.at("message").get_to(c.message);
    j.at("audio_action").get_to(c.audio_action);
    j.at("audio_file").get_to(c.audio_file);
    j.at("volume").get_to(c.volume);
    j.at("loop").get_to(c.loop);
    j.at("matrix_action").get_to(c.matrix_action);
    j.at("matrix_passes").get_to(c.matrix_passes);
    j.at("brightness").get_to(c.brightness);
    j.at("timestamp").get_to(c.timestamp);
    c.is_test = j.value("is_test", false);   // 없으면 false(실제 알람 취급) — 구버전 발신자 호환
}

#endif // VEDA_MESSAGES_HPP
