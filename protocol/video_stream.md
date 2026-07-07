# 영상 스트림 프로토콜 명세 (서버 → Qt 관제 클라이언트)

- 버전: v1 (2026-07-07)
- 작성: 박민용 (중앙 서버 파트)
- 구조체 정의: [`video_stream.h`](video_stream.h) — Qt 프로젝트에서 그대로 include 가능

## 1. 개요

중앙 서버(RPi 4)가 CCTV 4채널 영상을 수신·처리한 뒤, JPEG 프레임 단위로
접속한 관제 클라이언트에게 송출한다.

```
서버(RPi 4)                         Qt 클라이언트
 5500/TCP 대기  ◀──── 접속 ────────  QTcpSocket::connectToHost()
               ─── 프레임 패킷 ───▶  (접속 즉시 4채널 스트림 시작, 요청 메시지 불필요)
               ─── 프레임 패킷 ───▶
               ...
```

- **전송 방향**: 서버 → 클라이언트 단방향 (클라이언트는 아무것도 보낼 필요 없음)
- **v1은 평문 TCP.** 검증 완료 후 동일 패킷 그대로 TLS(OpenSSL)로 감쌀 예정
  (Qt 쪽은 `QTcpSocket` → `QSslSocket` 교체만 하면 됨)
- 여러 클라이언트 동시 접속 가능. 클라이언트가 느리면 그 클라이언트의
  오래된 프레임만 서버에서 버려짐 (다른 클라이언트에 영향 없음)

## 2. 접속 정보

| 항목 | 값 |
| --- | --- |
| 포트 | **5500**/TCP (서버 `config/cameras.conf`의 `stream_port`로 변경 가능) |
| 서버 IP | 개발 환경에서는 RPi 주소 (팀 공유) |

## 3. 패킷 구조

스트림은 아래 패킷의 무한 반복이다. **모든 다바이트 필드는 리틀엔디언.**

```
┌────────────── 헤더 16바이트 ──────────────┬─────────────────┐
│ magic │ version │ channel │ timestamp_ms │ payload_len │ JPEG 데이터        │
│  2B   │   1B    │   1B    │      8B      │     4B      │ payload_len 바이트 │
└──────────────────────────────────────────┴─────────────────┘
```

| 필드 | 타입 | 값/설명 |
| --- | --- | --- |
| `magic` | uint16 | `0xDB4B` 고정. 다르면 스트림 어긋난 것 → 연결 끊고 재접속 |
| `version` | uint8 | `0x01`. 다르면 프로토콜 버전 불일치 경고 |
| `channel` | uint8 | CCTV 채널 0~3 → 4분할 화면 위치 매핑 |
| `timestamp_ms` | uint64 | 서버가 인코딩한 시각 (Unix epoch 밀리초). 지연 측정용 |
| `payload_len` | uint32 | 이어지는 JPEG 바이트 수 (현재 640×360 품질80, 15~40KB 수준) |
| (payload) | bytes | JPEG 이미지 1장 |

- 프레임 규격(현재): 채널당 640×360, 최대 15fps, 4채널 합산 대역폭 약 11Mbps
- 낙상 팝업용 고해상도 전환은 v2에서 협의 (채널별 품질 요청 메시지 추가 예정)

## 4. Qt 수신 구현 가이드

핵심은 **TCP는 스트림이라 패킷 경계가 없다**는 것 — `readyRead()` 한 번에
패킷 하나가 온다고 가정하면 안 되고, 버퍼에 쌓아놓고 완성된 패킷만 꺼내야 한다.

```cpp
// 멤버: QTcpSocket socket; QByteArray buffer;
#include "protocol/video_stream.h"

connect(&socket, &QTcpSocket::readyRead, this, [this] {
    buffer.append(socket.readAll());

    while (true) {
        if (buffer.size() < (int)sizeof(dbj_vs_header_t))
            return;  // 헤더가 아직 다 안 옴

        dbj_vs_header_t header;
        memcpy(&header, buffer.constData(), sizeof(header));

        if (header.magic != DBJ_VS_MAGIC) {   // 스트림 어긋남 → 재접속
            socket.disconnectFromHost();
            return;
        }
        int total = sizeof(header) + header.payload_len;
        if (buffer.size() < total)
            return;  // JPEG이 아직 다 안 옴

        QImage image = QImage::fromData(
            (const uchar*)buffer.constData() + sizeof(header),
            header.payload_len, "JPEG");
        buffer.remove(0, total);

        if (!image.isNull())
            emit frameReceived(header.channel, image);  // 4분할 위젯으로
    }
});
```

- RPi·x86 모두 리틀엔디언이라 `memcpy` 파싱으로 충분 (별도 바이트 변환 불필요)
- 연결이 끊기면 `QTimer`로 2~3초 후 재접속 권장 (서버가 재시작돼도 자동 복구)
- `timestamp_ms`와 현재 시각 차이를 표시하면 지연 모니터링 가능 (시연 어필 포인트)

## 5. 빠른 검증 (Qt 코드 없이)

서버가 켜져 있을 때 아무 PC에서:

```bash
nc <서버IP> 5500 | xxd | head -5    # 첫 두 바이트가 4b db 로 보이면 정상 송출 중
```

## 변경 이력

| 버전 | 날짜 | 내용 |
| --- | --- | --- |
| v1 | 2026-07-07 | 최초 작성 — 평문 TCP, 단방향 JPEG 스트림 |
