# PSX controllers over SNAC — wave 3 (273 cores)

Use a real PlayStation controller on a MiSTer through a SNAC adapter, in every core and in
the menu. Wave 3 rebuilds all 273 cores and **fixes five that never worked**.

If you are on wave 1 or wave 2, this is worth taking. If you are on wave 2, **read the
install order below before copying anything** — six of the fixes change only the letter case
of a filename, and on a MiSTer's card that means copying alone will not fix them.

## What is in this release

| Download | For |
|---|---|
| `snac-psx-pad-wave3-cores.zip` | the cores, and `menu.rbf`. Everyone needs this. |
| `SNAC-PSX-v3-firmware.zip` | the firmware, if you want SNAC **without** the Classic Home front-end |

If you want SNAC *and* Classic Home, take the cores archive from here and the firmware from
the [Classic Home release](https://github.com/Dino-fly/Main_MiSTer/releases) instead — that
firmware contains SNAC too. The cores archive is the same either way.

Both zips unpack to a folder called `SD-CARD-ROOT` whose contents mirror your card, so
installing is copying that folder's *contents* into the card root and letting it merge.

---

## Five cores that never loaded now do

A MiSTer arcade core is not chosen by name. The `.mra` file names a core, and the firmware
scans `_Arcade/cores/` for files matching it — then keeps the **lexicographically greatest**
filename. Two ways that goes wrong, and wave 2 hit both:

- **Nothing resolved them at all.** `ATetris`, `GundamSD` and `RushnAttack` were named after
  their source repositories, but their `.mra` files ask for `ataritetris`, `SDGundamPS` and
  `rshnatk`. No match, so those three games have been running the official build without SNAC
  since wave 2 went out, with no error anywhere to say so.
- **Case beat the date.** The comparison is case sensitive, so an upper-case letter always
  loses to its lower-case twin. `Freeze_20260731.rbf` lost to the official
  `freeze_20240526.rbf`, and `StarForce_20260731.rbf` to `Starforce_20260418.rbf` — newer
  builds, dead on the card.

Fixed:

```
AtariTetris_20260807.rbf     Atari Tetris
RshnAtk_20260807.rbf         Rush'n Attack
SDGundamPS_20260807.rbf      SD Gundam
freeze_20260731.rbf          Freeze
Starforce_20260731.rbf       Star Force
```

Three more were renamed for the same reason before they could bite: `SVI328` was losing to the
official `Svi328_20241016.rbf` on case, which would have made a `bootcore=Svi328` setting boot
the SNAC-less core, and `Dcon`, `Orao` and `MemTest` now follow the rule rather than winning by
luck.

**The naming rule, for anyone building their own:** copy the basename the *official
distribution installs*, in its exact case, and use a datecode strictly greater than upstream's.
Not the repository name, and not the Quartus revision — upstream renames the artifact after
compiling, and the distribution strips the `Arcade-` prefix when it installs.

## Twelve cores rebuilt against newer upstream releases

Arcadia, Asteroids, DECOCassette, Gaplus, Qix, SGB, SNK6502, UK101, Salamander, and the three
renamed above. Upstream had released newer versions of these, which would otherwise shadow the
SNAC build and quietly take the feature away.

---

## Installing — the order matters

**1. Delete the old files first.** `MiSTer_SAM`-style clean-up scripts cannot help here: they
remove names they installed, and none of these names existed before. The full list ships as
`CLEANUP_wave3.txt` in the archive. These six are the ones that *must* go before you copy:

```
_Arcade/cores/Dcon_20260731.rbf
_Arcade/cores/Freeze_20260731.rbf
_Arcade/cores/StarForce_20260731.rbf
_Computer/Orao_20260731.rbf
_Computer/SVI328_20260731.rbf
_Utility/MemTest_20260731.rbf
```

**Why this is not optional:** a MiSTer card is exFAT or FAT32, which is case insensitive.
Copying `freeze_20260731.rbf` onto a card that already holds `Freeze_20260731.rbf` overwrites
the contents but **leaves the directory entry spelled the old way** — so the file keeps losing
to upstream's `freeze_20240526.rbf` and the core stays exactly as dead as it was in wave 2.
Deleting first is the only order that works. This was confirmed on a real card, not assumed.

The other twelve entries in `CLEANUP_wave3.txt` are superseded datecodes and can go whenever
you like; leaving them costs disk and a duplicate in your core list, nothing more.

**2. Back up `menu.rbf`**, then copy the contents of `SD-CARD-ROOT` into your card root.

**3. Turn it on** in `MiSTer.ini` under `[MiSTer]`:

```
snac_pad=1
```

Select+Start acts as the menu button. See `SNAC_PSX_PAD.md` in the repository for the PSX-core
options (`snac_psx`, `snac_psx_fallback`, `snac_psx_memcard`).

## Surviving `update_all`

`update_all` replaces `MiSTer` and `menu.rbf` with the official builds and puts ours in
`.MiSTer.old`, silently removing SNAC support. That is not a bug in the updater: those two
paths belong to the official distribution database, and no third-party database is permitted to
supply them.

The firmware zip carries a fix. Run it once:

```
/media/fat/Scripts/classic_home_protect.sh
```

It stores your firmware where the updater may never write (`linux/`, which is on the
downloader's forbidden list for *every* database, official one included) and installs a boot
hook that puts it back if `update_all` has replaced it. `classic_home_unprotect.sh` removes it
again. Despite the name it protects either firmware, SNAC-only or Classic Home. Your
`MiSTer.ini` is never touched by `update_all`.

Your cores are safe from the updater — they have their own filenames, so nothing overwrites
them. Only the firmware and `menu.rbf` need protecting.

---

## Known limitations — please do not report these

- **`menu.rbf` overwrites the official one, and cannot be made to lose gracefully.** A core has
  a datecode to compete on; `menu.rbf` does not. If you remove ours, `update_all` puts the
  official one back and SNAC stops working *in the menu* while continuing to work in cores.
- **The firmware in this release has not been tested with a pad on this upstream base.** It
  merges 26 upstream commits that wave 2 did not have, so both published firmwares now sit on
  the same upstream code. The SNAC pad path itself is unchanged, but nobody has put a
  controller on the adapter and confirmed it since that merge. If you have an adapter, this is
  the single most useful thing you can report.
- Eleven console and computer cores appear twice in your core list because the official
  distribution installs them under a different word — `Intv`/Intellivision,
  `Minimig-AGA`/Minimig, `Genesis`/MegaDrive and so on. Both entries work; ours is the one with
  SNAC. Renaming them would collide with cores we already ship.
- `dorodon` has an `.mra` that no `.rbf` satisfies, ours or upstream's. That is a gap upstream,
  not something this release caused.

## Reporting a bug

Please say **which core**, **which adapter**, and whether the pad works in the *menu* as well
as in the game — those fail for different reasons. `snac_pad=1` in the `[MiSTer]` section
rather than a core section, please; a setting in the wrong section silently does nothing.

If a specific arcade game ignores SNAC, the likely cause is filename resolution rather than the
pad: send `ls /media/fat/_Arcade/cores/ | grep -i <game>` and we can tell immediately.

## If it goes wrong

Copy your backed-up `menu.rbf` and `MiSTer` back — that is the whole of it. Or set
`snac_pad=0` to keep this firmware and turn the feature off.
