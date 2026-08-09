/*
  Classic Home - what the disc in the drive is actually called.

  A disc has no filename. Everything else on the shelf is named by the file it came
  from, which is why the library can show "Metal Gear Solid" for a .chd: somebody
  typed that name when they made the file. A pressed disc carries no such gift. What
  it carries is a serial - SLES-01506 - and that is what the disc prompt shows today,
  which is correct and useless in the same breath: it is the right identifier and
  nobody recognises it.

  So this is a lookup table, and nothing more ambitious than that:

      SLES01506  ->  "Metal Gear Solid"

  ---------------------------------------------------------------------------
  Offline, optional, and silent when it is not installed.
  ---------------------------------------------------------------------------

  Three constraints shaped the whole thing, and each of them rules out an obvious
  design:

  1. **No network, ever.** chome_ss.cpp already exists for the case where the card
     cannot answer a question, and it is compile-time inert for want of a devid.
     Nothing here may depend on it, or on curl, or on anything with a timeout: a
     player with no Wi-Fi must get the same title as a player with it. That means
     the mapping ships as a file on the card and is read from the card.

  2. **Not installed is the normal case.** Every user who has not gone and built the
     file - which is all of them on day one - must see exactly today's behaviour and
     no sign that anything is missing. Not a warning, not a slower first frame, and
     specifically not an open() per frame for a file that is not there. The absence
     is therefore *cached*: looked for once, and once the answer is "no", never
     looked for again until disc_titles_forget().

  3. **The whole table does not fit anywhere sensible.** PlayStation alone is around
     12,000 releases. Reading ~600 KB into RAM to answer one question per disc
     insertion, on a board where the art cache is already the biggest allocation in
     the front-end, is not a trade worth making - and parsing it on the thread that
     draws is the mistake chome_disc.cpp was built to avoid making with the drive.

  Hence: a sorted file, binary-searched in place, nothing held between calls but a
  handful of recent answers.

  ------------------------------------------------------------------ the file ---

      /media/fat/classicui/disctitles.txt

  Line-oriented, UTF-8-free ASCII, sorted, and readable by a human who wants to add
  their own line:

      #classicui-disctitles 1
      GMMK440700	Sonic CD
      SLES01506	Metal Gear Solid
      SLUS00594	Final Fantasy VII

  A tab between the key and the title. The first line is the format's magic and its
  version; it begins with '#', which sorts below every character a key can contain,
  so the search never has to know it is there - a probe that lands on it simply
  reads as "before everything" and moves on.

  Keys are **normalised**: upper-cased, with every character that is not A-Z or 0-9
  removed. SLES-01506 is stored as SLES01506, and the Mega CD header's
  "GM MK-4407 -00" as GMMK440700. That is not tidiness, it is the only way the two
  ends can agree: Redump writes a PlayStation serial with a dash, a Saturn one
  without, and a Sega product code with spaces whose count differs between pressings
  of the same game. Normalising both the stored key and the query removes every one
  of those disagreements, and it lets the search compare with a plain strcmp - which
  in turn is what makes "sorted" a well-defined instruction to the generator.

  Both ends of that agreement matter, so both are stated in one place. The generator
  is support/classicui/tools/disctitles.py; it must normalise and sort exactly as
  this file's tdb_normalise() and strcmp do, and it says so in its own header.

  Regional variants are deliberately *not* collapsed. SLES-01506, SLUS-00594 and
  SLPS-00123 are three serials, three rows, and possibly three different titles -
  which is right, because they are three different discs, and a player holding the
  European one should be told the name printed on the European one.

  ------------------------------------------------ which discs this helps today ---

  Be clear about this, because the table covers four systems and the firmware does
  not yet hand it four kinds of key:

    PlayStation   works. disc_serial_at() digs a real serial out of the filesystem,
                  and Redump's PlayStation set is ~10,600 serials deep. This is the
                  case the whole thing was built for.
    PC Engine CD  works only as far as the volume label goes. Those discs carry no
                  serial that disc_serial_at() looks for, so the key is the label,
                  and whether a label matches a Redump catalogue code is a per-disc
                  accident. Rows are generated for them anyway.
    Neo Geo CD    the same.
    Mega CD       generated for, and not yet reachable. The identifier is the
                  product code in the disc header at 0x180 ("GM MK-4407 -00"), which
                  physical_disc.cpp reads and chome_disc.cpp does not - it only digs
                  out PlayStation serials. Teaching disc_serial_at() to read that
                  header is a small change to identification and deliberately not
                  bundled in here; the table is already sorted and waiting for it.

  The generator writes rows for all four so that nothing has to be regenerated when
  that last gap is closed.

  ------------------------------------------------------------- what it is not ---

  Not a ROM database. It maps an identifier to a display string and knows nothing
  about regions, revisions, publishers or cover art - chome_art.cpp and the gamelist
  already own that layer, and both key off a filename that a disc does not have.
  Extending this into a metadata store would mean deciding what to do when it
  disagrees with gamelist.xml, and there is no good answer to that.
*/

#ifndef CHOME_TITLES_H
#define CHOME_TITLES_H

// As CH_TITLE_LEN in chome_lib.h, and for the same reason: a disc's name is drawn
// in the same places at the same size as a shelf card's, so it may as well be
// truncated at the same point rather than at a second arbitrary one.
#define DISC_TITLE_LEN 64

/*
  The title for a serial, product code or volume label, or 0 when we have none -
  which includes every case where the file is missing, unreadable, or not ours.

  `key` is taken raw, in whatever form the disc gave it: disc_serial() with its
  dashes, or disc_label() with its spaces. Normalisation happens in here, so no
  caller has to know the file's conventions.

  Safe to call from drawing code, and it is: disc_display_name() calls it every
  frame. The last few answers are cached, *including the misses*, so a disc that is
  not in the table costs one file read per insertion rather than one per frame.

  The returned pointer is owned by this module and survives a few further distinct
  queries before its cache slot is reused - which at two keys per disc is effectively
  for ever, but copy it if you intend to keep it across an eject.
*/
const char *disc_title_for(const char *key);

/*
  Forget both the cached answers and the verdict on the file itself.

  Two callers, one real and one hypothetical: the harness, which needs to install
  and remove the file inside one process, and any future "the player just copied the
  file onto the card" path. Without this, the first lookup's verdict of "there is no
  such file" is permanent for the life of the process - which is the intended
  behaviour and exactly why it needs an escape hatch.

  Nothing in the front-end calls it today, and the gap is benign rather than
  overlooked: every core switch re-execs the binary, so a table copied onto the card
  mid-session is picked up the next time a game is launched or left. The case that
  stays stale is "copied it and then did nothing at all", which is the case where
  nobody is looking.
*/
void disc_titles_forget();

/*
  Settle the verdict on the file now, rather than on the frame a disc is identified.

  Pure housekeeping: it opens the table, checks the magic, and closes it again. No
  answer is cached and no row is read, because there is no key to look one up by yet -
  what it removes is the *first* lookup having to discover whether there is a file at
  all, on the thread that draws, at the moment the player is already waiting.

  Two reasons it is worth the one open(). The verdict is sticky by design - see
  disc_titles_forget() - so the question is asked once per process either way, and this
  only moves when. And the "disc titles from ..." line it produces then lands at the
  point the drive is found instead of minutes later beside the first disc, which is
  what makes a log answer "was the table ever seen?" without a disc having to be in
  the machine to ask it.

  Called from disc_watch_start() once a drive has actually been found, so a card with
  no optical drive on it never opens the file - the same gate the rest of the disc
  layer sits behind.
*/
void disc_titles_preload();

#endif
