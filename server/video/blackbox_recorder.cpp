#include "blackbox_recorder.hpp"

#include <cstdio>
#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
}

namespace {
int64_t nowUnixMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// 한 클립의 절대 상한. 이벤트가 계속 재트리거되면 postDeadline_이 끝없이 밀리는데,
// armed 동안에는 pre 버퍼 트리밍이 멈춰 있어 그동안 압축 패킷이 계속 쌓인다.
// 몇 Mbps 스트림이면 1분에 수십 MB라, 라즈베리파이에서 오래 끌면 위험하다.
// 여기서 한 번 끊고 다음 트리거가 새 클립을 시작하게 한다.
constexpr double kMaxArmedSec = 60.0;

std::chrono::steady_clock::duration toDuration(double sec) {
    // duration<double>을 time_point에 바로 더하면 double 기반 time_point가 나와서
    // steady_clock::time_point에 대입이 안 된다 (실제 빌드에서 걸린 에러).
    return std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double>(sec));
}
}  // namespace

BlackboxRecorder::BlackboxRecorder(int channel, std::string outputDir,
                                   double preSec, double postSec)
    : channel_(channel), outputDir_(std::move(outputDir)),
      preSec_(preSec), postSec_(postSec) {}

BlackboxRecorder::~BlackboxRecorder() {
    if (codecpar_) avcodec_parameters_free(&codecpar_);
}

void BlackboxRecorder::setStreamInfo(const AVCodecParameters* codecpar,
                                     int timeBaseNum, int timeBaseDen) {
    std::lock_guard<std::mutex> lock(mutex_);

    // 재연결 등으로 스트림 정보가 갱신되는 순간 이벤트가 걸려 있었다면
    // (armed_) 버리지 말고 지금까지 버퍼만이라도 저장해준다.
    if (armed_ && !buf_.empty()) {
        flushLocked();
    }

    if (codecpar_) avcodec_parameters_free(&codecpar_);
    codecpar_ = avcodec_parameters_alloc();
    avcodec_parameters_copy(codecpar_, codecpar);
    tbNum_ = timeBaseNum;
    tbDen_ = timeBaseDen;
    haveStreamInfo_ = true;
    buf_.clear();
    armed_ = false;
}

void BlackboxRecorder::onPacket(const AVPacket* pkt) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!haveStreamInfo_) return;

    PacketRecord rec;
    rec.data.assign(pkt->data, pkt->data + pkt->size);
    rec.pts = pkt->pts;
    rec.dts = pkt->dts;
    rec.flags = pkt->flags;
    buf_.push_back(std::move(rec));

    if (armed_) {
        if (dueLocked()) {
            flushLocked();
        }
        return;
    }

    // 트리거 전(평시)에는 preSec_ + 여유분(GOP 하나)만큼만 유지 — 그보다
    // 오래된 패킷은 버린다. 정확히 preSec_ 경계에서 자르지 않고 여유를 두는
    // 이유는, flush 시점에 "가장 가까운 키프레임부터" 잘라내기 위해서다
    // (GOP 경계와 preSec_ 경계가 딱 맞아떨어지지 않을 수 있음).
    // 수신 초기 패킷은 pts가 아직 없을 수(NOPTS) 있어 dts로 폴백하고,
    // 둘 다 없는 패킷이 양끝에 걸리면 그 턴은 트리밍을 건너뛴다
    // (NOPTS는 거대한 음수라 age 계산에 섞이면 버퍼를 통째로 비우거나
    //  안 자르는 오동작이 된다).
    constexpr double kGopMarginSec = 2.0;
    const double keepSec = preSec_ + kGopMarginSec;
    const double tb = static_cast<double>(tbNum_) / tbDen_;
    auto stampOf = [](const PacketRecord& r) {
        return (r.pts != AV_NOPTS_VALUE) ? r.pts : r.dts;
    };
    while (buf_.size() > 1) {
        const int64_t newest = stampOf(buf_.back());
        const int64_t oldest = stampOf(buf_.front());
        if (oldest == AV_NOPTS_VALUE) {
            buf_.pop_front();  // 나이를 잴 수 없는 앞쪽 패킷 — 버리고 계속
            continue;
        }
        if (newest == AV_NOPTS_VALUE) break;  // 최신 쪽은 다음 패킷에서 재평가
        double age = (newest - oldest) * tb;
        if (age <= keepSec) break;
        buf_.pop_front();
    }
}

int64_t BlackboxRecorder::trigger(const std::string& eventType) {
    std::lock_guard<std::mutex> lock(mutex_);

    // 종류가 다른 이벤트가 겹치면 지금 것을 먼저 끊어 저장한다.
    // 파일명은 ch{채널}_{시각}_{종류}.mp4 인데 한 파일에 두 종류를 쓸 수 없다.
    // 합쳐 버리면 저장은 첫 종류로 되는데 Qt는 두 번째 종류로 요청해서
    // 없는 파일을 찾게 된다(낙상 직후 침상 이탈이 실제로 자주 겹친다).
    // 끊고 새로 시작하면 두 번째 클립에 pre 구간이 거의 없지만, 그 앞부분은
    // 이미 첫 클립에 담겨 있다.
    if (armed_ && eventType != eventType_) {
        flushLocked();   // armed_를 false로 되돌린다
    }

    const auto now = std::chrono::steady_clock::now();
    if (!armed_) {
        armed_ = true;
        eventUnixMs_ = nowUnixMs();
        eventType_ = eventType;
        // 재트리거로 밀리지 않는 상한 — 여기서만 정한다
        armDeadline_ = now + toDuration(kMaxArmedSec);
    }
    // 재트리거(짧은 시간 안에 같은 이벤트가 다시 발생)면 post 구간을 그만큼 연장
    postDeadline_ = now + toDuration(postSec_);
    return eventUnixMs_;
}

void BlackboxRecorder::flush() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (armed_) flushLocked();
}

void BlackboxRecorder::flushIfDue() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (armed_ && dueLocked()) flushLocked();
}

bool BlackboxRecorder::dueLocked() const {
    const auto now = std::chrono::steady_clock::now();
    return now >= postDeadline_ || now >= armDeadline_;
}

void BlackboxRecorder::flushLocked() {
    armed_ = false;
    if (buf_.empty() || !codecpar_) {
        buf_.clear();
        return;
    }

    // 맨 앞부터 첫 키프레임 전까지는 단독 디코딩이 불가능하므로 버린다.
    while (!buf_.empty() && !(buf_.front().flags & AV_PKT_FLAG_KEY)) {
        buf_.pop_front();
    }
    if (buf_.empty()) {
        // std::fprintf(stderr, "[blackbox] ch%d 키프레임 없음 — 저장 취소\n", channel_);
        return;
    }

    char path[256];
    std::snprintf(path, sizeof(path), "%s/ch%d_%lld_%s.mp4", outputDir_.c_str(),
                  channel_, static_cast<long long>(eventUnixMs_), eventType_.c_str());

    // 최종 파일명으로 바로 쓰면, 아직 다 안 써진 파일을 HTTP 서버가 서빙해
    // 클라이언트가 반쪽 파일을 재생하게 된다. 임시 파일(.part)에 다 쓴 뒤
    // 원자적으로 rename해서, 최종 .mp4는 "완성된 순간"에만 나타나게 한다.
    char tmp_path[288];
    std::snprintf(tmp_path, sizeof(tmp_path), "%s.part", path);

    AVFormatContext* ofmt = nullptr;
    avformat_alloc_output_context2(&ofmt, nullptr, "mp4", tmp_path);
    if (!ofmt) {
        // std::fprintf(stderr, "[blackbox] ch%d 출력 컨텍스트 생성 실패\n", channel_);
        buf_.clear();
        return;
    }

    AVStream* out_stream = avformat_new_stream(ofmt, nullptr);
    avcodec_parameters_copy(out_stream->codecpar, codecpar_);
    out_stream->time_base = AVRational{tbNum_, tbDen_};

    if (!(ofmt->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&ofmt->pb, tmp_path, AVIO_FLAG_WRITE) < 0) {
            // std::fprintf(stderr, "[blackbox] ch%d 파일 열기 실패: %s\n", channel_, tmp_path);
            avformat_free_context(ofmt);
            buf_.clear();
            return;
        }
    }

    // moov 아톰을 앞쪽에 둬서(faststart) HTTP로 순차 다운로드하며 바로
    // 재생을 시작할 수 있게 한다 (기본값은 파일 맨 끝에 써서, 전체를
    // 받아야 재생 가능해짐).
    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "movflags", "faststart", 0);
    if (avformat_write_header(ofmt, &opts) < 0) {
        // std::fprintf(stderr, "[blackbox] ch%d 헤더 쓰기 실패\n", channel_);
        av_dict_free(&opts);
        avio_closep(&ofmt->pb);
        avformat_free_context(ofmt);
        buf_.clear();
        return;
    }
    av_dict_free(&opts);

    // pts/dts를 정리한 뒤 0부터 시작하도록 재기준.
    // RTSP 수신 초기 구간에는 pts가 아직 없는 패킷("pts has no value" 경고의
    // 원인)이나, RTCP 시계 동기화 시점에 타임라인이 뒤로 튀는 패킷이 섞일 수
    // 있다(음수 timestamp "out of range" 경고의 원인 — 실측). mp4는 누락·
    // 역행 타임스탬프를 허용하지 않으므로, 누락은 이웃 값으로 채우고 역행은
    // 직전 값 뒤로 밀어 단조 증가를 강제한다.
    int64_t base = AV_NOPTS_VALUE;
    for (const auto& rec : buf_) {  // 첫 유효 타임스탬프를 기준점으로
        if (rec.dts != AV_NOPTS_VALUE) { base = rec.dts; break; }
        if (rec.pts != AV_NOPTS_VALUE) { base = rec.pts; break; }
    }
    if (base == AV_NOPTS_VALUE) base = 0;

    int64_t last_dts = -1;  // 직전에 쓴 dts (재기준 후)
    int64_t est_dur = 0;    // 최근 관측한 프레임 간격 (타임스탬프 전부 누락 대비)
    for (auto& rec : buf_) {
        int64_t dts = (rec.dts != AV_NOPTS_VALUE) ? rec.dts : rec.pts;
        int64_t pts = (rec.pts != AV_NOPTS_VALUE) ? rec.pts : dts;
        if (dts == AV_NOPTS_VALUE) {  // 둘 다 없음 — 직전 프레임에서 이어붙임
            dts = pts = base + last_dts + (est_dur > 0 ? est_dur : 1);
        }
        dts -= base;
        pts -= base;
        if (dts <= last_dts) dts = last_dts + 1;  // 역행/중복 → 단조 증가 강제
        if (pts < dts) pts = dts;                 // mp4 규칙: pts >= dts
        if (last_dts >= 0 && dts - last_dts > 1) est_dur = dts - last_dts;
        last_dts = dts;

        AVPacket* pkt = av_packet_alloc();
        av_new_packet(pkt, static_cast<int>(rec.data.size()));
        std::memcpy(pkt->data, rec.data.data(), rec.data.size());
        pkt->pts = pts;
        pkt->dts = dts;
        pkt->flags = rec.flags;
        pkt->stream_index = out_stream->index;
        av_interleaved_write_frame(ofmt, pkt);
        av_packet_free(&pkt);
    }

    av_write_trailer(ofmt);
    avio_closep(&ofmt->pb);
    avformat_free_context(ofmt);

    // 다 썼으니 최종 이름으로 원자적 노출 — 이 시점부터 HTTP로 재생 가능.
    if (std::rename(tmp_path, path) != 0) {
        // std::fprintf(stderr, "[blackbox] ch%d rename 실패 (%s → %s)\n", channel_,
        //              tmp_path, path);
        std::remove(tmp_path);
        buf_.clear();
        return;
    }

    // std::fprintf(stderr, "[blackbox] ch%d 저장 완료: %s\n", channel_, path);

    if (onClipReady_) onClipReady_(channel_, path, eventUnixMs_);
    buf_.clear();
}