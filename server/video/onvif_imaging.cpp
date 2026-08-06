#include "onvif_imaging.hpp"

#include <curl/curl.h>
#include <openssl/evp.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <random>
#include <string>
#include <vector>

namespace {

// ── ONVIF/SOAP 네임스페이스 ──────────────────────────────────
constexpr const char* NS_S    = "http://www.w3.org/2003/05/soap-envelope";
constexpr const char* NS_TDS  = "http://www.onvif.org/ver10/device/wsdl";
constexpr const char* NS_TRT  = "http://www.onvif.org/ver10/media/wsdl";
constexpr const char* NS_TIMG = "http://www.onvif.org/ver20/imaging/wsdl";
constexpr const char* NS_TT   = "http://www.onvif.org/ver10/schema";

// ── Base64 인코딩 (OpenSSL EVP_EncodeBlock) ──────────────────
std::string b64(const unsigned char* data, int len) {
    if (len <= 0) return {};
    // EVP_EncodeBlock은 인코딩 n바이트 + 널종단까지 쓰므로 +1 여유를 둔다.
    std::string out(4 * ((len + 2) / 3) + 1, '\0');
    int n = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(&out[0]), data, len);
    if (n < 0) return {};
    out.resize(static_cast<size_t>(n));
    return out;
}

// ── SHA1 (OpenSSL EVP) ───────────────────────────────────────
std::vector<unsigned char> sha1(const std::string& in) {
    std::vector<unsigned char> md(EVP_MAX_MD_SIZE);
    unsigned int mdlen = 0;
    if (EVP_Digest(in.data(), in.size(), md.data(), &mdlen, EVP_sha1(), nullptr) != 1)
        return {};
    md.resize(mdlen);
    return md;
}

// ── libcurl HTTP POST (SOAP) ─────────────────────────────────
size_t writeCb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

// url로 body를 POST. 반환값=HTTP 상태코드(전송 실패 시 0), resp에 응답 본문.
long httpPost(const std::string& url, const std::string& body, std::string* resp) {
    resp->clear();
    CURL* curl = curl_easy_init();
    if (!curl) return 0;

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/soap+xml; charset=utf-8");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, resp);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 8L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    // ONVIF Imaging은 평문 HTTP(80). TLS 카메라라면 여기서 검증 옵션을 다뤄야 함.

    CURLcode rc = curl_easy_perform(curl);
    long code = 0;
    if (rc == CURLE_OK)
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return code;
}

// ── WS-Security UsernameToken(digest) 헤더 ───────────────────
std::string securityHeader(const std::string& user, const std::string& pw,
                           const std::string& created) {
    unsigned char nonce[16];
    std::random_device rd;
    for (unsigned char& c : nonce) c = static_cast<unsigned char>(rd());

    std::string material;
    material.append(reinterpret_cast<char*>(nonce), sizeof(nonce));
    material += created;
    material += pw;
    const std::vector<unsigned char> digest = sha1(material);

    const std::string d64 = b64(digest.data(), static_cast<int>(digest.size()));
    const std::string n64 = b64(nonce, static_cast<int>(sizeof(nonce)));

    const std::string wsse =
        "http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-secext-1.0.xsd";
    const std::string wsu =
        "http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-utility-1.0.xsd";
    const std::string pt =
        "http://docs.oasis-open.org/wss/2004/01/"
        "oasis-200401-wss-username-token-profile-1.0#PasswordDigest";
    const std::string enc =
        "http://docs.oasis-open.org/wss/2004/01/"
        "oasis-200401-wss-soap-message-security-1.0#Base64Binary";

    return "<s:Header><Security s:mustUnderstand=\"1\" xmlns=\"" + wsse + "\">"
           "<UsernameToken><Username>" + user + "</Username>"
           "<Password Type=\"" + pt + "\">" + d64 + "</Password>"
           "<Nonce EncodingType=\"" + enc + "\">" + n64 + "</Nonce>"
           "<Created xmlns=\"" + wsu + "\">" + created + "</Created>"
           "</UsernameToken></Security></s:Header>";
}

// SOAP 봉투. user 비어있지 않으면 보안 헤더 포함.
std::string envelope(const std::string& bodyInner, const std::string& user,
                     const std::string& pw, const std::string& created) {
    std::string header;
    if (!user.empty()) header = securityHeader(user, pw, created);
    return std::string("<?xml version=\"1.0\" encoding=\"UTF-8\"?>") +
           "<s:Envelope xmlns:s=\"" + NS_S + "\" xmlns:tds=\"" + NS_TDS +
           "\" xmlns:trt=\"" + NS_TRT + "\" xmlns:timg=\"" + NS_TIMG +
           "\" xmlns:tt=\"" + NS_TT + "\">" + header + "<s:Body>" + bodyInner +
           "</s:Body></s:Envelope>";
}

// ── XML 미니 파서 (네임스페이스 접두사 무시하고 로컬명으로 검색) ──
// <...:tag> 또는 <tag> 의 여는 태그를 pos부터 찾아 그 위치를 반환(없으면 npos).
size_t findOpenTag(const std::string& xml, const std::string& localName, size_t pos = 0) {
    while (pos < xml.size()) {
        size_t lt = xml.find('<', pos);
        if (lt == std::string::npos) return std::string::npos;
        size_t start = lt + 1;
        if (start < xml.size() && (xml[start] == '/' || xml[start] == '?' ||
                                   xml[start] == '!')) {
            pos = start;
            continue;
        }
        // 접두사(prefix:) 건너뛰기
        size_t nameStart = start;
        size_t colon = xml.find_first_of(":> /\t\r\n", start);
        if (colon != std::string::npos && colon < xml.size() && xml[colon] == ':')
            nameStart = colon + 1;
        size_t nameEnd = xml.find_first_of("> /\t\r\n", nameStart);
        if (nameEnd == std::string::npos) return std::string::npos;
        if (xml.compare(nameStart, nameEnd - nameStart, localName) == 0)
            return lt;
        pos = start;
    }
    return std::string::npos;
}

// <...:tag>텍스트</...:tag> 의 텍스트를 반환. 없으면 빈 문자열.
std::string tagText(const std::string& xml, const std::string& localName, size_t from = 0) {
    size_t open = findOpenTag(xml, localName, from);
    if (open == std::string::npos) return {};
    size_t gt = xml.find('>', open);
    if (gt == std::string::npos) return {};
    if (gt > 0 && xml[gt - 1] == '/') return {};  // 자기닫힘 <tag/>
    size_t end = xml.find('<', gt + 1);
    if (end == std::string::npos) return {};
    return xml.substr(gt + 1, end - (gt + 1));
}

// 여는 태그의 attr="값" 속성을 반환. 없으면 빈 문자열.
std::string tagAttr(const std::string& xml, const std::string& localName,
                    const std::string& attr) {
    size_t open = findOpenTag(xml, localName);
    if (open == std::string::npos) return {};
    size_t gt = xml.find('>', open);
    if (gt == std::string::npos) return {};
    std::string seg = xml.substr(open, gt - open);
    std::string key = attr + "=\"";
    size_t p = seg.find(key);
    if (p == std::string::npos) return {};
    p += key.size();
    size_t q = seg.find('"', p);
    if (q == std::string::npos) return {};
    return seg.substr(p, q - p);
}

// localName 요소 안의 Min/Max를 파싱. 성공 시 true.
bool tagRange(const std::string& xml, const std::string& localName, double* mn, double* mx) {
    size_t open = findOpenTag(xml, localName);
    if (open == std::string::npos) return {};
    // 해당 요소의 닫는 태그까지 범위로 한정하지 않고, 바로 뒤 Min/Max를 읽는다
    // (ONVIF GetOptions는 <tt:Brightness><tt:Min>..</tt:Min><tt:Max>..</tt:Max> 구조).
    std::string mnS = tagText(xml, "Min", open);
    std::string mxS = tagText(xml, "Max", open);
    if (mnS.empty() || mxS.empty()) return false;
    *mn = std::strtod(mnS.c_str(), nullptr);
    *mx = std::strtod(mxS.c_str(), nullptr);
    return true;
}

// SOAP Fault 사유 텍스트(없으면 응답 앞부분).
std::string faultText(const std::string& resp) {
    std::string t = tagText(resp, "Text");
    if (!t.empty()) return t;
    t = tagText(resp, "Reason");
    if (!t.empty()) return t;
    t = tagText(resp, "faultstring");
    if (!t.empty()) return t;
    return resp.substr(0, 200);
}

// rtsp://user:pw@host:port/path 에서 host/user/pw 파싱.
bool parseRtsp(const std::string& url, std::string* host, std::string* user,
               std::string* pw) {
    size_t s = url.find("://");
    if (s == std::string::npos) return false;
    s += 3;
    size_t at = url.find('@', s);
    size_t slash = url.find('/', s);
    if (at != std::string::npos && (slash == std::string::npos || at < slash)) {
        std::string creds = url.substr(s, at - s);
        size_t colon = creds.find(':');
        if (colon != std::string::npos) {
            *user = creds.substr(0, colon);
            *pw = creds.substr(colon + 1);
        } else {
            *user = creds;
            pw->clear();
        }
        s = at + 1;
    }
    size_t hostEnd = url.find_first_of(":/", s);
    *host = (hostEnd == std::string::npos) ? url.substr(s) : url.substr(s, hostEnd - s);
    return !host->empty();
}

// 카메라 UTC 시각을 ISO8601로. 실패 시 로컬(PC) UTC로 폴백.
std::string cameraCreated(const std::string& devUrl) {
    std::string resp;
    if (httpPost(devUrl, envelope("<tds:GetSystemDateAndTime/>", "", "", ""), &resp) == 200) {
        std::string y = tagText(resp, "Year"),  mo = tagText(resp, "Month");
        std::string d = tagText(resp, "Day"),   h = tagText(resp, "Hour");
        std::string mi = tagText(resp, "Minute"), se = tagText(resp, "Second");
        if (!y.empty() && !mo.empty() && !d.empty()) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ",
                          atoi(y.c_str()), atoi(mo.c_str()), atoi(d.c_str()),
                          atoi(h.c_str()), atoi(mi.c_str()), atoi(se.c_str()));
            return buf;
        }
    }
    std::time_t now = std::time(nullptr);
    std::tm tmv{};
#if defined(_WIN32)
    gmtime_s(&tmv, &now);
#else
    gmtime_r(&now, &tmv);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tmv);
    return buf;
}

// 0~100 정규화 값을 [mn,mx] 정수로 환산.
int mapPct(int pct, double mn, double mx) {
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    return static_cast<int>(std::lround(mn + (pct / 100.0) * (mx - mn)));
}

}  // namespace

bool applyOnvifImaging(const std::string& rtsp_url, const OnvifImageParams& p,
                       std::string* err) {
    auto fail = [&](const std::string& m) { if (err) *err = m; return false; };

    std::string host, user, pw;
    if (!parseRtsp(rtsp_url, &host, &user, &pw))
        return fail("RTSP URL 파싱 실패: " + rtsp_url);

    const std::string devUrl = "http://" + host + "/onvif/device_service";

    // ⓪ 인증용 시각
    const std::string created = cameraCreated(devUrl);

    std::string resp;

    // ① 서비스 주소 탐색 (GetCapabilities)
    long code = httpPost(devUrl,
        envelope("<tds:GetCapabilities><tds:Category>All</tds:Category>"
                 "</tds:GetCapabilities>", user, pw, created), &resp);
    if (code != 200) return fail("GetCapabilities 실패 (HTTP " + std::to_string(code) +
                                 "): " + faultText(resp));

    // Media/Imaging XAddr — 카메라가 준 host는 무시하고 우리가 접속한 host로 고정.
    auto fixHost = [&](std::string xaddr, const std::string& fallback) -> std::string {
        if (xaddr.empty()) return fallback;
        size_t s = xaddr.find("://");
        s = (s == std::string::npos) ? 0 : s + 3;
        size_t path = xaddr.find('/', s);
        std::string p2 = (path == std::string::npos) ? "/onvif/device_service"
                                                      : xaddr.substr(path);
        return "http://" + host + p2;
    };
    // GetCapabilities 응답에서 Imaging/Media 블록의 XAddr을 각각 추출.
    std::string imagingUrl = fixHost(tagText(resp, "XAddr",
                                     findOpenTag(resp, "Imaging")), devUrl);
    std::string mediaUrl = fixHost(tagText(resp, "XAddr",
                                   findOpenTag(resp, "Media")), devUrl);

    // ② VideoSourceToken
    code = httpPost(mediaUrl, envelope("<trt:GetVideoSources/>", user, pw, created), &resp);
    if (code != 200) return fail("GetVideoSources 실패 (HTTP " + std::to_string(code) +
                                 "): " + faultText(resp));
    std::string token = tagAttr(resp, "VideoSources", "token");
    if (token.empty()) return fail("VideoSourceToken 을 못 찾음");

    // ③ 범위(GetOptions)
    code = httpPost(imagingUrl,
        envelope("<timg:GetOptions><timg:VideoSourceToken>" + token +
                 "</timg:VideoSourceToken></timg:GetOptions>", user, pw, created), &resp);
    if (code != 200) return fail("GetOptions 실패 (HTTP " + std::to_string(code) +
                                 "): " + faultText(resp));

    double bMn, bMx, cMn, cMx, sMn, sMx;
    const bool hasB = tagRange(resp, "Brightness", &bMn, &bMx);
    const bool hasC = tagRange(resp, "Contrast", &cMn, &cMx);
    const bool hasS = tagRange(resp, "ColorSaturation", &sMn, &sMx);

    // ④⑤ 적용값 조립 (ImagingSettings 자식은 스키마 순서: Brightness, ColorSaturation, Contrast)
    std::string settings;
    int applied = 0;
    if (p.brightness >= 0 && hasB) {
        settings += "<tt:Brightness>" + std::to_string(mapPct(p.brightness, bMn, bMx)) +
                    "</tt:Brightness>";
        ++applied;
    }
    if (p.saturation >= 0 && hasS) {
        settings += "<tt:ColorSaturation>" + std::to_string(mapPct(p.saturation, sMn, sMx)) +
                    "</tt:ColorSaturation>";
        ++applied;
    }
    if (p.contrast >= 0 && hasC) {
        settings += "<tt:Contrast>" + std::to_string(mapPct(p.contrast, cMn, cMx)) +
                    "</tt:Contrast>";
        ++applied;
    }
    // 노출(exposure)은 ONVIF Exposure 구조체(Mode/ExposureTime)라 v1 미적용 — 실측 후 별도.
    if (p.exposure >= 0)
        std::fprintf(stderr, "[onvif] 노출은 아직 미지원(v1) — 무시. (요청값=%d)\n", p.exposure);

    if (applied == 0)
        return fail("적용할 항목 없음 (카메라가 해당 옵션을 지원하지 않거나 값 미지정)");

    // SetImagingSettings
    code = httpPost(imagingUrl,
        envelope("<timg:SetImagingSettings><timg:VideoSourceToken>" + token +
                 "</timg:VideoSourceToken><timg:ImagingSettings>" + settings +
                 "</timg:ImagingSettings><timg:ForcePersistence>true"
                 "</timg:ForcePersistence></timg:SetImagingSettings>", user, pw, created),
        &resp);
    if (code != 200) return fail("SetImagingSettings 실패 (HTTP " + std::to_string(code) +
                                 "): " + faultText(resp));

    if (err) err->clear();
    return true;
}
