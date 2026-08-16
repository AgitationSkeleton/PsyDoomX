#pragma once

//------------------------------------------------------------------------------------------------------------------------------------------
// Borrowing the Master Edition's extra monsters: the Arch-Vile, the Wolfenstein SS and Commander Keen.
//
// All three are already in this engine. Their types, states and sprite names are present because PsyDoom added them
// back for Master Edition support, so a Doom or Final Doom disc is missing only their artwork - the lumps 'VILE',
// 'FIRE', 'SSWV' and 'KEEN' simply have nothing under them.
//
// Which is the problem the super shotgun already solved here, and it is solved the same way: the launcher reads the
// lumps off the Master Edition's disc once and caches them, and the game adds that cache to its WAD list. The launcher
// is the only thing that sees all three discs - a game only ever sees the one it was started with - so it is the only
// thing that can do the reading.
//
// Unlike the super shotgun's frames, nothing is renamed. That rename existed so a game could hold its own set and a
// borrowed set at the same time and switch between them; here the running game has nothing under these names at all,
// so the borrowed lumps can simply be themselves.
//
// Nothing here decides whether the monsters get used. That falls out on its own: the Randomizer only offers a type the
// running game can actually draw, so these appear in the roster the moment their lumps do and are quietly absent when
// the Master Edition is not installed - which is exactly the behaviour asked for.
//
// Not portable: the launcher this depends on exists because this console needs one.
//------------------------------------------------------------------------------------------------------------------------------------------
#if defined(__XBOX__)

#include "Macros.h"

class WadList;

BEGIN_NAMESPACE(MasterMonsters)

// Where the launcher leaves the borrowed sprites
const char* wadPath() noexcept;

// The sprite names borrowed, and how many. Shared with the launcher so the two cannot disagree about what to look for.
extern const char* const SPRITE_NAMES[];
extern const int         NUM_SPRITE_NAMES;

// Add the borrowed monsters to the main WAD list, if the launcher managed to write them out.
// Call from 'W_Init', before the game's own WADs.
void addOverrideWad(WadList& wadList) noexcept;

// Did they load? For the log, so a game without them says why rather than just lacking monsters.
bool isLoaded() noexcept;

//--------------------------------------------------------------------------------------------------------------------------------------
// The sound side of the borrowing.
//
// Sprites alone leave the three mute, and not for want of samples: 'sfx_vilsit' is sequence 120 while a Doom module
// defines 110, so there is nothing to trigger. The samples have to arrive together with a module that knows what to do
// with them, which means standing the Master Edition's own module in for the running game's.
//
// That substitution was checked against the discs rather than assumed, by parsing all three modules the way
// 'wess_load_module' reads them and comparing the patch data element by element:
//
//      Final Doom  149/149 patches, 254/254 patch voices, 144/144 patch samples identical - an exact superset
//      Doom        127/127 patches, 122/122 patch samples identical; 175/176 patch voices
//
// Doom's one difference is patch voice 99 (belonging to patch 93), where 'reverb' reads 160 against the Master
// Edition's 128. The PSX driver never reads that field - the only patch voice fields it touches are 'adsr', 'note_max',
// 'sample_idx', 'base_note_frac', 'pan', 'pitchstep_down', 'pitchstep_up' and 'volume' - so nothing can hear it. Both
// games are safe. Worth knowing that the risk ran the opposite way to the obvious guess: Final Doom's module is the
// larger and looked like the dangerous one, and is in fact the perfect match.
//
// The comparison is worth repeating if a disc revision ever turns up that this does not suit. Parse each module the
// way 'wess_load_module' does - the 16 byte header, then per patch group a 28 byte header followed by its patches,
// patch voices, patch samples, drum patches and extra data, each present only if its load flag is set - and compare
// the patch arrays element by element. Comparing the files as raw bytes says nothing: the modules declare different
// sequence counts, so every offset past the header shifts and even identical patch data does not line up.
//--------------------------------------------------------------------------------------------------------------------------------------

// Where the launcher leaves the substitute module and the borrowed samples
const char* soundDirPath() noexcept;

// Work out whether both of those are actually present. Call once at startup, before anything reads the module.
void initSounds() noexcept;

// Are the borrowed sounds there? False on a console without the Master Edition, and the three then stay mute rather
// than the game failing.
bool haveBorrowedSounds() noexcept;

// The LCD holding the borrowed samples, by name, for 'wess_dig_lcd_load'
const char* monsterSoundLcdName() noexcept;

// The directory to point 'ProgArgs::gDataDirPath' at so the file override system substitutes the module, or null if
// there is nothing to substitute. Must be set before 'ModMgr::init' and before sound starts: the module is read once
// at startup and never again.
//
// Setting this alone is NOT enough on this console. 'ModMgr' finds overrides by iterating that directory, which nxdk
// cannot do, so the two files also have to be named to 'ModMgr::addFileOverride' after 'ModMgr::init' has run. Without
// that the substitution quietly does nothing at all.
const char* soundOverrideDirPath() noexcept;

// Report that the overrides were registered, and confirm the files can actually be opened through them.
//
// Worth doing because the failure this guards against was silent: pointing the override system at the cache without
// registering the names left the game using the disc's own module and nothing said so. Called once at startup.
void logSoundOverrides() noexcept;

END_NAMESPACE(MasterMonsters)

#endif  // #if defined(__XBOX__)
