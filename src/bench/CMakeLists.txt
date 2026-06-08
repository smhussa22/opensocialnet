cmake_minimum_required(VERSION 3.24)

project(opensocialnet_bench LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

# bench is fully offline: opus + the shared transport / audio / plc
# sources, no SDL, no UDP. Builds in ~5s on a warm cache, runs without
# any audio device, takes WAV in and writes WAV out.
find_package(PkgConfig REQUIRED)
pkg_check_modules(OPUS REQUIRED IMPORTED_TARGET opus)

add_executable(bench
    src/main.cc
    src/WavIO.cc
    ../audio_engine/src/AudioEncoder.cc
    ../audio_engine/src/AudioDecoder.cc
    ../plc/src/AudioPlc.cc
)

target_include_directories(bench PRIVATE
    include
    ../network/include
    ../audio_engine/include
    ../plc/include
)

target_link_libraries(bench PRIVATE PkgConfig::OPUS)

target_compile_options(bench PRIVATE -Wall -Wextra -Wpedantic)
