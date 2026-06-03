#pragma once

// AudioIO — microphone capture and speaker playback device interfaces.
//
// These mirror the video CaptureDevice contract (src/capture/CaptureDevice.h)
// but for audio. Both speak the project's AudioFrame: interleaved 16-bit PCM at
// 48 kHz in 20 ms blocks (960 samples/channel), which is exactly what the Opus
// AudioEncoder/AudioDecoder consume and produce.
//
// AudioCapture delivers frames on the platform audio thread; the callback must
// not block. AudioPlayback is fed decoded frames from the render/playout thread
// and buffers them internally for the OS output callback to pull.
//
// The macOS implementation (MacAudio.mm) uses AVAudioEngine. Other platforms
// currently return null factories (audio is a macOS feature for now).

#include <functional>
#include <memory>

#include "common/AudioFrame.h"
#include "common/Error.h"
#include "common/Types.h"

namespace vc::audio {

// Microphone capture. Delivers fixed 20 ms AudioFrames at 48 kHz with a 48 kHz
// rtpTimestamp assigned at capture (see VideoFrame's same contract).
class AudioCapture {
public:
    using FrameCallback = std::function<void(const AudioFrame&)>;

    virtual ~AudioCapture() = default;

    // Open the default input device for `format` (48 kHz; 1 or 2 channels) and
    // begin delivering 20 ms frames to `cb` on the audio thread.
    [[nodiscard]] virtual Status start(AudioFormat format, FrameCallback cb) = 0;

    // Stop delivery. Safe to call start() again afterwards.
    [[nodiscard]] virtual Status stop() = 0;

    [[nodiscard]] virtual bool isCapturing() const = 0;
};

// Speaker playback. enqueue() hands decoded PCM to an internal jitter/ring
// buffer that the OS output callback drains; underruns play silence.
class AudioPlayback {
public:
    virtual ~AudioPlayback() = default;

    // Open the default output device for `format` and start the output stream.
    [[nodiscard]] virtual Status start(AudioFormat format) = 0;

    // Queue one decoded PCM frame for playout. Thread-safe; non-blocking.
    virtual void enqueue(const AudioFrame& frame) = 0;

    [[nodiscard]] virtual Status stop() = 0;
};

// Factories. Return null on platforms without an audio backend.
[[nodiscard]] std::unique_ptr<AudioCapture> createAudioCapture();
[[nodiscard]] std::unique_ptr<AudioPlayback> createAudioPlayback();

}  // namespace vc::audio
