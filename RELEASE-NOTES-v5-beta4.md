# Classic Home v5 **beta 4** — SNAC cores included, and a way to close a game

**Still a beta.** Beta 4 is beta 3's front-end with the SNAC-modified cores in the same
archive, a working **Close Game**, a faster menu on HDMI, and disc copying. If you run any v5
beta, take this one. If you want a quiet life, stay on v4.

## Installing

`ClassicHome-v5-beta4.zip` unpacks to a folder called `SD-CARD-ROOT` whose contents mirror
your card. Copy that folder's *contents* into the card root and let it merge:

```
SD-CARD-ROOT/
  MiSTer                            replaces the firmware in the card root
  menu.rbf                          replaces the menu core - new in beta 4
  classicui/disctitles.txt          the disc name table - a new file, replaces nothing
  linux/classic-home/               the update_all boot hook's worker
  Scripts/classic_home_protect.sh   installs the hook
  Scripts/classic_home_unprotect.sh removes it again
  _Console/ _Computer/ _Arcade/     271 cores rebuilt with PSX-controller-over-SNAC support
```

**Back up `MiSTer` first** — copy it beside itself as `MiSTer.backup`. That is your way back,
and it is one copy.

Then turn it on in `MiSTer.ini`, under `[MiSTer]`:

```
classicui=1
```

**The section matters as much as the line.** `MiSTer.ini` is divided into sections, and on a
card that has been in use a while the file usually *ends* inside a core or video section — so
adding `classicui=1` at the bottom scopes it to that one core and it looks like the release
simply does not work. Put it under `[MiSTer]`.

### Delete these six files before you merge

Six of the cores in this archive differ from the ones in earlier waves **only in letter case**.
A MiSTer card is case insensitive: copying `NeoGeo_20260731.rbf` over `Neogeo_20260731.rbf`
keeps the *old* spelling, and the core stays broken. The list is in `CLEANUP_wave3.txt` inside
the archive. Delete those files from the card first, then merge. This was proven on a real
exFAT card, not assumed.

## Surviving `update_all`

Unchanged from beta 3, and still the thing that matters most. Run once:

```
/media/fat/Scripts/classic_home_protect.sh
```

`update_all` replaces `MiSTer` with the official build and puts yours in `.MiSTer.old`,
silently reverting the front-end. The hook puts yours back on the next boot. It only acts when
the installed firmware is not one of ours, so a newer build you copied on by hand is left alone
rather than quietly downgraded. `classic_home_unprotect.sh` removes it.

## What changed since beta 3

### The SNAC cores are in this archive now

Beta 3 shipped the firmware and told you to fetch the cores separately. In practice that meant
people installed Classic Home, plugged in a PlayStation pad over SNAC, and found it dead —
because the cores on the card were the stock ones. **271 rebuilt cores ship here**, and the
front-end you install is the front-end that supports your pad.

**The PSX core is deliberately not among them.** SNAC on PSX has a problem nobody has looked
into yet, and shipping a broken PSX core would break disc playback — which does work. PSX stays
on the official build. Everything else on the card gets SNAC.

If you only want SNAC and not this front-end, the
[wave 3 release](https://github.com/Dino-fly/Main_MiSTer/releases/tag/snac-psx-pad-wave3) is
still the download for you; nothing there has changed.

### Close Game

There is now a **Close Game** row at the bottom of the in-game Options panel. Press it twice —
the row turns red and says so — and the core is put away.

It was written for beta 3 and **nobody on a 240p television could reach it**: the panel drew as
many rows as fitted and quietly dropped the rest, and Close Game was the eleventh of eleven. The
list scrolls now, with a bar down the side showing where you are, and the warning sentence under
it wraps instead of being cut off mid-word — that was clipped at *every* resolution, not only
the small one.

### A faster menu on HDMI

**Settings ▸ Menu Resolution**, new, and set to **Fast** out of the box.

Fast draws the menu into a canvas half as wide and half as tall, so a quarter of the pixels. The
**layout does not change** — a 720p display keeps its five-card shelf, same sizes, same
positions — so what Fast costs is sharpness and nothing else. Games are not affected in any way;
this is the menu's own canvas.

Choose **Sharp** if you have a big panel and prefer crisp text to a smooth carousel. On a 240p
television the setting does nothing at all, by design: the request is refused below 320×240, so
analog output is byte-for-byte what it was.

### The disc actually spins

The disc on the disc screen turned in 64 steps a turn and repainted about **16 times a
second** — which read as a stutter, not a spin. It now turns in 256 steps and repaints **62
times a second at every resolution**: a new angle on every single frame.

That is more drawing, not less, so it is paid for rather than wished for. A rotated disc is
expensive to compute, but a quarter turn is not — rotating about the point *between* pixels
rather than on one makes 90° an exact rearrangement of the same pixels. So one quadrant is
computed and the other three are quarter-turns of it, cached, and the cache is
**byte-for-byte identical** to computing every angle the slow way. That was checked, not
assumed, at every quadrant boundary.

**It gives the CPU back when something else needs it.** While a disc is being copied, the
library is being scanned, or cover art is still decoding, the disc steps only through angles
it already has cached — which cost nothing to draw. At the shipped default the whole turn is
cached, so there is nothing to give up and nothing changes. It goes back to full smoothness
within a frame or two of the other work finishing, not seconds later.

### Copy a disc onto the card

With a disc in the drive, the disc screen's **Options** now offers **Copy to PlayStation** (or
Mega CD, or whichever system the disc belongs to). It writes a `.cue` and its tracks into that
system's games folder in the layout the core expects, and the copy appears on the shelf.

While it runs, the disc art fills the dialog and un-dims like a pie as the copy advances, with
the percentage under it. Unreadable sectors are counted and reported rather than hidden.

The row is only offered for systems that have somewhere to put the copy. A disc whose system has
no games folder configured gets no row, which is a real answer rather than a copy that lands
nowhere.

### The CD consoles are systems of their own

Saturn, Mega CD, PC Engine CD and Neo Geo CD now appear in the systems list in their own right,
instead of being folded into their cartridge siblings. A folder of tracks — a `.cue` with its
`.bin` files beside it — is **one card on the shelf**, named after the folder, rather than a
shelf full of "Track 02" fragments.

### Browsing by letter

The shoulder buttons **L** and **R** jump to the previous and next first letter, not the
previous and next page. Pages were small and jumping by them was barely faster than walking; in
a 900-game library, getting to M now takes one press per letter.

### Typography

Three new rows in **Settings**, all live as you turn them:

- **Font** — pick a `.pf` font off the card, or Built-in. Same glyph table the stock OSD uses.
- **Letter Spacing** — −2 to +2. Positive spacing fits fewer words per line, and the help line
  says so, because several panels were written to exactly the width the stock font gives.
- **Capital Letters** — Off draws titles and labels as they are written. Worth having if you put
  a font on the card for its lowercase.

### One card per version

A title you own in several versions is one card wearing a **stack of frames** in its corner —
so you can see at a glance that there is more than one, and pick between them on the card
instead of hunting the shelf. A disc you copy merges into the card that was already there rather
than starting a second one.

Everything else from the
[beta 2 notes](https://github.com/Dino-fly/Main_MiSTer/releases/tag/classic-home-v5-beta2) still
applies — physical CDs, artwork from ScreenScraper, per-system core settings with **Y**. Read
those for what the front-end does.

## Credit

Physical CD playback is [Anime0t4ku](https://github.com/Anime0t4ku)'s work, from
[Main_MiSTer_Physical_Disc](https://github.com/Anime0t4ku/Main_MiSTer_Physical_Disc) — the
streaming sector reader, the read-ahead worker, the drive-speed cap, the recovery when a USB
drive drops out, the disc swapping. Used here whole rather than rewritten, GPLv3 as this tree
is. None of the disc support would exist without that author; see
`support/physical_disc/CREDITS.md`.

---

## Known limitations — please do not report these

- **SNAC on the PSX core does not work**, and that core is not in this archive for that reason.
  Every other core here has it.
- **On S-Video and composite the menu is black and white.** Games keep their colour. The colour
  encoder in the shared FPGA framework sits only on the core's video path, not the one this menu
  goes out through, so it cannot be fixed in firmware. **RGB SCART and YPbPr are in full
  colour.**
- **With an HDMI display attached the front-end deliberately leaves the analog output alone**,
  so a CRT shows the game rather than the menu. Unplug HDMI to see the menu on a CRT. Making
  that a choice is on the list, not in this build.
- **No still picture behind the in-game menu over a disc game** — you get black. Mostly hidden
  by the disc screen now. File-launched games are fine.
- **Everything above 240p is verified in the test harness only.** There is no HDMI display here,
  so the 720p and 1080p layouts have never been seen on a real panel. Still the single most
  useful thing to report — including whether **Fast** looks acceptable to you on a big screen.
- Mega CD, PC Engine CD and Neo Geo CD disc playback is **untested on hardware**. Saturn
  likewise.

## What was actually tested

**The front-end changes in this release are verified in the host test harness only** — 1875
checks, 0 failures. The device was switched off while beta 4 was assembled, so nothing here has
been seen on real hardware.

What *has* been on the device, from the beta 3 cycle and with the same code: the shelf with
cover art, the disc screen reading a real PlayStation disc, a real disc copied onto the card and
the copy launched. The photographs in
[GUIDE.md](https://github.com/Dino-fly/Main_MiSTer/blob/master/support/classicui/GUIDE.md) are
those runs.

The firmware in this archive is md5 `ea91d850`, and it carries the ScreenScraper application
credentials — a clean rebuild had silently dropped them, and the packaging tool now refuses to
build an archive whose firmware is missing them.

## Reporting a bug

Include your `MiSTer.ini` (**remove your ScreenScraper password first**), how your display is
connected and whether the set is PAL or NTSC, what you saw in your own words, and
`/tmp/debug.txt` after setting `debug=2` and reproducing. Say whether it also happens with
`classicui=0` — if it does, it is not this front-end.

## If it goes wrong

Copy your `MiSTer.backup` over `MiSTer`. `classicui/disctitles.txt` can stay; nothing else reads
it. Or set `classicui=0` for the stock menu with this firmware.
