# Build environment for OpenSocialNet.
# Reproducible Ubuntu image with every system dependency the C++ engines
# need. Source is mounted at /workspace; build artifacts land in build/
# under each engine. Camera/audio devices must be passed in at runtime
# via --device, --group-add video, etc. (host machine concern).
#
# usage:
#   docker build -t opensocialnet-build .
#   docker run --rm -it -v "$PWD:/workspace" opensocialnet-build bash

FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive
ENV TZ=Etc/UTC

# core toolchain + project deps (codecs, V4L2, SDL3 build deps, signaling
# server deps). SDL3 + libdatachannel are pulled by CMake FetchContent, so
# we only install their build prerequisites here, not the libs themselves.
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        git \
        pkg-config \
        ca-certificates \
        ninja-build \
        # video codec deps
        libx264-dev \
        libavcodec-dev \
        libavutil-dev \
        libswscale-dev \
        # audio codec
        libopus-dev \
        # V4L2 (camera capture)
        libv4l-dev \
        v4l-utils \
        # SDL3 build prerequisites (X11, ALSA, PulseAudio dev headers)
        libx11-dev \
        libxext-dev \
        libxrandr-dev \
        libxcursor-dev \
        libxi-dev \
        libxss-dev \
        libgl1-mesa-dev \
        libasound2-dev \
        libpulse-dev \
        # libdatachannel build prerequisites (TLS + SCTP build via FetchContent)
        libssl-dev \
        # signaling_server deps
        protobuf-compiler \
        libprotobuf-dev \
        librdkafka-dev \
        libuwebsockets-dev \
        # debug tools
        gdb \
        vim \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /workspace

CMD ["/bin/bash"]
