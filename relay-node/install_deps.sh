#!/bin/bash
set -e

SIMPLEBLE_TAG=v1.0.0
WORK_DIR=$(mktemp -d)

echo "===== 빌드 도구 및 라이브러리 설치 ====="

sudo apt update

sudo apt install -y g++ cmake git pkg-config \
    libdbus-1-dev \
    mosquitto mosquitto-clients libmosquitto-dev \
    nlohmann-json3-dev

echo "===== SimpleBLE 설치 (${SIMPLEBLE_TAG}) ====="

# apt 패키지가 없어서 소스 빌드해야 한다.
# /usr/local 에 설치되고, CMake 는 find_package(simpleble) 로 찾는다.
if [ -f /usr/local/lib/cmake/simpleble/simpleble-config.cmake ]; then
    echo "이미 설치되어 있음 — 건너뜀"
else
    git clone --depth 1 --branch ${SIMPLEBLE_TAG} \
        https://github.com/OpenBluetoothToolbox/SimpleBLE.git ${WORK_DIR}/SimpleBLE

    cmake -S ${WORK_DIR}/SimpleBLE/simpleble -B ${WORK_DIR}/build
    cmake --build ${WORK_DIR}/build -j$(nproc)
    sudo cmake --install ${WORK_DIR}/build

    sudo ldconfig
    rm -rf ${WORK_DIR}
fi

echo "===== 설치 완료 ====="
echo "빌드: cmake -S . -B build && cmake --build build"
echo "실행: ./build/relay_node"
