# The handheld panels, and what their gaps actually do

Written because every handheld look drew the same thing — a dark gutter — and that
is right for three of the twelve panels and wrong for nine. Dinofly caught it on
the DMG: *"the pixel gap is dark, when it should be the light background of the
screen"*. He was right, and the reason is not a matter of taste.

## Three kinds of panel

A pixel gap has no electrode over it. On a **reflective** panel that means it can
never be driven, so it sits permanently in the off state — which is the lit
substrate, and therefore *brighter* than a dark pixel. On a **backlit**
transmissive panel the gap needs a real black matrix, or the backlight leaks
between cells. That single distinction sorts the whole set.

| class | gap | shadow | machines |
|---|---|---|---|
| mono reflective | **1.15×**, lit substrate | yes | DMG ×3, Pocket, WonderSwan |
| colour reflective | **0.92×**, a thin dark line | yes, gentler | GBC, AGB-001, AGS-001, WS Color, NGPC |
| backlit | **0.80×**, a real matrix | **none** | AGS-101, Game Gear, Lynx |

Backlit panels get no pixel shadow at all. A drop shadow is a pixel casting onto
a reflector; a transmissive panel has nothing behind it to cast onto.

The colour-reflective number is not a compromise between the other two. A colour
panel has RGB subpixels behind a colour filter array, and that array does leave a
dark line — [GBCC](https://gbcc.dev/technology/) measures the subpixels 2/7 of a
pixel apart, *"leaving a small dark line on the right of each pixel"*, and gives
the subpixel primaries as `#FF7145` / `#C1D650` / `#3BCEFF`.

### Sources

- [BGB "reality"](https://bgb.bircd.org/reality/index.html) — the author worked
  from macro photographs of a DMG and describes the inter-pixel edge as **yellow**
  against a *bluish* pixel square. Not merely lighter: a different hue, because it
  is the substrate rather than a driven cell.
- [GBCC technology](https://gbcc.dev/technology/) — GBC subpixel layout, spacing
  and measured primaries, as above.
- [Newhaven](https://newhavendisplay.com/blog/transmissive-vs-reflective-vs-transflective-displays/)
  on why the black matrix exists at all: it absorbs backlight. Reflective designs
  omit it.
- Game Gear and Lynx were CCFL **backlit**; WonderSwan and Neo Geo Pocket Color
  were reflective and had famously long battery life for exactly that reason.
- GBA AGB-001 is a reflective TFT; AGS-001 is *frontlit*, so still a reflective
  panel; AGS-101 is backlit. That is why the three GBA looks are not all one class.

Contrary reading, recorded because it looks authoritative and is not about the
panel: **SameBoy's `LCD.fsh` darkens its gap** (`SCANLINE_DEPTH 0.2`). That is a
stylistic scanline, not a claim about the substrate, and the display physics goes
the other way.

## Why the grid is a mask and the shadow is a filter

The fabric's order, read off `sys_top.v` in Template_MiSTer, is

    core video -> video_mixer (gamma_corr) -> ascal -> shadowmask -> osd -> HDMI

so **gamma runs before the scaler** and **the mask after it**. There is no
programmable stage after the mask. Two consequences:

**The grid belongs in the mask.** A filter runs on each axis separately, so two
gap lifts multiply and every intersection comes out as their square — 1.18× lines
gave 1.39× crossings, a bright dot at every cell corner. Dinofly spotted those on
screen. A mask is a 2D cell table, so the corner is written with the same
multiplier as the lines and the dots are gone. Mask cells are *output* pixels and
`shadowmask.sv` anchors its counters to `brd_in`, the active picture border, which
is why a mask lines up with the picture rather than the screen — and why it must
be rewritten whenever the magnification changes.

**The shadow belongs in the filter**, because it is the one part that depends on
the picture: it exists only where a pixel casts. The core's own `Screen Shadow` is
the instructive contrast — measured on hardware, it darkens *all seven output
samples of a cell by the same amount*, because the core draws at 160×144. The
filter's is evaluated per output sample and fades across the cell. `Screen Shadow`
is now off in the Game Boy looks.

One hard ceiling: ascal has four taps spanning `i-1..i+2`, so the pixel a shadow
mixes in is always the one immediately to its left. **A pixel cannot cast past its
own neighbour**, so the shadow's falloff is capped at one cell.

## The mask word

`shadowmask.sv` lines 80-82:

```verilog
r_mul <= lut[10] ? {1'b1,lut[7:4]} : {1'b0,lut[3:0]};
g_mul <= lut[9]  ? {1'b1,lut[7:4]} : {1'b0,lut[3:0]};
b_mul <= lut[8]  ? {1'b1,lut[7:4]} : {1'b0,lut[3:0]};
```

So each cell is 11 bits: three select bits and two 4-bit nibbles. A channel takes
either `{1, high nibble}` — 16..31, i.e. **1.0 to 1.9375** — or `{0, low nibble}`
— 0..15, i.e. **0 to 0.9375**. The multiply is a shift-add of the selected bits
(`r2[7:4]` for bit 0 through `r2[7:0]` for bit 4), which is why the value is read
as 1.4 fixed point.

Unity is therefore a *bright* selection with a **zero** high nibble, not anything
expressible on the dim side. A gap above unity uses the bright branch; a gap below
unity has to switch the channels to the dim branch instead. Getting that wrong is
how the first version of `simulate_pipeline.py` clamped every dark gap to unity
and silently drew no grid at all on the colour and backlit panels.

## Hardware verification, 2026-08-20

Zelda: Link's Awakening on the Game Boy core at **6.99×** (a 1119×1007 picture in
1080p), captured over HDMI. Luma read from the **raw UYVY** bytes rather than from
a converted PNG, and corrected for studio range's +16 offset — both of which
matter here, and the first pass through RGB was wrong for the second reason.

Verified:

- **The mask is rebuilt for the running scale.** The file's own header read
  `# LCD gap for 7x at 1.15x` after the core came up, having been 4× from the
  previous session.
- **It aligns with the picture.** The observed period is exactly 7 output pixels,
  one cell per source pixel, stable across the frame — which is `brd_in`
  anchoring doing its job.
- **The gap is lighter than the body, on both axes.** Over a uniform 8×6 source
  block: body 127.9, vertical gap lines **1.123×**, against a designed 1.125
  (nibble 2 → 1 + 2/16). That is the reflective panel behaviour, and the number is
  the design's.
- **The corner is not the square.** 1.21× measured, against 1.266 for a separable
  filter. The bright-corner artefact is gone.
- **The look's core half applies too**: the Olive palette is live, its lightest
  shade landing at luma 135 exactly as authored.

Open, and honestly unresolved: the corner measures 1.21 where the mask should make
it identical to the lines at 1.125. It is clearly not the separable 1.266, so the
mechanism is right, but the residual is unexplained. The likeliest cause is the
sampling phase — the scale is 6.993 rather than a clean 7, so a fixed 7-pixel
category assignment drifts, and the two gap lines inside one measured block were
not equal (row 7 read brighter than row 0). Settling it wants a flat synthetic
field rather than a game frame, and device control to put one up.

Not verified: the colour-reflective and backlit gaps, and the shadow, all of which
have only been rendered through `simulate_pipeline.py`. The device dropped off the
network before those could be shot.
