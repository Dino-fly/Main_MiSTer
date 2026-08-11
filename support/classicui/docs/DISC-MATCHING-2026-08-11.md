# Matching a physical disc to cover art — what ScreenScraper actually accepts

Measured 2026-08-11 against the live API. 218 requests total, against a daily allowance of
20000 general and 2000 unmatched; the account ended the study at 180/20000 and 60/2000, so
under 1% and 3%. One request in flight at a time (`maxthreads` is 1 on this account), 1.5 s
apart, with an abort-on-refusal rule that never fired.

## Why the question is different for a disc

A file scrapes by its name, and Redump filenames are exact, so files were never the problem.
A disc in a tray has no filename. It has only what is written on it, so the question is which
of those strings a database will accept — and that had never been measured. The code carried
an honest note saying so: *"serialnum waits for a deliberate test rather than being switched
on speculatively"* (`chome_art.cpp`). This is that test.

## The sample

52 discs, read off the card with `discid.py`: **37 Saturn, 15 PlayStation.** Ground truth is
the Redump folder name, which is why the sample comes from files rather than from swapping
discs in a drive — 52 known-correct answers beats two.

**Mega CD, PC Engine CD and Neo Geo CD could not be tested at all.** Those folders on the
card hold only BIOS files, so there is no disc data on this machine to match. Everything
below about them is unmeasured, and is marked as such rather than inferred from Saturn.

## Results

Per disc, asking `jeuInfos.php` with the right `systemeid`:

| | Saturn (37) | PlayStation (9 with a serial) |
|---|---|---|
| `serialnum=` product number | 11 correct, 26 miss, **0 wrong** | **9 of 9 correct** |
| `romnom=` disc header title | **30 correct**, 7 miss, **0 wrong** | — |
| `romnom=` ISO volume label | 23 correct, 7 of them have no label at all | weak |
| `romnom=` product number | **0 correct** | **returns the wrong game** |

### The three findings that matter

**PlayStation: `serialnum` is the key.** Nine of nine, exactly right. The one my checker
scored as wrong — `SCES-01909` returning "Wipeout 3" for *Wip3out* — is the same game; the
checker could not see it, I could.

**Saturn: the disc's own header title is the key**, at 30/37 with not one wrong answer across
74 label queries. Cleaning the label up first made it *worse* (28), so it should go out raw.

**Never put a serial in `romnom`.** Asked for `SLUS-00594`, ScreenScraper returned
*Beyblade Burst — Battle Zero*: a real game with a real cover and nothing to do with Metal
Gear Solid. It does not miss, it answers confidently wrong. This is the failure mode worth
designing against, because nothing downstream can detect it.

## Three defects the study exposed

**1. No physical Saturn or Mega CD disc has ever produced a serial.** The helper called
`disc_serial_at()`, the PlayStation-only reader, instead of the dispatcher
`disc_serial_for()`. On a Saturn disc it correctly found no Sony prefix and wrote `""`.
`disc_saturn_serial_at()` and `disc_megacd_serial_at()` were both implemented, both
documented down to the six malformed Mega CD headers they cope with, and both never called
on the path that reads a real disc. Verified on hardware: the Sega Rally disc in the drive
carries `MK-81207` — `dd if=/dev/sr0` shows it — and the state file said the serial was empty.

**Why the suite could not catch it:** `disc_ingest_identify()`, the copy the harness can
reach, called `disc_serial_for()` correctly from the day it was written. The tested path was
right and the shipped path was not. The two are now marked as a pair that must change
together.

**2. The name came from the ISO volume id, which is the worse of the two strings.** On
Saturn it is missing outright on 7 of 37 discs — Daytona USA, Virtua Cop, Panzer Dragoon,
Myst, Bug!, Magic Knight Rayearth, Clockwork Knight are simply nameless on the shelf today —
and where present it is a filename: `B_RANGERS`, `S_BOMBERMAN`, `AZEL_1`,
`SEGARALLY_CHAMPIONSHIP` with the space missing. The header title is present on 37/37 and
reads like a title.

**3. Fixing (1) alone would have made Saturn art worse.** With a serial finally populated,
`disc_scrape_name()` took its serial branch, missed in the offline table — 12762 entries, 7
beginning `MK`, no Sega product numbers — and returned the bare product number as the search
name. That is the one string measured to *never* match. Caught before shipping only because
the numbers existed to check it against.

## What changed

- The helper dispatches on disc type, so Saturn and Mega CD serials are read.
- New `disc_title_at()`: the title a Sega disc writes in its own header, preferred over the
  ISO volume id on both identify paths.
- `disc_scrape_name()` reordered — offline table first, then the disc's own name, and the
  bare serial only as the last resort it always was.
- Confined to Saturn and Mega CD. **Neo Geo CD and PC Engine CD stay silent**, because they
  carry no product code and nothing here measured them; the existing test guarding their
  silence failed when I first widened the rule, which is the test doing its job.

Verified on hardware after a reboot: `2|2|MK-81207|SEGA RALLY CHAMPIONSHIP`, where it read
`2|2||SEGARALLY CHAMPIONSHIP` before. The art fetch itself fires when the disc dialog is
opened and was **not** observed end to end — what is established is that the name now sent
is the exact string measured as a correct hit.

## Still open, and they are decisions rather than work

- **The bare-serial fallback can return a wrong cover** (the Beyblade case). It only fires
  for a PlayStation disc the offline table does not know. The narrow fix is `serialnum`,
  measured at 9/9; the cheap fix is to drop the branch and show the generated disc face. Both
  beat a wrong cover.
- **`serialnum` for PlayStation** is now evidenced and worth switching on. Note it *forces*
  the search, so what it does to a `romnom` that would have matched still needs one test.
- **The 6 Saturn discs nothing found** are ones whose header title is not the retail name:
  `J:AZEL PANZER DRAGOON RPG` (Panzer Dragoon Saga, 4 discs), `SHINING FORCE 3 SCENARIO 1`,
  `CLOCKWORK KNIGHT Pepperouchau's`. A handful of overrides keyed on product number would
  finish the set — the serials are now readable, which is what made that possible.
- **Mega CD, PC Engine CD, Neo Geo CD are unmeasured.** One disc of each in the drive would
  settle them, and the Mega CD reader is written and now reachable.
