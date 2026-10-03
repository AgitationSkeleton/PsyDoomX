# PsyDoomX

PlayStation Doom on the original Xbox. A port of [PsyDoom](https://github.com/BodbDearg/PsyDoom), which
reverse engineered the PlayStation version of Doom and Final Doom into portable C++.

You bring your own game disc. Nothing from a retail game ships here.

![splitscreen](https://img.shields.io/badge/splitscreen-2%20player-blue) ![platform](https://img.shields.io/badge/platform-Original%20Xbox-green)

## What this adds to PsyDoom

- **Two player splitscreen**, side by side or one above the other, on one console with two pads. The
  PlayStation game only ever had link cable multiplayer between two machines.
- **A launcher** that finds whichever games are installed and starts them, wearing the menu of
  whichever one you pick - its background, its font, its music and its sounds, read out of the disc.
- **Doom Forever**, the Russian fan conversion of Final Doom, alongside Doom, Final Doom and the
  Master Edition. It is listed after the Master Edition and its menu can be worn by the launcher like
  the others. Its saves and level password are kept apart from Final Doom's.
- **A Randomizer mode**, listed after Singleplayer, Cooperative and Deathmatch. Every monster, item and
  pickup in the level is rolled for something else that fits where it stands, along with the sky and the
  music. It only ever places what the running game can actually draw, so nothing turns into a missing
  sprite.
- **Coop Rando**, listed after the Randomizer: the same rolled levels, played cooperatively in
  splitscreen. Each player is handed their own random weapon at the start of every level, and again on
  respawning.
- **Cross edition super shotgun sprites.** Doom's and Final Doom's are different art. Carry either, per
  player, and it changes in your hands.
- **The Master Edition's monsters in the other games.** If you have it installed, the Randomizer can put
  the Arch-Vile, the Wolfenstein SS and Commander Keen into Doom and Final Doom, with their own sounds.
  Without it those three are simply left out of the roster.
- **Player colours**, the way PC Doom does them - green, indigo, brown and red - so two marines can be
  told apart. Per player, and it follows the body.
- **Named levels**, optionally. The cooperative and deathmatch level select can read `MAP24: Hell
  Beneath` rather than `Level 24`, using each game's own map names.
- **A 30 FPS lock** as an alternative to the uncapped frame rate, timed to the television's own refresh
  so frames arrive evenly and without tearing.
- **A brightness setting**, for televisions that show PSX Doom's dark rooms darker than a PlayStation does.
- Per player turn speed, autorun and stat display, and an on screen frame rate readout.

## What you need

- An original Xbox that can run unsigned software.
- **Your own** copy of one or more of: PSX Doom, PSX Final Doom, the [GEC] Master Edition, or Doom
  Forever, as a `.cue` and its `.bin` files. These are not included and never will be.

## Installing

Put the executable and your discs in a folder of their own, anywhere on the Xbox's hard disk -
`E:\Apps\PsyDoomX`, `E:\Games\PsyDoomX` and `F:\Homebrew\PsyDoomX` all work the same:

```
<your folder>\
    default.xbe
    Doom\Doom.cue                          + its .bin files
    FinalDoom\FinalDoom.cue                + its .bin files
    MasterEdition\PSXDOOM_BETA_4.cue       + its .bin file
    DoomForever\Doom.cue                   + its .bin file
```

Only the folders you actually have need to exist - the launcher lists what it finds and nothing else.
`Final Doom`, `Master Edition` and `Doom Forever`, with spaces, are accepted as folder names too. The cue
sheet does not have to be called what is shown above: if that name is not there, the launcher takes
whichever `.cue` file the folder holds. Launch `default.xbe` from a dashboard.

Everything the launcher and the games write - settings, saves, a cached copy of each disc's menu artwork,
and logs - goes in that same folder. Deleting `cache\` is harmless; it is rebuilt on the next start.

**Upgrading from an earlier version:** copy the new `default.xbe` over the old one. Saves and settings in
that folder carry on as they were. The cache is now kept under each game's folder name, so the old
`cache\Doom.cue`, `cache\FinalDoom.cue` and `cache\PSXDOOM_BETA_4.cue` folders can be deleted. Earlier
versions only ran from `E:\Apps\PsyDoomX`; a second copy kept there to make another location work is no
longer needed.

## Controls

Standard Duke pad or S controller, or a third party pad. With two pads, choosing Cooperative, Deathmatch
or Coop Rando on the main menu starts a splitscreen game. Any connected pad can drive the launcher's menu.

| | |
|---|---|
| Left stick | Move |
| Right stick | Turn |
| A | Use |
| Right trigger | Fire |
| White / Black | Change weapon |
| Back | Automap |
| Start | Pause |

Turn speed, autorun, the stat display, the super shotgun style, the frame rate and brightness are under
**Options → Extra Options**. In a multiplayer game, player colour and the splitscreen layout are there too.
In splitscreen each player has their own menu, driven by their own pad.

The launcher itself has settings for the frame rate readout, the menu style, and whether levels are
named or numbered. They are remembered between runs.

### Picture and frame rate

**Brightness** runs from `PS1`, which is exactly the picture the PlayStation draws, up to 8. It lifts the
darker tones without greying out black, and changes as you press, so it can be set while looking at a
dark room. If a dark scene is too dark on your television, this is the setting to raise.

**Frame rate** is either `Uncapped FPS`, which draws as fast as the console can and smooths movement
between frames, or `30 FPS Lock`, which holds every frame for two refreshes of the television. The lock is
steadier; uncapped is faster on average but varies from moment to moment. On a 50Hz television the lock
is 25 FPS.

### Cheat codes

The PlayStation cheat codes work, in single player. Pause the game with Start, then enter the buttons in
order:

| Cheat | Buttons |
|---|---|
| God mode | Down, Left trigger, X, Black, Right, White, Left, B |
| All weapons, ammo and keys | A, Y, White, Up, Down, Right trigger, Left, Left |
| All weapons and ammo, no keys | A, Y, White, Up, Down, Right trigger, Right, Right |
| Level select | Right, Left, Right trigger, Black, Y, White, B, A |
| X-ray vision | White, Right trigger, Left trigger, Black, Right, Y, A, Right |
| Show the whole map | Y, Y, Left trigger, Right trigger, Left trigger, Right trigger, Black, X |
| Show every thing on the map | Y, Y, Left trigger, Right trigger, Left trigger, Right trigger, Black, B |
| No clipping | Up, Up, Up, Up, Up, Up, Up, Black |
| Monsters ignore you | A, Up, A, Up, X, X, A, X |

The PlayStation buttons they stand for are Square → X, Circle → B, Triangle → Y, Cross → A, L1 → White,
R1 → Black, L2 → Left trigger and R2 → Right trigger. Level select lets you pick any map with left and
right, the secret ones included, and A goes there.

## Building

Needs [nxdk](https://github.com/XboxDev/nxdk) and CMake.

```sh
git clone --recursive https://github.com/XboxDev/nxdk
make -C nxdk NXDK_ONLY=y

git clone --recursive <this repo>
cd PsyDoomX
NXDK_DIR=../nxdk ./scripts/build_xbox.sh
```

The result is `build-xbox/game/default.xbe`.

On Windows, run that script under a bash that shares its MSYS2 runtime with cmake - devkitPro's bash
works, Git Bash does not, because environment variables do not carry between the two.

Tagged releases are built the same way by GitHub Actions and the executable is attached to the release.

## The diagnostic relay

There is a development tool built in that sends a running commentary of what the console is doing to a
listener on another machine. **It does nothing unless you ask it to.** Create a file called
`logserver.txt` in the same folder as `default.xbe`, containing one line:

```
192.168.0.5:9909
```

Without that file no socket is opened and nothing leaves the console. No address is compiled in.

Controller problems are also written to `pads.log` in that folder, with no listener needed: each pad as it
connects - its vendor and product ID and how it reports - and any USB errors it was recovered from. If a
controller misbehaves, that file is the thing to attach to an issue.

## Not everything is here

This is one platform's port. The Vulkan renderer, the FLTK launcher and the various tools that PsyDoom
carries for other platforms are all still in the tree and still build for those platforms, but they are
switched off for the Xbox build - it has no Vulkan and no windowing system.

Xbox specific code is marked with `__XBOX__` and, where a decision only makes sense on this hardware,
says so in a comment. A good deal of it is about one console's quirks: a single framebuffer with no back
buffer, a clock that does not advance dependably, and an audio path that has to be driven by polling
rather than by interrupt.

## Credits and licence

PsyDoom is by **Darragh Coy (BodbDearg)** and is the reason any of this exists. The reverse engineering
of PSX Doom, the renderer, the sound system and the disc handling are all his work; this port adds an
Xbox back end and splitscreen on top of it.

Doom is by **id Software**. The PlayStation version is by **Williams Entertainment**.

GPLv3, the same as PsyDoom. See [LICENSE](LICENSE) and [CONTRIBUTORS.md](CONTRIBUTORS.md).
