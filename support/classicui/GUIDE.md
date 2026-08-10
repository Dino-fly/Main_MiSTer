# Classic Home — a guide

A game-first front-end for MiSTer. You get a shelf of your games with cover art,
save states you can see, and a menu you can reach from inside a game — without
meeting a single line of technical text.

It replaces the menu core's screen only. Everything MiSTer normally does still
works, and the classic menu is one deliberate choice away on **Options ▸ Advanced
Settings**.

All the pictures below are rendered by the test harness from the real UI code, so
they are exactly what the code draws. The section at the end is different: those are
photographs of the thing running, taken off a real MiSTer.

---

## On a real machine

Captured from a DE10-Nano over its analog output at 320x240 - which is why they are
small and why the Display screen is missing from them. See
[Systems that cannot save](#systems-that-cannot-save) and the note on 240p below.

| | |
|---|---|
| ![Shelf](img/device/shelf.png) | ![Shelf](img/device/shelf-zelda.png) |
| The shelf, with cover art off the card | The game that is running, picked out |
| ![Systems](img/device/systems.png) | ![Saves](img/device/saves.png) |
| Browsing by system | Suspend points, each with the moment it holds |
| ![Options](img/device/options.png) | ![Controllers](img/device/controllers.png) |
| Options | Every controller the machine can see |
| ![Controller test](img/device/controller-test.png) | ![Still playing](img/device/still-playing.png) |
| Testing a pad | A core that cannot pause says so |

The Display screen is deliberately absent at 240p: the preview tiles would come out
about 66px wide, which is too small to judge a filter by. It is there on HDMI.

---

## The shelf

![The shelf](img/shelf.png)

Your whole library in one row: cover art, the system, and how many times you have
played each game. Left and right walk the shelf; **the shoulder buttons jump by first
letter**, which is how you cross a big library - a page is only three cards at 240p, so
paging a thousand games was barely faster than walking.

The row starts with **Favourites** and **Systems**, so from anywhere in the shelf
**B** jumps straight back to them rather than making you walk.

The strip of pips under a card is its save-state slots — filled ones are green.

### System icons

![System icons](img/system-icons.png)

Almost every system has an icon. They come from licensed icon sets, not hand-drawn — see
[ICONS.md](ICONS.md) for the attribution. Mega CD, PC Engine CD and Neo Geo CD draw the
machine they plug into, since that is the console you are looking for; Saturn has no icon
yet and draws a plain folder.

### The games folders it reads

One folder per system under `games/`, and they are the names the official MiSTer
Distribution uses — so if you have downloaded romsets or used `update_all`, your games are
already in the right places and there is nothing to move.

| Console | Folder |
|---|---|
| NES, SNES, Game Boy, Game Boy Advance, Nintendo 64 | `NES` `SNES` `GAMEBOY` `GBA` `N64` |
| Mega Drive, Master System, TurboGrafx-16, Atari 7800 | `Genesis` `SMS` `TGFX16` `A7800` |
| PlayStation | `PSX` |
| **Mega CD** | **`MegaCD`** |
| **PC Engine CD** | **`TGFX16-CD`** |
| **Neo Geo CD** | **`NeoGeo-CD`** |
| **Saturn** | **`Saturn`** |
| Neo Geo (romsets) | `NEOGEO` |
| Arcade | `_Arcade` |
| Lynx, WonderSwan, Neo Geo Pocket | `AtariLynx` `WonderSwan` `NGP` |
| Amiga, Atari ST, C64, Spectrum, Amstrad, MSX, PC/DOS, Apple II | `Amiga` `AtariST` `C64` `Spectrum` `Amstrad` `MSX` `AO486` `Apple-II` |

The five disc systems read `.cue` and `.chd` and nothing else, which is why the BIOS files
that live in those same folders — `TGFX16-CD/cd_bios.rom`, `NeoGeo-CD/neocd.bin`,
`Saturn/boot.rom` — never appear as games. A disc copied into a folder as a `.cue` and a
set of `Track NN.bin` files is one card named after the folder; the tracks are not listed
separately. Regional subfolders (`MegaCD/Europe/`, `MegaCD/USA/`) are walked into and the
same game in two of them is one card, not two.

A card renamed for your region works too — `SegaCD` instead of `MegaCD`, `MegaDrive`
instead of `Genesis` — as long as the core file is renamed the same way, which is what the
downloader's `names.txt` does.

### Your own font

**MiSTer's `font=` option works here.** The front-end draws its text from the same glyph
table the stock OSD does, so a font you load for one is the font you get in the other —
there is nothing extra to switch on:

```
[MiSTer]
font=font/myfont.pf
```

Or pick one on screen: **Options > More Settings** has a **Font** row that lists every `.pf`
file in the card's `font/` folder, plus **Built-in**. Left and right change it and the screen
redraws in it straight away, so you can see a font before you keep it; **Save Changes** writes
`font=` into MiSTer.ini, and **X** puts back the one the file already names. Leaving without
saving puts the old font back too. If a file will not load, the row says so and the previous
font stays on screen rather than the screen going blank.

The format is the stock one: a plain 8x8 bitmap, 768 bytes for characters 32-127, or 1024 /
1136 / up to 2048 bytes for the wider ranges. 768 bytes is enough for everything the shelf
draws.

Two things worth knowing. The four arrow glyphs the front-end draws in button prompts are
its own, not the font's, so they keep their shape whatever you load — which is deliberate,
since a font with something unrelated at those codes would otherwise put garbage in the
button bar. And the stock 8x8 font is really a 6-wide font with two columns of bearing: if
your font uses all eight columns it will look tighter than the stock one does — which is what
the next two options are for.

### Letter spacing and capitals

Two settings that go with a custom font, both on **Options > More Settings** and both also
MiSTer.ini keys.

**Letter Spacing** (`classicui_tracking`, -2 to +2, default 0) adds or removes space between
characters. It is measured in font pixels and multiplied by the layout's text scale, so +1 is
one pixel at 240p and three on the HD layout.

- **-1** is the value a font that fills all eight columns wants. The built-in font uses six,
  and every panel in the front-end was laid out to that.
- **-2** makes the widest built-in glyphs — `& M W ^ _ m w ~` — touch the character next to
  them. It is offered anyway, because a narrow custom font may want it.
- **Positive spacing fits fewer words.** A full-width row at 240p holds 35 characters at 0 and
  28 at +2, and some of the front-end's wording was written to 35. Those lines start ending in
  a `>`. Nothing runs outside its panel — panels are measured in characters, so they grow with
  the spacing — but text that no longer fits is shortened rather than wrapped.

**Capital Letters** (`classicui_caps`, default on) is what draws every title, label and header
in capitals. Turn it off and they are drawn as they are written. It is worth trying with a
custom font: a font chosen for its lowercase never shows it otherwise.

## Save states, called Suspend Points

![Suspend points](img/suspend-points.png)

Press **down** on a game to see its save states. Three slots, each with a picture
of the moment it holds.

- **A** plays from that point
- **Y** saves the moment you are in now
- **X** deletes a slot (twice, deliberately)
- **down** locks a slot so it cannot be overwritten

Opening the menu inside a game does **not** let the game run on. On a core that can
pause properly, it pauses; on one that cannot, the moment is held with a state and
put back when you leave. Either way the game is where you left it.

### Systems that cannot save

![No save states](img/no-savestates.png)

Not every system supports save states — Neo Geo, Neo Geo CD, PC Engine CD, Mega Drive, N64
and most home computers do not. Rather than showing you three empty slots to try and fail
at, the shelf says so before you launch, and says it again inside the game.

Mega CD and Saturn say nothing either way before you launch, because nobody has checked
those two cores. Once the game is running the core answers for itself and the slots appear
if it has them.

---

## The menu, from inside a game

![In-game menu](img/in-game-menu.png)

Press the menu button while playing and the whole front-end comes up over a still
of your game: the shelf, your save states, settings, everything. Press it again to
go straight back.

The classic MiSTer OSD never appears on its own.

---

## Buttons look like your controller

![Button glyphs](img/button-glyphs.png)

Wherever the interface names a button it draws it, in the shape and colour that is
actually printed on the pad in your hands. Four sets: PlayStation shapes, Nintendo
letters, Xbox letters, and key names for a keyboard. It follows whichever
controller you used last.

| PlayStation | Nintendo, Xbox and the rest |
|---|---|
| ![PlayStation legend](img/legend-psx-240p.png) | ![Lettered legend](img/legend-letters-240p.png) |

Xbox pads keep the same *positions* as everyone else, which means different
letters: the button that confirms is the east one, which a Nintendo pad calls **A**
and an Xbox pad calls **B**.

---

## Settings

![The menu bar](img/menubar.png)

Press **up** from the shelf for the menu bar: Display, Options, Power, About.

![Options](img/options.png)

### Best Settings

![Best settings](img/best-settings.png)

MiSTer ships with several pop-ups that interrupt a game — the resolution banner,
the button-map panel, the autofire announcement. This turns them off in one go, and
checks that the front-end itself is switched on in a place that holds for every core
and every video mode, which on a card that has been in use for a while is not a given.

It shows you exactly which lines of `MiSTer.ini` it will change, keeps a copy of
your old file, and needs a second press to do anything. It is a short list on purpose:
your own settings — how games are scaled, where your art lives, whether the menu
pauses a game — are yours, and it does not have an opinion about them. Those live on
**More Settings**, where a value is offered rather than assumed.

### More Settings

![More settings](img/more-settings.png)

The `MiSTer.ini` options worth changing, with names a person can read —
**Button pop-up: Hidden** rather than `controller_info=0`. Grouped by what they
affect, with a line of help for the selected one.

Anything not at its usual value is **amber**, and the footer tells you what the
usual value is. **X** puts a setting back. Nothing is written until you choose
Save Changes.

The last three rows are about this menu's own text: **Letter Spacing**, **Capital Letters**
and **Font**. They are described under [Your own font](#your-own-font) above, because that is
what they are for. Font is the one row here that takes effect as you move it rather than when
you save — you are choosing how the letters look, and a font picked from a list of file names
without seeing it is a guess.

Options that could leave you with no picture at all are deliberately not here.

---

## On a CRT or a television

This front-end draws into MiSTer's *framebuffer*, and the framebuffer is composited
into the **scaler** output — the same output HDMI carries. Every other MiSTer screen
you know, including the stock menu, is drawn by the core into the video signal itself.
That difference is invisible on HDMI and it decides everything on an analog output, so
this section is about where your picture goes and what survives the trip.

**Options ▸ Best Settings** reads your setup and names whichever of these applies,
under the heading **Analog video**. It never changes any of them: getting video
routing wrong is a black screen with no way back except taking the card out, so these
are yours to set with the file in front of you.

### Which cable, and what it costs

| `vga_mode` | Cable | Colour on this menu | Notes |
|---|---|---|---|
| `rgb` (default) | SCART RGB, VGA | **Yes** | RGB carries no subcarrier, so there is nothing to lose |
| `ypbpr` | Component | **Yes** | Same: colour rides on separate wires |
| `svideo` | S-Video | **No — black and white** | Confirmed from the FPGA source, see below |
| `cvbs` | Composite | **No — black and white** | Same |
| `subcarrier` | External encoder (CXA2075 and friends) | **No** | The subcarrier is switched off on this path |

**Your games are not affected.** A running core sends its video down the direct path,
where the colour encoder is, and it keeps its colour on every one of these. It is only
this menu — and MiSTer's framebuffer terminal, and anything else drawn into the
framebuffer — that comes out grey on S-Video and composite.

This is not a setting anyone can change. MiSTer's S-Video/composite encoder
(`yc_out` in a core's `sys/sys_top.v`) is wired onto the *core's* video path only; the
analog output switches to the scaler path whenever the framebuffer needs to be shown
there, and the encoder is not on that leg. Fixing it means changing the shared core
framework and rebuilding every core, not the firmware. See
[README.md](README.md#analog-video) for the exact lines.

### Getting the menu onto the CRT at all

| Your setup | Where this menu appears |
|---|---|
| HDMI only | HDMI. Nothing to think about. |
| Analog only (`vga_scaler=0`, `direct_video=0`), **no HDMI lead** | The front-end takes the analog output for as long as it is open, in a 240p TV mode, and hands it back to the game. This is the intended path. |
| Analog **and** HDMI at once, `vga_scaler=0` | **HDMI only.** The CRT keeps showing the core. |
| `vga_scaler=1` | The analog output carries the scaler permanently, so this menu is on it — in whatever `video_mode` you configured, which is usually not a TV mode. |
| `direct_video=1` | On it, in the TV mode `direct_video` already runs. |

The third row is the one that surprises people. With a display on HDMI the front-end
deliberately leaves the analog output alone: taking it would drag the HDMI display down
to a 240p television mode as well, and a CRT beside an HDMI screen is an ordinary
setup that must not lose its picture because a menu wanted the other socket. So on that
setup the CRT shows the running core and nothing else, and changing
`classicui_profile` cannot alter that — the layout is not on that wire. **Unplug the
HDMI lead** if you want this menu on the television.

### The settings that decide it

None of these are written by this front-end. Set them yourself, in `[MiSTer]`.

| Key | For a CRT / television |
|---|---|
| `vga_mode` | `rgb` for SCART or VGA, `ypbpr` for component, `svideo` or `cvbs` for a TV's own inputs. Colour on this menu only on the first two. |
| `composite_sync` | `1` for anything that is not a VGA monitor. |
| `forced_scandoubler` | **`0`.** `1` asks for a 31 kHz signal, which a television cannot lock to at all — a scrambled, rolling picture. |
| `vga_scaler` | `0` normally. `1` puts the scaler on the analog socket permanently; it makes this menu visible even with HDMI attached, at the cost of the colour and of needing a `video_mode` a CRT can accept. |
| `direct_video` | `1` only for a VGA-to-HDMI converter or a DAC. It sends raw core timing out and turns the scaler off, so filters, shadow masks and the Display screen all stop applying. |
| `menu_pal` | `1` if your set is 50 Hz only. This chooses between the 240p60 and 288p50 modes the front-end takes the analog output in, and it defaults to `0`, so a PAL-only television gets 60 Hz and rolls. |
| `vsync_adjust` | `0` or `1` is safe. It has nothing to do with this menu — the mode is pinned while the front-end holds the screen — but `2` retimes the output on every mode change, which some sets dislike. |
| `video_mode`, `video_mode_pal`, `video_mode_ntsc` | Only read when `direct_video=0`. They set the **scaler's** output mode, which is what `vga_scaler=1` puts on the analog socket, so with `vga_scaler=1` this has to be a mode a CRT accepts. With `vga_scaler=0` they do not affect this menu, because the front-end sets its own TV mode while it is open. |

### Known good and known bad

Marked **confirmed** where it has been run on hardware, **from the source** where it
follows from the firmware and the FPGA framework but nobody has photographed it.

| Combination | Result |
|---|---|
| SCART RGB, `vga_mode=rgb`, `composite_sync=1`, no HDMI | **Good, confirmed.** This is what the photographs in this guide were taken on. |
| HDMI only | **Should be good, from the source — but no HDMI panel has ever been attached here.** The 720p and 1080p layouts are checked in the test harness at every release and have never been seen on real glass, so this row is the one in the table most worth reporting on. |
| Component, `vga_mode=ypbpr`, no HDMI, `menu_pal=0`, 50 Hz set | **Bad, reported:** rolling picture. Colour is fine. Try `menu_pal=1`. *(From the source: the front-end's TV mode is chosen on `menu_pal` alone.)* |
| S-Video or composite, no HDMI | **Colour is gone, from the source.** Sync should be correct; if it also rolls, see the `menu_pal` row above. |
| S-Video or composite **with** `forced_scandoubler=1` | **Bad:** a 31 kHz signal on a 15 kHz input. Set it to `0`. *(The front-end now ignores `forced_scandoubler` when it takes the analog output itself, so this only still bites under `direct_video=1`.)* |
| Anything analog **plus** an HDMI lead, `vga_scaler=0` | **This menu is not on the CRT at all.** By design; unplug HDMI. |
| `vga_scaler=1` with a 15 kHz `video_mode` | **Picture yes, colour no, from the source.** The layout is correct as of this version; earlier ones drew it for the wrong canvas shape. |
| `vga_scaler=1` with an HDMI `video_mode` (720p, 1080p) | **Bad:** nothing a television can display. |

---

## Controllers

![Controllers](img/controllers.png)

Every controller the machine can see — USB, Bluetooth and SNAC — with which player
number it is. Pairing a wireless pad is at the bottom of the list.

![Controller test](img/controller-test.png)

Choose a controller to test it. Press a button and it lights up on the diagram,
drawn in that pad's own button set. Useful for "is this thing connected?" and for
"which button is which?".

---

## Wi-Fi

![Wi-Fi scanning](img/wifi-scanning.png)

Networks, signal strength, and an on-screen keyboard for the password. Scanning
and joining show their progress, so a slow network looks busy rather than broken.
If joining fails it puts your old settings back.

---

## Art you already scraped

If you have ever scraped this card with another front-end — EmulationStation,
Batocera, Recalbox, ES-DE — or with Skraper or Skyscraper on a PC, your pictures
already work here. Those tools write a `gamelist.xml` into each system's games folder
saying which picture belongs to which game, and that file is read as it is: no second
scrape, no renaming, nothing to copy. ScreenScraper art scraped with Skraper is the
common case and needs nothing done to it.

Scrapes with no `gamelist.xml` work too, as long as the pictures are named after the
ROM file: `games/<System>/media/box2d/`, `boxart/`, `images/`, `media/mixed/`,
`media/screenshot/` and `screenshots/` are all looked in.

What a gamelist says wins over the art in `classicui_artdir`, because it is your own
scrape naming exact files rather than us guessing from a name. If its pictures are
worse than the ones in your art pack, `classicui_gamelist=0` turns it off. Only
pictures are read — names and descriptions are not, since titles here come from
filenames.

### Where a cover is looked for, in order

1. **Whatever is already on the card** — a `gamelist.xml`, a scraper's media folder, your
   `classicui_artdir`, or a picture beside the ROM. Nothing is ever downloaded over a
   picture you already have, so a scrape you did yourself is never overwritten.
2. **ScreenScraper**, if you have turned it on and given it your account.
3. **The libretro thumbnail pack**, if `classicui_artfetch` is on.
4. Nothing: a plain plate in the system's colour.

ScreenScraper goes above the libretro pack on purpose. If you have entered your own
credentials you have said which database you want your shelf built from, and a downloaded
cover is written to the card and then never looked for again — so whichever source answers
first is the one you are stuck with. Anything ScreenScraper has no cover for falls through
to the pack, and so does everything if your daily quota runs out.

### Your ScreenScraper allowance, and how it is looked after

A ScreenScraper account has two daily budgets, and the smaller one is the one a shelf
spends: alongside the ordinary request limit there is a limit on *unmatched* searches —
games the database could not find. A shelf of regional variants, hacks and homebrew misses
often, and if that budget runs out the server refuses your account for the rest of the day
with `Faite du tri dans vos fichiers roms et repassez demain !` — nothing to do with the
game you asked about.

Four things keep that from happening.

- **A game with no cover is asked about once a week, not once a boot.** The answer is
  written to `classicui/ss-misses.txt` on your card and honoured for seven days. Seven
  rather than forever because ScreenScraper is a live database and a game with no cover
  today may have one next month. Delete the file to ask about everything again.
- **Only a real answer is remembered.** A quota, a rate limit, a lost connection or the
  message above leave every game exactly as unasked as they found it, so one bad afternoon
  cannot blank your shelf.
- **The shelf stops scraping at 90% of the unmatched budget**, leaving the rest for the
  physical-disc dialog — the one place you are actually waiting for a picture. Covers fall
  back to the libretro pack, and the shelf starts again the next day by itself.
- **Requests are at least 1.2 seconds apart**, so scrolling fast cannot machine-gun their
  server.
- **A game that has never been asked about goes first.** When a week is up and a game
  becomes askable again, it queues behind every game nobody has asked about yet — those are
  likelier to match, and it is the unmatched budget that is scarce. The card under the
  cursor still comes first: a cover that can be drawn now is never held up by any of this.

## Installing

**From a release, which is what most people want.** The archive unzips to a folder called
**`SD-CARD-ROOT`** whose contents mirror your card. Installing is copying that folder's
*contents* into the card root and letting it merge — the folders line up with the ones
already there, so nothing needs sorting by hand:

```
SD-CARD-ROOT/
  MiSTer                            replaces the firmware in the card root
  classicui/disctitles.txt          the disc name table - a new file, replaces nothing
  Scripts/classic_home_protect.sh   run once, so updates stop reverting the front-end
  Scripts/classic_home_unprotect.sh undoes that again
  linux/classic-home/               what those two install; nothing to open yourself
  MANIFEST_*.txt                    every file in the archive, with its size and md5
```

and, if you took the build that also carries PSX controllers over SNAC:

```
  menu.rbf                          replaces the menu core
  _Console/*.rbf                    our core builds, named so they sit beside your own
  _Computer/*.rbf                   rather than overwrite them
  _Arcade/cores/*.rbf
  Scripts/snac_remove_old_cores.sh  optional clean-up for superseded duplicates
```

**Back up the files you are replacing first** — `MiSTer`, and `menu.rbf` if it is in the
archive. Copy them beside themselves as `MiSTer.backup` and `menu.rbf.backup`. That is your
way back, and it is one copy each.

Then turn it on, at step 4 below — and run **`classic_home_protect`** from the MiSTer
Scripts menu once you are there, or the next `update_all` puts the official firmware back
and the front-end with it. [Surviving `update_all`](#surviving-update_all) is the whole
story.

**Building it yourself** is the rest of this section.

1. **Build the firmware.** From a checkout of this fork:

   ```
   source ./setup_default_toolchain.sh     # downloads the ARM toolchain the first time
   make
   ```

   The result is `bin/MiSTer`. Building on macOS needs an `linux/amd64` container;
   see `CLAUDE.md` at the repo root.

2. **Choose your branch.**

   | Branch | Take this if |
   |---|---|
   | `classic-ui` | you want the front-end |
   | `deploy-all` | you also want PSX controllers over SNAC in every core |

3. **Copy it to the card**, as `MiSTer` in the card root. Keep the old one:

   ```
   cp /media/fat/MiSTer /media/fat/MiSTer.prev
   cp bin/MiSTer /media/fat/MiSTer
   ```

   Copying over the running firmware fails with "text file busy" — `mv` a new file
   over it, or copy while the machine is off.

4. **Turn it on** in `/media/fat/MiSTer.ini`:

   ```
   [MiSTer]
   classicui=1
   ```

   **The section matters as much as the line.** `MiSTer.ini` is divided into sections
   — `[MiSTer]` for the general settings, then one per core (`[NES]`, `[Minimig]`) and
   one per video mode (`[video=1280x720]`) — and a setting belongs to whichever section
   it is written under. On a card that has been in use for a while the file usually
   *ends* inside one of those, so adding `classicui=1` at the bottom scopes it to that
   one core or that one video mode, where it does nothing you will notice and looks
   exactly like a firmware that did not take. Put it near the top, under `[MiSTer]`,
   with the rest of the general settings.

5. **Reboot.** First boot scans your `games/` folders and builds the library, which
   takes a moment; after that it is cached.

If something goes wrong, `classicui=0` gives you the stock menu back, and
`MiSTer.prev` is the firmware you were running before.

### Surviving `update_all`

**Run `classic_home_protect` once, from the MiSTer Scripts menu.** Then updates stop
reverting the front-end.

They do revert it otherwise, and it is not a bug anyone can fix in an update script:
the file `MiSTer` belongs to MiSTer's official distribution database, so `update_all`
and the MiSTer Downloader replace it with the official build, move ours to
`.MiSTer.old`, and ask you to reboot. `menu.rbf` is owned the same way, which matters
in the build that also carries PSX controllers over SNAC. No third-party database is
allowed to supply either file, so there is nowhere to publish this as an update — the
only place to put the firmware back is at boot, on your own machine.

That is what the Scripts entry sets up. It keeps a verified copy of the firmware in
`linux/classic-home/`, a folder no database may write to for the same reason
`MiSTer.ini` is safe from them, and adds a marked block to
`linux/user-startup.sh` — the file `/etc/init.d/S99user` runs at boot. From then on:

- boot with the right firmware in place and it does nothing at all, silently;
- boot after an updater replaced it and ours goes back, with a line saying so in
  `linux/classic-home/restore.log`;
- the official build it displaced is kept as `MiSTer.official`, so you can go back;
- a **newer** Classic Home firmware that you copied on yourself is left alone, not
  quietly downgraded. Re-run the Scripts entry after copying one on, so the stored
  copy is the one you are actually running.

Nothing is ever restored from a copy that has not been checked first — its size and
md5 must match what was recorded, and the firmware must still be an ARM executable —
and the new file is written under a temporary name and renamed into place, so there is
no moment at which the card has no `MiSTer` on it. If a check fails it changes nothing
and says why in the log.

One thing it cannot do: if the restore happens after the firmware has already been
launched, it takes effect at the *next* boot, so an update can still cost you one boot
of the stock menu. It will not kill and restart the firmware to avoid that — nothing
supervises that process, so a failed relaunch would be a machine showing nothing at
all.

`classic_home_unprotect` undoes all of it: the block comes out of `user-startup.sh`
(anything else in that file, including MiSTer_SAM's own lines, is left exactly as it
was), and the official firmware goes back if we kept a copy. Note that `.MiSTer.old`
is *not* the official firmware — an updater puts the file it replaced there, so that
copy is ours.

Both scripts print exactly what they changed, and running either twice is harmless.

### Is your ini set up?

Once the shelf is on screen you do not have to read the file to find out.
**Options ▸ Best Settings** says so on the row itself — `All Set`, or
`3 To Change >` — and opening it lists the exact lines it would write, keeps a copy of
your old file, and needs a second press before it touches anything. Everything not on
that list is left alone, including options it has never heard of and your own comments.

It is worth one look on a card that was set up for something else. Besides MiSTer's
pop-ups it checks `classicui=1` itself, so the sectioning trap above in its other
form — the front-end running from `[MiSTer]` while a `classicui=0` sits in some core's
section, which costs you this menu inside that one game — is something it finds and
offers to repair. What it cannot help with is a machine where the switch never took at
all: that one never gets here to be asked.

### Settings it adds

All optional; the defaults are what most people want.

| Key | Default | What it does |
|---|---|---|
| `classicui` | `0` | Turns the front-end on |
| `classicui_profile` | `0` | Layout size: auto, or force hd/sd/240p |
| `classicui_overscan` | `6` | Percent kept clear of the screen edge, for a CRT |
| `classicui_artdir` | `boxart` | Where cover art lives, under the games folder |
| `classicui_artfetch` | `0` | Download missing cover art over the network |
| `classicui_arturl` | libretro's thumbnail server | Where `classicui_artfetch` fetches from |
| `classicui_gamelist` | `1` | Read `gamelist.xml`, so art scraped elsewhere works here |
| `classicui_freeze` | `1` | Hold the game still while the menu is open |
| `classicui_screenscraper` | `0` | Ask ScreenScraper for the covers no local file can supply, before the libretro pack. Needs an account of your own |
| `classicui_ss_user` | unset | Your ScreenScraper user name |
| `classicui_ss_pass` | unset | And its password, in clear text |
| `classicui_ss_replace_pack` | `0` | Try ScreenScraper once more for covers already downloaded from the libretro pack |
| `classicui_disc` | `0` | Recognise a physical CD in a USB drive, and play it |

**`classicui_screenscraper` needs a ScreenScraper account of your own.** Make one — it
is free, at [screenscraper.fr](https://www.screenscraper.fr) — and put it in
`classicui_ss_user` and `classicui_ss_pass`. Without an account the option does
nothing at all: there is no anonymous access, and the guest pool everyone shares is a
few requests a day for the whole world. Your own login is what earns you your own
quota. Two more things worth knowing before you fill it in. The password sits in clear
text in `MiSTer.ini`, on a FAT partition anything on the machine can read, so use one
you do not use anywhere else. And the front-end asks for one thing at a time, so art
fills in gradually rather than all at once — it never holds the menu up waiting.

**If your card was filled from the libretro pack before you had an account**, set
`classicui_ss_replace_pack=1`. Covers this front-end downloaded from the pack are noted in
`classicui/art-from-libretro.txt`, and with that option on each of them is offered to
ScreenScraper once — from an idle shelf, after everything else, and once only per cover
whichever way the answer goes. Nothing else is ever re-scraped: a cover your own
`gamelist.xml` names, one you scraped with another tool, and a pack you installed by hand
are all left exactly alone. With the option off — which is how it ships — the file is
noted and nothing is asked; delete it and the covers are simply kept.

If you build the firmware yourself the option is inert unless you supply your own
per-application developer credential, which ScreenScraper's staff issue on request; see
`support/classicui/tools/ss_creds.sh`. The published builds carry one, so an account of
your own is all you need.

**`classicui_disc` is for a real CD in a real drive.** Any powered USB optical drive,
including the SuperDock's slot loader. A disc is recognised when you put it in, and
four cores play it straight from the drive with no image on the card: PC Engine CD,
PlayStation, Mega CD and Neo Geo CD. Any other disc is identified and named, with
nothing to boot it into — a Saturn disc is the one worth knowing about: it cannot be
played from the drive, and it can still be copied onto the card and played from there.
It is off by default and that is not caution for its own sake:
every command sent to the drive queues behind whatever the drive is already doing, and
a drive that stops answering used to take the whole front-end down with it. See
[The disc title table](#the-disc-title-table) for where the name on the shelf comes
from.

### Copying a disc onto the card

*Only of interest with `classicui_disc=1`.*

Playing from the drive needs the disc in the tray every time, and it needs the drive to
keep up for as long as the game lasts. Copying the disc onto the card is the other
answer: do it once, and afterwards the game loads like everything else on the shelf.

Open the disc dialog, move to **Options**, and the last entry is **Copy to
PlayStation** — or to whichever console the disc was recognised as, or whichever one you
picked from the list above it. It goes into that console's own games folder, in a folder
named after the game:

```
/media/fat/games/PSX/Metal Gear Solid (Europe)/Metal Gear Solid (Europe) (SLES-01506).cue
/media/fat/games/PSX/Metal Gear Solid (Europe)/Metal Gear Solid (Europe) (SLES-01506) - Track 01.bin
/media/fat/games/PSX/Metal Gear Solid (Europe)/Metal Gear Solid (Europe) (SLES-01506) - Track 02.bin
```

A disc that carries no serial — PC Engine CD and Neo Geo CD discs do not — keeps the
simpler shape it always had, `Metal Gear Solid/Metal Gear Solid.cue` beside
`Track 01.bin`, because there is nothing to tell two copies of it apart with.

A Mega CD, PC Engine CD or Neo Geo CD disc goes into that console's **CD** folder rather
than its cartridge one — `games/MegaCD`, `games/TGFX16-CD` and `games/NeoGeo-CD`, which
are the folders those cores read their discs and their BIOS out of, and the ones a
downloaded set lands in. The row says which, so you can see where the card will turn up
before you press it.

**A Saturn disc can be copied even though it cannot be played from the drive.** Those are
two different things: playing from the drive needs the Saturn core to be fed sectors while
the game runs, which it cannot yet do, while copying is the front-end reading the disc and
writing `games/Saturn/<game>/<game>.cue` — which the Saturn core loads exactly like any
other Saturn image you already have. So a Saturn disc shows **Play** greyed out and **Copy
to Saturn** in the Options list, and once the copy finishes it is a card like any other.

What still gets no copy row is a disc there is nowhere to put: 3DO and CD-i, which this
firmware has no shelf system for at all, an MSU-1 SNES disc, whose core wants the `.sfc`
off the disc rather than a copy of the disc, an audio CD, and a disc nothing recognised.
For those the dialog says what it found and offers no copy, rather than a greyed-out row
that could never work.

The name is the same one the shelf shows for the disc — the title table's answer if you
have one, otherwise the disc's label or its serial. The copy appears on the shelf as one
card under that name as soon as it finishes; the tracks are not listed as games of their
own, and neither the region nor the serial in the filenames shows on the card.

**Every disc of a game goes in the one folder.** Copy disc 1 of Metal Gear Solid, then
put disc 2 in and copy that, and both land side by side in
`Metal Gear Solid (Europe)/`. They appear as a single card, and **X** cycles between the
discs the same way it cycles any other set of versions. That layout is not tidiness: the
PlayStation core decides whether a disc you load is a swap or a new game by looking at
the folder it came from, so two discs in one folder swap without resetting the console
and keep the same memory card, and two discs in two folders do not. There is no `.m3u`
and none is needed — nothing here reads one.

The region in the folder name comes from the serial: `SLES` and `SCES` are European,
`SLUS` and `SCUS` American, `SLPS` and `SLPM` Japanese. It is there so that a PAL copy
and an NTSC copy of the same game are two folders with two memory cards, instead of one
landing on top of the other.

**The filename says the serial, not "Disc 2".** Nothing the console can read at copy
time says which disc of a set it is holding — the disc carries a serial and no disc
number, and the title table gives both Metal Gear Solid discs the same name. Guessing
from the serial works for some sets and not others (the two American discs are
`SLUS-00594` and `SLUS-00776`), so the copy is filed under the serial, which is always
right, rather than a disc number that would sometimes be wrong. Discs of a set do
generally sort into disc order anyway, so the cycle usually runs 1, 2, 3.

**What you see while it runs.** The disc spins fast and fills in from twelve o'clock as
it is copied, with the percentage under it. That fraction is the sectors actually
written, so it moves at the speed the work does rather than guessing. You can press B
and leave — the copy carries on in the background, the disc badge in the corner keeps
turning, and pressing A on it comes back to the progress. **A** on **Stop** twice stops
it, and a stopped copy leaves nothing behind at all: it is built in a hidden folder and
only put in place, in one step, once every byte is written. The same is true if the
power goes.

**It will not quietly overwrite anything.** If you have already copied *this disc* — the
same serial — the entry changes to *Replace this disc? Press again*, the way deleting a
suspend point does. Another disc of a game you have already started is simply added, with
nothing to confirm, because adding it costs you nothing. Even a confirmed replacement
leaves the copy you already have untouched until the new one is complete, and only that
disc's own files are replaced; the others in the folder are not touched. Free space is checked before the drive is touched, and
a card without room refuses with the two numbers rather than filling up.

**A scratched disc still copies, and says so.** A sector that will not read after four
attempts is written as silence — 2352 zero bytes — so every track stays exactly the
length the cue sheet says and nothing after the bad spot slips out of place. The screen
then reports *Copied, but N sectors would not read* instead of *Copied*, and the folder
carries an `unreadable-sectors.txt` listing them with the track each was in. A zeroed
audio frame is a click; a zeroed data sector may stop the game loading, which is why you
are told rather than left to find out from the core. In a folder holding more than one
disc the list is named after the disc it belongs to, so one disc's account of what went
wrong is not overwritten by the next. A drive that disappears mid-copy —
a USB reset, which this dock does — is waited out rather than treated as an error, and
the percentage stops moving while that happens.

**What each console gets.** All four are raw 2352-byte tracks, one file per track,
because that is what every CD core in this firmware reads. The only difference is the
sheet: PlayStation gets the disc's real sector mode, `MODE2/2352` on most discs, because
its parser understands it. Mega CD, Neo Geo CD and PC Engine CD get `MODE1/2352`, which
is what their parsers understand and what those discs are anyway. Where the drive can
read the raw subchannel, real pregaps are recovered and written into the track that owns
them; where it cannot, the sheet has no pregaps, which is what a cue sheet has always
meant by their absence.

**All four now become cards, which they did not used to.** A Mega CD, PC Engine CD or
Neo Geo CD copy went into the cartridge machine's folder, and those shelf entries take
cartridges and not `.cue` — so the copy was written correctly and could only be loaded
from the core's own file browser. Each of those consoles has a shelf entry of its own
now, reading its own CD folder and launching its own CD core, so the copy is a card as
soon as it finishes. Nothing about the copy itself changed; only where it is put.

**Reading the disc is [Anime0t4ku](https://github.com/Anime0t4ku)'s work, not ours.**
Everything above rests on
[Main_MiSTer_Physical_Disc](https://github.com/Anime0t4ku/Main_MiSTer_Physical_Disc), a
fork of MiSTer's firmware by that author, which is what made a real CD playable on this
hardware in the first place. That code is used here as it was written rather than
rewritten, under the same GPLv3 this tree carries. What this front-end adds is the shelf
around it: recognising the disc without stalling the menu, naming it, its artwork, and
giving it save states of its own. `support/physical_disc/CREDITS.md` says exactly which
part is whose.

**`classicui_freeze` is worth knowing about.** Holding a game still means asking the
core for a save state, and at least one core cannot survive being asked at a bad
moment: the SNES core dies — black picture, no more save states — if asked during a
demanding scene, and only reloading the core recovers it. That is a bug in the core,
not here, and it happens equally from MiSTer's own Alt-F1 hotkey. If you hit it, set
`classicui_freeze=0`: the game keeps playing behind the menu, which is what already
happens on cores with no save states at all.

### The disc title table

*Only of interest with `classicui_disc=1`.*

A pressed disc has no filename. Everything else on the shelf is named after the file it
came from; a disc offers a serial — `SLES-01506` — which is the right identifier and
recognisable to nobody. So the name you see comes from a table of serials, and that table
is a file on the card. **Without it a disc still works — you just see `SLES-01506` on the
shelf where a name would be.**

Copy the `disctitles.txt` that ships beside the firmware to

```
/media/fat/classicui/disctitles.txt
```

and discs get their real names. It is plain text, one disc per line,
a tab between the serial and the title:

```
#classicui-disctitles 1
SLES01506	Metal Gear Solid
SLUS00594	Final Fantasy VII
```

Serials are stored upper-case with the punctuation removed — `SLES-01506` becomes
`SLES01506` — and the file must stay sorted by that key, because it is searched in
place rather than read into memory. Correcting one disc is therefore editing one line
on the card, which is half the reason the format is text.

The table shipped here covers 12,761 releases across the four systems. To build a fresher
one yourself, the script that ships with this front-end fetches the data and writes the
file:

```
python3 support/classicui/tools/disctitles.py --fetch -o disctitles.txt
```

The data is [Redump](https://redump.info)'s. Their position is that their metadata is
public domain, which is a clearly stated intent rather than a formal grant, so where the
shipped table came from is written down in `DISCTITLES.md` beside it. The script's own
header lists the other sources it accepts and what each one's licence allows.

**If you fetch the DAT files by hand, the `/serial` on the end of the URL is not
optional:**

```
https://redump.info/datfile/psx/serial      yes
https://redump.info/datfile/psx             silently useless
```

Without that suffix Redump builds the DAT with no serial fields in it at all. Nothing
errors: the script reads the file, finds nothing to key on, and writes a table that
matches no disc ever, so every disc goes on showing its bare serial with no clue as to
why. Note the domain too — `redump.info`; the old `redump.org` no longer answers.

---

## If it does not work

**Start here, and start with the card in a PC:**

```
classicui/config-report.txt
```

Written on every boot, whether or not the shelf came up. It lists every `classicui`
setting the parser saw, which section it came from, which line, and whether it was
actually read — and then says in words what to do about each one it does not like.

Read this one *before* `debug=2`, not after. `debug` is a setting in `MiSTer.ini` like
any other, so a `debug=2` that landed in the wrong section produces no log at all, and
"the wrong section" is the very thing you are most likely looking for. This report is
never in that log; it is a plain file on the card and it does not consult `debug`.

When the shelf *is* up and the report found something, the foot of **Options** says so
in amber and names the file.

---

Everything the front-end decides, it prints too. With `debug=2` in `MiSTer.ini`:

```
grep ClassicUI /tmp/debug.txt
```

- **Stock menu instead of the shelf** — `classicui=1` missing, or the firmware did
  not replace. `classicui/config-report.txt` answers this one outright — and it is the
  case where the debug log cannot, because with no shelf there is nothing to say it on
  and a misplaced `debug=` writes no log. If the line is in your `MiSTer.ini` and the
  report still says `CLASSICUI=0`, look at what section it landed in: below a `[NES]`
  or a `[video=...]` header it belongs to that core or that video mode and nothing
  else. It wants to be under `[MiSTer]`.
- **The shelf is there, but the menu button inside one game gives the classic OSD** —
  the same thing the other way round: a `classicui=0` in that core's section.
  **Options ▸ Best Settings** lists it and offers to put it right.
- **"No games found"** — your games are not under `games/<System>/`. The log lists
  every folder it looked at and what it found.
- **No cover art** — art goes in `boxart/<System Name>/Named_Boxarts/`, named after
  the ROM, and `games/<System>/media/box2d/` works too. If you scraped on a PC, see
  "Art you already scraped" above. `classicui_artfetch=1` downloads what is missing.
- **A disc shows a serial instead of a name** — that serial is not in the table. Add
  the one line yourself, or build a fresh table; see
  [The disc title table](#the-disc-title-table).
- **Scrambled black and white with a rolling image on a CRT, and fine on HDMI** —
  four different faults look like this, and [On a CRT or a
  television](#on-a-crt-or-a-television) has the whole picture. In order of how often
  they are it:
  1. **`forced_scandoubler=1`.** That asks for a 31 kHz signal. A television is a
     15 kHz device and cannot lock to one, so what you get is a rolling mess whatever
     else is right. Set it to `0`. (This firmware now ignores it while the front-end
     has the analog output, so if you are still seeing it, `direct_video=1` is on.)
  2. **An HDMI lead plugged in at the same time.** Then this menu is on HDMI only and
     your CRT is showing the *core*, which is why changing `classicui_profile` between
     HD, SD and 240p makes no difference at all — the layout was never on that wire.
     Unplug HDMI.
  3. **`menu_pal=0` on a 50 Hz-only set.** The front-end takes the analog output at
     60 Hz unless told otherwise. Set `menu_pal=1`.
  4. **`vsync_adjust=2`.** Not this menu's doing — the mode is pinned while it is open
     — but worth ruling out with `0`.
- **The colours vanish when the menu opens, and come back in the game** — expected on
  `vga_mode=svideo`, `cvbs` or `subcarrier`, and not fixable in the firmware. See
  [Which cable, and what it costs](#which-cable-and-what-it-costs).
- **Buttons and text overlapping each other on a CRT** — fixed in this version. It was
  the layout being computed for a 640x240 canvas as though its pixels were square,
  which happened with `vga_scaler=1` or `direct_video=1`. If you still see it, the log
  line `ClassicUI: profile ... canvas ...` says which canvas arrived.

---

## For developers

[README.md](README.md) covers how it is built: the modules, how it draws, the
integration points in the rest of the firmware, and the host test harness that
renders every screen in this guide.
