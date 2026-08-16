#include "VideoBackend_SDL.h"

#include "Asserts.h"
#include "Config/Config.h"
#include "Gpu.h"
#include "PsxVm.h"
#include "Video.h"
#include "VideoSurface_SDL.h"

#include <algorithm>
#include <cmath>
#include <SDL.h>

#if defined(__XBOX__)
#include "Splitscreen.h"
#include "XboxDiag.h"
#include "XboxLog.h"
#include "Doom/Base/i_texcache.h"
#include "Doom/Base/z_zone.h"
#include "Wess/psxcmd.h"
#include "Wess/wessarc.h"
#include "Doom/Game/p_tick.h"

#include <hal/video.h>
#include <hal/audio.h>

//------------------------------------------------------------------------------------------------------------------------------------------
// Present straight into the Xbox framebuffer rather than through SDL.
//
// Left as a switch because this is the first real change to how the picture reaches the screen, and if it misbehaves on
// hardware the old path is one definition away rather than a revert. Set to 0 to go back to the SDL present.
//------------------------------------------------------------------------------------------------------------------------------------------
#if defined(__XBOX__)
    // Microseconds of the last frame spent writing into the framebuffer, as opposed to building the rows to write
    static uint64_t gXbFbWriteMicros = 0;

    // ...and how much of it went on drawing the debug overlay
    static uint64_t gXbOverlayMicros = 0;

    // Gathered across each second for the performance line.
    //
    // These carry what the on-screen readout used to show, plus the thing it could not: the worst frame of the second.
    // A number that only updates once a second cannot show a stall, and a stall is exactly what is being chased.
    static uint32_t gXbSecFrames = 0;
    static uint64_t gXbSecTotalUs = 0;
    static uint64_t gXbSecDirectUs = 0;
    static uint64_t gXbSecRenderUs = 0;
    static uint64_t gXbSecPaceUs = 0;
    static uint64_t gXbSecOtherUs = 0;

    // The present, split into the write to the framebuffer and everything else - which is the colour conversion and
    // the scaling. A GPU blit removes the conversion entirely and does the write itself, so this says what is
    // actually on the table before committing to that rewrite.
    static uint64_t gXbSecFbWriteUs = 0;
    static uint64_t gXbSecStatusBarUs = 0;      // The status bar, drawn every frame and twice in splitscreen
    static uint64_t gXbSecInputUs = 0;          // The input update, USB polling included
    static uint64_t gXbSecWorstUs = 0;
    static uint64_t gXbSecBestUs = 0;
#endif

#if !defined(XBOX_DIRECT_FRAMEBUFFER_PRESENT)
    #define XBOX_DIRECT_FRAMEBUFFER_PRESENT 1
#endif

//------------------------------------------------------------------------------------------------------------------------------------------
// Where a frame's time actually goes.
//
// The game reports around 360ms a frame. The theory is that little of that is the emulated PlayStation GPU drawing the
// scene, and most of it is what happens afterwards: every pixel is touched three separate times, on the CPU, because
// the SDL here has only its software renderer. Once to turn 256x240 sixteen bit into 512x480 thirty-two bit, again by
// SDL_RenderCopy, and again by SDL_RenderPresent.
//
// That is a theory, and rewriting the video path on the strength of one would be exactly the mistake the 3DS port kept
// teaching. So each stage is timed and sent out, and the numbers decide.
//
// The cost of measuring is two clock reads per stage, against stages measured in milliseconds.
//------------------------------------------------------------------------------------------------------------------------------------------
static uint64_t gXbPrevFrameEndMicros = 0;      // For the frame to frame interval
static uint64_t gXbPrescaleMicros = 0;          // 256x240 16-bit -> 512x480 32-bit, with the 2x2 expansion
static uint32_t gXbFrameNum = 0;
#include <cstdint>
#include <cstring>
#endif


#if defined(__XBOX__)
//------------------------------------------------------------------------------------------------------------------------------------------
// Present a rectangle of PlayStation VRAM into a rectangle of the 15-bit framebuffer, at any scale.
//
// The full screen present is a fixed 2x2 expansion, which is all single player ever needs. Splitscreen needs neither
// axis fixed: side by side puts a 256x240 view into 320x240, which is 1.25x across and 1x down, and top and bottom
// puts it into 640x240, which is 2.5x across and 1x down. So both axes step in 16.16 fixed point, one add per output
// pixel, and the single player case falls out of it as the particular case where both steps are a half.
//
// The source is read once per output row rather than per output pixel, into a cached row buffer, and only then written
// to the framebuffer. That ordering is what makes write combining work: the framebuffer sees one long sequential run
// per row instead of scattered stores.
//
// GAMECUBE: does not port. The framebuffer here is A1R5G5B5 and that console's is YUV.
//------------------------------------------------------------------------------------------------------------------------------------------
// Adds the elapsed time to a total when it goes out of scope
struct FinallyBlitTimer {
    uint64_t    start;
    uint64_t&   totalInOut;

    ~FinallyBlitTimer() noexcept {
        totalInOut += SDL_GetPerformanceCounter() - start;
    }
};

static void presentVramRect(
    uint16_t* const pDstBase,
    const int32_t dstStride,
    const Gpu::Color16* const vramPixels,
    const uint32_t vramStride,
    const uint32_t srcX,
    const uint32_t srcY,
    const uint32_t srcW,
    const uint32_t srcH,
    const int32_t dstX,
    const int32_t dstY,
    const int32_t dstW,
    const int32_t dstH,
    uint64_t& fbWriteTicksInOut
) noexcept {
    if ((dstW <= 0) || (dstH <= 0) || (srcW == 0) || (srcH == 0))
        return;

    // Timed as a whole rather than per row. Two timer reads per row was 960 a frame in the innermost loop, each one a
    // compiler barrier around the memcpy it wrapped, so nothing could be pipelined across rows. 'fbwrite' now means
    // the whole blit - conversion included - rather than only the copies, so it is no longer comparable to the
    // figures from before this change.
    const uint64_t blitStart = SDL_GetPerformanceCounter();
    const auto blitTimeGuard = FinallyBlitTimer{ blitStart, fbWriteTicksInOut };

    // How far to step through the source for each output pixel
    const uint32_t xStepFixed = (uint32_t)(((uint64_t) srcW << 16) / (uint32_t) dstW);
    const uint32_t yStepFixed = (uint32_t)(((uint64_t) srcH << 16) / (uint32_t) dstH);

    // One output row, built in cached memory before it goes anywhere near the framebuffer
    static uint16_t rowBuf[1024];

    const int32_t rowLen = (dstW <= 1024) ? dstW : 1024;

    uint32_t srcYFixed = 0;

    // Exactly two output pixels per source pixel: convert once and store the pair in a single 32-bit write.
    //
    // This is the full size single player view, 256 across drawn at 512. Halves both the conversion work and the
    // number of stores against stepping the source a pixel at a time.
    const bool bDoubleWidth = ((xStepFixed == 0x8000u) && ((rowLen & 1) == 0));

    for (int32_t y = 0; y < dstH; ++y) {
        const uint32_t sy = srcY + (srcYFixed >> 16);
        srcYFixed += yStepFixed;

        const Gpu::Color16* const pSrcRow = vramPixels + ((size_t) sy * vramStride) + srcX;
        uint32_t srcXFixed = 0;

        if (bDoubleWidth) {
            // One source pixel per pair of output pixels, written together
            uint32_t* const pRowPairs = (uint32_t*) rowBuf;
            const int32_t numPairs = rowLen / 2;

            for (int32_t x = 0; x < numPairs; ++x) {
                const uint16_t srcBits = pSrcRow[x].bits;

                const uint32_t out = (uint32_t)(
                    0x8000u |
                    ((srcBits & 0x001Fu) << 10) |
                    (srcBits & 0x03E0u) |
                    ((srcBits >> 10) & 0x001Fu)
                );

                pRowPairs[x] = out | (out << 16);
            }
        }
        else {
            for (int32_t x = 0; x < rowLen; ++x) {
                const uint16_t srcBits = pSrcRow[srcXFixed >> 16].bits;
                srcXFixed += xStepFixed;

                // PlayStation packs blue high and red low; the Xbox wants the opposite, with the top bit set opaque
                rowBuf[x] = (uint16_t)(
                    0x8000u |
                    ((srcBits & 0x001Fu) << 10) |
                    (srcBits & 0x03E0u) |
                    ((srcBits >> 10) & 0x001Fu)
                );
            }
        }

        const int32_t screenY = dstY + y;
        XboxDiag::mergeOverlayIntoRow15(rowBuf, rowLen, screenY, dstX);

        uint16_t* const pRow = pDstBase + ((intptr_t) screenY * dstStride) + dstX;

        std::memcpy(pRow, rowBuf, (size_t) rowLen * sizeof(uint16_t));
    }
}
#endif

BEGIN_NAMESPACE(Video)

//------------------------------------------------------------------------------------------------------------------------------------------
// Creates the backend with the SDL renderer uninitialized
//------------------------------------------------------------------------------------------------------------------------------------------
VideoBackend_SDL::VideoBackend_SDL() noexcept 
    : mpSdlWindow(nullptr)
    , mpRenderer(nullptr)
    , mpFramebufferTexture(nullptr)
    , mpFramebufferPixels(nullptr)
    , mFbTexW(0)
    , mFbTexH(0)
    , mFbTexPitch(0)
{
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Ensures everything is cleaned up
//------------------------------------------------------------------------------------------------------------------------------------------
VideoBackend_SDL::~VideoBackend_SDL() noexcept {
    destroyRenderers();
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Get the window create flags required for an SDL video backend window
//------------------------------------------------------------------------------------------------------------------------------------------
uint32_t VideoBackend_SDL::getSdlWindowCreateFlags() noexcept {
    // Use OpenGL where it is supported since that is the main implementation for SDL renderer.
    // On MacOS it's better to use Metal however since OpenGL is deprecated.
    // On Xbox/nxdk, use SHOWN with no rendering API flag since there is no GL runtime.
    #if defined(__XBOX__)
        return SDL_WINDOW_SHOWN;
    #elif __APPLE__
        return SDL_WINDOW_METAL;
    #else
        return SDL_WINDOW_OPENGL;
    #endif
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Initializes the SDL renderer used by this backend
//------------------------------------------------------------------------------------------------------------------------------------------
void VideoBackend_SDL::initRenderers(SDL_Window* const pSdlWindow) noexcept {
    ASSERT(pSdlWindow);

    // Must not already be initialized
    ASSERT(!mpRenderer);
    ASSERT(!mpFramebufferTexture);
    ASSERT(!mpFramebufferPixels);

    // Create the renderer and framebuffer texture
    mpSdlWindow = pSdlWindow;
#if defined(__XBOX__)
    // nxdk SDL2 only has the software renderer compiled in; ACCELERATED flag causes NULL return.
    const Uint32 rendererFlags = SDL_RENDERER_SOFTWARE;
    (void) Config::gbEnableVSync;  // vsync not applicable to software renderer
#else
    const Uint32 vsyncFlag = (Config::gbEnableVSync) ? SDL_RENDERER_PRESENTVSYNC : 0;
    const Uint32 rendererFlags = SDL_RENDERER_ACCELERATED | vsyncFlag;
#endif
    mpRenderer = SDL_CreateRenderer(pSdlWindow, -1, rendererFlags);

    if (!mpRenderer) {
        FatalErrors::raise("Failed to create renderer!");
    }

    // Xbox: create a 2x pre-scaled texture (512x480) so SDL_RenderCopy can do a 1:1 blit
    // instead of a slow software upscale from 256x240. This is the main render performance win.
#if defined(__XBOX__)
    mFbTexW = ORIG_DRAW_RES_X * 2;   // 512
    mFbTexH = ORIG_DRAW_RES_Y * 2;   // 480
#else
    mFbTexW = ORIG_DRAW_RES_X;       // 256
    mFbTexH = ORIG_DRAW_RES_Y;       // 240
#endif

    mpFramebufferTexture = SDL_CreateTexture(
        mpRenderer,
#if defined(__XBOX__)
        SDL_PIXELFORMAT_RGB888,         // nxdk window surface is RGB888 - no conversion needed
#else
        SDL_PIXELFORMAT_ABGR8888,
#endif
        SDL_TEXTUREACCESS_STREAMING,
        mFbTexW,
        mFbTexH
    );

    if (!mpFramebufferTexture) {
        FatalErrors::raise("Failed to create a framebuffer texture!");
    }

    // Clear the renderer to black
    SDL_SetRenderDrawColor(mpRenderer, 0, 0, 0, 0);
    SDL_RenderClear(mpRenderer);

    // Immediately lock the framebuffer texture in preparation for the next update
    lockFramebufferTexture();
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Cleans up and destroys the SDL renderer used by this video backend
//------------------------------------------------------------------------------------------------------------------------------------------
void VideoBackend_SDL::destroyRenderers() noexcept {
    if (mpFramebufferPixels) {
        unlockFramebufferTexture();
    }

    if (mpFramebufferTexture) {
        SDL_DestroyTexture(mpFramebufferTexture);
        mpFramebufferTexture = nullptr;
    }

    if (mpRenderer) {
        SDL_DestroyRenderer(mpRenderer);
        mpRenderer = nullptr;
    }

    mpSdlWindow = nullptr;
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Copies the output from the classic renderer (PSX framebuffer) to an SDL texture and then blits that to the screen.
//------------------------------------------------------------------------------------------------------------------------------------------
void VideoBackend_SDL::displayFramebuffer() noexcept {
    // Note: the direct path reads the PlayStation framebuffer itself, so the copy into the SDL texture is not merely
    // redundant there - leaving it in would still cost its 38ms a frame to fill a texture nothing then looks at.
    #if defined(__XBOX__) && XBOX_DIRECT_FRAMEBUFFER_PRESENT
        presentSdlFramebufferTexture();
    #else
        copyPsxToSdlFramebufferTexture();
        presentSdlFramebufferTexture();
    #endif
}

//------------------------------------------------------------------------------------------------------------------------------------------
// These functions are no-ops for the SDL backend.
// Don't need to do anything special to display an external surface at any given time.
//------------------------------------------------------------------------------------------------------------------------------------------
void VideoBackend_SDL::beginExternalSurfaceDisplay() noexcept {}
void VideoBackend_SDL::endExternalSurfaceDisplay() noexcept {}

//------------------------------------------------------------------------------------------------------------------------------------------
// Displays the specified surface to the screen
//------------------------------------------------------------------------------------------------------------------------------------------
void VideoBackend_SDL::displayExternalSurface(
    IVideoSurface& surface,
    const int32_t displayX,
    const int32_t displayY,
    const uint32_t displayW,
    const uint32_t displayH,
    const bool bUseFiltering
) noexcept {
    // Must be an SDL video surface!
    VideoSurface_SDL& sdlSurface = static_cast<VideoSurface_SDL&>(surface);

    // Decide source and destination rectangles
    SDL_Rect srcRect = {};
    SDL_Rect dstRect = {};
    dstRect.x = displayX;
    dstRect.y = displayY;
    dstRect.w = (int) displayW;
    dstRect.h = (int) displayH;

    // Clear the screen and blit the surface to the display using the specified scaling.
    // If there is no valid texture then just clear the screen.
    SDL_RenderClear(mpRenderer);
    SDL_Texture* const pSdlTexture = sdlSurface.getTexture();

#if defined(__XBOX__)
    // On Xbox: query the actual texture dimensions (2x pre-scaled) for srcRect so SDL_RenderCopy
    // can do a near-1:1 blit instead of slower software scaling from the logical resolution.
    if (pSdlTexture) {
        int tw = 0, th = 0;
        SDL_QueryTexture(pSdlTexture, nullptr, nullptr, &tw, &th);
        srcRect.w = tw;
        srcRect.h = th;
    } else {
        srcRect.w = (int) sdlSurface.getWidth();
        srcRect.h = (int) sdlSurface.getHeight();
    }
#else
    srcRect.w = (int) sdlSurface.getWidth();
    srcRect.h = (int) sdlSurface.getHeight();
#endif

    if (pSdlTexture) {
#if !defined(__XBOX__)
        SDL_SetTextureScaleMode(pSdlTexture, (bUseFiltering) ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
#else
        (void) bUseFiltering;
#endif
        SDL_RenderCopy(mpRenderer, pSdlTexture, &srcRect, &dstRect);
    }

    // Present the rendered frame
    SDL_RenderPresent(mpRenderer);
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Gives the size of the swapchain/window in pixels
//------------------------------------------------------------------------------------------------------------------------------------------
void VideoBackend_SDL::getScreenSizeInPixels(uint32_t& width, uint32_t& height) noexcept {
    int sdlWidth = 0;
    int sdlHeight = 0;

    if (mpRenderer) {
        if (SDL_GetRendererOutputSize(mpRenderer, &sdlWidth, &sdlHeight) != 0) {
            // Just to be safe, clear these again on an error...
            sdlWidth = 0;
            sdlHeight = 0;
        }
    }

    width = sdlWidth;
    height = sdlHeight;
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Creates and returns an SDL format video surface.
// Fails if renderers have not been initialized.
//------------------------------------------------------------------------------------------------------------------------------------------
std::unique_ptr<IVideoSurface> VideoBackend_SDL::createSurface(const uint32_t width, const uint32_t height) noexcept {
    return (mpRenderer) ? std::make_unique<VideoSurface_SDL>(*mpRenderer, width, height) : std::unique_ptr<IVideoSurface>();
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Lock the SDL texture we upload the PSX framebuffer to for writing
//------------------------------------------------------------------------------------------------------------------------------------------
void VideoBackend_SDL::lockFramebufferTexture() noexcept {
    ASSERT(mpFramebufferTexture);
    ASSERT(!mpFramebufferPixels);

    int pitch = 0;

    if (SDL_LockTexture(mpFramebufferTexture, nullptr, reinterpret_cast<void**>(&mpFramebufferPixels), &pitch) != 0) {
        FatalErrors::raise("Failed to lock the framebuffer texture for writing!");
    }

    mFbTexPitch = pitch;
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Unlock the SDL texture containing the PSX framebuffer after we finish writing to it
//------------------------------------------------------------------------------------------------------------------------------------------
void VideoBackend_SDL::unlockFramebufferTexture() noexcept {
    ASSERT(mpFramebufferPixels);
    ASSERT(mpFramebufferTexture);

    SDL_UnlockTexture(mpFramebufferTexture);
    mpFramebufferPixels = nullptr;
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Copies the rendered PSX GPU framebuffer the locked SDL texture, in preparation for blitting to the screen
//------------------------------------------------------------------------------------------------------------------------------------------
void VideoBackend_SDL::copyPsxToSdlFramebufferTexture() noexcept {
    // Sanity checks
    ASSERT(mpFramebufferPixels);

#if defined(__XBOX__)
    const uint64_t xbPrescaleStart = XboxLog::nowMicros();
#endif

    // Copy the framebuffer
    Gpu::Core& gpu = PsxVm::gGpu;
    const Gpu::Color16* const vramPixels = reinterpret_cast<const Gpu::Color16*>(gpu.pRam);

#if defined(__XBOX__)
    // Xbox: write each PSX pixel as a 2x2 block directly into the 512x480 texture.
    // SDL_RenderCopy will then do a fast 1:1 blit with no software upscaling needed.
    const int stride = mFbTexPitch / (int)sizeof(uint32_t);  // pixels per row (should be 512)
    const uint32_t xStart = (uint32_t)gpu.displayAreaX;
    const uint32_t xEnd   = xStart + ORIG_DRAW_RES_X;

    for (uint32_t y = 0; y < ORIG_DRAW_RES_Y; ++y) {
        const Gpu::Color16* const rowPixels = vramPixels + ((intptr_t)y + gpu.displayAreaY) * gpu.ramStride;
        uint32_t* pRow0 = mpFramebufferPixels + (y * 2) * stride;
        uint32_t* pRow1 = pRow0 + stride;

        for (uint32_t x = xStart, ox = 0; x < xEnd; ++x, ox += 2) {
            const Gpu::Color16 srcPixel = rowPixels[x];
            const uint32_t r = (uint32_t)srcPixel.getR() << 3;
            const uint32_t g = (uint32_t)srcPixel.getG() << 3;
            const uint32_t b = (uint32_t)srcPixel.getB() << 3;
            const uint32_t px = (r << 16) | (g << 8) | b;  // RGB888 matches nxdk native window format
            // Pack two identical pixels into a 64-bit value and write both rows in two stores
            // instead of four.  This halves the number of memory writes per pixel (128 vs 256
            // per 256-wide scanline pair) — measurable on the Pentium III's narrow store pipeline.
            const uint64_t ppx = ((uint64_t)px << 32) | px;
            std::memcpy(&pRow0[ox], &ppx, sizeof(ppx));
            std::memcpy(&pRow1[ox], &ppx, sizeof(ppx));
        }
    }
    // Write diagnostic overlay directly into the locked texture pixels (no SDL API calls)
    XboxDiag::setTexCachePage(I_GetCurTexCacheFillPage());
    XboxDiag::writeToFramePixels(mpFramebufferPixels, mFbTexPitch);

    // How long that took. Reported with the rest of the split by 'presentSdlFramebufferTexture'.
    gXbPrescaleMicros = XboxLog::nowMicros() - xbPrescaleStart;
#else
    uint32_t* pDstPixel = mpFramebufferPixels;

    for (uint32_t y = 0; y < ORIG_DRAW_RES_Y; ++y) {
        const Gpu::Color16* const rowPixels = vramPixels + ((intptr_t) y + gpu.displayAreaY) * gpu.ramStride;
        const uint32_t xStart = (uint32_t) gpu.displayAreaX;
        const uint32_t xEnd = xStart + ORIG_DRAW_RES_X;
        ASSERT(xEnd <= gpu.ramPixelW);

        // Note: don't bother doing multiple pixels at a time - compiler is smart and already optimizes this to use SIMD
        for (uint32_t x = xStart; x < xEnd; ++x, ++pDstPixel) {
            const Gpu::Color16 srcPixel = rowPixels[x];
            const uint32_t r = (uint32_t) srcPixel.getR() << 3;
            const uint32_t g = (uint32_t) srcPixel.getG() << 3;
            const uint32_t b = (uint32_t) srcPixel.getB() << 3;

            *pDstPixel = (0xFF000000 | (b << 16) | (g << 8 ) | (r << 0));
        }
    }
#endif
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Presents the SDL texture which contains a rendered frame from the PSX GPU to the screen
//------------------------------------------------------------------------------------------------------------------------------------------
void VideoBackend_SDL::presentSdlFramebufferTexture() noexcept {
    // Sanity checks
    ASSERT(mpSdlWindow);
    ASSERT(mpRenderer);
    ASSERT(mpFramebufferTexture);

#if defined(__XBOX__) && XBOX_DIRECT_FRAMEBUFFER_PRESENT
    //--------------------------------------------------------------------------------------------------------------
    // Straight from the PlayStation framebuffer to the Xbox one, in a single pass.
    //
    // What this replaces is two full passes over a quarter of a million pixels, both on the CPU:
    //
    //   1. A 2x2 expansion of 256x240 sixteen bit into a 512x480 thirty-two bit SDL texture. Measured at 38ms.
    //   2. SDL's own present, which on this platform is 'SDL_ConvertPixels' from that texture into the framebuffer
    //      returned by XVideoGetFB, converting format as it goes. Measured at 44ms.
    //
    // Step 2 is the whole of what SDL does here - there is no GPU involved anywhere in it - so writing where it would
    // have written, ourselves, once, costs nothing that was not already being paid twice over.
    //
    // The scale stays 2x with the image centred and black bars either side, exactly as before: 640 is not a whole
    // multiple of 256 and stretching to fill would distort the picture as well as costing more.
    //--------------------------------------------------------------------------------------------------------------
    const uint64_t xbDirectStart = XboxLog::nowMicros();

    const VIDEO_MODE videoMode = XVideoGetMode();
    uint8_t* const pFrameBuffer = XVideoGetFB();

    //--------------------------------------------------------------------------------------------------------------
    // 15-bit present: half the bytes, and not one pixel different.
    //
    // PlayStation VRAM is 15-bit - five bits a channel plus the mask bit - and the 32-bit path below expands each
    // channel to eight bits by shifting it up three. That adds no information whatsoever; it is the same colour
    // written in twice the space. At 512x480 it is 983 KB pushed into write-combined memory every frame, measured at
    // 14.3ms, against 491 KB here.
    //
    // A1R5G5B5 holds the source exactly, so this is not a reduction in quality - the picture is bit for bit what the
    // emulated console produced, which is more than can be said for widening it and narrowing it again on the way to
    // a 5-bit-per-channel display signal anyway.
    //
    // The conversion is cheaper too: red and blue swap places and the top bit is set, rather than three shifts and
    // three masks assembled into a 32-bit word.
    //
    // GAMECUBE: the idea carries, the code does not. Its framebuffer is YUV rather than any RGB layout, so the
    // equivalent saving there is a different conversion entirely.
    //--------------------------------------------------------------------------------------------------------------
    if (pFrameBuffer && ((videoMode.bpp == 15) || (videoMode.bpp == 16))) {
        Gpu::Core& gpu = PsxVm::gGpu;
        const Gpu::Color16* const vramPixels = reinterpret_cast<const Gpu::Color16*>(gpu.pRam);

        const int32_t dstStride = videoMode.width;
        const int32_t screenW = videoMode.width;
        const int32_t screenH = videoMode.height;
        const uint32_t xStart = (uint32_t) gpu.displayAreaX;
        const uint32_t yStart = (uint32_t) gpu.displayAreaY;

        uint16_t* const pDstBase = reinterpret_cast<uint16_t*>(pFrameBuffer);
        uint64_t fbWriteTicks = 0;

        XboxDiag::setTexCachePage(I_GetCurTexCacheFillPage());

        // Keep the overlay over player one's view, in the place it would sit in a full size one.
        //
        // It merges into picture rows as they are written, so it has to be inside a region that is actually drawn.
        // Side by side leaves the top of the screen black, and the fixed position sits up there - no row passes
        // through it, so nothing merged and the overlay did not appear at all.
        {
            const bool bSplitViewports = (Splitscreen::isActive() && (!Splitscreen::isFullScreenSequence()));

            if (bSplitViewports) {
                const int32_t fullW = (int32_t)(ORIG_DRAW_RES_X * 2);        // Where a full size view sits, and how big
                const int32_t fullH = (int32_t)(ORIG_DRAW_RES_Y * 2);
                const int32_t fullX = (screenW - fullW) / 2;
                const int32_t fullY = (screenH - fullH) / 2;

                int32_t viewX, viewY, viewW, viewH;

                if (Splitscreen::getLayout() == Splitscreen::Layout::SideBySide) {
                    viewW = screenW / 2;
                    viewH = (viewW * 3) / 4;
                    viewX = 0;
                    viewY = (screenH - viewH) / 2;
                } else {
                    viewW = screenW;
                    viewH = screenH / 2;
                    viewX = 0;
                    viewY = 0;
                }

                // The same fraction across player one's view as it occupies across a full one
                const int32_t relX = XboxDiag::OVL_X - fullX;
                const int32_t relY = XboxDiag::OVL_Y - fullY;

                XboxDiag::setOverlayOrigin(
                    viewX + (relX * viewW) / fullW,
                    viewY + (relY * viewH) / fullH
                );
            } else {
                XboxDiag::setOverlayOrigin(XboxDiag::OVL_X, XboxDiag::OVL_Y);
            }
        }

        const uint64_t overlayStart15 = XboxLog::nowMicros();
        XboxDiag::renderOverlay();
        gXbOverlayMicros = XboxLog::nowMicros() - overlayStart15;

        // Clear the screen once when it needs it, NOT every frame.
        //
        // This was the flicker. 'XVideoGetFB' hands back a single framebuffer - there is no back buffer to build the
        // frame in - and this blanked the whole thing on every present. The display scans out at 60Hz while the game
        // presents at around 20 and the blits below take several milliseconds, so the screen was repeatedly read after
        // being wiped black and before being refilled. That shows up as the whole screen flickering, both viewports at
        // once and in step, which is exactly what it looked like. Single player never memsets, which is why only
        // splitscreen flickered and why no amount of fixing the rendering ever moved it.
        //
        // Clearing per frame was never needed: each viewport blit covers its own region completely, so nothing of the
        // previous frame survives inside them, and the bars around them are static black. One clear when splitscreen
        // starts, when the layout changes, and when it ends is enough.
        if (Splitscreen::consumeScreenClearRequest()) {
            std::memset(pDstBase, 0, (size_t) dstStride * (size_t) screenH * sizeof(uint16_t));
        }

        // Once splitscreen is on, always two viewports - and always with the most recent capture.
        //
        // The previous attempt made this conditional on the frame having drawn a pair, so that menus and plaques got
        // the full screen view. That was worse, and is what the flicker is: the present is reached from plenty of
        // places that do not draw the world twice, so the screen alternated between a two viewport layout and a full
        // screen one, frame by frame. Changing layout every other frame reads as violent flicker no matter how correct
        // each individual frame is.
        //
        // Holding the last capture instead means the layout never changes while splitscreen is on. A frame that did
        // not draw a fresh view for player one shows their previous one, which at worst is a frame stale and at best
        // is identical - and either is invisible next to the layout jumping about.
        // Each player's view lives in its own half of VRAM and is drawn once per frame, so there is nothing to be
        // stale and no condition to test. Every previous variation copied one view out of a shared buffer, and each
        // was wrong in a different way.
        // Not while a full screen sequence is running: the text screens and the cast call are one thing shown to
        // both players, and splitting them leaves the other half holding stale pixels beside what is being read.
        const bool bDrawTwoViewports = (Splitscreen::isActive() && (!Splitscreen::isFullScreenSequence()));

        if (!bDrawTwoViewports) {
            // Single view: the whole screen, at the 2x scale the game has always used, centred with bars either side
            const int32_t viewW = (int32_t)(ORIG_DRAW_RES_X * 2);
            const int32_t viewH = (int32_t)(ORIG_DRAW_RES_Y * 2);

            // A full screen sequence during splitscreen draws into player one's half, and the framebuffers do not
            // swap while splitscreen is on, so read from there rather than from the display area
            const bool bFullScreenSeq = Splitscreen::isFullScreenSequence();
            const int32_t srcX = (bFullScreenSeq) ? Splitscreen::VIEW_VRAM_X[0] : xStart;
            const int32_t srcY = (bFullScreenSeq) ? Splitscreen::VIEW_VRAM_Y[0] : yStart;

            presentVramRect(
                pDstBase, dstStride, vramPixels, gpu.ramStride,
                srcX, srcY, ORIG_DRAW_RES_X, ORIG_DRAW_RES_Y,
                (screenW - viewW) / 2, (screenH - viewH) / 2, viewW, viewH,
                fbWriteTicks
            );
        }
        else if (Splitscreen::getLayout() == Splitscreen::Layout::SideBySide) {
            // Two 4:3 viewports side by side.
            //
            // Half the width each, and a height that keeps them 4:3 against that width, centred vertically with bars
            // above and below. A 256x240 view shown in a 4:3 region is not stretched - PlayStation pixels are not
            // square, and filling a 4:3 frame is how the game is meant to look.
            const int32_t viewW = screenW / 2;
            const int32_t viewH = (viewW * 3) / 4;
            const int32_t viewY = (screenH - viewH) / 2;

            // Each player read straight from their own half of VRAM. Nothing copied, nothing shared.
            for (int32_t player = 0; player < 2; ++player) {
                presentVramRect(
                    pDstBase, dstStride, vramPixels, gpu.ramStride,
                    Splitscreen::VIEW_VRAM_X[player], Splitscreen::VIEW_VRAM_Y[player],
                    ORIG_DRAW_RES_X, ORIG_DRAW_RES_Y,
                    viewW * player, viewY, viewW, viewH,
                    fbWriteTicks
                );
            }
        }
        else {
            // One above the other, each the full width and half the height.
            //
            // Nothing is wasted, but the region is far wider than 4:3, so this is only honest once the view is drawn
            // wider to match - see 'Config::gLogicalDisplayW' and the note in the splitscreen plan. Until then it fills
            // the region from a 4:3 view, which is the one place in this port something is knowingly stretched.
            const int32_t viewW = screenW;
            const int32_t viewH = screenH / 2;

            for (int32_t player = 0; player < 2; ++player) {
                presentVramRect(
                    pDstBase, dstStride, vramPixels, gpu.ramStride,
                    Splitscreen::VIEW_VRAM_X[player], Splitscreen::VIEW_VRAM_Y[player],
                    ORIG_DRAW_RES_X, ORIG_DRAW_RES_Y,
                    0, viewH * player, viewW, viewH,
                    fbWriteTicks
                );
            }
        }

        {
            const uint64_t perfFreq = SDL_GetPerformanceFrequency();
            gXbFbWriteMicros = (perfFreq > 0) ? ((fbWriteTicks * 1000000u) / perfFreq) : 0;
        }

        XVideoFlushFB();
    }
    else if (pFrameBuffer && (videoMode.bpp == 32)) {
        Gpu::Core& gpu = PsxVm::gGpu;
        const Gpu::Color16* const vramPixels = reinterpret_cast<const Gpu::Color16*>(gpu.pRam);

        const int32_t dstStride = videoMode.width;                          // In pixels: the mode is 32bpp
        const int32_t dstX = (videoMode.width - (int32_t)(ORIG_DRAW_RES_X * 2)) / 2;
        const uint32_t xStart = (uint32_t) gpu.displayAreaX;

        uint32_t* const pDstBase = reinterpret_cast<uint32_t*>(pFrameBuffer);

        // One row at a time, built in cached memory and then flushed twice.
        //
        // The obvious loop - convert a pixel, write it to both output rows, move on - costs far more than it looks on
        // this machine. The framebuffer is write-combined memory, which is fast only when it is written straight
        // through in long runs. Alternating between two rows on every single pixel means alternating between addresses
        // two and a half kilobytes apart, which flushes a partially filled write-combine buffer every time. There are
        // only a handful of those buffers, so almost every store pays for one.
        //
        // Building the row in a 2KB stack buffer instead keeps all of that arithmetic in cache, and the framebuffer
        // then sees two long sequential runs per row, which is the access pattern write-combining exists for.
        uint32_t rowBuf[ORIG_DRAW_RES_X * 2];

        // Split the cost of this loop in two: turning PlayStation pixels into screen pixels, and getting them into the
        // framebuffer. The whole thing measures 27ms of a 51ms frame and is the single largest item in it, but which
        // half that is decides what to do about it - a conversion that dominates is worth doing differently, whereas
        // writes that dominate mean the picture wants handing to the GPU instead of pushed there by the CPU.
        //
        // Reading the performance counter is a timestamp instruction; at two per row, 480 rows, it costs a few
        // microseconds a frame and cannot meaningfully disturb what it is measuring.
        uint64_t fbWriteTicks = 0;

        // The overlay is prepared before any of the picture is written, and merged into the rows it crosses as they
        // are built. It used to be painted onto the framebuffer afterwards, in a second pass back over the top of the
        // screen - which is why it flickered: the display had usually scanned past those rows by then, so it appeared
        // on some frames and not others.
        XboxDiag::setTexCachePage(I_GetCurTexCacheFillPage());

        // Timed on its own. It is inside the same stage as the picture, redraws its text every frame, and until it is
        // measured there is no telling whether a debug readout is costing a meaningful slice of the frame it reports.
        const uint64_t overlayStart = XboxLog::nowMicros();
        XboxDiag::renderOverlay();
        gXbOverlayMicros = XboxLog::nowMicros() - overlayStart;

        for (uint32_t y = 0; y < ORIG_DRAW_RES_Y; ++y) {
            const Gpu::Color16* const rowPixels = vramPixels + ((intptr_t) y + gpu.displayAreaY) * gpu.ramStride;
            const Gpu::Color16* const pSrc = rowPixels + xStart;

            for (uint32_t x = 0; x < ORIG_DRAW_RES_X; ++x) {
                const uint16_t srcBits = pSrc[x].bits;
                const uint32_t r = (uint32_t)(srcBits & 0x1F) << 3;
                const uint32_t g = (uint32_t)((srcBits >> 5) & 0x1F) << 3;
                const uint32_t b = (uint32_t)((srcBits >> 10) & 0x1F) << 3;
                const uint32_t px = (r << 16) | (g << 8) | b;

                // Doubled horizontally with two plain stores, into cache rather than into the framebuffer.
                //
                // This used to build a 64-bit value and 'memcpy' it in one go, on the reasoning that one wide store
                // beats two narrow ones. This is a 32-bit Pentium III: there is no 64-bit general store to lower it
                // to, and an eight byte copy of a non-constant size is entitled to become a call to 'memcpy' - once
                // per pixel, 61,440 times a frame. The measurement fits that and nothing else: the row building half
                // of this loop costs about 14ms, some 165 cycles for every pixel, against a handful of shifts and two
                // stores that should be nearer ten.
                uint32_t* const pOut = &rowBuf[x * 2];
                pOut[0] = px;
                pOut[1] = px;
            }

            // The overlay goes in before the row leaves the cache, so the picture reaches the screen complete
            const int screenY0 = (int)(y * 2);
            XboxDiag::mergeOverlayIntoRow(rowBuf, (int)(ORIG_DRAW_RES_X * 2), screenY0, dstX);

            uint32_t* const pRow0 = pDstBase + ((intptr_t) screenY0 * dstStride) + dstX;

            const uint64_t fbWriteStart = SDL_GetPerformanceCounter();
            std::memcpy(pRow0, rowBuf, sizeof(rowBuf));

            // The second row of the pair needs its own overlay line, so it cannot simply reuse the first
            XboxDiag::mergeOverlayIntoRow(rowBuf, (int)(ORIG_DRAW_RES_X * 2), screenY0 + 1, dstX);
            std::memcpy(pRow0 + dstStride, rowBuf, sizeof(rowBuf));
            fbWriteTicks += SDL_GetPerformanceCounter() - fbWriteStart;
        }

        {
            const uint64_t perfFreq = SDL_GetPerformanceFrequency();
            gXbFbWriteMicros = (perfFreq > 0) ? ((fbWriteTicks * 1000000u) / perfFreq) : 0;
        }

        XVideoFlushFB();    // Write-combined memory: make sure it has actually landed
    }

    const uint64_t xbDirectEnd = XboxLog::nowMicros();

    {
        const uint64_t interval = (gXbPrevFrameEndMicros > 0) ? (xbDirectEnd - gXbPrevFrameEndMicros) : 0;
        const uint64_t directUs = xbDirectEnd - xbDirectStart;
        const uint64_t rest = (interval > directUs) ? (interval - directUs) : 0;

        gXbPrevFrameEndMicros = xbDirectEnd;
        gXbFrameNum++;

        // Tell the relay the game is still going. If this stops, its sender thread says so.
        XboxLog::heartbeat("present");

        // 'rest' broken into its parts. 'other' is what is left once the rasteriser and the game tick are taken out -
        // texture cache work, the status bar, sound updates, and anything else between two presents.
        const uint64_t renderUs = gXbRenderMicros;
        gXbRenderMicros = 0;        // Accumulated across every view drawn this frame; start the next frame at zero
        const uint64_t tickUs = gXbTickMicros;
        // 'other' is what is left once everything with a name is taken out, so 'pace' comes out of it too - otherwise
        // the pacing tail would be counted in both and 'other' would never shrink no matter what was explained.
        const uint64_t namedUs = renderUs + tickUs + gXbPaceMicros + XboxLog::gXbStatusBarMicros + XboxLog::gXbInputMicros;
        const uint64_t otherUs = (rest > namedUs) ? (rest - namedUs) : 0;

        XBOX_LOGT(
            Video,
            "frame %u total=%lluus direct=%lluus fbwrite=%lluus ovl=%lluus rest=%lluus render=%lluus tick=%lluus pace=%lluus other=%lluus",
            gXbFrameNum,
            (unsigned long long) interval,
            (unsigned long long) directUs,
            (unsigned long long) gXbFbWriteMicros,
            (unsigned long long) gXbOverlayMicros,
            (unsigned long long) rest,
            (unsigned long long) renderUs,
            (unsigned long long) tickUs,
            (unsigned long long) gXbPaceMicros,
            (unsigned long long) otherUs
        );

        // Gather the frame across the second as well as tracing it.
        //
        // An average alone has been misleading here: the frame averages 31.9ms, which is over 30fps, while the game
        // was reported as running at 23. Both are true - the average is the average and 23fps is what the worst frames
        // feel like - and it is the worst frames that decide whether this is playable, and whether two viewports will
        // ever fit in the budget. So the slowest and fastest of each second are kept, not just the mean.
        if (interval > 0) {
            gXbSecFrames++;
            gXbSecTotalUs += interval;
            gXbSecDirectUs += directUs;
            gXbSecRenderUs += renderUs;
            gXbSecPaceUs += gXbPaceMicros;
            gXbSecOtherUs += otherUs;
            gXbSecFbWriteUs += gXbFbWriteMicros;
            gXbSecStatusBarUs += XboxLog::gXbStatusBarMicros;
            gXbSecInputUs += XboxLog::gXbInputMicros;

            if ((gXbSecWorstUs == 0) || (interval > gXbSecWorstUs)) { gXbSecWorstUs = interval; }
            if ((gXbSecBestUs == 0) || (interval < gXbSecBestUs)) { gXbSecBestUs = interval; }
        }

        //----------------------------------------------------------------------------------------------------------
        // Once a second, the things that would explain a game getting slower the longer it runs.
        //
        // Measured over a session: the frame went from 38ms to 84ms in under three minutes, and 'direct' went with it,
        // 23ms to 41ms. That stage does a fixed amount of work - the same loop over the same 61,440 pixels every frame
        // - so it getting slower means something is taking the machine away from it rather than it having more to do.
        //
        // Sound effects failing while the music keeps playing points at voices: if they are allocated and not
        // released, the audio callback mixes more of them every frame until there is no CPU left and no free voice for
        // a new sound. This is what would show that.
        //
        // Once a second, so it costs nothing and cannot itself become the flood the drop counter once was.
        //----------------------------------------------------------------------------------------------------------
        {
            static uint64_t sLastHealthMicros = 0;
            static uint32_t sLastAudioCbs = 0;
            static uint32_t sLastAudioExits = 0;
            static uint32_t sLastAudioSilence = 0;
            static uint32_t sLastLockSpu = 0;
            static uint32_t sLastUsbPolls = 0;      // For the USB poll rate on the performance line

            if (xbDirectEnd - sLastHealthMicros >= 1000000) {
                const uint32_t audioCbs = XboxDiag::gAudioCbs.load(std::memory_order_relaxed);
                const uint32_t lockSpu = XboxDiag::gLockSpuCont.load(std::memory_order_relaxed);
                const uint32_t voices = XboxDiag::gWessVoices.load(std::memory_order_relaxed);
                const int32_t zoneFree = (gpMainMemZone) ? Z_FreeMemory(*gpMainMemZone) : -1;

                // Break the voice count into the three states that tell apart why they are not being reclaimed.
                // 'stuck' - active but the SPU already says finished - means the reclaim loop is not running at all.
                uint32_t vActive = 0, vReleasing = 0, vSpuOff = 0, vStuck = 0;
                PSX_GetVoiceDiagCounts(vActive, vReleasing, vSpuOff, vStuck);

                // What SDL thinks of the audio device.
                //
                // Doom stops getting audio callbacks entirely a minute in - 90 a second in Final Doom, zero here - so
                // this is not the mixing failing but SDL ceasing to ask for samples at all. Its own view of the device
                // is the one thing that could say why, and nothing was reporting it.
                const SDL_AudioDeviceID audioDev = XboxDiag::gAudioDevId;
                const int audioStatus = (audioDev != 0) ? (int) SDL_GetAudioDeviceStatus(audioDev) : -1;

                XBOX_LOGI(
                    Audio,
                    "health voices=%u active=%u releasing=%u spuoff=%u stuck=%u millicount=%u audiocb/s=%u cbexit/s=%u silence/s=%u lockspu/s=%u zonefree=%d dev=%d devstatus=%d frame=%lluus",
                    voices,
                    vActive,
                    vReleasing,
                    vSpuOff,
                    vStuck,
                    gWess_Millicount,
                    audioCbs - sLastAudioCbs,
                    XboxDiag::gAudioCbExits.load(std::memory_order_relaxed) - sLastAudioExits,
                    XboxDiag::gAudioSilence.load(std::memory_order_relaxed) - sLastAudioSilence,
                    lockSpu - sLastLockSpu,
                    (int) zoneFree,
                    (int) audioDev,
                    audioStatus,
                    (unsigned long long) interval
                );

                // Everything the on-screen readout used to carry, and the frame floor it could not.
                //
                // 'worst' is the point of this line. The mean says 31.9ms and the game plays like 23fps, and both are
                // honest - it is the slow frames that are felt, and they are what has to fit inside 33ms before two
                // viewports can be asked to fit inside it as well.
                {
                    const uint32_t secFrames = (gXbSecFrames > 0) ? gXbSecFrames : 1;

                    XBOX_LOGI(
                        Video,
                        "perf fps=%u avg=%lluus worst=%lluus best=%lluus direct=%lluus (fbwrite=%lluus conv=%lluus) render=%lluus statusbar=%lluus input=%lluus pace=%lluus other=%lluus usb/s=%u texpage=%u",
                        gXbSecFrames,
                        (unsigned long long)(gXbSecTotalUs / secFrames),
                        (unsigned long long) gXbSecWorstUs,
                        (unsigned long long) gXbSecBestUs,
                        (unsigned long long)(gXbSecDirectUs / secFrames),
                        (unsigned long long)(gXbSecFbWriteUs / secFrames),
                        (unsigned long long)((gXbSecDirectUs > gXbSecFbWriteUs) ? ((gXbSecDirectUs - gXbSecFbWriteUs) / secFrames) : 0),
                        (unsigned long long)(gXbSecRenderUs / secFrames),
                        (unsigned long long)(gXbSecStatusBarUs / secFrames),
                        (unsigned long long)(gXbSecInputUs / secFrames),
                        (unsigned long long)(gXbSecPaceUs / secFrames),
                        (unsigned long long)(gXbSecOtherUs / secFrames),
                        XboxDiag::gUsbPolls.load(std::memory_order_relaxed) - sLastUsbPolls,
                        XboxDiag::gTexCachePage.load(std::memory_order_relaxed)
                    );

                    sLastUsbPolls = XboxDiag::gUsbPolls.load(std::memory_order_relaxed);

                    // What the rasteriser spent its time on, and on how many primitives.
                    //
                    // 'render' is the whole problem now, and this says whether it is slow per pixel - in which case the
                    // GPU is the answer - or whether far too many primitives are being submitted, which would be a
                    // different fix entirely. Times are per frame; counts are per frame too.
                    {
                        const uint64_t tscFreq = SDL_GetPerformanceFrequency();
                        const uint32_t f = (gXbSecFrames > 0) ? gXbSecFrames : 1;

                        const auto toUsPerFrame = [=](const uint64_t ticks) noexcept -> unsigned long long {
                            return (tscFreq > 0) ? (unsigned long long)((ticks * 1000000ull) / tscFreq / f) : 0ull;
                        };

                        // Nanoseconds per pixel, which is the only figure comparable between one session and another.
                        //
                        // Time per primitive is not: a wall column's cost depends on how tall it is and a rectangle's
                        // on how large, so the same number means different things in different scenes. That is how the
                        // last change came to look like it did nothing - 14.0us a column before and 13.6 after, with no
                        // way to tell whether the columns were the same size.
                        const auto nsPerPixel = [=](const uint64_t ticks, const uint64_t pixels) noexcept -> unsigned long long {
                            if ((tscFreq == 0) || (pixels == 0))
                                return 0ull;

                            return (unsigned long long)((ticks * 1000000000ull) / tscFreq / pixels);
                        };

                        XBOX_LOGI(
                            Video,
                            "raster wall=%lluus/%u/%lluns floor=%lluus/%u tri=%lluus/%u rect=%lluus/%u/%lluns (per frame, ns=per pixel)",
                            toUsPerFrame(Gpu::gDbgWallColTicks),
                            Gpu::gDbgNumWallCols / f,
                            nsPerPixel(Gpu::gDbgWallColTicks, Gpu::gDbgWallPixels),
                            toUsPerFrame(Gpu::gDbgFloorRowTicks),
                            Gpu::gDbgNumFloorRows / f,
                            toUsPerFrame(Gpu::gDbgTriangleTicks),
                            Gpu::gDbgNumTriangles / f,
                            toUsPerFrame(Gpu::gDbgRectTicks),
                            Gpu::gDbgNumRects / f,
                            nsPerPixel(Gpu::gDbgRectTicks, Gpu::gDbgRectPixels)
                        );

                        Gpu::resetDrawStats();
                    }

                    gXbSecFrames = 0;
                    gXbSecTotalUs = 0;
                    gXbSecDirectUs = 0;
                    gXbSecRenderUs = 0;
                    gXbSecPaceUs = 0;
                    gXbSecOtherUs = 0;
                    gXbSecFbWriteUs = 0;
                    gXbSecStatusBarUs = 0;
                    gXbSecInputUs = 0;
                    gXbSecWorstUs = 0;
                    gXbSecBestUs = 0;
                }

                // Reviving stopped audio is no longer attempted from here.
                //
                // It used to call 'XAudioPlay' once the callbacks had stopped for two seconds, on the reasoning that
                // the hardware needed setting going again. It fired 93 times in one session and the game stayed silent
                // for all of them, because the run bit was never what had stopped: the descriptor ring was empty, and
                // an engine with nothing to play is not helped by being told to play.
                //
                // Refilling the ring is what actually releases the waiting audio thread, and that now happens on a
                // thread of its own in 'XboxAudioWatchdog'. It has to, since the ring runs dry while a map is loading,
                // which is exactly when this loop is not running to notice.
                sLastHealthMicros = xbDirectEnd;
                sLastAudioCbs = audioCbs;
                sLastAudioExits = XboxDiag::gAudioCbExits.load(std::memory_order_relaxed);
                sLastAudioSilence = XboxDiag::gAudioSilence.load(std::memory_order_relaxed);
                sLastLockSpu = lockSpu;
            }
        }
    }
#elif defined(__XBOX__)
    // Xbox fast path: the texture is already 2x pre-scaled to 512x480.
    // Center it in the 640x480 window (64px black bars each side) with no SDL software upscaling.
    unlockFramebufferTexture();

    SDL_Rect srcRect = {};
    srcRect.w = mFbTexW;   // 512
    srcRect.h = mFbTexH;   // 480

    SDL_Rect dstRect = {};
    dstRect.x = (640 - mFbTexW) / 2;  // 64
    dstRect.y = 0;
    dstRect.w = mFbTexW;   // 512
    dstRect.h = mFbTexH;   // 480

    // Clear only the side bars if needed (just clear whole screen - it's a fast memset)
    const uint64_t xbClearStart = XboxLog::nowMicros();
    SDL_RenderClear(mpRenderer);

    const uint64_t xbCopyStart = XboxLog::nowMicros();
    SDL_RenderCopy(mpRenderer, mpFramebufferTexture, &srcRect, &dstRect);

    const uint64_t xbPresentStart = XboxLog::nowMicros();
    SDL_RenderPresent(mpRenderer);

    const uint64_t xbPresentEnd = XboxLog::nowMicros();
    lockFramebufferTexture();

    // Report the split.
    //
    // 'rest' is everything this does not account for - the emulated PlayStation GPU drawing the scene, the game's own
    // tick, and anything else between two presents. If 'rest' turns out to be the bulk of it then the software present
    // is not the problem and the plan changes.
    {
        const uint64_t frameEnd = XboxLog::nowMicros();
        const uint64_t interval = (gXbPrevFrameEndMicros > 0) ? (frameEnd - gXbPrevFrameEndMicros) : 0;
        const uint64_t clearUs = xbCopyStart - xbClearStart;
        const uint64_t copyUs = xbPresentStart - xbCopyStart;
        const uint64_t presentUs = xbPresentEnd - xbPresentStart;
        const uint64_t accounted = gXbPrescaleMicros + clearUs + copyUs + presentUs;
        const uint64_t rest = (interval > accounted) ? (interval - accounted) : 0;

        gXbPrevFrameEndMicros = frameEnd;
        gXbFrameNum++;

        XBOX_LOGT(
            Video,
            "frame %u total=%lluus prescale=%lluus clear=%lluus copy=%lluus present=%lluus rest=%lluus",
            gXbFrameNum,
            (unsigned long long) interval,
            (unsigned long long) gXbPrescaleMicros,
            (unsigned long long) clearUs,
            (unsigned long long) copyUs,
            (unsigned long long) presentUs,
            (unsigned long long) rest
        );
    }
#else
    int32_t windowW = {};
    int32_t windowH = {};
    SDL_GetRendererOutputSize(mpRenderer, &windowW, &windowH);

    if ((windowW <= 0) || (windowH <= 0))
        return;

    // Get the window area to output the PSX framebuffer to  and don't bother outputting if zero sized
    float outputRectX = {};
    float outputRectY = {};
    float outputRectW = {};
    float outputRectH = {};

    Video::getClassicFramebufferWindowRect(
        (float) windowW,
        (float) windowH,
        outputRectX,
        outputRectY,
        outputRectW,
        outputRectH
    );

    if ((outputRectW <= 0.0f) || (outputRectH <= 0.0f))
        return;

    // These are the source and destination regions to blit
    ASSERT((Video::gTopOverscan >= 0) && (Video::gTopOverscan < Video::ORIG_DRAW_RES_Y / 2));
    ASSERT((Video::gBotOverscan >= 0) && (Video::gBotOverscan < Video::ORIG_DRAW_RES_Y / 2));

    SDL_Rect srcRect = {};
    srcRect.y = Video::gTopOverscan;
    srcRect.w = Video::ORIG_DRAW_RES_X;
    srcRect.h = Video::ORIG_DRAW_RES_Y - Video::gTopOverscan - Video::gBotOverscan;

    SDL_Rect dstRect = {};
    dstRect.x = (int) outputRectX;
    dstRect.y = (int) outputRectY;
    dstRect.w = (int) std::ceil(outputRectW);
    dstRect.h = (int) std::ceil(outputRectH);

    // Done writing to the locked framebuffer, update the texture with whatever writes we made
    unlockFramebufferTexture();

    // Need to clear the window if we are not filling the whole screen.
    // Some stuff like NVIDIA video recording notifications can leave marks in the unused regions otherwise...
    if ((dstRect.w != windowW) || (dstRect.h != windowH)) {
        SDL_RenderClear(mpRenderer);
    }

    // Blit the framebuffer to the display
    SDL_RenderCopy(mpRenderer, mpFramebufferTexture, &srcRect, &dstRect);

    // Present the rendered frame and re-lock the framebuffer texture
    SDL_RenderPresent(mpRenderer);
    lockFramebufferTexture();
#endif  // #if defined(__XBOX__)
}

END_NAMESPACE(Video)
