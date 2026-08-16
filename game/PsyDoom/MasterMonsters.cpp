#include "MasterMonsters.h"

#if defined(__XBOX__)

#include "WadList.h"

#include <windows.h>

#include <cstdio>

extern void xbLog(const char* msg) noexcept;

BEGIN_NAMESPACE(MasterMonsters)

// The four sprites a Doom or Final Doom disc has nothing under.
//
// 'FIRE' is the Arch-Vile's attack rather than a monster of its own, and is needed for the same reason: without it the
// Arch-Vile can be drawn but what it does to you cannot.
const char* const SPRITE_NAMES[] = { "VILE", "FIRE", "SSWV", "KEEN" };
const int NUM_SPRITE_NAMES = (int)(sizeof(SPRITE_NAMES) / sizeof(SPRITE_NAMES[0]));

static constexpr const char* WAD_PATH   = "E:\\Apps\\PsyDoomX\\cache\\memonsters.wad";
static constexpr const char* SOUND_DIR  = "E:\\Apps\\PsyDoomX\\cache\\sound";
static constexpr const char* WMD_NAME   = "DOOMSND.WMD";
static constexpr const char* LCD_NAME   = "MEMONST.LCD";

static bool gbLoaded = false;
static bool gbHaveSounds = false;

const char* wadPath() noexcept {
    return WAD_PATH;
}

const char* soundDirPath() noexcept {
    return SOUND_DIR;
}

const char* monsterSoundLcdName() noexcept {
    return LCD_NAME;
}

// Whether a cached file is there, and how big. Size as well as presence: a zero byte file is a build that was
// interrupted partway, which looks identical to a good one if only existence is checked.
static bool fileSize(const char* const dir, const char* const name, int32_t& sizeOut) noexcept {
    char path[260];
    std::snprintf(path, sizeof(path), "%s\\%s", dir, name);

    WIN32_FILE_ATTRIBUTE_DATA attrs = {};

    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &attrs)) {
        sizeOut = -1;
        return false;
    }

    sizeOut = (int32_t) attrs.nFileSizeLow;
    return (sizeOut > 0);
}

void initSounds() noexcept {
    // Both halves or neither: the samples without the module have nothing to play them, and the module without the
    // samples would leave the three triggering sequences that point at silence.
    int32_t wmdSize = 0;
    int32_t lcdSize = 0;

    const bool bHaveWmd = fileSize(SOUND_DIR, WMD_NAME, wmdSize);
    const bool bHaveLcd = fileSize(SOUND_DIR, LCD_NAME, lcdSize);

    gbHaveSounds = (bHaveWmd && bHaveLcd);

    char msg[256];
    std::snprintf(
        msg, sizeof(msg),
        "master monsters: sounds %s - %s %s (%d bytes), %s %s (%d bytes), dir '%s'",
        gbHaveSounds ? "READY, the Arch-Vile, Wolfenstein SS and Keen can be heard"
                     : "ABSENT, those three will be silent",
        WMD_NAME, bHaveWmd ? "ok" : "MISSING", (int) wmdSize,
        LCD_NAME, bHaveLcd ? "ok" : "MISSING", (int) lcdSize,
        SOUND_DIR
    );
    xbLog(msg);
}

bool haveBorrowedSounds() noexcept {
    return gbHaveSounds;
}

void logSoundOverrides() noexcept {
    // Checked through the same route the game will use, rather than merely asserted: the override path is built by
    // joining the data directory to the name, and a separator this file layer will not accept is exactly the kind of
    // fault that shows up later as a fatal error in the middle of a level load.
    for (const char* const name : { WMD_NAME, LCD_NAME }) {
        char path[260];
        std::snprintf(path, sizeof(path), "%s\\%s", SOUND_DIR, name);

        std::FILE* const pFile = std::fopen(path, "rb");
        const bool bOpened = (pFile != nullptr);

        if (pFile) {
            std::fclose(pFile);
        }

        char msg[320];
        std::snprintf(
            msg, sizeof(msg), "master monsters: override '%s' registered, opens=%s via '%s'",
            name, bOpened ? "yes" : "NO", path
        );
        xbLog(msg);
    }
}

const char* soundOverrideDirPath() noexcept {
    return (gbHaveSounds) ? SOUND_DIR : nullptr;
}

bool isLoaded() noexcept {
    return gbLoaded;
}

void addOverrideWad(WadList& wadList) noexcept {
    gbLoaded = false;

    // Checked rather than attempted: 'WadFile::open' raises a fatal error instead of returning a failure, so a console
    // without the Master Edition installed would die on startup rather than simply going without the monsters.
    WIN32_FILE_ATTRIBUTE_DATA attrs = {};

    if (!GetFileAttributesExA(WAD_PATH, GetFileExInfoStandard, &attrs)) {
        char msg[256];
        std::snprintf(
            msg, sizeof(msg),
            "master monsters: sprites not cached at '%s' - the Arch-Vile, Wolfenstein SS and Keen will be absent from "
            "the roster (run the launcher once with a Master Edition disc configured to build this)",
            WAD_PATH
        );
        xbLog(msg);
        return;
    }

    wadList.add(WAD_PATH);
    gbLoaded = true;

    char msg[256];
    std::snprintf(
        msg, sizeof(msg),
        "master monsters: sprites loaded from '%s' (%d bytes) - the Arch-Vile, Wolfenstein SS and Keen are available",
        WAD_PATH, (int) attrs.nFileSizeLow
    );
    xbLog(msg);
}

END_NAMESPACE(MasterMonsters)

#endif  // #if defined(__XBOX__)
