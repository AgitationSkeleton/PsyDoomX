#include "Splitscreen.h"

#include "Controls.h"
#include "Input.h"

#include "Gpu.h"
#include "PsxVm.h"
#include "Video.h"

#include <cstring>

#include "Doom/Base/i_main.h"
#include "PsyQ/LIBGPU.h"

#if defined(__XBOX__)
    #include "XboxLog.h"
#endif

BEGIN_NAMESPACE(Splitscreen)

static bool     gbActive = false;
static Layout   gLayout = Layout::SideBySide;

// Player one's view, copied out of VRAM before player two's is drawn over it
static uint16_t gCapturedView[Video::ORIG_DRAW_RES_X * Video::ORIG_DRAW_RES_Y];
static bool     gbHaveCapturedView = false;

// Set when the screen must be wiped once, because what is about to be drawn does not cover everything that is on it
static bool     gbNeedScreenClear = false;

// Showing one full size view, for a sequence that is the same for both players
static bool     gbFullScreenSequence = false;

//------------------------------------------------------------------------------------------------------------------------------------------
// Point drawing at one player's half of VRAM, clearing it first.
//
// This is the equivalent of Doom Legacy moving 'view_window_y' and swapping its row address table: it decides which
// part of the screen the rasteriser writes into. Each player owns one of the two framebuffers the game already has.
//------------------------------------------------------------------------------------------------------------------------------------------
void beginPlayerView(const int32_t playerIdx) noexcept {
    DRAWENV& env = gDrawEnvs[(playerIdx == 1) ? 1 : 0];

    // Clear this view's area, and only this view's area
    const bool bWasBg = env.isbg;
    env.isbg = true;
    LIBGPU_PutDrawEnv(env);
    env.isbg = bWasBg;
}

void pointDrawAt(const int32_t playerIdx) noexcept {
    DRAWENV& env = gDrawEnvs[(playerIdx == 1) ? 1 : 0];
    const bool bWasBg = env.isbg;
    env.isbg = false;               // Do NOT wipe what is already there - this draws on top of a finished view
    LIBGPU_PutDrawEnv(env);
    env.isbg = bWasBg;
}

void beginFullScreenSequence() noexcept {
    if (gbFullScreenSequence)
        return;

    gbFullScreenSequence = true;
    gbNeedScreenClear = true;       // The two viewports are about to be replaced by one, so clear what they leave
    pointDrawAt(0);                 // One view, drawn where the full screen present reads from
}

void endFullScreenSequence() noexcept {
    if (!gbFullScreenSequence)
        return;

    gbFullScreenSequence = false;
    gbNeedScreenClear = true;       // And back the other way, so the full size view does not linger in the bars
}

bool isFullScreenSequence() noexcept {
    return gbFullScreenSequence;
}

bool isActive() noexcept {
    return gbActive;
}

bool isLocalPlayer(const int32_t playerIdx) noexcept {
    if (playerIdx == gCurPlayerIndex)
        return true;

    // Player two is sitting right here too
    return (gbActive && (playerIdx == 1));
}

Layout getLayout() noexcept {
    return gLayout;
}

void setLayout(const Layout layout) noexcept {
    if (gLayout != layout) {
        gLayout = layout;
        gbNeedScreenClear = true;   // The bars move when the layout changes, so what is outside the new one must go
    }
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Start a splitscreen game
//------------------------------------------------------------------------------------------------------------------------------------------
void begin() noexcept {
    if (gbActive)
        return;

    gbActive = true;
    gbNeedScreenClear = true;   // Going from one full size view to two leaves the old picture in the bars

    // This console's player is player one, and player one's view is the one shown first.
    //
    // The link cable connect used to decide this, since the two consoles had to agree which of them was which. There
    // is nothing to agree here, so it is set outright - and it has to be set, because skipping the connect skips the
    // only place that ever did.
    gCurPlayerIndex = 0;

    // Take the automatic clear off both draw environments.
    //
    // 'LIBGPU_PutDrawEnv' wipes the draw area when 'isbg' is set, and both environments set it. That is right for one
    // view a frame and ruinous for two: 'I_DrawPresent' calls PutDrawEnv before the present reads VRAM, so one of the
    // two views was being erased every frame just before it was due on screen. It is also why restoring the draw area
    // after player two's view turned player one's black - the restore cleared it.
    //
    // Splitscreen does its own clearing instead, once per view, immediately before that view is drawn.
    gDrawEnvs[0].isbg = false;
    gDrawEnvs[1].isbg = false;

    // Leave the active player as one. It is set around reading each player's inputs and around a player rebinding
    // their controls, and everything outside those places should go on seeing player one's bindings as before.
    Controls::setActivePlayer(0);

    #if defined(__XBOX__)
        XBOX_LOGI(
            Split,
            "splitscreen begin - layout %s",
            (gLayout == Layout::SideBySide) ? "side by side (two 4:3)" : "top and bottom (two wide)"
        );

        // What devices exist decides which code path player two's bindings take, and those paths read different state
        Input::logInputDevices();

        // Say how many input sources each player's key bindings actually have.
        //
        // Player two does nothing, both pads are open as game controllers, and every read is routed per player - so
        // the remaining explanation is that player two's bindings are empty. A binding with no sources returns zero
        // for ever and looks identical to a pad that is not there. This says which it is, in one line, rather than a
        // sixth round of inference.
        {
            const auto srcCount = [](const uint8_t player, const Controls::Binding binding) noexcept -> int {
                Controls::setActivePlayer(player);
                const int n = (int) Controls::getBindingData(binding).numInputSources;
                Controls::setActivePlayer(0);
                return n;
            };

            // What player two's pause binding actually resolves to.
            //
            // It has sources, their start button reaches the pad array cleanly, and the edge test is correct - yet the
            // gather never produces a pause edge for them. That leaves what those sources actually ARE: a source list
            // with the right count but no gamepad button in it behaves exactly like this.
            {
                Controls::setActivePlayer(1);
                const Controls::BindingData& pauseData = Controls::getBindingData(Controls::Binding::Toggle_Pause);

                for (uint32_t i = 0; (i < pauseData.numInputSources) && (i < 8); ++i) {
                    const Controls::InputSrc& src = pauseData.inputSources[i];
                    XBOX_LOGI(Split, "p2 pause source %u: device=%u input=%u subaxis=%u", (unsigned) i,
                        (unsigned) src.device, (unsigned) src.input, (unsigned) src.subaxis);
                }

                Controls::setActivePlayer(0);
            }

            XBOX_LOGI(
                Split,
                "bindings sources: p1 fwd=%d strafe=%d attack=%d pause=%d | p2 fwd=%d strafe=%d attack=%d pause=%d",
                srcCount(0, Controls::Binding::Analog_MoveForward),
                srcCount(0, Controls::Binding::Analog_StrafeRight),
                srcCount(0, Controls::Binding::Action_Attack),
                srcCount(0, Controls::Binding::Toggle_Pause),
                srcCount(1, Controls::Binding::Analog_MoveForward),
                srcCount(1, Controls::Binding::Analog_StrafeRight),
                srcCount(1, Controls::Binding::Action_Attack),
                srcCount(1, Controls::Binding::Toggle_Pause)
            );
        }
    #endif
}

//------------------------------------------------------------------------------------------------------------------------------------------
// End it and put the screen back to a single full size view
//------------------------------------------------------------------------------------------------------------------------------------------
void end() noexcept {
    if (!gbActive)
        return;

    gbActive = false;
    gbFullScreenSequence = false;
    gbHaveCapturedView = false;
    gbNeedScreenClear = true;   // The bars either side still hold splitscreen pixels; the menu does not cover them
    Controls::setActivePlayer(0);

    #if defined(__XBOX__)
        XBOX_LOGI(Split, "splitscreen end - back to a single full size view");
    #endif
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Copy player one's finished view out of VRAM, before player two's is drawn over the top of it
//------------------------------------------------------------------------------------------------------------------------------------------
void captureView() noexcept {
    const Gpu::Core& gpu = PsxVm::gGpu;
    const uint16_t* const pVram = (const uint16_t*) gpu.pRam;

    if (!pVram)
        return;

    // Read the area being DRAWN to, not the area being displayed.
    //
    // These are different buffers, and which is which changes every frame. The swap happens inside 'I_DrawPresent',
    // which runs after all the drawing, so at this point the display area still holds the previous frame while the
    // fresh view is in the draw area. Copying the display area gave player two's viewport last frame's picture, which
    // is why it looked dark and a step behind.
    const uint32_t srcX = (uint32_t) gpu.drawOffsetX;
    const uint32_t srcY = (uint32_t) gpu.drawOffsetY;

    // Row by row: the view is a rectangle inside a much wider VRAM
    for (uint32_t y = 0; y < Video::ORIG_DRAW_RES_Y; ++y) {
        const uint16_t* const pSrc = pVram + ((size_t)(y + srcY) * gpu.ramStride) + srcX;
        std::memcpy(&gCapturedView[y * Video::ORIG_DRAW_RES_X], pSrc, Video::ORIG_DRAW_RES_X * sizeof(uint16_t));
    }

    gbHaveCapturedView = true;
}

bool consumeScreenClearRequest() noexcept {
    const bool bNeeded = gbNeedScreenClear;
    gbNeedScreenClear = false;
    return bNeeded;
}

const uint16_t* getCapturedView() noexcept {
    return (gbHaveCapturedView) ? gCapturedView : nullptr;
}

void releaseCapturedView() noexcept {
    gbHaveCapturedView = false;
}

END_NAMESPACE(Splitscreen)
