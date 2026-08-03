# Driving core settings from Classic Home

An analysis, not an implementation. Nothing in here is built yet.

Data gathered 2026-08-03 by loading each core on the device and keeping the CONF_STR the
firmware read from it, so every option below is what that core actually published on
Derek's card — not what its source suggests it might.

---

## 1. Can we drive core settings at all?

**Yes, completely, with no new firmware plumbing.** Everything needed already exists and
is already used elsewhere in this tree.

| Need | Existing call |
|---|---|
| List a core's options | `user_io_get_confstr(i)` — the raw CONF_STR entries, the same ones the classic OSD parses |
| Read the current value | `user_io_status_get("[48:45]")` |
| Change one | `user_io_status_set("[48:45]", n)` |
| Persist | `user_io_status_save(user_io_create_config_name())` → `<CORE>.CFG` |

`snacpad.cpp` already does the read-and-set half of this for the PSX pad options, finding
the option and the value **by name** rather than by index. That is the pattern to build on.

Persistence writes the same `<CORE>.CFG` the OSD writes, so our changes and the OSD's are
the same thing — no divergence to reconcile.

### The grammar we would have to honour

This is where the work is, and where a naive implementation goes wrong.

```
P1,Audio & Video;                    a page (submenu) definition
P1O[3:1],Scandoubler Fx,None,HQ2x    an option, on page 1, bits 3..1
O[54:53],Widescreen Hack,Off,3:2     an option with no page
D8O[48:45],Pad1,Dualshock,Off        shown only when mask bit 8 is set
h0O[66],SNAC MemCard,Virtual,Real    hidden unless mask bit 0 (lowercase inverts)
T[0],Reset                           a momentary trigger, not a setting
S1,BIN,Load ROM                      a file selector
P2-,(U) = unsafe -> can crash        a plain label
```

Four things must be handled or the screen will be wrong:

1. **Hide/disable masks** (`H`/`h`/`D`/`d` + bit). Not cosmetic. Without them:
   - Game Boy publishes **two** different options both called `Mapper` — one for
     cartridges (`D7`), one for MD carts (`d7`). Both would appear.
   - NES reuses bit `L` for `Famicom Keyboard` **and** `Zapper Trigger`, distinguished
     only by mask.
   - SMS hides an entire group of nine options behind `H8`.
2. **Pages.** Cores already group their own options — "Audio & Video", "Input Options",
   "Hardware", "Miscellaneous", "System settings", "Debug settings". We should reuse that
   grouping rather than invent one; the core author knows which is which.
3. **Two bit-spec forms.** Modern `[63:62]` and legacy two-character (`EF`, `13`, `78`).
   `user_io_status_bits()` already parses both, so pass the spec through rather than
   parsing it ourselves.
4. **`(U)` means unsafe.** PSX publishes `P2-,(U) = unsafe -> can crash` and marks values
   with it — `Turbo`, `CD Fast Seek`, `PAL 60Hz Hack`, `GPU Slowdown`, `RAM(Homebrew)` and
   more. Its status strings even include "Unsafe option used!". **The core tells us which
   of its own options are dangerous**, so part of the curation can be automatic rather than
   a list we maintain by hand.

Some options also need a core reset to take effect; where that is true the core usually
says so in the name (`Homebrew BIOS(Reset!)`).

---

## 2. What the seven cores actually offer

Counting only real settings (`O`-type), excluding triggers, file selectors, labels and
joystick names:

| Core | Settings | Own pages |
|---|---|---|
| NES | 37 | Audio & Video, Input Options, Miscellaneous, Advanced |
| SNES | 32 | Audio & Video, Input Options, Hardware |
| Game Boy | 32 | Audio & Video, Misc. |
| GBA | 23 | Video & Audio, Hardware, Miscellaneous |
| Mega Drive | 30 | Audio & Video, Input |
| SMS | 35 | Audio & Video, Input |
| N64 | 58 | Video & Audio, System settings, **Debug settings (23)** |
| PSX | 52 | Video & Audio, Miscellaneous |

> **These counts are the corrected ones.** My first pass reported every core 3–13 options
> short, because prefixes arrive in either order — `P1O[3:1]` *and* `D1P1O[104]` — and I
> stripped the page before the mask, so every option with a mask in front of its page was
> silently dropped. That is the same class of mistake the parser will make in the real
> implementation if it assumes an order, and it hid the single most relevant group in this
> whole document (see N64's VI filters below). Strip both, in a loop, in any order.

N64 and PSX are the outliers, and almost entirely because of debug and unsafe options.
N64's Debug page alone is 23 entries of cache timing, DDR3 delays and bit-9 toggles.

### The things Derek asked for, and whether they exist

| Wanted | Reality |
|---|---|
| **NES palette** | ✅ NES `Palette` — Kitrinx / Smooth / Wavebeam / Sony … |
| **Game Boy SGB + palette** | ✅ `Super Game Boy` (Off/Palette/On), `Custom Palette` (Off/Auto/On), `Super Game Boy + GBC`, plus `Inverted color`, `Screen Shadow` |
| **Widescreen hacks** | ✅ PSX `Widescreen Hack` — Off / 3:2 / 5:3 / 16:9. **PSX only** of these seven |
| **Dithering** | ✅ PSX `Dithering`, `Dither 24 Bit for VGA`; N64 `Dithering` |
| **Overclocking** | ⚠️ No general one. Per-core and mostly marked unsafe: GBA `Underclock CPU` (0–3), SMS `Z80 Speed` (Normal/Turbo), PSX `Turbo`/`GPU Slowdown`/`CD Speed` (all `(U)`), N64 `Fast RAM access`/`Fast ROM access` (Debug page) |
| **Anti-aliasing / N64 blur** | ✅ **Better than I first said** — the N64 core exposes its whole VI filter chain. See below |
| **Post-processing** | ✅ `Scandoubler Fx` (None/HQ2x/CRT 25%/50%/75%) on **every** core — but see the conflict below |

---

## 2b. The N64's blurring — the answer, and it is a good one

The N64 core exposes **its entire VI filter chain**, one option per stage. This is the
group my first parse dropped, and it is exactly what was being asked for:

| Option | Values | What it is |
|---|---|---|
| `VI Deblur` | Original / **On** | Undoes the VI's horizontal blur — the "N64 looks soft" effect |
| `VI Antialias` | Original / **Off** | The VI's edge anti-aliasing |
| `VI Bilinear` | Original / **Off** | Bilinear resampling in the VI |
| `VI Divot` | Original / **Off** | The divot filter, which smears across pixel gaps |
| `VI Noisedither` | Original / **Off** | The VI's noise dither |
| `VI Dedither` | Original / Off / Force | Removes the RDP's dither pattern |
| `VI Gamma` | Original / Off | The VI's gamma boost |
| `VI Colorbits` | Original(21) / 24 | Output colour depth |

All eight are gated on mask bit 1, which tracks `Video Out` — they apply to
`Original(VI)` and are irrelevant under `Clean HDMI`, which bypasses the VI entirely.
So `Video Out` belongs directly above them, and the group should grey out when it is set
to Clean HDMI. The core has already expressed that relationship; we just have to honour
the mask.

This is a strong Tier 1 group: every one is a single visible toggle, none is unsafe, and
"why does N64 look blurry" is a question people actually have. `Texture Filter`,
`Dithering` and `LOD Textures` are separate RDP-side controls and sit alongside them.

---

## 2c. GB and GBA: do the cores duplicate our Display looks?

Partly — on exactly one axis, and that axis is the one where **the core is better**.

Our looks for these systems do three things at once:

| Our look | gamma | mask | filter |
|---|---|---|---|
| Game Boy DMG | `G_DMG` | matrix 2x | sharp |
| Game Boy Pocket | `G_POCKET` | matrix 1x | sharp |
| Game Boy Color | `G_GBC` | matrix 1x | sharp |
| GBA (AGB-001) | `G_AGB001` | matrix 1x | sharp |
| GBA SP (AGS-001) | `G_AGS001` | matrix 1x | sharp |
| GBA SP (AGS-101) | `G_AGS101` | off | sharp |

What the cores offer against that:

| Axis | Core | Ours | Better |
|---|---|---|---|
| Colour correction | GBA `Modify Colors` (Off/GBA 2.2/GBA 1.6/NDS 1.6/VBA…), GB `GBC Colors` (Corrected/Raw) | gamma LUT in the scaler | **Core.** Applied in its own pixel pipeline before scaling, and authored per hardware model |
| Palette replacement | GB `Custom Palette` (Off/Auto/On), `Super Game Boy` | — | **Core only.** A LUT cannot remap four grey levels to arbitrary colours; a palette file can |
| LCD pixel grid | — | shadow mask, 1x/2x | **Ours only.** The cores have no mask; that is a scaler feature |
| Frame blending | GB `Frame blend`, GBA `Flickerblend` (Off/Blend/30Hz) | — | **Core only.** Temporal blending needs consecutive frames, which the scaler does not have |
| Inverted colour, screen shadow, sprite limits | yes | — | **Core only** |

So they are **complementary, not redundant — except on colour, where they collide.** Turn
on our `GBA (AGB-001)` look *and* the core's `Modify Colors: GBA 2.2` and the picture is
corrected twice. Nothing warns about it; it just looks wrong, and a player would have no
way to know which of two screens caused it.

**Recommendation: let our Display look drive the core's option instead of our LUT, when the
core has one.** Choosing "GBA (AGB-001)" would set `Modify Colors` in the core and leave
our gamma off, while still applying our mask and filter — which the core cannot do. One
control for the player, the better mechanism for each half, and no double correction.
Where the core offers nothing (`AGS-101` has no core equivalent), our LUT stays as it is.

That keeps the answer to "which is better" as "each, at what it does", rather than making
the player arbitrate between two screens.

---

## 3. What I would expose

Three tiers. The point of the tiers is that a player should meet the interesting knobs
without ever meeting a cache delay.

### Tier 1 — the default page: *picture*

Visible effect, safe, and the reason someone opens this screen at all.

- **NES** — Palette, Mask Edges, Overscan, Extra Sprites
- **SNES** — Pseudo Transparency (Blend/Off), Force 256px
- **Game Boy** — Super Game Boy, Custom Palette, GBC Colors, Inverted color, Screen Shadow,
  Frame blend, Super Game Boy + GBC, Extra sprites
- **GBA** — Modify Colors, Flickerblend, 2XResolution, Spritelimit
- **Mega Drive** — 320x224 Aspect, Border, Composite Blend, CRAM Dots
- **SMS** — Masked Left Column, Sprites Per Line, Border, Game Gear Res., Orientation,
  Flip Screen
- **N64** — Video Out, then the VI group (`VI Deblur`, `VI Antialias`, `VI Bilinear`,
  `VI Divot`, `VI Noisedither`), plus Texture Filter, Dithering, LOD Textures
- **PSX** — Widescreen Hack, Dithering, Texture Filter, Horizontal Crop, Deinterlacing, Rotate

### Tier 2 — a second page: *sound, region, hardware*

Worth reaching, not worth meeting first.

- Region and timing: NES `System Type`, SNES `Video Region`, GB `System`, MD
  `Region`/`Auto Region`, SMS `Region`/`TV System`, N64 `System Type`, PSX `System Type`
- Sound: `Stereo Mix` (all), NES `Audio Enable`, GB `Audio mode`, MD `Audio Filter` /
  `FM Chip` / `SMS FM Chip`, SMS `SMS FM Sound`, SNES `Audio Clock`
- Hardware character: SNES `Initial WRAM`/`Initial ARAM`, NES `RAM Clear` /
  `PPU Reset Behavior`, MD `TMSS`, SMS `Mapper` / `SMS BIOS`

Also newly found and worth having: **`Vertical Crop` (Disabled / 216p 5x) and
`Crop Offset`** on NES, SNES, Mega Drive, SMS and PSX. That is the integer-scaling crop
people set up by hand for a CRT, and it is one option away.

### Tier 3 — behind one deliberate step, labelled as risky

Confirmed wanted, so it goes in — but as its own page, with the core's own warning shown
rather than a wording of ours:
GBA `Underclock CPU`, SMS `Z80 Speed`, PSX `Turbo` / `CD Speed` / `CD Fast Seek` /
`GPU Slowdown` / `PAL 60Hz Hack` / `RAM(Homebrew)`, N64 `Fast RAM/ROM access`.

Any value ending `(U)` gets the warning automatically — no list to maintain.

---

## 4. What I would *not* expose, and why

This half matters as much as the other.

**Because we already own it.** Showing a second control for the same thing is worse than
showing none. `Savestate Slot`, `Savestates to SDCard`, `Autosave`, `Save to SDCard`,
`Autoincrement Slot` — our suspend points own all of that. `SNAC` / `USERIO` / `Pad1` /
`Pad2` / `Pad N Type` — the Controllers screen and `snac_psx` own those.

**`Pause when OSD is open` — actively dangerous to expose.** Every core has it, and it is
the mechanism our freeze depends on. A player turning it off would silently change what
happens when they open the menu. We should set it, not offer it.

**`Aspect ratio`, `Scale`, `Scandoubler Fx` — a genuine conflict to resolve first.** These
are core-side video controls, and Classic Home already has **Display looks**, which set the
scaler's filters, mask and gamma through presets. Exposing both gives the player two
competing controls over one picture, and the interaction is not obvious. My recommendation
is to leave these out of the core screen and, if we want them, fold them into Display —
but that is a design decision to make deliberately rather than by adding a row.

**Because it is not a setting a person wants.** N64's entire Debug page (23 entries:
`Cache Delay`, `DDR3 Delay`, `Write Bit 9`, `RDRAM Calib Waittime`, …), N64 `CIC` and
`RAM size`, PSX `Old GPU(CXD8514Q)`, the `FPS Overlay` / `Error Overlay` / `Debug Dots`
family. These are why the classic OSD should stay reachable — which it now is.

---

## 5. The contextual menu-bar entry

**Nothing to invent: the abbreviations already exist.** `chome_lib.cpp`'s system table
carries a short name in its third field, already sized for tight spaces:

| System | Label |
|---|---|
| Nintendo Entertainment System | `NES` |
| Super Nintendo | `SNES` |
| Game Boy | `GB` |
| Game Boy Advance | `GBA` |
| Nintendo 64 | `N64` |
| Mega Drive | `MD` |
| Master System | `SMS` |
| TurboGrafx-16 | `TG16` |
| PlayStation | `PSX` |
| Neo Geo | `NEO` |
| Arcade | `ARC` |

Behaviour:

- The entry exists **only while a core is loaded** — the same condition `Core Settings`
  uses. On the shelf with no core, the bar is unchanged.
- Label is the running system's short name. When the running core is not one of ours
  (an arcade `.mra`, something loaded behind our back), fall back to
  `user_io_get_core_name()`.
- It should also disappear when the core publishes nothing worth showing, rather than
  opening an empty page. Which is knowable: parse first, add the entry only if the
  curated list is non-empty.

`mb_visible()` already does exactly this kind of conditional hiding for `Display` at 240p,
so the mechanism is in place.

One thing to check when building it: the bar currently has four entries and is laid out for
that. At 240p `Display` is already dropped for space, so a fifth entry needs the layout
looked at rather than assumed.

---

## 5b. It is the running core's list, and only that

Worth stating plainly because the harness screenshot invites the opposite reading.

`core_opts_scan()` walks `user_io_get_confstr()`, which returns **the loaded core's**
strings and nothing else. There is no combined table and no per-core list in the firmware;
if a core is not running there is nothing to read, which is why the bar entry does not
exist on the shelf.

Measured on the device, same firmware, one after the other:

| Core | Offered | Picture | System | Mask |
|---|---|---|---|---|
| SNES | 23 | 4 | 19 | `0056` |
| Game Boy | 23 | 8 | 15 | `016a` |

Game Boy's picture page: Super Game Boy, Extra Sprites, Inverted Color, Screen Shadow,
Custom Palette, Frame Blend, GBC Colors, Super Game Boy + GBC. SNES's: Vertical Crop,
Crop Offset, Force 256px, Pseudo Transparency. No overlap, because they are different
cores' own options.

The harness picture is a **synthetic fixture**: one fake core carrying options borrowed
from several real ones, so a single test covers every awkward case in the grammar. It shows
`Palette` beside `VI Deblur`, which no real core does. The dump is named
`core-options-synthetic-fixture` for that reason, and the per-core screenshots under
`docs/img/device/` are the honest examples.

---

## 6. Suggested order of work

1. A CONF_STR parser that yields (page, name, values, bit spec, mask condition) and
   evaluates the hide/disable masks. This is the whole risk; everything else is drawing.
2. The contextual bar entry, gated on the parse finding something.
3. Tier 1 as a single page, reading and writing live, persisting to `<CORE>.CFG`.
4. Tiers 2 and 3, the second behind a deliberate step with the `(U)` warning.
5. Decide the Display-versus-core-video overlap before shipping any of `Aspect ratio`,
   `Scale` or `Scandoubler Fx`.

The harness can cover all of it: a fake CONF_STR is already how `stubs.cpp` models a core,
and there are five of them in there now, so adding one per core shape under test is the
existing pattern.
