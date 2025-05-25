cmake_minimum_required(VERSION 3.22)
project(1t VERSION 0.1 LANGUAGES CXX)

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    find_program(LOCAL_CLANG clang++)
    if(LOCAL_CLANG)
        message(STATUS "Clang detected: switching compiler to ${LOCAL_CLANG}")
        set(CMAKE_CXX_COMPILER "${LOCAL_CLANG}" CACHE FILEPATH "" FORCE)
    endif()
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    add_compile_options(
        -O3
        -Wall -Wextra -Wpedantic
        -Wconversion -Wsign-conversion -Wshadow
        -Wunused -Wunreachable-code
        -fcolor-diagnostics
        -fdata-sections -ffunction-sections
    )
    add_link_options(
        -Wl,--gc-sections
    )
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    add_compile_options(
        -O3
        -Wunused -Wunreachable-code
        -fanalyzer
        -fdata-sections -ffunction-sections
    )
    add_link_options(-Wl,--gc-sections)
endif()

find_program(CLANG_TIDY_EXE NAMES clang-tidy)
if(CLANG_TIDY_EXE AND ENABLE_DEBUG)
    set(CMAKE_CXX_CLANG_TIDY
        ${CLANG_TIDY_EXE};
        -checks=*;
        -warnings-as-errors=*;
        -extra-arg=-std=c++23
    )
    message(STATUS "Clang-Tidy enabled")
endif()

find_program(SCAN_BUILD_EXE NAMES scan-build)
if(SCAN_BUILD_EXE AND ENABLE_DEBUG)
    set_property(GLOBAL PROPERTY RULE_LAUNCH_COMPILE "${SCAN_BUILD_EXE}")
    message(STATUS "scan-build enabled – static analysis HTML will appear after build")
endif()

if(ENABLE_DEBUG)
    message(STATUS "Debug build enabled")
    add_definitions(-DENABLE_DEBUG=1)
    add_compile_options(
        -g
        -DDEBUG=1
        -Wall -Wextra -Wpedantic
        -Wconversion -Wsign-conversion -Wshadow
    )
    set(DEBUG_SRC src/debug.cpp)
else()
    message(STATUS "Release build enabled")
    add_definitions(-DENABLE_DEBUG=0)
    add_compile_options(
        -DNDEBUG
    )
    set(DEBUG_SRC "")
endif()

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 6.2 REQUIRED COMPONENTS Core Gui Widgets)

add_executable(1t
    src/1t.cpp
    src/terminalwidget.cpp
    src/escapeparser.cpp
    ${DEBUG_SRC}
)

target_link_libraries(1t PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
)

foreach(mod Core Gui Widgets)
    target_include_directories(1t SYSTEM PRIVATE
        $<TARGET_PROPERTY:Qt6::${mod},INTERFACE_INCLUDE_DIRECTORIES>)
endforeach()

install(TARGETS 1t RUNTIME DESTINATION bin)
install(FILES 1t.desktop DESTINATION share/applications)
install(FILES 1t.png     DESTINATION share/icons/hicolor/256x256/apps)
