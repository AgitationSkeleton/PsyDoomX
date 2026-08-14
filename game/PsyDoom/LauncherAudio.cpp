#include "LauncherAudio.h"

#if defined(__XBOX__)

#include "DiscInfo.h"
#include "DiscReader.h"
#include "Game.h"
#include "LauncherAssets.h"
#include "PlayerPrefs.h"
#include "PsxVm.h"
#include "Spu.h"
#include "XboxAudioOut.h"

#include "Doom/Base/s_sound.h"
#include "Doom/Base/sounds.h"
#include "Doom/cdmaptbl.h"
#include "Doom/UI/o_main.h"
#include "Wess/psxcd.h"
#include "Wess/psxspu.h"
#include "Wess/wessapi.h"
#include "Wess/wessarc.h"
#include "Wess/wessseq.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

extern void xbLog(const char* msg) noexcept;

BEGIN_NAMESPACE(LauncherAudio)

//------------------------------------------------------------------------------------------------------------------------------------------
// What starting sound in the launcher actually involves, and why it is safe.
//
// 'PsxSoundInit' is the engine's audio startup and it wants a good deal in place first: an SPU, an open disc, that
// disc's file system, and the table saying where each file on it lives. All of that is 'PsxVm::init', which needs
// nothing from the game itself - the same property that made the styled menu possible is what makes this possible.
//
// It cannot be undone or repeated though. 'wess_load_module' fills a buffer with pointers into itself and the sample
// loader uploads to fixed addresses in SPU RAM; running it twice in one process is the same fault that made switching
// from Doom to Final Doom crash before each game was given a process of its own. So it runs once, and the music is
// deliberately kept out of it.
//------------------------------------------------------------------------------------------------------------------------------------------

static void audioLog(const char* const msg) noexcept {
    xbLog(msg);
}

// Has 'init' been attempted, and did it work?
static bool gbInitAttempted = false;
static bool gbReady = false;

//------------------------------------------------------------------------------------------------------------------------------------------
// The music player.
//
// A disc reader of its own, rather than the engine's. The engine's CD player reads through 'PsxVm::gDiscInfo', which is
// the one disc 'PsxVm::init' was given and cannot be repointed while a game would be reading it. The launcher has to be
// able to change discs whenever the style changes, so it keeps its own - and since all it ever does is play one looping
// track, that is about eighty lines rather than a subsystem.
//
// Track numbers are not assumed. They are read from each disc's own file system by 'LauncherAssets' and cached, the same
// way the artwork is - all three editions happen to put the menu music on track 3, but that is something the discs said
// rather than something relied upon.
//------------------------------------------------------------------------------------------------------------------------------------------
static constexpr int32_t CDDA_SECTOR_SIZE   = 2352;
static constexpr int32_t NUM_BUFFER_SAMPLES = CDDA_SECTOR_SIZE / (int32_t) sizeof(int16_t);

static DiscInfo    gMusicDisc;
static DiscReader  gMusicReader(gMusicDisc);
static bool        gbMusicPlay      = false;
static int32_t     gMusicBufOffset  = NUM_BUFFER_SAMPLES;
static int16_t     gMusicBuf[NUM_BUFFER_SAMPLES];

// Guards everything above.
//
// The audio thread takes the SPU lock and then this one; so the main thread must never do the reverse, which is why
// nothing here calls into the SPU while holding it. Same rule, and the same reason, as the engine's own CD player.
static std::recursive_mutex gMusicMutex;

//------------------------------------------------------------------------------------------------------------------------------------------
// Called by the SPU on the audio thread when it wants a sample of external input
//------------------------------------------------------------------------------------------------------------------------------------------
static Spu::StereoSample musicCallback([[maybe_unused]] void* pUserData) noexcept {
    // Never wait here. The audio thread already holds the SPU lock, and blocking on a lock the main thread holds is how
    // an AB-BA deadlock silences everything for the rest of the session; a missing sample is inaudible.
    if (!gMusicMutex.try_lock())
        return Spu::StereoSample{};

    struct Unlock { ~Unlock() noexcept { gMusicMutex.unlock(); } } unlock;

    if ((!gbMusicPlay) || (!gMusicReader.isTrackOpen()))
        return Spu::StereoSample{};

    if (gMusicBufOffset + 1 >= NUM_BUFFER_SAMPLES) {
        const DiscTrack* const pTrack = gMusicReader.getOpenTrack();
        const int32_t trackSize = pTrack->trackPayloadSize;
        int32_t trackOffset = gMusicReader.tell();

        // The menu's music loops, the way it does in the game
        if (trackOffset >= trackSize) {
            gMusicReader.trackSeekAbs(0);
            trackOffset = gMusicReader.tell();
        }

        const int32_t samplesToRead = std::min<int32_t>((trackSize - trackOffset) / (int32_t) sizeof(int16_t), NUM_BUFFER_SAMPLES);

        if (samplesToRead > 0) {
            gMusicReader.read(gMusicBuf, samplesToRead * (int32_t) sizeof(int16_t));
        }

        // Anything the last sector was short by is silence rather than whatever was there before
        if (samplesToRead < NUM_BUFFER_SAMPLES) {
            std::memset(gMusicBuf + samplesToRead, 0, (size_t)(NUM_BUFFER_SAMPLES - samplesToRead) * sizeof(int16_t));
        }

        gMusicBufOffset = 0;
    }

    const Spu::StereoSample sample = { gMusicBuf[gMusicBufOffset], gMusicBuf[gMusicBufOffset + 1] };
    gMusicBufOffset += 2;
    return sample;
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Bring the sound system up
//------------------------------------------------------------------------------------------------------------------------------------------
bool init(const char* const cuePath) noexcept {
    if (gbInitAttempted)
        return gbReady;

    // Set before anything else: whether this works or not, it is not to be tried a second time
    gbInitAttempted = true;

    if ((!cuePath) || (cuePath[0] == '\0'))
        return false;

    // The player's own volume settings, read from the same file the game reads them from.
    //
    // Worth doing rather than picking a level here: a player who has turned the music down has said what they want, and
    // a launcher that ignores that is louder than the game they are about to start.
    PlayerPrefs::load();
    PlayerPrefs::pushSoundAndMusicPrefs();

    audioLog("launcher audio: starting the emulated PlayStation audio hardware");

    if (!PsxVm::init(cuePath)) {
        audioLog("launcher audio: the PSX components would not start - the menu stays silent");
        return false;
    }

    // Which game this is, and where each file on its disc lives. 'PsxSoundInit' opens files by name and needs both.
    Game::determineGameTypeAndVariant();
    CdMapTbl_Init();

    // The .WMD is read into a temporary buffer and then unpacked into one of its own, so its size has to be asked for
    // first. Doom's is 55,502 bytes and the Master Edition's 106,496, so a fixed buffer would have to fit the largest.
    const int32_t wmdFileSize = psxcd_get_file_size(CdFile::DOOMSND_WMD);
    void* const pWmdBuf = std::malloc((wmdFileSize > 0) ? (size_t) wmdFileSize : 1u);

    if (!pWmdBuf) {
        audioLog("launcher audio: no memory for the .WMD - the menu stays silent");
        return false;
    }

    PsxSoundInit(doomToWessVol(gOptionsSndVol), doomToWessVol(gOptionsMusVol), pWmdBuf);
    std::free(pWmdBuf);

    // Take the SPU's external input over from the engine's CD player.
    //
    // 'PsxSoundInit' installs that player as part of starting up, and it reads through the one disc this process was
    // given. Replacing it here is what lets the menu's music follow the style onto another disc.
    {
        PsxVm::LockSpu spuLock;
        PsxVm::gSpu.pExtInputCallback = musicCallback;
        PsxVm::gSpu.pExtInputUserData = nullptr;
    }

    // External input is mixed only when asked for. The engine does this as part of starting a track; nothing does it
    // here, so without it the music plays into silence and looks like a disc that could not be read.
    psxspu_setcdmixon();

    gbReady = true;
    audioLog("launcher audio: ready");
    return true;
}

bool isReady() noexcept {
    return gbReady;
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Music
//------------------------------------------------------------------------------------------------------------------------------------------
void stopMusic() noexcept {
    std::lock_guard<std::recursive_mutex> lock(gMusicMutex);
    gbMusicPlay = false;
    gMusicReader.closeTrack();
}

void playMenuMusic(const char* const cuePath) noexcept {
    if (!gbReady)
        return;

    if ((!cuePath) || (cuePath[0] == '\0')) {
        stopMusic();
        return;
    }

    // Which track, according to the disc rather than according to a guess
    const int32_t trackNum = LauncherAssets::cachedMusicTrack(cuePath);

    if (trackNum <= 1) {
        // Track one is the data track, so anything at or below it means the lookup found nothing
        stopMusic();
        return;
    }

    // Parsed before the lock is taken, because this reads a file and the audio thread should not be held off while it
    // does. It is only a .cue - a few hundred bytes - but the reader is being used sixty times a second.
    DiscInfo parsedDisc;
    std::string errorMsg;

    if (!parsedDisc.parseFromCueFile(cuePath, errorMsg)) {
        audioLog("launcher audio: could not read the disc for the menu music");
        stopMusic();
        return;
    }

    {
        std::lock_guard<std::recursive_mutex> lock(gMusicMutex);

        // Close first. The reader holds a pointer into the old track list, and that list is about to be replaced.
        gbMusicPlay = false;
        gMusicReader.closeTrack();
        gMusicDisc = std::move(parsedDisc);

        if (!gMusicReader.setTrackNum(trackNum)) {
            audioLog("launcher audio: the menu music track would not open");
            return;
        }

        gMusicReader.trackSeekAbs(0);
        gMusicBufOffset = NUM_BUFFER_SAMPLES;    // Nothing buffered yet, so the callback reads a sector first
        gbMusicPlay = true;
    }
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Sound effects.
//
// The same three the game's own main menu uses, so the launcher sounds like part of it rather than like something in
// front of it. These are triggered directly rather than through 'S_StartSound', which queues sounds to be played at the
// end of a game tick - there are no ticks here.
//------------------------------------------------------------------------------------------------------------------------------------------
void playMoveSound() noexcept {
    if (gbReady) { wess_seq_trigger(sfx_pstop); }
}

void playSelectSound() noexcept {
    if (gbReady) { wess_seq_trigger(sfx_swtchx); }
}

void playConfirmSound() noexcept {
    if (gbReady) { wess_seq_trigger(sfx_pistol); }
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Keep the sequencer moving.
//
// On the PlayStation a hardware timer drove this at 120 Hz. PsyDoom drives it from the main loop instead, with a real
// elapsed time, and the launcher has to do the same or nothing it triggers is ever heard.
//------------------------------------------------------------------------------------------------------------------------------------------
void update() noexcept {
    if (!gbReady)
        return;

    PsxVm::generateTimerEvents();

    if (gbWess_SeqOn) {
        SeqEngine();
    }
}

void shutdown() noexcept {
    if (!gbReady)
        return;

    stopMusic();
    psxspu_setcdmixoff();
    XboxAudioOut::stop();
}

END_NAMESPACE(LauncherAudio)

#endif  // #if defined(__XBOX__)
