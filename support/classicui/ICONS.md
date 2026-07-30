# Icons in Classic Home

The icons in `chome_icons32.h` (32x32, one per system) and `chome_icons16.h` (16x16
pictograms) are **not original artwork**. They are other people's icons, scaled down
and reduced to one bit per pixel by `tools/icons32.py`, which is the only thing that
should ever write those headers.

Nothing pictorial in the front-end is drawn by hand. Where an icon was wanted and
neither set had one, the answer was MiSTer's own OSD font ROM, which already carries
a closed padlock at 0x17 and an open one at 0x18 - used for locked savestate slots
and for secured networks in the Wi-Fi list.

Both sources are licensed **CC BY 4.0**
(<https://creativecommons.org/licenses/by/4.0/>), which is one-way compatible with
the GPLv3 this firmware ships under, and which requires that they be credited, that
the licence be named, and that changes be stated.

## Sources

- **RetroArch monochrome icons** — the `xmb/monochrome/png` set from
  [libretro/retroarch-assets](https://github.com/libretro/retroarch-assets),
  © the libretro/RetroArch contributors, CC BY 4.0.
  All the system icons but one, plus the favourite star (its `add-favorite`) and the
  folder a card falls back to.
- **Twemoji** — [jdecked/twemoji](https://github.com/jdecked/twemoji),
  © 2020 Twitter, Inc and other contributors; graphics CC BY 4.0.
  The joystick used for Arcade. Twemoji's own README names an "About" section as
  acceptable attribution, which is where this appears on screen.

Both commits are pinned in `tools/icons32.py`, and every icon in the generated
header records the sha256 of the file it came from, so what shipped can always be
traced back to what it was made from.

## Changes made to the originals

Cropped to the drawing's own bounds, scaled to fit 30x30 with the aspect ratio kept,
centred in a 32x32 grid, and reduced to one bit per pixel at a coverage threshold of
0.42. Only the alpha channel is read: the artwork is a flat colour on transparency,
so the alpha *is* the drawing, and the result is a mask the front-end fills with one
colour of its own.

## Where the credit appears to a user

On the About screen, in the front-end itself. If the icon set changes, that screen
has to change with it.

## What is deliberately not an icon

Signal-strength bars in the Wi-Fi list, the radio dots on the Display screen and the
text caret on the keyboard are drawn from rectangles here. They are gauges and
controls whose *state* is the whole point - a four-level bar cannot be a fixed
picture - not pictures of things.

## Why these two

They are already flat single-colour silhouettes with their details cut out as
negative space, drawn to be read small — so an 8:1 reduction keeps the shape and the
cut-outs instead of turning into mush. Two icons in the RetroArch set were replaced
because they say the wrong thing at this size, and `tools/icons32.py` records why
next to each of them.
