# Classic Home v5 **beta 2** — physical CDs, and a much lighter menu

**Still a beta.** Beta 1 shipped with two credential-handling bugs and a menu that
repainted the whole screen to move one card; both are fixed here. If you ran beta 1,
replace it. If you want a quiet life, stay on v4.

**Installing.** The archive unzips to a folder called `SD-CARD-ROOT` whose contents mirror
your card. Copy that folder's *contents* into the card root and let it merge:

```
SD-CARD-ROOT/
  MiSTer                     replaces the firmware in the card root
  classicui/disctitles.txt   the disc name table - a new file, replaces nothing
```

`MiSTer` is the only thing that overwrites anything, so back it up first — copy it beside
itself as `MiSTer.backup`. That is your way back.

![The disc dialog](https://raw.githubusercontent.com/Dino-fly/Main_MiSTer/disc-shelf/support/classicui/docs/img/disc-dialog-art.png)
![Suspend points for a disc](https://raw.githubusercontent.com/Dino-fly/Main_MiSTer/disc-shelf/support/classicui/docs/img/disc-suspend-points.png)

---

## Physical CDs

**This is built on [Anime0t4ku](https://github.com/Anime0t4ku)'s work.** Reading a real CD
on a MiSTer at all comes from
**[Anime0t4ku/Main_MiSTer_Physical_Disc](https://github.com/Anime0t4ku/Main_MiSTer_Physical_Disc)**
— the streaming sector reader, the read-ahead worker, the drive-speed cap, the recovery
when a USB drive drops out, the disc swapping. That code is used here whole rather than
rewritten, GPLv3 as this tree is, because every part of it is subtle: the one time it was
paraphrased smaller during this work it cost two frozen consoles. What follows is a
front-end built on top of it, and none of it would exist without that author. Credit and
the full division of labour are in `support/physical_disc/CREDITS.md`.

Put a game CD in a USB optical drive and the shelf notices it. The disc gets its own
screen — press up from the shelf — with the game's name, its artwork, and buttons to
**Play** it or send it to a different core with **Options**.

Four cores read a disc directly: **PlayStation, Mega CD, PC Engine CD, Neo Geo CD**. Only
PlayStation has been tested on real hardware. Set `classicui_disc=1` to enable any of it;
it is off by default.

**A disc is a game like any other.** Its own save states, its own memory card, its own
per-game core settings, all filed under the disc's identity rather than shared between
every disc you own.

**The name and the artwork.** A pressed disc has no filename, so the front-end reads its
serial off the disc — `SLES-01506` — and looks that up. The name comes from a table on the
card; the artwork is fetched, sized once, and cached. Both arrive while the disc spins, so
the screen is ready when you get there.

**Swapping discs mid-game** works on Mega CD and Neo Geo CD.

---

## New in beta 2

### The menu got much lighter

Moving one card used to repaint the entire screen every frame of the animation. Now a slide
repaints only the band the cards occupy — and the game title and button prompts hold still
until the shelf settles or you let go of the arrow, so a fast scroll stops redrawing text
nobody can read yet.

Measured over a 200-frame held scroll. **These are host numbers from the test harness under
Docker, not from a DE10-Nano** — treat the ratios as real and the microseconds as
indicative. Rows is the figure that matters on hardware, where the copy goes into an
uncached FPGA-shared mapping and is charged by the row:

| Canvas | Rows repainted | Compose | Copy to screen |
|---|---|---|---|
| 320×240 | 240 → **96** | 116 → **93 µs** | 7 → **3 µs** |
| 1280×720 | 720 → **304** | 679 → **427 µs** | 100 → **39 µs** |

195 of 200 frames take the light path. The saving in *compose* is smaller than the row
count suggests, and that is expected: the cards are the expensive part of a frame and the
cards are what stays in the band.

**A decoded cover no longer forces a full repaint.** One cover finishes decoding per frame,
so scrolling into shelf you had not visited yet was forcing a whole-screen redraw on most
frames of the scroll. On the shelf a new cover can only change a card's face, so that is
all it asks for now.

**Two real bugs fell out of this.** The shelf was coming to rest with the selected card one
pixel short of full height — invisible until something else asked for a frame — because the
animation's final snap sat in a branch that only ran on the *next* pass. And the animation
advanced by how many times it had been called rather than by elapsed time, so composing one
instant twice moved the shelf between the two composes.

### The disc screen fits the screen

At 720p the disc dialog was covering 84% of the picture. It now takes **21%**: the disc is
two fifths of the canvas height, capped so 1080p stops growing, and the panel is only as
wide as its own longest line. At 240p it deliberately keeps the full width — there is not
enough room there for anything else — and that layout is byte-for-byte the one beta 1 had.

### A dark game no longer turns into a grey slab

The shade drawn behind a panel was a *floor*, not a dim: over a dark scene it forced half
the pixels to the same near-black value, so detail vanished rather than darkened. It now
multiplies, which keeps every ratio in the picture. Behind a panel the background is
slightly darker than before, so nothing drawn over it reads worse.

### Artwork, and your account in the UI

Covers are looked for on your card first, then **ScreenScraper**, then the libretro
thumbnail pack. In beta 1 ScreenScraper was only consulted for discs; it now supplies any
missing cover, and it is asked before the libretro pack rather than after.

You no longer have to edit `MiSTer.ini` to switch it on: **Options ▸ Online Covers** has the
on/off, your account name and your password, typed with the on-screen keyboard, and writes
them into `MiSTer.ini` for you (keeping a copy of the file first).

**Two credential bugs fixed.** ScreenScraper's media URLs embed *all four* secrets — the
application's and yours — and they were reaching the log and the on-disk cache. They are now
scrubbed from both. Note the password still sits in `MiSTer.ini` in clear text, as MiSTer
options do, so use one you do not use anywhere else.

You need **your own free ScreenScraper account** for this: the application is registered and
compiled in, but the request quota is per player, and the guest pool everyone shares is a
few requests a day for the whole world.

### Disc titles

Copy `disctitles.txt` from this release to `/media/fat/classicui/disctitles.txt` and discs
show their real names instead of a serial. Without it everything still works — you just see
`SLES-01506` where a name would be.

It covers 12,761 releases across the four systems, built from Redump's own DAT files. To
build a fresher one: `support/classicui/tools/disctitles.py --fetch`.

### Also

- **A core setting for the whole system, from inside a game.** In core options, changing a
  value applies to that game only; **Y** promotes the setting under the cursor to every game
  on that core, writing only that one option.
- **Bigger save-state pictures** — roughly two and a half times the area, and long game
  names are no longer cut short.
- **The disc icon** is drawn at the screen's resolution rather than as a 32-cell sprite, and
  breathes when it has focus.
- **Analog / CRT fixes**: a scandoubler setting meant for VGA was forcing 31 kHz at 15 kHz
  televisions, and the layout was computed for the wrong canvas shape on 640×240 outputs.
  **Options ▸ Best Settings** now tells you what is wrong with an analog setup, and checks
  that the front-end is switched on in the right section of `MiSTer.ini`.
- **A frozen console, fixed**: the disc detection helper was polling the drive underneath a
  running game.

---

## Known limitations — please do not report these

- **On S-Video and composite the menu is black and white.** Games keep their colour. The
  colour encoder in the shared FPGA framework sits only on the core's video path, not the
  one this menu goes out through, so it cannot be fixed in firmware — it needs a change to
  `sys/sys_top.v` and every core rebuilt. **RGB SCART and YPbPr are in full colour.**
- **With an HDMI display attached, the front-end deliberately leaves the analog output
  alone**, so a CRT shows the game rather than the menu. Unplug HDMI to see the menu on a
  CRT. By design, not a fault.
- **No still picture behind the in-game menu over a disc game** — you get black instead of
  the paused game. Mostly hidden by the disc screen itself now. File-launched games are fine.
- **Everything above 240p has been verified in the test harness only.** There is no HDMI
  display here, so the 720p and 1080p layouts — including the resized disc screen — have
  never been seen on a real panel. This is the single most useful thing to report on.
- Mega CD, PC Engine CD and Neo Geo CD disc playback is **untested on hardware**.

## What we need tested

Most valuable first.

1. **A 720p or 1080p HDMI display.** Does the disc screen look sensibly sized? Do panels,
   the shelf and the bottom bar line up, with nothing overlapping? Is scrolling smooth?
2. **Analog output on a CRT.** The fixes above are reasoned from the source and *nobody has
   seen them on a television*. With HDMI unplugged: does the menu appear and hold sync? Try
   `forced_scandoubler=1` (should still be a 15 kHz picture) and `menu_pal=0` versus `1`.
3. **Mega CD, PC Engine CD or Neo Geo CD discs**, if you own any. Does a disc identify,
   launch and play? Does swapping discs mid-game work?
4. **Fast scrolling.** Hold an arrow through a long shelf: any torn or stale rows, any
   smear at the top or bottom edge of the card band, any title that stays wrong after you
   let go?
5. **Core settings promoted with Y** — set an option for one game, promote it, switch to
   another game on that core, confirm it took, then confirm the first game's *other*
   overrides did not follow it.

## Reporting a bug

Please include:

- **Your `MiSTer.ini`** — most of what goes wrong is configuration. Remove your
  ScreenScraper password before posting it.
- **How your display is connected** — HDMI, RGB SCART, S-Video, composite, YPbPr, VGA — and
  whether the set is PAL or NTSC.
- **What you saw**, in your own words. "Scrambled black and white with a rolling image" is a
  better report than "video is broken".
- **`/tmp/debug.txt`**, after setting `debug=2` in `MiSTer.ini` and reproducing the problem.
- Whether it also happens with `classicui=0`. If it does, it is not this front-end.

## If it goes wrong

Copy your `MiSTer.backup` over `MiSTer` — that is the only file this release replaces, so
rolling back is one copy. `classicui/disctitles.txt` can stay; nothing else reads it. Or
set `classicui=0` in `MiSTer.ini` for the stock menu with this firmware.

**If you use `update_all`, read this.** It will replace `MiSTer` with the official build
and put ours in `.MiSTer.old`, silently reverting the front-end — the file is owned by the
official distribution database and no third-party database is permitted to supply it. Until
the installer in the next release handles this for you, after running `update_all` copy your
kept binary back. Your `MiSTer.ini` is never touched by it.
