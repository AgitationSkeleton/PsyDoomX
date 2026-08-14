#pragma once

//------------------------------------------------------------------------------------------------------------------------------------------
// Sound and music for the launcher's menu, taken from the game discs like everything else the styled menu wears.
//
// The launcher and the game are the same executable, so the engine's audio startup is already here. The open question
// was whether it could be run in the launcher without disturbing the game it later starts, and the answer is that the
// question does not arise: choosing a game relaunches this XBE, so the launcher's process is torn down before the
// game's begins. Nothing survives to be disturbed.
//
// What does matter is that none of the engine's startup can be run twice in ONE process, which is why 'init' does its
// work once and only once. The music is therefore kept on a disc reader of its own rather than the engine's, so that
// changing the menu style can change the music with it without any of that being torn down and built again.
//
// Not portable: this is the Xbox launcher, which only exists because this console needs one.
//------------------------------------------------------------------------------------------------------------------------------------------
#if defined(__XBOX__)

#include "Macros.h"

#include <cstdint>

BEGIN_NAMESPACE(LauncherAudio)

// Bring the sound system up, using the given disc for the sound effects.
//
// Only the first call does anything: 'wess_load_module' and the rest cannot be run a second time in one process. That
// costs nothing in fidelity here because 'DOOMSFX.LCD' is byte for byte the same 177,104 bytes on all three discs, so
// the menu sounds do not depend on which one they came from. Only the music differs between editions, and that is
// handled separately below.
//
// Returns whether sound is usable afterwards.
bool init(const char* const cuePath) noexcept;

// Is sound up and usable?
bool isReady() noexcept;

// Play a disc's main menu music, looping, replacing whatever was playing.
//
// The disc may be a different one to the disc 'init' was given - that is the point of it being separate.
void playMenuMusic(const char* const cuePath) noexcept;

void stopMusic() noexcept;

// The menu's own sounds, which are the ones the game's main menu uses for the same three actions
void playMoveSound() noexcept;      // Moving the selection
void playSelectSound() noexcept;    // Changing a setting
void playConfirmSound() noexcept;   // Starting a game or leaving

// Step the sequencer and the volume fades. Call once per menu frame; without it nothing plays.
void update() noexcept;

// Silence the hardware before handing the console to another program
void shutdown() noexcept;

END_NAMESPACE(LauncherAudio)

#endif  // #if defined(__XBOX__)
