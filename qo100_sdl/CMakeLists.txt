cmake_minimum_required(VERSION 3.16)
project(qo100_sdl_receiver LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
pkg_check_modules(SDL2_TTF REQUIRED IMPORTED_TARGET SDL2_ttf)
pkg_check_modules(WEBSOCKETS REQUIRED IMPORTED_TARGET libwebsockets)

add_executable(qo100sdl src/main.cpp)
target_compile_options(qo100sdl PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(qo100sdl PRIVATE
    PkgConfig::SDL2
    PkgConfig::SDL2_TTF
    PkgConfig::WEBSOCKETS
    pthread)

set(QO100_FONT_SOURCE
    "${CMAKE_CURRENT_SOURCE_DIR}/../qo100_lvgl/lvgl/scripts/built_in_font/Montserrat-Medium.ttf")
if(EXISTS "${QO100_FONT_SOURCE}")
    add_custom_command(TARGET qo100sdl POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${QO100_FONT_SOURCE}"
            "$<TARGET_FILE_DIR:qo100sdl>/Montserrat-Medium.ttf")
else()
    message(FATAL_ERROR "Montserrat font not found at ${QO100_FONT_SOURCE}")
endif()
