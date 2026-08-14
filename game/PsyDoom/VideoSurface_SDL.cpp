#include "VideoSurface_SDL.h"

#include "Asserts.h"

#include <cstring>
#include <SDL.h>

BEGIN_NAMESPACE(Video)

//------------------------------------------------------------------------------------------------------------------------------------------
// Attempts to create the surface with the specified dimensions.
// If creation fails then the failure is silent and usage of the surface results in NO-OPs.
//------------------------------------------------------------------------------------------------------------------------------------------
VideoSurface_SDL::VideoSurface_SDL(SDL_Renderer& renderer, const uint32_t width, const uint32_t height) noexcept
    : mpTexture(nullptr)
    , mWidth(width)
    , mHeight(height)
{
    if ((width > 0) && (height > 0)) {
        // On Xbox (nxdk software renderer): create the texture at 2x the logical size so
        // displayExternalSurface can do a near-1:1 SDL_RenderCopy instead of a slow scaled blit.
        // setPixels() writes each source pixel as a 2x2 block to fill the larger texture.
#if defined(__XBOX__)
        const uint32_t texW = width * 2;
        const uint32_t texH = height * 2;
#else
        const uint32_t texW = width;
        const uint32_t texH = height;
#endif
#if defined(__XBOX__)
        mpTexture = SDL_CreateTexture(&renderer, SDL_PIXELFORMAT_RGB888, SDL_TEXTUREACCESS_STREAMING, texW, texH);
#else
        mpTexture = SDL_CreateTexture(&renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, texW, texH);
#endif
    }
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Cleans up surface resources
//------------------------------------------------------------------------------------------------------------------------------------------
VideoSurface_SDL::~VideoSurface_SDL() noexcept {
    if (mpTexture) {
        SDL_DestroyTexture(mpTexture);
        mpTexture = nullptr;
    }
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Get the dimensions of the surface
//------------------------------------------------------------------------------------------------------------------------------------------
uint32_t VideoSurface_SDL::getWidth() const noexcept { return mWidth; }
uint32_t VideoSurface_SDL::getHeight() const noexcept { return mHeight; }

//------------------------------------------------------------------------------------------------------------------------------------------
// Sets the pixels for the surface.
// Ignores the call if the surface was not successfully initialized.
//------------------------------------------------------------------------------------------------------------------------------------------
void VideoSurface_SDL::setPixels(const uint32_t* const pSrcPixels) noexcept {
    ASSERT(pSrcPixels);

    // Abort if the texture wasn't successfully initialized
    if (!mpTexture)
        return;

    // Abort if we can't lock the texture for some reason
    void* pCurDstRow = nullptr;
    int pitch = 0;

    if (SDL_LockTexture(mpTexture, nullptr, &pCurDstRow, &pitch) != 0) {
        ASSERT_FAIL("VideoSurface_SDL::setPixels: locking the texture failed!");
        return;
    }

#if defined(__XBOX__)
    // Xbox: write each source pixel as a 2x2 block into the 2x pre-scaled texture.
    // This matches the 2x texture created in the constructor so SDL_RenderCopy can do a 1:1 blit.
    {
        const int strideWords = pitch / (int)sizeof(uint32_t);
        const uint32_t* pSrc = pSrcPixels;
        for (uint32_t y = 0; y < mHeight; ++y) {
            uint32_t* const pRow0 = (uint32_t*)pCurDstRow + (y * 2) * strideWords;
            uint32_t* const pRow1 = pRow0 + strideWords;
            for (uint32_t x = 0; x < mWidth; ++x) {
                const uint32_t px = pSrc[x];
                // Convert ABGR8888 input to RGB888 for nxdk native window format
                const uint32_t rgb = ((px & 0xFFu) << 16) | (px & 0xFF00u) | ((px >> 16) & 0xFFu);
                pRow0[x * 2    ] = rgb;
                pRow0[x * 2 + 1] = rgb;
                pRow1[x * 2    ] = rgb;
                pRow1[x * 2 + 1] = rgb;
            }
            pSrc += mWidth;
        }
    }
#else
    // Copy the texture data row by row
    const uint32_t* pCurSrcPixels = pSrcPixels;

    for (uint32_t y = 0; y < mHeight; ++y) {
        std::memcpy(pCurDstRow, pCurSrcPixels, sizeof(uint32_t) * mWidth);
        pCurSrcPixels += mWidth;
        pCurDstRow = (char*) pCurDstRow + pitch;
    }
#endif

    // Done copying, unlock the texture
    SDL_UnlockTexture(mpTexture);
}

END_NAMESPACE(Video)
