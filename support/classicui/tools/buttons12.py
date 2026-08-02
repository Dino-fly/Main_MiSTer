#!/usr/bin/env python3
"""
Generates chome_btn12.h - the button glyphs the front-end draws wherever it names a
button.

    python3 support/classicui/tools/buttons12.py

These are the one set of graphics in the front-end that is *not* fetched from somebody
else's icon set. Two reasons. The shapes are too small to survive being reduced from
anything larger - at twelve pixels a filled triangle and a filled square are both just
blobs, and a one-pixel outline reduced from 16x16 loses half of itself. And they are
trivial geometry: a ring, a triangle, a square, a cross, and four letters. Dinofly drew a
PlayStation reference set and asked for these to match it, so they are authored here from
that reference rather than copied from it, which keeps the repo's graphics either
licensed-and-attributed or its own.

Twelve pixels is the floor he set. Below that the four PlayStation shapes stop being
distinguishable from each other, which is the whole point of using them.

Each glyph is a 12x12 grid of three states:

    .  nothing - the corners, so the chip reads as rounded rather than as a box
    k  the chip, near-black, as the plastic around a button is
    c  the accent, which the front-end fills with that button's own colour

so one grid serves every colour: circle red, triangle green, square pink, cross blue,
and the four letters in whichever palette the pad in hand uses.

That last point is why there is no separate Xbox glyph set here and should not be one. An
Xbox pad's faces are the same four letters on the same four discs as a Super Famicom's -
only the colours differ, and colour is the caller's, from chome_theme.h. Nor do the letters
need reshuffling for Xbox's swapped diamond: the caller picks a glyph from the button code
the pad reported, not from where the button sits, so the letter follows the silkscreen.
"""
import os

N = 12
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "chome_btn12.h")

# The chip: a 12x12 square with two pixels taken off each corner.
CORNER = {(0, 0), (0, 1), (1, 0), (0, 10), (0, 11), (1, 11),
          (10, 0), (11, 0), (11, 1), (10, 11), (11, 11), (11, 10)}


def chip():
    return [["." if (y, x) in CORNER else "k" for x in range(N)] for y in range(N)]


def paint(grid, spans):
    """spans: {row: [columns]} painted in the accent."""
    for y, xs in spans.items():
        for x in xs:
            grid[y][x] = "c"
    return grid


def rng(a, b):
    return list(range(a, b + 1))


# --- the four PlayStation shapes, outlined so they tell each other apart ----------
#
# All four are eight by eight inside the twelve, on rows 2-9 and columns 2-9, so they read
# as the same size as each other. The first attempt made the ring six rows tall and eight
# wide, which at this size is unmistakably an oval.
TRIANGLE = {2: [5, 6], 3: [4, 7], 4: [4, 7], 5: [3, 8], 6: [3, 8], 7: [2, 9],
            8: [2, 9], 9: rng(2, 9)}

CIRCLE   = {2: rng(4, 7), 3: [3, 8], 4: [2, 9], 5: [2, 9],
            6: [2, 9], 7: [2, 9], 8: [3, 8], 9: rng(4, 7)}

SQUARE   = {2: rng(2, 9), 3: [2, 9], 4: [2, 9], 5: [2, 9],
            6: [2, 9], 7: [2, 9], 8: [2, 9], 9: rng(2, 9)}

# The cross is solid, unlike the other three: outlining it gave two hollow diagonals that
# read as a star with a hole in it. On the pad itself it is two strokes, not an outline.
CROSS    = {2: [2, 3, 8, 9], 3: [3, 4, 7, 8], 4: rng(4, 7), 5: [5, 6],
            6: [5, 6], 7: rng(4, 7), 8: [3, 4, 7, 8], 9: [2, 3, 8, 9]}

# --- and the letters, on a round coloured button with the letter knocked out of it ----
#
# What a Super Famicom pad actually looks like, and the shape Dinofly described: a circle
# with a letter in it. The disc fills the whole twelve - a smaller face left one pixel of
# colour around the letter, and at that margin the dark strokes ran into the dark
# background and the letter stopped being a letter.
DISC = {0: rng(4, 7), 1: rng(2, 9), 2: rng(1, 10),
        3: rng(0, 11), 4: rng(0, 11), 5: rng(0, 11),
        6: rng(0, 11), 7: rng(0, 11), 8: rng(0, 11),
        9: rng(1, 10), 10: rng(2, 9), 11: rng(4, 7)}

# Six by six, on rows 3-8 and columns 3-8. Six rather than five so the letter sits exactly
# on the middle of a twelve-wide grid; three pixels of colour are left either side.
LETTERS = {
    # A flat apex, not a pointed one: pointed leaves a two-pixel counter, and a
    # two-pixel hole in a dark shape reads as a keyhole rather than as a letter.
    "a": ["·####·",
          "#····#",
          "#····#",
          "######",
          "#····#",
          "#····#"],
    "b": ["#####·",
          "#····#",
          "#####·",
          "#····#",
          "#····#",
          "#####·"],
    "x": ["#····#",
          "·#··#·",
          "··##··",
          "··##··",
          "·#··#·",
          "#····#"],
    "y": ["#····#",
          "·#··#·",
          "··##··",
          "··##··",
          "··##··",
          "··##··"],
}

# --- the direction cues -------------------------------------------------------------
#
# Not buttons, but they sit in the same legend row, and a light keycap beside a dark
# button chip was the one thing in that row that looked like it came from another
# program. Same chip, an arrow instead of a symbol. Left and right are one glyph because
# the legend always offers them together.
UP    = {4: [5, 6], 5: rng(4, 7), 6: rng(3, 8), 7: rng(2, 9)}
DOWN  = {4: rng(2, 9), 5: rng(3, 8), 6: rng(4, 7), 7: [5, 6]}


def dpad_lr():
    w = 22
    grid = [["k"] * w for _ in range(N)]
    for dy, dx in ((0, 0), (0, 1), (1, 0), (11, 0), (11, 1), (10, 0)):
        grid[dy][dx] = "."
        grid[dy][w - 1 - dx] = "."

    # A four-wide, seven-tall triangle at each end, pointing outwards.
    for i, row in enumerate(range(3, 10)):
        reach = 4 - abs(3 - i)                  # 1,2,3,4,3,2,1
        for k in range(reach):
            grid[row][6 - k] = "c"              # left arrow, tip at column 3
            grid[row][15 + k] = "c"             # and its mirror
    return grid


# --- Start and Select: a long pill with the word in it -----------------------------
#
# The same on either pad - neither a PlayStation nor a Super Famicom gives these two a
# colour or a shape, only a word - so one glyph serves both. A 3x5 alphabet, because an
# eight-pixel font would make "SELECT" nearly fifty pixels wide and the legend has three
# other prompts to fit beside it.
MINI = {
    "S": ["###", "#··", "###", "··#", "###"],
    "T": ["###", "·#·", "·#·", "·#·", "·#·"],
    "A": ["###", "#·#", "###", "#·#", "#·#"],
    "R": ["###", "#·#", "###", "##·", "#·#"],
    "E": ["###", "#··", "###", "#··", "###"],
    "L": ["#··", "#··", "#··", "#··", "###"],
    "C": ["###", "#··", "#··", "#··", "###"],
}

LETTER_W, LETTER_H, GAP, PAD = 3, 5, 1, 4


def pill(word):
    """A rounded bar N wide and 12 tall with the word across it in the accent."""
    inner = len(word) * LETTER_W + (len(word) - 1) * GAP
    w = inner + PAD * 2

    grid = [["k"] * w for _ in range(N)]
    # Round the ends: two pixels off the top and bottom of each end column pair.
    for dy, dx in ((0, 0), (0, 1), (1, 0), (11, 0), (11, 1), (10, 0)):
        grid[dy][dx] = "."
        grid[dy][w - 1 - dx] = "."

    y0 = (N - LETTER_H + 1) // 2      # a pixel lower reads as centred, 3 above and 3 below
    x = PAD
    for ch in word:
        art = MINI[ch]
        for dy, row in enumerate(art):
            for dx, c in enumerate(row):
                if c == "#":
                    grid[y0 + dy][x + dx] = "c"
        x += LETTER_W + GAP
    return grid


GLYPHS = []
for name, spans in (("psx_triangle", TRIANGLE), ("psx_circle", CIRCLE),
                    ("psx_square", SQUARE), ("psx_cross", CROSS)):
    GLYPHS.append((name, paint(chip(), spans)))

for name, art in LETTERS.items():
    g = paint([["."] * N for _ in range(N)], DISC)
    for dy, row in enumerate(art):
        for dx, ch in enumerate(row):
            if ch == "#":
                g[3 + dy][3 + dx] = "k"
    GLYPHS.append(("btn_" + name, g))

for name, spans in (("dpad_up", UP), ("dpad_down", DOWN)):
    GLYPHS.append((name, paint(chip(), spans)))
GLYPHS.append(("dpad_lr", dpad_lr()))

for word in ("START", "SELECT"):
    GLYPHS.append(("btn_" + word.lower(), pill(word)))

with open(os.path.normpath(OUT), "w") as f:
    f.write(f"""/*
  Button glyphs, {N} tall. The face buttons are {N} wide too; Start and Select are
  wider, because they carry a word - so read the width from the row length.

  GENERATED by support/classicui/tools/buttons12.py - do not edit.

  '.' is nothing, 'k' is the chip, 'c' is the accent the caller fills with that button's
  colour. Authored in this repo rather than taken from an icon set - see the tool's header
  for why - so unlike chome_icons32.h and chome_icons16.h there is nothing to attribute.
*/

#ifndef CHOME_BTN12_H
#define CHOME_BTN12_H

#define BTN12 {N}

struct btn12_def {{ const char *name; const char *rows[BTN12]; }};

static const btn12_def btn12s[] =
{{
""")
    for name, g in GLYPHS:
        f.write(f'\t{{ "{name}", {{\n')
        for row in g:
            f.write('\t\t"%s",\n' % "".join(row))
        f.write("\t} },\n")
    f.write("};\n\n#endif\n")

print("wrote %s with %d glyphs" % (os.path.normpath(OUT), len(GLYPHS)))
for name, g in GLYPHS:
    ink = sum(r.count("c") for r in g)
    print("  %-14s %3d accent px" % (name, ink))
