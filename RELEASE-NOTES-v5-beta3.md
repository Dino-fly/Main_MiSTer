# Classic Home v5 **beta 3** — same front-end, packaged properly

**Still a beta.** Beta 3 is beta 2's front-end on current upstream firmware, packaged as an
SD-card structure rather than a loose file, and able to survive `update_all`. If you ran
beta 2, take this. If you want a quiet life, stay on v4.

## Installing

`ClassicHome-v5-beta3.zip` unpacks to a folder called `SD-CARD-ROOT` whose contents mirror
your card. Copy that folder's *contents* into the card root and let it merge:

```
SD-CARD-ROOT/
  MiSTer                            replaces the firmware in the card root
  classicui/disctitles.txt          the disc name table - a new file, replaces nothing
  linux/classic-home/               the update_all boot hook's worker
  Scripts/classic_home_protect.sh   installs the hook
  Scripts/classic_home_unprotect.sh removes it again
```

`MiSTer` is the only file that overwrites anything, so **back it up first** — copy it beside
itself as `MiSTer.backup`. That is your way back, and it is one copy.

Then turn it on in `MiSTer.ini`, under `[MiSTer]`:

```
classicui=1
```

**The section matters as much as the line.** `MiSTer.ini` is divided into sections, and on a
card that has been in use a while the file usually *ends* inside a core or video section — so
adding `classicui=1` at the bottom scopes it to that one core and it looks like the release
simply does not work. Put it under `[MiSTer]`.

## Surviving `update_all`

This is new, and it matters more than anything else in the release.

`update_all` replaces `MiSTer` with the official build and puts yours in `.MiSTer.old`,
silently reverting the front-end. That is not a bug in the updater: the `MiSTer` path belongs
to the official distribution database and no third-party database is permitted to supply it —
we checked the downloader's source rather than guessing.

Run this once:

```
/media/fat/Scripts/classic_home_protect.sh
```

It stores your firmware under `linux/`, which is on the downloader's forbidden list for *every*
database including the official one, and installs a boot hook that puts it back when
`update_all` has replaced it. It only acts when the installed firmware is not one of ours, so a
newer build you copied on by hand is left alone rather than quietly downgraded.
`classic_home_unprotect.sh` removes the hook and leaves the official firmware in place. Your
`MiSTer.ini` is never touched by `update_all` either — it is forbidden to every database.

## What changed since beta 2

**Current upstream firmware.** 25 upstream commits, including the EDID/HDMI cold-boot fixes.
Both of this project's firmwares — this one and the SNAC-only build — now sit on the same
upstream base, so a bug report means the same thing whichever you run.

**Packaged as a card structure.** Beta 2's notes claimed "everything is one file", which was
never true: the release is the firmware *and* the disc title table, and now the two Scripts
entries as well.

**Nothing else in the front-end changed.** Everything in the
[beta 2 notes](https://github.com/Dino-fly/Main_MiSTer/releases/tag/classic-home-v5-beta2)
still applies — physical CDs, the lighter menu, the resized disc screen, artwork from
ScreenScraper, per-system core settings with **Y**. Read those for what the front-end does.

**A custom font works, and always did.** `font=font/myfont.pf` in `MiSTer.ini` changes every
glyph the shelf draws, because the front-end reads the same glyph table the stock OSD does.
Nobody knew because it was documented only in the generic ini block. The four arrow glyphs in
the button bar are the front-end's own and keep their shape. Note the stock 8x8 font is really
6 wide with two columns of bearing, so a font using all eight columns will read tighter.

**Cross-folder title grouping** — if you have a romset split into `USA/` and `Europe/` folders
and `Batman (U).nes` shows as a separate card from `Batman (E).nes`, you are on **v4**. Both v5
betas already group them. Grouping happens when the shelf is built rather than in the index
cache, so the new binary alone fixes it — no rescan needed.

## PSX controllers over SNAC

This firmware carries SNAC support as well. If you want it, take
`snac-psx-pad-wave3-cores.zip` from the
[wave 3 release](https://github.com/Dino-fly/Main_MiSTer/releases/tag/snac-psx-pad-wave3) —
that archive is the cores and `menu.rbf`, and it is the same download whichever firmware you
run. **Read its install order before copying**: six core fixes change only letter case, and a
card is case insensitive, so those files must be deleted before the merge or the fix does not
take.

You do not need the firmware from that release; this one already has SNAC in it.

## Credit

Physical CD playback is [Anime0t4ku](https://github.com/Anime0t4ku)'s work, from
[Main_MiSTer_Physical_Disc](https://github.com/Anime0t4ku/Main_MiSTer_Physical_Disc) — the
streaming sector reader, the read-ahead worker, the drive-speed cap, the recovery when a USB
drive drops out, the disc swapping. Used here whole rather than rewritten, GPLv3 as this tree
is. None of the disc support would exist without that author; see
`support/physical_disc/CREDITS.md`.

---

## Known limitations — please do not report these

- **On S-Video and composite the menu is black and white.** Games keep their colour. The colour
  encoder in the shared FPGA framework sits only on the core's video path, not the one this menu
  goes out through, so it cannot be fixed in firmware. **RGB SCART and YPbPr are in full
  colour.**
- **With an HDMI display attached the front-end deliberately leaves the analog output alone**,
  so a CRT shows the game rather than the menu. Unplug HDMI to see the menu on a CRT.
- **No still picture behind the in-game menu over a disc game** — you get black. Mostly hidden
  by the disc screen now. File-launched games are fine.
- **Everything above 240p is verified in the test harness only.** There is no HDMI display here,
  so the 720p and 1080p layouts have never been seen on a real panel. Still the single most
  useful thing to report.
- Mega CD, PC Engine CD and Neo Geo CD disc playback is **untested on hardware**.

## What was actually tested

On a real DE10-Nano over analog output, with this exact binary (md5 `5526bbe1`): the shelf with
cover art and badges, a carousel slide, and the disc screen showing a real disc's title from the
shipped table with its scanned artwork. The test harness runs 1457 checks with 0 failures, but
that is a host build — it has never caught an analog-output or device-lifecycle problem, which is
why the device pass matters.

## Reporting a bug

Include your `MiSTer.ini` (**remove your ScreenScraper password first**), how your display is
connected and whether the set is PAL or NTSC, what you saw in your own words, and
`/tmp/debug.txt` after setting `debug=2` and reproducing. Say whether it also happens with
`classicui=0` — if it does, it is not this front-end.

## If it goes wrong

Copy your `MiSTer.backup` over `MiSTer`. `classicui/disctitles.txt` can stay; nothing else
reads it. Or set `classicui=0` for the stock menu with this firmware.
