//------------------------------------------------------------------------------------------------------------------------------------------
// This is an entirely new menu added for PsyDoom.
// It provides extra options for turn sensitivity, autorun and renderer etc.
// It is not available in multiplayer, similar to other nested menus in the options screen.
//------------------------------------------------------------------------------------------------------------------------------------------
#if PSYDOOM_MODS

#include "xoptions_main.h"

#include "Doom/Base/i_main.h"
#include "Doom/Base/i_misc.h"
#include "Doom/Base/s_sound.h"
#include "Doom/Base/sounds.h"
#include "Doom/d_main.h"
#include "Doom/Game/g_game.h"
#include "Doom/Game/p_tick.h"
#include "Doom/Renderer/r_data.h"
#include "m_main.h"
#include "o_main.h"
#include "PsyDoom/Game.h"
#include "PsyDoom/PlayerPrefs.h"
#include "PsyDoom/PlayerColour.h"
#include "PsyDoom/Splitscreen.h"
#include "PsyDoom/SsgStyle.h"
#include "PsyDoom/Utils.h"
#include "PsyDoom/Video.h"
#if PSYDOOM_VULKAN_RENDERER
#include "PsyDoom/Vulkan/VRenderer.h"
#endif

#include <cstdio>

// The available menu items
enum MenuItem : int32_t {
    menu_turn_speed,
    menu_always_run,
    menu_stat_display,
#if !defined(__XBOX__)
    // Not offered on Xbox.
    //
    // With the cap on, 'I_DrawPresent' spins until enough vblanks have passed, which pins the frame to 33.3ms and
    // makes every optimisation below that threshold worth nothing. There is no frame rate here to protect - the game
    // does not reach the cap - so the setting can only make things worse, and offering it invites exactly that.
    menu_uncapped_framerate,
#endif
#if defined(__XBOX__)
    // How the screen is divided in a two player game. Only offered in one, since there is nothing to divide otherwise.
    menu_splitscreen_layout,

    // What colour this player is drawn in. Also multiplayer only: it exists to tell two players apart.
    menu_player_colour,

    // Which edition's super shotgun to carry
    menu_ssg_style,
#endif
#if PSYDOOM_VULKAN_RENDERER
    menu_renderer,
#endif
    menu_exit,
    num_menu_items
};

#if defined(__XBOX__)
//------------------------------------------------------------------------------------------------------------------------------------------
// Which rows this menu is showing, and where they sit.
//
// The list used to be fixed, with every row at a hardcoded height. Two of them have no meaning outside a multiplayer
// game - how to divide the screen, and what colour to be, when there is only ever one player and one whole screen - so
// the list is now worked out when the menu is entered. Rows below a hidden one move up to fill the gap, and the cursor
// moves between what is actually on screen rather than through slots that are not there.
//------------------------------------------------------------------------------------------------------------------------------------------
static MenuItem gVisibleRows[num_menu_items];
static int16_t  gVisibleRowY[num_menu_items];
static int32_t  gNumVisibleRows;

static bool XOptions_IsRowVisible(const MenuItem item) noexcept {
    switch (item) {
        // Nothing to divide and nobody to be told apart from in a single player game
        case menu_splitscreen_layout:   return (gNetGame != gt_single);
        case menu_player_colour:        return ((gNetGame != gt_single) && PlayerColour::haveChoice());
        default:                        return true;
    }
}

static void XOptions_BuildRows() noexcept {
    static constexpr int16_t FIRST_ROW_Y  = 50;
    static constexpr int16_t ROW_SPACING  = 23;
    static constexpr int16_t SLIDER_EXTRA = 15;     // The turn speed row carries a slider under its label
    static constexpr int16_t EXIT_Y       = 205;    // Where 'Back' has always been, and where it stays

    gNumVisibleRows = 0;
    int16_t y = FIRST_ROW_Y;

    for (int32_t i = 0; i < num_menu_items; ++i) {
        const MenuItem item = (MenuItem) i;

        if (!XOptions_IsRowVisible(item))
            continue;

        // 'Back' is pinned to the bottom rather than following the settings down the screen.
        //
        // Following them is what pushed it off: with two more rows than this menu used to have, and a gap of its own on
        // top of that, it landed below the status bar. Pinning it also means the way out does not move about depending
        // on how many settings the game is showing.
        if (item == menu_exit) {
            gVisibleRows[gNumVisibleRows] = item;
            gVisibleRowY[gNumVisibleRows] = EXIT_Y;
            gNumVisibleRows++;
            continue;
        }

        if ((gNumVisibleRows > 0) && (gVisibleRows[gNumVisibleRows - 1] == menu_turn_speed)) {
            y = (int16_t)(y + SLIDER_EXTRA);
        }

        gVisibleRows[gNumVisibleRows] = item;
        gVisibleRowY[gNumVisibleRows] = y;
        gNumVisibleRows++;
        y = (int16_t)(y + ROW_SPACING);
    }
}

// Which menu item the cursor is on, and where that row is drawn
static MenuItem XOptions_SelectedItem() noexcept {
    const int32_t pos = gCursorPos[gCurPlayerIndex];
    return ((pos >= 0) && (pos < gNumVisibleRows)) ? gVisibleRows[pos] : menu_exit;
}
#endif  // #if defined(__XBOX__)

//------------------------------------------------------------------------------------------------------------------------------------------
// Draw the cursor at the specified position
//------------------------------------------------------------------------------------------------------------------------------------------
static void DrawCursor(const int16_t cursorX, const int16_t cursorY) noexcept {
    I_DrawSprite(
        gTex_STATUS.texPageId,
        Game::getTexClut_STATUS(),
        (int16_t) cursorX - 24,
        (int16_t) cursorY - 2,
        (int16_t)(gTex_STATUS.texPageCoordX + M_SKULL_TEX_U + (uint8_t) gCursorFrame * M_SKULL_W),
        (int16_t)(gTex_STATUS.texPageCoordY + M_SKULL_TEX_V),
        M_SKULL_W,
        M_SKULL_H
    );
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Initializes the menu
//------------------------------------------------------------------------------------------------------------------------------------------
void XOptions_Init() noexcept {
    S_StartSound(nullptr, sfx_pistol);

    // Which rows this game is showing. Done on entry rather than once at startup, because whether it is a multiplayer
    // game is not known until one has been started.
    #if defined(__XBOX__)
        XOptions_BuildRows();
    #endif

    // Initialize cursor position and vblanks until move
    gCursorFrame = 0;
    gCursorPos[gCurPlayerIndex] = 0;
    gVBlanksUntilMenuMove[gCurPlayerIndex] = 0;
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Shuts down the menu
//------------------------------------------------------------------------------------------------------------------------------------------
void XOptions_Shutdown([[maybe_unused]] const gameaction_t exitAction) noexcept {
    gCursorPos[gCurPlayerIndex] = 0;
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Runs update logic for the menu: does menu controls
//------------------------------------------------------------------------------------------------------------------------------------------
gameaction_t XOptions_Update() noexcept {
    // PsyDoom: in all UIs tick only if vblanks are registered as elapsed; this restricts the code to ticking at 30 Hz for NTSC
    const uint32_t playerIdx = gCurPlayerIndex;

    if (gPlayersElapsedVBlanks[playerIdx] <= 0) {
        gbKeepInputEvents = true;   // Don't consume 'key pressed' etc. events yet, not ticking...
        return ga_nothing;
    }

    // Animate the skull cursor
    if ((gGameTic > gPrevGameTic) && ((gGameTic & 3) == 0)) {
        gCursorFrame ^= 1;
    }

    // Gather menu inputs and exit if the back button has just been pressed
    const TickInputs& inputs = gTickInputs[playerIdx];
    const TickInputs& oldInputs = gOldTickInputs[playerIdx];

    const bool bMenuBack = (inputs.fMenuBack() && (!oldInputs.fMenuBack()));
    const bool bMenuOk = (inputs.fMenuOk() && (!oldInputs.fMenuOk()));
    const bool bMenuUp = inputs.fMenuUp();
    const bool bMenuDown = inputs.fMenuDown();
    const bool bMenuLeft = inputs.fMenuLeft();
    const bool bMenuRight = inputs.fMenuRight();
    const bool bMenuMove = (bMenuUp || bMenuDown || bMenuLeft || bMenuRight);

    if (bMenuBack) {
        S_StartSound(nullptr, sfx_pistol);
        return ga_exit;
    }

    // Check for up/down movement
    if (!bMenuMove) {
        // If there are no direction buttons pressed then the next move is allowed instantly
        gVBlanksUntilMenuMove[playerIdx] = 0;
    } else {
        // Direction buttons pressed or held down, check to see if we can move up/down now
        gVBlanksUntilMenuMove[playerIdx] -= gPlayersElapsedVBlanks[playerIdx];

        if (gVBlanksUntilMenuMove[playerIdx] <= 0) {
            gVBlanksUntilMenuMove[playerIdx] = 15;

            if (bMenuDown) {
                gCursorPos[playerIdx]++;

                #if defined(__XBOX__)
                    if (gCursorPos[playerIdx] >= gNumVisibleRows) {
                        gCursorPos[playerIdx] = 0;
                    }
                #else
                    if (gCursorPos[playerIdx] >= num_menu_items) {
                        gCursorPos[playerIdx] = 0;
                    }
                #endif

                S_StartSound(nullptr, sfx_pstop);
            }
            else if (bMenuUp) {
                gCursorPos[playerIdx]--;

                #if defined(__XBOX__)
                    if (gCursorPos[playerIdx] < 0) {
                        gCursorPos[playerIdx] = gNumVisibleRows - 1;
                    }
                #else
                    if (gCursorPos[playerIdx] < 0) {
                        gCursorPos[playerIdx] = num_menu_items - 1;
                    }
                #endif

                S_StartSound(nullptr, sfx_pstop);
            }
        }
    }

    // Handle option actions and adjustment.
    //
    // Keyed on which row the cursor is actually on rather than on its index: with rows hidden the two are not the same,
    // and using the index would act on whichever setting used to sit at that height.
    #if defined(__XBOX__)
        const MenuItem selectedItem = XOptions_SelectedItem();
    #else
        const MenuItem selectedItem = (MenuItem) gCursorPos[playerIdx];
    #endif

    switch (selectedItem) {
        // Adjust turn speed
        case menu_turn_speed: {
            // Only process audio updates for this player
            if (bMenuRight) {
                PlayerPrefs::gTurnSpeedMult100[gCurPlayerIndex]++;

                if (PlayerPrefs::gTurnSpeedMult100[gCurPlayerIndex] > PlayerPrefs::TURN_SPEED_MULT_MAX) {
                    PlayerPrefs::gTurnSpeedMult100[gCurPlayerIndex] = PlayerPrefs::TURN_SPEED_MULT_MAX;
                } else {
                    if ((PlayerPrefs::gTurnSpeedMult100[gCurPlayerIndex] / 4) & 1) {
                        S_StartSound(nullptr, sfx_stnmov);
                    }
                }
            }
            else if (bMenuLeft) {
                if (PlayerPrefs::gTurnSpeedMult100[gCurPlayerIndex] > PlayerPrefs::TURN_SPEED_MULT_MIN) {
                    PlayerPrefs::gTurnSpeedMult100[gCurPlayerIndex]--;;

                    if ((PlayerPrefs::gTurnSpeedMult100[gCurPlayerIndex] / 4) & 1) {
                        S_StartSound(nullptr, sfx_stnmov);
                    }
                } else {
                    PlayerPrefs::gTurnSpeedMult100[gCurPlayerIndex] = PlayerPrefs::TURN_SPEED_MULT_MIN;
                }
            }
        }   break;

        // Turn on/off always run
        case menu_always_run: {
            if (bMenuLeft && PlayerPrefs::gbAlwaysRun[gCurPlayerIndex]) {
                PlayerPrefs::gbAlwaysRun[gCurPlayerIndex] = false;
                PlayerPrefs::save();
                S_StartSound(nullptr, sfx_swtchx);
            }
            else if (bMenuRight && (!PlayerPrefs::gbAlwaysRun[gCurPlayerIndex])) {
                PlayerPrefs::gbAlwaysRun[gCurPlayerIndex] = true;
                PlayerPrefs::save();
                S_StartSound(nullptr, sfx_swtchx);
            }
        }   break;

        // Stat display setting
        case menu_stat_display: {
            if (bMenuLeft && (!oldInputs.fMenuLeft()) && (PlayerPrefs::gStatDisplayMode[gCurPlayerIndex] > StatDisplayMode::None)) {
                PlayerPrefs::gStatDisplayMode[gCurPlayerIndex] = (StatDisplayMode)((int32_t) PlayerPrefs::gStatDisplayMode[gCurPlayerIndex] - 1);
                S_StartSound(nullptr, sfx_swtchx);
            }
            else if (bMenuRight && (!oldInputs.fMenuRight()) && (PlayerPrefs::gStatDisplayMode[gCurPlayerIndex] < StatDisplayMode::KillsSecretsAndItems)) {
                PlayerPrefs::gStatDisplayMode[gCurPlayerIndex] = (StatDisplayMode)((int32_t) PlayerPrefs::gStatDisplayMode[gCurPlayerIndex] + 1);
                S_StartSound(nullptr, sfx_swtchx);
            }
        }   break;

    #if defined(__XBOX__)
        // Side by side, or one above the other
        case menu_splitscreen_layout: {
            const Splitscreen::Layout layout = Splitscreen::getLayout();

            if (bMenuLeft && (layout != Splitscreen::Layout::SideBySide)) {
                Splitscreen::setLayout(Splitscreen::Layout::SideBySide);
                S_StartSound(nullptr, sfx_swtchx);
            }
            else if (bMenuRight && (layout != Splitscreen::Layout::TopAndBottom)) {
                Splitscreen::setLayout(Splitscreen::Layout::TopAndBottom);
                S_StartSound(nullptr, sfx_swtchx);
            }
        }   break;

        // Which edition's super shotgun sprites to carry.
        //
        // The change is applied here and now rather than on the next start: the borrowed sets are already loaded as
        // sprites of their own, so this only points the super shotgun's frames at a different set of lumps. A player
        // holding the gun sees it change in their hands.
        // What colour this player is drawn in
        case menu_player_colour: {
            const bool bLeftEdge = (bMenuLeft && (!oldInputs.fMenuLeft()));
            const bool bRightEdge = (bMenuRight && (!oldInputs.fMenuRight()));

            if (bLeftEdge || bRightEdge) {
                const PlayerColour::Colour current = PlayerColour::forPlayer(gCurPlayerIndex);
                PlayerPrefs::gPlayerColour[gCurPlayerIndex] = (int32_t) PlayerColour::nextAvailable(current, (bRightEdge) ? 1 : -1);

                // Written now rather than on the way out.
                //
                // The only save was at the end of 'psx_main', which is reached by quitting through the menus - and a
                // console gets switched off instead. The launcher already writes its own settings as they change for
                // exactly this reason.
                PlayerPrefs::save();
                S_StartSound(nullptr, sfx_swtchx);
            }
        }   break;

        case menu_ssg_style: {
            const bool bLeftEdge = (bMenuLeft && (!oldInputs.fMenuLeft()));
            const bool bRightEdge = (bMenuRight && (!oldInputs.fMenuRight()));

            if (bLeftEdge || bRightEdge) {
                if (SsgStyle::haveChoice()) {
                    const SsgStyle::Style current = (SsgStyle::Style) PlayerPrefs::gSsgStyle[gCurPlayerIndex];
                    const SsgStyle::Style resolved = (SsgStyle::isAvailable(current)) ? current : SsgStyle::nativeStyle();

                    PlayerPrefs::gSsgStyle[gCurPlayerIndex] = (int32_t) SsgStyle::nextAvailable(resolved, (bRightEdge) ? 1 : -1);
                    SsgStyle::applyForPlayer(gCurPlayerIndex);
                    PlayerPrefs::save();    // As it changes, not on the way out - see the note on the colour row
                    S_StartSound(nullptr, sfx_swtchx);
                } else {
                    // The same sound the renderer row makes when there is no other renderer to switch to
                    S_StartSound(nullptr, sfx_itemup);
                }
            }
        }   break;
    #endif

    #if !defined(__XBOX__)
        // Turn on/off uncapped framerate
        case menu_uncapped_framerate: {
            if (bMenuLeft && PlayerPrefs::gbUncapFramerate) {
                PlayerPrefs::gbUncapFramerate = false;
                S_StartSound(nullptr, sfx_swtchx);
            }
            else if (bMenuRight && (!PlayerPrefs::gbUncapFramerate)) {
                PlayerPrefs::gbUncapFramerate = true;
                S_StartSound(nullptr, sfx_swtchx);
            }
        }   break;
    #endif

    #if PSYDOOM_VULKAN_RENDERER
        // Renderer toggle
        case menu_renderer: {
            const bool bCanSwitchRenderers = (Video::gBackendType == Video::BackendType::Vulkan);

            if (bCanSwitchRenderers) {
                if (bMenuLeft && (!Video::isUsingVulkanRenderPath())) {
                    VRenderer::switchToMainVulkanRenderPath();
                    S_StartSound(nullptr, sfx_swtchx);
                }
                else if (bMenuRight && Video::isUsingVulkanRenderPath()) {
                    VRenderer::switchToPsxRenderPath();
                    S_StartSound(nullptr, sfx_swtchx);
                }
            }

            // If renderer switch is not possible and an attempt was made to do so then play this sound
            if (!bCanSwitchRenderers) {
                if ((bMenuLeft && (!oldInputs.fMenuLeft())) || (bMenuRight && (!oldInputs.fMenuRight()))) {
                    S_StartSound(nullptr, sfx_itemup);
                }
            }
        }   break;
    #endif  // #if PSYDOOM_VULKAN_RENDERER

        // Exit to the options menu
        case menu_exit: {
            if (bMenuOk) {
                S_StartSound(nullptr, sfx_pistol);
                return ga_exit;
            }
        } break;

        default:
            break;
    }

    return ga_nothing;
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Draws the menu
//------------------------------------------------------------------------------------------------------------------------------------------
void XOptions_Draw() noexcept {
    // Increment the frame count for the texture cache and draw the background
    I_IncDrawnFrameCount();
    Utils::onBeginUIDrawing();
    O_DrawBackground(gTex_OptionsBg, Game::getTexClut_OptionsBg(), 128, 128, 128);

    // Don't do any rendering if we are about to exit the menu
    if (gGameAction == ga_nothing) {
        // Menu title
        I_DrawString(-1, 20, "Extra Options");

        int16_t cursorX = 62;
        int16_t cursorY = 50;

#if defined(__XBOX__)
        // Draw whichever rows this game is showing, at the heights worked out when the menu was entered.
        //
        // Everything below used to be a run of hardcoded heights, which meant a row could only ever be hidden by
        // leaving a hole where it was. See 'XOptions_BuildRows'.
        for (int32_t rowIdx = 0; rowIdx < gNumVisibleRows; ++rowIdx) {
            const MenuItem item = gVisibleRows[rowIdx];
            const int16_t rowY = gVisibleRowY[rowIdx];

            if (gCursorPos[gCurPlayerIndex] == rowIdx) {
                cursorY = rowY;
            }

            switch (item) {
                case menu_turn_speed: {
                    const int32_t turnSpeed = PlayerPrefs::gTurnSpeedMult100[gCurPlayerIndex];
                    char turnSpeedLabel[32];
                    std::snprintf(turnSpeedLabel, sizeof(turnSpeedLabel), "Turn Speed %d.%02d", turnSpeed / 100, turnSpeed % 100);
                    I_DrawString(62, rowY, turnSpeedLabel);

                    // The slider background and its handle, which sit under the label
                    I_DrawSprite(
                        gTex_STATUS.texPageId, Game::getTexClut_STATUS(),
                        (int16_t)(62 + 13), (int16_t)(rowY + 20),
                        (int16_t)(gTex_STATUS.texPageCoordX + 0), (int16_t)(gTex_STATUS.texPageCoordY + 184),
                        108, 11
                    );

                    const int16_t sliderVal = (int16_t)(turnSpeed / 5);

                    I_DrawSprite(
                        gTex_STATUS.texPageId, Game::getTexClut_STATUS(),
                        (int16_t)(62 + 14 + sliderVal), (int16_t)(rowY + 20),
                        (int16_t)(gTex_STATUS.texPageCoordX + 108), (int16_t)(gTex_STATUS.texPageCoordY + 184),
                        6, 11
                    );
                }   break;

                case menu_always_run:
                    I_DrawString(62, rowY, (PlayerPrefs::gbAlwaysRun[gCurPlayerIndex]) ? "Always Run On" : "Always Run Off");
                    break;

                case menu_stat_display: {
                    const StatDisplayMode mode = PlayerPrefs::gStatDisplayMode[gCurPlayerIndex];
                    const char* statDisplayStr = "Stat Display Off";

                    if (mode >= StatDisplayMode::KillsSecretsAndItems) {
                        statDisplayStr = "Stat Display KSI";
                    } else if (mode >= StatDisplayMode::KillsAndSecrets) {
                        statDisplayStr = "Stat Display KS";
                    } else if (mode >= StatDisplayMode::Kills) {
                        statDisplayStr = "Stat Display K";
                    }

                    I_DrawString(62, rowY, statDisplayStr);
                }   break;

                case menu_splitscreen_layout:
                    I_DrawString(
                        62, rowY,
                        (Splitscreen::getLayout() == Splitscreen::Layout::SideBySide) ? "Split: Side By Side" : "Split: Top/Bottom"
                    );
                    break;

                case menu_player_colour: {
                    char label[32];
                    std::snprintf(label, sizeof(label), "Color %s", PlayerColour::displayName(PlayerColour::forPlayer(gCurPlayerIndex)));
                    I_DrawString(62, rowY, label);
                }   break;

                case menu_ssg_style: {
                    const SsgStyle::Style style = (SsgStyle::Style) PlayerPrefs::gSsgStyle[gCurPlayerIndex];
                    const SsgStyle::Style shown = (SsgStyle::isAvailable(style)) ? style : SsgStyle::nativeStyle();

                    char label[32];

                    // Says when there is nothing to choose. A row reading 'SSG Doom' on a console with only Doom on it
                    // would look like a setting that refuses to change.
                    if (SsgStyle::haveChoice()) {
                        std::snprintf(label, sizeof(label), "SSG Style %s", SsgStyle::displayName(shown));
                    } else {
                        std::snprintf(label, sizeof(label), "SSG Style Only %s", SsgStyle::displayName(shown));
                    }

                    I_DrawString(62, rowY, label);
                }   break;

                case menu_exit:
                    I_DrawString(62, rowY, "Back");
                    break;

                default:
                    break;
            }
        }
#else
        {
            const int16_t menuItemX = 62;
            const int16_t menuItemY = 50;

            int32_t turnSpeed = PlayerPrefs::gTurnSpeedMult100[gCurPlayerIndex];
            char turnSpeedLabel[32];

            std::snprintf(turnSpeedLabel, sizeof(turnSpeedLabel), "Turn Speed %d.%02d", turnSpeed / 100, turnSpeed % 100);
            I_DrawString(menuItemX, menuItemY, turnSpeedLabel);

            I_DrawSprite(
                gTex_STATUS.texPageId, Game::getTexClut_STATUS(),
                (int16_t)(menuItemX + 13), (int16_t)(menuItemY + 20),
                (int16_t)(gTex_STATUS.texPageCoordX + 0), (int16_t)(gTex_STATUS.texPageCoordY + 184),
                108, 11
            );

            const int16_t sliderVal = (int16_t)(PlayerPrefs::gTurnSpeedMult100[gCurPlayerIndex] / 5);

            I_DrawSprite(
                gTex_STATUS.texPageId, Game::getTexClut_STATUS(),
                (int16_t)(menuItemX + 14 + sliderVal), (int16_t)(menuItemY + 20),
                (int16_t)(gTex_STATUS.texPageCoordX + 108), (int16_t)(gTex_STATUS.texPageCoordY + 184),
                6, 11
            );
        }

        I_DrawString(62, 90, (PlayerPrefs::gbAlwaysRun[gCurPlayerIndex]) ? "Always Run On" : "Always Run Off");

        if (gCursorPos[gCurPlayerIndex] == menu_always_run) {
            cursorY = 90;
        }

        {
            const char* statDisplayStr = "Stat Display Off";

            if (PlayerPrefs::gStatDisplayMode[gCurPlayerIndex] >= StatDisplayMode::KillsSecretsAndItems) {
                statDisplayStr = "Stat Display KSI";
            } else if (PlayerPrefs::gStatDisplayMode[gCurPlayerIndex] >= StatDisplayMode::KillsAndSecrets) {
                statDisplayStr = "Stat Display KS";
            } else if (PlayerPrefs::gStatDisplayMode[gCurPlayerIndex] >= StatDisplayMode::Kills) {
                statDisplayStr = "Stat Display K";
            }

            I_DrawString(62, 115, statDisplayStr);
        }

        if (gCursorPos[gCurPlayerIndex] == menu_stat_display) {
            cursorY = 115;
        }

        I_DrawString(62, 140, (PlayerPrefs::gbUncapFramerate) ? "Uncapped FPS" : "Original FPS");

        if (gCursorPos[gCurPlayerIndex] == menu_uncapped_framerate) {
            cursorY = 140;
        }

        #if PSYDOOM_VULKAN_RENDERER
            const bool bIsUsingVulkan = Video::isUsingVulkanRenderPath();
            I_DrawString(62, 165, (bIsUsingVulkan) ? "Vulkan Renderer" : "Classic Renderer");

            if (gCursorPos[gCurPlayerIndex] == menu_renderer) {
                cursorY = 165;
            }
        #endif

        I_DrawString(62, 205, "Back");

        if (gCursorPos[gCurPlayerIndex] == menu_exit) {
            cursorY = 205;
        }
#endif

        // Draw the skull cursor
        DrawCursor(cursorX, cursorY);
    }

    // PsyDoom: draw any enabled performance counters
    #if PSYDOOM_MODS
        I_DrawEnabledPerfCounters();
    #endif

    // Finish up the frame
    I_SubmitGpuCmds();
    I_DrawPresent();
}

#endif  // #if PSYDOOM_MODS
