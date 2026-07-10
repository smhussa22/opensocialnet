cmake_minimum_required(VERSION 3.24)
project(opensocialnet_native_client LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

include(FetchContent)

# ---- Protobuf for the IPC bridge ----
# The GUI <-> network-child link uses the same signaling.Envelope wire
# format the gateway already speaks, so we regenerate it locally rather
# than depending on the network module's build tree (keeps the two
# binaries cleanly decoupled).
find_package(Protobuf REQUIRED)

# Recent Protobuf is built on Abseil, and the generated .pb.cc pulls Abseil's
# CHECK/LOG machinery inline (absl::log_internal::*). The FindProtobuf module's
# protobuf::libprotobuf target under-declares those deps on some platforms
# (Homebrew's arm64 build), so link the public absl::log / absl::check targets
# explicitly — they transitively provide the log_internal symbols. QUIET +
# target check: Ubuntu's protobuf predates the Abseil rewrite and ships no
# absl CMake config at all (and needs none).
find_package(absl CONFIG QUIET)
set(PROTOBUF_ABSL_LIBS "")
if(TARGET absl::log AND TARGET absl::check)
    list(APPEND PROTOBUF_ABSL_LIBS absl::log absl::check)
endif()

set(SIGNALING_PROTO_FILE ${CMAKE_CURRENT_SOURCE_DIR}/../signaling_server/proto/signaling.proto)
set(NATIVE_PROTO_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/proto_gen)
file(MAKE_DIRECTORY ${NATIVE_PROTO_GEN_DIR}/proto)
set(NATIVE_PB_CC ${NATIVE_PROTO_GEN_DIR}/proto/signaling.pb.cc)
set(NATIVE_PB_H  ${NATIVE_PROTO_GEN_DIR}/proto/signaling.pb.h)
add_custom_command(
    OUTPUT ${NATIVE_PB_CC} ${NATIVE_PB_H}
    COMMAND ${Protobuf_PROTOC_EXECUTABLE}
            --proto_path=${CMAKE_CURRENT_SOURCE_DIR}/../signaling_server/proto
            --cpp_out=${NATIVE_PROTO_GEN_DIR}/proto
            ${SIGNALING_PROTO_FILE}
    DEPENDS ${SIGNALING_PROTO_FILE}
    COMMENT "Generating signaling.pb.{cc,h} for native_client"
)

# ---- Google OAuth desktop loopback flow deps ----
# OpenSSL: PKCE verifier hashing (SHA256) + httplib HTTPS for the token
# exchange. cpp-httplib pulls in the same dep transitively when its
# OPENSSL_SUPPORT define is on.
#
# On macOS, Homebrew's openssl@3 is keg-only, so it's off CMake's default
# search path. Point OPENSSL_ROOT_DIR at the brew prefix so FindOpenSSL
# resolves the real Cellar path fresh each configure — without this, a
# cached path can go stale across `brew upgrade openssl@3` (the versioned
# Cellar dir moves), leaving OPENSSL_FOUND true but the OpenSSL::SSL /
# OpenSSL::Crypto imported targets uncreated because the .dylib no longer
# EXISTS at the cached location.
if(APPLE AND NOT DEFINED OPENSSL_ROOT_DIR)
    find_program(BREW_EXECUTABLE brew)
    if(BREW_EXECUTABLE)
        execute_process(
            COMMAND ${BREW_EXECUTABLE} --prefix openssl@3
            OUTPUT_VARIABLE OPENSSL_ROOT_DIR
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )
    endif()
endif()
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
    ${NATIVE_PB_CC}
    ${IMGUI_SOURCES}
)

target_include_directories(native_client PRIVATE
    include
    ${CMAKE_CURRENT_SOURCE_DIR}/../network/include  # Ipc.hh (header-only)
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party         # stb_image.h (avatar decode)
    ${NATIVE_PROTO_GEN_DIR}                         # generated signaling.pb.h
    ${Protobuf_INCLUDE_DIRS}
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)

target_link_libraries(native_client PRIVATE
    SDL3::SDL3
    OpenSSL::SSL
    OpenSSL::Crypto
    httplib::httplib
    # Link the imported target rather than ${Protobuf_LIBRARIES}: recent
    # Protobuf (Homebrew's) is built on Abseil, and only the target carries
    # the transitive absl deps (log/check, etc.) through its interface. The
    # bare variable drops them, yielding undefined absl::log_internal symbols
    # at link time.
    protobuf::libprotobuf
    ${PROTOBUF_ABSL_LIBS}
)

# cpp-httplib's HTTPS code only compiles when this is defined — gated as
# a target define so SDL/ImGui aren't pulled into the SSL conditional.
target_compile_definitions(native_client PRIVATE CPPHTTPLIB_OPENSSL_SUPPORT)

target_compile_options(native_client PRIVATE -Wall -Wextra -Wpedantic)
