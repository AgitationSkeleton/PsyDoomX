//------------------------------------------------------------------------------------------------------------------------------------------
// Xbox: audio output driven by polling the hardware, in place of SDL's audio device.
//
// SDL's audio device cannot be made to work here. Its nxdk driver keeps one 21ms buffer in flight, and its audio
// thread only runs when woken by the AC97 completion interrupt, so the first stall longer than that buffer empties the
// queue - and an empty queue raises no completion interrupt, so nothing ever wakes the thread that would refill it.
//
// That much could be worked around by refilling the queue from outside, and that was tried. It failed for a second,
// deeper reason, and the measurement is worth keeping:
//
//     civ=17 lvi=0    civ=20 lvi=4    civ=25 lvi=8    civ=0 lvi=12
//
// 'civ' is the descriptor the hardware is playing and it keeps advancing, so the engine was healthy and playing
// everything handed to it - while the callback count stayed at zero. The interrupt was reaching the ISR and being
// discarded. In 'hal/audio.c' the ISR only queues its DPC when 'analogBufferCount' and 'digitalBufferCount' differ,
// 'XAudioProvideSamples' raises both together, and once the drained flags stop them decrementing symmetrically the two
// can no longer diverge. The DPC is then never queued again and the semaphore is never posted again. Nothing outside
// the HAL can repair that bookkeeping.
//
// So this does not use the interrupt at all. A thread polls the bus master's current descriptor index to see what the
// hardware has finished, and keeps the ring topped up. Nothing here can be missed, coalesced or lost, and a stall is
// just a gap in the sound rather than the end of it.
//
// Samples come from 'PsxVm::generateAudioSamples' at the PlayStation's 44.1 KHz and are converted to the 48 KHz the
// Xbox hardware runs at by an 'SDL_AudioStream', which is used purely as a converter.
//------------------------------------------------------------------------------------------------------------------------------------------
#pragma once

#if defined(__XBOX__)

#include "Macros.h"

#include <cstdint>

BEGIN_NAMESPACE(XboxAudioOut)

// Start audio output. Returns false if the hardware or its buffers could not be set up.
bool init() noexcept;

// Stop feeding the hardware and silence it.
void stop() noexcept;

// True once output is running.
bool isRunning() noexcept;

// How many times the ring was found empty, meaning sound will have dropped out briefly. Zero over a session means the
// hardware was never left waiting.
uint32_t numUnderruns() noexcept;

END_NAMESPACE(XboxAudioOut)

#endif  // #if defined(__XBOX__)
