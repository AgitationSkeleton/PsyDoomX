#pragma once

#include "Doom/doomdef.h"

struct texture_t;

extern texture_t    gTex_OptionsBg;
extern int32_t      gOptionsSndVol;
extern int32_t      gOptionsMusVol;

#if PSYDOOM_MODS
    extern bool gbUnpauseAfterOptionsMenu;
#endif

void O_Init() noexcept;
void O_Shutdown(const gameaction_t exitAction) noexcept;
gameaction_t O_Control() noexcept;
void O_Drawer() noexcept;

#if defined(__XBOX__)
    // Splitscreen: both players' menus at once, each on their own pad and in their own half of the screen
    gameaction_t O_ControlSplit() noexcept;
    void O_DrawerSplit() noexcept;
#endif

void O_DrawBackground(
    texture_t& bgTex,
    const uint16_t bgTexClutId,
    const uint8_t colR,
    const uint8_t colG,
    const uint8_t colB
) noexcept;
