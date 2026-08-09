# PSX controllers over SNAC — in every core, and in the menu

Plug a real PlayStation controller into the user port through a PSX SNAC
adapter and use it **everywhere**: in the MiSTer menu, and in any core built
with this framework change — not just the PSX core.

> **Unofficial build.** This is not part of official MiSTer. It changes the
> main firmware and requires cores rebuilt against a modified framework.
> Back up what you replace.
>
> Developed with **Claude Code** assistance. Confirmed working on a
> **SuperStation One** with a PSX pad over SNAC: menu navigation plus the
> NES, SNES, Game Boy, GBA and MegaDrive cores.

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

The archive unzips to a folder called **`SD-CARD-ROOT`** whose contents mirror
your MiSTer's SD card. Installing is copying that folder's *contents* onto the
card and letting it merge.

**1. Power the MiSTer off and put its SD card in your computer.** The card root
is the top level, where you can see `_Arcade`, `_Console`, `games`, `config` and
a file called `MiSTer` with no extension.

**2. Back up the two files you are replacing.** In the card root, copy `MiSTer`
and `menu.rbf` and rename the copies to `MiSTer.backup` and `menu.rbf.backup`.
That is your way back.

**3. Copy everything inside `SD-CARD-ROOT` into the card root** and confirm the
merge when your computer asks. The folders line up with the ones already on your
card:

```
SD-CARD-ROOT/
  MiSTer                    replaces the firmware in the card root
  menu.rbf                  replaces the menu core
  _Console/*.rbf            console cores
  _Computer/*.rbf           computer cores
  _Arcade/cores/*.rbf       arcade cores
  _Other/, _Utility/        the rest
  Scripts/                  optional clean-up script (see below)
  README.md, MANIFEST_*.txt documentation
```

Only `MiSTer` and `menu.rbf` overwrite anything. The cores are named with
today's date (`SNES_20260731.rbf`), so they sit **alongside** whatever you have
rather than replacing it — nothing of yours is lost.

**4. Add one line to `MiSTer.ini`** in the card root, using any plain-text
editor:

```ini
snac_pad=1
```

The archive deliberately does not include a `MiSTer.ini`, because overwriting
yours would wipe your settings.

**5. Put the card back**, power on, and plug in the PSX SNAC adapter and pad.

### Tidying up the duplicates (optional)

Because the new cores carry a datecode, you will now have two entries for some
systems — your old `SNES_20240101.rbf` and the new `SNES_20260731.rbf`. To keep
only the new ones, open the MiSTer's **Scripts** menu and run
**`snac_remove_old_cores`**. It deletes older copies of the cores this package
installed and touches nothing else.

Prefer to do it by hand? Just delete the older-dated `.rbf` files. Or leave them
— both work, you simply see two entries.

> **macOS:** eject the card properly before pulling it out, or the writes may
> not be flushed.
>
> **Any OS:** the firmware file must end up named exactly `MiSTer` — no `.bin`,
> no `(1)`. Some browsers and unzip tools rename things.

If you would rather copy over the network to a running MiSTer, the same layout
maps onto `/media/fat/`, and the firmware needs `chmod +x /media/fat/MiSTer`.

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

## Troubleshooting

### `snac_pad: unknown option` at boot or when a core loads

**The old firmware is still on the card.** That message is MiSTer telling you it
does not recognise the `snac_pad` setting — which only the new firmware
understands. So it proves step 2 has not taken effect. Cores are not involved.

The fix is the same whatever the cause: **copy `MiSTer` from the archive's
`SD-CARD-ROOT` folder into the card root again**, replacing what is there, and
reboot. Copying it twice does no harm.

Why it usually happens:

1. **Only the cores were copied.** Easy to miss, because the cores are the bulky
   part — but the firmware is the piece that makes the pad appear at all.
2. **An updater put the official one back.** `update_all.sh`, the MiSTer
   Downloader and similar tools replace the `MiSTer` file with the official
   release. If you run one of those, copy this firmware in again afterwards.
   This is the usual explanation when it worked for a while and then stopped.
3. **The file is in the wrong place or renamed.** It must sit in the card root
   as `MiSTer` — not inside a folder, not `MiSTer.bin`, not `MiSTer (1)`.

The error is harmless in itself: MiSTer skips the line it does not understand
and carries on booting.

### The pad works in the menu but not in a game (or the other way round)

The menu is driven by the firmware; each game core is its own `.rbf` file. If
the menu responds but a game does not, that core has not been replaced with one
from the archive. If nothing responds anywhere, see above — the firmware is the
common factor.

### An arcade game says the core is missing

Arcade `.rbf` files must keep the plain name the `.mra` expects —
`ActFancer.rbf`, not `Arcade-ActFancer.rbf`. Archives downloaded before
31 July 2026 had this wrong; re-download if yours contains `Arcade-` prefixed
files.

## Reverting

Put the card back in your computer, rename `MiSTer.backup` to `MiSTer`
(replacing the new one) and `menu.rbf.backup` to `menu.rbf`, then delete the
`snac_pad` line from `MiSTer.ini`. The cores do nothing without the firmware,
so you can leave them where they are.

## Source

- Firmware: [`Dino-fly/Main_MiSTer`](https://github.com/Dino-fly/Main_MiSTer/tree/snac-pr),
  branch `snac-pr` — `snacpad.cpp`, `snacpad.h`, plus the hookup in
  `user_io.cpp`, `cfg.cpp`, `cfg.h`, `MiSTer.ini`.
- Framework: [`Dino-fly/Template_MiSTer`](https://github.com/Dino-fly/Template_MiSTer/tree/psx-snac),
  branch `psx-snac` — `sys/psx_snac_pad.sv`, `sys/sys_top.v`, `sys/sys.qip`,
  and a simulation testbench.

Cores are built by copying that `sys/` change into each core's own `sys/`
folder and compiling with Quartus 17.0.2 — no per-core source changes.

**Want to build it yourself?** [BUILDING.md](BUILDING.md) is a step-by-step
guide for people who have never compiled an FPGA core, including the patch
script (`tools/patch_sys.py`) that applies the change to any core.

## Status and provenance

The RTL and firmware were written with **Claude Code** assistance: the pad
protocol engine passes a simulation testbench, the cores build with zero
errors and meet timing, and the whole feature costs about 118 logic modules
(~0.3% of the FPGA).

**Confirmed on hardware** (SuperStation One, PSX pad via a SNAC adapter):

| Tested | Result |
|---|---|
| Menu core — OSD navigation | works |
| NES | works |
| SNES | works |
| Game Boy | works |
| GBA | works |
| MegaDrive | works |

That covers the headline capability — driving the MiSTer menu with a PSX pad,
which was not possible with SNAC before — and in-game input across five cores.

It remains far less exercised than anything in official MiSTer: one board, one
adapter, six of the 274 cores here. The rest are built the same way from the
same framework change, so they are expected to behave identically, but they
have not each been verified. Keep the backups.

Most useful things to report:

1. Does the pad appear and drive the **menu**?
2. Which cores did you try, and did input work in-game?
3. Are the buttons where they should be?
4. DualShock: do the sticks work after pressing ANALOG?
5. Unplug and replug — does it recover?
6. With `snac_pad=0`, does everything behave exactly like stock?

If a pad is detected but erratic, mention your adapter model — pull-up values
differ between them, and the protocol timing is the likely suspect.
