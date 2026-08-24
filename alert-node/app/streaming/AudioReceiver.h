#ifndef AUDIORECEIVER_H
#define AUDIORECEIVER_H

#include <string>
#include <thread>
#include <atomic>
#include <functional>
#include <gst/gst.h>

class AudioReceiver {
public:
    AudioReceiver();
    ~AudioReceiver();

    // hw:2,0 점유 해제 및 복구용 콜백 함수 타입 정의
    using PreemptCallback = std::function<void()>;
    using RestoreCallback = std::function<void()>;

    // 콜백 등록
    void setPreemptCallback(PreemptCallback onPreempt) { m_onPreempt = onPreempt; }
    void setRestoreCallback(RestoreCallback onRestore) { m_onRestore = onRestore; }

    // 비동기 수신 시작 / 중지
    bool start(int port = 5000, const std::string &alsaDevice = "hw:2,0");
    void stop();

    bool isRunning() const;

private:
    void workerLoop(int port, std::string alsaDevice);

    std::atomic<bool> m_isRunning{false};
    std::thread m_workerThread;

    PreemptCallback m_onPreempt = nullptr;
    PreemptCallback m_onRestore = nullptr;

    GstElement *m_pipeline = nullptr;
    GMainLoop *m_mainLoop = nullptr;
};

#endif // AUDIORECEIVER_H
