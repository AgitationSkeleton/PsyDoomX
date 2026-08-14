#include "SsgStyle.h"

#if defined(__XBOX__)

#include "Game.h"
#include "Controls.h"
#include "PlayerPrefs.h"
#include "WadList.h"

#include "Doom/Game/sprinfo.h"

#include <windows.h>

#include <cstdio>
#include <cstring>

extern void xbLog(const char* msg) noexcept;

BEGIN_NAMESPACE(SsgStyle)

// The sprite the game's own super shotgun frames live under, in its own main WAD
static constexpr const char* NATIVE_SPRITE_NAME = "SHT2";

// What each style is called, where it lives, and what it is written out as
static constexpr const char* kSpriteNames[STYLE_COUNT] = { "SSGD", "SSGF", "SSGM" };
static constexpr const char* kDisplayNames[STYLE_COUNT] = { "Doom", "Final", "Master" };

static constexpr const char* kWadPaths[STYLE_COUNT] = {
    "E:\\Apps\\PsyDoomX\\cache\\ssgd.wad",
    "E:\\Apps\\PsyDoomX\\cache\\ssgf.wad",
    "E:\\Apps\\PsyDoomX\\cache\\ssgm.wad"
};

// The game's own frames, saved before anything is pointed anywhere else, so the native style is always reachable
static spriteframe_t gNativeFrames[NUM_FRAMES];
static bool gbHaveNativeFrames = false;

// Which borrowed sets actually loaded. A file that was written but would not open leaves this false, and the setting
// then will not offer that style - which is better than offering one that silently shows the wrong gun.
static bool gbStyleLoaded[STYLE_COUNT] = {};

const char* spriteName(const Style style) noexcept {
    return ((style >= 0) && (style < STYLE_COUNT)) ? kSpriteNames[style] : "";
}

const char* wadPath(const Style style) noexcept {
    return ((style >= 0) && (style < STYLE_COUNT)) ? kWadPaths[style] : "";
}

const char* displayName(const Style style) noexcept {
    return ((style >= 0) && (style < STYLE_COUNT)) ? kDisplayNames[style] : "?";
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Which edition the running game came with
//------------------------------------------------------------------------------------------------------------------------------------------
Style nativeStyle() noexcept {
    switch (Game::gGameType) {
        case GameType::FinalDoom:
            return STYLE_FINAL;

        case GameType::GEC_ME_Beta3:
        case GameType::GEC_ME_Beta4:
        case GameType::GEC_ME_TestMap_Doom:
        case GameType::GEC_ME_TestMap_FinalDoom:
            return STYLE_MASTER;

        default:
            return STYLE_DOOM;
    }
}

bool isAvailable(const Style style) noexcept {
    if ((style < 0) || (style >= STYLE_COUNT))
        return false;

    return ((style == nativeStyle()) || gbStyleLoaded[style]);
}

bool haveChoice() noexcept {
    int32_t numAvailable = 0;

    for (int32_t i = 0; i < STYLE_COUNT; ++i) {
        if (isAvailable((Style) i)) {
            numAvailable++;
        }
    }

    return (numAvailable > 1);
}

Style nextAvailable(const Style style, const int32_t dir) noexcept {
    Style next = style;

    for (int32_t tries = 0; tries < STYLE_COUNT; ++tries) {
        int32_t candidate = (int32_t) next + ((dir >= 0) ? 1 : -1);

        if (candidate >= STYLE_COUNT) { candidate = 0; }
        if (candidate < 0) { candidate = STYLE_COUNT - 1; }

        next = (Style) candidate;

        if (isAvailable(next))
            return next;
    }

    return style;
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Add the borrowed sets to the main WAD list
//------------------------------------------------------------------------------------------------------------------------------------------
void addOverrideWads(WadList& wadList) noexcept {
    const Style native = nativeStyle();

    for (int32_t i = 0; i < STYLE_COUNT; ++i) {
        gbStyleLoaded[i] = false;

        // The game's own set is already in its own main WAD; borrowing it from a file would be a duplicate
        if ((Style) i == native)
            continue;

        const char* const pPath = kWadPaths[i];

        if (GetFileAttributesA(pPath) == INVALID_FILE_ATTRIBUTES)
            continue;

        // Note: 'WadFile::open' raises a fatal error rather than returning a failure, so the file has to be known to be
        // there before asking. That is what the check above is for; it is not merely an optimisation.
        wadList.add(pPath);
        gbStyleLoaded[i] = true;

        char msg[128];
        std::snprintf(msg, sizeof(msg), "ssg style: loaded '%s'", pPath);
        xbLog(msg);
    }
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Find a sprite by its four character name, or null if it is not there
//------------------------------------------------------------------------------------------------------------------------------------------
static const spritedef_t* findSprite(const char* const name) noexcept {
    const sprname_t sprName(name[0], name[1], name[2], name[3]);
    const int32_t spriteIdx = P_SpriteCheckNumForName(sprName);

    if ((spriteIdx < 0) || (spriteIdx >= gNumSprites))
        return nullptr;

    const spritedef_t& def = gSprites[spriteIdx];
    return (def.numframes >= NUM_FRAMES) ? &def : nullptr;
}

void init() noexcept {
    gbHaveNativeFrames = false;

    const spritedef_t* const pNative = findSprite(NATIVE_SPRITE_NAME);

    if (!pNative) {
        xbLog("ssg style: this game has no super shotgun sprite - the setting will do nothing");
        return;
    }

    // Saved whole rather than just the lump numbers. 'flip' and 'rotate' belong to the frame as much as the lump does,
    // and a set copied without them would be right in every frame but drawn the wrong way round in some.
    for (int32_t i = 0; i < NUM_FRAMES; ++i) {
        gNativeFrames[i] = pNative->spriteframes[i];
    }

    gbHaveNativeFrames = true;

    // Any style that did load but whose sprite did not come through is not usable
    for (int32_t i = 0; i < STYLE_COUNT; ++i) {
        if (gbStyleLoaded[i] && (!findSprite(kSpriteNames[i]))) {
            gbStyleLoaded[i] = false;
            xbLog("ssg style: a borrowed set loaded but its sprite is missing - dropping it");
        }
    }

    applyForPlayer(0);
}

void applyForPlayer(const int32_t playerIdx) noexcept {
    if (!gbHaveNativeFrames)
        return;

    const spritedef_t* const pNative = findSprite(NATIVE_SPRITE_NAME);

    if (!pNative)
        return;

    // Fall back to the game's own set if the setting names one that is not here.
    //
    // This is reachable in normal use rather than only through a corrupt file: the setting is shared by all three games
    // because they share one preferences file, so a player who chose 'Master' while playing the Master Edition and then
    // starts Doom asks for a style that Doom can only have if the launcher wrote it out.
    const int32_t idx = ((playerIdx >= 0) && (playerIdx < Controls::MAX_LOCAL_PLAYERS)) ? playerIdx : 0;
    Style style = (Style) PlayerPrefs::gSsgStyle[idx];

    if (!isAvailable(style)) {
        style = nativeStyle();
    }

    const spriteframe_t* pSrc = gNativeFrames;

    if (style != nativeStyle()) {
        const spritedef_t* const pBorrowed = findSprite(kSpriteNames[style]);

        if (!pBorrowed)
            return;

        pSrc = pBorrowed->spriteframes;
    }

    for (int32_t i = 0; i < NUM_FRAMES; ++i) {
        pNative->spriteframes[i] = pSrc[i];
    }
}

END_NAMESPACE(SsgStyle)

#endif  // #if defined(__XBOX__)
