#!/bin/bash

echo "===== 빌드 도구들 설치 ====="

sudo apt update

sudo apt install -y g++ cmake mosquitto mosquitto-clients libmosquitto-dev openssl libssl-dev nlohmann-json3-dev

echo "===== 설치 완료 ====="
