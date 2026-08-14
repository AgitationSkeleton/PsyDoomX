#include "LauncherAssets.h"

#if defined(__XBOX__)

#include "DiscInfo.h"
#include "DiscReader.h"
#include "IsoFileSys.h"
#include "PlayerColour.h"
#include "SsgStyle.h"
#include "WadUtils.h"

#include "Doom/Base/i_misc.h"
#include "Doom/UI/m_main.h"

#include <hal/video.h>
#include <windows.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

extern void xbLog(const char* msg) noexcept;

BEGIN_NAMESPACE(LauncherAssets)

// The sizes the header hands out have to be the game's own sizes doubled, or a repaint would cover the wrong area
static_assert(CURSOR_DRAW_W == M_SKULL_W * 2);
static_assert(CURSOR_DRAW_H == M_SKULL_H * 2);
static_assert(BIG_TEXT_DRAW_H == BIG_FONT_LINE_HEIGHT * 2);

//------------------------------------------------------------------------------------------------------------------------------------------
// Reading a game's disc from the launcher, without starting that game.
//
// The launcher and the game are the same executable, so the disc handling the game uses is already here - and the parts
// that matter are standalone. 'DiscInfo::parseFromCueFile' and 'IsoFileSys::build' need nothing initialised and no
// edition chosen, which is what makes a styled launcher possible at all: it can look inside any edition's disc to take
// the menu it should be wearing.
//
// This step only proves that. It opens a disc, builds its file system, finds the WAD and says how big it is. Nothing is
// decoded and nothing is drawn yet, because if this does not work none of that could, and if it does then every later
// step is about the contents rather than the access.
//
// Not portable: paths and logging are this console's.
//------------------------------------------------------------------------------------------------------------------------------------------

static void assetLog(const char* const fmt, ...) noexcept {
    char msg[256];

    va_list args;
    va_start(args, fmt);
    std::vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    xbLog(msg);
}

// Read one lump out of an open disc, inflating it if it is compressed
static bool readLump(
    DiscReader& discReader,
    const uint32_t wadStartLba,
    const int32_t lumpOffset,
    const int32_t lumpSize,
    const bool bCompressed,
    std::vector<std::byte>& dataOut
) noexcept {
    dataOut.clear();

    if ((lumpSize <= 0) || (!discReader.trackSeekAbs(((int32_t) wadStartLba * 2048) + lumpOffset)))
        return false;

    std::vector<std::byte> packed((size_t) lumpSize);

    if (!discReader.read(packed.data(), lumpSize))
        return false;

    const int32_t size = (bCompressed) ? WadUtils::getDecompressedLumpSize(packed.data()) : lumpSize;

    if ((size <= 0) || (size > 4 * 1024 * 1024))
        return false;

    dataOut.resize((size_t) size);

    if (bCompressed) {
        WadUtils::decompressLump(packed.data(), dataOut.data());
    } else {
        std::memcpy(dataOut.data(), packed.data(), (size_t) size);
    }

    return true;
}

// Where a disc's decoded assets are kept.
//
// Named after the disc's own file name, so the three editions cannot overwrite each other's cache.
static void cacheDirForCue(const char* const cuePath, char* const dirOut, const size_t dirOutSize) noexcept {
    const char* const pLastSlash = std::strrchr(cuePath, '\\');
    const char* const pLeaf = (pLastSlash) ? (pLastSlash + 1) : cuePath;
    std::snprintf(dirOut, dirOutSize, "E:\\Apps\\PsyDoomX\\cache\\%s", pLeaf);
}

// Write a decoded image beside the executable, so each disc is read once rather than on every boot
static bool writeCache(const char* const cacheDir, const char* const name, const void* const pData, const int32_t size) noexcept {
    CreateDirectoryA("E:\\Apps\\PsyDoomX\\cache", nullptr);
    CreateDirectoryA(cacheDir, nullptr);

    char path[260];
    std::snprintf(path, sizeof(path), "%s\\%s", cacheDir, name);

    HANDLE const h = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

    if (h == INVALID_HANDLE_VALUE)
        return false;

    DWORD written = 0;
    const bool bOk = (WriteFile(h, pData, (DWORD) size, &written, nullptr) && (written == (DWORD) size));
    CloseHandle(h);
    return bOk;
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Write out this disc's super shotgun sprites, so the other editions can borrow them.
//
// The ten first person frames, 'SHT2A0' to 'SHT2J0', copied byte for byte and renamed. They are compressed on the disc
// and stay that way: the top bit of a lump name's first character is the WAD's 'this lump is compressed' flag, so
// carrying it across with the data means nothing has to be unpacked here or repacked. The game's own WAD reader then
// treats them exactly as it treats the lumps they came from.
//
// Renamed rather than left as 'SHT2' so a game can hold its own set and the borrowed sets at the same time, which is
// what makes the setting take effect immediately instead of on the next start. Wrapped in 'S_START' and 'S_END' because
// that is how the sprite scanner finds sprites at all - lumps outside that range are not sprites as far as it is
// concerned, whatever they are named.
//
// The size in a WAD's directory is the size the lump is once unpacked, NOT the number of bytes it occupies. How many
// bytes to actually read is the distance to the next entry's offset - that is what 'WadFile::getRawSize' does. So
// 'SHT2A0' on the Doom disc reads 1,644 in its directory entry while occupying 800 bytes. Both numbers are needed here
// and they are not interchangeable: the directory has to carry the unpacked size across, the copying has to use the
// occupied size, and the end marker has to sit at the end of the data so the last frame's size can be worked out the
// same way.
//------------------------------------------------------------------------------------------------------------------------------------------
static bool writeSsgWad(
    DiscReader& discReader,
    const uint32_t wadStartLba,
    const char* const ssgSpriteName,
    const int32_t (&lumpOffsets)[SsgStyle::NUM_FRAMES],
    const int32_t (&lumpUnpackedSizes)[SsgStyle::NUM_FRAMES],
    const int32_t (&lumpRawSizes)[SsgStyle::NUM_FRAMES],
    const bool (&bLumpCompressed)[SsgStyle::NUM_FRAMES]
) noexcept {
    struct OutLump {
        int32_t offset;
        int32_t size;
        char    name[8];
    };

    static constexpr int32_t NUM_OUT_LUMPS = SsgStyle::NUM_FRAMES + 2;      // The two markers either side
    static constexpr int32_t HEADER_SIZE = 12;                              // Id, lump count, directory offset

    OutLump dir[NUM_OUT_LUMPS] = {};
    std::vector<std::byte> body;

    // The opening marker carries no data of its own, and sits where the first frame does so its own size works out as
    // zero rather than as the whole of the first frame
    std::memcpy(dir[0].name, "S_START", 7);
    dir[0].offset = HEADER_SIZE;

    for (int32_t i = 0; i < SsgStyle::NUM_FRAMES; ++i) {
        if ((lumpRawSizes[i] <= 0) || (lumpRawSizes[i] > 256 * 1024))
            return false;

        if ((lumpUnpackedSizes[i] <= 0) || (lumpUnpackedSizes[i] > 1024 * 1024))
            return false;

        if (!discReader.trackSeekAbs(((int32_t) wadStartLba * 2048) + lumpOffsets[i]))
            return false;

        std::vector<std::byte> lumpData((size_t) lumpRawSizes[i]);

        if (!discReader.read(lumpData.data(), lumpRawSizes[i]))
            return false;

        OutLump& out = dir[1 + i];
        out.offset = HEADER_SIZE + (int32_t) body.size();
        out.size = lumpUnpackedSizes[i];

        std::memcpy(out.name, ssgSpriteName, 4);
        out.name[4] = (char)('A' + i);      // Frame
        out.name[5] = '0';                  // Every angle uses the one image, which is what a weapon sprite is

        if (bLumpCompressed[i]) {
            out.name[0] = (char)(out.name[0] | 0x80);
        }

        body.insert(body.end(), lumpData.begin(), lumpData.end());
    }

    // Assemble the file: header, then the lump data, then the directory
    const int32_t dirOffset = HEADER_SIZE + (int32_t) body.size();

    // The closing marker sits at the end of the data, which is what gives the last frame its size
    OutLump& endMarker = dir[NUM_OUT_LUMPS - 1];
    std::memcpy(endMarker.name, "S_END", 5);
    endMarker.offset = dirOffset;

    std::vector<std::byte> file;
    file.reserve((size_t)(dirOffset + sizeof(dir)));

    {
        char header[HEADER_SIZE];
        std::memcpy(header, "IWAD", 4);
        const int32_t numLumps = NUM_OUT_LUMPS;
        std::memcpy(header + 4, &numLumps, sizeof(numLumps));
        std::memcpy(header + 8, &dirOffset, sizeof(dirOffset));
        file.insert(file.end(), (const std::byte*) header, (const std::byte*) header + HEADER_SIZE);
    }

    file.insert(file.end(), body.begin(), body.end());
    file.insert(file.end(), (const std::byte*) dir, (const std::byte*) dir + sizeof(dir));

    // Named after the sprite it holds, lowercased, which is what 'SsgStyle::wadPath' expects to find
    char fileName[32];
    std::snprintf(fileName, sizeof(fileName), "%s.wad", ssgSpriteName);

    for (char* p = fileName; *p; ++p) {
        if ((*p >= 'A') && (*p <= 'Z')) {
            *p = (char)(*p - 'A' + 'a');
        }
    }

    return writeCache("E:\\Apps\\PsyDoomX\\cache", fileName, file.data(), (int32_t) file.size());
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Write out recoloured copies of this disc's marine sprites, so players can be told apart.
//
// PC Doom recolours a player by remapping sixteen palette entries as each column is drawn - see 'PlayerColour.h' for
// the table it uses. The PlayStation renderer has no per column step to hang that on, so the same remap is done here
// instead, once, on the pixels themselves.
//
// Which means the lumps have to be unpacked rather than copied. A packed lump is a byte stream, not an image; remapping
// its bytes would corrupt it rather than recolour it. So each one is decompressed, remapped, and written out plain -
// which is also why the compression flag is cleared on the way out. Fifty one frames at about 104 KiB unpacked, three
// times over, is a little over 300 KiB per edition.
//------------------------------------------------------------------------------------------------------------------------------------------
// An eight character lump name. A bare array cannot go in a vector - it is not assignable - so it travels in this.
struct LumpName8 {
    char chars[8];
};

static bool writePlayerColourWad(
    DiscReader& discReader,
    const uint32_t wadStartLba,
    const char* const outPath,
    const std::vector<LumpName8>& lumpNames,
    const std::vector<int32_t>& lumpOffsets,
    const std::vector<int32_t>& lumpUnpackedSizes,
    const std::vector<int32_t>& lumpRawSizes,
    const std::vector<bool>& bLumpCompressed
) noexcept {
    struct OutLump {
        int32_t offset;
        int32_t size;
        char    name[8];
    };

    static constexpr int32_t HEADER_SIZE = 12;

    const int32_t numFrames = (int32_t) lumpOffsets.size();

    if (numFrames <= 0)
        return false;

    // Unpack every frame once, then write a recoloured copy of the set for each colour that needs one
    std::vector<std::vector<std::byte>> frames((size_t) numFrames);

    for (int32_t i = 0; i < numFrames; ++i) {
        if (!readLump(discReader, wadStartLba, lumpOffsets[i], lumpRawSizes[i], bLumpCompressed[i], frames[(size_t) i]))
            return false;

        if ((int32_t) frames[(size_t) i].size() != lumpUnpackedSizes[i])
            return false;
    }

    std::vector<OutLump> dir;
    std::vector<std::byte> body;

    {
        OutLump& startMarker = dir.emplace_back();
        std::memcpy(startMarker.name, "S_START", 7);
        startMarker.offset = HEADER_SIZE;
    }

    for (int32_t colour = 0; colour < PlayerColour::COLOUR_COUNT; ++colour) {
        // Green is what the sprites already are; a copy of them would only take up room
        if ((PlayerColour::Colour) colour == PlayerColour::GREEN)
            continue;

        const int32_t rampStart = PlayerColour::rampStart((PlayerColour::Colour) colour);
        const char* const pSpriteName = PlayerColour::spriteName((PlayerColour::Colour) colour);

        for (int32_t i = 0; i < numFrames; ++i) {
            std::vector<std::byte> pixels = frames[(size_t) i];

            // The eight byte header - two offsets, a width and a height - is not pixels and must not be touched
            static constexpr size_t TEX_HEADER_SIZE = 8;

            if (pixels.size() <= TEX_HEADER_SIZE)
                return false;

            for (size_t p = TEX_HEADER_SIZE; p < pixels.size(); ++p) {
                const uint32_t idx = (uint32_t) pixels[p];

                // Only the sixteen greens, exactly as 'R_InitTranslationTables' does it. Everything else is left alone,
                // which is what keeps the marine's brown boots brown and the visor's blue blue.
                if ((idx >= (uint32_t) PlayerColour::GREEN_RAMP_START) &&
                    (idx < (uint32_t)(PlayerColour::GREEN_RAMP_START + PlayerColour::RAMP_LENGTH)))
                {
                    pixels[p] = (std::byte)((uint32_t) rampStart + (idx & 0xFu));
                }
            }

            OutLump& out = dir.emplace_back();
            out.offset = HEADER_SIZE + (int32_t) body.size();
            out.size = (int32_t) pixels.size();

            // The sprite name changes, the frame and rotation letters after it do not
            std::memcpy(out.name, lumpNames[(size_t) i].chars, 8);
            std::memcpy(out.name, pSpriteName, 4);

            body.insert(body.end(), pixels.begin(), pixels.end());
        }
    }

    const int32_t dirOffset = HEADER_SIZE + (int32_t) body.size();

    {
        OutLump& endMarker = dir.emplace_back();
        std::memcpy(endMarker.name, "S_END", 5);
        endMarker.offset = dirOffset;
    }

    std::vector<std::byte> file;
    file.reserve((size_t) dirOffset + dir.size() * sizeof(OutLump));

    {
        char header[HEADER_SIZE];
        std::memcpy(header, "IWAD", 4);
        const int32_t numLumps = (int32_t) dir.size();
        std::memcpy(header + 4, &numLumps, sizeof(numLumps));
        std::memcpy(header + 8, &dirOffset, sizeof(dirOffset));
        file.insert(file.end(), (const std::byte*) header, (const std::byte*) header + HEADER_SIZE);
    }

    file.insert(file.end(), body.begin(), body.end());
    file.insert(file.end(), (const std::byte*) dir.data(), (const std::byte*) (dir.data() + dir.size()));

    // Split back into a directory and a file name, since that is what the cache writer takes
    const char* const pLastSlash = std::strrchr(outPath, '\\');

    if (!pLastSlash)
        return false;

    char dirPath[260] = {};
    std::memcpy(dirPath, outPath, (size_t)(pLastSlash - outPath));

    return writeCache(dirPath, pLastSlash + 1, file.data(), (int32_t) file.size());
}

bool probeDisc(const char* const cuePath, const MenuArt& menuArt, const int32_t ssgStyle) noexcept {
    if ((!cuePath) || (cuePath[0] == '\0'))
        return false;

    // The disc's table of contents
    DiscInfo discInfo;
    std::string errorMsg;

    if (!discInfo.parseFromCueFile(cuePath, errorMsg)) {
        assetLog("launcher assets: could not read '%s' - %s", cuePath, errorMsg.c_str());
        return false;
    }

    // And its file system
    IsoFileSys fileSys;
    {
        DiscReader discReader(discInfo);

        if (!fileSys.build(discReader)) {
            assetLog("launcher assets: '%s' has no readable file system", cuePath);
            return false;
        }
    }

    // Which track the menu music is on, taken from the disc rather than assumed.
    //
    // The '.RAW' files in 'PSXDOOM/CDAUDIO' are placeholders: each one's start sector falls inside the audio track that
    // actually holds that piece of music, which is how the game itself works out its track numbers. Reading the three
    // discs here says main menu music is track 3 on all of them - but that is what they said, not what was assumed, and
    // a demo disc would not agree.
    //
    // Cached alongside the artwork so that changing the menu style does not mean walking a disc's file system again.
    {
        const IsoFileSysEntry* pMusicEntry = fileSys.getEntry("PSXDOOM/CDAUDIO/DMSELECT.RAW");

        if (!pMusicEntry) {
            pMusicEntry = fileSys.getEntry("PSXDOOM/CDAUDIO/SAMPMAIN.RAW");     // What some demo discs call it
        }

        const int32_t musicTrack = (pMusicEntry) ? discInfo.getSectorTrack(pMusicEntry->startLba) : 0;

        char cacheDir[260];
        cacheDirForCue(cuePath, cacheDir, sizeof(cacheDir));

        char trackStr[16];
        const int trackStrLen = std::snprintf(trackStr, sizeof(trackStr), "%d", (int) musicTrack);
        writeCache(cacheDir, "music.txt", trackStr, trackStrLen);

        assetLog("launcher assets: '%s' menu music is on track %d", cuePath, (int) musicTrack);
    }

    // The WAD everything the menu needs lives in
    const IsoFileSysEntry* const pWadEntry = fileSys.getEntry("PSXDOOM/ABIN/PSXDOOM.WAD");

    if (!pWadEntry) {
        assetLog("launcher assets: '%s' has no PSXDOOM.WAD where one is expected", cuePath);
        return false;
    }

    assetLog(
        "launcher assets: '%s' opened - WAD at lba %u, %u bytes",
        cuePath,
        (unsigned) pWadEntry->startLba,
        (unsigned) pWadEntry->size
    );

    // Now the lumps the menu is made of.
    //
    // A WAD is a header saying how many lumps there are and where their directory starts, then that directory: name,
    // offset and size for each. Reading it needs no engine and no game - which is the whole reason this is possible
    // from the launcher.
    {
        DiscReader discReader(discInfo);

        // Track one, which is where the data lives on these discs - the same track the file system was built from
        if (!discReader.setTrackNum(1)) {
            assetLog("launcher assets: could not select the data track");
            return false;
        }

        if (!discReader.trackSeekAbs((int32_t) pWadEntry->startLba * 2048)) {
            assetLog("launcher assets: could not seek to the WAD");
            return false;
        }

        struct WadHeader {
            char        fileId[4];      // 'IWAD' or 'PWAD'
            int32_t     numLumps;
            int32_t     dirOffset;
        };

        WadHeader header = {};

        if (!discReader.read(&header, sizeof(header))) {
            assetLog("launcher assets: could not read the WAD header");
            return false;
        }

        assetLog(
            "launcher assets:   id '%c%c%c%c', %d lumps, directory at %d",
            header.fileId[0], header.fileId[1], header.fileId[2], header.fileId[3],
            (int) header.numLumps,
            (int) header.dirOffset
        );

        if ((header.numLumps <= 0) || (header.numLumps > 20000))
            return false;

        // The directory, and whether what the menu needs is in it
        if (!discReader.trackSeekAbs(((int32_t) pWadEntry->startLba * 2048) + header.dirOffset))
            return false;

        struct WadLump {
            int32_t     offset;
            int32_t     size;
            char        name[8];
        };

        int foundBack = -1, foundDoom = -1, foundStatus = -1, foundPal = -1;
        WadLump backLump = {}, doomLump = {}, statusLump = {}, palLump = {};
        bool bBackCompressed = false, bPalCompressed = false, bStatusCompressed = false;

        // The super shotgun's ten first person frames, collected as the directory goes by.
        //
        // Two sizes are kept because a WAD directory only gives one of them. The size it records is what the lump
        // unpacks to; how many bytes it actually occupies is the distance to the next entry, which is only known once
        // the next entry has been read - hence 'pendingSsgFrame', which is settled on the pass after.
        int32_t ssgOffsets[SsgStyle::NUM_FRAMES] = {};
        int32_t ssgUnpackedSizes[SsgStyle::NUM_FRAMES] = {};
        int32_t ssgRawSizes[SsgStyle::NUM_FRAMES] = {};
        bool bSsgCompressed[SsgStyle::NUM_FRAMES] = {};
        int32_t numSsgFramesFound = 0;
        int32_t pendingSsgFrame = -1;

        // The marine's own frames, for telling players apart by colour. Same two-sizes problem as above, so the same
        // 'settle it on the next entry' handling.
        std::vector<LumpName8> playNames;
        std::vector<int32_t> playOffsets;
        std::vector<int32_t> playUnpackedSizes;
        std::vector<int32_t> playRawSizes;
        std::vector<bool> bPlayCompressed;
        bool bPlayPending = false;

        for (int32_t i = 0; i < header.numLumps; ++i) {
            WadLump lump = {};

            if (!discReader.read(&lump, sizeof(lump)))
                break;

            // How many bytes the frame before this one actually occupies: the distance from it to this entry
            if (pendingSsgFrame >= 0) {
                ssgRawSizes[pendingSsgFrame] = lump.offset - ssgOffsets[pendingSsgFrame];
                pendingSsgFrame = -1;
            }

            if (bPlayPending) {
                playRawSizes.back() = lump.offset - playOffsets.back();
                bPlayPending = false;
            }

            // Names are eight characters, padded rather than terminated, and the top bit of the first marks compression
            char name[9] = {};
            std::memcpy(name, lump.name, 8);
            const bool bCompressed = ((name[0] & 0x80) != 0);
            name[0] = (char)(name[0] & 0x7F);

            if (std::strncmp(name, menuArt.lumpName, 8) == 0) { foundBack = i; backLump = lump; bBackCompressed = bCompressed; }
            if (std::strncmp(name, "DOOM", 8) == 0)     { foundDoom = i;   doomLump = lump;   }
            if (std::strncmp(name, "STATUS", 8) == 0)   { foundStatus = i; statusLump = lump; bStatusCompressed = bCompressed; }
            if (std::strncmp(name, "PLAYPAL", 8) == 0)  { foundPal = i;    palLump = lump;    bPalCompressed = bCompressed; }

            // The super shotgun's first person frames: 'SHT2A0' through 'SHT2J0'.
            //
            // Matched on the whole name rather than a prefix. 'SHT2' is also the start of nothing else here, but a
            // prefix match would take a frame beyond 'J' or a rotation other than zero and write it into the wrong
            // slot, which would show up as one frame of the animation being another frame of it.
            if ((std::strncmp(name, "SHT2", 4) == 0) && (name[5] == '0') && (name[6] == '\0')) {
                const int32_t frameIdx = (int32_t)(name[4] - 'A');

                if ((frameIdx >= 0) && (frameIdx < SsgStyle::NUM_FRAMES) && (ssgUnpackedSizes[frameIdx] == 0)) {
                    ssgOffsets[frameIdx] = lump.offset;
                    ssgUnpackedSizes[frameIdx] = lump.size;
                    bSsgCompressed[frameIdx] = bCompressed;
                    numSsgFramesFound++;
                    pendingSsgFrame = frameIdx;     // Its occupied size is settled by whatever entry comes next
                }
            }

            // The marine, whose frames run from 'PLAYA1' to 'PLAYW0'.
            //
            // 'PLAYPAL' begins with the same four letters and is the palette rather than a sprite, so it is excluded by
            // name. Everything else starting 'PLAY' is a frame of the player.
            if ((std::strncmp(name, "PLAY", 4) == 0) && (std::strncmp(name, "PLAYPAL", 8) != 0)) {
                playNames.emplace_back();
                std::memcpy(playNames.back().chars, lump.name, 8);
                playNames.back().chars[0] = (char)(playNames.back().chars[0] & 0x7F);    // Drop the compression flag from the name

                playOffsets.push_back(lump.offset);
                playUnpackedSizes.push_back(lump.size);
                playRawSizes.push_back(lump.size);      // Settled properly by the next entry
                bPlayCompressed.push_back(bCompressed);
                bPlayPending = true;
            }

            // Anything that might be a menu background, by name.
            //
            // The Master Edition's menu is a different picture entirely - not the warped skulls the other two share -
            // so its background is not simply 'BACK' with another palette, and its settings come from a MAPINFO on the
            // disc rather than from anything hardcoded. Listing the candidates says what it is actually called.
            if ((std::strstr(name, "BACK") != nullptr) ||
                (std::strstr(name, "TITLE") != nullptr) ||
                (std::strstr(name, "MARB") != nullptr))
            {
                assetLog("launcher assets:     candidate '%s' at %d, %d bytes%s",
                    name, (int) i, (int) lump.size, bCompressed ? " (compressed)" : "");
            }
        }

        assetLog(
            "launcher assets:   %s=%d DOOM=%d STATUS=%d (-1 means absent)",
            menuArt.lumpName, foundBack, foundDoom, foundStatus
        );

        // The super shotgun, for the cross edition setting.
        //
        // All ten frames or none: a partial set would leave the sprite with a gap in its frames, and the sprite scanner
        // treats that as an error rather than as a missing picture.
        if ((ssgStyle >= 0) && (ssgStyle < SsgStyle::STYLE_COUNT)) {
            // A frame that was the very last entry in the directory would have nothing after it to measure against.
            // That does not happen on these discs - the super shotgun sits at entry 20 of well over a thousand - but a
            // zero here would be read as "no data" rather than as "not measured", so it is settled rather than left.
            if (pendingSsgFrame >= 0) {
                ssgRawSizes[pendingSsgFrame] = ssgUnpackedSizes[pendingSsgFrame];
                pendingSsgFrame = -1;
            }

            if (bPlayPending) {
                bPlayPending = false;   // Already holds the unpacked size, which is the right answer when uncompressed
            }

            if (numSsgFramesFound == SsgStyle::NUM_FRAMES) {
                const char* const pSpriteName = SsgStyle::spriteName((SsgStyle::Style) ssgStyle);

                const bool bWroteSsg = writeSsgWad(
                    discReader, pWadEntry->startLba, pSpriteName, ssgOffsets, ssgUnpackedSizes, ssgRawSizes, bSsgCompressed
                );

                assetLog(
                    "launcher assets:   super shotgun as '%s', frame A %d bytes packed / %d unpacked, written=%s",
                    pSpriteName, (int) ssgRawSizes[0], (int) ssgUnpackedSizes[0], bWroteSsg ? "yes" : "NO"
                );
            } else {
                assetLog("launcher assets:   super shotgun has only %d of %d frames - not written",
                    (int) numSsgFramesFound, (int) SsgStyle::NUM_FRAMES);
            }

            // The recoloured marines, for telling players apart
            if (!playOffsets.empty()) {
                const bool bWrotePlayer = writePlayerColourWad(
                    discReader,
                    pWadEntry->startLba,
                    PlayerColour::wadPathForEdition(ssgStyle),
                    playNames, playOffsets, playUnpackedSizes, playRawSizes, bPlayCompressed
                );

                assetLog(
                    "launcher assets:   %d marine frames recoloured %d ways, written=%s",
                    (int) playOffsets.size(), (int) PlayerColour::COLOUR_COUNT - 1, bWrotePlayer ? "yes" : "NO"
                );
            }
        }

        // The background, decoded through the palette and cached.
        //
        // 'BACK' is 8bpp: one byte per pixel, each an index into a palette rather than a colour of its own. 256x240 is
        // 61,440 bytes and the lump is 61,448 - that plus the eight byte header and nothing else - which is what says
        // the format outright rather than by assumption.
        //
        // The palette is 'PLAYPAL', a list of 256 colour tables, and the menu is drawn with the first of them. Decoding
        // it here means the launcher never has to do it again: what gets written out is finished 16-bit pixels.
        if ((foundBack >= 0) && (foundPal >= 0)) {
            std::vector<std::byte> backData;
            std::vector<std::byte> palData;

            const bool bReadOk = (
                readLump(discReader, pWadEntry->startLba, backLump.offset, backLump.size, bBackCompressed, backData) &&
                readLump(discReader, pWadEntry->startLba, palLump.offset, palLump.size, bPalCompressed, palData)
            );

            if (bReadOk && (backData.size() > 8) && (palData.size() >= 512)) {
                struct TexHeader {
                    int16_t offsetX;
                    int16_t offsetY;
                    int16_t width;
                    int16_t height;
                };

                TexHeader texHeader = {};
                std::memcpy(&texHeader, backData.data(), sizeof(texHeader));

                const int32_t w = texHeader.width;
                const int32_t h = texHeader.height;

                if ((w > 0) && (h > 0) && (backData.size() >= sizeof(TexHeader) + (size_t)(w * h))) {
                    // The edition's own palette, not simply the first one
                    const uint32_t numPalettes = (uint32_t)(palData.size() / 512);
                    const uint32_t palIdx = (menuArt.palette < numPalettes) ? menuArt.palette : 0;
                    const uint16_t* const pPalette = ((const uint16_t*) palData.data()) + (palIdx * 256);
                    const uint8_t* const pIndices = (const uint8_t*)(backData.data() + sizeof(TexHeader));

                    std::vector<uint16_t> pixels((size_t)(w * h));

                    for (size_t i = 0; i < pixels.size(); ++i) {
                        pixels[i] = pPalette[pIndices[i]];
                    }

                    // Named after the disc it came from, so the three editions do not overwrite each other
                    char cacheDir[260];
                    cacheDirForCue(cuePath, cacheDir, sizeof(cacheDir));

                    const bool bCached = writeCache(
                        cacheDir, "back.raw", pixels.data(), (int32_t)(pixels.size() * sizeof(uint16_t))
                    );

                    assetLog(
                        "launcher assets:   %s %dx%d decoded with palette %u of %u, cached=%s",
                        menuArt.lumpName, (int) w, (int) h,
                        (unsigned) palIdx,
                        (unsigned) numPalettes,
                        bCached ? "yes" : "NO"
                    );
                }
            }
            else {
                assetLog("launcher assets:   could not read the background and PLAYPAL together");
            }

            // The STATUS atlas: the font and the menu cursor, both of them.
            //
            // Always palette 16 - PsyDoom's UIPAL for Doom and Final Doom, and the Master Edition's own MAPINFO asks
            // for 16 as well, so this one genuinely is the same everywhere.
            std::vector<std::byte> statusData;

            if ((foundStatus >= 0) &&
                readLump(discReader, pWadEntry->startLba, statusLump.offset, statusLump.size, bStatusCompressed, statusData) &&
                (statusData.size() > 8) && (palData.size() >= 512 * 17))
            {
                struct TexHeader { int16_t offsetX, offsetY, width, height; };

                TexHeader sh = {};
                std::memcpy(&sh, statusData.data(), sizeof(sh));

                if ((sh.width > 0) && (sh.height > 0) && (statusData.size() >= sizeof(sh) + (size_t)(sh.width * sh.height))) {
                    const uint16_t* const pUiPal = ((const uint16_t*) palData.data()) + (16 * 256);
                    const uint8_t* const pIdx = (const uint8_t*)(statusData.data() + sizeof(sh));

                    // Width and height lead the file, so the drawing side does not have to be told them
                    std::vector<uint16_t> out(2 + (size_t)(sh.width * sh.height));
                    out[0] = (uint16_t) sh.width;
                    out[1] = (uint16_t) sh.height;

                    for (size_t i = 0; i < (size_t)(sh.width * sh.height); ++i) {
                        out[2 + i] = pUiPal[pIdx[i]];
                    }

                    char dir2[260];
                    cacheDirForCue(cuePath, dir2, sizeof(dir2));

                    const bool bOk = writeCache(dir2, "status.raw", out.data(), (int32_t)(out.size() * sizeof(uint16_t)));
                    assetLog("launcher assets:   STATUS %dx%d cached=%s", (int) sh.width, (int) sh.height, bOk ? "yes" : "NO");
                }
            }
        }
    }

    return true;
}

//------------------------------------------------------------------------------------------------------------------------------------------
// The background, kept ready to put on screen.
//
// Held as finished 32-bit pixels at the size it is actually shown - 512x480, twice the PlayStation's 256x240, the way
// the game shows it. Unpacking 15-bit colour and doubling every pixel is done once here rather than on every repaint,
// which turns drawing the background into a row-by-row copy.
//
// That matters for more than speed. This console has one framebuffer and no back buffer, so the display is reading it
// while it is being written; the longer a repaint takes the more of it is seen half-finished. A megabyte of RAM buys a
// repaint that is short enough not to be seen, and lets a small part of it be repainted on its own.
//------------------------------------------------------------------------------------------------------------------------------------------
static constexpr int32_t BACK_SRC_W = 256;
static constexpr int32_t BACK_SRC_H = 240;
static constexpr int32_t BACK_W     = BACK_SRC_W * 2;
static constexpr int32_t BACK_H     = BACK_SRC_H * 2;

static std::vector<uint32_t> gBackScaled;

// Widen 15-bit PlayStation colour - five bits each, red lowest, opposite to this console - to 32-bit.
// The top bits are repeated into the bottom rather than shifted, or everything comes out slightly dark.
static inline uint32_t psxColourTo32(const uint16_t c) noexcept {
    const uint32_t r5 = (uint32_t)(c & 0x1F);
    const uint32_t g5 = (uint32_t)((c >> 5) & 0x1F);
    const uint32_t b5 = (uint32_t)((c >> 10) & 0x1F);

    return (
        0xFF000000u |
        (((r5 << 3) | (r5 >> 2)) << 16) |
        (((g5 << 3) | (g5 >> 2)) << 8) |
        ((b5 << 3) | (b5 >> 2))
    );
}

// Read a style's cached background and get it ready to draw. Called when the style changes, not when it is drawn.
static bool loadBackground(const char* const cuePath) noexcept {
    gBackScaled.clear();

    if ((!cuePath) || (cuePath[0] == '\0'))
        return false;

    char cacheDir[260];
    cacheDirForCue(cuePath, cacheDir, sizeof(cacheDir));

    char path[300];
    std::snprintf(path, sizeof(path), "%s\\back.raw", cacheDir);

    HANDLE const h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

    if (h == INVALID_HANDLE_VALUE)
        return false;

    std::vector<uint16_t> src((size_t)(BACK_SRC_W * BACK_SRC_H));
    DWORD read = 0;
    const bool bReadOk = (ReadFile(h, src.data(), (DWORD)(src.size() * sizeof(uint16_t)), &read, nullptr) &&
                          (read == src.size() * sizeof(uint16_t)));
    CloseHandle(h);

    if (!bReadOk)
        return false;

    gBackScaled.resize((size_t)(BACK_W * BACK_H));

    for (int32_t y = 0; y < BACK_H; ++y) {
        const uint16_t* const pSrcRow = src.data() + ((size_t)(y / 2) * BACK_SRC_W);
        uint32_t* const pDstRow = gBackScaled.data() + ((size_t) y * BACK_W);

        for (int32_t x = 0; x < BACK_W; ++x) {
            pDstRow[x] = psxColourTo32(pSrcRow[x / 2]);
        }
    }

    return true;
}

// Where the picture sits on screen. Returns false if there is no picture, or nowhere to put it.
static bool backgroundPlacement(int32_t& screenWOut, int32_t& screenHOut, int32_t& dstXOut, int32_t& dstYOut) noexcept {
    if (gBackScaled.empty())
        return false;

    const VIDEO_MODE videoMode = XVideoGetMode();

    if (videoMode.bpp != 32)
        return false;

    screenWOut = videoMode.width;
    screenHOut = videoMode.height;
    dstXOut = (screenWOut - BACK_W) / 2;
    dstYOut = (screenHOut - BACK_H) / 2;

    return ((dstXOut >= 0) && (dstYOut >= 0));
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Put the whole background on screen
//------------------------------------------------------------------------------------------------------------------------------------------
bool drawCachedBackground([[maybe_unused]] const char* const cuePath) noexcept {
    int32_t screenW = 0, screenH = 0, dstX = 0, dstY = 0;

    if (!backgroundPlacement(screenW, screenH, dstX, dstY))
        return false;

    uint8_t* const pFrameBuffer = XVideoGetFB();

    if (!pFrameBuffer)
        return false;

    for (int32_t y = 0; y < screenH; ++y) {
        uint32_t* const pRow = (uint32_t*)(pFrameBuffer + ((size_t) y * screenW) * sizeof(uint32_t));

        // The bars either side and above, which the picture does not reach.
        //
        // Painted here rather than by clearing the whole screen first: a clear followed by a redraw is a visible flicker
        // every repaint, and whatever was in those bars - including the plain menu's header - would otherwise stay there
        // underneath the styled menu.
        const bool bRowHasPicture = ((y >= dstY) && (y < dstY + BACK_H));

        for (int32_t x = 0; x < dstX; ++x) {
            pRow[x] = 0xFF000000u;
            pRow[screenW - 1 - x] = 0xFF000000u;
        }

        if (bRowHasPicture) {
            std::memcpy(pRow + dstX, gBackScaled.data() + ((size_t)(y - dstY) * BACK_W), (size_t) BACK_W * sizeof(uint32_t));
        } else {
            for (int32_t x = dstX; x < dstX + BACK_W; ++x) {
                pRow[x] = 0xFF000000u;
            }
        }
    }

    return true;
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Put one rectangle of the background back, undrawing whatever was on top of it
//------------------------------------------------------------------------------------------------------------------------------------------
void restoreBackgroundRect(const int32_t x, const int32_t y, const int32_t w, const int32_t h) noexcept {
    int32_t screenW = 0, screenH = 0, dstX = 0, dstY = 0;

    if (!backgroundPlacement(screenW, screenH, dstX, dstY))
        return;

    uint8_t* const pFrameBuffer = XVideoGetFB();

    if (!pFrameBuffer)
        return;

    const int32_t x0 = std::max(x, 0);
    const int32_t y0 = std::max(y, 0);
    const int32_t x1 = std::min(x + w, screenW);
    const int32_t y1 = std::min(y + h, screenH);

    for (int32_t sy = y0; sy < y1; ++sy) {
        uint32_t* const pRow = (uint32_t*)(pFrameBuffer + ((size_t) sy * screenW) * sizeof(uint32_t));
        const bool bRowHasPicture = ((sy >= dstY) && (sy < dstY + BACK_H));
        const uint32_t* const pSrcRow = (bRowHasPicture) ? (gBackScaled.data() + ((size_t)(sy - dstY) * BACK_W)) : nullptr;

        for (int32_t sx = x0; sx < x1; ++sx) {
            const int32_t picX = sx - dstX;
            const bool bInPicture = (pSrcRow && (picX >= 0) && (picX < BACK_W));
            pRow[sx] = (bInPicture) ? pSrcRow[picX] : 0xFF000000u;
        }
    }
}


//------------------------------------------------------------------------------------------------------------------------------------------
// Which CD track a disc's main menu music is on, as found by 'probeDisc'.
//
// Returns zero if that disc has not been probed, or if it has no menu music - which the caller must treat as "play
// nothing" rather than falling back to a number, since a wrong track is a different piece of music rather than silence.
//------------------------------------------------------------------------------------------------------------------------------------------
int32_t cachedMusicTrack(const char* const cuePath) noexcept {
    if ((!cuePath) || (cuePath[0] == '\0'))
        return 0;

    char cacheDir[260];
    cacheDirForCue(cuePath, cacheDir, sizeof(cacheDir));

    char path[300];
    std::snprintf(path, sizeof(path), "%s\\music.txt", cacheDir);

    HANDLE const h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

    if (h == INVALID_HANDLE_VALUE)
        return 0;

    char buf[16] = {};
    DWORD read = 0;
    const bool bOk = (ReadFile(h, buf, sizeof(buf) - 1, &read, nullptr) && (read > 0));
    CloseHandle(h);

    return (bOk) ? (int32_t) std::atoi(buf) : 0;
}

//------------------------------------------------------------------------------------------------------------------------------------------
// Drawing with PSX Doom's own fonts and cursor.
//
// The glyph tables come from the game rather than from a copy of them: 'gBigFontChars' is already in this binary, and
// the small font's layout is arithmetic - eight pixel cells, thirty two to a row, starting at v=168. So these are the
// same letters the game draws, and they cannot drift from it.
//------------------------------------------------------------------------------------------------------------------------------------------

// The loaded style's STATUS atlas
static std::vector<uint16_t>    gStatusPixels;
static int32_t                  gStatusW = 0;
static int32_t                  gStatusH = 0;

bool isStyleLoaded() noexcept {
    return (!gStatusPixels.empty());
}

bool useStyle(const char* const cuePath) noexcept {
    gStatusPixels.clear();
    gStatusW = 0;
    gStatusH = 0;

    // The background comes with the style rather than being fetched when it is drawn. Doing it here is what lets a
    // repaint be a copy, and what lets a small part of the screen be repainted on its own.
    loadBackground(cuePath);

    if ((!cuePath) || (cuePath[0] == '\0'))
        return false;

    char cacheDir[260];
    cacheDirForCue(cuePath, cacheDir, sizeof(cacheDir));

    char path[300];
    std::snprintf(path, sizeof(path), "%s\\status.raw", cacheDir);

    HANDLE const h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

    if (h == INVALID_HANDLE_VALUE)
        return false;

    const DWORD fileSize = GetFileSize(h, nullptr);

    if ((fileSize == INVALID_FILE_SIZE) || (fileSize < 4)) {
        CloseHandle(h);
        return false;
    }

    std::vector<uint16_t> raw(fileSize / sizeof(uint16_t));
    DWORD read = 0;
    const bool bOk = (ReadFile(h, raw.data(), fileSize, &read, nullptr) && (read == fileSize));
    CloseHandle(h);

    if ((!bOk) || (raw.size() < 3))
        return false;

    const int32_t w = raw[0];
    const int32_t h2 = raw[1];

    if ((w <= 0) || (h2 <= 0) || (raw.size() < (size_t)(2 + w * h2)))
        return false;

    gStatusW = w;
    gStatusH = h2;
    gStatusPixels.assign(raw.begin() + 2, raw.begin() + 2 + (size_t)(w * h2));
    return true;
}

// One glyph, scaled by two to match the background, skipping the colour the PlayStation treats as transparent
static void blitGlyph(const int32_t dstX, const int32_t dstY, const int32_t u, const int32_t v, const int32_t w, const int32_t h) noexcept {
    const VIDEO_MODE videoMode = XVideoGetMode();
    uint8_t* const pFrameBuffer = XVideoGetFB();

    if ((!pFrameBuffer) || (videoMode.bpp != 32) || gStatusPixels.empty())
        return;

    const int32_t screenW = videoMode.width;
    const int32_t screenH = videoMode.height;

    for (int32_t y = 0; y < h * 2; ++y) {
        const int32_t sy = v + (y / 2);
        const int32_t dy = dstY + y;

        if ((sy < 0) || (sy >= gStatusH) || (dy < 0) || (dy >= screenH))
            continue;

        uint32_t* const pDstRow = (uint32_t*)(pFrameBuffer + ((size_t) dy * screenW) * sizeof(uint32_t));

        for (int32_t x = 0; x < w * 2; ++x) {
            const int32_t sx = u + (x / 2);
            const int32_t dx = dstX + x;

            if ((sx < 0) || (sx >= gStatusW) || (dx < 0) || (dx >= screenW))
                continue;

            const uint16_t c = gStatusPixels[(size_t) sy * gStatusW + sx];

            // Black is transparent in this format, which is how the letters keep their shape over the background
            if ((c & 0x7FFF) == 0)
                continue;

            pDstRow[dx] = psxColourTo32(c);
        }
    }
}

// Where a character sits in the big font, or -1 if it is not in it
static int32_t bigFontIndex(const char c) noexcept {
    if ((c >= 'A') && (c <= 'Z')) return BIG_FONT_UCASE_ALPHA + (c - 'A');
    if ((c >= 'a') && (c <= 'z')) return BIG_FONT_LCASE_ALPHA + (c - 'a');
    if ((c >= '0') && (c <= '9')) return BIG_FONT_DIGITS + (c - '0');
    if (c == '%') return BIG_FONT_PERCENT;
    if (c == '!') return BIG_FONT_EXCLAMATION;
    if (c == '.') return BIG_FONT_PERIOD;
    if (c == '-') return BIG_FONT_MINUS;
    return -1;
}

int32_t bigTextWidth(const char* const str) noexcept {
    if (!str)
        return 0;

    int32_t width = 0;

    for (const char* p = str; *p; ++p) {
        if (*p == ' ') {
            width += 6 * 2;
            continue;
        }

        const int32_t idx = bigFontIndex(*p);
        width += (idx >= 0) ? (gBigFontChars[idx].w + 1) * 2 : 0;
    }

    return width;
}

void drawBigText(const int32_t x, const int32_t y, const char* const str) noexcept {
    if ((!str) || (!isStyleLoaded()))
        return;

    int32_t curX = x;

    for (const char* p = str; *p; ++p) {
        if (*p == ' ') {
            curX += 6 * 2;
            continue;
        }

        const int32_t idx = bigFontIndex(*p);

        if (idx < 0)
            continue;

        const fontchar_t& fc = gBigFontChars[idx];
        blitGlyph(curX, y, fc.u, fc.v, fc.w, fc.h);
        curX += (fc.w + 1) * 2;
    }
}

void drawSmallText(const int32_t x, const int32_t y, const char* const str) noexcept {
    if ((!str) || (!isStyleLoaded()))
        return;

    int32_t curX = x;

    for (const char* p = str; *p; ++p) {
        const int32_t charIdx = (int32_t)(*p) - 33;

        if ((charIdx >= 0) && (charIdx < 64)) {
            const int32_t row = charIdx / 32;
            const int32_t col = charIdx - row * 32;
            blitGlyph(curX, y, col * SMALL_FONT_SIZE, row * SMALL_FONT_SIZE + SMALL_FONT_V_MIN, SMALL_FONT_SIZE, SMALL_FONT_SIZE);
        }

        curX += SMALL_FONT_SIZE * 2;
    }
}

void drawSkull(const int32_t x, const int32_t y, const int32_t frame) noexcept {
    if (!isStyleLoaded())
        return;

    // Two frames side by side in the atlas, which is the whole of the cursor's animation
    blitGlyph(x, y, M_SKULL_TEX_U + ((frame & 1) * M_SKULL_W), M_SKULL_TEX_V, M_SKULL_W, M_SKULL_H);
}

END_NAMESPACE(LauncherAssets)

#endif  // #if defined(__XBOX__)
