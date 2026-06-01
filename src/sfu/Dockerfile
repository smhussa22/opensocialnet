# SFU image — libdatachannel + gRPC SfuControl server.
# Multi-stage: builder compiles the C++; runtime carries only the binary,
# libdatachannel.so, and runtime libs from apt.
#
# build (from repo root):
#   docker build -f src/sfu/Dockerfile -t opensocialnet-sfu .
# run:
#   docker run --rm -p 50051:50051 -p 50000-60000:50000-60000/udp opensocialnet-sfu

# ---------- builder ----------
FROM ubuntu:24.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        git \
        pkg-config \
        ca-certificates \
        libssl-dev \
        libprotobuf-dev \
        protobuf-compiler \
        libgrpc++-dev \
        protobuf-compiler-grpc \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /workspace

# the SFU's sfu_control.proto imports signaling.proto via
# --proto_path=../signaling_server/proto, so the signaling.proto file
# must sit at that relative location during the build.
COPY src/sfu/ ./src/sfu/
COPY src/signaling_server/proto/signaling.proto ./src/signaling_server/proto/

WORKDIR /workspace/src/sfu

# libdatachannel is fetched via FetchContent (~3 min first build,
# cached on subsequent Docker layer rebuilds via cmake's _deps).
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build --parallel "$(nproc)" --target sfu

# ---------- runtime ----------
FROM ubuntu:24.04 AS runtime

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates \
        libssl3 \
        libprotobuf32t64 \
        libgrpc++1.51t64 \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /workspace/src/sfu/build/sfu /usr/local/bin/sfu
COPY --from=builder /workspace/src/sfu/build/_deps/libdatachannel-build/libdatachannel.so* /usr/local/lib/
RUN ldconfig

# 50051: gRPC SfuControl (signaling_server connects here)
# 50000-60000/udp: WebRTC media (browsers connect here via ICE)
EXPOSE 50051
EXPOSE 50000-60000/udp

ENTRYPOINT ["/usr/local/bin/sfu"]
