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
- **The corner is correct in the fabric** - the residual is the capture instrument, see below.
- **The look's core half applies too**: the Olive palette is live, its lightest
  shade landing at luma 135 exactly as authored.

## The corner test, 2026-08-21: the mask is a real 2D table

The corner was withdrawn as a failure and should not have been. Three tests on
hardware, all on a flat field from the Game Boy core with no cartridge, so there
is no content for a filter to act on:

**1. A deliberately impossible corner.** A mask with lines at 1.50× and the corner
at **0.50×** — darker than the body — with every filter off, so only the mask can
touch the picture. Measured over 12,000 cells: body 1.000, lines 1.553 / 1.561,
corner **0.375**. The corner came out *darker than the body*, which no separable
mechanism can produce. The 11-bit table is addressed as a table. That also matches
the RTL: `mask_idx <= {vindex, hindex}`, a 256-entry lookup at stride 16, with
`setShadowMask` uploading 16 words per row and `SM_HMAX(w-1)` bounding `hindex`.

**2. The filter contributes nothing.** The real DMG look and the gap mask *alone*
with filters off produced **byte-identical means** — body 130.20, row 145.57, col
146.36, corner 158.70 in both. So the elevated corner was never the shadow filter.
Independently, the device's `Pixel Shadow.txt` is byte-identical to what
`simulate_pipeline.py` emits, at all 64 phases, and every phase row sums to 128.

**3. The excess is the capture instrument, and this time it is measured.** A Fable
agent reverse-engineered the pipeline from the RTL and settled it on hardware. Two
experiments decide it:

**The excess follows geometry, not the LUT index.** A *shifted* grid — bright row 3
and column 3, with cell {0,0} carrying the body word — put the crossing at interior
index {3,3}, where nothing happens in the RTL: no counter reset, no border fudge,
no table edge, `hcount` and `mask_idx` merely increment. It rendered **158.71**
against the original corner's **158.77** at index {0,0}. The anomaly moved with the
crossing. That refutes the registered-lookup-across-a-counter-reset hypothesis and
every upload-corruption story with it — and static analysis agrees, because the
index/LUT/mul pipeline is a uniform two-clock lag, which can only translate the
whole mask sideways, never change a value.

**A uniform mask calibrates the wire.** An all-0x720 table multiplies every pixel
by 1.125 and produces a flat field the capture cannot spatially alter: it reads
**142.00 raw**, all 49 phase classes identical to 0.01. So in dongle units the
truth is body 128.0, line level 142.0. Against that:

| feature (fabric truth) | reads | delta |
|---|---|---|
| flat body {128.0} | 128.0 | 0 — transparent on flat |
| flat 1.125× field {142.0} | 142.0 | 0 |
| isolated 1px bright dot {142} | 130.8 | **−11.2, annihilated** |
| isolated 1px line {142} | 142.2–142.7 | ≈0, with ±1 undershoot / ±2 overshoot |
| grid line pixel {142} | 144.4–147 | +2.4 … +5 |
| grid crossing {142} | 152.3 | **+10.3 — the anomaly** |
| 1px dark hole at a crossing {128} | 140.8 | **+12.8, filled to line level** |
| 2×2 crossing at maskmode=2x {142} | 143.5 | +1.5, uniform |

An isolated bright pixel is deleted; a dark pixel surrounded by bright ones is
filled in. That is content-adaptive single-pixel suppression — a denoise/enhance
engine in the capture chip — and it boosts thin lines in proportion to local
pattern density, which is why a crossing gains most. Superposition fails
quantitatively (row excess 14.7 + column excess 16.3 ≠ crossing excess 29.1), so it
is nonlinear, not a linear kernel, and it collapses at 2px feature size.

That the engine is in the dongle rather than the transmitter is **inferred**, not
measured: everything between the mask and the HDMI pins is pointwise (the OSD
passes `din` through when hidden, then one output register), and the vertical
ringing and 2D dot deletion both need line buffers, which an ADV7513 does not have.
A second capture device on the same output is the experiment that would close it.

**So the fabric is right and the model is right.** The corner on the wire is
byte-identical to the lines, as the design intends, and the +11.7 was never in the
picture.

### What this means for measuring with the dongle

**It is only trustworthy for features two output pixels wide or more.** A
single-pixel claim measured through it is worthless — and worse, plausible. The way
to validate a single-pixel mask word is a **uniform field of that word**, which the
adaptive engine cannot touch; that is how the 142.00 truth above was established,
and it is the technique to reuse for any future mask.

An earlier entry here said the dongle neither blurs nor rings. That was over-read
from one measurement — horizontal body at ±1 from a line, which really is flat
(−0.26). It was never checked at ±2 (**+1.3 overshoot**) or vertically (**−1.6 at
±1**). It rings, faintly and anisotropically, and it deletes isolated pixels
outright. It also smooths large single-pixel steps: a 177→135 sawtooth boundary
read 165.8/148.5.

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
| bias on corner | **−7.0** (the capture's adaptive engine, not the model - see below) |

Read that as: **structure yes, photometry approximately, corners no.** The gap
multiplier - the number every structural decision turns on - is reproduced to
0.03% on the vertical axis and 0.7% on the horizontal. Body and line pixels are
unbiased to well under half a luma level. The whole residual of consequence is a tone-response mismatch that shows as a per-shade bias swinging
+4.4 / −5.9 / −5.0 / +1.8 across the four palette entries - the same
"photometry is approximate and the error changes sign" finding as
SCALER-MODEL-2026-08-19.md, at the same magnitude.

Fitted gain 0.755 against BT.709 studio range's expected 0.859 is part of that
tone mismatch: the model's contrast is slightly wider than the transmitter's.

Practical consequence: gap direction, period, alignment, shadow width, cell
geometry can all be decided locally and confirmed on the
device at the end. Exact brightness cannot - anything resting on a few luma levels
needs a capture, and even then the capture is the weaker instrument for features
one output pixel wide.

Not verified at all: the colour-reflective and backlit gaps, and the shadow, all
of which have only been rendered through the model.
