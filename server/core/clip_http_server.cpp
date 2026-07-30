#include "clip_http_server.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <filesystem>

namespace {
// 파일명만 허용(경로 구분자·상위 디렉토리 이동 차단) — 정적 파일 서버의
// 기본 방어. 블랙박스 파일명은 서버가 "ch{N}_{ms}.mp4" 형태로만 만들므로
// 정상 사용에선 걸릴 일이 없다.
bool isSafeFilename(const std::string& name) {
    if (name.empty() || name == "." || name == "..") return false;
    return name.find('/') == std::string::npos &&
           name.find('\\') == std::string::npos &&
           name.find("..") == std::string::npos;
}
}  // namespace

ClipHttpServer::ClipHttpServer(int port, std::string rootDir)
    : port_(port), rootDir_(std::move(rootDir)) {}

ClipHttpServer::~ClipHttpServer() {
    stop();
}

bool ClipHttpServer::start() {
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        std::perror("[clip-http] socket");
        return false;
    }
    int on = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(static_cast<uint16_t>(port_));
    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::perror("[clip-http] bind");
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }
    if (::listen(listen_fd_, 4) < 0) {
        std::perror("[clip-http] listen");
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }

    running_.store(true);
    accept_thread_ = std::thread(&ClipHttpServer::acceptLoop, this);
    std::fprintf(stderr, "[clip-http] %d 포트에서 블랙박스 클립 서빙 (%s)\n", port_,
                 rootDir_.c_str());
    return true;
}

void ClipHttpServer::stop() {
    if (!running_.exchange(false) && listen_fd_ < 0) {
        return;
    }
    if (listen_fd_ >= 0) {
        // accept() 블로킹 해제
        ::shutdown(listen_fd_, SHUT_RDWR);
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
    if (accept_thread_.joinable()) {
        accept_thread_.join();
    }
}

void ClipHttpServer::acceptLoop() {
    while (running_.load()) {
        sockaddr_in peer{};
        socklen_t len = sizeof(peer);
        int fd = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&peer), &len);
        if (fd < 0) {
            if (running_.load()) std::perror("[clip-http] accept");
            continue;
        }
        // 요청 하나 처리하고 끝나는 짧은 연결이라 별도 추적 없이 detach.
        std::thread(&ClipHttpServer::handleClient, this, fd).detach();
    }
}

void ClipHttpServer::handleClient(int fd) {
    // 요청 헤더 전체(빈 줄까지) 수신 — Range 헤더가 첫 recv 뒤 세그먼트로
    // 나뉘어 도착할 수 있어 \r\n\r\n이 나올 때까지 모은다.
    std::string req;
    {
        char buf[2048];
        while (req.find("\r\n\r\n") == std::string::npos && req.size() < 16 * 1024) {
            ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
            if (n <= 0) {
                ::close(fd);
                return;
            }
            req.append(buf, static_cast<size_t>(n));
        }
    }

    // 요청 라인 파싱: "GET /파일명 HTTP/1.1"
    auto sp1 = req.find(' ');
    auto sp2 = (sp1 == std::string::npos) ? std::string::npos : req.find(' ', sp1 + 1);
    std::string method = (sp1 != std::string::npos) ? req.substr(0, sp1) : "";
    std::string path = (sp1 != std::string::npos && sp2 != std::string::npos)
                            ? req.substr(sp1 + 1, sp2 - sp1 - 1)
                            : "";

    if (method == "GET" && path == "/list") {
        std::string json = "[";
        bool first = true;
        
        try {
            // rootDir_(blackbox_clips) 폴더를 돌며 .mp4 파일 목록을 수집합니다.
            for (const auto& entry : std::filesystem::directory_iterator(rootDir_)) {
                if (entry.is_regular_file() && entry.path().extension() == ".mp4") {
                    if (!first) json += ",";
                    json += "\"" + entry.path().filename().string() + "\"";
                    first = false;
                }
            }
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[clip-http] 디렉토리 스캔 실패: %s\n", e.what());
        }
        json += "]";

        // HTTP JSON 응답 조립
        std::ostringstream resp;
        resp << "HTTP/1.1 200 OK\r\n"
             << "Content-Type: application/json\r\n"
             << "Content-Length: " << json.size() << "\r\n"
             << "Connection: close\r\n\r\n"
             << json;

        std::string r = resp.str();
        ::send(fd, r.data(), r.size(), MSG_NOSIGNAL);
        ::close(fd);
        return; // 🚀 파일 다운로드 로직으로 내려가지 않고 여기서 끝냅니다.
    }
    
    // 모든 send는 MSG_NOSIGNAL — 클라이언트(FFmpeg)는 재생바 탐색 때마다
    // 연결을 끊고 다시 여는데, 끊긴 소켓에 쓰면 SIGPIPE로 프로세스가
    // 통째로 죽는다(기본 동작). stream_server.cpp와 동일한 방어.
    auto sendStatus = [fd](const char* status) {
        std::string resp = std::string("HTTP/1.1 ") + status +
                            "\r\nAccept-Ranges: bytes"
                            "\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";
        ::send(fd, resp.data(), resp.size(), MSG_NOSIGNAL);
    };

    if (method != "GET" || path.empty() || path[0] != '/') {
        sendStatus("400 Bad Request");
        ::close(fd);
        return;
    }
    std::string filename = path.substr(1);
    if (!isSafeFilename(filename)) {
        sendStatus("404 Not Found");
        ::close(fd);
        return;
    }

    std::string fullPath = rootDir_ + "/" + filename;
    struct stat st{};
    if (::stat(fullPath.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
        sendStatus("404 Not Found");
        ::close(fd);
        return;
    }
    const long long file_size = static_cast<long long>(st.st_size);

    // Range 헤더 파싱: "Range: bytes=START-" 또는 "bytes=START-END".
    // 재생바 탐색 시 FFmpeg가 이 부분 요청으로 파일 중간부터 다시 받는다 —
    // 미지원이면 스트림이 '탐색 불가'로 취급돼 seek가 ENOSYS로 실패한다.
    long long range_start = -1, range_end = -1;
    {
        std::string lower(req.size(), '\0');
        std::transform(req.begin(), req.end(), lower.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        static const char kRangeKey[] = "\r\nrange: bytes=";
        auto pos = lower.find(kRangeKey);
        if (pos != std::string::npos) {
            const char* p = req.c_str() + pos + sizeof(kRangeKey) - 1;
            char* endp = nullptr;
            long long v = std::strtoll(p, &endp, 10);
            if (endp != p && v >= 0 && *endp == '-') {
                range_start = v;
                const char* q = endp + 1;
                if (*q >= '0' && *q <= '9') range_end = std::strtoll(q, nullptr, 10);
            }
        }
    }

    long long start = 0, end = file_size - 1;
    bool partial = false;
    if (range_start >= 0) {
        if (range_start >= file_size) {
            sendStatus("416 Range Not Satisfiable");
            ::close(fd);
            return;
        }
        start = range_start;
        if (range_end >= start && range_end < file_size) end = range_end;
        partial = true;
    }

    std::ifstream file(fullPath, std::ios::binary);
    if (!file) {
        sendStatus("500 Internal Server Error");
        ::close(fd);
        return;
    }
    if (start > 0) file.seekg(start);

    long long remaining = end - start + 1;
    std::ostringstream header;
    if (partial) {
        header << "HTTP/1.1 206 Partial Content\r\n"
               << "Content-Range: bytes " << start << '-' << end << '/' << file_size
               << "\r\n";
    } else {
        header << "HTTP/1.1 200 OK\r\n";
    }
    header << "Content-Type: video/mp4\r\n"
           << "Accept-Ranges: bytes\r\n"
           << "Content-Length: " << remaining << "\r\n"
           << "Connection: close\r\n\r\n";
    std::string h = header.str();
    if (::send(fd, h.data(), h.size(), MSG_NOSIGNAL) <= 0) {
        ::close(fd);
        return;
    }

    char chunk[64 * 1024];
    bool client_gone = false;
    while (remaining > 0 && !client_gone) {
        const std::streamsize want = static_cast<std::streamsize>(
            std::min<long long>(remaining, sizeof(chunk)));
        file.read(chunk, want);
        std::streamsize got = file.gcount();
        if (got <= 0) break;
        std::streamsize sent = 0;
        while (sent < got) {
            ssize_t s = ::send(fd, chunk + sent, static_cast<size_t>(got - sent),
                               MSG_NOSIGNAL);
            if (s <= 0) { client_gone = true; break; }  // 클라이언트가 끊음
            sent += s;
        }
        remaining -= got;
    }
    ::close(fd);
}
