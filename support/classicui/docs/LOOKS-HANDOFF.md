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
| GBC: GBC, GBC on GBA / GBA SP / SP Brighter (+ GBA mode), SGB, None, None with SGB Border | approved; GBA mode confirmed on device (Oracle of Seasons) |
| GBA: GBA, GBA SP, SP Brighter, Game Boy Micro, GBA on DS, None (all Flickerblend=Blend) | approved; options confirmed on the card's core |
| Game Gear: stock, Backlit Mod, McWill | approved (sim) |
| Lynx: stock, McWill | approved (sim) |
| WonderSwan / Color: WS, WSC, SwanCrystal | approved (sim) |
| NGPC: NGPC, NGPC IPS Mod; NGP (mono class): Neo Geo Pocket, Mono on NGPC | approved, all four captured live on the official core (#9-#11) |
| CRT: Sharp, PVM RGB, PVM S-Video, Composite, PAL TV, BVM RGB, VGA | phase 3, not started |
| computers | phase 4, not started |

Branches: `classic-ui` carries all the handheld work (the NGP/GBA-mode merge of
`classicui-handhelds2` on 2026-10-28); 5bd5299e is pushed to the fork, anything after
it needs Dinofly's `approve-once push`. The working worktree is `../Main_MiSTer-looks`;
start phase 3 on a new branch off `classic-ui` there.

## Todo

4. **Official NGPC core, savestates** (fixed 18f49ed1; cause and fix in git log): a
   player's "save here" waits 8 s for a deferred save; states from the old community
   core share savestates/NeoGeoPocket and must not be offered to this one.
4b. **Official NGPC core, live switch**: NGPC -> NGPC IPS Mod applied live once left the
   picture white until relaunch (LCD Response Panel -> Raw plus Saturation 75 -> 100 at
   once). Seen once, not yet reproduced; the same look from a launch is fine.
5. **Harness gap**: the clip log only sees Display screens the harness draws (GB,
   GBC, GBA, console). Render every handheld class's Display screen there so a tile
   name that wraps past two lines fails the cut-text check ("Neo Geo Pocket Color"
   lost "Color" on the device and nothing caught it).
6. **Phase 1 leftover**: an explicit `look_is_panel` field, now that CRT looks carry
   core options.
7. **Phase 3, TV/arcade looks** (branch `classicui-crt-looks`). The dongle is unreliable
   for 1 px structure (see below); validate scanline depth against the simulator.
   - **Next:** confirm the approved renders on the device (dongle captures of each
     console look on Genesis and SNES, scanline swing vs the model), then the 480i ifilter.
   - **CRT renders (merged 403f8a51, Dinofly approved reviews #1-3):** console corpus in
     tools/corpus (SNES 512x224 with every column doubled, Genesis 320x224, NES 256x224,
     PSX 405x240/352x240 incl. blanking, arcade Rastan 320x240; no TG16 games on the card).
     render_looks.py renders the TV classes at vscale_mode=1 geometry (integer height, 4:3
     width: 1195x896 for 224 lines) with a scanline-swing metric; distribution files the
     presets name live in tools/dist. Not simulated: core halves (Composite Blend, NES
     Budget TV), interlace.
   - **What a CRT look can drive in the core** (CONF_STRs read 2026-10-30, exact names):
     `Composite Blend` Off/On/Adaptive (Genesis, MegaCD, S32X; Saturn Off/On);
     `Composite Filter` Off/Budget TV/Premium TV (NES; hides Scandoubler Fx) and Off/On
     (Atari7800, with Smear/AGC/Comb); N64 `Video Out` Original(VI)/Clean HDMI plus the
     VI chain; SNES `Pseudo Transparency`; PSX `Dithering`. No signal option at all: SMS,
     TG16, NeoGeo, Coleco, Intellivision, the arcade template. A look can list both
     `Composite Filter=Budget TV;Composite Filter=On`: a value an option lacks is skipped.
   - **480i:** set_vfilter uses the interlace filter (else the hfilter) vertically for an
     interlaced source, and our presets write no `ifilter` line - so on PSX/Saturn/N64
     480i a CRT look loses its scanlines (right) and inherits whatever ifilter the
     player had (wrong: should be written).
   - **Dinofly's calls (2026-10-30):** Composite drives the core signal options
     (`Composite Blend=On;Composite Filter=Budget TV;Composite Filter=On`) with the
     scaler hblur lowered Blurry -> Soft; arcade swaps S-Video for a new **Arcade CRT**
     look (slot mask, appended as P_ARCADE_CRT); no PAL exception for consoles (SCART was
     RGB); every preset writes `ifilter=` its hfilter. Vectrex later, as its own class.
   - CRT scanlines live in the vfilter (+ sfilter copy): set_vfilter ignores the sfilter
     unless the core's Scandoubler Fx is on. Device swing 23-31 levels (`docs/img/device/crt-*`).
   - **Super Game Boy (Dinofly 2026-10-30):** it ran on a Super Nintendo, so it offers
     every console television - SGB PVM RGB (the old "sgb" index), SGB BVM RGB, SGB
     S-Video, SGB Composite - plus the adaptor alone (none-sgb, renamed "Super Game Boy"
     2026-10-30: "None with SGB Border" lost "Border" at every profile). Shared CRT_* macros keep the
     scaler halves identical to the console's. 11 GB looks: at 240p the tiles become
     names, three to a row. On device the adaptor switches on and off correctly, but no
     border showed on Link's Awakening (open: try an SGB-enhanced game).
   - **Analog output (Dinofly 2026-10-30):** a CRT shows no CRT emulation and no LCD
     structure - only what reaches a tube: handheld palettes/colour, the SGB border, the
     off switch. Every television look carries `analog_as` (CRT looks -> Sharp, SGB
     televisions -> none-sgb); it is not listed on analog, and a stored choice plays its
     stand-in there via for_output() inside vp_preset_path_for/vp_apply_core_side, so the
     choice itself is never rewritten. Console/arcade/computer/VGA classes are left with
     Sharp alone, so Display drops off the bar for them on a tube. "A tube" means
     vp_output_is_lowres(): analog, unless vga_scaler drives it at >= 720 lines (then
     every look, CRT ones included - Dinofly). HDMI is never low-res. Harness only - the
     device has HDMI attached, so the analog path was not exercised there.
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

- `.claude/scripts/mister-ui` - place the front-end by name instead of by arrow keys:
  `mister-ui goto gbc "Oracle of Seasons"`, `look "GBC on GBA"`, `display`, `launch`
  (waits for the core), `menu` (waits for the shelf), `state`. Each prints the
  firmware's one-line answer (`ui <verb> -> ok screen= sys= sel= title= look=`) and
  exits non-zero on FAILED, so a script needs no picture to know where it is.
  It is the `ui` command in /dev/MiSTer_cmd (chome_test_ui). It proves nothing about
  the key or pad paths; capture when the picture is the point.
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
