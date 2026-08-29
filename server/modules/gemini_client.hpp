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
                         const std::string& question,
                         VlmError* err = nullptr) override;
    bool available() const override { return !api_key_.empty(); }
    std::string ask(const std::string& prompt, VlmError* err = nullptr) override;

private:
    // describe()/ask() 공용: 이미 조립된 요청 본문을 POST하고 candidates[0]의
    // 텍스트를 뽑아 반환(네트워크/파싱 실패 시 빈 문자열).
    //  timeoutSec : 연결·업로드·모델 생성까지 합친 총 예산. 이미지가 붙는
    //               describe()는 ask()보다 훨씬 오래 걸려 예산을 따로 준다.
    std::string postGenerateContent(const std::string& payloadJson, long timeoutSec,
                                    VlmError* err);

    // 업로드용으로 키프레임을 줄인다(가로 640px, q70 재인코딩). 720p 원본을
    // 그대로 보내면 3장 base64가 0.5MB를 넘어 업로드만으로 예산을 다 쓴다.
    // 디코딩/인코딩에 실패하면 원본을 그대로 돌려준다(축소는 최적화일 뿐).
    static Jpeg shrinkForUpload(const Jpeg& jpeg);

    std::string api_key_;
    std::string model_;
};
