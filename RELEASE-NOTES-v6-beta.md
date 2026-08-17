# Classic Home v6 beta

Fifty-three commits since v5. The visible half is the Display screen, which is now a
proper picture of what each look does to *your* game rather than a shipped sample. The
other half is the front-end getting out of the way: on a 1080p display it was making the
picture wobble while a core ran behind it, and finding out why took the fabric's own
source to settle.

**Firmware only.** `menu.rbf` and the SNAC-rebuilt cores are unchanged since
[v5](https://github.com/Dino-fly/Main_MiSTer/releases/tag/classic-home-v5) — if you are
installing on a fresh card, take v5 first for those, then this on top.

## Installing

`ClassicHome-v6-beta.zip` unpacks to a folder called `SD-CARD-ROOT` whose contents mirror
your card. Copy that folder's *contents* onto the card root and let it merge:

```
SD-CARD-ROOT/
  MiSTer                            replaces the firmware in the card root
  classicui/disctitles.txt          the disc name table
  classicui/disctoc.txt             names the discs that carry no product code
  classicui/lookshots/              the Display screen's stand-in pictures
  linux/classic-home/               the update_all boot hook's worker
  Scripts/classic_home_protect.sh   installs the hook
  Scripts/classic_home_unprotect.sh removes it again
```

**Back up `MiSTer` first** — copy it beside itself as `MiSTer.backup`. That is your way back.

Then, in `MiSTer.ini`, under `[MiSTer]`:

```
classicui=1
```

**The section matters as much as the line.** `MiSTer.ini` is divided into sections, and on
a card that has been in use a while the file usually *ends* inside a core or video section
— so adding `classicui=1` at the bottom scopes it to that one core and it looks like the
release simply does not work. Put it under `[MiSTer]`.

## The Display screen

One look large at the top with its description under it, and the choices as a centred row
of small pictures. Every one of them is **your game, right now**, run through that look's
own arithmetic — the same code the fabric runs — rather than a picture of somebody else's
game shipped in the zip. The stand-in pictures are still there for when nothing is running.

- **Filtered at the size the television draws, then zoomed.** The previous build magnified
  the frame first and filtered the magnification, which is why Sharp looked nothing like
  sharp pixels: it was a box-average of a blow-up.
- **The proportions come from the scaler.** A core's frame is not square-pixel — the Super
  Nintendo emits 512x224 for a 4:3 picture — so anything that assumed one scale for both
  axes came out stretched sideways.
- **CRT looks are a tube and a signal**: PVM, BVM, Trinitron and a consumer set, against
  RGB, S-Video and composite. Plus Sharp and None, which turn everything off.
- **The handhelds drive the core's own screen**, not an imitation of it: Game Boy, Game Boy
  Color, GBA, Game Gear, Lynx, WonderSwan and Neo Geo Pocket Color set the core's palette
  and screen-shadow options, and the LCD grid follows the magnification the scaler actually
  settles on. A grid narrower than the phases a magnification visits darkens nothing at all,
  which is why the GBA had no grid at 4x.
- **On an analog display the panel effects come off** and the palette stays. A pixel grid
  and a drop shadow on a CRT are not a Game Boy, they are a defect.
- **New defaults**: integer scaling everywhere, Super Game Boy on the Game Boy looks, and
  TrashUncle's PVM shadowmask for composite.

## The menu over a running game

The still behind the menu is the picture the television is showing: the look is run over
it on the ARM, at the size and in the place the scaler puts the game, so the scanlines and
grids are the ones the game has rather than a resampled approximation of them.

Opening the menu now lands on the game you are playing, wherever you had browsed to before.

## Speed, and the wobble

On a 1080p display with a core running behind the menu, the picture wobbled. It turned out
to be four separate costs, and the last one is not ours:

- **The framebuffer is scanned out 60 times a second whether anything redrew it or not.**
  At 1920x1080x32 that is 8.1MB a frame — half a gigabyte a second of DDR3 the running core
  is also using. The menu framebuffer is now **RGB565** above a megapixel and a half, which
  halves both the bytes and the number of read bursts per line.
- **A settled move on the shelf repaints the block and the cards, not the whole screen.**
  Measured: a one-card move changes 223 of 720 rows.
- **The scaler's geometry is read once a second and remembered**, not mapped out of
  `/dev/mem` every frame — and it is read while the game still owns the screen, because
  once the framebuffer takes over the scaler describes the menu instead.
- **Nothing writes to the log on a timer any more.** With `debug=2` a log line is a write,
  and a write while a game runs is DDR3 the framebuffer reader is competing for. The
  repaint counters are still there; `echo gfxstat > /dev/MiSTer_cmd` prints them on demand.

What is left is in `sys/`, not here: with a game loaded, ascal keeps scaling that core's
video into DDR and throwing it away, its Avalon arbiter serves writes before reads, and in
a game core the framebuffer reader has 256-byte bursts with 512 bytes of prefetch —
`N_BURST(2048)` is compiled in only under `MENU_CORE`. One line in `sys_top.v`
(`.freeze(freeze | LFB_EN)`) would stop the waste, at the cost of every core being rebuilt.

## Libraries, artwork and the shelf

- **Twenty thousand games**, and the cache is kept when it fills rather than thrown away —
  a 15,000-game card used to settle at about 960 and freeze a refresh at 6,000.
- **Local artwork**: the ScreenScraper / EmulationStation layout on the card is read
  directly (`media/box2d`, `gamelist.xml`), so an offline collection needs no downloads.
- **Missing covers fill themselves in** while nothing else is happening, behind the cards
  you are actually looking at — and never while a game is paused under the menu.
- **Icons** for Saturn and Game Gear, drawn as their own machines.
- **X riffles the pile**: a multi-file card's other versions cycle with a shuffle rather
  than a cut, and the card that leaves files back in behind the others.

## Fixes

- In-game saves land in every slot and wear the right picture — a save into slots 2 and 3
  used to fail on the PlayStation, because a UI frame over 31ms starves that core's
  savestate handshake.
- The disc dialog keeps its place when the strip is open, and a blanked reopen can no
  longer poison a slot's picture.
- `physical_disc` refreshed to upstream `5ac97bc` — the CD-DA streaming fixes.
- Game Gear is a system of its own and loads into its own slot.

## Known

- Opening the menu over a Super Nintendo game holds it still with a savestate, because that
  core reports no pause. During an animated intro that can blank the core until the state
  is restored — an upstream core issue, not this firmware's, but you may see it.
- The Display screen takes a second or two to appear on a full-resolution 1080p canvas:
  six previews, each run through the fabric's own arithmetic.
