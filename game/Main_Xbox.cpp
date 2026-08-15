#include "Doom/psx_main.h"
#include "FatalErrors.h"
#include "PsyDoom/Game.h"
#include "PsyDoom/LauncherAssets.h"
#include "PsyDoom/LauncherAudio.h"
#include "PsyDoom/SsgStyle.h"
#include "PsyDoom/XboxDiag.h"
#include "PsyDoom/XboxLog.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <algorithm>

#include <hal/debug.h>
#include <hal/video.h>
#include <hal/xbox.h>
#include <nxdk/mount.h>
#include <windows.h>
#include <SDL.h>

#define BUILD_ID __DATE__ " " __TIME__
#define MAX_PATH_LEN 260
#define STATUS_LEN 200

typedef enum GameEdition {
    EDITION_DOOM = 0,
    EDITION_FINAL_DOOM,
    EDITION_MASTER,
    EDITION_MAX
} GameEdition;

typedef struct GameOption {
    const char* name;
    const char* paths[4];  // Multiple search paths for this edition
    int found;
    char resolved_path[MAX_PATH_LEN];
} GameOption;

namespace {

// Boot log for diagnostics
static constexpr const char* kBootLogPaths[] = {
    "E:\\Apps\\PsyDoomX\\bootlog.txt",
    "bootlog.txt"
};

static GameOption g_editions[EDITION_MAX] = {
    {
        "Doom",
        {
            "E:\\Apps\\PsyDoomX\\Doom\\Doom.cue",
            "E:\\Doom\\Doom.cue",
            nullptr,
            nullptr
        },
        0,
        ""
    },
    {
        "Final Doom",
        {
            "E:\\Apps\\PsyDoomX\\FinalDoom\\FinalDoom.cue",
            "E:\\FinalDoom\\FinalDoom.cue",
            nullptr,
            nullptr
        },
        0,
        ""
    },
    {
        "Master Edition",
        {
            "E:\\Apps\\PsyDoomX\\MasterEdition\\PSXDOOM_BETA_4.cue",
            nullptr,
            nullptr,
            nullptr
        },
        0,
        ""
    }
};

static void tryMountDrive(char drive, const char* device_path) {
    (void)nxMountDrive(drive, device_path);
}

static void mountCommonDrives() {
    tryMountDrive('C', "\\Device\\Harddisk0\\Partition2\\");
    tryMountDrive('E', "\\Device\\Harddisk0\\Partition1\\");
    tryMountDrive('X', "\\Device\\Harddisk0\\Partition3\\");
    tryMountDrive('Y', "\\Device\\Harddisk0\\Partition4\\");
    tryMountDrive('Z', "\\Device\\Harddisk0\\Partition5\\");
    tryMountDrive('F', "\\Device\\Harddisk0\\Partition6\\");
    tryMountDrive('G', "\\Device\\Harddisk0\\Partition7\\");
}

static bool pathExists(const char* path) {
    DWORD attrs = GetFileAttributesA(path);
    return attrs != INVALID_FILE_ATTRIBUTES;
}

// Whether this run has started its own boot log yet.
//
// The log was opened for append and never truncated, so every boot piled onto the last one and there was no way to tell
// this run's lines from a run three days ago - a log that has to be deleted by hand to be trustworthy is a log that
// will eventually be read while stale. The first write of a session replaces the file; the rest append to it.
static bool gbBootLogStarted = false;

static void appendBootLog(const char* const msg) {
    for (const char* const path : kBootLogPaths) {
        HANDLE const h = CreateFileA(
            path,
            gbBootLogStarted ? FILE_APPEND_DATA : GENERIC_WRITE,
            FILE_SHARE_READ,
            nullptr,
            gbBootLogStarted ? OPEN_ALWAYS : CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr
        );

        if (h == INVALID_HANDLE_VALUE)
            continue;

        // Seek to the end before writing.
        //
        // 'FILE_APPEND_DATA' is not honoured here, so every line was landing at offset zero and overwriting the one
        // before it - the log only ever held whichever line was written last. The same fault as the game's own writer,
        // in a second copy of the same idea, and it hid the disc probe's results behind the menu's later chatter.
        SetFilePointer(h, 0, nullptr, FILE_END);

        DWORD written = 0;
        const DWORD len = (DWORD) std::strlen(msg);
        WriteFile(h, msg, len, &written, nullptr);

        static const char* kNewline = "\r\n";
        WriteFile(h, kNewline, 2, &written, nullptr);
        CloseHandle(h);
        gbBootLogStarted = true;    // Everything after this adds to the file rather than replacing it
        return;
    }
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Starting a game, and coming back from one.
//
// The menu used to call 'psx_main' in this process and, when the player quit, call it again for the next game. The
// engine was never built to have its entry point run twice: every global still held the last game's contents, along
// with the SDL video device, the zone allocator and the whole of WESS. That is the crash on switching from Doom to
// Final Doom, and no amount of tidying up audio or video on the way out would have fixed it - there is far too much
// state to unwind by hand, and any one thing missed brings it down again.
//
// So each game gets a process of its own. Choosing a game relaunches this XBE with the choice carried across in the
// launch data page, which the firmware preserves over the quick reboot; quitting a game relaunches it once more, back
// to the menu. Nothing is ever reused, so there is nothing to reset.
//------------------------------------------------------------------------------------------------------------------------------------------
static constexpr uint32_t LAUNCH_MAGIC = 0x58595350;    // 'PSYX' - says the launch data is ours and not a dashboard's

enum LaunchAction : uint32_t {
    LAUNCH_ACTION_MENU = 1,     // Come back to the game selection menu
    LAUNCH_ACTION_GAME = 2      // Boot straight into the edition named below
};

// The launch data page hands over a fixed 3072 bytes, and 'XLaunchXBEEx' copies exactly that much from whatever it is
// given - so this has to be that size or it would read past the end of it.
// How much of the frame rate readout to show in game.
//
// Four steps rather than a switch, because the useful states are not simply on and off: the plain rate is what a player
// wants, the rate with a frame time is what a player reporting a problem wants, and the full readout is a development
// tool that covers a good part of the picture.
enum OverlayMode : int32_t {
    OVERLAY_OFF = 0,
    OVERLAY_FPS,
    OVERLAY_FPS_MS,
    OVERLAY_VERBOSE,
    OVERLAY_MODE_COUNT
};

// Whether the cooperative and deathmatch level select names its maps or just numbers them.
//
// 'Level 24' says nothing about where you are about to play; 'MAP24: Hell Beneath' does. The name comes from the
// running game's own MAPINFO, so each game says what it calls that map rather than this having a table of its own.
enum LevelNameMode : int32_t {
    LEVELNAMES_VANILLA = 0,     // As the game shipped
    LEVELNAMES_NAMED,
    LEVELNAMES_MODE_COUNT
};

static const char* const kLevelNameModeNames[LEVELNAMES_MODE_COUNT] = {
    "Vanilla",
    "Named"
};

static const char* const kOverlayModeNames[OVERLAY_MODE_COUNT] = {
    "Off",
    "On",
    "On+MS",
    "Verbose"
};

struct LaunchPayload {
    uint32_t    magic;
    uint32_t    action;
    int32_t     edition;
    int32_t     overlayMode;
    int32_t     levelNameMode;
    uint8_t     reserved[3072 - (sizeof(uint32_t) * 2) - (sizeof(int32_t) * 3)];
};

static_assert(sizeof(LaunchPayload) == 3072, "The launch payload must fill the launch data page exactly");

static LaunchPayload gLaunchPayload;

// The overlay setting, remembered between runs.
//
// Kept beside the executable rather than in the game's config, because the launcher is what offers it and the launcher
// runs before any game is chosen.
static constexpr const char* kSettingsPath = "E:\\Apps\\PsyDoomX\\launcher.ini";

static int32_t gOverlayMode = OVERLAY_OFF;
static int32_t gLevelNameMode = LEVELNAMES_VANILLA;

// Which look the menu wears: an edition index, or SIMPLE for the plain text one it started as.
//
// Defaults to Doom, as asked. An edition that is not on the console cannot be a style, so both this and the game list
// are limited to what was actually found.
static constexpr int32_t MENU_STYLE_SIMPLE = -1;

static int32_t gMenuStyle = EDITION_DOOM;

static const char* menuStyleName(const int32_t style) noexcept {
    if (style == MENU_STYLE_SIMPLE)
        return "Simple";

    return ((style >= 0) && (style < EDITION_MAX)) ? g_editions[style].name : "Simple";
}

static void loadSettings() noexcept {
    gOverlayMode = OVERLAY_OFF;
    gLevelNameMode = LEVELNAMES_VANILLA;

    HANDLE const h = CreateFileA(kSettingsPath, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

    if (h == INVALID_HANDLE_VALUE)
        return;

    char buf[64] = {};
    DWORD read = 0;

    if (ReadFile(h, buf, sizeof(buf) - 1, &read, nullptr) && (read > 0)) {
        const char* const pFound = std::strstr(buf, "overlay=");

        if (pFound) {
            const int32_t value = std::atoi(pFound + 8);

            if ((value >= 0) && (value < OVERLAY_MODE_COUNT)) {
                gOverlayMode = value;
            }
        }

        const char* const pLevelNames = std::strstr(buf, "levelnames=");

        if (pLevelNames) {
            const int32_t value = std::atoi(pLevelNames + 11);

            if ((value >= 0) && (value < LEVELNAMES_MODE_COUNT)) {
                gLevelNameMode = value;
            }
        }

        const char* const pStyle = std::strstr(buf, "style=");

        if (pStyle) {
            const int32_t value = std::atoi(pStyle + 6);

            if ((value >= MENU_STYLE_SIMPLE) && (value < EDITION_MAX)) {
                gMenuStyle = value;
            }
        }
    }

    CloseHandle(h);
}

static void saveSettings() noexcept {
    HANDLE const h = CreateFileA(kSettingsPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

    if (h == INVALID_HANDLE_VALUE)
        return;

    // Both settings, not just the overlay.
    //
    // 'loadSettings' reads 'style' and has done since the setting existed, but this only ever wrote 'overlay' - so the
    // menu style was read back from a line that was never there and fell to its default on every boot. The two are
    // written together now so the reader and the writer cannot disagree about what the file holds.
    char buf[96] = {};
    const int len = std::snprintf(
        buf, sizeof(buf), "overlay=%d\r\nstyle=%d\r\nlevelnames=%d\r\n",
        (int) gOverlayMode, (int) gMenuStyle, (int) gLevelNameMode
    );

    if (len > 0) {
        DWORD written = 0;
        WriteFile(h, buf, (DWORD) len, &written, nullptr);
    }

    CloseHandle(h);
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Start the launcher's sound, or move its music onto the disc the current style belongs to.
//
// The SIMPLE style has no sound at all, as asked - which also means a console with no discs on it never brings any of
// this up, and behaves exactly as it did before.
//
// Only the first call actually starts anything; after that this just changes the track. The reason is in
// 'LauncherAudio.h': the engine's audio startup cannot be run twice in one process. The sound effects therefore stay
// with whichever disc was first needed, which costs nothing because 'DOOMSFX.LCD' is identical on all three, while the
// music does follow the style because it is read on a disc reader of its own.
//------------------------------------------------------------------------------------------------------------------------------------------
static void startAudioForStyle() noexcept {
    const bool bHaveStyle = ((gMenuStyle >= 0) && (gMenuStyle < EDITION_MAX) && g_editions[gMenuStyle].found);

    if (!bHaveStyle) {
        LauncherAudio::stopMusic();
        return;
    }

    const char* const pCuePath = g_editions[gMenuStyle].resolved_path;

    if (!LauncherAudio::init(pCuePath)) {
        appendBootLog("MENU: sound would not start - the menu stays silent");
        return;
    }

    LauncherAudio::playMenuMusic(pCuePath);
}

// The menu lists the games and then one more row to leave by.
//
// Kept as a row rather than a button of its own, so that everything the menu can do is visible in the list instead of
// having to be known about beforehand.
static constexpr int MENU_ITEM_FPS = EDITION_MAX;
static constexpr int MENU_ITEM_LEVELNAMES = EDITION_MAX + 1;
static constexpr int MENU_ITEM_STYLE = EDITION_MAX + 2;
static constexpr int MENU_ITEM_EXIT = EDITION_MAX + 3;
static constexpr int MENU_ITEM_COUNT = EDITION_MAX + 4;

// Where this XBE lives, most likely first.
//
// The install path leads because 'D:' is the DVD drive on this console, not this title's own directory - that mapping
// only holds for a title launched from disc. Asking for 'D:\default.xbe' first sent the console to a black screen:
// nothing was there, and by then it was too late to change course.
static constexpr const char* kSelfXbePaths[] = {
    "E:\\Apps\\PsyDoomX\\default.xbe",
    "D:\\default.xbe"
};

//------------------------------------------------------------------------------------------------------------------------------------------
// Restart this XBE, telling the new copy what to do. Does not return unless the relaunch could not be done at all.
//------------------------------------------------------------------------------------------------------------------------------------------
static void relaunchSelf(const LaunchAction action, const int edition) noexcept {
    std::memset(&gLaunchPayload, 0, sizeof(gLaunchPayload));
    gLaunchPayload.magic = LAUNCH_MAGIC;
    gLaunchPayload.action = (uint32_t) action;
    gLaunchPayload.overlayMode = gOverlayMode;
    gLaunchPayload.levelNameMode = gLevelNameMode;
    gLaunchPayload.edition = edition;

    for (const char* const pXbePath : kSelfXbePaths) {
        // Check the file is really there before asking the firmware for it.
        //
        // This matters more than it looks. 'XLaunchXBEEx' only turns back if it cannot make sense of the path it was
        // given; a path that converts cleanly but points at nothing still reboots the console, and lands it on a black
        // screen with no way back. There is no second chance after that call, so the checking has to happen here.
        if (!pathExists(pXbePath)) {
            appendBootLog("LAUNCH: no XBE at candidate path, trying the next");
            continue;
        }

        appendBootLog((action == LAUNCH_ACTION_GAME) ? "LAUNCH: restarting into game" : "LAUNCH: restarting into menu");
        appendBootLog(pXbePath);
        XLaunchXBEEx(pXbePath, &gLaunchPayload);

        // Only reached if the firmware refused the path outright
        appendBootLog("LAUNCH: the firmware would not take that path");
    }

    appendBootLog("LAUNCH: could not restart - no usable path to this XBE");
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Hand the console back to the dashboard.
//
// Passing no path asks for the dashboard rather than a title, so this lands back in whatever is installed as one -
// XBMC here - instead of leaving the console to be power cycled to get out of the game.
//------------------------------------------------------------------------------------------------------------------------------------------
static void exitToDashboard() noexcept {
    appendBootLog("MENU: returning to the dashboard");
    XLaunchXBE(nullptr);
    appendBootLog("MENU: the dashboard would not launch");
}

//------------------------------------------------------------------------------------------------------------------------------------------
// What the copy of this XBE that started us asked for, if it was one of ours. Returns false for a normal cold start.
//------------------------------------------------------------------------------------------------------------------------------------------
static bool readLaunchRequest(LaunchAction& actionOut, int& editionOut) noexcept {
    actionOut = LAUNCH_ACTION_MENU;
    editionOut = -1;

    // Note: the settings are deliberately NOT defaulted here.
    //
    // They were, and it undid the file that had just been read. 'loadSettings' runs immediately before this and puts
    // what was saved into those globals; resetting them at the top of this threw that away before even looking at
    // whether there was a payload to replace it with. On a cold boot there is none - the function returns below - and
    // the settings were back at their defaults having been loaded correctly a moment earlier.
    //
    // It hid because the game and the launcher hand these back and forth in the payload: start a game, quit to the
    // menu, and the value comes back from the payload rather than from the file, so it looks like it stuck. Only
    // turning the console off showed otherwise. The frame rate setting had the same fault; the menu style did not,
    // which is why that one survived a power cycle and the other two did not.
    //
    // Anything the payload carries is applied further down, which is the only place these should be written.

    unsigned long launchDataType = 0;
    const unsigned char* pLaunchData = nullptr;

    if (XGetLaunchInfo(&launchDataType, &pLaunchData) != 0)
        return false;

    if (!pLaunchData)
        return false;

    LaunchPayload payload;
    std::memcpy(&payload, pLaunchData, sizeof(payload));

    if (payload.magic != LAUNCH_MAGIC)
        return false;

    actionOut = (LaunchAction) payload.action;
    editionOut = payload.edition;

    if ((payload.overlayMode >= 0) && (payload.overlayMode < OVERLAY_MODE_COUNT)) {
        gOverlayMode = payload.overlayMode;
    }

    if ((payload.levelNameMode >= 0) && (payload.levelNameMode < LEVELNAMES_MODE_COUNT)) {
        gLevelNameMode = payload.levelNameMode;
    }

    return true;
}

static SDL_GameController* openFirstController() {
    for (int i = 0; i < SDL_NumJoysticks(); ++i) {
        if (SDL_IsGameController(i)) {
            SDL_GameController* pad = SDL_GameControllerOpen(i);
            if (pad) {
                return pad;
            }
        }
    }
    return nullptr;
}

static int buttonPressedEdge(SDL_GameController* pad, SDL_GameControllerButton button, Uint8* previous) {
    Uint8 current = 0;
    int rising;

    if (pad) {
        current = (Uint8)SDL_GameControllerGetButton(pad, button);
    }

    rising = (current != 0) && (*previous == 0);
    *previous = current;
    return rising;
}

static void scanEditions() {
    for (int i = 0; i < EDITION_MAX; ++i) {
        g_editions[i].found = 0;
        g_editions[i].resolved_path[0] = '\0';

        for (int j = 0; g_editions[i].paths[j] != nullptr; ++j) {
            if (pathExists(g_editions[i].paths[j])) {
                g_editions[i].found = 1;
                std::strcpy(g_editions[i].resolved_path, g_editions[i].paths[j]);
                break;
            }
        }
    }
}

static const char* getFileName(const char* path) {
    if (!path)
        return "";
    const char* slash = std::strrchr(path, '\\');
    return slash ? (slash + 1) : path;
}

[[noreturn]] static void showErrorAndHalt(const char* msg) {
    appendBootLog(msg);
    debugClearScreen();
    debugPrint("PSYDOOM BOOTSTRAP ERROR\n");
    debugPrint("BUILD: %s\n\n", BUILD_ID);
    debugPrint("%s\n\n", msg);
    debugPrint("This screen will remain until restart.\n");
    
    while (true) {
        Sleep(500);
    }
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Show a fatal error rather than aborting on one.
//
// Nothing in the launcher could raise one before, so the default behaviour - print to a console nobody is watching, then
// 'std::abort' - never came up. Starting the sound changes that: it opens a disc, reads its file system and works out
// which game it is, and a disc that is readable but not one the engine recognises ends in a fatal error. Without this
// that is a black screen saying nothing; with it, it says what happened.
//------------------------------------------------------------------------------------------------------------------------------------------
static void launcherFatalErrorHandler(const char* const msg) noexcept {
    showErrorAndHalt((msg) ? msg : "An unspecified fatal error occurred in the launcher.");
}

static void runBootstrapMenu() {
    FatalErrors::gFatalErrorHandler = launcherFatalErrorHandler;
    XVideoSetMode(640, 480, 32, REFRESH_DEFAULT);

    SDL_GameController* pad = nullptr;
    SDL_Event event;
    int sdl_ready = 0;
    int selected = EDITION_DOOM;
    int needs_redraw = 1;

    // The cursor's two frame animation
    int cursorFrame = 0;
    Uint32 lastCursorTick = SDL_GetTicks();
    Uint8 prev_a = 0;
    Uint8 prev_start = 0;
    Uint8 prev_back = 0;
    char status[STATUS_LEN] = "Select game edition and press START";

    appendBootLog("MENU: Initializing menu system");

    // Initialize SDL for controller input
    if (SDL_Init(SDL_INIT_GAMECONTROLLER) != 0) {
        debugPrint("[WARN] SDL_Init FAILED: %s\n", SDL_GetError());
        debugPrint("Continuing without controller input.\n");
        Sleep(1500);
    } else {
        sdl_ready = 1;
        pad = openFirstController();
    }

    // Scan for available editions
    scanEditions();
    appendBootLog("MENU: Scanned editions");

    // Can the launcher read the games' discs? Asked once, here, because this is the first point at which it is known
    // which editions exist - asked any earlier and every one of them looks absent.
    //
    // The styled menu needs the background, the font and the skull out of whichever edition it is wearing, and all of
    // that lives inside these discs. Nothing is decoded or drawn yet: if they cannot be opened then none of that could
    // work either, and if they can then everything after is about their contents rather than reaching them.
    // What each edition's menu is made of, read out of the discs themselves rather than guessed.
    //
    // Doom and Final Doom share a lump name and differ only in palette. The Master Edition uses a different picture
    // altogether and names it in its own MAPINFO on the disc - 'PicBack = "COVERG"  28' - which is nowhere in PsyDoom
    // and could not have been arrived at by trying values on hardware.
    static const LauncherAssets::MenuArt kMenuArt[EDITION_MAX] = {
        LauncherAssets::MENU_ART_DOOM,
        LauncherAssets::MENU_ART_FINAL_DOOM,
        LauncherAssets::MENU_ART_MASTER
    };

    // Which edition each disc is, in the terms the super shotgun setting uses.
    //
    // This is the only place all three discs are readable at once - a game only ever sees the one it was started with -
    // so it is where the sprites one edition might borrow from another have to be taken.
    static const int32_t kSsgStyles[EDITION_MAX] = {
        SsgStyle::STYLE_DOOM,
        SsgStyle::STYLE_FINAL,
        SsgStyle::STYLE_MASTER
    };

    for (int i = 0; i < EDITION_MAX; ++i) {
        if (g_editions[i].found) {
            LauncherAssets::probeDisc(g_editions[i].resolved_path, kMenuArt[i], kSsgStyles[i]);
        }
    }

    // Only now can a style be loaded.
    //
    // The first attempt did this before the editions were even scanned, let alone cached, so it asked for a look that
    // did not exist yet and quietly fell back to plain text - which is exactly what appeared on screen.
    //
    // 'gMenuStyle' is an edition index, or SIMPLE for the plain text menu. An edition that is not present cannot be
    // worn, so the setting falls back rather than leaving the menu blank.
    if ((gMenuStyle >= 0) && (gMenuStyle < EDITION_MAX) && g_editions[gMenuStyle].found) {
        LauncherAssets::useStyle(g_editions[gMenuStyle].resolved_path);
    }

    // Sound is started after the menu is on screen rather than before it.
    //
    // Bringing it up reads a .WMD and a 177 KiB .LCD off the disc and uploads the samples into SPU RAM, which is not
    // instant. Doing that first would leave the console showing whatever the last program left there for as long as it
    // took, which is indistinguishable from a launcher that has failed to start.
    int framesDrawn = 0;
    bool audioStartPending = true;

    // How much of the styled menu needs putting on screen again.
    //
    // There is one framebuffer on this console and no back buffer, so the display is reading it while it is being
    // written. Repainting the whole thing - background, then every line of text over it - means that for part of every
    // repaint the text is simply not there yet, and that is what was seen as the text and the cursor flickering. It
    // happens four times a second whatever the player does, because the cursor animates.
    //
    // So the whole menu is only repainted when the whole menu has changed. The cursor's own animation moves 32x36
    // pixels; changing a setting repaints one line. Neither is on screen long enough to catch.
    bool cursorDirty = false;
    int  dirtyRow = -1;                         // A single row whose text has changed, or -1
    int32_t rowYs[MENU_ITEM_COUNT] = {};        // Where each row was drawn, filled in by a full repaint
    int32_t prevSkullY = -1;                    // Where the cursor was last drawn, so it can be taken back off

    for (int i = 0; i < MENU_ITEM_COUNT; ++i) {
        rowYs[i] = -1;
    }

    // Is the menu wearing a style, or is it the plain text list?
    //
    // Asked wherever it matters rather than kept in a variable, because changing the style changes the answer and a
    // stale copy of it would repaint the wrong menu.
    const auto isStyled = [&]() noexcept -> bool {
        return (
            (gMenuStyle >= 0) && (gMenuStyle < EDITION_MAX) && g_editions[gMenuStyle].found &&
            LauncherAssets::isStyleLoaded()
        );
    };

    // Moving the selection, which the styled menu can do without repainting anything else
    const auto markSelectionChanged = [&]() noexcept {
        if (isStyled()) {
            cursorDirty = true;
        } else {
            needs_redraw = 1;
        }
    };

    // Changing a setting, which alters one line of text
    const auto markRowChanged = [&](const int row) noexcept {
        if (isStyled()) {
            dirtyRow = row;
        } else {
            needs_redraw = 1;
        }
    };

    //--------------------------------------------------------------------------------------------------------------------------------------
    // The styled menu is driven the way the game's menus are; the plain list keeps the scheme it always had.
    //
    // Three differences, all of them from 'M_Control' in 'm_main.cpp':
    //
    //   - A direction acts at once when pressed and then repeats while held. PsyDoom waits 15 vblanks between repeats,
    //     which at 60 Hz is 250 milliseconds. The launcher used to move once per press however long it was held.
    //   - The 'ok' button confirms as well as start. That is 'Gamepad A' in PsyDoom's default bindings, and A here was
    //     a logged no-op.
    //   - The menu makes its own sounds. Those are silent on the plain list, which has no style and therefore no disc
    //     to have taken them from.
    //--------------------------------------------------------------------------------------------------------------------------------------
    static constexpr Uint32 MENU_REPEAT_MS = 250;   // 15 vblanks at 60 Hz, which is what the game uses

    struct DirState {
        Uint8   bHeld;
        Uint32  nextRepeatTick;
    };

    DirState dirUp = {}, dirDown = {}, dirLeft = {}, dirRight = {};

    const auto dirPressed = [&](const SDL_GameControllerButton button, DirState& state) noexcept -> bool {
        const Uint8 bNowHeld = (pad) ? (Uint8) SDL_GameControllerGetButton(pad, button) : (Uint8) 0;
        const Uint32 now = SDL_GetTicks();
        bool bFire = false;

        if (bNowHeld && (!state.bHeld)) {
            bFire = true;                                   // Just pressed: acts immediately, as the game does
            state.nextRepeatTick = now + MENU_REPEAT_MS;
        }
        else if (bNowHeld && isStyled() && ((int32_t)(now - state.nextRepeatTick) >= 0)) {
            bFire = true;                                   // Held down: repeats, but only where the game's scheme is in force
            state.nextRepeatTick = now + MENU_REPEAT_MS;
        }

        state.bHeld = bNowHeld;
        return bFire;
    };

    // The menu's sounds, which belong to the style and so are silent without one
    const auto moveSound    = [&]() noexcept { if (isStyled()) { LauncherAudio::playMoveSound(); } };
    const auto selectSound  = [&]() noexcept { if (isStyled()) { LauncherAudio::playSelectSound(); } };
    const auto confirmSound = [&]() noexcept { if (isStyled()) { LauncherAudio::playConfirmSound(); } };

    while (true) {
        // Keep the sequencer moving. Cheap, and nothing that is triggered is heard without it.
        LauncherAudio::update();

        if (audioStartPending && (framesDrawn > 0)) {
            audioStartPending = false;
            startAudioForStyle();
            needs_redraw = 1;   // The wait for the disc will have cost a frame or two of the cursor's animation
        }

        if (sdl_ready) {
            while (SDL_PollEvent(&event)) {
                if (event.type == SDL_CONTROLLERDEVICEADDED && !pad) {
                    pad = SDL_GameControllerOpen(event.cdevice.which);
                } else if (event.type == SDL_CONTROLLERDEVICEREMOVED && pad) {
                    SDL_GameController* removed = SDL_GameControllerFromInstanceID(event.cdevice.which);
                    if (removed == pad) {
                        SDL_GameControllerClose(pad);
                        pad = nullptr;
                    }
                }
            }
        }

        // The cursor blinks between its two frames the way the game's does
        {
            const Uint32 now = SDL_GetTicks();

            if (now - lastCursorTick >= 250u) {
                lastCursorTick = now;
                cursorFrame ^= 1;

                // Only the cursor changed, so only the cursor is repainted. This used to repaint everything, four
                // times a second, which is where most of the flicker came from.
                if (isStyled()) {
                    cursorDirty = true;
                } else {
                    needs_redraw = 1;
                }
            }
        }

        // Handle input
        if (dirPressed(SDL_CONTROLLER_BUTTON_DPAD_UP, dirUp)) {
            selected = (selected + MENU_ITEM_COUNT - 1) % MENU_ITEM_COUNT;
            markSelectionChanged();
            moveSound();
        }

        if (dirPressed(SDL_CONTROLLER_BUTTON_DPAD_DOWN, dirDown)) {
            selected = (selected + 1) % MENU_ITEM_COUNT;
            markSelectionChanged();
            moveSound();
        }

        // Left and right change the setting on the row that has one.
        //
        // Both are read every pass whatever row is selected, so that a direction held while moving between rows does
        // not carry a stale 'was pressed' state onto the row that arrives under it.
        const bool bLeftPressed = dirPressed(SDL_CONTROLLER_BUTTON_DPAD_LEFT, dirLeft);
        const bool bRightPressed = dirPressed(SDL_CONTROLLER_BUTTON_DPAD_RIGHT, dirRight);

        if (selected == MENU_ITEM_FPS) {
            if (bLeftPressed) {
                gOverlayMode = (gOverlayMode + OVERLAY_MODE_COUNT - 1) % OVERLAY_MODE_COUNT;
                saveSettings();     // Written as it changes, so it survives however the launcher is left
                markRowChanged(MENU_ITEM_FPS);
                selectSound();
            }

            if (bRightPressed) {
                gOverlayMode = (gOverlayMode + 1) % OVERLAY_MODE_COUNT;
                saveSettings();
                markRowChanged(MENU_ITEM_FPS);
                selectSound();
            }
        }

        if (selected == MENU_ITEM_LEVELNAMES) {
            if (bLeftPressed || bRightPressed) {
                const int dir = (bRightPressed) ? 1 : (LEVELNAMES_MODE_COUNT - 1);
                gLevelNameMode = (gLevelNameMode + dir) % LEVELNAMES_MODE_COUNT;
                saveSettings();     // Written as it changes, so it survives however the launcher is left
                markRowChanged(MENU_ITEM_LEVELNAMES);
                selectSound();
                appendBootLog("MENU: level names changed");
            }
        }

        // The style row cycles through Simple and whichever editions are actually here.
        //
        // Skipping the absent ones matters: an edition with no disc has no background and no font, so offering it as a
        // style would land the menu back on plain text with no way to tell that from a fault.
        if (selected == MENU_ITEM_STYLE) {
            if (bLeftPressed || bRightPressed) {
                const int dir = (bRightPressed) ? 1 : -1;
                int next = gMenuStyle;

                for (int tries = 0; tries < EDITION_MAX + 2; ++tries) {
                    next += dir;

                    if (next >= EDITION_MAX) { next = MENU_STYLE_SIMPLE; }
                    if (next < MENU_STYLE_SIMPLE) { next = EDITION_MAX - 1; }

                    if ((next == MENU_STYLE_SIMPLE) || g_editions[next].found)
                        break;
                }

                gMenuStyle = next;
                saveSettings();

                // Live, as asked: the look changes as the setting does
                if ((gMenuStyle >= 0) && g_editions[gMenuStyle].found) {
                    LauncherAssets::useStyle(g_editions[gMenuStyle].resolved_path);
                } else {
                    LauncherAssets::useStyle("");
                }

                // And so does what it sounds like. The music moves onto the new style's disc; SIMPLE falls silent.
                //
                // If the launcher started on SIMPLE then this is where sound is brought up for the first time, which is
                // why it is the same call as the one at startup rather than a lighter one.
                selectSound();
                startAudioForStyle();

                needs_redraw = 1;
                appendBootLog("MENU: style changed");
            }
        }

        // Confirm.
        //
        // Start has always done this. A does it too on the styled menu, because that is 'Menu Ok' in PsyDoom's default
        // bindings and the game's own menus are confirmed with it - on the plain list A stays the no-op it was, so that
        // scheme is unchanged.
        // Both are read every pass rather than short circuited, or the one that is not reached keeps a stale 'was held'
        // state and fires an edge of its own on the next pass.
        const bool bStartPressed = buttonPressedEdge(pad, SDL_CONTROLLER_BUTTON_START, &prev_start);
        const bool bOkPressed = buttonPressedEdge(pad, SDL_CONTROLLER_BUTTON_A, &prev_a);
        const bool bConfirmPressed = (bStartPressed || (bOkPressed && isStyled()));

        if (bConfirmPressed) {
            appendBootLog("MENU: confirmed");
            confirmSound();

            // The last row leaves rather than starting anything
            if (selected == MENU_ITEM_EXIT) {
                std::strcpy(status, "Returning to the dashboard...");
                needs_redraw = 1;
                LauncherAudio::shutdown();
                exitToDashboard();

                // Only reached if the dashboard would not launch
                std::strcpy(status, "ERROR: could not return to the dashboard");
                continue;
            }

            if (!g_editions[selected].found) {
                std::snprintf(status, sizeof(status), "ERROR: %s not found!", g_editions[selected].name);
                needs_redraw = 1;
                Sleep(2000);
                continue;
            }

            // Restart into the game rather than running it here.
            //
            // This used to call 'psx_main' directly, which meant a second game ran in a process that still held all of
            // the first one's state. See the note on 'relaunchSelf'.
            // Silence the hardware first. The console is about to reboot into another copy of this XBE, and the sound
            // chip is still being fed by a thread that is about to stop existing.
            appendBootLog("MENU: restarting into the selected game");
            LauncherAudio::shutdown();
            relaunchSelf(LAUNCH_ACTION_GAME, selected);

            // Only reached if the restart failed
            std::strcpy(status, "ERROR: could not restart to launch the game");
            needs_redraw = 1;
        }

        // BACK to hand the console back to the dashboard
        if (buttonPressedEdge(pad, SDL_CONTROLLER_BUTTON_BACK, &prev_back)) {
            std::strcpy(status, "Returning to the dashboard...");
            confirmSound();
            LauncherAudio::shutdown();
            exitToDashboard();

            // Only reached if the dashboard would not launch
            std::strcpy(status, "ERROR: could not return to the dashboard");
            needs_redraw = 1;
        }

        if ((!needs_redraw) && (!cursorDirty) && (dirtyRow < 0)) {
            Sleep(16);
            continue;
        }

        // How the styled menu is laid out. Shared between painting all of it and painting one line of it, so the two
        // cannot disagree about where anything is.
        static constexpr int32_t ROW_H       = 30;
        static constexpr int32_t TEXT_X      = 128;
        static constexpr int32_t SKULL_X     = 90;
        static constexpr int32_t FIRST_ROW_Y = 90;
        static constexpr int32_t SKULL_DY    = -2;
        static constexpr int32_t SKULL_W     = LauncherAssets::CURSOR_DRAW_W;
        static constexpr int32_t SKULL_H     = LauncherAssets::CURSOR_DRAW_H;
        static constexpr int32_t TEXT_H      = LauncherAssets::BIG_TEXT_DRAW_H;

        // What each row says. Built here so that repainting one line says exactly what a full repaint would have.
        const auto rowText = [&](const int row, char* const out, const size_t outSize) noexcept {
            if ((row >= 0) && (row < EDITION_MAX)) {
                std::snprintf(out, outSize, "%s", g_editions[row].name);
            } else if (row == MENU_ITEM_FPS) {
                std::snprintf(out, outSize, "FPS %s", kOverlayModeNames[gOverlayMode]);
            } else if (row == MENU_ITEM_LEVELNAMES) {
                std::snprintf(out, outSize, "Levels %s", kLevelNameModeNames[gLevelNameMode]);
            } else if (row == MENU_ITEM_STYLE) {
                std::snprintf(out, outSize, "Style %s", menuStyleName(gMenuStyle));
            } else {
                std::snprintf(out, outSize, "Quit");
            }
        };

        // Which menu is being drawn, decided before anything is put on screen.
        //
        // This used to clear the screen and print the header first, and ask afterwards. Both of the faults that showed
        // came from that: the clear followed by a redraw is a flicker every frame - the same shape as the splitscreen
        // one - and the header survived in the bars either side of the picture, where the background does not reach.
        //
        // Nothing is drawn now until it is known which menu is doing the drawing.
        const bool bStyled = isStyled();
        bool bStyledDrawn = false;

        if (bStyled) {
            // A whole repaint, or just the parts that changed.
            //
            // Only the first of these touches the background, and it is the only one that can be seen half-finished.
            // Everything the player does moment to moment - moving the cursor, watching it animate, changing a setting -
            // goes down one of the other two paths and repaints a few hundred pixels instead of a quarter of a million.
            if (!needs_redraw) {
                // One line of text, where a setting has changed under it
                if ((dirtyRow >= 0) && (dirtyRow < MENU_ITEM_COUNT) && (rowYs[dirtyRow] >= 0)) {
                    char text[64];
                    rowText(dirtyRow, text, sizeof(text));

                    LauncherAssets::restoreBackgroundRect(TEXT_X, rowYs[dirtyRow], 640 - TEXT_X, TEXT_H);
                    LauncherAssets::drawBigText(TEXT_X, rowYs[dirtyRow], text);
                }

                // And the cursor, taken off where it was and put back where it is
                if (cursorDirty) {
                    if (prevSkullY >= 0) {
                        LauncherAssets::restoreBackgroundRect(SKULL_X, prevSkullY, SKULL_W, SKULL_H);
                        prevSkullY = -1;
                    }

                    if ((selected >= 0) && (selected < MENU_ITEM_COUNT) && (rowYs[selected] >= 0)) {
                        LauncherAssets::drawSkull(SKULL_X, rowYs[selected] + SKULL_DY, cursorFrame);
                        prevSkullY = rowYs[selected] + SKULL_DY;
                    }
                }

                bStyledDrawn = true;
            }
            else if (LauncherAssets::drawCachedBackground(g_editions[gMenuStyle].resolved_path)) {
                // The title, centred, in the same font the game titles its own menus with
                {
                    const char* const pTitle = "PsyDoomX";
                    LauncherAssets::drawBigText((640 - LauncherAssets::bigTextWidth(pTitle)) / 2, 36, pTitle);
                }

                // Editions that are not on the console are not listed, so the rows are not the menu's items - which is
                // why where each one landed is written down rather than worked out again later.
                int32_t rowY = FIRST_ROW_Y;
                prevSkullY = -1;

                for (int row = 0; row < MENU_ITEM_COUNT; ++row) {
                    if ((row < EDITION_MAX) && (!g_editions[row].found)) {
                        rowYs[row] = -1;
                        continue;
                    }

                    char text[64];
                    rowText(row, text, sizeof(text));
                    LauncherAssets::drawBigText(TEXT_X, rowY, text);
                    rowYs[row] = rowY;

                    if (selected == row) {
                        LauncherAssets::drawSkull(SKULL_X, rowY + SKULL_DY, cursorFrame);
                        prevSkullY = rowY + SKULL_DY;
                    }

                    rowY += ROW_H;
                }

                bStyledDrawn = true;
            }
        }

        if (bStyledDrawn) {
            XVideoFlushFB();
            needs_redraw = 0;
            cursorDirty = false;
            dirtyRow = -1;
            framesDrawn++;
            continue;   // The styled menu draws everything itself; the text list below is the fallback
        }

        debugClearScreen();
        debugPrint("=[ PsyDoomX Bootstrap | %s ]=\n\n", BUILD_ID);

        debugPrint("Select Game Edition:\n\n");

        for (int i = 0; i < EDITION_MAX; ++i) {
            const char* marker = (i == selected) ? ">" : " ";
            const char* status_str = g_editions[i].found ? "[OK]" : "[--]";

            debugPrint("[%c] %s   %s\n", *marker, g_editions[i].name, status_str);

            if (i == selected && g_editions[i].found) {
                debugPrint("    Path: %s\n", getFileName(g_editions[i].resolved_path));
            }
        }

        debugPrint(
            "[%c] FPS Counter: %-8s%s\n",
            (selected == MENU_ITEM_FPS) ? '>' : ' ',
            kOverlayModeNames[gOverlayMode],
            (selected == MENU_ITEM_FPS) ? "  (LEFT/RIGHT changes)" : ""
        );

        debugPrint(
            "[%c] Level Names: %-14s%s\n",
            (selected == MENU_ITEM_LEVELNAMES) ? '>' : ' ',
            kLevelNameModeNames[gLevelNameMode],
            (selected == MENU_ITEM_LEVELNAMES) ? "  (LEFT/RIGHT changes)" : ""
        );

        // The style row, which this list did not show.
        //
        // It was navigable and changeable here but never printed, so on the plain menu the cursor could sit on a row
        // that was not there and LEFT and RIGHT would appear to do nothing - and the only way back to a styled menu is
        // through this row, which made SIMPLE a one way trip.
        debugPrint(
            "[%c] Menu Style:  %-14s%s\n",
            (selected == MENU_ITEM_STYLE) ? '>' : ' ',
            menuStyleName(gMenuStyle),
            (selected == MENU_ITEM_STYLE) ? "  (LEFT/RIGHT changes)" : ""
        );

        debugPrint("[%c] Exit to Dashboard\n", (selected == MENU_ITEM_EXIT) ? '>' : ' ');

        debugPrint("\n");
        debugPrint("Diagnostics:\n");

        int doom_found = 0, final_found = 0, master_found = 0;
        for (int i = 0; i < EDITION_MAX; ++i) {
            if (g_editions[i].found) {
                if (i == EDITION_DOOM) doom_found = 1;
                else if (i == EDITION_FINAL_DOOM) final_found = 1;
                else if (i == EDITION_MASTER) master_found = 1;
            }
        }

        debugPrint("  Doom Available: %s\n", doom_found ? "YES" : "NO");
        debugPrint("  Final Doom Available: %s\n", final_found ? "YES" : "NO");
        debugPrint("  Master Edition Available: %s\n", master_found ? "YES" : "NO");

        debugPrint("\nControls:\n");
        debugPrint("  UP/DOWN = Move selection\n");
        debugPrint("  START = Confirm selection\n");
        debugPrint("  BACK  = Exit to dashboard (shortcut)\n\n");
        debugPrint("Status: %s\n", status);

        // Cleared here as well as on the styled path. The plain list repaints all of itself whatever changed, and a
        // partial-repaint flag left set behind it would make this loop redraw without pause.
        needs_redraw = 0;
        cursorDirty = false;
        dirtyRow = -1;
        framesDrawn++;
        Sleep(16);
    }
}

} // namespace

int main(const int argc, const char* const* const argv) {
    appendBootLog("BOOT: entered Main_Xbox");

    try {
        // Mount Xbox drives
        appendBootLog("BOOT: mounting drives");
        mountCommonDrives();

        // Start the diagnostic relay as early as the drives allow, since it reads its server address from E:.
        //
        // This does not wait for the network: bringing that up and connecting happen on a background thread, so a
        // console sat waiting for DHCP does not delay the game by a millisecond. If nothing is listening the relay is
        // inert and the game neither knows nor cares.
        XboxLog::init();
        // Say which build this is, first thing. Without it a log cannot be tied to an XBE, and a session spent on the
        // wrong binary looks exactly like a change that did not work.
        #if defined(PSYDOOM_XBOX_BUILD_ID)
            XBOX_LOGI(General, "build %s (compiled %s)", PSYDOOM_XBOX_BUILD_ID, PSYDOOM_XBOX_BUILD_TIME);
        #endif

        XBOX_LOGI(General, "boot: drives mounted, relay started");

        // Initialize video
        appendBootLog("BOOT: initializing video mode");
        XVideoSetMode(640, 480, 32, REFRESH_DEFAULT);

        // Check if we're being passed a CUE argument from command line
        // (This could happen from XBMC launch parameters)
        for (int i = 1; i < argc; ++i) {
            if (argv[i] && std::strcmp(argv[i], "-cue") == 0) {
                appendBootLog("BOOT: detected -cue in command line, using passthrough launch");
                try {
                    return psx_main(argc, argv);
                } catch (...) {
                    showErrorAndHalt("Exception during command-line passthrough launch");
                }
            }
        }

        // Were we restarted by the menu to play something?
        //
        // The choice is carried in the launch data page rather than run in the menu's own process, so that the engine
        // only ever starts once per process. See the note on 'relaunchSelf'.
        LaunchAction launchAction = LAUNCH_ACTION_MENU;
        int launchEdition = -1;

        // What the setting was last left at.
        //
        // Read before the launch data, which overrides it when a game is being started - the launcher passes its
        // current value through rather than relying on both processes reading the same file at the same moment.
        loadSettings();


        if (readLaunchRequest(launchAction, launchEdition) && (launchAction == LAUNCH_ACTION_GAME)) {
            // Whatever the launcher was set to, applied before anything draws
            XboxDiag::gShowFullOverlay = (gOverlayMode == OVERLAY_VERBOSE);
            XboxDiag::gShowFpsOverlay = ((gOverlayMode == OVERLAY_FPS) || (gOverlayMode == OVERLAY_FPS_MS));
            XboxDiag::gShowFpsMs = (gOverlayMode == OVERLAY_FPS_MS);
            Game::gbUseNamedLevels = (gLevelNameMode == LEVELNAMES_NAMED);

            appendBootLog("BOOT: restarted to play a game");

            if ((launchEdition >= 0) && (launchEdition < EDITION_MAX)) {
                scanEditions();

                if (g_editions[launchEdition].found) {
                    XBOX_LOGI(General, "boot: starting '%s'", g_editions[launchEdition].name);

                    std::vector<const char*> gameArgv;
                    gameArgv.push_back("PsyDoomX");
                    gameArgv.push_back("-cue");
                    gameArgv.push_back(g_editions[launchEdition].resolved_path);

                    // Run the game in 15-bit.
                    //
                    // PlayStation VRAM is 15-bit, so this is the depth the picture actually has: presenting it at 32
                    // writes twice the bytes to say exactly the same thing, and that write was measured at 14.3ms of a
                    // 48ms frame. Nothing is lost by matching the source.
                    //
                    // Set here rather than at boot so the menu keeps its 32-bit screen, and set before 'psx_main'
                    // because SDL reads the mode when it starts and follows whatever it finds - it never sets one
                    // itself, so there is nothing to fight over.
                    //
                    // GAMECUBE: does not apply. Its framebuffer is YUV, so the equivalent saving is a different change.
                    appendBootLog("BOOT: switching to a 15-bit screen for the game");
                    XVideoSetMode(640, 480, 15, REFRESH_DEFAULT);

                    appendBootLog("BOOT: calling psx_main");
                    psx_main((int) gameArgv.size(), gameArgv.data());

                    // The game has been quit. Go back to the menu in a process of its own rather than carrying this
                    // one's state into it - which is the whole point of restarting to get here.
                    appendBootLog("BOOT: game exited, restarting into the menu");
                    relaunchSelf(LAUNCH_ACTION_MENU, 0);
                } else {
                    appendBootLog("BOOT: the requested game is no longer present - falling back to the menu");
                }
            }
        }

        // Otherwise, show the menu
        appendBootLog("BOOT: showing game selection menu");
        runBootstrapMenu();

    } catch (const std::exception& e) {
        std::string msg = std::string("Exception in Main_Xbox: ") + e.what();
        showErrorAndHalt(msg.c_str());
    } catch (...) {
        showErrorAndHalt("Unknown exception in Main_Xbox");
    }

    // Should never reach here
    return 1;
}