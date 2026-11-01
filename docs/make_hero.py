# /// script
# dependencies = ["pillow"]
# ///
"""The README's hero image: a row of screens from the preview, each in a round bezel.

    uv run docs/make_hero.py      # after firmware/tools/preview/render.sh
"""
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter

ROOT = Path(__file__).resolve().parent.parent
SHOTS = ["08-light-red-slower", "12-turn", "25-map-tilt", "07-camera-too-fast", "35-map-chargers"]
D = 300        # glass diameter
BEZEL = 14     # case ring around it
GAP = 36
PAD = 48
BG = (11, 11, 13)

w = PAD * 2 + len(SHOTS) * (D + 2 * BEZEL) + (len(SHOTS) - 1) * GAP
h = PAD * 2 + D + 2 * BEZEL
S = 2  # drawn at twice the size, then scaled down: smooth edges
img = Image.new("RGB", (w * S, h * S), BG)

for i, name in enumerate(SHOTS):
    x = (PAD + i * (D + 2 * BEZEL + GAP)) * S
    y = PAD * S
    outer = (D + 2 * BEZEL) * S
    # Soft shadow, then the case ring with a faint top highlight.
    shadow = Image.new("L", img.size, 0)
    ImageDraw.Draw(shadow).ellipse((x, y + 10 * S, x + outer, y + outer + 10 * S), fill=150)
    img.paste((0, 0, 0), mask=shadow.filter(ImageFilter.GaussianBlur(18 * S)))
    ring = ImageDraw.Draw(img)
    ring.ellipse((x, y, x + outer, y + outer), fill=(38, 39, 43))
    ring.arc((x + S, y + S, x + outer - S, y + outer - S), 200, 340, fill=(70, 72, 78), width=2 * S)
    glass = Image.open(ROOT / "firmware/preview" / f"{name}.png").convert("RGB").resize((D * S, D * S), Image.LANCZOS)
    mask = Image.new("L", glass.size, 0)
    ImageDraw.Draw(mask).ellipse((0, 0, D * S - 1, D * S - 1), fill=255)
    img.paste(glass, (x + BEZEL * S, y + BEZEL * S), mask)

img = img.resize((w, h), Image.LANCZOS)
img.save(ROOT / "docs/images/hero.png", optimize=True)
print("docs/images/hero.png", img.size)
