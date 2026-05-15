cmake_minimum_required(VERSION 3.16)

project(rpgai LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_LIST_DIR}/cmake)

# ===========================================================================
# Required: LuaJIT
# ===========================================================================
find_package(LuaJIT REQUIRED)

# ===========================================================================
# Required: libcurl
# ===========================================================================
find_package(CURL REQUIRED)

# ===========================================================================
# Required: Asio
#   macOS:  brew install asio
#   Ubuntu: sudo apt install libasio-dev
# ===========================================================================
find_path(ASIO_INCLUDE_DIR asio.hpp
    PATHS /usr/include /usr/local/include /opt/homebrew/include /opt/local/include
    PATH_SUFFIXES asio)
if(NOT ASIO_INCLUDE_DIR)
    message(FATAL_ERROR "Asio not found. macOS: brew install asio | Ubuntu: sudo apt install libasio-dev")
endif()

# ===========================================================================
# Header-only dependencies — included directly from vendor/
#
# No find_path: we just verify the expected files exist and add vendor/ to
# the include path. If a file is missing, cmake prints a clear error.
#
# Expected layout:
#   vendor/nlohmann/json.hpp
#   vendor/sol/sol.hpp
#   vendor/crow/crow_all.h
#   vendor/ollama/ollama.hpp
#   vendor/stb_image.h
# ===========================================================================
set(VENDOR_DIR ${CMAKE_CURRENT_SOURCE_DIR}/vendor)

foreach(HEADER
        "nlohmann/json.hpp"
        "sol/sol.hpp"
        "crow/crow_all.h"
        "ollama/ollama.hpp"
        "stb_image.h")
    if(NOT EXISTS "${VENDOR_DIR}/${HEADER}")
        message(FATAL_ERROR "Missing vendor header: vendor/${HEADER}\nSee README for dependency setup.")
    endif()
endforeach()

# ===========================================================================
# Sources
# ===========================================================================
add_executable(rpgai src/main.cpp)

# ===========================================================================
# Include directories
# ===========================================================================
target_include_directories(rpgai PRIVATE
    ${VENDOR_DIR}
    ${LUAJIT_INCLUDE_DIR}
    ${CURL_INCLUDE_DIRS}
    ${ASIO_INCLUDE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# ===========================================================================
# Link libraries
# ===========================================================================
target_link_libraries(rpgai PRIVATE
    ${LUAJIT_LIBRARIES}
    ${CURL_LIBRARIES}
)

if(APPLE)
    target_link_libraries(rpgai PRIVATE
        "-framework IOKit"
        "-framework Cocoa"
        "-framework OpenGL"
    )
endif()

if(UNIX AND NOT APPLE)
    find_package(Threads REQUIRED)
    target_link_libraries(rpgai PRIVATE Threads::Threads)
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS "9.0")
    target_link_libraries(rpgai PRIVATE stdc++fs)
endif()

if(WIN32)
    target_compile_definitions(rpgai PRIVATE _WIN32_WINNT=0x0602)
    target_link_libraries(rpgai PRIVATE ws2_32 mswsock)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_link_options(rpgai PRIVATE -static-libgcc -static-libstdc++ -static-libgcc)
        target_link_libraries(rpgai PRIVATE -static winpthread)
    endif()
endif()

# ===========================================================================
# Build type
# ===========================================================================
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

target_compile_options(rpgai PRIVATE
    $<$<CONFIG:Release>:-O2>
    $<$<CONFIG:Debug>:-g -O0>
)

install(TARGETS rpgai RUNTIME DESTINATION bin)
