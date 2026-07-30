#pragma once

#include <string>
#include <vector>

// [케어 봇] 비전-언어 모델(VLM) 추상 인터페이스.
//
// 구현체를 갈아 끼우면 백엔드(Gemini / Claude / …)를 바꿀 수 있다. 봇 로직은
// 이 인터페이스에만 의존하므로 gemini_client.* 외의 파일은 손댈 일이 없다.
class VlmClient {
public:
    using Jpeg = std::vector<unsigned char>;

    virtual ~VlmClient() = default;

    // 이미지 여러 장(촬영 순서)과 질문을 주면 한국어 상황 설명을 반환한다.
    // 실패(키 미설정·네트워크·타임아웃 등) 시 빈 문자열을 반환 — 호출자가 폴백.
    virtual std::string describe(const std::vector<Jpeg>& frames,
                                 const std::string& question) = 0;

    // 사용 가능(키 설정됨) 여부. false면 호출자가 아예 VLM을 안 부르고 안내만.
    virtual bool available() const = 0;
};
