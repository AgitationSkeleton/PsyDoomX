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
- **Cross edition super shotgun sprites.** Doom's and Final Doom's are different art. Carry either, per
  player, and it changes in your hands.
- **Player colours**, the way PC Doom does them - green, indigo, brown and red - so two marines can be
  told apart. Per player, and it follows the body.
- Per player turn speed, autorun and stat display, and an on screen frame rate readout.

## What you need

- An original Xbox that can run unsigned software.
- **Your own** copy of one or more of: PSX Doom, PSX Final Doom, or the [GEC] Master Edition, as a
  `.cue` and its `.bin` files. These are not included and never will be.

## Installing

Put the executable and your discs on the Xbox's hard disk like this:

```
E:\Apps\PsyDoomX\
    default.xbe
    Doom\Doom.cue                          + its .bin files
    FinalDoom\FinalDoom.cue                + its .bin files
    MasterEdition\PSXDOOM_BETA_4.cue       + its .bin file
```

Only the folders you actually have need to exist - the launcher lists what it finds and nothing else.
Launch `default.xbe` from a dashboard.

The launcher writes its settings, a cached copy of each disc's menu artwork, and its logs into
`E:\Apps\PsyDoomX\`. Deleting `E:\Apps\PsyDoomX\cache\` is harmless; it is rebuilt on the next start.

## Controls

Standard Duke pad or S controller. Two pads gives you splitscreen from the main menu's multiplayer
option.

| | |
|---|---|
| Left stick | Move |
| Right stick | Turn |
| A | Use |
| Right trigger | Fire |
| White / Black | Change weapon |
| Back | Automap |
| Start | Pause |

Bindings, turn speed and autorun are under **Options → Extra Options**.

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
listener on another machine. **It does nothing unless you ask it to.** Create a file at
`E:\Apps\PsyDoomX\logserver.txt` containing one line:

```
192.168.0.5:9909
```

Without that file no socket is opened and nothing leaves the console. No address is compiled in.

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
