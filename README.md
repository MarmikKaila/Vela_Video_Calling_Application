# video-call-app

![CI](https://github.com/MarmikKaila/Video_Calling_Cpp/actions/workflows/ci.yml/badge.svg)

Low-latency, multi-party video calling in modern C++ (C++20), built on a
**custom RTP/SFU stack** rather than libwebrtc — the protocol work is the point.

- 1:1 and up to 8-party calls via a custom Selective Forwarding Unit (SFU)
- Target glass-to-glass latency &lt; 150 ms
- Adaptive bitrate driven by RTCP feedback (REMB)
- Cross-platform capture behind one interface (macOS / Linux / Windows)
- H.264 (FFmpeg/x264) video, Opus audio

See **[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)** for diagrams, the threading
model, and design decisions.

## Status

All modules implemented and tested together (**9/9 unit tests green** on macOS):

| Module | Phase | What it does | Verified by |
|--------|-------|--------------|-------------|
| `src/common`   | 0 | Frozen contracts (Result, frames, FramePool, clock) | `contracts_selfcheck` |
| `src/capture`  | 1 | AVFoundation capture (+ V4L2/DShow stubs), `capture_dump` | live camera, `capture_tests` |
| `src/codec`    | 2 | H.264 (FFmpeg) + Opus encode/decode | `codec_roundtrip` (PSNR) |
| `src/network`  | 3 | RTP, FU-A, thread-safe JitterBuffer | `rtp_handler`, `jitter_buffer` |
| `src/signaling`| 3 | uWebSockets server + client + libjuice STUN/ICE | `test_signaling` |
| `src/media`    | 4 | Send/receive pipeline + AVSync | `media_tests` (end-to-end loopback) |
| `src/sfu`      | 5 | True SFU forwarding + REMB ABR | `sfu_tests` |
| `src/ui`       | 6 | Qt6 grid + OpenGL `VideoWidget` | `ui_yuv_test` (GL render needs a display) |
| `src/metrics`  | 7 | spdlog logging + latency/loss/CPU metrics | `metrics_tests` |
| `bench`        | 7 | `sfu_benchmark` — forwarding throughput, rooms/core | — |

Benchmark (Apple M-series, single core): **~5,374 eight-party rooms/core** of
encoded-RTP forwarding (see [docs/ARCHITECTURE.md §9](docs/ARCHITECTURE.md)).

## Architecture (data flow)

```
 Camera ─▶ CaptureDevice ─▶ VideoEncoder ─▶ RTPHandler ─▶┐
 Mic    ─▶ AudioCapture  ─▶ AudioEncoder ─▶ RTPHandler ─▶│ DTLS/SRTP ─▶ network
                                                          │
   ┌──────────────────────────────────────────────────── network ◀── SFU (forwards
   ▼                                                                    encoded RTP,
 JitterBuffer ─▶ VideoDecoder ─▶ AVSync ─▶ VideoWidget (Qt/OpenGL)      never decodes)
```

Everything crosses module boundaries through the header-only contracts in
`src/common`: `Result<T,E>`/`Status` (no exceptions in hot paths), `VideoFrame`
/ `AudioFrame` / `EncodedFrame` (pool-backed, zero-copy shareable), `FramePool`
(no malloc in the media loop), and `MediaClock` (monotonic + NTP for AVSync).

## Prerequisites

**macOS dev host** uses Homebrew for the heavy/common deps + vcpkg for the niche
RTC libs (building FFmpeg/Qt from source via vcpkg is too slow here):

```sh
brew install cmake pkg-config ffmpeg opus qt spdlog googletest nlohmann-json srtp
git clone https://github.com/microsoft/vcpkg ~/vcpkg && ~/vcpkg/bootstrap-vcpkg.sh
(cd /tmp && ~/vcpkg/vcpkg install libjuice uwebsockets)   # classic mode (no manifest here)
```

**Linux** (and CI) use system packages — see [.github/workflows/ci.yml](.github/workflows/ci.yml).

> Neither V4L2 nor DirectShow exist on macOS; the local capture backend is
> AVFoundation (`MacCapture`). Linux (V4L2) and Windows (DirectShow) backends
> share the same `CaptureDevice` interface and are built on their own hosts/CI.

## Build

```sh
# Point CMake at the deps (macOS):
export CMAKE_PREFIX_PATH="$(brew --prefix qt):$(brew --prefix googletest):$(brew --prefix spdlog):$(brew --prefix nlohmann-json):$HOME/vcpkg/installed/arm64-osx:$(brew --prefix)"
export PKG_CONFIG_PATH="$(brew --prefix)/lib/pkgconfig:$(brew --prefix srtp)/lib/pkgconfig:$HOME/vcpkg/installed/arm64-osx/lib/pkgconfig"

cmake -S . -B build \
  -DVC_BUILD_CAPTURE=ON -DVC_BUILD_CODEC=ON -DVC_BUILD_NETWORK=ON \
  -DVC_BUILD_SIGNALING=ON -DVC_BUILD_UI=ON -DVC_BUILD_SFU=ON \
  -DVC_BUILD_MEDIA=ON -DVC_BUILD_METRICS=ON -DVC_BUILD_BENCH=ON -DVC_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

Every module is behind a `VC_BUILD_<MODULE>` flag (all default OFF except tests),
so you can build any subset. Demos: `build/src/ui/ui_demo`,
`build/src/ui/loopback_call`, `build/signaling-server/signaling_server`,
`build/bench/sfu_benchmark`, `build/src/capture/capture_dump`.

`loopback_call` is the closest thing to a real call on one machine: your live
camera rendered raw on the left, and the *same* feed after a full
encode → RTP → JitterBuffer → decode round trip on the right — the gap between
the two tiles is the pipeline's glass-to-glass latency. On macOS, grant your
terminal Camera permission (System Settings → Privacy & Security → Camera).

## Layout

| Path                | Phase | Contents                                        |
|---------------------|-------|-------------------------------------------------|
| `src/common`        | 0     | Shared contracts (this is the frozen seam)      |
| `src/capture`       | 1     | `CaptureDevice` + Mac/Linux/Windows backends    |
| `src/codec`         | 2     | H.264 (FFmpeg) + Opus encode/decode             |
| `src/network`       | 3     | RTP, JitterBuffer, signaling client, STUN/ICE   |
| `src/sfu`           | 5     | SFU server, Room, Participant, REMB/ABR         |
| `src/media`         | 4     | MediaPipeline wiring + AVSync                   |
| `src/ui`            | 6     | Qt6 client (OpenGL video grid)                  |
| `src/metrics`       | 7     | spdlog logging + latency/loss/CPU metrics       |
| `signaling-server`  | 3     | Standalone uWebSockets signaling server         |
| `bench`             | 7     | `sfu_benchmark` (forwarding throughput)         |
| `docs`              | 7     | Architecture documentation                      |
