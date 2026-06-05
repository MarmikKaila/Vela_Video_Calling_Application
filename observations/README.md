# Observations — measured results

Real measurements captured on this project's binaries, so the performance claims
in the top-level README are backed by numbers rather than assertions.

**Environment:** Apple M5, macOS 26.5, 10 cores, Release build · captured 2026-06-05.

## Files

| File | What |
|------|------|
| [latency_matrix.md](latency_matrix.md) | Curated matrix: per-stage media latency, channel setup, SFU throughput, codec quality, and a glass-to-glass budget |
| [pipeline_latency.txt](pipeline_latency.txt) | Raw `latency_probe` output (encode / packetize / jitter / decode, video + audio) |
| [sfu_forwarding.txt](sfu_forwarding.txt) | Raw `sfu_benchmark` output (forwarding throughput, rooms/core) |
| [setup_and_codec.txt](setup_and_codec.txt) | ICE connect, DTLS-SRTP handshake (loopback), and codec PSNR from the test binaries |

## How to reproduce

Build with benchmarks enabled (`-DVC_BUILD_BENCH=ON`, plus the usual module
flags — see the top-level README), then:

```sh
# Per-stage pipeline latency (N synthetic frames through the real codecs/RTP/jitter)
./build/bench/latency_probe 500

# SFU forwarding throughput (2/4/8-party rooms, single core)
./build/bench/sfu_benchmark

# Channel setup + codec quality (from the test binaries)
./build/src/signaling/test_rtp_transport     # ICE connect + RTP round trip (loopback)
./build/src/signaling/test_dtls_srtp         # ICE + DTLS-SRTP handshake (loopback)
./build/src/codec/test_codec                 # video/audio round-trip PSNR
```

## Headline numbers

- **Video codec+RTP processing:** ~0.84 ms mean (sub-millisecond), p99 ~1.7 ms.
- **SFU forwarding:** ~0.5 µs/packet; **~5,366 eight-party rooms/core**.
- **DTLS-SRTP secure channel setup:** ~155 ms on loopback (one-time, at join).
- **Video round-trip PSNR:** 49.97 dB.

## Honesty note

Per-stage compute and SFU forwarding are **measured**. Capture, render, and
network legs in the glass-to-glass budget are **standard estimates** (they depend
on camera/display/link and aren't measured here), so the end-to-end total is a
budget, not a captured camera-to-display number — see the note in
[latency_matrix.md](latency_matrix.md).
