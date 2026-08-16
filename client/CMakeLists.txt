cmake_minimum_required(VERSION 3.19)
project(daboyijo LANGUAGES CXX)
find_package(Qt6 6.5 REQUIRED COMPONENTS Core Widgets Network Sql Multimedia MultimediaWidgets LinguistTools Mqtt)

# 알림 노드 원격 제어(MQTT).
# 라즈베리파이 노드들은 libmosquitto 를 쓰지만 이 앱은 윈도우에서 돌기 때문에
# Qt MQTT(Qt::Mqtt)로 붙는다. 라이브러리가 달라도 주고받는 JSON 은 같아야 하므로
# 구조체 정의(veda_messages.hpp)만 노드 쪽과 공유한다.
# nlohmann 은 저장소에 들어있는 단일 헤더를 쓰므로 윈도우에 따로 설치할 게 없다.
set(VEDA_MSG_DIR    ${CMAKE_CURRENT_SOURCE_DIR}/../MQTT/MQTT_prod)
set(VEDA_THIRDPARTY ${CMAKE_CURRENT_SOURCE_DIR}/../server/third_party)

qt_standard_project_setup()
qt_add_executable(daboyijo
    WIN32 MACOSX_BUNDLE
    main.cpp
    mainwindow.cpp
    mainwindow.h
    mainwindow.ui
    videoview.cpp
    videoview.h
    theme.h
    thememanager.cpp
    thememanager.h
    wintheme.cpp
    wintheme.h
    sparkline.cpp
    sparkline.h
    vitaltile.cpp
    vitaltile.h
    alertbanner.cpp
    alertbanner.h
    activitychart.cpp
    activitychart.h
    clickslider.cpp
    clickslider.h
    auth.cpp
    auth.h
    logindialog.cpp
    logindialog.h
    signupdialog.cpp
    signupdialog.h
    mqttqtmanager.cpp
    mqttqtmanager.h
    alertmatrixpreview.cpp
    alertmatrixpreview.h
    hub75-font16.h
)

# 앱 전역 스타일시트 리소스.
# qt_add_executable 의 소스 목록에 .qrc 를 넣는 것만으로는 컴파일되지 않는다
# (이 프로젝트는 qt_standard_project_setup() 방식이라 rcc 가 자동으로 걸리지 않는다).
# Qt 6 권장대로 qt_add_resources 로 명시해야 :/style/base.qss 가 실제로 exe 에 들어간다.
qt_add_resources(daboyijo "appstyle"
    PREFIX "/style"
    BASE "resources/style"
    FILES resources/style/base.qss resources/style/dialogs.qss
)

# veda_messages.hpp (노드들과 공유하는 JSON 규격) 과 nlohmann/json.hpp 를 찾을 위치
target_include_directories(daboyijo PRIVATE ${VEDA_MSG_DIR} ${VEDA_THIRDPARTY})
qt_add_translations(
    TARGETS daboyijo
    TS_FILES daboyijo_ko_KR.ts
)
target_link_libraries(daboyijo
    PRIVATE
        Qt::Core
        Qt::Widgets
        Qt::Network
        Qt::Sql
        Qt::Multimedia
        Qt::MultimediaWidgets
        Qt::Mqtt
)
if(WIN32)
    target_link_libraries(daboyijo PRIVATE dwmapi)  # 네이티브 다크 타이틀바
endif()

# 브로커 검증용 CA 인증서를 실행 파일 옆 certs/ 로 복사한다.
# 앱은 applicationDirPath()/certs/ca.crt 를 보는데(mainwindow.cpp brokerCaPath),
# 윈도우는 exe 옆에 두면 그만이지만 macOS 는 그 경로가 번들 안
# (daboyijo.app/Contents/MacOS)이라 손으로 넣기 쉽게 빠뜨린다. 그러면 앱이 TLS 를
# 포기하고 평문으로 8883 에 붙었다가 브로커에 즉시 끊긴다.
# ca.crt 는 .gitignore 대상이라 새로 받은 저장소에는 없다 → 있을 때만 복사한다.
set(VEDA_CA_CERT ${CMAKE_CURRENT_SOURCE_DIR}/../MQTT/certs/ca.crt)
if(EXISTS ${VEDA_CA_CERT})
    add_custom_command(TARGET daboyijo POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:daboyijo>/certs
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${VEDA_CA_CERT} $<TARGET_FILE_DIR:daboyijo>/certs/ca.crt
        COMMENT "MQTT CA 인증서를 실행 파일 옆 certs/ 로 복사")
else()
    message(WARNING "MQTT CA 인증서가 없습니다(${VEDA_CA_CERT}) — MQTTS 대신 평문으로 접속하게 됩니다.")
endif()
install(TARGETS daboyijo
    BUNDLE  DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
qt_generate_deploy_app_script(
    TARGET daboyijo
    OUTPUT_SCRIPT deploy_script
    NO_UNSUPPORTED_PLATFORM_ERROR
)
install(SCRIPT ${deploy_script})