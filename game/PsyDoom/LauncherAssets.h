#pragma once

//------------------------------------------------------------------------------------------------------------------------------------------
// Reading a game's disc from the launcher, so it can wear that game's menu.
//
// See the notes in the .cpp. The point of this module existing separately is that none of it runs while a game is being
// played - it is the launcher's own business, and it must not be able to disturb the three games it launches.
//------------------------------------------------------------------------------------------------------------------------------------------
#include "Macros.h"

#include <cstdint>

BEGIN_NAMESPACE(LauncherAssets)

// Open a disc, build its file system and find the WAD. Says whether all of that worked.
//
// The first step of the styled launcher, on its own, because everything after it depends on this working and none of it
// can be judged until this does.
// Which lump an edition's menu background is, and which palette it is drawn with.
//
// Neither can be guessed. Doom and Final Doom share the lump name and differ only in palette; the Master Edition uses a
// different picture entirely, and says so in its own MAPINFO on the disc rather than anywhere in PsyDoom:
//
//     PicBack = "COVERG"  28
//
// Read out of the disc image directly rather than discovered by trying things on hardware, which is what the first two
// attempts at this did.
struct MenuArt {
    const char* lumpName;
    uint32_t    palette;
};

static constexpr MenuArt MENU_ART_DOOM       = { "BACK",   0  };    // MAINPAL
static constexpr MenuArt MENU_ART_FINAL_DOOM = { "BACK",   17 };    // TITLEPAL
static constexpr MenuArt MENU_ART_MASTER     = { "COVERG", 28 };    // From PSXGMINF.TXT on the disc

// Open a disc, decode what the menu needs from it, and take a copy of its super shotgun sprites.
//
// 'ssgStyle' says which edition this disc is, so its sprites are written out under a name of their own for the other
// editions to borrow. See 'SsgStyle.h' for what that is for and why the launcher is the only thing that can do it -
// the games only ever see one disc each, and this is the one place all three are readable at once.
bool probeDisc(const char* const cuePath, const MenuArt& menuArt, const int32_t ssgStyle) noexcept;

// Draw the loaded style's background, scaled to fill the screen.
//
// Returns false if no style is loaded, which is how the launcher knows to stay with its plain text look.
bool drawCachedBackground(const char* const cuePath) noexcept;

// Repaint one rectangle of the screen from the background, undrawing whatever was on top of it.
//
// This is what stops the menu flickering. The console has one framebuffer and no back buffer, so the display reads it
// while it is being written: redrawing the whole background and then the text over it means that for part of every
// repaint the text is simply not there yet, which is seen as the text blinking. Repainting only the part that actually
// changed - a cursor cell is 32x36 against the background's 245,760 pixels - makes that window too small to see.
void restoreBackgroundRect(const int32_t x, const int32_t y, const int32_t w, const int32_t h) noexcept;

// Load an edition's cached menu assets, ready for drawing. Call once when the style changes.
bool useStyle(const char* const cuePath) noexcept;

// Which CD track that disc's main menu music is on, as 'probeDisc' found it. Zero means "do not play anything".
//
// Read from the disc's own file system rather than assumed: the '.RAW' placeholders in 'PSXDOOM/CDAUDIO' start inside
// the audio track that holds them, which is how the game works this out too.
int32_t cachedMusicTrack(const char* const cuePath) noexcept;

// Draw with PSX Doom's own fonts and cursor, from the STATUS atlas of whichever style is loaded.
//
// The glyph tables are the game's own - 'gBigFontChars' is in this binary already - so these letters are the same
// letters the game draws, not an imitation of them.
void drawBigText(const int32_t x, const int32_t y, const char* const str) noexcept;
void drawSmallText(const int32_t x, const int32_t y, const char* const str) noexcept;
void drawSkull(const int32_t x, const int32_t y, const int32_t frame) noexcept;
int32_t bigTextWidth(const char* const str) noexcept;

// How big the things above come out on screen. Everything is drawn at twice the PlayStation's size, the way the game
// itself shows it. Given here so the launcher can work out what area a repaint has to cover without pulling the game's
// own headers in; the .cpp asserts these against the game's constants so they cannot drift apart.
static constexpr int32_t CURSOR_DRAW_W  = 16 * 2;
static constexpr int32_t CURSOR_DRAW_H  = 18 * 2;
static constexpr int32_t BIG_TEXT_DRAW_H = 16 * 2;

// Is a style loaded and drawable? False means the launcher should stay with its plain text look.
bool isStyleLoaded() noexcept;

END_NAMESPACE(LauncherAssets)
