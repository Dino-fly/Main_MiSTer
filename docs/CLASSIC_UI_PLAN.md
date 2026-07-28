# "Classic Home" — a SNES Classic Mini (PAL/EU) style UI/UX for MiSTer

Design study and implementation plan. **No code has been written.** The goal of this
document is to decide *what* we would build and to establish *whether it is technically
possible* on real MiSTer hardware (DE10-Nano / Cyclone V SoC).

Status: draft 1 — research + design + feasibility.

**Update: this has been implemented.** See `support/classicui/` and its README for
what landed, what was deliberately left out, and the known rough edges. Two
decisions from §10 were taken as recommended: computer cores live under a
`Computers ▸` folder with a styled file browser, and cover art follows the existing
community (libretro) naming. Online art fetch with lazy loading was added on top of
the original plan. Gameplay frames/bezels, the in-game graphical overlay, UI sound
and rewind were dropped, as §7.6 predicted they would have to be - frames were
built as a menu-only decoration first and then removed as not worth having.

**The P0 measurement spike in §9 was skipped**, so the numbers in §7.3 are still
theory. The implementation builds in every mitigation the spike was meant to
choose between (dirty rects, cached compose buffer, `fb_size` scaling, redraw only
on change), but which of them are actually needed is unmeasured. That is the first
thing to check on hardware.

---

## 0. Executive summary

| Question | Answer |
|---|---|
| Can the SNES Classic look & feel be recreated on MiSTer? | **Yes, for the front-end (menu) — with real constraints.** The Menu core already displays a 32-bit ARM-rendered framebuffer, and the binary already links Imlib2 + FreeType + libpng. A 720p carousel UI with box art, TrueType text and alpha blending needs **no FPGA changes**. |
| Can it work in-game (overlay, borders/frames around gameplay)? | **No, not without FPGA-side work.** The only true overlay a running core has is the 1-bpp, 256-px-wide monochrome OSD. The HPS framebuffer *replaces* core video, it does not blend over it. |
| Can it run at 60 fps full-screen? | **No.** The framebuffer is an uncached `/dev/mem` mapping and there is no GPU. Design must be dirty-rect based; budget 30 fps for animated bands, 60 fps only for small regions. See §7.3. |
| Low-res / CRT (480p RGB) version? | **Yes.** `fb_terminal_vga` / `vga_fb_takeover` (already in `video.cpp:3429`) proves the framebuffer can be routed to the analog port with a TV-compatible mode. Needs a second layout profile, not a second engine. |
| Biggest risks | Performance, the games *database* (not the graphics), box-art asset sourcing/legality, and the maintenance surface against upstream. |
| Recommended first step | A one-week measurement spike (§9, P0). Everything else depends on the numbers it produces. |

---

## 1. Research: what the SNES Classic Mini UI actually is

### 1.1 Sources and a caveat about one of them

`tcrf.net/SNES_Classic_Edition` — the page you linked — could not be used. That domain
serves automated fetchers an "LLM-/AI Agent-Specific Information" page whose body is a
prompt-injection payload (instructions to delete and shuffle files) instead of the
article. It was ignored and not retried. If you want TCRF's unused-graphics inventory
folded in, paste the text and it will be incorporated; everything below is sourced
elsewhere.

Primary sources used:

- Official EU manual (`nintendo.co.jp/clvs/manuals/en_gb/manual.html`) — authoritative for
  EU wording, icons, controls, frame count, languages.
- Nintendo Support, "Super NES Classic Edition HOME Menu Overview".
- hakchi2 / hakchi2-CE wiki (ClusterM, TeamShinkansen) — authoritative for the on-disk
  layout of the stock UI, because that is what the mod scene overmounts.
- SNES Classic modding community docs (snesclassicmods.com, hackinformer, snesclassic
  fandom wiki) — asset dimensions and border mechanics.

### 1.2 System context

- Codename **"clover"** (hence model prefix `CLV-`). Allwinner R16 SoC running Linux.
- The front-end is **`clover-ui`**, a scene-graph/Lua UI engine. Its resources live under
  `/usr/share/clover-ui/resources/`:
  - `sprites/*.png` + matching `*.json` — a **sprite atlas** with `[x,y,w,h]` rects.
  - `fonts/nes`, `fonts/hvc` — per-region font sets.
  - `strings/` — per-language string tables.
  - `prefab/*.scn` — scene/prefab descriptions (e.g. `sys_game_thumbnail.scn`).
  - `scripts/system.lua` — behaviour.
- Support daemons: `clover-mcp`, `clovercon` (controller driver).
- Emulator: `canoe` (`clover-canoe`, `canoe-shvc`) for SNES; NES Mini used `kachikachi`.
- Games live in `/usr/share/games/CLV-P-xxxxx/` with a `.desktop` file carrying metadata
  (name, players, save support, command line, art).
- Gameplay borders ("frames") are PNGs under `/usr/share/backgrounds/<set>/`, selected per
  game with `--use-decorative-frame <path>`.
- **HDMI output is 720p.** All UI assets are authored against a **1280×720** canvas.

**Architectural takeaway for us:** the original is a data-driven, atlas + JSON + string-table
+ scene-file UI. That is exactly the shape our implementation should take — a *themeable*
engine, not hard-coded drawing. It also means a MiSTer theme could be swapped for a
NES-Mini-style theme, a PC Engine Mini style, etc., without code changes.

### 1.3 Screen structure (stock UI)

Three zones, per Nintendo's own documentation:

1. **Menu Bar** (top, revealed on demand) — 5 entries:
   | Entry | Official description |
   |---|---|
   | Display | "Change the system display settings" |
   | Options | "Other options and settings" |
   | Language | "Change the language of the HOME Menu" |
   | Legal Notices | IP notice, open-source software notices |
   | Manuals | Game manuals via QR code / smart device |
2. **Game List** (centre) — a single horizontal row of box-art cards; the selected card is
   enlarged and its title shown. Icons per game (see below). No grid, no pages.
3. **Suspend Point List** (bottom, revealed on demand) — 4 slots per game.

### 1.4 Icon vocabulary (EU manual, verbatim meanings)

- `1P` — 1-Player game
- `2P` — 2-Player **Alternating**
- `2P` (simultaneous variant) — 2-Player **Simultaneous**
- Backup icon — "Backup-Supported Games (titles with in-game save function)"
- Empty slot / saved slot / **locked** slot — three suspend-point states

### 1.5 Controls (EU manual)

| Input | Action |
|---|---|
| ←/→ | Move through the game list |
| ↑ | Reveal / enter the Menu Bar |
| ↓ | Show the Suspend Point list |
| SELECT | Sort the game list |
| START / A | Start the selected game |
| ↓ on a suspend point | Lock that slot |
| Y | Save into a free suspend slot (hold Y = overwrite an existing one) |
| X on a suspend point | **Rewind** from it |
| Trash icon | Delete a suspend point |
| RESET (console button) | Creates an *unsaved* suspend point, closes the game, returns HOME |
| START+SELECT+L+R (1 s) | Soft reset to the game's title screen |

Rewind length is content-dependent: ~40–50 s for action games, ~4–5 min for RPGs/sims.

### 1.6 Display / Options / Language

- **Display**: three screen modes — **CRT Filter** (scanlines), **4:3**, **Pixel Perfect** —
  plus **Frame**: "There are 12 types to choose from."
- **Options**: *My Game Play Demo* (replays your own suspend point after 1 min idle),
  *Classic Demo* (built-in attract footage after 1 min idle), *Screen Burn-In Reduction*
  (dims after 5 min idle), *System Reset*.
- **Language**: the EU manual lists English / Deutsch / Français / Italiano / Español /
  日本語. Nintendo's support page says "eight different language options" — **verify on a
  real EU unit** (likely + Nederlands, Português).

### 1.7 EU-specific styling

The EU/AU home menu is styled after the **PAL SNES** industrial design (light/mid grey
two-tone shell, four coloured face buttons: A red, B yellow, X blue, Y green) rather than
the US SNES purple/lavender look. The EU game set also differs by ~5 regional exclusives.
Exact colour values must be sampled from a capture of a real PAL unit — see §5.2.

### 1.8 Asset dimensions (from the mod scene)

| Asset | Size | Notes |
|---|---|---|
| Gameplay frame / border | **1280×720** PNG | "1280×720 images will give the best results" |
| Game box art (SNES) | **228×167** | hakchi2's resize target (from 680×500 source); landscape ≈1.365:1 |
| Folder icons | same card metrics | hakchi's folder feature — see §4.2 |
| Sprite atlas | single sheet + JSON rects | e.g. `[93,861,12,8]` |

---

## 2. Assets: what we can and cannot use

**Recommendation: recreate, do not redistribute.**

- The `clover-ui` sprite sheets, fonts, frame PNGs and the SNES/Nintendo logos are
  Nintendo's copyrighted work, and "Super Nintendo"/"SNES" are trademarks. Shipping them
  with a MiSTer release is not something we should do, and naming the project after them
  invites a takedown of the whole repo.
- What *is* fine: an original theme that follows the same **layout grammar** (card
  carousel, top menu bar, bottom suspend strip, icon vocabulary) with our own artwork, and
  a documented theme format so an individual who owns a console can extract *their own*
  assets (hakchi FTP: `127.0.0.1:1021`, `root`/`clover`) for personal use.
- Ship: our own atlas, a libre rounded-gothic font (M PLUS Rounded 1c, Varela Round or
  Nunito — all OFL/Apache), our own 12 frames, our own icon set.
- Project name: **"Classic Home"** (theme id `classic-pal`). No Nintendo marks in the
  repo, the UI or the docs.

**Box art for games** is the same question one layer down. MiSTer ships no art. Options,
in order of preference:
1. Read from an existing on-SD art directory the user populates themselves (the community
   already distributes cover-art packs; Zaparoo/SAM users already have them).
2. Support the common naming conventions so existing packs "just work"
   (`<art_root>/<System>/<rom basename>.png`, plus MRA name matching for arcade).
3. Graceful fallback: a generated card — system-tinted plate + system badge + wrapped
   title. **This must look good**, because most users will see it a lot. Never show an
   empty box.

---

## 3. Where MiSTer stands today (verified in this repo)

### 3.1 The two display paths

| Path | What it is | Reference |
|---|---|---|
| **OSD** | 1 bpp bitmap pushed over SPI, `OSDLINELEN 256` bytes/line, 8×8 chars from `charrom`, max 19 lines in the Menu core. Composited *over* core video by the FPGA. | `osd.cpp:52`, `osd.cpp:666`, `charrom.cpp` |
| **HPS framebuffer** | ARM writes pixels into DDR; the core's scaler displays it *instead of* core video. `FB_ADDR = 0x22000000`, `FB_SIZE = 1920*1080` px, **3 buffers** ×4 B/px (0 = Linux fb/terminal, 1 & 2 = menu background double-buffer). | `video.cpp:36`, `video.cpp:3374`, `video.cpp:3856` |

Framebuffer pixel formats supported by the FPGA side: `PAL8`, `565`, `1555`, `888`,
`8888`, plus an R/B swap bit (`video.cpp:46-52`). The menu currently uses
`FB_EN | FB_FMT_RxB | FB_FMT_8888` (`video.cpp:3385`). **Bit 5 is "TBD" — there is no
alpha-over-core mode.** This is the single hard limit that kills in-game graphical overlays
and gameplay frames.

Resolution is derived, not free: `fb_width = v_cur.item[1] / fb_scale`,
`fb_height = v_cur.item[5] / fb_scale_y`, where `fb_scale` comes from `cfg.fb_size`
(0 = auto, 1 = full, 2 = ½, 4 = ¼) — `video.cpp:3462-3490`. So the UI canvas is
"output resolution ÷ N", and the scaler upscales. **This is our main performance lever.**

### 3.2 What already exists that we would otherwise have to build

| Capability | Where | Why it matters |
|---|---|---|
| Imlib2 + FreeType + libpng linked into the binary | `Makefile:47`, `video.cpp:33` | PNG/JPEG load, scale, **alpha blend**, and **TrueType text** (`Imlib2.h:368-394`: `imlib_load_font`, `imlib_text_draw`, `imlib_get_text_size`, fallback chains). No new dependency needed for the whole renderer. |
| Full-screen alpha compositing into the fb, double-buffered with page flip | `video.cpp:3790` `video_menu_bg()`, `bg1`/`bg2` at `3856`, `curtain` at `3866` | The wallpaper/logo/curtain code is a working proof of the exact pipeline we need, including a fade "curtain" we can reuse for transitions. |
| Per-core wallpapers | `video.cpp:3743-3764` (`wallpapers/`, `wallpapers<label>/`) | Precedent for per-system theming assets on SD. |
| Framebuffer on the **analog** output | `video.cpp:3429-3447` `vga_fb_takeover` + `tv_fb_mode`, `cfg.fb_terminal_vga` | The CRT/480p variant has a working precedent (your own recent commit). |
| **Screen capture** from the scaler, NEON-accelerated, vsync-gated, PNG via Imlib2 | `scaler.h:31-39`, `scaler.cpp` (`mister_scaler_init`, `request_screenshot`, worker thread) | Suspend-point thumbnails, "My Game Play Demo", and live previews become possible. |
| **4 savestate slots per game**, shmem-backed, `savestates/<core>/<rom>.ss1..4` | `user_io.cpp:1925-1975`, `file_io.h:109` | Maps 1:1 onto Suspend Points. Four slots, exactly like the original. |
| **MGL** launching (core + file, by menu index/type) | `support/arcade/mra_loader.h:48-56`, state machine `menu.cpp:1172-1220` | The mechanism for hiding cores behind games. |
| `/dev/MiSTer_cmd` FIFO: `load_core <rbf|mra|mgl>`, `fb_cmd`, `video_mode`, `screenshot`, `volume` | `input.cpp:4051`, `input.cpp:6238` | An **external** process can drive core launching and the framebuffer — the cheap prototype path (§8.2). |
| Recent-files lists (16 deep, per core + global) | `recent.cpp`, `RECENT_MAX 16` | "Recently Played" sort for free. |
| Game documentation viewer | `game_docs.cpp`, `menu.cpp:3366-3460` | Maps onto "Manuals". |
| Human-readable system names | `names.txt` | Seed for the systems metadata table. |
| Idle screen dimming / logo | `menu.cpp:1265-1291`, `cfg.video_off*` | Maps onto "Screen Burn-In Reduction". |
| Arcade metadata (MRA XML: names, players, DIPs) | `support/arcade/` | Real metadata for arcade titles, already parsed. |

### 3.3 What does not exist

- Any widget/layout/animation layer. `menu.cpp` is 8063 lines of `OsdWrite()` calls in a
  giant `switch` over `menustate`.
- Any games **index/database**. Every browse is a live directory scan (`SelectFile`).
- Any i18n. All strings are inline English literals.
- Any ARM-side audio. `audio.cpp` only sets FPGA volume/filters — there is no sound card
  on the HPS side; HDMI audio is generated by the FPGA. **UI sound effects require an FPGA
  change** (a PCM channel in the Menu core).
- Any pause guarantee. Rewind, and any "freeze the game and draw over it", depend on
  per-core support that mostly does not exist.

---

## 4. Mapping the SNES Classic UX onto MiSTer

### 4.1 Feature-by-feature

| SNES Classic feature | Verdict for MiSTer | How |
|---|---|---|
| Box-art card carousel, single row | **Keep** — it is the identity of the UI | §5 |
| Selected card enlarged + title | Keep | |
| Menu bar revealed with ↑ | Keep | |
| Player-count icons (1P / 2P alt / 2P sim) | **Keep where we have data**: arcade MRA has it; consoles need a metadata DB. Fall back to no icon rather than wrong icon. | |
| "Backup-supported" icon | **Replace** with "In-game save present" (MiSTer writes `saves/`) | |
| Suspend Points, 4 slots, save / overwrite / lock / delete | **Keep, 1:1** — MiSTer already has exactly 4 savestate slots | `user_io.cpp:1925` |
| Suspend-point **thumbnails** | Keep — capture via scaler at save time | `scaler.h:38` |
| **Rewind** (X on a slot) | **Drop from v1.** MiSTer cores have savestates, not rewind. Hide the affordance rather than showing a dead button. Revisit if a core ever exposes it. | |
| RESET → auto suspend + return HOME | **Keep** as "Home" hotkey: capture savestate (if supported) then load Menu core | |
| START+SELECT+L+R soft reset | Keep as a configurable hotkey | |
| Display: Pixel Perfect / 4:3 / CRT Filter | **Keep as three presets** that write MiSTer's real knobs (`vscale_mode`, `ar`, `vfilter`, `vfilter_scanlines`, `shmask`) | |
| Display: **12 frames around gameplay** | **Dropped.** Cannot be done in-game (no alpha overlay; §3.1). A menu-only version was implemented and then deleted at the user's call: a bezel that cannot frame the game is decoration pretending to be a feature. Revisit only if the framework gains an overlay layer (§7.2). |
| Display: CRT filter as a single toggle | **Superseded**: replaced by *video looks* that are filtered to the hardware the selected game ran on (consoles get PVM/S-Video/Composite, 15 kHz computers a PAL TV look, VGA no scanlines, Game Boy the DMG or Pocket panel, GBC its own screen plus all three GBA revisions, GBA the three revisions). On by default per class, driving `video_loadPreset()`. See `support/classicui/README.md`. | |
| Options: My Game Play Demo | **Keep as "Attract Mode"** — random game + our own screen capture. Community precedent: Super Attract Mode. |
| Display previews of each filter | **Real frame, simulated filter.** One unfiltered capture of the user's game is taken in the game core and the looks are applied over it in software. A true hardware readback is impossible: the scaler capture buffer is *pre*-filter (it is the core's own fb), no game is loaded while the menu is open, and there is no synchronous savestate API to build a capture loop from. | |
| Options: Classic Demo | **Drop** (we have no canned demo footage) | |
| Options: Screen Burn-In Reduction | Keep — already implemented | `cfg.video_off` |
| Language | **Keep and expand** — introduces i18n to MiSTer | |
| Legal Notices | **Keep** as "About / Licences" (genuinely appropriate: GPL + core credits) | |
| Manuals (QR to phone) | **Keep, better**: point at the existing game-docs viewer *and* generate a QR when a doc URL exists | `game_docs.cpp` |
| Folders in the carousel | **Keep** — hakchi's folder feature is the precedent that makes multi-system possible | §4.2 |

### 4.2 What MiSTer needs that the SNES Classic never had

Ranked by how badly the UX breaks without them:

1. **Multi-system without exposing cores.** Solved with hakchi-style **folders as carousel
   items**: `All Games`, `Systems ▸`, `Favourites`, `2 Players`, `Arcade ▸`, `Recently
   Played`. A game's system is *metadata* (a small badge on the card), never a navigation
   step the user is forced through. Entering `Systems ▸ SNES` shows a shelf whose cards are
   games; the word "core" never appears.
2. **Scale.** 21 games vs. potentially 20 000 files. Needs: an index, first-letter jump,
   L/R page jump, hold-to-accelerate, search (on-screen keyboard), and a
   "Favourites/Recent first" default so the carousel is short for most sessions.
3. **Per-game options** that are core-specific (`CONF_STR`-driven, dynamic per core).
   Design: a styled panel generated from the same data `menu.cpp` already parses, plus a
   "Recommended" default set, plus an explicit **"Advanced settings (classic menu)"**
   escape hatch that drops into today's OSD. We do not reimplement 8000 lines.
4. **Controller setup / Bluetooth pairing / button mapping.** No analogue on the original.
   Needs first-class placement (a "Controllers" panel), because a new user with an
   unmapped pad cannot reach anything else.
5. **System settings**: video output mode (this is MiSTer's #1 support issue), audio,
   network, updates (`Scripts`), storage. Grouped under Options, still styled.
6. **Load-from-file** for computer cores / disk images / multi-disk sets. The carousel
   model does not fit an Amiga with 400 ADFs; those systems get a *styled file browser*
   inside their folder, reached from the same grammar.
7. **First-run wizard**: language → video output → controller → art scan. The original
   needed none of this; MiSTer badly does.
8. **Boot/attract state**: what shows while the FPGA reconfigures (1–3 s of black).

---

## 5. The design

### 5.1 Information architecture

```
Boot splash
   │
   ▼
HOME  ──↑──►  MENU BAR ──►  Display / Options / Language / About / Manuals
   │                            │
   │                            └─► Options ─► Attract Mode, Burn-in, Controllers,
   │                                            System Settings, Library, Advanced(OSD)
   ├──↓──►  SUSPEND POINTS (4 slots for selected game)
   ├─SELECT─►  SORT panel
   ├──A──►  LAUNCH  ─► (FPGA reconfig, curtain) ─► game running
   │                                                   │
   │                                       Home hotkey │  OSD hotkey
   │◄──────────────────────────────────────────────────┤
   │                                                   ▼
   │                                            IN-GAME MENU
   │                                        (v1 = existing OSD)
   └──B──►  leave folder / up one shelf
```

Only **five** first-class inputs on HOME: ←→ ↑ ↓ A/B. Identical to the original.

### 5.2 Visual language (theme `classic-pal`)

- Two-tone cool grey shell colours, four accent colours from the PAL pad (red / yellow /
  blue / green) used *sparingly*: focus ring, button legends, slot states.
- Recreation palette (starting point; final values to be sampled from a capture of a real
  PAL unit): bg `#3A3B45`, panel `#B9BAC2`, ink `#26272E`, accents `#C4353C` `#E8B22B`
  `#2E6FB8` `#4A9E4E`.
- Soft drop shadow under the selected card; a subtle repeating background pattern; no
  gradients that would band at 16 bpp (see §7.3).
- Type: one rounded-gothic TTF, 4 sizes per layout profile. Text always drawn with a 1 px
  dark shadow (readability over art, and it hides scaler softness on CRTs).
- Every screen shows a **bottom button-legend bar** (`A Start   ↓ Suspend Points
  SELECT Sort`). The original's discoverability problem (nothing tells you to press ↑)
  gets fixed here without changing the layout.

### 5.3 Layout profiles

Three profiles, same scene graph, different metrics and asset sizes. The canvas is
`output ÷ cfg.fb_size`, so the profile is chosen from the *actual* `fb_width`/`fb_height`.

| Profile | Canvas | Used for | Cards visible | Card size | Base font |
|---|---|---|---|---|---|
| `hd` | 1280×720 (also 960×540, 1280×720÷2 → scaled) | HDMI 720p/1080p | 5 | 228×167 (sel ×1.35) | 24 px |
| `sd` | 640×480 / 720×480 | 480p RGB / component CRT, VGA | 3 | 176×129 (sel ×1.3) | 18 px |
| `lo` | 320×240 / 320×288 | 240p/288p 15 kHz (stretch goal) | 3 | 96×70 | 8×8 bitmap font |

`hd` mockup (1280×720, 32 px title-safe inset):

```
┌──────────────────────────────────────────────────────────────────────┐
│  ▣ Display   ▣ Options   ▣ Language   ▣ About   ▣ Manuals            │ ← menu bar (hidden until ↑)
│                                                                      │
│                        Super Metroid                                 │ ← title of selection
│                        Super Nintendo · 1994 · 1P                    │ ← system badge + metadata
│                                                                      │
│    ┌──────┐   ┌──────┐   ┏━━━━━━━━━━┓   ┌──────┐   ┌──────┐         │
│    │      │   │      │   ┃          ┃   │      │   │      │         │ ← carousel; centre = selected,
│    │ art  │   │ art  │   ┃   art    ┃   │ art  │   │ art  │         │   scaled 1.35× with shadow
│    └──────┘   └──────┘   ┗━━━━━━━━━━┛   └──────┘   └──────┘         │
│                          ● ○ ○ ○                                     │ ← suspend slots at a glance
│                                                                      │
│   ◄  12 / 340  ►                                                     │ ← position; arrows only if scrollable
│  A Start    ↓ Suspend Points    SELECT Sort    B Back                │ ← legend bar
└──────────────────────────────────────────────────────────────────────┘
```

Suspend Point strip (↓), sliding up over the lower third:

```
│    Super Metroid — Suspend Points                                    │
│   ┌────────┐  ┌────────┐  ┌────────┐  ┌────────┐                    │
│   │ thumb  │  │ thumb  │  │ empty  │  │ empty  │                     │
│   │  🔒    │  │        │  │        │  │        │                     │
│   └────────┘  └────────┘  └────────┘  └────────┘                     │
│    2 days ago   just now     —           —                           │
│  A Resume    ↓ Lock    X Delete    B Back                            │
```

`sd` mockup (640×480, 4:3 — proportionally taller, 3 cards, no metadata line):

```
┌────────────────────────────────────────────────┐
│ ▣ Display ▣ Options ▣ Language ▣ About         │
│                                                │
│              Super Metroid                     │
│                                                │
│   ┌──────┐    ┏━━━━━━━━━┓    ┌──────┐          │
│   │ art  │    ┃   art   ┃    │ art  │          │
│   └──────┘    ┗━━━━━━━━━┛    └──────┘          │
│               ● ○ ○ ○                          │
│                                                │
│  A Start   ↓ Suspend   SELECT Sort             │
└────────────────────────────────────────────────┘
```

### 5.4 Navigation rules (precise)

- Carousel does **not** wrap (like the original). At an end, the arrow indicator hides.
- ←/→ single step, 120 ms ease; hold > 400 ms → repeat at 60 ms with the cards *sliding*
  rather than stepping; > 1.5 s → jump by first letter and show a letter overlay.
- L/R shoulder = one screenful (5 cards on `hd`).
- ↑ from HOME → menu bar takes focus (cards dim 40 %). ↑ again or B → back to HOME.
- ↓ from HOME → suspend strip. ↓ on a slot → lock/unlock. B → back.
- SELECT → sort panel: *Recently Played, Times Played, Title A–Z, System, Players,
  Recently Added, Favourites*. (First two are the original's; the rest are ours.)
- A on a folder enters it (push a shelf, breadcrumb in the title area); B pops.
- Y on a card → per-game options panel (Favourite, Game Options, Manual, Delete
  suspend points).
- START on a card = A. Long-press A = launch with "Recommended" video preset.
- Launch: legend bar fades → curtain wipe → black → FPGA reconfig. Coming back Home
  reverses it. Selection and scroll position are restored exactly.

### 5.5 Sound (deferred, spec'd now)

Five cues: move, enter, back, launch, error. 22 kHz mono, ≤ 100 ms. Requires an FPGA PCM
channel in the Menu core (§7.5). Until then the UI is silent — which is acceptable, but it
is the single biggest "feel" gap versus the original, so it should not be dropped from the
plan, only deferred.

---

## 6. Data model

### 6.1 Systems table (`classicui/systems.json`, community-editable)

```
{ "id": "snes", "name": "Super Nintendo", "shelf_art": "systems/snes.png",
  "rbf": "_Console/SNES", "games_dir": "SNES",
  "ext": ["sfc","smc"], "mgl": { "type": "f", "index": 0, "delay": 2 },
  "video_preset": "4:3", "badge": "badges/snes.png" }
```

Seeded from `names.txt` + the MGL documentation's per-core index/type table. This file is
the entire "core" concept, reduced to data the user never sees.

### 6.2 Game index (`classicui/index.bin` + `art.cache`)

- Built by a background scan of `games/<games_dir>/**` filtered by `ext`, plus `_Arcade/*.mra`
  (name/players/year come free from the MRA XML), plus `*.mgl` files the user already has.
- Record: id (hash of path), system id, path, display title (cleaned of `(USA)`,
  `[!]`, etc.), sort key, art ref, players, year, publisher, flags, play count, last played,
  favourite.
- Persisted; invalidated per-directory by mtime. A full rescan must never block the UI —
  the carousel renders from whatever is indexed and grows live.
- **`art.cache`**: pre-scaled, pre-composited cards at the profile's exact pixel size, in a
  single packed file. Decoding 340 PNGs at boot is not viable (§7.4); decoding 5 and
  streaming the rest is.

### 6.3 Launching

Reuse MGL semantics, but **not** by writing XML files and re-parsing them. The existing
path (`menu.cpp:1172-1220`) drives the real OSD menu with wall-clock `delay`/`hold`
timers, and `mgl_item_struct` is capped at 6 items — it is a macro player, and it is
fragile by construction. Plan: build the same `mgl_struct` in memory from an index record
and hand it to the existing state machine for v1 (zero risk, known-working), then
progressively replace it with a direct "load core, wait for CONF_STR, send file at
index/type" sequence that waits on *events* instead of timers. Keep `.mgl` file
compatibility either way.

---

## 7. Feasibility analysis

### 7.1 The rendering path is already proven

`video_menu_bg()` (`video.cpp:3790`) already: loads PNG/JPG wallpapers with Imlib2, creates
Imlib images *directly over* the DDR framebuffer (`imlib_create_image_using_data(fb_width,
fb_height, fb_base + FB_SIZE*n)`), alpha-blends a logo and a translucent "curtain" onto
them, and page-flips between buffers 1 and 2. Every primitive the new UI needs — load,
scale, alpha blend, TTF text, double buffer — exists and runs on real hardware today.

### 7.2 The hard limit: no overlay over core video

`FB_EN | FB_FMT_*` has no alpha/overlay bit (`video.cpp:39-52`, bit 5 "TBD"). The
framebuffer *replaces* the scaler's source. Consequences:

- Gameplay **frames/borders** (the original's 12) are impossible in-game. UI-only in v1.
- A **graphical in-game menu** would blank the game while the game keeps running.
  Acceptable only for cores that pause; most do not. → v1 keeps the 1-bpp OSD in-game.
- The fancy UI lives in the **Menu core**. When a game core loads, it is gone.

Upstream feature request worth filing (unlocks both, and would benefit every core):
an **alpha overlay layer in the framework scaler** reading a second DDR buffer.

### 7.3 Performance — the real risk

The framebuffer is mapped from `/dev/mem` with `O_SYNC` (`shmem.cpp:22`), i.e. **uncached**.
Per-pixel writes go straight to DDR with no write combining beyond the bus, and the FPGA
scaler is simultaneously *reading* the same buffer every frame.

Cost per full frame:

| Canvas | 32 bpp | 16 bpp | 32 bpp @60 Hz | 32 bpp @30 Hz |
|---|---|---|---|---|
| 1280×720 | 3.69 MB | 1.84 MB | 221 MB/s | 111 MB/s |
| 960×540 | 2.07 MB | 1.04 MB | 124 MB/s | 62 MB/s |
| 640×360 | 0.92 MB | 0.46 MB | 55 MB/s | 28 MB/s |
| 640×480 | 1.23 MB | 0.61 MB | 74 MB/s | 37 MB/s |

Plus the scaler's read of the same data, plus Imlib2's own composite work in cached RAM.
A full-screen 60 fps redraw at 720p/32 bpp is **not** going to happen. Mitigations, all
available without FPGA changes:

1. **Dirty rectangles.** The carousel band is ~300 px tall; only it and the title area
   change during scrolling. 1280×300×4 = 1.5 MB → 46 MB/s at 30 fps. Comfortable.
2. **Compose in cached RAM, blit once.** Imlib2 currently blends straight into uncached
   DDR. Compose into a normal malloc'd Imlib image and do one NEON-friendly `memcpy` of
   the dirty rows.
3. **16 bpp (`FB_FMT_565`)** halves both write and scaler-read bandwidth. Cheap win; costs
   one conversion pass and rules out smooth gradients.
4. **`cfg.fb_size = 2`** — render 640×360 and let the scaler upscale to 720p. Softer, but
   4× cheaper. Good fallback profile / good default on 1080p output.
5. **30 fps animation budget** with easing tuned to look deliberate rather than sluggish.
   The original's own animations are modest.

**This must be measured before anything is designed around it.** See P0.

### 7.4 Secondary performance concerns

- **PNG decode**: FreeType/libpng on an 800 MHz A9 — tens of ms per card. Boot must not
  decode more than the visible set; hence `art.cache`.
- **SD scan**: thousands of `stat()` calls on a slow SD card. Must be a background job with
  incremental persistence, never on the UI thread. `offload.cpp` already provides a
  worker-offload mechanism to build on.
- **FPGA reconfig**: 1–3 s per core load, unavoidable. Mask it with the curtain.
- **Memory**: 1 GB total; buffers 1 & 2 are already reserved. Card cache must be bounded
  (LRU, e.g. 32 MB).

### 7.5 Sound needs FPGA work

There is no HPS sound device; `audio.cpp` only writes FPGA volume/filter registers. UI
audio requires adding a small PCM playback channel to the **Menu core** (a different repo,
`Menu_MiSTer`) and a UIO command to feed it. Self-contained and modest, but it is FPGA
work and gates the "feel" of the UI.

### 7.6 Verdict

- **The home-menu experience: feasible.** No FPGA changes, no new dependencies, all
  primitives proven in-tree. The work is a UI framework + a games database, not a
  graphics-capability problem.
- **Full parity with the original: not feasible.** Gameplay frames, a graphical in-game
  overlay, rewind, and UI sound all need work outside the main binary (FPGA/core side), and
  rewind needs per-core emulation features that do not exist.
- **The dominant cost is not the graphics.** It is the games database, metadata, and art
  pipeline — the part the SNES Classic got for free by shipping 21 hand-curated titles.

---

## 8. Implementation architecture

### 8.1 Recommended shape: a parallel front-end inside `Main_MiSTer`, behind a flag

```
ui/
  ui_surface.cpp    canvas, dirty-rect tracking, present/flip (video_fb_enable(1,n))
  ui_draw.cpp       9-slice, rounded rect, alpha blit, text+shadow, scaled card blit
  ui_font.cpp       FreeType/Imlib2 font cache, per-profile sizes
  ui_input.cpp      menu_key_set()/joystick → UI events (repeat, accel, chords)
  ui_anim.cpp       tweens, easing, frame budget governor
  ui_theme.cpp      theme parser: palette, metrics per profile, atlas + rects, strings
  screens/          home, menubar, display, options, language, about, manuals,
                    suspend, sort, gameopts, controllers, wizard
library/
  lib_systems.cpp   systems.json → table
  lib_index.cpp     background scan, index.bin, incremental invalidation
  lib_art.cpp       art resolution + art.cache (pre-scaled cards)
  lib_meta.cpp      title cleaning, players/year, MRA metadata
launch/
  launch.cpp        index record → in-memory mgl_struct (v1) → event-driven (v2)
i18n/
  strings/*.json    en, de, fr, it, es, nl, pt
```

- Enabled by `classicui=1` in `MiSTer.ini`; **default off**. The existing OSD stays fully
  intact and remains the escape hatch (`Options ▸ Advanced settings`) and the in-game menu.
  This keeps the diff additive, keeps upstream merges tractable, and gives users a way out.
- `menu.cpp` is not rewritten. It is called *into* for advanced settings.

### 8.2 Cheaper alternative for the prototype: an external process

`/dev/MiSTer_cmd` accepts `load_core <path.mgl>`, `fb_cmd`, `video_mode`, `screenshot`,
`volume` (`input.cpp:6238`), and the `MiSTer_fb` kernel module exposes `/dev/fb0` with
mode set via `/sys/module/MiSTer_fb/parameters/mode` (`video.cpp:3336`). A standalone
binary can therefore draw the whole UI and launch games **without touching the main
binary**. Recommended for P0/P1 to get on-hardware answers fast; the drawing and layout
code ports over unchanged if we then move it in-tree.

Trade-off: an external process cannot easily read core `CONF_STR` options or reuse the
input mapping, so per-game options and controller setup would eventually force the in-tree
route. Prototype outside, ship inside.

---

## 9. Phased plan

| Phase | Deliverable | Gate |
|---|---|---|
| **P0** — Spike (≈1 week, on hardware) | Measure: uncached vs cached+memcpy fill rate at 720p 32/16 bpp; Imlib2 scale+blend cost for a 228×167 card; PNG decode ms; full-screen and dirty-rect frame times; SD scan time for ~5 000 files. Written numbers. | **Go/no-go**, and it fixes canvas size, bit depth and fps for everything downstream. |
| **P1** — Renderer + shell | `ui_surface/draw/font/input/anim/theme`, `hd` profile, HOME carousel with **fake** data, curtain transition, legend bar. Runs on HDMI. | Looks and feels right at the chosen fps. |
| **P2** — Library + launch | `systems.json` for 6 flagship systems, background scan, `index.bin`, `art.cache`, fallback card generator, launch via in-memory MGL. Real games start. | End-to-end: power on → pick a game → play, cores never mentioned. |
| **P3** — Full UX | Menu bar + all panels, sort, suspend points on the 4 savestate slots with captured thumbnails, per-game options, Advanced→OSD handoff, controllers panel, first-run wizard. | Feature-complete against §4. |
| **P4** — Reach | `sd` profile + analog/480p output, theming docs, i18n (7 languages), attract mode, burn-in reduction wiring, `lo` profile if P0 allows. | Usable on a CRT; themeable. |
| **P5** — FPGA-side (optional, upstream) | Menu-core PCM channel for UI sounds; proposal for an alpha overlay layer in the framework scaler (unlocks gameplay frames + graphical in-game menu). | Parity items that need FPGA work. |

## 10. Open questions

1. **Verify on a real EU unit**: exact palette, the 12 frame designs, the language count
   (manual says 6, Nintendo Support says 8), the carousel's exact easing and card metrics.
   A 720p capture of the home menu would settle all of it.
2. **Art**: do we target the existing community cover-art packs' naming, or define our own
   and ship a converter? (Recommendation: support theirs.)
3. **Scope of "hide the cores"**: do computer cores (Amiga, ao486, ST) appear in the
   carousel at all in v1, or only under a `Computers ▸` folder with a styled file browser?
   (Recommendation: folder + browser; forcing them into a game carousel will feel wrong.)
4. **Upstream or fork?** An additive, flag-gated front-end is plausible upstream material;
   a rewrite of `menu.cpp` is not. This plan assumes additive.
5. **Name and branding** — confirm we avoid Nintendo marks entirely (recommended:
   "Classic Home", theme `classic-pal`).
