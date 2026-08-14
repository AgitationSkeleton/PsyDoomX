#pragma once
//=============================================================================
// XboxDiag.h — on-screen diagnostic overlay for PsyDoom Xbox/nxdk port.
//
// Compiled only when __XBOX__ is defined.
// Set XBOX_DIAG_ENABLED=0 to compile away all instrumentation at zero cost.
//
// Usage — call from the noted sites each frame:
//   XboxDiag::writeToFramePixels(fb, pitch)  → VideoBackend_SDL::copyPsxToSdlFramebufferTexture(),
//                                               after the Xbox 2×2 pixel copy loop
//   XboxDiag::setAudioDev(devId)             → PsxVm::init(), after SDL_PauseAudioDevice(false)
//   XboxDiag::tickAudio()                    → SdlAudioCallback (PsxVm.cpp)
//   XboxDiag::tickLockSpu()                  → lockSpu() contention path (PsxVm.cpp)
//   XboxDiag::tickUsb()                      → Input::update() (Input.cpp)
//=============================================================================

#if defined(__XBOX__)

#ifndef XBOX_DIAG_ENABLED
#define XBOX_DIAG_ENABLED 1
#endif

#include <SDL.h>
#include <atomic>
#include <cstdio>
#include <cstring>

#if XBOX_DIAG_ENABLED

namespace XboxDiag {

//-----------------------------------------------------------------------------
// Runtime enable flag
//-----------------------------------------------------------------------------
inline bool gEnabled = true;

//-----------------------------------------------------------------------------
// Diagnostic counters (relaxed atomics — display-only metrics)
//-----------------------------------------------------------------------------
inline std::atomic<uint32_t> gFrameCount{0};    // total rendered frames
inline std::atomic<uint32_t> gAudioCbs{0};      // total audio callback invocations (counted on entry)
inline std::atomic<uint32_t> gAudioCbExits{0};  // ...and on the way out, so a callback that enters and never returns shows
inline std::atomic<uint32_t> gAudioSilence{0};  // times it gave up on the lock and wrote silence
inline std::atomic<uint32_t> gUsbPolls{0};      // total usbh_pooling_hubs() calls
inline std::atomic<uint32_t> gLockSpuCont{0};   // times lockSpu() had to wait (contention)
inline std::atomic<uint32_t> gWessVoices{0};    // current WESS active voice count (set each game tick)
inline std::atomic<uint32_t> gTexCachePage{0};  // current texture cache fill page index (set each frame)

// How hard the texture cache is being worked, counted per frame and read by the overlay.
//
// 'gTexUploads' is how many textures were sent to VRAM this frame and 'gTexEvicts' how many were thrown out to make
// room for them. A frame that draws the same scene twice - which is what splitscreen is - should upload roughly what
// it drew and evict nothing. Uploads climbing with evictions alongside them means the two views are fighting over the
// cache and each is undoing the other's work, which costs the frame time it looks like it costs.
inline std::atomic<uint32_t> gTexUploads{0};
inline std::atomic<uint32_t> gTexEvicts{0};
inline std::atomic<uint32_t> gTexUploadsPrev{0};
inline std::atomic<uint32_t> gTexEvictsPrev{0};

// Audio device ID — set once at init
inline SDL_AudioDeviceID gAudioDevId{0};

// Per-second display values
inline uint32_t _fpsDisp  = 0;
inline uint32_t _cbpsDisp = 0;

//=============================================================================
// 8×8 bitmap font (MSB = leftmost pixel per row).
// Only characters used in the diagnostic format strings are defined;
// unknowns render as a small outlined square.
//=============================================================================
struct Gly8 { uint8_t r[8]; };

static inline Gly8 glyph(const char c) noexcept {
    switch (c) {
        case ' ': return {{0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}};
        case '+': return {{0x00,0x18,0x18,0x7E,0x18,0x18,0x00,0x00}};
        case '-': return {{0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00}};
        case '/': return {{0x06,0x0C,0x0C,0x18,0x30,0x30,0x60,0x00}};
        case ':': return {{0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x00}};
        case '0': return {{0x3C,0x66,0x6E,0x76,0x66,0x66,0x3C,0x00}};
        case '1': return {{0x18,0x38,0x18,0x18,0x18,0x18,0x7E,0x00}};
        case '2': return {{0x3C,0x46,0x06,0x1C,0x30,0x60,0x7E,0x00}};
        case '3': return {{0x3C,0x66,0x06,0x1C,0x06,0x66,0x3C,0x00}};
        case '4': return {{0x0E,0x1E,0x36,0x66,0x7F,0x06,0x06,0x00}};
        case '5': return {{0x7E,0x60,0x7C,0x06,0x06,0x66,0x3C,0x00}};
        case '6': return {{0x1C,0x30,0x60,0x7C,0x66,0x66,0x3C,0x00}};
        case '7': return {{0x7E,0x66,0x06,0x0C,0x18,0x18,0x18,0x00}};
        case '8': return {{0x3C,0x66,0x66,0x3C,0x66,0x66,0x3C,0x00}};
        case '9': return {{0x3C,0x66,0x66,0x3E,0x06,0x0C,0x38,0x00}};
        case 'A': return {{0x18,0x3C,0x66,0x66,0x7E,0x66,0x66,0x00}};
        case 'B': return {{0x7C,0x66,0x66,0x7C,0x66,0x66,0x7C,0x00}};
        case 'C': return {{0x3C,0x66,0x60,0x60,0x60,0x66,0x3C,0x00}};
        case 'D': return {{0x78,0x6C,0x66,0x66,0x66,0x6C,0x78,0x00}};
        case 'F': return {{0x7E,0x60,0x60,0x7C,0x60,0x60,0x60,0x00}};
        case 'K': return {{0x66,0x6C,0x78,0x70,0x78,0x6C,0x66,0x00}};
        case 'L': return {{0x60,0x60,0x60,0x60,0x60,0x60,0x7E,0x00}};
        case 'M': return {{0x63,0x77,0x7F,0x6B,0x63,0x63,0x63,0x00}};
        case 'N': return {{0x63,0x73,0x7B,0x6F,0x67,0x63,0x63,0x00}};
        case 'P': return {{0x7C,0x66,0x66,0x7C,0x60,0x60,0x60,0x00}};
        case 'R': return {{0x7C,0x66,0x66,0x7C,0x78,0x6C,0x66,0x00}};
        case 'S': return {{0x3C,0x66,0x60,0x3C,0x06,0x66,0x3C,0x00}};
        case 'U': return {{0x66,0x66,0x66,0x66,0x66,0x66,0x3C,0x00}};
        case 'E': return {{0x7E,0x60,0x60,0x7C,0x60,0x60,0x7E,0x00}};
        case 'O': return {{0x3C,0x66,0x66,0x66,0x66,0x66,0x3C,0x00}};
        case 'T': return {{0x7E,0x18,0x18,0x18,0x18,0x18,0x18,0x00}};
        case 'V': return {{0x66,0x66,0x66,0x66,0x66,0x3C,0x18,0x00}};
        case 'X': return {{0x66,0x66,0x3C,0x18,0x3C,0x66,0x66,0x00}};
        default:  return {{0x00,0x3C,0x24,0x24,0x24,0x24,0x3C,0x00}};
    }
}

static inline void drawChar(uint32_t* fb, int stride, int x, int y, char c, uint32_t color) noexcept {
    constexpr int SCALE = 2;
    const Gly8 g = glyph(c);
    for (int row = 0; row < 8; ++row) {
        const int py = y + row * SCALE;
        uint8_t bits = g.r[row];
        for (int col = 0; col < 8; ++col) {
            if (bits & (0x80u >> col)) {
                const int px = x + col * SCALE;
                fb[py * stride + px]           = color;
                fb[py * stride + px + 1]       = color;
                fb[(py + 1) * stride + px]     = color;
                fb[(py + 1) * stride + px + 1] = color;
            }
        }
    }
}

static inline void drawStr(uint32_t* fb, int stride, int x, int y, const char* s, uint32_t color) noexcept {
    constexpr int CW = 8 * 2;
    for (; *s; ++s, x += CW) {
        drawChar(fb, stride, x, y, *s, color);
    }
}

//=============================================================================
// Instrumentation entry points
//=============================================================================
inline void tickAudio()   noexcept { gAudioCbs.fetch_add(1,    std::memory_order_relaxed); }
inline void tickAudioExit() noexcept { gAudioCbExits.fetch_add(1, std::memory_order_relaxed); }
inline void tickAudioSilence() noexcept { gAudioSilence.fetch_add(1, std::memory_order_relaxed); }
inline void tickLockSpu() noexcept { gLockSpuCont.fetch_add(1, std::memory_order_relaxed); }
inline void tickUsb()     noexcept { gUsbPolls.fetch_add(1,    std::memory_order_relaxed); }

inline void setAudioDev(SDL_AudioDeviceID id) noexcept { gAudioDevId = id; }
inline void setWessVoices(uint32_t n)  noexcept { gWessVoices.store(n,  std::memory_order_relaxed); }
inline void setTexCachePage(uint32_t n) noexcept { gTexCachePage.store(n, std::memory_order_relaxed); }
inline void tickTexUpload() noexcept { gTexUploads.fetch_add(1, std::memory_order_relaxed); }
inline void tickTexEvict()  noexcept { gTexEvicts.fetch_add(1, std::memory_order_relaxed); }

// Roll the per frame counts over. Called once a frame, after the frame has been drawn.
inline void endTexFrame() noexcept {
    gTexUploadsPrev.store(gTexUploads.exchange(0, std::memory_order_relaxed), std::memory_order_relaxed);
    gTexEvictsPrev.store(gTexEvicts.exchange(0, std::memory_order_relaxed), std::memory_order_relaxed);
}

//=============================================================================
// writeToFramePixels() — call after the Xbox 2×2 pixel copy loop in
// copyPsxToSdlFramebufferTexture(), while the texture is still locked.
// Writes a dark background + 3-line diagnostic text into the texture pixels.
//   fb        : mpFramebufferPixels (locked texture pixel buffer)
//   pitchBytes: mFbTexPitch (bytes per row from SDL_LockTexture)
//=============================================================================
//------------------------------------------------------------------------------------------------------------------------------------------
// The overlay, rendered into a buffer of its own rather than straight onto the screen.
//
// It used to be drawn onto the framebuffer after the whole picture had been written - a second pass back over the top
// of the screen. Two things wrong with that. The display is scanned out continuously from a framebuffer that is being
// written in place, and by the time the overlay was drawn the scan had usually passed the rows it occupies, so it
// appeared on some frames and not others - the flicker. And going back to the top broke the long sequential write the
// rest of the frame had just been restructured to produce.
//
// Rendered here into its own small buffer instead, and merged into each row as that row is built, so it arrives with
// the rest of the picture in one pass and in the right order.
//------------------------------------------------------------------------------------------------------------------------------------------
static constexpr int OVL_W = 368;       // 22 columns of 16px, plus the border
static constexpr int OVL_H = 76;        // 4 lines of 18px, plus the border
static constexpr int OVL_X = 66;        // Inside the picture area, so it merges into the row buffer
static constexpr int OVL_Y = 24;

// Where the overlay actually lands this frame.
//
// It merges into rows of the picture as they are written, so it has to sit inside a region that is actually being
// drawn. In splitscreen the viewports do not cover the top of the screen, and the fixed position above falls in the
// black bar over them - no row passes through it, so nothing was merged and the overlay simply did not appear. The
// present sets these to keep it in the same place relative to player one's view as it has in a full size one.
inline int gOvlX = OVL_X;
inline int gOvlY = OVL_Y;

inline void setOverlayOrigin(const int x, const int y) noexcept {
    gOvlX = x;
    gOvlY = y;
}

// The full four line readout: frames, audio callbacks, USB polls, SPU contention, voices, texture page.
//
// Off by default. It is for working on a specific fault and says nothing useful the rest of the time, while costing a
// redraw of 28,000 pixels every frame and covering a good part of the picture.
inline bool gShowFullOverlay = false;

// Just the frame rate. On, because the whole point of the current work is the frame rate and it wants watching.
inline bool gShowFpsOverlay = false;

// Include the frame time beside the rate.
//
// Separate from showing the rate at all because they answer different questions: a player wants to know it is smooth,
// and someone reporting a problem wants the number that says how badly it is not.
inline bool gShowFpsMs = false;

// How much of the panel is actually in use.
//
// The buffer stays its full size either way; these say how much of it to merge into the picture, so the FPS readout
// costs one short line rather than the whole four line panel.
inline int gOvlActiveW = OVL_W;
inline int gOvlActiveH = OVL_H;

inline uint32_t gOverlayBuf[OVL_W * OVL_H];
inline bool gbOverlayReady = false;

// Renders the overlay into its own buffer. Called once a frame, before the rows are built.
inline void renderOverlay() noexcept;

// As 'mergeOverlayIntoRow', but for a 15-bit row.
//
// The overlay is drawn once a frame in 32-bit and converted here rather than kept twice, because it only crosses 76
// rows of the 480 and converting those costs far less than maintaining a second copy of it.
inline void mergeOverlayIntoRow15(uint16_t* const rowBuf, const int rowLen, const int screenY, const int rowScreenX) noexcept;

// Merges the overlay into one row of the picture, if that row passes through it.
//
// 'rowBuf' is one output row of the picture and 'rowScreenX' is where its first pixel lands on screen.
inline void mergeOverlayIntoRow(uint32_t* const rowBuf, const int rowLen, const int screenY, const int rowScreenX) noexcept {
    if ((!gEnabled) || (!gbOverlayReady))
        return;

    if ((screenY < gOvlY) || (screenY >= gOvlY + gOvlActiveH))
        return;

    const uint32_t* const pSrc = gOverlayBuf + (size_t)(screenY - gOvlY) * OVL_W;
    const int dstStart = gOvlX - rowScreenX;         // Where the overlay begins within this row
    const int copyStart = (dstStart < 0) ? 0 : dstStart;
    const int copyEnd = (dstStart + gOvlActiveW > rowLen) ? rowLen : dstStart + gOvlActiveW;

    for (int x = copyStart; x < copyEnd; ++x) {
        rowBuf[x] = pSrc[x - dstStart];
    }
}

inline void mergeOverlayIntoRow15(uint16_t* const rowBuf, const int rowLen, const int screenY, const int rowScreenX) noexcept {
    if ((!gEnabled) || (!gbOverlayReady))
        return;

    if ((screenY < gOvlY) || (screenY >= gOvlY + gOvlActiveH))
        return;

    const uint32_t* const pSrc = gOverlayBuf + (size_t)(screenY - gOvlY) * OVL_W;
    const int dstStart = gOvlX - rowScreenX;
    const int copyStart = (dstStart < 0) ? 0 : dstStart;
    const int copyEnd = (dstStart + gOvlActiveW > rowLen) ? rowLen : dstStart + gOvlActiveW;

    for (int x = copyStart; x < copyEnd; ++x) {
        const uint32_t px = pSrc[x - dstStart];
        const uint16_t r = (uint16_t)((px >> 19) & 0x1F);
        const uint16_t g = (uint16_t)((px >> 11) & 0x1F);
        const uint16_t b = (uint16_t)((px >> 3) & 0x1F);
        rowBuf[x] = (uint16_t)(0x8000u | (r << 10) | (g << 5) | b);
    }
}

inline void writeToFramePixels(uint32_t* fb, int pitchBytes) noexcept {
    if (!gEnabled || !fb) return;

    gFrameCount.fetch_add(1, std::memory_order_relaxed);

    const uint32_t now = SDL_GetTicks();
    {
        static uint32_t lastSec   = 0;
        static uint32_t lastFrame = 0;
        static uint32_t lastAudio = 0;

        if (now - lastSec >= 1000u) {
            const uint32_t fc = gFrameCount.load(std::memory_order_relaxed);
            const uint32_t ac = gAudioCbs.load(std::memory_order_relaxed);
            _fpsDisp  = fc - lastFrame;
            _cbpsDisp = ac - lastAudio;
            lastFrame = fc;
            lastAudio = ac;
            lastSec   = now;
        }
    }

    constexpr int SCALE = 2;
    constexpr int CW    = 8  * SCALE;      // character cell width  (16px)
    constexpr int CH    = 8  * SCALE + 2;  // character cell height + gap (18px)
    constexpr int LINES = 4;
    constexpr int COLS  = 22;
    constexpr int OX    = 2;
    constexpr int OY    = 24;

    const int stride = pitchBytes / (int)sizeof(uint32_t);

    // Dark background
    constexpr uint32_t BG = 0x0F0F0Fu;
    const int bgX0 = OX - 1,  bgY0 = OY - 1;
    const int bgX1 = bgX0 + COLS * CW + 2;
    const int bgY1 = bgY0 + LINES * CH + 2;
    for (int ry = bgY0; ry < bgY1; ++ry) {
        uint32_t* row = fb + ry * stride;
        for (int rx = bgX0; rx < bgX1; ++rx) row[rx] = BG;
    }

    const uint32_t fc  = gFrameCount.load(std::memory_order_relaxed);
    const uint32_t ac  = gAudioCbs.load(std::memory_order_relaxed);
    const uint32_t up  = gUsbPolls.load(std::memory_order_relaxed);
    const uint32_t lck = gLockSpuCont.load(std::memory_order_relaxed);
    const uint32_t vox = gWessVoices.load(std::memory_order_relaxed);
    const uint32_t tex = gTexCachePage.load(std::memory_order_relaxed);
    const uint32_t txu = gTexUploadsPrev.load(std::memory_order_relaxed);
    const uint32_t txe = gTexEvictsPrev.load(std::memory_order_relaxed);

    // Audio device status: P=playing, N=paused, S=stopped, X=invalid
    char audStat = 'X';
    if (gAudioDevId != 0) {
        switch (SDL_GetAudioDeviceStatus(gAudioDevId)) {
            case SDL_AUDIO_PLAYING: audStat = 'P'; break;
            case SDL_AUDIO_PAUSED:  audStat = 'N'; break;
            case SDL_AUDIO_STOPPED: audStat = 'S'; break;
            default: break;
        }
    }

    static uint32_t lastTick = 0;
    const uint32_t frameMs = (lastTick == 0) ? 0u : (now - lastTick);
    lastTick = now;

    char buf[48];
    constexpr uint32_t COL_YEL = 0xFFDC00u;

    // Line 0: frame count, FPS, frame time
    std::snprintf(buf, sizeof(buf), "FRM:%-5u FPS:%-2u %3uMS",
                  fc % 100000u, _fpsDisp % 100u, frameMs % 1000u);
    drawStr(fb, stride, OX, OY + 0 * CH, buf, COL_YEL);

    // Line 1: audio callback count, callbacks/sec, device status
    std::snprintf(buf, sizeof(buf), "AUD:%-5u CBS:%-3u/S %c",
                  ac % 100000u, _cbpsDisp % 1000u, audStat);
    drawStr(fb, stride, OX, OY + 1 * CH, buf, COL_YEL);

    // Line 2: USB polls total, LockSpu contentions total
    std::snprintf(buf, sizeof(buf), "USB:%-5u LCK:%-5u",
                  up % 100000u, lck % 100000u);
    drawStr(fb, stride, OX, OY + 2 * CH, buf, COL_YEL);

    // Line 3: WESS voices, the texture cache fill page, and what the cache did last frame.
    //
    // 'U' is textures uploaded to VRAM and 'E' textures thrown out to make room. Uploads should be close to the number
    // of distinct things on screen and evictions should be near zero. Evictions climbing means the frame does not fit
    // in the cache and is re-uploading what it just threw away - which in splitscreen would mean the two views are
    // undoing each other's work, and is the first thing to rule in or out for a stutter that comes and goes.
    std::snprintf(buf, sizeof(buf), "VOX:%-3u PG%-2u U%-3u E%-3u",
                  vox % 1000u, tex % 100u, txu % 1000u, txe % 1000u);
    drawStr(fb, stride, OX, OY + 3 * CH, buf, COL_YEL);
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Renders the overlay into its own buffer, at that buffer's own coordinates.
//
// Reuses the drawing above by pointing it at the overlay buffer rather than the framebuffer: it takes a target and a
// stride, so it neither knows nor cares which it is given.
//------------------------------------------------------------------------------------------------------------------------------------------
inline void renderOverlay() noexcept {
    if (!gEnabled)
        return;

    // Drawn at the buffer's own coordinates. The earlier version offset the pointer so the screen coordinates would
    // land inside the buffer, which computes a pointer outside the array - undefined behaviour even where it happens
    // to produce the right address.
    constexpr int SCALE = 2;
    constexpr int CW = 8 * SCALE;
    constexpr int CH = 8 * SCALE + 2;
    constexpr int PAD = 2;

    gFrameCount.fetch_add(1, std::memory_order_relaxed);

    const uint32_t now = SDL_GetTicks();
    {
        static uint32_t lastSec = 0;
        static uint32_t lastFrame = 0;
        static uint32_t lastAudio = 0;

        if (now - lastSec >= 1000u) {
            const uint32_t fc = gFrameCount.load(std::memory_order_relaxed);
            const uint32_t ac = gAudioCbs.load(std::memory_order_relaxed);
            _fpsDisp = fc - lastFrame;
            _cbpsDisp = ac - lastAudio;
            lastFrame = fc;
            lastAudio = ac;
            lastSec = now;
        }
    }

    const uint32_t fc = gFrameCount.load(std::memory_order_relaxed);
    const uint32_t ac = gAudioCbs.load(std::memory_order_relaxed);
    const uint32_t up = gUsbPolls.load(std::memory_order_relaxed);
    const uint32_t lck = gLockSpuCont.load(std::memory_order_relaxed);
    const uint32_t vox = gWessVoices.load(std::memory_order_relaxed);
    const uint32_t tex = gTexCachePage.load(std::memory_order_relaxed);
    const uint32_t frameMs = (_fpsDisp > 0) ? (1000u / _fpsDisp) : 0u;
    const char audStat = (gAudioDevId != 0) ? 'P' : '-';

    constexpr uint32_t BG = 0x0F0F0Fu;
    constexpr uint32_t COL_YEL = 0xFFDC00u;
    char buf[48];

    // Nothing asked for: draw nothing and merge nothing
    if ((!gShowFullOverlay) && (!gShowFpsOverlay)) {
        gOvlActiveW = 0;
        gOvlActiveH = 0;
        gbOverlayReady = false;
        return;
    }

    // Just the frame rate.
    //
    // One short line rather than the four line panel, and only that much of the buffer is cleared and merged - the
    // full readout redraws 28,000 pixels a frame and covers a good part of the picture, neither of which is worth
    // paying to watch a number that is the whole point of the current work.
    if (!gShowFullOverlay) {
        if (gShowFpsMs) {
            std::snprintf(buf, sizeof(buf), "FPS:%-2u %3uMS", _fpsDisp % 100u, frameMs % 1000u);
        } else {
            std::snprintf(buf, sizeof(buf), "FPS:%-2u", _fpsDisp % 100u);
        }

        gOvlActiveW = PAD * 2 + (int) std::strlen(buf) * CW;
        gOvlActiveH = PAD * 2 + CH;

        if (gOvlActiveW > OVL_W) { gOvlActiveW = OVL_W; }
        if (gOvlActiveH > OVL_H) { gOvlActiveH = OVL_H; }

        for (int y = 0; y < gOvlActiveH; ++y) {
            for (int x = 0; x < gOvlActiveW; ++x) {
                gOverlayBuf[y * OVL_W + x] = BG;
            }
        }

        drawStr(gOverlayBuf, OVL_W, PAD, PAD, buf, COL_YEL);
        gbOverlayReady = true;
        return;
    }

    // The full readout
    gOvlActiveW = OVL_W;
    gOvlActiveH = OVL_H;

    for (int i = 0; i < OVL_W * OVL_H; ++i) {
        gOverlayBuf[i] = BG;
    }

    std::snprintf(buf, sizeof(buf), "FRM:%-5u FPS:%-2u %3uMS", fc % 100000u, _fpsDisp % 100u, frameMs % 1000u);
    drawStr(gOverlayBuf, OVL_W, PAD, PAD + 0 * CH, buf, COL_YEL);

    std::snprintf(buf, sizeof(buf), "AUD:%-5u CBS:%-3u/S %c", ac % 100000u, _cbpsDisp % 1000u, audStat);
    drawStr(gOverlayBuf, OVL_W, PAD, PAD + 1 * CH, buf, COL_YEL);

    std::snprintf(buf, sizeof(buf), "USB:%-5u LCK:%-5u", up % 100000u, lck % 100000u);
    drawStr(gOverlayBuf, OVL_W, PAD, PAD + 2 * CH, buf, COL_YEL);

    std::snprintf(buf, sizeof(buf), "VOX:%-3u TEX:PG%-2u", vox % 1000u, tex % 100u);
    drawStr(gOverlayBuf, OVL_W, PAD, PAD + 3 * CH, buf, COL_YEL);

    gbOverlayReady = true;
}

} // namespace XboxDiag

#else // XBOX_DIAG_ENABLED == 0

namespace XboxDiag {
    inline void tickAudio()                        noexcept {}
    inline void tickLockSpu()                      noexcept {}
    inline void tickUsb()                          noexcept {}
    inline void setAudioDev(SDL_AudioDeviceID)     noexcept {}
    inline void setWessVoices(uint32_t)            noexcept {}
    inline void setTexCachePage(uint32_t)          noexcept {}
    inline void tickTexUpload()                    noexcept {}
    inline void tickTexEvict()                     noexcept {}
    inline void endTexFrame()                      noexcept {}
    inline void writeToFramePixels(uint32_t*, int) noexcept {}
}

#endif // XBOX_DIAG_ENABLED
#endif // __XBOX__
