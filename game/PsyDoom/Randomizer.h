#pragma once

//------------------------------------------------------------------------------------------------------------------------------------------
// The Randomizer game mode.
//
// A fourth entry on the main menu's game mode selector. What it does is decided here and nowhere else: none of it is
// configurable, and turning it on changes a level rather than changing a setting.
//
// It is single player with a flag rather than a fourth game type, and that is deliberate. Seventy eight places in the
// engine branch on the game type, most of them asking "is this multiplayer" by testing against 'gt_single'. A fourth
// value would be caught by all of them and each would need a decision; a missed one would quietly make this behave
// like a network game. The flag leaves every one of them alone, and only the handful of places that genuinely differ
// ask about it.
//
// Nothing in here is specific to one console. It is meant to be liftable to the Xbox port unchanged, so it keeps to
// the engine's own types and does its own random number generation rather than borrowing a platform's.
//------------------------------------------------------------------------------------------------------------------------------------------
#include "Macros.h"

#include <cstdint>

enum mobjtype_t : int32_t;
struct mobj_t;

BEGIN_NAMESPACE(Randomizer)

// Is the Randomizer mode active for the game being played?
//
// Set when a game is started from the menu and cleared when one ends, so it is safe to ask at any point. Everything
// else in this module does nothing at all when this is false.
extern bool gbEnabled;

// Turn the mode on or off. Called when starting a game and when returning to the menu.
void setEnabled(const bool bEnabled) noexcept;

// May the player save or load in the game currently being played?
//
// False in the Randomizer: a run is meant to be played as it was rolled, and a save would in any case be reloaded
// into a differently randomized level, since the roll happens on load.
bool allowSaveAndLoad() noexcept;

// Randomize everything in the level that may be randomized.
//
// Call once at the end of level setup, after every thing has been spawned. Doing it afterwards rather than as each
// thing spawns is what lets a candidate be tested against the things already in the map, and it is also the only
// point at which the whole set is known - which the sprite budget will need.
void randomizeLevel() noexcept;

// Pick a different sky for the level, including the animated fire sky if the game has one.
//
// Call once the map's own sky has been decided, and before the fire sky is set up: what makes a sky a fire sky is the
// name of the texture it ended up with, so choosing the texture is all this has to do.
void randomizeSky() noexcept;

// Which music track to play, given the one the map asked for.
//
// Returns the map's own choice unchanged when the mode is off, so callers do not need to ask whether it is on.
int32_t chooseMusicTrack(const int32_t mapMusicTrack) noexcept;

// Report what the level cost in sprite memory, and what there was to spend. Call after precaching.
void reportSpriteBudget() noexcept;

// Hand the player a weapon to face the level with.
//
// Call once the player has been spawned, which is after the roll rather than before it. A rolled level can put a
// crowd in front of the player that the map never had, and a player who has just died starts with a pistol; this is
// what stops those two facts meeting. It runs on every level for the same reason.
void grantStartingWeapon() noexcept;

// A readable name for a thing type, for the log. Never null.
const char* nameOfType(const mobjtype_t type) noexcept;

// Tell the Randomizer that a map thing was never spawned at all.
//
// Player and deathmatch starts are dealt with by 'P_SpawnMapThing' before anything is created, so they never appear in
// the list of things and cannot be reported as left alone unless they are counted as they go past. Does nothing when
// the mode is off.
void noteMapThingSkipped(const int32_t doomednum) noexcept;

// What a thing was before it was randomized, or its current type if it was left alone.
//
// The boss death specials are keyed on what died, so a map that waits for the last Mancubus has to keep waiting for
// whatever that Mancubus became. Everything asking "is this one of those" must ask this rather than 'mobj.type'.
mobjtype_t originalTypeOf(const mobj_t& mobj) noexcept;

END_NAMESPACE(Randomizer)
