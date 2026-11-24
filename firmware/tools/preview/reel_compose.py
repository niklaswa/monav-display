"""Lays the reel's glass frames out on 1080 × 1080 with the scene's name underneath.

Each scene fades in from black over its first frames. Captions come from captions.tsv
(first frame, title), written by `preview -reel`.
"""
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

raw, out = Path(sys.argv[1]), Path(sys.argv[2])
font_path = Path.home() / "Library/Fonts/Inter-VariableFont_slnt,wght.ttf"


def font(size, weight):
    f = ImageFont.truetype(str(font_path), size)
    try:
        f.set_variation_by_axes([weight, 0])
    except Exception:
        pass
    return f


title_font, brand_font = font(44, 600), font(26, 500)
scenes = []
for line in (raw / "captions.tsv").read_text().splitlines():
    start, title = line.split("\t", 1)
    scenes.append((int(start), title))
frames = sorted(raw.glob("frame_*.png"))
size, glass = 1080, 820
mask = Image.new("L", (glass, glass), 0)
ImageDraw.Draw(mask).ellipse((0, 0, glass - 1, glass - 1), fill=255)

for i, path in enumerate(frames):
    start, title = max((s for s in scenes if s[0] <= i), key=lambda s: s[0])
    fade = min(1.0, (i - start) / 8.0)
    canvas = Image.new("RGB", (size, size), (0, 0, 0))
    im = Image.open(path).resize((glass, glass), Image.LANCZOS)
    if fade < 1:
        im = Image.eval(im, lambda v: int(v * fade))
    x0 = (size - glass) // 2
    # A faint bezel round the glass.
    d = ImageDraw.Draw(canvas)
    d.ellipse((x0 - 10, 40 - 10, x0 + glass + 10, 40 + glass + 10), outline=(38, 38, 42), width=6)
    canvas.paste(im, (x0, 40), mask)
    shade = int(255 * fade)
    d.text((size / 2, 935), title, font=title_font, fill=(shade, shade, shade), anchor="mm")
    d.text((size / 2, 1000), "Monav Display", font=brand_font, fill=(110, 110, 116), anchor="mm")
    canvas.save(out / f"{i:05d}.png")
    if i % 300 == 0:
        print(f"  {i}/{len(frames)}", flush=True)
