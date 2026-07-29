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
