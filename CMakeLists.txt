cmake_minimum_required(VERSION 3.20)
project(Raylib_Test LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# FORCE MSVC (geen Clang)
if (MSVC)
    message(STATUS "Using MSVC")
else()
    message(FATAL_ERROR "This project must be built with MSVC")
endif()

add_executable(Raylib_Test
    src/main.cpp
    src/Player.cpp
)

target_include_directories(Raylib_Test PRIVATE src)

# ===== RAYLIB =====
find_package(raylib CONFIG REQUIRED)
target_link_libraries(Raylib_Test PRIVATE raylib)
