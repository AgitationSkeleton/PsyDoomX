#pragma once

#include "Macros.h"

#include <cstddef>
#include <cstdint>

//------------------------------------------------------------------------------------------------------------------------------------------
// PlayStation 1 GPU emulation: simplified.
// This is a heavily stripped down implementation of a PlayStation 1 style GPU specifically for PsyDoom.
// 
// Simplifications made for this GPU include:
//  (1) The removal of all links to an emulated PlayStation system, including DMA and interrupts etc.
//  (2) All memory transfer stuff, status registers and I/O registers are removed, the host game can just access everything directly.
//  (3) This GPU does not concern itself with output format or video timings (PAL vs NTSC) - it just stores the VRAM region being displayed.
//  (4) The output display format is assumed to be 15-bit color, 24-bit color is not supported.
//  (5) Dithering is not supported, since Doom did not use this at all.
//  (6) Drawing primitives are always modulated by the primitive color, there is no mode where this does not happen.
//  (7) The GPU 'mask bit' for masking pixels is not supported, Doom did not use this.
//  (8) X and Y flipping textures is not supported; original PS1 models did not have this anyway so games could not use it.
//  (9) All rendering/command primitives are fed directly to the GPU and handled immediately - command buffers are not supported.
//  (10) Only rectangles, lines, triangles, and a few (newly added) Doom specific primitives are supported.
//       Quads must be decomposed externally into triangles.
//  (11) The full range of draw primitives exposed by the original LIBGPU is NOT provided, only the ones that Doom uses.
//  (12) Various not that useful bits of GPU state have been removed, for example the 'display enable' flag (originally in the status reg)
//  (13) The drawing and display areas must not wrap around in VRAM, it is assumed they do not.
//  (14) CLUTs are not allowed to wrap around in VRAM, it is assumed they do not.
//
// There are some improvements over an original PS1 GPU also, which can allow extended capabilities:
//  (1) The texture window and page can exceed 256x256 units.
//  (2) VRAM can be made bigger than the standard 1024x512 pixels.
//  (3) Texture coordinates are now 16-bit, which allows for (1) to be taken advantage of.
//------------------------------------------------------------------------------------------------------------------------------------------
// How many pixels of padding to add to each VRAM row.
//
// Rows a power of two apart make every texel of a wall column land in the same L1 cache set, so every one is a
// conflict miss. Padding breaks that. Zero until the width/stride split is proven not to change anything on its own -
// see 'docs/VRAM_STRIDE_AUDIT.md' for why that order matters.
// 8 pixels - 16 bytes, so rows stay cache line aligned while no longer being a power of two apart.
//
// 32 was tried and measured against 8. It is better for single player - render median 11.1ms against 13.7ms - and
// worse for splitscreen, 28.2ms against 24.2ms, taking its framerate from a median of 26 down to 24. 8 wins because
// of which cost each mode is held by: single player is bound by the present at ~16ms, so a few ms off its rendering
// barely moves the frame, while splitscreen is bound by rendering, so the regression there costs real frames.
//
// Larger padding presumably spreads a column so far that rows stop sharing pages usefully; splitscreen draws two
// views over more of VRAM and feels that first. Not worth chasing further - the remaining rasteriser cost is no
// longer dominated by aliasing, so the next gain has to come from elsewhere.
//
// 8 pixels worked and was measured: splitscreen rendering fell from a 34.1ms median to 24.2ms and single player's
// worst case from 33.7ms to 19.4ms. 8 is only 16 bytes though - a quarter of a 64 byte line - so consecutive rows
// still share a line's worth of set space and a short column can alias within itself. A full line of padding gives
// each row its own step through the sets.
//
// Costs 32 * 512 * 2 bytes of VRAM, around 32KB, which is nothing against what the texture cache already claims.
//
// If this is no better than 8, the remaining cost is not aliasing and the next gain has to come from elsewhere.
#if defined(__XBOX__)
    static constexpr uint16_t GPU_VRAM_STRIDE_PAD = 8;
#else
    static constexpr uint16_t GPU_VRAM_STRIDE_PAD = 0;      // Desktop has cache to spare and gains nothing here
#endif

BEGIN_NAMESPACE(Gpu)

// The original VRAM width and height (in 16-bit pixels) for the PS1
static constexpr uint16_t PS1_VRAM_W = 1024;
static constexpr uint16_t PS1_VRAM_H = 512;

//----------------------------------------------------------------------------------------------------------------------
// Represents a 24-bit RGB888 color used by the GPU with each component in 1.7 fixed point format.
// This is used as an intermediate color strength or multiplier for GPU commands and rendering.
// The value '128' is equal to 1.0 or full strength and values over that are 'overbright'.
//----------------------------------------------------------------------------------------------------------------------
union Color24F {
    // The individual components of the color
    struct {
        uint8_t r;
        uint8_t g;
        uint8_t b;
        uint8_t x;      // Unused component/padding
    } comp;

    // The full 32-bits of the color (8-bits are padding)
    uint32_t bits;

    inline constexpr Color24F() noexcept : bits(0) {}
    inline constexpr Color24F(const uint32_t bits) noexcept : bits(bits) {}

    inline constexpr Color24F(const uint8_t r, const uint8_t g, const uint8_t b) noexcept : comp{} {
        comp.r = r;
        comp.g = g;
        comp.b = b;
    }
    
    inline constexpr operator uint32_t() const noexcept { return bits; }
};

//----------------------------------------------------------------------------------------------------------------------
// Represents a 15-bit TBGR1555 color used by the GPU.
// This is used as a dest/output format for the framebuffer and also as an input format for 16-bit textures & CLUTs.
// Note: the top bit (T) is the PlayStation 'semi-transparency' bit.
//----------------------------------------------------------------------------------------------------------------------
struct Color16 {
    uint16_t bits;
    
    inline constexpr uint16_t getR() const noexcept { return bits & 0x1F; }
    inline constexpr uint16_t getG() const noexcept { return (bits >> 5) & 0x1F; }
    inline constexpr uint16_t getB() const noexcept { return (bits >> 10) & 0x1F; }
    inline constexpr uint16_t getT() const noexcept { return bits >> 15; }

    inline constexpr Color16() noexcept : bits(0) {}
    inline constexpr Color16(const uint16_t bits) noexcept : bits(bits) {}
    inline constexpr operator uint16_t() const noexcept { return bits; }
    
    // Set the color values using RGB555 components that are assumed to be in range
    void setRGB(const uint16_t r5, const uint16_t g5, const uint16_t b5) noexcept {
        bits &= 0x8000;
        bits |= (r5 | (g5 << 5) | (b5 << 10));
    }
    
    // Makes a color from the individual components.
    // Note: the components are already assumed to be in range: 5-bits for RGB and 1-bit for semi-transparency.
    static inline constexpr Color16 make(const uint16_t r5, const uint16_t g5, const uint16_t b5, const uint16_t t1) noexcept {
        return Color16(r5 | (g5 << 5) | (b5 << 10) | (t1 << 15));
    }
    
    // Same as 'make' but with the semi transparency flag not set (RGB only)
    static inline constexpr Color16 make(const uint16_t r5, const uint16_t g5, const uint16_t b5) noexcept {
        return Color16(r5 | (g5 << 5) | (b5 << 10));
    }
};

//----------------------------------------------------------------------------------------------------------------------
// Represents a 'semi-transparency' or blending mode for the GPU between foreground (fg) and background (bg) colors
//----------------------------------------------------------------------------------------------------------------------
enum class BlendMode : uint8_t {
    Alpha50,        // 50% opacity alpha blend (bg/2 + fg/2)
    Add,            // Additive blend at 100% opacity (bg + fg)
    Subtract,       // Subtractive blend at 100% opacity (bg - fg)
    Add25           // Additive blend at 25% opacity (bg + fg/4)
};

//----------------------------------------------------------------------------------------------------------------------
// Texture format used by the GPU
//----------------------------------------------------------------------------------------------------------------------
enum class TexFmt : uint8_t {
    Bpp4,           // 4-bits per pixel color indexed (using a CLUT)
    Bpp8,           // 8-bits per pixel color indexed (using a CLUT)
    Bpp16,          // 15-bit direct RGB color plus a 1-bit semi-transparency flag (16-bits per pixel overall)
};

//----------------------------------------------------------------------------------------------------------------------
// What type of drawing to do
//----------------------------------------------------------------------------------------------------------------------
enum class DrawMode : uint8_t {
    Colored,            // Draw the geometry colored only (no texture mapping) and without blending
    ColoredBlended,     // Draw the geometry colored only (no texture mapping) and with blending
    Textured,           // Draw the geometry textured with color modulation (no blending)
    TexturedBlended,    // Draw the geometry textured with color modulation (blending enabled)
};

//----------------------------------------------------------------------------------------------------------------------
// GPU drawing primitives: rectangles, lines and triangles.
// PsyDoom also adds new Doom specific GPU primitives, floor rows and wall columns to accelerate rendering.
// These should produce similar results to standard triangles, but at a much lower cost.
//----------------------------------------------------------------------------------------------------------------------
struct DrawRect {
    int16_t     x;          // Position of rectangle: x
    int16_t     y;          // Position of rectangle: y
    uint16_t    w;          // Width of rectangle. Note: not allowed to exceed 1023!
    uint16_t    h;          // Height of rectangle. Note: not allowed to exceed 511!
    uint16_t    u;          // Top left texcoord: u
    uint16_t    v;          // Top right texcoord: v
    Color24F    color;      // Color to shade the rectangle with
};

struct DrawLine {
    int16_t     x1;         // Line point 1: x
    int16_t     y1;         // Line point 1: y
    int16_t     x2;         // Line point 2: x
    int16_t     y2;         // Line point 2: y
    Color24F    color;      // Color to draw the line with
};

struct DrawTriangle {
    int16_t     x1;         // Triangle point 1: x
    int16_t     y1;         // Triangle point 1: y
    int16_t     u1;         // Triangle point 1: u texcoord
    int16_t     v1;         // Triangle point 1: v texcoord
    int16_t     x2;         // Triangle point 2: x
    int16_t     y2;         // Triangle point 2: y
    int16_t     u2;         // Triangle point 2: u texcoord
    int16_t     v2;         // Triangle point 2: v texcoord
    int16_t     x3;         // Triangle point 3: x
    int16_t     y3;         // Triangle point 3: y
    int16_t     u3;         // Triangle point 3: u texcoord
    int16_t     v3;         // Triangle point 3: v texcoord
    Color24F    color;      // Color to draw the triangle with
};

struct DrawTriangleGouraud {
    int16_t     x1;         // Triangle point 1: x
    int16_t     y1;         // Triangle point 1: y
    int16_t     u1;         // Triangle point 1: u texcoord
    int16_t     v1;         // Triangle point 1: v texcoord
    int16_t     x2;         // Triangle point 2: x
    int16_t     y2;         // Triangle point 2: y
    int16_t     u2;         // Triangle point 2: u texcoord
    int16_t     v2;         // Triangle point 2: v texcoord
    int16_t     x3;         // Triangle point 3: x
    int16_t     y3;         // Triangle point 3: y
    int16_t     u3;         // Triangle point 3: u texcoord
    int16_t     v3;         // Triangle point 3: v texcoord
    Color24F    color1;     // Triangle point 1: color
    Color24F    color2;     // Triangle point 1: color
    Color24F    color3;     // Triangle point 1: color
};

// New Doom specific primitive (textured floor row)
struct DrawFloorRow {
    int16_t     y;          // Row y value
    int16_t     x1;         // Row point 1: x
    int16_t     u1;         // Row point 1: u texcoord
    int16_t     v1;         // Row point 1: v texcoord
    int16_t     x2;         // Row point 2: x
    int16_t     u2;         // Row point 2: u texcoord
    int16_t     v2;         // Row point 2: v texcoord
    Color24F    color;      // Color to draw the row with
};

// New Doom specific primitive with a constant 'u' value (textured wall column)
struct DrawWallCol {
    int16_t     x;          // Column x value
    int16_t     u;          // Column u texcoord
    int16_t     y1;         // Column point 1: y
    int16_t     v1;         // Column point 1: v texcoord
    int16_t     y2;         // Column point 2: y
    int16_t     v2;         // Column point 2: v texcoord
    Color24F    color;      // Color to draw the column with
};

// New Doom specific primitive with a constant 'u' value (textured and gouraud shaded wall column)
struct DrawWallColGouraud {
    int16_t     x;          // Column x value
    int16_t     u;          // Column u texcoord
    int16_t     y1;         // Column point 1: y
    int16_t     v1;         // Column point 1: v texcoord
    int16_t     y2;         // Column point 2: y
    int16_t     v2;         // Column point 2: v texcoord
    Color24F    color1;     // Column point 1: color
    Color24F    color2;     // Column point 2: color
};

//----------------------------------------------------------------------------------------------------------------------
// The GPU core/device itself
//----------------------------------------------------------------------------------------------------------------------
struct Core {
    uint16_t*       pRam;               // The VRAM for the GPU: declared as an array of 16-bit/2-byte pixels
    uint16_t        ramPixelW;          // The width of VRAM (in terms of 16-bit/2-byte pixels) - always a power of 2
    uint16_t        ramStride;          // Distance between rows in memory, in pixels. See 'docs/VRAM_STRIDE_AUDIT.md'.
                                        //
                                        // Separate from the width because the two are different things and sharing one
                                        // field for both is what made padding the stride corrupt every texture before:
                                        // the width is a power of 2 and the wrap masks are built from it, while the
                                        // stride only ever decides how far apart rows sit. Use this for address
                                        // arithmetic and 'ramPixelW' for anything that masks, clamps or bounds checks.
    uint16_t        ramPixelH;          // The height of VRAM (in terms of 16-bit/2-byte pixels) - always a power of 2
    uint16_t        ramXMask;           // A mask which wraps coordinates to be inside of VRAM
    uint16_t        ramYMask;           // A mask which wraps coordinates to be inside of VRAM
    int16_t         drawOffsetX;        // X offset added to vertices before rasterizing, brings the geometry into the area of VRAM being drawn to
    int16_t         drawOffsetY;        // Y offset added to vertices before rasterizing, brings the geometry into the area of VRAM being drawn to
    uint16_t        drawAreaLx;         // The area of VRAM being drawn to (left X, inclusive)
    uint16_t        drawAreaRx;         // The area of VRAM being drawn to (right X, inclusive)
    uint16_t        drawAreaTy;         // The area of VRAM being drawn to (top Y, inclusive)
    uint16_t        drawAreaBy;         // The area of VRAM being drawn to (bottom Y, inclusive)
    uint16_t        displayAreaX;       // The area of VRAM being displayed (top left X)
    uint16_t        displayAreaY;       // The area of VRAM being displayed (top left Y)
    uint16_t        displayAreaW;       // The area of VRAM being displayed (width)
    uint16_t        displayAreaH;       // The area of VRAM being displayed (height)
    uint16_t        texPageX;           // Location of the area used for texturing (top left X, in terms of 16-bit pixels)
    uint16_t        texPageY;           // Location of the area used for texturing (top left Y, in terms of 16-bit pixels)
    uint16_t        texPageXMask;       // Mask used to wrap X coordinates to be within the texture page (e.g 0xFF for 256 pixel wrap, in terms of 16-bit pixels)
    uint16_t        texPageYMask;       // Mask used to wrap Y coordinates to be within the texture page (e.g 0xFF for 256 pixel wrap, in terms of 16-bit pixels)
    uint16_t        texWinX;            // Location of a window within the texture page to use for texturing (top left X, in terms of current format pixels)
    uint16_t        texWinY;            // Location of a window within the texture page to use for texturing (top left Y, in terms of current format pixels)
    uint16_t        texWinXMask;        // Masks X coordinates to be within the texture window (e.g 0xF for 16 pixel wrap, in terms of current format pixels)
    uint16_t        texWinYMask;        // Masks Y coordinates to be within the texture window (e.g 0xF for 16 pixel wrap, in terms of current format pixels)
    BlendMode       blendMode;          // Blend mode for blended/semi-transparent geometry
    TexFmt          texFmt;             // Current texture format in use
    uint16_t        clutX;              // X position of the current CLUT/color-index table in 16-bit VRAM pixels (CLUT is arranged in a row at this location)
    uint16_t        clutY;              // Y position of the current CLUT/color-index table in 16-bit VRAM pixels (CLUT is arranged in a row at this location)
    bool            bDisableMasking;    // PSX GPU extension: disable pixel discard during texture mapping when all the texel bits are '0'?

    // CLUT cache to speed up texture mapping and the settings it was last saved with
    TexFmt          clutCacheFmt;
    uint16_t        clutCacheX;
    uint16_t        clutCacheY;
    Color16         clutCache[256];
};

// Initializing and shutting down a core
void initCore(Core& core, const uint16_t ramPixelW, const uint16_t ramPixelH) noexcept;
void destroyCore(Core& core) noexcept;

// VRAM reading
uint16_t vramReadU16(const Core& core, const uint16_t x, const uint16_t y) noexcept;
void vramWriteU16(Core& core, const uint16_t x, const uint16_t y, const uint16_t value) noexcept;
Color16 readTexel(Core& core, const uint16_t coordX, const uint16_t coordY) noexcept;

// Miscellaneous
void updateClutCache(Core& core) noexcept;
bool isPixelInDrawArea(const Core& core, const uint16_t x, const uint16_t y) noexcept;
void clearRect(Core& core, const Color16 color, const uint16_t x, const uint16_t y, const uint16_t w, const uint16_t h) noexcept;

// Color manipulation and conversion
template <DrawMode DrawMode>
Color16 color24FTo16(const Color24F colorIn) noexcept;

Color16 colorMul(const Color16 color1, const Color24F color2) noexcept;
Color16 colorBlend(const Color16 bg, const Color16 fg, const BlendMode mode) noexcept;

// Drawing functions: note that lines CANNOT be textured!
template <DrawMode DrawMode>
void draw(Core& core, const DrawRect& rect) noexcept;

template <DrawMode DrawMode>
void draw(Core& core, const DrawLine& line) noexcept;

template <DrawMode DrawMode>
void draw(Core& core, const DrawTriangle& triangle) noexcept;

template <DrawMode DrawMode>
void draw(Core& core, const DrawTriangleGouraud& triangle) noexcept;

template <DrawMode DrawMode>
void draw(Core& core, const DrawFloorRow& row) noexcept;

template <DrawMode DrawMode>
void draw(Core& core, const DrawWallCol& col) noexcept;

template <DrawMode DrawMode>
void draw(Core& core, const DrawWallColGouraud& col) noexcept;

#if defined(__XBOX__)
//------------------------------------------------------------------------------------------------------------------------------------------
// Xbox: where the rasteriser's time goes, by kind of primitive.
//
// 'render' is 24.4ms of a 34.7ms frame and is now the whole problem: even a present costing nothing leaves the worst
// frames around 41ms, which is short of a 30fps floor, never mind 60. Before deciding whether that time can be
// optimised away or has to be moved onto the GPU, it is worth knowing what it is made of - walls, floors, sprites - and
// how many of each there are.
//
// Two timestamp reads per primitive against a few thousand primitives a frame is a fraction of a millisecond, and is
// the only way to tell a rasteriser that is slow per pixel from a renderer submitting far too many primitives.
//------------------------------------------------------------------------------------------------------------------------------------------
extern uint64_t gDbgWallColTicks;       // Time in wall column drawing
extern uint64_t gDbgFloorRowTicks;      // ...in floor and ceiling spans
extern uint64_t gDbgTriangleTicks;      // ...in triangles, which is sprites and the sky
extern uint64_t gDbgRectTicks;          // ...in rectangles, which is mostly UI and the status bar
extern uint32_t gDbgNumWallCols;
extern uint32_t gDbgNumFloorRows;
extern uint32_t gDbgNumTriangles;
extern uint32_t gDbgNumRects;

// Pixels actually written, per kind.
//
// Time per primitive turned out to be useless for comparing one session against another: a wall column's cost depends
// on how tall it is and a rectangle's on how large it is, so the same figure means different things in different
// scenes. Nanoseconds per pixel is the number that can be compared, and it is the one that says whether a change to
// the inner loop did anything.
extern uint64_t gDbgWallPixels;
extern uint64_t gDbgFloorPixels;
extern uint64_t gDbgRectPixels;

void resetDrawStats() noexcept;
#endif

END_NAMESPACE(Gpu)
