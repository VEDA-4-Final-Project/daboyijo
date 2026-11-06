#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "snapshot_buffer.hpp"

// [영상검색] 사건 시점 블러 프레임 보관소.
//
// 블랙박스 클립(mp4)은 압축 패킷을 그대로 remux한 "원본"이라 얼굴 블러가 없다.
// 그래서 영상 검색이 사건 상황을 VLM에 설명시킬 때 클립을 디코딩해 보내면
// 원본 얼굴이 외부로 나간다. 대신 사건이 터진 순간, 이미 블러가 적용된
// 스냅샷 버퍼(버퍼 A)에서 사건 전후 프레임을 골라 클립 옆에 JPEG로 저장해 둔다.
//
//   ch0_1730000000000_FALL.mp4        ← 블랙박스 클립 (Qt 재생용, 원본)
//   ch0_1730000000000_FALL_f0.jpg     ← 사건 전후 블러 프레임 (VLM 설명용)
//   ...
//   ch0_1730000000000_FALL_f7.jpg
//
// 검색(video_search_module)은 결과 중 가장 최근 사건의 프레임을 load()로 읽어
// VLM에 보내고 "어떻게 일어났는지" 설명을 답변에 붙인다.
class EventFrameStore {
public:
    using Jpeg = SnapshotBuffer::Jpeg;

    // 저장 프레임 수. 스냅샷 버퍼가 2fps·5초 보관이라 최대 10장 — 그중 8장.
    static constexpr int kFrames = 8;

    // snaps: 반드시 "전원 블러본" 버퍼(버퍼 A)를 넘길 것 — 낙상 선택본(버퍼 B)은
    //        낙상자 얼굴이 노출돼 있어 외부 VLM으로 보내면 안 된다.
    // dir  : 블랙박스 클립 디렉토리와 같은 곳(clip_url 파일명과 짝을 맞추기 위해).
    explicit EventFrameStore(SnapshotBuffer& snaps,
                             std::string dir = "blackbox_clips")
        : snaps_(snaps), dir_(std::move(dir)) {}

    // 블랙박스 trigger 직후 호출. 사건 "이후" 장면도 담기 위해 잠깐 기다렸다가
    // (kCaptureDelaySec) 최근 구간에서 고르게 kFrames장을 뽑아 저장한다.
    // 대기·파일 쓰기는 detached 스레드에서 — 호출한 콜백(AI 워커 등)을 막지 않는다.
    void captureAsync(int channel, int64_t evt_ms, const std::string& type);

    // clip_url("ch0_..._FALL.mp4")에 짝지어 저장된 프레임을 촬영 순서대로 읽는다.
    // 없으면(저장 전 / 예전 사건 / 실패) 빈 벡터.
    std::vector<Jpeg> load(const std::string& clip_url) const;

private:
    // "ch0_..._FALL.mp4" → "<dir>/ch0_..._FALL_f<i>.jpg"
    std::string framePath(const std::string& clip_base, int i) const;

    SnapshotBuffer& snaps_;
    const std::string dir_;
};
