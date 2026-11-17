#!/usr/bin/env python3
"""Draw the BotW Native launcher emblem: a Sheikah-style eye (three lashes, an almond eye with an iris, a teardrop)
inside the same tick ring and slate disc as the TotK Native emblem (make_icon.py), in Sheikah blue and gold.

    python3 make_icon_botw.py [--out botw-native-1024.png] [--size 1024]
Deterministic: pure Pillow vector drawing, no fonts, no randomness, transparent background (the APK builder puts it
on the adaptive background and fits it inside the safe zone).
"""
import argparse
import math
from pathlib import Path
from PIL import Image, ImageChops, ImageDraw, ImageFilter

HERE = Path(__file__).resolve().parent
SLATE, DEEP = (14, 26, 44, 255), (7, 13, 24, 255)
BLUE, BLUE_DIM, GOLD = (92, 200, 255, 255), (40, 120, 190, 255), (226, 182, 92, 255)
SS = 4  # supersampling factor


def ring(draw, c, r, width, color):
    draw.ellipse((c - r, c - r, c + r, c + r), outline=color, width=width)


def polar(c, r, degrees):
    a = math.radians(degrees - 90)
    return c + r * math.cos(a), c + r * math.sin(a)


def arc_points(cx, cy, r, a0, a1, steps=64):
    """Points along a circle (screen coordinates, degrees measured clockwise from +x)."""
    return [(cx + r * math.cos(math.radians(a0 + (a1 - a0) * i / steps)),
             cy + r * math.sin(math.radians(a0 + (a1 - a0) * i / steps))) for i in range(steps + 1)]


def emblem(size):
    n = size * SS
    c, u = n / 2, n / 1024
    P = lambda x, y: (c + x * u, c + y * u)  # noqa: E731  design grid -> pixels
    base = Image.new('RGBA', (n, n), (0, 0, 0, 0))
    shade = Image.linear_gradient('L').resize((n, n)).point(lambda v: v * 0.55)
    disc = Image.composite(Image.new('RGBA', (n, n), DEEP), Image.new('RGBA', (n, n), SLATE), shade)
    mask = Image.new('L', (n, n), 0)
    ImageDraw.Draw(mask).ellipse((c - 486 * u, c - 486 * u, c + 486 * u, c + 486 * u), fill=255)
    base.paste(disc, (0, 0), mask)

    lines = Image.new('RGBA', (n, n), (0, 0, 0, 0))
    g = ImageDraw.Draw(lines)
    ring(g, c, 470 * u, round(12 * u), BLUE)
    ring(g, c, 398 * u, round(5 * u), GOLD)
    for i in range(48):
        long = i % 6 == 0
        g.line([polar(c, (404 if long else 420) * u, i * 7.5), polar(c, 452 * u, i * 7.5)],
               fill=GOLD if long else BLUE_DIM, width=round((9 if long else 5) * u))

    def dot(x, y, r, fill=BLUE):
        g.ellipse((x - r * u, y - r * u, x + r * u, y + r * u), fill=fill)

    def curve(pts, w):  # a polyline in grid units with round caps and joints
        px = [P(x, y) for x, y in pts]
        g.line(px, fill=BLUE, width=round(w * u), joint='curve')
        for x, y in (px[0], px[-1]):
            dot(x, y, w / 2)

    def arc(cx, cy, r, a0, a1, steps=48):  # grid units; degrees clockwise from +x
        return [(cx + r * math.cos(math.radians(a0 + (a1 - a0) * i / steps)),
                 cy + r * math.sin(math.radians(a0 + (a1 - a0) * i / steps))) for i in range(steps + 1)]

    def bow(x0, x1, y_end, y_mid, steps=64):  # a circular arc from (x0, y_end) to (x1, y_end) through (0, y_mid)
        h, s = abs(x1 - x0) / 2, y_end - y_mid
        r = (h * h + s * s) / (2 * abs(s))
        cy = y_mid + (r if s > 0 else -r)
        span = math.degrees(math.asin(h / r))
        mid = 270 if s > 0 else 90
        return arc(0, cy, r, mid - span, mid + span, steps) if s > 0 else arc(0, cy, r, mid + span, mid - span, steps)

    # The eye, after the Sheikah crest: a heavy upper lid whose ends curl up and outward, a lighter lower lid,
    # a ringed iris, three triangular lashes and a long hanging teardrop. Grid units; y grows downward.
    ey = -21  # centres the whole mark (lash tips to drop end) on the disc
    curve(bow(-215, 215, ey, ey - 125), 26)                                   # upper lid
    curve(arc(-215 - 34, ey, 34, 0, -150), 22)                                  # left curl
    curve(arc(215 + 34, ey, 34, 180, 330), 22)                                  # right curl
    curve(bow(-178, 178, ey + 4, ey + 112), 20)                                 # lower lid
    g.ellipse((c - 84 * u, c + (ey - 84) * u, c + 84 * u, c + (ey + 84) * u), outline=BLUE, width=round(18 * u))
    g.ellipse((c - 48 * u, c + (ey - 48) * u, c + 48 * u, c + (ey + 48) * u), outline=BLUE, width=round(13 * u))
    dot(c, c + ey * u, 20, GOLD)                                                # pupil
    # Lashes: triangles standing on the upper lid's own circle (same centre), so they follow its curve and point
    # straight out from it, with an even gap above the lid.
    lid_r = (215 * 215 + 125 * 125) / (2 * 125)
    lid_cy = ey - 125 + lid_r
    for deg, h in ((-30, 84), (0, 100), (30, 84)):
        base_r = lid_r + 13 + 34
        tip = polar(0, base_r + h, deg)
        a = polar(0, base_r, deg - 8.5); b = polar(0, base_r, deg + 8.5)
        g.polygon([P(x, y + lid_cy) for x, y in (tip, a, b)], fill=BLUE)
    # Teardrop: a stem from the lower lid that widens slightly to a round end.
    top, bottom = ey + 112, ey + 300
    g.polygon([P(-11, top), P(11, top), P(26, bottom), P(-26, bottom)], fill=BLUE)
    dot(*P(0, bottom), 27)


    glow = lines.filter(ImageFilter.GaussianBlur(22 * u))
    glow.putalpha(glow.getchannel('A').point(lambda v: v * 0.9))
    glow = Image.composite(glow, Image.new('RGBA', (n, n), (0, 0, 0, 0)), mask)
    out = Image.alpha_composite(base, glow)
    out = Image.alpha_composite(out, lines)
    edge = Image.new('L', (n, n), 0)
    ImageDraw.Draw(edge).ellipse((c - 492 * u, c - 492 * u, c + 492 * u, c + 492 * u), fill=255)
    out.putalpha(ImageChops.multiply(out.getchannel('A'), edge))
    return out.resize((size, size), Image.Resampling.LANCZOS)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, default=HERE / 'botw-native-1024.png')
    parser.add_argument('--size', type=int, default=1024)
    args = parser.parse_args(argv)
    emblem(args.size).save(args.out, format='PNG', optimize=True)
    print(f'wrote {args.out}')


if __name__ == '__main__':
    main()
