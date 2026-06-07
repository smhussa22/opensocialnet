# relay image — pure-POSIX UDP fan-out for the native voice/video path.
#
# Multi-stage: an ubuntu builder with build-essential + cmake produces a
# tiny static-ish binary, then the runtime layer carries only that binary
# plus the C++ runtime libs (~50 MB image vs ~500 MB if we shipped the
# builder layer).
#
# Build (from repo root):
#   docker build -f src/relay/Dockerfile -t opensocialnet-relay .
# Run (host network so UDP isn't NATed by docker's bridge):
#   docker run --rm --network host -e RELAY_PORT=50100 opensocialnet-relay
#
# The relay needs Packet.hh from src/network/include for the on-the-wire
# format — that path is mounted at the matching relative location inside
# the build context so target_include_directories(../network/include) in
# the CMakeLists resolves.

# ---------- builder ----------
FROM ubuntu:24.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /workspace

# Only the headers the relay actually compiles against. Keeping the COPY
# narrow means a touch in audio_engine/ or sfu/ doesn't bust this layer's
# cache.
COPY src/network/include/ ./src/network/include/
COPY src/relay/ ./src/relay/

WORKDIR /workspace/src/relay
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build --parallel 4 --target relay

# ---------- runtime ----------
FROM ubuntu:24.04 AS runtime

ENV DEBIAN_FRONTEND=noninteractive

# C++ runtime + libgcc. No other libs needed — the relay is pure POSIX.
RUN apt-get update && apt-get install -y --no-install-recommends \
        libstdc++6 \
        libgcc-s1 \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /workspace/src/relay/build/relay /usr/local/bin/relay

# 50100/udp: media fan-out (native clients connect here).
EXPOSE 50100/udp
ENV RELAY_PORT=50100

ENTRYPOINT ["/usr/local/bin/relay"]
