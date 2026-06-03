// audio_loopback — the audio counterpart of loopback_call.
//
// Captures the microphone, runs each 20 ms frame through the real Opus
// encode -> decode round trip, and plays the result back through the speaker.
// You should hear yourself with a short delay. USE HEADPHONES to avoid the mic
// picking up the speaker (there is no echo cancellation).
//
// Grant the terminal Microphone permission: System Settings > Privacy &
// Security > Microphone. Ctrl-C to quit.

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <thread>

#include "audio/AudioIO.h"
#include "codec/AudioDecoder.h"
#include "codec/AudioEncoder.h"
#include "common/Types.h"

using namespace vc;

namespace {
std::atomic<bool> g_running{true};
void onSigint(int) { g_running = false; }
}  // namespace

int main() {
    std::signal(SIGINT, onSigint);

    AudioFormat fmt;
    fmt.sampleRateHz = 48000;
    fmt.channels = 1;  // mono is the conferencing default

    AudioEncoder enc;
    AudioDecoder dec;
    if (!enc.open(fmt, 32000) || !dec.open(fmt)) {
        std::fprintf(stderr, "Failed to open Opus codec\n");
        return 1;
    }

    auto capture = audio::createAudioCapture();
    auto playback = audio::createAudioPlayback();
    if (!capture || !playback) {
        std::fprintf(stderr, "No audio backend on this platform\n");
        return 1;
    }

    if (!playback->start(fmt)) {
        std::fprintf(stderr, "Failed to start audio output\n");
        return 1;
    }

    std::atomic<uint64_t> frames{0};
    // Encoder/decoder are touched only on the capture thread (the callback runs
    // serially on the AVAudioEngine tap thread), so no extra locking is needed.
    auto onFrame = [&](const AudioFrame& pcm) {
        auto encoded = enc.encode(pcm);
        if (!encoded) return;
        auto decoded = dec.decode(encoded.value());
        if (!decoded) return;
        playback->enqueue(decoded.value());
        frames.fetch_add(1, std::memory_order_relaxed);
    };

    if (!capture->start(fmt, onFrame)) {
        std::fprintf(stderr, "Failed to start microphone (check Mic permission)\n");
        return 1;
    }

    std::printf("Audio loopback running at 48 kHz mono. Speak into the mic "
                "(headphones recommended). Ctrl-C to quit.\n");
    while (g_running.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        std::printf("\r  frames looped: %llu",
                    static_cast<unsigned long long>(frames.load()));
        std::fflush(stdout);
    }

    (void)capture->stop();
    (void)playback->stop();
    std::printf("\nStopped.\n");
    return 0;
}
