# Display looks: where the work stands, and what you need to know to carry on

Written 2026-10-28 so the filters work can continue from a clean context. It is the
todo list plus the knowledge that took the most time to learn. Read it top to bottom
once; after that the "Todo" section is the working list.

## The goal (Dinofly's brief)

3 to 5 presets ("looks") for each type of screen the original hardware drew on:

- every machine that went to a consumer CRT shares one set, with exceptions where
  needed;
- a look may combine scaler filters/masks/gamma with **core options** - this is what
  sets the Classic UI looks apart from stock MiSTer presets;
- each handheld gets its own set, with model variants (DMG/Pocket/Light/Color,
  AGB-001/AGS-001/AGS-101/Micro/DS, ...); Brick Boy (Time Extension, Aug 2026) is the
  ambition for Game Boy;
- computer CRT looks are era-accurate and grouped by **what each machine actually
  output to** (TV/RF, low-res RGB monitor, mono monitor, 31 kHz), not one class per
  family;
- mod screens are welcome (backlight kits, McWill, IPS);
- iterate in simulation, confirm on the device.

His standing calls, all still in force:

- The Display screen may use a **second row** of tiles (wraps past six).
- The shadowmask RTL patch, if it is ever needed, goes in the **shared sys/**.
- **Handhelds first**, then CRT (TV/arcade), then computers.
- **GBC never mixes the SGB border with a panel grid**: either SGB border + CRT
  treatment (or no filter) - the SGB is a SNES on a TV - or no border + the grid.
- Every class keeps an off switch (None, or Sharp for CRT classes).

## Where it stands

| family | status |
|---|---|
| Game Boy: DMG (Olive), DMG Bright, DMG Faded, Pocket, Light, SGB, None, None with SGB Border | approved, on device |
| GBC: GBC, GBC on GBA / GBA SP / SP Brighter (+ GBA mode), SGB, None, None with SGB Border | approved; GBA mode untested on device |
| GBA: GBA, GBA SP, SP Brighter, Game Boy Micro, GBA on DS, None (all Flickerblend=Blend) | approved; options confirmed on the card's core |
| Game Gear: stock, Backlit Mod, McWill | approved (sim) |
| Lynx: stock, McWill | approved (sim) |
| WonderSwan / Color: WS, WSC, SwanCrystal | approved (sim) |
| NGPC: NGPC, NGPC IPS Mod; NGP (mono class): Neo Geo Pocket, Mono on NGPC | NGPC approved live (#9); IPS captured live (#10); mono looks not yet captured |
| CRT: Sharp, PVM RGB, PVM S-Video, Composite, PAL TV, BVM RGB, VGA | phase 3, not started |
| computers | phase 4, not started |

Branches: `classic-ui` has everything through the GBC/GBA/mod-screen work (merge
5bd5299e); **classic-ui is not pushed yet** - a push needs Dinofly's
`approve-once push`. The NGP/GBA-mode work is on `classicui-handhelds2` (worktree
`../Main_MiSTer-looks`), not merged.

## Todo

1. **Push `classic-ui`** once Dinofly sets the marker (`approve-once push`); then
   merge `classicui-handhelds2` (after review #9 and the NGP captures) and push again.
2. **NGP family on device**: capture a mono cart (KOF R-1 is mono)
   under Neo Geo Pocket and Mono on NGPC. `System` is a reset-time strap: these need a
   relaunch, not a live apply. Show each as a review; the renderer cannot simulate the
   core's own Saturation/Panel/System, so use real HDMI captures for these.
3. **GBA mode on device**: launch a GBA-aware GBC game (Oracle of Seasons/Ages,
   Shantae) under GBC on GBA; confirm `GBC/GBA mode = GBA` in the log and a brighter
   palette than under the GBC look.
4. **Official NGPC core, savestates**: the row is `CH_SS_NO`, yet the front-end drove a
   suspend on menu open ("save state -> slot 4 via [42]") and the card then held only
   `..._1.ss`; restoring that "slot 4" froze the game on a dim frame. The core's own
   layout is `d7O[46:45],Savestate Slot` - check what the front-end's generic savestate
   path assumes against NGPC.sv, measure, and set the row's CH_SS_* from that.
   Re-homing points this core at savestates/NeoGeoPocket, which the old community core
   wrote too: states from that core must not be offered to this one.
4b. **Official NGPC core, live switch**: NGPC -> NGPC IPS Mod applied live once left the
   picture white until relaunch (LCD Response Panel -> Raw plus Saturation 75 -> 100 at
   once). Seen once, not yet reproduced; the same look from a launch is fine.
5. **Harness gap**: the clip log only sees Display screens the harness draws (GB,
   GBC, GBA, console). Render every handheld class's Display screen there so a tile
   name that wraps past two lines fails the cut-text check ("Neo Geo Pocket Color"
   lost "Color" on the device and nothing caught it).
6. **Phase 1 leftovers before CRT**: the adaptive scaler filters in the model (read
   ascal.vhd's adaptive path), the `sfilter` rule, and an explicit `look_is_panel`
   field before CRT looks carry core options.
7. **Phase 3, TV/arcade looks**: start with what the consumer-CRT set should be and
   which exceptions exist (arcade monitors, PAL machines). The dongle is unreliable for
   1 px structure (see below); validate scanline depth against the simulator.
8. **Phase 4, computers** grouped by era output; **phase 5** ship; **phase 6** the
   shadowmask RTL patch - possibly unnecessary now that `SUB_BLEND` is 0, re-check
   every panel first.
9. Small, known, not urgent: the in-game "None" tile previews the *captured* frame,
   which already carries the running look's core colour; the firmware restarted under
   a running core believes the core holds the player's own LUT.

## How the pipeline works (what a look can and cannot do)

Core -> per-channel **gamma LUT** (before the scaler) -> **ascal** polyphase scaler
(64/256 phases) -> **shadowmask** (an output-space 2D cell table) -> HDMI. Nothing
programmable after the mask.

- **The phase floor**: at integer scale N only N of the 256 phases are visited, so a
  filter feature narrower than 256/N phases does nothing. The GBA grid vanished at 4x
  for exactly this reason.
- A gamma LUT is per channel: it cannot mix channels. Anything that mixes (GBA/GBC
  colour correction) must come from the core.
- Prefer core options over scaler tricks whenever the core models the panel (GB
  palette slot, GB colour LUT, GBA Modify Colors, GBA/Lynx/WS Flickerblend, NGPC
  Saturation and LCD Response). A gamma LUT stacked on core colour does it twice.
- Panel structure (grid, shadow, ghosting, integer scale) is `core_struct` and never
  applied on an analog output; colour is `core_colour` and applies everywhere.

## Firmware facts that cost time

- **The preset table is ABI.** `classicui_video.cfg` stores raw preset indices and
  the class number is inside `sys_key`. Append presets and classes at the end; never
  insert. Names can change freely.
- **Tile labels**: two lines of about ten characters. Long names lose words.
- **Gaps and shadows** (docs/HANDHELD-PANELS.md): reflective gaps are lit substrate
  (1.15x), colour-reflective 0.92x, backlit 0.80x; backlit panels get no shadow.
  `SUB_BLEND` is 0: no substrate cells are written, so every core draws the
  multiplier gap and the shadow reaches into it ("the pixel shadows should leak into
  the space between pixels").
- **GB colour LUT (FC7)**: 32768 entries x R,G,B, address `{r5,g5,b5}` (lcd.v), used
  only with `GBC Colors=Corrected`. It lives in core RAM until the core reloads, and
  FC7 is a *remembered* slot: the core reloads `<core>.f7` at boot. So "the player's
  own table" is that file, not the stock one. The stock table regenerates bit-exact
  from 1056 bytes (`lut_stock_t`, `lut_stock_g`).
- **GBA colour math** is gba_gpu_colorshade.vhd, ported twice (C `lut_entry`, Python
  `tools/gba_colorshade.py`) and pinned to each other on all 32768 colours.
- **A quiet file push** (`user_io_file_tx_quiet`): a look's .gbp/.lut must not raise
  the "Loading" box (the HDMI OSD is mixed before the scaler and gets baked into the
  frozen frame the preview is drawn from), must not call MenuHide at the end (that
  re-enters HandleUI and the front-end from inside the apply - a held key nested 156
  applies and killed the firmware), and must not overwrite /tmp/GAMEID.
- **Setname**: an MGL setname re-homes a core (games folder, config, savestates).
  Game Gear and GBC rows use one; a core found under its own name whose folder came
  from an alias is re-homed automatically (official NGPC core on a NeoGeoPocket card).
- **Core option names** are matched by name; a core that does not publish one skips it
  and logs `core has no X, skipped`. Check the card's real core: `get cfgstring` lines
  in the device log list them at core load.

## Tools

- `test/run.sh` - the host harness (thousands of checks); `test/looks.sh` - the
  firmware's own generated look files through the bit-exact model plus metrics.
- `tools/render_looks.py --generate --only id,id --out DIR --corpus tools/corpus` -
  1080p renders from the files the firmware generates (never from a Python restatement).
  Simulates palettes, GB LUTs (inverting the corpus frame through the stock table) and
  GBA Modify Colors; does NOT simulate core-side ghosting, NGPC Saturation/Panel, or
  System/GBA-mode straps.
- `tools/look_review.py screen.png outdir` - the 1:1 crop and 10x zoom (terminal and
  desktop variants) the review pane shows.
- **The look-review pane** is a Claude Code dev mod outside this repo
  (`mcp__look-review__show_look`): pass it the JSON look_review.py prints, a title and
  a question; Dinofly answers in the pane and it arrives as a `[look review] #N ...`
  prompt. Several looks side by side in one image work well (crop each to whole
  terminal cells: 16x34 by default).

## Device testing (read `.claude/skills/device-test` first)

- Build, deploy, capture through `.claude/scripts/` only. Restart checks: one MiSTer
  with ppid 1 (a child with that parent is the disc helper).
- **The HDMI dongle** (UGREEN 15389) shows the real post-scaler picture. Open it **by
  name** (`-f avfoundation -i "UGREEN 15389:none"`), never by index - index 0 was the
  webcam once. Skip ~45 frames; a UGREEN logo means no signal. It stretches contrast
  and boosts 1 px features: never judge a grid's strength from it alone.
- **The framebuffer grab** (`mister-capture`) shows the front-end, and a stale frame
  when a core owns the screen.
- **keyinj** (the virtual keyboard) drops and repeats keys. Send one key per call,
  and prove every apply by counting `now uses look` lines in the device log; an apply
  you did not see logged did not happen. If "applying video look" outnumbers "now uses
  look", something re-entered the apply. The menu bar remembers its last item, and
  left/right wrap. When a dialog stops responding, `echo "load_core /media/fat/menu.rbf"
  > /dev/MiSTer_cmd` gets back to the shelf.
- Every device test keeps its captures in `docs/img/device/` with the firmware md5.

## People and rules

- Nothing public without Dinofly approving the exact text; no `git push` without his
  `approve-once push`. Merge finished, tested work `--no-ff` into `classic-ui`; never
  switch branches in the main checkout (use worktrees).
- No credential, private path or device address in any repo file.
- Show looks through the review pane and wait for his verdict; he answers "looks good"
  or a change. When a verdict covers several questions, take it as approving all.
