#!/bin/bash
# 영상 스트림(stream_port, 기본 5500) TLS용 리프 인증서 발급.
# generate_certs.sh(MQTT 브로커용)가 만든 DavoCA를 그대로 재사용한다 — 새 CA를
# 여기서 만들지 않는다. 이미 배포된 ca.crt(Qt 클라이언트 certs/ca.crt)가 이 CA로
# 서명한 인증서라면 뭐든 검증할 수 있으므로, 클라이언트 쪽은 재배포가 필요 없다.

set -e

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
CERT_DIR="$SCRIPT_DIR/../certs"

# 각 Pi(2-Pi 분할: A=ch0·1, B=ch2·3)의 접속 IP. client/mainwindow.cpp의
# kDefaultHostA/kDefaultHostB와 기본값을 맞춰뒀다 — 실제 배포 IP가 다르면
# 환경변수로 넘길 것: STREAM_A_IP=172.20.32.51 STREAM_B_IP=172.20.32.50 ./generate_stream_certs.sh
STREAM_A_IP="${STREAM_A_IP:-172.20.32.51}"
STREAM_B_IP="${STREAM_B_IP:-172.20.32.50}"

# CN은 client/mainwindow.cpp의 kStreamCommonName[A|B]와 반드시 같아야 한다.
# (mqttqtmanager.cpp의 kBrokerCommonName과 같은 방식 — 이름을 바꾸면 양쪽 다 바꿀 것)
CN_A="DaboStreamA"
CN_B="DaboStreamB"

# Apple 398일 제한(MQTT 브로커 인증서와 같은 이유) — 397일로 여유를 둔다.
DAYS_SERVER=397

echo "=== 1. 인증서 저장 디렉토리 ($CERT_DIR) ==="
mkdir -p "$CERT_DIR"
cd "$CERT_DIR"

if [[ ! -f ca.key || ! -f ca.crt ]]; then
    cat >&2 <<EOF
[중단] CA를 찾지 못했습니다: $CERT_DIR/ca.key, $CERT_DIR/ca.crt

이 스크립트는 새 CA를 만들지 않습니다 — MQTT 브로커용으로 이미 발급된 DavoCA를
그대로 재사용해야 Qt 클라이언트에 배포된 certs/ca.crt가 그대로 유효합니다.
CA가 아직 없다면 먼저 generate_certs.sh를 실행해 CA를 만드세요:

    ./generate_certs.sh
EOF
    exit 1
fi
echo "기존 CA(DavoCA)를 재사용합니다."

issue_leaf() {
    local name="$1" cn="$2" ip="$3"
    echo "=== $name 발급 (CN=$cn, SAN=IP:$ip) ==="
    cat > "${name}_ext.cnf" <<EOF
basicConstraints = CA:FALSE
keyUsage = critical, digitalSignature, keyEncipherment
extendedKeyUsage = serverAuth
subjectAltName = IP:$ip
EOF
    openssl genrsa -out "${name}.key" 2048
    openssl req -new -key "${name}.key" -out "${name}.csr" -subj "/CN=${cn}"
    openssl x509 -req -in "${name}.csr" -CA ca.crt -CAkey ca.key -CAcreateserial \
        -out "${name}.crt" -days "$DAYS_SERVER" -extfile "${name}_ext.cnf"
    openssl verify -CAfile ca.crt "${name}.crt"
    rm -f "${name}.csr" "${name}_ext.cnf"
}

issue_leaf "streamA" "$CN_A" "$STREAM_A_IP"
issue_leaf "streamB" "$CN_B" "$STREAM_B_IP"

echo "=========================================================="
echo " 발급 완료. Pi에 배포하세요 (CA는 그대로 두고 리프만 옮기면 됨):"
echo "   scp $CERT_DIR/streamA.crt $CERT_DIR/streamA.key <Pi A>:~/daboyijo/server/config/"
echo "   scp $CERT_DIR/streamB.crt $CERT_DIR/streamB.key <Pi B>:~/daboyijo/server/config/"
echo ""
echo " 각 Pi의 cameras.conf에 추가:"
echo "   stream_cert_path=config/streamA.crt   (Pi B는 streamB.crt)"
echo "   stream_key_path=config/streamA.key    (Pi B는 streamB.key)"
echo ""
echo " Qt 쪽은 certs/ca.crt(기존 MQTT용과 동일 파일)를 그대로 쓰므로 재배포 불필요."
echo "=========================================================="
