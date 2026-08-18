add_executable(qt_node mqtt_qt_node.cpp) 

target_link_libraries(qt_node PRIVATE common_mqtt_lib)

