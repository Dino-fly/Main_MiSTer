/*
  Classic Home - cover art: lazy loading, LRU cache, optional online fetch.

  Decoding happens on the main thread, at most one image per frame. Imlib2 keeps
  its state in a global context, so decoding from a worker thread would race with
  the wallpaper code in video.cpp. One 228x167 PNG per frame is enough to fill a
  shelf as the user scrolls, which is what "lazy" should feel like.

  Network fetch runs as a forked curl, one at a time, polled without blocking.
  It is opt-in (CLASSICUI_ARTFETCH) because it necessarily sends game names to a
  third-party server.

  Local layout follows the libretro thumbnail convention, which is what the
  community art packs already use:
      <artdir>/<System Name>/Named_Boxarts/<ROM name>.png
  Fetched art is written into that same layout, so a fetch permanently populates
  the local pack and the next boot needs no network.

  ---------------------------------------------------------------------------

  The ladder, in the order it is walked. One rung per pass of art_step(), and
  art_next_source() is the whole of it as a single answer - the step consults that
  function rather than repeating the order, so there is one copy of it to be wrong.

    1. a file already on the card, through find_local_art(): whatever gamelist.xml
       names, then the scraper media folders beside the ROMs, then our own artdir,
       then next to the ROM. This is the cache and it is the top of the ladder.
    2. ScreenScraper, when the player has turned it on and given it an account.
    3. the libretro thumbnail pack.
    4. nothing: a plate in the system's colour, ART_MISSING.

  Two things about that order are decisions rather than accidents, and both were
  Derek's.

  ScreenScraper above libretro. It used to be the other way round - spend the free
  repository, keep the metered account for what it could not supply. But a player who
  has entered their own ScreenScraper credentials has said which database they want
  their shelf built from, and filling the card from libretro first means they get the
  answer they did not ask for, permanently, because a fetched cover is written to the
  card and never looked for again.

  The card above both of them. Taken literally, "use ScreenScraper for all art" would
  mean re-fetching over covers that are already there - including the ones a player
  scraped themselves with another tool and pointed a gamelist.xml at, which
  find_local_art() deliberately honours above everything else. So "first priority"
  applies to art that has to be *fetched*, and a cover already on the SD is an answer,
  not a gap. Re-scraping over the card would be a separate feature with a switch of its
  own, and nobody has asked for one.
*/

#ifndef CHOME_ART_H
#define CHOME_ART_H

#include <inttypes.h>

#define ART_NONE     0   // not looked at yet
#define ART_READY    1
#define ART_MISSING  2   // no local file, and fetch disabled or exhausted
#define ART_PENDING  3   // queued for decode
#define ART_FETCHING 4

void art_init(int card_w, int card_h);
void art_shutdown();

// Ask for a game's art. prio is the distance from the selection: lower wins.
void art_request(int item, int prio);

// Does at most one decode and services the fetch queue. Call once per frame.
void art_step();

/*
  How many decodes are queued for the art_step()s to come. Zero means the next pass will
  spend nothing on covers; nonzero means each pass is about to pay for an image decode.

  Exported for the disc's animation and not for the queue's own sake: a decode is
  milliseconds, which is the scale of a whole frame, so the disc backs its rotation steps
  off while these are pending - see disc_step_fine() in chome_ui.cpp. A depth rather than
  a flag so a caller could weight by it, though today anything nonzero reads as "busy".
*/
int  art_pending();

// Returns the bitmap, or 0. w/h are the decoded size.
const uint32_t *art_get(int item, int *w, int *h);
int  art_state(int item);

int  art_fetch_active();
int  art_cache_count();
int  art_cache_bytes();

/*
  Which rung of the ladder this game is on: what art_step() would do for it next, without
  doing any of it.

  Exposed because the ordering *is* the feature, and none of it shows in a pixel. A test
  that could only see the end state would pass just as happily on a ladder walked in the
  wrong order - a game with a cover on the card looks identical whether we used that cover
  or fetched a new one over it - so the order is asserted here, against the same function
  the step itself calls. There is one ladder, and this is it.
*/
#define ART_SRC_NONE     0   // nothing left to try: a plate, and ART_MISSING
#define ART_SRC_LOCAL    1   // a file already on the card
#define ART_SRC_SS       2   // ask ScreenScraper
#define ART_SRC_LIBRETRO 3   // the libretro thumbnail pack
int art_next_source(int item);

/*
  1 when ScreenScraper has answered about this game and had no cover for it, so it will
  not be asked again this session.

  The half of the crux that lives per-game. Set only where ss_verdict() is true - the
  database looked and came back with nothing, or with media but no cover among them. A
  refusal to answer at all is never this: a quota that ran out while the shelf was being
  scrolled must leave every game it touched exactly as unasked as it found them, or one
  afternoon at the wrong end of the allowance blanks a library for good. The other half of
  the crux is ss_hold_reason(), which holds the module rather than the game.
*/
int art_ss_absent(int item);

// How many times the cover ladder has reached its ScreenScraper rung this session,
// refusals included. The same shape and the same reason as disc_art_asks(): no request is
// ever made in the harness, and whether the rung is reached at all - and in what order
// against the libretro one - is precisely the shape of this feature.
unsigned art_ss_asks();

/*
  Read a jeuInfos reply that has landed for one shelf item and settle what it means:
  parse it, fold the outcome and the quota counters into the module's state, remember a
  miss if and only if the reply was a verdict, and keep the cover URL it named for the
  download stage. Returns 1 when there is a cover to fetch.

  The reply is unlinked before this returns, whatever it contained. It is a list of URLs
  with our devid, our devpassword and the player's ScreenScraper password in every one of
  them, and it lives in tmpfs for exactly as long as it takes to read - see the note on
  credentials in chome_ss.h.

  Public for the same reason disc_art_scale() is: it is the half of the fetch a host test
  can reach. The two curls cannot be run there and must not be - the account allows one
  thread and the fixtures point at .invalid so that a bug cannot become a request - but
  everything that decides whether a miss is remembered happens in here, on a file. So the
  harness writes the replies a server would have sent and drives this directly, which is
  the live path rather than a model of it.

  Makes no request of its own and needs no network: it reads a file that is already there.
*/
int art_ss_settle(int item, const char *reply_path);

// Suspend-point thumbnails, written next to the savestate by process_ss().
// Small ring cache keyed by path; decoded on demand at w x h.
const uint32_t *art_thumb(const char *fullpath, int w, int h);

// Forget every decode of this file - call it after rewriting one.
void art_forget(const char *fullpath);

/*
  Where a physical disc's scan lives, keyed on the disc identity rather than a filename
  because a disc has not got one. Fills `out` and returns 1 for any usable key, whether
  or not the file exists yet - the caller decides what an absent file means.

  This is the seam between fetching that picture and drawing it: the fetcher writes this
  path, and the disc dialog reads it through art_thumb(), which already caches decodes
  by path and size.
*/
int disc_art_path(const char *key, char *out, int len);

/*
  How big a disc scan is stored, on its long side.

  Measured source: ScreenScraper's support-2D for a PlayStation game is a 600x600 RGBA
  PNG of 417 KB, the disc a 588 px circle centred with a 6 px margin, corners fully
  transparent, and the hub hole genuinely transparent out to a radius of 45 px - 15% of
  the disc's own radius.

  144 px is where the title printed round the disc stops being a smudge and starts being
  legible on a TV, and it is ~35 KB as a PNG. The 417 KB original is not kept: it is
  eight times the size of every other picture the card holds for a game, for a sprite
  that is drawn at a fraction of it, on a device whose art cache is 24 MB and whose
  storage is somebody's SD card.
*/
#define DISC_ART_PX 144

/*
  Ask for the disc scan for a disc identity, fetching it if it is not on the card yet.

  `key`    the disc identity - the same string that names its savestates, see
           PHYSICAL_DISC_IDENT_FILE. For a PlayStation disc that is its serial, which
           is also where the region preference comes from.
  `sysid`  the chome_lib system id to scrape as ("psx", "md", ...).
  `romnom` what to ask the database for by name, or 0 to ask by the key.

  Returns 1 when the file is already there or a fetch is under way for this key, 0 when
  nothing will happen - which is the answer in every build we ship, since this needs
  both ss_enabled() and classicui_artfetch and ss_available() is compile-time false.

  Never blocks. The request forks curl and returns; the reply is parsed and the picture
  scaled from art_step(), one stage per frame, the same discipline cover art uses. Ask
  again on a later frame and read the result through art_thumb(disc_art_path(key)).

  Only one attempt per key per session, for the same reason cover art only tries once: a
  disc the database does not have would otherwise be asked for on every frame the dialog
  is open.
*/
int disc_art_request(const char *key, const char *sysid, const char *romnom);

/*
  1 while a *disc scan* is being fetched, for a spinner and to keep callers from piling up.

  One fetcher serves both the disc scan and ScreenScraper covers now, and this deliberately
  answers only for the disc: a cover being fetched for the shelf must not put a "fetching the
  disc scan" spinner over a dialog that is going to go on drawing the generated disc face.
  The one-download-at-a-time rule is enforced inside chome_art.cpp against all kinds, which
  is where it belongs - a caller cannot get that wrong by reading this the obvious way.
*/
int disc_art_active();

/*
  1 once for every scan that has just been written to the card, and 0 afterwards - the
  same shape as disc_take_dirty(), and for the same reason.

  Nothing about a picture finishing comes through a keypress. The front-end repaints
  only when something tells it to, so without this the dialog goes on drawing the
  generated face until the player presses something or the spin timer happens to come
  round: the scan would be on the card, correct, decodable, and invisible. That exact
  omission is what made the disc badge appear and vanish without a repaint.

  The alternative - the drawing code asking whether the file has turned up yet - would
  be a stat of the card on every frame for an event that happens at most once per disc
  per session. The scale is where the answer is already known, so the answer is raised
  there and this hands it over once.

  Not keyed on which disc: it is one repaint at most once per disc, and a repaint of a
  screen that does not happen to be showing that disc costs a frame nobody sees. A key
  here would be a second thing to keep in step with disc_art_path() for no gain.
*/
int disc_art_take_ready();

/*
  How many times anything has asked for a disc scan this session, refusals included.

  For the harness, which cannot see the fetch itself: no request is ever made there -
  classicui_artfetch is off, so disc_art_request() returns before it forks anything -
  and yet whether the ask happens once, when the disc is identified, or over and over
  from the dialog's draw is precisely the shape of this feature. Neither shows in a
  pixel, and a counter of calls is the smallest thing that tells them apart.
*/
unsigned disc_art_asks();

/*
  Scale a fetched disc scan down to DISC_ART_PX and write it as the sprite the dialog
  draws. Returns 1 on success.

  Public because it is the half of the fetch that can be tested without a network, and
  because getting it wrong is invisible until somebody looks at a real disc on a real TV:
  the transparent pixels in these scans are RGB (0,0,0), so a plain RGBA box average
  drags every partly-covered pixel toward black and the result has a dirty grey rim
  round the disc and a smudged ring round the hub. The colours are therefore
  premultiplied by alpha before they are summed and divided by the sum of the alphas,
  not by the pixel count.
*/
int disc_art_scale(const char *src_png, const char *dst_png);

#endif
