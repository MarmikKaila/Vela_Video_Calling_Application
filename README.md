# Vela



**Vela** — low-latency, multi-party video calling in modern C++ (C++20), built on
a **custom RTP/SFU stack** rather than libwebrtc — the protocol work is the point.
(Plus a zero-install browser client so anyone can join from a link.)

- 1:1 and up to 8-party calls via a custom Selective Forwarding Unit (SFU)
- Real cross-laptop calls: camera + mic over a custom **ICE/RTP transport**
  (`RtpTransport` over libjuice), demuxed by SSRC into one tile per participant
- Target glass-to-glass latency &lt; 150 ms; adaptive bitrate via RTCP REMB
- H.264 (FFmpeg/x264) video, Opus audio; AVFoundation capture + playback on macOS
- Cross-platform capture behind one interface (macOS / Linux / Windows)

See **[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)** for diagrams, the threading
model, and design decisions.

### Two ways to call

| | Native app (`group_call` + `sfu_server`) | Browser app ([web/](web/)) |
|---|---|---|
| **What it is** | The from-scratch C++ RTP/SFU stack | Zero-install WebRTC mesh |
| **Other machine needs** | Build the project (or a bundled `.app`) | Just a browser — open a link |
| **Media** | Custom: AVFoundation + FFmpeg + libjuice ICE + custom RTP/SFU | Browser's built-in WebRTC |
| **Best for** | Showing the protocol work; up to 8 parties via SFU | The easy "send a link, click to join" demo; ~2–4 on a LAN |

The native stack is the engineering centerpiece; the browser app exists so anyone
can join a call instantly without building anything.

## Status

All modules implemented and tested together (**10/10 unit tests green** on macOS):

| Module | What it does | Verified by |
|--------|--------------|-------------|
| `src/common`   | Frozen contracts (Result, frames, FramePool, clock) | `contracts_selfcheck` |
| `src/capture`  | AVFoundation capture (+ V4L2/DShow stubs), `capture_dump` | live camera, `capture_tests` |
| `src/codec`    | H.264 (FFmpeg) + Opus encode/decode | `codec_roundtrip` (PSNR) |
| `src/network`  | RTP, FU-A, thread-safe JitterBuffer | `rtp_handler`, `jitter_buffer` |
| `src/signaling`| WS signaling client + libjuice ICE + `RtpTransport` | `test_signaling`, `test_rtp_transport` |
| `src/audio`    | Mic capture + speaker playback (AVAudioEngine) | `audio_loopback` (hear yourself) |
| `src/media`    | Send/receive pipeline, AVSync, `ReceiveRouter` (SSRC demux) | `media_tests` (end-to-end loopback) |
| `src/sfu`      | True SFU forwarding + REMB ABR | `sfu_tests` |
| `sfu-server`   | Integrated signaling + SFU media hub | `sfu_smoketest` (2 clients, RTP forwarded) |
| `src/ui`       | Qt6 grid + OpenGL `VideoWidget`; `group_call` | `ui_yuv_test` (GL render needs a display) |
| `src/metrics`  | spdlog logging + latency/loss/CPU metrics | `metrics_tests` |
| `web/`         | Zero-install browser WebRTC mesh client | signaling relay verified headlessly |

Benchmark (Apple M-series, single core): **~5,374 eight-party rooms/core** of
encoded-RTP forwarding (see [docs/ARCHITECTURE.md §9](docs/ARCHITECTURE.md)).

## Architecture (native stack)

A real call: each client opens **one ICE channel** to the SFU server, sends its
camera+mic up it, and receives every other participant's RTP back down it.

```
        ┌──────────────────── one client ─────────────────────┐
  Cam ─▶ Capture ─▶ VideoEncoder ─┐                            │
  Mic ─▶ AudioCap ─▶ AudioEncoder ┤                            │
                                  ▼                            │
                             RTPHandler ─▶ RtpPacket ─┐        │
                                                      ▼        │
                                              RtpTransport ────┼──┐ ICE
   Tiles ◀─ ReceiveRouter ◀─ RtpPacket ◀──── RtpTransport ◀───┼──┘ (libjuice/UDP)
   (1 per      │ (per-SSRC: JitterBuffer ─▶ Decoder)          │
    sender)    └─ audio ─▶ speaker (AudioPlayback)            │
        └─────────────────────────────────────────────────────┘
                                  │  WebSocket signaling (offer/answer/ICE)
                                  ▼
                  ┌────────────────────────────────────┐
                  │  sfu_server: signaling + SFUServer  │
                  │  one RtpTransport per client;        │
                  │  forwards encoded RTP, never decodes │
                  └────────────────────────────────────┘
```

`RtpTransport` (in `src/signaling`) is the seam that turns the single-machine
pipeline into a networked one: `send(RtpPacket)` serializes to the wire and
inbound bytes parse back to packets, all over a libjuice ICE/UDP channel.
`ReceiveRouter` (in `src/media`) demultiplexes the merged inbound stream by SSRC
(payload type 96 = H.264, 111 = Opus) into a per-sender JitterBuffer + decoder.

Everything crosses module boundaries through the header-only contracts in
`src/common`: `Result<T,E>`/`Status` (no exceptions in hot paths), `VideoFrame`
/ `AudioFrame` / `EncodedFrame` (pool-backed, zero-copy shareable), `FramePool`
(no malloc in the media loop), and `MediaClock` (monotonic + NTP for AVSync).
The **browser** path ([web/](web/)) is independent: it uses the browser's own
WebRTC in a mesh, with a small Node server for HTTPS + signaling relay.

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
  -DVC_BUILD_MEDIA=ON -DVC_BUILD_AUDIO=ON -DVC_BUILD_METRICS=ON \
  -DVC_BUILD_BENCH=ON -DVC_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

Every module is behind a `VC_BUILD_<MODULE>` flag (all default OFF except tests),
so you can build any subset. Demos: `build/src/ui/ui_demo`,
`build/src/ui/loopback_call`, `build/src/ui/group_call`,
`build/sfu-server/sfu_server`, `build/src/audio/audio_loopback`,
`build/signaling-server/signaling_server`, `build/bench/sfu_benchmark`,
`build/src/capture/capture_dump`.

`loopback_call` is the closest thing to a real call on one machine: your live
camera rendered raw on the left, and the *same* feed after a full
encode → RTP → JitterBuffer → decode round trip on the right — the gap between
the two tiles is the pipeline's glass-to-glass latency. On macOS, grant your
terminal Camera permission (System Settings → Privacy & Security → Camera).

## Easiest: browser meeting via an invite link (no install)

Want the "send a link, click, you're in" experience with nothing to install on
the other machine? See **[web/](web/)** — a zero-install browser client (WebRTC
mesh) for laptops on the same Wi-Fi. The host runs `node server.js` and shares a
`https://<lan-ip>:8443/?room=demo` link; everyone else just opens it in a browser
and clicks Join. This uses the browser's built-in WebRTC (so it runs anywhere,
Intel or Apple Silicon); the native `group_call` below is the from-scratch
raw-RTP/SFU implementation.

## Run a real call with the native app (two laptops, same Wi-Fi)

`group_call` is an actual multi-party meeting: each client opens one ICE channel
to an `sfu_server`, sends its camera + microphone up it, and receives every other
participant's audio/video back down it (the SFU forwards encoded RTP verbatim,
demuxed by SSRC into one tile per remote video).

```sh
# Laptop A — run the hub, then join:
scripts/run_call.sh server                 # prints the LAN IP to dial
scripts/run_call.sh client <A-ip> demo

# Laptop B — join the same room:
scripts/run_call.sh client <A-ip> demo
```

Grant the terminal **Camera and Microphone** permission on both machines (System
Settings → Privacy & Security). **Use headphones** — there is no echo
cancellation. A third laptop can join `demo` and a tile appears within ~2 s (the
next GOP keyframe). Audio is 48 kHz Opus; video is H.264. Currently LAN-only
(ICE host candidates, no STUN); a newcomer relies on the encoder's 2 s keyframe
interval rather than an explicit PLI.

To verify the network path without cameras: `build/src/audio/audio_loopback`
(hear yourself) and the `sfu_smoketest` / `test_rtp_transport` unit tests.

## Layout

| Path                | Phase | Contents                                        |
|---------------------|-------|-------------------------------------------------|
| `src/common`        | 0     | Shared contracts (this is the frozen seam)      |
| `src/capture`       | 1     | `CaptureDevice` + Mac/Linux/Windows backends    |
| `src/codec`         | 2     | H.264 (FFmpeg) + Opus encode/decode             |
| `src/network`       | 3     | RTP, JitterBuffer, signaling client, STUN/ICE   |
| `src/sfu`           | 5     | SFU server, Room, Participant, REMB/ABR         |
| `src/media`         | 4     | MediaPipeline, AVSync, ReceiveRouter (SSRC demux)|
| `src/audio`         | 8     | Mic capture + speaker playback (AVAudioEngine)  |
| `src/ui`            | 6     | Qt6 client (OpenGL grid), `group_call`          |
| `src/metrics`       | 7     | spdlog logging + latency/loss/CPU metrics       |
| `src/signaling`     | 3     | WS signaling client, libjuice ICE, `RtpTransport`|
| `signaling-server`  | 3     | Standalone uWebSockets relay server             |
| `sfu-server`        | 8     | Integrated signaling + SFU media hub (`sfu_server`)|
| `web`               | 8     | Zero-install browser WebRTC mesh client         |
| `scripts`           | 8     | `run_call.sh` launcher                          |
| `bench`             | 7     | `sfu_benchmark` (forwarding throughput)         |
| `docs`              | 7     | Architecture documentation                      |

## Roadmap / known limits

The networked call is an honest MVP. Deferred (and where they'd go):
- **DTLS-SRTP encryption** on the native transport (currently plain RTP over ICE
  — trusted-LAN use).
- **Internet calls** — add STUN/TURN + a reachable host (currently LAN-only).
- **Explicit PLI** keyframe-on-join (a newcomer currently waits up to one 2 s GOP).
- **Lip-sync** wiring of `AVSync`'s RTCP mapping into playout, and **echo
  cancellation** (use headphones for now).
