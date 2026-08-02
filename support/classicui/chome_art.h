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

#endif
