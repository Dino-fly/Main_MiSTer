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

## The per-look shadow models (2026-08-22)

The DMG and the Pocket each carry a **model** now, not a share of two constants.
Dinofly tuned them in `tools/shadow_bench.html` — a live render of the whole
pipeline with the four knobs and the four constraint measurements — and picked:

| | gain | offset | spread | pixel opacity | gap | floor |
|---|---|---|---|---|---|---|
| DMG    | 1.85 | 2 | 3 | 0.19 | 0.10 | on |
| Pocket | 1.25 | 1 | 4 | 0.31 | 0.08 | on |

They live in `lcd_dmg` / `lcd_pocket` in `chome_video.cpp`. The DMG family
(`dmg`, `dmg-bright`, `dmg-faded`) shares the DMG model; the Pocket has its own.
**WonderSwan deliberately does not** — it is FSTN, cooler, its own substrate, and
has not been tuned, so it keeps the generic `SHADOW_MIX_MONO` / `SHADOW_SPAN`
rather than inheriting numbers measured on a different panel.

### What of a model reaches the fabric

The full model wants the shadow **after** the grid, which needs a stage that does
not exist yet. What ships is the pre-mask scaler filter, driven by two values
derived from the model rather than guessed beside it:

- `mix` — the filter's peak depth, **measured off the bench render** at the
  leading edge of a bright pixel beside a dark one. DMG 0.59, Pocket 0.31,
  against the generic 0.45. It lands as a tap at unity 128, so 76 and 40.
- `span` — `(offset + spread)` output pixels, which `lcd_span()` converts to the
  share of a cell `write_shadow_filter()` wants. Both looks reach 5 px of a 7 px
  cell at 7×.

Measured, model against shipped path, on the Zelda frame at 7× (probes are the
bench's own, `tools/simulate_pipeline.py --look dmg`):

| | model | shipped | diff |
|---|---|---|---|
| gap buried in darkness | 66.15 | 68.15 | +2.00 |
| gap, open bright field | 139.71 | 139.71 | 0 |
| dark pixel | 52.60 | 56.25 | +3.66 |
| bright pixel, open | 139.71 | 139.71 | 0 |
| bright pixel, shadowed edge | 106.71 | 102.56 | −4.15 |

**Within ~4 luma on every probe** — much closer than the stage-order argument
suggests, and worth being precise about why. An earlier claim in this file's
history, that a pre-mask shadow cannot reach the gap, is **wrong**: a gap cell's
value is the *scaled pixel* times the gap multiplier, so a pixel already darkened
by the filter darkens the gap with it. What the pre-mask filter cannot do is
darken the gap **independently** of the pixel above it, or offset the shadow, or
blur it. The visible residue is that the filter mixes neighbouring *source*
pixels, which softens pixel edges slightly — compare the two renders and the
shipped one is a touch blurrier, not a touch flatter.

So the post-mask stage is still the right thing to build, but it buys refinement
rather than rescuing the look.

### Two couplings that had already drifted

Both were latent before these models and would have bitten harder with them:

- `vp_synth_gap_mask()` draws the **Display preview** and must match
  `write_grid_mask()`. It hardcoded a one-pixel gap (`!x || !y`), which agreed
  with the writer only because 0.15 rounds to one pixel at 6×–9×; at 12× the
  device drew two and the preview drew one. It reads the proportion now.
- Both generation call sites carried their own copy of the mask/filter list and
  had already diverged once. There is one `write_lcd_structure()` now.

`gap_frac` at 0.10 and 0.08 both round to the **same single pixel** as the old
0.15 from 6× to 9×, so 1080p looks identical whichever is used; they part at 10×
and above. They are stored as the proportion chosen, because that is what a 4K
set reads. `assert_per_look_gaps()` tests both the coincidence and the parting.

### Hardware verification, 2026-08-22

Build `834e70bd` (`Version 260822`) deployed to the device and the generated files
read back off the card. **Every value the models ask for arrives:**

| file | measured on the card | expected |
|---|---|---|
| `Pixel Shadow DMG.txt` | peak tap **76**/128 | mix 0.59 |
| `Pixel Shadow Pocket.txt` | peak tap **40**/128 | mix 0.31 |
| `Pixel Shadow.txt` | peak tap **58**/128 | 0.45, untouched |
| `LCD Gap DMG.txt` | `0.10 of a cell`, `substrate=8B9814`, `blend=3`, gap `F20` body `700` | DMG model |
| `LCD Gap Pocket.txt` | `0.08 of a cell`, `substrate=E0DBCD`, `blend=3` | Pocket model |

`F20` is `0x800` (substrate) | `0x720` (`7<<8` with lit `= lround(0.15*16) = 2`), and
the corner cell equals the line cells rather than their square. `blend=3` had never
reached the card before — the binary on it predated that fix.

Also confirmed: a 144-line core at 1080p really is scaled **7×**. `screenshot scaled`
returned **1120×1008** (and `screenshot` 160×144), so the span the model asks for
(offset+spread = 5 px) fits a 7 px cell without the clamp.

**No regression.** A/B tested by swapping `MiSTer.prev-20260822` back in: both builds
respond to input identically (framebuffer `4c465a9c` -> `50864348` on a keypress). An
earlier reading of "my build never draws" was wrong — it was a UI state where the key
sent genuinely did nothing.

**Not verified: the composited look at 7×.** The files on the card are generated for
**4×**, and that is correct behaviour rather than a bug: `vp_output_watch()` returns
early while our own framebuffer is up, so the scale reflects the last core whose
*picture* was up — a 240-line NES core, 1080/240 = 4. Reaching 7× needs a Game Boy
game actually running with the front-end's screen closed, i.e. a launch through the
shelf, and driving that headlessly did not work this session (see the memory note on
the device test path). The look itself therefore still rests on the simulator, which
tracks the model within ~4 luma; what is proven on hardware is that the numbers
reach the scaler's files intact.

## Four fixes off the television, 2026-08-23

Dinofly ran the looks on the set and found four things. Three are done; the fourth
is a design note.

### The substrate leaked outside the picture (fixed, needs a core rebuild)

The grid and a dark olive cast covered the **whole screen**, not just the game.
Cause: a substrate cell is the one element here that *adds* light. Every other
component multiplies, and multiplying the black pillarbox leaves it black - but
`blend=3` paints three quarters of the substrate over whatever is under it, so the
border came out as a dark olive lattice. At 7x a 160x144 core fills 1120x1008 of a
1920x1080 frame, so most of the screen is border; with Super Game Boy on it was
worse still.

`shadowmask.sv` already carries `brd_in` (ascal's `o_brd`, "border enable", driven
from `not o_pev` - high *in* the border). `rtl/shadowmask-substrate.patch` now
delays it four stages, matching the depth `vid` carries hs/vs/de before the output
assignment reads its oldest slot, and gates the substrate mux on it:

    dout <= (sub4 && !brd_d[3]) ? ...blend8... : ...product...

**Proven in simulation, not just reasoned about.** `rtl/run_rtl_test.sh` patches a
pristine `shadowmask.sv`, runs `rtl/shadowmask_border_tb.sv` against it, then runs the
same testbench against a copy with the gate removed - and requires the second to
FAIL. Black in everywhere, sampling away from the region edges so the five-stage
pipeline cannot misattribute a pixel:

| | border pixels lit | picture pixels lit |
|---|---|---|
| gate removed (control) | **570 / 912** | 266 / 456 |
| gate in place | **0 / 912** | 266 / 456 |

The picture column is identical, so the gate costs the feature nothing. Worth knowing
if this test is ever edited: it had two vacuous passes before it worked, both reading
as `X` on `dout` and both counted as "lit" by a naive not-black check - the LUT is
strided by 16 (`mask_idx = {vindex, hindex}`, so a 2x2 mask lives at 0,1,16,17, not
0..3), and `vcount` is only cleared on a vsync FALLING edge while `next_v` is only
written on a border RISING edge, so a testbench that drives neither poisons the row
index for the whole run.

Built and fitted 2026-08-23 from Gameboy_MiSTer `7a5ff50`: 0 errors, timing clean,
**52% of the ALMs** (21,806 / 41,910), 27 minutes. The gate is four registers and a
mux, so it is free in practice. Artifact and log kept at
`snac-build/cores/Gameboy_subst_bordergate_20260823.{rbf,log}`.

Only the substrate is gated. The multipliers are left applying frame-wide, as
upstream has them - they cannot brighten the border, so there is nothing to fix and
no reason to diverge. Checked the other layers for the same class of leak and there
is none: the pixel shadow and the gamma both run before or inside the scaler, which
only touches the picture, and every gap multiplier - 1.15x as much as 0.80x - leaves
a black border black. The substrate was the only additive element in the chain.

The **preview renderer does not leak either**, for a duller reason: `vp_mask` carries
`mul[][][3]` and nothing else, so `vp_synth_gap_mask()` can only express the
multiplier approximation, and `vp_render_exact_rect()` maps its whole destination
onto the source and never draws a border at all. Which is one more concrete instance
of the preview limitation below - a DMG preview cannot show the substrate gap that is
the whole point of the look, only the pre-substrate version of it. **This is RTL: it needs the cores rebuilt before it takes
effect.** Until then the mitigation is a lower `SUB_BLEND`, which trades the leak
for a weaker gap.

### Super Game Boy is its own look now, not a rider on the panel ones

It used to be `On` in `CO_GB_DMG_COLOUR`, on the strength of Dinofly playing Zelda
with the border. On the television that turned out to be the wrong call: the SGB
canvas is 256x224 with the Game Boy picture **inset**, so the panel a DMG look
claims to simulate ends up smaller than the screen the grid is drawn on. A panel
look and a second machine's canvas cannot both be the subject of one picture.

So `Super Game Boy=Off` on every DMG/Pocket look, asked for **by name** (an absent
option is the core's last value, not a known one), and two appended looks:

- **Super Game Boy** - a CRT treatment, not a panel one: sharp, scanlines, aperture
  grille, no gap mask and no pixel shadow. A grid pitched to a Game Boy pixel fits
  neither the inset picture nor the border. The core keeps its own SGB colours.
- **None with Super Game Boy** - None cannot simply gain a flag, because None
  deliberately carries no core half at all ("touch nothing"); this is None with
  exactly one thing asked for.

`GBC` still asks for SGB On, deliberately - that look exists for the border - and is
outside what was reported. It has the same border-leak exposure until the cores are
rebuilt.

### The core's own shadow is off wherever ours is on

`CO_NO_SHADOW` ("Screen Shadow=No") now rides every look that wears a pixel-shadow
filter. Five were missing it: `agb001`, `ags001`, `ws`, `wsc`, `ngpc`. Two shadows
disagree - the core's darkens a whole Game Boy pixel because it draws at 160x144,
ours fades across the cell per output sample. Only the Gameboy core's option name is
measured; a core publishing one under a different name needs adding explicitly.
`assert_look_shadow_depth()`'s neighbour test now fails the build if a look gains a
shadow filter without it.

### The Display previews cannot show a core-side look (design note, not built)

`vp_render_exact_rect()` loads the scaler filters and synthesises the gap mask. It
cannot show anything the **core** does - the palette, Super Game Boy, Screen Shadow,
Modify Colors, Flickerblend - because those happen before the picture ever reaches
us. So the previews are honest about our half and silent about the core's, which is
why the two new SGB looks preview identically to their non-SGB twins, and why a DMG
preview cannot show its own palette.

Dinofly's conclusion, which is the right one: stop generating them on the fly and
ship **captures**. What that needs:

1. a capture harness that, per look, boots a fixed core+ROM+savestate, applies the
   look, waits for the scaler, and grabs the dongle - the same loop as
   `tools/filmstrip.sh`, driven by the look list rather than by frames;
2. one reference scene per video class, chosen for coverage rather than looks: a
   deep dark field, a full-bright field, a hard edge, and a single-pixel feature;
3. the results in `classicui/lookshots/`, which `make_sdcard_root.sh` already ships,
   keyed by look id and integer scale;
4. and the renderer kept for the fallback case only - an unknown scale, or a look
   whose shot is missing.

The dongle's own limits apply and are the reason this is a design note rather than a
patch: it rewrites single-pixel structure, so a 1-px gap at 7x cannot be
photographed faithfully (see the capture-dongle memory note). A preview shot has to
be taken at a scale where the gap is >= 2 px and then labelled as indicative, or
taken from the model for that one component. Deciding which is the open question.

## The menu background, and a wrong-pitch grid (2026-08-23)

Dinofly opened the menu over a running Game Boy and saw **vertical green segments,
not full screen height, slightly changing while the menu itself was still**, and read
it as our filters leaking into the menu.

They do reach the menu, but by design rather than by leak: `ig_open()` grabs the
core's pre-scaler frame, calls `vp_grid_for_now(1)`, and runs the look over the still
with `vp_render_exact()` so the menu sits over what the television is actually
showing. Three things it is therefore NOT, each ruled out rather than assumed:

- **not the fabric mask** - `shadowmask` is instantiated once, with
  `.enable(~LFB_EN)`, and `video_fb_enable()` sets bit 15 (`FB_EN 0x8000` ->
  `LFB_EN`) whenever the front-end takes the screen. The mask is hard off while the
  menu is up.
- **not the scaler filters** - with `LFB_EN` set, ascal's filter select becomes
  `LFB_FLT`, a single bilinear bit, not our polyphase taps.
- **not core video showing through** - the framebuffer replaces the video output.
  That is exactly why the front-end has to draw the still itself; if the core were
  visible underneath there would be nothing for `ig_bg` to do.

### A real bug found while looking (fixed)

`vp_render_exact_rect()` picked its mask like this:

    int have_mask = d->mask ? vp_synth_gap_mask(d->mask, mscale, &mask) : 0;
    if (!have_mask) have_mask = vp_load_mask(d->mask, &mask);

`vp_synth_gap_mask()` returns 0 for two unrelated reasons: the name is not a gap mask
(where reading the file is right), or it IS one and the magnification is unknown or
below 2x. In the second case this loaded the card's gap file - written for whatever
scale the SCALER is running - and applied it at the render's own `dw/sw`. Cells at the
wrong pitch do not draw a coarse grid, they draw a **moire**: irregular vertical
banding that shifts whenever the canvas resizes. `vp_is_gap_mask()` now separates the
two cases and a gap mask that cannot be built is simply not drawn.

Confirmed real rather than theoretical, with a control:
`assert_ig_still_has_no_wrong_pitch_grid()` renders a **flat** still - flat stays flat
through any linear filter, so structure can only be the mask - and checks both
directions. At 1x it must stay flat; at 7x it must NOT, or the probe cannot see a grid
and proves nothing. Reverting the fix makes the 1x case fail, which is the evidence
that the old fallback really did invent a grid. (The probe also has to sample
**mid-cell**: a gap row is one where every x is a gap cell, so it reads as uniform,
and picking `dh/2` at 7x lands on row 504 - exactly such a row - which failed the
control for the wrong reason.)

### Honest limit

This is probably **not** what Dinofly saw. It only fires below 2x, and a 160x144 core
in a 1080p canvas renders at 7x-12x. It is a real defect in the same code path and
worth having fixed, but the reported artefact is still unexplained.

What would settle it, and needs the device: open the in-game menu with the **None**
look, which has no mask and no filters. If the segments persist, our look is not
involved at all and the artefact is in the framebuffer we draw or in the compositing
after it. Alongside that, an `fbgrab` while the artefact is visible says whether it is
in what we drew - if the grab is clean and the screen is not, the artefact is added
downstream of us.
