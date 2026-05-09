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

add_executable(
    audio_engine

    src/main.cc
    src/AudioStream.cc
)

target_include_directories(
    audio_engine

    PRIVATE
    include
)

target_link_libraries(
    audio_engine

    PRIVATE
    SDL3::SDL3
)

target_compile_options(audio_engine PRIVATE -fsanitize=address -g)

target_link_options(audio_engine PRIVATE -fsanitize=address)