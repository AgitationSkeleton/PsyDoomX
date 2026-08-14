#pragma once

#include "Doom/doomdef.h"

#include <vector>

// GTE rotation constants: 1.0 and the shift to go from 16.16 to 4.12.
// The GTE stores rotation matrix entries as 4.12 fixed point numbers!
static constexpr int16_t    GTE_ROTFRAC_UNIT    = 4096;
static constexpr uint16_t   GTE_ROTFRAC_SHIFT   = 4;

// Renderer constant: maximum number of subsectors that can be drawn in the original engine
#if !PSYDOOM_LIMIT_REMOVING
    static constexpr uint32_t MAX_DRAW_SUBSECTORS = 192;
#endif

// Renderer constant: clip geometry at this depth
static constexpr int32_t NEAR_CLIP_DIST = 2;

struct light_t;
struct MATRIX;
struct node_t;
struct side_t;
struct subsector_t;

extern int32_t          gValidCount;
extern player_t*        gpViewPlayer;
extern fixed_t          gViewX;
extern fixed_t          gViewY;
extern fixed_t          gViewZ;
extern angle_t          gViewAngle;
extern fixed_t          gViewCos;
extern fixed_t          gViewSin;
extern bool             gbIsSkyVisible;
extern MATRIX           gDrawMatrix;
extern bool             gbDoViewLighting;

// PsyDoom: these are not used anymore with dual colored lighting.
// They were only usable when sectors were guaranteed to have a single color.
#if !PSYDOOM_MODS
    extern const light_t*   gpCurLight;
    extern uint32_t         gCurLightValR;
    extern uint32_t         gCurLightValG;
    extern uint32_t         gCurLightValB;
#endif

// PsyDoom: the number of draw subsectors is now unlimited
#if PSYDOOM_LIMIT_REMOVING
    extern std::vector<subsector_t*> gpDrawSubsectors;
#else
    extern subsector_t*     gpDrawSubsectors[MAX_DRAW_SUBSECTORS];
    extern int32_t          gNumDrawSubsectors;
#endif

extern sector_t*        gpCurDrawSector;
extern subsector_t**    gppEndDrawSubsector;

#if PSYDOOM_MODS
    extern fixed_t      gPlayerLerpFactor;
    extern fixed_t      gWorldLerpFactor;
    // Where each player's view was at the last tick, which is what this frame interpolates away from.
    //
    // One set of these was shared between both players, and 'R_SnapPlayerInterpolation' fills them for whichever player
    // happens to be current when a tick begins. So both views were drawn as a blend from ONE player's last position and
    // angle: when player one turned, player two's view swung part of the way towards player one's angle and then back,
    // without player two touching anything. The nearer the two are to each other the smaller it looks, which is why it
    // reads as a jitter rather than as the view being plainly wrong.
    //
    // Reached through 'gCurPlayerIndex', the same way the rest of the per-view state here is.
    extern fixed_t      gOldViewXs[MAXPLAYERS];
    extern fixed_t      gOldViewYs[MAXPLAYERS];
    extern fixed_t      gOldViewZs[MAXPLAYERS];
    extern angle_t      gOldViewAngles[MAXPLAYERS];

    #define gOldViewX (gOldViewXs[gCurPlayerIndex])
    #define gOldViewY (gOldViewYs[gCurPlayerIndex])
    #define gOldViewZ (gOldViewZs[gCurPlayerIndex])
    #define gOldViewAngle (gOldViewAngles[gCurPlayerIndex])

    extern fixed_t      gOldAutomapX;
    extern fixed_t      gOldAutomapY;
    extern fixed_t      gOldAutomapScale;
    // View z interpolation state, one set per player.
    //
    // A player carried by a lift or crusher moves at the world's 15Hz rather than their own 30Hz, so their view z has
    // to be interpolated differently and sometimes snapped outright. All of this was global, so one player standing on
    // a moving sector snapped and pushed the OTHER player's view as well - which is the jitter seen when one player
    // rides a platform and the other is nowhere near it.
    //
    // Reached through 'gCurPlayerIndex', which is set to the player being drawn while each view renders.
    extern bool         gbSnapViewZInterpolations[MAXPLAYERS];

    #define gbSnapViewZInterpolation (gbSnapViewZInterpolations[gCurPlayerIndex])
    extern fixed_t      gViewPushedZs[MAXPLAYERS];

    #define gViewPushedZ (gViewPushedZs[gCurPlayerIndex])
    extern bool         gbOldViewZIsPusheds[MAXPLAYERS];

    #define gbOldViewZIsPushed (gbOldViewZIsPusheds[gCurPlayerIndex])
#endif

void R_Init() noexcept;
void R_RenderPlayerView() noexcept;
int32_t R_SlopeDiv(const uint32_t num, const uint32_t den) noexcept;
angle_t R_PointToAngle2(const fixed_t x1, const fixed_t y1, const fixed_t x2, const fixed_t y2) noexcept;
int32_t R_PointOnSide(const fixed_t x, const fixed_t y, const node_t& node) noexcept;
subsector_t* R_PointInSubsector(const fixed_t x, const fixed_t y) noexcept;

#if PSYDOOM_MODS
    void R_SnapPlayerInterpolation() noexcept;
    void R_InterpBeginPlayerFrame() noexcept;
    void R_InterpBeginWorldFrame() noexcept;
    void R_SnapViewZInterpolation() noexcept;
    void R_SnapSectorInterpolation(sector_t& sector) noexcept;
    void R_SnapSideInterpolation(side_t& side) noexcept;
    void R_SnapMobjInterpolation(mobj_t& mobj) noexcept;
    void R_SnapPsprInterpolation(pspdef_t& pspr) noexcept;
    void R_CalcLerpFactors() noexcept;
    fixed_t R_LerpCoord(const fixed_t oldCoord, const fixed_t newCoord, const fixed_t mix) noexcept;
    angle_t R_LerpAngle(const angle_t oldAngle, const angle_t newAngle, const fixed_t mix) noexcept;
    bool R_HasHigherSurroundingSkyCeiling(const sector_t& sector) noexcept;
    bool R_HasLowerSurroundingSkyFloor(const sector_t& sector) noexcept;
    void R_UpdateSectorDrawHeights(sector_t& sector) noexcept;
    void R_UpdateShadingParams(sector_t& sector) noexcept;
    light_t R_GetSectorLightColor(const sector_t& sector, const fixed_t z) noexcept;
    void R_GetSectorDrawColor(const sector_t& sector, const fixed_t z, uint8_t& r, uint8_t& g, uint8_t& b) noexcept;
    fixed_t R_FindLowestSurroundingInterpFloorHeight(sector_t& sector) noexcept;
#endif
