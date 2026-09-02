// See native_fb.h. GPL-3.0, following the tree it lives in.

#include <stdio.h>
#include <string.h>
#include "native_fb.h"
#include "shmem.h"
#include "user_io.h"
#include "cfg.h"
#include "video.h"
#include "osd.h"

#define NFB_CTRL_ADDR   0x3A000000u
#define NFB_BUF0_ADDR   0x3A000100u

// per-mode geometry, mirrors native_video_reader.sv
struct nfb_mode_t { int mode; int width; int lines; };

/*
  Source geometry per raster mode, checked against native_video_reader.sv on the
  consolemode branch: line_words counts 64-bit words, so pixels = line_words * 2, and
  the reader's own total_lines is scan_lines doubled for an interlaced mode.

  Both columns matter and for different reasons. width is the stride the reader walks a
  line with, and lines is what buffer 1 is offset by - the RTL computes
  buf1_addr = BUF0_ADDR + line_words * total_lines, so an interlaced mode's second
  buffer sits a whole frame away even though the reader fetches half of it per field
  (src_line = cur_line*2 + fetch_field). Getting lines wrong there does not tear, it
  puts buffer 1 on top of buffer 0's lower half.

  240p is square-pixel: 640 active samples in an 859-sample line, which is a
  correct 4:3 picture that does not reach the edges of a set expecting the ~720
  sample D1 active width. The 720-wide modes fill the line; all of them are
  either interlaced or 31kHz, which is the trade being offered, not a bug.
*/
static const nfb_mode_t NFB_MODES[8] =
{
	{ 0, 320, 240 },   // NTSC 240p        15kHz progressive
	{ 1, 640, 480 },   // 480i 640         15kHz interlaced
	{ 2, 352, 288 },   // PAL 288p         15kHz progressive
	{ 3, 720, 480 },   // 480i D1          15kHz interlaced, full width
	{ 4, 720, 576 },   // 576i D1          15kHz interlaced, full width
	{ 5, 720, 480 },   // 480p             31kHz - VGA/PVM only
	{ 6, 720, 576 },   // 576p             31kHz - VGA/PVM only
	{ 7, 640, 480 },   // 480p square      31kHz - VGA/PVM only
};
static const nfb_mode_t NFB_NTSC = { 0, 320, 240 };
static const nfb_mode_t NFB_PAL  = { 2, 352, 288 };

static volatile uint32_t *ctrl = 0;
static volatile uint32_t *buf0 = 0;
static uint32_t map_size = 0;
static int active = 0;
static uint32_t frame_counter = 0;
static const nfb_mode_t *cur = &NFB_NTSC;

static const nfb_mode_t *pick_mode()
{
	// An explicit choice wins; 0 keeps the old behaviour, which follows menu_pal.
	if (cfg.classicui_native_mode) return &NFB_MODES[cfg.classicui_native_mode & 7];
	return cfg.menu_pal ? &NFB_PAL : &NFB_NTSC;
}

/*
  Asked from video_menu_fb_present(), so on every frame the front-end paints until the
  answer is yes - which is why the yes is remembered rather than re-derived. A no is not
  cached: the takeover can run before the core's config string has been read, and that
  is exactly the case the late-detect path in video_menu_fb_present() exists to catch.
  Both are dropped when a different core is loaded, because a core load re-runs this
  process anyway - see the firmware lifecycle - but core_id guards the case where it
  does not.
*/
int native_fb_available()
{
	static int cached = 0;
	static char cached_for[128] = {};

	const char *name = user_io_get_core_name(1);
	if (!name) name = "";
	if (strncmp(cached_for, name, sizeof(cached_for) - 1))
	{
		snprintf(cached_for, sizeof(cached_for), "%s", name);
		cached = 0;
	}
	if (cached) return 1;

	// The Console Mode menu core is the only menu whose CONF_STR offers this row.
	int items = 0;
	for (int i = 0; i < 8; i++)
	{
		const char *p = user_io_get_confstr(i);
		if (!p) break;
		items++;
		if (strstr(p, "Video Mode,NTSC 240p"))
		{
			cached = 1;
			return 1;
		}
	}

	// not found: log what we actually saw, once per state, so a debug log settles why
	static int logged = -1;
	if (logged != items)
	{
		logged = items;
		printf("native_fb: not detected, confstr has %d items\n", items);
		for (int i = 0; i < items; i++) printf("native_fb: confstr[%d]=%s\n", i, user_io_get_confstr(i));
	}
	return 0;
}

static int nfb_map()
{
	if (ctrl) return 1;

	// ctrl page + two buffers of the largest raster the core can be put into
	uint32_t buf_bytes = 720 * 4 * 576;
	map_size = (NFB_BUF0_ADDR - NFB_CTRL_ADDR) + buf_bytes * 2;

	void *m = shmem_map(NFB_CTRL_ADDR, map_size);
	if (!m)
	{
		printf("native_fb: shmem_map(0x%08X) failed\n", NFB_CTRL_ADDR);
		return 0;
	}
	ctrl = (volatile uint32_t *)m;
	buf0 = ctrl + (NFB_BUF0_ADDR - NFB_CTRL_ADDR) / 4;
	return 1;
}

/*
  All four fields in one place. user_io_status_set() is a read-modify-write over a cached
  status word, so these do not disturb the bits the menu core is already using ([4] for
  PAL, [8:5] for the fb terminal) - but anything that rewrites the whole word, a core
  reset among them, drops bit 9 and the reader falls back to its test pattern. Enabling
  is a one-shot, so nothing would ever put it back; native_fb_present() therefore checks
  the cache each frame and calls this again if the bit has gone.
*/
static void nfb_status_write()
{
	user_io_status_set("[24:22]", (uint32_t)cur->mode);
	// signed 6-bit centering, 0 = core default; from MiSTer.ini
	user_io_status_set("[15:10]", (uint32_t)(cfg.classicui_native_hoff & 0x3F));
	user_io_status_set("[21:16]", (uint32_t)(cfg.classicui_native_voff & 0x3F));
	user_io_status_set("[9]", 1);
}

void native_fb_enable(int on)
{
	on = on ? 1 : 0;
	if (on == active) return;

	if (on)
	{
		if (!nfb_map()) return;
		cur = pick_mode();

		// black both buffers so the first scan-out is not last core's leftovers
		uint32_t buf_words = cur->width * cur->lines;
		for (uint32_t i = 0; i < buf_words * 2; i++) buf0[i] = 0;
		frame_counter = 0;
		ctrl[0] = 0;
		__sync_synchronize();

		nfb_status_write();
		printf("native_fb: on, mode %d (%dx%d)\n", cur->mode, cur->width, cur->lines);

		// The menu core boots with the stock OSD window blended over core video
		// (in the stock menu the OSD IS the UI); we draw our own UI, so hide it.
		OsdDisable();

		// Program YC/subcarrier for the reader's timing. Must be after 'active'
		// is set below, so set it early - video_mode_adjust checks it.
		active = 1;
		video_mode_adjust(true);
	}
	else
	{
		user_io_status_set("[9]", 0);
		printf("native_fb: off\n");
	}
	active = on;
}

int native_fb_active()
{
	return active;
}

void native_fb_set_mode()
{
	if (!active) return;

	const nfb_mode_t *want = pick_mode();
	if (want == cur) return;

	cur = want;

	// Geometry changed, so both buffers hold garbage for the new raster.
	uint32_t buf_words = cur->width * cur->lines;
	for (uint32_t i = 0; i < buf_words * 2; i++) buf0[i] = 0;
	frame_counter = 0;
	ctrl[0] = 0;
	__sync_synchronize();

	user_io_status_set("[24:22]", (uint32_t)cur->mode);
	printf("native_fb: mode %d (%dx%d)\n", cur->mode, cur->width, cur->lines);

	// Re-measure the new timing so the subcarrier is programmed for it.
	video_mode_adjust(true);
}

void native_fb_apply_offsets()
{
	if (!active) return;
	user_io_status_set("[15:10]", (uint32_t)(cfg.classicui_native_hoff & 0x3F));
	user_io_status_set("[21:16]", (uint32_t)(cfg.classicui_native_voff & 0x3F));
	printf("native_fb: offsets h=%d v=%d\n", cfg.classicui_native_hoff, cfg.classicui_native_voff);
}

int native_fb_present(const uint32_t *src, int w, int h, int n)
{
	if (!active || !src || !buf0 || w < 2 || h < 2) return 0;

	// see nfb_status_write(): cheap, and it is the difference between a colour picture
	// and the core's test pattern after anything that rewrites the status word.
	if (!user_io_status_get("[9]"))
	{
		printf("native_fb: enable bit was cleared, re-asserting\n");
		nfb_status_write();
	}

	int b = n & 1;
	volatile uint32_t *dst = buf0 + (uint32_t)b * cur->width * cur->lines;

	if (w <= cur->width && h <= cur->lines)
	{
		// canvas fits: center it (PAL: 320-wide canvas in a 352 line)
		int xo = (cur->width - w) / 2;
		int yo = (cur->lines - h) / 2;
		for (int y = 0; y < h; y++)
		{
			const uint64_t *s = (const uint64_t *)(src + (size_t)y * w);
			volatile uint64_t *d = (volatile uint64_t *)(dst + (size_t)(y + yo) * cur->width + xo);
			int words = w / 2;
			for (int i = 0; i < words; i++) d[i] = s[i];
			if (w & 1) dst[(size_t)(y + yo) * cur->width + xo + w - 1] = src[(size_t)y * w + w - 1];
		}
	}
	else
	{
		// canvas is bigger than the raster in at least one axis (front-end display
		// settings can resize it to 640-wide / 480-tall): nearest-neighbour downscale
		// to the full raster. 16.16 fixed point steps.
		uint32_t xstep = ((uint32_t)w << 16) / cur->width;
		uint32_t ystep = ((uint32_t)h << 16) / cur->lines;
		uint32_t yacc = ystep / 2;
		for (int y = 0; y < cur->lines; y++, yacc += ystep)
		{
			const uint32_t *srow = src + (size_t)(yacc >> 16) * w;
			volatile uint32_t *d = dst + (size_t)y * cur->width;
			uint32_t xacc = xstep / 2;
			for (int x = 0; x < cur->width; x++, xacc += xstep) d[x] = srow[xacc >> 16];
		}
	}

	__sync_synchronize();
	frame_counter++;
	ctrl[0] = (frame_counter << 2) | (uint32_t)b;
	__sync_synchronize();
	return 1;
}

