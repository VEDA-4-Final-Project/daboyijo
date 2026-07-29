cmake_minimum_required(VERSION 3.19)
project(daboyijo LANGUAGES CXX)
find_package(Qt6 6.5 REQUIRED COMPONENTS Core Widgets Network Sql Multimedia MultimediaWidgets LinguistTools)
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
    wintheme.cpp
    wintheme.h
    auth.cpp
    auth.h
    logindialog.cpp
    logindialog.h
    signupdialog.cpp
    signupdialog.h
)
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
)
if(WIN32)
    target_link_libraries(daboyijo PRIVATE dwmapi)  # 네이티브 다크 타이틀바
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