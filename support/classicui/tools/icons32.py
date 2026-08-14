#!/usr/bin/env python3
"""
Generates the icon headers from RetroArch's "monochrome" icon set and Twemoji:

    chome_icons32.h   one per system, for the shelf (N below, currently 64x64)
    chome_icons16.h   16x16 pictograms used as badges and button prompts

    python3 support/classicui/tools/icons32.py            # fetch and generate
    python3 support/classicui/tools/icons32.py --keep      # ...and leave the PNGs

Why this set: they are already flat single-colour silhouettes with the details cut
out of them as negative space, drawn to read at small sizes. Reducing one to a 32x32
1-bit mask keeps the shape and the cut-outs, which is what a system needs to be
recognisable on a shelf. Nothing here is drawn by hand.

    Sources: libretro/retroarch-assets (xmb/monochrome), and jdecked/twemoji for
             the one icon the first set has no equivalent for.
    Licence: both CC BY 4.0, https://creativecommons.org/licenses/by/4.0/
    Changes: cropped, scaled to 32x32, reduced to one bit per pixel.

The source commit is pinned, so re-running this produces the same icons rather than
whatever upstream looks like today, and every icon records the sha256 of the file it
came from. See support/classicui/ICONS.md for the attribution that ships.

Only the alpha channel is read: the art is one flat colour on transparency, so alpha
*is* the drawing. Coverage is area-averaged over each 8x8 block and thresholded; the
threshold was picked by eye against the icons where the cut-outs are finest (the Neo
Geo's ring, the Game Boy's screen, every d-pad). Higher and the holes fill in.
"""

import argparse, hashlib, os, struct, sys, urllib.parse, urllib.request, zlib

# Both sets are CC BY 4.0, and both commits are pinned so that re-running this
# produces the same icons rather than whatever upstream looks like today.
SOURCES = {
    "retroarch": dict(
        who="RetroArch monochrome icons, libretro/retroarch-assets",
        commit="0959892093bdf85d96206993685a7450b26a1732",
        base="https://raw.githubusercontent.com/libretro/retroarch-assets/"
             "{commit}/xmb/monochrome/png/{name}.png"),
    "twemoji": dict(
        who="Twemoji, jdecked/twemoji",
        commit="b6b55fef1e8636b540a6d016a4729ca8cdf2e60b",
        base="https://raw.githubusercontent.com/jdecked/twemoji/"
             "{commit}/assets/72x72/{name}.png"),
}

# The card can give a system icon up to about 114 pixels at HD, so a 32x32 source was
# being blown up three and a half times and looked it. 64 halves that; the cost is about
# a hundred kilobytes of header, which is the trade being made deliberately.
N = 64
THRESHOLD = 0.42
FIT_MARGIN = 2          # air left around the drawing inside its box

# The pictograms are half the size and stay legible there because they are simple
# shapes. The same reduction of a console silhouette to 16 pixels is mush, which is
# why systems get 32 and these do not.
N16 = 16

# ...and an 8x8 for the 240p legend, where a 16x16 mask cannot survive the reduction.
N8 = 8

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "chome_icons32.h")
OUT16 = os.path.join(HERE, "..", "chome_icons16.h")
CACHE = os.path.join(HERE, ".icons-cache")

# Classic Home's system ids, from the table in chome_lib.cpp, against the source and
# the file in it that depicts that machine.
MAP = [
    ("nes", "retroarch", "Nintendo - Nintendo Entertainment System"),
    ("snes", "retroarch", "Nintendo - Super Nintendo Entertainment System"),
    ("gb", "retroarch", "Nintendo - Game Boy"),
    ("gba", "retroarch", "Nintendo - Game Boy Advance"),
    ("n64", "retroarch", "Nintendo - Nintendo 64"),
    ("md", "retroarch", "Sega - Mega Drive - Genesis"),
    ("sms", "retroarch", "Sega - Master System - Mark III"),
    # Game Gear rides in the SMS core but is its own shelf row, and it borrowed the
    # Master System's drawing until this entry existed. The set has the real machine.
    ("gg", "retroarch", "Sega - Game Gear"),
    ("tg16", "retroarch", "NEC - PC Engine - TurboGrafx 16"),
    ("a7800", "retroarch", "Atari - 7800"),
    ("psx", "retroarch", "Sony - PlayStation"),
    # The one CD console with nothing to borrow: Mega CD, PC Engine CD and Neo Geo CD
    # deliberately wear the machine they bolt onto, but a Saturn is a machine of its
    # own, and without this row it drew the folder.
    ("saturn", "retroarch", "Sega - Saturn"),
    ("neogeo", "retroarch", "SNK - Neo Geo"),
    # Arcade needed a second source. There is no cabinet anywhere in the RetroArch
    # set; FBNeo's own icon is a light gun, which reads as "shooter", and the input
    # glyph that looked like a joystick turns out to be a stick being pressed - at
    # this size, a download arrow. Twemoji's joystick is a ball-top on a base, which
    # is the symbol everyone knows, and is under the same licence.
    ("arcade", "twemoji", "1f579"),
    ("lynx", "retroarch", "Atari - Lynx"),
    ("ws", "retroarch", "Bandai - WonderSwan"),
    ("ngp", "retroarch", "SNK - Neo Geo Pocket Color"),
    ("amiga", "retroarch", "Commodore - Amiga"),
    ("st", "retroarch", "Atari - ST"),
    ("c64", "retroarch", "Commodore - 64"),
    ("spec", "retroarch", "Sinclair - ZX Spectrum"),
    ("cpc", "retroarch", "Amstrad - CPC"),
    ("msx", "retroarch", "Microsoft - MSX"),
    # The "DOS" icon is a wordmark that turns to mush at this size; the IBM PC is a
    # monitor with a command prompt on it, which survives and says the same thing.
    ("ao486", "retroarch", "IBM - PC and Compatibles"),
    ("apple2", "retroarch", "Apple - II"),

    # Not a system: what a card falls back to when it is a folder, or a system this
    # table has never heard of. Kept here rather than with the pictograms so that
    # everything drawn on a card comes through the same 32x32 path.
    ("folder", "retroarch", "folder"),
]


# Pictograms: badges and prompts rather than systems. Same sources, same licence.
PICTOS = [
    # The favourite badge. RetroArch's "favorites" is a star inside a rounded
    # square, too busy for the corner of a cover; "add-favorite" is the star alone.
    ("star", "retroarch", "add-favorite"),

    # PlayStation face buttons, for prompts shown while a PSX pad is the thing in
    # someone's hands. The cross is not here: the OSD font's own X is a clean one at
    # eight pixels, where every emoji cross falls apart into dots. Twemoji has no
    # hollow triangle, so that one is filled; it still reads as a triangle.
    # Outlined, not filled: at eight pixels a filled triangle is a blob and a filled
    # square is a block, and the three shapes stop being distinguishable from each other.
    # The outline is taken from the source silhouette mechanically - see outline().
    ("psx_circle",   "twemoji", "2b55",  True),
    ("psx_triangle", "twemoji", "1f53a", True),
    ("psx_square",   "twemoji", "2b1b",  True),

    # The controller-pairing screen. This is the one symbol for "wireless" that a
    # player already knows from every phone and television, so it is worth more than
    # any picture of a gamepad - and the set has no gamepad anyway.
    ("bluetooth", "retroarch", "bluetooth"),
]


def outline(grid):
    """Keeps only the edge of a filled shape: a pixel that is ink and has a non-ink
    neighbour. Purely mechanical - no shape is drawn here, the silhouette is the source
    art's own - and it is what makes these read at eight pixels. A filled 5x5 triangle is
    a blob; its outline is a triangle."""
    h = len(grid)
    w = len(grid[0])

    def ink(x, y):
        return 0 <= x < w and 0 <= y < h and grid[y][x] == "#"

    out = []
    for y in range(h):
        row = ""
        for x in range(w):
            edge = ink(x, y) and not (ink(x - 1, y) and ink(x + 1, y)
                                      and ink(x, y - 1) and ink(x, y + 1))
            row += "#" if edge else "."
        out.append(row)
    return out


def fetch(source, name):
    src = SOURCES[source]
    os.makedirs(CACHE, exist_ok=True)
    path = os.path.join(CACHE, source + "_" + name + ".png")
    if not os.path.exists(path):
        url = src["base"].format(commit=src["commit"], name=urllib.parse.quote(name))
        with urllib.request.urlopen(url, timeout=60) as r:
            body = r.read()
        with open(path, "wb") as f:
            f.write(body)
    return path


def read_alpha(path):
    """(w, h, alpha bytes) for a non-interlaced 8-bit PNG, palette or RGBA."""
    data = open(path, "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise SystemExit(path + ": not a png")

    idat, trns = b"", b""
    w = h = ctype = None
    i = 8
    while i < len(data):
        n = struct.unpack(">I", data[i:i + 4])[0]
        tag = data[i + 4:i + 8]
        body = data[i + 8:i + 8 + n]
        if tag == b"IHDR":
            w, h, depth, ctype, _c, _f, inter = struct.unpack(">IIBBBBB", body)
            if inter:
                raise SystemExit(f"{path}: interlaced")
            # Part of the set is stored 4 bits per pixel, some of it 1 or 2: a
            # palette of a handful of colours does not need a whole byte.
            if depth not in (1, 2, 4, 8) or (depth != 8 and ctype != 3):
                raise SystemExit(f"{path}: depth {depth}, colour type {ctype}")
        elif tag == b"tRNS":
            trns = body
        elif tag == b"IDAT":
            idat += body
        i += 12 + n

    bpp = {3: 1, 4: 2, 6: 4}.get(ctype)
    if not bpp:
        raise SystemExit(f"{path}: colour type {ctype}")

    raw = zlib.decompress(idat)
    # The filters work on bytes: one byte per pixel at most, and whole rows rounded
    # up when several pixels share a byte.
    if depth == 8:
        stride = w * bpp
    else:
        stride = (w * depth + 7) // 8
        bpp = 1
    rows = bytearray(stride * h)
    prev = bytearray(stride)
    pos = 0

    for y in range(h):
        f = raw[pos]
        pos += 1
        line = bytearray(raw[pos:pos + stride])
        pos += stride

        if f == 1:
            for x in range(bpp, stride):
                line[x] = (line[x] + line[x - bpp]) & 0xff
        elif f == 2:
            for x in range(stride):
                line[x] = (line[x] + prev[x]) & 0xff
        elif f == 3:
            for x in range(stride):
                a = line[x - bpp] if x >= bpp else 0
                line[x] = (line[x] + ((a + prev[x]) >> 1)) & 0xff
        elif f == 4:
            for x in range(stride):
                a = line[x - bpp] if x >= bpp else 0
                b = prev[x]
                c = prev[x - bpp] if x >= bpp else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[x] = (line[x] + pr) & 0xff
        elif f != 0:
            raise SystemExit(f"{path}: filter {f}")

        rows[y * stride:(y + 1) * stride] = line
        prev = line

    alpha = bytearray(w * h)
    if ctype == 3:
        tab = bytes(trns) + b"\xff" * (256 - len(trns))
        if depth == 8:
            for k in range(w * h):
                alpha[k] = tab[rows[k]]
        else:
            per = 8 // depth
            mask = (1 << depth) - 1
            for y in range(h):
                base = y * stride
                for x in range(w):
                    byte = rows[base + x // per]
                    shift = 8 - depth * (x % per + 1)
                    alpha[y * w + x] = tab[(byte >> shift) & mask]
    elif ctype == 6:
        for k in range(w * h):
            alpha[k] = rows[k * 4 + 3]
    else:
        for k in range(w * h):
            alpha[k] = rows[k * 2 + 1]

    return w, h, alpha


def bbox(w, h, alpha):
    """The drawing's own bounds, ignoring the transparent border around it."""
    x0, y0, x1, y1 = w, h, -1, -1
    for y in range(h):
        base = y * w
        for x in range(w):
            if alpha[base + x] > 8:
                if x < x0: x0 = x
                if x > x1: x1 = x
                if y < y0: y0 = y
                if y > y1: y1 = y
    if x1 < 0:
        return 0, 0, w - 1, h - 1
    return x0, y0, x1, y1


def to_grid(w, h, alpha, n=None):
    """
    Crops to the drawing, scales it to fit, and thresholds to one bit.

    Cropping first is what makes the set look like a set. The system icons are drawn
    filling their frame but the input glyphs sit in a lot of air, so anything taken
    from those would arrive on the shelf half the size of its neighbours. Aspect is
    preserved - a Game Boy is taller than it is wide and has to stay that way.
    """
    if n is None:
        n = N
    fit = n - FIT_MARGIN

    x0, y0, x1, y1 = bbox(w, h, alpha)
    bw, bh = x1 - x0 + 1, y1 - y0 + 1

    if bw >= bh:
        tw = fit
        th = max(1, round(bh * fit / bw))
    else:
        th = fit
        tw = max(1, round(bw * fit / bh))

    ox, oy = (n - tw) // 2, (n - th) // 2

    out = []
    for gy in range(n):
        row = ""
        for gx in range(n):
            tx, ty = gx - ox, gy - oy
            if tx < 0 or ty < 0 or tx >= tw or ty >= th:
                row += "."
                continue

            sx0 = x0 + tx * bw // tw
            sx1 = x0 + (tx + 1) * bw // tw
            sy0 = y0 + ty * bh // th
            sy1 = y0 + (ty + 1) * bh // th
            if sx1 <= sx0: sx1 = sx0 + 1
            if sy1 <= sy0: sy1 = sy0 + 1

            s = 0
            for y in range(sy0, sy1):
                base = y * w
                for x in range(sx0, sx1):
                    s += alpha[base + x]
            cnt = (sy1 - sy0) * (sx1 - sx0)
            cov = s / (255.0 * cnt) if cnt else 0.0
            row += "#" if cov >= THRESHOLD else "."
        out.append(row)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--keep", action="store_true", help="keep the downloaded PNGs")
    args = ap.parse_args()

    icons = []
    for sid, source, name in MAP:
        path = fetch(source, name)
        digest = hashlib.sha256(open(path, "rb").read()).hexdigest()
        w, h, alpha = read_alpha(path)
        grid = to_grid(w, h, alpha)

        ink = sum(r.count("#") for r in grid)
        if ink < 40:
            raise SystemExit(f"{sid}: only {ink} pixels of ink - wrong file?")

        icons.append((sid, source, name, digest, grid))
        print(f"  {sid:7s} {ink:4d} px  {source}: {name}")

    with open(os.path.normpath(OUT), "w") as f:
        f.write(f'''/*
  Per-system icons: {N}x{N}, one bit per pixel, drawn in a single flat colour.

  GENERATED by support/classicui/tools/icons32.py - do not edit. Re-run that script
  to change the mapping or the threshold; it explains both.

  Not drawn here: they are somebody else's artwork, scaled down and reduced to one
  bit. Each icon below names the file it came from.

      RetroArch monochrome icons - https://github.com/libretro/retroarch-assets
        xmb/monochrome/png at commit {SOURCES["retroarch"]["commit"][:12]}
      Twemoji - https://github.com/jdecked/twemoji
        assets/72x72 at commit {SOURCES["twemoji"]["commit"][:12]}

      Licence: both are Creative Commons Attribution 4.0 International (CC BY 4.0)
               https://creativecommons.org/licenses/by/4.0/
      Changes: cropped to the drawing, scaled to fit its box with the aspect
               kept, and reduced to one bit per pixel at a coverage threshold
               of {THRESHOLD}.

  The attribution that ships with the firmware is in support/classicui/ICONS.md and
  on the About screen. A system with no entry here falls back to the generic set.
*/

#ifndef CHOME_ICONS32_H
#define CHOME_ICONS32_H

#define ICON_SYS {N}

struct sysicon_def {{ const char *id; const char *rows[ICON_SYS]; }};

static const sysicon_def sysicons[] =
{{
''')
        for sid, source, name, digest, grid in icons:
            f.write(f'\t// {name}  ({source}, sha256 {digest[:16]})\n')
            f.write(f'\t{{ "{sid}", {{\n')
            for r in grid:
                f.write(f'\t\t"{r}",\n')
            f.write("\t} },\n")
        f.write("};\n\n#endif\n")

    pictos = []
    for entry in PICTOS:
        pid, source, name = entry[0], entry[1], entry[2]
        want_outline = len(entry) > 3 and entry[3]

        path = fetch(source, name)
        digest = hashlib.sha256(open(path, "rb").read()).hexdigest()
        w, h, alpha = read_alpha(path)
        grid = to_grid(w, h, alpha, N16)

        # A native 8x8 as well. picto() samples the mask down to the box it is given, and
        # at 240p that box is 8 pixels - where a one-pixel outline drawn at 16x16 loses
        # half of itself to the subsampling and the shapes stop being shapes. So reduce
        # first, then outline, and let the front-end pick the size it is actually drawing.
        grid8 = to_grid(w, h, alpha, N8)

        if want_outline:
            grid = outline(grid)
            grid8 = outline(grid8)

        ink = sum(r.count("#") for r in grid)
        if ink < 8:
            raise SystemExit(f"{pid}: only {ink} pixels of ink - wrong file?")

        pictos.append((pid, source, name, digest, grid, grid8))
        print(f"  {pid:7s} {ink:4d} px  {source}: {name}  (16x16)")

    with open(os.path.normpath(OUT16), "w") as f:
        f.write(f"""/*
  Pictograms: {N16}x{N16}, one bit per pixel, drawn in a single flat colour.

  GENERATED by support/classicui/tools/icons32.py - do not edit.

  Same sources and the same CC BY 4.0 licence as the system icons; see the header of
  chome_icons32.h and support/classicui/ICONS.md.
*/

#ifndef CHOME_ICONS16_H
#define CHOME_ICONS16_H

#define ICON16 {N16}

struct picto_def {{ const char *name; const char *rows[ICON16]; }};

/*
  The same shapes at 8x8, reduced from the source art and *then* outlined. picto() samples
  a mask down to whatever box it is drawing into, and at 240p that box is 8 pixels - where
  a one-pixel outline authored at 16x16 loses half of itself and a triangle stops looking
  like one. The front-end picks whichever of the two fits the box.
*/
#define ICON8 {N8}

struct picto8_def {{ const char *name; const char *rows[ICON8]; }};

static const picto_def pictos[] =
{{
""")
        for pid, source, name, digest, grid, grid8 in pictos:
            f.write(f'\t// {name}  ({source}, sha256 {digest[:16]})\n')
            f.write(f'\t{{ "{pid}", {{\n')
            for r in grid:
                f.write(f'\t\t"{r}",\n')
            f.write("\t} },\n")
        f.write("};\n\nstatic const picto8_def pictos8[] =\n{\n")
        for pid, source, name, digest, grid, grid8 in pictos:
            f.write(f'\t{{ "{pid}", {{\n')
            for r in grid8:
                f.write(f'\t\t"{r}",\n')
            f.write("\t} },\n")
        f.write("};\n\n#endif\n")

    print(f"wrote {os.path.normpath(OUT16)} with {len(pictos)} pictograms")

    if not args.keep:
        # icons are 5-tuples and pictos are 6 (they carry an 8x8 grid too), so index
        # rather than unpack.
        for entry in icons + pictos:
            source, name = entry[1], entry[2]
            p = os.path.join(CACHE, source + "_" + name + ".png")
            if os.path.exists(p):
                os.remove(p)
        if os.path.isdir(CACHE) and not os.listdir(CACHE):
            os.rmdir(CACHE)

    print(f"wrote {os.path.normpath(OUT)} with {len(icons)} icons")


if __name__ == "__main__":
    main()
