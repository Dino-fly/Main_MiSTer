# Classic Home v5 **beta** — physical CDs

**This is a beta, for testing.** It has had more hardware testing than any previous
release in one area and almost none in another, and the notes below say which is which.
If you want a quiet life, stay on v4.

Everything is one file, as before: copy `MiSTer` to the root of the SD card. There is
one optional second file this time — see *Disc titles* below.

![The disc dialog](https://raw.githubusercontent.com/Dino-fly/Main_MiSTer/disc-shelf/support/classicui/docs/img/disc-dialog-art.png)
![Suspend points for a disc](https://raw.githubusercontent.com/Dino-fly/Main_MiSTer/disc-shelf/support/classicui/docs/img/disc-suspend-points.png)

---

## Physical CDs

Put a game CD in a USB optical drive and the shelf notices it. The disc gets its own
screen — press up from the shelf — with the game's name, its artwork, and two buttons:
**Play**, and **Options** to send it to a different core.

Four cores read a disc directly: **PlayStation, Mega CD, PC Engine CD, Neo Geo CD**.
Only PlayStation has been tested on real hardware. Set `classicui_disc=1` to enable any
of it; it is off by default.

**A disc is a game like any other now.** It has its own save states, its own memory
card, and its own per-game core settings, all filed under the disc's identity rather
than shared between every disc you own. Save a state in Metal Gear Solid, come back to
the shelf, and the disc's suspend points are there with your screenshot in slot 1.

**The name and the artwork.** A pressed disc has no filename, so the front-end reads its
serial off the disc — `SLES-01506` — and looks that up. The name comes from a table on
the card; the artwork is fetched from ScreenScraper, sized once, and cached. Both arrive
while the disc spins, before you open the screen, so it is ready when you get there. If
the picture lands late it appears by itself.

### Artwork from ScreenScraper

Off by default. To switch it on you need **your own free ScreenScraper account** — the
application is registered already, but the per-user request quota is tied to the person
using it, so an account of your own is what gets you a usable allowance instead of
sharing the guest pool. In `MiSTer.ini`, under `[MiSTer]`:

```
classicui_screenscraper=1
classicui_ss_user=yourname
classicui_ss_pass=yourpassword
classicui_artfetch=1
```

Two things to know. The password sits in `MiSTer.ini` in clear text, as MiSTer options
do. And one request at a time is the rule — the front-end never fetches in parallel, so
art fills in gradually rather than all at once, and it never blocks the menu.

**Swapping discs mid-game** works on Mega CD and Neo Geo CD.

### Disc titles

Copy `disctitles.txt` from this release to `/media/fat/classicui/disctitles.txt` and
discs show their real names instead of a serial. Without it everything still works — you
just see `SLES-01506` where a name would be.

It covers 12,761 releases across the four systems, built from Redump's own DAT files. To
build a fresher one yourself: `support/classicui/tools/disctitles.py --fetch`. Note the
`/serial` suffix on Redump's URLs is mandatory — without it the DAT contains no serials
at all and the table silently matches nothing.

---

## Everything else

**A core setting for the whole system, from inside a game.** In the core options screen,
changing a value still applies to that game only. **Y** now promotes the setting under
the cursor to every game on that core. It writes only that one option — the other
overrides the running game happens to have stay its own, which is what made this
impossible before.

**Bigger save-state pictures.** The suspend strip was using about 60% of its width and
leaving a third of its height empty. The tiles are now roughly two and a half times the
area, and long game names are no longer cut short.

**The disc icon** is drawn at the screen's resolution rather than as a 32-cell sprite, so
at 720p it is a smooth 480-pixel disc instead of chunky blocks — and it breathes when it
has focus. When real disc artwork is available it replaces the drawn disc at the same
size, with the same edges, so the two do not clash.

**Analog / CRT output.** Two real bugs fixed: a scandoubler setting meant for a VGA
monitor was forcing a 31 kHz signal at 15 kHz televisions (no lock at all, which looks
like a scrambled mess), and the layout was computed for the wrong canvas shape on
640×240 outputs, overlapping the button bar. That second fix also covers `direct_video`,
where nobody had looked. **Options ▸ Best Settings** now tells you what is wrong with an
analog setup instead of leaving you guessing — see the *Known limitations*.

**Setup help.** Best Settings now also checks that the front-end is switched on in the
right section of `MiSTer.ini` — a `classicui=0` left in a core or video section silently
turns it off for that one game while the shelf looks perfectly normal. The guide covers
analog output, the disc options and the title table.

**A frozen console, fixed.** The disc detection helper was polling the drive underneath a
running game, which is the documented cause of two frozen consoles during development.
It now stays out of the way once a core owns the drive.

---

## Known limitations — please do not report these

- **On S-Video and composite the menu is black and white.** Games keep their colour. The
  colour encoder in the shared FPGA framework sits only on the core's video path, not the
  one this menu goes out through, so it cannot be fixed in firmware — it needs a change to
  `sys/sys_top.v` and every core rebuilt. **RGB SCART and YPbPr are in full colour.**
- **With an HDMI display attached, the front-end deliberately leaves the analog output
  alone**, so a CRT shows the game rather than the menu. Unplug HDMI to see the menu on a
  CRT. This is by design, not a fault.
- **No still picture behind the in-game menu over a disc game** — you get black instead of
  the paused game. Cosmetic; diagnosed, not yet fixed. File-launched games are fine.
- Mega CD, PC Engine CD and Neo Geo CD disc playback is **untested on hardware** — no
  media here to test with. It may not work at all.

## What we need tested

Most valuable first.

1. **Analog output on a CRT.** This is the big one — the fixes above are reasoned from the
   source and *nobody has seen them on a television*. With HDMI unplugged: does the menu
   appear, and does it hold sync? Try `forced_scandoubler=1` (should now still be a 15 kHz
   picture) and `menu_pal=0` versus `1` (which holds on your set?). If you use
   `vga_scaler=1` or `direct_video=1`, are the cards roughly square with nothing
   overlapping the bottom bar?
2. **Mega CD, PC Engine CD or Neo Geo CD discs**, if you own any. Does a disc identify,
   launch, and play? Does swapping discs mid-game work?
3. **Core settings promoted with Y** — set an option for one game, promote it, switch to
   another game on that core, and confirm it took. Then confirm the first game's *other*
   overrides did not follow it.
4. **Save states on a disc game**, including starting a disc at a suspend point from the
   shelf.

## Reporting a bug

Please include:

- **Your `MiSTer.ini`** — most of what goes wrong is configuration, and this is the single
  most useful thing you can send.
- **How your display is connected** — HDMI, RGB SCART, S-Video, composite, YPbPr, VGA — and
  whether the set is PAL or NTSC.
- **What you saw**, in the words you would use to describe it. "Scrambled black and white
  with a rolling image" is a better bug report than "video is broken".
- **`/tmp/debug.txt`**, after setting `debug=2` in `MiSTer.ini` and reproducing the problem.
  That file names what the front-end was doing and is often decisive.
- Whether it also happens with `classicui=0`. If it does, it is not this front-end.

## If it goes wrong

Keep your previous `MiSTer` binary and copy it back — the firmware is one file, so
rolling back is one copy. Or set `classicui=0` in `MiSTer.ini` to get the stock menu back
while keeping this firmware.
