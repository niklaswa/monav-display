#!/bin/sh
# Renders every screen and scenario of the display UI to PNG on the Mac.
#
#   tools/preview/render.sh        # -> preview/*.png and preview/overview.png
#   tools/preview/render.sh -v     # also logs the app and the messages it sends
#
# Compiles the firmware's portable code (src/core, src/ui) with clang++ against LovyanGFX's
# SDL platform (no window is opened - frames are drawn into an in-memory sprite).
# Needs: Xcode command line tools, Homebrew `sdl2-compat` (or SDL2), and the libraries
# PlatformIO downloads for the firmware (fetched here if missing).
set -eu

cd "$(dirname "$0")/../.."
ROOT=$(pwd)
LIBS="$ROOT/.pio/libdeps/amoled175"
BUILD="$ROOT/.pio/preview"
OUT="$ROOT/preview"

if [ ! -d "$LIBS/LovyanGFX" ] || [ ! -d "$LIBS/ArduinoJson" ]; then
  echo "Fetching libraries (pio pkg install -e amoled175)..."
  pio pkg install -e amoled175 >/dev/null
fi

if command -v sdl2-config >/dev/null 2>&1; then
  # The prefix's include dir first: LovyanGFX includes <SDL2/SDL.h>, and a stray x86 SDL2 in
  # /usr/local/include would win otherwise on Apple silicon.
  SDL_CFLAGS="-I$(sdl2-config --prefix)/include $(sdl2-config --cflags)"
  SDL_LIBS=$(sdl2-config --libs)
else
  SDL_CFLAGS=$(pkg-config --cflags sdl2)
  SDL_LIBS=$(pkg-config --libs sdl2)
fi

CXX=${CXX:-clang++}
FLAGS="-std=c++17 -O2 -DARDUINOJSON_USE_LONG_LONG=1 $SDL_CFLAGS -I$LIBS/LovyanGFX/src -I$LIBS/ArduinoJson/src -I$ROOT/src"

mkdir -p "$BUILD/lgfx" "$OUT"

# LovyanGFX changes rarely: build its objects once.
if [ ! -f "$BUILD/lgfx/liblgfx.a" ]; then
  echo "Compiling LovyanGFX for the host (once)..."
  find "$LIBS/LovyanGFX/src" -name '*.cpp' -o -name '*.c' | while read -r f; do
    o="$BUILD/lgfx/$(echo "$f" | sed "s#$LIBS/LovyanGFX/src/##; s#/#_#g; s#\.cpp\$#.o#; s#\.c\$#.o#")"
    case "$f" in
      *.c) ${CC:-clang} -O2 -w $SDL_CFLAGS -I"$LIBS/LovyanGFX/src" -c "$f" -o "$o" ;;
      *) $CXX $FLAGS -w -c "$f" -o "$o" ;;
    esac
  done
  ar rcs "$BUILD/lgfx/liblgfx.a" "$BUILD"/lgfx/*.o
fi

echo "Compiling the preview..."
# shellcheck disable=SC2086
# The checks cover the dev-token path too (env amoled175-dev on the device).
$CXX $FLAGS '-DMONAV_DEV_TOKEN="host-dev-token"' -Wall -Wno-unused-function \
  "$ROOT"/src/core/*.cpp "$ROOT"/src/ui/*.cpp "$ROOT/tools/preview/preview.cpp" "$ROOT/tools/preview/checks.cpp" \
  "$BUILD/lgfx/liblgfx.a" $SDL_LIBS -lz -o "$BUILD/preview"

rm -f "$OUT"/*.png
"$BUILD/preview" "$ROOT/data/fonts" "$OUT" "${1:--q}" "$ROOT/tools/preview/maps"
