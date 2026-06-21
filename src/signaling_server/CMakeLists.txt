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
# Needed at link time for HMAC / CRYPTO_memcmp used by the Hello auth path.
# cassandra-cpp-driver pulls libcrypto at runtime but does not export the
# symbols, so we have to link it ourselves.
find_package(OpenSSL REQUIRED)

# ---- Google OAuth verification deps (header-only via FetchContent) ----
# cpp-httplib: HTTPS GET for the JWKS endpoint. Compiled with OpenSSL so
# it can talk to https://www.googleapis.com/oauth2/v3/certs directly.
# jwt-cpp: RS256 JWT verification against the JWKS-derived public key.
include(FetchContent)
FetchContent_Declare(
        cpp-httplib
        GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
        GIT_TAG        v0.15.3
        GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(cpp-httplib)

FetchContent_Declare(
        jwt-cpp
        GIT_REPOSITORY https://github.com/Thalhammer/jwt-cpp.git
        GIT_TAG        v0.7.0
        GIT_SHALLOW    TRUE
)
# Tell jwt-cpp to skip the default OpenSSL search — we already have the
# OpenSSL::SSL/Crypto targets from find_package above.
set(JWT_DISABLE_PICOJSON OFF CACHE BOOL "" FORCE)
set(JWT_BUILD_EXAMPLES   OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(jwt-cpp)

# cassandra-cpp-driver: built from source into $HOME/local (vcpkg dropped this port).
# Find the header + .so by hint rather than CMake config since the driver doesn't
# install one. Embed an RPATH so the binary can locate libcassandra.so at runtime.
find_path(CASSANDRA_INCLUDE_DIR cassandra.h
        HINTS $ENV{HOME}/local/include
        REQUIRED)
find_library(CASSANDRA_LIB cassandra
        HINTS $ENV{HOME}/local/lib
        REQUIRED)

set(PROTO_DIR ${CMAKE_CURRENT_SOURCE_DIR}/proto)
set(PROTO_FILE ${PROTO_DIR}/signaling.proto)
set(GENERATED_DIR ${PROTO_DIR})

# Regenerate signaling.pb.cc from signaling.proto. No gRPC now that the SFU
# is gone — the only inter-service control plane the gateway needs going
# forward (relay membership) is small enough to do over a simple protobuf
# message or even just CLI on the relay. We can add gRPC back if it grows.
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
        COMMENT "Generating signaling.pb.{cc,h}"
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
        src/Auth.cc
        src/EnvelopeHandlers.cc
        src/JwksCache.cc
        src/JwtVerifier.cc
        src/KafkaBus.cc
        src/ScyllaClient.cc
        src/Session.cc
        ${GENERATED_DIR}/signaling.pb.cc
)


add_dependencies(signaling_server generate_proto)


target_include_directories(signaling_server PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/proto
        ${Protobuf_INCLUDE_DIRS}
        ${CASSANDRA_INCLUDE_DIR}
)

target_link_libraries(signaling_server PRIVATE
        protobuf::libprotobuf
        RdKafka::rdkafka++
        unofficial::uwebsockets::uwebsockets
        ${CASSANDRA_LIB}
        OpenSSL::Crypto
        OpenSSL::SSL          # cpp-httplib needs SSL for the HTTPS JWKS fetch
        httplib::httplib
        jwt-cpp::jwt-cpp
)

# cpp-httplib needs the OpenSSL macro toggled on so its HTTPS code path
# compiles into the header. Set it as a target define rather than a global
# one so other deps don't get pulled into the SSL conditional.
target_compile_definitions(signaling_server PRIVATE CPPHTTPLIB_OPENSSL_SUPPORT)

set_target_properties(signaling_server PROPERTIES
        BUILD_RPATH   "$ENV{HOME}/local/lib"
        INSTALL_RPATH "$ENV{HOME}/local/lib"
)
