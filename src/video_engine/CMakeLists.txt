cmake_minimum_required(VERSION 3.24)
project(opensocialnet LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)

pkg_check_modules(X264 REQUIRED IMPORTED_TARGET x264)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavcodec libavutil libswscale)

# Find SDL3 from vcpkg or local build
find_package(SDL3 QUIET)
if(NOT SDL3_FOUND)
  # Try pkg-config as fallback
  pkg_check_modules(SDL3 QUIET IMPORTED_TARGET sdl3-3.0)
endif()

add_library(video_engine STATIC
    src/VideoCapture.cc
    src/VideoEncoder.cc
    src/VideoDecoder.cc
    # src/VideoRenderer.cc  (requires SDL3, not available)
)

target_include_directories(video_engine PUBLIC include)

target_link_libraries(video_engine
    PUBLIC
        PkgConfig::X264
        PkgConfig::FFMPEG
)

if(SDL3_FOUND)
  target_link_libraries(video_engine PUBLIC SDL3::SDL3)
elseif(SDL3_FOUND_PKGCONFIG)
  target_link_libraries(video_engine PUBLIC PkgConfig::SDL3)
endif()

target_compile_options(video_engine PRIVATE -Wall -Wextra -Wpedantic)

# Test executables
add_executable(test_capture_encode src/test_capture_encode.cc)
target_link_libraries(test_capture_encode PRIVATE video_engine)

add_executable(test_synthetic src/test_synthetic.cc)
target_link_libraries(test_synthetic PRIVATE video_engine)