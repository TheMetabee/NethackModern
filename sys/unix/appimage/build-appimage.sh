#!/bin/sh
# NetHack Modern: package an installed game directory as an AppImage.
#
#   sys/unix/appimage/build-appimage.sh <installed game dir> <output dir>
#
# Build the game first with WANT_RELOCATABLE=1, so it reads sysconf from
# wherever it runs, and with the SDL window as its default (the terminal
# version is the fallback when there's no desktop):
#
#   sh sys/unix/setup.sh sys/unix/hints/linux.501
#   make WANT_WIN_TTY=1 WANT_WIN_SDL=1 WANT_DEFAULT=sdl WANT_MINIAUDIO=1 \
#        WANT_RELOCATABLE=1 install
#   sys/unix/appimage/build-appimage.sh playground dist
#
# The SDL window's font is bundled: $FONT (default DejaVu Sans Mono from
# the system), with $FONT_LICENSE beside it if given.
#
# Always makes <output dir>/NetHackModern.AppDir (its AppRun can be run as
# it is, for testing). If appimagetool is found ($APPIMAGETOOL or on the
# PATH), also packs it into <output dir>/NetHackModern-<version>-x86_64.AppImage;
# $APPIMAGE_RUNTIME, if set, is passed to it as the runtime file. The
# relocatable build looks for libraries in usr/lib beside the binary.

set -e

if [ $# -ne 2 ] || [ ! -x "$1/nethack" ]; then
    echo "usage: $0 <installed game dir> <output dir>" >&2
    exit 2
fi

HERE=$(cd "$(dirname "$0")" && pwd)
TOP=$(cd "$HERE/../../.." && pwd)
SRC=$(cd "$1" && pwd)
mkdir -p "$2"
OUT=$(cd "$2" && pwd)
APPDIR="$OUT/NetHackModern.AppDir"
VERSION=${VERSION:-$(git -C "$TOP" describe --tags --match 'v[0-9]*' \
                     --always 2>/dev/null \
                     || echo dev)}

rm -rf "$APPDIR"
mkdir -p "$APPDIR/usr/bin" "$APPDIR/usr/lib" \
         "$APPDIR/usr/share/nethackmodern"

# the game, and its read-only data
cp "$SRC/nethack" "$APPDIR/usr/bin/nethack"
for f in nhdat symbols license sysconf recover; do
    cp "$SRC/$f" "$APPDIR/usr/share/nethackmodern/$f"
done
cp -R "$SRC/sounds" "$APPDIR/usr/share/nethackmodern/sounds"
# the SDL window's tile sheet and font
if [ -f "$SRC/tiles.bmp" ]; then
    cp "$SRC/tiles.bmp" "$APPDIR/usr/share/nethackmodern/tiles.bmp"
fi
# the title screens' painting
if [ -f "$SRC/splash_art.png" ]; then
    cp "$SRC/splash_art.png" "$APPDIR/usr/share/nethackmodern/splash_art.png"
fi
FONT=${FONT:-/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf}
if [ -f "$FONT" ]; then
    mkdir -p "$APPDIR/usr/share/nethackmodern/fonts"
    cp "$FONT" "$APPDIR/usr/share/nethackmodern/fonts/DejaVuSansMono.ttf"
    # and the screens' fonts (title, character, death, scores), from
    # the same family where it's installed
    for face in DejaVuSerif-Bold.ttf DejaVuSans.ttf DejaVuSans-Bold.ttf; do
        if [ -f "$(dirname "$FONT")/$face" ]; then
            cp "$(dirname "$FONT")/$face"                "$APPDIR/usr/share/nethackmodern/fonts/$face"
        fi
    done
    if [ -n "$FONT_LICENSE" ] && [ -f "$FONT_LICENSE" ]; then
        cp "$FONT_LICENSE" "$APPDIR/usr/share/nethackmodern/fonts/LICENSE"
    fi
else
    echo "$0: no font at $FONT; the SDL window will look for one" >&2
fi

# bundle the libraries it links, except the C library itself, which
# must come from the system it runs on
ldd "$APPDIR/usr/bin/nethack" | awk '/=> \// { print $3 }' |
while read -r lib; do
    case "$(basename "$lib")" in
    libc.so* | libm.so* | libdl.so* | libpthread.so* | librt.so* \
    | ld-linux*) ;;
    *) cp -L "$lib" "$APPDIR/usr/lib/" ;;
    esac
done

cp "$HERE/AppRun" "$APPDIR/AppRun"
chmod 755 "$APPDIR/AppRun"
cp "$HERE/nethackmodern.desktop" "$APPDIR/nethackmodern.desktop"
cp "$TOP/sys/windows/vs/Images/Square44x44Logo.targetsize-256.png" \
   "$APPDIR/nethackmodern.png"
ln -sf nethackmodern.png "$APPDIR/.DirIcon"
echo "made $APPDIR"

TOOL=${APPIMAGETOOL:-$(command -v appimagetool || true)}
if [ -n "$TOOL" ]; then
    IMAGE="$OUT/NetHackModern-$VERSION-x86_64.AppImage"
    if [ -n "$APPIMAGE_RUNTIME" ]; then
        ARCH=x86_64 "$TOOL" --runtime-file "$APPIMAGE_RUNTIME" \
            "$APPDIR" "$IMAGE"
    else
        ARCH=x86_64 "$TOOL" "$APPDIR" "$IMAGE"
    fi
    echo "made $IMAGE"
else
    echo "appimagetool not found; only the AppDir was made"
fi
