#pragma once

#include <string>
#include <vector>

#include "vlm_client.hpp"

// [케어 봇] Google Gemini API 기반 VLM 구현.
//
// generativelanguage.googleapis.com의 :generateContent 엔드포인트에 키프레임
// 여러 장(base64 inline_data) + 질문 텍스트를 POST하고, 응답 JSON에서
// candidates[0].content.parts[0].text 를 뽑아 돌려준다. libcurl(HTTPS) 사용.
class GeminiClient : public VlmClient {
public:
    GeminiClient(std::string api_key, std::string model);

    std::string describe(const std::vector<Jpeg>& frames,
                         const std::string& question) override;
    bool available() const override { return !api_key_.empty(); }
    std::string ask(const std::string& prompt) override;

private:
    // describe()/ask() 공용: 이미 조립된 요청 본문을 POST하고 candidates[0]의
    // 텍스트를 뽑아 반환(네트워크/파싱 실패 시 빈 문자열).
    std::string postGenerateContent(const std::string& payloadJson);

    std::string api_key_;
    std::string model_;
};
