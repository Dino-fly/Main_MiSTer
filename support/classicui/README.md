# Classic Home

A game-oriented, SNES-Classic-style front-end for the MiSTer **menu core**.
Design rationale: `docs/CLASSIC_UI_PLAN.md`.

Enable with `classicui=1` in `MiSTer.ini`. Default is off.

**None of this has run on hardware yet.** It cross-compiles clean with `-Wall
-Wextra` and the logic has been reviewed, but every path below is untested on a
real DE10-Nano. Treat the first boot as a bring-up exercise, starting with the
console log: every module prints what it decided.

## Shape

| File | Role |
|---|---|
| `chome_gfx.cpp` | Compose buffer in cached RAM, dirty-rect blit to the menu framebuffer, page flip, primitives, text in the 8x8 OSD ROM font |
| `chome_theme.cpp` | Layout profiles (hd/sd/lo) derived proportionally from the canvas, palette |
| `chome_lib.cpp` | Systems table, background scan, game index, shelf views, favourites/play counts, suspend-slot state |
| `chome_art.cpp` | Cover art: local lookup, lazy decode, LRU cache, optional online fetch |
| `chome_video.cpp` | Video looks: preset/filter/mask/gamma generation, per-system defaults, previews |
| `chome_ui.cpp` | Screens, navigation, launch |
| `test/` | Host harness: compiles the modules unmodified against fakes, asserts behaviour, renders every screen to PNG |

Integration points outside this directory, all additive:

- `video.cpp` / `video.h`: `video_menu_fb()`, `video_menu_fb_width/height()`,
  `video_menu_fb_present()` expose the menu background double-buffer.
- `menu.cpp`: one call in `HandleUI()`, `chome_active()` in `menu_key_get()`'s
  repeat condition and in the idle-dim guard, one include.
- `cfg.cpp` / `cfg.h` / `MiSTer.ini`: five `CLASSICUI*` options.

## How it draws

The framebuffer is an uncached `/dev/mem` mapping shared with the FPGA scaler, so
per-pixel writes into it are slow. Everything composes into a malloc'd buffer and
only the damaged rows are `memcpy`'d across. Because the two framebuffers
alternate, each blit covers this frame's damage **plus** the previous frame's -
the buffer being filled is two frames stale. `gfx_end()` is where that happens.

Frames are only drawn when something changed (`mark_dirty()`); an idle menu costs
almost nothing.

## How it hides the cores

`chome_lib.cpp` holds the only mapping from a game to a core. Launching writes a
one-shot MGL to `/tmp/classicui_launch.mgl` and hands it to `xml_load()`, the same
entry point `/dev/MiSTer_cmd load_core` uses, so the launch path is the existing
tested one. Arcade `.mra` files are passed to `xml_load()` directly.

**The riskiest data in the whole feature** is the MGL `type`/`index` per core, which
comes from each core's `CONF_STR`. A wrong value hands the ROM to the wrong slot
and the game will not boot. The built-in table uses the common values but they are
not verified per core. Override without rebuilding by copying
`docs/classicui_systems.example.txt` to `classicui_systems.txt` on the SD root.

## The Display screen

Laid out like the SNES Classic's: a row of preview tiles with a radio under each.
Left/Right chooses, A applies. Panel height is computed from its content so the
bands cannot collide at any profile.

The original also has a **Frame** carousel here. There is no equivalent, and no
placeholder for one: MiSTer cannot composite anything over a running core, because
the HPS framebuffer *replaces* core video rather than blending with it (the `FB_*`
format bits in `video.cpp` have no alpha or overlay mode). A bezel that cannot
frame the game is not a feature, so nothing about frames or bezels exists in this
code. It would need an overlay layer in the framework scaler - FPGA work, see
`docs/CLASSIC_UI_PLAN.md` section 7.2 - and would be designed then, not now.

**The entry disappears entirely at 240p**, where the tiles would come out around
66px wide - too small to judge a filter by, and a canvas that small means an analog
CRT in practice anyway. The looks still apply from their class defaults; force
`classicui_profile=1` if you want the screen back on a small canvas.

**And it disappears entirely on analog output.** Everything in it lives in the
scaler - filters, shadow mask, gamma - so `video_scaler_is_visible()` drops the
whole menu-bar entry when the scaler's output is not what reaches the screen:
`direct_video` (raw core timing straight out the DAC) or an analog-only setup
without `vga_scaler`. HDMI presence comes from the ADV7513's HPD and monitor-sense
bits (`video_hdmi_connected()`), and when that cannot be read we assume the usual
HDMI setup. Showing a CRT-filter picker to somebody already looking at a real CRT
would be daft. The menu bar re-spaces itself over the remaining entries, and
**Menu Layout moved to Options** so it stays reachable.

## Video looks

Eighteen presets exist, but **the list is never shown in full**: the Display screen
offers only the screens the selected game could plausibly have been played on, with
a "for &lt;hardware&gt;" heading so the short list explains itself. A Mega Drive game
is never offered an LCD; a Game Boy game is never offered a PVM.

Each look is written out as a real MiSTer preset file and applied with
`video_loadPreset(path, save=true)`, which persists per core - this drives the
existing scaler rather than reimplementing anything.

`vp_install()` **generates** everything a preset references, so the looks work on
a stock install with no community filter packs:

| Generated | What it is |
|---|---|
| `filters/ClassicHome Sharp/Soft/Blurry.txt` | 4-tap 32-phase polyphase coefficients: nearest, linear, wide tent |
| `filters/ClassicHome Scanlines*.txt` | Same, with a per-phase gain envelope. Lines summing under 128 come out darker, which is how MiSTer scanline filters work |
| `shadow_masks/ClassicHome Grille.txt` | 3x1 R/G/B aperture grille |
| `shadow_masks/ClassicHome Dot Matrix.txt` | 4x4 LCD cell grid with a black gutter |
| `gamma/ClassicHome DMG / GB Pocket / GBC / GBA AGB-001 / GBA AGS-001 / GBA AGS-101.txt` | Full 256-entry per-channel LUTs. This is what makes the handheld screens possible at all |
| `presets/ClassicHome *.ini` | One per look |

Existing files are never overwritten, so hand-tuning a generated file sticks;
delete it and use Options > Reinstall Looks to get the default back.

### What each class is offered

Most authentic first, and the first entry is also the default.

| Class | Offered | Reasoning |
|---|---|---|
| Consoles | PVM RGB, PVM S-Video, Composite TV, Sharp | A console reached a TV by RGB, S-Video or composite depending on your luck |
| Arcade | PVM RGB, PVM S-Video, Sharp | Arcade monitors were direct RGB; composite never entered the picture |
| Home computers (15 kHz: C64, Spectrum, Amiga, CPC, MSX) | PAL TV, Composite TV, PVM S-Video, Sharp | These lived on televisions, not monitors |
| VGA (31 kHz: ao486, x86, Archie) | VGA Monitor, Sharp | A VGA CRT at 480p had **no** visible scanline gaps, so no CRT looks belong here at all |
| Game Boy | Game Boy DMG, Game Boy Pocket | Both grey reflective panels: the original olive-green, then the Pocket's neutral grey with better contrast and a finer grid |
| Game Boy Color | Game Boy Color, GBA (AGB-001), GBA SP (AGS-001), GBA SP (AGS-101) | A GBC cartridge also runs on either Game Boy Advance, so all four screens apply |
| Game Boy Advance | GBA (AGB-001), GBA SP (AGS-001), GBA SP (AGS-101) | The three screen revisions and nothing else |
| Game Gear | Game Gear, Game Gear (Backlit Mod) | Backlit but murky; the LED backlight mod is common enough to deserve its own entry |
| Atari Lynx | Atari Lynx | Backlit colour with a cool cast and washed blacks |
| WonderSwan | WonderSwan | Reflective mono FSTN, warm grey, narrow range |
| WonderSwan Color | WonderSwan Color, WonderSwan | A colour Swan also plays mono titles |
| Neo Geo Pocket Color | Neo Geo Pocket Color | Reflective pastel, gentle contrast |

Shared cores are split by extension in `class_of()`: `.gbc` in the Game Boy core is
a GBC game, `.gg` in the SMS core is a Game Gear game, `.wsc` in the WonderSwan
core is a Colour game. Lynx, WonderSwan and Neo Geo Pocket are new entries in the
systems table - like every other entry, their rbf path and MGL index need verifying
on hardware.

The three GBA revisions differ in black level and contrast, which is exactly what
distinguished them in the hand (measured from the generated LUTs, black -> white):

| Revision | Range | Character |
|---|---|---|
| AGB-001 | 62 -> 184 | Unlit. Lifted blacks, crushed whites, washed out |
| AGS-001 | 45 -> 206 | Frontlit. Brighter, warmer, still washed |
| AGS-101 | 15 -> 248 | Backlit. Proper black level and full contrast |

Handhelds deliberately get no "Sharp" option: an unfiltered Game Boy is not a look
anyone is after, and the LCD is the point.

A `.gbc` cartridge in the Game Boy core resolves to the **Game Boy Color** class,
decided from the extension by `class_of()` in `chome_ui.cpp` - the panel and the
launch path both go through it so they cannot disagree. User choices are stored in
`classicui_video.cfg` keyed by system **and** class, so a DMG cart and a GBC cart
sharing one core remember different screens. A stored choice that is no longer
offered for its class is ignored rather than applied.

One limitation worth knowing: MiSTer's gamma format is three independent
per-channel LUTs, so brightness, contrast and tint are all reachable, but a true
colour-space correction matrix - the usual "GBA colour correction" approach, which
mixes channels - is not expressible.

Applying happens across the core switch: `vp_arm_for_launch()` writes the chosen
preset path to `/tmp/classicui_preset`, and `chome_core_boot()` - called from
`HandleUI()` in *every* core - applies it once the new core has booted.

### Previews

Each tile shows **a real frame of the user's own game** with the look applied over
it, so the tiles are a genuine side-by-side comparison of the same moment through
different screens. Source, in order of preference:

1. `classicui/refshots/<system>/<rom>.png` - captured by `chome_core_poll()` about
   20 seconds into a session, once per game, only when missing (SD wear). The delay
   is a heuristic: past the boot logos, into something representative.
2. an existing suspend-point thumbnail, which is also a real capture
3. failing both, a synthetic test pattern (colour bars, grey ramp, blocky sprite)

Decoding reuses `art_thumb()`'s cache, so no new decode path. `/tmp/classicui_current`
carries the system id and ROM path across the core switch so the game core knows
what it is running and where to put the shot.

**The frame is real; the filter is not.** The look is applied in software by
`vp_preview()` - LUT, horizontal bleed, scanline envelope, mask - and that is an
approximation of what the FPGA scaler does, not a readback of it. See below for why
a true readback is impossible.

The LCD cell grid uses a small fixed period in preview pixels (3, with a gentle
quarter-shade gutter) rather than one derived from the source scale. Deriving it
made the cells up to 36px across, which read as giant blocks instead of a panel -
a GBA pixel is only about one preview pixel at tile size, so a little exaggeration
is needed to be visible at all, but only a little.

**Possible future direction:** ship real captures per look, taken from hardware
once and stored as reference PNGs, instead of simulating the effect. That would be
exact where this is only indicative, at the cost of showing a fixed scene rather
than the user's own game. The two could coexist - a real capture for "what this
filter does", the user's frame for "what my game looks like".

### Why previews cannot be captured through the real scaler

The obvious idea is to let the hardware render each look and screenshot the result.
It cannot work, for three independent reasons:

1. **The capture buffer is pre-scaler.** `MISTER_SCALER_BASEADDR` is `0x20000000`,
   which `video.cpp` labels "Core's fb". `mister_scaler_read()` reads
   `width x height` at the *core's* resolution with its own stride, while
   `output_width`/`output_height` sit alongside as metadata that `do_screenshot()`
   optionally rescales *to* - pointless if the buffer were already the output.
   Filters, shadow mask and gamma are applied downstream and nothing reads back
   from there, which is also why MiSTer screenshots come out pixel-exact and
   unfiltered. Every look would capture identically.
2. **No game is loaded when the menu is open.** Classic Home runs only in the menu
   core, so there is nothing to capture at the moment the user asks.
3. **No synchronous savestate API.** Savestates exist in a subset of cores and the
   save is polled once a second (`process_ss`), so a save/switch/capture/restore
   loop cannot be driven from here.

Capturing one real frame while the game runs and applying the looks in software
sidesteps all three, works on cores without savestates, and shows every look at
once instead of one at a time.

**Scanline depth and mask strength are the most subjective numbers in the whole
feature and the first thing to tune on hardware.** A full aperture grille masks two
of three channels per pixel; stacked with scanlines it can get dark. The shipped
depth is moderate (dimming to about 73% at the darkest point).

## The index cache

Every core switch re-execs the binary, so without a cache the first menu open
after each switch would walk the whole library again - which matters most exactly
where it is least welcome, opening the menu from inside a game.

So a completed scan writes `classicui/index.bin`: a header, the item array, and
the mtime of every directory the scan visited. `lib_init()` loads it and skips
scanning when it is still valid.

Validation is deliberately cheap. Adding or removing a file changes its parent
directory's mtime, so stat()ing the recorded directories catches library changes at
a fraction of the cost of re-reading them - one stat per directory, versus a
readdir of every entry. The cache is rejected when:

- a recorded directory is gone or its mtime moved (a game was added or removed)
- a system's folder exists now but was not walked then (a whole system appeared,
  which mtimes cannot catch on their own since the new folder has no record)
- the systems table signature changed (`classicui_systems.txt` was edited)
- the build changed: version, or `sizeof(chome_item)`, no longer match

Favourites and play counts are re-read from the state file rather than trusted from
the cache, and savestate slots are zeroed on load since they are re-read per
selection anyway.

`lib_rescan()` (Options > Rescan Library) deletes the cache and scans from scratch,
which is the escape hatch for the one case validation can miss.

## Cover art

Local lookup order, all under `classicui_artdir` (default `boxart`):

1. `<artdir>/<System Name>/Named_Boxarts/<ROM name>.png` - the libretro
   convention the community packs already use
2. `<artdir>/<games dir>/<ROM name>.png`
3. `<artdir>/<games dir>/<cleaned title>.png`
4. next to the ROM

`.jpg` is tried too, but the bundled Imlib2 links only libpng, so JPEG support
depends on the shipped `libImlib2.so` loaders - PNG is the safe format.

Lazy: the UI requests art for the visible cards plus a lookahead, prioritised by
distance from the selection, and at most **one image is decoded per frame**.
Imlib2 keeps its state in a global context, so decoding on a worker thread would
race with the wallpaper code in `video.cpp`; one card per frame fills a shelf as
fast as anyone scrolls. Decoded cards are cached at the selected-card size with a
24 MB LRU, aspect-fitted onto the system's plate colour rather than stretched.

Fetching (`classicui_artfetch=1`, **off by default**) forks `curl` for one image at
a time and polls it without blocking. It writes into layout 1 above, so a fetch
permanently populates the local pack and the next boot needs no network. It is
opt-in because it necessarily sends ROM names to a third party.

## Keys

Gamepad mapping follows `input.cpp`'s OSD translation, so pads work exactly as
they do in the classic menu.

| Input | Key | Action |
|---|---|---|
| D-pad | arrows | move / reveal menu bar (up) / suspend points (down) |
| A | Enter | start, open folder, confirm |
| B | Esc | back, leave folder |
| X | Tab | delete a suspend point (two presses) |
| Y | Backspace | toggle favourite |
| Select | ` | sort |
| L / R | - / = | jump one screenful |
| OSD / menu | F12 | hand off to the classic menu, and come back |

Key repeat for the shelf needs `chome_active()` in `menu_key_get()` - without it
`menu_key_get()` only repeats for ASCII keys and the file browser.

## Running it on a laptop

```
support/classicui/test/play.sh              # 1280x720
support/classicui/test/play.sh --sd         # 640x480
support/classicui/test/play.sh --lo         # 320x240
support/classicui/test/play.sh --no-fetch   # stay offline
```

Then open <http://localhost:8099> (`PORT=9000 play.sh` if that clashes). Arrow keys move, Enter is A, Esc is B, Tab is X,
Backspace is Y, backtick is Select, `-`/`=` are L/R, and **M is the OSD/menu
button**.

It runs the real front-end against a fake SD card built from
`test/placeholder_games.txt`: **528 placeholder games** across 19 system and
extension groups, in **No-Intro / Redump naming**. That naming is the point - it is
what the libretro thumbnail server keys covers on, so cover fetching is exercised
for real rather than simulated. A spot check of 18 random entries resolved 15
against the live server; the rest are name variants that fall back to the generated
card, which is useful too since it puts all three states on screen at once.

Only two covers are planted locally, so you can watch the rest arrive lazily as
cards come into view. Fetching is **on** here (unlike the product default) because
testing it is the point; `--no-fetch` turns it off, and the container gets `curl`
because that is what the fetcher forks.

Nothing is installed on the host: the container already has imlib2, and
`linux/input.h` is the only Linux-specific header the project pulls in, so building
natively would need a shim that this avoids.

**Launching is simulated rather than refused.** Pressing A on a game flips the
viewer into "game core" mode with a synthetic game picture derived from that game's
name, so pressing M then exercises the in-game menu, its live save states, Video
Look over the running frame, and Close Game - the whole loop, without a MiSTer.

How it works: the viewer runs the UI and serves the compose buffer over HTTP; the
page draws it into a canvas and posts keys back. Frames are sent only when the
generation counter moves, so an idle menu costs nothing.

What it cannot tell you, and this is the important part: redraw cost on uncached
memory, SPI behaviour, whether a core accepts an MGL, or what the video looks
actually do - those are the FPGA scaler's, and here they are only the software
approximation `vp_preview()` draws.

## Testing without hardware

`support/classicui/test/run.sh` builds a host binary in Docker from the real
`chome_*.cpp` files plus fakes for the framebuffer, SD card, clock, `xml_load()`
and `video_loadPreset()`. It creates a fake SD card, walks every screen at four
canvas sizes, writes a PNG of each to `test/out/`, and runs 61 assertions over the
index, sorting, views, savestate slots, art decode, the generated video files and
the exact MGL emitted at launch.

It found real bugs that compile cleanly: a tap-run being mistaken for a held key,
an enter-then-immediately-leave on the first menu press, and cards 30% too small at
240p. What it cannot tell you: redraw cost on uncached memory, SPI behaviour, or
whether a core accepts the MGL.

## Deliberately not implemented

- **Gameplay frames/bezels, in-game graphical overlay, UI sound, rewind.** All
  four need FPGA-side work or per-core emulation features that do not exist. A
  menu-only "frame" was built and then removed: decorating the menu with a bezel
  while the game itself cannot have one is a feature in name only.
- ~~Suspend-point thumbnails.~~ **Done.** `process_ss()` in the main binary is
  what notices a savestate (it polls a counter in shared memory and writes the
  `.ss` itself), so no core change was needed: it now also calls
  `screenshot_thumbnail()` from `scaler.cpp` to write
  `savestates/<core>/<rom>_<n>.png`, which the strip displays and falls back to
  the cover when absent. The poll can be up to a second behind the actual save,
  so the frame is close but not exact.
- **i18n.** The Language panel lists the EU unit's languages and marks the six
  non-English ones as untranslated. Strings are still inline English; a string
  table is the next step, not a rewrite.
- **Attract mode, first-run wizard, per-game core options.** Options hands off to
  the classic menu for anything it does not own.
- **Aspect and scaling** (4:3 vs pixel-perfect) are not in the Video Look panel:
  `video_loadPreset` has no key for them and `vscale_mode` does not survive the
  core switch. They stay an ini setting.
- **Game metadata.** `chome_item` has year/publisher/players fields but nothing
  fills them, so the shelf shows system and play count only.
- **Slot locking is ours, not MiSTer's.** Savestate files have no lock concept,
  so locks live in `classicui_state.cfg` and only stop *this* UI from deleting a
  slot.

## Known rough edges

- During the first scan the shelf re-sorts each time a system finishes, so the
  selection can jump for a second or two.
- The index caps at 6000 games and the browser at 512 entries per directory; both
  log when they truncate rather than silently hiding games.
- The index cache's validation cannot see a change deeper than the directories it
  recorded, if that set overflowed its 2048 cap. Options > Rescan Library forces a
  fresh scan, and says so when validation was incomplete.
