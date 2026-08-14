#include "PlayerColour.h"

#if defined(__XBOX__)

#include "Game.h"
#include "PlayerPrefs.h"
#include "WadList.h"

#include "Doom/doomdef.h"
#include "Doom/Game/g_game.h"
#include "Doom/Game/sprinfo.h"

#include <windows.h>

#include <cstdio>
#include <cstring>

extern void xbLog(const char* msg) noexcept;

BEGIN_NAMESPACE(PlayerColour)

// Everything that defines a colour, in one place. Adding another means adding a row here and nothing else.
struct ColourDef {
    int32_t     rampStart;      // Where this colour's sixteen palette entries begin
    const char* displayName;    // What the menu calls it
    const char* spriteName;     // Four characters, the sprite its recoloured frames live under
};

static constexpr ColourDef kColours[COLOUR_COUNT] = {
    { GREEN_RAMP_START, "Green",  "PLAY" },     // The sprites as they already are
    { 0x60,             "Indigo", "PLYI" },     // The grey ramp; the PC manual calls this indigo
    { 0x40,             "Brown",  "PLYB" },
    { 0x20,             "Red",    "PLYR" }
};

// Where the launcher leaves the recoloured sprites, one file per edition.
//
// Per edition because the Master Edition's marine is drawn differently: of the fifty one 'PLAY' lumps, only fourteen
// match Doom's. Doom and Final Doom are byte for byte identical in all fifty one, but keeping a file each costs little
// and means nothing has to know that.
static constexpr const char* kWadPaths[] = {
    "E:\\Apps\\PsyDoomX\\cache\\plrd.wad",
    "E:\\Apps\\PsyDoomX\\cache\\plrf.wad",
    "E:\\Apps\\PsyDoomX\\cache\\plrm.wad"
};

static constexpr int32_t NUM_EDITIONS = (int32_t) (sizeof(kWadPaths) / sizeof(kWadPaths[0]));

// The recoloured sprites, once they have been found among the loaded ones
static const spritedef_t* gpColourSprites[COLOUR_COUNT] = {};

// Whether the recoloured sprite WAD was added to the list at all
static bool gbLoadedWad = false;

int32_t rampStart(const Colour colour) noexcept {
    return ((colour >= 0) && (colour < COLOUR_COUNT)) ? kColours[colour].rampStart : GREEN_RAMP_START;
}

const char* displayName(const Colour colour) noexcept {
    return ((colour >= 0) && (colour < COLOUR_COUNT)) ? kColours[colour].displayName : "Green";
}

const char* spriteName(const Colour colour) noexcept {
    return ((colour >= 0) && (colour < COLOUR_COUNT)) ? kColours[colour].spriteName : "PLAY";
}

const char* wadPathForEdition(const int32_t editionIdx) noexcept {
    return ((editionIdx >= 0) && (editionIdx < NUM_EDITIONS)) ? kWadPaths[editionIdx] : "";
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Which edition's file the running game wants
//------------------------------------------------------------------------------------------------------------------------------------------
static int32_t runningEditionIdx() noexcept {
    switch (Game::gGameType) {
        case GameType::FinalDoom:
            return 1;

        case GameType::GEC_ME_Beta3:
        case GameType::GEC_ME_Beta4:
        case GameType::GEC_ME_TestMap_Doom:
        case GameType::GEC_ME_TestMap_FinalDoom:
            return 2;

        default:
            return 0;
    }
}

void addOverrideWad(WadList& wadList) noexcept {
    gbLoadedWad = false;

    const char* const pPath = wadPathForEdition(runningEditionIdx());

    // Checked rather than attempted: 'WadFile::open' raises a fatal error instead of returning a failure
    if ((!pPath[0]) || (GetFileAttributesA(pPath) == INVALID_FILE_ATTRIBUTES))
        return;

    wadList.add(pPath);
    gbLoadedWad = true;

    char msg[128];
    std::snprintf(msg, sizeof(msg), "player colour: loaded '%s'", pPath);
    xbLog(msg);
}

void init() noexcept {
    for (int32_t i = 0; i < COLOUR_COUNT; ++i) {
        gpColourSprites[i] = nullptr;
    }

    if (!gbLoadedWad)
        return;

    for (int32_t i = 0; i < COLOUR_COUNT; ++i) {
        // Green needs no recoloured sprite: it is what the game already draws
        if ((Colour) i == GREEN)
            continue;

        const char* const pName = kColours[i].spriteName;
        const sprname_t sprName(pName[0], pName[1], pName[2], pName[3]);
        const int32_t spriteIdx = P_SpriteCheckNumForName(sprName);

        if ((spriteIdx >= 0) && (spriteIdx < gNumSprites)) {
            gpColourSprites[i] = &gSprites[spriteIdx];
        }
    }
}

bool isAvailable(const Colour colour) noexcept {
    if ((colour < 0) || (colour >= COLOUR_COUNT))
        return false;

    return ((colour == GREEN) || (gpColourSprites[colour] != nullptr));
}

bool haveChoice() noexcept {
    int32_t numAvailable = 0;

    for (int32_t i = 0; i < COLOUR_COUNT; ++i) {
        if (isAvailable((Colour) i)) {
            numAvailable++;
        }
    }

    return (numAvailable > 1);
}

Colour nextAvailable(const Colour colour, const int32_t dir) noexcept {
    Colour next = colour;

    for (int32_t tries = 0; tries < COLOUR_COUNT; ++tries) {
        int32_t candidate = (int32_t) next + ((dir >= 0) ? 1 : -1);

        if (candidate >= COLOUR_COUNT) { candidate = 0; }
        if (candidate < 0) { candidate = COLOUR_COUNT - 1; }

        next = (Colour) candidate;

        if (isAvailable(next))
            return next;
    }

    return colour;
}

Colour forPlayer(const int32_t playerIdx) noexcept {
    if ((playerIdx < 0) || (playerIdx >= MAXPLAYERS))
        return GREEN;

    const Colour colour = (Colour) PlayerPrefs::gPlayerColour[playerIdx];
    return (isAvailable(colour)) ? colour : GREEN;
}

const spritedef_t* spriteDefForThing(const mobj_t& thing) noexcept {
    // Only a player's own object has this, which is what keeps the level's own dead marines and gibs out of it
    if (!thing.player)
        return nullptr;

    const int32_t playerIdx = (int32_t)(thing.player - gPlayers);

    if ((playerIdx < 0) || (playerIdx >= MAXPLAYERS))
        return nullptr;

    return gpColourSprites[forPlayer(playerIdx)];    // Null for green, which means "draw it as it is"
}

void markCorpse(mobj_t& corpse, const int32_t playerIdx) noexcept {
    const spritedef_t* const pSprite = gpColourSprites[forPlayer(playerIdx)];

    if (!pSprite)
        return;

    // The sprite index is what the renderer looks the frames up by, so writing it here is enough. Safe to write: the
    // body is in its final state and will not be given another, so nothing sets this from a state again.
    corpse.sprite = (spritenum_t)(pSprite - gSprites);
}

void statusBarRgb(const Colour colour, uint8_t& r, uint8_t& g, uint8_t& b) noexcept {
    // The middle of each ramp, read out of the game's own palette rather than picked by eye.
    //
    // These are entry 0x7 of each sixteen colour ramp in 'PLAYPAL': the green the marine is actually drawn in, and what
    // the three translations turn it into. Held as numbers because the palettes are handed to the GPU and freed during
    // startup, so there is nothing left in memory to read them from by the time the status bar is drawn.
    struct Rgb { uint8_t r, g, b; };

    static constexpr Rgb kRgb[COLOUR_COUNT] = {
        {  66, 140,  49 },      // 0x77 - green
        {  82,  82,  82 },      // 0x67 - grey/indigo
        { 123,  82,  41 },      // 0x47 - brown
        { 107,  16,  16 }       // 0x27 - red
    };

    const Rgb& rgb = kRgb[((colour >= 0) && (colour < COLOUR_COUNT)) ? colour : GREEN];
    r = rgb.r;
    g = rgb.g;
    b = rgb.b;
}

END_NAMESPACE(PlayerColour)

#endif  // #if defined(__XBOX__)
