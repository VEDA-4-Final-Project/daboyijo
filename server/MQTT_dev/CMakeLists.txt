cmake_minimum_required(VERSION 3.10)
project(MqttSystem VERSION 1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Threads REQUIRED)
find_package(nlohmann_json REQUIRED)
find_library(MOSQUITTO_LIB mosquitto REQUIRED)

# 공통 경로 헤더 common 폴더속 mqtt 
include_directories(common)

add_library(common_mqtt_lib STATIC common/MqttClient_veda.cpp )


target_link_libraries(common_mqtt_lib PRIVATE ${MOSQUITTO_LIB} Threads::Threads nlohmann_json::nlohmann_json)

add_subdirectory(wearable_node)
add_subdirectory(master_node)
add_subdirectory(alarm_node)


