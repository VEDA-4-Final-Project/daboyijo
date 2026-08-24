#!/bin/bash

# 에러 발생시 멈추는 설정 코드
set -e

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
CERT_DIR="$SCRIPT_DIR/../certs"
MOSQUITTO_CONF_DIR="/etc/mosquitto/ca_certificates"

# ── 브로커 인증서에 적을 주소 ─────────────────────────────────────────────
# 클라이언트가 접속에 쓰는 IP/이름은 반드시 여기 다 들어 있어야 한다.
# 여기 없는 주소로 붙으면 이름 불일치로 핸드셰이크가 끊긴다.
# 브로커 IP 가 바뀌거나 늘면 이 값만 고치거나 환경변수로 넘긴다:
#   BROKER_IPS="172.20.31.17,172.20.31.16" ./generate_certs.sh
BROKER_IPS="${BROKER_IPS:-172.20.32.79}"
BROKER_DNS="${BROKER_DNS:-DaboBroker}"

# 서버 인증서 유효기간은 398 일을 넘기면 안 된다.
# Apple(macOS/iOS)은 2020-09-01 이후 발급된 TLS 서버 인증서가 398 일보다 길면
# 무조건 거부한다 — 맥 클라이언트에서 "root CA is not trusted" 로 끊기는 원인이었다.
# OpenSSL(윈도우/리눅스)에는 이 제한이 없어서 예전 3650 일짜리도 통과했다.
# 경계에 걸치지 않게 397 일로 잡는다. 1 년마다 이 스크립트를 다시 돌려야 한다.
DAYS_SERVER=397
DAYS_CA=3650

# SAN(subjectAltName) 문자열 조립 — "IP:1.2.3.4,DNS:name" 형태
SAN=""
IFS=',' read -ra _ips <<< "$BROKER_IPS"
for ip in "${_ips[@]}"; do SAN+="IP:${ip// /},"; done
IFS=',' read -ra _dns <<< "$BROKER_DNS"
for d in "${_dns[@]}"; do SAN+="DNS:${d// /},"; done
SAN="${SAN%,}"

echo "=== 1. 인증서 저장 디렉토리 생성 ($CERT_DIR) ==="
mkdir -p "$CERT_DIR"
cd "$CERT_DIR"

echo "=== 2. CA 키 및 루트 인증서 ==="
# CA 를 새로 만들면 이미 배포된 모든 기기(Qt 관제앱·웨어러블·알림노드)의 ca.crt 가
# 한꺼번에 무효가 된다. 그래서 이미 있으면 절대 건드리지 않고 그대로 쓴다.
# 브로커 인증서만 다시 발급하는 것은 ca.crt 재배포 없이 안전하게 할 수 있다.
if [[ -f ca.key && -f ca.crt ]]; then
    echo "기존 CA 를 그대로 사용합니다 (ca.crt 재배포 불필요)."
elif [[ "${ALLOW_NEW_CA:-0}" != "1" ]]; then
    # 여기서 그냥 새 CA 를 만들어 버리면, 브로커 인증서만 갱신하려던 작업이
    # 배포된 모든 기기의 ca.crt 를 조용히 무효로 만든다. 실수로 밟기 쉬운 길이라 막는다.
    # 브로커에서 돌릴 때 ca.key 는 보통 /etc/mosquitto/ca_certificates/ 에 있다.
    cat >&2 <<EOF
[중단] CA 키를 찾지 못했습니다: $CERT_DIR/ca.key

여기서 새 CA 를 만들면 이미 배포된 모든 기기(Qt 관제앱·웨어러블·알림노드)의
ca.crt 가 한꺼번에 무효가 됩니다. 브로커 인증서만 갱신하려던 것이라면 기존 CA 를
먼저 가져오세요:

    sudo cp /etc/mosquitto/ca_certificates/ca.key $CERT_DIR/
    sudo cp /etc/mosquitto/ca_certificates/ca.crt $CERT_DIR/
    sudo chown \$USER $CERT_DIR/ca.key $CERT_DIR/ca.crt

정말로 CA 부터 새로 만들고 모든 기기에 ca.crt 를 다시 배포할 생각이라면:

    ALLOW_NEW_CA=1 $0
EOF
    exit 1
else
    echo "CA 를 새로 만듭니다 — 모든 기기에 ca.crt 를 다시 배포해야 합니다."
    openssl genrsa -out ca.key 2048
    openssl req -new -x509 -days "$DAYS_CA" -key ca.key -out ca.crt -subj "/CN=DavoCA" \
        -addext "basicConstraints=critical,CA:TRUE" \
        -addext "keyUsage=critical,keyCertSign,cRLSign"
fi

echo "=== 3. 브로커 키 및 인증서 생성 (SAN: $SAN) ==="
# openssl x509 -req 는 -extfile 을 주지 않으면 확장을 하나도 넣지 않는다.
# 그렇게 만든 CN 만 있는 인증서는 OpenSSL 은 CN 으로 폴백해 받아주지만,
# Apple 은 macOS 10.15 부터 CN 폴백을 없애서 SAN 이 없으면 그냥 거부한다.
cat > server_ext.cnf <<EOF
basicConstraints = CA:FALSE
keyUsage = critical, digitalSignature, keyEncipherment
extendedKeyUsage = serverAuth
subjectAltName = $SAN
EOF

openssl genrsa -out server.key 2048
openssl req -new -key server.key -out server.csr -subj "/CN=DaboBroker"
openssl x509 -req -in server.csr -CA ca.crt -CAkey ca.key -CAcreateserial \
    -out server.crt -days "$DAYS_SERVER" -extfile server_ext.cnf

echo "=== 4. 발급 결과 확인 ==="
openssl verify -CAfile ca.crt server.crt
openssl x509 -in server.crt -noout -subject -issuer -dates -ext subjectAltName

echo "=== 5. Mosquitto 시스템 폴더로 복사 및 권한 설정 ==="
sudo mkdir -p "$MOSQUITTO_CONF_DIR"
sudo cp ca.crt server.crt server.key "$MOSQUITTO_CONF_DIR/"
sudo chown -R mosquitto:mosquitto "$MOSQUITTO_CONF_DIR/"

echo "=========================================================="
echo " 브로커 인증서를 발급했습니다. 아래를 실행해야 반영됩니다:"
echo "   sudo systemctl restart mosquitto"
echo ""
echo " CA 를 새로 만든 경우에만 클라이언트 기기(Qt, 웨어러블 등)에"
echo " 아래 파일을 다시 배포하세요:"
echo " -> $CERT_DIR/ca.crt"
echo "=========================================================="
