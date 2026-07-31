cmake_minimum_required(VERSION 3.10)
project(VedaRelayNode VERSION 1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Threads REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(simpleble REQUIRED)
find_library(MOSQUITTO_LIB mosquitto REQUIRED)

include_directories(${CMAKE_SOURCE_DIR}/src)

add_executable(relay_node src/main_relay.cpp src/MqttClient_veda.cpp)

target_link_libraries(relay_node PRIVATE ${MOSQUITTO_LIB} Threads::Threads nlohmann_json::nlohmann_json simpleble::simpleble)
