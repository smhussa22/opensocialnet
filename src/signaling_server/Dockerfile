# signaling_server image — uWebSockets gateway + gRPC client to SFU.
#
# Heavier than the SFU image because vcpkg builds protobuf / grpc /
# uwebsockets / librdkafka from source (~15-20 min first build, cached
# in layers thereafter), and the scylla cpp-driver isn't in any
# package manager so we build it from source.
#
# build (from repo root):
#   docker build -f src/signaling_server/Dockerfile -t opensocialnet-signaling .
# run:
#   docker run --rm \
#       -e OPENSOCIALNET_AUTH_SECRET=devsecret123 \
#       -p 9001:9001 \
#       opensocialnet-signaling

FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

# base build deps + vcpkg prerequisites + scylla cpp-driver deps.
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        git \
        pkg-config \
        ca-certificates \
        curl \
        zip \
        unzip \
        tar \
        autoconf \
        automake \
        libtool \
        flex \
        bison \
        python3 \
        libssl-dev \
        libuv1-dev \
        zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*

# scylla cpp-driver. The Scylla fork (a) installs the library under the
# Debian multiarch path /usr/local/lib/x86_64-linux-gnu/, and (b) renames
# it from libcassandra.so to libscylla-cpp-driver.so. The signaling_server's
# CMakeLists still uses find_library(... cassandra ...) per the local-dev
# convention, so we drop libcassandra.so + .so.2 symlinks at /usr/local/lib
# pointing at the real shared objects. The Scylla fork is API/ABI compatible
# with the original DataStax driver — those symlinks are a supported drop-in.
WORKDIR /opt
RUN git clone --depth 1 https://github.com/scylladb/cpp-driver.git scylla-cpp-driver \
    && cd scylla-cpp-driver \
    && cmake -S . -B build \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/usr/local \
        -DCASS_BUILD_STATIC=OFF \
        -DCASS_BUILD_SHARED=ON \
        -DCASS_BUILD_EXAMPLES=OFF \
        -DCASS_BUILD_TESTS=OFF \
    && cmake --build build --parallel "$(nproc)" \
    && cmake --install build \
    && ln -sf /usr/local/lib/x86_64-linux-gnu/libscylla-cpp-driver.so /usr/local/lib/libcassandra.so \
    && ln -sf /usr/local/lib/x86_64-linux-gnu/libscylla-cpp-driver.so.2 /usr/local/lib/libcassandra.so.2 \
    && echo "/usr/local/lib/x86_64-linux-gnu" > /etc/ld.so.conf.d/scylla-cpp-driver.conf \
    && ldconfig \
    && rm -rf /opt/scylla-cpp-driver

# vcpkg. classic-mode used to install the deps named in vpckg.json;
# the existing local build does the same (vcpkg.json/vpckg.json at the
# repo root isn't auto-discovered because of the misspelling).
WORKDIR /opt
RUN git clone --depth 1 https://github.com/microsoft/vcpkg.git \
    && /opt/vcpkg/bootstrap-vcpkg.sh -disableMetrics

ENV VCPKG_ROOT=/opt/vcpkg

# install C++ deps via vcpkg classic mode. takes the longest in this
# build — primarily because grpc + protobuf are each ~5-10 min from
# source. cached as long as the package set / vcpkg version is stable.
RUN /opt/vcpkg/vcpkg install \
        protobuf \
        grpc \
        librdkafka \
        uwebsockets

WORKDIR /workspace

# bring in the signaling_server source plus the SFU's gRPC service .proto.
# the signaling_server is a gRPC client of the SFU and its CMakeLists runs
# protoc against sfu_control.proto to generate the SfuControl stubs; the
# file lives in the SFU's source tree and is referenced via the relative
# path ../sfu/proto/sfu_control.proto, so we have to surface it at the
# matching layout inside the build context.
COPY src/signaling_server/ ./src/signaling_server/
COPY src/sfu/proto/sfu_control.proto ./src/sfu/proto/

WORKDIR /workspace/src/signaling_server

# build using vcpkg toolchain; HOME-relative HINTS still resolve into
# /usr/local through find_path/library's default search paths.
RUN cmake -S . -B build \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_TOOLCHAIN_FILE=/opt/vcpkg/scripts/buildsystems/vcpkg.cmake \
    && cmake --build build --parallel "$(nproc)"

# 9001: WebSocket gateway browsers connect to.
EXPOSE 9001

# signaling_server reads OPENSOCIALNET_AUTH_SECRET at startup and
# rejects every Hello frame if it's unset. Caller must set it.
ENV OPENSOCIALNET_AUTH_SECRET=""

ENTRYPOINT ["/workspace/src/signaling_server/build/signaling_server"]
