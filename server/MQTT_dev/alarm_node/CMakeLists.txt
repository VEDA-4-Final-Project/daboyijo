add_executable(alarm_node_bin main_alarm.cpp AudioPlayer/VedaAudioPlayer.cpp)

target_link_libraries(alarm_node_bin PRIVATE common_mqtt_lib nlohmann_json::nlohmann_json asound )


