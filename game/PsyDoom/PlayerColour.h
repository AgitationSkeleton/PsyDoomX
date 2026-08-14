#pragma once

//------------------------------------------------------------------------------------------------------------------------------------------
// Telling players apart by colour, the way PC Doom does.
//
// PSX Doom draws both players green because it never had the machinery: the PC game recolours the player sprite through
// a translation table applied per column as it is drawn, and the PlayStation renderer has no such step - it hands a
// texture and a palette to the GPU and that is that.
//
// What the PC game does, from 'R_InitTranslationTables' in 'linuxdoom-1.10/r_draw.c':
//
//     if (i >= 0x70 && i <= 0x7f) {
//         translationtables[i]       = 0x60 + (i & 0xf);   // gray  - 'indigo' in the manual
//         translationtables[i + 256] = 0x40 + (i & 0xf);   // brown
//         translationtables[i + 512] = 0x20 + (i & 0xf);   // red
//     }
//
// Sixteen entries, and every other colour left alone. So the recolouring is done here where the PlayStation can afford
// it - once, offline, on the sprite's pixels - rather than per column at draw time. The launcher writes recoloured
// copies of the fifty one 'PLAY' lumps under sprite names of their own, and the renderer picks the right one when it
// comes to draw a player.
//
// What must NOT be recoloured, and is not:
//
//   - Anything that is not a player. The choice is made from 'mobj_t::player', which only a player's own object has, so
//     the dead marines and gibs already lying about the level keep the green they were drawn with.
//   - The player's own hands. The PC game does not translate the weapon sprite either - 'R_DrawPSprite' clears the
//     translation flags - so what a player sees of themselves is unchanged.
//
// What must be, and is: a player's corpse. The PC game keeps the flags on the body, and this keeps the colour with it
// by writing the recoloured sprite into the corpse when it is orphaned at respawn.
//
// Not portable: the recoloured sprites are made by the Xbox launcher.
//------------------------------------------------------------------------------------------------------------------------------------------
#if defined(__XBOX__)

#include "Macros.h"

#include <cstdint>

class WadList;
struct mobj_t;
struct spritedef_t;

BEGIN_NAMESPACE(PlayerColour)

// The four colours PC Doom gives players, in the order it hands them out
enum Colour : int32_t {
    GREEN = 0,      // What the sprites already are; needs no recolouring
    INDIGO,
    BROWN,
    RED,
    COLOUR_COUNT
};

// The green ramp in the palette, and where each colour's ramp starts. Straight from 'R_InitTranslationTables'.
static constexpr int32_t GREEN_RAMP_START = 0x70;
static constexpr int32_t RAMP_LENGTH      = 16;

// Adding a colour is a matter of adding a ramp start, a name and a four character sprite name below - nothing else here
// knows how many there are.
int32_t     rampStart(const Colour colour) noexcept;
const char* displayName(const Colour colour) noexcept;
const char* spriteName(const Colour colour) noexcept;    // Four characters: 'PLAY' for green, 'PLYI'/'PLYB'/'PLYR' otherwise

// Where the launcher leaves the recoloured sprites for a given edition, and for the running game
const char* wadPathForEdition(const int32_t editionIdx) noexcept;

// Add the recoloured sprites to the main WAD list. Call from 'W_Init'.
void addOverrideWad(WadList& wadList) noexcept;

// Find the recoloured sprites among the loaded sprites. Call after 'P_InitSprites'.
void init() noexcept;

// Is this colour usable? Green always is; the rest need the launcher to have written them out.
bool isAvailable(const Colour colour) noexcept;

// Is there more than one colour to choose between? If not the setting should not be offered.
bool haveChoice() noexcept;

// The next available colour in the given direction
Colour nextAvailable(const Colour colour, const int32_t dir) noexcept;

// What colour a player is set to, falling back to green for anything not available
Colour forPlayer(const int32_t playerIdx) noexcept;

// Which sprite to draw a thing with, or null to use its own. Returns null for anything that is not a coloured player.
const spritedef_t* spriteDefForThing(const mobj_t& thing) noexcept;

// Keep a player's colour on their body when it stops being theirs.
//
// The body's link back to the player is broken at respawn, so from then on there is nothing left on it to say whose it
// was. This writes the recoloured sprite into it once, at that moment; the body is in its final state and will not
// change again, so nothing overwrites it. The PC game gets this for free because its marker is a flag on the object.
void markCorpse(mobj_t& corpse, const int32_t playerIdx) noexcept;

// The colour to draw a player's marker on the status bar with, as 8 bit RGB
void statusBarRgb(const Colour colour, uint8_t& r, uint8_t& g, uint8_t& b) noexcept;

END_NAMESPACE(PlayerColour)

#endif  // #if defined(__XBOX__)
