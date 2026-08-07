#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

#include "blackbox_recorder.hpp"
#include "clip_http_server.hpp"
#include "rtsp_av_client.hpp"

// ══ [블랙박스] 모듈 — 담당자는 이 파일과 video/blackbox_recorder.*,
//    core/clip_http_server.* 만 수정하면 된다 ══
//
// 채널별로 최근 몇 초의 압축 영상을 버퍼링하다가 낙상 등 이벤트 발생 시
// [이벤트 전 pre초 + 후 post초] 구간을 mp4로 저장하고, 저장된 클립을
// Qt(QMediaPlayer)가 바로 재생할 수 있게 HTTP로 서빙한다.
// 저장 시간·포트 등 튜닝값은 blackbox_module.cpp 상단에 있다.
class BlackboxModule {
public:
    BlackboxModule();   // 클립 디렉토리 생성 + 저장 감시 스레드 기동
    ~BlackboxModule();  // 감시 스레드 정리

    BlackboxModule(const BlackboxModule&) = delete;
    BlackboxModule& operator=(const BlackboxModule&) = delete;

    // HTTP 서버 시작. 실패해도 녹화 자체는 계속된다(경고만 출력).
    void startHttp();
    void stopHttp();

    // 카메라 1대 배선 — 압축 패킷/스트림 정보 콜백 등록 + 레코더 생성
    void attachChannel(RtspAvClient& client);

    // 이벤트(낙상 등) 발생 — 클립 저장 예약, 파일명에 쓰인 이벤트 시각(unix ms)
    // 반환. Qt가 이 값으로 클립 파일명(ch{채널}_{시각}.mp4)을 재구성해 재생한다.
    int64_t trigger(int channel, const std::string& eventType);

    // 종료 시점에 post 구간 수집 중이던 클립을 못 채운 만큼이라도 저장 (유실 방지)
    void flushAll();

private:
    // 저장 시점이 지난 클립을 주기적으로 확인해 마감한다.
    // BlackboxRecorder는 압축 패킷이 들어올 때만 마감 시각을 볼 수 있어서,
    // 트리거 직후 카메라가 끊기면 그 클립이 저장되지 않은 채 남는다.
    void watchdogLoop();

    std::map<int, std::unique_ptr<BlackboxRecorder>> recorders_;
    std::mutex recorders_mutex_;   // attachChannel(설정) ↔ trigger/감시 스레드

    std::atomic<bool> watchdog_running_{false};
    std::condition_variable watchdog_cv_;
    std::mutex watchdog_mutex_;
    std::thread watchdog_;

    ClipHttpServer http_;
};
