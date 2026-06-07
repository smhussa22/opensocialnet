cmake_minimum_required(VERSION 3.24)

project(opensocialnet_relay LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

# Zero external deps — pure POSIX sockets + STL. The only project header
# the relay pulls in is Packet.hh (so the wire layout stays single-sourced
# with the native client). That keeps the runtime container tiny and the
# build time near-instant.
add_executable(relay
    src/main.cc
    src/RoomTable.cc
)

target_include_directories(relay PRIVATE
    include
    ../network/include
)

target_compile_options(relay PRIVATE -Wall -Wextra -Wpedantic)
