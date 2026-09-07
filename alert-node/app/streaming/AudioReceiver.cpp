#include "AudioReceiver.h"
#include <iostream>
#include <sstream>
#include <chrono>

AudioReceiver::AudioReceiver() {
    // 프로세스 내에서 GStreamer 최초 1회 초기화
    if (!gst_is_initialized()) {
        gst_init(nullptr, nullptr);
    }
}

AudioReceiver::~AudioReceiver() {
    stop();
}

bool AudioReceiver::start(int port, const std::string &alsaDevice) {
    if (m_isRunning) {
        std::cerr << "[AudioReceiver] 이미 수신 루프가 실행 중입니다.\n";
        return true;
    }

    // isRunning() 을 여기서 먼저 세운다 — preempt 콜백이 도는 동안(잠깐이지만) 다른
    // 스레드가 isRunning()==false 를 보고 ALSA 장치를 새로 열려 들면 우리가 곧 열
    // 장치와 부딪힌다. 파이프라인 스레드는 아직 없어도 "곧 이 장치를 쓴다" 는
    // 사실 자체는 이 시점부터 참이다.
    m_isRunning = true;

    // 1. 기존 재생 스레드가 ALSA hw:2,0을 점유 중인 경우 해제 요청 (동기 대기)
    if (m_onPreempt) {
        std::cout << "[AudioReceiver] 기존 오디오 점유 해제 요청 중...\n";
        m_onPreempt(); // 이 콜백 내에서 기존 재생 중지 & snd_pcm_close 수행
        std::this_thread::sleep_for(std::chrono::milliseconds(50)); // 커널 디바이스 반환 대기
    }

    m_workerThread = std::thread(&AudioReceiver::workerLoop, this, port, alsaDevice);
    return true;
}

void AudioReceiver::workerLoop(int port, std::string alsaDevice) {
    // GStreamer 파이프라인 생성 (검증된 48kHz, Mono, S16LE)
    std::ostringstream ss;
    ss << "udpsrc port=" << port 
       << " caps=\"audio/x-raw, format=S16LE, channels=1, rate=48000, layout=interleaved\" ! "
       << "audioconvert ! audioresample ! "
       << "alsasink device=" << alsaDevice << " sync=false";

    GError *error = nullptr;
    m_pipeline = gst_parse_launch(ss.str().c_str(), &error);

    if (error) {
        std::cerr << "[AudioReceiver] 파이프라인 생성 실패: " << error->message << "\n";
        g_error_free(error);
        m_isRunning = false;
        return;
    }

    m_mainLoop = g_main_loop_new(nullptr, FALSE);

    // 파이프라인 재생 상태 전환
    gst_element_set_state(m_pipeline, GST_STATE_PLAYING);
    std::cout << "[AudioReceiver] 방송 수신 대기 시작 (Port: " << port << ", Device: " << alsaDevice << ")\n";

    // 메인 루프 실행 (블로킹)
    g_main_loop_run(m_mainLoop);

    // 루프 탈출 후 파이프라인 자원 정리
    gst_element_set_state(m_pipeline, GST_STATE_NULL);
    gst_object_unref(m_pipeline);
    g_main_loop_unref(m_mainLoop);
    m_pipeline = nullptr;
    m_mainLoop = nullptr;

    std::cout << "[AudioReceiver] 방송 수신 종료 완료\n";

    // 2. 방송 종료 후 기존 오디오 스레드 상태 복구 알림
    if (m_onRestore) {
        m_onRestore();
    }
}

void AudioReceiver::stop() {
    if (!m_isRunning) return;

    m_isRunning = false;

    if (m_mainLoop) {
        g_main_loop_quit(m_mainLoop);
    }

    if (m_workerThread.joinable()) {
        m_workerThread.join();
    }
}

bool AudioReceiver::isRunning() const {
    return m_isRunning;
}
