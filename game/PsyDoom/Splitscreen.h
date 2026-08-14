//------------------------------------------------------------------------------------------------------------------------------------------
// Two player splitscreen, in place of the link cable.
//
// The link cable only ever carried player two's inputs - it is a transport, not a game mode. Everything else about a
// two player game already exists: 'MAXPLAYERS' is 2, 'gPlayers' and 'gTickInputs' are arrays, co-op and deathmatch
// rules are implemented, and 'gCurPlayerIndex' already decides whose eyes the renderer draws from. So this does not
// add a mode; it replaces a network with a second pad in the same room.
//
// The second view is not something the game boots into. It appears when player one confirms a co-op or deathmatch
// game - exactly where the other console used to be searched for - and goes away when that game ends and the main
// menu returns. Single player never touches any of this.
//
// See 'docs/SPLITSCREEN_PLAN.md' for the layouts and the order of work.
//------------------------------------------------------------------------------------------------------------------------------------------
#pragma once

#include "Macros.h"

#include <cstdint>

BEGIN_NAMESPACE(Splitscreen)

//------------------------------------------------------------------------------------------------------------------------------------------
// How the screen is divided between the two players
//------------------------------------------------------------------------------------------------------------------------------------------
enum class Layout : uint8_t {
    // Two 4:3 viewports side by side, 320x240 each on a 640x480 screen, with bars above and below.
    //
    // Needs nothing from the renderer: the view is drawn at its native 256x240 and the present places it into a
    // 320x240 region. 256x240 shown in a 4:3 region is how the game is meant to look, since PlayStation pixels are
    // not square, so no aspect distortion is introduced.
    SideBySide,

    // One above the other, 640x240 each, using the whole screen.
    //
    // That region is far wider than 4:3, so filling it honestly means rendering a wider field of view rather than
    // stretching a 4:3 image. PsyDoom already does this on desktop through 'Config::gLogicalDisplayW'. Better looking
    // and more work; see the plan.
    TopAndBottom,
};

// Is a splitscreen game running? False for all of single player.
bool isActive() noexcept;

// Is this player one of the ones sitting in front of this console?
//
// Plain Doom asks 'is this player gCurPlayerIndex' to decide whether to play a pickup sound or flash the screen,
// because on one console only one player is ever local. Splitscreen has two, and player two was failing every one
// of those tests - no pickup sounds, no weapon up sound, no screen flashes.
bool isLocalPlayer(const int32_t playerIdx) noexcept;

// Which way the screen is currently divided
Layout getLayout() noexcept;
void setLayout(const Layout layout) noexcept;

// Start a splitscreen game: assign the second pad and switch the present to a two viewport layout.
//
// Called where the link cable connect used to happen, once player one has chosen co-op or deathmatch and confirmed.
void begin() noexcept;

// End it: put the screen back to one full size view and release the second player.
//
// Called when the game returns to the main menu, where 'Network::shutdown' is called for a link cable game.
void end() noexcept;

//------------------------------------------------------------------------------------------------------------------------------------------
// Player one's finished view, kept while player two's is drawn.
//
// The world is drawn into one place in PlayStation VRAM and the present reads that same place, so drawing player two
// overwrites player one. Rather than find a second area of VRAM and manage draw environments for it, player one's view
// is copied out once it is finished and handed to the present alongside player two's.
//
// 120KB and a copy per frame, against the alternative of restructuring how the game addresses VRAM.
//------------------------------------------------------------------------------------------------------------------------------------------
//------------------------------------------------------------------------------------------------------------------------------------------
// Where each player's view is drawn in PlayStation VRAM.
//
// The game already has two 256x240 framebuffers side by side, at (0,0) and (256,0), which it alternates for double
// buffering. Splitscreen gives one to each player and uses both at once, which costs nothing here because the present
// writes to the Xbox framebuffer directly and only reads these as source rectangles.
//------------------------------------------------------------------------------------------------------------------------------------------
static constexpr uint32_t VIEW_VRAM_X[2] = { 0, 256 };
static constexpr uint32_t VIEW_VRAM_Y[2] = { 0, 0 };

void captureView() noexcept;
// Player one's view for this frame, or null if this frame did not draw a pair.
//
// Null matters: the present composites two viewports only when both were actually drawn. Menus, pause, loading
// plaques and the automap all present without going through the two view path, and compositing those against a
// capture from some earlier frame is what made the screen flicker and player one's view bleed into player two's.
const uint16_t* getCapturedView() noexcept;

// Called by the present once it has used the capture, so the next frame must draw its own
void releaseCapturedView() noexcept;

// Whether the screen needs wiping once before the next present, and clears the request.
//
// Leaving splitscreen leaves its bars behind: the single view does not cover the whole screen, so the menu returns
// with splitscreen pixels still framing it.
bool consumeScreenClearRequest() noexcept;

// Point drawing at one player's half of VRAM, clearing that half first.
//
// The equivalent of Doom Legacy moving 'view_window_y' and swapping its row address table - it decides which part of
// the screen the rasteriser writes into. Splitscreen does its own clearing because the automatic one wipes whichever
// view it is pointed at, including views that have already been drawn and are waiting to be shown.
void beginPlayerView(const int32_t playerIdx) noexcept;

// Point drawing at one player's half WITHOUT clearing it.
//
// For things that draw over a view rather than replacing it - the options menu is the case that needs it, since it
// runs its own loop and draws straight over whichever half is current.
void pointDrawAt(const int32_t playerIdx) noexcept;

// Show one full size view for a sequence that is not per player.
//
// The text screens between episodes and the cast call are one thing shown to both players, not two points of view.
// Splitting them leaves the other half holding whatever was on it last, which reads as dead pixels beside the screen
// being read. Splitscreen stays active throughout - only the presentation changes - so inputs, timing and both
// players' state carry on as normal and gameplay resumes split.
void beginFullScreenSequence() noexcept;
void endFullScreenSequence() noexcept;
bool isFullScreenSequence() noexcept;

END_NAMESPACE(Splitscreen)
