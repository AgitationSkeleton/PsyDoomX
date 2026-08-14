#pragma once

//------------------------------------------------------------------------------------------------------------------------------------------
// Presenting through the NV2A instead of by writing the framebuffer with the CPU.
//
// The CPU present costs around 10.7ms of a 25ms frame in single player and 10.2ms of 40ms in splitscreen, doing work
// that counts out at roughly 2ms however it is measured. Nothing found explains the gap, so the path is being replaced
// rather than shaved at. A textured quad puts the frame on screen with the GPU doing the scaling and the colour swap.
//
// Built in steps, each proving one thing, because this replaces the one subsystem that has been reliable throughout:
//
//   1. Bring pbkit up and swap buffers, writing the back buffer with the CPU. No register programming at all. Proves
//      pbkit coexists with the video mode the game already sets, which is the thing most likely to fail outright.
//   2. A textured quad from a copy of VRAM, linear A1R5G5B5 with the register combiner swapping red and blue.
//   3. Texture straight out of VRAM if the upload proves to matter, which needs VRAM in contiguous GPU visible memory.
//
// Everything is behind 'isEnabled' with the CPU present left intact, so a failure is one flag from working. Not
// portable: this is the Xbox's GPU and nothing here applies to the GameCube.
//------------------------------------------------------------------------------------------------------------------------------------------
#include "Macros.h"

#include <cstdint>

BEGIN_NAMESPACE(XboxNv2aPresent)

// Is the GPU present in use? False means the CPU present runs exactly as it always has.
bool isEnabled() noexcept;

// Brings pbkit up. Safe to call more than once; does nothing if the GPU present is off or already started.
// Returns false if pbkit would not start, in which case the CPU path stays in charge.
bool init() noexcept;

void shutdown() noexcept;

// Step one: fill the back buffer with a colour and show it.
//
// Deliberately done with the CPU. The point is to prove that pbkit's buffers and swapping work alongside the video mode
// the game sets, without any GPU register programming in the way of finding out.
void presentSolidColour(const uint32_t argb) noexcept;

// Step two: put a rectangle of VRAM on screen as a textured quad.
//
// 'srcX/srcY/srcW/srcH' is the area of VRAM to show, in 16-bit pixels; 'dstX/dstY/dstW/dstH' is where it lands on
// screen. Called once for a single view and twice for splitscreen, which is why it takes both rectangles rather than
// assuming the layout.
//
// Begin and end bracket the frame so a pair of views costs one swap rather than two.
void beginFrame() noexcept;
void drawVramRect(
    const uint32_t srcX,
    const uint32_t srcY,
    const uint32_t srcW,
    const uint32_t srcH,
    const int32_t dstX,
    const int32_t dstY,
    const int32_t dstW,
    const int32_t dstH
) noexcept;
void endFrame() noexcept;

END_NAMESPACE(XboxNv2aPresent)
