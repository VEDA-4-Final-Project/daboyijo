#include "stream_server.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>

#include "protocol/video_stream.h"

namespace {
constexpr size_t kMaxOutbox = 8;  // 클라이언트당 대기 프레임 상한
constexpr size_t kRecvBufCap = 64 * 1024;  // 제어 수신 버퍼 상한(동기 깨지면 리셋)

// 이벤트(낙상 통보 등) 패킷인지 — outbox가 가득 차도 드롭하면 안 되는 패킷
bool isEventPacket(const std::vector<unsigned char>& buf) {
    if (buf.size() < sizeof(uint16_t)) return false;
    uint16_t magic;
    std::memcpy(&magic, buf.data(), sizeof(magic));
    return magic == DBJ_EVT_MAGIC;
}
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
        closeClient(*client);
    }
    clients_.clear();
}

// 송신·수신 스레드를 안전하게 내리고 소켓을 닫는다.
// 순서 중요: alive=false → shutdown(recv/send 블로킹 해제) → join → close.
// (join 전에 close하면 안 됨 — receiver가 recv()에서 못 깨어나 데드락)
void StreamServer::closeClient(Client& client) {
    client.alive.store(false);
    if (client.fd >= 0) {
        ::shutdown(client.fd, SHUT_RDWR);  // recv()/send() 즉시 반환시킴
    }
    client.cv.notify_all();
    if (client.sender.joinable()) {
        client.sender.join();
    }
    if (client.receiver.joinable()) {
        client.receiver.join();
    }
    if (client.fd >= 0) {
        ::close(client.fd);
        client.fd = -1;
    }
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
        client->receiver =
            std::thread(&StreamServer::receiverLoop, this, std::ref(*client));

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

// 클라이언트(Qt)가 보내는 제어 메시지를 파싱한다.
// TCP는 경계 없이 도착하므로 버퍼에 쌓아가며 완성된 메시지만 뽑아낸다.
void StreamServer::receiverLoop(Client& client) {
    std::vector<uint8_t> buf;
    uint8_t chunk[1024];
    while (running_.load() && client.alive.load()) {
        ssize_t n = ::recv(client.fd, chunk, sizeof(chunk), 0);
        if (n <= 0) {
            client.alive.store(false);  // 끊김/오류 — broadcast()가 정리
            break;
        }
        buf.insert(buf.end(), chunk, chunk + static_cast<size_t>(n));

        size_t off = 0;
        while (buf.size() - off >= sizeof(dbj_ctrl_header_t)) {
            dbj_ctrl_header_t h;
            std::memcpy(&h, buf.data() + off, sizeof(h));
            if (h.magic != DBJ_CTRL_MAGIC) {
                ++off;  // 스트림 어긋남 — 1바이트씩 버리며 재동기
                continue;
            }

            size_t need = sizeof(dbj_ctrl_header_t);
            if (h.type == DBJ_CTRL_ROI_SET) {
                need += static_cast<size_t>(h.point_count) * sizeof(dbj_roi_point_t);
            } else if (h.type == DBJ_CTRL_CAMERA_SET) {
                need += h.reserved;  // 헤더 뒤에 URL 문자열(reserved 바이트)이 옴
            }
            if (buf.size() - off < need) {
                break;  // 데이터가 아직 덜 옴 — 다음 recv 대기
            }

            if (h.channel < 4) { // 채널 유효성 공통 체크
                switch (h.type) {
                    case DBJ_CTRL_ROI_SET:
                    case DBJ_CTRL_ROI_CLEAR: {
                        if (on_roi_) {
                            RoiUpdate up;
                            up.channel = h.channel;
                            up.clear = (h.type == DBJ_CTRL_ROI_CLEAR);
                            
                            // ROI 지정일 때만 뒤따라오는 꼭짓점 점 데이터 파싱
                            if (h.type == DBJ_CTRL_ROI_SET) {
                                const uint8_t* pts =
                                    buf.data() + off + sizeof(dbj_ctrl_header_t);
                                up.points.reserve(h.point_count);
                                for (int i = 0; i < h.point_count; ++i) {
                                    dbj_roi_point_t p;
                                    std::memcpy(&p, pts + i * sizeof(p), sizeof(p));
                                    up.points.emplace_back(
                                        static_cast<float>(p.x) / DBJ_ROI_COORD_SCALE,
                                        static_cast<float>(p.y) / DBJ_ROI_COORD_SCALE);
                                }
                            }
                            on_roi_(up);
                        }
                        break;
                    }

                    case DBJ_CTRL_ALARM_CONFIRM: {
                        if (on_confirm_) {
                            on_confirm_(h.channel);
                        }
                        break;
                    }

                    case DBJ_CTRL_RISK_UPDATE: {
                        if (on_risk_level_) {
                            int risk_level = h.point_count;
                            on_risk_level_(h.channel, risk_level);
                        }
                        break;
                    }

                    case DBJ_CTRL_CAMERA_SET: {
                        if (on_camera_set_) {
                            const char* p = reinterpret_cast<const char*>(
                                buf.data() + off + sizeof(dbj_ctrl_header_t));
                            std::string url(p, h.reserved);
                            on_camera_set_(h.channel, url);
                        }
                        break;
                    }

                    case DBJ_CTRL_CAMERA_CLEAR: {
                        if (on_camera_clear_) {
                            on_camera_clear_(h.channel);
                        }
                        break;
                    }

                    default:
                        std::cerr << "[stream] 알 수 없는 제어 명령 타입: " << (int)h.type << std::endl;
                        break;
                }
            }

            off += need;
        }
        if (off > 0) {
            buf.erase(buf.begin(), buf.begin() + static_cast<long>(off));
        }
        if (buf.size() > kRecvBufCap) {
            buf.clear();  // 동기 완전 상실 방어
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

    enqueueAll(std::move(packet));
}

void StreamServer::broadcastEvent(int channel, uint8_t type, float x, float y,
                                  int64_t timestampMsOverride) {
    dbj_evt_header_t evt{};
    evt.magic = DBJ_EVT_MAGIC;
    evt.version = DBJ_VS_VERSION;
    evt.type = type;
    evt.channel = static_cast<uint8_t>(channel);
    evt.x = static_cast<uint16_t>(x * DBJ_ROI_COORD_SCALE);
    evt.y = static_cast<uint16_t>(y * DBJ_ROI_COORD_SCALE);
    evt.timestamp_ms = timestampMsOverride != 0
        ? static_cast<uint64_t>(timestampMsOverride)
        : static_cast<uint64_t>(
              std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::system_clock::now().time_since_epoch())
                  .count());

    auto packet = std::make_shared<std::vector<unsigned char>>(sizeof(evt));
    std::memcpy(packet->data(), &evt, sizeof(evt));
    enqueueAll(std::move(packet));
}

void StreamServer::enqueueAll(Packet packet) {
    std::lock_guard<std::mutex> lock(clients_mutex_);
    for (auto it = clients_.begin(); it != clients_.end();) {
        Client& client = **it;
        if (!client.alive.load()) {
            closeClient(client);
            std::fprintf(stderr, "[stream] 클라이언트 연결 종료\n");
            it = clients_.erase(it);
            continue;
        }
        {
            std::lock_guard<std::mutex> client_lock(client.mutex);
            if (client.outbox.size() >= kMaxOutbox) {
                // 느린 클라이언트: 오래된 "영상 프레임"부터 드롭.
                // 이벤트(낙상 통보)는 절대 버리지 않는다 — 전부 이벤트면 상한 초과 허용
                // (이벤트는 18바이트라 메모리 부담 없음).
                auto victim = std::find_if(
                    client.outbox.begin(), client.outbox.end(),
                    [](const Packet& p) { return !isEventPacket(*p); });
                if (victim != client.outbox.end()) {
                    client.outbox.erase(victim);
                }
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
