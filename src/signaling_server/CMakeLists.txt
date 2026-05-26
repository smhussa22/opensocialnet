cmake_minimum_required(VERSION 3.24)
project(signaling_server LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

find_package(Protobuf CONFIG REQUIRED)
find_package(RdKafka CONFIG REQUIRED)
find_package(unofficial-uwebsockets CONFIG REQUIRED)
find_package(cassandra-cpp-driver CONFIG REQUIRED)

set(PROTO_DIR ${CMAKE_CURRENT_SOURCE_DIR}/proto)
set(PROTO_FILE ${PROTO_DIR}/signaling.proto)
set(GENERATED_DIR ${PROTO_DIR})

add_custom_command(
        OUTPUT
        ${GENERATED_DIR}/signaling.pb.cc
        ${GENERATED_DIR}/signaling.pb.h

        COMMAND protobuf::protoc
        ARGS
        --proto_path=${PROTO_DIR}
        --cpp_out=${GENERATED_DIR}
        ${PROTO_FILE}

        DEPENDS ${PROTO_FILE}
        COMMENT "Generating protobuf sources"
)

set_source_files_properties(
        ${GENERATED_DIR}/signaling.pb.cc
        PROPERTIES GENERATED TRUE
)

add_custom_target(generate_proto
        DEPENDS
        ${GENERATED_DIR}/signaling.pb.cc
)


add_executable(signaling_server
        src/main.cc
        ${GENERATED_DIR}/signaling.pb.cc
)


add_dependencies(signaling_server generate_proto)


target_include_directories(signaling_server PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/proto
        ${Protobuf_INCLUDE_DIRS}
)

target_link_libraries(signaling_server PRIVATE
        protobuf::libprotobuf
        RdKafka::rdkafka++
        unofficial::uwebsockets::uwebsockets
        cassandra-cpp-driver
)
