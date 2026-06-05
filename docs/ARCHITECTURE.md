# Architecture

A low-latency, multi-party video calling stack in C++20 built on a **custom
RTP/SFU pipeline** (not libwebrtc). This document explains how the pieces fit,
the threading model, and the key design decisions.

## 1. System overview

Each client opens **one ICE channel** (libjuice/UDP, via `RtpTransport`) to the
SFU server, sends its own audio+video RTP up it, and receives every other
participant's RTP back down it — demultiplexed by SSRC into one decode path and
tile per remote sender.

```
   ┌─────────────────────────────── one client ───────────────────────────────┐
   │  Camera ─▶ CaptureDevice ─▶ VideoEncoder ─┐                                │
   │  Mic    ─▶ AudioCapture   ─▶ AudioEncoder ─┤                               │
   │                                            ▼                               │
   │                                       RTPHandler ─▶ RtpPacket ─┐           │
   │                                                                ▼           │
   │   tiles ◀─ ReceiveRouter ◀── RtpPacket ◀──────────────── RtpTransport ─────┼──┐ ICE
   │   speaker ◀─ (per-SSRC: JitterBuffer ─▶ Decoder)         RtpTransport ◀────┼──┘ /UDP
   └────────────────────────────────────────────────────────────────────────────┘
                                          │  WebSocket signaling
                                          ▼  (offer/answer + ICE candidates)
                       ┌──────────────────────────────────┐
                       │  sfu_server (multi-party)          │
                       │  signaling + SFUServer +           │
                       │  one RtpTransport per client.      │
                       │  Forwards encoded RTP to others    │
                       │  WITHOUT decoding (true SFU).       │
                       └──────────────────────────────────┘
```

Single-machine variants exercise the same pipeline without a network:
`loopback_call` (camera → encode → RTP → jitter → decode → render, in-process)
and `audio_loopback` (mic → Opus → speaker). A separate **browser** client
([web/](../web/)) offers a zero-install WebRTC-mesh call — see §11.

> Transport: RTP over a libjuice ICE/UDP channel with **STUN/TURN** for
> cross-network NAT traversal, secured end-to-end with **DTLS-SRTP** (the SFU
> terminates it per participant). See §11.

## 2. Module dependency graph

Everything crosses module boundaries through the header-only **contracts** in
`src/common`. Arrows mean "links against".

```
                         ┌────────────┐
                         │ vc_common  │  Result/Status, VideoFrame, AudioFrame,
                         │ (contracts)│  EncodedFrame, FramePool, MediaClock, Types
                         └─────▲──────┘
     ┌──────────┬──────────┬──┼────────┬────────────┬──────────┬──────────┐
     │          │          │  │        │            │          │          │
 vc_capture vc_codec  vc_network vc_signaling   vc_audio    vc_ui    vc_metrics
     │          │          │     (RtpTransport)     │          │
     └────┬─────┴────┬─────┘         │              │          │
          ▼          ▼               │              │          │
       vc_media ◀────┴───────────────┴──────────────┘          │
   (pipeline, AVSync, ReceiveRouter)                            │
          │                                                     ▼
       vc_sfu ──▶ sfu_server (signaling+SFU+RtpTransport)   sfu_benchmark
   (forwarding+REMB)   group_call (vc_ui+media+audio+signaling)
```

- **vc_common** — no dependencies; pure value types and the error model.
- **vc_capture / vc_codec / vc_network / vc_audio / vc_ui** — independent; each
  depends only on `vc_common` (+ its external lib: FFmpeg, Opus, AVFoundation,
  Qt6).
- **vc_signaling** — WebSocket signaling client, libjuice ICE (`NATTraversal`),
  and **`RtpTransport`** (ICE ↔ `RtpPacket` bridge).
- **vc_media** — wires capture+codec+network into the send/receive pipeline; adds
  AVSync and **`ReceiveRouter`** (per-SSRC inbound demux).
- **vc_sfu** — depends on `vc_network` for the `RtpPacket` wire type only.
- **sfu_server** — signaling + `SFUServer` + one `RtpTransport` per client.
- **group_call** — the real client (signaling + transport + capture + pipeline + UI).

The top-level `CMakeLists.txt` discovers each module dir automatically and each
module is behind a `VC_BUILD_<MODULE>` flag, so you can build any subset.

## 3. The contracts (`src/common`)

| Type | Responsibility |
|------|----------------|
| `Result<T,E>` / `Status` | Exception-free error propagation; `[[nodiscard]]`. No throwing in hot paths. |
| `VideoFrame` / `AudioFrame` | Pool-backed, zero-copy-shareable raw media (I420 / interleaved PCM). |
| `EncodedFrame` | Compressed access unit; shared payload so the SFU fans out without copying. |
| `FramePool` | Pre-allocated buffer pool → **no malloc in the per-frame path**; `acquire()` returns null under pressure (drop, don't block). |
| `MediaClock` | Monotonic time + RTP (90k/48k) and NTP mapping for AVSync. |

## 4. Send / receive data flow (`vc_media`)

```
SendPipeline (capture thread):
    VideoFrame ─▶ VideoEncoder.encode ─▶ EncodedFrame ─▶ RTPHandler.packetize
                                                       └─▶ [RtpPacket...] ─▶ PacketSink

ReceivePipeline (recv thread → render thread):
    RtpPacket ─push─▶ JitterBuffer ──pop(now)──▶ EncodedFrame ─▶ VideoDecoder.decode ─▶ VideoFrame ─▶ sink
                      (reorder/dedup/gap,        (render thread only)
                       50 ms target delay)
```

The two halves are dependency-injected (encoders/decoders passed in), so the
whole path is unit-tested with **synthetic frames and no camera or socket** — see
`media_tests` (`EndToEndLoopbackDecodesFrame`).

## 5. Threading model

| Thread | Work | Synchronisation |
|--------|------|-----------------|
| Capture | device callback → `SendPipeline::pushVideoFrame` (encode+packetize) | runs to completion, hands packets to transport |
| Receive | `ReceivePipeline::pushPacket` → `JitterBuffer::push` | JitterBuffer is mutex-guarded |
| Render  | `ReceivePipeline::tick` → pop+decode+display | sole caller of decoders/sinks |
| SFU loop | `SFUServer::routePacket` fan-out | `Room` mutex per call |

Rule: keep capture-thread work short (encode then enqueue); never block it. The
`FramePool` returning null is the back-pressure signal — drop the frame.

## 6. JitterBuffer (`vc_network`)

Handles the four hard cases explicitly:

- **Out-of-order** — packets keyed by RTP timestamp into frames; within a frame,
  reassembled in wrap-aware sequence order (`minSeq..maxSeq`).
- **Duplicates** — dropped by sequence number.
- **Late arrivals** — packets for an already-emitted timestamp are discarded.
- **Gaps** — missing sequence runs reported via `gaps()` for NACK/concealment.

A frame is released only once (a) its target delay (50 ms) has elapsed and (b)
it is complete (marker bit + contiguous sequence run). Incomplete video frames
are dropped after the delay rather than stalling playout. 16-bit sequence and
32-bit timestamp wraparound are handled throughout.

## 7. SFU (`vc_sfu`)

A **true** SFU: it forwards encoded `RtpPacket`s verbatim and never decodes.

- `Room::forward(sender, pkt)` delivers to every participant except the sender.
- **Keyframe-on-join (PLI):** a new participant can't decode mid-GOP, so on join
  the room asks each existing video sender (via a callback) for an IDR.
- **Adaptive bitrate (REMB):** each receiver reports an estimate; a sender's
  target bitrate is the **min over its receivers**, fed back to its encoder via
  `VideoEncoder::setBitrate`.

Capacity is single-core forwarding throughput; rooms parallelise across cores.
See `sfu_benchmark` and §9.

## 8. AVSync (`vc_media`)

Audio and video have independent RTP timestamp bases. RTCP Sender Reports give,
per stream, an `(rtpTimestamp ↔ NTP wall instant)` anchor. AVSync maps any RTP
timestamp to wall-clock time:

```
wall(ts) = ntpAnchor + (int32)(ts − rtpAnchor) / clockRateHz
```

Mapping both streams onto the shared wall clock lets the renderer release the
audio sample and the video frame for the same instant together — lip-sync. Tested
to <1 ms skew (`AVSync.AlignsAudioAndVideoToSameWallInstant`).

## 9. Metrics & benchmark (`vc_metrics`, `bench`)

- `LatencyHistogram` — bucketed, O(1) record, p50/p90/p99.
- `LossEstimator` — RFC 3550-style loss% from RTP sequence numbers (wrap-aware).
- `CpuSampler` — process CPU-seconds per wall-second (100% == one core).
- Logging via spdlog behind a `VC_INFO/WARN/ERROR` facade.

Representative `sfu_benchmark` result (Apple M-series, single core, Release):

```
n=2 | route=12.5M/s  deliveries=12.5M/s  p50=0.5us p99=0.5us | ~24900 rooms/core
n=4 | route=14.1M/s  deliveries=42.3M/s  p50=0.5us p99=0.5us | ~14100 rooms/core
n=8 | route=10.8M/s  deliveries=75.2M/s  p50=0.5us p99=0.5us |  ~5374 rooms/core
```

i.e. one core sustains thousands of concurrent 8-party rooms of pure forwarding;
the real ceiling in production is the network and DTLS/SRTP crypto, not routing.

## 10. Key design decisions

- **Custom RTP/SFU over libwebrtc** — the protocol work (RTP packetization, FU-A,
  jitter buffer, SFU, AVSync) is the point; a multi-GB opaque dependency would
  hide exactly the parts worth demonstrating.
- **Exception-free `Result<T,E>`** — predictable control flow on the media hot
  path; errors are values.
- **Pre-allocated `FramePool`** — bounded memory and zero per-frame allocation;
  drop-on-pressure is the correct real-time behaviour.
- **One capture interface, three backends** — `MacCapture` (AVFoundation),
  `LinuxCapture` (V4L2), `WindowsCapture` (DirectShow) behind `CaptureDevice`.
- **Contracts frozen first, modules built in parallel** — the `src/common`
  headers are the stable seam every module compiles against.

## 11. The networked call (`group_call` + `sfu_server`)

What turns the in-process `loopback_call` into a real cross-machine call:

- **`RtpTransport`** (`src/signaling`) bridges the ICE channel and the RTP model:
  `send(RtpPacket)` → `RtpPacket::serialize()` → `NATTraversal::send()`; inbound
  bytes → `RtpPacket::parse()` → `onPacket`. One instance == one ICE connection.
- **`sfu_server`** embeds the uWebSockets signaling server + `SFUServer` + one
  `RtpTransport` per client. In SFU mode the server is the **ICE answerer**: it
  handles each client's offer itself (rather than relaying it to a peer), then
  `routePacket`s inbound RTP to the room's other members.
- **`ReceiveRouter`** (`src/media`) demuxes the merged inbound stream by SSRC,
  building a `JitterBuffer` + decoder per (sender, kind) — video vs audio by
  payload type (96 = H.264, 111 = Opus). It reaps streams idle > 3 s (the SFU
  sends no peer-left). `pushPacket` runs on the transport thread; `tick()` runs
  on the GUI thread and is the only place decoders run and tiles are created.

**Threading across the network seam.** uWebSockets is single-threaded, while
libjuice fires `RtpTransport` callbacks on its own thread. So in `sfu_server`,
any `ws->send` from a libjuice callback is marshaled onto the loop with
`uWS::Loop::defer`; the high-rate `onPacket` path calls `routePacket` directly
(the SFU/Rooms are mutex-guarded), keeping media off the loop thread.

Verified end-to-end headlessly by `sfu_smoketest` (two clients connect ICE
through a live server; one's RTP is forwarded to the other) and
`test_rtp_transport` (byte-identical RTP over a loopback ICE pair).

**Security (DTLS-SRTP).** After ICE connects, `RtpTransport` runs a DTLS
handshake over the channel (OpenSSL, `src/signaling/DtlsSrtp`) and derives
**SRTP** keys via the `use_srtp` extension; every RTP packet is then encrypted
and authenticated with libsrtp. Each peer's self-signed-certificate fingerprint
is carried in the signaling offer/answer and verified during the handshake, so a
network attacker can neither read nor inject media (no MITM). Encryption is
transparent in `RtpTransport`, so the **SFU terminates DTLS-SRTP per
participant** — it decrypts inbound to read RTP headers for routing, then
re-encrypts per recipient, exactly like a production SFU. `test_dtls_srtp` covers
the handshake, an SRTP round trip, and fingerprint-mismatch rejection.

**Cross-network (STUN/TURN).** `NATConfig` carries a STUN server and TURN relays
(`iceConfigFromEnv` reads `VC_STUN` / `VC_TURN[_USER|_PASS]`), populated into
libjuice. STUN discovers the public address; TURN relays media when a direct path
is blocked, so calls work across arbitrary networks.

**MVP limits:** keyframe-on-join relies on the 2 s GOP rather than an explicit
PLI; audio/video play from independent jitter buffers (AVSync mapping not yet
wired into playout); no echo cancellation.

## 12. The browser client (`web/`)

A separate, zero-install path so anyone can join with only a browser. It uses the
**browser's built-in WebRTC** (capture, codecs, DTLS-SRTP, rendering) in a full
**mesh** — each participant connects directly to every other (good for ~2–4 on a
LAN; an SFU is the scale-up beyond that). `web/server.js` (Node) only (a) serves
the page over **HTTPS** — required for camera/mic access off `localhost`, with a
self-signed cert generated on first run — and (b) relays WebSocket signaling
(`join` / `signal` / `peer-joined` / `peer-left`) between peers in a room. Glare
is avoided by a simple rule: the newer participant always initiates the offer.

This intentionally does **not** reuse the C++ SFU: browsers require WebRTC's
DTLS-SRTP + SDP semantics, which the custom raw-RTP SFU does not implement. The
two clients are independent demonstrations — the C++ stack for the protocol work,
the web app for instant access.
