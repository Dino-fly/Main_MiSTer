# Task 25 — what is left for a pair of eyes and a controller

Both halves are built, the harness is at zero failures across 3,238 checks, and a
device session on 2026-08-18 settled everything a script can settle. This is the
remainder: four things that need somebody in front of the machine. Each says what
to do, what to look at, and what to report.

The session that produced the rest is written up at the bottom, including what it
found — two bugs the harness could not have caught on its own.

---

## 1. Does a cheat actually change the game? *(half one's one real gap)*

**Proven already, from here:** the pack loads (`cheats: 25087` on Fire Emblem),
our screen switches an entry and the firmware pushes the buffer to the core
(`Cheat codes: 1`, then `Cheat codes: 0` on the way back). That is upstream's
`cheats_send()` doing exactly what the classic OSD makes it do.

**Not proven:** that the core, holding those codes, changes the game. It is the one
hop past everything this change touches, and reaching a screen where the effect is
visible means navigating a game past its title — which from a script is a synthetic
keyboard pressing Start into an attract loop. Four attempts on Metroid all landed
back on the title. With a pad in your hand it is two minutes.

| Step | You do | You look at | Report |
|---|---|---|---|
| 1.1 | Start a game whose pack has an obvious cheat — Sonic Advance's "Infinite Rings", Metroid's energy, anything with a number on the HUD | The HUD | What it reads normally |
| 1.2 | Menu → Options → Cheats, switch that cheat on, close the menu | The same HUD | **Did it change?** |
| 1.3 | Menu → Cheats, switch it off, close the menu | The same HUD | Does it go back? |

Both directions, because half the toggles that have shipped in this project were
exercised in one.

---

## 2. Is the scale really integer? *(half two's whole point)*

This is the thing the beta report asked for and the only question no measurement
answers: `vscale_mode=1` is MiSTer's default and means "integer scale only", and a
480-line mode is three whole 160-line GBA frames — but whether the picture looks
right is a picture.

**You will need `vga_scaler=1`, or an HDMI display.** On the machine as it stands
(analog only, `vga_scaler=0`) the scaler output reaches nothing and the screen
correctly refuses — see §3 of the session notes.

| Step | You do | You look at | Report |
|---|---|---|---|
| 2.1 | Start a GBA game. Menu → Options → Video Mode | The footer on each row | Does it say "3x of this core's 160 lines" on 720x480? |
| 2.2 | Press A on 720x480 | The screen | Does the countdown appear? Does the display lock to the mode? |
| 2.3 | Press A again to keep it | The game | **Is the picture an exact 3x, filling the height with no bars?** |
| 2.4 | Close the game, start it again | The game | Does it come up in that mode on its own? |
| 2.5 | Menu → Options → Video Mode → Automatic | The game | Does it go back? Is the `[GBA]` line gone from MiSTer.ini? |

---

## 3. Does the countdown save you? *(the thing that makes this safe to ship)*

Deliberately choose a mode the display cannot show, and do nothing.

| Step | You do | You look at | Report |
|---|---|---|---|
| 3.1 | On a display that will not take 1920x1080 (a CRT through vga_scaler), pick it and press A | The screen | It should go black or lose sync |
| 3.2 | **Press nothing for fifteen seconds** | The screen | Does the picture come back on its own? |
| 3.3 | Afterwards | `MiSTer.ini` | Is there no `video_mode` under that core's section? |

If 3.2 fails, that is a stop-ship: nothing else in the design protects somebody who
cannot see the menu.

---

## 4. A pad, not a keyboard

Every press in the session was `key <name>` down `/dev/MiSTer_cmd`, which travels the
front-end's own key path. A controller reaches it through `input.cpp` instead, and a
keyboard-driven pass has hidden a total pad failure in this project before.

| Step | You do | You look at | Report |
|---|---|---|---|
| 4.1 | With a pad: Options → Cheats, walk the list, open a group, switch a code | The screen | Does everything answer the pad? |
| 4.2 | Press **X** on the cheats list | The footer | Does it say the set is kept? Does X again arm a forget? |
| 4.3 | Options → Video Mode, press A on a mode | The legend | Does it read "Keep This" and "Put Back" while the countdown runs? |

---

## What the session of 2026-08-18 settled

Build `ec6aef9218955dc993cd25abb5676f6a`, deployed and md5-checked. The old firmware
is kept on the card as `MiSTer.before-t25` and can be deleted whenever you like.
`MiSTer.ini` was compared byte-for-byte against its backup afterwards and is unchanged.

**Confirmed on hardware:**

- The Cheats row appears on a game whose pack matched, and not otherwise.
- The fold works at real scale: Fire Emblem's 25,087 entries came out as **367
  groups**, the same number the host model computes from the same zip.
- A folded row opens, and its codes are labelled with the pack's own numbers in the
  order the store really sorts them — `Code 1, Code 10, Code 100, Code 101 …`, with
  the unnumbered one last. That ordering is a consequence of sorting names with
  `.gg` still on them, and it is now pinned by the harness because the device
  agreed with it.
- Toggling drives upstream's send path, both directions.
- The Video Mode screen refuses on a machine where the setting would do nothing,
  and says which of the two reasons applies.

**Two bugs the device found that the harness had not:**

1. `vm_supported()` was `cfg.direct_video ? 0 : 1`, which said *yes* on an
   analog-only machine — where the scaler output reaches no screen at all. It would
   have previewed a mode nobody could see, counted down against it, and written the
   setting on a confirmation that meant nothing. It is now
   `video_scaler_is_visible()`, and the harness covers both refusals.

2. The footer chose between a roomy wording and a terse one with a column count, and
   column counts move with `classicui_tracking` — which the harness runs at 0 and the
   television runs at −1. "Nothing shows the scaler - try vga_scaler=1" was drawn as
   "…try vga_sc…" on the one screen whose job is naming the setting to change. The
   choice is now made by measuring the string (`fit2()`), and the harness runs both
   new screens across the tracking range at 240p.
