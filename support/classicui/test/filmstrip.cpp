/*
  Classic Home riffle filmstrip.

  Boots the real front-end against a tiny fake card - one PSX game in three discs,
  two single-file neighbours, local covers for all of them - selects the multi-disc
  card, presses X, and dumps every 16 ms frame of the riffle as a PNG. The point is
  judging the MOTION on a laptop, frame by frame, with no device in the loop: at
  device speed a 400 ms choreography is over before it can be criticised, which is
  exactly how the first cut's faults survived to hardware.

  Same modules as the harness and the viewer; only the platform is faked
  (stubs.cpp), and the clock is the fake one, stepped 16 ms per frame, so the strip
  is deterministic - two runs produce byte-identical frames.

  Build and run with filmstrip.sh; frames land in the directory it mounts as /out.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <linux/input.h>

#include "../../../cfg.h"
#include "../../../input.h"
#include "../chome.h"
#include "../chome_lib.h"
#include "../chome_art.h"
#include "../chome_theme.h"
#include "../chome_gfx.h"
#include "../chome_video.h"
#include "../../../lib/imlib2/Imlib2.h"

#include "harness.h"

#define ROOT "/tmp/chome_strip"
#define OUT  "/out"

static void mkpath(const char *p)
{
	char tmp[1024];
	snprintf(tmp, sizeof(tmp), "%s", p);
	for (char *q = tmp + 1; *q; q++)
	{
		if (*q != '/') continue;
		*q = 0;
		mkdir(tmp, 0777);
		*q = '/';
	}
	mkdir(tmp, 0777);
}

static void touch(const char *dir, const char *name)
{
	char p[1024];
	snprintf(p, sizeof(p), "%s/%s", dir, name);
	FILE *f = fopen(p, "wb");
	if (!f) return;
	for (int i = 0; i < 1024; i++) fputc(i & 0xff, f);
	fclose(f);
}

/*
  A cover that says which disc it is without a font: a coloured plate carrying N
  thick pale bars. Disc 1 wears one bar, disc 3 wears three - so in the strip it is
  always obvious WHICH cover is moving where, which is the entire job.
*/
static void make_cover(const char *path, uint32_t col, int bars)
{
	const int w = 400, h = 300;
	Imlib_Image im = imlib_create_image(w, h);
	if (!im) return;

	imlib_context_set_image(im);
	imlib_image_set_has_alpha(0);
	uint32_t *d = (uint32_t*)imlib_image_get_data();

	for (int i = 0; i < w * h; i++) d[i] = col;
	for (int b = 0; b < bars; b++)
	{
		int by = h / 5 + b * (h / 5);
		for (int yy = by; yy < by + h / 10; yy++)
			for (int xx = w / 6; xx < w - w / 6; xx++)
				d[yy * w + xx] = 0xffe8e9f0u;
	}
	// A dark frame, so a cover's edge is visible against anything.
	for (int xx = 0; xx < w; xx++) { d[xx] = 0xff101118u; d[(h - 1) * w + xx] = 0xff101118u; }
	for (int yy = 0; yy < h; yy++) { d[yy * w] = 0xff101118u; d[yy * w + w - 1] = 0xff101118u; }

	imlib_image_put_back_data(d);
	imlib_image_set_format("png");
	imlib_save_image(path);
	imlib_free_image();
}

static void build_sd()
{
	if (system("rm -rf " ROOT)) {}
	mkpath(ROOT "/config");
	mkpath(ROOT "/games/PSX");

	touch(ROOT "/games/PSX", "Alundra (USA).cue");
	touch(ROOT "/games/PSX", "Final Fantasy VII (USA) (Disc 1).cue");
	touch(ROOT "/games/PSX", "Final Fantasy VII (USA) (Disc 2).cue");
	touch(ROOT "/games/PSX", "Final Fantasy VII (USA) (Disc 3).cue");
	touch(ROOT "/games/PSX", "Vagrant Story (USA).cue");

	mkpath(ROOT "/boxart/Sony - PlayStation/Named_Boxarts");
	make_cover(ROOT "/boxart/Sony - PlayStation/Named_Boxarts/Alundra (USA).png",
		0xff4a9e4eu, 0);
	make_cover(ROOT "/boxart/Sony - PlayStation/Named_Boxarts/Final Fantasy VII (USA) (Disc 1).png",
		0xff2e6fb8u, 1);
	make_cover(ROOT "/boxart/Sony - PlayStation/Named_Boxarts/Final Fantasy VII (USA) (Disc 2).png",
		0xffc4353cu, 2);
	make_cover(ROOT "/boxart/Sony - PlayStation/Named_Boxarts/Final Fantasy VII (USA) (Disc 3).png",
		0xffe8b22bu, 3);
	make_cover(ROOT "/boxart/Sony - PlayStation/Named_Boxarts/Vagrant Story (USA).png",
		0xff8e8f99u, 0);
}

static void frame(int n)
{
	for (int i = 0; i < n; i++)
	{
		harness_advance(16);
		chome_handle(0);
	}
}

static void press(int key)
{
	chome_handle((uint32_t)key);
	harness_advance(16);
	chome_handle((uint32_t)key | UPSTROKE);
	frame(30);
}

static void dumpf(const char *name)
{
	char path[512];
	snprintf(path, sizeof(path), OUT "/%s", name);

	Imlib_Image im = imlib_create_image_using_copied_data(
		gfx_w(), gfx_h(), (uint32_t*)harness_fb_shown());
	if (!im) return;
	imlib_context_set_image(im);
	imlib_image_set_has_alpha(0);
	imlib_image_set_format("png");
	imlib_save_image(path);
	imlib_free_image();
	printf("  %s\n", name);
}

int main()
{
	setvbuf(stdout, 0, _IOLBF, 0);

	build_sd();
	harness_set_root(ROOT);

	cfg.classicui = 1;
	cfg.classicui_artfetch = 0;
	cfg.classicui_tracking = 0;
	cfg.classicui_caps = 1;
	snprintf(cfg.classicui_artdir, sizeof(cfg.classicui_artdir), "boxart");

	harness_set_fb(1280, 720);
	theme_update(1280, 720, 1);

	// Boot: let the scan finish, then let the visible covers decode.
	for (int i = 0; i < 200 && lib_scanning(); i++) frame(1);
	frame(80);

	// Onto the three-disc card. The root shelf leads with folders, so walk right
	// until the selected entry is the group - and refuse to guess if it never is.
	int guard = 0;
	for (; guard < 60; guard++)
	{
		const chome_entry *e = lib_view_entry(chome_sel_index());
		if (e && e->kind == ENT_GAME && e->nvar == 3) break;
		press(KEY_RIGHT);
	}
	if (guard == 60) { printf("no three-disc card found on the shelf\n"); return 1; }

	// Let the preloader decode the NEXT disc's cover too - the riffle's whole
	// point is that it comes forward already dressed.
	frame(80);

	dumpf("strip-00-rest.png");

	// The riffle: press X, then one dump per 16 ms frame until well past landing.
	chome_handle(KEY_TAB);
	chome_handle(KEY_TAB | UPSTROKE);
	char name[64];
	// The riffle is VER_RIFFLE_MS long (chome_ui.cpp); frames past its end just
	// prove the landing holds still, so a stale count here errs harmless.
	int frames = 32;
	for (int f = 1; f <= frames; f++)
	{
		snprintf(name, sizeof(name), "strip-%02d.png", f);
		dumpf(name);
		harness_advance(16);
		chome_handle(0);
	}
	dumpf("strip-99-rest.png");

	printf("done: %d riffle frames\n", frames);
	return 0;
}
