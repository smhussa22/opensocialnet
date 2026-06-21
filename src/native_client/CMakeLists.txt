cmake_minimum_required(VERSION 3.24)
project(opensocialnet_native_client LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

include(FetchContent)

# ---- Google OAuth desktop loopback flow deps ----
# OpenSSL: PKCE verifier hashing (SHA256) + httplib HTTPS for the token
# exchange. cpp-httplib pulls in the same dep transitively when its
# OPENSSL_SUPPORT define is on.
find_package(OpenSSL REQUIRED)
FetchContent_Declare(
    cpp-httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG        v0.15.3
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(cpp-httplib)

# ---- SDL3 (windowing + audio + renderer) ----
# Kept on the shared default for now to match src/network's loopback exe.
# For a distributable single-binary build we'll flip SDL_STATIC=ON +
# SDL_SHARED=OFF here (and rebuild the FetchContent cache) so the resulting
# native_client binary doesn't require libSDL3.so to be installed on the
# user's machine. TODO once the rest of the pipeline is functional.
FetchContent_Declare(
    SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG        release-3.2.x
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(SDL3)

# ---- Dear ImGui (docking branch — gives us dockable panels for chat,
# room list, video tiles laid out independently) ----
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        docking
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(imgui)

# ImGui ships its source as a loose set of .cpp files — there's no CMakeLists
# upstream, you compile them yourself. Bundle the core + SDL3 + SDL_Renderer
# backends into the native_client target directly so we don't grow a separate
# imgui static lib that everything else has to link against too.
set(IMGUI_SOURCES
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer3.cpp
)

add_executable(native_client
    src/main.cc
    src/OAuthClient.cc
    ${IMGUI_SOURCES}
)

target_include_directories(native_client PRIVATE
    include
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)

target_link_libraries(native_client PRIVATE
    SDL3::SDL3
    OpenSSL::SSL
    OpenSSL::Crypto
    httplib::httplib
)

# cpp-httplib's HTTPS code only compiles when this is defined — gated as
# a target define so SDL/ImGui aren't pulled into the SSL conditional.
target_compile_definitions(native_client PRIVATE CPPHTTPLIB_OPENSSL_SUPPORT)

target_compile_options(native_client PRIVATE -Wall -Wextra -Wpedantic)
