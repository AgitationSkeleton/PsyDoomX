#pragma once

#include "Macros.h"

#include <cstdint>

struct DiscInfo;
struct IsoFileSys;

namespace Gpu {
    struct Core;
}

namespace Spu {
    struct Core;
}

BEGIN_NAMESPACE(PsxVm)

// Information for the game disc and the filesystem
extern DiscInfo     gDiscInfo;
extern IsoFileSys   gIsoFileSys;

// Access to the implementation of the PlayStation GPU and SPU
extern Gpu::Core    gGpu;
extern Spu::Core    gSpu;

bool init(const char* const doomCdCuePath) noexcept;
void shutdown() noexcept;

// Returns 'true' if there is valid audio output device
bool haveAudioOutputDevice() noexcept;

#if defined(__XBOX__)
// Produce audio on demand: 'numFrames' stereo frames of 44.1 KHz floating point, or silence if the SPU is busy.
// Pulled by 'XboxAudioOut', which drives the hardware itself rather than waiting to be called by SDL.
void generateAudioSamples(float* const pSamples, const uint32_t numFrames) noexcept;

// Pause/resume the SDL audio output device.
//
// Note: these act on the SDL device, which this platform no longer uses for output - see 'XboxAudioOut'. They are kept
// because they are harmless and still referenced, but they do not affect what is heard.
void pauseAudioDevice() noexcept;
void resumeAudioDevice() noexcept;

// Unpause the SDL audio device if it is not already playing.
void ensureAudioDevicePlaying() noexcept;

// Pause and immediately unpause the SDL audio device.
//
// The name and its old comment both claimed this closed and reopened the device to clear a "ghost-PLAYING" state. It
// never closed anything, and it never cleared that state either: the callbacks had stopped because the hardware buffer
// queue had run dry, and nothing done to the SDL device could refill it. That is what 'XboxAudioOut' now handles.
void restartAudioDevice() noexcept;

// Begin/end a long disc+SPU load operation.
//
// Both are empty. Their comments used to describe pausing audio and keeping USB alive across a load, neither of which
// they have ever done - USB is kept alive per sector inside 'lcdload.cpp', and audio is no longer able to die from a
// long load. Kept only so the call sites read the same as upstream.
void beginLongLoad() noexcept;
void endLongLoad() noexcept;
#endif

// Fire timer (root counter) related events if appropriate.
// Note: this is implemented in LIBAPI, where timers are handled.
void generateTimerEvents() noexcept;

// Lock and unlock the SPU and a helper to do it via the RAII pattern.
// The SPU should be locked before reading from or writing to any of it's properties.
void lockSpu() noexcept;
void unlockSpu() noexcept;

struct LockSpu {
    LockSpu() noexcept { lockSpu(); }
    ~LockSpu() noexcept { unlockSpu(); }
};

END_NAMESPACE(PsxVm)
