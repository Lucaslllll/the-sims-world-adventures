cmake_minimum_required(VERSION 3.18)
project(ts3a C CXX ASM)

set(TP ${CMAKE_CURRENT_SOURCE_DIR}/../../../../third_party)

# --- SDL2 -------------------------------------------------------------------
set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
add_subdirectory(${TP}/SDL2-2.30.9 ${CMAKE_BINARY_DIR}/sdl2)

# --- OpenAL Soft ------------------------------------------------------------
set(ALSOFT_UTILS OFF CACHE BOOL "" FORCE)
set(ALSOFT_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ALSOFT_TESTS OFF CACHE BOOL "" FORCE)
set(ALSOFT_INSTALL OFF CACHE BOOL "" FORCE)
set(ALSOFT_INSTALL_CONFIG OFF CACHE BOOL "" FORCE)
set(ALSOFT_INSTALL_HRTF_DATA OFF CACHE BOOL "" FORCE)
set(ALSOFT_INSTALL_AMBDEC_PRESETS OFF CACHE BOOL "" FORCE)
set(ALSOFT_INSTALL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ALSOFT_INSTALL_UTILS OFF CACHE BOOL "" FORCE)
set(ALSOFT_NO_CONFIG_UTIL ON CACHE BOOL "" FORCE)
add_subdirectory(${TP}/openal-soft-1.23.1 ${CMAKE_BINARY_DIR}/openal)

# --- Loader + HLE -----------------------------------------------------------
file(GLOB_RECURSE TS3WA_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/loader/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/loader/*.S
    ${CMAKE_CURRENT_SOURCE_DIR}/objc/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/objc/*.S
    ${CMAKE_CURRENT_SOURCE_DIR}/hle/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/hle/*.S
    ${CMAKE_CURRENT_SOURCE_DIR}/platform/*.cpp
)

add_library(ts3a SHARED ${TS3WA_SOURCES})
target_include_directories(ts3a PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${TP}
    ${TP}/openal-soft-1.23.1/include
)
target_compile_options(ts3a PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-std=c++20>
    -Wall -Wno-unused-function -Wno-unused-variable -Wno-c++11-narrowing
    -fno-omit-frame-pointer
)
target_link_libraries(ts3a
    SDL2 OpenAL
    GLESv3 EGL
    mediandk android log z dl
)
