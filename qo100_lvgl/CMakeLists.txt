cmake_minimum_required(VERSION 3.13)
project(qo100_datv_receiver C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_definitions(LV_CONF_INCLUDE_SIMPLE)
include_directories(${CMAKE_SOURCE_DIR})
include_directories(${CMAKE_SOURCE_DIR}/src)
include_directories(${CMAKE_SOURCE_DIR}/src/lib)

add_subdirectory(lvgl)

find_package(SDL2 REQUIRED)
include_directories(${SDL2_INCLUDE_DIRS})

find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavformat libavcodec libavutil libswscale libswresample)
pkg_check_modules(WEBSOCKETS REQUIRED IMPORTED_TARGET libwebsockets)
pkg_check_modules(JSONC REQUIRED IMPORTED_TARGET json-c)

# The real target application (see tempo/mydream.png for the visual spec).
add_executable(qo100datv
    src/qo100datv.cpp
    src/lib/driver_backends.c
    src/lib/simulator_util.c
    src/lib/mouse_cursor_icon.c
    src/lib/display_backends/sdl.c
)

target_link_libraries(qo100datv lvgl ${SDL2_LIBRARIES} PkgConfig::WEBSOCKETS PkgConfig::FFMPEG PkgConfig::JSONC pthread)

# The chat uses this at runtime for complete Latin/Western-European glyph
# coverage (including German umlauts and sharp-s). Keep it beside the binary
# so debug/release builds and copied deployments resolve it identically.
add_custom_command(TARGET qo100datv POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_SOURCE_DIR}/lvgl/scripts/built_in_font/Montserrat-Medium.ttf
        $<TARGET_FILE_DIR:qo100datv>/Montserrat-Medium.ttf)
