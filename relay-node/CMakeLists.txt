# 의존성 설치는 ./install_deps.sh 참고
cmake_minimum_required(VERSION 3.10)
project(VedaRelayNode VERSION 1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Threads REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(simpleble REQUIRED)
find_library(MOSQUITTO_LIB mosquitto REQUIRED)

# MQTT 클라이언트는 프로젝트 공용 구현(MQTT/MQTT_prod)을 그대로 사용한다.
set(MQTT_COMMON_DIR ${CMAKE_SOURCE_DIR}/../MQTT/MQTT_prod)

include_directories(${CMAKE_SOURCE_DIR}/src ${MQTT_COMMON_DIR})

add_executable(relay_node src/main_relay.cpp ${MQTT_COMMON_DIR}/MqttClient_veda.cpp)

target_link_libraries(relay_node PRIVATE ${MOSQUITTO_LIB} Threads::Threads nlohmann_json::nlohmann_json simpleble::simpleble)
