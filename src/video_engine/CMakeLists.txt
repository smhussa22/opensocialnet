cmake_minimum_required(VERSION 3.24)
project(opensocialnet LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# system deps (apt or pkg-config): x264, FFmpeg, Opus
find_package(PkgConfig REQUIRED)
pkg_check_modules(X264 REQUIRED IMPORTED_TARGET x264)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavcodec libavutil libswscale)
pkg_check_modules(OPUS REQUIRED IMPORTED_TARGET opus)

# fetched at configure time: SDL3 (no apt package on Ubuntu 24.04) and libdatachannel (WebRTC)
include(FetchContent)

FetchContent_Declare(
    SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG        release-3.2.x
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(SDL3)

set(NO_TESTS  ON CACHE BOOL "" FORCE)
set(NO_EXAMPLES ON CACHE BOOL "" FORCE)
FetchContent_Declare(
    LibDataChannel
    GIT_REPOSITORY https://github.com/paullouisageneau/libdatachannel.git
    GIT_TAG        master
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(LibDataChannel)

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
    ../audio_engine/include
)

target_link_libraries(video_engine
    PUBLIC
        PkgConfig::X264
        PkgConfig::FFMPEG
        SDL3::SDL3
        LibDataChannel::LibDataChannel
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

add_executable(video_sender src/video_sender.cc)
target_link_libraries(video_sender PRIVATE video_engine)

add_executable(video_receiver src/video_receiver.cc)
target_link_libraries(video_receiver PRIVATE video_engine)

add_executable(audio_video_client
    src/audio_video_client.cc
    ../audio_engine/src/AudioStream.cc
    ../audio_engine/src/AudioCapture.cc
    ../audio_engine/src/AudioEncoder.cc
    ../audio_engine/src/AudioDecoder.cc
)
target_include_directories(audio_video_client PRIVATE ../audio_engine/include)
target_link_libraries(audio_video_client PRIVATE video_engine PkgConfig::OPUS)