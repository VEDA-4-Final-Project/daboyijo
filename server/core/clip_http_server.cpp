#include "clip_http_server.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <sys/time.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <filesystem>

namespace {
// 파일명만 허용 — 경로 구분자·상위 디렉토리 이동 차단
// 블랙박스 파일명은 서버가 "ch{N}_{ms}_{종류}.mp4" 로만 만들어 정상 사용엔 무영향
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
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        std::perror("[clip-http] socket");
        return false;
    }
    int on = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(static_cast<uint16_t>(port_));
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::perror("[clip-http] bind");
        ::close(fd);
        return false;
    }
    if (::listen(fd, 4) < 0) {
        std::perror("[clip-http] listen");
        ::close(fd);
        return false;
    }

    listen_fd_.store(fd);
    running_.store(true);
    accept_thread_ = std::thread(&ClipHttpServer::acceptLoop, this);
    // LOGOFF std::fprintf(stderr, "[clip-http] %d 포트에서 블랙박스 클립 서빙 (%s)\n", port_,
    //              rootDir_.c_str());
    return true;
}

void ClipHttpServer::stop() {
    const bool was_running = running_.exchange(false);
    const int fd = listen_fd_.exchange(-1);
    if (!was_running && fd < 0) {
        return;
    }
    if (fd >= 0) {
        ::shutdown(fd, SHUT_RDWR);   // accept() 블로킹 해제
        ::close(fd);
    }
    if (accept_thread_.joinable()) {
        accept_thread_.join();
    }

    // 처리 중인 요청 스레드 대기 — detach 라 join 이 안 되고, 안 기다리면
    // 전송 중인 스레드가 파괴된 멤버를 읽음
    // 무한정은 아님 — 클라이언트 하나가 서버 종료를 막으면 곤란
    // handleClient 가 소켓 타임아웃을 걸어둬서 정상적으론 금방 빠짐
    std::unique_lock<std::mutex> lock(inflight_mutex_);
    if (!inflight_cv_.wait_for(lock, std::chrono::seconds(5),
                               [this] { return inflight_ == 0; })) {
        // LOGOFF std::fprintf(stderr,
        //              "[clip-http] 경고: 요청 스레드 %d개가 아직 처리 중 (기다리지 않고 진행)\n",
        //              inflight_);
    }
}

void ClipHttpServer::acceptLoop() {
    while (running_.load()) {
        const int lfd = listen_fd_.load();
        if (lfd < 0) break;

        sockaddr_in peer{};
        socklen_t len = sizeof(peer);
        int fd = ::accept(lfd, reinterpret_cast<sockaddr*>(&peer), &len);
        if (fd < 0) {
            if (!running_.load()) break;
            // 바로 continue 하면 fd 고갈(EMFILE) 때 perror 를 뿜는 바쁜 루프가 됨
            std::perror("[clip-http] accept");
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(inflight_mutex_);
            ++inflight_;   // 스레드를 띄우기 전에 세야 stop() 이 안 놓침
        }
        std::thread(&ClipHttpServer::handleClient, this, fd).detach();
    }
}

void ClipHttpServer::handleClient(int fd) {
    // detach 된 스레드라 어느 경로로 빠져나가든 카운트 복구 필요
    struct InflightGuard {
        ClipHttpServer* self;
        ~InflightGuard() {
            std::lock_guard<std::mutex> lock(self->inflight_mutex_);
            if (--self->inflight_ == 0) self->inflight_cv_.notify_all();
        }
    } inflight_guard{this};

    // 아무것도 안 보내고 붙어만 있는 클라이언트가 recv 에서 스레드를 영영 잡는 걸 방지
    // stop() 이 이 스레드를 기다리므로 상한이 없으면 서버 종료가 막힘
    //
    // 수신 3초 — 요청 헤더는 사내망에서 밀리초 안에 도착, stop() 대기(5초)보다 짧아야
    // 놀고 있는 연결이 알아서 빠짐
    // 송신 10초 — 큰 mp4 를 받는 중인 느린 클라이언트를 끊으면 안 됨
    timeval rcv{};
    rcv.tv_sec = 3;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcv, sizeof(rcv));
    timeval snd{};
    snd.tv_sec = 10;
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &snd, sizeof(snd));

    // 요청 헤더 전체 수신 — Range 가 뒤 세그먼트로 쪼개져 올 수 있어 \r\n\r\n 까지 모음
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
            // rootDir_ 를 돌며 .mp4 목록 수집
            for (const auto& entry : std::filesystem::directory_iterator(rootDir_)) {
                if (entry.is_regular_file() && entry.path().extension() == ".mp4") {
                    if (!first) json += ",";
                    json += "\"" + entry.path().filename().string() + "\"";
                    first = false;
                }
            }
        } catch (const std::exception& e) {
            // LOGOFF std::fprintf(stderr, "[clip-http] 디렉토리 스캔 실패: %s\n", e.what());
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
        return; // 아래 파일 다운로드 경로로 안 내려감
    }

    // 모든 send 는 MSG_NOSIGNAL — FFmpeg 는 재생바 탐색마다 연결을 끊었다 다시 여는데,
    // 끊긴 소켓에 쓰면 SIGPIPE 로 프로세스가 통째로 죽음 (stream_server.cpp 와 동일)
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

    // Range 헤더 파싱 — "bytes=START-" 또는 "bytes=START-END"
    // 미지원이면 스트림이 탐색 불가로 취급돼 seek 가 ENOSYS 로 실패
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
