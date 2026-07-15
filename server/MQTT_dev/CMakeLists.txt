cmake_minimum_required(VERSION 3.10)
project(MqttSystem VERSION 1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_library(MOSQUITTO_LIB mosquitto REQUIRED)


add_executable(mqtt_test_bin client_test/main.cpp common/MqttClient_veda.cpp )

target_include_directories(mqtt_test_bin PRIVATE common)

target_link_libraries(mqtt_test_bin PRIVATE ${MOSQUITTO_LIB})
