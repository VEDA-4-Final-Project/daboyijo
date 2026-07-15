#include "clip_http_server.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

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
    char buf[2048] = {0};
    ssize_t n = ::recv(fd, buf, sizeof(buf) - 1, 0);
    if (n <= 0) {
        ::close(fd);
        return;
    }
    buf[n] = '\0';

    // 요청 라인만 파싱: "GET /파일명 HTTP/1.1"
    std::string req(buf);
    auto sp1 = req.find(' ');
    auto sp2 = (sp1 == std::string::npos) ? std::string::npos : req.find(' ', sp1 + 1);
    std::string method = (sp1 != std::string::npos) ? req.substr(0, sp1) : "";
    std::string path = (sp1 != std::string::npos && sp2 != std::string::npos)
                            ? req.substr(sp1 + 1, sp2 - sp1 - 1)
                            : "";

    auto sendStatus = [fd](const char* status) {
        std::string resp = std::string("HTTP/1.1 ") + status +
                            "\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";
        ::send(fd, resp.data(), resp.size(), 0);
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

    std::ifstream file(fullPath, std::ios::binary);
    if (!file) {
        sendStatus("500 Internal Server Error");
        ::close(fd);
        return;
    }

    std::ostringstream header;
    header << "HTTP/1.1 200 OK\r\n"
           << "Content-Type: video/mp4\r\n"
           << "Content-Length: " << st.st_size << "\r\n"
           << "Connection: close\r\n\r\n";
    std::string h = header.str();
    ::send(fd, h.data(), h.size(), 0);

    char chunk[64 * 1024];
    while (true) {
        file.read(chunk, sizeof(chunk));
        std::streamsize got = file.gcount();
        if (got <= 0) break;
        std::streamsize sent = 0;
        while (sent < got) {
            ssize_t s = ::send(fd, chunk + sent, static_cast<size_t>(got - sent), 0);
            if (s <= 0) { sent = got; break; }  // 클라이언트가 끊음 — 전송 중단
            sent += s;
        }
    }
    ::close(fd);
}
