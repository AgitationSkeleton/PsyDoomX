#pragma once

//------------------------------------------------------------------------------------------------------------------------------------------
// PsyDoom Xbox: where this copy of the executable lives, and every file path that follows from that.
//
// Everything used to assume 'E:\Apps\PsyDoomX\'. A copy installed anywhere else - 'E:\Games', 'F:\Homebrew' - drew its
// menu, and then picking a game restarted the copy in 'E:\Apps' instead of itself, which is either a black screen or,
// when a second copy happened to be there, a game that quietly ran from somewhere the player did not put it. The discs,
// the settings, the saves and the cache were all read from 'E:\Apps' too.
//
// So the location is taken from the kernel, which records the full path of the XBE it started, and everything is built
// from that. Nothing in the game should name a folder of its own; it asks here.
//------------------------------------------------------------------------------------------------------------------------------------------
#include <cstddef>

namespace XboxPaths {

// Work out where this XBE was started from. Call once the drives are mounted and before anything opens a file of its own.
// Calling it again does nothing.
void init() noexcept;

// The folder this XBE lives in, as a path the file APIs accept, with a trailing backslash - 'F:\Homebrew\PsyDoomX\'.
//
// Before 'init' this is the old fixed location, so a caller that runs too early still gets a usable answer.
const char* dir() noexcept;

// This XBE in the kernel's own terms - '\Device\Harddisk0\Partition6\Homebrew\PsyDoomX\default.xbe' - which is what
// relaunching needs: it names exactly this copy, whatever drive letters the dashboard or this process have mounted.
const char* xbeNtPath() noexcept;

// The same file through a drive letter, for checking it is there with the ordinary file APIs
const char* xbeDosPath() noexcept;

// 'dir()' followed by 'leaf'. Returns 'out'.
const char* make(char* const out, const size_t outSize, const char* const leaf) noexcept;

// The boot log beside the executable
const char* bootLogPath() noexcept;

// A sentence for the logs saying how the location was found, so a report from someone else's console can be read
const char* describe() noexcept;

}   // namespace XboxPaths
