//------------------------------------------------------------------------------------------------------------------------------------------
// PsyDoom Xbox: where this copy of the executable lives. See 'XboxPaths.h'.
//------------------------------------------------------------------------------------------------------------------------------------------
#include "XboxPaths.h"

#if defined(__XBOX__)

#include <algorithm>
#include <cstdio>
#include <cstring>

#include <nxdk/mount.h>
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>

namespace XboxPaths {

static constexpr size_t PATH_LEN = 260;

// Where everything lived before this module existed. Still the answer when nothing better can be worked out, so a
// console set up the way the original instructions said keeps working whatever goes wrong here.
static constexpr const char* LEGACY_DIR = "E:\\Apps\\PsyDoomX\\";

static char gDir[PATH_LEN]          = "E:\\Apps\\PsyDoomX\\";
static char gXbeNtPath[PATH_LEN]    = "";
static char gXbeDosPath[PATH_LEN]   = "E:\\Apps\\PsyDoomX\\default.xbe";
static char gBootLogPath[PATH_LEN]  = "E:\\Apps\\PsyDoomX\\bootlog.txt";
static char gDescription[PATH_LEN * 2] = "not worked out yet - using the old fixed location E:\\Apps\\PsyDoomX\\";
static bool gbInitDone = false;

// The hard disk's partitions and the letters 'Main_Xbox' mounts them as, which are the letters every dashboard uses
struct DriveMapping {
    char    letter;
    int     partition;
};

static constexpr DriveMapping STANDARD_DRIVES[] = {
    { 'E', 1 }, { 'C', 2 }, { 'X', 3 }, { 'Y', 4 }, { 'Z', 5 }, { 'F', 6 }, { 'G', 7 }
};

// Letters for a folder none of the above reach - a partition past the seventh on a large drive, or the DVD drive. Not
// any letter a dashboard is known to hand out, so mounting one cannot hide something that is already there.
static constexpr char SPARE_LETTERS[] = { 'P', 'R', 'S', 'T', 'V', 'W' };

static bool startsWithNoCase(const char* const str, const char* const prefix) noexcept {
    for (size_t i = 0; prefix[i] != '\0'; ++i) {
        const char a = (char)(((str[i] >= 'A') && (str[i] <= 'Z')) ? (str[i] + 32) : str[i]);
        const char b = (char)(((prefix[i] >= 'A') && (prefix[i] <= 'Z')) ? (prefix[i] + 32) : prefix[i]);

        if (a != b)
            return false;
    }

    return true;
}

static bool fileExists(const char* const path) noexcept {
    return (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES);
}

// Fill in everything that follows from the folder, once it is known
static void setDir(const char* const dirWithSlash, const char* const xbeName) noexcept {
    std::snprintf(gDir, sizeof(gDir), "%s", dirWithSlash);
    std::snprintf(gXbeDosPath, sizeof(gXbeDosPath), "%s%s", gDir, xbeName);
    std::snprintf(gBootLogPath, sizeof(gBootLogPath), "%sbootlog.txt", gDir);
}

void init() noexcept {
    if (gbInitDone)
        return;

    gbInitDone = true;

    // The kernel keeps the full path of the image it started, in its own terms
    const size_t ntLen = std::min((size_t) XeImageFileName->Length, PATH_LEN - 1);
    std::memcpy(gXbeNtPath, XeImageFileName->Buffer, ntLen);
    gXbeNtPath[ntLen] = '\0';

    const char* const pLastSlash = std::strrchr(gXbeNtPath, '\\');

    if ((ntLen == 0) || (!pLastSlash)) {
        std::snprintf(gDescription, sizeof(gDescription),
            "the kernel did not say where this XBE is ('%s') - using the old fixed location %s", gXbeNtPath, LEGACY_DIR);
        return;
    }

    const char* const pXbeName = pLastSlash + 1;

    // A partition of the hard disk that has a letter already: '\Device\Harddisk0\Partition6\Homebrew\PsyDoomX\...' is
    // 'F:\Homebrew\PsyDoomX\...'. Checked by asking for the file through that letter, so a letter that turned out to point
    // somewhere else is caught here rather than when the game cannot find its discs.
    static constexpr const char HDD_PREFIX[] = "\\Device\\Harddisk0\\Partition";

    if (startsWithNoCase(gXbeNtPath, HDD_PREFIX)) {
        const char* pRest = gXbeNtPath + (sizeof(HDD_PREFIX) - 1);
        int partition = 0;

        while ((*pRest >= '0') && (*pRest <= '9')) {
            partition = (partition * 10) + (*pRest - '0');
            ++pRest;
        }

        for (const DriveMapping& drive : STANDARD_DRIVES) {
            if ((drive.partition != partition) || (*pRest != '\\'))
                continue;

            // 'pRest' is the folder and file from the partition's root down, starting with its backslash
            char dirPath[PATH_LEN];
            const size_t restDirLen = (size_t)(pLastSlash - pRest) + 1;     // Up to and including the last backslash
            std::snprintf(dirPath, sizeof(dirPath), "%c:%.*s", drive.letter, (int) restDirLen, pRest);

            char dosXbe[PATH_LEN];
            std::snprintf(dosXbe, sizeof(dosXbe), "%s%s", dirPath, pXbeName);

            if (fileExists(dosXbe)) {
                setDir(dirPath, pXbeName);
                std::snprintf(gDescription, sizeof(gDescription),
                    "running from '%s', which is partition %d, reached as %s", gXbeNtPath, partition, gDir);
                return;
            }
        }
    }

    // Somewhere no standard letter reaches. Give the folder a letter of its own, the way the official libraries map
    // 'D:' to a title's folder, and use that.
    {
        char ntDir[PATH_LEN];
        const size_t ntDirLen = (size_t)(pLastSlash - gXbeNtPath) + 1;
        std::snprintf(ntDir, sizeof(ntDir), "%.*s", (int) ntDirLen, gXbeNtPath);

        for (const char letter : SPARE_LETTERS) {
            if (nxIsDriveMounted(letter))
                continue;

            if (!nxMountDrive(letter, ntDir))
                continue;

            char dirPath[8];
            std::snprintf(dirPath, sizeof(dirPath), "%c:\\", letter);

            char dosXbe[PATH_LEN];
            std::snprintf(dosXbe, sizeof(dosXbe), "%s%s", dirPath, pXbeName);

            if (fileExists(dosXbe)) {
                setDir(dirPath, pXbeName);
                std::snprintf(gDescription, sizeof(gDescription),
                    "running from '%s', which has no standard drive letter, so it was mounted as %s", gXbeNtPath, gDir);
                return;
            }

            nxUnmountDrive(letter);
        }
    }

    std::snprintf(gDescription, sizeof(gDescription),
        "running from '%s' but that could not be reached through any drive letter - using the old fixed location %s",
        gXbeNtPath, LEGACY_DIR);
}

const char* dir() noexcept {
    return gDir;
}

const char* xbeNtPath() noexcept {
    return gXbeNtPath;
}

const char* xbeDosPath() noexcept {
    return gXbeDosPath;
}

const char* make(char* const out, const size_t outSize, const char* const leaf) noexcept {
    if ((!out) || (outSize == 0))
        return "";

    std::snprintf(out, outSize, "%s%s", gDir, (leaf) ? leaf : "");
    return out;
}

const char* bootLogPath() noexcept {
    return gBootLogPath;
}

const char* describe() noexcept {
    return gDescription;
}

}   // namespace XboxPaths

#endif  // #if defined(__XBOX__)
