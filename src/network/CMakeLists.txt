cmake_minimum_required(VERSION 3.24)

project(opensocialnet LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(network src/main.cc)

# temporary; remove this include after testing
target_include_directories(network PRIVATE include../audio_engine/include)
