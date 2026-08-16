//------------------------------------------------------------------------------------------------------------------------------------------
// The Randomizer game mode. See the header.
//------------------------------------------------------------------------------------------------------------------------------------------
#include "Randomizer.h"

#include "Game.h"

#include "Doom/Base/i_main.h"
#include "Doom/doomdef.h"
#include "Doom/Game/g_game.h"
#include "Doom/Game/info.h"
#include "Doom/Game/p_inter.h"
#include "Doom/Game/p_map.h"
#include "Doom/Game/p_maputl.h"
#include "Doom/Game/p_mobj.h"
#include "Doom/Game/p_move.h"
#include "Doom/Game/p_spec.h"
#include "Doom/Game/p_tick.h"
#include "Doom/Base/w_wad.h"
#include "Doom/Base/z_zone.h"
#include "Doom/Renderer/r_data.h"

#include "MapInfo/MapInfo.h"
#include "MasterMonsters.h"
#include "MobjSpritePrecacher.h"
#include "Doom/Game/sprinfo.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

// The only console specific thing in here, and only so that a roll can be read back off a real machine's log
#if defined(__XBOX__)
    #include "XboxLog.h"
#endif

BEGIN_NAMESPACE(Randomizer)

bool gbEnabled = false;

//------------------------------------------------------------------------------------------------------------------------------------------
// A random number generator of this mode's own.
//
// Deliberately not the game's 'P_Random'. That one drives gameplay and is played back exactly during demos and kept in
// step across a network game; drawing from it here would change what every monster does afterwards. This one is seeded
// from the clock so a level is never rolled the same way twice, which is the point of the mode.
//------------------------------------------------------------------------------------------------------------------------------------------
static uint32_t gRngState = 1;

static void seedRng() noexcept {
    const uint64_t now = (uint64_t) std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()
    ).count();

    gRngState = (uint32_t)(now ^ (now >> 32));

    if (gRngState == 0) {
        gRngState = 1;      // Zero is a fixed point of the generator below and would give the same number forever
    }
}

static uint32_t nextRandom() noexcept {
    // Xorshift: small, fast, and good enough to shuffle a list of monsters
    gRngState ^= gRngState << 13;
    gRngState ^= gRngState >> 17;
    gRngState ^= gRngState << 5;
    return gRngState;
}

static int32_t randomBelow(const int32_t limit) noexcept {
    return (limit > 0) ? (int32_t)(nextRandom() % (uint32_t) limit) : 0;
}

//------------------------------------------------------------------------------------------------------------------------------------------
// What to call each thing in the log.
//
// In the order of 'mobjtype_t', and taken from what the engine's own table says each type is rather than from what the
// PC game calls the same number - they do not always agree. This engine gives the Arch-Vile DoomEd number 91 where the
// PC gives it 64, so anything written from memory would have been wrong.
//
// Runs past the 137 built in types to cover the sixteen markers PsyDoom adds after them, because the count the game
// runs with is 'gNumMobjInfo' rather than the size of the base table. Without them the log said 'Unknown' sixteen
// times over.
//------------------------------------------------------------------------------------------------------------------------------------------
static const char* const gTypeNames[] = {
    "Player", "Zombieman", "Shotgun Guy",
    "Revenant", "Revenant Missile", "Smoke",
    "Mancubus", "Mancubus Fireball", "Chaingunner",
    "Imp", "Demon", "Cacodemon",
    "Baron of Hell", "Hell Knight", "Lost Soul",
    "Spider Mastermind", "Arachnotron", "Cyberdemon",
    "Pain Elemental", "Barrel", "Imp Fireball",
    "Cacodemon Fireball", "Baron Fireball", "Rocket",
    "Plasma Bolt", "BFG Ball", "Arachnotron Plasma",
    "Bullet Puff", "Blood", "Teleport Fog",
    "Item Fog", "Teleport Exit", "BFG Extra",
    "Green Armor", "Megarmor", "Health Bonus",
    "Armor Bonus", "Blue Keycard", "Red Keycard",
    "Yellow Keycard", "Yellow Skull Key", "Red Skull Key",
    "Blue Skull Key", "Stimpack", "Medikit",
    "Soulsphere", "Invulnerability", "Berserk",
    "Invisibility", "Radiation Suit", "Computer Map",
    "Light Visor", "Megasphere", "Clip",
    "Box of Bullets", "Rocket Ammo", "Box of Rockets",
    "Cell Charge", "Cell Pack", "Shotgun Shells",
    "Box of Shells", "Backpack", "BFG9000",
    "Chaingun", "Chainsaw", "Rocket Launcher",
    "Plasma Rifle", "Shotgun", "Super Shotgun",
    "Tall Techno Lamp", "Short Techno Lamp", "Floor Lamp",
    "Tall Green Pillar", "Short Green Pillar", "Tall Red Pillar",
    "Short Red Pillar", "Pillar with Skull", "Pillar with Heart",
    "Evil Eye", "Floating Skulls", "Grey Tree",
    "Tall Blue Torch", "Tall Green Torch", "Tall Red Torch",
    "Short Blue Torch", "Short Green Torch", "Short Red Torch",
    "Stalagmite", "Techno Pillar", "Candle",
    "Candelabra", "Hanging Victim Twitching", "Hanging Victim Arms Out",
    "Hanging Victim One-Legged", "Hanging Pair of Legs", "Hanging Victim Arms Out (Passable)",
    "Hanging Pair of Legs (Passable)", "Hanging Victim One-Legged (Passable)", "Hanging Leg",
    "Hanging Leg (Passable)", "Hanging Chain", "Blood Hook",
    "Hanging Lamp", "Dead Cacodemon", "Dead Zombieman",
    "Dead Demon", "Dead Imp", "Dead Shotgun Guy",
    "Bloody Mess", "Bloody Mess 2", "Five Skull Pole",
    "Pile of Skulls and Candles", "Pool of Blood and Guts", "Skull on a Pole",
    "Impaled Human", "Twitching Impaled Human", "Large Brown Tree",
    "Burning Barrel", "Hanging Victim Guts Removed", "Hanging Victim Guts and Brain Removed",
    "Hanging Torso Looking Down", "Hanging Torso Open Skull", "Hanging Torso Looking Up",
    "Hanging Torso Brain Removed", "Pool of Blood", "Pool of Blood 2",
    "Pool of Brains", "Hanging Lamp 2", "Arch-Vile",
    "Arch-Vile Fire", "Wolfenstein SS", "Commander Keen",
    "Icon of Sin", "Monster Spawner", "Spawn Target",
    "Spawn Cube", "Spawn Fire",

    // PsyDoom's map markers, which follow the built in types
    "Marker 1", "Marker 2", "Marker 3", "Marker 4",
    "Marker 5", "Marker 6", "Marker 7", "Marker 8",
    "Marker 9", "Marker 10", "Marker 11", "Marker 12",
    "Marker 13", "Marker 14", "Marker 15", "Marker 16",
};

const char* nameOfType(const mobjtype_t type) noexcept {
    if ((type >= 0) && ((size_t) type < C_ARRAY_SIZE(gTypeNames)))
        return gTypeNames[type];

    return "Unknown";
}

//------------------------------------------------------------------------------------------------------------------------------------------
// What the roll did, kept so that it can be written out afterwards.
//
// Counted rather than listed one by one: a map has well over a hundred things in it, and forty lines each saying
// 'Imp->Barrel' are harder to read than one line saying it happened forty times.
//------------------------------------------------------------------------------------------------------------------------------------------
struct TallyEntry {
    mobjtype_t  from;
    mobjtype_t  to;
    int32_t     count;
};

static std::vector<TallyEntry> gReplaced;       // Became something else
static std::vector<TallyEntry> gUntouched;      // Left exactly as it was, for whatever reason
static std::vector<TallyEntry> gNothingFit;     // Left alone because nothing at all would fit there
static std::vector<TallyEntry> gNoArtwork;      // Types the running game has no sprites for, so cannot offer
static std::vector<TallyEntry> gSkipped;        // Never became a thing at all: player and deathmatch starts

// How many monsters came out of the roll wearing each of the three traits
static int32_t gNumNightmare = 0;
static int32_t gNumSpectre = 0;
static int32_t gNumTransparent = 0;

static void tally(std::vector<TallyEntry>& list, const mobjtype_t from, const mobjtype_t to) noexcept {
    for (TallyEntry& entry : list) {
        if ((entry.from == from) && (entry.to == to)) {
            entry.count++;
            return;
        }
    }

    list.push_back({ from, to, 1 });
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Map things that were never spawned. Only of interest while the mode is on, and cleared once each roll is reported.
//------------------------------------------------------------------------------------------------------------------------------------------
void noteMapThingSkipped(const int32_t doomednum) noexcept {
    if (!gbEnabled)
        return;

    // A pseudo type, only ever used to name the thing in the log. These never become things, so they have no type.
    mobjtype_t asType;

    switch (doomednum) {
        case 1:     asType = (mobjtype_t) -1;   break;      // Player start
        case 2:
        case 3:
        case 4:     asType = (mobjtype_t) -2;   break;      // Co-op player starts
        case 11:    asType = (mobjtype_t) -3;   break;      // Deathmatch start
        default:    return;
    }

    tally(gSkipped, asType, asType);
}

static const char* nameOfSkipped(const mobjtype_t pseudoType) noexcept {
    switch ((int32_t) pseudoType) {
        case -1:    return "Player Start";
        case -2:    return "Co-op Player Start";
        case -3:    return "Deathmatch Start";
        default:    return "Unknown";
    }
}

//------------------------------------------------------------------------------------------------------------------------------------------
// What a thing started out as
//------------------------------------------------------------------------------------------------------------------------------------------
mobjtype_t originalTypeOf(const mobj_t& mobj) noexcept {
    return mobj.randomizerOriginalType;
}

void setEnabled(const bool bEnabled) noexcept {
    gbEnabled = bEnabled;
}

bool allowSaveAndLoad() noexcept {
    return (!gbEnabled);
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Things that are never replaced.
//
// These are not a matter of taste: each one is machinery that a map depends on, and swapping it breaks the map rather
// than varying it. Player and deathmatch starts are not here because they never become things at all - 'P_SpawnMapThing'
// deals with them and returns before anything is created.
//------------------------------------------------------------------------------------------------------------------------------------------
static bool isProtectedType(const mobjtype_t type) noexcept {
    switch (type) {
        // The six keys: the way through a map
        case MT_MISC4:  case MT_MISC5:  case MT_MISC6:
        case MT_MISC7:  case MT_MISC8:  case MT_MISC9:
            return true;

        // The Icon of Sin: its head, the thing that shoots the boxes, and the places they land
        case MT_BOSSBRAIN:
        case MT_BOSSSPIT:
        case MT_BOSSTARGET:
            return true;

        // Where a teleporter puts you
        case MT_TELEPORTMAN:
            return true;

        default:
            return false;
    }
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Things a map is waiting to see the last of.
//
// Seven types can have a map special hanging off them - the last Mancubus on Dead Simple lowers the floor, the last
// Baron on Phobos Anomaly opens the way out. 'gMapBossSpecialFlags' says which of them this particular map is waiting
// on, and is set by 'P_SpawnSpecials', which has already run by the time the roll happens.
//------------------------------------------------------------------------------------------------------------------------------------------
static uint32_t bossFlagForType(const mobjtype_t type) noexcept {
    switch (type) {
        case MT_FATSO:      return 0x01;    // Tag 666
        case MT_BABY:       return 0x02;    // Tag 667
        case MT_SPIDER:     return 0x04;    // Tag 668
        case MT_KNIGHT:     return 0x08;    // Tag 669
        case MT_CYBORG:     return 0x10;    // Tag 670
        case MT_BRUISER:    return 0x20;    // Tag 671
        case MT_KEEN:       return 0x40;    // Tag 672
        default:            return 0;
    }
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Is the way on through this map waiting for this particular thing to die?
//------------------------------------------------------------------------------------------------------------------------------------------
static bool isTriggerThing(const mobj_t& mobj) noexcept {
    const uint32_t flag = bossFlagForType(originalTypeOf(mobj));
    return ((flag != 0) && (((uint32_t) gMapBossSpecialFlags & flag) != 0));
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Is this type's artwork actually in the running game?
//
// Every type is defined in the engine, including several the Master Edition reimplemented, but a Doom disc carries no
// Archvile sprites - so spawning one would leave something with nothing to draw. A sprite with no frames is one whose
// lumps were not found, which is exactly the question being asked.
//------------------------------------------------------------------------------------------------------------------------------------------
static bool isTypeDrawable(const mobjtype_t type) noexcept {
    const mobjinfo_t& info = gMobjInfo[type];
    const statenum_t spawnState = info.spawnstate;

    if ((spawnState <= S_NULL) || (spawnState >= gNumStates))
        return false;

    const int32_t spriteNum = gStates[spawnState].sprite;

    if ((spriteNum < 0) || (spriteNum >= gNumSprites))
        return false;

    return (gSprites[spriteNum].numframes > 0);
}

//------------------------------------------------------------------------------------------------------------------------------------------
// How much more likely one kind of thing is to be rolled than another: monsters first, then weapons, then ammo and
// health, then powerups, then furniture.
//
// The weights are per type rather than per category, so that ordering holds however many types each category happens
// to contain. Without them a roll is dominated by scenery - more than half of everything placeable is a decoration and
// barely a sixth is a monster - so a randomized map came out quieter than the one it replaced rather than stranger.
//
// On Doom these work out at roughly half of all swaps landing on a monster and a fifth on furniture.
//------------------------------------------------------------------------------------------------------------------------------------------
static constexpr int32_t WEIGHT_MONSTER   = 24;
static constexpr int32_t WEIGHT_WEAPON    = 16;
static constexpr int32_t WEIGHT_SUPPLY    = 8;      // Ammo, health and armour: asked for as one band
static constexpr int32_t WEIGHT_POWERUP   = 5;
static constexpr int32_t WEIGHT_FURNITURE = 3;

static int32_t weightOfType(const mobjtype_t type) noexcept {
    const mobjinfo_t& info = gMobjInfo[type];

    // Whether something is a monster is the one part of this the engine already knows
    if (info.flags & MF_COUNTKILL)
        return WEIGHT_MONSTER;

    // The rest go by the number a map knows them by. Their type names are 'MT_MISC' numbers that say nothing about
    // what they are - the super shotgun is 'MT_SUPERSHOTGUN' but a medikit is 'MT_MISC11' - so this is the readable
    // way to ask, and it is the same number the map editor used.
    switch (info.doomednum) {
        // Shotgun, chaingun, rocket launcher, plasma rifle, chainsaw, BFG, super shotgun
        case 2001: case 2002: case 2003: case 2004: case 2005: case 2006: case 82:
            return WEIGHT_WEAPON;

        // Clip, shells, rocket, box of rockets, cell, box of ammo, box of shells, cell pack, backpack
        case 2007: case 2008: case 2010: case 2046: case 2047: case 2048: case 2049: case 17: case 8:
            return WEIGHT_SUPPLY;

        // Stimpack, medikit, health potion, armour helmet, green armour, blue armour
        case 2011: case 2012: case 2014: case 2015: case 2018: case 2019:
            return WEIGHT_SUPPLY;

        // Soulsphere, invulnerability, berserk, invisibility, radiation suit, computer map, light visor, megasphere
        case 2013: case 2022: case 2023: case 2024: case 2025: case 2026: case 2045: case 83:
            return WEIGHT_POWERUP;

        // Everything else is scenery
        default:
            return WEIGHT_FURNITURE;
    }
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Everything a thing could be turned into, worked out once per level.
//
// Built from what the running game can actually draw, so a game without the Master Edition's extra monsters simply
// never offers them - which is the behaviour asked for when that edition is not installed.
//------------------------------------------------------------------------------------------------------------------------------------------
static std::vector<mobjtype_t> gCandidates;
static std::vector<int32_t> gCandidateWeights;

static void buildCandidateList() noexcept {
    gCandidates.clear();
    gCandidateWeights.clear();

    for (int32_t i = 0; i < gNumMobjInfo; ++i) {
        const mobjtype_t type = (mobjtype_t) i;
        const mobjinfo_t& info = gMobjInfo[i];

        // Only things a map could have contained in the first place. Everything else is spawned by the game as it
        // runs - projectiles, puffs, the pieces of an exploding barrel - and has no business being placed.
        if (info.doomednum <= 0)
            continue;

        if (isProtectedType(type))
            continue;

        if (!isTypeDrawable(type)) {
            // Only worth reporting for things that were meant to be seen. PsyDoom's map markers have no artwork by
            // design, and listing all sixteen of them buries the one case this is for: a monster that should have been
            // borrowed and was not.
            if (type < MT_MARKER1) {
                tally(gNoArtwork, type, type);
            }

            continue;
        }

        gCandidates.push_back(type);
        gCandidateWeights.push_back(weightOfType(type));
    }
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Drawing a candidate for one thing.
//
// What is left to try for the thing being rolled, kept as a scratch list so that a candidate which does not fit can be
// dropped and the next draw made from what remains. Drawing without replacement rather than picking repeatedly at
// random means a cramped spot still settles on something rather than giving up after a run of bad luck, and it keeps
// the weighting honest as the list shrinks.
//------------------------------------------------------------------------------------------------------------------------------------------
static std::vector<mobjtype_t> gRemaining;
static std::vector<int32_t> gRemainingWeights;

static bool drawCandidate(mobjtype_t& out) noexcept {
    const int32_t numRemaining = (int32_t) gRemaining.size();

    if (numRemaining <= 0)
        return false;

    int32_t totalWeight = 0;

    for (const int32_t weight : gRemainingWeights) {
        totalWeight += weight;
    }

    // Walk the weights until the roll is used up. Summing every time is cheap next to the fit test that follows.
    int32_t roll = randomBelow(totalWeight);
    int32_t idx = numRemaining - 1;

    for (int32_t i = 0; i < numRemaining; ++i) {
        roll -= gRemainingWeights[i];

        if (roll < 0) {
            idx = i;
            break;
        }
    }

    out = gRemaining[idx];

    // Drop what was drawn. Order does not matter here, only what is left, so the last entry fills the gap.
    gRemaining[idx] = gRemaining.back();
    gRemainingWeights[idx] = gRemainingWeights.back();
    gRemaining.pop_back();
    gRemainingWeights.pop_back();
    return true;
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Would this candidate fit where that thing is standing?
//
// Asked of the map rather than of a size table. The engine already answers it: 'P_CheckPosition' takes walls and other
// things into account, and leaves behind the floor and ceiling it found so the headroom is known too.
//
// The test has to run with the candidate's own dimensions, so they are stood in and put back afterwards. Nothing else
// observes the thing in between - this is called during level setup, before anything is running.
//------------------------------------------------------------------------------------------------------------------------------------------
//------------------------------------------------------------------------------------------------------------------------------------------
// Would this thing block the way for good?
//
// Something that blocks and can also be destroyed is not a problem: a monster in a doorway can be killed and a barrel
// can be shot. A pillar cannot, so a pillar standing where a health bonus used to lie can wall off a corridor, a
// doorway or a lift for the rest of the run - which is the one way this mode can make a level impossible rather than
// merely strange.
//------------------------------------------------------------------------------------------------------------------------------------------
static bool isPermanentBlocker(const mobjinfo_t& info) noexcept {
    return (((info.flags & MF_SOLID) != 0) && ((info.flags & MF_SHOOTABLE) == 0));
}

static bool candidateFits(mobj_t& mobj, const mobjtype_t candidate) noexcept {
    const mobjinfo_t& info = gMobjInfo[candidate];

    // Nothing may become a permanent blocker that was not already one, nor may one grow.
    //
    // Fitting where a thing stands is not the same question as leaving room to get past it: the spot itself can be
    // free while the doorway it sits in is now shut. Rather than work out whether every spot is a way through, which
    // is the whole map's business, the mode simply never adds a blockage it did not inherit. It stays free the other
    // way round - a pillar may still become a monster or a pickup - so this costs variety in one direction only.
    if (isPermanentBlocker(info)) {
        const mobjinfo_t& origInfo = gMobjInfo[originalTypeOf(mobj)];

        if (!isPermanentBlocker(origInfo))
            return false;

        if (info.radius > origInfo.radius)
            return false;
    }

    // Something that does not collide fits anywhere its original did
    const bool bCandidateSolid = ((info.flags & MF_SOLID) != 0);

    const fixed_t savedRadius = mobj.radius;
    const fixed_t savedHeight = mobj.height;
    const uint32_t savedFlags = mobj.flags;

    mobj.radius = info.radius;
    mobj.height = info.height;
    mobj.flags = info.flags;

    const bool bPositionOk = P_CheckPosition(mobj, mobj.x, mobj.y);
    const fixed_t headroom = gTmCeilingZ - gTmFloorZ;
    const bool bHeadroomOk = (headroom >= info.height);

    mobj.radius = savedRadius;
    mobj.height = savedHeight;
    mobj.flags = savedFlags;

    if (!bPositionOk)
        return false;

    // Only a solid thing needs the headroom: something you walk through can share the space
    if (bCandidateSolid && (!bHeadroomOk))
        return false;

    return true;
}

//------------------------------------------------------------------------------------------------------------------------------------------
// The three traits a randomized monster may pick up, and how often.
//
// Mutually exclusive, because all three are the same field: the PSX blend mode, of which a thing has exactly one.
//
// Only 'nightmare' is the engine's own name for what it does - 'doomdef.h' says outright that a subtractive blend
// "also makes monsters 'nightmare' and have 2x hit points", and 'P_SpawnMapThing' is where that doubling normally
// happens, so it has to be done here too. The other two are a choice rather than something the engine names: the
// half transparency is the look PSX Doom gives a spectre, and the faint additive blend is the ghostly one. Swapping
// which is which is a one line change.
//
// Monsters only. Nightmare means nothing to a health bonus, and a see-through pillar reads as a fault rather than as
// a variation.
//------------------------------------------------------------------------------------------------------------------------------------------
static constexpr int32_t TRAIT_CHANCE_ONE_IN = 12;

static void maybeGiveTrait(mobj_t& mobj) noexcept {
    if ((mobj.flags & MF_COUNTKILL) == 0)
        return;

    if (randomBelow(TRAIT_CHANCE_ONE_IN) != 0)
        return;

    // Whatever the map may have asked for is replaced rather than added to: there is only one blend mode to have
    mobj.flags &= ~MF_ALL_BLEND_FLAGS;

    switch (randomBelow(3)) {
        case 0:
            mobj.flags |= MF_BLEND_SUBTRACT;    // Nightmare
            mobj.health *= 2;                   // As 'P_SpawnMapThing' does for a nightmare thing placed by a map
            gNumNightmare++;
            break;

        case 1:
            mobj.flags |= MF_BLEND_ALPHA_50;    // Spectre
            gNumSpectre++;
            break;

        default:
            mobj.flags |= MF_BLEND_ADD_25;      // Transparent
            gNumTransparent++;
            break;
    }
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Turn a thing into something else
//------------------------------------------------------------------------------------------------------------------------------------------
static void morphThing(mobj_t& mobj, const mobjtype_t newType) noexcept {
    mobjinfo_t& info = gMobjInfo[newType];      // Not const: a thing holds a mutable pointer to its own info

    // Note 'randomizerOriginalType' is deliberately not touched: it was set to what this was when it spawned, and it
    // has to keep saying that however many times this runs.

    // The blockmap and sector lists are keyed on where a thing is and how big it is, so it comes out before its size
    // changes and goes back in afterwards
    P_UnsetThingPosition(mobj);

    // Whether the thing was told to be deaf belongs to the spot rather than to what stands on it, so it survives
    const uint32_t keptFlags = (mobj.flags & MF_AMBUSH);

    mobj.type = newType;
    mobj.info = &info;
    mobj.radius = info.radius;
    mobj.height = info.height;
    mobj.health = info.spawnhealth;
    mobj.flags = info.flags | keptFlags;

    P_SetThingPosition(mobj);

    // Whether a thing hangs from the ceiling or stands on the floor belongs to what it is rather than to where it is,
    // so it is decided again here. Without this a hanging corpse that became a barrel would float where the corpse hung.
    if (info.flags & MF_SPAWNCEILING) {
        mobj.z = mobj.ceilingz - info.height;
    } else {
        mobj.z = mobj.floorz;
    }

    // The state is set by hand rather than through 'P_SetMobjState', for the same reason 'P_SpawnMobj' does it by hand
    // and says so: that function runs the state's action, and the action for a monster's spawn state is 'A_Look'.
    //
    // Running it here is too early. The roll happens before 'P_SpawnPlayer', so 'A_Look' would target 'gEmptyMobj' -
    // the placeholder both players hold until they spawn - and that is a thing with no position at all. A monster
    // holding it would then crash the sight check on dereferencing a null subsector. It only happened sometimes,
    // because the placeholder has no health and the next 'A_Look' replaces it; it had to be caught in between.
    state_t& state = gStates[info.spawnstate];
    mobj.state = &state;
    mobj.tics = state.tics;
    mobj.sprite = state.sprite;
    mobj.frame = state.frame;

    maybeGiveTrait(mobj);

    // Stagger the animation as spawning does, so a room full of the same thing does not move as one
    if (mobj.tics > 0) {
        mobj.tics = 1 + randomBelow(mobj.tics);
    }
}

//------------------------------------------------------------------------------------------------------------------------------------------
// A different sky.
//
// The game's skies are textures named 'SKYnn', so the choice is made from whatever the running game turns out to have
// rather than from a list written here - which is what lets the Master Edition's extra skies be picked without this
// knowing anything about them.
//
// Nothing needs to be done about the fire sky: what makes a sky a fire sky is a texture name ending in '9', and the
// setup that looks for that runs after this and reads the name of whatever texture it finds. Choosing one is enough.
//------------------------------------------------------------------------------------------------------------------------------------------
static char gChosenSkyName[16] = {};
static int32_t gNumSkiesAvailable = 0;
static int32_t gChosenSkyTexIdx = -1;

void randomizeSky() noexcept {
    gChosenSkyName[0] = '\0';
    gNumSkiesAvailable = 0;

    if (!gbEnabled) {
        gChosenSkyTexIdx = -1;
        return;
    }

    // A level being restarted keeps the sky it already had.
    //
    // Choosing the texture is only half of what makes a sky work: the palette, and whether the fire is running, are
    // set up in 'P_Init' - and 'P_SetupLevel' skips that entirely when a level is restarted, because a map's own sky
    // cannot change. This runs from 'P_LoadSectors', which is not skipped, so rolling again here would leave that
    // setup describing the previous sky. Rolling the fire sky and then dying did exactly that: the fire updater was
    // left running against a texture that is not the fire sky, and it crashed.
    //
    // So a restart puts back the same sky, which is the one the setup still describes.
    if (gbIsLevelBeingRestarted) {
        if ((gChosenSkyTexIdx >= 0) && (gChosenSkyTexIdx < gNumTexLumps)) {
            gpSkyTexture = &gpTextures[gChosenSkyTexIdx];
        }

        return;
    }

    // A map with no sky at all keeps not having one: it is indoors, and giving it a sky would show nothing
    if (!gpSkyTexture) {
        std::snprintf(gChosenSkyName, sizeof(gChosenSkyName), "none (indoors)");
        return;
    }

    // Asked for by name, one at a time, rather than by reading the texture list directly.
    //
    // 'R_TextureNumForName' masks the name before comparing it, because the first character of a lump name carries a
    // flag saying the lump is compressed. Comparing the characters here instead found no skies at all - every one of
    // them is compressed, so not one of them appeared to start with an 'S'.
    std::vector<int32_t> skies;

    for (int32_t skyNum = 1; skyNum <= 99; ++skyNum) {
        char skyName[16] = {};
        std::snprintf(skyName, sizeof(skyName), "SKY%02d", (int) skyNum);

        const int32_t texIdx = R_TextureNumForName(skyName, false);

        if (texIdx >= 0) {
            skies.push_back(texIdx);
        }
    }

    gNumSkiesAvailable = (int32_t) skies.size();

    if (skies.empty())
        return;

    const int32_t chosen = skies[randomBelow((int32_t) skies.size())];
    gpSkyTexture = &gpTextures[chosen];
    gChosenSkyTexIdx = chosen;

    const WadLumpName name = W_GetLumpName(gpSkyTexture->lumpNum);

    // The top bit of the first character says the lump is compressed and is not part of the name, so it is masked off
    // here as well - otherwise the log prints the sky as garbage
    for (int32_t i = 0; i < 8; ++i) {
        gChosenSkyName[i] = (char)((uint8_t) name.chars[i] & 0x7Fu);
    }

    gChosenSkyName[8] = '\0';
}

//------------------------------------------------------------------------------------------------------------------------------------------
// A different music track, chosen from the ones the running game has
//------------------------------------------------------------------------------------------------------------------------------------------
static int32_t gChosenMusicTrack = -1;

int32_t chooseMusicTrack(const int32_t mapMusicTrack) noexcept {
    gChosenMusicTrack = -1;

    if (!gbEnabled)
        return mapMusicTrack;

    // A map with no music keeps having none, rather than gaining some it was never meant to have
    if (mapMusicTrack <= 0)
        return mapMusicTrack;

    // Chosen from the tracks the maps of this game actually play.
    //
    // Not from the full list of defined tracks: that includes numbers this game has no 'MUSLEV' file for, and asking
    // for one is a fatal error rather than silence. What a map already plays is known to exist, because it plays it.
    std::vector<int32_t> usable;

    const int32_t numMaps = Game::getNumMaps();

    for (int32_t i = 1; i <= numMaps; ++i) {
        const MapInfo::Map* const pOtherMap = MapInfo::getMap(i);

        if ((!pOtherMap) || (pOtherMap->music <= 0))
            continue;

        // Leave out the ones that play a CD track rather than a sequence: those are not interchangeable with these
        if (pOtherMap->bPlayCdMusic)
            continue;

        bool bAlreadyHave = false;

        for (const int32_t track : usable) {
            if (track == pOtherMap->music) {
                bAlreadyHave = true;
                break;
            }
        }

        if (!bAlreadyHave) {
            usable.push_back(pOtherMap->music);
        }
    }

    if (usable.empty())
        return mapMusicTrack;

    gChosenMusicTrack = usable[randomBelow((int32_t) usable.size())];
    return gChosenMusicTrack;
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Writing out what the roll did.
//
// Long lists are broken over several lines rather than written as one enormous one, because a log is read by eye.
//------------------------------------------------------------------------------------------------------------------------------------------
#if defined(__XBOX__)

static const char* gameDisplayName() noexcept {
    switch (Game::gGameType) {
        case GameType::Doom:                        return "DOOM";
        case GameType::FinalDoom:                   return "FINAL DOOM";
        case GameType::GEC_ME_Beta3:                return "MASTER EDITION";
        case GameType::GEC_ME_Beta4:                return "MASTER EDITION";
        case GameType::GEC_ME_TestMap_Doom:         return "MASTER EDITION TEST";
        case GameType::GEC_ME_TestMap_FinalDoom:    return "MASTER EDITION TEST";
        default:                                    return "DOOM";
    }
}

static void logTally(
    const char* const heading,
    const std::vector<TallyEntry>& list,
    const bool bShowArrow,
    const bool bSkippedNames
) noexcept {
    if (list.empty())
        return;

    std::string line;
    int32_t numOnLine = 0;

    const auto flush = [&]() noexcept {
        if (!line.empty()) {
            XBOX_LOGI(General, "Randomizer - %s: %s", heading, line.c_str());
            line.clear();
            numOnLine = 0;
        }
    };

    for (const TallyEntry& entry : list) {
        const char* const fromName = (bSkippedNames) ? nameOfSkipped(entry.from) : nameOfType(entry.from);

        char text[160] = {};

        if (bShowArrow) {
            std::snprintf(text, sizeof(text), "%s->%s", fromName, nameOfType(entry.to));
        } else {
            std::snprintf(text, sizeof(text), "%s", fromName);
        }

        std::string piece = text;

        if (entry.count > 1) {
            char countText[16] = {};
            std::snprintf(countText, sizeof(countText), " x%d", (int) entry.count);
            piece += countText;
        }

        if (!line.empty()) {
            line += ", ";
        }

        line += piece;
        numOnLine++;

        // Six to a line keeps them readable without wrapping
        if (numOnLine >= 6) {
            flush();
        }
    }

    flush();
}

#endif  // defined(__XBOX__)

//------------------------------------------------------------------------------------------------------------------------------------------
// Roll the level
//------------------------------------------------------------------------------------------------------------------------------------------
void randomizeLevel() noexcept {
    if (!gbEnabled) {
        gSkipped.clear();
        return;
    }

    gReplaced.clear();
    gUntouched.clear();
    gNothingFit.clear();
    gNoArtwork.clear();

    gNumNightmare = 0;
    gNumSpectre = 0;
    gNumTransparent = 0;

    seedRng();
    buildCandidateList();

    if (gCandidates.empty()) {
        #if defined(__XBOX__)
            XBOX_LOGI(General, "Randomizer - FAILED: nothing at all is available to place, the level is unchanged");
        #endif

        gSkipped.clear();
        return;
    }

    int32_t numRolled = 0;
    int32_t numChanged = 0;

    for (mobj_t* pMobj = gMobjHead.next; pMobj != &gMobjHead; pMobj = pMobj->next) {
        mobj_t& mobj = *pMobj;

        // Players are not things that were placed, whatever else is true of them
        if (mobj.player)
            continue;

        if (isProtectedType(mobj.type)) {
            tally(gUntouched, mobj.type, mobj.type);
            continue;
        }

        // Only things a map placed. Anything already spawned by the game as it started - and there is little of that
        // this early - is left alone.
        if (gMobjInfo[mobj.type].doomednum <= 0)
            continue;

        // If the way on through this map is waiting for the last of these to die, then whatever stands here has to be
        // something that can die. Restricting the candidates is all that is needed to keep such a map completable:
        // it leaves no case where the trigger cannot be reached.
        const bool bMustBeKillable = isTriggerThing(mobj);

        // Draw candidates, weighted, until one fits
        gRemaining = gCandidates;
        gRemainingWeights = gCandidateWeights;

        numRolled++;

        mobjtype_t candidate = MT_PLAYER;
        bool bSettled = false;

        while (drawCandidate(candidate)) {
            if (candidate == mobj.type) {
                // It rolled itself: leave it exactly as it was
                tally(gUntouched, mobj.type, mobj.type);
                bSettled = true;
                break;
            }

            if (bMustBeKillable && ((gMobjInfo[candidate].flags & MF_COUNTKILL) == 0))
                continue;

            if (!candidateFits(mobj, candidate))
                continue;

            const mobjtype_t wasType = mobj.type;
            morphThing(mobj, candidate);
            tally(gReplaced, wasType, candidate);
            numChanged++;
            bSettled = true;
            break;
        }

        // Nothing in the whole roster would go here. Worth saying out loud: it means a spot so cramped that not even
        // the smallest thing fits, which is likelier to be a fault in the fit test than a real map.
        if (!bSettled) {
            tally(gNothingFit, mobj.type, mobj.type);
        }
    }

    // The counts on the intermission screen have to describe what is actually in the level now
    gTotalKills = 0;
    gTotalItems = 0;

    for (mobj_t* pMobj = gMobjHead.next; pMobj != &gMobjHead; pMobj = pMobj->next) {
        if (pMobj->flags & MF_COUNTKILL) {
            gTotalKills++;
        }

        if (pMobj->flags & MF_COUNTITEM) {
            gTotalItems++;
        }
    }

    // What the roll came out as. Worth having: a roll that changed nothing and a roll that changed everything look
    // identical from the title screen, and only this says which happened.
    #if defined(__XBOX__)
    {
        int32_t numUntouched = 0;

        for (const TallyEntry& entry : gUntouched)  { numUntouched += entry.count; }
        for (const TallyEntry& entry : gSkipped)    { numUntouched += entry.count; }
        for (const TallyEntry& entry : gNothingFit) { numUntouched += entry.count; }

        XBOX_LOGI(General, 
            "%s - MAP%02d: %s - Randomizer: Replaced %d entities, left %d untouched.",
            gameDisplayName(),
            (int) gGameMap,
            Game::getMapName(gGameMap).c_str().data(),
            (int) numChanged,
            (int) numUntouched
        );

        logTally("Replacements", gReplaced, true, false);
        logTally("Untouched", gUntouched, false, false);
        logTally("Never spawned", gSkipped, false, true);
        logTally("WARNING nothing would fit", gNothingFit, false, false);

        // What the roster came out as, and what was left out of it. This is how a missing borrowed monster shows up:
        // an edition that cannot draw the Arch-Vile simply never offers it, and this says so rather than staying quiet.
        XBOX_LOGI(General, 
            "Randomizer - Roster: %d of %d types available, %d rolled, %d kills, %d items",
            (int) gCandidates.size(),
            (int) gNumMobjInfo,
            (int) numRolled,
            (int) gTotalKills,
            (int) gTotalItems
        );

        if (gNumNightmare + gNumSpectre + gNumTransparent > 0) {
            XBOX_LOGI(General, 
                "Randomizer - Traits: %d nightmare, %d spectre, %d transparent",
                (int) gNumNightmare,
                (int) gNumSpectre,
                (int) gNumTransparent
            );
        }

        XBOX_LOGI(General, 
            "Randomizer - Sky: %s (%d to choose from), music track: %d",
            (gChosenSkyName[0]) ? gChosenSkyName : "unchanged",
            (int) gNumSkiesAvailable,
            (int) gChosenMusicTrack
        );

        logTally("Unavailable, no artwork in this game", gNoArtwork, false, false);

        // The three the Master Edition lends, called out by name: they are the whole point of the borrowing, and
        // "there is no Arch-Vile" is far easier to act on than a roster count that is three short.
        // Sprites and sounds are cached separately and can disagree - a console that got one build of the launcher
        // cache but not the other would draw the three and hear nothing, which from in front of the television looks
        // like the sound work simply not having taken. Reported together so the two cannot be confused.
        XBOX_LOGI(General,
            "Randomizer - Borrowed monsters: Arch-Vile %s, Wolfenstein SS %s, Commander Keen %s; sprites %s, sounds %s",
            isTypeDrawable(MT_VILE) ? "yes" : "NO",
            isTypeDrawable(MT_WOLFSS) ? "yes" : "NO",
            isTypeDrawable(MT_KEEN) ? "yes" : "NO",
            #if defined(__XBOX__)
                MasterMonsters::isLoaded() ? "cached" : "ABSENT",
                MasterMonsters::haveBorrowedSounds() ? "cached" : "ABSENT"
            #else
                "n/a", "n/a"
            #endif
        );

        if (gMapBossSpecialFlags != 0) {
            XBOX_LOGI(General, 
                "Randomizer - This map waits on boss deaths (flags 0x%02x), so those things were kept killable",
                (unsigned) gMapBossSpecialFlags
            );
        }
    }
    #endif

    gSkipped.clear();
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Something to fight with.
//
// One weapon, drawn evenly from the seven that are not the fist or the pistol, and a large pack of whatever it eats.
// The chainsaw eats nothing, so it comes on its own.
//
// The size of the pack is the same five clips a box pickup gives, rather than a number chosen here, so it is a box of
// bullets or shells or rockets or a cell pack exactly as the game means them. Handing over the weapon itself already
// grants the two clips a weapon pickup carries, so the player ends up with that plus the pack.
//------------------------------------------------------------------------------------------------------------------------------------------
struct StartingWeapon {
    weapontype_t    weapon;
    ammotype_t      ammo;
    bool            bHasAmmo;       // The chainsaw does not
    const char*     name;
};

static constexpr StartingWeapon STARTING_WEAPONS[] = {
    { wp_chainsaw,      am_clip,    false,  "Chainsaw"          },
    { wp_shotgun,       am_shell,   true,   "Shotgun"           },
    { wp_supershotgun,  am_shell,   true,   "Super Shotgun"     },
    { wp_chaingun,      am_clip,    true,   "Chaingun"          },
    { wp_missile,       am_misl,    true,   "Rocket Launcher"   },
    { wp_plasma,        am_cell,    true,   "Plasma Gun"        },
    { wp_bfg,           am_cell,    true,   "BFG9000"           },
};

// Five clips is what a box pickup is worth: fifty bullets, twenty shells, five rockets or a hundred cells
static constexpr int32_t LARGE_AMMO_PACK_CLIPS = 5;

static const char* gGrantedWeaponName = nullptr;

void grantStartingWeapon() noexcept {
    gGrantedWeaponName = nullptr;

    if (!gbEnabled)
        return;

    player_t& player = gPlayers[0];

    // Nothing to give a player who is not there. This runs during level setup, so the thing should exist by now, but
    // the check costs nothing and the alternative is a crash.
    if (!player.mo)
        return;

    const int32_t numWeapons = (int32_t) C_ARRAY_SIZE(STARTING_WEAPONS);
    const StartingWeapon& choice = STARTING_WEAPONS[randomBelow(numWeapons)];

    P_GiveWeapon(player, choice.weapon, false);

    if (choice.bHasAmmo) {
        P_GiveAmmo(player, choice.ammo, LARGE_AMMO_PACK_CLIPS);
    }

    gGrantedWeaponName = choice.name;

    // Said here rather than with the rest of the roll's report, because this happens after that is written
    #if defined(__XBOX__)
        XBOX_LOGI(General, 
            "Randomizer - Granted: %s%s",
            choice.name,
            (choice.bHasAmmo) ? " and a large ammo pack" : " (no ammo: it needs none)"
        );
    #endif
}

//------------------------------------------------------------------------------------------------------------------------------------------
// What the level cost, and what there was to spend.
//
// The one thing in this mode that can break a level rather than vary it: a roll that puts several large monsters into
// a map that never had any asks for more sprite memory than the map was built for. This does not yet refuse anything -
// it measures, so that a limit can be set from what actually happens rather than from a guess.
//------------------------------------------------------------------------------------------------------------------------------------------
void reportSpriteBudget() noexcept {
    if (!gbEnabled)
        return;

    #if defined(__XBOX__)
    {
        // What this level's sprites weigh, and what the whole game's would - so a roll can be seen creeping towards
        // the ceiling rather than only discovered on the level that goes over it.
        const int64_t neededBytes = MobjSpritePrecacher::getLastPrecachedBytes();
        const int32_t neededLumps = MobjSpritePrecacher::getLastPrecachedLumps();
        const int32_t zoneFree = Z_FreeMemory(*gpMainMemZone);

        XBOX_LOGI(General, 
            "Randomizer - Budget: sprites %d KiB in %d lumps, zone free %d KiB after caching them",
            (int) (neededBytes / 1024),
            (int) neededLumps,
            (int) (zoneFree / 1024)
        );
    }
    #endif
}

END_NAMESPACE(Randomizer)
