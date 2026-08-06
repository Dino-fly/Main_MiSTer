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

// Returns the bitmap, or 0. w/h are the decoded size.
const uint32_t *art_get(int item, int *w, int *h);
int  art_state(int item);

int  art_fetch_active();
int  art_cache_count();
int  art_cache_bytes();

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

// 1 while a disc scan is being fetched, for a spinner and to keep callers from piling up.
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
