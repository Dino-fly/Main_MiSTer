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

1. **Push `classic-ui`** past 5bd5299e once Dinofly sets the marker (`approve-once push`).
2. (done 2026-10-28) NGP family captured live and approved.
3. (done 2026-10-29) GBA mode on device. Under None, GBA mode alone brightens Oracle of
   Seasons' intro by ~+30/255 in red (the game's GBA palettes). Under GBC on GBA the
   look sets it after the ROM upload, yet the frame is byte-identical to a launch with
   GBA mode already in GBC.CFG - it lands before the game checks. Captures
   `gbc-oracle-none-hdmi.png`, `gbc-oracle-on-gba-gbamode-hdmi.png` (firmware 5b9d1e48).
4. **Official NGPC core, savestates** - DONE 2026-10-30 (branch `classicui-ngpc-ss`,
   firmware 18f49ed1). Row is now `CH_SS_YES`.

   **Cause (measured).** The save entries are `d7` - gated on menumask bit 7, `allow_ss` -
   and the core's `savestate_ui` drops a request made while the gate is shut: its save/load
   edge detector only runs inside `if (allow_ss)`. On Sonic Pocket Adventure the gate is
   shut at the instant the menu opens, so the hold-still pulse went nowhere: no file, and
   the restore on the way out loaded whatever slot 4 (or the core's slot) held before - the
   dim frozen frame. KOF R-1 was never affected: its gate is open at menu open (it mounts
   no cart save, which points at the overlay's flash autosave on the `OSD_STATUS` rising
   edge - read from the source, not measured).
   The handoff's earlier theory (a slot restored from NGPC.CFG at load) was wrong for this
   symptom: the card has no NGPC/NeoGeoPocket .CFG at all, and the baseline wrote `_4.ss`
   on KOF. The slot step-through it led to is kept - it is correct for a CFG slot, cheap,
   and harmless - with its comment corrected.

   **Fix.** `ss_scan_hooks()` records the save entry's D/d gate; `ss_do_save/load` refuse
   (one read, no wait) while it is shut; `freeze_engage()` defers the hold-still save and
   `freeze_due_poll()` (from `chome_pend_poll`, every frame) fires it when the gate opens,
   bounded at 30 s; `freeze_release()` skips the restore if the save never went. A
   *blocking* wait does not work: blocked 20 s, the gate never opened - the core needs the
   main loop to finish whatever shut it. Unblocked, it opened 28 ms later.
   Harness: `assert_savestate_gate` (20 checks). Device: Sonic suspend -> `_4.ss` written
   28 ms after menu open, resume into a running game; KOF unchanged. Captures
   `ngpc-sonic-*-{5b9d1e48,18f49ed1}.png`, `ngpc-kof-resumed-*.png`.
   Still open: a player-chosen "save here" copies the held state and waits 8 s for it - a
   deferral longer than that would make it give up (not seen: 28 ms measured). States from
   the old community core share savestates/NeoGeoPocket and must not be offered to this one.
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
