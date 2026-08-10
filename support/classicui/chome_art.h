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
  Dinofly's.

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
  own.

  It now has one, and it is deliberately the narrowest switch that answers the complaint:
  classicui_ss_replace_pack retries ScreenScraper for the covers this front-end fetched
  from the libretro pack itself, and for nothing else - never a gamelist.xml cover, never a
  scrape a player did with another tool, never a pack they installed by hand. The ladder
  above is untouched by it: rung one still wins, the cover on the card is still decoded and
  drawn first, and the retry happens afterwards from an idle shelf. See art_pack_marked()
  below, and pack_retry_step() in chome_art.cpp for the ordering.
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

/*
  The same answer, remembered on the card instead of in a slot.

  ---------------------------------------------------------------------------
  The bug this exists to fix, because it is not visible from the code.
  ---------------------------------------------------------------------------

  art_ss_absent() above is a field on an in-memory slot. It is correct, it is guarded by
  ss_verdict(), and it is thrown away by every core change - which on this device is
  every time the player launches a game. So on the owner's 1469-game shelf the front-end
  asked the database about every coverless game, learned that it had no cover, and then
  asked again on the next boot, and the next, and the next. Nothing was ever written
  down.

  What that costs is not the daily request budget - 200 of 20000 used on 2026-08-05, not
  close - but the *unmatched* one, which is a tenth the size and which a filename matcher
  against regional variants, hacks and homebrew spends far faster. Spent, the account is
  refused for the rest of the day: "Faite du tri dans vos fichiers roms et repassez
  demain". That is what one request from this machine got on 2026-08-05, and this store
  is why it will not get it again.

  ---------------------------------------------------------------------------

  Keyed on the *query* rather than on the item, deliberately: "<systemeid>/<rom name>" is
  what was actually asked, so it survives a rescan renumbering the shelf, it is stable
  across the two callers, and the disc dialog - which has no shelf slot at all and whose
  in-RAM tried-list is eight entries cleared by every core change - can use the same
  store as the shelf.

  Only ever record where ss_verdict() is true. Everything in the note on art_ss_absent()
  applies here and harder: a session flag set wrongly costs a session, a line in this file
  set wrongly costs a week.
*/

/*
  How long a remembered miss stands, in days.

  A week rather than forever, and rather than a day. Forever is wrong because
  ScreenScraper is a live database that people add to - a game with no cover today may
  have one next month, and a store with no expiry would mean this front-end never found
  out. A day is wrong because it would put us back to re-asking about the whole shelf
  every week's worth of boots, which is the bill this exists to stop paying.

  Seven days is the owner's call and it prices out sensibly: 1469 unmatched games asked
  once a week is ~210 a day against a 2000-a-day unmatched allowance, and that is only if
  the whole shelf misses and the whole shelf is scrolled.
*/
#define ART_SS_MISS_DAYS 7

/*
  And how long the record itself is kept, which is a longer thing than the answer it holds.

  Past ART_SS_MISS_DAYS a miss stops counting: the game is on the ScreenScraper rung again,
  because the database is one people add to. But the record stays, saying nothing except
  "this was asked about once", until this second horizon.

  That is not tidiness, it is the whole of the fairness rule in art_step(). A re-ask must
  not go out ahead of a game nobody has ever asked about - a first ask is strictly likelier
  to match, and the unmatched allowance is the thing being spent - and "never asked about"
  cannot be told from "asked about, aged out this morning" if the evidence was deleted the
  moment it stopped counting. So the store keeps it for four windows, and then genuinely
  forgets: a month on, a game that was asked about once and left alone is as good as new,
  and the shelf may treat it as never-asked without anybody being able to tell the
  difference.

  A month of records for a 1469-game shelf is at most 1469 of the 4096 the cap allows, so
  keeping them costs nothing that was not already budgeted for.
*/
#define ART_SS_MISS_KEEP_DAYS (4 * ART_SS_MISS_DAYS)

/*
  And the ceiling on the store, which is a real one rather than a formality.

  4096 is comfortably past the owner's 1469 and past any shelf this front-end indexes
  (the library index itself is bounded well below it). Full, the oldest entry is dropped
  to make room - oldest rather than newest, because the oldest is the one closest to
  expiring anyway and the newest is the one we just paid a request to learn.

  A dropped entry costs one request, once, at some point in the next week. It is not a
  correctness problem, which is why the cap can be a plain number rather than a policy.
*/
#define ART_SS_MISS_MAX 4096

/*
  1 when the database has been asked this exact query inside the window and had nothing.
  systemeid and name are the two the query is built from; either missing is 0.
*/
int art_ss_miss_known(const char *systemeid, const char *name);

/*
  Write one down. Appends to the card immediately rather than at shutdown: the firmware is
  killed rather than exited on every core change, so anything held for a tidy save at the
  end is a save that never happens.
*/
void art_ss_miss_record(const char *systemeid, const char *name);

/*
  1 when the database was asked this query, had nothing, and that answer has since aged out
  of the window: the game is askable again, and it is *not* a game nobody has ever asked
  about.

  The distinction the fetch order is built on. See ART_SS_MISS_KEEP_DAYS above for why the
  record is still there to be asked about, and queue_best() in chome_art.cpp for what is
  done with the answer. Never a reason not to ask - only a reason to ask later than a game
  that has never been asked at all.
*/
int art_ss_miss_stale(const char *systemeid, const char *name);

/*
  Drop the in-memory copy so the next question re-reads the card. This is how the harness
  simulates a restart, which is the only thing about this store worth testing and the only
  thing the old code got wrong.
*/
void art_ss_miss_reload();

/*
  How many entries count as misses, how many records are held at all, and where the file is.
  For the harness and for anyone diagnosing a shelf that has stopped scraping.

  The two counts differ by the records that have aged out of the window and not out of the
  store - the games that are askable again, and that a re-ask must queue behind.
*/
int art_ss_miss_count();
int art_ss_miss_held();
const char *art_ss_miss_path();

/*
  ---------------------------------------------------------------------------
  And the other store: covers that came from the libretro pack.
  ---------------------------------------------------------------------------

  A cover fetched from the thumbnail pack is written into the same file the ScreenScraper
  one would have been, and from that moment nothing on the card says where it came from -
  so a player who has entered their own ScreenScraper credentials, and whose card was
  filled from the pack before they did, has no way to ever get the art they asked for. The
  answer is permanent because a fetched cover is found at rung one for ever.

  So the provenance is written down: <root>/classicui/art-from-libretro.txt, keyed on the
  same "<systemeid>/<romnom>" query key the miss store uses - which is what makes it
  survive a rescan renumbering the shelf - and carrying its own explanation at the top of
  the file, because a stray file on somebody's SD card that does not say what it is for is
  a support question.

  Three things this is not, and each of them was a decision:

    not a sidecar. <cover>.png.from beside every fetched cover would put a second file in
    every Named_Boxarts folder on the card, in the very layout the community art packs use
    and people copy around by hand. One file in our own directory is one thing to explain
    and one thing to delete.

    not part of the miss store. "They have not got it" and "we have something, from
    somewhere else" are opposite claims about a game, and a single store would sooner or
    later have one read as the other. Separate files, separate questions, and no function
    that answers both.

    not a source of requests. Marking is free and silent. Nothing is re-asked because a
    mark exists unless the player sets classicui_ss_replace_pack, which is off, is not
    written by anything on their behalf, and is the only thing that turns these marks into
    a single retry each. Yesterday's work was about not burning somebody's allowance; a
    feature that quietly re-asked about every pack cover on the card would have undone it.

  Only covers this front-end downloaded itself are marked. A pack a player installed by
  hand is indistinguishable from a scrape they did themselves, and guessing would mean
  overwriting art they chose.
*/
int art_pack_marked(const char *systemeid, const char *name);
void art_pack_mark(const char *systemeid, const char *name);
void art_pack_forget(const char *systemeid, const char *name);
void art_pack_reload();
int art_pack_count();
const char *art_pack_path();

/*
  A libretro pack download for one shelf item has landed at `src`: put it on the card and
  write down that this is where it came from. 1 when the file got there. `src` is consumed
  either way.

  Public for the reason art_ss_settle() is - it is the half of that fetch a host test can
  reach. The curl cannot be run there, so the harness writes the file the pack would have
  sent and hands it in, which is the live path rather than a model of it.
*/
int art_pack_landed(int item, const char *src);

/*
  How many pack covers are waiting for the retry the player switched on.

  Zero in every build with classicui_ss_replace_pack off, which is every default build, and
  that is the property worth being able to assert: the marks exist, they cost nothing, and
  nothing is queued because of them.
*/
int art_pack_retry_pending();

// How many times the cover ladder has reached its ScreenScraper rung this session,
// refusals included. The same shape and the same reason as disc_art_asks(): no request is
// ever made in the harness, and whether the rung is reached at all - and in what order
// against the libretro one - is precisely the shape of this feature.
unsigned art_ss_asks();

/*
  And which item the rung was last reached for, or -1.

  The counter above says how much of the allowance the shelf would have spent; this says on
  what, and it is here because the *order* is the feature this time. Two games both on the
  ScreenScraper rung, one never asked about and one whose miss aged out last night: which of
  them the next request would be about is the whole of the fairness rule, it shows in no
  pixel and no count, and this is the smallest thing that reveals it.
*/
int art_ss_last_ask();

/*
  Which queued item the next art_step() would take, or -1 for an empty queue.

  Pure, and cheap: it reads the queue and the slots, stats nothing and decides nothing. The
  ordering is three keys deep now - a re-ask last, then the distance from the selection,
  then the order things were asked for - and exactly none of that is visible from a
  screenshot, so it is asserted here against the same function the step itself pops with.
*/
int art_queue_next();

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
