//------------------------------------------------------------------------------------------------------------------------------------------
// Basic setup and management for some of the emulated elements of a PlayStation 1, including the CD-ROM, SPU and GPU.
//------------------------------------------------------------------------------------------------------------------------------------------
#include "PsxVm.h"

#include "Asserts.h"
#include "AudioCompressor.h"
#include "Config/Config.h"
#include "DiscInfo.h"
#include "DiscReader.h"
#include "Gpu.h"
#include "Input.h"
#include "IsoFileSys.h"
#include "ProgArgs.h"
#include "Spu.h"

#include <chrono>
#include <SDL.h>
#include <mutex>

#if defined(__XBOX__)
#include "PlayerPrefs.h"
#include "XboxAudioOut.h"
#include "XboxDiag.h"
#include "XboxLog.h"
    #include <hal/debug.h>
    #include <windows.h>
    #include <cstring>
    #include "Wess/wessapi.h"
    #include "Wess/wessarc.h"
    static void psxvmStep(const char* msg) noexcept {
        debugPrint("[PSXVM] %s\n", msg);
        HANDLE h = CreateFileA("E:\\Apps\\PsyDoomX\\bootlog.txt",
            FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD w = 0;
            WriteFile(h, "[PSXVM] ", 8, &w, nullptr);
            WriteFile(h, msg, (DWORD)strlen(msg), &w, nullptr);
            WriteFile(h, "\r\n", 2, &w, nullptr);
            CloseHandle(h);
        }
    }
    #define PSXVM_STEP(msg) psxvmStep(msg)
#else
    #define PSXVM_STEP(msg) ((void)0)
#endif

BEGIN_NAMESPACE(PsxVm)

DiscInfo    gDiscInfo;
IsoFileSys  gIsoFileSys;
Gpu::Core   gGpu;
Spu::Core   gSpu;

static SDL_AudioDeviceID        gSdlAudioDeviceId;

// Note: an ordinary mutex on every platform now.
//
// Xbox used a timed one so the audio callback could wait a moment for the lock rather than output silence. That wait
// is what silenced Doom - see the callback for the detail - and with nothing waiting on a deadline there is no longer
// any reason for the two platforms to differ here.
static std::recursive_mutex         gSpuMutex;

// The audio compressor is only needed if we have a floating point SPU
#if SIMPLE_SPU_FLOAT_SPU
    static AudioCompressor::State gAudioCompState;
#endif

//------------------------------------------------------------------------------------------------------------------------------------------
// A callback invoked by SDL to ask for audio from PsyDoom
//------------------------------------------------------------------------------------------------------------------------------------------
// Not used on Xbox: output there is pulled by 'XboxAudioOut' rather than pushed by SDL's audio thread.
#if !defined(__XBOX__)
static void SdlAudioCallback([[maybe_unused]] void* userData, Uint8* pOutput, int outputSize) noexcept {
    XboxDiag::tickAudio();    // diagnostic: count audio callback invocations

    // Ignore invalid requests
    if (outputSize <= 0) {
        XboxDiag::tickAudioExit();
        return;
    }

    // How many samples are to be output? SDL provides a float32 stereo buffer.
    const uint32_t numSamples = (uint32_t) outputSize / (sizeof(float) * 2);
    float* pOutputF = reinterpret_cast<float*>(pOutput);

#if defined(__XBOX__)
    // Take the lock if it is free, and give up immediately if it is not. No timed wait.
    //
    // This used to wait 500 microseconds using 'try_lock_for', and that is what silenced Doom. Measured on hardware,
    // the callback runs at 89 a second and then stops dead - zero for the rest of the session - while SDL still
    // reports the device as playing. A device that is playing but never asks for samples means the audio thread is
    // stuck inside this function, and the only thing here that can block is that wait.
    //
    // 'try_lock_for' has to work out a deadline, and it does that from a clock. The clock on this hardware is not
    // dependable - it is what froze the music sequencer, where a single step backwards left a comparison that could
    // never come true again. A deadline computed from a bad reading can sit far enough in the future that the wait
    // never ends, and an audio thread that never returns is never called again. That the contention counter reads zero
    // fits exactly: it is not timing out, it is stuck.
    //
    // Nothing is lost by not waiting. That same counter says contention essentially never happens, and one buffer of
    // silence is a far smaller price than the rest of the session in silence.
    if (!gSpuMutex.try_lock()) {
        XboxDiag::tickLockSpu();  // diagnostic: count how often audio had to skip
        XboxDiag::tickAudioSilence();
        std::memset(pOutput, 0, (size_t) outputSize);
        XboxDiag::tickAudioExit();
        return;
    }
#else
    // Lock the SPU and generate the requested number of samples
    PsxVm::LockSpu spuLock;
#endif

    for (uint32_t sampleIdx = 0; sampleIdx < numSamples; ++sampleIdx) {
        // Get this sample in floating point format
        const Spu::StereoSample sample = Spu::stepCore(gSpu);

        #if SIMPLE_SPU_FLOAT_SPU
            float sampleL = sample.left;
            float sampleR = sample.right;
        #else
            float sampleL = Spu::toFloatSample(sample.left);
            float sampleR = Spu::toFloatSample(sample.right);
        #endif

        // If using the floating point SPU apply audio compression.
        // When using floating point sound the audio can get EXTREMELY loud (and painful to listen to) if not capped.
        // When using the original 16-bit SPU the sound will also clip/distort if too loud, so no point in using compression in that case.
        #if SIMPLE_SPU_FLOAT_SPU
            AudioCompressor::compress(gAudioCompState, sampleL, sampleR);
        #endif

        pOutputF[0] = sampleL;
        pOutputF[1] = sampleR;
        pOutputF += 2;
    }

#if defined(__XBOX__)
    gSpuMutex.unlock();

    // Update WESS active voice count for the diagnostic overlay
    if (const master_status_structure* const pMStat = wess_get_master_status()) {
        XboxDiag::setWessVoices(pMStat->num_active_voices);
    }

    // Counted on the way out as well as the way in.
    //
    // The callback stops being invoked entirely - 88 a second, then nothing, while SDL still reports the device as
    // playing - and two very different things look identical from outside: a callback that is entered and never
    // returns, and a callback that is never entered again. Counting both ends tells them apart, and I have now been
    // wrong twice guessing between them.
    XboxDiag::tickAudioExit();
#endif
}

#endif  // #if !defined(__XBOX__)

#if defined(__XBOX__)
//------------------------------------------------------------------------------------------------------------------------------------------
// Xbox: produce audio on demand, rather than waiting to be asked for it.
//
// Same work as the callback above, but pulled by 'XboxAudioOut' instead of being driven by SDL's audio thread. The
// difference matters: SDL's thread only runs when a hardware interrupt wakes it, and on this console that interrupt
// stops for good the first time the buffer queue empties.
//
// Writes 'numFrames' stereo frames of 44.1 KHz floating point, and silence if the SPU is busy.
//------------------------------------------------------------------------------------------------------------------------------------------
void generateAudioSamples(float* const pSamples, const uint32_t numFrames) noexcept {
    XboxDiag::tickAudio();

    if ((!pSamples) || (numFrames <= 0)) {
        XboxDiag::tickAudioExit();
        return;
    }

    // Take the lock if it is free and give up at once if it is not; never wait.
    //
    // The game thread holds this for long stretches while a map loads, and waiting on it here would stall the audio
    // ring for exactly as long - which is the fault this whole path exists to avoid. Silence for the duration of a
    // load is the right trade.
    if (!gSpuMutex.try_lock()) {
        XboxDiag::tickLockSpu();
        XboxDiag::tickAudioSilence();
        std::memset(pSamples, 0, (size_t) numFrames * sizeof(float) * 2);
        XboxDiag::tickAudioExit();
        return;
    }

    float* pOutputF = pSamples;

    for (uint32_t frameIdx = 0; frameIdx < numFrames; ++frameIdx) {
        const Spu::StereoSample sample = Spu::stepCore(gSpu);

        #if SIMPLE_SPU_FLOAT_SPU
            float sampleL = sample.left;
            float sampleR = sample.right;
        #else
            float sampleL = Spu::toFloatSample(sample.left);
            float sampleR = Spu::toFloatSample(sample.right);
        #endif

        #if SIMPLE_SPU_FLOAT_SPU
            AudioCompressor::compress(gAudioCompState, sampleL, sampleR);
        #endif

        pOutputF[0] = sampleL;
        pOutputF[1] = sampleR;
        pOutputF += 2;
    }

    gSpuMutex.unlock();

    if (const master_status_structure* const pMStat = wess_get_master_status()) {
        XboxDiag::setWessVoices(pMStat->num_active_voices);
    }

    XboxDiag::tickAudioExit();
}
#endif  // #if defined(__XBOX__)

//------------------------------------------------------------------------------------------------------------------------------------------
// Determine the VRAM size to use from user config
//------------------------------------------------------------------------------------------------------------------------------------------
static void getVramSize(uint16_t& vramW, uint16_t& vramH) noexcept {
    // Xbox: original PSX VRAM size only - 64 MB total RAM cannot support 128 MiB VRAM
    #if defined(__XBOX__)
        vramW = 1024; vramH = 512;         // 1 MiB (original PSX)
    #elif PSYDOOM_LIMIT_REMOVING
    const int32_t vramSize = Config::gVramSizeInMegabytes;
        if (vramSize > 64) {
            vramW = 8192; vramH = 8192;     // 128 MiB
        } else if (vramSize > 32) {
            vramW = 4096; vramH = 8192;     // 64 MiB
        } else if (vramSize > 16) {
            vramW = 4096; vramH = 4096;     // 32 MiB
        } else if (vramSize > 8) {
            vramW = 2048; vramH = 4096;     // 16 MiB
        } else if (vramSize > 4) {
            vramW = 2048; vramH = 2048;     // 8 MiB
        } else if (vramSize > 2) {
            vramW = 1024; vramH = 2048;     // 4 MiB
        } else if (vramSize == 2) {
            vramW = 1024; vramH = 1024;     // 2 MiB
        } else if (vramSize == 1) {
            vramW = 1024; vramH = 512;      // 1 MiB
        } else {
            vramW = 8192; vramH = 8192;     // Default: 128 MiB
        }
    #else
         vramW = 1024; vramH = 512;         // 1 MiB
    #endif
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Initialize emulated PlayStation system components and use the given .cue file for the game disc
//------------------------------------------------------------------------------------------------------------------------------------------
bool init(const char* const doomCdCuePath) noexcept {
    // Init the GPU core
    PSXVM_STEP("Gpu::initCore");
    {
        uint16_t vramW = {};
        uint16_t vramH = {};
        getVramSize(vramW, vramH);
        Gpu::initCore(gGpu, vramW, vramH);
    }

    // Init the SPU core.
    // On Xbox: use original PSX voice count and RAM size — extended defaults would exhaust memory.
    constexpr uint32_t PSX_SPU_RAM_SIZE = 512 * 1024;

    #if defined(__XBOX__)
        // Original voice count, but more than the original RAM.
        //
        // 24 voices because that is a rendering cost and this console has none to spare. The RAM is a different matter:
        // it is one flat allocation in main memory and costs nothing to run, and 512 KiB is not enough for the
        // Randomizer. That mode makes every monster in the game audible by loading the union of every map's sounds,
        // which the engine's own comment in 's_sound.cpp' says is only reasonable "because of PsyDoom's greatly
        // expanded sound RAM" - so keeping the original size here was quietly ruling the feature out.
        //
        // Measured off the discs, the union comes to 429.7 KiB for Doom and Final Doom and 512.9 KiB for the Master
        // Edition, against 508 KiB usable once the capture buffers are taken off. Doom and Final Doom then add about
        // 164 KiB of borrowed Master Edition monster samples on top. So the worst case is a little under 700 KiB
        // before the general sounds and the map's music, neither of which is counted above.
        //
        // 2 MiB leaves better than twice that headroom for 1.5 MiB of main memory, which is affordable here in a way
        // the 16 MiB limit-removing default is not.
        constexpr uint32_t SPU_VOICE_COUNT = 24;
        constexpr uint32_t spuRamSize = 2 * 1024 * 1024;
    #elif PSYDOOM_LIMIT_REMOVING
        constexpr uint32_t DEFAULT_SPU_RAM_SIZE = 16 * 1024 * 1024;
        constexpr uint32_t SPU_VOICE_COUNT = 64;
        const uint32_t spuRamSize = std::max<uint32_t>((Config::gSpuRamSize <= 0) ? DEFAULT_SPU_RAM_SIZE : Config::gSpuRamSize, PSX_SPU_RAM_SIZE);
    #else
        constexpr uint32_t SPU_VOICE_COUNT = 24;
        constexpr uint32_t spuRamSize = PSX_SPU_RAM_SIZE;
    #endif

    PSXVM_STEP("Spu::initCore");
    Spu::initCore(gSpu, spuRamSize, SPU_VOICE_COUNT);

    // Init the audio compressor if using the float SPU (don't need it for the 16-bit SPU)
    #if SIMPLE_SPU_FLOAT_SPU
        constexpr float kPostGainDb = 4.0f;
        AudioCompressor::init(
            gAudioCompState,
            -12.0f,             // thresholdDB
            10.0f,              // kneeWidthDB
            2.0f,               // compressionRatio
            kPostGainDb,        // postGainDB
            0.150f,             // lpfResponseTime
            0.002f,             // attackTime
            0.005f              // releaseTime
        );
    #endif

    // Parse the .cue info for the game disc
    PSXVM_STEP("parseFromCueFile");
    {
        std::string parseErrorMsg;

        if (!gDiscInfo.parseFromCueFile(doomCdCuePath, parseErrorMsg)) {
            FatalErrors::raiseF(
                "Couldn't open or failed to parse the game disc .cue file '%s'!\nError message: %s",
                doomCdCuePath,
                parseErrorMsg.c_str()
            );
        }
    }

    // Build up the ISO file system from the game disc
    PSXVM_STEP("IsoFileSys::build");
    {
        DiscReader discReader(gDiscInfo);

        if (!gIsoFileSys.build(discReader)) {
            FatalErrors::raise(
                "Failed to extract the ISO 9960 filesystem records from the game's disc! "
                "Is the disc in a strange format, or is the image corrupt?"
            );
        }
    }

    // Setup sound
    PSXVM_STEP("SDL_InitSubSystem AUDIO");
#if defined(__XBOX__)
    // Xbox: drive the audio hardware directly instead of opening an SDL audio device.
    //
    // SDL's device cannot be made to work here. Its driver keeps a single 21ms buffer in flight and its audio thread
    // only runs when the completion interrupt wakes it, so the 250ms frame that loads a map empties the queue - and an
    // empty queue raises no interrupt, so nothing wakes the thread that would refill it.
    //
    // Refilling it from outside was tried and did not work either: the hardware went on playing what it was given, with
    // the descriptor index advancing the whole time, while the callback count stayed at zero. The interrupt was
    // reaching the ISR and being thrown away, for a reason no code outside the HAL can undo. The full account is at the
    // top of 'XboxAudioOut.h'.
    //
    // So nothing here waits to be asked for audio: 'XboxAudioOut' keeps the hardware supplied by polling it.
    PSXVM_STEP("XboxAudioOut::init");

    if (!XboxAudioOut::init()) {
        PSXVM_STEP("XboxAudioOut::init failed - running silent");
    }

    // Say which settings are actually in force, not which ones the build defaults to.
    //
    // These are read from ini files in 'E:\Apps\PsyDoomX\', and a file written by an earlier run keeps its old value
    // whatever the build now defaults to. 'EnhanceWallDrawPrecision' matters most: it puts the wall renderer on 64-bit
    // arithmetic, which this 32-bit CPU synthesises several instructions at a time in the innermost drawing loop, and
    // it is now meant to default off here. If it reads 1 below, 'graphics_cfg.ini' is overriding that and wants
    // deleting for the change to take effect.
    XBOX_LOGI(
        Video,
        "settings wallprec=%d floorgapfix=%d skyleakfix=%d uncapfps=%d vramMB=%d",
        (int) Config::gbEnhanceWallDrawPrecision,
        (int) Config::gbFloorRenderGapFix,
        (int) Config::gbSkyLeakFix,
        (int) PlayerPrefs::gbUncapFramerate,
        (int) Config::gVramSizeInMegabytes
    );

#else
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) >= 0) {
        // Firstly try to open an audio device sampling at 44,100 Hz stereo in floating point mode.
        // Note that if initialization succeeds then we've got our requested format, since we ask SDL not to allow any deviation.
        SDL_AudioSpec wantFmt = {};
        wantFmt.freq = 44100;
        wantFmt.format = AUDIO_F32;
        wantFmt.channels = 2;

        if (Config::gAudioBufferSize > 0) {
            wantFmt.samples = (uint16_t) std::min<int32_t>(Config::gAudioBufferSize, UINT16_MAX);
        } else {
            wantFmt.samples = 256;  // Use a default of '256' samples (~5.8 MS latency) when using 'auto' configure mode
        }

        wantFmt.callback = SdlAudioCallback;

        SDL_AudioSpec gotFmt = {};
        gSdlAudioDeviceId = SDL_OpenAudioDevice(nullptr, false, &wantFmt, &gotFmt, false);

        if (gSdlAudioDeviceId != 0) {
            SDL_PauseAudioDevice(gSdlAudioDeviceId, false);
        } else {
            SDL_QuitSubSystem(SDL_INIT_AUDIO);
        }
    }
#endif

    // Initialize game controller support (already initialized by bootstrap menu, but safe to call again)
    PSXVM_STEP("SDL_InitSubSystem GAMECONTROLLER");
    SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER);
    PSXVM_STEP("PsxVm::init done");
    return true;
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Tear down emulated PlayStation components
//------------------------------------------------------------------------------------------------------------------------------------------
void shutdown() noexcept {
#if defined(__XBOX__)
    // Xbox: silence the device but leave it open, and leave the audio subsystem up.
    //
    // Closing it is not survivable here. 'XBOXAUDIO_CloseDevice' calls 'XAudioInit(16, 2, NULL, NULL)' to quieten the
    // hardware, and that function ends with 'KeConnectInterrupt' on an interrupt object that is already connected -
    // then opening a device again connects the very same object a third time. It also cold resets the AC97 chip and
    // frees the DMA buffers with 'MmFreeContiguousMemory' while the engine may still be reading from them.
    //
    // None of that matters on a platform that closes audio once on the way out, but this build returns to the launcher
    // and starts another game in the same process, so the sequence runs for real - which fits the launcher hanging
    // after quitting Doom and choosing Final Doom.
    //
    // The device is handed back to 'init' below rather than reopened, so nothing is leaked by keeping it.
    XboxAudioOut::stop();

    if (gSdlAudioDeviceId != 0) {
        SDL_PauseAudioDevice(gSdlAudioDeviceId, true);
    }
#else
    if (gSdlAudioDeviceId != 0) {
        SDL_PauseAudioDevice(gSdlAudioDeviceId, true);
        SDL_CloseAudioDevice(gSdlAudioDeviceId);
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        gSdlAudioDeviceId = 0;
    }
#endif

    Spu::destroyCore(gSpu);     // Note: no locking of the SPU here because all threads should be done with it at this point
    Gpu::destroyCore(gGpu);
}

bool haveAudioOutputDevice() noexcept {
#if defined(__XBOX__)
    // No SDL audio device is opened on this platform; output is driven directly by 'XboxAudioOut'
    return XboxAudioOut::isRunning();
#else
    return (gSdlAudioDeviceId != 0);
#endif
}

#if defined(__XBOX__)
//------------------------------------------------------------------------------------------------------------------------------------------
// Xbox: pause the SDL audio output device.
// Call before long WESS init operations that hold the SPU mutex for extended periods,
// so the audio callback cannot starve and cause SDL to stop the device.
//------------------------------------------------------------------------------------------------------------------------------------------
void pauseAudioDevice() noexcept {
    if (gSdlAudioDeviceId != 0) {
        SDL_PauseAudioDevice(gSdlAudioDeviceId, true);
    }
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Xbox: resume the SDL audio output device after a pause.
//------------------------------------------------------------------------------------------------------------------------------------------
void resumeAudioDevice() noexcept {
    if (gSdlAudioDeviceId != 0) {
        SDL_PauseAudioDevice(gSdlAudioDeviceId, false);
    }
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Xbox: ensure the SDL audio output device is in the playing state.
// Call after light operations that might have paused the device.
//------------------------------------------------------------------------------------------------------------------------------------------
void ensureAudioDevicePlaying() noexcept {
    if (gSdlAudioDeviceId != 0 && SDL_GetAudioDeviceStatus(gSdlAudioDeviceId) != SDL_AUDIO_PLAYING) {
        SDL_PauseAudioDevice(gSdlAudioDeviceId, false);
    }
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Xbox: reset the SDL audio device to a clean PLAYING state.
// A brief pause/unpause cycle is sufficient: it flushes the nxdk DirectSound buffer state
// without closing the device.  SDL_CloseAudioDevice is NOT safe mid-game on nxdk — once
// the voice is released it cannot be recreated (same class of issue as the D3D8 crash when
// SDL_QuitSubSystem(SDL_INIT_VIDEO) destroys the device).
//------------------------------------------------------------------------------------------------------------------------------------------
void restartAudioDevice() noexcept {
    if (gSdlAudioDeviceId == 0)
        return;
    SDL_PauseAudioDevice(gSdlAudioDeviceId, true);
    SDL_PauseAudioDevice(gSdlAudioDeviceId, false);
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Xbox: begin a "long load" guard (e.g. during S_LoadMapSoundAndMusic).
// Audio is not paused here; per-sector usbh_pooling_hubs() calls in lcdload.cpp keep USB
// alive, and the SPU try_lock() in the callback handles contention without blocking.
//------------------------------------------------------------------------------------------------------------------------------------------
void beginLongLoad() noexcept {
    // No-op: see comment above.
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Xbox: end the "long load" guard started by beginLongLoad().
// No action needed: the audio callback never blocks, so it cannot hold DirectSound up
// throughout the load without blocking.  Calling pause/unpause here kills the DirectSound
// notification on nxdk — the device was already running fine (CBS≥80 during LOADING).
//------------------------------------------------------------------------------------------------------------------------------------------
void endLongLoad() noexcept {
    // no-op
}
#endif  // defined(__XBOX__)

void lockSpu() noexcept {
    // On Xbox: the audio callback never waits on the lock, so it cannot block the main
    // thread indefinitely.  The main thread uses a plain blocking lock.
    gSpuMutex.lock();
}

void unlockSpu() noexcept {
    gSpuMutex.unlock();
}

END_NAMESPACE(PsxVm)
