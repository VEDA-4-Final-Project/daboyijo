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
        if (std::chrono::steady_clock::now() >= postDeadline_) {
            flushLocked();
        }
        return;
    }

    // 트리거 전(평시)에는 preSec_ + 여유분(GOP 하나)만큼만 유지 — 그보다
    // 오래된 패킷은 버린다. 정확히 preSec_ 경계에서 자르지 않고 여유를 두는
    // 이유는, flush 시점에 "가장 가까운 키프레임부터" 잘라내기 위해서다
    // (GOP 경계와 preSec_ 경계가 딱 맞아떨어지지 않을 수 있음).
    constexpr double kGopMarginSec = 2.0;
    const double keepSec = preSec_ + kGopMarginSec;
    const double tb = static_cast<double>(tbNum_) / tbDen_;
    while (buf_.size() > 1) {
        double age = (buf_.back().pts - buf_.front().pts) * tb;
        if (age <= keepSec) break;
        buf_.pop_front();
    }
}

int64_t BlackboxRecorder::trigger() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!armed_) {
        armed_ = true;
        eventUnixMs_ = nowUnixMs();
    }
    // 재트리거(짧은 시간 안에 이벤트가 다시 발생)면 post 구간을 그만큼 연장.
    postDeadline_ = std::chrono::steady_clock::now() +
                     std::chrono::duration<double>(postSec_);
    return eventUnixMs_;
}

void BlackboxRecorder::flush() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (armed_) flushLocked();
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
        std::fprintf(stderr, "[blackbox] ch%d 키프레임 없음 — 저장 취소\n", channel_);
        return;
    }

    char path[256];
    std::snprintf(path, sizeof(path), "%s/ch%d_%lld.mp4", outputDir_.c_str(),
                  channel_, static_cast<long long>(eventUnixMs_));

    AVFormatContext* ofmt = nullptr;
    avformat_alloc_output_context2(&ofmt, nullptr, "mp4", path);
    if (!ofmt) {
        std::fprintf(stderr, "[blackbox] ch%d 출력 컨텍스트 생성 실패\n", channel_);
        buf_.clear();
        return;
    }

    AVStream* out_stream = avformat_new_stream(ofmt, nullptr);
    avcodec_parameters_copy(out_stream->codecpar, codecpar_);
    out_stream->time_base = AVRational{tbNum_, tbDen_};

    if (!(ofmt->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&ofmt->pb, path, AVIO_FLAG_WRITE) < 0) {
            std::fprintf(stderr, "[blackbox] ch%d 파일 열기 실패: %s\n", channel_, path);
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
        std::fprintf(stderr, "[blackbox] ch%d 헤더 쓰기 실패\n", channel_);
        av_dict_free(&opts);
        avio_closep(&ofmt->pb);
        avformat_free_context(ofmt);
        buf_.clear();
        return;
    }
    av_dict_free(&opts);

    // pts/dts를 0부터 시작하도록 재기준 — 같은 기준값을 pts·dts 모두에
    // 적용해야 둘 사이 간격(리오더링 지연)이 그대로 보존된다.
    const int64_t base = buf_.front().dts;
    const size_t packet_count = buf_.size();
    for (auto& rec : buf_) {
        AVPacket* pkt = av_packet_alloc();
        av_new_packet(pkt, static_cast<int>(rec.data.size()));
        std::memcpy(pkt->data, rec.data.data(), rec.data.size());
        pkt->pts = rec.pts - base;
        pkt->dts = rec.dts - base;
        pkt->flags = rec.flags;
        pkt->stream_index = out_stream->index;
        av_interleaved_write_frame(ofmt, pkt);
        av_packet_free(&pkt);
    }

    av_write_trailer(ofmt);
    avio_closep(&ofmt->pb);
    avformat_free_context(ofmt);

    std::fprintf(stderr, "[blackbox] ch%d 저장 완료: %s (%zu패킷)\n", channel_, path,
                 packet_count);

    if (onClipReady_) onClipReady_(channel_, path, eventUnixMs_);
    buf_.clear();
}
