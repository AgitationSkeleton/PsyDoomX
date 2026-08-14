#include "XboxAudioOut.h"

#if defined(__XBOX__)

#include "PsxVm.h"
#include "XboxDiag.h"
#include "XboxLog.h"

#include <atomic>
#include <cstring>

#include <SDL.h>
#include <hal/audio.h>
#include <hal/debug.h>
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>

// The AC97 device the nxdk audio HAL drives. It has external linkage in 'hal/audio.c' but is not declared in the
// header. Only its MMIO pointer and descriptor index are wanted, and only to read them.
extern "C" AC97_DEVICE ac97Device;

BEGIN_NAMESPACE(XboxAudioOut)

// Hardware output format. The Xbox AC97 runs at 48 KHz, 16-bit stereo, and nothing else.
static constexpr int      HW_FREQ         = 48000;
static constexpr int      HW_CHANNELS     = 2;
static constexpr uint32_t HW_FRAMES       = 1024;                                   // Per buffer: about 21ms
static constexpr uint16_t HW_BUFFER_BYTES = HW_FRAMES * HW_CHANNELS * sizeof(int16_t);

// The rate the emulated PlayStation SPU produces
static constexpr int SPU_FREQ = 44100;

// How many buffers to cycle through.
//
// 16 buffers is about 341ms of sound queued ahead. That is chosen against a measurement rather than a guess: the frame
// that loads a map took 250ms, and it is that stall which used to empty the queue. Holding more than the worst stall
// means the hardware plays on through it without a gap.
//
// This must divide the 32 entry descriptor ring evenly, so that a buffer is always reused at the same ring position.
static constexpr uint32_t NUM_BUFFERS = 16;

// How many buffers to keep queued ahead of the hardware. This is the latency: whatever is queued has to play out
// before anything new is heard.
//
// This was the full 15, which is 320ms, and it was audible as a delay of about half a second between firing and
// hearing it. That depth was chosen to ride out the 250ms frame that loads a map without a gap, back when running dry
// killed the audio for the rest of the session. It no longer does - the feed is polled and refills itself - so a brief
// drop out during a load is a far better trade than carrying a third of a second of lag through the whole game.
//
// Six buffers is 128ms. Frames here run 36 to 90ms, so this still covers more than a frame of the thread not being
// scheduled, which is what it has to survive moment to moment.
static constexpr uint32_t TARGET_QUEUED = 6;

// How often to look at the hardware. Buffers last about 21ms, so this checks several times per buffer.
static constexpr uint32_t POLL_MS = 4;

static SDL_Thread*              gpThread = nullptr;
static std::atomic<bool>        gRun{false};
static std::atomic<bool>        gRunning{false};
static std::atomic<uint32_t>    gUnderruns{0};
static SDL_AudioStream*         gpConverter = nullptr;
static unsigned char*           gpBuffers[NUM_BUFFERS] = {};

// Total buffers handed to the hardware. Which descriptor was written last follows from this, and the queue depth is
// worked out from that against what the hardware is playing - see 'queuedBufferCount'.
static uint64_t gProvided = 0;

// Scratch for the 44.1 KHz side of the conversion
static constexpr uint32_t SPU_CHUNK_FRAMES = 512;
static float gSpuChunk[SPU_CHUNK_FRAMES * 2];

//------------------------------------------------------------------------------------------------------------------------------------------
// Note a step of startup to the boot log, on screen and on disk.
//
// Setting this up froze the console outright, and a freeze inside a function says only that - which call did it is
// left to guesswork, and there are several here that can hang rather than fail. The AC97 reset in 'XAudioInit' spins
// on a hardware bit with no way out, and 'KeConnectInterrupt' is not safe to run twice. Written before each so the
// last line in the log names the call that did not return.
//------------------------------------------------------------------------------------------------------------------------------------------
static void audioStep(const char* const msg) noexcept {
    debugPrint("[XAUDIO] %s\n", msg);

    const HANDLE h = CreateFileA(
        "E:\\Apps\\PsyDoomX\\bootlog.txt",
        FILE_APPEND_DATA,
        FILE_SHARE_READ,
        nullptr,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );

    if (h != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(h, "[XAUDIO] ", 9, &written, nullptr);
        WriteFile(h, msg, (DWORD) strlen(msg), &written, nullptr);
        WriteFile(h, "\r\n", 2, &written, nullptr);
        CloseHandle(h);
    }
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Read the descriptor index the hardware is currently playing
//------------------------------------------------------------------------------------------------------------------------------------------
static int32_t readCurrentDescriptor() noexcept {
    const volatile unsigned char* const pRegs = (const volatile unsigned char*) ac97Device.mmio;
    return (pRegs) ? (int32_t) pRegs[0x114] : -1;
}

//------------------------------------------------------------------------------------------------------------------------------------------
// How many buffers the hardware still has left to play.
//
// Read from the hardware every time rather than counted up as buffers complete. Counting drifts, and drifting here is
// ruinous: an earlier version accumulated the change in the descriptor index each poll, and when this thread was
// starved through a map load the index wrapped its 32 entries more than once between looks. The count then overshot
// what had actually been provided, the queue depth read as zero from then on, and the loop below refilled the whole
// ring on every single poll - mixing audio flat out, which is what made the game crawl and take so long to load. The
// hardware meanwhile ran on over stale descriptors, replaying the last third of a second of sound in a loop.
//
// The descriptor last handed over is known exactly - it follows from how many buffers have been provided - so the
// distance from there to the one playing is the depth, and nothing has to be remembered between calls.
//------------------------------------------------------------------------------------------------------------------------------------------
static uint32_t queuedBufferCount() noexcept {
    const int32_t civ = readCurrentDescriptor();

    if ((civ < 0) || (gProvided == 0))
        return 0;

    const uint32_t lastWritten = (uint32_t)((gProvided + 31) % 32);
    const uint32_t depth = (lastWritten - (uint32_t) civ) & 31;

    // More outstanding than could possibly have been given means the engine has run past the last buffer handed to it,
    // so what it is playing now is stale. Treat that as empty and refill; saying anything else would keep this loop
    // waiting on buffers that are never coming back.
    return (depth > NUM_BUFFERS) ? 0 : depth;
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Fill one hardware buffer with 48 KHz 16-bit stereo, pulling from the SPU and converting on the way
//------------------------------------------------------------------------------------------------------------------------------------------
static void fillBuffer(unsigned char* const pBuffer) noexcept {
    // Keep feeding the converter until it can give back a whole buffer
    int guard = 0;

    while (SDL_AudioStreamAvailable(gpConverter) < (int) HW_BUFFER_BYTES) {
        PsxVm::generateAudioSamples(gSpuChunk, SPU_CHUNK_FRAMES);

        if (SDL_AudioStreamPut(gpConverter, gSpuChunk, (int) sizeof(gSpuChunk)) < 0)
            break;

        // Should never be reached; only here so a converter that stops accepting input cannot spin this thread forever
        if (++guard > 16)
            break;
    }

    const int got = SDL_AudioStreamGet(gpConverter, pBuffer, (int) HW_BUFFER_BYTES);

    // Pad with silence rather than leaving whatever the buffer held before
    if (got < (int) HW_BUFFER_BYTES) {
        const int from = (got > 0) ? got : 0;
        std::memset(pBuffer + from, 0, (size_t)((int) HW_BUFFER_BYTES - from));
    }
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Keeps the hardware supplied with sound
//------------------------------------------------------------------------------------------------------------------------------------------
static int outputThreadMain([[maybe_unused]] void* pUserData) noexcept {
    // Ask to be scheduled ahead of the game.
    //
    // Sound was cutting out and coming back, which is this thread not running often enough to keep the hardware fed.
    // It was left at the default priority, while the thread SDL used to run its audio callback on asks for the highest
    // there is - so this gave up ground that the old path had. The work per second is small and mostly spent waiting;
    // what matters is being woken promptly when a buffer needs filling, against a game thread that holds the CPU for
    // 36 to 90ms at a stretch.
    //
    // This is also what makes the reduced queue depth above affordable: less audio held in reserve is only safe if the
    // thread refilling it is reliably scheduled.
    SDL_SetThreadPriority(SDL_THREAD_PRIORITY_TIME_CRITICAL);

    uint32_t sinceReport = 0;

    while (gRun.load(std::memory_order_relaxed)) {
        uint32_t queued = queuedBufferCount();
        const uint32_t queuedBeforeFill = queued;

        // The hardware has caught up and sound has dropped out. Survivable here - it is refilled below and carries on -
        // but worth counting, since it is the difference between smooth sound and a stutter.
        if (queued == 0) {
            const uint32_t n = gUnderruns.fetch_add(1, std::memory_order_relaxed) + 1;

            if ((n <= 8) || ((n % 100) == 0)) {
                XBOX_LOGW(Audio, "audio ring emptied - sound dropped out briefly (underrun %u)", n);
            }
        }

        // Refill, but never more than a couple of buffers in one pass.
        //
        // Each buffer is 21ms of sound against a 4ms poll, so two is ample to catch up, and the cap means that however
        // wrong the depth above ever goes this thread can still only ever mix a bounded amount before yielding. Losing
        // the audio is one thing; taking the frame rate down with it is another.
        uint32_t filledThisPass = 0;

        while ((queued < TARGET_QUEUED) && (filledThisPass < 2)) {
            unsigned char* const pBuffer = gpBuffers[(uint32_t)(gProvided % NUM_BUFFERS)];
            fillBuffer(pBuffer);
            XAudioProvideSamples(pBuffer, HW_BUFFER_BYTES, FALSE);
            gProvided++;
            queued++;
            filledThisPass++;
        }

        // Report what the feed is actually doing, once every few seconds
        if (++sinceReport >= 1000) {
            sinceReport = 0;
            XBOX_LOGI(
                Audio,
                "audio feed civ=%d queued=%u filled=%u provided=%llu underruns=%u",
                (int) readCurrentDescriptor(),
                queuedBeforeFill,
                filledThisPass,
                (unsigned long long) gProvided,
                gUnderruns.load(std::memory_order_relaxed)
            );
        }

        SDL_Delay(POLL_MS);
    }

    return 0;
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Set up the hardware and start feeding it
//------------------------------------------------------------------------------------------------------------------------------------------
bool init() noexcept {
    if (gRunning.load(std::memory_order_relaxed))
        return true;

    // Converter from what the SPU makes to what the hardware wants
    audioStep("SDL_NewAudioStream");

    if (!gpConverter) {
        gpConverter = SDL_NewAudioStream(AUDIO_F32SYS, 2, SPU_FREQ, AUDIO_S16LSB, HW_CHANNELS, HW_FREQ);

        if (!gpConverter) {
            audioStep("SDL_NewAudioStream FAILED");
            XBOX_LOGE(Audio, "could not create the 44.1 to 48 KHz converter: %s", SDL_GetError());
            return false;
        }
    }

    // The hardware reads these by physical address, so they have to be contiguous and page locked
    audioStep("MmAllocateContiguousMemoryEx");

    for (uint32_t i = 0; i < NUM_BUFFERS; ++i) {
        if (gpBuffers[i])
            continue;

        gpBuffers[i] = (unsigned char*) MmAllocateContiguousMemoryEx(
            HW_BUFFER_BYTES,
            0,
            0xFFFFFFFF,
            0,
            PAGE_READWRITE | PAGE_WRITECOMBINE
        );

        if (!gpBuffers[i]) {
            audioStep("MmAllocateContiguousMemoryEx FAILED");
            XBOX_LOGE(Audio, "could not allocate audio buffer %u of %u - running silent", i, NUM_BUFFERS);
            return false;
        }

        std::memset(gpBuffers[i], 0, HW_BUFFER_BYTES);
    }

    // Take the hardware, with no callback: nothing here waits on the interrupt.
    //
    // Called once and never again. This spins on a hardware bit waiting for the AC97 codec to finish resetting, with no
    // way out if it never does, and it ends in 'KeConnectInterrupt' - which is not safe to run against an interrupt
    // object that is already connected. Both are why closing and reopening audio is unsurvivable on this platform, and
    // both are why the step above is written before it rather than after.
    // Once per run of the program, never again - this is called a second time when another game is started, and running
    // it twice would connect an interrupt object that is already connected.
    static bool sHaveTakenHardware = false;

    if (!sHaveTakenHardware) {
        audioStep("XAudioInit");
        XAudioInit(16, HW_CHANNELS, nullptr, nullptr);
        sHaveTakenHardware = true;
    } else {
        audioStep("XAudioInit skipped - hardware already taken");
    }

    gProvided = 0;

    // Fill the ring BEFORE starting the engine.
    //
    // The order matters and getting it wrong froze the console. 'XAudioInit' leaves all 32 descriptors zeroed - address
    // zero, length zero - and 'XAudioPlay' sets the run bit with interrupts enabled. Started against an empty ring the
    // engine runs away through those empty descriptors, and the interrupts that come back from it are enough to take
    // the machine down. SDL's own driver queues its buffers first and starts second, for this reason.
    //
    // Filled here rather than left to the thread as well, so that if handing buffers over is what goes wrong, it goes
    // wrong where the boot log will show it.
    audioStep("priming buffers");

    while (gProvided < TARGET_QUEUED) {
        unsigned char* const pBuffer = gpBuffers[(uint32_t)(gProvided % NUM_BUFFERS)];
        fillBuffer(pBuffer);
        XAudioProvideSamples(pBuffer, HW_BUFFER_BYTES, FALSE);
        gProvided++;
    }

    // Now there is something to play, start it
    audioStep("XAudioPlay");
    XAudioPlay();

    audioStep("SDL_CreateThread");
    gRun.store(true, std::memory_order_relaxed);
    gpThread = SDL_CreateThread(outputThreadMain, "XboxAudioOut", nullptr);

    if (!gpThread) {
        audioStep("SDL_CreateThread FAILED");
        XBOX_LOGE(Audio, "audio output thread could not be started: %s", SDL_GetError());
        gRun.store(false, std::memory_order_relaxed);
        return false;
    }

    SDL_DetachThread(gpThread);
    gRunning.store(true, std::memory_order_relaxed);
    audioStep("XboxAudioOut::init done");

    XBOX_LOGI(
        Audio,
        "audio output running - %u buffers of %u frames at %u Hz, %ums queued ahead, polled every %ums",
        NUM_BUFFERS,
        HW_FRAMES,
        (uint32_t) HW_FREQ,
        (uint32_t)((NUM_BUFFERS * HW_FRAMES * 1000u) / (uint32_t) HW_FREQ),
        POLL_MS
    );

    return true;
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Stop feeding the hardware
//------------------------------------------------------------------------------------------------------------------------------------------
void stop() noexcept {
    if (!gRunning.load(std::memory_order_relaxed))
        return;

    gRun.store(false, std::memory_order_relaxed);
    gRunning.store(false, std::memory_order_relaxed);
    gpThread = nullptr;

    // Quiet, but still held. 'XAudioInit' is not called again, for the reason given where it is called.
    XAudioPause();

    // Note: the buffers are deliberately kept. The thread is detached and may be part way through handing one over, and
    // starting a second game reuses them rather than allocating more.
}

bool isRunning() noexcept {
    return gRunning.load(std::memory_order_relaxed);
}

uint32_t numUnderruns() noexcept {
    return gUnderruns.load(std::memory_order_relaxed);
}

END_NAMESPACE(XboxAudioOut)

#endif  // #if defined(__XBOX__)
