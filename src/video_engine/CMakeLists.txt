cmake_minimum_required(VERSION 3.24)
project(opensocialnet LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(x264 REQUIRED)
find_package(FFMPEG REQUIRED COMPONENTS AVCODEC AVUTIL SWSCALE)
find_package(SDL2 REQUIRED)

add_library(video_engine STATIC
    src/VideoCapture.cc
    src/VideoEncoder.cc
    src/VideoDecoder.cc
    src/VideoRenderer.cc
)

target_include_directories(video_engine PUBLIC include)

target_link_libraries(video_engine
    PUBLIC
        x264::x264
        FFMPEG::AVCODEC
        FFMPEG::AVUTIL
        FFMPEG::SWSCALE
        SDL2::SDL2
)

target_compile_options(video_engine PRIVATE -Wall -Wextra -Wpedantic)