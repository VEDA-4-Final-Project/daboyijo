#include "video_pipeline.hpp"

#include <chrono>
#include <map>
#include <utility>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

namespace {

// 캡처·송출 모두 1280x720로 통일 → raw.size()==kViewSize라 리사이즈는 clone만
// 수행(video_pipeline.cpp의 크기 분기 참조). AI 크롭 해상도도 720p 확보.
const cv::Size kViewSize(1280, 720);
const std::vector<int> kJpegParams = {cv::IMWRITE_JPEG_QUALITY, 80};
// 프레임 레이트 방어선 — 너무 빨리 들어온 프레임은 버려서 발열·CPU 폭주 방지
constexpr double kMainProcessInterval = 1.0 / 15.0;  // 최대 15fps

}  // namespace

void VideoPipeline::run(const volatile std::sig_atomic_t& stop) {
    std::map<int, std::chrono::steady_clock::time_point> last_proc_time;

    while (!stop) {
        // 대기실(FrameQueue)에서 프레임을 하나 꺼냄 (최대 200ms 대기)
        auto frame = queue_.pop(std::chrono::milliseconds(200));
        
        if (frame) {
            std::map<int, decltype(frame)> latest_frames;
            
            // 첫 프레임을 바구니에 담아둡니다.
            latest_frames[frame->channel] = std::move(frame);
            
            while (true) {
                // 대기 시간 0ms로 대기실에 바로 꺼낼 수 있는 다음 프레임이 있는지 즉시 검사
                auto next_frame = queue_.pop(std::chrono::milliseconds(0)); 
                if (!next_frame) {
                    break; // 더 이상 쌓여있는 프레임이 없다면 루프 탈출!
                }
                
                // 해당 채널의 슬롯에 최신 프레임으로 덮어씁니다. (기존 낡은 프레임은 자동 소멸)
                latest_frames[next_frame->channel] = std::move(next_frame);
            }

            // 채널별 바구니에 수집된 진짜 '최신 프레임들'만 순서대로 처리합니다.
            for (auto& [ch_id, latest_frame] : latest_frames) {
                const auto now = std::chrono::steady_clock::now();
                
                // 너무 빨리 들어온 프레임은 버려서 라즈베리 파이 발열 방지 (최대 15fps 제한)
                if (std::chrono::duration<double>(now - last_proc_time[latest_frame->channel]).count() < kMainProcessInterval) {
                    continue;  // 15fps 초과분은 버림
                }
                last_proc_time[latest_frame->channel] = now;

                const auto t0 = std::chrono::steady_clock::now();

                // 수신단은 원본 해상도 BGR로 보낸다 — MoveNet 크롭 해상도 확보용
                cv::Mat raw = std::move(latest_frame->image);
                cv::Mat small;
                if (raw.size() != kViewSize) {
                    cv::resize(raw, small, kViewSize);
                } else {
                    small = raw.clone();  // 크기가 같아도 버퍼 공유 금지 (마스킹 오염 방지)
                }
                
                // AI 전달용 깨끗한 복사본
                cv::Mat clean = small.clone();

                // 이 프레임의 생성 시각과 가장 궁합이 맞는 감지 좌표 선택 (칼싱크 동기화)
                auto dets = store_.closestTo(latest_frame->channel, latest_frame->received_at);

                // 송출 영상 가공 단계 실행 (등록된 프라이버시 블러 마스킹 스테이지 등)
                for (auto& stage : stages_) {
                    stage(latest_frame->channel, small, dets);
                }

                // AI 워커에 최신 일감 던지기 (덮어쓰기 방식 — 밀림 방지)
                ai_.submit({std::move(raw), std::move(clean), latest_frame->channel, std::move(dets)});

                // 가공 완료된 이미지를 인코딩해서 Qt로 송출
                std::vector<unsigned char> jpeg;
                cv::imencode(".jpg", small, jpeg, kJpegParams);
                const size_t bytes = jpeg.size();
                server_.broadcast(latest_frame->channel, std::move(jpeg));

                // 송출 소요 시간 측정 및 통계 기록
                const double ms = std::chrono::duration<double, std::milli>(
                                      std::chrono::steady_clock::now() - t0)
                                      .count();
                stats_.onFrameSent(latest_frame->channel, bytes, ms);
            }
        }
        stats_.maybeReport();
    }
}