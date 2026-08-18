#pragma once

#include <string>

#include "vlm_client.hpp"

class Database;

// [영상검색] 자연어 질의("어제 저녁에 낙상 있었어?")를 VLM으로 (사건유형,
// 시간범위) 구조화한 뒤 events 테이블에서 찾아 클립 링크가 포함된 한국어
// 답변을 만든다. 케어봇의 "🔍 영상 검색" 버튼에서 쓰인다(care_qa.cpp).
//
// NVR 연속녹화 세그먼트가 아니라 events.clip_url(블랙박스 클립, insertEvent
// 시점에 이미 결정된 파일명)을 그대로 쓴다 — 이벤트 자체를 찾는 질의라
// 별도로 NVR 세그먼트와 시간을 맞춰볼 필요가 없다.
class VideoSearchModule {
public:
    // public_host: 클립 링크에 쓸 이 서버의 접속 주소(빈 값이면 링크 대신
    // 안내 문구). 서버는 자기 외부/LAN IP를 스스로 알 방법이 없어(config의
    // db_host와 같은 이유) 설정으로 받는다.
    VideoSearchModule(VlmClient& vlm, Database& db, std::string public_host)
        : vlm_(vlm), db_(db), public_host_(std::move(public_host)) {}

    // channel: 요청자(보호자)의 방 — 결과는 이 채널로만 제한한다(다른 방
    // 사생활을 보여주지 않기 위해, channel<0으로 호출하지 말 것).
    // 반환은 항상 사람이 읽을 완성된 한국어 문장(질의 이해 실패·결과 없음도
    // 문장으로) — 호출부는 그대로 전송하면 된다.
    std::string search(int channel, const std::string& query);

private:
    VlmClient& vlm_;
    Database& db_;
    std::string public_host_;
};
