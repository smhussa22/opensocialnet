cmake_minimum_required(VERSION 3.24)

project(opensocialnet LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)
FetchContent_Declare(
    SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG        release-3.2.x
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(SDL3)

find_package(PkgConfig REQUIRED)
pkg_check_modules(OPUS REQUIRED IMPORTED_TARGET opus)

add_executable(network
    src/main.cc
    src/UdpSocket.cc
    src/UdpSender.cc
    src/UdpReceiver.cc
    src/PacketJitterBuffer.cc
    src/LossSim.cc
    ../audio_engine/src/AudioStream.cc
    ../audio_engine/src/AudioCapture.cc
    ../audio_engine/src/AudioEncoder.cc
    ../audio_engine/src/AudioDecoder.cc
)

target_include_directories(network PRIVATE
    include
    ../audio_engine/include
)

target_link_libraries(network PRIVATE SDL3::SDL3 PkgConfig::OPUS)