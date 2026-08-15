//------------------------------------------------------------------------------------------------------------------------------------------
// Management of some in-game player preferences and saving passwords for the last level completed so it can be restored on relaunch
//------------------------------------------------------------------------------------------------------------------------------------------
#include "PlayerPrefs.h"

#include "Asserts.h"
#include "Doom/Base/s_sound.h"
#include "Doom/UI/o_main.h"
#include "Doom/UI/pw_main.h"
#include "FileUtils.h"
#include "Game.h"
#include "IniUtils.h"
#include "Utils.h"
#include "Video.h"
#include "Wess/psxspu.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#if defined(__XBOX__)
    #include <windows.h>
#endif

#if defined(__XBOX__)
    extern "C++" void xbLog(const char* msg) noexcept;
#endif

BEGIN_NAMESPACE(PlayerPrefs)

 // Sanity check!
static_assert(C_ARRAY_SIZE(gPasswordCharBuffer) == C_ARRAY_SIZE(Password::pwChars));

// Name of the user prefs file: it can reside in either the writable user data folder (default) or in the current working directory.
// We save to the current working directory if the file is found existing there on launch.
static constexpr const char* const PREFS_FILE_NAME = "saved_prefs.ini";

// Globally exposed settings
int32_t             gTurnSpeedMult100[Controls::MAX_LOCAL_PLAYERS];          // In-game tweakable turn speed multiplier expressed in integer percentage points (0-500)
bool                gbAlwaysRun[Controls::MAX_LOCAL_PLAYERS];                // If set then the player runs by default and the run action causes slower walking
bool                gbUncapFramerate;           // Run the game with an uncapped framerate?
StatDisplayMode     gStatDisplayMode[Controls::MAX_LOCAL_PLAYERS];           // Which stats should be displayed
Password            gLastPassword_Doom;         // Password for the current level the player is on: Doom
Password            gLastPassword_FDoom;        // Password for the current level the player is on: Final Doom
Password            gLastPassword_GecMe;        // Password for the current level the player is on: GEC Master Edition

#if defined(__XBOX__)
    int32_t         gSsgStyle[Controls::MAX_LOCAL_PLAYERS];         // Whose super shotgun each player carries - see 'SsgStyle.h'
    int32_t         gPlayerColour[Controls::MAX_LOCAL_PLAYERS];     // What colour each player is drawn in - see 'PlayerColour.h'
#endif

// Internally kept settings
static int32_t      gSoundVol;                      // Option for sound volume
static int32_t      gMusicVol;                      // Option for music volume
static bool         gbStartupWithVulkanRenderer;    // Startup using the Vulkan renderer? (if enabled, and the host machine is capable)

// If true then we save the prefs file to the current working directory rather than to the user data folder
static bool gbUseWorkingDirPrefsFile = false;

//------------------------------------------------------------------------------------------------------------------------------------------
// Converts a 'Password' object into a null terminated 'String16'
//------------------------------------------------------------------------------------------------------------------------------------------
static String16 getPasswordCString(const Password& password) noexcept {
    static_assert(sizeof(String16) > sizeof(Password));
    return String16(password.pwChars, sizeof(password.pwChars));
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Returns the path to the ini file used to hold player prefs
//------------------------------------------------------------------------------------------------------------------------------------------
static std::string getPrefsFilePath() noexcept {
    if (gbUseWorkingDirPrefsFile) {
        return PREFS_FILE_NAME;
    } else {
        const std::string userDataFolder = Utils::getOrCreateUserDataFolder();
        return userDataFolder + PREFS_FILE_NAME;
    }
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Returns the 'Password' struct associated with the specified preferences field name
//------------------------------------------------------------------------------------------------------------------------------------------
static Password* getPasswordForPrefsFieldName(const char* const fieldName) noexcept {
    if (std::strcmp(fieldName, "lastPassword_Doom") == 0)
        return &gLastPassword_Doom;

    if (std::strcmp(fieldName, "lastPassword_FinalDoom") == 0)
        return &gLastPassword_FDoom;

    if (std::strcmp(fieldName, "lastPassword_GecMe") == 0)
        return &gLastPassword_GecMe;

    return nullptr;
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Converts a character to a PSX Doom password character index.
// Returns -1 if there is no valid conversion.
//------------------------------------------------------------------------------------------------------------------------------------------
static int32_t charToPwCharIndex(const char c) noexcept {
    const char cUpper = (char) std::toupper(c);

    switch (cUpper) {
        case 'B': return 0;     case 'L': return 8;     case 'V': return 16;    case '3': return 24;
        case 'C': return 1;     case 'M': return 9;     case 'W': return 17;    case '4': return 25;
        case 'D': return 2;     case 'N': return 10;    case 'X': return 18;    case '5': return 26;
        case 'F': return 3;     case 'P': return 11;    case 'Y': return 19;    case '6': return 27;
        case 'G': return 4;     case 'Q': return 12;    case 'Z': return 20;    case '7': return 28;
        case 'H': return 5;     case 'R': return 13;    case '0': return 21;    case '8': return 29;
        case 'J': return 6;     case 'S': return 14;    case '1': return 22;    case '9': return 30;
        case 'K': return 7;     case 'T': return 15;    case '2': return 23;    case '!': return 31;
    }

    return -1;
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Converts PSX Doom password character index to a character.
// Returns 'NUL' if there is no valid conversion.
//------------------------------------------------------------------------------------------------------------------------------------------
static char pwCharIndexToChar(const int32_t pwCharIdx) noexcept {
    switch (pwCharIdx) {
        case 0: return 'B';     case 8:  return 'L';    case 16: return 'V';    case 24: return '3';
        case 1: return 'C';     case 9:  return 'M';    case 17: return 'W';    case 25: return '4';
        case 2: return 'D';     case 10: return 'N';    case 18: return 'X';    case 26: return '5';
        case 3: return 'F';     case 11: return 'P';    case 19: return 'Y';    case 27: return '6';
        case 4: return 'G';     case 12: return 'Q';    case 20: return 'Z';    case 28: return '7';
        case 5: return 'H';     case 13: return 'R';    case 21: return '0';    case 29: return '8';
        case 6: return 'J';     case 14: return 'S';    case 22: return '1';    case 30: return '9';
        case 7: return 'K';     case 15: return 'T';    case 23: return '2';    case 31: return '!';
    }

    return 0;
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Handle the loading of an entry in the .ini prefs file
//------------------------------------------------------------------------------------------------------------------------------------------
static void loadPrefsFileIniEntry(const IniUtils::IniEntry& entry) noexcept {
    if (entry.key == "soundVol") {
        gSoundVol = std::clamp(entry.value.tryGetAsInt(gSoundVol), VOLUME_MIN, VOLUME_MAX);
    } 
    else if (entry.key == "musicVol") {
        gMusicVol = std::clamp(entry.value.tryGetAsInt(gMusicVol), VOLUME_MIN, VOLUME_MAX);
    }
    else if (Password* const pPassword = getPasswordForPrefsFieldName(entry.key.c_str())) {
        // Read the password field up to the password length
        std::memset(pPassword, 0, sizeof(Password));
        std::memcpy(pPassword->pwChars, entry.value.strValue.c_str(), std::min(entry.value.strValue.length(), (size_t) PASSWORD_LEN));

        // Sanitize the password chars: set unrecognised ones to null and uppercase everything
        for (char& c : pPassword->pwChars) {
            c = (char) std::toupper(c);
            c = (charToPwCharIndex(c) >= 0) ? c : 0;
        }
    }
    else if (entry.key == "turnSpeedPercentMultiplier") {
        gTurnSpeedMult100[0] = std::clamp(entry.value.tryGetAsInt(gTurnSpeedMult100[0]), TURN_SPEED_MULT_MIN, TURN_SPEED_MULT_MAX);
    }
#if defined(__XBOX__)
    else if (entry.key == "turnSpeedPercentMultiplier2") {
        gTurnSpeedMult100[1] = std::clamp(entry.value.tryGetAsInt(gTurnSpeedMult100[1]), TURN_SPEED_MULT_MIN, TURN_SPEED_MULT_MAX);
    }
    else if (entry.key == "statDisplayMode2") {
        gStatDisplayMode[1] = (StatDisplayMode) entry.value.tryGetAsInt((int32_t) gStatDisplayMode[1]);
        gStatDisplayMode[1] = std::clamp(gStatDisplayMode[1], StatDisplayMode::None, StatDisplayMode::KillsSecretsAndItems);
    }
#endif
    else if (entry.key == "alwaysRun") {
        gbAlwaysRun[0] = entry.value.tryGetAsBool(gbAlwaysRun[0]);
    }
#if defined(__XBOX__)
    else if (entry.key == "alwaysRun2") {
        gbAlwaysRun[1] = entry.value.tryGetAsBool(gbAlwaysRun[1]);
    }
#endif
    else if (entry.key == "uncapFramerate") {
        gbUncapFramerate = entry.value.tryGetAsBool(gbUncapFramerate);
    }
    else if (entry.key == "statDisplayMode") {
        gStatDisplayMode[0] = (StatDisplayMode) entry.value.tryGetAsInt((int32_t) gStatDisplayMode[0]);
        gStatDisplayMode[0] = std::clamp(gStatDisplayMode[0], StatDisplayMode::None, StatDisplayMode::KillsSecretsAndItems);  // Ensure it's in range
    }
    else if (entry.key == "startupWithVulkanRenderer") {
        gbStartupWithVulkanRenderer = entry.value.tryGetAsBool(gbStartupWithVulkanRenderer);
    }
#if defined(__XBOX__)
    else if (entry.key == "playerColour") {
        gPlayerColour[0] = entry.value.tryGetAsInt(gPlayerColour[0]);
    }
    else if (entry.key == "playerColour2") {
        gPlayerColour[1] = entry.value.tryGetAsInt(gPlayerColour[1]);
    }
    else if (entry.key == "ssgStyle2") {
        gSsgStyle[1] = entry.value.tryGetAsInt(gSsgStyle[1]);
    }
    else if (entry.key == "ssgStyle") {
        // Not range checked against the styles here, deliberately: this file is read before the game knows which
        // edition it is or which sets the launcher wrote out, so what is reachable is not yet known.
        // 'SsgStyle::apply' falls back to the game's own set for anything it cannot honour.
        gSsgStyle[0] = entry.value.tryGetAsInt(gSsgStyle[0]);
    }
#endif
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Set all preferences to the defaults
//------------------------------------------------------------------------------------------------------------------------------------------
void setToDefaults() noexcept {
    // Note: make sound volume 85 by default (Final Doom volume level) to make the music pop a bit more
    gSoundVol = 85;
    gMusicVol = 100;

    // Password is empty by default
    std::memset(&gLastPassword_Doom, 0, sizeof(gLastPassword_Doom));
    std::memset(&gLastPassword_FDoom, 0, sizeof(gLastPassword_FDoom));
    std::memset(&gLastPassword_GecMe, 0, sizeof(gLastPassword_GecMe));

    // Turn speed is normal by default, auto-run off and no stat display
    for (int32_t i = 0; i < Controls::MAX_LOCAL_PLAYERS; ++i) {
        gTurnSpeedMult100[i] = 100;
    }
    for (int32_t i = 0; i < Controls::MAX_LOCAL_PLAYERS; ++i) {
        gbAlwaysRun[i] = false;
        gStatDisplayMode[i] = StatDisplayMode::None;
    }
    gbUncapFramerate = true;
    gStatDisplayMode[0] = StatDisplayMode::None;

    // Prefer the Vulkan renderer by default
    gbStartupWithVulkanRenderer = true;

#if defined(__XBOX__)
    // The super shotgun the game came with.
    //
    // Negative rather than a real style, because which style that is depends on which game is running and that is not
    // known here. 'SsgStyle::apply' reads anything it cannot honour as "use the game's own", which this is.
    // Both players keep whatever the game came with, and are both green - which is what PSX Doom has always done.
    // Negative rather than a real style because which one that is depends on the game, and that is not known here.
    for (int32_t i = 0; i < Controls::MAX_LOCAL_PLAYERS; ++i) {
        gSsgStyle[i] = -1;
        gPlayerColour[i] = 0;   // PlayerColour::GREEN
    }
#endif
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Load the player preferences from the preferences file.
// If the file does not exist then the preferences are defaulted.
//------------------------------------------------------------------------------------------------------------------------------------------
void load() noexcept {
    // Firstly set everything to the defaults and determine whether we use a prefs file found in the current working directory.
    // We only do that if the .ini file is found there on launch!
    setToDefaults();
    gbUseWorkingDirPrefsFile = FileUtils::fileExists(PREFS_FILE_NAME);

    // Read the .ini file if it exists, otherwise stop here
    const std::string prefsFilePath = getPrefsFilePath();

    if (!FileUtils::fileExists(prefsFilePath.c_str()))
        return;

    const FileData prefsFileData = FileUtils::getContentsOfFile(prefsFilePath.c_str(), 1, std::byte(0));
    IniUtils::parseIniFromString((const char*) prefsFileData.bytes.get(), prefsFileData.size - 1, loadPrefsFileIniEntry);

    #if defined(__XBOX__)
    {
        char msg[128];
        std::snprintf(msg, sizeof(msg), "prefs: read - ssg %d/%d colour %d/%d",
            (int) gSsgStyle[0], (int) gSsgStyle[1], (int) gPlayerColour[0], (int) gPlayerColour[1]);
        xbLog(msg);
    }
    #endif
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Save the player preferences the preferences file
//------------------------------------------------------------------------------------------------------------------------------------------
void save() noexcept {
    const std::string prefsFilePath = getPrefsFilePath();

#if defined(__XBOX__)
    // Built into a buffer and written with the Win32 API, rather than with 'fopen'.
    //
    // The preferences file was never once written on this console. It asked 'fopen' for mode "wt" - the 't' is a
    // Microsoft extension rather than ISO C, and this platform's C library will not accept it - so the open failed
    // every time and 'save' returned having done nothing. Nothing persisted: not the colours, not the super shotgun,
    // not turn speed, autorun or the stat display, and not the sound volumes either. It looked like a settings bug
    // because settings were the only thing anyone went looking for.
    //
    // 'CreateFileA' is what the launcher writes its own settings with and what the boot log is written with, and both
    // of those files are demonstrably there. So this is written the same way rather than by guessing at which mode
    // string the library would have taken.
    std::string out;
    out.reserve(1024);

    {
        char line[256];

        #define PREF_LINE(...) do { std::snprintf(line, sizeof(line), __VA_ARGS__); out += line; } while (0)

        PREF_LINE("; WARNING: this file is auto-generated by PsyDoom, it may be overwritten at any time!\r\n");
        PREF_LINE("soundVol = %d\r\n", gSoundVol);
        PREF_LINE("musicVol = %d\r\n", gMusicVol);
        PREF_LINE("lastPassword_Doom = %s\r\n", getPasswordCString(gLastPassword_Doom).chars);
        PREF_LINE("lastPassword_FinalDoom = %s\r\n", getPasswordCString(gLastPassword_FDoom).chars);
        PREF_LINE("lastPassword_GecMe = %s\r\n", getPasswordCString(gLastPassword_GecMe).chars);
        PREF_LINE("turnSpeedPercentMultiplier = %d\r\n", gTurnSpeedMult100[0]);
        PREF_LINE("turnSpeedPercentMultiplier2 = %d\r\n", gTurnSpeedMult100[1]);
        PREF_LINE("alwaysRun = %d\r\n", (int) gbAlwaysRun[0]);
        PREF_LINE("alwaysRun2 = %d\r\n", (int) gbAlwaysRun[1]);
        PREF_LINE("uncapFramerate = %d\r\n", (int) gbUncapFramerate);
        PREF_LINE("statDisplayMode = %d\r\n", (int) gStatDisplayMode[0]);
        PREF_LINE("statDisplayMode2 = %d\r\n", (int) gStatDisplayMode[1]);
        PREF_LINE("startupWithVulkanRenderer = %d\r\n", (int) Video::isUsingVulkanRenderPath());
        PREF_LINE("ssgStyle = %d\r\n", (int) gSsgStyle[0]);
        PREF_LINE("ssgStyle2 = %d\r\n", (int) gSsgStyle[1]);
        PREF_LINE("playerColour = %d\r\n", (int) gPlayerColour[0]);
        PREF_LINE("playerColour2 = %d\r\n", (int) gPlayerColour[1]);

        #undef PREF_LINE
    }

    HANDLE const h = CreateFileA(
        prefsFilePath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr
    );

    if (h == INVALID_HANDLE_VALUE) {
        xbLog("prefs: could NOT open the preferences file for writing");
        return;
    }

    DWORD written = 0;
    const bool bOk = (WriteFile(h, out.data(), (DWORD) out.size(), &written, nullptr) && (written == out.size()));
    CloseHandle(h);

    {
        char msg[160];
        std::snprintf(msg, sizeof(msg), "prefs: %s %u bytes - ssg %d/%d colour %d/%d",
            bOk ? "wrote" : "FAILED writing", (unsigned) written,
            (int) gSsgStyle[0], (int) gSsgStyle[1], (int) gPlayerColour[0], (int) gPlayerColour[1]);
        xbLog(msg);
    }
#else
    // Open up the prefs file
    std::FILE* const pFile = std::fopen(prefsFilePath.c_str(), "wt");

    if (!pFile)
        return;

    // Write out the preferences
    std::fprintf(pFile, "; WARNING: this file is auto-generated by PsyDoom, it may be overwritten at any time!\n");
    std::fprintf(pFile, "soundVol = %d\n", gSoundVol);
    std::fprintf(pFile, "musicVol = %d\n", gMusicVol);
    std::fprintf(pFile, "lastPassword_Doom = %s\n", getPasswordCString(gLastPassword_Doom).chars);
    std::fprintf(pFile, "lastPassword_FinalDoom = %s\n", getPasswordCString(gLastPassword_FDoom).chars);
    std::fprintf(pFile, "lastPassword_GecMe = %s\n", getPasswordCString(gLastPassword_GecMe).chars);
    std::fprintf(pFile, "turnSpeedPercentMultiplier = %d\n", gTurnSpeedMult100[0]);
    std::fprintf(pFile, "alwaysRun = %d\n", (int) gbAlwaysRun[0]);
    std::fprintf(pFile, "uncapFramerate = %d\n", (int) gbUncapFramerate);
    std::fprintf(pFile, "statDisplayMode = %d\n", (int) gStatDisplayMode[0]);
    std::fprintf(pFile, "startupWithVulkanRenderer = %d\n", (int) Video::isUsingVulkanRenderPath());

    // Flush and close to finish up
    std::fflush(pFile);
    std::fclose(pFile);
#endif
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Get the turn speed multiplier expressed as a floating point fraction where 1.0 is 100%
//------------------------------------------------------------------------------------------------------------------------------------------
float getTurnSpeedMultiplier(const int32_t playerIdx) noexcept {
    const int32_t idx = ((playerIdx >= 0) && (playerIdx < Controls::MAX_LOCAL_PLAYERS)) ? playerIdx : 0;
    return (float) gTurnSpeedMult100[idx] / 100.0f;
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Apply the current sound and music preferences to the options sound and music preferences
//------------------------------------------------------------------------------------------------------------------------------------------
void pushSoundAndMusicPrefs() noexcept {
    gOptionsSndVol = gSoundVol;
    gOptionsMusVol = gMusicVol;
    gCdMusicVol = (gMusicVol * PSXSPU_MAX_CD_VOL) / S_MAX_VOL;      // Calculation copied from the options menu volume adjust
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Remember the current sound and music preferences stored in the options menu
//------------------------------------------------------------------------------------------------------------------------------------------
void pullSoundAndMusicPrefs() noexcept {
    gSoundVol = gOptionsSndVol;
    gMusicVol = gOptionsMusVol;
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Apply the saved last password to the password system
//------------------------------------------------------------------------------------------------------------------------------------------
void pushLastPassword() noexcept {
    // Clear the current password
    gNumPasswordCharsEntered = 0;
    std::memset(gPasswordCharBuffer, 0, sizeof(gPasswordCharBuffer));

    // Add in password characters to the buffer until we encounter an invalid one
    ASSERT(Game::gConstants.pLastPasswordField);
    Password& lastPassword = *Game::gConstants.pLastPasswordField;

    for (const char pwChar : lastPassword.pwChars) {
        const int32_t pwCharIdx = charToPwCharIndex(pwChar);

        if (pwCharIdx < 0)
            break;

        gPasswordCharBuffer[gNumPasswordCharsEntered] = (uint8_t) pwCharIdx;
        gNumPasswordCharsEntered++;
    }
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Remember current password input in the password system, so that it may be saved later to player preferences
//------------------------------------------------------------------------------------------------------------------------------------------
void pullLastPassword() noexcept {
    ASSERT(Game::gConstants.pLastPasswordField);
    Password& lastPassword = *Game::gConstants.pLastPasswordField;
    std::memset(&lastPassword, 0, sizeof(lastPassword));

    for (int32_t i = 0; i < gNumPasswordCharsEntered; ++i) {
        lastPassword.pwChars[i] = pwCharIndexToChar(gPasswordCharBuffer[i]);
    }
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Should we startup using the Vulkan renderer where possible?
//------------------------------------------------------------------------------------------------------------------------------------------
bool shouldStartupWithVulkanRenderer() noexcept {
    return gbStartupWithVulkanRenderer;
}

END_NAMESPACE(PlayerPrefs)
