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

/*
  Read the counters behind that log, so the harness can print a cost table per canvas
  instead of fishing numbers out of a periodic printf. `partial` picks the bucket the
  log tags "partial" (frames composed under a clip); returns the number of frames in
  it and fills whichever of the averages' numerators are asked for. Collection is
  still behind cfg.debug, exactly as the log is - reading is free either way.
*/
void gfx_stat_reset();
unsigned long gfx_stat_get(int partial,
	unsigned long *compose_us, unsigned long *copy_us, unsigned long *rows);

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
  feel alone: the sprite has 64 positions in a turn, and a disc that advances more than
  about four of them between repaints strobes instead of spinning. At GFX_DISC_MS these
  give roughly 4, 2 and 1 positions per frame; at GFX_DISC_PART_MS, roughly 1.3, 0.7 and
  0.3, which is under one position per frame and as smooth as 64 positions can be. (The
  dialog resolves the same turn to 256 positions - see disc_step_fine() in chome_ui.cpp -
  which moves these ratios, not the periods: the same angular speed, sampled finer.)

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

/*
  GFX_DISC_PART_MS is how often the spin *asks*, not how often it paints: a tick on
  which the step has not moved - and the rip's reveal with it - is skipped outright, so
  the repaint rate follows the angle, and a painted frame is never byte-identical to
  the one before it.

  How often the step moves depends on which disc is up. The badge quantises the turn to
  the 64 positions its sprite can express, so on the shelf the skip does most of the
  work: 16 paints a second at the slow rate, not 60. The dialog resolves the same turn
  to 256 positions - his ask, verbatim: "as close as possible to 60FPS when disc dialog
  is open" - so there a new angle lands on nearly every tick and nearly every tick
  paints. What makes that affordable is that a dialog frame is no longer a resample: a
  bounded cache of quadrant frames turns the steady state into a blit (see disc_rot()
  in chome_ui.cpp), the angle count backs off to the cached set when a rip, a library
  scan or a cover decode needs the time (see disc_step_fine()), and the half-resolution
  canvas (classicui_halfres, on by default) keeps the disc at 160px on a 720p display,
  the size at which the cache holds every angle of the turn.
*/

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
  two sides swapped, and the scan is the side Dinofly will actually be looking at.

  Costed for the caller that runs per rotation angle rather than per size: the two easy
  answers come from the squared distance and take no square root, so only the pixels a
  boundary passes through pay for one. That is the circumference and not the area - about 3%
  of the buffer with the four boundaries a masked scan has.
*/
int gfx_disc_cover(int rad, int d2);

/*
  The same ramp, measured from between pixels rather than from one.

  `d2h` is the squared distance in *half*-pixel units from the true centre of a dia-square
  buffer - the point between the four middle pixels, which is where gfx_disc_face() has
  always measured from (its X = 2*(x-r)+1). `rad` stays in whole pixels and the ramp stays
  one whole pixel wide, so this answers exactly what gfx_disc_cover() answers, half a pixel
  over.

  It exists because disc_rot() moved to the same centre. Rotating a square buffer by a
  quarter turn is an exact permutation of its pixels *only* about the between-pixels centre
  - about a pixel it is off by one, which is what made composing "rotate by the angle inside
  one quadrant, then turn quarters" impossible: the rings landed a pixel out and the disc
  jumped four times a turn. Measured about the true centre the rings are invariant under
  that permutation, the composition is byte-exact, and one cached quadrant frame can serve
  four angles of the turn. It also closes the half-pixel gap the harness used to have to
  tolerate between a scan's rings and the face's.

  Same cost shape as gfx_disc_cover(), for the same caller-per-angle reason: the two easy
  answers come from the squared distance, and only the circumference pays for a root.
*/
int gfx_disc_cover_h(int rad, int d2h);

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
  And code 5, the ellipsis: the mark gfx_clip() leaves where it cut a string.

  It was a '>' for a long time, which is a character the ROM font happens to have and not
  a character that means anything - "SUPER MARIO WORLD>" reads as a title with a
  greater-than sign on it. Three dots is what a reader already knows means "there is more
  of this".

  In extra_glyphs[] with the arrows rather than in charrom, and that is the whole
  degradation story: draw_glyph() answers codes 1..5 from our own table before it ever
  looks at charfont[], so a .pf font loaded off the card cannot take the mark away. A font
  file *can* write charfont[5] - a non-768-byte .pf starts at code 0 (see LoadFont) - and
  under any other arrangement the front-end would then be marking its cut text with
  somebody's spare glyph, or with nothing at all. The cost is that the ellipsis stays in
  the built-in style while the rest of the screen is in the player's; that is the right way
  round for a mark whose whole job is to be recognised.

  Drawn as three 2x2 dots on the same two rows as the ROM font's own full stop, so it has
  the weight of real punctuation at scale 1. That is the size that had to be checked and
  the reason it is not three single pixels: 240p on a television is where this front-end
  lives, and a one-pixel dot there is a smudge. Three 2x2 dots with one column of gap is
  exactly eight columns wide, so the mark inks column 0 and column 7 of its cell - see
  GLYPH_W below for why nothing else in the font does. At zero tracking that leaves one
  clear pixel to the letter before it, and at negative tracking the leading dot touches
  it. Legibility at 240p is worth more than a gap at a setting that already makes `M` and
  `W` touch.
*/
#define CH_ELLIPSIS "\x05"

/*
  A string too long for its space, shown by scrolling it instead of only cutting it.

  gfx_clip() answers "what fits"; this answers "what fits *now*", and the two agree at the
  start of the cycle - a marquee parked at offset 0 returns byte-for-byte what gfx_clip()
  would have returned, ellipsis and all. That is deliberate and it is what makes the
  feature safe to add to a screen: the resting frame is the frame that shipped.

  The window steps by whole characters, not by pixels. The font is a fixed 8-column cell
  and a pixel-smooth scroll would need a clip rectangle per string - and gfx_clip_set() is
  a single global rectangle, not a stack, so setting one inside a compose would silently
  drop the region clip that render_region() had put there. A partial repaint that lost its
  clip is a full-screen paint that reports itself as cheap. Whole characters need no clip
  at all.

  The cycle: hold at the start for GFX_MARQ_HOLD_MS, step one character every
  GFX_MARQ_STEP_MS until the tail is flush with the right-hand edge, hold there for
  GFX_MARQ_HOLD_MS, then back to the start. Back rather than reversing: a marquee running
  backwards reads as a fault, and "it returns to the beginning" is the behaviour a player
  waits for.

  There is no marker on the left. One would have to eat a cell, which changes how many
  characters the window holds, which moves the text sideways on the first step and again on
  the last - a centred title visibly jumping. The movement is what says there is more to
  the left; the ellipsis on the right says there is more to the right, and disappears when
  there is not.

  `ms` is a millisecond clock and the phase comes only from it, so this is a pure function
  of its arguments: composing one instant twice draws the same window twice. Everything in
  chome_ui.cpp that compares a partial repaint against a full repaint of the same moment
  depends on that being true of every animation in this file.

  `scrolling`, when given, comes back non-zero if the string did not fit - i.e. if there is
  a marquee here at all. `next_in`, when given, comes back as the milliseconds until this
  window changes, which is what lets the caller repaint when the text moves rather than
  polling at a frame rate. Both are left alone for a string that fits.

  Returns a static buffer, like gfx_clip(): a second call overwrites the first.
*/
#define GFX_MARQ_HOLD_MS 1200UL
#define GFX_MARQ_STEP_MS  180UL

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

  What is left of gfx_text_cols(), and why it is not the default any more.

  A question with a single answer for the whole font is a question only a fixed-width font
  can answer. "How many characters fit in 300 pixels" is 37 here and would be a different
  number per string in any font whose glyphs differ in width - so every screen that asked it
  in order to decide something was resting on the built-in font's cell being the only cell.
  Layout has stopped asking. A caller that wants to know whether a string fits measures that
  string (gfx_text_w(s) <= px); one choosing between a long and a short wording measures the
  long one; one wrapping a paragraph measures each line as it builds it. None of them need a
  count, and none of them would need revisiting to put a proportional font behind this file.

  It survives for the two things that really are counted in characters:

    - gfx_clip() and gfx_marquee(), which copy a prefix and therefore need a number of
      characters, not a width. These two are one piece: the marquee's window is exactly
      `max` cells at every offset by design (see the note above GFX_MARQ_HOLD_MS), and a
      parked marquee must return byte-for-byte what gfx_clip() returns. Making either of
      them measure per string means doing both together, and doing them together is part of
      proportional-font support rather than of layout.

    - draw_core_opts()'s `wide` and `room`, which pick between wordings on a hand-tuned
      column count rather than on whether anything fits. Named as such there.

  So a new caller reaching for this should check it is in one of those two situations, and
  otherwise measure the string it actually means.
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

// Truncate to fit maxpx, marking the cut with CH_ELLIPSIS. Returns a static buffer.
const char *gfx_clip(const char *s, int scale, int maxpx);

// The same cut, scrolled. See the note above GFX_MARQ_HOLD_MS.
const char *gfx_marquee(const char *s, int scale, int maxpx, unsigned long ms,
	int *scrolling, unsigned long *next_in);

#ifdef CHOME_HOST_TEST
/*
  The clip log - the host harness only, and absent from every shipped build.

  gfx_clip() is the one place in the front-end that knows a string did not fit, and until
  this existed it kept that to itself: it wrote a '>' over the last character and returned,
  and the only way anyone learned that a sentence had been cut in half was to read it off a
  television. Three were found that way in one day, one of them the single line explaining
  what a menu does.

  So the moment it truncates is recorded here - the whole string it was handed, the width it
  had, the scale, how many characters it lost, and the function that asked. The drawing is
  not touched: the same characters go to the same pixels whether anything is listening or
  not, which is what makes this safe to leave switched on for the whole suite.

  The caller's name comes from __func__ through the macro below rather than from a new
  argument at sixty call sites, and it is what lets the sweep tell a sentence we wrote from
  a game title that is simply longer than its card. Line numbers would have done the same
  job and then rotted the first time a function moved; a name survives edits.

  Records are unique on (text, site, width): a screen redrawn sixty times a second would
  otherwise fill the ring with one sentence, and the count of how often it happened is not
  what anybody needs to know.
*/
struct gfx_clip_rec
{
	char text[256];       // the string as handed in, before truncation
	char site[64];        // __func__ of the caller
	int  scale;
	int  maxpx;
	int  lost;            // characters that did not fit
	int  hits;            // times this same clip happened
	int  cw, ch;          // the canvas it happened on - which profile, in one glance
};

int  gfx_clip_log_n();
const gfx_clip_rec *gfx_clip_log(int i);
void gfx_clip_log_clear();

// Records the caller for the log. See the note above; the shipped build has neither.
const char *gfx_clip_at(const char *s, int scale, int maxpx, const char *site);
#define gfx_clip(s, scale, maxpx) gfx_clip_at((s), (scale), (maxpx), __func__)

/*
  And the marquee, which records too - the same string, the same width, the same site.

  Worth being explicit about, because the tempting thing was to exempt it: a string that
  scrolls is not lost any more, so why fail on it? Because assert_no_clipped_copy() is not
  asserting "the player can eventually read this". It is asserting that no sentence *we*
  wrote is too long for the space we gave it, and that is still a defect when the sentence
  scrolls - a footer a player has to wait four seconds to finish is worse than a shorter
  footer. Scrolling is for names off the card, which are as long as they are. If the
  marquee stopped logging, the guard would quietly lose its teeth on the day a screen
  switched a label of ours over to it.
*/
const char *gfx_marquee_at(const char *s, int scale, int maxpx, unsigned long ms,
	int *scrolling, unsigned long *next_in, const char *site);
#define gfx_marquee(s, scale, maxpx, ms, sc, ni) \
	gfx_marquee_at((s), (scale), (maxpx), (ms), (sc), (ni), __func__)
#endif

// Nearest-neighbour blit of an ARGB source. No filtering: at these scales
// integer-ish nearest keeps pixel art crisp and costs almost nothing.
void gfx_blit(const uint32_t *src, int sw, int sh, int dx, int dy, int dw, int dh);

#endif
