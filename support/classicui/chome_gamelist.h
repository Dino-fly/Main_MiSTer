/*
  Classic Home - gamelist.xml, the art a player already scraped.

  gamelist.xml is the EmulationStation metadata file, and every other front-end
  worth interoperating with reads or writes it: stock EmulationStation, RetroPie,
  Batocera, Recalbox, ES-DE, and the scrapers people actually use (Skraper,
  Skyscraper). A card that has been scraped once on a PC therefore already carries
  a description of where each game's pictures are - so the only thing standing
  between that scrape and our shelf is reading it.

      https://github.com/Aloshi/EmulationStation/blob/master/GAMELISTS.md
      https://github.com/batocera-linux/batocera-emulationstation/blob/master/GAMELISTS.md

  What that file says, and what this reads:

    - It lives in the system's ROM folder: games/<System>/gamelist.xml. (ES also
      looks in ~/.emulationstation/gamelists/<system>/ and /etc/..., neither of
      which exists on a MiSTer card, so neither is looked for.)
    - Root is <gameList>, whose children are <game> and <folder>. Only <game> is
      read: a <folder> names a directory, and the shelf has no card for one.
    - <path> names the ROM, <image>/<thumbnail>/<boxart>/... name pictures, and
      <name> is what the scraper decided the game is called - which is a file name
      as well as a title, because the media layouts below are keyed on it.
    - A picture path is absolute, or relative to the ROM folder and conventionally
      starts with "./", or starts with "~/" for the user's home directory. The
      first two are resolved; "~/" is not, since MiSTer has no meaningful home and
      an ES tree copied from a PC would not be at the same place anyway.
    - Text is entity-escaped (pugixml writes &amp; and friends, not CDATA), so
      &amp;/&lt;/&gt;/&quot;/&apos; and numeric references are decoded.

  ES-DE is the exception worth knowing about: it writes gamelist.xml but does not
  put media paths in it at all, matching media to ROM names under
  downloaded_media/<system>/covers/ instead. Its gamelists are still read here -
  they name no pictures, only the <name> that chome_art.cpp then looks for on disk,
  and past that the lookup falls through to the layers in chome_art.cpp, one of
  which is the same filename-matching idea.

  Deliberately bounded, because this is a user-supplied file of unknown size on a
  card we did not write: a file larger than GL_MAX_BYTES is not opened at all, the
  entry table and the string arena have hard caps, and a parse error throws away
  everything that file contributed rather than trusting half of it. Any of those
  means "no art from the gamelist" - never a crash, and never a stall that grows
  with the file.

  Parsing is lazy and per system, done the first time a card from that system asks
  for art, not during the library scan: the scan is already the slow part of a cold
  boot, and a player who never scrolls to the Mega Drive shelf never pays for its
  gamelist.
*/

#ifndef CHOME_GAMELIST_H
#define CHOME_GAMELIST_H

// Files above this are ignored outright - a renamed ROM or disc image is the case
// that matters, and no real gamelist comes close (a 3000-game one measures ~1 MB).
#define GL_MAX_BYTES    (16 * 1024 * 1024)

// Caps across all systems together, not per system.
#define GL_MAX_ENTRIES  4096
#define GL_ARENA_MAX    (256 * 1024)

/*
  Absolute path of the picture gamelist.xml names for this game, when there is one
  and it is really on the card. `relpath` is the game's path relative to the
  system's games dir, with any archive member already cut off (see rom_relpath() in
  chome_art.cpp). Returns 1 and fills `out`, or 0.

  The first call for a system reads that system's gamelist.xml; later ones are
  table lookups.
*/
int gl_art(int sysidx, const char *relpath, char *out, int len);

/*
  What gamelist.xml *calls* this game - its <name> - or 0 when it names none. Same
  `relpath` as gl_art(), same lazy load.

  Not for display. The shelf builds its own titles out of the file name and has no
  use for a scraper's idea of one; this exists because <name> is a file name in the
  layout ScreenScraper's own tools write, where the pictures beside the ROMs are
  called after the game rather than after the ROM:

      <system>/media/box2d/<name>.png
      <system>/media/screenshot/<name>.png

  So a card scraped by Skraper, Recalbox or ES into that layout is unreadable without
  this mapping - the file on it is "Sonic The Hedgehog 2.png" and the ROM is
  "Sonic The Hedgehog 2 (Europe).md". See find_local_art() in chome_art.cpp.
*/
int gl_name(int sysidx, const char *relpath, char *out, int len);

// Drops every parsed gamelist, so a rescan picks up a re-scrape.
void gl_forget();

/*
  Diagnostics, and what the tests assert on:
    gl_loaded()  1 once this system has been looked at
    gl_count()   entries taken from this system's gamelist - every <game> that named a
                 usable picture, a <name>, or both. An entry that named neither is not
                 one, since there would be nothing to answer with.
    gl_rejected() 1 when this system had a gamelist we refused: malformed, too
                 large, or over a cap. Distinct from "no entries", which is also
                 what a valid ES-DE gamelist gives.
*/
int gl_loaded(int sysidx);
int gl_count(int sysidx);
int gl_rejected(int sysidx);

#endif
