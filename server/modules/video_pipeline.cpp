#include "video_pipeline.hpp"

#include <chrono>
#include <map>
#include <thread>
#include <utility>
#include <vector>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

namespace {

// 송출·가공(블러/인코딩)용 해상도. frame_queue.hpp의 kViewWidth/Height와 동일.
// 축소는 디코딩 스레드의 sws_scale이 미리 해 두므로(frame->view), 여기선 그걸
// 그대로 쓴다. view가 비어 있을 때만 폴백으로 cv::resize.
const cv::Size kViewSize(kViewWidth, kViewHeight);
const std::vector<int> kJpegParams = {cv::IMWRITE_JPEG_QUALITY, 65};
// 프레임 레이트 방어선(폭주 방지). 판정은 벽시계가 아니라 촬영시각(PTS)으로 한다
// (아래 runChannel 참조) — 디코딩이 버스티하게 뭉쳐 나와도 촬영 간격이 정상이면 통과.
// ★ 운영: 단일 Pi 4채널. 한 대가 4채널 디코딩+블러+인코딩+연산을 모두 지므로
//   카메라는 서브프로파일(H.264 1280x720 이하 / 10~15fps)로 맞춘다(cameras.conf 참고).
//   캡(아래 kMainProcessInterval)은 폭주 차단용이라 입력 fps보다 넉넉히 위면 된다.
//   ★★ 캡은 반드시 입력보다 "넉넉히" 위여야 한다 — 입력과 같으면(예: 입력 30fps=33.3ms,
//   캡 30fps=33.3ms) 경계선이라 PTS 미세지터로 프레임이 버려져 실효 fps가 주저앉는다
//   (실측: 30fps 입력인데 캡이 1/30이라 out 17.6까지 하락). 40fps로 올려 30fps를 통과.
//   진짜 폭주(오설정 고fps)는 이 캡 + 큐 드랍이 막는다. (RTSP 수신단 kMaxConvertFps=60)
constexpr double kMainProcessInterval = 1.0 / 40.0;  // 40fps 초과만 방어(폭주 차단)

// 스냅샷 버퍼 갱신 주기. care_qa는 최근 5초에서 3장만 뽑아 VLM에 보내므로
// (recentKeyframes(ch, 3, 5.0)) 매 프레임 인코딩할 필요가 없다. 채널당 이 주기로만
// 스냅샷용 JPEG를 새로 만들어 인코딩 부하를 크게 줄인다. 실시간 Qt 송출은 별도로
// "클라가 붙어 있을 때만" 인코딩하므로, 아무도 안 보는 프레임은 인코딩하지 않는다.
constexpr double kSnapshotInterval = 1.0 / 2.0;  // 스냅샷 버퍼 2fps 갱신

// (테스트 후 싱크가 미세하게 안 맞으면 이 값을 늘리거나 줄여서 칼싱크 튜닝 가능!)
constexpr auto kDelayOffset = std::chrono::milliseconds(200);

}  // namespace

void VideoPipeline::run(const volatile std::sig_atomic_t& stop) {
    // 채널마다 전용 스레드 — 서로의 처리 지연·큐 드랍에 영향받지 않는다(격리).
    std::vector<std::thread> threads;
    for (const auto& entry : channel_queues_) {
        const int channel = entry.first;
        FrameQueue* q = entry.second;
        threads.emplace_back(
            [this, channel, q, &stop]() { runChannel(channel, *q, stop); });
    }
    for (auto& t : threads) t.join();
}

void VideoPipeline::runChannel(int /*channel*/, FrameQueue& queue,
                               const volatile std::sig_atomic_t& stop) {
    // 이 스레드는 한 채널만 담당 → 채널별 상태가 map이 아니라 스칼라로 충분.
    // (frame->channel이 곧 이 스레드의 채널이라 본문은 그대로 frame->channel 사용)
    std::chrono::steady_clock::time_point last_proc_ts{};  // 마지막 처리 프레임의 촬영시각
    std::chrono::steady_clock::time_point last_snap_time{};

    while (!stop) {
        auto frame = queue.pop(std::chrono::milliseconds(200));
        if (frame) {
            const auto now = std::chrono::steady_clock::now();
            // 레이트 방어는 벽시계(now)가 아니라 프레임 촬영시각(received_at, PTS 기반)
            // 으로 판정한다. 디코딩이 버스티하게 뭉쳐 나와도 촬영 간격이 정상(20fps=50ms)
            // 이면 다 통과시키고, 진짜 고fps(촬영 간격 자체가 짧음)만 걸러낸다.
            if (std::chrono::duration<double>(frame->received_at - last_proc_ts)
                    .count() < kMainProcessInterval) {
                continue;  // 목표 fps 초과분만 버림
            }
            last_proc_ts = frame->received_at;

            const auto t0 = std::chrono::steady_clock::now();

            // 수신단은 원본 해상도 BGR로 보낸다 — MoveNet 크롭 해상도 확보용
            // (rtsp_av_client.cpp 상단 실험 기록 참조). GUI용 축소는 여기서.
            cv::Mat raw = std::move(frame->image);
            cv::Mat small;

            // 축소본은 디코딩 스레드가 sws_scale로 미리 만들어 둔다 → 여기선 이동만
            // (병목 스레드에서 resize 부하 제거). view가 없는 경우에만 폴백 resize.
            if (!frame->view.empty()) small = std::move(frame->view);
            else if (raw.size() != kViewSize) cv::resize(raw, small, kViewSize);
            else small = raw.clone();

            // AI 전달용 깨끗한 복사본 (블러 전 원본 — 낙상 선택본 복원 소스로도 씀)
            cv::Mat clean = small.clone();

            // resize+clone 소요만 따로 (전체 처리시간 중 준비 단계 비중 진단용)
            const double prep_ms = std::chrono::duration<double, std::milli>(
                                       std::chrono::steady_clock::now() - t0)
                                       .count();

            // 이 프레임의 생성 시각과 가장 궁합이 맞는 감지 좌표 선택
            auto dets = store_.closestTo(frame->channel, frame->received_at - kDelayOffset);

            // 송출 영상 가공 단계 실행 (블러 마스킹 등) → small = 전원 블러본
            for (auto& stage : stages_) {
                stage(frame->channel, small, dets);
            }

            // 낙상 중이면 "낙상자만 노출한 선택본" 생성 (전원 블러본 + clean 기반).
            // ai_.submit이 clean·dets를 move 하기 전에 만들어야 한다.
            cv::Mat selective;
            const bool has_selective =
                fall_variant_ &&
                fall_variant_(frame->channel, small, clean, dets, selective);

            // AI 워커에 최신 일감 던지기 (덮어쓰기 방식 — 밀림 방지)
            ai_.submit({std::move(raw), std::move(clean), frame->channel,
                        std::move(dets)});

            // imencode 순수 소요만 따로 누적 (전체 처리시간 중 인코딩 비중 진단용)
            double encode_ms = 0;
            auto encode = [&](const cv::Mat& img,
                              std::vector<unsigned char>& out) {
                const auto e0 = std::chrono::steady_clock::now();
                cv::imencode(".jpg", img, out, kJpegParams);
                encode_ms += std::chrono::duration<double, std::milli>(
                                 std::chrono::steady_clock::now() - e0).count();
            };

            // 인코딩은 Pi에서 가장 비싼 구간이라, 정말 필요한 소비자에게만 인코딩한다.
            //  · 스냅샷 버퍼: care_qa가 5초에 3장만 뽑으므로 2fps 갱신이면 충분.
            //  · Qt 송출: 붙은 클라가 있을 때만.
            const bool snapshot_due =
                std::chrono::duration<double>(now - last_snap_time)
                    .count() >= kSnapshotInterval;
            const bool has_client = server_.clientCount() > 0;

            // 전원 블러본(버퍼 A): 스냅샷 주기이거나, 평상시 클라 송출용일 때만 인코딩.
            std::vector<unsigned char> jpeg_full;
            if (snapshot_due || (!has_selective && has_client))
                encode(small, jpeg_full);
            if (snapshot_due) snapshots_.put(frame->channel, jpeg_full, now);

            // Qt 송출·버퍼 B: 낙상 중이면 선택본, 아니면 전원 블러본.
            size_t bytes = 0;
            if (has_selective) {
                // 낙상 선택본: 스냅샷 주기이거나 클라 송출용일 때만 인코딩.
                std::vector<unsigned char> jpeg_sel;
                if (snapshot_due || has_client) encode(selective, jpeg_sel);
                // [보호자] 낙상 선택본 스냅샷 보관 (텔레그램 낙상 사진용).
                if (snapshot_due) snapshots_fall_.put(frame->channel, jpeg_sel, now);
                if (has_client) {
                    bytes = jpeg_sel.size();
                    server_.broadcast(frame->channel, std::move(jpeg_sel));
                }
            } else if (has_client) {
                // 평상시: 전원 블러본을 복사 없이 그대로 송출 (버퍼 A엔 이미 put 완료).
                bytes = jpeg_full.size();
                server_.broadcast(frame->channel, std::move(jpeg_full));
            }

            if (snapshot_due) last_snap_time = now;

            const double ms = std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - t0)
                                  .count();
            stats_.onFrameSent(frame->channel, bytes, ms, prep_ms, encode_ms);
        }

        stats_.maybeReport();
    }
}
