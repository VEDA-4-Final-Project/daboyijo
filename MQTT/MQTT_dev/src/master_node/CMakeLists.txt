add_executable(main_master_bin main_test_master_node.cpp MqttMasterManager.cpp )

target_link_libraries(main_master_bin PRIVATE common_mqtt_lib nlohmann_json::nlohmann_json )
