#pragma once

//------------------------------------------------------------------------------------------------------------------------------------------
// Wearing another edition's super shotgun.
//
// The ten first person frames of the super shotgun - 'SHT2A0' to 'SHT2J0' - are genuinely different art in Doom, Final
// Doom and the Master Edition. Every one of the ten differs between Doom and Final Doom, and three of them differ in
// size as well, so this is a redraw rather than a recolour. The pickup lying on the floor, 'SGN2A0', is byte for byte
// identical in Doom and Final Doom and is deliberately left alone.
//
// How this works, and why it is not much code: PsyDoom already has all of the machinery.
//
//   - 'WadList' presents several WAD files as one, and 'forEachSpriteLump' walks their sprite ranges deliberately
//     backwards so that a WAD nearer the front of the list wins. Overriding sprites from an extra WAD is a thing it was
//     built to do.
//   - 'R_GetTexForLump' finds a sprite's texture through a lump-to-texture map covering every WAD in the list, so an
//     extra WAD's lumps get their own texture entries with nothing else asked of them.
//
// So the launcher takes the ten frames off each disc it finds and writes them into a small WAD, renamed to a sprite of
// their own - 'SSGD', 'SSGF', 'SSGM' - so that a game can hold its own set and the borrowed sets at the same time.
// Changing the setting then only has to point the 'SHT2' frames at a different set of lumps, which is why it takes
// effect immediately rather than on the next restart.
//
// Not portable: the paths are this console's, and this exists because the launcher can read all three discs. A machine
// running one game from one disc has nothing to borrow from.
//------------------------------------------------------------------------------------------------------------------------------------------
#if defined(__XBOX__)

#include "Macros.h"

#include <cstdint>

class WadList;

BEGIN_NAMESPACE(SsgStyle)

// Which edition's super shotgun to use
enum Style : int32_t {
    STYLE_DOOM = 0,
    STYLE_FINAL,
    STYLE_MASTER,
    STYLE_COUNT
};

// How many frames the sprite has: 'A' through 'J'
static constexpr int32_t NUM_FRAMES = 10;

// The sprite name each borrowed set is written out under.
//
// Four characters because a sprite name is four characters and the frame letter follows it, giving 'SSGDA0' and so on.
// Shared with the launcher, which is what writes these files, so the two cannot disagree about the name.
const char* spriteName(const Style style) noexcept;

// Where the launcher leaves each set, and what a menu should call it
const char* wadPath(const Style style) noexcept;
const char* displayName(const Style style) noexcept;

// Which edition the running game came with. Its frames are in its own main WAD and need nothing borrowing.
Style nativeStyle() noexcept;

// Can this style be used? True for the native one, and for any the launcher managed to write out.
bool isAvailable(const Style style) noexcept;

// Add the borrowed sets to the main WAD list. Call from 'W_Init', before the game's own WADs.
void addOverrideWads(WadList& wadList) noexcept;

// Remember the game's own super shotgun frames. Call after 'P_InitSprites', which creates the sprites this reads.
void init() noexcept;

// Point the super shotgun's frames at the set the given player asked for.
//
// Per player, because in splitscreen the two are separate people - but there is only one sprite list between them, so
// this is called as each view is about to be drawn rather than once. It copies ten frame records, which is nothing
// beside drawing the view that follows it.
void applyForPlayer(const int32_t playerIdx) noexcept;

// The next style in the given direction that is actually available, or the current one if there is no other
Style nextAvailable(const Style style, const int32_t dir) noexcept;

// Is there more than one style to choose between? If not, the setting should not be offered.
bool haveChoice() noexcept;

END_NAMESPACE(SsgStyle)

#endif  // #if defined(__XBOX__)
