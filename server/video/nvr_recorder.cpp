#include "nvr_recorder.hpp"

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

std::chrono::steady_clock::duration toDuration(double sec) {
    return std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double>(sec));
}
}  // namespace

NvrRecorder::NvrRecorder(int channel, std::string outputDir, double segmentMinutes)
    : channel_(channel), outputDir_(std::move(outputDir)),
      segmentSec_(segmentMinutes * 60.0) {}

NvrRecorder::~NvrRecorder() {
    std::lock_guard<std::mutex> lock(mutex_);
    closeSegmentLocked();
    if (codecpar_) avcodec_parameters_free(&codecpar_);
}

void NvrRecorder::setStreamInfo(const AVCodecParameters* codecpar,
                                int timeBaseNum, int timeBaseDen) {
    std::lock_guard<std::mutex> lock(mutex_);

    // 재연결로 코덱 파라미터가 바뀔 수 있어 진행 중이던 세그먼트는 마감하고 새로 시작
    closeSegmentLocked();

    if (codecpar_) avcodec_parameters_free(&codecpar_);
    codecpar_ = avcodec_parameters_alloc();
    avcodec_parameters_copy(codecpar_, codecpar);
    tbNum_ = timeBaseNum;
    tbDen_ = timeBaseDen;
    haveStreamInfo_ = true;
}

bool NvrRecorder::openSegmentLocked(const AVPacket* firstPkt) {
    const int64_t startUnixMs = nowUnixMs();
    segmentStartSteady_ = std::chrono::steady_clock::now();

    char path[256];
    std::snprintf(path, sizeof(path), "%s/ch%d_%lld.mp4", outputDir_.c_str(),
                  channel_, static_cast<long long>(startUnixMs));
    curPath_ = path;
    curTmpPath_ = curPath_ + ".part";

    avformat_alloc_output_context2(&ofmt_, nullptr, "mp4", curTmpPath_.c_str());
    if (!ofmt_) return false;

    AVStream* out_stream = avformat_new_stream(ofmt_, nullptr);
    avcodec_parameters_copy(out_stream->codecpar, codecpar_);
    out_stream->time_base = AVRational{tbNum_, tbDen_};

    if (!(ofmt_->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&ofmt_->pb, curTmpPath_.c_str(), AVIO_FLAG_WRITE) < 0) {
            avformat_free_context(ofmt_);
            ofmt_ = nullptr;
            return false;
        }
    }

    // faststart — moov를 앞으로 빼서 다 받기 전에 재생/탐색 시작 가능
    // (세그먼트 마감 때 mdat 재배치가 한 번 더 도는 비용이 있으나, 몇 분짜리
    //  세그먼트 기준 로테이션 시점에만 발생해 실시간 기록 경로엔 영향 없음)
    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "movflags", "faststart", 0);
    if (avformat_write_header(ofmt_, &opts) < 0) {
        av_dict_free(&opts);
        avio_closep(&ofmt_->pb);
        avformat_free_context(ofmt_);
        ofmt_ = nullptr;
        return false;
    }
    av_dict_free(&opts);

    base_ = (firstPkt->dts != AV_NOPTS_VALUE) ? firstPkt->dts
            : (firstPkt->pts != AV_NOPTS_VALUE) ? firstPkt->pts : 0;
    lastDts_ = -1;
    estDur_ = 0;
    return true;
}

void NvrRecorder::closeSegmentLocked() {
    if (!ofmt_) return;
    av_write_trailer(ofmt_);
    avio_closep(&ofmt_->pb);
    avformat_free_context(ofmt_);
    ofmt_ = nullptr;

    if (std::rename(curTmpPath_.c_str(), curPath_.c_str()) != 0) {
        std::remove(curTmpPath_.c_str());
    }
}

void NvrRecorder::onPacket(const AVPacket* pkt) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!haveStreamInfo_) return;

    const bool isKey = pkt->flags & AV_PKT_FLAG_KEY;

    if (!ofmt_) {
        // 첫 키프레임 전까지는 단독 디코딩이 불가능하므로 세그먼트를 열지 않고 버림
        if (!isKey) return;
        if (!openSegmentLocked(pkt)) return;
    } else if (std::chrono::steady_clock::now() - segmentStartSteady_ >=
                   toDuration(segmentSec_) &&
               isKey) {
        closeSegmentLocked();
        if (!openSegmentLocked(pkt)) return;
    }

    // pts/dts를 세그먼트 시작 기준 0부터 단조증가하도록 재기준
    // (BlackboxRecorder::flushLocked와 같은 원리를 패킷 단위 스트리밍으로 적용)
    int64_t dts = (pkt->dts != AV_NOPTS_VALUE) ? pkt->dts : pkt->pts;
    int64_t pts = (pkt->pts != AV_NOPTS_VALUE) ? pkt->pts : dts;
    if (dts == AV_NOPTS_VALUE) {
        dts = pts = base_ + lastDts_ + (estDur_ > 0 ? estDur_ : 1);
    }
    dts -= base_;
    pts -= base_;
    if (dts <= lastDts_) dts = lastDts_ + 1;
    if (pts < dts) pts = dts;
    if (lastDts_ >= 0 && dts - lastDts_ > 1) estDur_ = dts - lastDts_;
    lastDts_ = dts;

    AVPacket* out = av_packet_alloc();
    av_new_packet(out, pkt->size);
    std::memcpy(out->data, pkt->data, pkt->size);
    out->pts = pts;
    out->dts = dts;
    out->flags = pkt->flags;
    out->stream_index = ofmt_->streams[0]->index;
    av_interleaved_write_frame(ofmt_, out);
    av_packet_free(&out);
}

void NvrRecorder::flush() {
    std::lock_guard<std::mutex> lock(mutex_);
    closeSegmentLocked();
}
