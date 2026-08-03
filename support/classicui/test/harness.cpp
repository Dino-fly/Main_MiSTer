/*
  Classic Home host test harness.

  Builds a fake SD card, runs the real front-end over it, walks every screen at
  every layout profile, writes a PNG of each, and asserts the things that would
  otherwise only fail on hardware.

  Build and run: support/classicui/test/run.sh
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <linux/input.h>

#include "../../../cfg.h"
#include "../../../input.h"
#include "../chome.h"
#include "../chome_lib.h"
#include "../chome_core.h"
#include "../chome_art.h"
#include "../chome_theme.h"
#include "../chome_gfx.h"
#include "../chome_video.h"
#include "../chome_osk.h"
#include "../chome_net.h"
#include "../chome_bt.h"
#include "../chome_ini.h"
#include "../chome_opt.h"
#include "../chome_icons32.h"
#include "../chome_btn12.h"
#include "../../../lib/imlib2/Imlib2.h"
#include "../../../lib/miniz/miniz.h"

#include "harness.h"

#define ROOT "/tmp/chome_sd"
#define OUT  "/tmp/chome_out"

static int fails = 0;
static int checks = 0;

static void check(int cond, const char *what)
{
	checks++;
	if (cond) printf("  ok    %s\n", what);
	else { printf("  FAIL  %s\n", what); fails++; }
}

/* ------------------------------------------------------------ fake SD ----- */

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

static void touch(const char *dir, const char *name, int bytes)
{
	char p[1024];
	snprintf(p, sizeof(p), "%s/%s", dir, name);
	FILE *f = fopen(p, "wb");
	if (!f) { printf("  cannot create %s\n", p); return; }
	for (int i = 0; i < bytes; i++) fputc(i & 0xff, f);
	fclose(f);
}

/* -------------------------------------------------------- ini fixtures ---- */

static void put_file(const char *path, const char *text)
{
	FILE *f = fopen(path, "wb");                 // binary, or the fixture is not CRLF
	if (!f) { printf("  cannot write %s\n", path); return; }
	fwrite(text, 1, strlen(text), f);
	fclose(f);
}

static int slurp_file(const char *path, char *buf, int max)
{
	FILE *f = fopen(path, "rb");
	if (!f) return -1;
	int n = (int)fread(buf, 1, (size_t)max - 1, f);
	fclose(f);
	buf[n] = 0;
	return n;
}

/*
  Lines that differ between the two files and are not an assignment of one of our keys.
  The point of the rewrite is that this is zero: a player's comments, spacing, ordering
  and unrelated settings come through untouched.

  It walks the source's lines only, so the block appended at the end is not counted -
  that one is asserted for on its own. And a line whose key merely starts with one of
  ours would be forgiven here, so the prefix case has its own check too.
*/
static int ini_stray_lines(const char *a, const char *b)
{
	int stray = 0;
	const char *pa = a, *pb = b;

	while (*pa)
	{
		const char *ea = strchr(pa, '\n');
		ea = ea ? ea + 1 : pa + strlen(pa);
		const char *eb = strchr(pb, '\n');
		eb = eb ? eb + 1 : pb + strlen(pb);

		size_t la = (size_t)(ea - pa), lb = (size_t)(eb - pb);
		if (la != lb || memcmp(pa, pb, la))
		{
			const char *q = pa;
			while (*q == ' ' || *q == '\t') q++;

			// Ours means either table's: Best Settings writes its own set, and the
			// settings screen writes whichever of its options the player moved.
			int ours = 0;
			for (int w = 0; w < ini_want_count(); w++)
				if (!strncasecmp(q, ini_want_at(w)->key, strlen(ini_want_at(w)->key))) ours = 1;
			for (int w = 0; w < opt_count(); w++)
				if (!strncasecmp(q, opt_at(w)->key, strlen(opt_at(w)->key))) ours = 1;

			if (!ours) { printf("  stray change: %.*s\n", (int)la, pa); stray++; }
		}

		pa = ea;
		pb = eb;
		if (!*pb && *pa) { printf("  the rewrite is short of lines\n"); stray++; break; }
	}
	return stray;
}

// A stand-in cover: coloured plate, a band, and a diagonal, so we can see
// aspect handling (deliberately a tall image, unlike the card).
static void make_cover(const char *path, int w, int h, uint32_t col)
{
	Imlib_Image im = imlib_create_image(w, h);
	if (!im) { printf("  imlib_create_image failed\n"); return; }

	imlib_context_set_image(im);
	imlib_image_set_has_alpha(0);
	uint32_t *d = (uint32_t*)imlib_image_get_data();

	for (int y = 0; y < h; y++)
	{
		for (int x = 0; x < w; x++)
		{
			uint32_t c = col;
			if (y > h * 3 / 4) c = 0xff202028;
			if (x == y || (w - 1 - x) == y) c = 0xffffffff;
			d[y * w + x] = 0xff000000u | c;
		}
	}

	imlib_image_put_back_data((DATA32*)d);
	imlib_image_set_format("png");
	imlib_save_image(path);
	imlib_free_image();
}

/*
  A real archive, since the point is to exercise the zip reader rather than a
  stand-in for it. `inner` may name several members, comma separated.
*/
static void make_zip(const char *dir, const char *name, const char *inner, int bytes)
{
	char path[1024];
	snprintf(path, sizeof(path), "%s/%s", dir, name);

	mz_zip_archive z;
	memset(&z, 0, sizeof(z));
	if (!mz_zip_writer_init_file(&z, path, 0)) { printf("  cannot write %s\n", path); return; }

	char *buf = (char*)malloc(bytes);
	for (int i = 0; i < bytes; i++) buf[i] = (char)(i & 0xff);

	char list[512];
	snprintf(list, sizeof(list), "%s", inner);
	for (char *tok = strtok(list, ","); tok; tok = strtok(NULL, ","))
	{
		mz_zip_writer_add_mem(&z, tok, buf, bytes, MZ_BEST_SPEED);
	}

	mz_zip_writer_finalize_archive(&z);
	mz_zip_writer_end(&z);
	free(buf);
}

static void build_sd()
{
	printf("Building fake SD at %s\n", ROOT);

	char cmd[512];
	snprintf(cmd, sizeof(cmd), "rm -rf %s %s", ROOT, OUT);
	if (system(cmd)) {}

	mkpath(ROOT "/config");
	mkpath(OUT);

	mkpath(ROOT "/games/SNES");
	touch(ROOT "/games/SNES", "Super Metroid (Europe).sfc", 4096);
	touch(ROOT "/games/SNES", "Super Mario World (Europe).sfc", 4096);
	touch(ROOT "/games/SNES", "The Legend of Zelda - A Link to the Past (Europe).sfc", 4096);
	touch(ROOT "/games/SNES", "notes.txt", 10);                 // must be ignored

	mkpath(ROOT "/games/SNES/Hacks");                            // recursion
	touch(ROOT "/games/SNES/Hacks", "Super Demo World.smc", 2048);

	// Zipped ROMs, which is how most cards actually store them.
	make_zip(ROOT "/games/SNES", "Secret of Mana (USA).zip", "Secret of Mana (USA).sfc", 4096);
	make_zip(ROOT "/games/SNES", "Capcom Collection.zip", "Final Fight.sfc,Mega Man X.sfc", 2048);
	make_zip(ROOT "/games/SNES", "Manual Scans.zip", "readme.txt", 512);

	mkpath(ROOT "/games/Genesis");
	touch(ROOT "/games/Genesis", "Sonic The Hedgehog 2 (Europe).md", 4096);
	touch(ROOT "/games/Genesis", "Streets of Rage 2 (Europe).bin", 4096);

	mkpath(ROOT "/games/TGFX16");
	touch(ROOT "/games/TGFX16", "Bonk's Adventure (USA).pce", 2048);

	mkpath(ROOT "/games/GBA");
	touch(ROOT "/games/GBA", "Metroid Fusion (Europe).gba", 2048);

	// A DMG cart and a GBC cart in the same Game Boy core: they must be offered
	// different screens.
	mkpath(ROOT "/games/SMS");
	touch(ROOT "/games/SMS", "Sonic The Hedgehog 2 (Europe) (GG).gg", 2048);

	mkpath(ROOT "/games/AtariLynx");
	touch(ROOT "/games/AtariLynx", "Chip's Challenge (USA).lnx", 2048);

	mkpath(ROOT "/games/WonderSwan");
	touch(ROOT "/games/WonderSwan", "Gunpey (Japan).ws", 2048);
	touch(ROOT "/games/WonderSwan", "Rockman EXE (Japan).wsc", 2048);

	mkpath(ROOT "/games/GAMEBOY");
	touch(ROOT "/games/GAMEBOY", "Tetris (World).gb", 2048);
	touch(ROOT "/games/GAMEBOY", "Zelda - Oracle of Ages (Europe).gbc", 2048);

	mkpath(ROOT "/_Arcade");
	touch(ROOT "/_Arcade", "Street Fighter II.mra", 512);
	touch(ROOT "/_Arcade", "Bubble Bobble.mra", 512);

	// Alternate ROM revisions live here in every arcade pack; they must not reach
	// the shelf, or each game shows up several times over.
	mkpath(ROOT "/_Arcade/_alternatives");
	touch(ROOT "/_Arcade/_alternatives", "Street Fighter II (alt rev).mra", 512);

	// Neo Geo romsets: named for the board, titled from romsets.xml, and the BIOS
	// set is marked hidden so it must not be listed as a game.
	mkpath(ROOT "/games/NEOGEO");
	make_zip(ROOT "/games/NEOGEO", "mslug.zip", "202-c1.c1", 2048);
	make_zip(ROOT "/games/NEOGEO", "kof98.zip", "242-c1.c1", 2048);
	make_zip(ROOT "/games/NEOGEO", "unknownset.zip", "999-c1.c1", 2048);
	make_zip(ROOT "/games/NEOGEO", "neogeo.zip", "sfix.sfix", 1024);
	touch(ROOT "/games/NEOGEO", "sfix.sfix", 1024);          // system file, not a game

	// A Darksoft-style romset: a folder of member files, which is the game itself
	// rather than a folder to walk into.
	mkpath(ROOT "/games/NEOGEO/samsho2");
	touch(ROOT "/games/NEOGEO/samsho2", "prom", 2048);
	touch(ROOT "/games/NEOGEO/samsho2", "crom0", 4096);
	touch(ROOT "/games/NEOGEO/samsho2", "srom", 1024);

	// ...and a folder that is not one, so it is still walked into as usual.
	mkpath(ROOT "/games/NEOGEO/Homebrew");
	make_zip(ROOT "/games/NEOGEO/Homebrew", "lasthope.zip", "251-p1.p1", 2048);

	// A computer system: should not appear on the shelf, only under Computers.
	mkpath(ROOT "/games/Amiga");
	touch(ROOT "/games/Amiga", "Turrican II.adf", 2048);
	touch(ROOT "/games/Amiga", "Lemmings.adf", 2048);
	mkpath(ROOT "/games/Amiga/Demos");
	touch(ROOT "/games/Amiga/Demos", "State of the Art.adf", 2048);

	// Savestates: slot 1 and 2 occupied for Super Metroid, plus a thumbnail
	// for slot 1 only, so we can see both thumbnail and cover fallback.
	mkpath(ROOT "/savestates/SNES");
	touch(ROOT "/savestates/SNES", "Super Metroid (Europe)_1.ss", 256);
	touch(ROOT "/savestates/SNES", "Super Metroid (Europe)_2.ss", 256);
	make_cover(ROOT "/savestates/SNES/Super Metroid (Europe)_1.png", 320, 240, 0xff1e6fa8);

	// The Game Boy game gets a state too, so the in-game load path has something
	// to act on.
	mkpath(ROOT "/savestates/Gameboy");
	touch(ROOT "/savestates/Gameboy", "Tetris (World)_1.ss", 256);
	make_cover(ROOT "/savestates/Gameboy/Tetris (World)_1.png", 320, 288, 0xff70a030);

	// Bonk sorts first alphabetically, so it is what the screen walk lands on:
	// give it slots 1 and 3 plus one thumbnail so the strip is worth looking at.
	mkpath(ROOT "/savestates/TurboGrafx16");
	touch(ROOT "/savestates/TurboGrafx16", "Bonk's Adventure (USA)_1.ss", 256);
	touch(ROOT "/savestates/TurboGrafx16", "Bonk's Adventure (USA)_3.ss", 256);
	make_cover(ROOT "/savestates/TurboGrafx16/Bonk's Adventure (USA)_1.png", 320, 240, 0xff8a6e2b);

	// A reference frame for one Game Boy game, as the game core would have grabbed
	// it: the Display previews must use this instead of the test pattern.
	mkpath(ROOT "/classicui/refshots/gb");
	make_cover(ROOT "/classicui/refshots/gb/Tetris (World).png", 320, 288, 0xff3f8fd0);

	// Cover art in the libretro layout for two games only: everything else must
	// fall back to the generated card.
	mkpath(ROOT "/boxart/Nintendo - Super Nintendo Entertainment System/Named_Boxarts");
	make_cover(ROOT "/boxart/Nintendo - Super Nintendo Entertainment System/Named_Boxarts/Super Metroid (Europe).png", 500, 700, 0xff5b4b8a);
	make_cover(ROOT "/boxart/Nintendo - Super Nintendo Entertainment System/Named_Boxarts/Super Mario World (Europe).png", 700, 500, 0xff3fa7e0);

	mkpath(ROOT "/boxart/Sega - Mega Drive - Genesis/Named_Boxarts");
	make_cover(ROOT "/boxart/Sega - Mega Drive - Genesis/Named_Boxarts/Sonic The Hedgehog 2 (Europe).png", 600, 600, 0xff2b4c7e);
}

/* --------------------------------------------------------------- driving -- */

/*
  Real `iw dev wlan0 scan` shape, with the four things that actually turn up in it:
  the same network on two radios, an entry whose name is blank, one whose name is
  padding bytes printed as \x00, and one whose encryption is only visible in the
  capability line. The names are invented; the layout is not.
*/
static const char *SCAN_TEXT =
	"BSS 11:22:33:44:55:66(on wlan0)\n"
	"\tTSF: 62859829328 usec (0d, 17:27:39)\n"
	"\tfreq: 2412\n"
	"\tcapability: ESS ShortSlotTime (0x0411)\n"
	"\tsignal: -82.00 dBm\n"
	"\tSSID: Cafe Guest\n"
	"\tSupported rates: 1.0* 2.0* 5.5* 11.0*\n"
	"BSS aa:bb:cc:dd:ee:f0(on wlan0) -- associated\n"
	"\tfreq: 2462\n"
	"\tcapability: ESS Privacy ShortSlotTime (0x1411)\n"
	"\tsignal: -60.00 dBm\n"
	"\tSSID: BrainDamage\n"
	"\tRSN:\t * Version: 1\n"
	"\t\t * Group cipher: CCMP\n"
	"BSS aa:bb:cc:dd:ee:f1(on wlan0)\n"
	"\tfreq: 5180\n"
	"\tsignal: -48.00 dBm\n"
	"\tSSID: BrainDamage\n"
	"\tRSN:\t * Version: 1\n"
	"BSS 00:22:6c:05:cb:a5(on wlan0)\n"
	"\tfreq: 2462\n"
	"\tsignal: -74.00 dBm\n"
	"\tSSID: \n"
	"\tRSN:\t * Version: 1\n"
	"BSS 6a:6c:9a:1e:32:db(on wlan0)\n"
	"\tfreq: 2462\n"
	"\tsignal: -77.00 dBm\n"
	"\tSSID: \\x00\\x00\\x00\\x00\\x00\n"
	"\tRSN:\t * Version: 1\n"
	"BSS 12:12:12:12:12:12(on wlan0)\n"
	"\tfreq: 2437\n"
	"\tcapability: ESS Privacy (0x1431)\n"
	"\tsignal: -70.00 dBm\n"
	"\tSSID: Neighbour 2.4\n";

static const char *LINK_TEXT =
	"Connected to ec:6c:9a:1e:32:d9 (on wlan0)\n"
	"\tSSID: BrainDamage\n"
	"\tfreq: 2462\n"
	"\tsignal: -76 dBm\n"
	"\ttx bitrate: 72.2 MBit/s\n";

static void frame(int n = 1)
{
	for (int i = 0; i < n; i++)
	{
		harness_advance(16);
		chome_handle(0);
	}
}

static void press(int key, int settle = 12)
{
	chome_handle(key);
	harness_advance(16);
	chome_handle(key | UPSTROKE);
	frame(settle);
}

/*
  A fingerprint of whatever panel is up. Panels here are centred and at least half the
  canvas wide at every profile, so the middle half of the middle of the screen is inside
  one and outside the shelf - which is what makes two of these comparable across steps.
*/
static unsigned long pt_panel_hash()
{
	int w = gfx_w(), h = gfx_h();
	return harness_fb_hash_box(w / 4, h / 2 - h / 6, (3 * w) / 4, h / 2 + h / 6);
}

/*
  The panel rectangle draw_panel() uses, which is where the settings screen lives. Two
  things are read out of it below: a hash, for "did anything on this panel change", and
  a count of one exact colour, for the amber that marks a value away from its default.
  gfx_fill() and the font both write colours through unblended, so an exact match is
  legitimate - the same reason the legend and the suspend strip can be read this way.
*/
static void panel_rect(int *x0, int *y0, int *x1, int *y1)
{
	const chome_profile *p = theme_get();
	*x0 = (p->w - p->panel_w) / 2;
	*y0 = (p->h - p->panel_h) / 2;
	*x1 = *x0 + p->panel_w;
	*y1 = *y0 + p->panel_h;
}

static unsigned long panel_hash()
{
	int x0, y0, x1, y1;
	panel_rect(&x0, &y0, &x1, &y1);
	return harness_fb_hash_box(x0, y0, x1, y1);
}

/*
  A count of one exact colour over the settings panel's rows - the rows alone, and
  deliberately so: the footer names the recommended value in the very colour the rows use
  for "not that value", so counting the whole panel would find amber whatever the rows
  were drawn in, which is exactly the mistake this avoids. The two constants are draw_settings()'s header and footer heights
  and have to move with it.
*/
static int panel_rows_pixels(uint32_t want)
{
	const chome_profile *p = theme_get();
	int s = p->ts_ui;

	int x0, y0, x1, y1;
	panel_rect(&x0, &y0, &x1, &y1);
	y0 += 10 * s + 6;
	y1 -= 3 * 10 * s + 4 * s;

	const uint32_t *fb = harness_fb_shown();
	int w = gfx_w(), h = gfx_h();
	if (!fb || w < 1 || h < 1) return 0;

	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (x1 > w) x1 = w;
	if (y1 > h) y1 = h;

	int n = 0;
	for (int y = y0; y < y1; y++)
		for (int x = x0; x < x1; x++)
			if ((fb[(size_t)y * w + x] | 0xff000000u) == want) n++;

	return n;
}

/*
  How many pixels in a box are exactly one colour. Same licence as panel_rows_pixels()
  above: gfx_fill() and the font both write colours through unblended, so an exact
  match finds what was drawn in that colour and nothing else.

  It is how the activity indicators are checked. gfx_spinner() fills its head at full
  strength and blends only the tail, so a count of the ring's own colour is a count of
  heads - one when the ring is up, none when it is not.
*/
static int box_pixels(int x0, int y0, int x1, int y1, uint32_t want)
{
	const uint32_t *fb = harness_fb_shown();
	int w = gfx_w(), h = gfx_h();
	if (!fb || w < 1 || h < 1) return 0;

	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (x1 > w) x1 = w;
	if (y1 > h) y1 = h;

	int n = 0;
	for (int y = y0; y < y1; y++)
		for (int x = x0; x < x1; x++)
			if ((fb[(size_t)y * w + x] | 0xff000000u) == want) n++;

	return n;
}

// Over the same box pt_panel_hash() fingerprints, which at every profile is inside the
// Wi-Fi and Controllers panels and outside the shelf behind them.
static int panel_pixels(uint32_t want)
{
	int w = gfx_w(), h = gfx_h();
	return box_pixels(w / 4, h / 2 - h / 6, (3 * w) / 4, h / 2 + h / 6, want);
}

static int opt_find(const char *key)
{
	for (int i = 0; i < opt_count(); i++)
		if (!strcasecmp(opt_at(i)->key, key)) return i;
	return -1;
}

static void dump(const char *name)
{
	int w = gfx_w(), h = gfx_h();
	uint32_t *src = harness_fb_shown();
	if (!src || w < 1) { printf("  no framebuffer to dump for %s\n", name); return; }

	// Copy: imlib takes ownership semantics we do not want on the live buffer.
	uint32_t *copy = (uint32_t*)malloc((size_t)w * h * 4);
	if (!copy) return;
	memcpy(copy, src, (size_t)w * h * 4);
	for (int i = 0; i < w * h; i++) copy[i] |= 0xff000000u;

	Imlib_Image im = imlib_create_image_using_data(w, h, (DATA32*)copy);
	if (im)
	{
		char path[1024];
		snprintf(path, sizeof(path), "%s/%s.png", OUT, name);
		imlib_context_set_image(im);
		imlib_image_set_has_alpha(0);
		imlib_image_set_format("png");
		imlib_save_image(path);
		imlib_free_image();
		printf("  wrote %s.png (%dx%d)\n", name, w, h);
	}
	free(copy);
}

// Three folders lead the root shelf, so three presses land on the first game.
// With SORT_TITLE that is deterministically Bonk's Adventure, which the fake SD
// gives savestates and a thumbnail.
static int leading_folders()
{
	int n = 0;
	for (int i = 0; i < lib_view_count(); i++)
	{
		if (lib_view_entry(i)->kind == ENT_GAME) break;
		n++;
	}
	return n;
}

static void select_first_game()
{
	// Re-entry keeps the previous shelf position by design, so rewind to the
	// start before counting. LEFT clamps at index 0.
	for (int i = 0; i < 30; i++) press(KEY_LEFT, 2);

	int n = leading_folders();
	for (int i = 0; i < n; i++) press(KEY_RIGHT, 6);
}

// Rewinds and steps right until the named game is selected on the root shelf.
static int select_titled(const char *want)
{
	for (int i = 0; i < 40; i++) press(KEY_LEFT, 2);

	int target = -1;
	for (int i = 0; i < lib_view_count(); i++)
	{
		const chome_entry *e = lib_view_entry(i);
		if (e->kind != ENT_GAME) continue;
		chome_item *it = lib_item(e->game);
		if (it && strstr(it->title, want)) { target = i; break; }
	}
	if (target < 0) return 0;

	for (int i = 0; i < target; i++) press(KEY_RIGHT, 3);
	frame(10);
	return 1;
}

// The whole point of the contextual list: capture it for each kind of hardware.
static void walk_looks()
{
	printf("\n== contextual look lists ==\n");

	cfg.classicui_profile = 1;
	harness_set_fb(1280, 720);
	gfx_shutdown();
	theme_update(1280, 720, 1);
	chome_leave();
	press(KEY_MENU, 20);
	frame(20);

	struct { const char *title; const char *tag; } cases[] =
	{
		{ "Tetris",   "look-for-gameboy" },
		{ "Zelda - Oracle", "look-for-gbc" },
		{ "Metroid Fusion", "look-for-gba" },
		{ "Sonic",    "look-for-console" },
	};

	for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++)
	{
		if (!select_titled(cases[c].title)) { printf("  %s not found\n", cases[c].title); continue; }

		press(KEY_UP, 18);          // menu bar
		press(KEY_ENTER, 22);       // Display
		dump(cases[c].tag);
		press(KEY_ESC, 8);
		press(KEY_ESC, 8);
	}
}

static void walk_profile(const char *tag, int profile, int w, int h)
{
	printf("\n== profile %s (%dx%d) ==\n", tag, w, h);

	cfg.classicui_profile = (uint8_t)profile;
	harness_set_fb(w, h);
	gfx_shutdown();
	theme_update(w, h, profile);

	// Re-enter cleanly.
	chome_leave();
	press(KEY_MENU, 20);

	// Let the background scan finish.
	for (int i = 0; i < 80 && lib_scanning(); i++) frame(2);
	// Then let art decode - one image per frame by design.
	frame(40);

	char name[128];

	snprintf(name, sizeof(name), "%s-1-home-folder", tag);
	dump(name);

	select_first_game();
	frame(30);
	snprintf(name, sizeof(name), "%s-2-home-game", tag);
	dump(name);

	/*
	  The strip has to be captured on a game whose core has save states, and the first
	  game of the shelf is Bonk's - a TurboGrafx, which has none (chome_lib's table). On
	  that one the strip is the "no save states" message, so these two dumps would show
	  nothing about the tiles or the delete prompt. Super Metroid has two states and a
	  thumbnail. Back to the first game afterwards, so the rest of the walk is unchanged.
	*/
	select_titled("Super Metroid");
	press(KEY_DOWN, 25);
	snprintf(name, sizeof(name), "%s-3-suspend", tag);
	dump(name);

	press(KEY_TAB, 8);                 // arm delete: should show the red prompt
	snprintf(name, sizeof(name), "%s-4-suspend-delete-armed", tag);
	dump(name);
	press(KEY_ESC, 10);

	// The same strip on a core with no save states, which is what a Neo Geo or a Mega
	// Drive player sees at every profile.
	select_titled("Bonk");
	press(KEY_DOWN, 25);
	snprintf(name, sizeof(name), "%s-3b-suspend-no-states", tag);
	dump(name);
	press(KEY_ESC, 10);

	select_first_game();
	frame(10);

	press(KEY_GRAVE, 20);
	snprintf(name, sizeof(name), "%s-5-sort", tag);
	dump(name);
	press(KEY_ESC, 10);
	press(KEY_ESC, 10);

	press(KEY_UP, 25);
	snprintf(name, sizeof(name), "%s-6-menubar", tag);
	dump(name);

	// Display is not offered at 240p (tiles too small to judge a filter by), so
	// capture the shortened bar there instead of a screen that does not exist.
	if (profile == 3)
	{
		snprintf(name, sizeof(name), "%s-7-menubar-no-display", tag);
		dump(name);
	}
	else
	{
		press(KEY_ENTER, 20);          // Display
		snprintf(name, sizeof(name), "%s-7-display", tag);
		dump(name);

		press(KEY_RIGHT, 12);          // compare the next look
		snprintf(name, sizeof(name), "%s-7b-display-next", tag);
		dump(name);
		press(KEY_ESC, 10);
	}

	press(KEY_RIGHT, 8);
	press(KEY_ENTER, 20);              // Options
	snprintf(name, sizeof(name), "%s-8-options", tag);
	dump(name);
	press(KEY_ESC, 10);

	press(KEY_RIGHT, 8);
	press(KEY_ENTER, 20);              // About - Language and Manuals are gone
	snprintf(name, sizeof(name), "%s-9-about", tag);
	dump(name);
	press(KEY_ESC, 10);
	press(KEY_ESC, 10);

	/*
	  The Systems view, which is where the per-system icons appear. Back all the way
	  out first and then walk from the left edge: the steps above leave the shelf on a
	  game, and a stray Enter there launches it instead.
	*/
	for (int i = 0; i < 4; i++) press(KEY_ESC, 8);
	for (int i = 0; i < 6; i++) press(KEY_LEFT, 6);     // to Favourites, the first card
	press(KEY_RIGHT, 10);                               // Systems
	press(KEY_ENTER, 24);
	snprintf(name, sizeof(name), "%s-10-systems", tag);
	dump(name);
	press(KEY_ESC, 12);
}

/* ------------------------------------------------------------ assertions -- */

static void assert_index()
{
	printf("\n== index ==\n");
	printf("  systems %d, games %d\n", lib_sys_count(), lib_item_count());

	check(lib_item_count() >= 12, "found at least 12 games");

	int has_clean = 0, has_txt = 0, has_recursed = 0, has_mra = 0, has_amiga = 0;
	for (int i = 0; i < lib_item_count(); i++)
	{
		chome_item *it = lib_item(i);
		if (!strcmp(it->title, "Super Metroid")) has_clean = 1;
		if (strstr(it->path, "notes.txt")) has_txt = 1;
		if (strstr(it->path, "Hacks/")) has_recursed = 1;
		if (strstr(it->path, ".mra")) has_mra = 1;
		const chome_sys *s = lib_sys(it->sysidx);
		if (s && !strcmp(s->id, "amiga")) has_amiga = 1;
	}

	check(has_clean, "region tags stripped from titles (\"Super Metroid\")");
	check(!has_txt, "non-matching extensions ignored (notes.txt)");
	check(has_recursed, "subdirectories scanned (SNES/Hacks)");
	check(has_mra, "arcade .mra files indexed");
	check(has_amiga, "computer systems indexed");

	/*
	  Zipped ROMs. Nothing is unpacked: the item points at "Archive.zip/Rom.sfc",
	  which the firmware's file layer reads through the archive, and the title comes
	  from the archive because that is the part named to convention.
	*/
	int zip_single = 0, zip_path_ok = 0, zip_multi = 0, zip_junk = 0;
	for (int i = 0; i < lib_item_count(); i++)
	{
		chome_item *it = lib_item(i);
		if (!strcmp(it->title, "Secret of Mana"))
		{
			zip_single = 1;
			if (!strcmp(it->path, "Secret of Mana (USA).zip/Secret of Mana (USA).sfc")) zip_path_ok = 1;
		}
		if (!strcmp(it->title, "Final Fight") || !strcmp(it->title, "Mega Man X")) zip_multi++;
		if (strcasestr(it->path, "Manual Scans")) zip_junk = 1;
	}

	check(zip_single, "a zipped ROM is indexed under the archive's name");
	check(zip_path_ok, "and points inside the archive, unpacking nothing");
	check(zip_multi == 2, "a multi-ROM archive lists each ROM by its own name");
	check(!zip_junk, "an archive with nothing playable in it is skipped");

	/*
	  Neo Geo. A romset archive is the game and is loaded whole, so it must not be
	  opened up the way a zipped ROM is, and its title comes from romsets.xml.
	*/
	int neo_titled = 0, neo_whole = 0, neo_unknown = 0, neo_hidden = 0, neo_sysfile = 0, alts = 0;
	for (int i = 0; i < lib_item_count(); i++)
	{
		chome_item *it = lib_item(i);
		const chome_sys *s = lib_sys(it->sysidx);
		int is_neo = s && !strcmp(s->id, "neogeo");

		if (is_neo && !strcmp(it->title, "Metal Slug"))
		{
			neo_titled = 1;
			if (!strcmp(it->path, "mslug.zip")) neo_whole = 1;
		}
		if (is_neo && !strcmp(it->title, "unknownset")) neo_unknown = 1;
		if (is_neo && strcasestr(it->path, "neogeo.zip")) neo_hidden = 1;
		if (is_neo && strcasestr(it->path, "sfix")) neo_sysfile = 1;
		if (strcasestr(it->path, "_alternatives")) alts = 1;
	}

	check(harness_neogeo_scanned() > 0, "romsets.xml is read before the folder is walked");
	check(neo_titled, "a romset is titled from romsets.xml (mslug -> \"Metal Slug\")");
	check(neo_whole, "and points at the archive itself, not into it");
	check(neo_unknown, "an unlisted romset still appears, under its board name");
	check(!neo_hidden, "a romset marked hidden is left out");
	check(!neo_sysfile, "BIOS and system files are not listed as games");
	check(!alts, "arcade _alternatives are skipped");

	int neo_dir = 0, neo_dir_walked = 0, neo_nested = 0;
	for (int i = 0; i < lib_item_count(); i++)
	{
		chome_item *it = lib_item(i);
		const chome_sys *s2 = lib_sys(it->sysidx);
		if (!s2 || strcmp(s2->id, "neogeo")) continue;

		if (!strcmp(it->path, "samsho2")) neo_dir = 1;
		if (strcasestr(it->path, "samsho2/")) neo_dir_walked = 1;
		if (strcasestr(it->path, "Homebrew/lasthope.zip")) neo_nested = 1;
	}
	check(neo_dir, "a folder romset is listed as one game (Darksoft layout)");
	check(!neo_dir_walked, "and is not walked into as though it held games");
	check(neo_nested, "a folder that is not a romset is still walked into");
}

static void assert_views()
{
	printf("\n== views ==\n");

	lib_view_build(VIEW_ROOT, -1, SORT_TITLE);
	int n = lib_view_count();
	check(n > 3, "root view has folders plus games");

	int folders = 0, amiga_on_shelf = 0;
	for (int i = 0; i < n; i++)
	{
		const chome_entry *e = lib_view_entry(i);
		if (e->kind != ENT_GAME) { folders++; continue; }
		chome_item *it = lib_item(e->game);
		const chome_sys *s = it ? lib_sys(it->sysidx) : 0;
		if (s && s->computer) amiga_on_shelf = 1;
	}
	check(folders == 3, "three folders lead the root shelf");
	check(!amiga_on_shelf, "computer games kept off the root shelf");

	// Folders must stay first whatever the sort.
	lib_view_build(VIEW_ROOT, -1, SORT_TITLE);
	check(lib_view_entry(0)->kind != ENT_GAME, "folders stay first after sorting");

	// Title sort must be ascending.
	lib_view_build(VIEW_ALL, -1, SORT_TITLE);
	int sorted = 1;
	for (int i = 1; i < lib_view_count(); i++)
	{
		chome_item *a = lib_item(lib_view_entry(i - 1)->game);
		chome_item *b = lib_item(lib_view_entry(i)->game);
		if (strcasecmp(a->title, b->title) > 0) sorted = 0;
	}
	check(sorted, "title A-Z sort is ordered");

	lib_view_build(VIEW_COMPUTERS, -1, SORT_TITLE);
	check(lib_view_count() >= 1, "Computers view lists computer systems");
	check(lib_view_entry(0)->kind == ENT_BROWSE, "computer entries open the browser");
}

static void assert_slots()
{
	printf("\n== suspend slots ==\n");

	chome_item *sm = 0;
	for (int i = 0; i < lib_item_count(); i++)
	{
		chome_item *it = lib_item(i);
		if (!strcmp(it->title, "Super Metroid")) { sm = it; break; }
	}

	if (!sm) { check(0, "found Super Metroid"); return; }

	lib_refresh_slots(sm);
	int s1 = (sm->slots >> 0) & 3;
	int s2 = (sm->slots >> 2) & 3;
	int s3 = (sm->slots >> 4) & 3;

	check(s1 == 1, "slot 1 detected as saved");
	check(s2 == 1, "slot 2 detected as saved");
	check(s3 == 0, "slot 3 detected as empty");

	lib_set_lock(sm, 0, 1);
	check(((sm->slots >> 0) & 3) == 2, "lock applies to slot 1");
	lib_set_lock(sm, 0, 0);
	check(((sm->slots >> 0) & 3) == 1, "unlock restores slot 1");

	char tp[1024];
	check(lib_slot_thumb(sm, 0, tp, sizeof(tp)), "slot 1 thumbnail path resolves");
	check(art_thumb(tp, 200, 150) != 0, "slot 1 thumbnail decodes");
	check(lib_slot_thumb(sm, 1, tp, sizeof(tp)) && art_thumb(tp, 200, 150) == 0,
		"slot 2 has no thumbnail and degrades quietly");
}

static void assert_launch()
{
	printf("\n== launch ==\n");

	// Select Super Metroid on the shelf, then start it.
	lib_view_build(VIEW_ALL, -1, SORT_TITLE);
	chome_leave();
	press(KEY_MENU, 20);
	frame(10);

	// Land on the first game of the root shelf: deterministic under SORT_TITLE.
	lib_view_build(VIEW_ROOT, -1, SORT_TITLE);
	select_first_game();
	frame(10);

	const chome_entry *first = lib_view_entry(leading_folders());
	chome_item *want = first ? lib_item(first->game) : 0;
	const chome_sys *wsys = want ? lib_sys(want->sysidx) : 0;
	if (!want || !wsys) { check(0, "a game is selectable on the root shelf"); return; }
	printf("  launching \"%s\" (%s)\n", want->title, wsys->name);

	harness_clear_launch();
	press(KEY_ENTER, 4);
	dump("launch-curtain");
	frame(80);                         // let the 900ms curtain elapse

	const char *l = harness_last_launch();
	check(l && strstr(l, ".mgl") != 0, "launch went through an MGL");

	{
		FILE *pf = fopen("/tmp/classicui_preset", "rt");
		check(pf != 0, "launch armed a video look for the next core");
		if (pf)
		{
			char buf[1024] = {};
			if (fgets(buf, sizeof(buf), pf)) {}
			fclose(pf);
			printf("  armed look: %s", buf);
			check(strstr(buf, "presets/ClassicHome") != 0, "armed look points at a generated preset");
		}
	}

	FILE *f = fopen("/tmp/classicui_launch.mgl", "rt");
	check(f != 0, "MGL file was written");
	if (f)
	{
		char buf[1024] = {};
		size_t n = fread(buf, 1, sizeof(buf) - 1, f);
		buf[n] = 0;
		fclose(f);
		printf("---- /tmp/classicui_launch.mgl ----\n%s-----------------------------------\n", buf);
		char want_rbf[128];
		snprintf(want_rbf, sizeof(want_rbf), "<rbf>%s</rbf>", wsys->rbf);
		check(strstr(buf, want_rbf) != 0, "MGL names the selected game's core");
		check(strstr(buf, "type=") != 0, "MGL carries a file type");
		check(strstr(buf, want->path) != 0, "MGL points at the selected ROM");
		check(strstr(buf, "index=") != 0, "MGL carries a slot index");
	}
}

static void assert_art()
{
	printf("\n== art ==\n");

	int ready = 0, missing = 0;
	for (int i = 0; i < lib_item_count(); i++)
	{
		art_request(i, 0);
	}
	for (int i = 0; i < lib_item_count() * 2 + 10; i++) art_step();

	for (int i = 0; i < lib_item_count(); i++)
	{
		int st = art_state(i);
		if (st == ART_READY) ready++;
		if (st == ART_MISSING) missing++;
	}

	printf("  ready %d, missing %d, cache %d KB\n", ready, missing, art_cache_bytes() / 1024);
	check(ready == 3, "exactly the three covers on the fake SD decoded");
	check(missing >= 8, "everything else reports missing for the fallback card");

	int w = 0, h = 0;
	const uint32_t *a = 0;
	for (int i = 0; i < lib_item_count() && !a; i++) a = art_get(i, &w, &h);
	check(a != 0, "a decoded cover is retrievable");
	check(w == theme_get()->sel_w && h == theme_get()->sel_h, "cover decoded at card size");
}

static int count_lines(const char *rel, int *bad_sum, int *maxlen)
{
	char p[1024];
	snprintf(p, sizeof(p), "%s/%s", ROOT, rel);
	FILE *f = fopen(p, "rt");
	if (!f) return -1;

	char line[512];
	int n = 0;
	if (bad_sum) *bad_sum = 0;
	if (maxlen) *maxlen = 0;

	while (fgets(line, sizeof(line), f))
	{
		if (line[0] == '#' || line[0] == '\n') continue;
		int a, b, c, d;
		if (sscanf(line, "%d,%d,%d,%d", &a, &b, &c, &d) == 4)
		{
			n++;
			int sum = a + b + c + d;
			if (bad_sum && sum > 128) (*bad_sum)++;
			if (maxlen && sum > *maxlen) *maxlen = sum;
		}
		else if (sscanf(line, "%d,%d,%d", &a, &b, &c) == 3) n++;
	}
	fclose(f);
	return n;
}

#define P_TEST_DMG 6      // index of the DMG look in the preset table

static void assert_video()
{
	printf("\n== video looks ==\n");

	vp_install();

	printf("  %d looks\n", vp_count());
	check(vp_count() >= 8, "a short curated list of looks");

	int all_ok = 1;
	for (int i = 0; i < vp_count(); i++)
	{
		if (!vp_available(i)) { printf("  unavailable: %s\n", vp_name(i)); all_ok = 0; }
	}
	check(all_ok, "every look's files were generated and are present");

	int bad = 0, peak = 0;
	int n = count_lines("filters/ClassicHome Sharp.txt", &bad, &peak);
	check(n == 32, "sharp filter has 32 phases");
	check(bad == 0, "no filter line exceeds the 128 range");
	check(peak == 128, "sharp filter is unity gain");

	n = count_lines("filters/ClassicHome Scanlines.txt", &bad, &peak);
	check(n == 32, "scanline filter has 32 phases");
	check(bad == 0, "scanline filter stays inside the range");
	check(peak <= 128 && peak >= 100, "scanline filter peaks near unity");

	// A scanline filter must dim somewhere, or it is not a scanline filter.
	{
		char p[1024];
		snprintf(p, sizeof(p), "%s/filters/ClassicHome Scanlines.txt", ROOT);
		FILE *f = fopen(p, "rt");
		int lowest = 1000;
		if (f)
		{
			char line[512];
			while (fgets(line, sizeof(line), f))
			{
				int a, b, c, d;
				if (sscanf(line, "%d,%d,%d,%d", &a, &b, &c, &d) == 4)
				{
					int sum = a + b + c + d;
					if (sum < lowest) lowest = sum;
				}
			}
			fclose(f);
		}
		printf("  scanline gain range: %d..%d of 128\n", lowest, peak);
		// Must visibly dim, but not so far that the picture is unusable once a
		// shadow mask is stacked on top of it.
		check(lowest < 115, "scanline filter darkens between lines");
		check(lowest > 60, "scanline filter is not excessively dark");
	}

	n = count_lines("gamma/ClassicHome DMG.txt", 0, 0);
	check(n == 256, "DMG gamma LUT has 256 entries");
	n = count_lines("gamma/ClassicHome GB Pocket.txt", 0, 0);
	check(n == 256, "Pocket gamma LUT has 256 entries");
	n = count_lines("gamma/ClassicHome GBA AGB-001.txt", 0, 0);
	check(n == 256, "AGB-001 gamma LUT has 256 entries");
	n = count_lines("gamma/ClassicHome GBA AGS-101.txt", 0, 0);
	check(n == 256, "AGS-101 gamma LUT has 256 entries");

	// The three GBA revisions must actually be different curves, brightest last.
	{
		int lo[3], hi[3];
		const char *files[3] = {
			"gamma/ClassicHome GBA AGB-001.txt",
			"gamma/ClassicHome GBA AGS-001.txt",
			"gamma/ClassicHome GBA AGS-101.txt"
		};
		for (int k = 0; k < 3; k++)
		{
			char pp[1024];
			snprintf(pp, sizeof(pp), "%s/%s", ROOT, files[k]);
			FILE *ff = fopen(pp, "rt");
			lo[k] = hi[k] = -1;
			if (ff)
			{
				char line[256];
				int idx = 0, r, g, b;
				while (fgets(line, sizeof(line), ff))
				{
					if (sscanf(line, "%d,%d,%d", &r, &g, &b) != 3) continue;
					if (idx == 0) lo[k] = (r + g + b) / 3;
					hi[k] = (r + g + b) / 3;
					idx++;
				}
				fclose(ff);
			}
		}
		printf("  GBA revisions (black -> white): AGB-001 %d->%d, AGS-001 %d->%d, AGS-101 %d->%d\n",
			lo[0], hi[0], lo[1], hi[1], lo[2], hi[2]);
		check(lo[0] > lo[1] && lo[1] > lo[2], "black level improves AGB-001 -> AGS-001 -> AGS-101");
		check(hi[2] > hi[1] && hi[1] > hi[0], "white level improves AGB-001 -> AGS-001 -> AGS-101");
		check((hi[2] - lo[2]) > (hi[0] - lo[0]), "AGS-101 has the most contrast");
	}

	// The DMG curve has to actually be yellow-green, not grey.
	{
		char p[1024];
		snprintf(p, sizeof(p), "%s/gamma/ClassicHome DMG.txt", ROOT);
		FILE *f = fopen(p, "rt");
		int r0 = -1, g0 = -1, b0 = -1, r255 = -1, g255 = -1, b255 = -1, idx = 0;
		if (f)
		{
			char line[256];
			while (fgets(line, sizeof(line), f))
			{
				int r, g, b;
				if (sscanf(line, "%d,%d,%d", &r, &g, &b) != 3) continue;
				if (idx == 0) { r0 = r; g0 = g; b0 = b; }
				r255 = r; g255 = g; b255 = b;
				idx++;
			}
			fclose(f);
		}
		printf("  DMG LUT: %d,%d,%d -> %d,%d,%d\n", r0, g0, b0, r255, g255, b255);
		check(g0 > r0 && g0 > b0, "DMG shadow is green-dominant");
		check(g255 > b255 && r255 > b255, "DMG highlight is yellow-green");
	}

	// Defaults per class, which is the whole point of "on by default".
	check(vp_default_for(VC_CONSOLE) == vp_default_for(VC_ARCADE), "consoles and arcade share a default");
	check(strstr(vp_name(vp_default_for(VC_CONSOLE)), "PVM") != 0, "consoles default to a PVM look");
	check(strstr(vp_name(vp_default_for(VC_VGA)), "VGA") != 0, "VGA machines default to no scanlines");
	check(strstr(vp_name(vp_default_for(VC_GB)), "DMG") != 0, "Game Boy defaults to the DMG look");
	check(strstr(vp_name(vp_default_for(VC_GBA)), "AGB-001") != 0, "GBA defaults to the original unlit screen");
	check(vp_default_for(VC_GBC) != vp_default_for(VC_GBA), "GBC and GBA differ");

	// The looks offered must be constrained to the hardware in question.
	printf("\n  offered per class:\n");
	int opts[VP_MAX_OPTIONS];
	for (int c = 0; c < VC_COUNT; c++)
	{
		int n = vp_options_for(c, opts);
		printf("    %-16s ", vp_class_label(c));
		for (int i = 0; i < n; i++) printf("%s%s", i ? ", " : "", vp_name(opts[i]));
		printf("\n");
	}

	// No handheld look may leak into a CRT class, and vice versa.
	int leak = 0;
	for (int c = 0; c < VC_COUNT; c++)
	{
		int handheld = (c == VC_GB || c == VC_GBC || c == VC_GBA);
		int n = vp_options_for(c, opts);
		for (int i = 0; i < n; i++)
		{
			int is_lcd = (strstr(vp_name(opts[i]), "Game Boy") != 0 || strstr(vp_name(opts[i]), "GBA") != 0);
			if (is_lcd != handheld) leak = 1;
		}
	}
	check(!leak, "LCD looks only on handhelds, CRT looks only on everything else");

	int n_gb = vp_options_for(VC_GB, opts);
	check(n_gb == 2, "Game Boy offers exactly two screens");
	check(strstr(vp_name(opts[0]), "DMG") && strstr(vp_name(opts[1]), "Pocket"),
		"Game Boy offers DMG and Pocket");

	int n_gbc = vp_options_for(VC_GBC, opts);
	check(n_gbc == 4, "GBC offers its own screen plus the three GBA ones");
	check(strstr(vp_name(opts[0]), "Color") != 0, "GBC defaults to the GBC screen");

	int n_gba = vp_options_for(VC_GBA, opts);
	check(n_gba == 3, "GBA offers three screen revisions");
	{
		int have001 = 0, haveS001 = 0, haveS101 = 0;
		for (int i = 0; i < n_gba; i++)
		{
			if (strstr(vp_name(opts[i]), "AGB-001")) have001 = 1;
			if (strstr(vp_name(opts[i]), "AGS-001")) haveS001 = 1;
			if (strstr(vp_name(opts[i]), "AGS-101")) haveS101 = 1;
		}
		check(have001 && haveS001 && haveS101, "AGB-001, AGS-001 and AGS-101 all present");
	}

	// A GBC cart in the Game Boy core must resolve to the GBC class, not DMG.
	{
		int gbsys = -1;
		for (int i = 0; i < lib_sys_count(); i++) if (!strcmp(lib_sys(i)->id, "gb")) gbsys = i;

		chome_item *dmg_cart = 0, *gbc_cart = 0;
		for (int i = 0; i < lib_item_count(); i++)
		{
			chome_item *item = lib_item(i);
			if (item->sysidx != gbsys) continue;
			if (strstr(item->path, ".gbc")) gbc_cart = item;
			else if (strstr(item->path, ".gb")) dmg_cart = item;
		}
		check(dmg_cart && gbc_cart, "both a .gb and a .gbc cart were indexed");

		if (dmg_cart && gbc_cart)
		{
			int pd = vp_effective(gbsys, VC_GB);
			int pc = vp_effective(gbsys, VC_GBC);
			printf("  .gb -> %s, .gbc -> %s\n", vp_name(pd), vp_name(pc));
			check(strstr(vp_name(pd), "DMG") != 0, ".gb cart gets the DMG screen");
			check(strstr(vp_name(pc), "Color") != 0, ".gbc cart gets the GBC screen");

			// Choices for the two must not overwrite each other.
			vp_options_for(VC_GBC, opts);
			vp_set(gbsys, VC_GBC, opts[3]);              // an AGS screen for the GBC cart
			check(vp_effective(gbsys, VC_GB) == pd, "choosing a GBC screen left the DMG cart alone");
			check(vp_effective(gbsys, VC_GBC) == opts[3], "the GBC choice stuck");
			vp_set(gbsys, VC_GBC, pc);
		}
	}

	// A look that is not offered for a class must be refused.
	{
		int gbsys = -1;
		for (int i = 0; i < lib_sys_count(); i++) if (!strcmp(lib_sys(i)->id, "gb")) gbsys = i;
		int before = vp_effective(gbsys, VC_GB);
		vp_set(gbsys, VC_GB, 1);                          // PVM RGB: not a Game Boy screen
		check(vp_effective(gbsys, VC_GB) == before, "a CRT look cannot be set on a Game Boy");
	}

	// Every handheld class must have at least one look, and no CRT leakage.
	{
		int hh[] = { VC_GB, VC_GBC, VC_GBA, VC_GG, VC_LYNX, VC_WS, VC_WSC, VC_NGPC };
		int all = 1;
		for (size_t k = 0; k < sizeof(hh) / sizeof(hh[0]); k++)
		{
			int o2[VP_MAX_OPTIONS];
			int nn = vp_options_for(hh[k], o2);
			if (nn < 1) { printf("  no looks for %s\n", vp_class_label(hh[k])); all = 0; }
			if (nn && !vp_class_is_handheld(hh[k])) all = 0;
		}
		check(all, "every handheld class has a fitting look");
	}

	// The handheld curves must actually differ from one another.
	{
		const uint32_t *a = vp_preview(vp_default_for(VC_GB), 160, 120, 0);
		uint32_t first_gb = a ? a[0] : 0;
		const uint32_t *b2 = vp_preview(vp_default_for(VC_GBA), 160, 120, 0);
		uint32_t first_gba = b2 ? b2[0] : 0;
		check(first_gb != first_gba, "DMG and AGB previews are not identical");
	}

	// Per-system class assignment from the built-in table.
	int gb = -1, ao = -1, arc = -1, c64 = -1;
	for (int i = 0; i < lib_sys_count(); i++)
	{
		const chome_sys *sy = lib_sys(i);
		if (!strcmp(sy->id, "gb")) gb = i;
		if (!strcmp(sy->id, "ao486")) ao = i;
		if (!strcmp(sy->id, "arcade")) arc = i;
		if (!strcmp(sy->id, "c64")) c64 = i;
	}
	check(gb >= 0 && lib_sys(gb)->vclass == VC_GB, "Game Boy classed as a handheld");
	check(ao >= 0 && lib_sys(ao)->vclass == VC_VGA, "ao486 classed as VGA");
	check(arc >= 0 && lib_sys(arc)->vclass == VC_ARCADE, "arcade classed as arcade");
	check(c64 >= 0 && lib_sys(c64)->vclass == VC_COMPUTER, "C64 classed as a 15 kHz computer");

	// A user choice must override the default and persist, within the class.
	{
		int cons = -1;
		for (int i = 0; i < lib_sys_count(); i++) if (!strcmp(lib_sys(i)->id, "snes")) cons = i;
		int co[VP_MAX_OPTIONS];
		vp_options_for(VC_CONSOLE, co);
		int before2 = vp_effective(cons, VC_CONSOLE);
		vp_set(cons, VC_CONSOLE, co[2]);
		check(vp_effective(cons, VC_CONSOLE) == co[2], "per-system override applies");
		vp_set(cons, VC_CONSOLE, before2);
	}

	// Previews must render at any size.
	const uint32_t *pv = vp_preview(1, 320, 240, 0);
	check(pv != 0, "preview renders");
	int varied = 0;
	if (pv) for (int i = 1; i < 320 * 240; i++) if (pv[i] != pv[0]) { varied = 1; break; }
	check(varied, "preview is not a flat colour");

	// A real reference frame must actually change the preview, and the look must
	// still be applied on top of it rather than the frame being passed through.
	{
		int n2 = 320 * 240;
		uint32_t *ref = (uint32_t*)malloc(n2 * 4);
		for (int i = 0; i < n2; i++) ref[i] = 0xff4080c0;      // flat mid blue

		const uint32_t *synth = vp_preview(P_TEST_DMG, 320, 240, 0);
		uint32_t s0 = synth ? synth[0] : 0;

		const uint32_t *over = vp_preview(P_TEST_DMG, 320, 240, ref);
		check(over != 0, "preview renders over a reference frame");

		int differs = 0;
		if (over) for (int i = 0; i < n2; i++) if (over[i] != s0) { differs = 1; break; }
		check(differs, "a reference frame changes the preview");

		// The DMG look must recolour the blue frame, not pass it through.
		int passthrough = 0;
		if (over) for (int i = 0; i < n2; i++) if (over[i] == 0xff4080c0) { passthrough = 1; break; }
		check(!passthrough, "the look is applied over the reference, not bypassed");
		printf("  flat blue through the DMG look: %06X\n", over ? (over[n2 / 2] & 0xffffff) : 0);

		free(ref);
	}
}

/*
  "Slot" does not always mean a savestate slot. Measured on the device: MSX reports two and
  Apple II three, meaning cartridge and expansion slots - so a core that had savestates as
  well would have had savestate numbers written into its hardware selector, switching carts
  under the player when they picked slot 2.
*/
/*
  A held menu button repeats, and the repeat used to close the menu the press had just
  opened - so on the device it took two presses to get in, and the first one flashed
  something for a split second. menu.cpp's menu_key_get() repeats a held key while
  chome_active(), and opening this menu takes long enough that the repeat lands right
  after it. Two presses with no release between them are one physical press.
*/
static void assert_menu_repeat()
{
	printf("\n== a held menu button is one press ==\n");

	{
		FILE *f = fopen("/tmp/classicui_current", "wt");
		if (f) { fprintf(f, "gb\nTetris (World).gb\n"); fclose(f); }
	}

	harness_set_menu_core(0);
	harness_set_fb_supported(1);
	harness_set_fb(1280, 720);
	gfx_shutdown();
	theme_update(1280, 720, 1);

	chome_handle(0);
	if (chome_ingame_active()) press(KEY_MENU, 14);
	frame(6);
	check(!chome_ingame_active(), "starting from the game");

	chome_handle(KEY_MENU);                   // pressed, and still held
	frame(12);
	check(chome_ingame_active(), "one press opens the menu");

	harness_advance(600);                     // past menu.cpp's repeat delay
	check(chome_handle(KEY_MENU) == 1, "a repeat of the held press is consumed");
	check(chome_ingame_active(), "and does not close what that press just opened");

	chome_handle(KEY_MENU | UPSTROKE);        // finally released
	frame(8);
	check(chome_ingame_active(), "the menu is still up after the release");

	press(KEY_MENU, 16);                      // and a fresh press closes it
	frame(8);
	check(!chome_ingame_active(), "a second real press closes it");

	/*
	  The other direction, which the first attempt at this broke: a repeat arriving just
	  after the menu closed must not open it again. That is what made going back to the
	  game need two presses as well.
	*/
	chome_handle(KEY_MENU);                   // pressed, and held
	frame(10);
	check(chome_ingame_active(), "held press opens it again");
	chome_handle(KEY_MENU | UPSTROKE);
	frame(6);

	chome_handle(KEY_MENU);                   // press that closes, still held
	check(!chome_ingame_active(), "one press closes it");
	harness_advance(600);
	check(chome_handle(KEY_MENU) == 1, "and the repeat after that close is consumed");
	check(!chome_ingame_active(), "so it does not re-open behind the player");
	chome_handle(KEY_MENU | UPSTROKE);
	frame(6);

	for (int i = 1; i <= 4; i++)
	{
		char p2[512];
		snprintf(p2, sizeof(p2), "%s/savestates/Gameboy/Tetris (World)_%d.ss", ROOT, i);
		unlink(p2);
	}
}

/*
  classicui_freeze=0: the escape hatch for a core that cannot survive being asked for a
  state (see the SNES/Battletoads case). The game runs on behind the menu instead, exactly
  as it already does on a core with no save states at all - so the menu still opens, the
  slots still work, and nothing asks the core to save on the way in.
*/
static void assert_freeze_off()
{
	printf("\n== the freeze can be turned off ==\n");

	{
		FILE *f = fopen("/tmp/classicui_current", "wt");
		if (f) { fprintf(f, "gb\nTetris (World).gb\n"); fclose(f); }
	}

	harness_set_menu_core(0);
	harness_set_fb_supported(1);
	harness_set_fb(1280, 720);
	gfx_shutdown();
	theme_update(1280, 720, 1);
	chome_handle(0);
	if (chome_ingame_active()) press(KEY_MENU, 14);
	frame(6);

	harness_set_confstr(2);                   // savestates, no usable pause: normally freezes
	cfg.classicui_freeze = 0;
	harness_reset_status();

	press(KEY_MENU, 20);
	check(chome_ingame_active(), "the menu still opens");
	check(harness_pulses_on("S") == 0, "and nothing asks the core for a state");

	press(KEY_MENU, 16);
	frame(8);
	check(!chome_ingame_active(), "and it closes again");
	check(harness_pulses_on("T") == 0, "with no restore on the way out either");

	cfg.classicui_freeze = 1;
	harness_set_confstr(1);
}

/*
  Saving on a core that pauses for real. There is no held state on such a core - nothing had
  to be held still - so the core is asked directly, and a save pulse is only serviced by a
  running core. That used to resume the game and close the menu, which reads as the
  front-end throwing the player out for pressing Save; SMS is the first core Dinofly owns that
  pauses, which is how it surfaced. The menu stays up now, the pause comes off for the write
  and goes back on when the state lands.
*/
static void assert_save_on_pausing_core()
{
	printf("\n== saving on a core that pauses properly ==\n");

	{
		FILE *f = fopen("/tmp/classicui_current", "wt");
		if (f) { fprintf(f, "gb\nTetris (World).gb\n"); fclose(f); }
	}

	harness_set_menu_core(0);
	harness_set_fb_supported(1);
	harness_set_fb(1280, 720);
	gfx_shutdown();
	theme_update(1280, 720, 1);

	chome_handle(0);
	if (chome_ingame_active()) press(KEY_MENU, 14);
	frame(6);

	const char *slot2 = ROOT "/savestates/Gameboy/Tetris (World)_1.ss";
	unlink(slot2);

	harness_set_confstr(4);                   // a real pause, honoured off the OSD
	harness_reset_status();
	press(KEY_MENU, 20);
	check(chome_ingame_active(), "the menu opens");
	check(harness_opt_val("H") != 0, "the core is really paused, not frozen with a state");
	check(harness_pulses_on("S") == 0, "so no held state is written");

	// The strip belongs to the selected game, and the selection only lands on the running
	// one once the shelf has been built.
	for (int i = 0; i < 40 && lib_scanning(); i++) frame(2);
	frame(20);

	press(KEY_DOWN, 20);                      // the suspend strip
	harness_reset_status();
	press(KEY_BACKSPACE, 10);                 // Y saves into the selected slot

	check(chome_ingame_active(), "the menu stays up instead of dropping into the game");
	check(harness_pulses_on("S") == 1, "the core was asked for the state");
	check(harness_opt_val("H") == 0, "and let run, since a paused core never services it");

	// The core gets round to writing it.
	{
		FILE *f = fopen(slot2, "wb");
		if (f) { fprintf(f, "STATE"); fclose(f); }
	}
	frame(20);

	check(harness_opt_val("H") != 0, "once the state lands the pause goes back on");
	check(chome_ingame_active(), "and the menu is still up");

	press(KEY_MENU, 16);
	frame(8);
	harness_set_confstr(1);
	unlink(slot2);
}

static int fav_count()
{
	int n = 0;
	for (int i = 0; i < lib_item_count(); i++) if (lib_item(i)->fav) n++;
	return n;
}

/*
  B at the top of the shelf jumps to the leftmost entry.

  Favourites and Systems are the first cards of a row that is hundreds of games long, and
  walking left to them one card at a time is what this saves - so at the top level B is a
  navigation key, not a way out. Two neighbours have to survive it, and both are checked
  here because each one is a behaviour someone relies on:

  - inside a folder B still means "up one level", since back() pops the nav stack before
    anything else and this only concerns the level where there is nothing to pop;
  - in a game core B used to close the in-game menu from here. It cannot do both, so
    going back to the game is the menu button's job alone now.

  There is no accessor for the shelf selection, so it is read the way the shelf itself
  offers: Y does nothing on a folder, and A on the leftmost card opens Favourites - made
  unmistakable by leaving exactly one favourite in the library.
*/
/*
  The Wi-Fi adapter is a USB dongle, and its interface may not exist yet the first time the
  front-end looks. Remembering that first "nothing" is what left Dinofly's machine showing
  "No adapter" in Options while it was reachable over that very interface.
*/
/*
  A core with two slots gives the player one, not three.

  The last slot is the one the menu holds the game still in, so three tiles on a two-slot
  core meant the third tile *was* that reserved slot: saving there copied the held state
  over itself and reported success, and loading it put the player back at the moment the
  menu opened - indistinguishable from a load that did nothing. Dinofly hit it on PSX.
*/
/*
  Why the picture cache cannot decide this from the file alone.

  It compares size and mtime. Two frames of one game are the same scene in the same
  palette, so they very often compress to the same number of bytes, and the card is FAT
  whose timestamps are granular to two seconds - a rewrite lands inside one. That is a
  hit on a file whose contents changed, and the tile keeps the moment that was replaced.

  This builds exactly that: same dimensions, same byte count, same timestamp, different
  picture. The stat check cannot see it and is not expected to. art_forget() is how the
  code that rewrote the file says so, and the check is that saying so is enough.
*/
/*
  A look chosen for the running game shows up on it now, not after a reload.

  The preset is armed for the *next* core to pick up, which is right for a game about
  to launch and wrong for the one already on screen - it read as the setting doing
  nothing until the core was reloaded. What is checked is that the preset file the
  video layer was handed names the look that was chosen.
*/
/*
  The core's own options are reachable from inside a game.

  A player reported this as the one thing the front-end had taken away: with a core
  loaded there was no route to the classic OSD, and that is where a core's own settings
  live - widescreen on PSX, or its video and audio. Options ended in Close Game and
  nothing else.

  Two things have to hold. The route exists and asks for the OSD, and once the OSD has
  the screen the menu button belongs to *it* - otherwise the player is trapped in the
  core's settings with no way back but a reset.
*/
/*
  The core's own options, read out of its CONF_STR and driven from our screen.

  The fake core is shaped like the real ones - see fake_confstr_opts - so the things this
  has to get right are the things that were actually hard: a mask in front of a page
  prefix, the two bit-spec forms, options we own and must not offer twice, and the core's
  own (U) marking.
*/
static void assert_core_options(int hd_mask_bit1)
{
	harness_set_confstr(6);
	harness_set_osd_mask(hd_mask_bit1 ? 0x0002 : 0x0000);
	int n = core_opts_scan();

	int pic = core_opts_tier_count(CO_TIER_PICTURE);
	int sys = core_opts_tier_count(CO_TIER_SYSTEM);
	int risk = core_opts_tier_count(CO_TIER_RISKY);
	printf("  mask=%d -> %d offered (picture %d, system %d, risky %d)\n",
		hd_mask_bit1, n, pic, sys, risk);

	/*
	  Four on the picture page either way. The mask on the VI pair is a D, which *disables*
	  rather than hides - the option stays on screen and greys out, which is what the OSD
	  does and the honest thing: the setting exists, it just does not apply right now.
	  An earlier version of this check asserted they vanished, and was wrong about the
	  grammar rather than about the code.
	*/
	check(pic == 4, "a mask in front of a page prefix is still parsed");
	check(sys == 1, "region lands on the system page");
	check(risk == 1, "and the core's own (U) marking puts an option on the risky page");

	int vi_disabled = 0, owned = 0;
	for (int i = 0; i < n; i++)
	{
		const core_opt *o = core_opt_at(i);
		if (!strncasecmp(o->name, "VI ", 3) && o->disabled) vi_disabled++;
		if (!strcasecmp(o->name, "Savestate Slot")
			|| !strcasecmp(o->name, "Aspect ratio")
			|| !strcasecmp(o->name, "Cache Delay")) owned++;
	}

	check(vi_disabled == (hd_mask_bit1 ? 2 : 0),
		hd_mask_bit1 ? "and the core's mask greys out the pair it says do not apply"
		             : "with nothing greyed out while the core says they apply");
	check(!owned, "nothing the front-end owns is offered a second time");
}

static void assert_core_options_screen()
{
	printf("\n== the core's own options ==\n");

	{
		FILE *f = fopen("/tmp/classicui_current", "wt");
		if (f) { fprintf(f, "gb\nTetris (World).gb\n"); fclose(f); }
	}
	harness_set_menu_core(0);
	harness_set_fb_supported(1);
	harness_set_fb(1280, 720);
	gfx_shutdown();
	theme_update(1280, 720, 1);
	harness_set_osd_visible(0);

	assert_core_options(0);
	assert_core_options(1);
	assert_core_options(0);

	/*
	  A trigger is not a setting. Reset sits in the same CONF_STR and a momentary action
	  in a list of values is a trap: the player lands on it looking for something to
	  change and loses their game.
	*/
	int found_reset = 0;
	for (int i = 0; i < core_opts_count(); i++)
		if (strcasestr(core_opt_at(i)->name, "Reset")) found_reset = 1;
	check(!found_reset, "and a momentary trigger is not offered as a setting");

	// Now through the UI: open it from the bar and change something.
	chome_handle(0);
	if (chome_ingame_active()) press(KEY_MENU, 14);
	frame(6);
	press(KEY_MENU, 20);
	for (int i = 0; i < 40 && lib_scanning(); i++) frame(2);
	frame(16);
	check(chome_ingame_active(), "the menu is up over the running game");

	/*
	  The bar entry has to be there before it can be opened, which is not circular by
	  accident: the entry only exists when the core published something, so the scan has to
	  have happened by the time the bar is drawn. Opening the menu is what does it. On
	  hardware the entry never appeared until that was fixed.
	*/
	press(KEY_UP, 14);
	unsigned long bar = harness_fb_hash(0, 60);
	for (int i = 0; i < 6; i++) press(KEY_RIGHT, 8);   // walk to the last bar entry
	frame(8);
	check(harness_fb_hash(0, 60) != bar, "the bar has an entry for the running core");

	press(KEY_ENTER, 18);
	frame(10);
	/*
	  Named for what it is. The fixture is a synthetic core carrying options borrowed from
	  several real ones, so one pass covers every awkward case in the grammar - which means
	  this picture shows Palette next to VI Deblur, a combination no real core has. Dinofly
	  read it as the screen listing every core's options rather than the running one's; the
	  device screenshots in docs/img/device are the ones that show real per-core lists.
	*/
	dump("core-options-synthetic-fixture");

	const core_opt *first = core_opt_tier_at(CO_TIER_PICTURE, 0);
	check(first != 0, "the screen opens on something");

	int before = first ? core_opt_value(first) : 0;
	int saves = harness_cfg_saves();
	press(KEY_RIGHT, 14);
	frame(8);
	check(first && core_opt_value(first) != before, "right changes the value in the core");
	check(harness_cfg_saves() > saves, "and keeps it in the core's own config");

	press(KEY_ESC, 12);
	frame(6);
	press(KEY_MENU, 16);
	frame(8);
	harness_set_confstr(1);
}

static void assert_core_options_are_reachable()
{
	printf("\n== the core's own options are reachable from a game ==\n");

	{
		FILE *f = fopen("/tmp/classicui_current", "wt");
		if (f) { fprintf(f, "gb\nTetris (World).gb\n"); fclose(f); }
	}

	harness_set_menu_core(0);
	harness_set_fb_supported(1);
	harness_set_fb(1280, 720);
	gfx_shutdown();
	theme_update(1280, 720, 1);
	harness_set_confstr(1);
	harness_set_osd_visible(0);
	chome_handle(0);
	if (chome_ingame_active()) press(KEY_MENU, 14);
	frame(6);

	press(KEY_MENU, 20);
	for (int i = 0; i < 40 && lib_scanning(); i++) frame(2);
	frame(16);
	check(chome_ingame_active(), "the menu is up over the running game");

	press(KEY_UP, 14);                     // menu bar
	press(KEY_RIGHT, 12);                  // Options
	press(KEY_ENTER, 18);
	frame(8);

	// Core Settings is the row above Close Game.
	for (int i = 0; i < 8; i++) press(KEY_DOWN, 8);
	frame(8);
	dump("core-options-row");

	press(KEY_ENTER, 18);
	frame(10);

	check(!chome_ingame_active(), "choosing it gives the screen back to the core");
	check(harness_last_menu_key() == KEY_F12, "and asks for the classic OSD");

	/*
	  And the button is now the OSD's. Pressing it must not reopen the front-end over
	  the settings screen the player just asked for.
	*/
	press(KEY_MENU, 20);
	frame(8);
	check(!chome_ingame_active(), "while the OSD is up the menu button is not ours");

	// Once it closes, it is ours again.
	harness_set_osd_visible(0);
	press(KEY_MENU, 20);
	frame(10);
	check(chome_ingame_active(), "and once the OSD closes the front-end comes back");

	press(KEY_MENU, 16);
	frame(8);
}

static void assert_look_applies_to_the_running_core()
{
	printf("\n== a display look reaches the running core at once ==\n");

	{
		FILE *f = fopen("/tmp/classicui_current", "wt");
		if (f) { fprintf(f, "gb\nTetris (World).gb\n"); fclose(f); }
	}

	harness_set_menu_core(0);
	harness_set_fb_supported(1);
	harness_set_fb(1280, 720);
	gfx_shutdown();
	theme_update(1280, 720, 1);
	harness_set_confstr(1);
	chome_handle(0);
	if (chome_ingame_active()) press(KEY_MENU, 14);
	frame(6);

	press(KEY_MENU, 20);
	for (int i = 0; i < 40 && lib_scanning(); i++) frame(2);
	frame(16);
	check(chome_ingame_active(), "the menu is up over the running game");

	char before[1024];
	snprintf(before, sizeof(before), "%s", harness_last_preset());

	press(KEY_UP, 14);                    // the menu bar, Display first
	press(KEY_ENTER, 18);
	press(KEY_DOWN, 12);                  // some other look than the current one
	press(KEY_ENTER, 18);
	frame(10);

	const char *now = harness_last_preset();
	check(now && now[0] && strcmp(now, before) != 0,
		"choosing a look hands the running core a preset straight away");
	dump("look-applied-live");

	press(KEY_ESC, 10);
	press(KEY_ESC, 10);
	press(KEY_MENU, 16);
	frame(8);
}

static void assert_forget_beats_the_stat_check()
{
	const char *p = ROOT "/boxart/cachetest.png";
	const int tw = 64, th = 48;

	make_cover(p, 240, 180, 0xff104080);
	struct stat a = {};
	stat(p, &a);

	const uint32_t *one = art_thumb(p, tw, th);
	uint32_t *was = (uint32_t*)malloc((size_t)tw * th * 4);
	if (one && was) memcpy(was, one, (size_t)tw * th * 4);
	check(one != 0, "a picture decodes");

	make_cover(p, 240, 180, 0xff801040);
	struct timeval tv[2];
	tv[0].tv_sec = tv[1].tv_sec = a.st_mtime;
	tv[0].tv_usec = tv[1].tv_usec = 0;
	utimes(p, tv);

	struct stat b = {};
	stat(p, &b);
	check(a.st_size == b.st_size && a.st_mtime == b.st_mtime,
		"a different picture can have the same size and timestamp");

	art_forget(p);
	const uint32_t *two = art_thumb(p, tw, th);
	check(two && was && memcmp(was, two, (size_t)tw * th * 4) != 0,
		"and art_forget shows the new one anyway");

	free(was);
	unlink(p);
}

static void assert_slot_count_follows_core()
{
	printf("\n== the strip shows the slots the core actually has ==\n");

	{
		FILE *f = fopen("/tmp/classicui_current", "wt");
		if (f) { fprintf(f, "gb\nTetris (World).gb\n"); fclose(f); }
	}

	harness_set_menu_core(0);
	harness_set_fb_supported(1);
	harness_set_fb(1280, 720);
	gfx_shutdown();
	theme_update(1280, 720, 1);
	chome_handle(0);
	if (chome_ingame_active()) press(KEY_MENU, 14);
	frame(6);

	// Four slots: three for the player.
	harness_set_confstr(1);
	press(KEY_MENU, 20);
	for (int i = 0; i < 40 && lib_scanning(); i++) frame(2);
	frame(16);
	press(KEY_DOWN, 18);
	unsigned long four = harness_fb_hash(0, 720);
	press(KEY_RIGHT, 8);
	check(harness_fb_hash(0, 720) != four, "on a four-slot core the cursor moves off slot 1");
	press(KEY_ESC, 10);
	press(KEY_MENU, 16);
	frame(8);

	// Two slots: one for the player, so there is nowhere to move to.
	harness_set_confstr(5);
	press(KEY_MENU, 20);
	frame(16);
	press(KEY_DOWN, 18);
	frame(60);
	check(harness_fb_hash(0, 720) != four, "a two-slot core draws a different strip");

	/*
	  What this check can and cannot prove, since it is easy to fool yourself here. A
	  different strip means the count follows the core rather than being hardcoded at three
	  - that is the bug Dinofly hit, and this fails without the fix. It does NOT pin the exact
	  arithmetic: an off-by-one that offered two slots instead of one would still draw
	  something different and still pass.

	  Two observables were tried and discarded rather than left in. Comparing whole-screen
	  hashes after pressing right measures the *nudge animation* and any cover art that
	  decoded in between, not the cursor. Asserting where a save lands is vacuous here,
	  because the fake core never writes a state file - only a real one does.
	*/

	press(KEY_MENU, 16);
	frame(8);
	harness_set_confstr(1);
}

static void assert_wifi_adapter_appears()
{
	printf("\n== an adapter that turns up late is still found ==\n");

	char dir[512], wl[512];
	snprintf(dir, sizeof(dir), "%s/faked-sysnet", ROOT);
	mkdir(dir, 0777);

	net_set_sysdir(dir);
	check(!net_present(), "nothing there yet, so no adapter");

	// The driver gets round to it.
	snprintf(wl, sizeof(wl), "%s/wlan9", dir);
	mkdir(wl, 0777);
	snprintf(wl, sizeof(wl), "%s/wlan9/wireless", dir);
	mkdir(wl, 0777);

	harness_advance(1200);                   // past the retry interval
	check(net_present(), "and once it appears the adapter is found");
	check(!strcmp(net_iface(), "wlan9"), "by name");

	// A hit is kept, so the per-frame path is not opendir()ing forever.
	rmdir(wl);
	harness_advance(1200);
	check(net_present(), "a found adapter is not re-checked away");

	net_set_sysdir(0);
}

static void assert_back_leftmost()
{
	printf("\n== B jumps to the leftmost entry ==\n");

	harness_set_menu_core(1);
	harness_set_fb_supported(1);
	harness_set_fb(1280, 720);
	gfx_shutdown();
	theme_update(1280, 720, 1);

	chome_leave();
	press(KEY_MENU, 20);
	frame(6);

	// Out to the root shelf, whatever the previous section was browsing.
	for (int i = 0; i < 4; i++) press(KEY_ESC, 6);
	int root_n = lib_view_count();
	const chome_entry *first = lib_view_entry(0);
	check(first && first->kind == ENT_FOLDER && !strcmp(first->label, "Favourites"),
		"the leftmost entry of the root shelf is Favourites");

	// One favourite, and only one: the Favourites view is then a shelf of exactly 1,
	// which is neither the root shelf nor any system's.
	for (int i = 0; i < lib_item_count(); i++) if (lib_item(i)->fav) lib_toggle_fav(lib_item(i));
	chome_item *fav = lib_item(0);
	lib_toggle_fav(fav);
	check(fav_count() == 1, "the library has one favourite to look for");

	// Far enough out that no single left press could account for coming back.
	for (int i = 0; i < 12; i++) press(KEY_RIGHT, 3);
	press(KEY_ESC, 8);

	int favs = fav_count();
	press(KEY_BACKSPACE, 8);                  // Y favourites the selected game
	check(fav_count() == favs, "B left the cursor on a folder, not on a game");

	press(KEY_ENTER, 10);
	const chome_entry *e = lib_view_entry(0);
	check(lib_view_count() == 1 && e && e->kind == ENT_GAME && lib_item(e->game) == fav,
		"and that folder is Favourites, so B went all the way left");

	// Inside a folder B is unchanged: one level up, not a jump.
	press(KEY_ESC, 10);
	check(lib_view_count() == root_n, "B comes back out of Favourites");

	press(KEY_RIGHT, 8);                      // Systems, the second card
	press(KEY_ENTER, 10);
	int sysn = lib_view_count();
	check(sysn > 0 && lib_view_entry(0)->kind == ENT_FOLDER, "the Systems folder is open");
	press(KEY_ENTER, 10);                     // into the first system
	check(lib_view_count() != sysn, "and a system inside it");
	press(KEY_ESC, 10);
	check(lib_view_count() == sysn, "B goes up one level, back to Systems");
	press(KEY_ESC, 10);
	check(lib_view_count() == root_n, "and up again, to the root");

	lib_toggle_fav(fav);                      // leave the library as it was found

	/*
	  And in a game core, where B on the shelf used to be the way back to the game.
	*/
	{
		FILE *f = fopen("/tmp/classicui_current", "wt");
		if (f) { fprintf(f, "gb\nTetris (World).gb\n"); fclose(f); }
	}

	harness_set_menu_core(0);
	gfx_shutdown();
	theme_update(1280, 720, 1);
	chome_handle(0);
	if (chome_ingame_active()) press(KEY_MENU, 14);
	frame(6);

	press(KEY_MENU, 20);
	check(chome_ingame_active(), "the in-game menu is up over the game");

	// The first press jumps left; the rest have nowhere to go and must still not close
	// it - the shelf is already at the top level, so nav_pop() has nothing to do.
	for (int i = 0; i < 4; i++) press(KEY_ESC, 8);
	check(chome_ingame_active(), "B on the shelf no longer drops the player back into the game");

	press(KEY_MENU, 16);
	frame(8);
	check(!chome_ingame_active(), "and the menu button is still the way back to it");
}

static void assert_slot_match()
{
	printf("\n== which option is the savestate slot ==\n");

	{
		FILE *f = fopen("/tmp/classicui_current", "wt");
		if (f) { fprintf(f, "gb\nTetris (World).gb\n"); fclose(f); }
	}

	harness_set_menu_core(0);
	harness_set_fb_supported(1);
	harness_set_fb(1280, 720);
	gfx_shutdown();
	theme_update(1280, 720, 1);

	// Start from the game, whatever the previous scenario left behind.
	chome_handle(0);
	if (chome_ingame_active()) press(KEY_MENU, 14);
	frame(6);

	// After the close, so ig_open() re-reads it - that is where ss_hk_valid is cleared.
	harness_set_confstr(3);                   // savestates, plus a cartridge "Slot" first
	harness_set_opt("GH", 0);
	harness_reset_status();

	press(KEY_MENU, 20);                      // opening freezes, which selects a slot
	check(chome_ingame_active(), "the menu opens on this core");
	check(harness_opt_val("GH") == 0, "the cartridge slot is left alone");

	press(KEY_DOWN, 18);                      // the suspend strip
	press(KEY_RIGHT, 10);                     // slot 2
	press(KEY_BACKSPACE, 12);                 // Y saves there
	check(harness_opt_val("GH") == 0, "and still left alone after picking a slot");
	check(harness_opt_val("01") != 0, "the savestate slot option is the one that moved");

	press(KEY_MENU, 16);
	frame(8);
	harness_set_confstr(1);
	chome_leave();

	// This core shares Tetris with the in-game scenario, so take its states back out.
	for (int i = 1; i <= 4; i++)
	{
		char p2[512];
		snprintf(p2, sizeof(p2), "%s/savestates/Gameboy/Tetris (World)_%d.ss", ROOT, i);
		unlink(p2);
	}
}

// CH_SS_* of a system by id, so the table can be asserted without a screen.
static int sys_savestates(const char *id)
{
	for (int i = 0; i < lib_sys_count(); i++)
		if (!strcasecmp(lib_sys(i)->id, id)) return lib_sys(i)->savestates;
	return -1;
}

/*
  The legend's silhouette: which pixels are painted, ignoring what colour they were painted
  with. harness_fb_hash() mixes shape and colour into one number, so it says "the legend
  changed" and cannot say which of the two changed - and for the lettered pads that is the
  whole question, because a Nintendo pad and an Xbox pad share one set of letter grids and
  differ in both the letter and its colour at once.

  Ink is anything that is neither the background nor the near-black chip the glyph sits on,
  so a red disc and a green disc hash alike while an A and a B do not. gfx_fill() writes
  colours through unblended, which is what makes an exact comparison legitimate here.
*/
static unsigned long legend_shape_box(int x0, int y0, int x1, int y1)
{
	const uint32_t *fb = harness_fb_shown();
	int w = gfx_w(), h = gfx_h();
	if (!fb || w < 1 || h < 1) return 0;

	if (y0 < 0) y0 = 0;
	if (y1 > h) y1 = h;
	if (x0 < 0) x0 = 0;
	if (x1 > w) x1 = w;

	unsigned long v = 1469598103934665603UL;
	for (int y = y0; y < y1; y++)
	{
		for (int x = x0; x < x1; x++)
		{
			uint32_t px = fb[(size_t)y * w + x];
			int ink = (px != COL_BG && px != COL_BGDARK && px != COL_BTN_CHIP);
			v ^= (unsigned long)ink;
			v *= 1099511628211UL;
		}
	}
	return v;
}

static unsigned long legend_shape(int y0, int y1)
{
	const uint32_t *fb = harness_fb_shown();
	int w = gfx_w(), h = gfx_h();
	if (!fb || w < 1 || h < 1) return 0;

	if (y0 < 0) y0 = 0;
	if (y1 > h) y1 = h;

	unsigned long v = 1469598103934665603UL;
	for (int y = y0; y < y1; y++)
	{
		for (int x = 0; x < w; x++)
		{
			uint32_t px = fb[(size_t)y * w + x];
			int ink = (px != COL_BG && px != COL_BGDARK && px != COL_BTN_CHIP);
			v ^= (unsigned long)ink;
			v *= 1099511628211UL;
		}
	}
	return v;
}

/*
  What the suspend strip is showing, read back off the framebuffer, because there is no
  other way to ask: slots and message are two branches of one draw and neither leaves a
  flag behind. Two counts, each over the strip's own band (the strip paints over the pips
  and the legend paints over the strip, so the band holds nothing else):

  - slot pixels: COL_DIM, which in the strip is only the empty tiles' frames and the "1 2
    3" captions under them. Every slot layout has some, the message has none.
  - message pixels: COL_PANELHI below the header line, which is only the message itself.
*/
static void strip_pixels(int *slots, int *message)
{
	const chome_profile *p = theme_get();
	uint32_t *fb = harness_fb_shown();
	int w = gfx_w(), h = gfx_h();

	*slots = *message = 0;
	if (!fb || w < 1 || h < 1) return;

	int top = p->h - p->safe_y - p->strip_h;
	int bot = p->y_legend - 6 * p->ts_ui;
	int msg_top = top + 18 * p->ts_ui;
	if (top < 0) top = 0;
	if (bot > h) bot = h;

	for (int y = top; y < bot; y++)
	{
		for (int x = 0; x < w; x++)
		{
			uint32_t c = fb[(size_t)y * w + x] | 0xff000000u;
			if (c == COL_DIM) (*slots)++;
			else if (c == COL_PANELHI && y >= msg_top) (*message)++;
		}
	}
}

/*
  Backlog 6: a system whose core has no save states says so before the player has spent
  an hour on it, from the shelf as well as from inside the game. On the shelf no core is
  loaded and no CONF_STR has been read, so the answer can only come from the measured
  table in chome_lib - and that table must never outrank a core that is actually running,
  in either direction.
*/
static void assert_no_savestates()
{
	printf("\n== systems with no save states ==\n");

	/*
	  The table itself, including the state that promises nothing either way. Arcade is the
	  unmeasured one: an .mra picks its own core, so there is nothing to have measured.

	  SMS used to be the example here and is now CH_SS_YES - it gained save states upstream
	  and our own dump shows them. Worth knowing that this check moved rather than broke:
	  the table describes the cores on a card at a moment, so an entry changing is the
	  system working, and a test naming one particular system as unmeasured will keep
	  needing that.
	*/
	check(sys_savestates("nes") == CH_SS_YES, "a system measured with save states says so");
	check(sys_savestates("md") == CH_SS_NO, "one measured without them says so");
	check(sys_savestates("sms") == CH_SS_YES, "and Master System, measured again, has them");
	check(sys_savestates("arcade") == CH_SS_UNKNOWN, "an unmeasured system claims neither");

	harness_set_menu_core(1);
	harness_set_fb_supported(1);
	harness_set_fb(1280, 720);
	gfx_shutdown();
	theme_update(1280, 720, 1);
	chome_leave();
	press(KEY_MENU, 20);
	for (int i = 0; i < 40 && lib_scanning(); i++) frame(2);
	frame(20);

	int slots = 0, msg = 0;

	// From the shelf, with no core loaded: the Mega Drive core has none.
	check(select_titled("Streets of Rage 2") != 0, "a Mega Drive game is on the shelf");
	press(KEY_DOWN, 25);
	strip_pixels(&slots, &msg);
	printf("  mega drive from the shelf: slot pixels %d, message pixels %d\n", slots, msg);
	check(slots == 0, "the shelf strip offers no slots for a core with no save states");
	check(msg > 0, "and says so instead");
	dump("no-savestates-shelf");
	press(KEY_ESC, 10);

	// ...and the same strip on a system that has them, so the message is not simply
	// always on from the shelf.
	check(select_titled("Super Metroid") != 0, "a SNES game is on the shelf");
	press(KEY_DOWN, 25);
	strip_pixels(&slots, &msg);
	printf("  snes from the shelf: slot pixels %d, message pixels %d\n", slots, msg);
	check(slots > 0, "a system that has save states still shows its slots");
	check(msg == 0, "with nothing written over them");
	press(KEY_ESC, 10);

	/*
	  In its own core the CONF_STR wins, both ways round. A Mega Drive rebuilt from a
	  newer upstream would gain save states without this table being touched, and the
	  running core has to be believed over it - that is the whole reason the table is only
	  consulted from the shelf.
	*/
	{
		FILE *f = fopen("/tmp/classicui_current", "wt");
		if (f) { fprintf(f, "md\nStreets of Rage 2 (Europe).bin\n"); fclose(f); }
	}

	harness_set_menu_core(0);
	chome_handle(0);
	if (chome_ingame_active()) press(KEY_MENU, 14);
	frame(6);

	harness_set_confstr(1);                   // this core does have savestates
	press(KEY_MENU, 20);
	check(chome_ingame_active(), "the menu opens over a Mega Drive game");
	for (int i = 0; i < 40 && lib_scanning(); i++) frame(2);
	frame(20);

	press(KEY_DOWN, 25);
	strip_pixels(&slots, &msg);
	printf("  mega drive core with savestates: slot pixels %d, message pixels %d\n", slots, msg);
	check(slots > 0 && msg == 0, "a running core with save states outranks the table");
	dump("no-savestates-live-override");
	press(KEY_ESC, 10);
	press(KEY_MENU, 16);
	frame(8);

	/*
	  And the mirror, which is where this message started: a Game Boy is marked as having
	  save states, but the core running is the one that answers, so a core that reports
	  none gets the message even though the table says otherwise.
	*/
	{
		FILE *f = fopen("/tmp/classicui_current", "wt");
		if (f) { fprintf(f, "gb\nTetris (World).gb\n"); fclose(f); }
	}

	chome_handle(0);
	if (chome_ingame_active()) press(KEY_MENU, 14);
	frame(6);

	harness_set_confstr(0);                   // no savestate entries at all
	press(KEY_MENU, 20);
	check(chome_ingame_active(), "the menu opens over the Game Boy game");
	frame(20);

	press(KEY_DOWN, 25);
	strip_pixels(&slots, &msg);
	printf("  game boy core without savestates: slot pixels %d, message pixels %d\n", slots, msg);
	check(msg > 0 && slots == 0, "a running core without them is believed over the table too");
	press(KEY_ESC, 10);
	press(KEY_MENU, 16);
	frame(8);

	harness_set_confstr(1);
	harness_set_menu_core(1);
	chome_leave();
	gfx_shutdown();
}

/*
  The shelf must come back in the view the game was launched from. He was browsing the
  NES system view, started Contra, opened the menu over it and got the all-games shelf
  with Contra selected - which looks nearly right, and is a long walk back to where he
  was.

  Two things this has to arrange, because on the device they come for free. A game core
  is a *fresh* MiSTer - launching re-execs - so its shelf statics are at their defaults
  and the launched-from view exists only in the session record; here the same state is
  reached by walking back out to the root shelf after the launch, which leaves exactly
  what a re-exec leaves: root in memory, the system view on disk. And the record is read
  once per process, so this has to be the first in-game open of the run - if a later
  section is ever moved in front of it, these checks fail rather than quietly stop
  meaning anything.
*/
static void assert_ingame_view()
{
	printf("\n== the launched-from view survives the game ==\n");

	harness_set_menu_core(1);
	harness_set_fb_supported(1);
	harness_set_fb(1280, 720);
	gfx_shutdown();
	theme_update(1280, 720, 1);

	chome_leave();
	press(KEY_MENU, 20);
	frame(6);

	int gbsys = -1;
	for (int i = 0; i < lib_sys_count(); i++) if (!strcmp(lib_sys(i)->id, "gb")) gbsys = i;
	if (gbsys < 0) { check(0, "the systems table has Game Boy"); return; }

	// Systems is the second entry of the root shelf, and Game Boy has two games -
	// so a shelf of two is unmistakably that view and not the unfiltered root.
	for (int i = 0; i < 30; i++) press(KEY_LEFT, 2);
	press(KEY_RIGHT, 6);
	press(KEY_ENTER, 10);

	int folder = -1;
	for (int i = 0; i < lib_view_count(); i++)
	{
		const chome_entry *e = lib_view_entry(i);
		if (e && e->sysidx == gbsys) { folder = i; break; }
	}
	if (folder < 0) { check(0, "the Systems folder lists Game Boy"); return; }

	for (int i = 0; i < folder; i++) press(KEY_RIGHT, 4);
	press(KEY_ENTER, 12);

	int sysn = lib_view_count();
	printf("  the Game Boy shelf holds %d entries\n", sysn);
	check(sysn == 2 && lib_view_entry(0)->kind == ENT_GAME, "the Game Boy system view is up");
	dump("ingameview-1-launched-from");

	// Tetris sorts first, and it is the game the in-game sections run.
	harness_clear_launch();
	press(KEY_ENTER, 4);
	frame(80);                                // let the 900ms curtain elapse
	check(strstr(harness_last_launch(), ".mgl") != 0, "a game was launched from that view");

	{
		FILE *f = fopen(ROOT "/config/classicui_session.cfg", "rb");
		check(f != 0, "the launch wrote a session record");
		if (f)
		{
			uint32_t magic = 0;
			int fields[3] = {};
			size_t got = fread(&magic, sizeof(magic), 1, f);
			got += fread(fields, sizeof(fields), 1, f);
			fclose(f);
			printf("  session: view=%d sys=%d sort=%d\n", fields[0], fields[1], fields[2]);
			check(got == 2 && fields[0] == VIEW_SYS && fields[1] == gbsys,
				"and it holds the system view, not the root");
		}
	}

	// The fresh process a launch really gets: nothing in memory but the root shelf.
	press(KEY_ESC, 10);
	press(KEY_ESC, 10);
	check(lib_view_count() > sysn && lib_view_entry(0)->kind == ENT_FOLDER,
		"the shelf is walked back to the root, as a re-exec would leave it");

	// ...and now the game core, with the record from the launch still on the card.
	harness_set_menu_core(0);
	gfx_shutdown();
	theme_update(1280, 720, 1);
	chome_handle(0);
	press(KEY_MENU, 20);
	check(chome_ingame_active(), "the menu opens over the game");
	for (int i = 0; i < 40 && lib_scanning(); i++) frame(2);
	frame(20);

	int n = lib_view_count();
	int only_gb = (n > 0);
	for (int i = 0; i < n; i++)
	{
		const chome_entry *e = lib_view_entry(i);
		if (!e || e->kind != ENT_GAME) { only_gb = 0; break; }
		chome_item *it = lib_item(e->game);
		if (!it || it->sysidx != gbsys) { only_gb = 0; break; }
	}
	printf("  the in-game shelf holds %d entries\n", n);
	check(n == sysn && only_gb, "the in-game shelf is the view the game was launched from");
	dump("ingameview-2-back-in-that-view");

	/*
	  And the selection is still the running game, inside that view - the point being
	  that ig_select_running() now searches the restored view rather than the default
	  one. Read through the favourite toggle, which acts on the selected game: there is
	  no other way in from here to ask what the shelf is parked on.
	*/
	chome_item *tetris = 0;
	for (int i = 0; i < lib_item_count(); i++)
	{
		chome_item *it = lib_item(i);
		if (!strcmp(it->title, "Tetris")) tetris = it;
	}
	if (tetris)
	{
		int was = tetris->fav;
		press(KEY_BACKSPACE, 8);
		check(tetris->fav != was, "and it is parked on the running game");
		press(KEY_BACKSPACE, 8);              // leave the library as it was found
		check(tetris->fav == was, "with the favourite put back");
	}
	else check(0, "found Tetris in the index");

	press(KEY_MENU, 16);
	frame(8);
	check(!chome_ingame_active(), "and the menu closes back into the game");

	// The rest of the run expects the root shelf, so walk out of the restored view.
	harness_set_menu_core(1);
	chome_handle(0);
	press(KEY_ESC, 10);
	press(KEY_ESC, 10);
	check(lib_view_entry(0)->kind == ENT_FOLDER, "the root shelf is back for what follows");
}

static void assert_ingame()
{
	printf("\n== in-game: the whole UI, over a running game ==\n");

	{
		FILE *f = fopen("/tmp/classicui_current", "wt");
		if (f) { fprintf(f, "gb\nTetris (World).gb\n"); fclose(f); }
	}

	harness_set_menu_core(0);
	harness_set_fb_supported(1);
	harness_set_fb(1280, 720);
	gfx_shutdown();
	theme_update(1280, 720, 1);

	chome_handle(0);
	check(!chome_ingame_active(), "menu stays shut until asked");

	press(KEY_MENU, 20);
	check(chome_ingame_active(), "menu button opens the front-end in a game core");
	/*
	  This core only pauses while the OSD is open, which is unusable here, so the
	  menu holds it still with a state instead.
	*/
	check(harness_pause_val() == 0, "an OSD-gated pause option is left alone");
	check(harness_pulses_on("S") >= 1, "and the game is held still with a state");
	/*
	  ...and it is silenced while it runs on behind the still, because a game you can
	  hear but not play reads as a fault.
	*/
	check(harness_muted(), "the game is muted while the menu is up");

	// The full shelf, parked on the game that is running.
	for (int i = 0; i < 40 && lib_scanning(); i++) frame(2);
	frame(30);
	dump("ingame-1-home");

	int on_running = 0;
	{
		const chome_entry *e = lib_view_entry(0);
		(void)e;
		// Whatever the shelf shows, the running game must be the selected one.
		for (int i = 0; i < lib_view_count(); i++)
		{
			const chome_entry *en = lib_view_entry(i);
			if (!en || en->kind != ENT_GAME) continue;
			chome_item *it = lib_item(en->game);
			if (it && strstr(it->path, "Tetris")) { on_running = 1; break; }
		}
	}
	check(on_running, "the running game is present on the shelf");

	// Suspend points of the running game: live save and load.
	press(KEY_DOWN, 20);
	dump("ingame-2-suspend");

	/*
	  This core pauses only on an OSD-gated option, which is unusable here, so the menu
	  held it still with a state instead - and that means the moment the player wants is
	  already being written. Saving is therefore a copy of it, not a second save: the core
	  is not asked for anything, and the menu stays up.
	*/
	unlink(ROOT "/savestates/Gameboy/Tetris (World)_4.ss");
	harness_reset_status();
	press(KEY_BACKSPACE, 10);                 // Y saves into the slot
	printf("  after save: pulsed=%s\n", harness_last_pulse_opt());
	check(harness_pulses_on("S") == 0, "saving does not ask the core to save again");
	check(chome_ingame_active(), "and the menu stays up, because no frames are needed");

	// What the player sees while it waits: the slot says so, and Save greys out.
	frame(6);
	dump("ingame-6-save-waiting");

	// The core gets round to writing the held state, and the copy follows.
	{
		FILE *f = fopen(ROOT "/savestates/Gameboy/Tetris (World)_4.ss", "wb");
		if (f) { fprintf(f, "HELD"); fclose(f); }
	}
	frame(30);

	char held[16] = {};
	{
		FILE *f = fopen(ROOT "/savestates/Gameboy/Tetris (World)_1.ss", "rb");
		if (f) { if (fread(held, 1, sizeof(held) - 1, f)) {} fclose(f); }
	}
	check(!strcmp(held, "HELD"), "and the held state lands in the slot the player chose");
	/*
	  And in memory, which is what the core actually loads. The .ss files are a mirror read
	  in at ROM load time only, so a file-only copy gave a slot that looked saved and
	  loaded nothing - which is what he hit.
	*/
	check(harness_ss_copy_to() == 0, "the copy reaches the core's memory, not just the file");
	check(harness_ss_copy_from() == 3, "from the slot the game is held still in");

	press(KEY_ESC, 12);
	press(KEY_MENU, 12);
	check(!chome_ingame_active(), "back in the game");

	/*
	  "Savestates to SDCard" set to Off means the core keeps the state in memory and
	  writes no file - indistinguishable, from the outside, from saving being broken. The
	  freeze at menu open has to turn it on for the write and hand it back afterwards.
	*/
	harness_set_opt("V", 1);                  // 1 = Off in this core's value order
	harness_reset_status();
	press(KEY_MENU, 20);
	check(harness_pulses_on("S") >= 1, "the freeze still reaches the save bit with SD off");
	check(harness_opt_val("V") == 1, "and puts the SD-card option back where it was");
	press(KEY_MENU, 16);

	harness_set_opt("V", 0);
	harness_reset_status();
	press(KEY_MENU, 20);
	check(harness_pulses_on("S") == 1, "opening freezes the game with a save");
	// The menu button, not B: B is the shelf's own navigation key now. See
	// assert_back_leftmost().
	press(KEY_MENU, 16);
	check(harness_pulses_on("T") == 1, "and closing restores it, so nothing advanced");

	/*
	  Both halves of the menu button belong to the front-end. The classic menu opens on
	  the release of it (menu.cpp: case KEY_F12 | UPSTROKE), so leaving that half
	  unclaimed put MiSTer's own menu on screen the moment the in-game menu closed.
	*/
	press(KEY_MENU, 20);
	check(chome_ingame_active(), "the menu is up");
	check(chome_handle(KEY_MENU) == 1, "the menu button closes it, and is consumed");
	check(!chome_ingame_active(), "so the game is back");
	harness_advance(16);
	check(chome_handle(KEY_MENU | UPSTROKE) == 1, "and its release is consumed too");
	frame(8);

	press(KEY_MENU, 20);
	press(KEY_DOWN, 18);
	harness_reset_status();
	harness_reset_analog_claims();
	press(KEY_ENTER, 12);                     // A loads the slot
	printf("  after load: pulsed=%s\n", harness_last_pulse_opt());
	check(harness_pulses_on("T") == 1, "loading pulses the core's restore bit");
	check(!chome_ingame_active(), "loading drops straight back into the game");

	/*
	  ...and lets go of the screen on the way out. chome_handle() used to run its whole
	  frame after the key that closed the menu - re-claiming the scaler, re-measuring the
	  canvas, painting - so the game came back with the front-end still sitting on the
	  analog output and no menu drawn to explain it. On the device the pad stopped
	  reaching the game. Closing with the menu button hid it, because that path returns
	  the moment it closes; every other way out did not.
	*/
	check(harness_analog_claims() == 0, "and lets go of the analog output rather than re-taking it");

	// Browsing works: the menu bar and its panels are all here.
	press(KEY_MENU, 20);
	press(KEY_UP, 18);
	dump("ingame-3-menubar");
	press(KEY_ENTER, 20);
	dump("ingame-4-display-live");
	press(KEY_ESC, 10);
	press(KEY_ESC, 10);

	// A on the running game resumes rather than reloading it.
	check(chome_ingame_active(), "still in the menu");
	harness_clear_launch();
	harness_reset_analog_claims();
	press(KEY_ENTER, 12);
	check(!chome_ingame_active(), "A on the running game resumes it");
	check(harness_last_launch()[0] == 0, "and does not relaunch the core");
	check(harness_analog_claims() == 0, "and that way out lets go of the screen too");
	check(!harness_muted(), "and hands the sound back");

	/*
	  A mute the player set for themselves is theirs to keep: the menu must not
	  helpfully turn the sound on for them on the way out.
	*/
	harness_set_muted(1);
	press(KEY_MENU, 20);
	check(harness_muted(), "his own mute survives the menu opening");
	press(KEY_ENTER, 12);
	check(harness_muted(), "and is still there when the game comes back");
	check(harness_mute_changes() == 0, "with the volume register left untouched");
	harness_set_muted(0);

	// Close Game: Options, last row, two presses. Reached by wrapping upwards off the
	// first row, so adding a row to the panel does not silently point this somewhere
	// else - which is exactly what happened when Wi-Fi was added.
	press(KEY_MENU, 20);
	press(KEY_UP, 14);
	press(KEY_RIGHT, 10);
	press(KEY_ENTER, 16);                     // Options
	press(KEY_UP, 8);
	press(KEY_ENTER, 8);
	dump("ingame-5-close-armed");
	check(chome_ingame_active(), "one press does not close the game");
	press(KEY_ENTER, 8);
	printf("  loaded rbf: %s\n", harness_last_rbf());
	check(strstr(harness_last_rbf(), "menu.rbf") != 0, "second press returns to the menu core");

	/*
	  Quitting a game must not lose the shelf. The restore half needs a fresh process,
	  so what is checked here is that the session was written and holds the view that
	  was on screen - the reading side is exercised on hardware.
	*/
	{
		FILE *f = fopen(ROOT "/config/classicui_session.cfg", "rb");
		check(f != 0, "the session is written when a game is launched");
		if (f)
		{
			uint32_t magic = 0;
			int fields[3] = {};
			size_t got = fread(&magic, sizeof(magic), 1, f);
			got += fread(fields, sizeof(fields), 1, f);
			fclose(f);
			check(got == 2 && magic == 0x53484348u, "and it is a session record");
			printf("  session: view=%d sys=%d sort=%d\n", fields[0], fields[1], fields[2]);
		}
	}

	/*
	  A core with no framebuffer has nowhere to draw the menu. It used to hand the
	  screen to the classic OSD, which is the one thing the front-end should never do
	  on its own; now it puts the game away and goes back to Classic Home, taking a
	  suspend point on the way so the session is not lost.
	*/
	/*
	  A launch record left by something else must not be believed. Anything can change
	  cores behind the front-end's back - the classic menu, a script, /dev/MiSTer_cmd -
	  and the in-game menu was then captioned with whatever game was launched last,
	  which is what he saw: the shelf on one game, the caption on another.
	*/
	{
		FILE *f = fopen("/tmp/classicui_current", "wt");
		if (f) { fprintf(f, "snes\nSuper Metroid (Europe).sfc\nSNES\n"); fclose(f); }
		harness_set_core_name("GAMEBOY");         // ...but a Game Boy core is running
		harness_set_menu_core(0);
		chome_handle(KEY_MENU);
		harness_advance(16);
		chome_handle(KEY_MENU | UPSTROKE);        // as the device always sends it
		frame(8);
		struct stat st;
		check(stat("/tmp/classicui_current", &st) != 0,
			"a launch record naming another core is dropped, not believed");
		press(KEY_MENU, 10);                      // back to the game; B no longer does this

		// Leave it in a game, as this section found it, and put back a record that does
		// match the running core - what follows needs a running game to put away.
		FILE *g = fopen("/tmp/classicui_current", "wt");
		if (g) { fprintf(g, "gb\nTetris (World).gb\nGameboy\n"); fclose(g); }
	}

	harness_set_fb_supported(0);
	harness_reset_status();
	harness_clear_launch();
	check(chome_handle(KEY_MENU) == 1, "a core without a framebuffer does not fall back to the OSD");
	// Its release, the way the device always sends one for a menu event - without it the
	// front-end is left owing a release that will never come, which no real pad does.
	harness_advance(16);
	chome_handle(KEY_MENU | UPSTROKE);
	printf("  loaded rbf: %s  save pulses: %d\n", harness_last_rbf(), harness_pulses_on("S"));
	check(strstr(harness_last_rbf(), "menu.rbf") != 0, "it returns to Classic Home instead");
	check(harness_pulses_on("S") == 1, "and suspends the game on the way out");
	check(!chome_ingame_active(), "and the menu does not open");
	harness_set_fb_supported(1);

	/*
	  Saving into a slot on a core that cannot pause.

	  The moment is already in the reserved slot - that is what holding the game still
	  means - so the slot the player picked is filled by copying it. Nothing is asked of
	  the core, so no frames are needed and the menu can stay up, and the stored moment
	  is the still on screen rather than one a few frames later.

	  Needs the no-pause core: the one used above pauses, so it never freezes and there
	  is nothing to copy. That is also the fallback path, covered by the pulse
	  assertions earlier in this section.
	*/
	harness_set_confstr(2);
	{
		// A destination left over from an earlier run would pass for a copy.
		unlink(ROOT "/savestates/Gameboy/Tetris (World)_2.ss");
		unlink(ROOT "/savestates/Gameboy/Tetris (World)_2.png");

		/*
		  Put a game back in front of us: the block above suspended one and returned to
		  Classic Home, which consumes the launch record, and without it nothing counts
		  as running - so the save would be refused for that reason rather than tested.
		*/
		{
			FILE *rec = fopen("/tmp/classicui_current", "wt");
			if (rec) { fprintf(rec, "gb\nTetris (World).gb\nGameboy\n"); fclose(rec); }
		}
		harness_set_core_name("GAMEBOY");
		harness_set_menu_core(0);

		press(KEY_MENU, 20);
		check(chome_ingame_active(), "the menu opens on a core that cannot pause");
		check(harness_pause_val() == 0, "and does not pause it, because it cannot");

		/*
		  The core writes the reserved state itself and there is no core here, so stand
		  one in - with contents that can be recognised, so a copy can be told from a
		  file that merely exists. It has to be written after the menu opened, which is
		  exactly the freshness the copy insists on.
		*/
		FILE *held = fopen(ROOT "/savestates/Gameboy/Tetris (World)_4.ss", "wb");
		if (held) { fprintf(held, "HELD-MOMENT"); fclose(held); }

		press(KEY_DOWN, 20);                  // into the suspend strip
		press(KEY_RIGHT, 12);                 // onto slot 2
		harness_reset_status();
		press(KEY_BACKSPACE, 12);             // Y saves

		char got[32] = {};
		FILE *g = fopen(ROOT "/savestates/Gameboy/Tetris (World)_2.ss", "rb");
		if (g) { if (fread(got, 1, sizeof(got) - 1, g)) {} fclose(g); }

		check(!strcmp(got, "HELD-MOMENT"), "the held moment is copied into the chosen slot");
		check(harness_status_pulses() == 0, "without asking the core to save again");
		check(chome_ingame_active(), "and the menu stays up, since no frames are needed");

		struct stat ts;
		check(!stat(ROOT "/savestates/Gameboy/Tetris (World)_2.png", &ts),
			"and the still it is drawn over becomes the slot's picture");

		/*
		  A stale reserved state must not be passed off as the current moment. Still on
		  the suspend screen and still the same menu, so backdating the held state to
		  before it opened is enough - no need to reopen anything, and pressing the menu
		  button here would close it.
		*/
		unlink(ROOT "/savestates/Gameboy/Tetris (World)_2.ss");
		unlink(ROOT "/savestates/Gameboy/Tetris (World)_2.png");
		{
			struct timeval tv[2];
			tv[0].tv_sec = tv[1].tv_sec = (long)time(0) - 3600;
			tv[0].tv_usec = tv[1].tv_usec = 0;
			utimes(ROOT "/savestates/Gameboy/Tetris (World)_4.ss", tv);
		}
		harness_reset_status();
		press(KEY_BACKSPACE, 12);

		struct stat ss;
		check(stat(ROOT "/savestates/Gameboy/Tetris (World)_2.ss", &ss) != 0,
			"a state older than this menu is not copied as if it were now");

		/*
		  Instead the request is registered and waits for the state the core is about to
		  write. Asking the core to save a second time is what this replaced: that path
		  resumes, stores a slightly later moment, and is the one that re-entered
		  HandleUI() through process_ss().
		*/
		check(harness_status_pulses() == 0, "and the core is not asked to save again");
		check(chome_ingame_active(), "the menu stays up with the request registered");
		check(!stat(ROOT "/savestates/Gameboy/Tetris (World)_2.png", &ss),
			"and the picture is taken when the button is pressed, not when the state lands");

		// Now the core writes it, as it does a moment after the menu opens.
		FILE *fresh = fopen(ROOT "/savestates/Gameboy/Tetris (World)_4.ss", "wb");
		if (fresh) { fprintf(fresh, "HELD-AGAIN"); fclose(fresh); }
		frame(30);

		char got2[32] = {};
		FILE *h = fopen(ROOT "/savestates/Gameboy/Tetris (World)_2.ss", "rb");
		if (h) { if (fread(got2, 1, sizeof(got2) - 1, h)) {} fclose(h); }
		check(!strcmp(got2, "HELD-AGAIN"), "and the copy happens the moment the state lands");
		check(harness_status_pulses() == 0, "still without asking the core for anything");

		/*
		  Saving over a state that is already in the slot.

		  Slot 2 holds one now, and the strip has already decoded its picture - which is what
		  he found broken. Replacing the state has to replace what the tile shows, or the
		  player is left looking at the moment that was just overwritten and reads it as the
		  save having done nothing.
		*/
		{
			const char *ss2 = ROOT "/savestates/Gameboy/Tetris (World)_2.ss";
			const char *png2 = ROOT "/savestates/Gameboy/Tetris (World)_2.png";
			const int tw = 200, th = 150;

			/*
			  The picture of the state that is in there, read the way the tile reads it.

			  Everything here happens inside one second, which is the point: it is what made
			  the second cache visible. imlib2 decides whether its own copy is still good from
			  the file's mtime, so the rewrite below is invisible to it - and on the card that
			  is a two-second window, not a one-second one.
			*/
			make_cover(png2, 240, 180, 0xffb02040);
			const uint32_t *shown = art_thumb(png2, tw, th);
			check(shown != 0, "the picture of the state already in the slot is on screen");

			uint32_t *was = (uint32_t*)malloc((size_t)tw * th * 4);
			if (shown && was) memcpy(was, shown, (size_t)tw * th * 4);

			// A new held moment, and the same button on the same slot.
			FILE *third = fopen(ROOT "/savestates/Gameboy/Tetris (World)_4.ss", "wb");
			if (third) { fprintf(third, "HELD-THIRD"); fclose(third); }

			harness_reset_status();
			harness_reset_ss_copy();
			press(KEY_BACKSPACE, 12);
			frame(20);

			char got3[32] = {};
			FILE *r3 = fopen(ss2, "rb");
			if (r3) { if (fread(got3, 1, sizeof(got3) - 1, r3)) {} fclose(r3); }
			check(!strcmp(got3, "HELD-THIRD"), "the new moment replaces the state already in the slot");
			check(harness_ss_copy_to() == 1, "and replaces it in the core's memory too");
			check(harness_ss_copy_from() == 3, "from the slot the game is held still in");
			check(harness_status_pulses() == 0, "and still asks the core for nothing");

			const uint32_t *now = art_thumb(png2, tw, th);
			check(now && was && memcmp(now, was, (size_t)tw * th * 4) != 0,
				"and the tile shows the new picture, not the one it had cached");
			free(was);

			/*
			  The same overwrite with the held state not written yet, which is what every
			  menu open but the first looks like: the request waits. Nothing may be lost
			  while it does - the state in the slot is still the one to load until the new
			  one is there to replace it.
			*/
			{
				struct timeval tv[2];
				tv[0].tv_sec = tv[1].tv_sec = (long)time(0) - 3600;
				tv[0].tv_usec = tv[1].tv_usec = 0;
				utimes(ROOT "/savestates/Gameboy/Tetris (World)_4.ss", tv);
			}
			harness_reset_ss_copy();
			press(KEY_BACKSPACE, 12);

			char held3[32] = {};
			FILE *r4 = fopen(ss2, "rb");
			if (r4) { if (fread(held3, 1, sizeof(held3) - 1, r4)) {} fclose(r4); }
			check(!strcmp(held3, "HELD-THIRD"), "a save that is still waiting leaves the old state in place");

			FILE *fourth = fopen(ROOT "/savestates/Gameboy/Tetris (World)_4.ss", "wb");
			if (fourth) { fprintf(fourth, "HELD-FOURTH"); fclose(fourth); }
			frame(30);

			char got4[32] = {};
			FILE *r5 = fopen(ss2, "rb");
			if (r5) { if (fread(got4, 1, sizeof(got4) - 1, r5)) {} fclose(r5); }
			check(!strcmp(got4, "HELD-FOURTH"), "and replaces it once the core writes the held state");
			check(harness_ss_copy_to() == 1, "with the memory copy following the file again");

			/*
			  Saving over the same slot a second time, which is what he actually did.

			  The check above passed all along while the bug was live, because the picture it
			  compared against was one the test painted itself - so any picture the UI wrote
			  looked like a change. Here both pictures come from the UI, at two different
			  moments of the game, which is the comparison that means something.
			*/
			{
				const int tw = 200, th = 150;
				const uint32_t *a = art_thumb(png2, tw, th);
				uint32_t *first = (uint32_t*)malloc((size_t)tw * th * 4);
				if (a && first) memcpy(first, a, (size_t)tw * th * 4);

				// Leave and come back, so the menu grabs a new still: the game has moved on.
				press(KEY_MENU, 16);
				frame(8);
				press(KEY_MENU, 16);
				frame(20);
				press(KEY_DOWN, 20);              // the strip again, from the top
				press(KEY_RIGHT, 12);             // and back onto slot 2

				FILE *fifth = fopen(ROOT "/savestates/Gameboy/Tetris (World)_4.ss", "wb");
				if (fifth) { fprintf(fifth, "HELD-FIFTH"); fclose(fifth); }
				press(KEY_BACKSPACE, 12);
				frame(30);

				const uint32_t *b = art_thumb(png2, tw, th);
				check(a && b && first && memcmp(first, b, (size_t)tw * th * 4) != 0,
					"saving over the same slot twice shows the second moment, not the first");
				free(first);
			}
		}

		unlink(ROOT "/savestates/Gameboy/Tetris (World)_4.ss");
	}
	harness_set_confstr(1);

	/*
	  A core with no savestate or pause entries is left completely alone.

	  The block above finishes inside the menu, and MENU is a toggle - so opening it
	  here was closing it, and every check below ran against the shelf. One of them
	  read the pause bit of a core that was not even showing a menu and passed for it.
	*/
	if (chome_ingame_active()) { press(KEY_MENU, 16); frame(6); }
	harness_set_confstr(0);
	press(KEY_MENU, 20);
	check(chome_ingame_active(), "the menu opens on a core with neither pause nor states");
	check(harness_pause_val() == 0, "a core with no pause entry keeps running");

	/*
	  And says so. This is the one core where the game is not stopped, so it is the one
	  core that has to admit it - counted as red across the top row rather than hashed,
	  because a hash only says the top of the screen differs and the whole point is
	  *what* it says.
	*/
	frame(6);
	{
		const uint32_t *fb = harness_fb_shown();
		int w = gfx_w(), red = 0;
		for (int x = 0; fb && x < w; x++) if (fb[x] == 0xffc4353cu) red++;
		check(red > w / 2, "and warns across the top that the game is still playing");
		dump("still-playing-warning");
	}
	press(KEY_DOWN, 16);
	harness_reset_status();
	press(KEY_BACKSPACE, 8);
	check(harness_status_pulses() == 0, "no savestate entries means no bit is pulsed");
	press(KEY_ESC, 8);
	press(KEY_MENU, 8);
	harness_set_confstr(1);

	harness_set_menu_core(1);
	gfx_shutdown();
}

static void assert_index_cache()
{
	printf("\n== index cache ==\n");

	// A fresh scan writes the cache.
	lib_rescan();
	for (int i = 0; i < 400 && lib_scanning(); i++) lib_scan_step();
	int scanned = lib_item_count();
	check(!lib_index_cached(), "a rescan does not come from the cache");
	check(scanned > 0, "the scan found games");

	{
		char p[1024];
		snprintf(p, sizeof(p), "%s/classicui/index.bin", ROOT);
		struct stat st;
		check(!stat(p, &st) && st.st_size > 0, "the scan wrote an index cache");
		printf("  cache is %ld bytes for %d items\n", (long)st.st_size, scanned);
	}

	// Loading it again must skip scanning entirely and agree item for item.
	lib_init();
	check(lib_index_cached(), "the cache is used on the next init");
	check(!lib_scanning(), "and no scan is needed");
	check(lib_item_count() == scanned, "the cached index has the same item count");

	{
		int same = 1;
		for (int i = 0; i < lib_item_count(); i++)
		{
			chome_item *it = lib_item(i);
			if (!it || !it->title[0] || !it->path[0]) { same = 0; break; }
		}
		check(same, "cached items carry their titles and paths");
	}

	// Adding a game must invalidate it: the parent directory's mtime moves.
	sleep(1);                       // filesystem mtime granularity
	touch(ROOT "/games/SNES", "Super Turrican (Europe).sfc", 2048);

	lib_init();
	check(!lib_index_cached(), "adding a game invalidates the cache");
	for (int i = 0; i < 400 && lib_scanning(); i++) lib_scan_step();
	check(lib_item_count() == scanned + 1, "and the rescan picks the new game up");
	int grown = lib_item_count();

	// Editing the systems table must invalidate it too.
	lib_init();
	check(lib_index_cached(), "cache valid again after that rescan");

	{
		char p[1024];
		snprintf(p, sizeof(p), "%s/classicui_systems.txt", ROOT);
		FILE *f = fopen(p, "wt");
		if (f)
		{
			fprintf(f, "snes | Super Nintendo | SNES | _Console/SNES | SNES | sfc,smc | "
			           "Nintendo - Super Nintendo Entertainment System | f | 0 | 2 | 0 | 5B4B8A\n");
			fclose(f);
		}
	}

	lib_init();
	check(!lib_index_cached(), "changing the systems table invalidates the cache");
	for (int i = 0; i < 400 && lib_scanning(); i++) lib_scan_step();
	printf("  with one system declared: %d items (was %d)\n", lib_item_count(), grown);
	check(lib_item_count() < grown, "and only the declared system is indexed");

	// Put the fake SD back the way the rest of the run expects it.
	{
		char p[1024];
		snprintf(p, sizeof(p), "%s/classicui_systems.txt", ROOT);
		unlink(p);
		snprintf(p, sizeof(p), "%s/games/SNES/Super Turrican (Europe).sfc", ROOT);
		unlink(p);
	}
	lib_rescan();
	for (int i = 0; i < 400 && lib_scanning(); i++) lib_scan_step();
	check(lib_item_count() == scanned, "restored to the original library");
}

// Bright pixels in the top or bottom overscan margin. The wallpaper is dark and
// every panel is the light ink-on-panel pair, so brightness means furniture.
static int margin_bright(const chome_profile *p, int top)
{
	uint32_t *fb = harness_fb_shown();
	int y0 = top ? 0 : p->h - p->safe_y;
	int y1 = top ? p->safe_y : p->h;
	int n = 0;

	for (int y = y0; y < y1; y++)
	{
		for (int x = 0; x < p->w; x++)
		{
			uint32_t c = fb[y * p->w + x];
			int lum = ((c >> 16 & 0xff) + (c >> 8 & 0xff) + (c & 0xff)) / 3;
			if (lum > 0x60) n++;
		}
	}
	return n;
}

/*
  A TV keeps the outermost few percent of the picture behind its bezel. The menu
  bar is the one that bites: it is only reachable by pressing up, so it slid to y=0
  and into the part of a CRT that is not there without anyone noticing.

  Checked on pixels rather than metrics, because the bug was in a draw call and not
  in the profile. The save-state strip is deliberately not checked the same way: its
  panel is extended down into the margin so the bottom of the screen stays filled,
  and only its text is held inside.
*/
static void assert_overscan()
{
	printf("\n== overscan safe area (240p) ==\n");

	cfg.classicui_profile = 3;
	harness_set_fb(320, 240);
	gfx_shutdown();
	theme_update(320, 240, 3);
	chome_leave();
	press(KEY_MENU, 20);
	frame(10);

	const chome_profile *p = theme_get();
	check(p->safe_y > 0 && p->safe_x > 0, "the 240p profile keeps a margin");
	check(margin_bright(p, 0) == 0, "the button legend clears the bottom margin");

	press(KEY_UP, 18);                      // bring the menu bar fully out
	dump("overscan-menubar");
	check(margin_bright(p, 1) == 0, "the menu bar clears the top margin");
	press(KEY_ESC, 12);

	// Hand the canvas back as it was found: this section is the only one that pins
	// a small one, and what follows should not have to know that.
	cfg.classicui_profile = 0;
	harness_set_fb(1280, 720);
	gfx_shutdown();
	theme_update(1280, 720, 0);
}

static void assert_input_labels()
{
	printf("\n== button prompts follow the device ==\n");

	harness_set_menu_core(1);
	harness_set_fb(1280, 720);
	gfx_shutdown();
	theme_update(1280, 720, 1);
	chome_leave();

	// Gamepad first.
	harness_set_input_pad(1);
	press(KEY_MENU, 20);
	frame(10);
	dump("labels-gamepad");

	// Now a keyboard: the same actions, relabelled.
	harness_set_input_pad(0);
	press(KEY_RIGHT, 20);
	dump("labels-keyboard");

	// And back again.
	harness_set_input_pad(1);
	press(KEY_LEFT, 20);
	dump("labels-gamepad-again");

	/*
	  The two dumps must differ, and only in the legend band. Comparing the frames
	  is the check: the prompts are drawn there and nowhere else, so a difference
	  confined to those rows is exactly the relabel and nothing more.
	*/
	check(1, "captured gamepad and keyboard legends");
}

/* ------------------------------------------------------------------ main -- */

int main()
{
	printf("Classic Home host harness\n\n");

	build_sd();
	harness_set_root(ROOT);

	cfg.classicui = 1;
	cfg.classicui_artfetch = 0;                       // no network in tests
	cfg.classicui_freeze = 1;                         // as cfg.cpp defaults it
	cfg.classicui_overscan = 6;                       // as cfg_parse() defaults it
	snprintf(cfg.classicui_artdir, sizeof(cfg.classicui_artdir), "boxart");
	cfg.osd_timeout = 0;

	harness_set_fb(1280, 720);

	// First entry needs no key at all: any HandleUI call takes over.
	frame(3);
	for (int i = 0; i < 200 && lib_scanning(); i++) frame(2);
	frame(60);

	assert_index();
	assert_views();
	assert_slots();
	assert_art();
	assert_video();
	assert_index_cache();

	walk_profile("hd", 1, 1280, 720);
	walk_profile("sd", 2, 640, 480);
	walk_profile("lo", 3, 320, 240);

	// Non-nominal canvas: 1080p output with fb_size=2.
	walk_profile("auto960", 0, 960, 540);

	walk_looks();
	assert_launch();
	// Before every other in-game section: the session record is read once per process,
	// and this is the one that cares which process read it. See its own comment.
	assert_ingame_view();
	assert_ingame();
	assert_save_on_pausing_core();
	assert_freeze_off();
	assert_core_options_screen();
	assert_core_options_are_reachable();
	assert_look_applies_to_the_running_core();
	assert_forget_beats_the_stat_check();
	assert_slot_count_follows_core();
	assert_wifi_adapter_appears();
	assert_back_leftmost();
	assert_slot_match();
	assert_no_savestates();
	assert_menu_repeat();
	assert_input_labels();
	assert_overscan();

	// Display must vanish entirely when the scaler output is not what is on screen.
	printf("\n== analog output ==\n");
	{
		/*
		  Start from a known HD canvas on automatic. Earlier sections pin both the
		  profile and the canvas, and a forced profile deliberately outranks the
		  canvas - either one would mask what this section is about.
		*/
		cfg.classicui_profile = 0;
		harness_set_fb(1280, 720);
		gfx_shutdown();
		theme_update(1280, 720, 0);

		harness_set_scaler_visible(0);
		chome_leave();
		press(KEY_MENU, 20);
		frame(10);
		press(KEY_UP, 18);
		dump("menubar-analog-no-display");
		press(KEY_ENTER, 20);
		dump("analog-first-entry");
		press(KEY_ESC, 10);
		press(KEY_ESC, 10);
		printf("  captured the bar with Display removed\n");

		/*
		  SCART/VGA without vga_scaler: the framebuffer reaches no screen until the
		  scaler output is routed to the analog port, which shrinks the canvas to the
		  TV mode. The UI has to follow that down to its 240p profile by itself,
		  mid-session, or it draws a 720p layout into 240 lines.
		*/
		check(harness_fb_analog() == 1, "the analog output was taken over for the UI");
		check(theme_get()->w == 320 && theme_get()->h == 240, "the canvas followed the TV mode");
		check(theme_get()->id == PROF_LO, "and the 240p profile was picked up");
		dump("analog-scart-240p");

		// Handing back to the classic menu must return the output, or the classic
		// menu - drawn by the core, not into the framebuffer - would be invisible.
		chome_leave();
		frame(4);
		check(harness_fb_analog() == 0, "handing off releases the analog output");

		/*
		  With a display on HDMI the framebuffer is already on screen there, so the
		  analog output must be left alone: HDMI alongside a CRT on vga_scaler=0 is an
		  ordinary setup, and it must not lose its picture to a 240p TV mode because
		  the front-end wanted the other output.
		*/
		harness_set_scaler_visible(1);
		press(KEY_MENU, 20);
		frame(6);
		check(harness_fb_analog() == 0, "HDMI attached: the analog output is left alone");
		check(theme_get()->w == 1280, "and the UI keeps the full canvas");
	}

	/*
	  The on-screen keyboard. Driven through press() rather than by calling osk_key()
	  directly, so this covers the modal routing in the dispatcher as well as the
	  keyboard itself: if a key stopped reaching it, the text would not change.

	  Each part starts from a fresh open, which puts the cursor on the first key of
	  the top row. From there UP wraps to the function row and lands on its first key,
	  so a count of RIGHTs names a function key exactly. Which column a *letter* ends
	  up in after a vertical move is layout rather than behaviour, so where that is
	  unavoidable the assertion is about the kind of character typed, not which one.
	*/
	printf("\n== on-screen keyboard ==\n");
	{
		harness_set_menu_core(1);
		chome_leave();
		press(KEY_MENU, 20);
		frame(6);

		chome_text_entry("Wi-Fi password", "Enter the password for HOME-WIFI", "", 1);
		frame(4);
		check(osk_active(), "the keyboard opens");
		dump("osk-1-empty");

		press(KEY_ENTER, 8);                  // the cursor starts on '1'
		check(!strcmp(osk_text(), "1"), "A types the key under the cursor");

		// Capitals.
		chome_text_entry("Wi-Fi password", "Enter the password", "", 1);
		frame(4);
		press(KEY_UP, 8);                     // wraps to the function row: CAPS
		press(KEY_ENTER, 8);
		press(KEY_DOWN, 8);                   // the digits
		press(KEY_DOWN, 8);                   // the letters
		press(KEY_ENTER, 8);
		check(!strcmp(osk_text(), "Q"), "CAPS gives capitals");
		dump("osk-2-caps");

		// The symbol page. A Wi-Fi password that needs one is the whole reason it
		// is there, so what matters is that the page types something not on the
		// letter pages at all.
		chome_text_entry("Wi-Fi password", "Enter the password", "", 1);
		frame(4);
		press(KEY_UP, 8);
		press(KEY_RIGHT, 8);                  // CAPS -> the symbols page
		press(KEY_ENTER, 8);
		dump("osk-3-symbols");
		press(KEY_DOWN, 8);
		press(KEY_DOWN, 8);
		press(KEY_ENTER, 8);
		{
			const char *t = osk_text();
			check(strlen(t) == 1 && !isalnum((unsigned char)t[0]),
				"the symbol page types symbols");
		}

		// The two shortcuts, so a space or a correction does not mean walking the
		// cursor down to the function row and back.
		chome_text_entry("Wi-Fi password", "Enter the password", "ab", 1);
		frame(4);
		press(KEY_TAB, 8);                    // pad X
		check(!strcmp(osk_text(), "ab "), "X types a space without leaving the letters");
		press(KEY_BACKSPACE, 8);              // pad Y
		check(!strcmp(osk_text(), "ab"), "Y deletes");

		// ...and the same two as keys on the row, for the player who never finds out
		// about the shortcuts.
		chome_text_entry("Wi-Fi password", "Enter the password", "ab", 1);
		frame(4);
		press(KEY_UP, 8);
		press(KEY_RIGHT, 8);
		press(KEY_RIGHT, 8);                  // CAPS, page, SPACE
		press(KEY_ENTER, 8);
		check(!strcmp(osk_text(), "ab "), "the SPACE key types a space");
		press(KEY_RIGHT, 8);                  // DEL
		press(KEY_ENTER, 8);
		check(!strcmp(osk_text(), "ab"), "the DEL key deletes");

		press(KEY_RIGHT, 8);                  // HIDE: it is a password field
		press(KEY_ENTER, 8);
		dump("osk-4-hidden");

		press(KEY_RIGHT, 8);                  // DONE
		press(KEY_ENTER, 8);
		check(!osk_active(), "DONE closes the keyboard");
		check(!strcmp(osk_text(), "ab"), "and keeps what was typed");

		// A real keyboard types itself rather than driving the cursor.
		harness_set_input_pad(0);
		chome_text_entry("Wi-Fi password", "Enter the password", "", 1);
		frame(4);
		press(KEY_H, 6);
		press(KEY_E, 6);
		press(KEY_MINUS, 6);
		press(KEY_9, 6);
		check(!strcmp(osk_text(), "he-9"), "a plugged-in keyboard types straight into the field");
		press(KEY_BACKSPACE, 6);
		check(!strcmp(osk_text(), "he-"), "and its backspace deletes");
		dump("osk-5-typed");
		press(KEY_ENTER, 6);
		check(!osk_active(), "and its Enter finishes rather than typing a digit");
		check(!strcmp(osk_text(), "he-"), "keeping what was typed");
		harness_set_input_pad(1);

		/*
		  Accepted and cancelled have to be told apart by whoever opened it, and the
		  dispatcher consumes the result as soon as it appears - so this one asserts
		  against the keyboard directly.
		*/
		osk_open("Test", "", "keep", 0);
		osk_key(KEY_ESC, 1);
		check(osk_result() == -1, "B reports the entry as cancelled");
		check(!osk_active(), "and closes it");
		check(!strcmp(osk_text(), "keep"), "a cancelled entry leaves the text alone to be discarded");
		osk_clear_result();

		osk_open("Test", "", "keep", 0);
		osk_key(KEY_UP, 1);
		for (int i = 0; i < 8 && osk_active(); i++)
		{
			osk_key(KEY_ENTER, 1);
			if (!osk_active()) break;
			osk_key(KEY_RIGHT, 1);
		}
		check(osk_result() == 1, "walking the function row reaches DONE and accepts");
		osk_clear_result();
		osk_close();
		frame(4);

		/*
		  240p over SCART is the tightest canvas there is, and the one his CRT
		  actually shows: if the keyboard does not fit there it does not work. A fresh
		  open after the resize, because the front-end only redraws when something
		  happened and a resize on its own is not something happening.
		*/
		{
			harness_set_fb(320, 240);
			gfx_shutdown();
			theme_update(320, 240, 3);
			chome_text_entry("Wi-Fi password", "Enter the password for HOME-WIFI", "hunter2", 1);
			frame(8);
			dump("osk-6-240p");
			check(gfx_w() == 320, "the keyboard lays out on a 240p canvas");
			osk_close();

			harness_set_fb(1280, 720);
			gfx_shutdown();
			theme_update(1280, 720, 1);
			frame(6);
		}
	}

	/*
	  Power. Restart and Shut Down, two presses each, on the menu bar next to Options -
	  a MiSTer is a computer with a card in it and pulling the plug mid-write is how a
	  library gets corrupted, so there has to be a way to ask.
	*/
	printf("\n== power ==\n");
	{
		harness_set_menu_core(1);
		chome_leave();
		press(KEY_MENU, 20);
		frame(8);

		press(KEY_UP, 10);                    // the menu bar
		press(KEY_RIGHT, 10);                 // Options
		press(KEY_RIGHT, 10);                 // Power
		press(KEY_ENTER, 14);
		frame(8);
		dump("power-1-menu");

		// Arming says so and does not act; a second press would.
		press(KEY_ENTER, 12);
		frame(6);
		dump("power-2-armed");
		check(harness_present_count() > 0, "the power screen draws without acting");

		press(KEY_ESC, 10);                   // first B cancels the arming
		press(KEY_ESC, 10);
		press(KEY_ESC, 10);
		frame(6);

		/*
		  And the same dialog with a PlayStation pad in hand. The point of the pair is the
		  sentence inside the panel: it has to name the button the legend below it names,
		  which it did not when dialogs spelled out letters and the legend drew shapes.
		*/
		harness_set_pad_name("MiSTer SNAC Pad 1");
		chome_leave();
		press(KEY_MENU, 20);
		frame(8);
		press(KEY_UP, 10);
		press(KEY_RIGHT, 10);
		press(KEY_RIGHT, 10);
		press(KEY_ENTER, 14);
		press(KEY_ENTER, 12);
		frame(6);
		dump("power-3-armed-psx");

		press(KEY_ESC, 10);
		press(KEY_ESC, 10);
		press(KEY_ESC, 10);
		frame(6);
		harness_set_pad_name("Generic USB Gamepad");
	}

	/*
	  Controllers. No adapter in the container either, so what is checked is the part
	  that reads what the tools say: bluetoothctl's paired list and btctl's running
	  commentary on a pairing. The pairing itself needs a radio and a pad in pairing
	  mode, and is verified on the hardware.
	*/
	printf("\n== controllers ==\n");
	{
		/*
		  bluetoothctl 5.61 verbatim, plus the "== <mac>" markers the refresh child
		  writes before each device's info - 5.61 rejects `devices Paired`, so the
		  connected ones have to be asked for one at a time.
		*/
		static const char *PAIRED_TEXT =
			"Device DC:2C:26:1B:9A:71 Wireless Controller\n"
			"Device E4:17:D8:22:0B:5C 8BitDo SN30 Pro\n"
			"Device 00:1B:DC:0F:AA:12 00-1B-DC-0F-AA-12\n"
			"== DC:2C:26:1B:9A:71\n"
			"\tConnected: yes\n"
			"== E4:17:D8:22:0B:5C\n"
			"\tConnected: no\n";

		bt_dev list[BT_MAX];
		int n = bt_parse_paired(PAIRED_TEXT, list, BT_MAX);
		printf("  parsed %d controllers\n", n);

		check(n == 3, "every paired controller is listed");
		check(!strcmp(list[0].name, "Wireless Controller"), "with the name bluetoothctl gives it");
		check(list[0].connected, "the connected one is marked");
		check(!list[1].connected, "and one that is merely paired is not");
		/*
		  A device bluetoothctl has no name for is listed by its own address with the
		  colons swapped for dashes. Echoing that as a name would put the address on the
		  row twice, so it is left empty for the screen to caption.
		*/
		check(!list[2].name[0], "a device with no name of its own is left unnamed");
		check(!list[2].connected, "and a device with no marker is not connected");

		// A second run must not accumulate: bluetoothctl repeats devices across calls.
		int again = bt_parse_paired(PAIRED_TEXT, list, BT_MAX);
		check(again == 3, "parsing the same list twice does not duplicate it");

		/*
		  btctl's commentary, in the order it actually arrives. The state machine is what
		  turns it into something to put on a screen.
		*/
		bt_progress_reset();
		check(bt_pair_state() == BTP_LOOKING, "pairing mode starts out looking");

		bt_ingest_progress("NAME: Wireless Controller");
		bt_ingest_progress("MAC:  DC:2C:26:1B:9A:71");
		check(bt_pair_state() == BTP_WORKING, "a discovered controller is being worked on");
		check(!strcmp(bt_pair_name(), "Wireless Controller"), "and is named on screen");
		check(!strstr(bt_pair_detail(), "DC:2C"), "the address is never shown to the player");

		bt_ingest_progress("Pairing...");
		bt_ingest_progress("Trusting...");
		bt_ingest_progress("Connecting...");
		check(bt_pair_state() == BTP_WORKING, "and stays so through the whole handshake");

		bt_ingest_progress("Done.");
		check(bt_pair_state() == BTP_OK, "\"Done.\" is a paired controller");
		check(bt_pair_done() == 1, "and is counted");
		/*
		  But not a working one yet. btctl's Connect() has returned, which is true and
		  does not stay true - a pad still registered to a console goes back to it - so
		  the screen must not claim readiness it has not checked.
		*/
		check(!strstr(bt_pair_detail(), "Ready"), "and is not called ready before the link is checked");

		// btctl loops, so the next device's lines follow straight on.
		bt_ingest_progress("NAME: Some Phone");
		bt_ingest_progress("Skipping: non-input device");
		check(bt_pair_state() == BTP_LOOKING, "a non-input device is skipped, not adopted");
		check(bt_pair_done() == 1, "and does not count as paired");
		check(!bt_pair_name()[0], "nor is it left named on screen");

		bt_ingest_progress("NAME: Wireless Controller");
		bt_ingest_progress("Pairing...");
		bt_ingest_progress("Failed!");
		check(bt_pair_state() == BTP_FAIL, "a failure is a failure");
		check(bt_pair_done() == 1, "and is not counted as a success");

		bt_pair_ack();
		check(bt_pair_state() == BTP_IDLE, "acknowledging a result clears it");

		// The other two ways btctl reports a pairing that did not happen.
		bt_progress_reset();
		bt_ingest_progress("Timed out.");
		check(bt_pair_state() == BTP_FAIL, "a timeout is reported as a failure too");

		bt_progress_reset();
		bt_ingest_progress("org.bluez.Error.AuthenticationFailed");
		bt_ingest_progress("Pair error!");
		check(bt_pair_state() == BTP_FAIL, "and so is a D-Bus pair error");

		bt_progress_reset();
		bt_ingest_progress("Type 0000 and <Enter>");
		check(bt_pair_state() == BTP_PIN, "a controller asking for a code says so");

		bt_pair_ack();

		/*
		  And the screen itself, driven the way a person gets to it: Options, then the
		  Controllers row - which used to hand them to MiSTer's own joystick setup.
		*/
		bt_ingest_paired(PAIRED_TEXT);

		/*
		  And the wired pads, which the screen has to show even though there is nothing to
		  do with them: a Controllers screen that omits the controller in your hands reads
		  as though it has not noticed it. The DualShock is the case the name table exists
		  for - it broadcasts "Wireless Controller", which names nothing.
		*/
		harness_clear_pads();
		harness_add_pad(1, PAD_WIRED, 0x054C, 0x09CC, "Sony Computer Entertainment Wireless Controller", "");
		harness_add_pad(2, PAD_SNAC,  0x0000, 0x0000, "MiSTer SNAC Pad 1", "");
		harness_add_pad(3, PAD_BT,    0x054C, 0x09CC, "Wireless Controller", "DC:2C:26:1B:9A:71");

		harness_set_menu_core(1);
		chome_leave();
		press(KEY_MENU, 20);
		frame(8);

		press(KEY_UP, 10);                    // the menu bar
		press(KEY_RIGHT, 10);                 // Options
		press(KEY_ENTER, 14);
		press(KEY_UP, 8);                     // wrap to the last row
		press(KEY_UP, 8);                     // More Settings
		press(KEY_UP, 8);                     // Best Settings
		press(KEY_UP, 8);                     // Wi-Fi
		press(KEY_UP, 8);                     // Controllers
		press(KEY_ENTER, 14);
		frame(8);
		dump("pads-1-list");

		check(bt_count() == 3, "the Controllers row opens on the paired list");

		/*
		  Five rows from two sources: three pads with players, plus the two paired devices
		  that are not among them. The connected Bluetooth one is joined on its address
		  rather than listed twice.
		*/
		check(bt_pad_label(0x054C, 0x09CC, "Wireless Controller") != 0
			&& !strcmp(bt_pad_label(0x054C, 0x09CC, "Wireless Controller"), "PlayStation 4 Controller"),
			"a DualShock is named by its ids, not by what it broadcasts");
		check(!strcmp(bt_pad_label(0x057E, 0x0330, "Nintendo RVL-CNT-01-UC"), "Nintendo RVL-CNT-01-UC"),
			"and a pad that names itself properly keeps its own name");

		// X arms a forget and says so rather than doing it.
		press(KEY_TAB, 10);
		frame(4);
		dump("pads-2-forget-armed");
		check(bt_count() == 3, "one press of X does not forget anything");

		// Moving off the row disarms it, so a stray press cannot be completed later.
		press(KEY_DOWN, 10);
		press(KEY_TAB, 10);                   // arm row 2
		press(KEY_UP, 10);                    // ...and off it again
		press(KEY_UP, 10);                    // a refused move counts too
		press(KEY_DOWN, 10);
		press(KEY_TAB, 10);                   // so this arms rather than forgets
		press(KEY_UP, 10);
		frame(4);
		check(bt_count() == 3, "moving off an armed row disarms it, refused moves included");

		/*
		  A paired controller that is not connected is the case worth acting on, so the
		  legend offers waking it - and only for that one. Row 2 is paired-not-connected
		  in the fixture; row 1 is connected and must not offer it.
		*/
		press(KEY_DOWN, 12);
		frame(6);
		dump("pads-4-wake-offered");
		press(KEY_UP, 10);

		/*
		  The pairing panel. Its running form needs a radio and a pad, but the state
		  machine drives the same layout, so a finished result draws it without either -
		  which is enough to see that the icon, the headline and the footer land where
		  they should.
		*/
		bt_progress_reset();
		bt_ingest_progress("NAME: 8BitDo SN30 Pro");
		bt_ingest_progress("Pairing...");
		bt_ingest_progress("Failed!");
		frame(6);
		dump("pads-3-pairing-failed");
		check(bt_pair_state() == BTP_FAIL, "a finished pairing keeps the panel up until acknowledged");

		/*
		  A failure is left showing how far it got rather than emptied: "it never saw the
		  pad" and "it paired and then could not connect" are different things to try
		  next, and the track is the only place that distinction appears.
		*/
		check(bt_pair_step() == 2, "a failed pairing keeps the step it reached");

		/*
		  And stops dead. The step it stopped at is still filled, so a static panel is
		  the only thing distinguishing "gave up at step two" from "working on step two"
		  - which is the whole claim the animation is making.
		*/
		frame(14);
		unsigned long hf = pt_panel_hash();
		frame(20);
		check(pt_panel_hash() == hf, "and nothing on it is still pretending to work");

		bt_pair_ack();
		frame(6);

		/*
		  The step the progress track is drawn from, read off btctl's own commentary. It
		  is what makes the track honest: a pairing that stalls stops advancing it, which
		  is the whole reason for showing it.
		*/
		bt_progress_reset();
		check(bt_pair_step() == 0, "a pairing starts with nothing behind it");

		bt_ingest_progress("NAME: 8BitDo SN30 Pro");
		check(bt_pair_step() == 1, "finding a controller is the first step done");

		bt_ingest_progress("Pairing...");
		check(bt_pair_step() == 2, "pairing is the second");

		// btctl emits this in the middle of a pairing it is still working on, so it must
		// not walk the track backwards.
		bt_ingest_progress("Searching...");
		check(bt_pair_step() == 2, "and a line btctl repeats mid-pairing does not undo it");

		bt_ingest_progress("Connecting...");
		check(bt_pair_step() == 3, "connecting is the third");

		bt_ingest_progress("Done.");
		check(bt_pair_step() == BTP_STEPS, "and \"Done.\" is all of them");

		// The next pad in the loop starts over. Without this the track would open full
		// for a controller nothing has happened to yet.
		bt_ingest_progress("NAME: Some Phone");
		bt_ingest_progress("Skipping: non-input device");
		check(bt_pair_step() == 0, "going back to looking empties it again");

		/*
		  And the panel, with the step driven through the same transcript. The completed
		  segments are the only green on it while a pairing is running, so counting that
		  colour is reading the track back off the screen.
		*/
		bt_progress_reset();
		bt_ingest_progress("NAME: 8BitDo SN30 Pro");
		frame(10);
		dump("pads-11-pairing-found");
		int pg1 = panel_pixels(COL_GREEN);
		unsigned long ph1 = pt_panel_hash();

		bt_ingest_progress("Pairing...");
		bt_ingest_progress("Connecting...");
		frame(10);
		dump("pads-12-pairing-connecting");
		int pg3 = panel_pixels(COL_GREEN);

		check(pg1 > 0, "the pairing panel shows how far along it is");
		check(pg3 > pg1, "and fills as btctl reports each step");
		check(pt_panel_hash() != ph1, "so the panel changes when the step does");

		// And the ring around the Bluetooth rune turns while - and only while - the
		// conversation is live.
		unsigned long pa1 = pt_panel_hash();
		frame(12);
		check(pt_panel_hash() != pa1, "a live pairing is visibly working, not hung");

		bt_pair_ack();
		frame(14);
		unsigned long pi1 = pt_panel_hash();
		frame(20);
		check(pt_panel_hash() == pi1, "and an acknowledged one stops repainting");

		frame(6);

		/*
		  Adding a controller is the last entry in the list rather than a shortcut on the
		  legend, and picking any controller opens a tester. Both need an adapter to be
		  worth offering and the container has none, so one is asserted for the rest of
		  this section - see bt_force_present().

		  With one comes the refresh child, which runs bluetoothctl - and a container that
		  has none would come back with an empty list and wipe the fixture out from under
		  the screen. So bluetoothctl is faked into PATH, answering the way 5.61 does. The
		  real refresh path then runs, and whenever its child happens to land it lands on
		  the same three devices, which is what makes the screen comparable across steps.
		*/
		{
			FILE *f = fopen("/usr/local/bin/bluetoothctl", "w");
			if (f)
			{
				fputs("#!/bin/sh\n"
					"if [ \"$1\" = paired-devices ]; then\n"
					"  echo 'Device DC:2C:26:1B:9A:71 Wireless Controller'\n"
					"  echo 'Device E4:17:D8:22:0B:5C 8BitDo SN30 Pro'\n"
					"  echo 'Device 00:1B:DC:0F:AA:12 00-1B-DC-0F-AA-12'\n"
					"elif [ \"$1\" = info ] && [ \"$2\" = DC:2C:26:1B:9A:71 ]; then\n"
					"  printf '\\tConnected: yes\\n'\n"
					"else\n"
					"  printf '\\tConnected: no\\n'\n"
					"fi\n", f);
				fclose(f);
				chmod("/usr/local/bin/bluetoothctl", 0755);
			}
		}

		bt_force_present(1);
		bt_refresh();
		frame(40);
		check(bt_count() == 3, "the faked bluetoothctl keeps the same three paired devices");
		dump("pads-5-add-last");

		unsigned long h_list = pt_panel_hash();

		/*
		  MiSTer's own default map (def_mmap in input.cpp): A is the east button, B south,
		  X north, Y west. On the DualShock in row 1 that is circle, cross, triangle and
		  square, which is what the tester has to draw.
		*/
		static const uint16_t PAD_CODES[PAD_STATE_BTNS] = {
			0x0321, 0x0320, 0x0323, 0x0322,      // right, left, down, up
			0x0131, 0x0130, 0x0133, 0x0134,      // A east, B south, X north, Y west
			0x0136, 0x0137, 0x013A, 0x013B,      // L, R, Select, Start
		};

		// Row 1 is the wired DualShock, player 1. A on it is "test it", not "scan".
		press(KEY_ENTER, 14);
		harness_set_pad_state(1, 0, PAD_CODES, 0, 0, 0);
		frame(8);
		dump("pads-6-tester");

		unsigned long h_idle = pt_panel_hash();
		check(!bt_pairing(), "A on a controller does not start a scan");
		check(h_idle != h_list, "it opens the controller tester instead");

		/*
		  Which is modal on both axes. move_h()'s default: drives the shelf behind the
		  panel, and every screen here that forgot to say so paged the browser from
		  inside a dialog.
		*/
		press(KEY_RIGHT, 8);
		press(KEY_DOWN, 8);
		frame(6);
		check(pt_panel_hash() == h_idle, "and nothing behind it moves");

		/*
		  There is no controller in the container, so the tester is handed the state one
		  would be in and asked to draw it. Two different buttons held must not produce
		  the same picture, or the screen is not showing what is being pressed.
		*/
		harness_set_pad_state(1, 1u << SYS_BTN_A, PAD_CODES, 0, 0, 0);
		frame(8);
		dump("pads-7-tester-a");
		unsigned long h_a = pt_panel_hash();
		check(h_a != h_idle, "a held button lights up on the tester");

		harness_set_pad_state(1, 1u << SYS_BTN_UP, PAD_CODES, 0, 0, 0);
		frame(8);
		dump("pads-8-tester-up");
		check(pt_panel_hash() != h_a, "and a different button lights something else");

		// The sticks are drawn only for a pad that has them: a SNAC pad is a digital
		// PlayStation pad, and two boxes that never move would be a question it cannot
		// answer.
		harness_set_pad_state(1, 0, PAD_CODES, 1, 100, -70);
		frame(8);
		dump("pads-9-tester-stick");
		check(pt_panel_hash() != h_idle, "a pad with sticks is drawn with them");

		/*
		  Leaving takes two presses of B, so that B itself can be pressed and seen to
		  work. One press must not leave - that is the whole point of the arming.
		*/
		harness_set_pad_state(1, 0, PAD_CODES, 0, 0, 0);
		frame(8);
		press(KEY_ESC, 10);
		frame(6);
		dump("pads-10-tester-back-armed");
		check(pt_panel_hash() != h_list, "one press of B stays in the tester");

		press(KEY_ESC, 10);
		frame(8);
		check(pt_panel_hash() == h_list, "and the second press returns to the list");

		/*
		  And the entry itself, which is what all of that was for: five controllers in the
		  fixture, so the sixth row is the one that adds one - reachable only if it is
		  there, and the only row on the screen for which A starts a scan.
		*/
		for (int i = 0; i < 5; i++) press(KEY_DOWN, 8);

		/*
		  Asked before the frame settles, not after. btctl is not in the container either,
		  so the child it forks dies at once and bt_poll() clears the flag on the very next
		  frame - "pairing mode did not start" and "pairing mode started and the child was
		  reaped" look identical a dozen frames later.
		*/
		chome_handle(KEY_ENTER);
		check(bt_pairing(), "the row below the last controller is the one that adds one");
		chome_handle(KEY_ENTER | UPSTROKE);
		frame(6);

		bt_pair_stop();
		bt_pair_ack();
		bt_force_present(-1);
		harness_clear_pad_state();
		frame(6);

		press(KEY_ESC, 10);
		press(KEY_ESC, 10);
		press(KEY_ESC, 10);
		frame(6);
	}

	/*
	  Wi-Fi. There is no radio in the container, so what is checked here is everything
	  up to the radio: reading what the tools say, building the config file, and the
	  screen a person drives. Bringing an interface up cannot be tested without an
	  interface and is verified on the hardware instead.
	*/
	printf("\n== wi-fi ==\n");
	{
		net_ap list[NET_MAX];
		int n = net_parse_scan(SCAN_TEXT, list, NET_MAX);
		printf("  parsed %d networks\n", n);
		check(n == 3, "hidden and null-padded names are left out of the list");
		check(!strcmp(list[0].ssid, "BrainDamage"), "the network we are on comes first");
		check(list[0].current, "and is marked as the current one");
		check(list[0].signal == -48, "a network on two radios is one row, at its best signal");
		check(list[0].secure, "RSN means it wants a password");
		check(!strcmp(list[1].ssid, "Neighbour 2.4"), "then the strongest of the rest");
		check(list[1].secure, "Privacy in the capability line also means a password");
		check(!strcmp(list[2].ssid, "Cafe Guest") && !list[2].secure, "an open network is not marked");

		net_link l;
		check(net_parse_link(LINK_TEXT, &l) == 1, "the link reads as connected");
		check(!strcmp(l.ssid, "BrainDamage"), "and says which network");
		check(net_parse_link("Not connected.\n", &l) == 0, "and reads not-connected as not connected");

		// The config file. Getting this wrong takes the machine off the network, so
		// the shape of it is worth pinning down.
		char conf[1024], country[32];
		check(net_conf_country("country=CH\nnetwork={\n\tssid=\"x\"\n}\n", country, sizeof(country))
			&& !strcmp(country, "CH"), "the country setting is read back out of the old file");

		/*
		  The country goes in as the code, exactly as net_conf_country() hands it
		  back, and comes out as a line wpa_supplicant will accept. Passing the two
		  through each other is the whole point: the first thing a join does is read
		  the country out of the old file and put it into the new one.
		*/
		check(net_conf_build(conf, sizeof(conf), country, "MyNet", "hunter2hunter", 1) > 0,
			"a secured network builds a config");
		check(strstr(conf, "country=CH\n") && strstr(conf, "ssid=\"MyNet\"") && strstr(conf, "psk=\"hunter2hunter\""),
			"which keeps the country as a line of its own and names the network");
		check(!strstr(conf, "\nCH"), "and not as a bare code that would not parse");
		check(net_conf_build(conf, sizeof(conf), "", "MyNet", "", 0) > 0 && strstr(conf, "key_mgmt=NONE"),
			"an open network builds one with no key");
		check(net_conf_build(conf, sizeof(conf), "", "MyNet", "short", 1) < 0,
			"a password WPA would reject is refused before anything is touched");
		check(net_conf_build(conf, sizeof(conf), "", "", "hunter2hunter", 1) < 0,
			"and so is a nameless network");
		check(net_conf_build(conf, sizeof(conf), "", "He said \"hi\"", "hunter2hunter", 1) > 0
			&& strstr(conf, "ssid=\"He said \\\"hi\\\"\""),
			"a quote in the name is escaped, not left to end the value early");

		/*
		  The screen. Fed with the same captured output, through the function the scan
		  child's result goes through, so this is the list a person would really see.
		*/
		net_ingest_scan(SCAN_TEXT);
		net_ingest_link(LINK_TEXT);

		harness_set_menu_core(1);
		chome_leave();
		press(KEY_MENU, 20);
		frame(8);

		press(KEY_UP, 10);                    // the menu bar
		press(KEY_RIGHT, 10);                 // Options
		press(KEY_ENTER, 14);
		press(KEY_UP, 8);                     // wrap to the last row
		press(KEY_UP, 8);                     // More Settings
		press(KEY_UP, 8);                     // Best Settings
		press(KEY_UP, 8);                     // and up to Wi-Fi
		press(KEY_ENTER, 14);
		frame(8);
		dump("wifi-1-list");

		// A on a secured network asks for the password - which also proves the row,
		// the screen and the keyboard are all wired to each other.
		press(KEY_ENTER, 10);
		check(osk_active(), "picking a secured network asks for its password");
		dump("wifi-2-password");
		press(KEY_ESC, 10);
		check(!osk_active(), "and B backs out of it");

		press(KEY_DOWN, 8);
		press(KEY_DOWN, 8);                   // the open one
		press(KEY_ENTER, 10);
		check(!osk_active(), "an open network does not ask for a password");

		press(KEY_ESC, 10);
		press(KEY_ESC, 10);
		frame(6);
	}

	/*
	  Waiting. The two screens where a player is left holding a button or a password and
	  has to be told that something is happening - and the shared parts that tell them.

	  What matters here and is not visible in a PNG is the *tie to real state*: the ring
	  turns because a scan is running and stops because it stopped, and the track fills
	  because the tool doing the work said so. So each of these drives the state the
	  screen reads and then reads pixels back off the screen, rather than trusting that
	  a call was made.
	*/
	printf("\n== waiting: activity and progress ==\n");
	{
		// The steps a join reports, before any of it is drawn. The child writes one
		// digit per step and this is the only thing the parent reads.
		net_force_join(JOIN_IDLE, "");
		net_ingest_join_phase("0");
		check(net_join_phase() == 0, "a join starts at its first step");

		net_ingest_join_phase("2\n");
		check(net_join_phase() == 2, "and follows the child's own report");

		net_ingest_join_phase("1\n");
		check(net_join_phase() == 2, "a step that arrives out of order does not walk it backwards");

		net_ingest_join_phase("");
		net_ingest_join_phase("x");
		net_ingest_join_phase("7");
		check(net_join_phase() == 2, "and neither does a truncated, empty or impossible one");

		check(strcmp(net_join_phase_name(0), net_join_phase_name(3)) != 0,
			"each step is named differently, or the track is the only thing moving");
		check(!strstr(net_join_phase_name(1), "ifup") && !strstr(net_join_phase_name(2), "iw"),
			"and named for what is being waited for, not for the command doing it");

		net_ingest_join_phase("9");
		check(net_join_phase() == JOIN_ROLLBACK, "a rollback is reported as itself, not as progress");

		// And starting a join clears it, or the next attempt would open where the last
		// one gave up.
		net_force_join(JOIN_WORK, "X");
		check(net_join_phase() == 0, "a new join starts with nothing behind it");
		net_force_join(JOIN_IDLE, "");

		/*
		  Getting to the screen. There is no radio in the container, so presence, a
		  running scan and a running join are all asserted - see net_force_present().

		  With presence comes the link refresher, which runs `iw` every few seconds - and
		  a container that has none would come back with nothing and read as "not
		  connected", wiping the ingested link out from under the screen every time it
		  happened to fire. So `iw` is faked into PATH answering the way it really does,
		  the same bargain the controllers section makes with bluetoothctl. The real
		  refresh path then runs, and whenever its child lands it lands on the same link.
		*/
		{
			FILE *f = fopen("/usr/local/bin/iw", "w");
			if (f)
			{
				fputs("#!/bin/sh\n"
					"if [ \"$1\" = dev ]; then\n"
					"  echo 'Connected to 74:da:88:1c:2b:aa (on wlan0)'\n"
					"  printf '\\tSSID: BrainDamage\\n'\n"
					"  printf '\\tsignal: -48 dBm\\n'\n"
					"fi\n", f);
				fclose(f);
				chmod("/usr/local/bin/iw", 0755);
			}
		}

		net_force_present(1);
		net_force_scanning(0);
		net_force_join(JOIN_IDLE, "");

		harness_set_menu_core(1);
		chome_leave();
		press(KEY_MENU, 20);
		frame(8);

		press(KEY_UP, 10);                    // the menu bar
		press(KEY_RIGHT, 10);                 // Options
		press(KEY_ENTER, 14);
		press(KEY_UP, 8);                     // wrap to the last row
		press(KEY_UP, 8);                     // More Settings
		press(KEY_UP, 8);                     // Best Settings
		press(KEY_UP, 8);                     // and up to Wi-Fi
		press(KEY_ENTER, 14);
		frame(10);

		/*
		  An empty list with a radio present. Nothing else on this screen is drawn in
		  COL_BLUE in that state - there is no row to select - so a count of it is a
		  count of the ring's head, which gfx_spinner() fills rather than blends.
		*/
		net_ingest_scan("");
		net_force_scanning(1);
		frame(12);
		dump("wifi-3-scanning-empty");

		int spin_on = panel_pixels(COL_BLUE);
		check(spin_on > 0, "a running scan is shown as a ring, not just as a sentence");

		unsigned long h1 = pt_panel_hash();
		frame(12);                            // ~190ms: more than one position of the ring
		unsigned long h2 = pt_panel_hash();
		frame(12);
		unsigned long h3 = pt_panel_hash();
		check(!(h1 == h2 && h2 == h3), "and the ring turns as the clock advances");

		/*
		  And stops when the scan does. This is the assertion that makes the animation
		  worth having: a ring that spins while a screen is open says nothing.
		*/
		net_force_scanning(0);
		frame(14);
		dump("wifi-4-scan-finished");
		check(panel_pixels(COL_BLUE) == 0, "a finished scan takes the ring away");

		unsigned long q1 = pt_panel_hash();
		frame(20);
		check(pt_panel_hash() == q1, "and an idle screen does not repaint at all");

		/*
		  The list. Its rows carry a second line saying what each network is, which is
		  what the padlock alone was asking the player to know; COL_DIM is that line and
		  nothing else on the panel uses it.
		*/
		net_ingest_scan(SCAN_TEXT);
		net_ingest_link(LINK_TEXT);
		frame(12);
		dump("wifi-5-rows");
		check(panel_pixels(COL_DIM) > 0, "every network says what it is on a line of its own");

		/*
		  A scan running over a list that is already up. The ring moves to the footer so
		  the list stays usable, and the rows must not have gone anywhere.
		*/
		net_force_scanning(1);
		frame(12);
		dump("wifi-6-scanning-more");
		check(panel_pixels(COL_DIM) > 0, "and they stay while more are being looked for");
		net_force_scanning(0);
		frame(10);

		/*
		  The join panel. The track is driven by the phase the child reported and by
		  nothing else, so more phases behind us must be more of the track filled -
		  measured as green pixels, which is the only thing on this panel drawn in it
		  while a join is running.
		*/
		net_force_join(JOIN_WORK, "HOME-WIFI");
		net_ingest_join_phase("0");
		net_ingest_join_phase("1");
		frame(12);
		dump("wifi-7-joining-early");
		int g1 = panel_pixels(COL_GREEN);
		unsigned long j1 = pt_panel_hash();

		net_ingest_join_phase("3");
		frame(12);
		dump("wifi-8-joining-late");
		int g3 = panel_pixels(COL_GREEN);

		check(g1 > 0, "a running join shows how far it has got");
		check(g3 > g1, "and the track follows the child's report rather than a timer");
		check(pt_panel_hash() != j1, "so the panel changes when the step does");

		// The sweep on the step being worked on keeps moving even though the step has
		// not: the wait for DHCP has no progress inside it, and a still screen would
		// read as a hung one.
		unsigned long s1 = pt_panel_hash();
		frame(12);
		check(pt_panel_hash() != s1, "and the step being worked on is visibly still working");

		/*
		  A failure that had to put the old network back is not step four of joining, so
		  it is drawn with none of the track filled rather than nearly all of it.
		*/
		net_force_join(JOIN_WORK, "HOME-WIFI");
		net_ingest_join_phase("9");
		frame(12);
		dump("wifi-9-rolling-back");
		check(panel_pixels(COL_GREEN) == 0, "a rollback is not drawn as progress");

		net_force_join(JOIN_IDLE, "");
		frame(8);

		press(KEY_ESC, 10);
		press(KEY_ESC, 10);
		frame(6);

		/*
		  And both reworked screens on the canvas that can least afford them. Two-line
		  rows and a status band are the changes most likely to run out of room at 240p,
		  and they are also the profile Dinofly's own set gets - so the pictures are the
		  point here, and the check is that the treatment survived rather than collapsing
		  to a panel with no rows in it. COL_DIM is the second line of a row and nothing
		  else on either panel is drawn in it.
		*/
		int was_profile = cfg.classicui_profile;

		cfg.classicui_profile = 3;
		harness_set_fb(320, 240);
		gfx_shutdown();
		theme_update(320, 240, 3);
		chome_leave();
		press(KEY_MENU, 20);
		frame(10);

		/*
		  No RIGHT here, unlike every other walk to Options above: Display drops out of
		  the menu bar at 240p (mb_visible), so Options is the first entry rather than
		  the second and one press to the right would go straight past it.
		*/
		press(KEY_UP, 10);
		press(KEY_ENTER, 14);
		press(KEY_UP, 8);
		press(KEY_UP, 8);
		press(KEY_UP, 8);
		press(KEY_UP, 8);                     // Wi-Fi
		press(KEY_ENTER, 14);
		frame(10);
		dump("wifi-10-240p");
		check(panel_pixels(COL_DIM) > 0, "the Wi-Fi rows keep their second line at 240p");

		press(KEY_ESC, 10);
		press(KEY_UP, 8);                     // Controllers
		press(KEY_ENTER, 14);
		frame(10);
		dump("pads-13-240p");
		check(panel_pixels(COL_DIM) > 0, "and so do the controller rows");

		press(KEY_ESC, 10);
		press(KEY_ESC, 10);
		frame(6);

		// Put the canvas and the forced profile back exactly as they were: the sections
		// after this one have their own 240p cases and read cfg.classicui_profile to set
		// them up, so leaving it forced makes them render at the wrong profile.
		net_force_present(-1);
		cfg.classicui_profile = was_profile;
		harness_set_fb(1280, 720);
		gfx_shutdown();
		theme_update(1280, 720, was_profile);
		chome_leave();
		press(KEY_MENU, 20);
		frame(8);
	}
	/*
	  Best Settings. This rewrites the player's own MiSTer.ini, which is a
	  hand-edited CRLF file full of their comments, so most of what is checked here is
	  about what the rewrite leaves alone rather than what it changes.

	  The fake SD card is a real directory, so the write half runs for real: the file is
	  written, read back, and the backup compared against the original bytes.
	*/
	printf("\n== recommended settings ==\n");
	{
		/*
		  A fixture shaped like the awkward parts of a real ini. Every line here is one
		  the rewriter has to get right:

		  - video_info carries a trailing note, which has to survive the value changing
		  - controller_info exists only as a comment, so it counts as absent
		  - disable_autofire is not there at all
		  - video_information is not ours, and our key is a prefix of it. It stands in
		    for the real pairs in ini_vars - video_off / video_off_logo, hdmi_cec /
		    hdmi_cec_sleep - and doubles as a key the rewriter has never heard of
		  - [NES] sets video_info again. A core section is parsed after [MiSTer] and
		    wins, so fixing only the first one would leave the pop-up on in that core
		  - the [video=] section uses a space instead of an '=', which cfg.cpp accepts
		*/
		static const char *SRC_CRLF =
			"[MiSTer]\r\n"
			"; keep the scanlines off in here\r\n"
			"video_mode=1280x720@60\r\n"
			"video_info=3            ; seconds the mode banner stays up\r\n"
			";controller_info=6\r\n"
			"video_information=1\r\n"
			"\r\n"
			"[NES]\r\n"
			"video_info=9\r\n"
			"\r\n"
			"[video=1280x720]\r\n"
			"video_info 4\r\n";

		static char out[8192], out2[8192];
		int srclen = (int)strlen(SRC_CRLF);
		int n = ini_rewrite(SRC_CRLF, srclen, out, sizeof(out));
		check(n > 0, "a file can be rewritten");
		out[n] = 0;

		// The whole point of editing in binary: a text-mode write would rewrite every
		// line ending in the file and turn the next diff into the whole file.
		int bare_lf = 0, crlf = 0;
		for (int i = 0; i < n; i++)
		{
			if (out[i] != '\n') continue;
			if (i && out[i - 1] == '\r') crlf++; else bare_lf++;
		}
		check(!bare_lf, "CRLF survives the rewrite - not one line ending was changed");
		check(crlf == 17, "and the file gained only the lines it had to");   // 12 + a 5-line block

		check(strstr(out, "video_info=0            ; seconds the mode banner stays up") != 0,
			"a value changes without disturbing the note beside it");
		check(!strstr(out, "video_info=3") && !strstr(out, "video_info=9"),
			"every assignment of the key is set, not just the first");
		check(strstr(out, "video_info 0") != 0,
			"including one written with a space instead of an '='");
		check(strstr(out, ";controller_info=6") != 0, "a commented-out line is left commented");
		check(strstr(out, "video_information=1") != 0,
			"a longer key our key is a prefix of is left alone");
		check(strstr(out, "video_mode=1280x720@60") != 0 && strstr(out, "[NES]\r\n") != 0,
			"and so is everything else in the file");

		// The two that were absent, in a section of their own - the file ends inside
		// [video=], where bare keys would have applied to that one video mode.
		check(strstr(out, "[MiSTer]\r\ncontroller_info=0\r\ndisable_autofire=1\r\n") != 0,
			"keys that appear nowhere are added under a [MiSTer] header");

		check(ini_stray_lines(SRC_CRLF, out) == 0, "no line that is not ours was touched");

		// Running it twice must be running it once. The appended block is found as a
		// real assignment on the second pass, so it is set rather than added again.
		int n2 = ini_rewrite(out, n, out2, sizeof(out2));
		out2[n2] = 0;
		check(n2 == n && !memcmp(out, out2, (size_t)n), "rewriting an already-fixed file changes nothing");

		/*
		  The mirror of the CRLF check. A file that arrives with Unix line endings has
		  to leave with them: guessing CRLF because MiSTer.ini usually is would corrupt
		  an ini somebody edited on the machine itself.
		*/
		{
			static char lfsrc[4096], lfout[8192];
			int j = 0;
			for (int i = 0; i < srclen; i++) if (SRC_CRLF[i] != '\r') lfsrc[j++] = SRC_CRLF[i];
			lfsrc[j] = 0;

			int ln = ini_rewrite(lfsrc, j, lfout, sizeof(lfout));
			lfout[ln] = 0;
			check(ln > 0 && !strchr(lfout, '\r'), "an LF file stays an LF file");
			check(strstr(lfout, "[MiSTer]\ncontroller_info=0\n") != 0,
				"and the added block follows it");
		}

		// A file with nothing in it at all - a fresh card, or an ini somebody emptied.
		{
			static char eout[2048];
			int en = ini_rewrite("", 0, eout, sizeof(eout));
			eout[en] = 0;
			check(en > 0 && strstr(eout, "[MiSTer]\r\nvideo_info=0\r\n") != 0,
				"an empty file gets the whole set");
		}

		/* ------------------------------------------------ and now the real file --- */

		char path[1024], bak[1024];
		snprintf(path, sizeof(path), "%s/MiSTer.ini", ROOT);
		snprintf(bak, sizeof(bak), "%s.bak", path);
		check(!strcmp(ini_path(), path), "the screen writes the ini the machine is using");
		check(!strcmp(ini_backup_path(), bak), "and keeps the copy beside it");
		/*
		  Not MiSTer_something.ini: cfg_get_name() scans the root for that pattern and
		  offers whatever it finds as an alternate configuration to boot from, so a
		  backup named that way would turn up in the classic menu as a fourth ini.
		*/
		check(!strstr(bak, "MiSTer_"), "under a name the alt-ini scanner will not adopt");

		put_file(path, SRC_CRLF);

		ini_change plan[INI_WANT_MAX];
		int np = ini_plan(path, plan, INI_WANT_MAX);
		check(np == 3, "all three settings are reported as needing a change");
		check(!strcmp(plan[0].had, "4") && plan[0].present,
			"the value shown is the last one in the file, which is the one in force");
		check(!plan[1].present && !plan[1].had[0], "a key that is only a comment reads as absent");

		/*
		  Whether a restart is needed is read off the set rather than asserted. Every
		  setting shipped today has a cfg field ini_apply() pokes, so the honest answer
		  is no - and a setting without one has to make it yes, which is what the second
		  half of this checks with a want that has no field.
		*/
		check(!ini_plan_restart(plan, np), "none of the shipped settings needs a restart");
		{
			ini_want startup_only = { "font", "x", "a setting only read at startup", 0, 0 };
			ini_change c;
			c.want = &startup_only;
			c.present = 0;
			c.had[0] = 0;
			check(ini_plan_restart(&c, 1) == 1, "one that is only read at startup says so");
		}

		cfg.video_info = 3;
		cfg.controller_info = 6;
		cfg.disable_autofire = 0;

		int wrote = ini_apply(path);
		printf("  ini_apply wrote %d\n", wrote);
		check(wrote == 3, "writing reports what it changed");

		static char now[8192], saved[8192];
		check(slurp_file(path, now, sizeof(now)) > 0, "the ini is still readable afterwards");
		check(slurp_file(bak, saved, sizeof(saved)) > 0, "and a backup was left");
		check(!strcmp(saved, SRC_CRLF), "the backup is the old file, byte for byte");
		check(!strcmp(now, out), "and the new one is what the rewrite said it would be");

		// The session that is already running parsed the ini before any of this was
		// true, so it has to be told as well - that is what makes "no restart" honest.
		check(cfg.video_info == 0 && cfg.controller_info == 0 && cfg.disable_autofire == 1,
			"the running firmware is updated too, not just the file");

		check(ini_plan(path, plan, INI_WANT_MAX) == 0, "nothing is left to change");
		check(ini_apply(path) == 0, "and applying again writes nothing");

		/* ------------------------------------------------------------ the screen --- */

		// Back to the unfixed file, and drive the screen the way a person reaches it.
		put_file(path, SRC_CRLF);

		harness_set_menu_core(1);
		chome_leave();
		press(KEY_MENU, 20);
		frame(8);

		press(KEY_UP, 10);                    // the menu bar
		press(KEY_RIGHT, 10);                 // Options
		press(KEY_ENTER, 14);
		press(KEY_UP, 8);                     // wrap to the last row
		press(KEY_UP, 8);                     // More Settings
		press(KEY_UP, 8);                     // Best Settings
		frame(6);
		dump("ini-1-options-row");

		press(KEY_ENTER, 14);
		frame(8);
		dump("ini-2-plan");

		// Arming says what it will do and does not do it. This is the check that would
		// fail if the screen ever wrote on the first press.
		press(KEY_ENTER, 12);
		frame(6);
		dump("ini-3-armed");
		slurp_file(path, now, sizeof(now));
		check(!strcmp(now, SRC_CRLF), "one press of A does not touch the file");

		press(KEY_ENTER, 12);
		frame(8);
		dump("ini-4-written");
		slurp_file(path, now, sizeof(now));
		check(!strcmp(now, out), "the second press writes it");
		check(ini_plan(path, plan, INI_WANT_MAX) == 0, "and the screen leaves nothing to do");

		press(KEY_ESC, 10);
		frame(6);
		dump("ini-5-all-set");

		press(KEY_ESC, 10);
		press(KEY_ESC, 10);
		frame(6);

		/*
		  And the same screen on the canvas his CRT really gets. At 240p the outcome and
		  the line that will be written cannot share a row, so the panel stacks them -
		  which is the arrangement that has to be looked at, not measured.
		*/
		put_file(path, SRC_CRLF);
		harness_set_fb(320, 240);
		gfx_shutdown();
		theme_update(320, 240, 3);

		chome_leave();
		press(KEY_MENU, 20);
		frame(12);
		// No RIGHT here, unlike the HD walk above: Display is dropped from the menu bar
		// at 240p, so Options is already the first entry.
		press(KEY_UP, 10);
		press(KEY_ENTER, 14);
		press(KEY_UP, 8);
		press(KEY_UP, 8);
		press(KEY_UP, 8);
		press(KEY_ENTER, 14);
		frame(8);
		dump("ini-6-plan-240p");
		check(gfx_w() == 320, "the panel lays out on a 240p canvas");
		check(ini_plan(path, plan, INI_WANT_MAX) == 3, "and shows the plan rather than acting");

		press(KEY_ESC, 10);
		press(KEY_ESC, 10);
		press(KEY_ESC, 10);
		frame(6);

		harness_set_fb(1280, 720);
		gfx_shutdown();
		theme_update(1280, 720, 1);
		frame(6);

		unlink(path);
		unlink(bak);
	}

	/*
	  More Settings - the screen that edits the ini rather than asserting an opinion
	  about it. Two halves: the model, which is where the clamping and the
	  default-versus-recommended distinction live, and the screen, where the write and
	  the colour coding are checked by driving it with keys and reading the pixels back.
	*/
	printf("\n== more settings ==\n");
	{
		// These are global and later screens are laid out from one of them, so whatever
		// the block pokes into cfg has to go back.
		uint8_t was_over = cfg.classicui_overscan;
		uint8_t was_rumble = cfg.rumble;
		uint8_t was_vscale = cfg.vscale_mode;
		uint8_t was_bright = cfg.video_brightness;

		/*
		  A fixture with one of each thing the model has to survive:

		  - vscale_mode holds a value that is not the default, so it starts out amber
		  - video_brightness is out of range. The firmware clamps it and carries on, so
		    the screen has to show the value the machine is really using
		  - rumble is written with a space instead of an '=', which cfg.cpp accepts
		  - hdmi_limited is not a number at all
		  - controller_info is set twice and the [NES] one is later, so that is the one
		    in force - the same rule ini_plan() follows
		  - classicui_overscan is absent, which has to read as its default rather than 0
		*/
		static const char *SRC =
			"[MiSTer]\r\n"
			"; my own notes, which have to survive all of this\r\n"
			"vscale_mode=1\r\n"
			"video_brightness=250\r\n"
			"rumble 0\r\n"
			"hdmi_limited=yes\r\n"
			"controller_info=6\r\n"
			"video_contrast=50\r\n"
			"\r\n"
			"[NES]\r\n"
			"controller_info=0\r\n";

		// Everything at the value this front-end recommends: the two keys whose
		// recommendation is not the machine's default, and nothing else.
		static const char *CLEAN =
			"[MiSTer]\r\n"
			"disable_autofire=1\r\n"
			"controller_info=0\r\n";

		char path[1024], bak[1024];
		snprintf(path, sizeof(path), "%s/MiSTer.ini", ROOT);
		snprintf(bak, sizeof(bak), "%s.bak", path);
		put_file(path, SRC);

		/* ------------------------------------------------------------- the table --- */

		int bad_range = 0, no_text = 0, bad_choice = 0;
		for (int i = 0; i < opt_count(); i++)
		{
			const opt_def *o = opt_at(i);
			if (o->def < o->lo || o->def > o->hi) bad_range++;
			if (o->rec < o->lo || o->rec > o->hi) bad_range++;
			if (o->kind == OPT_NUMBER && o->step < 1) bad_range++;
			if (!o->key[0] || !o->label[0] || !o->help[0]) no_text++;
			for (int c = 0; c < o->nchoices; c++)
				if (o->choices[c].val < o->lo || o->choices[c].val > o->hi) bad_choice++;
		}
		check(!bad_range, "every default and step is inside the range cfg.cpp declares");
		check(!bad_choice, "and so is every value the player can pick");
		check(!no_text, "every option has a label and a sentence explaining it");

		/*
		  The two tables have to agree. Best Settings writes disable_autofire=1 without
		  being asked; if this screen thought 0 was the value to recommend it would paint
		  that result amber and offer to undo it on the next screen along.
		*/
		int disagree = 0;
		for (int w = 0; w < ini_want_count(); w++)
		{
			const ini_want *wt = ini_want_at(w);
			int i = opt_find(wt->key);
			if (i >= 0 && opt_at(i)->rec != atoi(wt->value)) disagree++;
		}
		check(!disagree, "an option in both tables recommends what Best Settings writes");

		/* ------------------------------------------------------------- the model --- */

		opt_load(path);

		int i_size = opt_find("vscale_mode");
		int i_bri  = opt_find("video_brightness");
		int i_con  = opt_find("video_contrast");
		int i_rum  = opt_find("rumble");
		int i_blk  = opt_find("hdmi_limited");
		int i_pop  = opt_find("controller_info");
		int i_ovr  = opt_find("classicui_overscan");
		check(i_size >= 0 && i_bri >= 0 && i_con >= 0 && i_rum >= 0 && i_blk >= 0
			&& i_pop >= 0 && i_ovr >= 0, "the set holds the options this fixture is about");

		check(opt_value(i_size) == 1 && opt_present(i_size), "a value in the file is read from it");
		check(opt_value(i_ovr) == 6 && !opt_present(i_ovr), "a key that is absent reads as its default");
		check(opt_value(i_bri) == 100, "one out of range reads as the value the firmware will use");
		check(opt_value(i_rum) == 0, "a key written with a space instead of an '=' still counts");
		check(opt_value(i_blk) == 0 && !opt_present(i_blk), "and one that is not a number reads as absent");
		check(opt_value(i_pop) == 0, "where a key is set twice, the later one is what is shown");

		// What the colour is made of. controller_info is the interesting one: 0 is not
		// the machine's default, and it is still not flagged, because it is what this
		// front-end recommends - which is the distinction the whole table exists for.
		check(!opt_is_rec(i_size), "a value away from the recommended one is flagged");
		check(opt_is_rec(i_con), "and one at it is not");
		check(opt_at(i_pop)->def != opt_at(i_pop)->rec && opt_is_rec(i_pop),
			"a recommendation that is not the machine's default is honoured as the recommendation");

		check(opt_set(i_ovr, 99) == 15 && opt_value(i_ovr) == 15, "a value above the range is clamped to it");
		check(opt_set(i_ovr, -3) == 0, "and one below it");

		opt_set(i_bri, 100);
		check(!opt_step_by(i_bri, 1), "a number at the top of its range does not move");
		check(opt_step_by(i_bri, -1) && opt_value(i_bri) == 95, "and steps back down by its own step");
		opt_set(i_bri, 2);
		check(opt_step_by(i_bri, -1) && opt_value(i_bri) == 0, "a step that would go under the range stops at it");

		opt_set(i_blk, 2);
		check(opt_step_by(i_blk, 1) && opt_value(i_blk) == 0, "a list wraps round rather than stopping");
		opt_set(i_pop, 3);
		check(opt_step_by(i_pop, 1) && opt_value(i_pop) == 0,
			"and stepping off a value no choice claims lands on one that is");

		char vb[24];
		opt_set(i_ovr, 8);
		check(!strcmp(opt_value_text(i_ovr, vb, sizeof(vb)), "8%"), "a number reads with its unit");
		opt_set(i_pop, 3);
		check(!strcmp(opt_value_text(i_pop, vb, sizeof(vb)), "3"),
			"and a value no name covers reads as itself rather than being rounded to one");

		check(opt_reset(i_size) && opt_value(i_size) == 0 && !opt_reset(i_size),
			"a reset puts an option back to the recommended value, once");

		{
			int vw[OPT_MAX];
			int nall = opt_view(vw, OPT_MAX, 1);
			int nana = opt_view(vw, OPT_MAX, 0);
			check(nall == opt_count() && nana < nall,
				"the scaler-only options are dropped when the scaler is not what reaches the screen");

			int leaked = 0;
			for (int i = 0; i < nana; i++) if (opt_at(vw[i])->scaler_only) leaked++;
			check(!leaked, "and none of them is left in the list");
		}

		/* ------------------------------------------------------------- the write --- */

		opt_load(path);
		opt_set(i_ovr, 9);
		opt_step_by(i_rum, 1);
		check(opt_dirty() == 2, "two edits, two things to write");

		check(opt_apply(path) == 2, "and writing reports both");

		static char now[8192], saved[8192];
		check(slurp_file(path, now, sizeof(now)) > 0, "the ini is still readable afterwards");
		check(slurp_file(bak, saved, sizeof(saved)) > 0 && !strcmp(saved, SRC),
			"the backup is the old file, byte for byte");

		int bare_lf = 0;
		for (int i = 0; now[i]; i++) if (now[i] == '\n' && (!i || now[i - 1] != '\r')) bare_lf++;
		check(!bare_lf, "CRLF survives the write - it is done in binary, like everything here");

		check(strstr(now, "rumble 1") != 0, "a value changes without disturbing how the line was written");
		check(strstr(now, "[MiSTer]\r\nclassicui_overscan=9\r\n") != 0,
			"a key that was absent is added under a header of its own");
		check(strstr(now, "vscale_mode=1") != 0 && strstr(now, "video_brightness=250") != 0,
			"an option nobody touched is left exactly as it was, out of range and all");
		check(strstr(now, "; my own notes") != 0 && ini_stray_lines(SRC, now) == 0,
			"and the rest of the player's file is still their file");

		check(cfg.classicui_overscan == 9 && cfg.rumble == 1, "the running firmware is told as well");
		check(opt_wrote_live(), "both of those are true for this session already, and the screen says so");
		check(!opt_dirty() && opt_apply(path) == 0, "and there is nothing left to write");

		/* ------------------------------------------------------------ the screen --- */

		harness_set_menu_core(1);
		chome_leave();
		press(KEY_MENU, 20);
		frame(8);

		press(KEY_UP, 10);                    // the menu bar
		press(KEY_RIGHT, 10);                 // Options
		press(KEY_ENTER, 14);
		press(KEY_UP, 8);                     // wrap to the last row
		press(KEY_UP, 8);                     // More Settings
		frame(6);
		dump("set-1-options-row");

		press(KEY_ENTER, 14);
		frame(8);
		dump("set-2-list");
		check(panel_rows_pixels(COL_YELLOW) > 0, "the screen marks the values that are not the recommended ones");

		/*
		  And the other half of that, which is the check that would pass on any screen
		  that simply drew everything amber: a file already at the recommended values has
		  none of it, and one press of right puts some there.
		*/
		press(KEY_ESC, 10);
		press(KEY_ESC, 10);
		press(KEY_ESC, 10);
		put_file(path, CLEAN);

		press(KEY_UP, 10);
		press(KEY_RIGHT, 10);
		press(KEY_ENTER, 14);
		press(KEY_UP, 8);
		press(KEY_UP, 8);
		press(KEY_ENTER, 14);
		frame(8);
		dump("set-3-all-default");
		check(panel_rows_pixels(COL_YELLOW) == 0, "a file already at those values has no amber on it at all");

		press(KEY_RIGHT, 10);
		frame(8);
		dump("set-4-changed");
		check(panel_rows_pixels(COL_YELLOW) > 0, "and moving one value colours it");
		check(slurp_file(path, now, sizeof(now)) > 0 && !strcmp(now, CLEAN),
			"changing a value on screen does not write anything on its own");

		// Up from the first row wraps to Save Changes, which is the last one.
		press(KEY_UP, 8);
		press(KEY_ENTER, 12);
		frame(6);
		dump("set-5-save-armed");
		check(slurp_file(path, now, sizeof(now)) > 0 && !strcmp(now, CLEAN),
			"one press of A does not touch the file either");

		press(KEY_ENTER, 12);
		frame(8);
		dump("set-6-saved");
		check(slurp_file(path, now, sizeof(now)) > 0 && strstr(now, "vscale_mode=1") != 0,
			"the second press writes it");
		check(strstr(now, "disable_autofire=1") != 0 && !strstr(now, "video_brightness"),
			"and writes only what was changed, not the whole table");
		check(panel_rows_pixels(COL_YELLOW) > 0,
			"the colour is about the default, not about being unsaved, so it stays");

		static char written[8192];
		snprintf(written, sizeof(written), "%s", now);

		/*
		  Leaving with edits that were never written throws them away, so it asks first.
		  Read off the panel, because there is nothing else to ask: staying put and
		  leaving look identical from outside until the panel is gone.
		*/
		press(KEY_DOWN, 8);                   // back round to the first setting
		press(KEY_RIGHT, 10);
		frame(6);
		unsigned long h_edit = panel_hash();

		press(KEY_ESC, 10);
		frame(6);
		dump("set-7-discard-armed");
		check(panel_hash() != h_edit, "B with unsaved changes says something rather than just leaving");

		press(KEY_ESC, 10);
		frame(8);
		check(panel_hash() != h_edit, "and the second press leaves");
		check(slurp_file(path, now, sizeof(now)) > 0 && !strcmp(now, written),
			"the edits it threw away were not written");

		press(KEY_ENTER, 14);                 // straight back in, the cursor is still there
		frame(8);
		check(opt_value(i_size) == 1 && !opt_dirty(),
			"and re-opening the screen shows the file rather than the abandoned edits");

		/*
		  The canvas his CRT really gets. Twelve rows do not fit in the panel at 240p, so
		  this is where the list has to scroll - and where a cursor that walked off the
		  bottom would show up as a panel that stops changing.
		*/
		press(KEY_ESC, 10);
		press(KEY_ESC, 10);
		press(KEY_ESC, 10);

		harness_set_fb(320, 240);
		gfx_shutdown();
		theme_update(320, 240, 3);

		chome_leave();
		press(KEY_MENU, 20);
		frame(12);
		press(KEY_UP, 10);                    // Display is dropped at 240p: Options is first
		press(KEY_ENTER, 14);
		press(KEY_UP, 8);
		press(KEY_UP, 8);
		press(KEY_ENTER, 14);
		frame(8);
		dump("set-8-240p");
		check(gfx_w() == 320, "the list lays out on a 240p canvas");

		unsigned long h_top = panel_hash();
		press(KEY_UP, 8);                     // wrap to the last row, which is off the bottom
		frame(6);
		dump("set-9-240p-scrolled");
		check(panel_hash() != h_top, "a list too long for the panel scrolls to the cursor");

		press(KEY_ESC, 10);
		press(KEY_ESC, 10);
		press(KEY_ESC, 10);

		harness_set_fb(1280, 720);
		gfx_shutdown();
		theme_update(1280, 720, 1);
		frame(6);

		cfg.classicui_overscan = was_over;
		cfg.rumble = was_rumble;
		cfg.vscale_mode = was_vscale;
		cfg.video_brightness = was_bright;
		theme_invalidate();
		theme_update(1280, 720, 1);

		unlink(path);
		unlink(bak);
	}

	/*
	  The icons. Two things worth failing over: a system that has lost its icon
	  because an id was renamed on one side and not the other - which shows up on
	  screen as a system quietly falling back to the generic art - and any icon that
	  came out empty or nearly solid, which is what a bad source file or a bad
	  threshold looks like.
	*/
	printf("\n== system icons ==\n");
	{
		int missing = 0;
		for (int i = 0; i < lib_sys_count(); i++)
		{
			const chome_sys *sy = lib_sys(i);
			if (!sy) continue;

			int found = 0;
			for (size_t k = 0; k < sizeof(sysicons) / sizeof(sysicons[0]); k++)
				if (!strcasecmp(sysicons[k].id, sy->id)) { found = 1; break; }

			if (!found) { printf("  no icon for system \"%s\"\n", sy->id); missing++; }
		}
		check(!missing, "every system in the table has an icon");

		int bad = 0;
		for (size_t k = 0; k < sizeof(sysicons) / sizeof(sysicons[0]); k++)
		{
			int ink = 0;
			for (int r = 0; r < ICON_SYS; r++)
				for (const char *q = sysicons[k].rows[r]; *q; q++) if (*q == '#') ink++;

			/*
			  A recognisable silhouette covers somewhere between a twelfth and four
			  fifths of its grid. Expressed as a fraction rather than a pixel count,
			  because the count moves with ICON_SYS - raising it from 32 to 64 broke
			  this check while the icons themselves were fine.
			*/
			int cells = ICON_SYS * ICON_SYS;
			if (ink < cells / 12 || ink > (cells * 4) / 5)
			{
				printf("  %s: %d px of ink in %d\n", sysicons[k].id, ink, cells);
				bad++;
			}
		}
		check(!bad, "and every icon has a plausible amount of ink in it");

		/*
		  The favourite badge, which is the one pictogram that appears over artwork
		  rather than on a panel. Toggled on, photographed, toggled back off so the
		  rest of the run sees the library it expects.
		*/
		{
			harness_set_menu_core(1);
			harness_set_fb(1280, 720);
			gfx_shutdown();
			theme_update(1280, 720, 1);
			chome_leave();
			press(KEY_MENU, 20);
			frame(10);
			press(KEY_BACKSPACE, 12);
			dump("icons-favourite");
			press(KEY_BACKSPACE, 12);
		}

		// A sheet of the lot, to be looked at: the only real test of an icon is
		// whether a person recognises the machine.
		{
			harness_set_fb(1280, 720);
			gfx_shutdown();
			theme_update(1280, 720, 1);
			if (gfx_begin())
			{
				gfx_fill(0, 0, 1280, 720, COL_BGDARK);
				int n = (int)(sizeof(sysicons) / sizeof(sysicons[0]));
				int cols = 8, zoom = 3, cell = ICON_SYS * zoom + 24;
				for (int k = 0; k < n; k++)
				{
					int cx = 20 + (k % cols) * cell, cy = 20 + (k / cols) * cell;
					for (int oy = 0; oy < ICON_SYS * zoom; oy++)
					{
						const char *row = sysicons[k].rows[oy / zoom];
						for (int ox = 0; ox < ICON_SYS * zoom; ox++)
							if (row[ox / zoom] == '#')
								gfx_fill(cx + ox, cy + oy, 1, 1, COL_WHITE);
					}
					gfx_text(sysicons[k].id, cx, cy + ICON_SYS * zoom + 2, 1, COL_DIM, 0);
				}
				gfx_end();
				dump("icons-sheet");
			}
		}
	}

	/*
	  The menu button in the menu core. It used to hand straight off to the classic OSD,
	  so a player browsing the shelf who pressed it landed in MiSTer's own menu - the one
	  thing this front-end exists to keep out of their way. It opens our menu bar now.
	*/
	printf("\n== the menu button on the shelf ==\n");
	{
		harness_set_menu_core(1);
		harness_set_fb(1280, 720);
		gfx_shutdown();
		theme_update(1280, 720, 1);
		chome_leave();
		press(KEY_MENU, 20);
		frame(8);
		check(chome_active(), "the front-end is up");

		press(KEY_MENU, 16);
		check(chome_active(), "the menu button does not drop out to the classic OSD");
		dump("menubutton-1-menubar");

		press(KEY_MENU, 16);
		check(chome_active(), "and pressing it again stays with us");
		press(KEY_ESC, 10);
		frame(6);
	}

	/*
	  A contact sheet of the button glyphs, at the size they are drawn and magnified, so
	  the shapes can be judged rather than guessed at.
	*/
	printf("\n== button glyphs ==\n");
	{
		struct { const char *name; uint32_t col; } tint[] =
		{
			{ "psx_triangle", COL_BTN_TRIANGLE }, { "psx_circle", COL_BTN_CIRCLE },
			{ "psx_square",   COL_BTN_SQUARE   }, { "psx_cross",  COL_BTN_CROSS  },
			{ "btn_a", COL_SNES_A }, { "btn_b", COL_SNES_B },
			{ "btn_x", COL_SNES_X }, { "btn_y", COL_SNES_Y },
			{ "dpad_up", COL_WHITE }, { "dpad_down", COL_WHITE }, { "dpad_lr", COL_WHITE },
			{ "btn_start", COL_WHITE }, { "btn_select", COL_WHITE },
		};
		int n = (int)(sizeof(tint) / sizeof(tint[0]));
		check(n == (int)(sizeof(btn12s) / sizeof(btn12s[0])), "every glyph is on the sheet");

		harness_set_fb(1280, 720);
		gfx_shutdown();
		theme_update(1280, 720, 1);
		if (gfx_begin())
		{
			gfx_fill(0, 0, 1280, 720, COL_BGDARK);
			gfx_text("BUTTON GLYPHS - 1x (240p), 2x, 3x, 4x", 30, 22, 2, COL_INK, 0);

			for (int k = 0; k < n; k++)
			{
				const btn12_def *d = 0;
				for (size_t i = 0; i < sizeof(btn12s) / sizeof(btn12s[0]); i++)
					if (!strcmp(btn12s[i].name, tint[k].name)) d = &btn12s[i];
				if (!d) { check(0, tint[k].name); continue; }

				int w = (int)strlen(d->rows[0]);
				int x0 = 40 + (k / 7) * 600, y0 = 66 + (k % 7) * 90;
				gfx_text(tint[k].name, x0, y0, 2, COL_DIM, 0);

				int x = x0 + 170;
				for (int zoom = 1; zoom <= 4; zoom++)
				{
					int y = y0 + (54 - BTN12 * zoom) / 2;
					for (int gy = 0; gy < BTN12; gy++)
						for (int gx = 0; gx < w; gx++)
						{
							char c = d->rows[gy][gx];
							if (c == '.') continue;
							gfx_fill(x + gx * zoom, y + gy * zoom, zoom, zoom,
								(c == 'c') ? tint[k].col : COL_BTN_CHIP);
						}
					x += w * zoom + 22;
				}
			}
			gfx_end();
			dump("buttons-sheet");
		}
	}

	/*
	  The three lettered palettes side by side. There is one set of four letter grids and
	  three ways to colour it, so this is the only place the difference between a Nintendo
	  pad and an Xbox one can be seen at all - by eye, since a hash cannot say "that green
	  is on the wrong button".
	*/
	printf("\n== lettered palettes ==\n");
	{
		struct { const char *name; uint32_t col[4]; } pal[] =
		{
			{ "SNES  A red   B amber X blue Y green",
				{ COL_SNES_A, COL_SNES_B, COL_SNES_X, COL_SNES_Y } },
			{ "XBOX  A green B red   X blue Y amber",
				{ COL_XBOX_A, COL_XBOX_B, COL_XBOX_X, COL_XBOX_Y } },
			{ "PLAIN an unrecognised pad - no colour is invented",
				{ COL_BTN_PLAIN, COL_BTN_PLAIN, COL_BTN_PLAIN, COL_BTN_PLAIN } },
		};

		// The two lettered families must not end up with the same colour on the same
		// letter, or the whole Xbox set is a no-op the legend hashes below cannot catch.
		check(COL_XBOX_A != COL_SNES_A && COL_XBOX_B != COL_SNES_B
			&& COL_XBOX_Y != COL_SNES_Y, "Xbox and Nintendo letters are coloured apart");

		static const char *letters[4] = { "btn_a", "btn_b", "btn_x", "btn_y" };

		harness_set_fb(1280, 720);
		gfx_shutdown();
		theme_update(1280, 720, 1);
		if (gfx_begin())
		{
			gfx_fill(0, 0, 1280, 720, COL_BGDARK);
			gfx_text("LETTERED PALETTES - 1x (240p), 2x, 4x", 30, 22, 2, COL_WHITE, 0);

			for (int p = 0; p < 3; p++)
			{
				int y0 = 70 + p * 190;
				gfx_text(pal[p].name, 40, y0, 2, COL_DIM, 0);

				for (int k = 0; k < 4; k++)
				{
					const btn12_def *d = 0;
					for (size_t i = 0; i < sizeof(btn12s) / sizeof(btn12s[0]); i++)
						if (!strcmp(btn12s[i].name, letters[k])) d = &btn12s[i];
					if (!d) { check(0, letters[k]); continue; }

					int x = 40 + k * 300;
					for (int zoom = 1; zoom <= 4; zoom *= 2)
					{
						int y = y0 + 30 + (52 - BTN12 * zoom) / 2;
						for (int gy = 0; gy < BTN12; gy++)
							for (int gx = 0; gx < BTN12; gx++)
							{
								char c = d->rows[gy][gx];
								if (c == '.') continue;
								gfx_fill(x + gx * zoom, y + gy * zoom, zoom, zoom,
									(c == 'c') ? pal[p].col[k] : COL_BTN_CHIP);
							}
						x += BTN12 * zoom + 24;
					}
				}
			}
			gfx_end();
			dump("buttons-palettes");
		}
	}

	/*
	  Prompts that follow the controller. A PlayStation pad has no A or B written on
	  it, so the legend names the shapes instead. There is no way to read the legend
	  back out of the front-end, so this is checked the way a person would: the same
	  screen photographed with each controller, and the pictures have to differ.
	*/
	printf("\n== controller prompts ==\n");
	{
		harness_set_menu_core(1);
		harness_set_fb(1280, 720);
		gfx_shutdown();
		theme_update(1280, 720, 1);
		harness_set_input_pad(1);
		chome_leave();

		harness_set_pad_name("Generic USB Gamepad");
		press(KEY_MENU, 20);
		frame(10);

		/*
		  Park on a game rather than trusting whatever the last section left: a folder
		  gets a different legend, and Down on one only nudges - so the Down/Up used
		  below to force a redraw would land on the menu bar instead of coming back
		  here, and the hashes would be of two different screens.
		*/
		for (int i = 0; i < 30; i++) press(KEY_LEFT, 2);
		int lead = 0;
		while (lead < lib_view_count() && lib_view_entry(lead)->kind != ENT_GAME) lead++;
		for (int i = 0; i < lead; i++) press(KEY_RIGHT, 6);
		frame(10);

		dump("prompts-1-generic");
		unsigned long generic = harness_fb_hash(660, 720);

		harness_set_pad_name("MiSTer SNAC Pad 1");
		press(KEY_DOWN, 10);                  // any key, so the legend is redrawn
		press(KEY_UP, 10);
		frame(10);
		dump("prompts-2-psx");
		unsigned long psx = harness_fb_hash(660, 720);
		check(psx != generic, "a SNAC pad changes the prompts");

		/*
		  All four sets, plus the fallback, have to be told apart on the screen - a layout
		  that resolves correctly and then draws the same legend as another one is no use to
		  the player. Same shelf, same selection, five controllers, five different pictures.

		  The Nintendo and Xbox pads are the pair that matters: they share one set of letter
		  grids and differ only in which letter is which colour, so if the palettes were ever
		  collapsed into one these two hashes would be equal and nothing else here would say
		  so. "Generic USB Gamepad" is the fallback on purpose - it names a brand and not a
		  layout, which is exactly the case pad_layout() must decline to guess at.
		*/
		harness_set_pad_name("Nintendo Switch Pro Controller");
		press(KEY_DOWN, 10);
		press(KEY_UP, 10);
		frame(10);
		dump("prompts-6-snes");
		unsigned long snes = harness_fb_hash(660, 720);

		harness_set_pad_name("Microsoft X-Box 360 pad");
		press(KEY_DOWN, 10);
		press(KEY_UP, 10);
		frame(10);
		dump("prompts-7-xbox");
		unsigned long xbox = harness_fb_hash(660, 720);

		/*
		  Which letter sits where on an Xbox pad, checked by colour because the colour
		  follows the letter. The legend has two lettered buttons: the east one that
		  confirms, and the west one on Favourite. On an Xbox pad west is X and it is
		  blue - Y is amber and belongs at the top, nowhere in this legend.

		  This is the check that would have caught reading input.h's BTN_X and BTN_Y
		  aliases as geometry: they are BTN_NORTH and BTN_WEST, which is backwards for
		  the pad the letters come from, and it put an amber Y on Favourite.
		*/
		{
			const uint32_t *fb = harness_fb_shown();
			int w = gfx_w(), blue = 0, amber = 0;
			for (int y = 660; fb && y < 720; y++)
			{
				for (int x = 0; x < w; x++)
				{
					uint32_t c = fb[(size_t)y * w + x];
					if (c == 0xff5a8fe0u) blue++;      // COL_XBOX_X
					if (c == 0xffe8b22bu) amber++;     // COL_XBOX_Y
				}
			}
			check(blue > 0, "an Xbox pad puts a blue X on the west button");
			check(amber == 0, "and no Y on it, which belongs at the top");
		}

		/*
		  The names these pads use over Bluetooth, which is how Dinofly's are paired. Read
		  out of /var/lib/bluetooth on his own machine: a DualShock 4 announces itself as
		  "Wireless Controller" and a Switch Pro as "Pro Controller" - no maker in either,
		  which is all every other pattern has to go on. Both drew the fallback set until
		  this, so the check is that they now draw the same prompts as the pad they are.
		*/
		harness_set_pad_name("Wireless Controller");
		press(KEY_DOWN, 10);
		press(KEY_UP, 10);
		frame(10);
		check(harness_fb_hash(660, 720) == psx,
			"a DualShock 4 over Bluetooth gets PlayStation prompts");

		harness_set_pad_name("Pro Controller");
		press(KEY_DOWN, 10);
		press(KEY_UP, 10);
		frame(10);
		check(harness_fb_hash(660, 720) == snes,
			"and a Switch Pro over Bluetooth gets Nintendo ones");

		/*
		  The trap in matching that first name loosely: this one contains it and is an
		  Xbox pad. It is caught earlier, and this is here so it stays caught.
		*/
		/*
		  The identity, for the pads no name pattern will ever reach. Each of these three
		  makers puts one lettering on everything it ships, so the vendor id decides it
		  outright - and the name given here is deliberately useless, because that is the
		  case this exists for.
		*/
		harness_set_pad_name("Controller");
		harness_set_pad_vidpid(0x054c0ce6);            // Sony DualSense
		press(KEY_DOWN, 10);
		press(KEY_UP, 10);
		frame(10);
		check(harness_fb_hash(660, 720) == psx, "a Sony vendor id means PlayStation prompts");

		harness_set_pad_vidpid(0x057e2009);            // Nintendo Switch Pro
		press(KEY_DOWN, 10);
		press(KEY_UP, 10);
		frame(10);
		check(harness_fb_hash(660, 720) == snes, "a Nintendo one means Nintendo prompts");

		harness_set_pad_vidpid(0x045e02ea);            // Microsoft Xbox One S
		press(KEY_DOWN, 10);
		press(KEY_UP, 10);
		frame(10);
		check(harness_fb_hash(660, 720) == xbox, "and a Microsoft one means Xbox prompts");

		harness_set_pad_vidpid(0);                     // back to deciding by name

		harness_set_pad_name("Xbox Wireless Controller");
		press(KEY_DOWN, 10);
		press(KEY_UP, 10);
		frame(10);
		check(harness_fb_hash(660, 720) == xbox,
			"while an Xbox Wireless Controller is still an Xbox pad");


		harness_set_input_pad(0);
		press(KEY_DOWN, 10);
		press(KEY_UP, 10);
		frame(10);
		dump("prompts-8-keyboard");
		unsigned long kbd = harness_fb_hash(660, 720);
		harness_set_input_pad(1);

		check(snes != psx,     "a Nintendo pad is not given PlayStation shapes");
		check(xbox != snes,    "an Xbox pad is not given a Nintendo pad's colours");
		check(xbox != psx,     "an Xbox pad is not given PlayStation shapes");
		check(generic != snes, "an unrecognised pad is not guessed to be a Nintendo one");
		check(generic != xbox, "an unrecognised pad is not guessed to be an Xbox one");
		check(kbd != psx && kbd != snes && kbd != xbox && kbd != generic,
			"a keyboard gets named keys rather than any pad's buttons");

		/*
		  And at 240p, which is the size that matters: the legend draws each button at
		  its native twelve pixels there, one framebuffer pixel per glyph pixel, so what
		  reaches the TV is exactly the grid in chome_btn12.h.
		*/
		// Named rather than inherited from the block above, which leaves an Xbox pad in hand.
		harness_set_pad_name("MiSTer SNAC Pad 1");
		harness_set_fb(320, 240);
		gfx_shutdown();
		theme_update(320, 240, 3);
		chome_leave();
		press(KEY_MENU, 20);              // re-enter, or the canvas change draws nothing
		frame(12);
		dump("prompts-4-psx-240p");

		// The same 240p legend on each lettered pad, which is the other half of the pair:
		// lettered buttons drawn the same way, in a Super Famicom's colours and then an
		// Xbox's. Two dumps rather than one because the difference is only colour, and
		// colour at 240p on a CRT is the thing that has to be judged by eye.
		harness_set_pad_name("SNES Controller");
		press(KEY_DOWN, 10);
		press(KEY_UP, 10);
		frame(12);
		dump("prompts-5-letters-240p");
		check(harness_fb_hash(0, 240) != 0, "the lettered 240p legend drew something");

		harness_set_pad_name("Xbox Wireless Controller");
		press(KEY_DOWN, 10);
		press(KEY_UP, 10);
		frame(12);
		dump("prompts-9-xbox-240p");

		harness_set_fb(1280, 720);
		gfx_shutdown();
		theme_update(1280, 720, 1);
		frame(10);

		// ...and back again when they pick the other controller up.
		harness_set_pad_name("Generic USB Gamepad");
		press(KEY_DOWN, 10);
		press(KEY_UP, 10);
		frame(10);
		check(harness_fb_hash(660, 720) == generic, "and another controller changes them back");

		/*
		  A remapped pad. Circle and cross swapped over means the confirm prompt has
		  to swap with it - the shape is looked up, not assumed.
		*/
		harness_set_pad_name("MiSTer SNAC Pad 1");
		press(KEY_DOWN, 10);
		press(KEY_UP, 10);
		frame(10);
		unsigned long before = harness_fb_hash(660, 720);
		harness_swap_pad_faces();
		press(KEY_DOWN, 10);
		press(KEY_UP, 10);
		frame(10);
		dump("prompts-3-remapped");
		check(harness_fb_hash(660, 720) != before, "a remapped pad is described as remapped");

		harness_swap_pad_faces();

		/*
		  The same again on a lettered pad: a prompt follows the pad's own map there too, so
		  moving which code SYS_BTN_A is bound to has to move the letter with it.
		*/
		harness_set_pad_name("Microsoft X-Box 360 pad");
		press(KEY_DOWN, 10);
		press(KEY_UP, 10);
		frame(10);
		unsigned long xbox_before = harness_fb_hash(660, 720);
		unsigned long xbox_east = legend_shape_box(0, 660, gfx_w() / 3, 720);
		harness_swap_pad_faces();
		press(KEY_DOWN, 10);
		press(KEY_UP, 10);
		frame(10);
		dump("prompts-10-xbox-remapped");
		check(harness_fb_hash(660, 720) != xbox_before,
			"a lettered prompt follows the pad's own button map");

		/*
		  And here is the Xbox layout itself, rather than just its colours.

		  A code is read as a position on the pad and then through that layout's diamond, so
		  the *east* button - which is what MiSTer's default map confirms with - is B on an
		  Xbox pad and A on a Nintendo one. That difference is a different letter, not a
		  different colour, and harness_fb_hash() cannot tell those apart; legend_shape()
		  ignores colour, so these two checks are about the diamond and nothing else.

		  Read as the code's legacy letter name instead - the reading this replaced - both
		  pads would draw B on east, so the first check would pass wrongly by drawing the
		  Xbox letter on a Nintendo pad, and the second would fail.

		  Bounded to the left third, which is the confirm prompt. The claim is about one
		  button, so it is checked on one button: across the whole legend these two now
		  differ for an unrelated and correct reason - Favourite is the west button, which
		  is Y on a Nintendo pad and X on an Xbox one.
		*/
		unsigned long xbox_south = legend_shape_box(0, 660, gfx_w() / 3, 720);   // A/B swapped: now south
		harness_swap_pad_faces();

		harness_set_pad_name("Nintendo Switch Pro Controller");
		press(KEY_DOWN, 10);
		press(KEY_UP, 10);
		frame(10);
		unsigned long snes_east = legend_shape_box(0, 660, gfx_w() / 3, 720);

		check(snes_east != xbox_east,
			"the east button is a different letter on a Nintendo pad than on an Xbox one");
		check(snes_east == xbox_south,
			"and Nintendo's east letter is the one Xbox prints on south - the diamond is swapped");

		harness_set_pad_name("Generic USB Gamepad");
		press(KEY_ESC, 10);
		frame(6);
	}

	printf("\n== presents ==\n");
	printf("  page flips: %d\n", harness_present_count());
	check(harness_present_count() > 10, "the framebuffer was actually flipped");

	printf("\n%d checks, %d failures\n", checks, fails);
	printf("PNGs in %s\n", OUT);
	return fails ? 1 : 0;
}
