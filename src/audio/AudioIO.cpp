#include "audio/AudioIO.h"

// Platform factory fallback. The macOS backend lives in MacAudio.mm and defines
// these for __APPLE__; on every other platform audio I/O is not yet implemented,
// so the factories return null and callers degrade to video-only.

#if !defined(__APPLE__)

namespace vc::audio {

std::unique_ptr<AudioCapture> createAudioCapture() { return nullptr; }
std::unique_ptr<AudioPlayback> createAudioPlayback() { return nullptr; }

}  // namespace vc::audio

#endif  // !__APPLE__
