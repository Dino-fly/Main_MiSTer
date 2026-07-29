# PSX controllers over SNAC — in every core, and in the menu

Plug a real PlayStation controller into the user port through a PSX SNAC
adapter and use it **everywhere**: in the MiSTer menu, and in any core built
with this framework change — not just the PSX core.

> **Unofficial build.** This is not part of official MiSTer. It changes the
> main firmware and requires cores rebuilt against a modified framework.
> Back up what you replace.

## Why this didn't work before

SNAC is not an adapter system with any intelligence in it — it is seven raw
FPGA pins on a USB-shaped socket plus a passive cable per console. When a core
offers a SNAC option, it simply routes the *emulated console's own controller
pins* out to those wires, so an emulated SNES talks to a real SNES pad exactly
as 1990 hardware did. That is why it is so accurate, and why it is so rigid:

- The **SNES** pulses a latch line and clocks 16 bits out of a dumb shift
  register.
- A **PSX pad** expects a conversation: select it, send command bytes, and it
  answers byte for byte, acknowledging each one.

No pin rewiring reconciles those two protocols. And because the MiSTer menu is
drawn by Linux on the ARM side rather than by the core, anything that only
reaches the FPGA is invisible to the menu no matter how well it works in a
game.

## How this works

Translation happens in two places:

```
PSX pad ── PSX SNAC adapter ── user port
                                  │
                     psx_snac_pad.sv   (new, in the shared sys framework)
                     talks the real PSX protocol: selects a port, sends the
                     0x42 poll at 250 kHz, handles each ACK, decodes buttons
                     and sticks, detects unplugging
                                  │  internal bus, UIO command 0x45
                     snacpad.cpp      (new, in the firmware)
                     publishes the pad through Linux uinput as an ordinary
                     gamepad named "MiSTer SNAC Pad 1" / "…2"
                                  │
                     the normal MiSTer input pipeline
                                  │
              menu navigation · per-core button mapping · the core itself
```

Because the pad arrives as a regular input device, everything MiSTer already
does applies to it — menu control, the Define-buttons screens, saved per-core
mappings — and **no core needs a single line of its own code changed**. The
reader lives in `sys/`, the framework folder every core is built on, so a core
gains the feature simply by being rebuilt.

## Install

**1. Back up the firmware you are replacing:**

```
cp /media/fat/MiSTer /media/fat/MiSTer.backup
```

**2. Copy the new firmware** to the SD card root as `/media/fat/MiSTer`, then:

```
chmod +x /media/fat/MiSTer
```

**3. Copy the cores you want.** Keep your originals — these do not replace
them unless the filenames collide.

| Core type | Destination |
|---|---|
| Menu (`menu.rbf`) | `/media/fat/menu.rbf` — back up the existing one first |
| Console cores | wherever you keep them (`/media/fat/_Console/`, `_Computer/`, or the root) |
| Arcade cores | `/media/fat/_Arcade/cores/` — your existing `.mra` files find them automatically |

**4. Enable it** in `/media/fat/MiSTer.ini`:

```ini
snac_pad=1
```

- `1` — on, with **Select+Start** acting as the menu button.
- `2` — on, without that combo.
- `0` or omitted — off (the default).

Per-core sections work. To let a core keep the user port for its own native
SNAC passthrough:

```ini
[PSX]
snac_pad=0
```

**5. Reboot**, plug in the adapter and pad, and load one of the rebuilt cores.

## What you need

- A **PSX SNAC adapter** — the passive kind used by the PSX core, wired for
  two ports (ATT1/ATT2/CMD/CLK out, DAT/ACK in). Existing adapters work
  unmodified.
- A PSX or PS2 digital pad, or a DualShock. Press the pad's **ANALOG** button
  for stick support.

## Button mapping

Standard layout, so the built-in defaults apply: D-pad, Cross/Circle/Square/
Triangle on the usual South/East/West/North positions, L1/R1/L2/R2, Select,
Start, and both sticks on a DualShock. Remap per core through the normal
Define-buttons screens if you prefer something else.

## Limits — read before reporting a bug

- **No rumble.** SNAC adapters do not carry the 7.6 V motor supply.
- **No multitap.** Two ports only.
- **No light guns.** GunCon and Justifier need video-synchronous timing; use
  the PSX core's own SNAC option for those.
- **Latency is roughly 3–6 ms** — comparable to a good USB adapter and well
  under one 60 Hz frame, but *not* the near-zero of a native SNAC pairing. For
  a PSX pad in the PSX core with original timing, set `snac_pad=0` for that
  core and use its built-in SNAC option instead.
- **The user port is exclusive.** While enabled, a core's own user-port
  features (native SNAC, serial, MIDI) are unavailable — disable per core as
  shown above.
- **Only rebuilt cores gain the feature.** A stock core will simply ignore the
  pad; the firmware detects this and does nothing rather than misbehaving.

## Reverting

Restore `MiSTer.backup` over `/media/fat/MiSTer`, put your original
`menu.rbf` back, and remove `snac_pad` from `MiSTer.ini`. The cores are inert
without the firmware, so they can be left in place.

## Source

- Firmware: [`Dino-fly/Main_MiSTer`](https://github.com/Dino-fly/Main_MiSTer/tree/snac-pr),
  branch `snac-pr` — `snacpad.cpp`, `snacpad.h`, plus the hookup in
  `user_io.cpp`, `cfg.cpp`, `cfg.h`, `MiSTer.ini`.
- Framework: [`Dino-fly/Template_MiSTer`](https://github.com/Dino-fly/Template_MiSTer/tree/psx-snac),
  branch `psx-snac` — `sys/psx_snac_pad.sv`, `sys/sys_top.v`, `sys/sys.qip`,
  and a simulation testbench.

Cores are built by copying that `sys/` change into each core's own `sys/`
folder and compiling with Quartus 17.0.2 — no per-core source changes.

## Status

The design is simulated and compiles cleanly, and cores are built and timing-
clean, but it is **new and lightly tested on real hardware**. Most useful
things to report:

1. Does the pad appear and drive the **menu**?
2. Which cores did you try, and did input work in-game?
3. Are the buttons where they should be?
4. DualShock: do the sticks work after pressing ANALOG?
5. Unplug and replug — does it recover?
6. With `snac_pad=0`, does everything behave exactly like stock?

If a pad is detected but erratic, mention your adapter model — pull-up values
differ between them, and the protocol timing is the likely suspect.
