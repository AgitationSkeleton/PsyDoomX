#!/usr/bin/env bash
#
# Build the Xbox executable.
#
# Needs the nxdk toolchain. Point NXDK_DIR at it, or let this find it beside the repo:
#
#   NXDK_DIR=/path/to/nxdk ./scripts/build_xbox.sh
#
# On Windows this must be run under a bash that shares its MSYS2 runtime with cmake - devkitPro's
# bash works, Git Bash does not, because environment variables do not propagate between the two.
#
# The finished file is 'build-xbox/game/default.xbe'.
set -e

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$REPO_DIR/build-xbox}"

# Where nxdk is, if it was not given
if [ -z "$NXDK_DIR" ]; then
    for candidate in "$REPO_DIR/../nxdk" "$REPO_DIR/nxdk" "$HOME/nxdk"; do
        if [ -f "$candidate/share/toolchain-nxdk.cmake" ]; then
            NXDK_DIR="$(cd "$candidate" && pwd)"
            break
        fi
    done
fi

if [ -z "$NXDK_DIR" ] || [ ! -f "$NXDK_DIR/share/toolchain-nxdk.cmake" ]; then
    echo "Could not find nxdk. Set NXDK_DIR to a checkout that has been built:" >&2
    echo "    git clone --recursive https://github.com/XboxDev/nxdk" >&2
    echo "    make -C nxdk NXDK_ONLY=y" >&2
    exit 1
fi

export NXDK_DIR
export PATH="$NXDK_DIR/bin:$PATH"

echo "=== nxdk: $NXDK_DIR"
echo "=== build: $BUILD_DIR"

mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

# Optimisation, explicitly.
#
# None was being applied at all to begin with. CMAKE_BUILD_TYPE was never set, so no -O flag reached the compiler and
# the whole game - the emulated PlayStation GPU included - was built unoptimised. That measured 3.2 fps on hardware,
# with 55% of every frame inside that emulator, on a CPU that has no business being that slow at it.
#
# Set here rather than trusting the build type alone: the toolchain leaves CMAKE_CXX_FLAGS_RELEASE empty, so asking for
# Release on its own does not guarantee a -O flag actually arrives.
OPT_FLAGS="-O2 -DNDEBUG"

# Stamped into the binary and logged on startup, so a report can be tied to a build.
# Falls back to the date when there is no git checkout to ask.
BUILD_ID="${PSYDOOM_XBOX_BUILD_ID:-$(git -C "$REPO_DIR" rev-parse --short HEAD 2>/dev/null || date +%Y%m%d)}"

# Tell cmake where make is, if we can see it and cmake might not.
#
# On Windows this is the difference between working and not: cmake and the shell can disagree about what is on
# PATH, and cmake then reports "unable to find a build program corresponding to Unix Makefiles" without any hint
# that make is sitting right there. Resolving it here is unambiguous.
MAKE_ARG=""
MAKE_PROGRAM="${MAKE_PROGRAM:-$(command -v make || true)}"

if [ -n "$MAKE_PROGRAM" ]; then
    MAKE_ARG="-DCMAKE_MAKE_PROGRAM=$MAKE_PROGRAM"
fi

cmake -G "Unix Makefiles" $MAKE_ARG \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_FLAGS_RELEASE="$OPT_FLAGS" \
  -DCMAKE_CXX_FLAGS_RELEASE="$OPT_FLAGS" \
  -DCMAKE_TOOLCHAIN_FILE="$NXDK_DIR/share/toolchain-nxdk.cmake" \
  -DPSYDOOM_XBOX_BUILD_ID="$BUILD_ID" \
  -DPSYDOOM_INCLUDE_VULKAN_RENDERER=OFF \
  -DPSYDOOM_INCLUDE_LAUNCHER=OFF \
  -DPSYDOOM_INCLUDE_AUDIO_TOOLS=OFF \
  -DPSYDOOM_INCLUDE_OTHER_TOOLS=OFF \
  -DPSYDOOM_INCLUDE_OLD_CODE=OFF \
  "$REPO_DIR"

echo "=== Configure done, starting make ==="
"${MAKE_PROGRAM:-make}" -j"$(nproc 2>/dev/null || echo 2)" PsyDoom
echo "=== Build done ==="
echo "XBE: $BUILD_DIR/game/default.xbe"
