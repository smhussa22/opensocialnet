# signaling_server image — uWebSockets gateway + Scylla chat + Kafka pub/sub.
#
# Multi-stage: a heavy builder stage runs scylla-cpp-driver from source
# and vcpkg install of protobuf / librdkafka / uwebsockets,
# producing ~15 GB of build intermediates. A slim runtime stage copies
# only the signaling_server binary + the two .so files it actually
# needs, leaving the final image around a few hundred MB instead of
# 10+ GB. Previously the Dockerfile was single-stage and the buildtrees
# ended up baked into the published image, blowing out disk on EC2.
#
# build (from repo root):
#   docker build -f src/signaling_server/Dockerfile -t opensocialnet-signaling .
# run:
#   docker run --rm \
#       -e OPENSOCIALNET_AUTH_SECRET=devsecret123 \
#       -p 9001:9001 \
#       opensocialnet-signaling

# ---------- builder ----------
FROM ubuntu:24.04 AS builder

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
    && cmake --build build --parallel 4 \
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
# cap parallel compiles during vcpkg source builds. protobuf + grpc each
# link a few TUs that need ~3 GB RSS apiece; running 8 wide OOMs the
# WSL2 VM and takes the buildkit daemon down with it. 2 leaves headroom.
ENV VCPKG_MAX_CONCURRENCY=2

# install C++ deps via vcpkg classic mode. protobuf is the longest single
# package; grpc was removed when the SFU pathway was retired. cached as
# long as the package set / vcpkg version is stable.
RUN /opt/vcpkg/vcpkg install \
        protobuf \
        librdkafka \
        uwebsockets

WORKDIR /workspace

COPY src/signaling_server/ ./src/signaling_server/

WORKDIR /workspace/src/signaling_server

# build using vcpkg toolchain; HOME-relative HINTS still resolve into
# /usr/local through find_path/library's default search paths.
RUN cmake -S . -B build \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_TOOLCHAIN_FILE=/opt/vcpkg/scripts/buildsystems/vcpkg.cmake \
    && cmake --build build --parallel 4

# ---------- runtime ----------
FROM ubuntu:24.04 AS runtime

ENV DEBIAN_FRONTEND=noninteractive

# minimal runtime deps. vcpkg builds protobuf / librdkafka / uwebsockets /
# openssl as static libs that already linked into the signaling_server
# binary, so we only need libuv (cassandra-cpp-driver runtime dep),
# libstdc++/libgcc, and ca-certificates here.
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates \
        libuv1 \
        libstdc++6 \
        libgcc-s1 \
    && rm -rf /var/lib/apt/lists/*

# bring across the binary + the cassandra/scylla shared objects + the
# ld.so.conf entry that points at the multiarch path. ldconfig wires up
# the loader cache so libcassandra.so resolves at runtime without
# needing LD_LIBRARY_PATH.
COPY --from=builder /workspace/src/signaling_server/build/signaling_server /usr/local/bin/signaling_server
COPY --from=builder /usr/local/lib/x86_64-linux-gnu/libscylla-cpp-driver.so* /usr/local/lib/x86_64-linux-gnu/
COPY --from=builder /usr/local/lib/libcassandra.so* /usr/local/lib/
COPY --from=builder /etc/ld.so.conf.d/scylla-cpp-driver.conf /etc/ld.so.conf.d/scylla-cpp-driver.conf
RUN ldconfig

# 9001: WebSocket gateway browsers connect to.
EXPOSE 9001

# signaling_server reads OPENSOCIALNET_AUTH_SECRET at startup and
# rejects every Hello frame if it's unset. Caller must set it.
ENV OPENSOCIALNET_AUTH_SECRET=""

ENTRYPOINT ["/usr/local/bin/signaling_server"]
