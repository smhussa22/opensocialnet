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

# Signaling client (Hello/JoinVoice handshake against signaling_server)
# needs protobuf for the Envelope wire format and OpenSSL for the SHA1
# step of the WebSocket upgrade handshake. The generated pb files live
# under src/signaling_server/proto and are checked in, so we don't have
# to regenerate them here — the server's CMake owns regeneration.
find_package(Protobuf REQUIRED)
find_package(OpenSSL REQUIRED)

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
    src/LossSim.cc
    src/WebSocketClient.cc
    src/SignalingClient.cc
    ../audio_engine/src/AudioStream.cc
    ../audio_engine/src/AudioCapture.cc
    ../audio_engine/src/AudioEncoder.cc
    ../audio_engine/src/AudioDecoder.cc
    ../plc/src/AudioPlc.cc
    ${SIGNALING_PB_CC}
)

target_include_directories(network PRIVATE
    include
    ../audio_engine/include
    ../plc/include
    ${SIGNALING_PROTO_GEN_DIR}
    ${Protobuf_INCLUDE_DIRS}
)

target_link_libraries(network PRIVATE
    SDL3::SDL3
    PkgConfig::OPUS
    ${Protobuf_LIBRARIES}
    OpenSSL::Crypto
)

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