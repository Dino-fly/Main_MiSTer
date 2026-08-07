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

  `r` is the radius in pixels and sets the cell size with it: cells are r/16 pixels
  square, so r=16 gives single-pixel cells (a 32px icon) and r=32 gives 2x2 (a 64px one).
  A multiple of 16 keeps every cell the same size, which is what the fixed sizes pass.

  Anything in between is still drawn at exactly that radius rather than rounded down to
  the multiple below - the grid is mapped onto the pixel box, so one row or column in
  every few comes out a pixel wider than its neighbours. That is what lets the focused
  badge breathe: at 240p a cell is one pixel, so a radius quantised to whole cells could
  only double. See gfx_disc() for the mapping and draw_disc_badge() for the breath.

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
  roughly 4, 2 and 1 positions per frame; at GFX_DISC_PART_MS, roughly 1.3, 0.7 and 0.3,
  which is under one position per frame and as smooth as 64 positions can be.

  The first attempt used 400ms for focus, which at a 100ms repaint was sixteen positions
  a frame - a disc that looked like it was juddering rather than turning quickly.

  The periods do not change with the repaint rate, and must not: the angle comes from
  accumulated phase against the wall clock, so drawing more often samples the same turn
  more finely instead of turning faster. That is the whole reason for the accumulator.
*/
#define GFX_DISC_FOCUS_MS 800UL
#define GFX_DISC_FAST_MS  1500UL
#define GFX_DISC_SLOW_MS  4000UL

// One breath of the focus ring's pulse; see disc_focus_col() in chome_ui.cpp. Here
// with the disc's other periods so the harness can park the clock on its trough.
#define GFX_DISC_PULSE_MS 1200UL

/*
  How far the focused badge swells over that breath, as the crest in sixteenths of its
  resting radius: an eighth. Here beside the period for the same reason - the rectangle the
  partial repaint clips to has to cover the largest size the badge ever reaches, and a test
  that could not work out what that size is could not check the one thing that matters.
  See draw_disc_badge() and disc_note_rect() in chome_ui.cpp.
*/
#define GFX_DISC_BREATH_16 2

/*
  Repaint interval while a disc is on screen. Faster than GFX_SPIN_MS because the disc
  moves further per frame than the activity ring does.

  Two rates, because the two repaints do not cost remotely the same. Measured on the
  device: a full frame is 5.1ms and the disc's own rectangle is 0.91ms. At 50ms the full
  frame was already 10% of the loop; 60fps of it would be 31%, which is why the disc did
  not animate at 60fps before the partial path existed. 60fps of the rectangle is 5.5%,
  so when the turning disc is the only thing repainting it can have the smooth rate, and
  when the frame has to be redrawn whole it keeps the cheap one.

  Which of the two applies is not a guess: it is the same question the dispatch at the
  bottom of chome_handle() asks before choosing render() or render_region().
*/
#define GFX_DISC_MS       50UL
#define GFX_DISC_PART_MS  16UL

void gfx_disc(int cx, int cy, int r, int step,
	const uint32_t *bands, int nbands, uint32_t rim, uint32_t ring, uint32_t hole,
	uint32_t outline);

/*
  The spindle hole, as a percentage of the radius, shared by everything that punches one.

  Not one of gfx_disc()'s 32nds, because it is a measurement rather than a proportion
  somebody chose: the disc scans this front-end fetches are transparent out to 15% of the
  radius (see disc_box_average() in chome_art.cpp, which has to weight by alpha because of
  it), and a generated disc and a photographed one have to put their hole in the same place
  or the dialog visibly shifts when a scan lands. gfx_disc()'s own sprite keeps its 6/32:
  its output is asserted byte-for-byte and one cell there is one pixel at 240p anyway.
*/
#define GFX_DISC_HOLE_PCT 15

// Blend b over a by t/255, in a buffer rather than on screen. Opaque out: the buffers this
// composes are blitted whole, not composited.
uint32_t gfx_mix(uint32_t a, uint32_t b, int t);

/*
  How much of the pixel at squared distance `d2` from the centre falls inside radius `rad`,
  both in whole pixels: 255 a pixel inside, 0 a pixel outside, a straight ramp between.

  The shared answer to "how does a disc's edge fade out". Exported because two places have to
  give the same one - gfx_disc_face() below, which draws the rings, and disc_rot() in
  chome_ui.cpp, which masks a photograph into the same rings - and the point of both going
  through one blit is that a scan landing must not change what kind of edge the dialog has.
  The generated disc anti-aliased and the scan hard-edged is the same seam as before with the
  two sides swapped, and the scan is the side Derek will actually be looking at.

  Costed for the caller that runs per rotation angle rather than per size: the two easy
  answers come from the squared distance and take no square root, so only the pixels a
  boundary passes through pay for one. That is the circumference and not the area - about 3%
  of the buffer with the four boundaries a masked scan has.
*/
int gfx_disc_cover(int rad, int d2);

/*
  The same disc, resolved to the display instead of to a 32-cell grid: one dia*dia ARGB
  buffer, cached, for a caller that means to rotate and blit it.

  gfx_disc() above is a sprite and stays one. At badge size - 32 pixels in a shelf corner -
  square cells and hard edges are the look, and a smooth circle there would read as a
  blurred icon rather than as a CD.

  The disc dialog is the opposite problem. It fills the panel, so those same 32 cells come
  out ~15px blocks at 720p, and the dialog is also where a *scanned* disc label appears once
  something has fetched one. A photograph next to fifteen-pixel blocks does not read as the
  same object seen at a different moment; it reads as two objects, and switching between
  them looks like a fault.

  Which is why this returns a buffer rather than drawing. The dialog rotates and blits it
  through exactly the path the scan already goes through (disc_rot() in chome_ui.cpp), so
  the dialog is always turning a bitmap - generated or photographed - and the two cannot
  differ in kind however they differ in content.

  Proportions and palette are gfx_disc()'s, because this has to be recognisably the same
  object: a dark outer edge, a bright rim, the data area sweeping with colour, a clear inner
  ring, a hub ring and the hole. Two things are different, and both are things only a
  properly resolved disc can have - every ring boundary is anti-aliased by coverage, and the
  iridescence is interpolated between neighbouring bands instead of stepped between them.

  `back` is what sits behind the disc: the corners are filled with it and the outer rim is
  blended into it, because gfx_blit() writes every pixel it covers and does not blend. So a
  face generated for one background cannot be blitted onto another.

  Regenerated only when the size or the palette changes - the dialog asks for the same size
  every frame - and cached in one buffer that survives leaving the dialog, as the rotation
  buffer beside it does. dia*dia*4 bytes: 900 KB at 720p, 324 KB at 480p and on a 960x540
  canvas, 36 KB at 240p.

  Returns 0 if the buffer cannot be had, which is a caller's cue to fall back to the sprite.
*/
const uint32_t *gfx_disc_face(int dia, const uint32_t *bands, int nbands,
	uint32_t rim, uint32_t ring, uint32_t hole, uint32_t back);

/*
  How many times gfx_disc_face() has actually rendered, as opposed to answering from its
  cache. Only the harness reads it, and it exists because "generated once per size" is the
  entire cost argument for this being affordable at all: a test that could only see the
  pixels could not tell a cached face from one recomputed sixty times a second.
*/
int gfx_disc_face_gens();

// The size the cached face is held at, or 0 for none. Also the harness's: it is how a test
// can say "the face was made for the disc that is on screen" rather than "a face exists".
int gfx_disc_face_dia();

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

/*
  The width of the glyph *cell*, which is not the same thing as the advance any more.

  charrom's cell is 8 columns and draw_glyph() rasterises all 8 of them, so this is the
  bitmap's width. It is not how far the pen moves - that is gfx_adv(), which adds
  classicui_tracking. Anything meaning "how far to the next character" wants gfx_adv();
  only the bitmap itself wants GLYPH_W.

  Worth knowing which of the eight the font actually uses, because it is what makes
  negative spacing safe: no printable glyph in the stock ROM font inks column 8, 69 of the
  95 stop at column 6, and only `& M W ^ _ m w ~` reach column 7. So -1 always leaves a
  gap and -2 makes those eight touch.
*/
#define GLYPH_W 8

/*
  One character's advance in canvas pixels, and how many characters fit in a span of them.

  These two and gfx_text_w() are one model with three faces, and they are functions rather
  than arithmetic spelled out at each site because the front-end has a couple of dozen
  places that convert between pixels and characters - a panel capped at "46 characters
  wide", a paragraph wrapped to "however many columns the panel has". One of them left as
  `/ (8 * s)` is a paragraph drawn through the side of its panel.

  gfx_text_cols() is the exact inverse of gfx_text_w(): the count it returns is the largest
  n for which a string of n characters still measures <= px. gfx_clip() is built on it for
  that reason. When the two drifted apart, the disc dialog widened its panel to fit a
  measured title and then clipped that same title anyway - "Super Nintendo (n>".
*/
int gfx_adv(int scale);
int gfx_text_cols(int px, int scale);

/*
  Shout a string in place, or leave it exactly as it was written.

  Every title, label, header and legend in this front-end is drawn in capitals, and that was
  never a decision about those particular strings - it was one idiom copied twenty-two times,
  uppercasing whatever it was handed on its way to gfx_text(). It reads well in the MiSTer ROM
  font, whose lowercase glyphs are four rows tall and a little cramped.

  It reads badly in somebody else's font. A player who has put a .pf on the card usually chose
  it for its lowercase, and a front-end that never draws a lowercase glyph makes half of that
  font invisible - which is what classicui_caps=0 is for. One function rather than
  twenty-two conditionals is not only shorter: it is what makes the rule "text is drawn as it
  is written" true, with no screen quietly still shouting because its site was missed.

  It lives here, beside gfx_text(), because it is a property of how this front-end draws text
  rather than of any one screen - the on-screen keyboard's own panel header goes through it too.
*/
void gfx_shout(char *s);

void gfx_text(const char *s, int x, int y, int scale, uint32_t col, uint32_t shadow);
void gfx_text_c(const char *s, int cx, int y, int scale, uint32_t col, uint32_t shadow);
int  gfx_text_w(const char *s, int scale);

// Truncate to fit maxpx, appending '>' when clipped. Returns a static buffer.
const char *gfx_clip(const char *s, int scale, int maxpx);

// Nearest-neighbour blit of an ARGB source. No filtering: at these scales
// integer-ish nearest keeps pixel art crisp and costs almost nothing.
void gfx_blit(const uint32_t *src, int sw, int sh, int dx, int dy, int dw, int dh);

#endif
