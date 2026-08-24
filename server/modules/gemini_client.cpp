#include "gemini_client.hpp"

#include <cstdio>
#include <string>
#include <utility>

#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

namespace {

// 연결(DNS+TCP+TLS)까지의 예산. 총 예산과 나눠 잡아야 연결이 안 되는 상황에서
// 총 예산을 다 쓰고 늦게 실패하는 일이 없다.
constexpr long kConnectTimeoutSec = 10;
// 텍스트 전용 질의(검색어 파싱 등) — 짧게 끝나므로 예산도 짧게.
constexpr long kTextTimeoutSec = 20;
// 이미지 동봉 질의("지금 상황 보기") — 업로드 + 비전 추론이라 훨씬 오래 걸린다.
// 예전엔 20초 하나로 둘 다 처리하다 이 경로에서 늘 타임아웃이 났다.
constexpr long kVisionTimeoutSec = 60;

// 업로드용 축소 규격. Gemini 비전은 어차피 내부에서 타일 단위로 다운샘플하므로
// 720p 원본을 보내도 판단이 더 정확해지지 않는다 — 업로드 시간만 늘어난다.
constexpr int kUploadMaxWidth = 640;
constexpr int kUploadJpegQuality = 70;

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

void setErr(VlmError* err, VlmError v) {
    if (err) *err = v;
}

// 응답 본문에서 판단 근거가 되는 앞부분만 잘라 로그에 남긴다(키·이미지가 섞일
// 일은 없지만 429 본문이 길어 로그를 덮는 걸 막는다).
std::string briefly(const std::string& s, size_t n = 300) {
    return s.size() <= n ? s : s.substr(0, n) + "…";
}

// 요청 본문에서 generationConfig 를 통째로 들어낸다. 모델이 thinkingConfig 를
// 모른다고 400을 줄 때 그 항목만 빼고 한 번 더 시도하기 위한 것.
// ★ maxOutputTokens 까지 같이 빼는 이유: thinking 이 켜진 채로 출력 상한만 남으면
//   추론 토큰이 상한을 다 먹고 본문이 빈 채로 끝난다(더 나쁜 실패).
bool stripGenerationConfig(const std::string& payload, std::string& out) {
    try {
        auto j = nlohmann::json::parse(payload);
        if (!j.contains("generationConfig")) return false;
        j.erase("generationConfig");
        out = j.dump();
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

// describe()/ask() 공용 생성 설정.
//  · thinkingBudget=0 : 2.5 Flash 계열은 기본으로 추론 토큰을 쓴다. 상황 설명과
//    질의 파싱은 추론이 필요한 작업이 아닌데 응답만 몇 초씩 늦어지고, 무료 티어
//    토큰 한도도 그만큼 빨리 닳는다.
//  · maxOutputTokens : 두세 문장이면 충분하다. 모델이 길게 늘어지는 걸 막는다.
nlohmann::json generationConfig(int maxOutputTokens) {
    nlohmann::json gen;
    gen["thinkingConfig"]["thinkingBudget"] = 0;
    gen["maxOutputTokens"] = maxOutputTokens;
    return gen;
}

constexpr int kDescribeMaxTokens = 400;  // 한국어 두세 문장
constexpr int kAskMaxTokens = 200;       // 검색 질의 JSON 한 줄

}  // namespace

GeminiClient::GeminiClient(std::string api_key, std::string model)
    : api_key_(std::move(api_key)), model_(std::move(model)) {}

GeminiClient::Jpeg GeminiClient::shrinkForUpload(const Jpeg& jpeg) {
    cv::Mat img = cv::imdecode(jpeg, cv::IMREAD_COLOR);
    if (img.empty() || img.cols <= kUploadMaxWidth) return jpeg;

    cv::Mat small;
    const double scale = static_cast<double>(kUploadMaxWidth) / img.cols;
    cv::resize(img, small, cv::Size(), scale, scale, cv::INTER_AREA);

    Jpeg out;
    const std::vector<int> params = {cv::IMWRITE_JPEG_QUALITY, kUploadJpegQuality};
    if (!cv::imencode(".jpg", small, out, params) || out.empty()) return jpeg;
    return out;
}

std::string GeminiClient::postGenerateContent(const std::string& payload,
                                              long timeoutSec, VlmError* err) {
    setErr(err, VlmError::Network);  // 아래에서 결과에 맞게 덮어쓴다
    CURL* curl = curl_easy_init();
    if (!curl) return "";

    const std::string url =
        "https://generativelanguage.googleapis.com/v1beta/models/" + model_ +
        ":generateContent?key=" + api_key_;

    std::string resp;
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    // ★ Expect: 100-continue 끄기. libcurl은 1KB 넘는 POST에 이 헤더를 자동으로
    //   붙이고 서버의 "100 Continue"를 기다렸다 본문을 보낸다. 중간 장비(공유기·
    //   프록시·테더링 NAT)가 이 응답을 삼키면 본문 전송이 통째로 지연된다.
    //   텍스트 질의(1KB 미만)엔 안 붙고 이미지 질의에만 붙어서, "검색은 되는데
    //   지금 상황 보기만 죽는" 지금 증상과 정확히 겹치는 후보다.
    headers = curl_slist_append(headers, "Expect:");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(payload.size()));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, collectWrite);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, kConnectTimeoutSec);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeoutSec);
    // ★ 이 함수는 항상 별도 스레드에서 불린다(care_qa가 폴링 루프를 막지 않으려고
    //   detach 스레드로 돌린다). NOSIGNAL을 안 켜면 libcurl이 이름 해석 타임아웃에
    //   SIGALRM을 쓰는 빌드에서 엉뚱한 스레드로 신호가 가 멈추거나 죽을 수 있다.
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    // 재시도는 "예산을 안 쓰고 빨리 실패한" 경우에만 한다. 타임아웃까지 재시도하면
    // 사용자가 텔레그램 앞에서 예산의 두 배를 기다리게 된다.
    CURLcode res = CURLE_OK;
    long http_code = 0;
    for (int attempt = 0; attempt < 2; ++attempt) {
        resp.clear();
        res = curl_easy_perform(curl);
        if (res == CURLE_COULDNT_CONNECT || res == CURLE_COULDNT_RESOLVE_HOST ||
            res == CURLE_SSL_CONNECT_ERROR || res == CURLE_RECV_ERROR) {
            if (attempt == 0) continue;  // 한 번만 더
        }
        break;
    }
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    // ── 어느 구간에서 시간을 썼는지 분해한다 ──
    // 총 시간만으로는 "업로드가 안 나간 것"과 "모델이 오래 생각한 것"을 구분할 수
    // 없다. 셋을 같이 보면 갈린다:
    //   sent < 요청 크기            → 업로드가 막힌 것(회선·MTU·중간 장비)
    //   sent == 요청 크기, ttfb==0  → 다 보냈는데 응답이 없는 것(모델 대기·쿼터)
    //   connect 가 크다             → 연결 자체가 느린 것(DNS·TLS)
    double total_sec = 0, connect_sec = 0, ttfb_sec = 0;
    curl_off_t sent = 0;
    curl_easy_getinfo(curl, CURLINFO_TOTAL_TIME, &total_sec);
    curl_easy_getinfo(curl, CURLINFO_CONNECT_TIME, &connect_sec);
    curl_easy_getinfo(curl, CURLINFO_STARTTRANSFER_TIME, &ttfb_sec);
    curl_easy_getinfo(curl, CURLINFO_SIZE_UPLOAD_T, &sent);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        setErr(err, res == CURLE_OPERATION_TIMEDOUT ? VlmError::Timeout
                                                    : VlmError::Network);
        std::fprintf(stderr,
                     "[Gemini] 전송 실패: %s (총 %.1fs/예산 %lds | 연결 %.1fs | "
                     "업로드 %lld/%zuB | 첫응답 %.1fs)\n",
                     curl_easy_strerror(res), total_sec, timeoutSec, connect_sec,
                     static_cast<long long>(sent), payload.size(), ttfb_sec);
        return "";
    }
    if (http_code == 429) {
        // 무료 티어 한도. 분당(RPM)이면 곧 풀리고 일일(RPD)이면 오늘은 끝인데,
        // 본문의 quota 이름으로만 갈린다 — 그대로 남겨서 눈으로 확인하게 한다.
        setErr(err, VlmError::Quota);
        std::fprintf(stderr, "[Gemini] 쿼터 초과(429): %s\n", briefly(resp).c_str());
        return "";
    }
    if (http_code == 400 && payload.find("thinkingConfig") != std::string::npos) {
        // 별칭(gemini-flash-latest)이 thinkingConfig 를 모르는 모델로 옮겨간 경우.
        // 그 설정만 빼고 딱 한 번 다시 보낸다 — 벗겨낸 본문엔 thinkingConfig 가
        // 없으니 이 가지로 다시 들어와 무한 재귀가 되지는 않는다.
        std::string stripped;
        if (stripGenerationConfig(payload, stripped)) {
            std::fprintf(stderr,
                         "[Gemini] generationConfig 미지원으로 보임 — 빼고 재시도: %s\n",
                         briefly(resp, 160).c_str());
            return postGenerateContent(stripped, timeoutSec, err);
        }
    }
    if (http_code != 200) {
        setErr(err, VlmError::Rejected);
        std::fprintf(stderr, "[Gemini] HTTP %ld: %s\n", http_code,
                     briefly(resp).c_str());
        return "";
    }
    // 성공도 한 줄 남긴다 — 버튼 누를 때만 도는 저빈도 경로라 로그가 안 밀리고,
    // 평소 왕복이 몇 초인지 알아야 예산(kVisionTimeoutSec)이 적정한지 판단된다.
    std::fprintf(stderr, "[Gemini] 응답 %.1fs (연결 %.1fs · 첫응답 %.1fs · 요청 %zuB)\n",
                 total_sec, connect_sec, ttfb_sec, payload.size());

    // ── 응답에서 candidates[0].content.parts[*].text 추출 ──
    // 200인데 본문이 비는 경우가 실제로 있다: 안전 필터에 걸리거나(finishReason
    // =SAFETY) 출력 상한에 먼저 닿으면(MAX_TOKENS) content 자체가 안 온다.
    // 그때 사유를 안 남기면 "왜 빈 답이 왔는지" 추적이 불가능해진다.
    try {
        auto j = nlohmann::json::parse(resp);
        const auto& cand = j.at("candidates");
        if (!cand.is_array() || cand.empty()) {
            setErr(err, VlmError::Empty);
            std::fprintf(stderr, "[Gemini] 후보 없음: %s\n", briefly(resp).c_str());
            return "";
        }
        std::string text;
        if (cand[0].contains("content") && cand[0]["content"].contains("parts")) {
            for (const auto& p : cand[0]["content"]["parts"]) {
                if (p.contains("text")) text += p.at("text").get<std::string>();
            }
        }
        if (text.empty()) {
            setErr(err, VlmError::Empty);
            std::fprintf(stderr, "[Gemini] 빈 응답 (finishReason=%s)\n",
                         cand[0].value("finishReason", "?").c_str());
            return "";
        }
        setErr(err, VlmError::None);
        return text;
    } catch (const std::exception& e) {
        setErr(err, VlmError::Empty);
        std::fprintf(stderr, "[Gemini] 응답 파싱 실패: %s / %s\n", e.what(),
                     briefly(resp).c_str());
        return "";
    }
}

std::string GeminiClient::describe(const std::vector<Jpeg>& frames,
                                   const std::string& question, VlmError* err) {
    if (api_key_.empty()) {
        setErr(err, VlmError::NotConfigured);
        return "";
    }
    if (frames.empty()) {
        setErr(err, VlmError::Empty);
        return "";
    }

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
        inline_data["data"] = base64Encode(shrinkForUpload(f));
        nlohmann::json img_part;
        img_part["inline_data"] = std::move(inline_data);
        parts.push_back(std::move(img_part));
    }
    nlohmann::json content;
    content["parts"] = std::move(parts);
    nlohmann::json body;
    body["contents"] = nlohmann::json::array();
    body["contents"].push_back(std::move(content));
    body["generationConfig"] = generationConfig(kDescribeMaxTokens);

    return postGenerateContent(body.dump(), kVisionTimeoutSec, err);
}

std::string GeminiClient::ask(const std::string& prompt, VlmError* err) {
    if (api_key_.empty()) {
        setErr(err, VlmError::NotConfigured);
        return "";
    }
    if (prompt.empty()) {
        setErr(err, VlmError::Empty);
        return "";
    }

    nlohmann::json text_part;
    text_part["text"] = prompt;
    nlohmann::json parts = nlohmann::json::array();
    parts.push_back(std::move(text_part));
    nlohmann::json content;
    content["parts"] = std::move(parts);
    nlohmann::json body;
    body["contents"] = nlohmann::json::array();
    body["contents"].push_back(std::move(content));
    body["generationConfig"] = generationConfig(kAskMaxTokens);

    return postGenerateContent(body.dump(), kTextTimeoutSec, err);
}
