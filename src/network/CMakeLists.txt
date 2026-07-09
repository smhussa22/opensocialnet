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

# Video path in the call client: x264 for encode, FFmpeg for decode +
# the encoder's colorspace plumbing. Same system deps as video_engine.
pkg_check_modules(X264 REQUIRED IMPORTED_TARGET x264)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavcodec libavutil libswscale)

# Screenshare grabs the X11 root window through MIT-SHM (Xext) — Linux
# only. macOS/Windows compile stubs in ScreenCapture.cc so no X11 dep.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    pkg_check_modules(X11 REQUIRED IMPORTED_TARGET x11 xext)
endif()

# ICE (STUN/TURN NAT traversal) for the peer-to-peer media lane. libnice
# drags in GLib; both come from the same pkg-config module.
pkg_check_modules(NICE REQUIRED IMPORTED_TARGET nice)

# Homebrew ships a macOS "system libffi" shim (.pc) whose include dir points at
# $SDKROOT/usr/include/ffi — a path the CommandLineTools SDK doesn't actually
# ship. It rides in transitively via glib -> libffi and lands in
# PkgConfig::NICE's INTERFACE_INCLUDE_DIRECTORIES, and CMake hard-errors on an
# imported target that references a non-existent include dir. libnice consumers
# don't need ffi.h, so drop any include dir that doesn't exist on disk. No-op
# everywhere the paths are real (e.g. Linux CI).
function(osn_strip_missing_includes tgt)
    if(NOT TARGET ${tgt})
        return()
    endif()
    get_target_property(_dirs ${tgt} INTERFACE_INCLUDE_DIRECTORIES)
    if(NOT _dirs)
        return()
    endif()
    set(_kept "")
    foreach(_d IN LISTS _dirs)
        if(EXISTS "${_d}")
            list(APPEND _kept "${_d}")
        endif()
    endforeach()
    set_target_properties(${tgt} PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${_kept}")
endfunction()

osn_strip_missing_includes(PkgConfig::NICE)

# Acoustic echo cancellation: SpeexEchoState + SpeexPreprocessState.
pkg_check_modules(SPEEXDSP REQUIRED IMPORTED_TARGET speexdsp)

# Signaling client (Hello/JoinVoice handshake against signaling_server)
# needs protobuf for the Envelope wire format and OpenSSL for the SHA1
# step of the WebSocket upgrade handshake. The generated pb files live
# under src/signaling_server/proto and are checked in, so we don't have
# to regenerate them here — the server's CMake owns regeneration.
find_package(Protobuf REQUIRED)
find_package(OpenSSL REQUIRED)

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

# Regenerate signaling.pb.{cc,h} into our build tree against the protobuf
# headers we're actually linking against. The checked-in copy in
# src/signaling_server/proto/ was emitted by a newer protoc (vcpkg-built)
# and includes runtime_version.h, which the system libprotobuf-dev doesn't
# ship — so we can't reuse the checked-in .pb.cc verbatim.
set(SIGNALING_PROTO_FILE ${CMAKE_CURRENT_SOURCE_DIR}/../signaling_server/proto/signaling.proto)
set(SIGNALING_PROTO_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/proto_gen)
file(MAKE_DIRECTORY ${SIGNALING_PROTO_GEN_DIR}/proto)
set(SIGNALING_PB_CC ${SIGNALING_PROTO_GEN_DIR}/proto/signaling.pb.cc)
set(SIGNALING_PB_H  ${SIGNALING_PROTO_GEN_DIR}/proto/signaling.pb.h)

add_custom_command(
    OUTPUT ${SIGNALING_PB_CC} ${SIGNALING_PB_H}
    COMMAND ${Protobuf_PROTOC_EXECUTABLE}
            --proto_path=${CMAKE_CURRENT_SOURCE_DIR}/../signaling_server/proto
            --cpp_out=${SIGNALING_PROTO_GEN_DIR}/proto
            ${SIGNALING_PROTO_FILE}
    DEPENDS ${SIGNALING_PROTO_FILE}
    COMMENT "Generating signaling.pb.{cc,h} into network build tree"
)

add_executable(network
    src/main.cc
    src/UdpSocket.cc
    src/UdpSender.cc
    src/UdpReceiver.cc
    src/PacketJitterBuffer.cc
    src/PeerMixer.cc
    src/LossSim.cc
    src/WebSocketClient.cc
    src/SignalingClient.cc
    src/IceTransport.cc
    src/RttProber.cc
    ../audio_engine/src/AudioStream.cc
    ../audio_engine/src/AudioCapture.cc
    ../audio_engine/src/AudioEncoder.cc
    ../audio_engine/src/AudioDecoder.cc
    ../plc/src/AudioPlc.cc
    ../plc/src/VideoPlc.cc
    src/PeerVideoRouter.cc
    src/AecFilter.cc
    ../video_engine/src/VideoCapture.cc
    ../video_engine/src/VideoEncoder.cc
    ../video_engine/src/VideoDecoder.cc
    ../video_engine/src/VideoRenderer.cc
    ../video_engine/src/VideoPacketizer.cc
    ../video_engine/src/VideoReassembler.cc
    ../video_engine/src/ScreenCapture.cc
    ${SIGNALING_PB_CC}
)

target_include_directories(network PRIVATE
    include
    ../audio_engine/include
    ../plc/include
    ../video_engine/include
    ${SIGNALING_PROTO_GEN_DIR}
    ${Protobuf_INCLUDE_DIRS}
)

target_link_libraries(network PRIVATE
    SDL3::SDL3
    PkgConfig::OPUS
    PkgConfig::X264
    PkgConfig::FFMPEG
    PkgConfig::NICE
    PkgConfig::SPEEXDSP
    # Link the imported target rather than ${Protobuf_LIBRARIES} so the
    # transitive Abseil deps come through (see the absl note above); the bare
    # variable drops them, yielding undefined absl::log_internal symbols.
    protobuf::libprotobuf
    ${PROTOBUF_ABSL_LIBS}
    OpenSSL::Crypto
)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(network PRIVATE PkgConfig::X11)
endif()

# macOS camera capture uses FFmpeg's avfoundation input device.
if(APPLE)
    pkg_check_modules(AVDEVICE QUIET IMPORTED_TARGET libavformat libavdevice)
    if(AVDEVICE_FOUND AND TARGET PkgConfig::AVDEVICE)
        target_link_libraries(network PRIVATE PkgConfig::AVDEVICE)
    else()
        message(STATUS "libavformat/libavdevice not found — macOS camera capture disabled (install Homebrew ffmpeg)")
    endif()
endif()

# Tiny mic-to-WAV recorder. Same SDL/AudioCapture stack as the network
# binary; lives here instead of in src/bench/ because the bench is
# deliberately offline and shouldn't pull SDL.
add_executable(record
    src/record.cc
    ../audio_engine/src/AudioCapture.cc
    ../audio_engine/src/AudioStream.cc
)

target_include_directories(record PRIVATE
    include
    ../audio_engine/include
)

target_link_libraries(record PRIVATE SDL3::SDL3)