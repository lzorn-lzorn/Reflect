cmake_minimum_required(VERSION 3.20)
project(reflect CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(reflect main.cpp)

target_include_directories(reflect PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
