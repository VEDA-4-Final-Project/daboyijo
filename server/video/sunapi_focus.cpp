#include "sunapi_focus.hpp"

#include <curl/curl.h>

#include <atomic>
#include <cmath>
#include <string>

namespace {

// 포커스 좌표계 (SUNAPI FocusAreaCoordinate 픽셀 공간).
// 실측: 전체프레임 드래그가 (0,32)~(2487,1490), 화면비 ≈5:3 → 2560x1536(4MP)로 추정.
// 포커스는 오차에 관대하므로 조금 달라도 무방. 다른 해상도면 이 두 값만 조정.
constexpr int kFocusW = 2560;
constexpr int kFocusH = 1536;
// 클릭 지점 주변 초점 영역 박스의 반크기(px). 웹뷰어가 ~170x150 박스를 씀 → 절반.
constexpr int kBoxHalfW = 85;
constexpr int kBoxHalfH = 75;

std::atomic<int> g_seq{1};  // SunapiSeqId — 요청마다 증가

size_t writeCb(char* p, size_t s, size_t n, void* u) {
    static_cast<std::string*>(u)->append(p, s * n);
    return s * n;
}

int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// rtsp://user:pw@host:port/path 에서 host/user/pw 파싱.
bool parseRtsp(const std::string& url, std::string* host, std::string* user,
               std::string* pw) {
    size_t s = url.find("://");
    if (s == std::string::npos) return false;
    s += 3;
    size_t at = url.find('@', s), slash = url.find('/', s);
    if (at != std::string::npos && (slash == std::string::npos || at < slash)) {
        std::string creds = url.substr(s, at - s);
        size_t c = creds.find(':');
        if (c != std::string::npos) {
            *user = creds.substr(0, c);
            *pw = creds.substr(c + 1);
        } else {
            *user = creds;
            pw->clear();
        }
        s = at + 1;
    }
    size_t e = url.find_first_of(":/", s);
    *host = (e == std::string::npos) ? url.substr(s) : url.substr(s, e - s);
    return !host->empty();
}

}  // namespace

bool sunapiFocus(const std::string& rtsp_url, int channel, bool area,
                 float nx, float ny, std::string* err) {
    auto fail = [&](const std::string& m) { if (err) *err = m; return false; };

    std::string host, user, pw;
    if (!parseRtsp(rtsp_url, &host, &user, &pw))
        return fail("RTSP URL 파싱 실패: " + rtsp_url);

    std::string url = "http://" + host +
        "/stw-cgi/image.cgi?msubmenu=focus&action=control&Channel=" +
        std::to_string(channel) + "&Mode=SimpleFocus";

    if (area) {
        int cx = clampi(static_cast<int>(std::lround(nx * kFocusW)), 0, kFocusW - 1);
        int cy = clampi(static_cast<int>(std::lround(ny * kFocusH)), 0, kFocusH - 1);
        int x1 = clampi(cx - kBoxHalfW, 0, kFocusW - 1);
        int y1 = clampi(cy - kBoxHalfH, 0, kFocusH - 1);
        int x2 = clampi(cx + kBoxHalfW, 0, kFocusW - 1);
        int y2 = clampi(cy + kBoxHalfH, 0, kFocusH - 1);
        url += "&FocusAreaCoordinate=" + std::to_string(x1) + "," +
               std::to_string(y1) + "," + std::to_string(x2) + "," + std::to_string(y2);
    }
    url += "&SunapiSeqId=" + std::to_string(g_seq.fetch_add(1));

    CURL* curl = curl_easy_init();
    if (!curl) return fail("curl init 실패");

    std::string resp;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_DIGEST);   // 카메라 Digest 인증
    curl_easy_setopt(curl, CURLOPT_USERPWD, (user + ":" + pw).c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 8L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);

    CURLcode rc = curl_easy_perform(curl);
    long code = 0;
    if (rc == CURLE_OK)
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK)
        return fail(std::string("전송 실패: ") + curl_easy_strerror(rc));
    if (code != 200)
        return fail("HTTP " + std::to_string(code) + ": " + resp.substr(0, 120));
    // SUNAPI 성공 응답: {"Response":"Success"}
    if (resp.find("Success") == std::string::npos)
        return fail("카메라 응답 비정상: " + resp.substr(0, 120));

    if (err) err->clear();
    return true;
}
