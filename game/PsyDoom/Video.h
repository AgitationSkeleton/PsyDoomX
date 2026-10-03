#pragma once

#include "Macros.h"

#include <cstdint>

struct SDL_Window;

BEGIN_NAMESPACE(Video)

class IVideoBackend;

// The original render/draw and output/display resolution of the game: the game rendered to a 256x240 framebuffer but stretched this image to
// approximately 292.57x240 in square pixel terms - even though the game asks for a 320x200 'pixel' display (CRTs did not have pixels).
//
// For more info on PS1, NES and PSX Doom pixel aspect ratios, see:
//  https://github.com/libretro/beetle-psx-libretro/issues/510
//  http://forums.nesdev.com/viewtopic.php?t=8983
//  https://doomwiki.org/wiki/Sony_PlayStation
//
static constexpr int32_t ORIG_DRAW_RES_X = 256;
static constexpr int32_t ORIG_DRAW_RES_Y = 240;
static constexpr int32_t ORIG_DISP_RES_X = 292;
static constexpr int32_t ORIG_DISP_RES_Y = 240;

// Which video backend is being used
enum BackendType {
    SDL,        // Using an SDL backend which only supports the classic renderer
    Vulkan      // Using a Vulkan backend which supports both the classic renderer and the new Vulkan renderer
};

extern BackendType  gBackendType;
extern SDL_Window*  gpSdlWindow;
extern int32_t      gTopOverscan;
extern int32_t      gBotOverscan;

void initVideo() noexcept;
void shutdownVideo() noexcept;

void getClassicFramebufferWindowRect(
    const float windowW,
    const float windowH,
    float& rectX,
    float& rectY,
    float& rectW,
    float& rectH
) noexcept;

void displayFramebuffer() noexcept;
bool isUsingVulkanRenderPath() noexcept;
IVideoBackend& getCurrentBackend() noexcept;

#if defined(__XBOX__)
    // With the frame rate locked: how many of the game's vblanks the frame just presented covers, worked out from the
    // television's real refreshes. -1 when there is no answer - unlocked, or the first frame after a change. Taking it
    // clears it, so a frame is only ever counted once. See 'waitForLockedPresent' in 'VideoBackend_SDL.cpp'.
    int32_t xbTakeLockedElapsedVBlanks() noexcept;

    // What the lock holds the frame rate to on this console and disc: 30 on a 60Hz television, 25 at 50Hz, and 20 for a
    // PAL disc on a 60Hz set, which needs three refreshes to cover two of its own vblanks.
    int32_t xbLockedFps() noexcept;
#endif

END_NAMESPACE(Video)
