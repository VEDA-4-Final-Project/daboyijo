#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "clip_http_server.hpp"
#include "nvr_recorder.hpp"
#include "rtsp_av_client.hpp"

// ══ [NVR] 모듈 — 연속 녹화. 담당 범위는 이 파일 + video/nvr_recorder.*,
//    core/clip_http_server.* ══
//
// 블랙박스(이벤트 트리거 전/후 클립)와 달리 항상 기록한다. USB 등 외장
// 저장장치 경로(storagePath)가 없거나 쓰기 불가하면 조용히 비활성화되고
// 서버의 나머지 기능은 그대로 동작한다(startHttp/attachChannel이 no-op).
class NvrModule {
public:
    // storagePath 가 비어 있거나 mkdir/쓰기 확인에 실패하면 비활성 상태로 생성됨
    NvrModule(std::string storagePath, int retentionHours, double segmentMinutes,
              int httpPort);
    ~NvrModule();

    NvrModule(const NvrModule&) = delete;
    NvrModule& operator=(const NvrModule&) = delete;

    // HTTP 서버 시작 — 실패해도 녹화는 계속 (경고만). 비활성 상태면 no-op
    void startHttp();
    void stopHttp();

    // 카메라 1대 배선 — 압축 패킷/스트림 정보 콜백 등록 + 레코더 생성. 비활성 상태면 no-op
    void attachChannel(RtspAvClient& client);

    // 종료 시점에 진행 중이던 세그먼트를 마감 (유실 방지)
    void flushAll();

    bool enabled() const { return enabled_; }

private:
    // 보존기간 지난 세그먼트를 주기적으로 삭제
    void retentionLoop();

    const std::string storagePath_;
    const int retentionHours_;
    const double segmentMinutes_;
    const int httpPort_;
    bool enabled_ = false;

    std::map<int, std::unique_ptr<NvrRecorder>> recorders_;
    std::mutex recorders_mutex_;

    std::atomic<bool> retention_running_{false};
    std::condition_variable retention_cv_;
    std::mutex retention_mutex_;
    std::thread retention_;

    ClipHttpServer http_;
};
