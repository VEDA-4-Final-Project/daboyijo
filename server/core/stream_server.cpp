#include "stream_server.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>

#include "protocol/video_stream.h"

namespace {
constexpr size_t kMaxOutbox = 8;  // 클라이언트당 대기 프레임 상한
}

StreamServer::StreamServer(int port) : port_(port) {}

StreamServer::~StreamServer() {
    stop();
}

bool StreamServer::start() {
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        std::perror("[stream] socket");
        return false;
    }
    int on = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(static_cast<uint16_t>(port_));
    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::perror("[stream] bind");
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }
    if (::listen(listen_fd_, 4) < 0) {
        std::perror("[stream] listen");
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }

    running_.store(true);
    accept_thread_ = std::thread(&StreamServer::acceptLoop, this);
    std::fprintf(stderr, "[stream] %d 포트에서 클라이언트 대기\n", port_);
    return true;
}

void StreamServer::stop() {
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

    std::lock_guard<std::mutex> lock(clients_mutex_);
    for (auto& client : clients_) {
        client->alive.store(false);
        client->cv.notify_all();
        if (client->sender.joinable()) {
            client->sender.join();
        }
        ::close(client->fd);
    }
    clients_.clear();
}

void StreamServer::acceptLoop() {
    while (running_.load()) {
        sockaddr_in peer{};
        socklen_t peer_len = sizeof(peer);
        int fd = ::accept(listen_fd_,
                          reinterpret_cast<sockaddr*>(&peer), &peer_len);
        if (fd < 0) {
            if (running_.load()) {
                continue;  // 일시적 오류
            }
            break;  // stop()에 의한 종료
        }

        int on = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));

        char ip[INET_ADDRSTRLEN] = {};
        ::inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
        std::fprintf(stderr, "[stream] 클라이언트 접속: %s\n", ip);

        auto client = std::make_shared<Client>();
        client->fd = fd;
        client->sender =
            std::thread(&StreamServer::senderLoop, this, std::ref(*client));

        std::lock_guard<std::mutex> lock(clients_mutex_);
        clients_.push_back(std::move(client));
    }
}

void StreamServer::senderLoop(Client& client) {
    while (running_.load() && client.alive.load()) {
        Packet packet;
        {
            std::unique_lock<std::mutex> lock(client.mutex);
            client.cv.wait_for(lock, std::chrono::milliseconds(200), [&] {
                return !client.outbox.empty() || !client.alive.load();
            });
            if (client.outbox.empty()) {
                continue;
            }
            packet = client.outbox.front();
            client.outbox.pop_front();
        }

        const auto& buf = *packet;
        size_t sent = 0;
        while (sent < buf.size()) {
            ssize_t n = ::send(client.fd, buf.data() + sent, buf.size() - sent,
                               MSG_NOSIGNAL);
            if (n <= 0) {
                client.alive.store(false);  // 연결 끊김 — broadcast()가 정리
                break;
            }
            sent += static_cast<size_t>(n);
        }
    }
}

void StreamServer::broadcast(int channel, std::vector<unsigned char> jpeg) {
    dbj_vs_header_t header{};
    header.magic = DBJ_VS_MAGIC;
    header.version = DBJ_VS_VERSION;
    header.channel = static_cast<uint8_t>(channel);
    header.timestamp_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    header.payload_len = static_cast<uint32_t>(jpeg.size());

    auto packet = std::make_shared<std::vector<unsigned char>>(
        sizeof(header) + jpeg.size());
    std::memcpy(packet->data(), &header, sizeof(header));
    std::memcpy(packet->data() + sizeof(header), jpeg.data(), jpeg.size());

    std::lock_guard<std::mutex> lock(clients_mutex_);
    for (auto it = clients_.begin(); it != clients_.end();) {
        Client& client = **it;
        if (!client.alive.load()) {
            if (client.sender.joinable()) {
                client.sender.join();
            }
            ::close(client.fd);
            std::fprintf(stderr, "[stream] 클라이언트 연결 종료\n");
            it = clients_.erase(it);
            continue;
        }
        {
            std::lock_guard<std::mutex> client_lock(client.mutex);
            if (client.outbox.size() >= kMaxOutbox) {
                client.outbox.pop_front();  // 느린 클라이언트: 오래된 프레임 드롭
            }
            client.outbox.push_back(packet);
        }
        client.cv.notify_one();
        ++it;
    }
}

size_t StreamServer::clientCount() {
    std::lock_guard<std::mutex> lock(clients_mutex_);
    size_t count = 0;
    for (const auto& client : clients_) {
        if (client->alive.load()) {
            ++count;
        }
    }
    return count;
}
