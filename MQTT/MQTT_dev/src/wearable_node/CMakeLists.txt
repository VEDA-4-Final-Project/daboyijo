#wearable_node/CMakeLists.txt

add_executable(wearable_node_bin main_wearable.cpp)

target_link_libraries(wearable_node_bin PRIVATE common_mqtt_lib nlohmann_json::nlohmann_json )

