#!/usr/bin/env python3
"""Bake the Inter VLW fonts the display draws with (data/fonts/*.vlw).

LovyanGFX draws VLW fonts anti-aliased from 8-bit alpha bitmaps. They do not scale well at
runtime, so every size the UI uses is baked here at its native pixel size. Inter is a variable
font; the weight axis is set per font (`set_variation_by_axes`).

Usage (from Display/firmware):
    python3 scripts/build_fonts.py                       # all fonts -> data/fonts/
    python3 scripts/build_fonts.py --ttf path/to/Inter.ttf

The output is checked in, so a normal firmware build never needs Python or the font.

VLW layout (all integers big-endian int32):
    header   gCount, version(11), size, 0, ascent, descent
    glyphs   gCount * (unicode, height, width, xAdvance, dY, dX, 0)
    bitmaps  concatenated 8-bit alpha, width*height per glyph, in glyph order
    trailer  length-prefixed font name + PostScript name
"""

from __future__ import annotations

import argparse
import struct
import sys
from dataclasses import dataclass
from pathlib import Path

from fontTools.ttLib import TTFont
from PIL import Image, ImageDraw, ImageFont, features

DEFAULT_TTF = Path.home() / "Library" / "Fonts" / "Inter-VariableFont_slnt,wght.ttf"
OUT_DIR = Path(__file__).resolve().parent.parent / "data" / "fonts"


def chars(*ranges: tuple[int, int] | str) -> list[int]:
    out: list[int] = []
    for r in ranges:
        if isinstance(r, str):
            out.extend(ord(c) for c in r)
        else:
            out.extend(range(r[0], r[1] + 1))
    return sorted(set(out))


# Everything a street name, a camera label or a UI string can contain: ASCII, Latin-1,
# Latin Extended-A (Polish, Czech, ... street names) and the punctuation the UI uses.
TEXT = chars(
    (0x20, 0x7E),
    (0xA0, 0xFF),
    (0x100, 0x17F),
    "–—‘’‚“”„•…‹›"
    "←↑→↓−✓✕·°",
)
DIGITS = chars("0123456789")
SPEED = chars("0123456789–-")
NUMBER = chars("0123456789 ,.:–-+%ØABCDEF")
CLOCK = chars("0123456789:")


@dataclass
class Spec:
    name: str
    size: int
    weight: float
    codepoints: list[int]
    tabular: bool = False
    tracking: float = 0.0  # extra advance per glyph, in em


SPECS = [
    # Speed in the middle of the HUD, and bigger in the focus layout.
    Spec("speed", 150, 700, SPEED, tabular=True, tracking=-0.02),
    Spec("speed_xl", 196, 700, SPEED, tabular=True, tracking=-0.025),
    Spec("speed_md", 80, 700, SPEED, tabular=True, tracking=-0.015),
    # Distances, countdowns, the display id.
    Spec("num", 44, 700, NUMBER, tabular=True),
    Spec("num_sm", 30, 700, NUMBER, tabular=True),
    # Digits inside the round speed-limit signs.
    Spec("sign_lg", 52, 700, DIGITS),
    Spec("sign_md", 38, 700, DIGITS),
    Spec("sign_sm", 28, 700, DIGITS),
    Spec("sign_xs", 21, 700, DIGITS),
    # Text.
    Spec("title", 30, 600, TEXT),
    Spec("label", 26, 600, TEXT),
    Spec("body", 23, 500, TEXT),
    Spec("small", 18, 600, TEXT),
    # Standby clock.
    Spec("clock", 104, 300, CLOCK, tabular=True),
    # The map attribution.
    Spec("tiny", 13, 500, chars((0x20, 0x7E), (0xA0, 0xFF))),
]


def load_font(path: Path, size: int, weight: float) -> ImageFont.FreeTypeFont:
    font = ImageFont.truetype(str(path), size, layout_engine=ImageFont.Layout.RAQM)
    try:
        axes = font.get_variation_axes()
    except OSError:
        return font  # static font, nothing to set
    values = []
    for axis in axes:
        name = axis.get("name", b"")
        name = name.decode() if isinstance(name, bytes) else str(name)
        if name.lower().startswith("weight"):
            values.append(weight)
        else:
            values.append(axis["default"])
    font.set_variation_by_axes(values)
    return font


def render_glyph(font, code: int, pad: int, ascent: int, feats: list[str] | None):
    """Return (width, height, dX, dY, advance, alpha_bytes) for one code point."""
    char = chr(code)
    canvas = Image.new("L", (pad * 4, pad * 4), 0)
    draw = ImageDraw.Draw(canvas)
    draw.text((pad, pad), char, font=font, fill=255, anchor="la", features=feats)
    advance = font.getlength(char, features=feats)
    box = canvas.getbbox()
    if box is None:  # whitespace
        return 0, 0, 0, 0, advance, b""
    left, top, right, bottom = box
    glyph = canvas.crop(box)
    baseline = pad + ascent
    return right - left, bottom - top, left - pad, baseline - top, advance, glyph.tobytes()


def build(ttf: Path, spec: Spec, cmap: dict[int, str]) -> None:
    font = load_font(ttf, spec.size, spec.weight)
    ascent, _descent = font.getmetrics()
    pad = spec.size * 2 + 8
    feats = ["tnum"] if spec.tabular else None
    if feats and not features.check("raqm"):
        sys.exit("tabular digits need Pillow with libraqm")

    missing = [c for c in spec.codepoints if c not in cmap]
    codes = [c for c in spec.codepoints if c in cmap]

    metrics = []
    bitmaps = []
    for code in codes:
        width, height, dx, dy, advance, alpha = render_glyph(font, code, pad, ascent, feats)
        adv = round(advance + spec.tracking * spec.size)
        if max(width, height, adv) > 255 or not -128 <= dx <= 127:
            sys.exit(f"{spec.name}: U+{code:04X} does not fit VLW's 8-bit metrics")
        metrics.append((code, height, width, adv, dy, dx))
        bitmaps.append(alpha)

    max_ascent = max(m[4] for m in metrics)
    max_descent = max(m[1] - m[4] for m in metrics)

    parts = [struct.pack(">6i", len(metrics), 11, spec.size, 0, max_ascent, max_descent)]
    for code, height, width, adv, dy, dx in metrics:
        parts.append(struct.pack(">7i", code, height, width, adv, dy, dx, 0))
    parts.extend(bitmaps)
    name = f"Inter {int(spec.weight)} {spec.size}".encode()
    parts.append(struct.pack(">h", len(name)) + name)
    parts.append(struct.pack(">h", len(name)) + name)
    parts.append(b"\x01")

    out = OUT_DIR / f"{spec.name}.vlw"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(b"".join(parts))
    note = f", missing {' '.join(f'U+{c:04X}' for c in missing)}" if missing else ""
    print(f"{out.name:14} {len(metrics):4} glyphs  {spec.size:3} px  wght {int(spec.weight)}  "
          f"{out.stat().st_size / 1024:7.1f} kB{note}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--ttf", type=Path, default=DEFAULT_TTF, help="Inter variable TTF")
    args = parser.parse_args()
    if not args.ttf.exists():
        sys.exit(f"{args.ttf} not found - install Inter (OFL, https://rsms.me/inter/)")
    cmap = TTFont(str(args.ttf)).getBestCmap()
    for spec in SPECS:
        build(args.ttf, spec, cmap)


if __name__ == "__main__":
    main()
