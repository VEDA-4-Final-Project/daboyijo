#include "gemini_client.hpp"

#include <cstdio>
#include <string>
#include <utility>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

namespace {

// 표준 base64 인코딩 (Gemini inline_data용). OpenSSL 의존을 피하려 자체 구현.
std::string base64Encode(const std::vector<unsigned char>& in) {
    static const char* tbl =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((in.size() + 2) / 3) * 4);
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        unsigned n = (in[i] << 16) | (in[i + 1] << 8) | in[i + 2];
        out.push_back(tbl[(n >> 18) & 0x3F]);
        out.push_back(tbl[(n >> 12) & 0x3F]);
        out.push_back(tbl[(n >> 6) & 0x3F]);
        out.push_back(tbl[n & 0x3F]);
    }
    if (i < in.size()) {
        unsigned n = in[i] << 16;
        bool two = (i + 1 < in.size());
        if (two) n |= in[i + 1] << 8;
        out.push_back(tbl[(n >> 18) & 0x3F]);
        out.push_back(tbl[(n >> 12) & 0x3F]);
        out.push_back(two ? tbl[(n >> 6) & 0x3F] : '=');
        out.push_back('=');
    }
    return out;
}

size_t collectWrite(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* s = static_cast<std::string*>(userdata);
    s->append(ptr, size * nmemb);
    return size * nmemb;
}

}  // namespace

GeminiClient::GeminiClient(std::string api_key, std::string model)
    : api_key_(std::move(api_key)), model_(std::move(model)) {}

std::string GeminiClient::postGenerateContent(const std::string& payload) {
    CURL* curl = curl_easy_init();
    if (!curl) return "";

    const std::string url =
        "https://generativelanguage.googleapis.com/v1beta/models/" + model_ +
        ":generateContent?key=" + api_key_;

    std::string resp;
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(payload.size()));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, collectWrite);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        // LOGOFF std::fprintf(stderr, "[Gemini] 전송 실패: %s\n", curl_easy_strerror(res));
        return "";
    }
    if (http_code != 200) {
        // LOGOFF std::fprintf(stderr, "[Gemini] HTTP %ld: %s\n", http_code, resp.c_str());
        return "";
    }

    // ── 응답에서 candidates[0].content.parts[*].text 추출 ──
    try {
        auto j = nlohmann::json::parse(resp);
        const auto& cand = j.at("candidates");
        if (!cand.is_array() || cand.empty()) return "";
        std::string text;
        for (const auto& p : cand[0].at("content").at("parts")) {
            if (p.contains("text")) text += p.at("text").get<std::string>();
        }
        return text;
    } catch (const std::exception& e) {
        // LOGOFF std::fprintf(stderr, "[Gemini] 응답 파싱 실패: %s\n", e.what());
        return "";
    }
}

std::string GeminiClient::describe(const std::vector<Jpeg>& frames,
                                   const std::string& question) {
    if (api_key_.empty() || frames.empty()) return "";

    // ── 프롬프트 + 이미지 파트로 요청 본문 구성 ──
    const std::string prompt =
        "다음은 요양원 병실 CCTV에서 몇 초 간격으로 촬영 순서대로 캡처한 "
        "사진들입니다. 사생활 보호를 위해 얼굴은 블러 처리되어 있을 수 있습니다. "
        "사진 속 어르신이 지금 무엇을 하고 있는지 파악해, 보호자의 질문에 "
        "한국어로 두세 문장 이내로 간결하고 다정하게 답해 주세요. 확실하지 "
        "않으면 단정하지 말고 보이는 대로만 설명하세요.\n질문: " +
        question;

    // 중괄호 초기화 모호성을 피하려 파트를 명시적으로 구성한다.
    nlohmann::json parts = nlohmann::json::array();
    {
        nlohmann::json text_part;
        text_part["text"] = prompt;
        parts.push_back(std::move(text_part));
    }
    for (const auto& f : frames) {
        nlohmann::json inline_data;
        inline_data["mime_type"] = "image/jpeg";
        inline_data["data"] = base64Encode(f);
        nlohmann::json img_part;
        img_part["inline_data"] = std::move(inline_data);
        parts.push_back(std::move(img_part));
    }
    nlohmann::json content;
    content["parts"] = std::move(parts);
    nlohmann::json body;
    body["contents"] = nlohmann::json::array();
    body["contents"].push_back(std::move(content));

    return postGenerateContent(body.dump());
}

std::string GeminiClient::ask(const std::string& prompt) {
    if (api_key_.empty() || prompt.empty()) return "";

    nlohmann::json text_part;
    text_part["text"] = prompt;
    nlohmann::json parts = nlohmann::json::array();
    parts.push_back(std::move(text_part));
    nlohmann::json content;
    content["parts"] = std::move(parts);
    nlohmann::json body;
    body["contents"] = nlohmann::json::array();
    body["contents"].push_back(std::move(content));

    return postGenerateContent(body.dump());
}
