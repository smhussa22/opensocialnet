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

# Protobuf + gRPC come from the system (apt). matches the pattern in
# signaling_server/CMakeLists.txt so the two services share one toolchain.
find_package(Protobuf REQUIRED)
find_package(gRPC CONFIG REQUIRED)

# generate signaling.pb.cc/.h (cross-imported by sfu_control.proto) and
# sfu_control.pb.cc/.h + sfu_control.grpc.pb.cc/.h. signaling.proto lives
# in the signaling_server directory; we pull it in via --proto_path so the
# import "signaling.proto" line in sfu_control.proto resolves.
set(SFU_PROTO_DIR ${CMAKE_CURRENT_SOURCE_DIR}/proto)
set(SIGNALING_PROTO_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../signaling_server/proto)
set(SFU_PROTO_FILE ${SFU_PROTO_DIR}/sfu_control.proto)
set(SIGNALING_PROTO_FILE ${SIGNALING_PROTO_DIR}/signaling.proto)

set(GENERATED_SFU_PB_CC  ${SFU_PROTO_DIR}/sfu_control.pb.cc)
set(GENERATED_SFU_PB_H   ${SFU_PROTO_DIR}/sfu_control.pb.h)
set(GENERATED_SFU_GRPC_CC ${SFU_PROTO_DIR}/sfu_control.grpc.pb.cc)
set(GENERATED_SFU_GRPC_H  ${SFU_PROTO_DIR}/sfu_control.grpc.pb.h)
set(GENERATED_SIG_PB_CC  ${SFU_PROTO_DIR}/signaling.pb.cc)
set(GENERATED_SIG_PB_H   ${SFU_PROTO_DIR}/signaling.pb.h)

add_custom_command(
    OUTPUT
        ${GENERATED_SFU_PB_CC} ${GENERATED_SFU_PB_H}
        ${GENERATED_SFU_GRPC_CC} ${GENERATED_SFU_GRPC_H}
        ${GENERATED_SIG_PB_CC} ${GENERATED_SIG_PB_H}
    COMMAND protobuf::protoc
    ARGS
        --proto_path=${SFU_PROTO_DIR}
        --proto_path=${SIGNALING_PROTO_DIR}
        --cpp_out=${SFU_PROTO_DIR}
        --grpc_out=${SFU_PROTO_DIR}
        --plugin=protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin>
        ${SFU_PROTO_FILE} ${SIGNALING_PROTO_FILE}
    DEPENDS ${SFU_PROTO_FILE} ${SIGNALING_PROTO_FILE}
    COMMENT "Generating SFU gRPC + Protobuf sources"
)

set_source_files_properties(
    ${GENERATED_SFU_PB_CC} ${GENERATED_SFU_GRPC_CC} ${GENERATED_SIG_PB_CC}
    PROPERTIES GENERATED TRUE
)

add_custom_target(generate_sfu_proto
    DEPENDS
        ${GENERATED_SFU_PB_CC} ${GENERATED_SFU_GRPC_CC} ${GENERATED_SIG_PB_CC}
)

add_library(sfu_lib STATIC
    src/SfuPeer.cc
    src/Room.cc
    src/RoomRegistry.cc
    src/SfuStats.cc
    src/SfuGrpcService.cc
    ${GENERATED_SFU_PB_CC}
    ${GENERATED_SFU_GRPC_CC}
    ${GENERATED_SIG_PB_CC}
)

add_dependencies(sfu_lib generate_sfu_proto)

target_include_directories(sfu_lib PUBLIC
    include
    ${SFU_PROTO_DIR}
)

target_link_libraries(sfu_lib
    PUBLIC
        LibDataChannel::LibDataChannel
        protobuf::libprotobuf
        gRPC::grpc++
)

target_compile_options(sfu_lib PRIVATE -Wall -Wextra -Wpedantic)

add_executable(sfu src/main.cc)
target_link_libraries(sfu PRIVATE sfu_lib)
