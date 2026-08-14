#pragma once

#include "Doom/doomdef.h"

// Describes 1 frame of a status bar face sprite
struct facesprite_t {
    uint8_t xPos;
    uint8_t yPos;
    uint8_t texU;
    uint8_t texV;
    uint8_t w;
    uint8_t h;
};

// Special face type
enum spclface_e : int32_t {
    f_none,         // Not a face - no special face
    f_normal,       // Regular face
    f_eyebrow,      // Eyebrow raised face
    f_faceleft,     // Damaged and turn face left
    f_faceright,    // Damaged and turn face right
    f_hurtbad,      // Super surpised look when receiving a lot of damage
    f_gotgat,       // Evil smile picking up a weapon
    f_mowdown,      // Grimmace while continously firing weapon
    NUMSPCLFACES
};

// State relating to flashing keycards on the status bar
struct sbflash_t {
    int16_t     active;     // Is the flash currently active?
    int16_t     doDraw;     // Are we currently drawing the keycard as part of the flash?
    int16_t     delay;      // Ticks until next draw/no-draw change
    int16_t     times;      // How many flashes are left
};

// Container for most status bar related state.
// PsyDoom: all 'bool' fields here were originally 'uint32_t', changed them to express meaning better.
struct stbar_t {
    uint32_t        face;                   // Index of the face sprite to currently use
    spclface_e      specialFace;            // What special face to do next
    bool            tryopen[NUMCARDS];      // Whether we are doing a keycard flash for each of the key types
    bool            gotgibbed;              // True if the player just got gibbed
    int32_t         gibframe;               // What frame of the gib animation is currently showing
    int32_t         gibframeTicsLeft;       // How many game ticks left in the current gib animation frame
    const char*     message;                // The current message to show on the status bar (string must be valid at all times)
    int32_t         messageTicsLeft;        // How many game ticks left to show the status bar message for
#if PSYDOOM_MODS
    char            alertMessage[32];       // PsyDoom: a message displayed near the center of the screen which is not interrupted by pickups
    int32_t         alertMessageTicsLeft;   // PsyDoom: how many tics left before the alert message is done displaying
#endif
};

// The number of face sprite definitions there are
static constexpr int32_t NUMFACES = 47;

// Some of the indexes into the face sprite array
static constexpr int32_t EVILFACE   = 6;
static constexpr int32_t GODFACE    = 40;
static constexpr int32_t DEADFACE   = 41;
static constexpr int32_t FIRSTSPLAT = 42;   // Gib frames

// Which slot (by index) on the weapon micronumbers display each weapon maps to
static constexpr int32_t WEAPON_MICRO_INDEXES[NUMWEAPONS] = { 0, 1, 2, 3, 4, 5, 6, 7, 0 };

extern sbflash_t                gFlashCards[NUMCARDS];
extern const facesprite_t       gFaceSprites[NUMFACES];

#if defined(__XBOX__)
// Put a player's face back after they have respawned.
//
// The gibbing animation ends by switching the face off entirely, which is right while there is no head to draw - but
// nothing switched it back on again, so a player who was gibbed spent the rest of the level with an empty box where
// their face should be. Only 'ST_Start' ever turned it on, and that runs once per level rather than once per life.
void ST_RestartPlayerFace(const int32_t playerIdx) noexcept;
#endif
// One status bar per player.
//
// This is the face, the messages, the keycard flashes and the gib animation - all of it was a single struct, so with
// two players on one console they shared a face. Whatever happened to one showed on both, which is what the synced
// invulnerability face was: the god face is chosen from the current player's powers, but there was only one place to
// record it.
//
// Reached through 'gCurPlayerIndex', which is already set to the player being drawn while each view is rendered and to
// the player being ticked while their status bar updates, so every existing use lands on the right one.
extern stbar_t                  gStatusBars[MAXPLAYERS];

#define gStatusBar (gStatusBars[gCurPlayerIndex])
extern int32_t                  gFaceTicsArr[MAXPLAYERS];

#define gFaceTics (gFaceTicsArr[gCurPlayerIndex])
extern bool                     gbDrawSBFaceArr[MAXPLAYERS];

#define gbDrawSBFace (gbDrawSBFaceArr[gCurPlayerIndex])
extern const facesprite_t*      gpCurSBFaceSprites[MAXPLAYERS];

#define gpCurSBFaceSprite (gpCurSBFaceSprites[gCurPlayerIndex])
extern bool                     gbGibDrawArr[MAXPLAYERS];

#define gbGibDraw (gbGibDrawArr[gCurPlayerIndex])
extern bool                     gbDoSpclFaceArr[MAXPLAYERS];

#define gbDoSpclFace (gbDoSpclFaceArr[gCurPlayerIndex])
extern int32_t                  gNewFaceArr[MAXPLAYERS];

#define gNewFace (gNewFaceArr[gCurPlayerIndex])
extern spclface_e               gSpclFaceTypeArr[MAXPLAYERS];

#define gSpclFaceType (gSpclFaceTypeArr[gCurPlayerIndex])

void ST_Init() noexcept;
void ST_InitEveryLevel() noexcept;
void ST_Ticker() noexcept;
void ST_Drawer() noexcept;

#if PSYDOOM_MODS
    void ST_AlertMessage(const char* const msg, const uint32_t numTics) noexcept;
#endif
