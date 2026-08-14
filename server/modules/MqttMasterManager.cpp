#include "MqttMasterManager.hpp"
#include <iostream>
#include <nlohmann/json.hpp>
#include <chrono>

namespace {
// 알람 발동 기준
constexpr int kHrAlarmHigh   = 180;   // 심박 이 값 초과 → 이상
constexpr int kSpo2AlarmLow  = 90;    // SpO2 이 값 미만 → 이상
// 정상 복귀 기준 — 발동 기준보다 한 칸 안쪽으로 잡는다(히스테리시스).
// 89 ↔ 90 을 오갈 때 알람이 껐다 켜졌다 하는 걸 막는다.
constexpr int kHrClearHigh   = 175;
constexpr int kSpo2ClearLow  = 93;
// 한 번 튄 값으로 성급히 "정상 복귀"라고 하지 않도록 연속 N회를 요구한다.
constexpr int kNormalStreakToClear = 3;
}  // namespace

MqttMasterManager::MqttMasterManager() {
        mqtt_client_ = std::make_unique<MqttClient_veda>("Main_Master_Module");
}

MqttMasterManager::~MqttMasterManager() {
    if(mqtt_client_) {
        mqtt_client_->stopLoop();
    }
}

bool MqttMasterManager::init(const std::string& broker_ip, int port) {


    std::string ca_path = "../MQTT/certs/ca.crt";
    mqtt_client_->setTlsConfig(ca_path);

    // 콜백·구독은 연결보다 먼저 걸어둔다 — 연결 직후 들어오는 메시지를 놓치지 않고,
    // 구독은 목록에 기록됐다가 on_connect 에서(재접속마다) 자동으로 다시 걸린다.
    mqtt_client_->setCallback([this](const::std::string& t , const std::string& p) {
        this->onMessageReceived(t,p);
    });
    mqtt_client_->subscribeTopic("veda/wearable/data");

    const bool connected = mqtt_client_->connectToBroker(broker_ip, port);

    // ★ 연결 실패해도 네트워크 루프는 반드시 띄운다.
    //   mosquitto 의 자동 재접속(reconnect_delay)은 이 루프 안에서만 돈다.
    //   예전처럼 여기서 return 해버리면 루프가 안 떠서 영원히 오프라인이 되고,
    //   그동안 발행한 알람은 전부 큐에만 쌓인다(사이렌은 안 울린다).
    mqtt_client_->startLoop();

    if(!connected) {
        std::cerr << "[MqttMasterManager] 브로커 최초 연결 실패 (" << broker_ip << ":" << port
                  << ") — 백그라운드에서 재접속을 계속 시도합니다." << std::endl;
        std::cerr << "[MqttMasterManager] TLS CA 경로: " << ca_path
                  << " (실행 위치 기준 상대경로 — 파일이 없으면 연결이 안 됩니다)" << std::endl;
        return false;
    }

    std::cout << "[MqttMasterManager] Subscribed to 'veda/wearable/data'" << std::endl;
    return true;
}


bool MqttMasterManager::checkFallStatus(const WearableData& data, AlarmCommand& out_cmd) {
    if(data.is_fall_detected) {
        out_cmd.target_device = "alarm_rpi_01";
        out_cmd.timestamp = data.timestamp;
        out_cmd.volume = 90;
        out_cmd.loop = true;

        
        out_cmd.message = "낙상 감지";
        out_cmd.audio_action = "PLAY";
        out_cmd.audio_file = "/home/mayoina/study_veda/daboyijo/server/MQTT_dev/build/alarm_node/fall_alert.wav";
        return true;
    }
    return false;
}


void MqttMasterManager::sendAlarmCommand(AlarmEventType event_type, int room ) {

    AlarmCommand cmd;
    cmd.room = room;
    
    if(event_type == AlarmEventType::FALL){
        cmd.type= "FALL";
        cmd.message = "room" + std::to_string(cmd.room) + " FALL";
        cmd.audio_file = "fall_alert.wav";
    }else if(event_type == AlarmEventType::EGRESS){
        cmd.type = "EGRESS";
        cmd.message = "room" + std::to_string(cmd.room) + " EGRESS";
        cmd.audio_file = "egress_alert.wav";
    }else if(event_type == AlarmEventType::VITAL_ABNORMAL){
        cmd.type = "VITAL_ABNORMAL";
        cmd.message = "room"+ std::to_string(cmd.room) + " VITAL_ABNORMAL";
        cmd.audio_file = "vital_alert.wav";
    }


    cmd.target_device = "alarm_rpi_01";
    //cmd.timestamp = data.timestamp;
    // chrono를 이용하여 시스템 타임 스탬프 대입 
    auto now = std::chrono::system_clock::now();
    cmd.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now.time_since_epoch()).count();
    cmd.volume = 90;
    cmd.loop = true;
    cmd.audio_action = "PLAY";

    // led 
    cmd.matrix_action = "SHOW";
    cmd.matrix_passes = 0;
    cmd.brightness = 0;
    
    nlohmann::json j = cmd;
    std::string payload = j.dump();

    // 알람은 "보냈다고 찍는 것"과 "실제로 나간 것"이 반드시 같아야 한다.
    // 브로커가 끊긴 상태면 큐에 담길 뿐 알림 노드는 아무 소리도 내지 않는다.
    const PublishResult pr = mqtt_client_->publish("veda/alarm/control", payload);
    if(pr == PublishResult::Sent) {
        std::cout << "[MqttMasterManager] SentAlarm Command to Node: " << payload << std::endl;
    } else if(pr == PublishResult::Queued) {
        std::cerr << "[MqttMasterManager] ⚠️ 알람 미전송 — 브로커 오프라인. 알림 노드는 울리지 않습니다."
                     " (재접속 시 전송 예정) payload: " << payload << std::endl;
    } else {
        std::cerr << "[MqttMasterManager] ⚠️ 알람 전송 실패. payload: " << payload << std::endl;
    }
}

void MqttMasterManager::onMessageReceived(const std::string& topic, const std::string& payload) {
    try {
        auto j = nlohmann::json::parse(payload);
        auto data = j.get<WearableData>();

        // 알람 판정과 무관하게 원본을 먼저 흘려보낸다(활동량 집계 등).
        // 아래 알람 로직보다 앞에 두는 이유: 여기서 return 되는 경로가 있어도
        // 평상시 데이터는 빠짐없이 쌓여야 한다.
        if(wearable_data_callback_) wearable_data_callback_(data);

        if(!wearable_callback_) return;   // 콜백 미등록 시 호출하면 bad_function_call

        AlarmLatch& latch = latches_[data.device_id];

        // ── 낙상: 사건이다. 플래그가 false→true 로 바뀌는 순간에만 알린다.
        //    기기가 플래그를 몇 초간 유지해도 사이렌은 한 번만 울린다.
        if(data.is_fall_detected){
            if(!latch.fall_alarming){
                latch.fall_alarming = true;
                wearable_callback_(AlarmEventType::FALL, data.device_id);
            }
        } else {
            latch.fall_alarming = false;   // 플래그가 내려가면 다음 낙상을 받을 준비
        }

        // 체온 조건은 제거했다 — WearableData 에서 temperature 필드가 사라졌다.
        // 측정 하드웨어가 없어 항상 0 이었으므로 이 조건은 발동한 적이 없다.
        //
        // ★ 미착용/측정실패는 0 으로 온다. 살아있는 사람의 심박·SpO2 가 0 일 수는
        //   없으므로 0 은 "값 없음"이지 "이상"이 아니다. 이 가드가 없으면
        //   spo2 == 0 이 <90 에 걸려 웨어러블을 벗어둔 내내 사이렌이 울린다.
        const bool hr_valid   = data.heart_rate > 0;
        const bool spo2_valid = data.spo2 > 0;

        // ── 생체신호: 상태다. 이상인 '동안'이 아니라 이상으로 '바뀔 때' 알린다.
        const bool abnormal =
            (hr_valid && data.heart_rate > kHrAlarmHigh) ||
            (spo2_valid && data.spo2 < kSpo2AlarmLow);
        // 복귀 판정은 더 빡빡하게(히스테리시스) — 경계값에서 알람이 떨리지 않게.
        const bool back_to_normal =
            (!hr_valid   || data.heart_rate <= kHrClearHigh) &&
            (!spo2_valid || data.spo2 >= kSpo2ClearLow);

        if(abnormal){
            latch.normal_streak = 0;
            if(!latch.vital_alarming){
                latch.vital_alarming = true;
                wearable_callback_(AlarmEventType::VITAL_ABNORMAL, data.device_id);
            }
            // 이미 알람 중이면 아무것도 하지 않는다 — 관제사가 해제한 경보를
            // 다음 패킷에서 되살리지 않기 위한 핵심 지점이다.
        } else if(back_to_normal && latch.vital_alarming){
            if(++latch.normal_streak >= kNormalStreakToClear){
                latch.vital_alarming = false;
                latch.normal_streak = 0;
                std::cout << "[MqttMasterManager] " << data.device_id
                          << " 생체신호 정상 복귀 — 경보 래치 해제(다음 이상 시 다시 알림)"
                          << std::endl;
            }
        }
        
    } catch (const std::exception& e) {
        std::cerr << "[MqttMasterManager] Parsing Error: " << e.what() << std::endl;
    }
}

void MqttMasterManager::setWearableCallback(WearableCallback cd){
    wearable_callback_ = cd;
}

void MqttMasterManager::setWearableDataCallback(WearableDataCallback cb){
    wearable_data_callback_ = cb;
}
