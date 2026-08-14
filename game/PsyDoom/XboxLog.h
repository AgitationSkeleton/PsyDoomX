//------------------------------------------------------------------------------------------------------------------------------------------
// PsyDoom Xbox: a diagnostic log that goes over the network to a listener on a development machine.
//
// The problem this solves: the interesting faults on a console are the ones a screenshot cannot show. A frame that
// takes 300ms, an audio ring that runs dry twice a second, a disc read that blocks the game thread - none of them look
// like anything, and asking someone to reproduce a fault while reading numbers off a television is no way to work.
//
// The rule this is built around is that measuring must not change what is measured. A diagnostic that costs the game
// thread a syscall, an allocation, or a wait on the network would make the timings it reports meaningless. So:
//
//  - Writing a record costs a timestamp read, a formatted write into a fixed slot, and one atomic increment.
//  - Nothing else happens on the calling thread. No socket, no allocation, no lock, no wait.
//  - A background thread does the sending. If it cannot keep up, or nothing is listening, the oldest records are
//    dropped and counted - the game is never held up to make room.
//  - Any thread may write. The audio callback and the game thread both have things worth saying.
//
// If no listener is there the whole thing is inert and costs a few atomics per record. It is safe to leave enabled.
//------------------------------------------------------------------------------------------------------------------------------------------
#pragma once

#include <cstdarg>
#include <cstdint>

namespace XboxLog {

// Parts of the frame that used to be lumped into 'other'.
//
// 'other' was 31.6ms of a 69ms frame - the largest single cost and attributed to nothing. It is not the rasteriser:
// 'I_SubmitGpuCmds' is empty and drawing is immediate mode inside the render timer. What is left is the status bar,
// which is drawn every frame through the same software rasteriser and twice over in splitscreen, the input update
// with its USB polling, and audio generation. Timing them says which.
inline unsigned long long gXbStatusBarMicros = 0;
inline unsigned long long gXbInputMicros = 0;
inline unsigned long long gXbAudioMicros = 0;

// Which part of the game a record came from.
//
// These are not arbitrary: each one is somewhere the 3DS port had a fault that was hard to see without one. Video and
// audio timing, CD audio reads starving on the wrong thread, input arriving too late to be noticed, and disc reads
// blocking a thread that could not afford it.
enum class Sys : uint8_t {
    General,
    Video,      // Frame timing, present cost, the CPU/GPU split
    Audio,      // Callback duration, underruns, ring occupancy
    CdAudio,    // Sector reads and how long they took
    Input,      // Devices arriving and leaving, button state on request
    Disc,       // File opens and read durations
    Mem,        // Heap high water marks
    Net,        // The relay talking about itself
    Split,      // Splitscreen, later
};

enum class Sev : uint8_t {
    Trace,      // Per frame or finer. Expected to be voluminous.
    Info,       // Milestones worth seeing in a normal session
    Warn,       // Something is wrong but the game continues
    Error,      // Something is broken
};

// Starts the relay. Safe to call before the network is up: connecting happens on the background thread, so a console
// waiting on DHCP does not hold up the game's startup by even a millisecond.
void init() noexcept;

// Stops the relay and flushes what it can, briefly. Never waits long.
//
// Called 'stop' rather than 'shutdown' on purpose: lwIP's sockets.h defines 'shutdown' as a macro, so a function of
// that name here does not survive contact with the network headers.
void stop() noexcept;

// Writes a record. Callable from any thread, including an audio callback.
void logf(const Sys sys, const Sev sev, const char* const format, ...) noexcept;
void vlogf(const Sys sys, const Sev sev, const char* const format, std::va_list args) noexcept;

// How many records had to be dropped because the sender could not keep up. Worth logging occasionally: a rising count
// means the instrument is saturated and the numbers coming out of it are a sample rather than the whole picture.
uint32_t droppedCount() noexcept;

// Called by the game thread to say it is still going, with a note of where it is.
//
// The sender runs on its own thread, so it keeps running when the game thread stops. If nothing marks progress for a
// few seconds it reports the stall and the last place the game got to. A session ended in a hard freeze with the log
// showing nothing wrong at all right up to the final frame - voices cycling, clock advancing, memory flat - so there
// has to be something that survives the game thread to say so.
void heartbeat(const char* const where) noexcept;

// Microseconds since the relay started. The same clock stamps every record, so a slow frame and the disc read that
// caused it can be laid against each other.
uint64_t nowMicros() noexcept;

}   // namespace XboxLog

//------------------------------------------------------------------------------------------------------------------------------------------
// Convenience macros.
//
// The trace macro compiles to nothing when tracing is off, so the per-frame instrumentation can be left in the source
// permanently and turned on when a question needs answering.
//------------------------------------------------------------------------------------------------------------------------------------------
#if !defined(XBOX_LOG_ENABLED)
    #define XBOX_LOG_ENABLED 1
#endif

#if !defined(XBOX_LOG_TRACE_ENABLED)
    #define XBOX_LOG_TRACE_ENABLED 1
#endif

#if XBOX_LOG_ENABLED
    #define XBOX_LOGI(sys, ...)  XboxLog::logf(XboxLog::Sys::sys, XboxLog::Sev::Info,  __VA_ARGS__)
    #define XBOX_LOGW(sys, ...)  XboxLog::logf(XboxLog::Sys::sys, XboxLog::Sev::Warn,  __VA_ARGS__)
    #define XBOX_LOGE(sys, ...)  XboxLog::logf(XboxLog::Sys::sys, XboxLog::Sev::Error, __VA_ARGS__)
#else
    #define XBOX_LOGI(sys, ...)  ((void) 0)
    #define XBOX_LOGW(sys, ...)  ((void) 0)
    #define XBOX_LOGE(sys, ...)  ((void) 0)
#endif

#if XBOX_LOG_ENABLED && XBOX_LOG_TRACE_ENABLED
    #define XBOX_LOGT(sys, ...)  XboxLog::logf(XboxLog::Sys::sys, XboxLog::Sev::Trace, __VA_ARGS__)
#else
    #define XBOX_LOGT(sys, ...)  ((void) 0)
#endif
