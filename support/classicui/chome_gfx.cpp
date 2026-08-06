#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "chome_gfx.h"
#include "../../cfg.h"
#include "../../video.h"
#include "../../charrom.h"

static uint32_t *cb = 0;      // compose buffer, cached RAM
static int cw = 0, ch = 0;
static int fbn = 1;           // framebuffer we will draw into next

struct rect_t { int x0, y0, x1, y1; };

static rect_t dmg_cur;        // damage submitted this frame
static rect_t dmg_prev;       // damage of the previous frame

static int dmg_rows = 0;

/*
  The clip region, for partial repaints.

  While one is set, every primitive draws - and records damage - only inside it, so a
  frame composed under a clip leaves the rest of the compose buffer untouched and
  unclaimed: gfx_end() then copies only the clipped rectangle (unioned with the previous
  frame's damage, exactly as it always has). The caller replays its normal drawing;
  everything that misses the region rejects in a comparison or two, so the cost of a
  clipped frame is proportional to the region, not to the screen.

  clipr is kept clamped to the canvas by gfx_clip_set(), so the primitives only ever
  intersect against one rectangle.
*/
static rect_t clipr;
static int clip_on = 0;
static int stat_was_clipped = 0;   // this frame composed under a clip; read by gfx_end()

static void rect_clear(rect_t *r)
{
	r->x0 = r->y0 = 0x7fffffff;
	r->x1 = r->y1 = -1;
}

static int rect_empty(const rect_t *r)
{
	return r->x1 < r->x0 || r->y1 < r->y0;
}

static void rect_add(rect_t *r, int x0, int y0, int x1, int y1)
{
	if (x0 < r->x0) r->x0 = x0;
	if (y0 < r->y0) r->y0 = y0;
	if (x1 > r->x1) r->x1 = x1;
	if (y1 > r->y1) r->y1 = y1;
}

void gfx_damage(int x, int y, int w, int h)
{
	if (w <= 0 || h <= 0) return;
	if (x < 0) { w += x; x = 0; }
	if (y < 0) { h += y; y = 0; }
	if (x + w > cw) w = cw - x;
	if (y + h > ch) h = ch - y;
	if (clip_on)
	{
		if (rect_empty(&clipr)) return;
		if (x < clipr.x0) { w -= clipr.x0 - x; x = clipr.x0; }
		if (y < clipr.y0) { h -= clipr.y0 - y; y = clipr.y0; }
		if (x + w > clipr.x1 + 1) w = clipr.x1 + 1 - x;
		if (y + h > clipr.y1 + 1) h = clipr.y1 + 1 - y;
	}
	if (w <= 0 || h <= 0) return;
	rect_add(&dmg_cur, x, y, x + w - 1, y + h - 1);
}

void gfx_damage_all()
{
	// Under a clip nothing outside the region was drawn, so nothing outside it may be
	// claimed either - a full-screen claim here would copy stale compose rows.
	if (clip_on)
	{
		if (!rect_empty(&clipr)) rect_add(&dmg_cur, clipr.x0, clipr.y0, clipr.x1, clipr.y1);
		return;
	}
	if (cw > 0 && ch > 0) rect_add(&dmg_cur, 0, 0, cw - 1, ch - 1);
}

void gfx_clip_set(int x, int y, int w, int h)
{
	rect_clear(&clipr);
	if (x < 0) { w += x; x = 0; }
	if (y < 0) { h += y; y = 0; }
	if (x + w > cw) w = cw - x;
	if (y + h > ch) h = ch - y;
	if (w > 0 && h > 0) rect_add(&clipr, x, y, x + w - 1, y + h - 1);
	clip_on = 1;
	stat_was_clipped = 1;
}

void gfx_clip_clear()
{
	clip_on = 0;
}

int gfx_damage_rows()
{
	return dmg_rows;
}

int gfx_w() { return cw; }
int gfx_h() { return ch; }

int gfx_begin()
{
	int w = video_menu_fb_width();
	int h = video_menu_fb_height();

	if (w <= 0 || h <= 0) return 0;
	if (!video_menu_fb(1) || !video_menu_fb(2)) return 0;

	if (w != cw || h != ch || !cb)
	{
		free(cb);
		cb = (uint32_t*)calloc((size_t)w * h, sizeof(uint32_t));
		if (!cb) { cw = ch = 0; return 0; }
		cw = w;
		ch = h;
		rect_clear(&dmg_cur);
		rect_clear(&dmg_prev);
		gfx_damage_all();
		// Both framebuffers hold stale content at the new size.
		dmg_prev = dmg_cur;
	}

	return 1;
}

/*
  Repaint cost, measured rather than guessed.

  Two numbers, because the two halves of a repaint are not alike: composing into the
  cached RAM buffer, and copying the damaged rows into the framebuffer, which is an
  uncached /dev/mem mapping shared with the FPGA. The second is expected to dominate and
  the point of measuring is to find out by how much.

  Why this exists at all: the firmware busy-polls, so it sits at 100% of one core whether
  it is drawing or not - measured on the device, both with the front-end open and closed.
  That means repaint work cannot be seen in CPU time at all. It can only be seen by
  timing it.

  Reported as a summary every GFX_STAT_EVERY copies so the log stays readable; at a 50ms
  repaint that is roughly every ten seconds.

  Full and partial repaints are accounted separately - averaging them together would
  bury the number the partial path exists to produce under the occasional full frame,
  and hide a regression in either.

  All of it is behind cfg.debug, measurement included. The reporting was already throttled,
  but the clock_gettime() pairs ran on every composed frame to feed it - work nobody asked
  for on the drawing path of a build that is not being debugged.
*/
#define GFX_STAT_EVERY 200

static unsigned long gfx_us()
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (unsigned long)ts.tv_sec * 1000000UL + (unsigned long)(ts.tv_nsec / 1000);
}

struct gfx_stat_t
{
	unsigned long n;
	unsigned long compose_us, compose_max;
	unsigned long copy_us, copy_max;
	unsigned long rows;
};

static gfx_stat_t stat_full, stat_part;
static unsigned long compose_t0 = 0;

static void stat_fmt(char *buf, size_t len, const char *tag, const gfx_stat_t *s)
{
	if (!s->n) { snprintf(buf, len, "%s none", tag); return; }
	snprintf(buf, len, "%s %lu: compose avg %lu us (max %lu), copy avg %lu us (max %lu), rows avg %lu",
		tag, s->n,
		s->compose_us / s->n, s->compose_max,
		s->copy_us / s->n, s->copy_max,
		s->rows / s->n);
}

// Called by the front-end when it starts composing a frame.
void gfx_stat_compose_begin()
{
	compose_t0 = cfg.debug ? gfx_us() : 0;
}

void gfx_end()
{
	// Which bucket this frame lands in. Reset here rather than in gfx_clip_clear(), so
	// a frame that set a clip and cleared it before ending still counts as partial.
	gfx_stat_t *st = stat_was_clipped ? &stat_part : &stat_full;
	stat_was_clipped = 0;

	/*
	  Compose time is everything between gfx_stat_compose_begin() and here, which is the
	  drawing itself - all the fills, text and blits - minus the copy below.
	*/
	if (compose_t0)
	{
		unsigned long c = gfx_us() - compose_t0;
		compose_t0 = 0;
		st->compose_us += c;
		if (c > st->compose_max) st->compose_max = c;
	}

	if (!cb) return;

	// The two framebuffers alternate, so the one we are about to fill is two
	// frames stale: it needs this frame's damage plus the previous frame's.
	rect_t u = dmg_cur;
	if (!rect_empty(&dmg_prev)) rect_add(&u, dmg_prev.x0, dmg_prev.y0, dmg_prev.x1, dmg_prev.y1);

	dmg_rows = rect_empty(&dmg_cur) ? 0 : (dmg_cur.y1 - dmg_cur.y0 + 1);

	if (!rect_empty(&u))
	{
		uint32_t *fb = video_menu_fb(fbn);
		if (fb)
		{
			unsigned long t_copy = cfg.debug ? gfx_us() : 0;
			int x = u.x0;
			int bytes = (u.x1 - u.x0 + 1) * sizeof(uint32_t);
			for (int y = u.y0; y <= u.y1; y++)
			{
				memcpy(fb + (size_t)y * cw + x, cb + (size_t)y * cw + x, bytes);
			}
			video_menu_fb_present(fbn);
			fbn = (fbn == 1) ? 2 : 1;

			if (cfg.debug)
			{
				unsigned long cp = gfx_us() - t_copy;
				st->copy_us += cp;
				if (cp > st->copy_max) st->copy_max = cp;
				st->rows += (unsigned long)(u.y1 - u.y0 + 1);
				st->n++;

				if (stat_full.n + stat_part.n >= GFX_STAT_EVERY)
				{
					char fs[160], ps[160];
					stat_fmt(fs, sizeof(fs), "full", &stat_full);
					stat_fmt(ps, sizeof(ps), "partial", &stat_part);

					printf("ClassicUI: repaint %dx%d over %lu frames: %s; %s\n",
						cw, ch, stat_full.n + stat_part.n, fs, ps);
					memset(&stat_full, 0, sizeof(stat_full));
					memset(&stat_part, 0, sizeof(stat_part));
				}
			}
		}
	}

	dmg_prev = dmg_cur;
	rect_clear(&dmg_cur);
}

void gfx_shutdown()
{
	free(cb);
	cb = 0;
	cw = ch = 0;
	rect_clear(&dmg_cur);
	rect_clear(&dmg_prev);
	clip_on = 0;
	stat_was_clipped = 0;
}

// Clip a rect to the canvas - and to the clip region while one is set. Returns 0 if
// nothing is left.
static int clip_rect(int *x, int *y, int *w, int *h)
{
	if (!cb) return 0;
	if (*x < 0) { *w += *x; *x = 0; }
	if (*y < 0) { *h += *y; *y = 0; }
	if (*x + *w > cw) *w = cw - *x;
	if (*y + *h > ch) *h = ch - *y;
	if (clip_on)
	{
		if (rect_empty(&clipr)) return 0;
		if (*x < clipr.x0) { *w -= clipr.x0 - *x; *x = clipr.x0; }
		if (*y < clipr.y0) { *h -= clipr.y0 - *y; *y = clipr.y0; }
		if (*x + *w > clipr.x1 + 1) *w = clipr.x1 + 1 - *x;
		if (*y + *h > clipr.y1 + 1) *h = clipr.y1 + 1 - *y;
	}
	return (*w > 0 && *h > 0);
}

void gfx_fill(int x, int y, int w, int h, uint32_t col)
{
	int ox = x, oy = y, ow = w, oh = h;
	if (!clip_rect(&x, &y, &w, &h)) return;

	for (int yy = y; yy < y + h; yy++)
	{
		uint32_t *p = cb + (size_t)yy * cw + x;
		for (int xx = 0; xx < w; xx++) *p++ = col;
	}
	gfx_damage(ox, oy, ow, oh);
}

void gfx_frame_rect(int x, int y, int w, int h, uint32_t col, int t)
{
	if (t < 1) t = 1;
	if (w <= 0 || h <= 0) return;
	gfx_fill(x, y, w, t, col);
	gfx_fill(x, y + h - t, w, t, col);
	gfx_fill(x, y, t, h, col);
	gfx_fill(x + w - t, y, t, h, col);
}

void gfx_blend(int x, int y, int w, int h, uint32_t col, int alpha)
{
	int ox = x, oy = y, ow = w, oh = h;
	if (!clip_rect(&x, &y, &w, &h)) return;
	if (alpha <= 0) return;
	if (alpha > 255) alpha = 255;

	uint32_t sr = (col >> 16) & 0xff, sg = (col >> 8) & 0xff, sb = col & 0xff;
	uint32_t ia = 255 - alpha;

	for (int yy = y; yy < y + h; yy++)
	{
		uint32_t *p = cb + (size_t)yy * cw + x;
		for (int xx = 0; xx < w; xx++)
		{
			uint32_t d = *p;
			uint32_t dr = (d >> 16) & 0xff, dg = (d >> 8) & 0xff, db = d & 0xff;
			dr = (sr * alpha + dr * ia) >> 8;
			dg = (sg * alpha + dg * ia) >> 8;
			db = (sb * alpha + db * ia) >> 8;
			*p++ = 0xff000000u | (dr << 16) | (dg << 8) | db;
		}
	}
	gfx_damage(ox, oy, ow, oh);
}

void gfx_scrim(int x, int y, int w, int h, uint32_t col, int step)
{
	int ox = x, oy = y, ow = w, oh = h;
	if (!clip_rect(&x, &y, &w, &h)) return;
	if (step < 2) step = 2;

	for (int yy = y; yy < y + h; yy++)
	{
		uint32_t *row = cb + (size_t)yy * cw;
		int phase = (yy % step);

		/*
		  The pattern is anchored to the caller's origin, not to wherever clipping moved
		  x: a partial repaint replays this call under a clip, and a phase computed from
		  the clipped edge would draw the checkerboard one pixel out of register with
		  the full frame around it.
		*/
		int x0 = ox + phase;
		if (x0 < x) x0 += (x - x0 + step - 1) / step * step;
		for (int xx = x0; xx < x + w; xx += step) row[xx] = col;
	}
	gfx_damage(ox, oy, ow, oh);
}

/* ------------------------------------------------------------ activity ---- */

/*
  A busy indicator and a progress track, both drawn from rectangles.

  They are here rather than in an icon header for the reason ICONS.md already gives
  for the Wi-Fi signal bars and the Display screen's radio dots: a gauge whose state
  is the whole point cannot be a fixed picture. Nothing pictorial is drawn by hand in
  this front-end, and neither of these is a picture of anything - one is eight dots on
  a circle and the other is a row of boxes.

  Both take a millisecond clock rather than counting frames. Frames are only drawn
  when something changed, so a frame counter would run at whatever rate the rest of
  the UI happened to repaint at, and the same animation would be a different speed on
  a busy screen than on an idle one.
*/

/*
  Eight positions on a circle of radius 128, quantised to whole pixels by the caller's
  radius. Eight and not more: at the radius this is drawn at - eight pixels at 240p -
  twelve dots overlap into a smudged ring, and the point of the comet is that you can
  see which way it is going.
*/
static const int spin_x[GFX_SPIN_DOTS] = {    0,   90,  128,   90,    0,  -90, -128,  -90 };
static const int spin_y[GFX_SPIN_DOTS] = { -128,  -90,    0,   90,  128,   90,    0,  -90 };

/*
  How bright each dot is, by how far behind the head it sits. Falls off fast: an even
  ring reads as a decoration, and only a clear head and a short tail read as rotation.
  The last two are dark on purpose, so there is a gap the eye can follow round.
*/
static const int spin_fade[GFX_SPIN_DOTS] = { 255, 190, 130, 80, 45, 20, 0, 0 };

void gfx_spinner(int cx, int cy, int r, int dot, unsigned long ms, uint32_t hot, uint32_t cold)
{
	if (r < 2) return;
	if (dot < 1) dot = 1;

	int head = (int)((ms / GFX_SPIN_MS) % GFX_SPIN_DOTS);

	for (int i = 0; i < GFX_SPIN_DOTS; i++)
	{
		int back = (head - i + GFX_SPIN_DOTS) % GFX_SPIN_DOTS;
		int a = spin_fade[back];

		int x = cx + (spin_x[i] * r) / 128 - dot / 2;
		int y = cy + (spin_y[i] * r) / 128 - dot / 2;

		/*
		  The head is filled, not blended. gfx_blend() at alpha 255 divides by 256 and
		  lands one short on every channel, so a blended head is nearly but not exactly
		  the colour it was asked for - which is invisible on screen and matters to
		  anything reading pixels back, the harness included.
		*/
		if (a >= 255) { gfx_fill(x, y, dot, dot, hot); continue; }

		gfx_fill(x, y, dot, dot, cold);
		if (a) gfx_blend(x, y, dot, dot, hot, a);
	}
}

/*
  A quarter turn of sine, scaled to 256, in 16 steps - so 64 positions round the
  circle. Written out rather than computed because this file has no math.h and does
  not want one for a table that never changes.
*/
static const int disc_sin[17] =
{
	  0,  25,  50,  74,  98, 121, 142, 162,
	181, 198, 213, 226, 237, 245, 251, 255, 256
};

static int disc_isin(int step)
{
	step &= 63;
	if (step <= 16) return disc_sin[step];
	if (step <= 32) return disc_sin[32 - step];
	if (step <= 48) return -disc_sin[step - 32];
	return -disc_sin[64 - step];
}

/*
  Which of 64 positions round the circle a point sits at, without atan2 or floats.

  Octant first from the signs and from whether |y| exceeds |x|, then eight steps
  within the octant by ratio. Good to about a position, which is all a sector lookup
  on a 16-cell grid can use.
*/
static int disc_iatan(int y, int x)
{
	if (!x && !y) return 0;

	int ax = (x < 0) ? -x : x;
	int ay = (y < 0) ? -y : y;

	int r = (ax >= ay) ? (8 * ay) / ax : 16 - (8 * ax) / ay;

	if (x >= 0 && y >= 0) return r;
	if (x < 0 && y >= 0) return 32 - r;
	if (x < 0 && y < 0)  return 32 + r;
	return (64 - r) & 63;
}

void gfx_disc(int cx, int cy, int r, int step,
	const uint32_t *bands, int nbands, uint32_t rim, uint32_t ring, uint32_t hole,
	uint32_t outline)
{
	if (r < 8 || !bands || nbands < 1) return;

	step &= 63;

	/*
	  A 32x32 sprite, not a 16x16 one scaled up.

	  Doubling a 16x16 disc gives bigger blocks and no more information: the same eight
	  wedges, the same two-cell rim. Drawing on twice the grid buys detail the smaller
	  one had no room for - a dark outer edge so the disc reads against a light panel as
	  well as a dark shelf, a rim that is thin in proportion rather than a quarter of the
	  radius, a hub ring distinct from the spindle hole, and enough angular resolution
	  for twelve or more wedges instead of eight.

	  Cells are r/16 pixels square, so r=16 gives a 32px icon at one pixel per cell and
	  r=32 gives 64px at 2x2. Multiples of 16 keep the cells whole; a radius between two
	  of them still comes out exactly the size it asked for, which is what the badge's
	  breath needs - see the grid-to-pixel mapping below.
	*/

	/*
	  Radii in half-cells, squared: cell centres land on odd numbers so nothing needs
	  fractions. The disc is 16 cells (32 half-cells) to the edge.

	  The proportions are a CD's, roughly to scale: the clear inner ring is about a third
	  of the radius and the hole about a sixth, which is what makes it read as a disc
	  rather than a washer.
	*/
	const int r2_edge  = 32 * 32;      // outside this, nothing
	const int r2_dark  = 30 * 30;      // outer edge, one cell of shadow
	const int r2_rim   = 27 * 27;      // bright rim
	const int r2_data  = 13 * 13;      // data area runs down to here
	const int r2_ring  =  9 * 9;       // clear inner ring
	const int r2_hub   =  6 * 6;       // hub ring
	// inside r2_hub is the spindle hole

	// One step of shadow between the rim and the outer edge, mixed from the two so the
	// palette does not need another entry.
	uint32_t edge = ((rim >> 1) & 0x7f7f7f7f) + ((hole >> 1) & 0x7f7f7f7f);
	edge |= 0xff000000u;

	/*
	  Two cells wider than the disc when there is an outline to draw, which is how focus
	  is shown: a ring just outside it. A filled plate behind the disc was the alternative
	  and it covered the shelf title at 240p.

	  Two cells and not one because at 240p a cell is one pixel, and a one-pixel ring on a
	  real TV was too subtle to read as focus at all. Thickness has to come as whole
	  cells: the sprite has no fractional pixels to give.
	*/
	int g = outline ? 18 : 16;
	const int r2_out = 36 * 36;

	/*
	  The grid mapped onto the pixel box, rather than multiplied out cell by cell.

	  A cell runs from (grid + 18) * r / 16 to the next one, measured from an origin 18
	  cells left of - and above - the centre. At a multiple of 16 that is exactly where
	  multiplying by an integer cell size put it, for both sprite sizes: the origin is
	  18 * cell and (grid + 18) * cell - 18 * cell is grid * cell, so every size this has
	  ever drawn at is unchanged to the pixel.

	  What it buys is the sizes in between, and the reason to want them is that r/16 as an
	  integer division quantises the whole sprite to 32-pixel steps at 240p: growing the
	  radius by anything short of doubling it rendered identically, so the badge's breath
	  had no size to grow to. See draw_disc_badge() in chome_ui.cpp.

	  The cost, at a radius that is not a multiple of 16: the cells are not all the same
	  size - one row or column in every few is a pixel wider - and the odd pixel of the
	  diameter lands on one side instead of being split. Both are invisible at these sizes
	  and neither can be avoided, for the same reason the focus ring's thickness is a whole
	  number of cells: the sprite has no fractional pixels to give.
	*/
	int org = 18 * r / 16;

	for (int gy = -g; gy < g; gy++)
	{
		int Y = 2 * gy + 1;
		int py = cy - org + (gy + 18) * r / 16;
		int ph = cy - org + (gy + 19) * r / 16 - py;

		for (int gx = -g; gx < g; gx++)
		{
			int X = 2 * gx + 1;
			int d2 = X * X + Y * Y;

			if (d2 > (outline ? r2_out : r2_edge)) continue;

			uint32_t col;

			if (d2 > r2_edge)       col = outline;
			else if (d2 > r2_dark)  col = edge;
			else if (d2 > r2_rim)   col = rim;
			else if (d2 > r2_data)
			{
				/*
				  The iridescence: wedges that sweep round as `step` advances. Sweeping
				  colour is what reads as a disc catching the light - a plain circle
				  turning is indistinguishable from one standing still.
				*/
				int sector = (disc_iatan(Y, X) + step) & 63;
				col = bands[(sector * nbands / 64) % nbands];
			}
			else if (d2 > r2_ring)  col = ring;
			else if (d2 > r2_hub)   col = edge;
			else                    col = hole;

			int px = cx - org + (gx + 18) * r / 16;
			gfx_fill(px, py, cx - org + (gx + 19) * r / 16 - px, ph, col);
		}
	}
}

/* ------------------------------------------------- the disc, properly resolved --- */

/*
  arctan over one octant, as 2048ths of it: entry i is atan(i/64) scaled so that a full
  eighth turn - which is eight of gfx_disc's 64 positions, each worth 256 here - comes out
  at 2048. Written out for the same reason disc_sin[] above is: no math.h in this file, and
  a table that never changes does not need one.
*/
static const int disc_atan_oct[65] =
{
	   0,   41,   81,  122,  163,  203,  244,  284,
	 324,  364,  404,  444,  483,  523,  562,  600,
	 639,  677,  715,  753,  790,  827,  863,  900,
	 936,  971, 1006, 1041, 1075, 1109, 1143, 1176,
	1209, 1241, 1273, 1305, 1336, 1367, 1397, 1427,
	1457, 1486, 1514, 1543, 1571, 1598, 1625, 1652,
	1678, 1704, 1729, 1754, 1779, 1804, 1828, 1851,
	1874, 1897, 1920, 1942, 1964, 1985, 2007, 2027,
	2048
};

// atan(num/den) from that table, num <= den, interpolated between two entries.
static int disc_atan_frac(int num, int den)
{
	if (den <= 0) return 0;

	int t = (num << 12) / den;                  // the ratio, twelve fractional bits
	if (t >= 4096) return disc_atan_oct[64];

	int i = t >> 6, f = t & 63;
	return disc_atan_oct[i] + ((disc_atan_oct[i + 1] - disc_atan_oct[i]) * f) / 64;
}

/*
  Where a point sits round the circle, in 256ths of one of gfx_disc's 64 positions: 0..16383,
  and at step 0 exactly 256 times what disc_iatan() answers, so the wedges of the sprite and
  the sweep of the resolved face start in the same place.

  disc_iatan() is deliberately cheap and takes the ratio as the angle, which is out by two
  thirds of a position near the octant boundary. On a stepped sprite that is nothing - it
  cannot move a wedge by less than a whole cell. In a smooth gradient across 480 pixels it is
  a sweep that visibly hurries through the diagonals and dawdles through the axes, so this
  one pays for the table.
*/
static int disc_ang_q8(int y, int x)
{
	if (!x && !y) return 0;

	int ax = (x < 0) ? -x : x;
	int ay = (y < 0) ? -y : y;

	int r = (ax >= ay) ? disc_atan_frac(ay, ax) : 4096 - disc_atan_frac(ax, ay);

	if (x >= 0 && y >= 0) return r;
	if (x < 0 && y >= 0) return 8192 - r;
	if (x < 0 && y < 0)  return 8192 + r;
	return (16384 - r) & 16383;
}

/*
  Integer square root, bit by bit. Needed because anti-aliasing a circle by coverage needs
  the distance from the centre and not the square of it: the squared distance is what tells
  you which side of a boundary a pixel is on, and coverage is about how far past it the pixel
  is, in pixels.
*/
static unsigned disc_isqrt(unsigned long long n)
{
	unsigned long long rem = n, root = 0, bit = 1ULL << 62;

	while (bit > rem) bit >>= 2;

	while (bit)
	{
		if (rem >= root + bit)
		{
			rem -= root + bit;
			root = (root >> 1) + bit;
		}
		else root >>= 1;

		bit >>= 2;
	}

	return (unsigned)root;
}

// Blend b over a by t/255. Opaque out: these buffers are blitted, not composited.
static uint32_t disc_mix(uint32_t a, uint32_t b, int t)
{
	if (t <= 0) return a | 0xff000000u;
	if (t >= 255) return b | 0xff000000u;

	int u = 255 - t;
	unsigned r = (((a >> 16) & 0xff) * u + ((b >> 16) & 0xff) * t) / 255;
	unsigned g = (((a >>  8) & 0xff) * u + ((b >>  8) & 0xff) * t) / 255;
	unsigned l = (((a      ) & 0xff) * u + ((b      ) & 0xff) * t) / 255;

	return 0xff000000u | (r << 16) | (g << 8) | l;
}

/*
  How much of a pixel whose centre is d from the centre falls inside radius rad, both in 8.8
  pixels: all of it a half pixel inside, none of it a half pixel outside, and a straight ramp
  across the boundary itself.

  A box filter over a straight edge rather than over an arc, which is what makes it one
  subtraction. The error is the arc's sag across one pixel, 1/8R, and the tightest circle
  here is the spindle hole at 240p - seven pixels of radius, so a fiftieth of a pixel, five
  levels out of 255 on a boundary between two greys. Every other boundary is looser than
  that by an order of magnitude.
*/
static int disc_cover(int rad, int d)
{
	int t = rad - d + 128;
	if (t <= 0) return 0;
	if (t >= 256) return 255;
	return (t * 255) >> 8;
}

static uint32_t *face_buf = 0;
static int face_dia = 0;
static const uint32_t *face_bands = 0;
static int face_nbands = 0;
static uint32_t face_rim = 0, face_ring = 0, face_hole = 0, face_back = 0;
static int face_gens = 0;

int gfx_disc_face_gens()
{
	return face_gens;
}

int gfx_disc_face_dia()
{
	return face_dia;
}

const uint32_t *gfx_disc_face(int dia, const uint32_t *bands, int nbands,
	uint32_t rim, uint32_t ring, uint32_t hole, uint32_t back)
{
	if (dia < 16 || !bands || nbands < 1) return 0;

	/*
	  The palette is in the key as well as the size, and not because it moves today - it is
	  one table of constants. It is there because the theme is the sort of thing that grows a
	  setting, and a face cached on its size alone would answer with the old colours for as
	  long as the dialog stayed the same shape, which is for ever.
	*/
	if (face_buf && face_dia == dia && face_bands == bands && face_nbands == nbands
		&& face_rim == rim && face_ring == ring && face_hole == hole && face_back == back)
	{
		return face_buf;
	}

	if (!face_buf || face_dia != dia)
	{
		free(face_buf);
		face_buf = (uint32_t*)malloc((size_t)dia * dia * 4);
		face_dia = face_buf ? dia : 0;
		if (!face_buf) return 0;
	}

	face_bands = bands;
	face_nbands = nbands;
	face_rim = rim;
	face_ring = ring;
	face_hole = hole;
	face_back = back;
	face_gens++;

	int r = dia / 2;

	/*
	  Drawn to a pixel short of the buffer, so the buffer's outermost ring is background and
	  nothing else.

	  Not tidiness - it is what makes an inverse-mapped rotation of this exact. The caller
	  turns this by sampling, for each destination pixel, where in here it came from, and the
	  corners of a square are further from the centre than its edges: a destination pixel out
	  in a corner asks for a source pixel off the end of the buffer, and a rotation clamps
	  rather than grows. Drawn to the very edge, what it clamps to is the middle of an edge -
	  which is the darkest part of the disc's outer rim - and the disc came out with four grey
	  smears flung off it at the diagonals, turning with it. A background border is what there
	  is to clamp to instead.

	  One pixel, and one is enough because the rotation cannot ask for more than that: it
	  preserves the radius bar the truncation of a 8.8 sine, which is under a pixel.
	*/
	int rq = (r - 1) << 8;

	/*
	  gfx_disc's radii, in the 32nds of the radius it states them in, so the two are the same
	  disc: the dark outer edge from 30/32, the bright rim from 27/32, the data area down to
	  13/32, the clear inner ring to 9/32, then the hub ring, then the hole.

	  The hole is the one that is not a 32nd - see GFX_DISC_HOLE_PCT. It sits inside the 6/32
	  the sprite uses, so the hub ring here is a little wider than the sprite's; that is the
	  price of the hole landing where a scan's transparency actually is, and it is the right
	  way round, because a real CD's clamping area is wider than its hole by more than this.
	*/
	int rr_edge = rq;
	int rr_dark = (rq * 30) / 32;
	int rr_rim  = (rq * 27) / 32;
	int rr_data = (rq * 13) / 32;
	int rr_ring = (rq *  9) / 32;
	int rr_hole = (rq * GFX_DISC_HOLE_PCT) / 100;

	// One step of shadow between the rim and the outer edge, mixed from the two, exactly as
	// gfx_disc does it and for the same reason: the palette does not need another entry.
	uint32_t edge = ((rim >> 1) & 0x7f7f7f7f) + ((hole >> 1) & 0x7f7f7f7f);
	edge |= 0xff000000u;

	for (int y = 0; y < dia; y++)
	{
		// Half-pixel units, so a pixel centre lands on an odd number and nothing below needs
		// a fraction - the same trick gfx_disc plays with half-cells.
		int Y = 2 * (y - r) + 1;
		uint32_t *dst = face_buf + (size_t)y * dia;

		for (int x = 0; x < dia; x++)
		{
			int X = 2 * (x - r) + 1;

			// Distance in 8.8 pixels: sqrt of the squared half-pixel distance scaled by
			// 16384, which is (256/2) squared - half because the units are half pixels.
			int d = (int)disc_isqrt(((unsigned long long)(X * X + Y * Y)) << 14);

			if (d > rr_edge + 256) { dst[x] = back | 0xff000000u; continue; }

			/*
			  Composited from the outside in, one boundary at a time, each with its own coverage.

			  Written this way rather than as the sprite's ladder of comparisons because that
			  is what anti-aliases every ring for free and in the same two lines: a boundary
			  the pixel straddles simply comes out part way through its blend. It also stays
			  correct where two boundaries fall inside one pixel, which at 240p - a 96 pixel
			  disc, whose hub ring and inner ring are about six pixels each - they come
			  close to doing.
			*/
			uint32_t col = back;
			col = disc_mix(col, edge, disc_cover(rr_edge, d));
			col = disc_mix(col, rim,  disc_cover(rr_dark, d));

			if (d < rr_rim + 256)
			{
				/*
				  The iridescence, interpolated rather than stepped. Twelve wedges with hard
				  boundaries are twelve visible seams at this size; the sprite gets away with
				  them because one of its cells is already a block the eye can see. What reads
				  as a disc catching the light is the colour moving, and a ramp moves.
				*/
				int a = disc_ang_q8(Y, X) * nbands;
				int i = (a >> 14) % nbands;
				int f = ((a & 16383) * 255) >> 14;

				uint32_t band = disc_mix(bands[i], bands[(i + 1) % nbands], f);

				col = disc_mix(col, band, disc_cover(rr_rim, d));
				col = disc_mix(col, ring, disc_cover(rr_data, d));
				col = disc_mix(col, edge, disc_cover(rr_ring, d));
				col = disc_mix(col, hole, disc_cover(rr_hole, d));
			}

			dst[x] = col;
		}
	}

	return face_buf;
}

/*
  A progress track of `nseg` boxes, `done` of them finished.

  The segment that is currently being worked on is not filled - it has a block
  travelling across it instead, because the step it stands for has no measurable
  progress inside it and a bar that crept forward would be inventing one. So the track
  says truthfully "three of these five steps are behind us and the fourth is running".

  done >= nseg fills the lot and stops moving, which is what a finished job looks like.

  `live` is the caller's, not derived from `done`, because the two say different things
  and the difference is the point: a job that failed at step two and a job working on
  step two have the same `done`, and only one of them should still be moving. A track
  that swept regardless would say "still going" over a pairing that had given up.
*/
void gfx_track(int x, int y, int w, int h, int nseg, int done, int live, unsigned long ms,
	uint32_t fill, uint32_t track, uint32_t glow)
{
	if (nseg < 1 || w < nseg * 3 || h < 1) return;

	int gap = (h / 3) + 1;
	int segw = (w - gap * (nseg - 1)) / nseg;
	if (segw < 2) { segw = 2; gap = 0; }

	for (int i = 0; i < nseg; i++)
	{
		int sx = x + i * (segw + gap);

		if (i < done) { gfx_fill(sx, y, segw, h, fill); continue; }

		gfx_fill(sx, y, segw, h, track);
		if (i != done || !live) continue;

		// The block sweeps one segment's width every GFX_SWEEP_MS, and is a third of
		// the segment wide so there is always track visible either side of it.
		int bw = segw / 3;
		if (bw < 2) bw = 2;

		int span = segw + bw;
		int at = (int)((ms % GFX_SWEEP_MS) * (unsigned long)span / GFX_SWEEP_MS) - bw;

		int bx = sx + at, bwc = bw;
		if (bx < sx) { bwc += bx - sx; bx = sx; }
		if (bx + bwc > sx + segw) bwc = sx + segw - bx;
		if (bwc > 0) gfx_fill(bx, y, bwc, h, glow);
	}
}

/* ---------------------------------------------------------------- text ---- */

// Up/down/left/right arrows in the same column-major format as charrom:
// byte n is column n, bit y is row y.
static const unsigned char extra_glyphs[5][8] =
{
	{ 0, 0, 0, 0, 0, 0, 0, 0 },
	{ 0x00, 0x40, 0x60, 0x70, 0x70, 0x60, 0x40, 0x00 }, // 1 up
	{ 0x00, 0x04, 0x0C, 0x1C, 0x1C, 0x0C, 0x04, 0x00 }, // 2 down
	{ 0x00, 0x18, 0x3C, 0x7E, 0x7E, 0x00, 0x00, 0x00 }, // 3 left
	{ 0x00, 0x00, 0x00, 0x7E, 0x7E, 0x3C, 0x18, 0x00 }  // 4 right
};

static void draw_glyph(unsigned char code, int x, int y, int s, uint32_t col)
{
	const unsigned char *g = (code >= 1 && code <= 4) ? extra_glyphs[code] : charfont[code];

	for (int gx = 0; gx < 8; gx++)
	{
		unsigned char bits = g[gx];
		if (!bits) continue;
		for (int gy = 0; gy < 8; gy++)
		{
			if (!(bits & (1 << gy))) continue;
			int px = x + gx * s, py = y + gy * s;
			int w = s, h = s;
			if (!clip_rect(&px, &py, &w, &h)) continue;
			for (int yy = py; yy < py + h; yy++)
			{
				uint32_t *p = cb + (size_t)yy * cw + px;
				for (int xx = 0; xx < w; xx++) *p++ = col;
			}
		}
	}
}

int gfx_text_w(const char *s, int scale)
{
	return s ? (int)strlen(s) * GLYPH_W * scale : 0;
}

void gfx_text(const char *s, int x, int y, int scale, uint32_t col, uint32_t shadow)
{
	if (!s || !*s || scale < 1) return;

	int len = (int)strlen(s);
	if (shadow)
	{
		for (int i = 0; i < len; i++)
			draw_glyph((unsigned char)s[i], x + i * GLYPH_W * scale + scale, y + scale, scale, shadow);
	}
	for (int i = 0; i < len; i++)
		draw_glyph((unsigned char)s[i], x + i * GLYPH_W * scale, y, scale, col);

	gfx_damage(x, y, len * GLYPH_W * scale + scale, 8 * scale + scale);
}

void gfx_text_c(const char *s, int cx, int y, int scale, uint32_t col, uint32_t shadow)
{
	gfx_text(s, cx - gfx_text_w(s, scale) / 2, y, scale, col, shadow);
}

const char *gfx_clip(const char *s, int scale, int maxpx)
{
	static char buf[256];
	if (!s) return "";

	int max = maxpx / (GLYPH_W * scale);
	if (max < 1) max = 1;
	if (max > (int)sizeof(buf) - 1) max = (int)sizeof(buf) - 1;

	int len = (int)strlen(s);
	if (len <= max)
	{
		snprintf(buf, sizeof(buf), "%s", s);
		return buf;
	}

	memcpy(buf, s, max);
	buf[max] = 0;
	if (max >= 1) buf[max - 1] = '>';
	return buf;
}

/* ---------------------------------------------------------------- blit ---- */

void gfx_blit(const uint32_t *src, int sw, int sh, int dx, int dy, int dw, int dh)
{
	if (!src || sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return;

	int odx = dx, ody = dy, odw = dw, odh = dh;

	// Fixed-point source stepping, 16.16.
	uint32_t stepx = ((uint32_t)sw << 16) / (uint32_t)dw;
	uint32_t stepy = ((uint32_t)sh << 16) / (uint32_t)dh;
	uint32_t srcx0 = 0, srcy0 = 0;

	// The clip bounds, when set, replace the canvas edges: clipr is kept inside the
	// canvas, so one intersection covers both.
	int bx0 = 0, by0 = 0, bx1 = cw - 1, by1 = ch - 1;
	if (clip_on)
	{
		if (rect_empty(&clipr)) return;
		bx0 = clipr.x0; by0 = clipr.y0; bx1 = clipr.x1; by1 = clipr.y1;
	}

	if (dx < bx0) { srcx0 = (uint32_t)(bx0 - dx) * stepx; dw -= bx0 - dx; dx = bx0; }
	if (dy < by0) { srcy0 = (uint32_t)(by0 - dy) * stepy; dh -= by0 - dy; dy = by0; }
	if (dx + dw > bx1 + 1) dw = bx1 + 1 - dx;
	if (dy + dh > by1 + 1) dh = by1 + 1 - dy;
	if (dw <= 0 || dh <= 0 || !cb) return;

	uint32_t sy = srcy0;
	for (int y = 0; y < dh; y++)
	{
		int syi = (int)(sy >> 16);
		if (syi >= sh) syi = sh - 1;
		const uint32_t *srow = src + (size_t)syi * sw;
		uint32_t *drow = cb + (size_t)(dy + y) * cw + dx;

		uint32_t sx = srcx0;
		for (int x = 0; x < dw; x++)
		{
			int sxi = (int)(sx >> 16);
			if (sxi >= sw) sxi = sw - 1;
			drow[x] = srow[sxi] | 0xff000000u;
			sx += stepx;
		}
		sy += stepy;
	}

	gfx_damage(odx, ody, odw, odh);
}
