#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "chome_gfx.h"
#include "../../video.h"
#include "../../charrom.h"

static uint32_t *cb = 0;      // compose buffer, cached RAM
static int cw = 0, ch = 0;
static int fbn = 1;           // framebuffer we will draw into next

struct rect_t { int x0, y0, x1, y1; };

static rect_t dmg_cur;        // damage submitted this frame
static rect_t dmg_prev;       // damage of the previous frame

static int dmg_rows = 0;

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
	if (w <= 0 || h <= 0) return;
	rect_add(&dmg_cur, x, y, x + w - 1, y + h - 1);
}

void gfx_damage_all()
{
	if (cw > 0 && ch > 0) rect_add(&dmg_cur, 0, 0, cw - 1, ch - 1);
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

void gfx_end()
{
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
			int x = u.x0;
			int bytes = (u.x1 - u.x0 + 1) * sizeof(uint32_t);
			for (int y = u.y0; y <= u.y1; y++)
			{
				memcpy(fb + (size_t)y * cw + x, cb + (size_t)y * cw + x, bytes);
			}
			video_menu_fb_present(fbn);
			fbn = (fbn == 1) ? 2 : 1;
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
}

// Clip a rect to the canvas. Returns 0 if nothing is left.
static int clip_rect(int *x, int *y, int *w, int *h)
{
	if (!cb) return 0;
	if (*x < 0) { *w += *x; *x = 0; }
	if (*y < 0) { *h += *y; *y = 0; }
	if (*x + *w > cw) *w = cw - *x;
	if (*y + *h > ch) *h = ch - *y;
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
		for (int xx = x + phase; xx < x + w; xx += step) row[xx] = col;
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
	  r=32 gives 64px at 2x2. Callers pass multiples of 16 to keep cells whole.
	*/
	int cell = r / 16;
	if (cell < 1) cell = 1;

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
	  One cell wider than the disc when there is an outline to draw, which is how focus is
	  shown: a ring just outside it. A filled plate behind the disc was the alternative
	  and it covered the shelf title at 240p.
	*/
	int g = outline ? 17 : 16;
	const int r2_out = 34 * 34;

	for (int gy = -g; gy < g; gy++)
	{
		int Y = 2 * gy + 1;

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

			gfx_fill(cx + gx * cell, cy + gy * cell, cell, cell, col);
		}
	}
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

	if (dx < 0) { srcx0 = (uint32_t)(-dx) * stepx; dw += dx; dx = 0; }
	if (dy < 0) { srcy0 = (uint32_t)(-dy) * stepy; dh += dy; dy = 0; }
	if (dx + dw > cw) dw = cw - dx;
	if (dy + dh > ch) dh = ch - dy;
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
