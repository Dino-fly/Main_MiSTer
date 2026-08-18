# What this changes in the firmware, and why

Classic Home is a new module under `support/classicui/`. That part is additive and
easy to reason about: nothing else calls into it except through `chome.h`, and with
`classicui=0` none of it runs.

This document is about the *other* part — the changes to files that were already
there. Those are the ones that can break a MiSTer that is not using the front-end at
all, so each one is written down here with the reason it exists and what it costs.

Lines changed against `master`, excluding the new module and its docs:

```
 MiSTer.ini      50        cfg.cpp/h      28        user_io.cpp/h  131
 input.cpp/h    357        video.cpp/h   298        menu.cpp        48
 scaler.cpp/h    81        audio.cpp/h    20        joymapping.cpp   6
 cheats.cpp/h    72
```

Everything below is guarded by `cfg.classicui`, by `chome_active()`, or by a request
the front-end only makes while it is on screen. The one exception is noted as such.

---

## 1. The framebuffer the UI draws into

**What was added.** `video_menu_fb(n)`, `video_menu_fb_width/height()`,
`video_menu_fb_present(n)` in `video.cpp`.

MiSTer already keeps an HPS framebuffer that the menu core uses for its background
picture, page-flipped between buffers 1 and 2. The front-end draws into that same
pair. Buffer 0 belongs to the Linux fb terminal and is never handed out.

**Why it is a real dependency, not a detail.** The framebuffer is composited by the
FPGA *over* core video. That is what makes an in-game menu possible at all — and it
is also why a core that does not carry the HPS framebuffer support cannot host the
front-end. `ig_open()` checks and declines:

```
ClassicUI: core has no HPS framebuffer, leaving the OSD to it
```

On such a core the menu button falls back to whatever the core does normally.

---

## 2. The analog takeover — the largest change, and the one to understand

This is the part with the most surprising consequences, so it gets the most space.
See [Video outputs](#video-outputs) below for the whole picture.

**The problem.** On a machine with no HDMI display, the scaler output — where the HPS
framebuffer is composited — goes nowhere visible. The analog port is carrying raw,
scandoubled core video instead. So the entire UI renders into a buffer nothing
displays, and the screen stays black.

**What was added.** `video_menu_fb_analog(on)` and the `vga_fb_takeover` machinery in
`video.cpp`. When the front-end asks and no HDMI sink is attached, the firmware:

1. saves the current video mode,
2. switches to a TV-compatible 240p mode (`tv_fb_mode()`, the same mode selection
   `direct_video` uses),
3. routes the scaler output to the analog DAC (`set_vga_fb(1)` → `CONF_VGA_FB`),
4. reverses all of it on the way out.

**The condition, which is the important line:**

```c
int want_ui = menu_fb_analog_req && fb_num && !hdmi_present();
```

`!hdmi_present()` is not an optimisation. Taking the analog port means changing the
*shared* video mode, and HDMI would be dragged down to a 240p TV mode with it. "HDMI
plus a CRT" is an ordinary MiSTer setup and must not lose its picture because a
front-end wanted the other output.

**Consequences you will actually notice:**

- On an analog-only machine the UI canvas is **320×240**, not 720p. That is a real
  240p canvas to draw on, not a 720p layout crushed into 240 lines — the flag goes up
  *before* `video_set_mode()` so `video_fb_config()` sizes the buffer for the new mode.
- The mode is **pinned** while the takeover holds (`if (vga_fb_takeover) return;` in
  `video_set_mode`). Core video changes are picked up after release.
- The request is re-asserted **every frame**, because the fb terminal shares the same
  takeover and drops it on the way out.

**Three attempted variations that were reverted.** Calling `video_fb_config()` after
taking the output, after releasing it, and reordering the two. The first genuinely
fixed a striped menu on one setup — and stopped the core servicing savestates. None
are in the tree; if the striping reappears, that is the history.

---

## 3. Input: making a pad reach a front-end that is not the OSD

`input.cpp` is the second-largest change and the least obvious.

**The gate.** Pad buttons do not arrive as themselves. `joy_digital()` turns them into
synthetic key events, and it only does so when the classic OSD is visible. A
front-end that is not the OSD therefore received *nothing at all* from a controller
while working perfectly from a keyboard. The gate now reads:

```c
if (user_io_osd_is_visible() || chome_active() || (bnum == BTN_OSD))
```

> This is the single most important line in the integration. Keyboard and pad reach
> the front-end through different paths (`user_io.cpp` versus `input.cpp`), so a
> keyboard-driven test says nothing about the other one. That mistake once hid a
> total pad-input failure for a full test pass.

**Y is no longer swallowed.** `JOY_BTN3` is `KEY_BACKSPACE`, which in the classic OSD
belongs to the synthesised A+B combo rather than to Y, so a standalone Y was dropped.
The front-end binds Y to Save on the suspend screen, so it is let through while the
front-end owns the screen. A+B still synthesises `BTN3` as before, and the classic OSD
is unchanged.

**Which device sent the key.** `joy_digital()` hands its synthetic event back with a
hardcoded device index of 0 — a slot belonging to whichever device happens to be first
in the pool, which is not the pad and on some pools is not open at all. So the last
real device to be handed an event is remembered, and exposed as
`input_menu_key_from_pad()`, `input_menu_key_devname()`, `input_menu_key_vidpid()`.
This is how the button prompts know whether to draw PlayStation shapes or letters.

**New read-only queries**, all additive: `input_pad_list()` (what controllers exist,
with vid/pid and wired/Bluetooth/SNAC), `input_pad_state()` (live buttons and sticks,
for the tester), `input_menu_key_btn()` (which physical button the *user's own* map
binds to a menu button — a remapped pad must still be described correctly).

**Menu-button auto-repeat** (`menu.cpp`). `menu_key_get()` auto-repeats held keys.
Applied to the menu button itself, holding it toggled the front-end open and shut
several times a second. The repeat branch now excludes it:

```c
int menu_btn = ((c1 & ~UPSTROKE) == KEY_F12 || (c1 & ~UPSTROKE) == KEY_MENU);
```

The fix lives in `menu.cpp` and not in the front-end because `menu_key` is a single
latch, not a queue: an earlier attempt to absorb the repeats downstream broke the
mirrored direction, so closing needed two presses.

---

## 4. Save states

**Thumbnails.** `process_ss()` in `user_io.cpp` writes a `.png` beside each `.ss` so
the suspend strip has a picture. Guarded by `cfg.classicui`, and skipped for the
reserved slot (`chome_hidden_slot()`), which the player never sees.

**Silence for states the player did not ask for.** Holding a game still means taking a
savestate, and two things in the firmware announce every state: the core's own
CONF_STR message ("Save to state 4") raised as an OSD panel, and the thumbnail.
`chome_ss_quiet()` suppresses the announcement for the reserved slot only.

**Re-entrancy.** `process_ss()` used to call `MenuHide()` unconditionally. While
Classic Home owns the screen that is re-entrancy into the front-end's own frame, and
it left a state save with a classic-OSD panel over the game and the framebuffer in a
state the next menu open drew garbage from. It is now skipped when the front-end is
up, which hides its own OSD anyway.

**`mister_scaler_quiet()`** (`scaler.cpp`) silences the two diagnostics
`mister_scaler_init()` prints on every call — a 16-byte hex dump and a line of
geometry. Off by default, so every upstream path prints exactly what it always did.
Classic Home reads that header once a second to learn how large the scaler is drawing
the game (the in-game menu's background has to be that size, and by the time it is
built the framebuffer has taken the screen and the scaler describes the menu instead).
Two log lines a second is an SD-card write a second, which showed up as the picture
wobbling.

**`screenshot_grab()`** (`scaler.cpp`) reads the current scaler output into a caller's
buffer. Used for the still the in-game menu is drawn over, and for slot pictures. The
existing screenshot path writes files on a worker thread; this needed the pixels
synchronously, and it declines if a file screenshot is already in flight.

---

## 5. Audio

`audio_mute()` / `audio_is_muted()`. `set_volume()` shows an on-screen "Mute" message
composited *over* the front-end, and rewrites the user's saved volume. This is a
temporary mute for as long as a menu is up, not a preference, so it sets the mute bit
directly and leaves `vol_set_timeout` alone.

---

## 6. Pop-ups suppressed while the front-end is up

`joymapping.cpp` — the controller-mapping banner (`cfg.controller_info`) is suppressed
via `chome_active()`. `menu.cpp` — `OsdDisable()` when the front-end owns the screen.

These are the only changes that alter behaviour for something other than the
front-end, and both are conditional on it being active.

---

## 7. Configuration

The keys in `cfg.h`/`cfg.cpp`/`MiSTer.ini`. Adding one means all three in step.

| Key | Default | Note |
|---|---|---|
| `classicui` | `0` | Off unless asked for |
| `classicui_profile` | `0` | auto / hd / sd / 240p |
| `classicui_overscan` | `6` | percent kept clear of the edge |
| `classicui_artdir` | `boxart` | under the SD root, and where downloaded covers are cached |
| `classicui_artfetch` | `0` | download missing art |
| `classicui_artfill` | `1` | keep fetching missing art from an idle shelf; needs `classicui_artfetch` |
| `classicui_arturl` | libretro thumbnails | where art is fetched from |
| `classicui_gamelist` | `1` | read `gamelist.xml` for art scraped elsewhere |
| `classicui_freeze` | `1` | hold the game still while the menu is open |

`classicui_freeze` and `classicui_gamelist` default to **1**, both set explicitly in
`cfg_parse()` — a zeroed struct would mean off.

---

## 8. Reading the cheat store

**What was added.** Five functions in `cheats.cpp`/`cheats.h`: `cheats_name(idx)`,
`cheats_is_enabled(idx)`, `cheats_set_enabled(idx, on)`, `cheats_active()`,
`cheats_max_lines()`. Nothing was changed or removed.

**Why.** The firmware already loads a game's cheats — `user_io.cpp` calls `cheats_init()`
on every ROM load behind `user_io_use_cheats()` — and by the time an in-game menu can be
opened the store is full. What it had no way to do is *read* it. Everything upstream
exposes is written around the OSD's own cursor and its 32-column buffer: `cheats_print()`
draws into the OSD, `cheats_scan()` moves that cursor, and `cheats_toggle()` acts on
wherever it happens to be. A front-end with its own layout can use none of it.

**How it stays additive.** `cheats_set_enabled()` does not reimplement the enable path —
it borrows `iSelectedEntry`, calls the existing `cheats_toggle()`, and puts the cursor
back. The lazy load from the zip, the length check, the `cheat_max_active` budget and the
resend to the core are therefore upstream's code on upstream's terms, and there is no
second copy of them to keep in step. It returns what the store says *afterwards* rather
than what was asked for, because enabling can be refused when the budget is spent — see
below.

**One thing worth knowing before using any of it.** `cheats_loaded()` is not "how many
cheats are loaded". It is `pos / cheat_unit_size` from `cheats_send()`: the number of cheat
*lines currently handed to the core*, and therefore zero until the player switches one on.
The count of records is `cheats_available()`, which is what `menu.cpp:2598` keys the classic
OSD's own entry on and what the front-end keys its row on. A feature gated on
`cheats_loaded()` can never be the thing that enables the first cheat.

**And the refusal.** A core takes `cheat_max_active` code lines (128 unless an MRA says
otherwise) and a cheat is a whole number of them, so `cheats_toggle()` can decline. Upstream
prints that to the log, which on a television is nowhere. The front-end's Cheats screen says
it under the list instead; the accessors are what let it tell a refusal from a success.

There is no version to check and none is needed: this is firmware-side only, so unlike the
SNAC reader (§4 of the hacks below) no core has to be rebuilt for it.

---

## 9. Setting the video mode at runtime

**What changed.** `video_mode_cmd()` in `video.cpp` now accepts everything MiSTer.ini's
`video_mode=` accepts, where it took custom modelines only. **What was added.**
`video_mode_restore()` beside it.

**The widening.** `video_mode_cmd()` parsed its argument and refused anything that was not
a modeline:

```
video_mode_cmd: only custom modelines are supported, got "2"
```

So the one form of the setting that MiSTer.ini documents — the predefined numbers, with the
table of them in a comment above the key — was the one form it could not apply. A
predefined number now fills `v` from `vmodes[]` exactly as `store_custom_video_mode()`
does for the same input: same timings, same VIC, same pixel-repeat flag.

That identity is the reason for the change rather than a nicety. The front-end previews a
mode and then writes the setting, and those two have to be the same thing or the preview
is answering a different question from the save — a modeline sets VIC to 0 where the
predefined form sets the CEA id, which over HDMI is the difference between a television
recognising 480p and treating it as an unknown PC mode.

`store_custom_video_mode()` is not called directly because it cannot refuse: a value it
fails to parse falls back to mode 8 or 0 and returns 0, which is right for a config file
being read at boot and wrong for a command that should say no. Nothing that worked before
behaves differently; the change is strictly a widening of what is accepted.

**`video_mode_restore()`** re-runs `video_mode_load()` and `video_set_mode(&v_def, 0)` —
"put back whatever the configuration says", by the path `video_init()` already uses. It
re-derives rather than restoring a snapshot, so there is no saved copy that can go stale.

**And `video_reinit()` now keeps a runtime mode across an HDMI re-initialisation**, which
is the third change and the one the device found. Changing the video mode reprograms the
transmitter; the sink re-asserts HPD; the interrupt fires; `video_reinit()` re-derives
`v_def` from cfg and puts the configured mode back. Measured: a mode applied at 800x600
was gone inside two seconds, with a fifteen-second confirmation still running and nothing
on screen to show for it. It is not dongle-specific — reprogramming the ADV7513 and moving
the pixel clock is exactly what makes a sink re-assert HPD, and some displays will.

So `video_mode_cmd()` remembers the string it applied and `video_reinit()` re-applies it
after `video_mode_load()` has overwritten `v_def`. Re-initialising is the right response to
a display appearing or changing; deciding the machine's *mode* is not, when something has
explicitly set one since boot. `video_mode_restore()` clears the memory, so a trial that
provokes a bounce on every attempt can only do so for as long as the countdown allows.

**Why either exists.** The front-end offers a per-core video mode, which is a setting
`chome_opt.h` names by name as too dangerous to hand to a player. What makes the per-core
one different is in `chome_video.h`; what makes it *safe* is these two calls. Choosing a
mode applies it and starts a fifteen-second countdown, and `video_mode_restore()` is what
runs when nobody confirms. Nothing is written to MiSTer.ini until a second press, so a
display that cannot lock to the mode — which on a board with an empty EDID cannot be
predicted — costs fifteen seconds of black and changes no file.

---

## 10. Configuration, per core

The front-end writes one key into a section of MiSTer.ini that is not `[MiSTer]`:
`video_mode` under `[<core name>]`. It is the only per-core key it writes, and it is
written with a section-aware rewriter of its own (`ini_rewrite_core()` in
`chome_ini.cpp`) rather than through the one everything else uses.

The distinction matters enough to state. `ini_rewrite_set()` sets a key **wherever it
appears**, deliberately — a setting this front-end has an opinion about is an opinion
about the machine, and one fixed in `[MiSTer]` and left wrong in a core section would be
silently overridden. A per-core setting is the opposite question: `video_mode` under
`[GBA]` must not touch the `video_mode` under `[MiSTer]`, because that one is the mode
every other core on the machine runs at.

Everything else is unchanged: the file is copied through byte for byte, `MiSTer.ini.bak`
is written first, and the new file is written beside the old one and renamed over it.
Two behaviours worth knowing:

- A `[<core>]` section the player already wrote gets the line **inside** it, not a second
  section underneath.
- "Automatic" **removes** the assignment rather than emptying it. An empty `video_mode=`
  is not "unset" to `cfg.cpp` — it is a parse failure, and `store_custom_video_mode()`
  answers a parse failure with mode 8 or 0. Written that way, "back to automatic" would
  mean "1080p for ever on this core".

The trap this cannot do anything about is the one `CLAUDE.md` describes: a `[video=WxH]`
section further down the file still outranks a `[<core>]` section above it.

---

## Workarounds and hacks, in one list

Ordered by how much they would surprise someone reading the code cold.

1. **The freeze.** There is no pause command in MiSTer. `sys_top` drives `osd_status`
   from whether the OSD is open, so a core that "pauses" only pauses while the classic
   OSD is up — which is exactly what the front-end replaces. On a core with save
   states, the game is instead held still by *saving a state on menu open and loading
   it on close*. This is a workaround for a missing feature, and it is the single
   most invasive idea in the project.

   It costs: a reserved slot the player never sees (the core's last one), a state
   write every time the menu opens, and exposure to a core bug — **the SNES core dies
   if asked for a state during a demanding scene**, which happens equally from
   MiSTer's own Alt-F1. `classicui_freeze=0` opts out.

2. **The analog takeover** (§2). Changing the machine's video mode so a UI can be
   seen is not a small thing to do behind the user's back.

3. **The pad gate** (§3). Widening a condition in `joy_digital()` that upstream wrote
   assuming the OSD was the only consumer.

4. **A magic byte instead of a version.** The SNAC reader replies with `0x4A` in the
   high byte so a core built before the feature reads as "not supported" rather than
   returning garbage. The firmware and the core framework are separate repositories
   and cores embed their own copy of `sys/`, so there is no version to check.

5. **Cores identified by name.** Several logically distinct cores share
   `CORE_TYPE_8BIT` and are told apart by name (`is_minimig()`, `is_menu()`). The
   front-end has to do the same, and additionally resolve regional core filenames —
   a card may carry `MegaDrive_*.rbf` where the table says `Genesis`.

6. **`bt_pad_label()` and vendor IDs.** Over Bluetooth a DualShock 4 announces itself
   as `Wireless Controller` and a Switch Pro as `Pro Controller` — no maker in either.
   The button set is resolved from the USB vendor id first (`054c`, `057e`, `045e`),
   and only then from the name.

7. **The picture cache cannot trust `stat`.** A slot's picture is rewritten in place
   whenever its state is. Size and mtime are not enough to notice: two frames of one
   game compress to a similar size, and FAT timestamps are granular to two seconds.
   Whoever rewrites one calls `art_forget()`.

8. **`Menu` is not in a folder.** The menu core lives at the card root as `menu.rbf`
   while every other core is under `_Console`/`_Computer`/etc.

9. **`MiSTer.ini` is CRLF.** The settings writer preserves the existing line ending
   and backs up to `MiSTer.ini.bak` — deliberately *not* `MiSTer_backup.ini`, which
   `cfg_get_name()` would offer as a bootable alternate config.

10. **240p drops the Display screen.** `mb_visible()` hides it at `PROF_LO`, because
    the filter preview tiles would be about 66px wide — too small to judge a filter by.
    This is a deliberate omission, not a bug.

---

## Video outputs

### How MiSTer drives the two outputs

There are two, and in normal operation **both are live at once**:

- **HDMI** always carries the **scaler output** — core video scaled, filtered, gamma
  and shadow-mask applied, with the HPS framebuffer composited on top.
- **The analog port** (VGA/SCART/component) carries, depending on configuration:
  - `vga_scaler=0` *(default)* — **raw, scandoubled core video**, straight from the
    core, bypassing the scaler entirely;
  - `vga_scaler=1` — the **same scaler output** HDMI gets;
  - `direct_video=1` — raw core timing straight out the DAC, no scaling at all, for
    RGB/SCART CRTs.

So yes: HDMI and analog work simultaneously, and that is the normal case. What they
cannot do is carry *different video modes* — there is one scaler and one PLL.

### What that means for the front-end

`video_scaler_is_visible()` encodes the consequence:

```c
if (cfg.direct_video) return 0;     // scaler bypassed entirely
if (cfg.vga_scaler)   return 1;     // analog carries the scaler output
return video_hdmi_connected();      // otherwise only HDMI shows it
```

| Setup | Where the UI appears | Canvas | Takeover |
|---|---|---|---|
| HDMI attached | HDMI, composited by the FPGA | 1280×720 | no |
| HDMI + CRT, `vga_scaler=0` | HDMI only; the CRT keeps raw core video | 1280×720 | **no** — deliberately |
| Analog only, `vga_scaler=0` | analog, after the takeover | 320×240 | yes |
| Analog only, `vga_scaler=1` | analog, no takeover needed | as configured | no |
| `direct_video=1` | not composited; the classic OSD is left to it | — | never |

**The one case people find surprising** is the second row. With both displays
connected, the front-end appears on HDMI and the CRT keeps showing the game. It is
not a failure to detect the CRT — taking the analog output would force *both* outputs
to 240p. If you want the UI on the CRT in that setup, either unplug HDMI or set
`vga_scaler=1`.

This was observed live during testing: switching the HDMI screen off flipped the log
from `hdmi=1 held=0` to `hdmi=0 held=1`, and the canvas from 1280×720 to 320×240,
with no configuration change.

`direct_video` and `vga_scaler` both disable the takeover outright
(`vga_fb_takeover_update()` returns immediately) — under `direct_video` there is no
composited framebuffer to show, and under `vga_scaler` none is needed.

---

## Performance

### How a frame is produced

Drawing goes into a software compose buffer in **cached** RAM, one `uint32_t` per
pixel. On present, the union of this frame's damage and the previous frame's is
copied into the **uncached** HPS framebuffer and the pair is page-flipped. The
previous frame's damage is included because the two buffers alternate, so the one
about to be filled is two frames stale.

Two properties follow, and they matter more than any single number:

- **Rendering is on demand.** `render()` runs only when something set `dirty` — a
  keypress, an animation step, or a cover-art decode completing. An idle shelf costs
  nothing.
- **Cost tracks the damaged rectangle, not the canvas.** Damage is a single union
  rectangle, not a list, so a change in two far-apart corners costs the bounding box
  of both.

### What scales with resolution

The uncached copy is the expensive part, and it is linear in damaged pixels:

| Canvas | Pixels | Full-frame copy |
|---|---|---|
| 320×240 (analog takeover) | 76,800 | 307 KB |
| 640×480 | 307,200 | 1.2 MB |
| 1280×720 (HDMI) | 921,600 | **3.6 MB** |

A full-canvas redraw on HDMI moves roughly **twelve times** the bytes it does at 240p,
into memory that is not cached. Full redraws happen on: shelf scrolling (the whole row
moves), panel open and close, and the first frame after a canvas change.

### Layout profiles

Chosen from canvas width, or forced with `classicui_profile`:

| Profile | Trigger | Cards visible | Text scales |
|---|---|---|---|
| `hd` | width ≥ 900 | 5 | 3 / 2 / 2 |
| `sd` | width ≥ 480 | 3 | 2 / 1 / 1 |
| `lo` | below that | 3 | 1 / 1 / 1 |

`hd` shows five cards rather than three, so a scroll damages more area *and* there is
more of it to damage. Text scales are integer only — the ROM font is never resampled.
`hd` also skips the overscan inset, since that canvas only arises on an HDMI display,
which shows the signal 1:1.

### The other costs

- **Cover art** is decoded one image per frame (`art_step()`), never in a batch, so a
  shelf full of missing art degrades into a slow fill rather than a stall. The cache
  is capped at **24 MB**; decoded size follows the card size, so `hd` cards cost more
  per entry and the cap holds fewer of them.
- **The library index** is built once and cached to `classicui/index.bin`. On the test
  machine that is **1430 items**, and later boots log `index cache hit`. The first
  boot after adding games pays the scan; the in-game menu shows "scanning" briefly on
  the first open after a core switch, because the index is rebuilt sliced across
  frames rather than blocking.
- **Slot pictures** are decoded through the same 4-entry thumbnail cache as cover art,
  and the filter previews are computed in software per preset.

### What has not been measured

Frame times on hardware. Everything above is the shape of the cost — bytes moved,
where the caches are, what triggers a full redraw — and the resolution table is
arithmetic, not a benchmark. The front-end is responsive at both 240p and 720p on a
DE10-Nano in normal use, but no frame-time figures have been taken, and the HDMI path
in particular has had far less hands-on time than the analog one.

---

## Testing

`support/classicui/test/` compiles the `chome_*.cpp` files **unmodified** against the
real project headers, linked against stubs for the FPGA, SD card and clock. It renders
every screen to PNG and asserts on the result. The count is printed at the end of the run;
quoting it here only produces a number that is wrong a week later.

What it cannot prove: framebuffer timing, the uncached-memory cost, SPI behaviour, or
whether a core accepts an MGL. Those need the hardware, and several bugs in this
project were only ever found there — the still-playing banner overflowing a 240p line,
and two Bluetooth pads getting the wrong button glyphs, among them.
