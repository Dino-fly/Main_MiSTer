/*
  Classic Home - pixel drawing layer.

  Everything is composed into a cached RAM buffer and only the damaged rows are
  copied into the menu framebuffer, which is an uncached /dev/mem mapping
  (shmem.cpp) shared with the FPGA scaler. Drawing straight into it a pixel at a
  time would be far too slow; see docs/CLASSIC_UI_PLAN.md section 7.3.

  Colours are 0xAARRGGBB, matching Imlib2's DATA32 convention, which is what the
  existing wallpaper code already writes into these buffers.
*/

#ifndef CHOME_GFX_H
#define CHOME_GFX_H

#include <inttypes.h>

// Acquire the framebuffer and compose buffer for this frame.
// Returns 0 when the framebuffer is unavailable (core without HPS fb support).
int  gfx_begin();

// Copy damaged rows to the back buffer and page-flip.
void gfx_end();

void gfx_shutdown();

int  gfx_w();
int  gfx_h();

// Damage tracking. gfx_damage_all() is implied by a resolution change.
void gfx_damage(int x, int y, int w, int h);
void gfx_damage_all();

// Height in rows of the damage submitted this frame, for telemetry.
int  gfx_damage_rows();

/*
  Region clip, for partial repaints.

  Between gfx_clip_set() and gfx_clip_clear() every primitive draws - and records
  damage - only inside the rectangle, and anything that misses it rejects in a
  comparison or two. A partial repaint is therefore the caller's *normal* compose,
  replayed under a clip: the layers underneath the region are reconstructed by the same
  code in the same order as a full frame, rather than cached or guessed at, and
  gfx_end() copies only the region (unioned with the previous frame's damage, exactly
  as always - which is what keeps the two alternating framebuffers coherent).

  Clear the clip before gfx_end(). Frames composed under a clip are accounted
  separately in the repaint-cost log, tagged "partial".
*/
void gfx_clip_set(int x, int y, int w, int h);
void gfx_clip_clear();

/*
  Repaint cost instrumentation. Call at the start of composing a frame; gfx_end() closes
  the measurement and logs a summary every couple of hundred frames.

  This exists because repaint work is invisible in CPU time on this board: the firmware
  busy-polls and sits at 100% of one core whether it draws or not, measured both ways on
  the device. Timing is the only way to see it.
*/
void gfx_stat_compose_begin();

void gfx_fill(int x, int y, int w, int h, uint32_t col);
void gfx_frame_rect(int x, int y, int w, int h, uint32_t col, int t);
void gfx_blend(int x, int y, int w, int h, uint32_t col, int alpha);

// 50% checkerboard scrim: dims a region without introducing a new colour.
void gfx_scrim(int x, int y, int w, int h, uint32_t col, int step);

/*
  Activity indicators. Both are driven by a millisecond clock, not by a frame count -
  frames here are only drawn when something changed, so a frame counter would make the
  same animation run at a different speed depending on what else was moving.

  GFX_SPIN_MS is also the rate at which a screen showing one has to repaint, and it is
  a tenth of a second rather than a frame time on purpose: the ring has eight
  positions, so painting faster than it moves is work with nothing to show for it, and
  a full repaint of this UI is not free.
*/
#define GFX_SPIN_DOTS 8
#define GFX_SPIN_MS   100UL
#define GFX_SWEEP_MS  900UL

// A ring of dots with a bright head and a fading tail: something is happening and has
// not stopped happening. Drawn around whatever sits at cx,cy - an icon, usually.
void gfx_spinner(int cx, int cy, int r, int dot, unsigned long ms, uint32_t hot, uint32_t cold);

/*
  A spinning disc, for the optical drive.

  Drawn rather than blitted, so there is no icon asset and nothing to license: a
  32x32 sprite of square cells, hard edged, no antialiasing - the pixel-art CD look
  rather than a smooth circle. A dark outer edge, a bright rim, the data area split
  into `nbands` wedges that sweep round as time passes, a clear inner ring, a hub ring
  and a spindle hole. Sweeping colour is what makes it read as a disc catching the
  light; a plain circle spinning is indistinguishable from one sitting still.

  32x32 rather than a 16x16 grid scaled up, which is a different thing: doubling the
  cells of a small sprite gives bigger blocks and no more information. Twice the grid
  buys the outer edge, a rim that is thin in proportion instead of a quarter of the
  radius, a hub distinct from the hole, and the angular resolution for a dozen wedges.

  `r` sets the cell size: cells are r/16 pixels square, so r=16 gives single-pixel
  cells (a 32px icon) and r=32 gives 2x2 (a 64px one). Pass a multiple of 16 or the
  cells come out fractional and the look is lost.

  `outline` non-zero draws a ring two cells thick just outside the disc, which is how
  the front-end shows the disc has focus. It grows the sprite by two cells rather than
  putting anything behind it: a filled plate covered the shelf title at 240p. Two cells
  because one cell is one pixel at 240p, and a one-pixel ring was invisible on a real
  TV. 0 for no ring.

  `step` is the rotation, 0-63, and the caller owns it. This does *not* derive the angle
  from a clock and a period, which is how it used to work and which was wrong: the angle
  came out of `ms % period_ms`, so changing the period moved the *angle* as well as the
  speed and the disc visibly teleported. At ms=10000 a 4s period gives step 32 and a
  400ms period gives step 0 - half a turn, instantly, every time it gained focus.

  So the caller accumulates phase instead, which is also what makes a smooth speed change
  possible at all. See disc_step() in chome_ui.cpp.

  Repaint at GFX_SPIN_MS like the other animations; 64 positions per turn.
*/
/*
  One full turn, per state. These are chosen against the repaint rate, not picked for
  feel alone: there are 64 positions in a turn, and a disc that advances more than about
  four of them between repaints strobes instead of spinning. At GFX_DISC_MS these give
  roughly 4, 2 and 1 positions per frame.

  The first attempt used 400ms for focus, which at a 100ms repaint was sixteen positions
  a frame - a disc that looked like it was juddering rather than turning quickly.
*/
#define GFX_DISC_FOCUS_MS 800UL
#define GFX_DISC_FAST_MS  1500UL
#define GFX_DISC_SLOW_MS  4000UL

// One breath of the focus ring's pulse; see disc_focus_col() in chome_ui.cpp. Here
// with the disc's other periods so the harness can park the clock on its trough.
#define GFX_DISC_PULSE_MS 1200UL

// Repaint interval while a disc is on screen. Faster than GFX_SPIN_MS because the disc
// moves further per frame than the activity ring does.
#define GFX_DISC_MS       50UL

void gfx_disc(int cx, int cy, int r, int step,
	const uint32_t *bands, int nbands, uint32_t rim, uint32_t ring, uint32_t hole,
	uint32_t outline);

// Named steps as a row of boxes: `done` behind us, the one being worked on sweeping,
// the rest empty. See the comment in chome_gfx.cpp for why the active one sweeps
// rather than creeping forward, and why `live` has to be told rather than assumed.
void gfx_track(int x, int y, int w, int h, int nseg, int done, int live, unsigned long ms,
	uint32_t fill, uint32_t track, uint32_t glow);

// Text, in MiSTer's 8x8 OSD ROM font (charrom.cpp), at integer scales only.
// Codes 1..4 are up/down/left/right arrows, which the ROM font lacks.
#define CH_UP    "\x01"
#define CH_DOWN  "\x02"
#define CH_LEFT  "\x03"
#define CH_RIGHT "\x04"

#define GLYPH_W 8

void gfx_text(const char *s, int x, int y, int scale, uint32_t col, uint32_t shadow);
void gfx_text_c(const char *s, int cx, int y, int scale, uint32_t col, uint32_t shadow);
int  gfx_text_w(const char *s, int scale);

// Truncate to fit maxpx, appending '>' when clipped. Returns a static buffer.
const char *gfx_clip(const char *s, int scale, int maxpx);

// Nearest-neighbour blit of an ARGB source. No filtering: at these scales
// integer-ish nearest keeps pixel art crisp and costs almost nothing.
void gfx_blit(const uint32_t *src, int sw, int sh, int dx, int dy, int dw, int dh);

#endif
