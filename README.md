# video-call-app

Low-latency, multi-party video calling in modern C++ (C++20), built on a
**custom RTP/SFU stack** rather than libwebrtc — the protocol work is the point.

- 1:1 and up to 8-party calls via a custom Selective Forwarding Unit (SFU)
- Target glass-to-glass latency &lt; 150 ms
- Adaptive bitrate driven by RTCP feedback
- Cross-platform capture behind one interface (macOS / Linux / Windows)
- H.264 (FFmpeg/x264) video, Opus audio

## Status

Foundation laid: shared contracts (`src/common`) + the Phase 1 capture
interface (`src/capture/CaptureDevice.h`) compile and pass `contracts_selfcheck`.
Module implementations land phase by phase.

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

## Prerequisites (macOS dev host)

```sh
brew install cmake
git clone https://github.com/microsoft/vcpkg ~/vcpkg
~/vcpkg/bootstrap-vcpkg.sh
export VCPKG_ROOT=~/vcpkg
```

> Neither V4L2 nor DirectShow exist on macOS; the local capture backend is
> AVFoundation (`MacCapture`). Linux (V4L2) and Windows (DirectShow) backends
> share the same `CaptureDevice` interface and are built on their own hosts/CI.

## Build

```sh
cmake -B build -S . -DVCPKG_MANIFEST_FEATURES="signaling"
cmake --build build
ctest --test-dir build
```

Module switches (default off until a phase lands them):
`-DVC_BUILD_CAPTURE=ON -DVC_BUILD_CODEC=ON -DVC_BUILD_NETWORK=ON`
`-DVC_BUILD_SFU=ON -DVC_BUILD_SIGNALING=ON -DVC_BUILD_UI=ON`.

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
| `signaling-server`  | 3     | Standalone uWebSockets signaling server         |
| `tests`             | all   | gtest unit tests                                |
```
