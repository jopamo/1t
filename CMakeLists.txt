cmake_minimum_required(VERSION 3.16)
project(Texxy LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 6.2 COMPONENTS Core Gui Widgets REQUIRED)

add_executable(1t
    src/1t.cpp
    src/terminalwidget.cpp
    src/escapeparser.cpp
    src/mousehandler.cpp
)

target_link_libraries(1t
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
)

install(TARGETS 1t
    RUNTIME DESTINATION bin
)

install(FILES 1t.desktop
    DESTINATION share/applications
)

install(FILES 1t.png
    DESTINATION share/icons/hicolor/256x256/apps
)
