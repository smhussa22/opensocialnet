OpenSocialNet is a from-scratch real-time chat + voice + video desktop app — a Discord-style native client written in C++ where the entire audio/video pipeline (custom UDP packet format, jitter buffer, packet-loss concealment, threading, encoder/decoder wrappers, server-side UDP relay) is implemented by hand instead of delegating to WebRTC.

An earlier browser + libdatachannel-SFU experiment is archived on the `archive/webrtc-sfu` branch as a credential snapshot; it is no longer the deployment target. master is built around the native client.

## Layout

- `src/native_client/` — desktop GUI (SDL3 window + Dear ImGui docking branch). Entry point of the product.
- `src/network/` — UDP transport: socket wrapper, sender/receiver, custom `Packet` header, jitter buffer, `JitterStats`, lock-free `SpscQueue`, sender-side `LossSim`. Loopback exe `network` exercises the whole pipeline on a single machine.
- `src/audio_engine/` — Opus capture/encode/decode wrappers + SDL3 audio I/O.
- `src/video_engine/` — V4L2 capture, x264 encode, FFmpeg decode, SDL3 render. Wiring into the native client is Phase 4.
- `src/signaling_server/` — WebSocket gateway: Protobuf envelopes (`Hello`/`Heartbeat`/`Ready`/`SendMessage`/`FetchHistory`/`JoinVoice`/`VoicePeerJoined`/…), HMAC auth, Scylla chat persistence, Kafka pub/sub fan-out.
- `src/relay/` — UDP relay that fans `Packet`s out per voice room. Not written yet (Phase 2).
- `docker-compose.yml` / `compose.prod.yml` — runtime data services (Kafka, ZooKeeper, Scylla) used by `signaling_server`.

## Build dependencies

| Dep | How to get it |
|---|---|
| build-essential, cmake (>=3.24), pkg-config, git | `apt install` |
| libx264-dev | `apt install` |
| libavcodec-dev, libavutil-dev, libswscale-dev | `apt install` |
| libopus-dev | `apt install` |
| libv4l-dev | `apt install` |
| libssl-dev | `apt install` |
| SDL3 | fetched by CMake (`FetchContent`) |
| Dear ImGui (docking branch) | fetched by CMake (`FetchContent`) — used by `native_client` |
| Protobuf, librdkafka, uWebSockets | `apt install` (only needed for `signaling_server`) |

The full apt command list lives in the `Dockerfile` (treat it as executable documentation — anything missing from your host shows up as a CMake error).

## Quick start

### Option A: Docker (recommended for first build)

Builds a reproducible Ubuntu 24.04 image with every system dep preinstalled. Source is mounted from the host, so edits stay on your machine.

```sh
docker build -t opensocialnet-build .
docker run --rm -it -v "$PWD:/workspace" opensocialnet-build bash
# inside the container:
mkdir -p src/video_engine/build && cd src/video_engine/build
cmake .. && cmake --build . -j
```

Camera/audio passthrough into the container is host-specific (`--device /dev/video0 --group-add video` on Linux). For most local dev it's easier to run binaries directly on the host.

### Option B: Native (Ubuntu 24.04 / WSL2)

```sh
# install system deps (subset of the Dockerfile)
sudo apt install -y build-essential cmake pkg-config git \
    libx264-dev libavcodec-dev libavutil-dev libswscale-dev \
    libopus-dev libv4l-dev libssl-dev v4l-utils

# build video_engine (SDL3 + libdatachannel are fetched + built automatically; first build is slow)
mkdir -p src/video_engine/build && cd src/video_engine/build
cmake .. && cmake --build . -j
```

Binaries land in `src/video_engine/build/`:

- `video_sender [host] [port]` — capture, encode, send. Defaults to `127.0.0.1:9001`.
- `video_receiver [port]` — receive, decode, render in an SDL window. Defaults to port `9001`.
- `audio_video_client` — both streams in one process (audio on `9000`, video on `9001`), self-loopback.
- `test_video_loopback` — capture → encode → UDP → decode → render in one process. Useful for sanity checks.
- `test_capture_encode` — dumps a few seconds of camera input to `output.h264` (smoke-test capture).
- `test_probe_camera` — enumerates V4L2 formats and supported (size, fps) tuples for `/dev/video0`.

### Camera on WSL2

`/dev/video*` doesn't exist by default on WSL2. To pass a host webcam through:

1. Install [usbipd-win](https://github.com/dorssel/usbipd-win) on Windows.
2. `usbipd list` (Windows) → find your camera's busid.
3. `usbipd bind --busid <id>` then `usbipd attach --wsl --busid <id>`.
4. WSL2 also needs UVC drivers in its kernel — stock WSL kernels are missing them. Build a custom WSL kernel with `CONFIG_USB_VIDEO_CLASS=y` (and the rest of `CONFIG_MEDIA_*`), drop the resulting `bzImage` somewhere on Windows, and point `%USERPROFILE%/.wslconfig` at it:

   ```ini
   [wsl2]
   kernel=C:\\Users\\<you>\\bzImage-wsl-custom
   ```
5. Restart WSL (`wsl --shutdown`), re-attach the camera, and `/dev/video0` should appear.
6. Open device permissions for your user: `sudo chmod a+rw /dev/video0` (or add yourself to the `video` group).

## Signaling server

Data services come up via Compose:

```sh
docker compose up -d zookeeper kafka scylla
```

Then build and run `src/signaling_server/`. See that directory's own `build.sh` / `scripts/` for vcpkg + Cassandra driver setup (the driver isn't in apt).
