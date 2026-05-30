cmake_minimum_required(VERSION 3.24)
project(opensocialnet_sfu LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# libdatachannel (WebRTC) — fetched at configure time; cached across runs.
include(FetchContent)

set(NO_TESTS    ON CACHE BOOL "" FORCE)
set(NO_EXAMPLES ON CACHE BOOL "" FORCE)
FetchContent_Declare(
    LibDataChannel
    GIT_REPOSITORY https://github.com/paullouisageneau/libdatachannel.git
    GIT_TAG        master
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(LibDataChannel)

# cpp-httplib — single-header HTTP server used for the throwaway Layer 1 SDP endpoint.
FetchContent_Declare(
    cpp_httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG        v0.18.5
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(cpp_httplib)

add_library(sfu_lib STATIC
    src/SfuPeer.cc
    src/HttpSignalingEndpoint.cc
)

target_include_directories(sfu_lib PUBLIC
    include
)

target_link_libraries(sfu_lib
    PUBLIC
        LibDataChannel::LibDataChannel
        httplib::httplib
)

target_compile_options(sfu_lib PRIVATE -Wall -Wextra -Wpedantic)

add_executable(sfu src/main.cc)
target_link_libraries(sfu PRIVATE sfu_lib)
