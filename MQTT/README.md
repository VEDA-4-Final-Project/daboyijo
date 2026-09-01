# MQTT Communication Module

이 폴더는 분산 환경에서 시스템 간 실시간 데이터 송수신 및 알림 처리를 담당하는 MQTT 통신 패키지입니다. TLS/SSL 보안 인증 기반의 통신을 지원하며, 스레드 안전 큐를 통한 비동기 메시지 처리 구조를 제공합니다.

---

## 1. 디렉토리 구조

```text
MQTT/
├── MQTT_prod/              # 배포 및 타 모듈 연동용 핵심 헤더/소스
│   ├── MqttClient_veda.cpp # MQTT 클라이언트 구현체
│   ├── MqttClient_veda.hpp # MQTT 클라이언트 헤더
│   ├── ThreadSafeQueue.hpp # 스레드 안전 메시지 큐
│   └── veda_messages.hpp   # 시스템 공통 MQTT 메시지 프로토콜 정의 Qt mqtt에서는 이 파일만 사용 
├── scripts/                # 인증서 생성 유틸리티
│   ├── generate_certs.sh   # 기본 TLS/SSL 인증서 생성 스크립트
│   └── generate_stream_certs.sh # 스트리밍용 TLS 인증서 생성 스크립트
└── README.md
```

## 2. 주요 모듈 설명

### MQTT_prod
각 시스템 노드에서 공통으로 참조하는 코어 라이브러리 모듈입니다.

- **MqttClient_veda (.cpp / .hpp)**: C/C++ 기반의 클라이언트 래퍼 클래스로, 브로커 연결, 토픽 구독(Subscribe), 메시지 발행(Publish), 재연결 및 TLS 핸드셰이크를 캡슐화하여 처리합니다.
- **ThreadSafeQueue.hpp**: 멀티스레드 환경에서 안전하게 메시지를 적재 및 소비할 수 있는 템플릿 기반 동기화 큐입니다.
- **veda_messages.hpp**: 노드 간 규격화된 통신을 위한 JSON/Payload 구조체 및 토픽 정의 헤더입니다.

### scripts
- **generate_certs.sh / generate_stream_certs.sh**: TLS 기반 암호화 통신을 위한 CA, 서버 및 클라이언트 인증서(CRT/KEY)를 자동 발급하는 쉘 스크립트입니다.

---

## 3. 사전 요구 사항 (Dependencies)
- **C++ Standard**: C++17 이상
- **Build System**: CMake 3.16+
- **Libraries**:
  - `OpenSSL` (libssl-dev)
  - `nlohmann-json` (JSON 파싱용)

```bash
# Ubuntu/Debian 의존성 패키지 설치 예시
sudo apt-get update
sudo apt-get install -y build-essential cmake libssl-dev libmosquitto-dev libmosquittopp-dev nlohmann-json3-dev
```

---

## 4. 인증서 생성 및 설정 (TLS/SSL)
보안 통신을 위해 `scripts` 디렉토리의 스크립트를 사용하여 인증서를 생성합니다.

```bash
cd scripts
chmod +x generate_certs.sh
./generate_certs.sh
```
생성된 `ca.crt` 파일을 클라이언트 설정 경로에 배치하여 초기화 시 로드합니다.

---

## 5. 빌드 및 모듈 연동 가이드
타 노드(Qt GUI, 센서 노드, 알람 노드 등)에서 본 모듈을 참조하여 빌드할 때의 표준 CMake 구성 방식입니다.

### CMakeLists.txt 연동 예시
```cmake
cmake_minimum_required(VERSION 3.16)
project(YourApp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)

# MQTT_prod 디렉토리 인클루드
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../MQTT_prod)

# 소스 파일 추가
add_executable(YourApp
    main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../MQTT_prod/MqttClient_veda.cpp
)

# 라이브러리 링크
find_package(OpenSSL REQUIRED)
target_link_libraries(YourApp PRIVATE
    mosquittopp
    mosquitto
    OpenSSL::SSL
    OpenSSL::Crypto
    pthread
)
```
