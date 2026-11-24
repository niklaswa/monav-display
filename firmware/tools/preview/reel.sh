#!/bin/sh
# A showreel of every design: each animated as on the glass, with its name underneath.
#
#   tools/preview/reel.sh            # -> preview/showreel.mp4
#   tools/preview/reel.sh notices    # -> preview/notices.mp4 (the iPhone's notifications in a drive)
#
# Builds the preview (render.sh), films the scenes frame by frame (preview -reel), lays the glass
# and the captions out with Pillow (Inter from ~/Library/Fonts) and encodes with ffmpeg.
set -eu

cd "$(dirname "$0")/../.."
ROOT=$(pwd)
FRAMES="$ROOT/.pio/reel"
MODE=${1:-reel}
if [ "$MODE" = notices ]; then OUT="$ROOT/preview/notices.mp4"; else OUT="$ROOT/preview/showreel.mp4"; fi

"$ROOT/tools/preview/render.sh" >/dev/null
rm -rf "$FRAMES" && mkdir -p "$FRAMES/raw" "$FRAMES/out"
"$ROOT/.pio/preview/preview" "$ROOT/data/fonts" "$FRAMES/raw" -q "$ROOT/tools/preview/maps" "-$MODE"
python3 "$ROOT/tools/preview/reel_compose.py" "$FRAMES/raw" "$FRAMES/out"
ffmpeg -y -loglevel error -framerate 30 -i "$FRAMES/out/%05d.png" -c:v libx264 -pix_fmt yuv420p -crf 18 \
  -movflags +faststart "$OUT"
echo "$OUT"
