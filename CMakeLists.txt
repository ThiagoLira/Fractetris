cmake_minimum_required(VERSION 3.16)
project(gbtetris C)

set(CMAKE_C_STANDARD 99)
add_compile_options(-Wall -Wextra)

add_executable(tetris
    src/main.c
    src/game.c
    src/input.c
    src/audio.c
    src/video.c
    src/assets.c
)

add_executable(fractal fractal/fractal.c)

if(EMSCRIPTEN)
    # emcmake cmake -B build-web && cmake --build build-web
    set_target_properties(tetris PROPERTIES SUFFIX ".html")
    target_compile_options(tetris PRIVATE -sUSE_SDL=2)
    target_link_options(tetris PRIVATE
        -sUSE_SDL=2
        # fixed-size heap: growth makes a resizable ArrayBuffer that trips
        # TextDecoder in Firefox; 64MB is far more than we need
        -sINITIAL_MEMORY=64MB
        --preload-file ${CMAKE_SOURCE_DIR}/assets@/assets
        --shell-file ${CMAKE_SOURCE_DIR}/web/shell.html
    )
    set_target_properties(fractal PROPERTIES SUFFIX ".html")
    target_compile_options(fractal PRIVATE -sUSE_SDL=2)
    target_link_options(fractal PRIVATE
        -sUSE_SDL=2
        -sINITIAL_MEMORY=128MB
        -sMIN_WEBGL_VERSION=2 -sMAX_WEBGL_VERSION=2
        --preload-file ${CMAKE_SOURCE_DIR}/assets@/assets
        --shell-file ${CMAKE_SOURCE_DIR}/web/shell.html
    )
else()
    find_package(SDL2 REQUIRED)
    target_link_libraries(tetris SDL2::SDL2 m)
    target_link_libraries(fractal SDL2::SDL2 m GLESv2)
endif()

if(EMSCRIPTEN)
    set_target_properties(tetris fractal PROPERTIES
        LINK_DEPENDS ${CMAKE_SOURCE_DIR}/web/shell.html)
endif()
