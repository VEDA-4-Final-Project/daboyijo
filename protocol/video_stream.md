# 영상 스트림 프로토콜 명세 (서버 ↔ Qt 관제 클라이언트)

- 버전: v2
- 작성: 박민용 (중앙 서버 파트)
- 구조체 정의: [`video_stream.h`](video_stream.h) — Qt 프로젝트에서 그대로 include 가능
- **메시지 4종 전체 목록과 제어 메시지 규격은 [`README.md`](README.md) 참조.**
  이 문서는 그중 **영상 프레임(0xDB4B)의 수신 구현 가이드**에 집중한다.

## 1. 개요

중앙 서버(RPi 4)가 CCTV 4채널 영상을 수신·처리한 뒤, JPEG 프레임 단위로
접속한 관제 클라이언트에게 송출한다.

```
서버(RPi 4)                         Qt 클라이언트
 5500/TCP 대기  ◀──── 접속 ────────  QSslSocket::connectToHostEncrypted()
               ─── 프레임 패킷 ───▶  (접속 즉시 4채널 스트림 시작, 요청 메시지 불필요)
               ─── 이벤트/검색결과 ─▶
               ◀── 제어 메시지 ────  (ROI·카메라 연결·이미지·초점·검색질의)
```

- **전송 방향**: **양방향.** 영상·이벤트·검색결과가 서버→클라로, 제어 메시지 10종이
  클라→서버로 **같은 연결** 위를 흐른다. 매직 넘버로 구분한다
  (`0xDB4B` 영상 / `0xDB4D` 이벤트 / `0xDB4E` 검색결과 / `0xDB4C` 제어)
- **TLS 적용됨.** 서버 `config/cameras.conf`에 `stream_cert_path`/`stream_key_path`를
  둘 다 주면 TLS, 비우면 평문(개발용). 한쪽만 채우면 서버가 시작을 거부한다.
  프레임 포맷은 TLS 유무와 무관하다 — 전송 계층만 다르다
- 여러 클라이언트 동시 접속 가능. 클라이언트가 느리면 그 클라이언트의
  오래된 프레임만 서버에서 버려짐 (다른 클라이언트에 영향 없음)
- **2-Pi 분할**: Qt는 Pi A(ch0·1)와 Pi B(ch2·3)에 각각 소켓을 열어 동시 접속한다

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
| `payload_len` | uint32 | 이어지는 JPEG 바이트 수 |
| (payload) | bytes | JPEG 이미지 1장 |

- 프레임 규격(현재): 채널당 **1280×720, JPEG 품질 80**
  (서버 `video/frame_queue.hpp`의 `kViewWidth`/`kViewHeight` — 관제 그리드 타일 크기 기준)
- 레이트 상한 40fps. 벽시계가 아니라 **촬영시각(PTS)** 기준으로 판정하므로,
  디코딩이 버스티하게 뭉쳐 나와도 촬영 간격이 정상이면 다 통과한다
- **아무도 안 보면 인코딩하지 않는다** — 접속한 클라이언트가 0개면 JPEG 인코딩 자체를
  건너뛴다(Pi에서 가장 비싼 구간). 스냅샷 버퍼만 2fps로 따로 갱신된다

## 4. Qt 수신 구현 가이드

핵심은 **TCP는 스트림이라 패킷 경계가 없다**는 것 — `readyRead()` 한 번에
패킷 하나가 온다고 가정하면 안 되고, 버퍼에 쌓아놓고 완성된 패킷만 꺼내야 한다.

⚠️ **이제 한 스트림에 4종이 섞여 온다.** 아래는 영상 프레임만 다루는 최소 예시이고,
실제 구현은 `magic`을 먼저 보고 이벤트(`0xDB4D`)·검색결과(`0xDB4E`)로 분기해야 한다
(`client/mainwindow.cpp`의 `onReadyRead()` 참조).

```cpp
// 멤버: QSslSocket socket; QByteArray buffer;
#include "protocol/video_stream.h"

connect(&socket, &QSslSocket::readyRead, this, [this] {
    buffer.append(socket.readAll());

    while (true) {
        if (buffer.size() < (int)sizeof(dbj_vs_header_t))
            return;  // 헤더가 아직 다 안 옴

        dbj_vs_header_t header;
        memcpy(&header, buffer.constData(), sizeof(header));

        // 실제 구현은 여기서 DBJ_EVT_MAGIC / DBJ_SEARCH_MAGIC 도 분기할 것
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

서버가 **평문 모드**(인증서 미설정)로 떠 있을 때 아무 PC에서:

```bash
nc <서버IP> 5500 | xxd | head -5    # 첫 두 바이트가 4b db 로 보이면 정상 송출 중
```

TLS 모드로 떠 있으면 `nc`로는 안 보인다. 이때는 `openssl s_client`를 쓴다.

```bash
openssl s_client -connect <서버IP>:5500 -CAfile certs/ca.crt -quiet | xxd | head -5
```

## 변경 이력

| 버전 | 날짜 | 내용 |
| --- | --- | --- |
| v1 | 2026-07-07 | 최초 작성 — 평문 TCP, 단방향 JPEG 스트림 |
| v2 | — | **TLS(OpenSSL) 적용**, 역방향 제어 메시지 10종, 이벤트(`0xDB4D`)·영상검색 결과(`0xDB4E`) 추가, 다중 침대 ROI(`roi_id`), 송출 규격 1280×720, 2-Pi 분할 |
