# Latency & throughput matrix (measured)

**Machine:** Apple M5, macOS 26.5, 10 cores · **Build:** Release (`-O` optimized)
· **Captured:** 2026-06-05 · Reproduce with the commands in [README.md](README.md).

All per-stage numbers are **measured on this machine** by `bench/latency_probe`
(500 synthetic 640×480 frames through the real codecs/RTP/jitter) and
`bench/sfu_benchmark`. Network/capture/render rows are labelled *measured*
(loopback) or *typical* (hardware-dependent, not measured here) so nothing is
overstated.

## Per-stage media latency (640×480 H.264, 48 kHz Opus)

| Stage | mean | p50 | p99 | Source |
|-------|-----:|----:|----:|--------|
| H.264 encode | 0.345 ms | 0.325 ms | 0.575 ms | measured (`latency_probe`) |
| RTP packetize | 0.004 ms | — | 0.010 ms | measured |
| JitterBuffer push+pop (processing) | 0.001 ms | — | 0.010 ms | measured |
| H.264 decode | 0.493 ms | 0.475 ms | 1.125 ms | measured |
| **Video codec+RTP path total** | **0.84 ms** | **0.83 ms** | **1.68 ms** | measured |
| Opus encode | 0.039 ms | 0.030 ms | 0.050 ms | measured |
| Opus decode | 0.010 ms | 0.010 ms | 0.010 ms | measured |

The codec/RTP **processing** cost is **sub-millisecond** (≈0.84 ms typical). The
dominant *deliberate* latency is the jitter-buffer playout delay (configurable,
default **50 ms**), not computation.

## Channel setup (loopback, measured)

| Event | Time | Source |
|-------|-----:|--------|
| ICE connect + first RTP delivered | ~200 ms | `test_rtp_transport` |
| ICE + DTLS-SRTP handshake + first secure RTP | ~155 ms | `test_dtls_srtp` |

One-time at call start (not per-frame). Over the internet add the network RTT;
TURN-relayed paths add the relay hop.

## SFU forwarding throughput (single core)

| Room size | route calls/s | deliveries/s | p50 | p99 | rooms/core |
|-----------|--------------:|-------------:|----:|----:|-----------:|
| 2-party | 14.9 M/s | 14.9 M/s | 0.5 µs | 0.5 µs | ~29,750 |
| 4-party | 13.9 M/s | 41.8 M/s | 0.5 µs | 0.5 µs | ~13,900 |
| 8-party | 10.7 M/s | 75.1 M/s | 0.5 µs | 0.5 µs | ~5,366 |

Per-packet forwarding is **~0.5 µs**; one core sustains thousands of concurrent
8-party rooms. (`bench/sfu_benchmark`.)

## Codec quality

| Metric | Value | Source |
|--------|------:|--------|
| Video round-trip PSNR (640×480) | **49.97 dB** | `codec_roundtrip` |

## Glass-to-glass budget (illustrative)

Built from the **measured** rows above plus *typical* capture/render and network
values (the latter not measured here — they depend on camera, display, and link):

| Component | LAN | Internet | Note |
|-----------|----:|---------:|------|
| Camera capture | ~16 ms | ~16 ms | *typical* (30 fps frame interval) |
| H.264 encode | 0.35 ms | 0.35 ms | measured |
| Packetize + send | <0.1 ms | <0.1 ms | measured |
| Network (one way) | <1 ms | ~10–40 ms | *typical*; TURN relay adds a hop |
| Jitter buffer playout | 50 ms | 50 ms | configured default |
| H.264 decode | 0.5 ms | 0.5 ms | measured |
| Render | ~16 ms | ~16 ms | *typical* (display refresh) |
| **Estimated total** | **~83 ms** | **~95–125 ms** | within the <150 ms target |

> Honest scope: the sub-millisecond compute and µs-scale SFU forwarding are
> measured; the capture/render/network rows are standard estimates, so the
> glass-to-glass total is a **budget**, not a captured end-to-end measurement.
> A true glass-to-glass capture needs instrumented camera→display hardware.
