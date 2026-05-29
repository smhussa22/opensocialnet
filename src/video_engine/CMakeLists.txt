cmake_minimum_required(VERSION 3.24)
project(opensocialnet LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)

pkg_check_modules(X264 REQUIRED IMPORTED_TARGET x264)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavcodec libavutil libswscale)
pkg_check_modules(SDL3 REQUIRED IMPORTED_TARGET sdl3)

add_library(video_engine STATIC
    src/VideoCapture.cc
    src/VideoEncoder.cc
    src/VideoDecoder.cc
    src/VideoRenderer.cc
    src/VideoPacketizer.cc
    src/VideoReassembler.cc
    ../network/src/UdpSocket.cc
    ../network/src/UdpSender.cc
    ../network/src/UdpReceiver.cc
    ../network/src/PacketJitterBuffer.cc
)

target_include_directories(video_engine PUBLIC
    include
    ../network/include
)

target_link_libraries(video_engine
    PUBLIC
        PkgConfig::X264
        PkgConfig::FFMPEG
        PkgConfig::SDL3
)

target_compile_options(video_engine PRIVATE -Wall -Wextra -Wpedantic)

# Test executables
add_executable(test_capture_encode src/test_capture_encode.cc)
target_link_libraries(test_capture_encode PRIVATE video_engine)

add_executable(test_synthetic src/test_synthetic.cc)
target_link_libraries(test_synthetic PRIVATE video_engine)

add_executable(test_render src/test_render.cc)
target_link_libraries(test_render PRIVATE video_engine)

add_executable(test_roundtrip src/test_roundtrip.cc)
target_link_libraries(test_roundtrip PRIVATE video_engine)

add_executable(test_video_loopback src/test_video_loopback.cc)
target_link_libraries(test_video_loopback PRIVATE video_engine)