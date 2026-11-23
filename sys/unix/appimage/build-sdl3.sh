#!/bin/sh
# NetHack Modern: build SDL3 and SDL3_ttf from their release sources into
# a prefix, for the SDL window port (WANT_WIN_SDL) where the system has no
# SDL3 packages (Ubuntu before 25.04, and the AppImage build).
#
#   sys/unix/appimage/build-sdl3.sh <prefix> [<work dir>]
#
# then build the game with PKG_CONFIG_PATH=<prefix>/lib/pkgconfig. Needs a
# C compiler, cmake, pkg-config, curl, and the X11, Wayland, OpenGL/EGL,
# xkbcommon and FreeType development packages (SDL only loads the X11 and
# Wayland libraries at run time, from the player's own system). The
# release tarballs are checked against the checksums below.

set -e

SDL_VERSION=3.4.20
SDL_SHA256=f3bb06f55bed9557227a700af2ea62e49497ebdf58037732a731a5fb3ef8581f
TTF_VERSION=3.2.2
TTF_SHA256=63547d58d0185c833213885b635a2c0548201cc8f301e6587c0be1a67e1e045d

if [ $# -lt 1 ]; then
    echo "usage: $0 <prefix> [<work dir>]" >&2
    exit 2
fi
mkdir -p "$1"
PREFIX=$(cd "$1" && pwd)
WORK=${2:-${TMPDIR:-/tmp}/nhm-sdl3-src}
mkdir -p "$WORK"
cd "$WORK"

fetch() { # url file sha256
    [ -f "$2" ] || curl -fsSL -o "$2" "$1"
    echo "$3  $2" | sha256sum -c -
}
fetch "https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VERSION/SDL3-$SDL_VERSION.tar.gz" \
      "SDL3-$SDL_VERSION.tar.gz" "$SDL_SHA256"
fetch "https://github.com/libsdl-org/SDL_ttf/releases/download/release-$TTF_VERSION/SDL3_ttf-$TTF_VERSION.tar.gz" \
      "SDL3_ttf-$TTF_VERSION.tar.gz" "$TTF_SHA256"
rm -rf "SDL3-$SDL_VERSION" "SDL3_ttf-$TTF_VERSION" build-sdl build-ttf
tar xzf "SDL3-$SDL_VERSION.tar.gz"
tar xzf "SDL3_ttf-$TTF_VERSION.tar.gz"
JOBS=$(nproc 2>/dev/null || echo 2)

# SDL3: the game uses its video, events and rendering (sound is
# miniaudio's); optional extras a system lacks headers for are turned off
EXTRA=""
for try in 1 2 3 4 5 6 7 8; do
    rm -rf build-sdl
    if cmake -S "SDL3-$SDL_VERSION" -B build-sdl -DCMAKE_BUILD_TYPE=Release \
             -DCMAKE_INSTALL_PREFIX="$PREFIX" -DSDL_TESTS=OFF \
             -DSDL_EXAMPLES=OFF -DSDL_AUDIO=OFF $EXTRA > cmake-sdl.log 2>&1
    then
        break
    fi
    opt=$(grep -o "configure with -D[A-Z0-9_]*=OFF" cmake-sdl.log | head -1 \
          | sed "s/configure with //")
    if [ -z "$opt" ]; then
        tail -30 cmake-sdl.log
        exit 1
    fi
    echo "SDL3: turning off optional $opt"
    EXTRA="$EXTRA $opt"
done
cmake --build build-sdl -j "$JOBS" > build-sdl.log 2>&1 \
    || { tail -30 build-sdl.log; exit 1; }
cmake --install build-sdl > /dev/null

# SDL3_ttf, with the system's FreeType (no HarfBuzz or SVG fonts needed)
cmake -S "SDL3_ttf-$TTF_VERSION" -B build-ttf -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_PREFIX_PATH="$PREFIX" \
      -DSDLTTF_VENDORED=OFF -DSDLTTF_HARFBUZZ=OFF -DSDLTTF_PLUTOSVG=OFF \
      -DSDLTTF_SAMPLES=OFF > cmake-ttf.log 2>&1 \
    || { tail -30 cmake-ttf.log; exit 1; }
cmake --build build-ttf -j "$JOBS" > build-ttf.log 2>&1 \
    || { tail -30 build-ttf.log; exit 1; }
cmake --install build-ttf > /dev/null

PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig" pkg-config --modversion sdl3 sdl3-ttf
echo "SDL3 $SDL_VERSION and SDL3_ttf $TTF_VERSION are in $PREFIX"
