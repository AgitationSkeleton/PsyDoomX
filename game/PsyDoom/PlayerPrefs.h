#pragma once

#include "Controls.h"
#include "Macros.h"

#include <cstdint>

// Stat display modes
enum class StatDisplayMode : int32_t {
    None = 0,
    Kills = 1,
    KillsAndSecrets = 2,
    KillsSecretsAndItems = 3
};

BEGIN_NAMESPACE(PlayerPrefs)

// Length of PSX Doom passwords
constexpr int32_t PASSWORD_LEN = 10;

// Holds the ASCII readable characters for a game password
struct Password {
    char pwChars[PASSWORD_LEN];
};

// Minimum and maximum values for sound and music volume
constexpr int32_t VOLUME_MIN = 0;
constexpr int32_t VOLUME_MAX = 100;

// Minimum and maximum values for the turn speed multiplier (expressed in whole percentages, 0-500%)
constexpr int32_t TURN_SPEED_MULT_MIN = 1;
constexpr int32_t TURN_SPEED_MULT_MAX = 500;

extern int32_t              gTurnSpeedMult100[Controls::MAX_LOCAL_PLAYERS];
extern bool                 gbAlwaysRun[Controls::MAX_LOCAL_PLAYERS];
extern bool                 gbUncapFramerate;
extern StatDisplayMode      gStatDisplayMode[Controls::MAX_LOCAL_PLAYERS];
extern Password             gLastPassword_Doom;
extern Password             gLastPassword_FDoom;
extern Password             gLastPassword_GecMe;

#if defined(__XBOX__)
    // Which edition's super shotgun sprites each player carries: a 'SsgStyle::Style'.
    //
    // Per player, like turn speed and autorun, because in splitscreen the two are separate people. Applied when each
    // view is drawn rather than once, since both share one sprite list. Kept as a plain int rather than the enum so
    // this header does not have to pull the module in. A style the running game cannot reach falls back to its own,
    // which 'SsgStyle::applyForPlayer' handles.
    extern int32_t          gSsgStyle[Controls::MAX_LOCAL_PLAYERS];

    // Which colour each player is drawn in: a 'PlayerColour::Colour'. Not portable - see 'PlayerColour.h'.
    extern int32_t          gPlayerColour[Controls::MAX_LOCAL_PLAYERS];
#endif

void setToDefaults() noexcept;
void load() noexcept;
void save() noexcept;
float getTurnSpeedMultiplier(const int32_t playerIdx) noexcept;
void pushSoundAndMusicPrefs() noexcept;
void pullSoundAndMusicPrefs() noexcept;
void pushLastPassword() noexcept;
void pullLastPassword() noexcept;
bool shouldStartupWithVulkanRenderer() noexcept;

END_NAMESPACE(PlayerPrefs)
