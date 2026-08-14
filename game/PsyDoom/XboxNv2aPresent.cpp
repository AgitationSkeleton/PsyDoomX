#include "XboxNv2aPresent.h"

#if defined(__XBOX__)

#include "XboxLog.h"
#include "PsxVm.h"

#include "Gpu.h"

#include <cstring>
#include <cstdarg>

#include <hal/video.h>
#include <cstdio>

// The boot log, which is a file on disk and works from the first instruction.
//
// The relay needs networking, and everything in here runs before networking is up - the lines logged while proving step
// one were written into nothing, and a blue screen was the only evidence there was. That was enough for a question with
// a visual answer. Texture setup is not: a wrong pitch, a wrong offset and a wrong format all look much the same on a
// television, and telling them apart needs the numbers.
extern void xbLog(const char* msg) noexcept;

extern "C" {
    #include <pbkit/pbkit.h>
    #include <pbkit/nv_regs.h>
    #include <pbkit/nv_objects.h>
    #include <hal/xbox.h>
}

// Masking a value into one of the register fields defined in 'nv_regs.h'.
//
// The header gives each field as a mask over the 32-bit register, so a value has to be shifted up to where its mask
// sits. Deriving the shift from the mask keeps the two from drifting apart.
#define NV2A_FIELD(mask, value)     (((value) << (__builtin_ctz(mask))) & (mask))

BEGIN_NAMESPACE(XboxNv2aPresent)

// Says it twice: to the boot log, which works this early, and to the relay, which does not yet but will later
static void nv2aLog(const char* const fmt, ...) noexcept {
    char msg[256];

    va_list args;
    va_start(args, fmt);
    std::vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    xbLog(msg);
    XBOX_LOGI(Video, "%s", msg);
}

// Off. The CPU present is what ships.
//
// The GPU path is written and does not work yet: it draws structured static rather than a picture. What that means is
// a correct quad sampling the texture at meaningless places, and four attempts at it each found a real fault - a video
// mode pbkit would not accept, a combiner asked for zero stages, an attribute order that latched vertices before their
// texture coordinates were set - without any of them being the last one.
//
// Left in place rather than removed. It is behind this constant, costs nothing while off, and everything learned about
// it is written down in docs/PERF_BASELINE.md.
//
// If it is picked up again, the next step is the one skipped: an untextured flat coloured quad drawn by the GPU. That
// separates geometry and transform from texturing, which have been confounded in every attempt so far - the reason
// four builds each found a real bug and none of them produced a picture.
//
// What the static rules out is as useful as what it does not. It is not black, so the quad is being drawn and the
// pipeline is running; and it is not uniform noise, so something with structure is being sampled. What is missing is
// everything between the texture sample and the pixel: no register combiner was ever configured, so what reaches the
// screen is whatever those registers happened to hold, and no transform state was set either, while the vertices are
// being pushed in screen coordinates as though one had been.
//
// Both need doing before this can show a picture. Turning it on again is one constant.
//
// It passed: a blue screen, which means pbkit comes up, its back buffer is where the display reads from and its
// swapping works alongside the video mode - once that mode is 32-bit, which is what step one found. The foundation is
// sound and anything that misbehaves from here is the GPU programming rather than the base.
//
// Turn on again for step two, the textured quad.
//
// One thing to fix first: the lines logged in here never reached the relay, because the first present happens before
// networking is up, so they were written into nothing. Step two needs its diagnostics to survive that - either held
// and flushed once the relay connects, or written to the boot log, which is on disk from the start.
static constexpr bool GPU_PRESENT_ENABLED = false;

static bool gbStarted = false;
static bool gbFailed = false;

// VRAM copied somewhere the GPU can actually read it.
//
// 'NV097_SET_TEXTURE_OFFSET' takes a physical address, and the emulated VRAM is a plain 'new uint16_t[]' - paged, and
// not addressable by the GPU at all. Pointing the texture straight at it would sample whatever happens to occupy that
// physical address. So VRAM is copied into contiguous memory each frame and the texture reads that.
//
// This copy is the thing a later step removes, by allocating VRAM itself this way. Worth measuring before doing so:
// around 120KB a frame may well be cheap enough not to bother.
static uint16_t*    gpTextureMem = nullptr;
static uint32_t     gTextureStride = 0;      // In pixels, matching VRAM's own row stride
static uint32_t     gTextureH = 0;

bool isEnabled() noexcept {
    return (GPU_PRESENT_ENABLED && gbStarted);
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Bring pbkit up.
//
// This is the step most likely to fail outright, because pbkit and the game both want a say in the display. pbkit reads
// the mode with 'XVideoGetMode' rather than setting one, so it should take the 640x480 15-bit mode the game already
// established and render into that - but 'should' is why this is being proven on its own before anything is drawn.
//------------------------------------------------------------------------------------------------------------------------------------------
bool init() noexcept {
    if ((!GPU_PRESENT_ENABLED) || gbStarted || gbFailed)
        return gbStarted;

    // pbkit will only run in a 32-bit video mode.
    //
    // Measured, not guessed: it asserts 'vm.bpp == 32' on line 2889 of pbkit.c and takes the console down on boot. The
    // game runs 15-bit, which was itself a CPU optimisation - half the bytes for the CPU to write every frame - and
    // that reasoning does not survive the GPU doing the writing instead. Nothing here reads the framebuffer back, so
    // the depth costs the CPU nothing once it is no longer the one filling it.
    //
    // Done before pb_init and only when the GPU present is being used, so the 15-bit mode stays exactly as it is for
    // the CPU path. If pb_init fails after this, the mode is put back so the fallback is not left in a mode chosen for
    // a path that did not start.
    if (!XVideoSetMode(640, 480, 32, REFRESH_DEFAULT)) {
        gbFailed = true;
        nv2aLog("nv2a present: could not set a 32-bit mode for pbkit, staying on the CPU present");
        return false;
    }

    const int result = pb_init();

    if (result != 0) {
        // Leave the CPU present in charge rather than failing the game, in the mode it was written for
        gbFailed = true;
        XVideoSetMode(640, 480, 15, REFRESH_DEFAULT);
        nv2aLog("nv2a present: pb_init failed with %d, back to 15-bit and the CPU present", result);
        return false;
    }

    pb_show_front_screen();

    gbStarted = true;

    // Everything worth knowing, in case there is nothing to see.
    //
    // If pbkit came up but its buffer is not where the display is reading from, this is what says so - and it says it
    // over the network, which works whether or not anything reaches the television.
    nv2aLog(
        "nv2a present: pbkit up - back buffer %p %ux%u pitch=%u",
        (void*) pb_back_buffer(),
        (unsigned) pb_back_buffer_width(),
        (unsigned) pb_back_buffer_height(),
        (unsigned) pb_back_buffer_pitch()
    );

    return true;
}

void shutdown() noexcept {
    if (!gbStarted)
        return;

    pb_kill();
    gbStarted = false;
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Step one: a colour on the screen, put there through pbkit's buffers.
//
// Written with the CPU on purpose. If this shows the colour then pbkit's initialisation, its back buffer and its
// swapping all work alongside the video mode the game sets, and whatever goes wrong from here is the GPU programming
// rather than the foundation. If it shows nothing, the foundation is the problem and no amount of correct register
// pushing would have shown anything either.
//------------------------------------------------------------------------------------------------------------------------------------------
void presentSolidColour(const uint32_t argb) noexcept {
    if (!isEnabled())
        return;

    pb_wait_for_vbl();
    pb_reset();
    pb_target_back_buffer();

    // pbkit hands this back as DWORD*, which is 'unsigned long' here rather than 'unsigned int'
    uint8_t* const pBackBuffer = (uint8_t*) pb_back_buffer();

    if (pBackBuffer) {
        // Pitch is in bytes and may be wider than the visible area, so fill row by row rather than in one go
        const uint32_t width = pb_back_buffer_width();
        const uint32_t height = pb_back_buffer_height();
        const uint32_t pitch = pb_back_buffer_pitch();

        for (uint32_t y = 0; y < height; ++y) {
            uint32_t* const pRow = (uint32_t*)(pBackBuffer + ((size_t) y * pitch));

            for (uint32_t x = 0; x < width; ++x) {
                pRow[x] = argb;
            }
        }
    }

    while (pb_busy()) {}        // Let the frame finish
    while (pb_finished()) {}    // And swap when it can

    pb_show_front_screen();

    // Once, so the log shows the loop is reached and completes rather than hanging in one of the waits above
    static bool sbReportedFirstFrame = false;

    if (!sbReportedFirstFrame) {
        sbReportedFirstFrame = true;
        nv2aLog("nv2a present: first frame swapped, colour 0x%08X", (unsigned) argb);
    }
}


//------------------------------------------------------------------------------------------------------------------------------------------
// Step two: VRAM on screen as a textured quad.
//------------------------------------------------------------------------------------------------------------------------------------------

// Make sure there is somewhere GPU readable to put VRAM, big enough for it
static bool ensureTextureMem(const uint32_t strideInPixels, const uint32_t height) noexcept {
    if (gpTextureMem && (gTextureStride == strideInPixels) && (gTextureH == height))
        return true;

    if (gpTextureMem) {
        MmFreeContiguousMemory(gpTextureMem);
        gpTextureMem = nullptr;
    }

    const size_t bytes = (size_t) strideInPixels * height * sizeof(uint16_t);

    // Contiguous and uncached: the GPU reads this directly, so it must not sit in the CPU's cache unwritten
    gpTextureMem = (uint16_t*) MmAllocateContiguousMemoryEx(bytes, 0, 0x03FFFFFF, 0, PAGE_READWRITE | PAGE_NOCACHE);

    if (!gpTextureMem) {
        nv2aLog("nv2a present: could not allocate %u bytes of contiguous memory for the texture", (unsigned) bytes);
        return false;
    }

    gTextureStride = strideInPixels;
    gTextureH = height;
    nv2aLog("nv2a present: texture memory %p, %u bytes, stride %u", (void*) gpTextureMem, (unsigned) bytes, (unsigned) strideInPixels);
    return true;
}

void beginFrame() noexcept {
    if (!isEnabled())
        return;

    pb_wait_for_vbl();
    pb_reset();
    pb_target_back_buffer();
}

void drawVramRect(
    const uint32_t srcX,
    const uint32_t srcY,
    const uint32_t srcW,
    const uint32_t srcH,
    const int32_t dstX,
    const int32_t dstY,
    const int32_t dstW,
    const int32_t dstH
) noexcept {
    if (!isEnabled())
        return;

    const Gpu::Core& gpu = PsxVm::gGpu;
    const uint16_t* const pVram = (const uint16_t*) gpu.pRam;

    if ((!pVram) || (srcW == 0) || (srcH == 0))
        return;

    if (!ensureTextureMem(gpu.ramStride, gpu.ramPixelH))
        return;

    // VRAM into memory the GPU can read.
    //
    // Copied whole rather than just the rectangle asked for, so the texture coordinates below can address VRAM in its
    // own coordinates and splitscreen's two calls do not each need their own upload.
    std::memcpy(gpTextureMem, pVram, (size_t) gpu.ramStride * gpu.ramPixelH * sizeof(uint16_t));

    uint32_t* p = pb_begin();

    // Fixed function, and no transforms in the way.
    //
    // Without this the vertices below - which are in screen coordinates - go through whatever transform state happens
    // to be current, which is why the first attempt drew structured static rather than a picture: the quad was being
    // drawn and the texture sampled, but neither ended up where it was meant to.
    p = pb_push1(p, NV097_SET_TRANSFORM_EXECUTION_MODE,
        NV2A_FIELD(NV097_SET_TRANSFORM_EXECUTION_MODE_MODE, NV097_SET_TRANSFORM_EXECUTION_MODE_MODE_FIXED));

    p = pb_push1(p, NV097_SET_TEXTURE_MATRIX_ENABLE, 0);
    p = pb_push1(p, NV097_SET_LIGHTING_ENABLE, 0);
    p = pb_push1(p, NV097_SET_DEPTH_TEST_ENABLE, 0);
    p = pb_push1(p, NV097_SET_CULL_FACE_ENABLE, 0);
    p = pb_push1(p, NV097_SET_BLEND_ENABLE, 0);
    p = pb_push1(p, NV097_SET_ALPHA_TEST_ENABLE, 0);

    // The texture: linear, 16-bit, in the PlayStation's own layout.
    //
    // Linear rather than swizzled is what lets it be VRAM's shape - a wide non-square image with a row stride - which
    // no power of two swizzled format could describe. That is why pitch and image rect appear here in place of the
    // log2 dimensions the swizzled path uses.
    p = pb_push1(p, NV097_SET_TEXTURE_OFFSET, ((uint32_t) gpTextureMem) & 0x03FFFFFFu);

    p = pb_push1(p, NV097_SET_TEXTURE_FORMAT,
        NV2A_FIELD(NV097_SET_TEXTURE_FORMAT_CONTEXT_DMA,    2) |
        NV2A_FIELD(NV097_SET_TEXTURE_FORMAT_COLOR,          NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A1R5G5B5) |
        NV2A_FIELD(NV097_SET_TEXTURE_FORMAT_DIMENSIONALITY, 2) |
        NV2A_FIELD(NV097_SET_TEXTURE_FORMAT_MIPMAP_LEVELS,  1));

    p = pb_push1(p, NV097_SET_TEXTURE_CONTROL1,
        NV2A_FIELD(NV097_SET_TEXTURE_CONTROL1_IMAGE_PITCH, gpu.ramStride * sizeof(uint16_t)));

    p = pb_push1(p, NV097_SET_TEXTURE_IMAGE_RECT,
        NV2A_FIELD(NV097_SET_TEXTURE_IMAGE_RECT_WIDTH,  gpu.ramStride) |
        NV2A_FIELD(NV097_SET_TEXTURE_IMAGE_RECT_HEIGHT, gpu.ramPixelH));

    p = pb_push1(p, NV097_SET_TEXTURE_CONTROL0,
        NV097_SET_TEXTURE_CONTROL0_ENABLE |
        NV2A_FIELD(NV097_SET_TEXTURE_CONTROL0_MIN_LOD_CLAMP, 0) |
        NV2A_FIELD(NV097_SET_TEXTURE_CONTROL0_MAX_LOD_CLAMP, 1));

    // Nearest, both ways. Byte identical output depends on this: any filtering and the pixels stop being the ones the
    // rasteriser produced, which is the whole promise of doing the scaling on the GPU.
    p = pb_push1(p, NV097_SET_TEXTURE_FILTER,
        0x2000 |
        NV2A_FIELD(NV097_SET_TEXTURE_FILTER_MIN, 1) |
        NV2A_FIELD(NV097_SET_TEXTURE_FILTER_MAG, 1));

    p = pb_push1(p, NV097_SET_TEXTURE_ADDRESS, 0x00030303);   // Clamp on every axis

    // The combiner: put the texture on screen, with red and blue exchanged.
    //
    // This is the output path, and leaving it out is why nothing recognisable appeared - not merely a wrong colour, as
    // I had expected, but no defined result at all, since without a combiner whatever these registers already held is
    // what reaches the pixel.
    //
    // The swap is here because the PlayStation packs blue high and red low and no 15-bit texture format has that
    // order. Doing it as part of the sample costs nothing per pixel, which is the whole reason the CPU no longer has
    // to make a converted copy of every row.
    // One stage. Zero is not 'a single default stage', it is no stages at all, and the GPU rejects it outright -
    // 'invalid data error' naming this very register, 0x1E60, which is how this was found.
    p = pb_push1(p, NV097_SET_COMBINER_CONTROL,
        NV2A_FIELD(NV097_SET_COMBINER_CONTROL_ITERATION_COUNT, NV097_SET_COMBINER_CONTROL_ITERATION_COUNT_ONE));

    // Stage zero: A = texture, B = one. Sources are named by channel, so asking for blue where red is expected is the
    // swap - nothing is computed to achieve it.
    p = pb_push1(p, NV097_SET_COMBINER_COLOR_ICW,
        NV2A_FIELD(NV097_SET_COMBINER_COLOR_ICW_A_SOURCE, 8) |    // Texture 0
        NV2A_FIELD(NV097_SET_COMBINER_COLOR_ICW_A_MAP,    0) |
        NV2A_FIELD(NV097_SET_COMBINER_COLOR_ICW_B_SOURCE, 0) |    // Zero, mapped to one below
        NV2A_FIELD(NV097_SET_COMBINER_COLOR_ICW_B_MAP,    2));

    p = pb_push1(p, NV097_SET_COMBINER_COLOR_OCW, 0x00000C00);

    // The final combiner takes the stage's result to the framebuffer, swapping the channels as it goes
    p = pb_push1(p, NV097_SET_COMBINER_SPECULAR_FOG_CW0, 0x00000000);
    p = pb_push1(p, NV097_SET_COMBINER_SPECULAR_FOG_CW1, 0x00000C00);

    pb_end(p);

    // The quad itself, as four vertices pushed straight into the command stream.
    //
    // Texture coordinates are in texels rather than normalised, which suits addressing VRAM by its own pixel
    // coordinates and means the two splitscreen views differ only in the numbers below.
    const float u0 = (float) srcX;
    const float v0 = (float) srcY;
    const float u1 = (float)(srcX + srcW);
    const float v1 = (float)(srcY + srcH);

    const float x0 = (float) dstX;
    const float y0 = (float) dstY;
    const float x1 = (float)(dstX + dstW);
    const float y1 = (float)(dstY + dstH);

    const float verts[4][4] = {
        { x0, y0, u0, v0 },
        { x1, y0, u1, v0 },
        { x1, y1, u1, v1 },
        { x0, y1, u0, v1 },
    };

    p = pb_begin();
    p = pb_push1(p, NV097_SET_BEGIN_END, NV097_SET_BEGIN_END_OP_QUADS);

    for (int32_t i = 0; i < 4; ++i) {
        // Texture coordinate first, position last.
        //
        // Writing the position is what latches the vertex, so everything else has to be set before it. Done the other
        // way round - which is how this was first written - each vertex takes whatever texture coordinate was left
        // over from the one before, and the first takes whatever happened to be in the register. That draws the quad
        // correctly and samples the texture at meaningless places, which is exactly the structured static seen.
        p = pb_push2f(p, NV097_SET_TEXCOORD0_2F, verts[i][2], verts[i][3]);
        p = pb_push4f(p, NV097_SET_VERTEX4F, verts[i][0], verts[i][1], 0.0f, 1.0f);
    }

    p = pb_push1(p, NV097_SET_BEGIN_END, NV097_SET_BEGIN_END_OP_END);
    pb_end(p);
}

void endFrame() noexcept {
    if (!isEnabled())
        return;

    while (pb_busy()) {}
    while (pb_finished()) {}

    pb_show_front_screen();
}

END_NAMESPACE(XboxNv2aPresent)

#endif  // #if defined(__XBOX__)
