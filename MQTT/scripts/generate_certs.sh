#!/bin/bash 

# 에러 발생시 멈추는 설정 코드 
set -e

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
CERT_DIR="$SCRIPT_DIR/../certs"
MOSQUITTO_CONF_DIR="/etc/mosquitto/ca_certificates"


echo "=== 1. 인증서 저장 디렉토리 생성 ($CERT_DIR) ==="
mkdir -p "$CERT_DIR"
cd "$CERT_DIR"


echo "=== 2. CA 키 및 루트 인증서 생성 ==="
openssl genrsa -out ca.key 2048
openssl req -new -x509 -days 3650 -key ca.key -out ca.crt -subj "/CN=DavoCA"

echo "=== 3. 브로커 키 및 인증서 생성 ==="
openssl genrsa -out ca.key 2048
openssl req -new -key server.key -out server.csr -subj "/CN=DaboBroker"
openssl x509 -reg -in server.csr -CA ca.crt -CAkey ca.key -CAcreateserial -out server.crt -days 3650

echo "=== 4. Mosquitto 시스템 폴더로 복사 및 권한 설정 ==="
sudo mkdir -p "$MOSQUITTO_CONF_DIR"
sudo cp ca.crt server.crt server.key "$MOSQUITTO_CONF_DIR/"
sudo chown -R mosquitto:mosquitto "$MOSQUITTO_CONF_DIR/"

echo "=========================================================="
echo " 성공적으로 인증서가 생성되었습니다!"
echo " 클라이언트 기기(Qt, 웨어러블 등)에는 아래 파일만 전달하세요:"
echo " -> $CERT_DIR/ca.crt"
echo "=========================================================="
