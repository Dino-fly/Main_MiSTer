# Naming a disc that carries no product code — PC Engine CD and Neo Geo CD

Measured 2026-08-12 against a real PC Engine CD disc in the SuperDock's drive.

## Why these two consoles have no name today

`disc_serial_for()` answers for PlayStation, Saturn and Mega CD. PC Engine CD and Neo Geo
CD have no case, and that is not an omission:

- a PC Engine CD disc has **no ISO filesystem at all** — `disc_label_at()` needs a primary
  volume descriptor and there is none, so there is not even a volume label to fall back on;
- there is **no product code** anywhere in the data;
- Neo Geo CD does have a volume label, but it is a house code as often as a name.

So `disc_scrape_name()` returns nothing, nothing is asked of the database, and
`disc_display_name()` falls through to the console's own name. A player sees "PC Engine CD"
and no cover. That is working as designed, and the design has nothing to work with.

## What is actually on the disc

The boot area was read to settle whether a title could simply be lifted off it. It cannot.
Sector 0 of the first data track holds a Shift-JIS copyright notice (Hudson) followed by
module labels — `PRODUCER`, `DIRECTOR`, `CD-ROM SIMULATOR`, `BIOS MAIN CODE, CD-PLAYER`,
`BACKUP MEMORY MAINTENANCE`, `PSG DRIVER`, `GRAPHIC DRIVER`. Credits and code-segment
names. **No title field.**

## What the disc does have: its table of contents

The disc read on 2026-08-12, in full:

    tracks 1..22
    track  1  lba      0  AUDIO
    track  2  lba   3890  DATA
    track  3  lba  14189  AUDIO
    track  4  lba  22173  AUDIO
    track  5  lba  26917  AUDIO
    track  6  lba  34134  AUDIO
    track  7  lba  40411  AUDIO
    track  8  lba  45260  AUDIO
    track  9  lba  58635  AUDIO
    track 10  lba  66403  AUDIO
    track 11  lba  78455  AUDIO
    track 12  lba  86083  AUDIO
    track 13  lba  97368  AUDIO
    track 14  lba 108444  AUDIO
    track 15  lba 124926  AUDIO
    track 16  lba 135835  AUDIO
    track 17  lba 150506  AUDIO
    track 18  lba 157229  AUDIO
    track 19  lba 164899  AUDIO
    track 20  lba 173900  AUDIO
    track 21  lba 192012  AUDIO
    track 22  lba 211262  DATA
    leadout   lba 221262

Twenty-two tracks at those offsets is an extremely specific pattern. This is how the wider
world catalogues CDs that carry no header - it is what a CDDB/freedb disc id is computed
from, and it is the layout Redump records per release.

Read with the same ioctls the helper already uses - `CDROMREADTOCHDR` and
`CDROMREADTOCENTRY` - so getting it costs nothing new and touches no sector data.

## How to match it, and why it has to be indirect

**ScreenScraper has no TOC key.** `jeuInfos.php` takes `romnom`, `serialnum`, `crc`, `md5`,
`sha1` and `gameid`. The three hashes are of whole dump files, which cannot be computed from
a spinning disc without reading all 221262 sectors - and would then be a hash of our read,
not of anybody's dump. So there is no direct lookup for a disc like this.

The match therefore goes through a local table, in two hops:

    TOC on the disc  ->  fingerprint  ->  title        (offline, our table)
    title            ->  ScreenScraper by romnom       (the path that already works)

The second hop is the machinery Saturn and Mega CD already use and needs no change. Only
the first hop is new.

### Where the table comes from

The same place `disctitles.txt` comes from, through the same script. Redump `.dat` files
list every track of every release with its exact byte size, and a track's size divided by
2352 is its length in sectors - so the whole TOC of a release can be derived from the dat
without owning the disc. `tools/disctitles.py` already fetches and parses those dats for
serials (`--fetch`, and see `DISCTITLES.md` for the redistribution basis); emitting a second
table keyed on a TOC fingerprint is an extension of a pipeline that exists rather than a new
one.

### The fingerprint

Track count, each track's start LBA, and the leadout, hashed to a short stable string. Two
properties matter more than the exact choice of hash:

- **it must be computed identically on both sides.** The device reads LBAs from the drive;
  the generator derives them from dat file sizes. Both must agree on whether track 1 starts
  at 0 or at 150 (the two-second pregap), because MSF-based tools differ on this by exactly
  150 sectors. Get that wrong and nothing ever matches, silently.
- **it must admit collisions.** Some releases share a track layout - regional pressings of
  the same master especially. The table must be able to say "these two" and the front-end
  must then decline to name the disc rather than assert the wrong one. A wrong title feeds
  the art path and produces a confidently wrong cover, which this project has already
  learned costs more than no cover at all.

## PROVEN, end to end, on this disc

Done on 2026-08-12 against the real disc and the real datfile, so none of the above is
theory any more.

**Redump.** The datfile fetched with the pipeline that already exists
(`http://redump.org/datfile/pce/`, 551 releases) contains exactly one release whose layout
is the disc in the drive:

    Akumajou Dracula X - Chi no Rondo (Japan)      [Castlevania: Rondo of Blood]

    22 tracks, as the drive reports
    17 of 21 track lengths identical to the drive's
    the three that differ:  -225, +75, +150  -  which SUM TO ZERO
    total sectors in the dat: 221262   device leadout: 221262   difference: 0

That is the pregap question answered, and answered better than expected. The per-track
split differs - Redump accounts index-0 pregaps to the adjacent track, the drive does not -
but **the total does not**. The leadout matches to the sector.

**So the fingerprint should be (track count, total sectors), not the track vector.** It has
no pregap ambiguity at all, it is two integers, and it was measured across the whole datfile:

    542 releases carrying track data
    492 distinct (ntracks, total) keys
    452 of them name exactly ONE release          -  83% of releases, unambiguous
    40 keys collide

and the collisions are mostly benign for artwork, because they are revisions of one game:
Doukyuusei Rev 3/Rev 4, Tokimeki Memorial Rev 1/2/3, Ys I & II vs its Alt. Same game, same
cover. A handful are genuinely different and must be refused: J. B. Harold (Japan) against
(USA), Fighting Street (Japan) against (USA), and the two PC Engine Hyper Catalog discs.
The table has to distinguish "several dumps of one game" from "several games" and the
front-end has to decline to name the latter.

**ScreenScraper.** Asked for the matched title on systemeid 114, every shape hits:

    romnom "Akumajou Dracula X - Chi no Rondo (Japan)"   ->  hit
    romnom "Akumajou Dracula X - Chi no Rondo"           ->  hit
    romnom "...(Japan).cue"                              ->  hit
    romnom "Castlevania - Rondo of Blood"                ->  hit   (its own fuzzy match)
    the same title on systemeid 31 (TurboGrafx-16)       ->  hit

game id 14466, with 40 media entries including box-2D, support-2D and wheel - so both a
cover and a disc scan are available. The second hop needs no new code: it is the romnom path
Saturn and Mega CD already use.

## Status: implemented, and working on real discs of both consoles

Implemented 2026-08-12. `tools/disctocdb.py` builds the table, `read_toc_shape()` and
`disc_toc_title()` read the disc and the table, and `disc_shape_identity()` decides what the
disc is then called. Proven on a real PC Engine CD disc end to end:

    2|4|22:221262|Akumajou Dracula X - Chi no Rondo
    asking for a disc scan for 22:221262 (system 114, as "Akumajou Dracula X ...")
    disc scan stored: classicui/discart/22_221262.png (30550 bytes)

### Neo Geo CD, added the same day

The mechanism is per-console-agnostic, so Neo Geo CD needed only its datfile - and two
measurements to confirm that was really all:

**One flat table serves both, measured not assumed.** PC Engine CD yields 492 keys, Neo Geo
CD 103, and the merge yields **595** - so not one key is shared between the two systems and
no name is lost to the merge. The key therefore needs no system field, and the firmware that
reads it needed no change at all. (Worth re-running if a third system is added: a
cross-system collision is not an error, it is written `?` and refused, but it would cost two
real names.)

**Redump's `A ~ B` titles hit ScreenScraper as they stand.** Neo Geo CD releases are named
`Garou Densetsu 2 ~ Fatal Fury 2` in the dat, which is a Redump convention and not obviously
a searchable string. Asked on systemeid 70:

    romnom "Garou Densetsu 2 ~ Fatal Fury 2"     ->  HIT  id 38358  Fatal Fury 2
    romnom "Garou Densetsu 2"                    ->  miss
    romnom "Fatal Fury 2"                        ->  HIT  id 38358
    romnom "Samurai Spirits ~ Samurai Shodown"   ->  HIT  id 38392
    romnom "Metal Slug"                          ->  HIT  id 107062
    romnom "King of Fighters '94, The"           ->  HIT  id 38366

So the tilde form is kept verbatim. Note the second line: splitting on the `~` and taking
the *first* half - the obvious tidy-up, and the one half a reader would assume is canonical -
**misses**, because ScreenScraper indexes the Western name. The fuzzy matcher finds the
English half inside the whole string, so doing nothing is strictly better than tidying.

### A defect this exposed, in the half PC Engine CD cannot reach

The first implementation set the serial on an unmatched shape and left the label alone.
Invisible on PC Engine CD, which has no filesystem and so no label to leave - and wrong on
Neo Geo CD, which has a volume label that is a house code as often as a name. That label
then satisfied `disc_scrape_name()`'s rule that "a label beside a serial containing `:` came
out of the table", and a disc called `DD_CD` would have gone to the database as a game name:
a miss against the scarce unmatched allowance, or a confidently wrong cover.

The decision now lives in `disc_shape_identity()` in the common half of the file rather than
inline in the helper, precisely so the harness can hold it - the bug survived review because
it sat in the device-only code the harness cannot compile.

### What still needs a disc

Everything above about Neo Geo CD is offline: the table, the systemeid and the romnom
shapes are measured, but no Neo Geo CD disc has been in the drive. Two things are therefore
unproven until one is: that the drive's TOC totals agree with the dat for this console as
they do for PC Engine CD, and that detection fires (`chome_disc.cpp:212`, the `NGCD` volume
identifier, or `:222`, an `IPL.TXT` in the root).
