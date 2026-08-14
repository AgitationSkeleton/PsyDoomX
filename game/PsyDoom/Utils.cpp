#include "Utils.h"

#include "Config/Config.h"
#include "Controls.h"
#include "DiscInfo.h"
#include "DiscReader.h"
#include "Doom/Game/p_tick.h"
#include "Doom/Base/i_main.h"      // For 'gCurPlayerIndex', which the per player status bar is reached through
#include "Doom/UI/st_main.h"
#include "FatalErrors.h"
#include "Input.h"
#include "IsoFileSys.h"
#include "Network.h"
#include "PlayerPrefs.h"
#include "ProgArgs.h"
#include "PsxVm.h"
#include "Video.h"
#if PSYDOOM_VULKAN_RENDERER
#include "Vulkan/VDrawing.h"
#include "Vulkan/VRenderer.h"
#include "Vulkan/VTypes.h"
#endif
#include "Wess/psxcd.h"
#include "Wess/psxspu.h"
#include "Wess/wessapi.h"
#include "Wess/wessseq.h"

#include <chrono>
#include <md5.h>
#include <SDL.h>
#include <thread>
#if defined(__XBOX__)
#include <hal/debug.h>
#include <windows.h>
#endif

BEGIN_NAMESPACE(Utils)

static constexpr const char* const SAVE_FILE_ORG        = "com.codelobster";    // Root folder to save config in (in a OS specific writable prefs location)
static constexpr const char* const SAVE_FILE_PRODUCT    = "PsyDoom";            // Sub-folder within the root folder to save the config in

// Because typing this is a pain...
typedef std::chrono::high_resolution_clock::time_point timepoint_t;

// When we last did platform updates
static timepoint_t gLastPlatformUpdateTime = {};

//------------------------------------------------------------------------------------------------------------------------------------------
// Gets the game version string.
// This is used for the window title.
//------------------------------------------------------------------------------------------------------------------------------------------
const char* getGameVersionString() noexcept {
    #ifdef GAME_VERSION_STR
        return "PsyDoom " GAME_VERSION_STR;
    #else
        return "PsyDoom <UNKNOWN_VERSION>";
    #endif
}

//------------------------------------------------------------------------------------------------------------------------------------------
// A custom handler for fatal errors and installing and uninstalling it
//------------------------------------------------------------------------------------------------------------------------------------------
static void fatalErrorHandler(const char* const msg) noexcept {
    // Kill the current window and show a GUI error box, except if in headless mode
    if (ProgArgs::gbHeadlessMode)
        return;

    // Only handle 1 fatal error in case further errors are raised while shutting down video!
    static bool bDidHandleFatalError = false;

    if (bDidHandleFatalError)
        return;

    bDidHandleFatalError = true;
    Video::shutdownVideo();
#if defined(__XBOX__)
    // nxdk SDL2 has no message box implementation; print to debug screen instead.
    debugClearScreen();
    debugPrint("PSYDOOM FATAL ERROR\n\n%s\n", msg);
    while (true) { Sleep(500); }
#else
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "A fatal error has occurred!", msg, nullptr);
#endif
}

void installFatalErrorHandler() noexcept {
    FatalErrors::gFatalErrorHandler = fatalErrorHandler;
}

void uninstallFatalErrorHandler() noexcept {
    FatalErrors::gFatalErrorHandler = nullptr;
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Get the folder that PsyDoom uses for user config and save data.
// If the folder does not exist then it is created, and if that fails a fatal error is issued.
// The Path is returned with a trailing path separator, so can be combined with a file name without any other modifications.
//------------------------------------------------------------------------------------------------------------------------------------------
std::string getOrCreateUserDataFolder() noexcept {
#if defined(__XBOX__)
    // nxdk SDL2 has SDL_FILESYSTEM_DUMMY; SDL_GetPrefPath always returns NULL.
    // Use a fixed path on the Xbox HDD instead.
    static constexpr const char* const kXboxDataDir = "E:\\Apps\\PsyDoomX\\";
    CreateDirectoryA(kXboxDataDir, nullptr);  // no-op if already exists
    return kXboxDataDir;
#else
    char* const pCfgFilePath = SDL_GetPrefPath(SAVE_FILE_ORG, SAVE_FILE_PRODUCT);

    if (!pCfgFilePath) {
        FatalErrors::raise("Unable to create or determine the user data folder (config/save folder) for PsyDoom!");
    }

    std::string path = pCfgFilePath;
    SDL_free(pCfgFilePath);
    return path;
#endif
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Run update actions that have to be done periodically, including running the window and processing sound
//------------------------------------------------------------------------------------------------------------------------------------------
void doPlatformUpdates() noexcept {
    // In headless mode we can skip this entirely
    if (ProgArgs::gbHeadlessMode)
        return;

    // Always generate timer events and update the music sequencer.
    // Note that for PsyDoom the sequencer is now manually updated here and it now uses a delta time rather than a fixed increment
    PsxVm::generateTimerEvents();

    if (gbWess_SeqOn) {
        SeqEngine();
    }

    // Only do these updates if enough time has elapsed.
    // Do this to prevent excessive CPU usage in loops that are periodically trying to update sound etc. while waiting for some event.
#if defined(__XBOX__)
    // On Xbox, std::chrono::high_resolution_clock::now() does not advance reliably.
    // Use SDL_GetTicks() instead (millisecond resolution, proven to work).
    static uint32_t gLastPlatformUpdateTick = 0;
    const uint32_t nowTick = SDL_GetTicks();
    if ((nowTick - gLastPlatformUpdateTick) < 4u)
        return;
    gLastPlatformUpdateTick = nowTick;
#else
    const timepoint_t now = std::chrono::high_resolution_clock::now();
    if (now - gLastPlatformUpdateTime < std::chrono::milliseconds(4))
        return;
    gLastPlatformUpdateTime = now;
#endif

    // Actually do the platform updates
    Network::doUpdates();
    Input::update();
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Wait for a condition to be true and only abort if the condition is met or if the user has requested an app quit.
// Returns 'true' if the condition was met, or 'false' if aborted due to a user quit or because the game is in headless mode.
// While we are waiting video and the window are updated, so the application does not freeze and can be quit.
//------------------------------------------------------------------------------------------------------------------------------------------
template <class T>
static bool waitForCond(const T& condLamba) noexcept {
    // We never wait in headless mode
    if (ProgArgs::gbHeadlessMode)
        return false;

#if defined(__XBOX__)
    // On Xbox, std::chrono::high_resolution_clock::now() does not advance reliably on nxdk.
    // If it stalls (always returning the same value), the displayFramebuffer call below fires
    // every single loop iteration, which starves the SDL audio DPC thread and kills audio (CBS:0).
    // Also: Input::update() must be called here or controller axes get stuck (camera spin bug)
    // since the main game loop can't run while we're in this wait.
    uint32_t lastDisplayTick = SDL_GetTicks() - 33u;  // ensure first iteration updates display

    while (true) {
        if (condLamba())
            return true;
        if (Input::isQuitRequested())
            return false;

        // Rate-limit to ~30Hz so we don't starve the audio DPC thread
        const uint32_t nowTick = SDL_GetTicks();
        if ((nowTick - lastDisplayTick) >= 33u) {
            lastDisplayTick = nowTick;
            Video::displayFramebuffer();
        }

        // Keep USB hub polling alive and process USB axis/button events
        // so the controller doesn't get stuck at a non-zero axis value.
        Input::update();
        SDL_Delay(1);  // Yield so audio callback can run
    }
#else
    // Time-gate the display calls to avoid hammering SDL_RenderPresent, which can starve the
    // audio callback thread on nxdk and prevent CDDA positions from advancing.
    timepoint_t lastDisplayTime = {};

    while (true) {
        // Is the condition now true?
        if (condLamba())
            return true;

        // Abort because the user asked for an app quit?
        if (Input::isQuitRequested())
            return false;

        // Only call displayFramebuffer at ~30Hz to avoid blocking the audio thread.
        // The previous code called it every iteration which could starve SDL audio callbacks.
        const timepoint_t now = std::chrono::high_resolution_clock::now();
        if (now - lastDisplayTime >= std::chrono::milliseconds(33)) {
            lastDisplayTime = now;
            if (Video::gBackendType != Video::BackendType::Vulkan) {
                Video::displayFramebuffer();
            }
        }

        Utils::threadYield();
        doPlatformUpdates();
    }
#endif

    // Should never get here!
    return false;
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Wait for a number of seconds while still doing platform updates; returns 'false' if wait was aborted
//------------------------------------------------------------------------------------------------------------------------------------------
bool waitForSeconds(const float seconds) noexcept {
#if defined(__XBOX__)
    // Use SDL_GetTicks() on Xbox: clock() may not advance correctly on nxdk.
    const uint32_t xbStart  = SDL_GetTicks();
    const uint32_t xbWaitMs = (uint32_t)(seconds * 1000.0f);
    return waitForCond([=]() noexcept {
        return ((SDL_GetTicks() - xbStart) >= xbWaitMs);
    });
#else
    const clock_t startTime = clock();
    return waitForCond([&]() noexcept {
        const clock_t now = clock();
        const double elapsed = (double)(now - startTime) / (double) CLOCKS_PER_SEC;
        return (elapsed >= seconds);
    });
#endif
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Implements an original PSX Doom wait loop with tweaks for PC to keep the app responsive; returns 'false' if the wait was aborted.
// Waits until at least 1 CD audio sector has been read before continuing.
//------------------------------------------------------------------------------------------------------------------------------------------
bool waitForCdAudioPlaybackStart() noexcept {
#if defined(__XBOX__)
    // On Xbox, CDDA may take time to start (or not advance at all).
    // Use SDL_GetTicks() for the timeout: std::chrono::steady_clock is unreliable on nxdk.
    const uint32_t xbStart = SDL_GetTicks();
    return waitForCond([=]() noexcept {
        return (
            ((SDL_GetTicks() - xbStart) > 500u) ||
            (PsxVm::gDiscInfo.getTrack(psxcd_get_playing_track()) == nullptr) ||
            (psxcd_elapsed_sectors() != 0) ||
            (!PsxVm::haveAudioOutputDevice())
        );
    });
#else
    return waitForCond([]() noexcept {
        // PsyDoom: skip the wait if there isn't a valid track playing.
        // Also avoid an infinite wait if we don't have a valid audio output device, since CD audio will never be consumed in that case.
        #if PSYDOOM_MODS
            return (
                (PsxVm::gDiscInfo.getTrack(psxcd_get_playing_track()) == nullptr) ||
                (psxcd_elapsed_sectors() != 0) ||
                (!PsxVm::haveAudioOutputDevice())
            );
        #else
            return (psxcd_elapsed_sectors() != 0);
        #endif
    });
#endif
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Implements an original PSX Doom wait loop with tweaks for PC to keep the app responsive; returns 'false' if the wait was aborted.
// Waits until a specified music or sound sequence has exited the specified status or the application is quitting.
//------------------------------------------------------------------------------------------------------------------------------------------
bool waitUntilSeqEnteredStatus(const int32_t sequenceIdx, const SequenceStatus status) noexcept {
#if defined(__XBOX__)
    // Use SDL_GetTicks() for the timeout: std::chrono::steady_clock is unreliable on nxdk.
    const uint32_t xbStart = SDL_GetTicks();
    return waitForCond([=]() noexcept {
        return (((SDL_GetTicks() - xbStart) > 2000u) || (wess_seq_status(sequenceIdx) == status));
    });
#else
    return waitForCond([=]() noexcept {
        return (wess_seq_status(sequenceIdx) == status);
    });
#endif
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Implements an original PSX Doom wait loop with tweaks for PC to keep the app responsive; returns 'false' if the wait was aborted.
// Waits until a specified music or sound sequence has exited the specified status or the application is quitting.
//------------------------------------------------------------------------------------------------------------------------------------------
bool waitUntilSeqExitedStatus(const int32_t sequenceIdx, const SequenceStatus status) noexcept {
#if defined(__XBOX__)
    // Use SDL_GetTicks() for the timeout: std::chrono::steady_clock is unreliable on nxdk.
    const uint32_t xbStart = SDL_GetTicks();
    return waitForCond([=]() noexcept {
        return (((SDL_GetTicks() - xbStart) > 2000u) || (wess_seq_status(sequenceIdx) != status));
    });
#else
    return waitForCond([=]() noexcept {
        return (wess_seq_status(sequenceIdx) != status);
    });
#endif
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Implements an original PSX Doom wait loop with tweaks for PC to keep the app responsive; returns 'false' if the wait was aborted.
// Waits until at CD audio has finished fading out.
//------------------------------------------------------------------------------------------------------------------------------------------
bool waitForCdAudioFadeOut() noexcept {
#if defined(__XBOX__)
    // Use SDL_GetTicks() for the timeout: std::chrono::steady_clock is unreliable on nxdk.
    const uint32_t xbStart = SDL_GetTicks();
    return waitForCond([=]() noexcept {
        return (((SDL_GetTicks() - xbStart) > 1000u) || (!psxspu_get_cd_fade_status()));
    });
#else
    return waitForCond([=]() noexcept {
        return (!psxspu_get_cd_fade_status());
    });
#endif
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Yield some CPU time to the host machine
//------------------------------------------------------------------------------------------------------------------------------------------
void threadYield() noexcept {
#if defined(__XBOX__)
    // Give up the rest of this slice, but do not sleep.
    //
    // This was 'SDL_Delay(1)', so that SDL's audio thread would be scheduled and could keep CD audio moving. That
    // thread no longer exists here: audio output is driven by a thread of this port's own, polling the hardware every
    // 4ms and asking for the highest priority there is, which the scheduler honours without anyone standing aside for
    // it.
    //
    // The sleep was not free. It is taken once per frame from 'I_DrawPresent', and measured across 19,491 frames it
    // cost 2.78ms of a 28ms frame - a tenth of the frame, spent asleep, for a thread that was removed. A millisecond
    // asked for is rarely a millisecond given either: it is a floor, rounded up to whatever the scheduler's next tick
    // happens to be.
    //
    // 'SwitchToThread' hands over to anything ready to run and comes straight back if nothing is, which is what was
    // wanted in the first place. USB polling is unaffected - that happens in 'doPlatformUpdates', just above the call
    // site, and not here.
    SwitchToThread();
#else
    std::this_thread::yield();
#endif
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Does some setup for UI drawing if using the new Vulkan based renderer
//------------------------------------------------------------------------------------------------------------------------------------------
void onBeginUIDrawing() noexcept {
    #if PSYDOOM_VULKAN_RENDERER
        // Setup the UI transform matrix for drawing if using the Vulkan renderer
        const bool bSetDrawMatrix = (
            (Video::gBackendType == Video::BackendType::Vulkan) &&
            (!VRenderer::isUsingPsxRenderPath()) &&
            VRenderer::isRendering()
        );

        if (bSetDrawMatrix) {
            // Note: before setting the transform matrix and other uniforms make sure we are on a compatible pipeline that can accept these push constants.
            // Also make sure to end the current drawing batch, in case draw commands before this are affected by the uniform changes.
            VDrawing::endCurrentDrawBatch();
            VDrawing::setDrawPipeline(VPipelineType::UI_8bpp);

            // Set the draw uniforms, including the transform matrix
            {
                VShaderUniforms_Draw uniforms = {};
                VRenderer::initRendererUniformFields(uniforms);
                uniforms.mvpMatrix = VDrawing::computeTransformMatrixForUI(Config::gbVulkanWidescreenEnabled);

                VDrawing::setDrawUniforms(uniforms);
            }
        }
    #endif
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Check to see if the button to toggle the Vulkan/classic renderers has been pressed.
// If that is the case then this function will begin a renderer toggle, if that is possible.
//------------------------------------------------------------------------------------------------------------------------------------------
void checkForRendererToggleInput() noexcept {
    #if PSYDOOM_VULKAN_RENDERER
        // Renderer can only be toggled if using the Vulkan backend
        if (Video::gBackendType != Video::BackendType::Vulkan)
            return;

        // Do the toggle if the toggle button is just pressed
        if (!Controls::isJustPressed(Controls::Binding::Toggle_Renderer))
            return;

        const bool bUseVulkan = VRenderer::isUsingPsxRenderPath();

        if (bUseVulkan) {
            VRenderer::switchToMainVulkanRenderPath();
        } else {
            VRenderer::switchToPsxRenderPath();
        }

        gStatusBar.message = (bUseVulkan) ? "Vulkan Renderer." : "Classic Renderer.";
        gStatusBar.messageTicsLeft = 30;
    #endif
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Checks to see if the button to toggle between capped and uncapped framerates has been pressed.
// Peforms the switch between framerate modes if it is pressed.
//------------------------------------------------------------------------------------------------------------------------------------------
void checkForUncappedFramerateToggleInput() noexcept {
    // Xbox: not offered, by hotkey any more than by menu.
    //
    // Capping the frame rate here does not protect anything - the game does not reach the cap - and turning it on
    // makes 'I_DrawPresent' spin until enough vblanks have passed, which pins the frame to 33.3ms and throws away
    // every millisecond saved below that. It can only make things worse, so it is not reachable at all rather than
    // hidden in one place and left bound to a button in another.
    #if defined(__XBOX__)
        return;
    #endif

    if (Controls::isJustPressed(Controls::Binding::Toggle_UncappedFps)) {
        PlayerPrefs::gbUncapFramerate = (!PlayerPrefs::gbUncapFramerate);
        gStatusBar.message = (PlayerPrefs::gbUncapFramerate) ? "Uncapped FPS." : "Original FPS.";
        gStatusBar.messageTicsLeft = 30;
    }
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Retrieves data from a specified file on a game disc with the associated file system.
// Starts reading the file from the specified offset and for the specified number of bytes.
// If the number of bytes specified is '-1' or below it means that all the data from the given offset should be read.
// Returns an empty data object on failure to read.
//------------------------------------------------------------------------------------------------------------------------------------------
DiscFileData getDiscFileData(
    const DiscInfo& discInfo,
    const IsoFileSys& isoFileSys,
    const char* const filePath,
    const uint32_t readOffset,
    const int32_t numBytesToRead
) noexcept {
    // Zero sized reads are always invalid
    if (numBytesToRead == 0)
        return {};

    // Retrieve the file system entry first and abort if that fails
    const IsoFileSysEntry* const pFsEntry = (filePath) ? isoFileSys.getEntry(filePath) : nullptr;
    const bool bValidFsEntry = (pFsEntry && (pFsEntry->size > 0));

    if (!bValidFsEntry)
        return {};

    // If the read starts past the end then it's invalid
    if (readOffset >= pFsEntry->size)
        return {};

    // Figure out the real size of the read ('-1' means all bytes past the offset).
    // If the read is out of bounds then fail the read also.
    const uint32_t realNumBytesToRead = (numBytesToRead < 0) ? pFsEntry->size - readOffset : (uint32_t) numBytesToRead;

    if (readOffset + realNumBytesToRead > pFsEntry->size)
        return {};

    // Do the read and abort if that failed
    std::unique_ptr<std::byte[]> fileBytes = std::make_unique<std::byte[]>(realNumBytesToRead);
    DiscReader discReader(discInfo);

    const bool bFileReadSuccess = (
        discReader.setTrackNum(1) &&
        discReader.trackSeekAbs((int32_t) pFsEntry->startLba * CDROM_SECTOR_SIZE + (int32_t) readOffset) &&
        discReader.read(fileBytes.get(), realNumBytesToRead)
    );

    if (!bFileReadSuccess)
        return {};

    // Success! Return the bytes just read:
    return { std::move(fileBytes), realNumBytesToRead };
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Gets the MD5 hash of a file on the given game disc (with associated file system) and returns 'true' if was successfully retrieved.
// The hash is returned as 2 64-bit words, with the bytes packed in the visual order that the MD5 would be read (as a string of bytes).
//------------------------------------------------------------------------------------------------------------------------------------------
bool getDiscFileMD5Hash(
    const DiscInfo& discInfo,
    const IsoFileSys& isoFileSys,
    const char* const filePath,
    uint64_t& hashWord1,
    uint64_t& hashWord2
) noexcept {
    // Get the data and abort if that fails
    DiscFileData data = getDiscFileData(discInfo, isoFileSys, filePath);

    if (!data.pBytes) {
        hashWord1 = {};
        hashWord2 = {};
        return false;
    }

    // Hash the data and turn the hash into 2 64-bit words
    MD5 md5Hasher;
    md5Hasher.reset();
    md5Hasher.add(data.pBytes.get(), data.numBytes);

    uint8_t md5[16] = {};
    md5Hasher.getHash(md5);

    hashWord1 = (
        ((uint64_t) md5[0 ] << 56) | ((uint64_t) md5[1 ] << 48) | ((uint64_t) md5[2 ] << 40) | ((uint64_t) md5[3 ] << 32) |
        ((uint64_t) md5[4 ] << 24) | ((uint64_t) md5[5 ] << 16) | ((uint64_t) md5[6 ] <<  8) | ((uint64_t) md5[7 ] <<  0)
    );

    hashWord2 = (
        ((uint64_t) md5[8 ] << 56) | ((uint64_t) md5[9 ] << 48) | ((uint64_t) md5[10] << 40) | ((uint64_t) md5[11] << 32) |
        ((uint64_t) md5[12] << 24) | ((uint64_t) md5[13] << 16) | ((uint64_t) md5[14] <<  8) | ((uint64_t) md5[15] <<  0)
    );

    return true;
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Checks if the specified file exists on a game disc (with associated file system) and whether it's MD5 hash matches the input hash.
// Returns 'true' if the file is found AND the hash matches.
// The MD5 hash is specified in the visual order that the MD5 would be read (as a string of bytes).
//------------------------------------------------------------------------------------------------------------------------------------------
bool checkDiscFileMD5Hash(
    const DiscInfo& discInfo,
    const IsoFileSys& isoFileSys,
    const char* const filePath,
    const uint64_t checkHashWord1,
    const uint64_t checkHashWord2
) noexcept {
    uint64_t actualHashWord1 = {};
    uint64_t actualHashWord2 = {};

    if (getDiscFileMD5Hash(discInfo, isoFileSys, filePath, actualHashWord1, actualHashWord2)) {
        return ((actualHashWord1 == checkHashWord1) && (actualHashWord2 == checkHashWord2));
    } else {
        return false;
    }
}

END_NAMESPACE(Utils)
