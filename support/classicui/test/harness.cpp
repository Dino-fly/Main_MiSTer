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
#include "../chome_art.h"
#include "../chome_theme.h"
#include "../chome_gfx.h"
#include "../chome_video.h"
#include "../chome_osk.h"
#include "../chome_net.h"
#include "../chome_bt.h"
#include "../chome_icons32.h"
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

	press(KEY_DOWN, 25);
	snprintf(name, sizeof(name), "%s-3-suspend", tag);
	dump(name);

	press(KEY_TAB, 8);                 // arm delete: should show the red prompt
	snprintf(name, sizeof(name), "%s-4-suspend-delete-armed", tag);
	dump(name);
	press(KEY_ESC, 10);

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
	press(KEY_ESC, 16);

	harness_set_opt("V", 0);
	harness_reset_status();
	press(KEY_MENU, 20);
	check(harness_pulses_on("S") == 1, "opening freezes the game with a save");
	press(KEY_ESC, 16);
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
	press(KEY_ENTER, 12);                     // A loads the slot
	printf("  after load: pulsed=%s\n", harness_last_pulse_opt());
	check(harness_pulses_on("T") == 1, "loading pulses the core's restore bit");
	check(!chome_ingame_active(), "loading drops straight back into the game");

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
	press(KEY_ENTER, 12);
	check(!chome_ingame_active(), "A on the running game resumes it");
	check(harness_last_launch()[0] == 0, "and does not relaunch the core");
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
		frame(8);
		struct stat st;
		check(stat("/tmp/classicui_current", &st) != 0,
			"a launch record naming another core is dropped, not believed");
		press(KEY_ESC, 10);

		// Leave it in a game, as this section found it, and put back a record that does
		// match the running core - what follows needs a running game to put away.
		FILE *g = fopen("/tmp/classicui_current", "wt");
		if (g) { fprintf(g, "gb\nTetris (World).gb\nGameboy\n"); fclose(g); }
	}

	harness_set_fb_supported(0);
	harness_reset_status();
	harness_clear_launch();
	check(chome_handle(KEY_MENU) == 1, "a core without a framebuffer does not fall back to the OSD");
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

		unlink(ROOT "/savestates/Gameboy/Tetris (World)_4.ss");
	}
	harness_set_confstr(1);

	// A core with no savestate or pause entries is left completely alone.
	harness_set_confstr(0);
	press(KEY_MENU, 20);
	check(harness_pause_val() == 0, "a core with no pause entry keeps running");
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
	assert_ingame();
	assert_input_labels();
	assert_overscan();

	// Display must vanish entirely when the scaler output is not what is on screen.
	printf("\n== analog output ==\n");
	{
		// Back to automatic: an earlier section pinned the profile to HD, and a
		// forced profile deliberately outranks the canvas, which would mask what
		// this section is about.
		cfg.classicui_profile = 0;

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

		harness_set_scaler_visible(1);
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

		bt_pair_ack();
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
			for (int r = 0; r < ICON32; r++)
				for (const char *q = sysicons[k].rows[r]; *q; q++) if (*q == '#') ink++;

			// A recognisable silhouette in a 1024-pixel grid is somewhere in the
			// middle. All ink or almost none means the reduction went wrong.
			if (ink < 80 || ink > 850) { printf("  %s: %d px of ink\n", sysicons[k].id, ink); bad++; }
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
				int cols = 8, zoom = 3, cell = ICON32 * zoom + 24;
				for (int k = 0; k < n; k++)
				{
					int cx = 20 + (k % cols) * cell, cy = 20 + (k / cols) * cell;
					for (int oy = 0; oy < ICON32 * zoom; oy++)
					{
						const char *row = sysicons[k].rows[oy / zoom];
						for (int ox = 0; ox < ICON32 * zoom; ox++)
							if (row[ox / zoom] == '#')
								gfx_fill(cx + ox, cy + oy, 1, 1, COL_WHITE);
					}
					gfx_text(sysicons[k].id, cx, cy + ICON32 * zoom + 2, 1, COL_DIM, 0);
				}
				gfx_end();
				dump("icons-sheet");
			}
		}
	}

	/*
	  Prompts that follow the controller. A PlayStation pad has no A or B written on
	  it, so the legend names the shapes instead. There is no way to read the legend
	  back out of the front-end, so this is checked the way a person would: the same
	  screen photographed with each controller, and the pictures have to differ.
	*/
	printf("\n== playstation prompts ==\n");
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
		dump("prompts-1-generic");
		unsigned long generic = harness_fb_hash(660, 720);

		harness_set_pad_name("MiSTer SNAC Pad 1");
		press(KEY_DOWN, 10);                  // any key, so the legend is redrawn
		press(KEY_UP, 10);
		frame(10);
		dump("prompts-2-psx");
		unsigned long psx = harness_fb_hash(660, 720);
		check(psx != generic, "a SNAC pad changes the prompts");

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
