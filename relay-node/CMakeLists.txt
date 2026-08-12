# 의존성 설치는 ./install_deps.sh 참고
cmake_minimum_required(VERSION 3.10)
project(VedaRelayNode VERSION 1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 실행 파일 위치를 relay-node/ 로 — 설정 파일을 실행 파일 옆에서 찾는데
# build/ 는 지웠다 다시 만드는 곳이라 거기 두면 매번 사라짐
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR})

find_package(Threads REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(simpleble REQUIRED)
find_library(MOSQUITTO_LIB mosquitto REQUIRED)

# MQTT 클라이언트는 프로젝트 공용 구현(MQTT/MQTT_prod) 사용
set(MQTT_COMMON_DIR ${CMAKE_SOURCE_DIR}/../MQTT/MQTT_prod)

include_directories(${CMAKE_SOURCE_DIR}/src ${MQTT_COMMON_DIR})

add_executable(relay_node src/main_relay.cpp ${MQTT_COMMON_DIR}/MqttClient_veda.cpp)

target_link_libraries(relay_node PRIVATE ${MOSQUITTO_LIB} Threads::Threads nlohmann_json::nlohmann_json simpleble::simpleble)
