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
- **The corner is NOT fixed.** See below - this is the one claim that failed.
- **The look's core half applies too**: the Olive palette is live, its lightest
  shade landing at luma 135 exactly as authored.

## The corner is still separable, and that is the point of the mask

First reading of the corner was 1.21× against 1.266 for a separable filter, and
that got written down as "the artefact is gone". It is not. Measured across **14
disjoint uniform blocks**, linearised out of studio range:

| | measured |
|---|---|
| gap line (row / col) | 1.112 / 1.122 |
| corner | **1.207** |
| what a 2D mask predicts (= the lines) | 1.118 |
| what a separable filter predicts (row × col) | 1.247 |

Mean distance from the separable product is **0.039**; from the line value it is
**0.089**. The corner sits more than twice as close to the product as to the
lines. Whatever is drawing this grid on hardware is still behaving separably.

Two candidates have been eliminated locally:

- **Not the shadow filter.** Every one of its 64 phase rows sums to 128, so it is
  unity in a flat region and cannot draw a line at all. And a transcription of
  `write_shadow_filter()` into Python is **bit-identical** to what
  `simulate_pipeline.py` emits, at all 64 phases - so tool and firmware agree.
- **Not the old grid filter sneaking back.** That one darkened its gutter; the
  measured gap is *lighter* than the body.

So the open question is whether the 11-bit mask table is being applied as a table
at all. The decisive experiment is cheap and needs the device: write a mask whose
corner cell is deliberately unlike its line cells - say lines at 1.5 and corner at
1.0 - and read one cell off a flat field. If hardware honours it, the mask is fine
and something upstream is separable; if it does not, the 2D assumption behind the
whole rework is wrong and the grid has to be built another way.

Also unchecked because the device went off the network mid-session: whether the
preset on the card actually references the gap mask. It could not be fetched.

## How good the local simulator is

The reason for asking: iterating against `simulate_pipeline.py` is far faster than
against the device, so it matters exactly how far it can be trusted. Measured over
**121,808 sampled pixels** of the whole 1120×1008 picture, model luma fitted to
hardware luma:

| | |
|---|---|
| best-fit transfer | `hw = 0.755 × model + 22.0` |
| R² | **0.932** |
| residual RMS | **7.1** luma levels (range 49..159) |
| mean absolute error | **5.0** levels |
| bias on body / gap row / gap col | **+0.16 / +0.41 / +0.01** |
| bias on corner | **−7.0** |

Read that as: **structure yes, photometry approximately, corners no.** The gap
multiplier - the number every structural decision turns on - is reproduced to
0.03% on the vertical axis and 0.7% on the horizontal. Body and line pixels are
unbiased to well under half a luma level. The whole residual of consequence is the
corner, plus a tone-response mismatch that shows as a per-shade bias swinging
+4.4 / −5.9 / −5.0 / +1.8 across the four palette entries - the same
"photometry is approximate and the error changes sign" finding as
SCALER-MODEL-2026-08-19.md, at the same magnitude.

Fitted gain 0.755 against BT.709 studio range's expected 0.859 is part of that
tone mismatch: the model's contrast is slightly wider than the transmitter's.

Practical consequence: gap direction, period, alignment, shadow width and cell
geometry can all be decided locally and confirmed on the device at the end. Exact
brightness cannot - anything resting on a few luma levels needs a capture. And the
corner cannot, until the experiment above is run.

Not verified at all: the colour-reflective and backlit gaps, and the shadow, all
of which have only been rendered through the model.
