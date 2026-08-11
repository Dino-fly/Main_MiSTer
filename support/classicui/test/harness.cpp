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
#include <strings.h>                       // strcasecmp, for the clipped-copy allow-list
#include <dirent.h>                        // and reading our own source back, for the same
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>                        // SIGKILL/SIGINT, for the child-collection section
#include <linux/input.h>

#include "../../../cfg.h"
#include "../../../input.h"
/*
  For user_io_status_bits() and user_io_create_config_name(). The shared-config section
  reads <CORE>.CFG off the fake card and decodes one option out of it with the firmware's
  own bit parser rather than a copy of it - a promotion that wrote the right value into the
  wrong bits has to fail here, and it cannot if the test agrees with the implementation by
  construction.
*/
#include "../../../user_io.h"
// The glyph table itself, for the font section: it checks that a loaded .pf really replaces
// charfont[] and that restoring the built-in puts back every one of the 2048 bytes.
#include "../../../charrom.h"
#include "../../../snacpad.h"
#include "../chome.h"
#include "../chome_lib.h"
#include "../chome_core.h"
#include "../chome_art.h"
#include "../chome_gamelist.h"
#include "../chome_ss.h"
#include "../chome_disc.h"
#include "../chome_rip.h"
#include "../chome_titles.h"
#include "../../physical_disc/physical_disc.h"
#include "../chome_theme.h"
#include "../chome_gfx.h"
#include "../chome_video.h"
#include "../chome_osk.h"
#include "../chome_net.h"
#include "../chome_bt.h"
#include "../chome_proc.h"
#include "../chome_ini.h"
#include "../chome_cfgrec.h"
#include "../chome_opt.h"
#include "../chome_icons32.h"
#include "../chome_btn12.h"
#include "../../../lib/imlib2/Imlib2.h"
#include "../../../lib/miniz/miniz.h"

#include "harness.h"

// From video.h, which this file does not include; the stub in stubs.cpp models it.
int video_menu_fb_div();

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
  A stand-in for a ScreenScraper support-2D, built to the geometry of a real one.

  Deliberately generated rather than committed. The measured article is somebody's
  copyrighted scan of a pressed disc, so it cannot go in the tree; what can is its
  shape, which is what the scaler has to get right. From the real 600x600 RGBA PNG:
  the disc is a 588 px circle centred with a 6 px margin, the corners are fully
  transparent, and the hub hole is genuinely transparent out to a radius of 45 px -
  15% of the disc's own radius.

  The part that matters for the test is what the transparent pixels hold: RGB (0,0,0)
  under alpha 0, exactly as the real file does. That is the whole reason
  disc_art_scale() has to premultiply - a plain RGBA box average mixes those zeros
  into every pixel that straddles the rim or the hub, and the sprite comes out with a
  grey rim and a smudged hub. A fixture that put the disc colour under the transparent
  pixels instead would pass either way and prove nothing.

  Flat colour on purpose too: any darkening of an edge pixel is then unambiguous
  rather than something that has to be told apart from the picture's own shading.
*/
#define DISC_FIX_PX   600
#define DISC_FIX_R    294        // 588 px across, 6 px margin
#define DISC_FIX_HUB  45         // 15% of DISC_FIX_R, transparent
#define DISC_FIX_RGB  0x00e04010u

static void make_disc_scan(const char *path)
{
	int n = DISC_FIX_PX;
	Imlib_Image im = imlib_create_image(n, n);
	if (!im) { printf("  imlib_create_image failed\n"); return; }

	imlib_context_set_image(im);
	imlib_image_set_has_alpha(1);
	uint32_t *d = (uint32_t*)imlib_image_get_data();

	double c = (n - 1) / 2.0;

	for (int y = 0; y < n; y++)
	{
		for (int x = 0; x < n; x++)
		{
			double dx = x - c, dy = y - c;
			double r = dx * dx + dy * dy;

			int on = (r <= (double)DISC_FIX_R * DISC_FIX_R) &&
			         (r >= (double)DISC_FIX_HUB * DISC_FIX_HUB);

			// Transparent means transparent *black*, as the real scans store it.
			d[y * n + x] = on ? (0xff000000u | DISC_FIX_RGB) : 0x00000000u;
		}
	}

	imlib_image_put_back_data((DATA32*)d);
	imlib_image_set_format("png");
	imlib_save_image(path);
	imlib_free_image();
}

// One pixel out of a PNG on disk, as 0xAARRGGBB. 0 and w/h of 0 when it cannot be read.
static uint32_t png_pixel(const char *path, int x, int y, int *w, int *h)
{
	if (w) *w = 0;
	if (h) *h = 0;

	Imlib_Image im = imlib_load_image(path);
	if (!im) return 0;

	imlib_context_set_image(im);
	int iw = imlib_image_get_width();
	int ih = imlib_image_get_height();
	if (w) *w = iw;
	if (h) *h = ih;

	uint32_t v = 0;
	const uint32_t *p = (const uint32_t*)imlib_image_get_data_for_reading_only();
	if (p && x >= 0 && y >= 0 && x < iw && y < ih) v = p[(size_t)y * iw + x];

	imlib_free_image_and_decache();
	return v;
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

/*
  gamelist.xml fixtures - the file every other front-end already has.

  Written the way the real thing is written (sources in chome_gamelist.h): root
  <gameList>, <game> and <folder> children, entity-escaped text, and media paths
  relative to the games folder with or without the conventional "./".

  Every picture a gamelist names here lives in media/covers, which is *not* one of
  the folders chome_art.cpp probes by name. That is deliberate: if these fixtures
  used media/box2d then the scraper-folder layer would find them on its own and the
  checks would pass whether the XML was read or not. media/box2d is used once, for
  the Game Boy, which has no gamelist - that is that layer's own check.
*/
static void build_gamelists()
{
	// SNES: art the player scraped, over a game that already has a local cover.
	mkpath(ROOT "/games/SNES/media/covers");
	make_cover(ROOT "/games/SNES/media/covers/Super Metroid (Europe).png", 400, 600, 0xff9a2f2f);
	make_cover(ROOT "/games/SNES/media/covers/Secret of Mana (USA) box.png", 400, 600, 0xff2f9a4f);
	make_cover(ROOT "/games/SNES/media/covers/Secret of Mana (USA) mix.png", 400, 600, 0xffd0d020);

	put_file(ROOT "/games/SNES/gamelist.xml",
		// A UTF-8 BOM, which is what a Windows tool leaves in front of the prolog.
		"\xEF\xBB\xBF"
		"<?xml version=\"1.0\"?>\n"
		"<gameList>\n"
		/*
		  id/source are what Skraper and the gamelist editors write; harmless, and
		  here to prove attributes on <game> are not tripped over.
		*/
		"\t<game id=\"4321\" source=\"ScreenScraper.fr\">\n"
		"\t\t<path>./Super Metroid (Europe).sfc</path>\n"
		"\t\t<name>Super Metroid</name>\n"
		/*
		  A description with an escaped '&' and a bare '>' in it. The '>' is what
		  makes the parser hand this text over in two pieces, since it reads up to
		  every '>' - which is why the reader appends text rather than assigning it.
		*/
		"\t\t<desc>Ridley &amp; friends: 5 > 3, honestly.</desc>\n"
		"\t\t<image>./media/covers/Super Metroid (Europe).png</image>\n"
		"\t\t<rating>0.9</rating>\n"
		"\t</game>\n"
		// Two pictures for one game: <boxart> is the box and must beat <image>.
		"\t<game>\n"
		"\t\t<path>./Secret of Mana (USA).zip</path>\n"
		"\t\t<image>./media/covers/Secret of Mana (USA) mix.png</image>\n"
		"\t\t<boxart>./media/covers/Secret of Mana (USA) box.png</boxart>\n"
		"\t</game>\n"
		// A folder, which the shelf has no card for: nothing here may be read.
		"\t<folder>\n"
		"\t\t<path>./Hacks</path>\n"
		"\t\t<image>./media/covers/Super Metroid (Europe).png</image>\n"
		"\t</folder>\n"
		// A game whose only media are a video and a logo: neither is a cover.
		"\t<game>\n"
		"\t\t<path>./Super Mario World (Japan).sfc</path>\n"
		"\t\t<video>./media/videos/smw.mp4</video>\n"
		"\t\t<marquee>./media/wheel/smw.png</marquee>\n"
		"\t</game>\n"
		"</gameList>\n");

	/*
	  Mega Drive, deliberately CRLF and without the "./" prefix, which is how a file
	  that has been through a Windows tool arrives. Plus the two entries that must
	  *not* produce a picture: one naming a file that is no longer on the card, and
	  one naming a path in the scraping machine's home directory.
	*/
	mkpath(ROOT "/games/Genesis/media/covers");
	make_cover(ROOT "/games/Genesis/media/covers/Streets of Rage 2 (Europe).png", 400, 600, 0xff7040b0);
	make_cover(ROOT "/games/Genesis/media/covers/Sonic & Knuckles.png", 400, 600, 0xffb08040);

	put_file(ROOT "/games/Genesis/gamelist.xml",
		"<?xml version=\"1.0\"?>\r\n"
		"<gameList>\r\n"
		"\t<game>\r\n"
		"\t\t<path>Streets of Rage 2 (Europe).bin</path>\r\n"
		"\t\t<thumbnail>media/covers/Streets of Rage 2 (Europe).png</thumbnail>\r\n"
		"\t</game>\r\n"
		// The file really is called "Sonic & Knuckles.png": the entity must decode.
		"\t<game>\r\n"
		"\t\t<path>./Sonic The Hedgehog 2 (USA).md</path>\r\n"
		"\t\t<image>./media/covers/Sonic &amp; Knuckles.png</image>\r\n"
		"\t</game>\r\n"
		// Stale scrape: the file is gone, so the local art pack must still be used.
		"\t<game>\r\n"
		"\t\t<path>./Sonic The Hedgehog 2 (Europe).md</path>\r\n"
		"\t\t<image>./media/covers/deleted by the player.png</image>\r\n"
		"\t</game>\r\n"
		// A path on the PC that did the scraping: not ours to resolve.
		"\t<game>\r\n"
		"\t\t<path>./Double Dragon (Europe).bin</path>\r\n"
		"\t\t<image>~/ES-DE/downloaded_media/megadrive/covers/Double Dragon.png</image>\r\n"
		"\t</game>\r\n"
		"</gameList>\r\n");

	/*
	  A malformed file, on a system whose game would otherwise get a cover out of it:
	  an unterminated tag, an unclosed attribute quote and a comment that never ends.
	  The picture it names really is there, so the only reason for that card to stay
	  blank is the reader refusing the whole file - which is the point.
	*/
	mkpath(ROOT "/games/TGFX16/media/covers");
	make_cover(ROOT "/games/TGFX16/media/covers/Bonk's Adventure (USA).png", 400, 600, 0xffe01010);

	put_file(ROOT "/games/TGFX16/gamelist.xml",
		"<?xml version=\"1.0\"?>\n"
		"<gameList>\n"
		"\t<game>\n"
		"\t\t<path>./Bonk's Adventure (USA).pce</path>\n"
		"\t\t<image>./media/covers/Bonk's Adventure (USA).png</image>\n"
		"\t</game>\n"
		"\t<game>\n"
		"\t\t<path>./oops.pce</path\n"
		"\t\t<image attr=\"unclosed>./nowhere.png</image>\n"
		"\t<!-- and a comment that never ends\n");

	/*
	  And one that is well-formed but absurdly large - a renamed disc image is the
	  case that matters - which has to be refused on its size before the parser is
	  handed it. The game comes first in the file, so removing the size check makes
	  this file work: that is what makes the check below a check.
	*/
	mkpath(ROOT "/games/GBA/media/covers");
	make_cover(ROOT "/games/GBA/media/covers/Metroid Fusion (Europe).png", 400, 600, 0xff1010e0);

	{
		FILE *f = fopen(ROOT "/games/GBA/gamelist.xml", "wb");
		if (f)
		{
			const char *head =
				"<?xml version=\"1.0\"?>\n"
				"<gameList>\n"
				"\t<game>\n"
				"\t\t<path>./Metroid Fusion (Europe).gba</path>\n"
				"\t\t<image>./media/covers/Metroid Fusion (Europe).png</image>\n"
				"\t</game>\n"
				"\t<!-- ";
			fwrite(head, 1, strlen(head), f);

			char *blk = (char*)malloc(64 * 1024);
			if (blk)
			{
				memset(blk, 'x', 64 * 1024);
				for (int i = 0; i < (GL_MAX_BYTES / (64 * 1024)) + 16; i++) fwrite(blk, 1, 64 * 1024, f);
				free(blk);
			}

			const char *tail = " -->\n</gameList>\n";
			fwrite(tail, 1, strlen(tail), f);
			fclose(f);
		}
		else printf("  cannot write the oversize gamelist fixture\n");
	}

	/*
	  The scraper media folders, on a system with no gamelist at all: this is the
	  layout Skraper writes, and the other half of the feedback that asked for this.
	*/
	mkpath(ROOT "/games/GAMEBOY/media/box2d");
	make_cover(ROOT "/games/GAMEBOY/media/box2d/Tetris (World).png", 400, 600, 0xff30a070);
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

	/*
	  Three dumps of one game, which is the whole point of title groups: all three clean
	  to "Super Mario World" and used to draw three identical cards. One card, cycled.
	  Named so the filename order the cycle follows is Europe, Japan, USA.
	*/
	touch(ROOT "/games/SNES", "Super Mario World (USA).sfc", 4096);
	touch(ROOT "/games/SNES", "Super Mario World (Japan).sfc", 4096);
	/*
	  A different game whose title starts with the one above. Nothing may merge these: a
	  grouping key that matched on a prefix rather than on the whole cleaned title would,
	  and it would then hide a game behind a button nobody would think to press.
	*/
	touch(ROOT "/games/SNES", "Super Mario World 2 - Yoshi's Island (Europe).sfc", 4096);

	mkpath(ROOT "/games/SNES/Hacks");                            // recursion
	touch(ROOT "/games/SNES/Hacks", "Super Demo World.smc", 2048);
	touch(ROOT "/games/SNES", "Super Demo World.smc", 2048);

	// Zipped ROMs, which is how most cards actually store them.
	make_zip(ROOT "/games/SNES", "Secret of Mana (USA).zip", "Secret of Mana (USA).sfc", 4096);
	// Two archives, one game: they group only if the archive counts as the file rather
	// than as a directory of its own.
	make_zip(ROOT "/games/SNES", "Secret of Mana (Europe).zip", "Secret of Mana (Europe).sfc", 4096);
	make_zip(ROOT "/games/SNES", "Capcom Collection.zip", "Final Fight.sfc,Mega Man X.sfc", 2048);
	make_zip(ROOT "/games/SNES", "Manual Scans.zip", "readme.txt", 512);

	mkpath(ROOT "/games/Genesis");
	touch(ROOT "/games/Genesis", "Sonic The Hedgehog 2 (Europe).md", 4096);
	touch(ROOT "/games/Genesis", "Sonic The Hedgehog 2 (USA).md", 4096);
	touch(ROOT "/games/Genesis", "Streets of Rage 2 (Europe).bin", 4096);
	/*
	  The same *filename* again, one folder down. Two cards with one title, and the only
	  thing that can tell them apart on screen is the folder - so this is the fixture that
	  fails if the line naming the file drops the folder for a card that has no other file
	  to share it with.
	*/
	mkpath(ROOT "/games/Genesis/Proto");
	touch(ROOT "/games/Genesis/Proto", "Streets of Rage 2 (Europe).bin", 4096);

	/*
	  A multi-disc set. clean_title() strips "(Disc 1)" along with everything else in
	  brackets, so the discs collide exactly the way regions do and group the same way -
	  and the file name on the card is the only thing that says which disc is loaded.
	*/
	mkpath(ROOT "/games/PSX");
	touch(ROOT "/games/PSX", "Final Fantasy VII (USA) (Disc 1).cue", 2048);
	touch(ROOT "/games/PSX", "Final Fantasy VII (USA) (Disc 2).cue", 2048);
	touch(ROOT "/games/PSX", "Final Fantasy VII (USA) (Disc 3).cue", 2048);

	/*
	  The other four CD systems, in the folder names the official MiSTer Distribution
	  uses - which is the whole reason these exist, since a romset download and a rip both
	  land here without anybody being told to move them.

	  Each folder is laid out the way a real one is, and each layout is a check:

	  MegaCD/Europe/ and MegaCD/USA/  the same game twice under the regional subfolders a
	                                  downloaded Mega CD set actually ships with. One card.
	  MegaCD/Silpheed/                a sheet and its tracks, which is what a rip writes.
	                                  One card, and the tracks are not games.
	  TGFX16-CD/cd_bios.rom           the cores' own BIOS images, at the root of the games
	  NeoGeo-CD/neocd.bin + two more  folder each core reads them from. These are the
	  Saturn/boot.rom                 fixtures that fail when "bin" or "rom" is added to a
	                                  CD system's extension list, which is the mistake that
	                                  turns a BIOS into a card nobody can press.

	                                  The two .bin ones are deliberately NOT beside a .cue,
	                                  because 73b0f71's rule would hide them if they were
	                                  and a widened list would then go unnoticed. The .rom
	                                  ones need no such care: that rule only covers
	                                  bin/iso/wav/raw, so Saturn/boot.rom is exposed even
	                                  sitting beside the two sheets below it.
	  Saturn/                         a two-disc game, flat, the way his own card has it.
	*/
	mkpath(ROOT "/games/MegaCD/Europe");
	touch(ROOT "/games/MegaCD/Europe", "Lunar - Eternal Blue.cue", 2048);
	mkpath(ROOT "/games/MegaCD/USA");
	touch(ROOT "/games/MegaCD/USA", "Lunar - Eternal Blue.cue", 2048);
	mkpath(ROOT "/games/MegaCD/Silpheed");
	touch(ROOT "/games/MegaCD/Silpheed", "Silpheed.cue", 512);
	touch(ROOT "/games/MegaCD/Silpheed", "Track 01.bin", 4096);
	touch(ROOT "/games/MegaCD/Silpheed", "Track 02.bin", 4096);

	mkpath(ROOT "/games/TGFX16-CD");
	touch(ROOT "/games/TGFX16-CD", "cd_bios.rom", 1024);
	mkpath(ROOT "/games/TGFX16-CD/Rondo of Blood");
	touch(ROOT "/games/TGFX16-CD/Rondo of Blood", "Rondo of Blood.cue", 512);
	touch(ROOT "/games/TGFX16-CD/Rondo of Blood", "Track 01.bin", 4096);

	mkpath(ROOT "/games/NeoGeo-CD");
	touch(ROOT "/games/NeoGeo-CD", "neocd.bin", 512);
	touch(ROOT "/games/NeoGeo-CD", "top-sp1.bin", 512);
	touch(ROOT "/games/NeoGeo-CD", "uni-bioscd.rom", 512);
	mkpath(ROOT "/games/NeoGeo-CD/Samurai Shodown RPG");
	touch(ROOT "/games/NeoGeo-CD/Samurai Shodown RPG", "Samurai Shodown RPG.cue", 512);
	touch(ROOT "/games/NeoGeo-CD/Samurai Shodown RPG", "Track 01.bin", 4096);

	mkpath(ROOT "/games/Saturn");
	touch(ROOT "/games/Saturn", "boot.rom", 1024);
	touch(ROOT "/games/Saturn", "Deep Fear (Europe) (Disc 1).cue", 2048);
	touch(ROOT "/games/Saturn", "Deep Fear (Europe) (Disc 2).cue", 2048);

	/*
	  Multi-track CD rips under the Mega Drive folder, which is where this bites: md accepts
	  "bin" and a rip names its tracks after their position, so two games' tracks are two
	  sets of files called the same thing. Both layouts in the wild:

	  Sonic CD/    a cue naming its tracks - the tracks are parts, not games
	  Snatcher/    the same, and the pair of them is what used to collide
	  Keio Flying Squadron/  tracks with no cue at all, so the folder has to name them

	  And a plain cartridge in the same folder, to prove an ordinary .bin is untouched.
	*/
	mkpath(ROOT "/games/Genesis/Sonic CD");
	touch(ROOT "/games/Genesis/Sonic CD", "Sonic CD.cue", 512);
	touch(ROOT "/games/Genesis/Sonic CD", "Track 01.bin", 4096);
	touch(ROOT "/games/Genesis/Sonic CD", "Track 02.bin", 4096);
	mkpath(ROOT "/games/Genesis/Snatcher");
	touch(ROOT "/games/Genesis/Snatcher", "Snatcher.cue", 512);
	touch(ROOT "/games/Genesis/Snatcher", "Track 01.bin", 4096);
	touch(ROOT "/games/Genesis/Snatcher", "Track 02.bin", 4096);
	mkpath(ROOT "/games/Genesis/Keio Flying Squadron");
	touch(ROOT "/games/Genesis/Keio Flying Squadron", "Track 01.bin", 4096);
	touch(ROOT "/games/Genesis/Keio Flying Squadron", "Track 02.bin", 4096);

	// In a folder, so that borrowing the folder's name is available and visibly not taken.
	mkpath(ROOT "/games/Genesis/Capcom");
	touch(ROOT "/games/Genesis/Capcom", "1942.bin", 4096);
	touch(ROOT "/games/Genesis/Capcom", "Discworld.bin", 4096);

	/*
	  One title on two systems, and *only* the system telling them apart: both are ".bin"
	  at the top of their own games folder, which the Mega Drive and the Atari 7800 both
	  accept. The Sonic pair below differs by extension as well, so it cannot be the check
	  that the system is in the grouping key - sabotaging the system out of the key left it
	  passing, which is how this fixture came to exist.
	*/
	touch(ROOT "/games/Genesis", "Double Dragon (Europe).bin", 2048);
	mkpath(ROOT "/games/A7800");
	touch(ROOT "/games/A7800", "Double Dragon (USA).bin", 2048);

	mkpath(ROOT "/games/TGFX16");
	touch(ROOT "/games/TGFX16", "Bonk's Adventure (USA).pce", 2048);

	mkpath(ROOT "/games/GBA");
	touch(ROOT "/games/GBA", "Metroid Fusion (Europe).gba", 2048);

	// A DMG cart and a GBC cart in the same Game Boy core: they must be offered
	// different screens.
	mkpath(ROOT "/games/SMS");
	touch(ROOT "/games/SMS", "Sonic The Hedgehog 2 (Europe) (GG).gg", 2048);
	/*
	  The same title beside it in the same folder and the same core, differing only by
	  extension - and they are different games with different levels, which is why the
	  extension is part of the grouping key. class_of() already treats the two as
	  different hardware; this is the shelf agreeing with it.
	*/
	touch(ROOT "/games/SMS", "Sonic The Hedgehog 2 (Europe).sms", 2048);

	mkpath(ROOT "/games/AtariLynx");
	touch(ROOT "/games/AtariLynx", "Chip's Challenge (USA).lnx", 2048);

	mkpath(ROOT "/games/WonderSwan");
	touch(ROOT "/games/WonderSwan", "Gunpey (Japan).ws", 2048);
	touch(ROOT "/games/WonderSwan", "Rockman EXE (Japan).wsc", 2048);

	// A .npc: the one extension left genuinely unresolved in ss_system_id() (see
	// chome_ss.cpp), used by assert_art_ladder() as its "no systemeid at all" example
	// now that every other system on the shelf has one.
	mkpath(ROOT "/games/NGP");
	touch(ROOT "/games/NGP", "SNK vs Capcom (USA).npc", 2048);

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

	/*
	  One dump of Super Mario World has a suspend point and the other two have none. The
	  three share a card, so this is what proves the strip follows the file on show rather
	  than the card: get it wrong and the player is offered another ROM's save state.
	*/
	touch(ROOT "/savestates/SNES", "Super Mario World (USA)_1.ss", 256);

	/*
	  The ActRaiser shape from the device (2026-08-11): the first slot empty, the real
	  state in slot 2 with its thumbnail, and a second state in slot 4 - which is past
	  the three slots the strip shows, so it must neither draw nor leak into another
	  tile. This is the fixture behind "ENT plays the state the strip opened on".
	*/
	touch(ROOT "/savestates/SNES", "The Legend of Zelda - A Link to the Past (Europe)_2.ss", 256);
	touch(ROOT "/savestates/SNES", "The Legend of Zelda - A Link to the Past (Europe)_4.ss", 256);
	make_cover(ROOT "/savestates/SNES/The Legend of Zelda - A Link to the Past (Europe)_2.png", 320, 240, 0xff35608e);

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

	build_gamelists();
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
  A key held down, delivered the way menu_key_get() delivers one: the press, then the same
  keycode again every REPEATRATE with *nothing at all* on the frames in between, and one
  release at the end. That shape is what the front-end has to tell apart from a run of taps -
  see held_key in chome_ui.cpp - and it is the same shape assert_carousel_slide() has pinned
  for the partial-repaint work since long before this existed.

  `repeats` is how many auto-repeats follow the press, so a cursor is offered repeats + 1
  chances to move. Every list now wraps at its ends on a press the player made, so this is
  the only way to walk one to its end and *stay* there: a run of press() calls is a run of
  fresh presses, and the one that arrives at the end wraps round.
*/
static void hold_dir(int key, int repeats, int settle = 8)
{
	chome_handle(key);
	for (int r = 0; r < repeats; r++)
	{
		// Three frames of nothing at 16 ms each, which is about the 50 ms of REPEATRATE.
		for (int f = 0; f < 3; f++) { harness_advance(16); chome_handle(0); }
		chome_handle(key);
	}
	harness_advance(16);
	chome_handle(key | UPSTROKE);
	frame(settle);
}

/*
  Put the cursor on a known entry of whatever list is up, from wherever it happens to be.

  A hold to the first entry and then that many taps. Counting taps from an unknown start is
  what stops working once the ends wrap: the walk arrives somewhere that depends on how long
  the list is and on where the previous part of a section left the cursor, and it does so
  silently. `axis` is 0 for a column and 1 for a row, as chome_list_cursor() takes it.
*/
static void list_goto(int axis, int row, int fwd, int back, int settle = 6)
{
	int n = 0;
	chome_list_cursor(axis, &n);
	hold_dir(back, n + 8, settle);
	for (int i = 0; i < row; i++) press(fwd, settle);
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
  The Online Covers panel, mirroring draw_covers()'s own arithmetic - the same bargain
  panel_rows_pixels() below makes, and for the same reason. That screen sizes its panel to
  its contents rather than taking the profile default, and what has to be read off it is
  the value column at the right-hand edge of the rows: a box small enough to be safely
  inside any panel whatever its size would not contain the one thing being looked at.

  The four constants are draw_covers()'s and have to move with it.
*/
static void covers_rect(int *x0, int *y0, int *x1, int *y1)
{
	const chome_profile *p = theme_get();
	int s = p->ts_ui;
	int foot = 3 * 10 * s + 4 * s;

	int pw = p->w - p->inset * 2;
	if (pw > 46 * gfx_adv(s)) pw = 46 * gfx_adv(s);

	int ph = (10 * s + 6) + 5 * s + 4 * (12 * s) + foot + 6 * s;
	if (ph > p->h - 2 * p->safe_y) ph = p->h - 2 * p->safe_y;

	*x0 = (p->w - pw) / 2;
	*y0 = (p->h - ph) / 2;
	*x1 = *x0 + pw;
	*y1 = *y0 + ph;
}

static unsigned long covers_hash()
{
	int x0, y0, x1, y1;
	covers_rect(&x0, &y0, &x1, &y1);
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

// The rows the legend occupies, which is where its button prompts and nothing else are.
// Up here with the other pixel helpers because two sections apart in this file count a
// button's colour in that band to ask whether a prompt is being offered at all.
static int legend_colour(uint32_t want)
{
	const chome_profile *p = theme_get();
	return box_pixels(0, p->y_legend - 6 * p->ts_ui, p->w, p->h, want);
}

// Over the same box pt_panel_hash() fingerprints, which at every profile is inside the
// Wi-Fi and Controllers panels and outside the shelf behind them.
static int panel_pixels(uint32_t want)
{
	int w = gfx_w(), h = gfx_h();
	return box_pixels(w / 4, h / 2 - h / 6, (3 * w) / 4, h / 2 + h / 6, want);
}

/*
  The Controllers panel, mirroring draw_pads()'s own arithmetic - the same bargain
  covers_rect() makes above, for the same reason and with the same obligation: that screen
  sizes its panel to its contents rather than taking the profile default, and these
  constants are draw_pads()'s and have to move with it.

  panel_pixels() above is not enough for what this is wanted for. Its box is the middle
  third of the screen, and the line being looked for is at the *foot* of the panel - at
  720p that is thirty pixels below the bottom edge of that box, so a check written against
  it would have passed whether the sentence was drawn or not.

  Takes no argument, and that is worth stating: the panel is the same size whether the SNAC
  line is up or not. It buys its row out of the list rather than out of the height, because
  at 240p ten more pixels of panel land on top of the legend. A version of this that grew
  with the notice would also have been a box that reached outside the panel in the quiet
  case, which is where the shelf behind the scrim is - and any amber out there would have
  been counted as the notice.
*/
static void pads_rect(int *x0, int *y0, int *x1, int *y1)
{
	const chome_profile *p = theme_get();
	int s = p->ts_ui;

	int pw = p->w - 2 * p->inset;
	if (pw > 44 * gfx_adv(s)) pw = 44 * gfx_adv(s);

	int ph = (10 * s + 6) + 18 * s + 5 * (21 * s) + 2 * (10 * s) + 12 * s + 4 * s;
	if (ph > p->h - 2 * p->safe_y) ph = p->h - 2 * p->safe_y;

	*x0 = (p->w - pw) / 2;
	*y0 = (p->h - ph) / 2;
	*x1 = *x0 + pw;
	*y1 = *y0 + ph;
}

/*
  Amber anywhere on the Controllers panel, which on that screen means exactly one thing: the
  line saying a PlayStation pad on the SNAC port cannot work in the core that is running.

  Nothing else there is drawn in it - a player number is green, the entry that adds a
  controller is blue, an armed forget is red, and everything else is ink or the panel's own
  two greys - so a count of this one colour is a yes-or-no about that sentence being on
  screen. Counted rather than hashed because the claim is "it is there" and "it is not
  there", and a hash can only say that two frames differ.
*/
static int pads_amber()
{
	int x0, y0, x1, y1;
	pads_rect(&x0, &y0, &x1, &y1);
	return box_pixels(x0, y0, x1, y1, COL_YELLOW);
}

/*
  Options > Controllers, from the shelf, at whatever profile is current.

  Written as a walk rather than as a row index for two reasons this file has been bitten by
  before. Up wraps to the *last* row of the Options list and counting back from there does
  not care how long that list is - which is the only navigation that survives a row being
  added to it. And the menu bar drops the Display entry at 240p, so Options is the first
  slot there and the second everywhere else.

  Re-entered from the shelf each time rather than nudged into repainting: the front-end only
  composes a frame when something has marked it dirty, and the state this is used to observe
  - what the SNAC reader found - changes outside the front-end entirely, so nothing marks
  anything. A screen left up would simply keep showing the frame it already had.
*/
static void open_controllers()
{
	harness_set_menu_core(1);
	chome_leave();
	press(KEY_MENU, 20);
	frame(8);

	press(KEY_UP, 10);                         // the menu bar
	if (theme_get()->id != PROF_LO) press(KEY_RIGHT, 10);   // Options; the first slot at 240p
	press(KEY_ENTER, 14);

	press(KEY_UP, 8);                          // About, the last row
	press(KEY_UP, 8);                          // Advanced / Core Settings
	press(KEY_UP, 8);                          // More Settings
	press(KEY_UP, 8);                          // Best Settings
	press(KEY_UP, 8);                          // Wi-Fi
	press(KEY_UP, 8);                          // Controllers
	press(KEY_ENTER, 14);
	frame(8);
}

/*
  The disc dialog's diameter, measured off the screen.

  Measured rather than recomputed from the profile, because the layout is the thing being
  checked: disc_layout_for() gives the disc whatever the panel has spare, and a copy of that
  arithmetic in this file would agree with itself for ever while both drifted away from what
  is drawn.

  The panel is a flat COL_PANEL and the disc is the widest thing on it, so the widest run of
  non-panel pixels *enclosed by* panel colour is the diameter. Enclosed on both sides is the
  whole guard, and it is what keeps the shelf out: a row that never touches the panel encloses
  nothing, and the shelf either side of a panel narrower than the canvas is only ever closed
  on one side, by the panel it runs into.

  It used to scan from the inset and require the row to *start* on panel colour, which was the
  same guard while the dialog was the canvas less that inset. It is not any more - the panel is
  sized to its contents on a canvas that can afford it - and read that way this answered zero
  at 720p, because the row began on scrimmed shelf and every row was skipped.

  The panel's own title bar is excluded either way: it is COL_INK from edge to edge of the
  panel, with the frame's COL_PANELLO outside it and no COL_PANEL on that row at all.
*/
static int disc_drawn_box(int *ocx, int *ocy)
{
	const uint32_t *fb = harness_fb_shown();
	int w = gfx_w(), h = gfx_h();
	if (ocx) *ocx = 0;
	if (ocy) *ocy = 0;
	if (!fb || w < 1 || h < 1) return 0;

	int best = 0, bestx = 0, besty = 0;
	for (int y = 0; y < h; y++)
	{
		const uint32_t *row = fb + (size_t)y * w;
		int run = 0, opened = 0;

		for (int x = 0; x < w; x++)
		{
			if ((row[x] | 0xff000000u) == COL_PANEL)
			{
				if (opened && run > best) { best = run; bestx = x - run + run / 2; besty = y; }
				run = 0;
				opened = 1;
			}
			else run++;
		}
	}

	/*
	  Then the centre row, down the column the widest run was centred on.

	  The widest run does NOT identify the centre row, which is the trap this walks into and
	  out of: a circle 480 pixels across has a dozen rows of equal maximum width, and the first
	  of them is seven pixels above the middle at 720p. A caller reading a ring two pixels thick
	  off that centre gets a lens rather than an annulus, and measures a fraction of the edge -
	  which is exactly how this first reported a properly anti-aliased disc as a hard-edged one.

	  So the vertical extent is measured too, down the one column that is certainly inside the
	  disc, and the middle of it is the centre. Measured rather than derived for the same reason
	  the diameter is: draw_disc() places the disc under however tall the lines above it came
	  out, and a second copy of that sum in this file would drift away from it.
	*/
	if (best > 0 && ocy)
	{
		int top = besty, bot = besty;
		while (top > 0 && (fb[(size_t)(top - 1) * w + bestx] | 0xff000000u) != COL_PANEL) top--;
		while (bot < h - 1 && (fb[(size_t)(bot + 1) * w + bestx] | 0xff000000u) != COL_PANEL) bot++;
		besty = (top + bot) / 2;
	}

	if (ocx) *ocx = bestx;
	if (ocy) *ocy = besty;

	return best;
}

static int disc_drawn_dia()
{
	return disc_drawn_box(0, 0);
}

/*
  The panel's plate, measured off the screen: where a dialog actually is and how big.

  Measured rather than asked of disc_layout_for(), for the same reason the diameter above is.
  The whole question this answers - is the dialog still eating the screen at 720p - is a
  question about pixels, and a test that recomputed the layout would agree with the layout for
  ever while both drifted away from the picture.

  The widest run of COL_PANEL in the frame is the plate's inner width: draw_panel_at() fills
  the rectangle with COL_PANEL and then draws its two-pixel COL_PANELLO frame inside it, so a
  panel `pw` wide leaves a run of pw-4. The rows are then walked up and down one column inside
  that left edge, which is flat plate for the whole body of the panel and stops at the COL_INK
  title bar above and the frame below. So the rectangle handed back is the plate, and the panel
  as disc_layout_for() sized it is two pixels wider each way plus its title bar.

  Only usable on a screen with one panel on it, which is every screen that calls this.

  Runs the full width of the canvas are not it, and that is the menu bar: draw_menubar() fills
  0..p->w in the same COL_PANEL, and compose() slides it in on exactly the screens that have a
  panel open, so on a 320x240 canvas this answered with the bar's 320 rather than the panel's
  250 and put the panel thirty rows too high. No panel is ever wider than the canvas less its
  inset either side, so anything within a few pixels of the full width is the bar.
*/
static int panel_plate_seen(int *ox, int *oy, int *ow, int *oh)
{
	const uint32_t *fb = harness_fb_shown();
	int w = gfx_w(), h = gfx_h();
	int best = 0, bx = 0, by = 0;

	if (ox) *ox = 0;
	if (oy) *oy = 0;
	if (ow) *ow = 0;
	if (oh) *oh = 0;
	if (!fb || w < 1 || h < 1) return 0;

	for (int y = 0; y < h; y++)
	{
		const uint32_t *row = fb + (size_t)y * w;
		int run = 0;

		for (int x = 0; x <= w; x++)
		{
			int on = (x < w) && ((row[x] | 0xff000000u) == COL_PANEL);
			if (on) { run++; continue; }
			if (run > best && run < w - 8) { best = run; bx = x - run; by = y; }
			run = 0;
		}
	}

	if (best < 8) return 0;

	// One pixel inside the plate's left edge: flat COL_PANEL down the whole body, whatever
	// is centred on the rows in between.
	int probe = bx + 1;
	int y0 = by, y1 = by;
	while (y0 > 0 && (fb[(size_t)(y0 - 1) * w + probe] | 0xff000000u) == COL_PANEL) y0--;
	while (y1 < h - 1 && (fb[(size_t)(y1 + 1) * w + probe] | 0xff000000u) == COL_PANEL) y1++;

	if (ox) *ox = bx;
	if (oy) *oy = y0;
	if (ow) *ow = best;
	if (oh) *oh = y1 - y0 + 1;
	return 1;
}

// The mean of the three channels over a box, times 100 so a fraction of a level is visible.
// Both halves of the in-game dim are a mean rather than a colour: the scrim leaves two values
// in alternate pixels and what the eye reads is the average of them.
static int box_mean_x100(int x0, int y0, int x1, int y1)
{
	const uint32_t *fb = harness_fb_shown();
	int w = gfx_w(), h = gfx_h();
	if (!fb || w < 1 || h < 1) return 0;

	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (x1 > w) x1 = w;
	if (y1 > h) y1 = h;
	if (x1 <= x0 || y1 <= y0) return 0;

	unsigned long sum = 0;
	unsigned long n = 0;
	for (int y = y0; y < y1; y++)
	{
		for (int x = x0; x < x1; x++)
		{
			uint32_t c = fb[(size_t)y * w + x];
			sum += ((c >> 16) & 0xff) + ((c >> 8) & 0xff) + (c & 0xff);
			n += 3;
		}
	}

	return n ? (int)((sum * 100) / n) : 0;
}

/*
  How many pixels on a ring are blends: within two pixels of radius `rad` from cx,cy and
  neither of the two colours that boundary lies between.

  This is what "the edge is anti-aliased" means as a number. A hard-edged circle answers zero
  by construction, whatever it looks like; a boundary drawn by coverage answers a good fraction
  of its own circumference, because that is how many pixels the arc actually passes through.

  Only usable where both sides of the boundary are flat UI colours. The disc's other two
  boundaries have a photograph on one side, where a blended pixel and a pixel of the picture
  are the same thing to a colour test - see the section that calls this.
*/
static int ring_blends(int cx, int cy, int rad, uint32_t a, uint32_t b, int *ntotal)
{
	const uint32_t *fb = harness_fb_shown();
	int w = gfx_w(), h = gfx_h();
	int lo = (rad - 2) * (rad - 2), hi = (rad + 2) * (rad + 2);
	int n = 0, tot = 0;

	if (ntotal) *ntotal = 0;
	if (!fb || rad < 3) return 0;

	for (int y = cy - rad - 2; y <= cy + rad + 2; y++)
	{
		if (y < 0 || y >= h) continue;

		for (int x = cx - rad - 2; x <= cx + rad + 2; x++)
		{
			if (x < 0 || x >= w) continue;

			int dx = x - cx, dy = y - cy;
			int d2 = dx * dx + dy * dy;
			if (d2 < lo || d2 > hi) continue;

			tot++;
			uint32_t c = fb[(size_t)y * w + x] | 0xff000000u;
			if (c != a && c != b) n++;
		}
	}

	if (ntotal) *ntotal = tot;
	return n;
}

// The one-cell shadow gfx_disc mixes from its rim and hole colours, which is also what
// disc_rot lays its outer ring and its hub ring in. Derived the same way rather than written
// out, so a palette change moves both together.
static uint32_t disc_edge_col()
{
	uint32_t e = ((COL_WHITE >> 1) & 0x7f7f7f7f) + ((COL_BGDARK >> 1) & 0x7f7f7f7f);
	return e | 0xff000000u;
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
		/*
		  With the error return, not without it. This printed "wrote" unconditionally and three
		  PNGs a section was writing never reached test/out - which reads as the section not
		  having run at all, and cost an hour of looking in the wrong place.
		*/
		Imlib_Load_Error err = IMLIB_LOAD_ERROR_NONE;
		imlib_save_image_with_error_return(path, &err);
		imlib_free_image();
		if (err != IMLIB_LOAD_ERROR_NONE)
			printf("  FAILED to write %s.png (%dx%d): imlib error %d\n", name, w, h, (int)err);
		else
			printf("  wrote %s.png (%dx%d)\n", name, w, h);
	}
	free(copy);
}

// The folders lead the root shelf, so that many presses land on the first game. With
// SORT_TITLE that is deterministically Bonk's Adventure, which the fake SD gives
// savestates and a thumbnail.
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

/*
  The shelf, wound back to its leftmost card, whatever it was showing before.

  A *held* LEFT and not a run of taps, which is task 54 seen from the test side. This used to
  read "for (i < 40) press(KEY_LEFT)" with the comment "LEFT clamps at index 0", and that was
  true and is not any more: the carousel wraps at its ends now, so forty taps mean forty
  cards and wrap round as often as the shelf is short - which lands the cursor somewhere that
  depends on how many games the fixture happens to have. Holding the key is what a player
  does to get to the front of a shelf, and holding is the thing that stops at the end.

  Enough repeats to cross the whole view from anywhere on it, plus a few spare: the first
  event of the hold is a real press, so a cursor already at 0 wraps to the far end on it and
  then has the whole view to walk back.
*/
static void shelf_rewind()
{
	hold_dir(KEY_LEFT, lib_view_count() + 8, 6);
	frame(8);
}

/*
  Out to the *root* shelf, however deep into a folder the previous section left it.

  B pops a folder only from its leftmost entry (see the SCR_HOME case of back()), and
  nav_pop() restores the position the folder was opened from rather than the leftmost - so a
  level costs the pair "jump left, then pop", and twenty presses cover any stack NAV_DEPTH
  allows. At the root both presses are no-ops: nav_pop() answers 0 and B jumps to a leftmost
  entry the cursor is already on. That is what makes a fixed count safe rather than hopeful.

  Worth having on its own, and not only for task 54: select_folder() and select_titled() below
  both look for a card *on the root shelf* and neither used to make sure it was looking at
  one. That went unnoticed while the shelf clamped, because nothing they did could pop a view
  and every section that opened a folder happened to close it; the marquee section found it
  the first time a section left the browser open behind it.
*/
static void shelf_root()
{
	for (int i = 0; i < 20; i++) press(KEY_ESC, 2);
	frame(6);
}

static void select_first_game()
{
	// Re-entry keeps the previous shelf position by design, so rewind to the
	// start before counting.
	shelf_rewind();

	int n = leading_folders();
	for (int i = 0; i < n; i++) press(KEY_RIGHT, 6);
}

/*
  Rewinds and steps right until the named leading folder is selected.

  By label rather than by a count of presses: the row of folders is no longer a fixed
  length - Recently Played joins it as soon as a game has been played - and a hardcoded
  "Systems is the second card" would then quietly open a different card rather than
  failing.
*/
static int select_folder(const char *label)
{
	// Out of any folder first, then to the front of the root shelf: the label being looked
	// for is on the root and nowhere else. See shelf_root().
	shelf_root();
	shelf_rewind();

	int target = -1;
	for (int i = 0; i < leading_folders(); i++)
	{
		if (!strcmp(lib_view_entry(i)->label, label)) { target = i; break; }
	}
	if (target < 0) return 0;

	for (int i = 0; i < target; i++) press(KEY_RIGHT, 6);
	frame(10);
	return 1;
}

// Rewinds and steps right until the named game is selected on the root shelf.
static int select_titled(const char *want)
{
	shelf_root();
	shelf_rewind();

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

/*
  The disc dialog, captured at every profile.

  It is the one screen here with no shelf card behind it to borrow proportions from - a
  title, a large disc and a row of buttons, all sized off the canvas - so it is the one
  most worth looking at on all four canvases rather than only on the one the assertions
  run at. Nothing is checked in here: the checks live in assert_disc_dialog(), and this is
  for the eyes.

  A title table goes on the card for the duration, because what the panel has to fit is a
  real game name and not a serial: "SLES-01506" is ten characters and "Metal Gear Solid"
  is sixteen, and the wider of the two is the one that decides the panel width. Removed
  again on the way out, with the module told to forget it - the sections after the walk
  assert that a card with no table on it draws exactly what it always did.
*/
// Defined down with the fake drive it needs, which is declared further on.
static void walk_disc_dialog(const char *tag);

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
	  And the shelf in the middle of a slide, which is a frame drawn by the partial path: the
	  cards are between two positions and only their band has been repainted. Worth an eye at
	  every profile rather than only at 720p, because what would go wrong here is geometric -
	  a band a row short of the focus ring or of the shadow leaves a line of the previous
	  frame at the edge of the row, and every profile sizes those differently.

	  Three frames in, which is far enough for the cards to have visibly moved and early
	  enough that the ease is still running. The release is sent first so the title in the
	  capture is the one under the cursor rather than the one before it - what is being looked
	  at here is the card row, not the deferral.
	*/
	chome_handle(KEY_RIGHT);
	harness_advance(16);
	chome_handle(KEY_RIGHT | UPSTROKE);
	for (int i = 0; i < 3; i++) { harness_advance(16); chome_handle(0); }
	snprintf(name, sizeof(name), "%s-2b-home-mid-slide", tag);
	dump(name);
	frame(20);
	press(KEY_LEFT);
	frame(10);

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

	/*
	  And a title long enough that the header cannot hold it *and* the words "SUSPEND
	  POINTS": this one is 22 characters and the label another 17, where 240p fits 35. The
	  label is what gives way, so this is where to look if the header ever starts eating
	  the game's name again ("LEGEND OF ZELDA, THE - SUSPEND POI>", on his own card).
	*/
	select_titled("Zelda - Oracle");
	press(KEY_DOWN, 25);
	snprintf(name, sizeof(name), "%s-3c-suspend-long-title", tag);
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

	/*
	  Display is not offered at 240p (tiles too small to judge a filter by), so capture
	  the shortened bar there instead of a screen that does not exist. Asked of the
	  resolved profile rather than of the forced value: a stretched 15 kHz canvas lands
	  on 240p from `auto`, and pressing Enter on a bar that has no Display entry opens
	  Options and files it under the wrong name.
	*/
	if (theme_get()->id == PROF_LO)
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

	/*
	  The entry after Options, which is Power - and was Power when this dump was named
	  "about" too, because Display is dropped at 240p and this walk counts from wherever
	  the bar happens to start. Named for what it captures now. About is not on the bar at
	  all any more; it is the last row of the Options panel, and it is walked to there in
	  the capitals section.
	*/
	press(KEY_RIGHT, 8);
	press(KEY_ENTER, 20);
	snprintf(name, sizeof(name), "%s-9-bar-after-options", tag);
	dump(name);
	press(KEY_ESC, 10);
	press(KEY_ESC, 10);

	/*
	  The Systems view, which is where the per-system icons appear. Back all the way
	  out first and then walk from the left edge: the steps above leave the shelf on a
	  game, and a stray Enter there launches it instead.
	*/
	for (int i = 0; i < 4; i++) press(KEY_ESC, 8);
	select_folder("Systems");
	press(KEY_ENTER, 24);
	snprintf(name, sizeof(name), "%s-10-systems", tag);
	dump(name);
	press(KEY_ESC, 12);

	walk_disc_dialog(tag);
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

	int folders = 0, amiga_on_shelf = 0, recent_card = 0;
	for (int i = 0; i < n; i++)
	{
		const chome_entry *e = lib_view_entry(i);
		if (e->kind != ENT_GAME)
		{
			folders++;
			if (!strcmp(e->label, "Recently Played")) recent_card = 1;
			continue;
		}
		chome_item *it = lib_item(e->game);
		const chome_sys *s = it ? lib_sys(it->sysidx) : 0;
		if (s && s->computer) amiga_on_shelf = 1;
	}
	check(folders == 3, "three folders lead the root shelf");
	check(!amiga_on_shelf, "computer games kept off the root shelf");

	// Nothing has been played yet - this section runs before any launch - so Recently
	// Played must not be there at all. An entry that opens on "NOTHING HERE" is worse
	// than no entry, and it would sit second on the shelf where the thumb lands.
	check(!recent_card, "Recently Played is absent until something has been played");
	check(lib_view_build(VIEW_RECENT, -1, SORT_TITLE) == 0, "and the view behind it is empty");

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

/* ----------------------------------------------------------- title groups -- */

// The index position of a game, by system id and path relative to its games dir.
static int item_at(const char *sysid, const char *relpath)
{
	for (int i = 0; i < lib_item_count(); i++)
	{
		chome_item *it = lib_item(i);
		const chome_sys *s = lib_sys(it->sysidx);
		if (s && !strcmp(s->id, sysid) && !strcmp(it->path, relpath)) return i;
	}
	return -1;
}

/*
  The entry carrying a game, whether or not it is the file that entry is showing. The
  distinction is the point: a check written against `e->game` alone would pass while the
  other files behind the card were unreachable.
*/
static int entry_carrying(int idx)
{
	if (idx < 0) return -1;

	for (int i = 0; i < lib_view_count(); i++)
	{
		const chome_entry *e = lib_view_entry(i);
		if (!e || e->kind != ENT_GAME) continue;
		for (int v = 0; v < e->nvar; v++) if (lib_view_variant(i, v) == idx) return i;
	}
	return -1;
}

static int entry_nvar(int entry)
{
	const chome_entry *e = lib_view_entry(entry);
	return e ? e->nvar : -1;
}

/*
  Files that share a title, drawn as one card.

  Every check here is written to fail one specific way. The grouping ones fail if the key
  stops looking at what it looks at now; the four negative ones fail if it starts looking
  at less, because a merge of two genuinely different games is the one fault this feature
  must not have - it would hide a game behind a button nobody knows to press, and worse,
  attach one ROM's save states and settings to another.

  Runs before anything has been launched, so no file has a play count and the card must be
  showing the first of its files in filename order.
*/
static void assert_variants()
{
	printf("\n== title groups ==\n");

	int smw_eu = item_at("snes", "Super Mario World (Europe).sfc");
	int smw_jp = item_at("snes", "Super Mario World (Japan).sfc");
	int smw_us = item_at("snes", "Super Mario World (USA).sfc");
	int som_us = item_at("snes", "Secret of Mana (USA).zip/Secret of Mana (USA).sfc");
	int som_eu = item_at("snes", "Secret of Mana (Europe).zip/Secret of Mana (Europe).sfc");
	int son_md = item_at("md", "Sonic The Hedgehog 2 (Europe).md");
	int son_us = item_at("md", "Sonic The Hedgehog 2 (USA).md");
	int son_gg = item_at("sms", "Sonic The Hedgehog 2 (Europe) (GG).gg");
	int son_sm = item_at("sms", "Sonic The Hedgehog 2 (Europe).sms");
	int ff7_2  = item_at("psx", "Final Fantasy VII (USA) (Disc 2).cue");
	int smw2   = item_at("snes", "Super Mario World 2 - Yoshi's Island (Europe).sfc");
	int dd_md  = item_at("md", "Double Dragon (Europe).bin");
	int dd_78  = item_at("a7800", "Double Dragon (USA).bin");

	if (smw_eu < 0 || smw_jp < 0 || smw_us < 0 || som_us < 0 || som_eu < 0 ||
		son_md < 0 || son_us < 0 || son_gg < 0 || son_sm < 0 || ff7_2 < 0 ||
		smw2 < 0 || dd_md < 0 || dd_78 < 0)
	{
		check(0, "this section's fixtures are indexed");
		return;
	}

	lib_view_build(VIEW_ALL, -1, SORT_TITLE);

	int card = entry_carrying(smw_eu);
	check(card >= 0 && card == entry_carrying(smw_jp) && card == entry_carrying(smw_us),
		"three dumps of one game are one card");
	check(entry_nvar(card) == 3, "and the card says it has three files behind it");

	// Filename order, not readdir order: the cycle has to be the same every boot.
	check(lib_view_variant(card, 0) == smw_eu &&
		lib_view_variant(card, 1) == smw_jp &&
		lib_view_variant(card, 2) == smw_us,
		"the files behind a card are in filename order");
	check(lib_view_variant(card, 3) == -1, "and asking past the last one answers nothing");

	const chome_entry *e = lib_view_entry(card);
	check(e && e->game == smw_eu && e->vsel == 0,
		"with nothing played yet the card shows the first file");
	check(!strcmp(lib_view_variant_file(card, 0), "Super Mario World (Europe).sfc"),
		"and names that file, in its own case, without the folder it shares");

	/*
	  One title in two folders is now ONE card - the directory left the grouping key so that
	  multi-disc sets group, and a hack sharing its original's card is the accepted cost.
	*/
	{
		int sdw_top = item_at("snes", "Super Demo World.smc");
		int sdw_hack = item_at("snes", "Hacks/Super Demo World.smc");
		check(sdw_top >= 0 && sdw_hack >= 0, "the same title exists in two folders");
		check(entry_carrying(sdw_top) == entry_carrying(sdw_hack),
			"and the two folders are one card, not two");
	}

	// The merges that must still not happen.
	check(entry_carrying(son_gg) != entry_carrying(son_sm),
		"the .sms and the .gg of one name are different games and stay apart");
	check(entry_carrying(dd_md) != entry_carrying(dd_78),
		"the same title on two systems stays two cards, alike in every other way");
	check(entry_carrying(son_md) == entry_carrying(son_us) && entry_nvar(entry_carrying(son_md)) == 2,
		"...while two dumps on the one system are one card");
	check(entry_carrying(smw_eu) != entry_carrying(smw2),
		"a title that merely starts with another title is a game of its own");

	// Zipped ROMs: the archive is the file, not a directory.
	int somcard = entry_carrying(som_us);
	check(somcard >= 0 && somcard == entry_carrying(som_eu) && entry_nvar(somcard) == 2,
		"two archives of one game are one card");
	check(strstr(lib_view_variant_file(somcard, 0), ".zip/") != 0,
		"and a zipped file is still named through its archive");

	// Multi-disc.
	int ff7 = entry_carrying(ff7_2);
	check(ff7 >= 0 && entry_nvar(ff7) == 3, "the three discs of one game are one card");

	/*
	  ...and the same arithmetic must not swallow CD rips. Dropping the directory from the
	  key made every "Track 01.bin" on the card one title, so two games' tracks landed
	  behind one card - and none of those files would have loaded anyway, since a Mega CD
	  track is not a Genesis cartridge. Two rules, checked separately because they fix
	  different halves and either could regress alone.
	*/
	check(item_at("md", "Sonic CD/Track 01.bin") < 0 &&
		item_at("md", "Snatcher/Track 02.bin") < 0,
		"a track beside its cue is not a game at all");
	check(item_at("md", "Sonic CD/Sonic CD.cue") < 0,
		"and the cue is not one either on a system that cannot load one");

	/*
	  With no cue there is nothing to say the tracks are parts, so they are listed - and
	  then the folder has to name them, or two folders of "Track 01" are one card again.
	*/
	int keio1 = item_at("md", "Keio Flying Squadron/Track 01.bin");
	int keio2 = item_at("md", "Keio Flying Squadron/Track 02.bin");
	check(keio1 >= 0 && keio2 >= 0, "tracks with no cue are still listed");
	check(keio1 >= 0 && !strcmp(lib_item(keio1)->title, "Keio Flying Squadron"),
		"and the folder names them, not the position in the set");
	check(keio1 >= 0 && keio2 >= 0 &&
		entry_carrying(keio1) == entry_carrying(keio2),
		"so one game's tracks are one card");

	/*
	  The two ways this rule could be too greedy, both in a folder so that borrowing the
	  folder name is available and therefore visibly not taken. A title of digits alone is
	  a title - 1942, 1943, 2048 and 720 are games - and a real title merely starting with
	  one of the words is a title too.
	*/
	int n1942 = item_at("md", "Capcom/1942.bin");
	check(n1942 >= 0 && !strcmp(lib_item(n1942)->title, "1942"),
		"a title that is only a number is a title, not a part number");
	int dw = item_at("md", "Capcom/Discworld.bin");
	check(dw >= 0 && !strcmp(lib_item(dw)->title, "Discworld"),
		"and a real title starting with one of those words is left alone");
	check(strstr(lib_view_variant_file(ff7, 1), "(Disc 2)") != 0,
		"and the second of them is the second disc");

	// Cycling, and that it wraps rather than stopping.
	check(lib_view_cycle(card, 1) && lib_view_entry(card)->game == smw_jp,
		"the cycle moves the card to its next file");
	check(lib_view_cycle(card, 1) && lib_view_entry(card)->game == smw_us,
		"and to the one after that");
	check(lib_view_cycle(card, 1) && lib_view_entry(card)->game == smw_eu,
		"and wraps back to the first");

	int lone = entry_carrying(item_at("snes", "Super Metroid (Europe).sfc"));
	check(lone >= 0 && entry_nvar(lone) == 1, "a title with one file is one card");
	check(!lib_view_cycle(lone, 1), "and there is nothing to cycle on it");

	/*
	  The flag that decides whether the title block names the file. A grouped card knows
	  it needs to; these are the cards that need to and are not grouped, because grouping
	  them would have been wrong.
	*/
	check(lib_view_entry(entry_carrying(son_gg))->dup &&
		lib_view_entry(entry_carrying(son_sm))->dup &&
		lib_view_entry(entry_carrying(son_md))->dup,
		"cards left sharing a title are marked as needing their file named");
	check(!lib_view_entry(lone)->dup, "and a card with a title of its own is not");

	/*
	  ...and naming them has to answer the question, which is hardest for two files of the
	  *same name* in two folders. Since the directory left the grouping key those are one
	  card with two files, so the line cannot fall back to the filename - it has to keep the
	  folder, or the card would offer two entries that read identically and the player could
	  not tell which they were about to start.
	*/
	int sor_top = item_at("md", "Streets of Rage 2 (Europe).bin");
	int sor_sub = item_at("md", "Proto/Streets of Rage 2 (Europe).bin");
	int sor_card = entry_carrying(sor_top);
	check(sor_top >= 0 && sor_sub >= 0 && sor_card >= 0 &&
		sor_card == entry_carrying(sor_sub),
		"two files of one name in two folders are one card");

	// Copied out, because the answer is a static buffer: comparing two calls in one
	// expression compares it with itself and passes whatever it is handed.
	{
		int a = -1, b = -1;
		for (int i = 0; i < entry_nvar(sor_card); i++)
		{
			if (lib_view_variant(sor_card, i) == sor_top) a = i;
			if (lib_view_variant(sor_card, i) == sor_sub) b = i;
		}
		char sor_name[CH_PATH_LEN] = {};
		if (a >= 0) snprintf(sor_name, sizeof(sor_name), "%s", lib_view_variant_file(sor_card, a));
		printf("  same-name pair reads as: \"%s\" and \"%s\"\n",
			sor_name, (b >= 0) ? lib_view_variant_file(sor_card, b) : "");
		check(a >= 0 && b >= 0 && strcmp(sor_name, lib_view_variant_file(sor_card, b)) != 0,
			"and the two are still told apart, by the folder they are in");
	}

	/*
	  Ties. Nothing has been launched yet, so all three files are level on play count -
	  and then a favourite is the player's own word about one of them and decides it.
	  Checked here because it needs the three level, which they only are before anything
	  has been played.
	*/
	lib_toggle_fav(lib_item(smw_us));
	lib_view_build(VIEW_ALL, -1, SORT_TITLE);
	check(lib_view_entry(entry_carrying(smw_us))->game == smw_us,
		"a favourite decides which file a card shows when nothing else separates them");

	lib_toggle_fav(lib_item(smw_us));
	lib_view_build(VIEW_ALL, -1, SORT_TITLE);
	check(lib_view_entry(entry_carrying(smw_us))->game == smw_eu,
		"and the card goes back to the first file when that is undone");

	/*
	  Finding a game behind a card. Coming back to where the player was, and parking the
	  shelf on the running game, both go through these - and both used to compare against
	  one item per entry, which would now miss two files out of every three.
	*/
	lib_view_build(VIEW_ALL, -1, SORT_TITLE);
	card = entry_carrying(smw_eu);
	int at = lib_view_select_key(lib_item(smw_us)->key);
	check(at == card && lib_view_entry(at)->game == smw_us && lib_view_entry(at)->vsel == 2,
		"a game is found by key behind its card, and the card turns to it");

	lib_view_build(VIEW_ALL, -1, SORT_TITLE);
	at = lib_view_select_path(lib_item(smw_jp)->sysidx, lib_item(smw_jp)->path);
	check(at == card && lib_view_entry(at)->game == smw_jp,
		"and by system and path, which is what the running game is known by");
	check(lib_view_select_path(lib_item(smw_jp)->sysidx, "Nothing Like This.sfc") == -1,
		"a game that is not on the shelf is not found");

	/*
	  A folder's count is what the shelf behind it will show. Counted in files it would
	  promise eight games and open on six cards.
	*/
	int snes = -1;
	for (int i = 0; i < lib_sys_count(); i++) if (!strcmp(lib_sys(i)->id, "snes")) snes = i;

	int cards = lib_view_build(VIEW_SYS, snes, SORT_TITLE);
	int files = 0;
	for (int i = 0; i < lib_item_count(); i++) if (lib_item(i)->sysidx == snes) files++;

	lib_view_build(VIEW_SYSTEMS, -1, SORT_TITLE);
	int promised = -1;
	for (int i = 0; i < lib_view_count(); i++)
	{
		const chome_entry *f = lib_view_entry(i);
		if (f && f->sysidx == snes) promised = f->count;
	}
	printf("  SNES: %d files, %d cards, folder promises %d\n", files, cards, promised);
	check(files > cards, "the fake card has more SNES files than SNES titles");
	check(promised == cards, "a folder promises as many games as its shelf will show");
}

/* ------------------------------------------------------------ CD systems --- */

/*
  The four disc consoles, from the folder a download lands in to the mount the MGL asks
  for. Both halves matter and they fail differently.

  The scanning half is what makes a card appear at all. Before these rows existed a Mega
  CD, PC Engine CD or Neo Geo CD image on the card was invisible to the shelf, because the
  only system pointing at a .cue was PlayStation - so the copies the rip feature wrote were
  correct and unreachable. The fixtures are laid out the way the real folders are, so what
  is checked is the layout a player will actually have rather than a convenient one.

  The route half is the riskier one and cannot be seen on screen. type and index come from
  each core's CONF_STR, and a wrong index does not draw wrong or refuse - it mounts the
  file into some other input of the right core and the game simply never starts. There is
  nothing to observe, so it is asserted against the table directly, field by field, with
  the source of each value written in the row's own comment in chome_lib.cpp.

  The BIOS assertions are the ones to keep. Every one of these folders holds the core's own
  boot ROM at its root - that is where the cores look for it - so an extension list widened
  by one word turns a BIOS into a card that scrapes, sorts and sits on the shelf like a
  game and then cannot boot. The two .bin ones sit in a folder with no .cue in it on
  purpose: 73b0f71's rule hides a .bin beside a sheet, so a .bin BIOS placed next to one
  would keep this passing while the hole it exists for was open.
*/
static void assert_cd_systems()
{
	printf("\n== the CD systems ==\n");

	struct route { const char *id, *name, *dir, *rbf; char type; int index; int ss; };
	static const route want[] =
	{
		// "S0,CUECHD,Insert Disk" in MegaCD.sv, and a core nobody has read for save states.
		{ "megacd",   "Mega CD",      "MegaCD",    "_Console/MegaCD",       's', 0, CH_SS_UNKNOWN },
		// "S0,CUECHD,Insert CD" in TurboGrafx16.sv - the same rbf as tg16, told apart by the
		// type, so it inherits tg16's measured CH_SS_NO rather than guessing.
		{ "pcecd",    "PC Engine CD", "TGFX16-CD", "_Console/TurboGrafx16", 's', 0, CH_SS_NO      },
		// "S1,CUECHD,Load CD Image" in neogeo.sv. Index 1 is also the romset slot, which is
		// type 'f' - menu.cpp routes an 's' mount here to neocd_set_image().
		{ "neogeocd", "Neo Geo CD",   "NeoGeo-CD", "_Console/NeoGeo",       's', 1, CH_SS_NO      },
		// Index 0 from menu.cpp, not from a .sv: saturn_set_image() is called when
		// ioctl_index is 0 and saturn_mount_save() for anything else, and index 1 is marked
		// SCANO_SAVES by the browser. So index 1 is the backup RAM, not the disc.
		{ "saturn",   "Saturn",       "Saturn",    "_Console/Saturn",       's', 0, CH_SS_UNKNOWN },
	};

	for (unsigned i = 0; i < sizeof(want) / sizeof(want[0]); i++)
	{
		const route *w = &want[i];
		int sx = -1;
		for (int j = 0; j < lib_sys_count(); j++)
			if (!strcmp(lib_sys(j)->id, w->id)) sx = j;

		char msg[160];
		snprintf(msg, sizeof(msg), "%s is a system on the shelf", w->name);
		check(sx >= 0, msg);
		if (sx < 0) continue;

		const chome_sys *s = lib_sys(sx);
		printf("  %-9s %-11s %-22s type=%c index=%d ext=%s\n",
			s->id, s->dir, s->rbf, s->type, s->index, s->ext);

		snprintf(msg, sizeof(msg), "%s reads the official %s folder", w->name, w->dir);
		check(!strcmp(s->dir, w->dir), msg);

		snprintf(msg, sizeof(msg), "%s launches %s", w->name, w->rbf);
		check(!strcmp(s->rbf, w->rbf), msg);

		/*
		  Written as one check per field rather than one per row, because these are the
		  three values a mistake in cannot be seen: the right core comes up and mounts
		  nothing. The message names the value so a failure says which one moved.
		*/
		snprintf(msg, sizeof(msg), "%s mounts its image rather than loading it to memory", w->name);
		check(s->type == w->type, msg);

		snprintf(msg, sizeof(msg), "%s mounts into slot %d", w->name, w->index);
		check(s->index == w->index, msg);

		snprintf(msg, sizeof(msg), "%s takes cue and chd and nothing else", w->name);
		check(!strcmp(s->ext, "cue,chd"), msg);

		snprintf(msg, sizeof(msg), "%s titles files by name, not through romsets.xml", w->name);
		check(s->romset == 0, msg);

		snprintf(msg, sizeof(msg), "%s says %s about save states", w->name,
			w->ss == CH_SS_UNKNOWN ? "nothing" : "no");
		check(s->savestates == w->ss, msg);
	}

	lib_view_build(VIEW_ALL, -1, SORT_TITLE);

	/* ------------------------------------------- the regional-subfolder case --- */

	/*
	  What his own card has: a downloaded Mega CD set files each release under Europe/,
	  Japan/ or USA/, so one game is the same filename in two directories. The folder left
	  the grouping key in 97775f0 precisely so this is one card - and the check is here
	  because the folder coming back would show up as duplicate cards for half his library.
	*/
	{
		int eu = item_at("megacd", "Europe/Lunar - Eternal Blue.cue");
		int us = item_at("megacd", "USA/Lunar - Eternal Blue.cue");
		check(eu >= 0 && us >= 0, "a Mega CD game filed under Europe/ and USA/ is indexed twice");
		if (eu >= 0 && us >= 0)
		{
			int card = entry_carrying(eu);
			check(card >= 0 && card == entry_carrying(us),
				"and the two regions are one card, not two");
			check(entry_nvar(card) == 2, "with both releases behind it");
		}
	}

	/* ------------------------------------------------ a sheet and its tracks --- */

	/*
	  The shape a rip writes, now read back by the system the rip goes to. 73b0f71's rule
	  does the hiding and it is not conditional on the system accepting "bin" - which is
	  what makes it right here, where the row accepts no "bin" at all and the tracks would
	  be invisible either way. Asserting it anyway is what keeps that true if the row ever
	  changes.
	*/
	{
		int cue = item_at("megacd", "Silpheed/Silpheed.cue");
		check(cue >= 0, "a folder holding a sheet and its tracks is on the shelf");
		check(item_at("megacd", "Silpheed/Track 01.bin") < 0,
			"and its first track is not a game of its own");
		check(item_at("megacd", "Silpheed/Track 02.bin") < 0, "nor its second");
		if (cue >= 0) check(entry_nvar(entry_carrying(cue)) == 1,
			"so the folder is exactly one card");
	}

	/* --------------------------------------------------------- and the BIOS --- */

	/*
	  Four boot ROMs, in the three folders the cores read them from. None of them is a
	  game, and the way each of these fails is the same: one more word in a row's extension
	  list and the shelf grows a card that cannot be pressed.
	*/
	check(item_at("pcecd", "cd_bios.rom") < 0,
		"the PC Engine CD BIOS is not a game");
	check(item_at("neogeocd", "neocd.bin") < 0,
		"nor the Neo Geo CD BIOS");
	check(item_at("neogeocd", "top-sp1.bin") < 0,
		"nor the Neo Geo top-sp1 BIOS beside it");
	check(item_at("neogeocd", "uni-bioscd.rom") < 0,
		"nor the universal Neo Geo CD BIOS");
	check(item_at("saturn", "boot.rom") < 0,
		"nor the Saturn boot ROM");

	// ...and the games in those same folders are, so the checks above are not passing
	// because the folder was never scanned.
	check(item_at("pcecd", "Rondo of Blood/Rondo of Blood.cue") >= 0,
		"while the PC Engine CD game in that folder is");
	check(item_at("neogeocd", "Samurai Shodown RPG/Samurai Shodown RPG.cue") >= 0,
		"and so is the Neo Geo CD one");

	/* ------------------------------------------------------ a two-disc game --- */

	/*
	  His Saturn folder, which is where this started: "Deep Fear (Europe) (Disc 1)" and
	  "(Disc 2)". clean_title() drops both brackets, so the two discs collide on "Deep
	  Fear" and group the way the PlayStation set above does.
	*/
	{
		int d1 = item_at("saturn", "Deep Fear (Europe) (Disc 1).cue");
		int d2 = item_at("saturn", "Deep Fear (Europe) (Disc 2).cue");
		check(d1 >= 0 && d2 >= 0, "both discs of a Saturn game are indexed");
		if (d1 >= 0 && d2 >= 0)
		{
			int card = entry_carrying(d1);
			check(card >= 0 && card == entry_carrying(d2), "and they are one card");
			check(entry_nvar(card) == 2, "with two discs to cycle");
		}
	}
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
	/*
	  Eight, and every one of them is named: three in our own art folder (Super
	  Metroid, Super Mario World, Sonic 2 Europe - the last of those through a stale
	  gamelist entry that has to fall through to it), four from the two gamelists
	  (Super Metroid again but a different picture, Secret of Mana, and Streets of
	  Rage 2 twice - the second by filename, from the Proto folder), and Tetris from
	  the Skraper media folder. assert_gamelist() below is where each of those is
	  told apart from the others.
	*/
	check(ready == 8, "exactly the eight covers on the fake SD decoded");
	check(missing >= 8, "everything else reports missing for the fallback card");

	int w = 0, h = 0;
	const uint32_t *a = 0;
	for (int i = 0; i < lib_item_count() && !a; i++) a = art_get(i, &w, &h);
	check(a != 0, "a decoded cover is retrievable");
	check(w == theme_get()->sel_w && h == theme_get()->sel_h, "cover decoded at card size");
}

/* ------------------------------------------------------------- gamelist --- */

static int sysidx_by_dir(const char *dir)
{
	for (int i = 0; i < lib_sys_count(); i++)
	{
		const chome_sys *s = lib_sys(i);
		if (s && !strcmp(s->dir, dir)) return i;
	}
	return -1;
}

static int item_by_path(const char *sysdir, const char *relpath)
{
	int sys = sysidx_by_dir(sysdir);
	for (int i = 0; i < lib_item_count(); i++)
	{
		chome_item *it = lib_item(i);
		if (it && it->sysidx == sys && !strcmp(it->path, relpath)) return i;
	}
	return -1;
}

/*
  Which picture a card ended up with, read off the card itself - the middle pixel of
  the decoded cover. make_cover() paints a flat plate with a band along the bottom
  and two diagonals, and neither of those goes near the middle of a 400x600 image,
  so the middle pixel is the plate colour and says which file was decoded. That is
  the only way from here to tell "found the right art" from "found some art".
*/
static int cover_colour(int item, uint32_t *out)
{
	int w = 0, h = 0;
	const uint32_t *a = art_get(item, &w, &h);
	if (!a || w < 8 || h < 8) return 0;
	*out = a[(h / 2) * w + (w / 2)] | 0xff000000u;
	return 1;
}

static int colour_near(uint32_t a, uint32_t b, int tol)
{
	for (int s = 0; s <= 16; s += 8)
	{
		int d = (int)((a >> s) & 0xff) - (int)((b >> s) & 0xff);
		if (d < 0) d = -d;
		if (d > tol) return 0;
	}
	return 1;
}

static int cover_is(int item, uint32_t want, const char *what)
{
	uint32_t got = 0;
	if (!cover_colour(item, &got))
	{
		printf("  %s: no decoded cover at all\n", what);
		return 0;
	}
	if (!colour_near(got, want, 24))
	{
		printf("  %s: cover is %06x, wanted %06x\n", what, got & 0xffffff, want & 0xffffff);
		return 0;
	}
	return 1;
}

// Re-decodes everything from scratch, the way Options > Rescan Library does.
static void art_redo()
{
	art_shutdown();
	art_init(theme_get()->sel_w, theme_get()->sel_h);
	for (int i = 0; i < lib_item_count(); i++) art_request(i, 0);
	for (int i = 0; i < lib_item_count() * 2 + 10; i++) art_step();
}

static void assert_gamelist()
{
	printf("\n== gamelist.xml ==\n");

	int snes = sysidx_by_dir("SNES");
	int md   = sysidx_by_dir("Genesis");
	int tg   = sysidx_by_dir("TGFX16");
	int gba  = sysidx_by_dir("GBA");
	int psx  = sysidx_by_dir("PSX");

	int metroid = item_by_path("SNES", "Super Metroid (Europe).sfc");
	int mana    = item_by_path("SNES", "Secret of Mana (USA).zip/Secret of Mana (USA).sfc");
	int smwjp   = item_by_path("SNES", "Super Mario World (Japan).sfc");
	int sor2    = item_by_path("Genesis", "Streets of Rage 2 (Europe).bin");
	int sor2p   = item_by_path("Genesis", "Proto/Streets of Rage 2 (Europe).bin");
	int sonusa  = item_by_path("Genesis", "Sonic The Hedgehog 2 (USA).md");
	int soneur  = item_by_path("Genesis", "Sonic The Hedgehog 2 (Europe).md");
	int ddragon = item_by_path("Genesis", "Double Dragon (Europe).bin");
	int bonk    = item_by_path("TGFX16", "Bonk's Adventure (USA).pce");
	int fusion  = item_by_path("GBA", "Metroid Fusion (Europe).gba");
	int tetris  = item_by_path("GAMEBOY", "Tetris (World).gb");

	check(metroid >= 0 && mana >= 0 && sor2 >= 0 && sor2p >= 0 && sonusa >= 0 &&
		soneur >= 0 && ddragon >= 0 && bonk >= 0 && fusion >= 0 && tetris >= 0 && smwjp >= 0,
		"every game the gamelist fixtures talk about is in the index");

	printf("  entries: SNES %d, Genesis %d, TGFX16 %d, GBA %d\n",
		gl_count(snes), gl_count(md), gl_count(tg), gl_count(gba));

	/*
	  Two, from four <game> elements and a <folder>: the folder is not a card, and the
	  game whose only media are a video and a logo has no cover to offer. Reading
	  either of those would show up here as three or four.
	*/
	check(gl_count(snes) == 2, "only <game> elements that name a picture are taken");
	check(gl_rejected(snes) == 0, "a well-formed gamelist is not rejected");

	// The whole point: the player's own scrape beats the local art pack.
	check(cover_is(metroid, 0xff9a2f2f, "Super Metroid"),
		"gamelist art wins over the same game's cover in the art folder");

	// <boxart> is the box; <image> in the same entry is a composite and must lose.
	check(cover_is(mana, 0xff2f9a4f, "Secret of Mana"),
		"<boxart> beats <image>, and an archive is matched by the archive's name");

	check(art_state(smwjp) == ART_MISSING,
		"a <video> and a <marquee> are not covers");

	/*
	  Three entries out of four: the one naming a path under "~" is dropped when the
	  file is read, not when it is looked up, so it never reaches the table.
	*/
	check(gl_count(md) == 3, "an entry whose picture is under ~/ is not stored at all");
	check(art_state(ddragon) == ART_MISSING, "and that game gets no cover");

	check(cover_is(sor2, 0xff7040b0, "Streets of Rage 2"),
		"a CRLF gamelist with no ./ prefix on its paths still resolves");
	check(cover_is(sonusa, 0xffb08040, "Sonic 2 USA"),
		"&amp; in a picture path decodes to the file that is really there");

	// The gamelist names a file that is not on the card, so the art pack still wins.
	check(cover_is(soneur, 0xff2b4c7e, "Sonic 2 Europe"),
		"a gamelist entry naming a missing file falls through to the art folder");

	/*
	  Only the root copy of this game is in the gamelist; the one in Proto/ is found
	  by its filename alone. That fallback is what saves a gamelist whose <path> is
	  absolute, and this is the check that it is there.
	*/
	check(cover_is(sor2p, 0xff7040b0, "Streets of Rage 2 (Proto)"),
		"a game the gamelist does not name by path is still matched by filename");

	// Malformed: refused whole, including the entry that parsed cleanly before it.
	check(gl_rejected(tg) == 1, "a malformed gamelist is reported as rejected");
	check(gl_count(tg) == 0, "and nothing it said is kept, not even the good entry");
	check(art_state(bonk) == ART_MISSING,
		"a malformed gamelist degrades to no art, though the file it named exists");

	// Too large to be a gamelist: refused on its size, before the parser sees it.
	check(gl_rejected(gba) == 1, "an oversized gamelist is refused");
	check(gl_count(gba) == 0, "and contributes nothing");
	check(art_state(fusion) == ART_MISSING, "so that game gets no cover either");

	// The Skraper media folders, on a system with no gamelist.
	check(gl_loaded(psx) && gl_count(psx) == 0 && gl_rejected(psx) == 0,
		"a system with no gamelist.xml is looked at, empty, and not rejected");
	check(cover_is(tetris, 0xff30a070, "Tetris"),
		"media/box2d beside the ROMs is found without any gamelist at all");

	/*
	  And the off switch, which is the escape hatch for a scrape whose pictures are
	  worse than the local pack: with it off, Super Metroid goes back to the cover in
	  the art folder.
	*/
	cfg.classicui_gamelist = 0;
	art_redo();
	check(cover_is(metroid, 0xff5b4b8a, "Super Metroid, gamelist off"),
		"classicui_gamelist=0 puts the art folder back in charge");
	check(art_state(sonusa) == ART_MISSING,
		"and a game whose only art was in the gamelist has none");

	cfg.classicui_gamelist = 1;
	art_redo();
	check(cover_is(metroid, 0xff9a2f2f, "Super Metroid, gamelist on again"),
		"and turning it back on restores the scraped art");
}

/* ---------------------------------------------------------- physical disc --- */

/*
  Identification, tested against discs nobody has to own.

  Every one of these is a real signature at a real offset (sources in chome_disc.h),
  laid out here the way a pressed disc lays it out: a raw sector is a 16-byte
  sync/header followed by the 2048-byte user area, or 24 bytes of it for the mode 2
  form CD-i uses. So a fixture describes the user area and the reader below builds the
  raw sector around it - which means a mistake about that offset shows up as a failed
  identification rather than as a test that agrees with the bug.

  What this cannot test is the drive: disc_poll() is a status ioctl and is compiled
  out of the harness. What it *can* test is the two-phase state machine, because that
  was deliberately split out of the ioctl path - see disc_ingest_present().
*/
struct fake_sector
{
	int lba;
	int mode2;                 // user area at offset 24 rather than 16
	uint8_t user[DISC_USER_SIZE];
};

struct fake_disc
{
	fake_sector sec[16];
	int n;
};

static int fake_read(int lba, int mode, uint8_t *dst, void *ctx)
{
	fake_disc *d = (fake_disc*)ctx;

	for (int i = 0; i < d->n; i++)
	{
		if (d->sec[i].lba != lba) continue;

		if (mode == DISC_READ_USER)
		{
			memcpy(dst, d->sec[i].user, DISC_USER_SIZE);
			return 0;
		}

		// Build the raw sector the way a real one is laid out.
		memset(dst, 0, DISC_RAW_SIZE);
		int off = d->sec[i].mode2 ? 24 : 16;
		int room = DISC_RAW_SIZE - off;
		memcpy(dst + off, d->sec[i].user, room < DISC_USER_SIZE ? (size_t)room : DISC_USER_SIZE);
		return 0;
	}

	return -1;                 // no such sector, which is normal while probing
}

static void fake_put(fake_disc *d, int lba, int mode2, const void *data, int len, int at)
{
	fake_sector *s = 0;
	for (int i = 0; i < d->n; i++) if (d->sec[i].lba == lba) s = &d->sec[i];
	if (!s)
	{
		if (d->n >= (int)(sizeof(d->sec) / sizeof(d->sec[0]))) return;
		s = &d->sec[d->n++];
		memset(s, 0, sizeof(*s));
		s->lba = lba;
		s->mode2 = mode2;
	}
	if (data && len > 0 && at + len <= DISC_USER_SIZE) memcpy(s->user + at, data, len);
}

// A minimal but structurally valid ISO: primary volume descriptor plus a root
// directory holding the given names.
static void fake_iso(fake_disc *d, int lba0, const char *label,
	const char *sig, const char *const *names, int nnames)
{
	uint8_t pvd[DISC_USER_SIZE];
	memset(pvd, 0, sizeof(pvd));

	pvd[0] = 1;
	memcpy(pvd + 1, "CD001", 5);
	if (sig) memcpy(pvd + 8, sig, strlen(sig));

	// Volume label: 32 bytes, space padded.
	memset(pvd + 40, ' ', 32);
	if (label) memcpy(pvd + 40, label, strlen(label) > 32 ? 32 : strlen(label));

	// Root directory record: length, then extent and size as little-endian 32.
	const uint32_t extent = 24;
	const uint32_t size = DISC_USER_SIZE;
	pvd[156] = 34;
	pvd[156 + 2] = (uint8_t)(extent & 0xff);
	pvd[156 + 3] = (uint8_t)((extent >> 8) & 0xff);
	pvd[156 + 10] = (uint8_t)(size & 0xff);
	pvd[156 + 11] = (uint8_t)((size >> 8) & 0xff);

	fake_put(d, lba0 + 16, 0, pvd, sizeof(pvd), 0);

	// The directory itself: a chain of length-prefixed records.
	uint8_t dir[DISC_USER_SIZE];
	memset(dir, 0, sizeof(dir));
	int off = 0;
	for (int i = 0; i < nnames; i++)
	{
		int nlen = (int)strlen(names[i]);
		if (!nlen) continue;
		int rlen = 33 + nlen;
		if (rlen & 1) rlen++;                     // records are even-aligned
		if (off + rlen > DISC_USER_SIZE) break;
		dir[off] = (uint8_t)rlen;
		dir[off + 32] = (uint8_t)nlen;
		memcpy(dir + off + 33, names[i], nlen);
		off += rlen;
	}
	fake_put(d, lba0 + (int)extent, 0, dir, sizeof(dir), 0);
}

// Declared up with walk_profile(), which is what calls it.
static void walk_disc_dialog(const char *tag)
{
	const char *tdb = ROOT "/classicui/disctitles.txt";
	char name[128];

	mkpath(ROOT "/classicui");
	{
		FILE *f = fopen(tdb, "wb");
		if (f)
		{
			fprintf(f, "#classicui-disctitles 1\n");
			fprintf(f, "SLES01506\tMetal Gear Solid\n");
			fclose(f);
		}
	}
	disc_titles_forget();

	cfg.classicui_disc = 1;

	fake_disc d; memset(&d, 0, sizeof(d));
	static const char *const none[] = { "" };
	fake_iso(&d, 0, "PLAYSTATION", "PLAYSTATION", none, 0);
	fake_put(&d, 20, 0, "BOOT = cdrom:\\SLES_015.06;1", 27, 100);

	disc_ingest_present(1);
	disc_set_reader(fake_read, &d);
	disc_ingest_identify(0);
	frame(6);

	press(KEY_UP, 18);
	press(KEY_ENTER, 20);
	snprintf(name, sizeof(name), "%s-11-disc", tag);
	dump(name);

	// Not a check - assert_disc_hires() does the checking, on the one canvas it pins. This is
	// so that the four numbers this dialog actually resolves the disc to are in the log beside
	// the four pictures, which is what anyone looking at them wants to know.
	printf("  disc drawn %d px across, face buffer %d\n", disc_drawn_dia(), gfx_disc_face_dia());

	press(KEY_RIGHT, 12);                 // the second button
	snprintf(name, sizeof(name), "%s-11b-disc-options", tag);
	dump(name);

	press(KEY_ENTER, 20);                 // the core chooser it opens
	snprintf(name, sizeof(name), "%s-12-disc-cores", tag);
	dump(name);

	for (int i = 0; i < 3; i++) press(KEY_ESC, 10);

	/*
	  The reader points at a local, so it has to go before this returns or the next
	  disc_poll() would read a dead stack frame.
	*/
	disc_reset_reader();
	disc_ingest_present(0);
	(void)disc_take_dirty();
	frame(6);
	cfg.classicui_disc = 0;

	unlink(tdb);
	disc_titles_forget();
}

static void assert_physical_disc()
{
	printf("\n== physical disc: detection and identification ==\n");

	const int L = 0;                              // first data track at LBA 0

	/* --------------------------------------------------- identification --- */

	{
		fake_disc d; memset(&d, 0, sizeof(d));
		fake_put(&d, L, 0, "SEGADISCSYSTEM", 14, 0);
		disc_set_reader(fake_read, &d);
		check(disc_identify_at(L) == DISC_T_MEGACD, "SEGADISCSYSTEM is a Mega CD disc");
	}

	{
		fake_disc d; memset(&d, 0, sizeof(d));
		fake_put(&d, L, 0, "SEGA SEGASATURN", 15, 0);
		disc_set_reader(fake_read, &d);
		check(disc_identify_at(L) == DISC_T_SATURN, "SEGA SEGASATURN is a Saturn disc");
	}

	{
		fake_disc d; memset(&d, 0, sizeof(d));
		uint8_t sig[6] = { 0x01, 0x5A, 0x5A, 0x5A, 0x5A, 0x5A };
		fake_put(&d, L, 0, sig, sizeof(sig), 0);
		disc_set_reader(fake_read, &d);
		check(disc_identify_at(L) == DISC_T_3DO, "the 0x5A run is a 3DO disc");
	}

	/*
	  What a Sega disc says about itself, and which of the two names we keep.

	  The bytes are the real ones, copied off the Sega Rally Championship disc in the
	  drive on 2026-08-11 - dd if=/dev/sr0 - rather than composed to fit the parser:

	    0x00 "SEGA SEGASATURN "   0x10 "SEGA ENTERPRISES"
	    0x20 "MK-81207  "         0x2A "V1.000"   0x30 "19951218"
	    0x60 "SEGA RALLY CHAMPIONSHIP"

	  Two separate defects were found here and both are asserted, because either one
	  alone leaves a physical Sega disc unidentified:

	  1. The serial was never read. The helper called disc_serial_at() - the PlayStation
	     reader, which walks sectors 16..64 for Sony prefixes - instead of the dispatcher
	     disc_serial_for(). On this disc it found nothing, correctly, and wrote "". So
	     no physical Saturn or Mega CD disc has ever produced a serial, while the readers
	     for both sat in the file unused.

	  2. The name came from the ISO volume id, which for Saturn is the worse of the two
	     strings and sometimes is not there at all. Measured against ScreenScraper over
	     the 37 Saturn discs on the card: volume id 23/37, header title 30/37, and seven
	     discs have no volume id whatsoever - Daytona USA among them.
	*/
	{
		fake_disc d; memset(&d, 0, sizeof(d));
		fake_put(&d, L, 0, "SEGA SEGASATURN SEGA ENTERPRISES", 32, 0);
		fake_put(&d, L, 0, "MK-81207  V1.000", 16, 0x20);
		fake_put(&d, L, 0, "19951218", 8, 0x30);
		fake_put(&d, L, 0, "SEGA RALLY CHAMPIONSHIP", 23, 0x60);
		disc_set_reader(fake_read, &d);

		char ser[DISC_SERIAL_LEN] = {};
		check(disc_serial_for(DISC_T_SATURN, L, ser, sizeof(ser)) > 0 && !strcmp(ser, "MK-81207"),
			"a Saturn disc's product number is read through the dispatcher");

		ser[0] = 0;
		check(disc_serial_at(L, ser, sizeof(ser)) == 0 && !ser[0],
			"and the PlayStation reader finds nothing on it, which is what the helper used to call");

		char t[DISC_LABEL_LEN] = {};
		check(disc_title_at(DISC_T_SATURN, L, t, sizeof(t)) > 0
			&& !strcmp(t, "SEGA RALLY CHAMPIONSHIP"),
			"the title comes out of the disc's own header, spaces and all");

		check(disc_title_at(DISC_T_PSX, L, t, sizeof(t)) == 0,
			"and nothing is claimed for a PlayStation disc, which has no such header");
	}

	/*
	  A Saturn disc with no ISO volume id at all - seven of the thirty-seven on the card
	  are like this, so it is the normal case rather than a corrupt one. Before the title
	  reader these were nameless on the shelf.
	*/
	{
		fake_disc d; memset(&d, 0, sizeof(d));
		fake_put(&d, L, 0, "SEGA SEGASATURN SEGA ENTERPRISES", 32, 0);
		fake_put(&d, L, 0, "MK-81200  ", 10, 0x20);
		fake_put(&d, L, 0, "DAYTONA USA", 11, 0x60);
		disc_set_reader(fake_read, &d);

		char lbl[DISC_LABEL_LEN] = {};
		check(disc_label_at(L, lbl, sizeof(lbl)) == 0,
			"there is no ISO volume descriptor to read a label from");

		check(disc_title_at(DISC_T_SATURN, L, lbl, sizeof(lbl)) > 0 && !strcmp(lbl, "DAYTONA USA"),
			"and the header still names the game, which is the whole point of preferring it");
	}

	/*
	  And the whole way through disc_ingest_identify(), which is what actually decides the
	  name and serial a disc is known by - the checks above only prove the readers work.

	  Worth doing separately because the divergence between this path and the helper's is
	  what let the serial bug ship: this copy always called disc_serial_for() and so was
	  always right, while the helper called the PlayStation reader. A test of the readers
	  alone would have passed in both worlds.
	*/
	{
		fake_disc d; memset(&d, 0, sizeof(d));
		fake_put(&d, 0, 0, "SEGA SEGASATURN SEGA ENTERPRISES", 32, 0);
		fake_put(&d, 0, 0, "MK-81307  ", 10, 0x20);
		fake_put(&d, 0, 0, "J:AZEL PANZER DRAGOON RPG", 25, 0x60);
		// ...and a volume id that is real, so this proves an order and not just a fallback.
		static const char *const none[] = { "" };
		fake_iso(&d, 0, "AZEL_1", "AZEL_1", none, 0);
		fake_put(&d, 0, 0, "SEGA SEGASATURN SEGA ENTERPRISES", 32, 0);
		fake_put(&d, 0, 0, "MK-81307  ", 10, 0x20);
		fake_put(&d, 0, 0, "J:AZEL PANZER DRAGOON RPG", 25, 0x60);

		disc_ingest_present(1);
		disc_set_reader(fake_read, &d);
		disc_ingest_identify(0);

		check(disc_type() == DISC_T_SATURN, "the ingest path calls it a Saturn disc");
		check(!strcmp(disc_serial(), "MK-81307"),
			"and gives it the product number the disc carries, which it never used to");
		check(!strcmp(disc_label(), "J:AZEL PANZER DRAGOON RPG"),
			"and the disc's own title rather than the AZEL_1 the volume id would have given");

		disc_ingest_present(0);
	}

	/*
	  Asking again when the first ask never reached the network.

	  The bug this guards was measured on the device, not imagined: a Saturn disc left in
	  the drive across a reboot is identified within seconds, before Wi-Fi has associated,
	  so the one attempt per key is spent on a host that will not resolve -

	      the disc scan query for MK-81207 failed, curl exit 6 (host would not resolve)

	  - and since nothing re-triggers a prefetch for a disc that is just sitting there, the
	  art could never arrive for as long as the machine stayed up. The query itself was
	  correct. Only the clock was wrong.

	  The decision is pure so it can be checked without waiting: same shape as
	  disc_probe_due() next door.
	*/
	{
		check(disc_retry_wait_s(0) == 10 && disc_retry_wait_s(4) == 300,
			"the retry backoff starts short and grows, for a Wi-Fi association not a dead network");
		check(disc_retry_wait_s(5) < 0,
			"and runs out, so a machine with no network stops knocking");

		check(!disc_retry_due_at(0, 0, 1000), "nothing armed is never due");
		check(!disc_retry_due_at(1, 1000, 1009), "the first retry is not due after nine seconds");
		check(disc_retry_due_at(1, 1000, 1010), "and is due at ten");
		check(!disc_retry_due_at(2, 1000, 1029) && disc_retry_due_at(2, 1000, 1030),
			"the second waits thirty, not another ten");
		check(!disc_retry_due_at(6, 1000, 99999),
			"and past the last one nothing is ever due again, however long it has been");
	}

	// Mega CD, where the international title wins and a Japanese disc falls back to the
	// domestic one rather than to a blank.
	{
		fake_disc d; memset(&d, 0, sizeof(d));
		fake_put(&d, L, 0, "SEGADISCSYSTEM", 14, 0);
		fake_put(&d, L, 0, "SONIC THE HEDGEHOG CD", 21, 0x120);
		fake_put(&d, L, 0, "SONIC CD", 8, 0x150);
		disc_set_reader(fake_read, &d);

		char t[DISC_LABEL_LEN] = {};
		check(disc_title_at(DISC_T_MEGACD, L, t, sizeof(t)) > 0 && !strcmp(t, "SONIC CD"),
			"a Mega CD disc gives its international title");

		fake_disc j; memset(&j, 0, sizeof(j));
		fake_put(&j, L, 0, "SEGADISCSYSTEM", 14, 0);
		fake_put(&j, L, 0, "LUNAR THE SILVER STAR", 21, 0x120);
		disc_set_reader(fake_read, &j);

		t[0] = 0;
		check(disc_title_at(DISC_T_MEGACD, L, t, sizeof(t)) > 0
			&& !strcmp(t, "LUNAR THE SILVER STAR"),
			"and one that left the international field blank falls back to the domestic name");
	}

	{
		fake_disc d; memset(&d, 0, sizeof(d));
		static const char *const none[] = { "" };
		fake_iso(&d, L, "PLAYSTATION", "PLAYSTATION", none, 0);
		fake_put(&d, L + 20, 0, "BOOT = cdrom:\\SLUS_006.26;1", 27, 100);
		disc_set_reader(fake_read, &d);

		check(disc_identify_at(L) == DISC_T_PSX, "PLAYSTATION in the volume descriptor is a PSX disc");

		char ser[DISC_SERIAL_LEN] = {};
		check(disc_serial_at(L, ser, sizeof(ser)) > 0 && !strcmp(ser, "SLUS-00626"),
			"and SLUS_006.26 comes out as SLUS-00626, the form the outside world uses");

		char lbl[DISC_LABEL_LEN] = {};
		check(disc_label_at(L, lbl, sizeof(lbl)) == 0,
			"the useless PLAYSTATION volume label is refused rather than displayed");
	}

	{
		fake_disc d; memset(&d, 0, sizeof(d));
		fake_put(&d, L, 0, "NOTHINGUSEFUL", 13, 0);
		fake_put(&d, L + 1, 0, "xx PC Engine CD-ROM SYSTEM xx", 29, 40);
		disc_set_reader(fake_read, &d);
		check(disc_identify_at(L) == DISC_T_PCECD,
			"the PC Engine string is found in the second sector, not just the first");
	}

	{
		fake_disc d; memset(&d, 0, sizeof(d));
		static const char *const none[] = { "" };
		fake_iso(&d, L, "NEOGEO CD", "NGCD", none, 0);
		disc_set_reader(fake_read, &d);
		check(disc_identify_at(L) == DISC_T_NEOGEO, "NGCD is a Neo Geo CD disc");
	}

	{
		// The other Neo Geo tell: an IPL.TXT, with no NGCD signature at all.
		fake_disc d; memset(&d, 0, sizeof(d));
		static const char *const none[] = { "" };
		fake_iso(&d, L, "SNK", 0, none, 0);
		fake_put(&d, L + 18, 0, "IPL.TXT", 7, 300);
		disc_set_reader(fake_read, &d);
		check(disc_identify_at(L) == DISC_T_NEOGEO, "an IPL.TXT alone identifies a Neo Geo CD");
	}

	{
		fake_disc d; memset(&d, 0, sizeof(d));
		uint8_t cdi[8] = { 0x01, 'C', 'D', '-', 'I', ' ', ' ', ' ' };
		// mode 2: the descriptor sits 24 bytes into the raw sector, not 16.
		fake_put(&d, L + 16, 1, cdi, sizeof(cdi), 0);
		disc_set_reader(fake_read, &d);
		check(disc_identify_at(L) == DISC_T_CDI,
			"a CD-i descriptor is found at the mode 2 offset, which is not the mode 1 one");
	}

	{
		// Mega Drive+: the *pair* is the format, so neither file alone counts.
		fake_disc d; memset(&d, 0, sizeof(d));
		static const char *const pair[] = { "SONIC.MD;1", "SONIC.CUE;1" };
		fake_iso(&d, L, "MDPLUS", 0, pair, 2);
		disc_set_reader(fake_read, &d);
		check(disc_identify_at(L) == DISC_T_MDPLUS, "matching .md and .cue names are a Mega Drive+ disc");

		fake_disc d2; memset(&d2, 0, sizeof(d2));
		static const char *const lone[] = { "SONIC.MD;1" };
		fake_iso(&d2, L, "MDPLUS", 0, lone, 1);
		disc_set_reader(fake_read, &d2);
		check(disc_identify_at(L) != DISC_T_MDPLUS, "a .md with no matching .cue is not one");
	}

	{
		fake_disc d; memset(&d, 0, sizeof(d));
		static const char *const sfc[] = { "GAME.SFC;1" };
		fake_iso(&d, L, "MSU1", 0, sfc, 1);
		disc_set_reader(fake_read, &d);
		check(disc_identify_at(L) == DISC_T_SNES, "a .sfc on the disc is an MSU-1 SNES disc");
	}

	{
		fake_disc d; memset(&d, 0, sizeof(d));
		static const char *const none[] = { "" };
		fake_iso(&d, L, "SONIC_CD", 0, none, 0);
		disc_set_reader(fake_read, &d);

		char lbl[DISC_LABEL_LEN] = {};
		check(disc_label_at(L, lbl, sizeof(lbl)) > 0 && !strcmp(lbl, "SONIC CD"),
			"an underscore in the volume label becomes a space");
	}

	{
		// Nothing readable: the drive is there, the disc is not one we know.
		fake_disc d; memset(&d, 0, sizeof(d));
		disc_set_reader(fake_read, &d);
		check(disc_identify_at(L) == DISC_T_UNKNOWN, "a disc matching nothing is UNKNOWN, not NONE");
		char ser[DISC_SERIAL_LEN] = {};
		check(disc_serial_at(L, ser, sizeof(ser)) == 0, "and has no serial");
	}

	// No data track at all. Decided from the table of contents, before any read.
	check(disc_identify_at(-1) == DISC_T_AUDIO, "a disc with no data track is an audio CD");

	/* ---------------------------------------------------- what it maps to --- */

	check(!strcmp(disc_system_id(DISC_T_PSX), "psx"), "a PSX disc maps to the psx shelf");
	check(!strcmp(disc_system_id(DISC_T_PCECD), "tg16"), "a PC Engine CD maps to tg16");
	check(!strcmp(disc_system_id(DISC_T_MEGACD), "md"), "a Mega CD maps to the Mega Drive core");

	/*
	  The ones that matter for the UI: identified, but with no core this firmware can hand
	  the *drive* to. "We know what it is" and "we can launch it" are different questions,
	  and this is the case that forces the player to be asked.

	  Saturn is the one to read carefully, because it is now a shelf system - a .cue in
	  games/Saturn is a card that launches the Saturn core - and this still has to answer
	  nothing. saturncdd.cpp cannot stream from a drive, so what it lacks is a
	  disc_playables entry, and a Play row here would be one that could only fail.
	*/
	check(disc_system_id(DISC_T_SATURN) == 0,
		"a Saturn disc is identified, and no core here can read it off the drive");
	check(disc_system_id(DISC_T_CDI) == 0, "nor a CD-i disc");
	check(disc_system_id(DISC_T_AUDIO) == 0, "and an audio CD is nobody's game");

	/*
	  And the other half of that split, which is the question everything about the CARD is
	  asked through: which console the disc BELONGS to. It answers for Saturn, because the
	  drive has nothing to do with writing a cue sheet and some .bins into games/Saturn and
	  reading them back with the Saturn core.

	  These two agreeing for every other type is the point rather than a coincidence -
	  disc_system_id() is this function less the one console whose daemon cannot read a
	  drive - so the check that matters is the single place they differ, and the checks
	  below it that the difference is exactly one console wide.
	*/
	check(disc_console_id(DISC_T_SATURN) && !strcmp(disc_console_id(DISC_T_SATURN), "saturn"),
		"...and it still belongs to the Saturn shelf, which is what a copy of it is filed under");

	for (int t = DISC_T_NONE; t <= DISC_T_UNKNOWN; t++)
	{
		if (t == DISC_T_SATURN) continue;
		const char *c = disc_console_id(t), *s = disc_system_id(t);
		char what[128];
		snprintf(what, sizeof(what), "disc type %d gives the same answer to both questions", t);
		check((!c && !s) || (c && s && !strcmp(c, s)), what);
	}

	check(disc_console_id(DISC_T_3DO) == 0 && disc_console_id(DISC_T_CDI) == 0,
		"3DO and CD-i belong to no shelf system at all - identified, and nowhere to put a copy");
	check(disc_console_id(DISC_T_AUDIO) == 0 && disc_console_id(DISC_T_UNKNOWN) == 0,
		"and neither an audio CD nor an unidentified disc names a console");

	/*
	  disc_capable_systems() is the Play list and is built from disc_system_id(), so adding
	  the console answer must not have leaked Saturn into it: a Saturn row there would be a
	  Play this firmware cannot honour.
	*/
	{
		const char *ids[16];
		int n = disc_capable_systems(ids, 16);
		int has_sat = 0;
		for (int i = 0; i < n; i++) if (!strcasecmp(ids[i], "saturn")) has_sat = 1;
		check(!has_sat, "and the \"pick a core\" list still does not offer Saturn");
	}
	check(disc_type_name(DISC_T_PSX) && disc_type_name(DISC_T_PSX)[0],
		"every type we can report has a name to show");

	/* ------------------------------------------------- the state machine --- */

	{
		fake_disc d; memset(&d, 0, sizeof(d));
		static const char *const none[] = { "" };
		fake_iso(&d, L, "PLAYSTATION", "PLAYSTATION", none, 0);
		fake_put(&d, L + 20, 0, "BOOT = cdrom:\\SLUS_006.26;1", 27, 100);
		disc_set_reader(fake_read, &d);

		disc_ingest_present(0);
		(void)disc_take_dirty();
		check(disc_state() == DISC_ABSENT, "no disc is ABSENT");

		disc_ingest_present(1);
		check(disc_state() == DISC_SPINNING,
			"a disc arriving is SPINNING immediately, before anything has been read");
		check(disc_type() == DISC_T_NONE, "with no type yet - that is the point of the state");
		check(disc_identify_due() == 1, "and identification is now due");
		check(disc_take_dirty() == 1, "the arrival marks the UI dirty");
		check(disc_take_dirty() == 0, "and the flag is consumed, not sticky");

		// A second poll while still spinning must not restart anything.
		disc_ingest_present(1);
		check(disc_state() == DISC_SPINNING && disc_take_dirty() == 0,
			"a repeated present does not re-trigger the arrival");

		disc_ingest_identify(L);
		check(disc_state() == DISC_READY, "identifying moves it to READY");
		check(disc_type() == DISC_T_PSX, "with the type filled in");
		check(!strcmp(disc_serial(), "SLUS-00626"), "and the serial");
		check(disc_identify_due() == 0, "identification is no longer due");
		check(disc_take_dirty() == 1, "and that is a second UI change");

		check(!strcmp(disc_display_name(), "SLUS-00626"),
			"the name under the icon falls back to the serial when the label is useless");

		disc_ingest_present(0);
		check(disc_state() == DISC_ABSENT, "ejecting forgets the disc");
		check(disc_type() == DISC_T_NONE && !disc_serial()[0], "and everything about it");
		check(disc_take_dirty() == 1, "which is also a UI change");
	}

	{
		// A disc we cannot identify must stop looking, or the icon spins forever.
		fake_disc d; memset(&d, 0, sizeof(d));
		disc_set_reader(fake_read, &d);

		disc_ingest_present(1);
		disc_ingest_identify(0);
		check(disc_state() == DISC_UNKNOWN,
			"an unidentifiable disc ends at UNKNOWN, not stuck at SPINNING");
		(void)disc_take_dirty();
		disc_ingest_present(0);
		(void)disc_take_dirty();
	}

	{
		// An audio CD identifies with no data track and no reads at all.
		disc_ingest_present(1);
		disc_ingest_identify(-1);
		check(disc_state() == DISC_READY && disc_type() == DISC_T_AUDIO,
			"an audio CD reaches READY without reading a sector");
		check(!strcmp(disc_display_name(), "Audio CD"),
			"and shows its console name when it has neither label nor serial");
		disc_ingest_present(0);
		(void)disc_take_dirty();
	}

	// Out of order: with no arrival there is nothing due, so a stray identify is a
	// no-op rather than inventing a disc.
	disc_ingest_identify(L);
	check(disc_state() == DISC_ABSENT, "an identify with no disc present changes nothing");

	disc_reset_reader();

	/*
	  Probe retry and helper backoff: disc_probe_due() and disc_refork_due().

	  The bug these replace was `watching = 1` set the moment open() failed once, in the
	  real (non-CHOME_HOST_TEST) disc_watch_start() - a drive that enumerated a second or
	  two after the menu loop started was then invisible for the life of the process. The
	  fix moved the "should I look now" and "should I fork now" decisions into these two
	  pure functions, precisely so they could be driven here without a device node, a
	  fork(), or a real clock - see chome_disc.cpp's own comment above them for the
	  reasoning, and disc_watch_start()/disc_poll() for how production drives them.

	  Both take their own notion of "now" as a plain int, so the timeline below is one
	  this test made up entirely; nothing here sleeps or reads the wall clock.
	*/
	{
		// A drive present at boot: due on the very first look, and never again once found
		// - so a machine where this always worked keeps doing exactly one open() per run.
		int now = 0;
		int last_probe = DISC_NEVER_PROBED;
		int found = 0;
		int looks = 0;

		check(disc_probe_due(found, last_probe, now) == 1,
			"a drive present at boot is due for a look immediately");
		looks++;
		found = 1;                                 // the look succeeds
		check(disc_probe_due(found, last_probe, now) == 0,
			"and once found, never due again at the same instant");

		for (now = 1; now <= 50; now++)
		{
			if (disc_probe_due(found, last_probe, now)) looks++;
		}
		check(looks == 1,
			"nor at any later time - a working drive costs exactly one look, ever");
	}

	{
		// No drive at boot, one appears a few seconds later. This is the reported shape:
		// "works from the F12 menu but not on the shelf", because the old code looked
		// once, found nothing, and latched - so a drive plugged in a moment later was
		// never noticed until a core relaunch reset the latch by accident.
		int last_probe = DISC_NEVER_PROBED;
		int found = 0;
		int attempts = 0;
		const int drive_shows_up_at = 12;          // arbitrary, well past one retry interval

		int found_at = -1;
		for (int now = 0; now <= 40 && !found; now++)
		{
			if (!disc_probe_due(found, last_probe, now)) continue;

			attempts++;
			last_probe = now;

			if (now >= drive_shows_up_at) { found = 1; found_at = now; }
		}

		// Retries land on a fixed schedule (every DISC_PROBE_RETRY_S in chome_disc.cpp,
		// 5 here to match), so "found" lands on the next scheduled look at or after the
		// drive actually appeared - not necessarily the exact second it did.
		check(found == 1, "a drive that shows up after boot is eventually found");
		check(found_at >= drive_shows_up_at,
			"never found before it actually appeared");
		check(found_at >= 0 && found_at - drive_shows_up_at < 5,
			"and found within one retry interval of it appearing, not left for longer");
		check(attempts >= 2 && attempts < drive_shows_up_at,
			"and it took more than one look but nowhere near one per second");
	}

	{
		// A helper that dies: re-forked once immediately, and only backed off if the
		// replacement also dies instantly. Mirrors disc_poll()'s own state - quick_deaths
		// and the time of the last fork - without a process anywhere in sight.
		int quick_deaths = 0;
		int last_fork = 0;

		// First helper forked at t=0, dies instantly (a bad binary, a denied open()).
		quick_deaths = 1;
		check(disc_refork_due(quick_deaths, last_fork, 0) == 1,
			"a first quick death reforks at once - one bad attempt is not a pattern");
		last_fork = 0;

		// The replacement dies instantly too: now it is a pattern, and immediate reforking
		// would spin a CPU forking and dying forever.
		quick_deaths = 2;
		check(disc_refork_due(quick_deaths, last_fork, 0) == 0,
			"a second consecutive quick death does not refork in the same instant");

		int refork_at = -1;
		for (int now = 0; now <= 20; now++)
		{
			if (disc_refork_due(quick_deaths, last_fork, now)) { refork_at = now; break; }
		}
		check(refork_at > 0, "but it does refork eventually, once the backoff elapses");

		// A helper that runs a while before dying resets the count - an unplugged drive,
		// not a helper that can never start - so it goes straight back to "refork at once".
		quick_deaths = 0;
		check(disc_refork_due(quick_deaths, last_fork, refork_at) == 1,
			"a helper that ran a while before dying is not held to the backoff");
	}

	/*
	  And giving the drive back, which only became a question when classicui_disc became a
	  row on Options > More Settings.

	  While the only way to change that flag was a keyboard and a reboot, disc_poll()
	  returning on a false flag was complete: the value it read on the first frame was the
	  value it would read for the life of the process. A player can now turn it off with a
	  pad, and a bare return would leave the helper process alive with /dev/sr0 open and
	  nothing reading what it wrote - the badge frozen on the last disc it identified, the
	  device unavailable to a core or to a rip, and the feature reporting itself as off. A
	  feature that is switched off and still running is the exact shape this front-end must
	  not have, and it is what would have made OW_NOW on that row true in one direction only.

	  disc_release_due() is the decision, pure and above the CHOME_HOST_TEST split for the
	  same reason disc_probe_due() and disc_refork_due() are: no fork() and no device node
	  in here. Asserted directly *and* through the harness's disc_poll(), which asks it
	  rather than restating it - so the second block is an assertion about the shipped
	  decision and not about a copy of it living in the stub.
	*/
	{
		check(disc_release_due(1, 1, 1) == 0 && disc_release_due(1, 0, 0) == 0,
			"nothing is handed back while the setting is on");
		check(disc_release_due(0, 0, 0) == 0,
			"nor when it is off and there was never a drive to hand back");
		check(disc_release_due(0, 1, 0) == 1,
			"a latched drive is handed back when the setting goes off");
		/*
		  The latch and the process are set and cleared in different places, so either one
		  outliving the flag is a drive nobody has given back. This is the half that would
		  have been missed by testing `watching` alone: disc_watch_stop() clears the latch
		  and then kills the helper, so a stop interrupted between the two leaves exactly
		  this state.
		*/
		check(disc_release_due(0, 0, 1) == 1, "and so is a helper still alive without it");
	}

	{
		uint8_t was = cfg.classicui_disc;

		cfg.classicui_disc = 1;
		check(disc_watch_start() && disc_watching(), "the drive is being watched");

		disc_poll();
		check(disc_watching(), "and a poll with the setting on leaves it that way");

		// The press on the Settings screen, in the only form a host test can stage it:
		// cfg is what disc_poll() reads, and the screen's own writer sets exactly this.
		cfg.classicui_disc = 0;
		disc_poll();
		check(!disc_watching(), "turning the setting off hands the drive back on the next poll");

		// Idempotent, because this runs every frame for as long as the machine is on: the
		// guard in disc_poll() is what keeps a card with the feature off from unlinking a
		// state file and forgetting a disc sixty times a second, for ever.
		disc_poll();
		check(!disc_watching() && !disc_release_due(0, 0, 0),
			"and asks for nothing more on every frame after that");

		cfg.classicui_disc = was;
	}
}

/*
  What identifier each console's disc actually carries, and - just as much the point -
  which consoles are asked for nothing at all.

  Every fixture below is the real header bytes at the real offset, so these are checks
  on the parse and not on a restatement of it. The Saturn and Mega CD offsets are the
  ones support/physical_disc/physical_disc.cpp reads in this same tree, which is a second
  implementation of the same specification.

  The negative checks are the ones worth keeping honest. A disc whose system this cannot
  key on must come back with an *empty* serial, because the serial is what goes out to
  ScreenScraper as a name - and an unmatchable name is not a free question. It spends the
  account's unmatched allowance, which is the scarce one, and caches a miss against a key
  that was never going to work. So "PC Engine CD asks for nothing" is asserted here in as
  many words, and a later edit that starts sending its volume label has to delete a check
  that says why rather than quietly widen a switch.
*/
static void assert_disc_serials()
{
	printf("\n== physical disc: the identifier each console presses onto it ==\n");

	const int L = 0;
	char ser[DISC_SERIAL_LEN];

	/* ------------------------------------------------------------- Saturn --- */

	{
		/*
		  The Saturn header: the maker id at 0x00, then a ten-byte product number at
		  0x20. Space-padded in the field, and Redump's own notation once trimmed, so
		  nothing is rewritten on the way out.
		*/
		fake_disc d; memset(&d, 0, sizeof(d));
		fake_put(&d, L, 0, "SEGA SEGASATURN ", 16, 0);
		fake_put(&d, L, 0, "SEGA ENTERPRISES", 16, 0x10);
		fake_put(&d, L, 0, "GS-9061   ", 10, 0x20);
		fake_put(&d, L, 0, "V1.000", 6, 0x2A);
		fake_put(&d, L, 0, "19950728", 8, 0x30);
		disc_set_reader(fake_read, &d);

		check(disc_identify_at(L) == DISC_T_SATURN, "a Saturn header is still a Saturn disc");

		memset(ser, 0, sizeof(ser));
		check(disc_saturn_serial_at(L, ser, sizeof(ser)) > 0 && !strcmp(ser, "GS-9061"),
			"and its product number reads GS-9061 out of the ten bytes at 0x20");

		// The version sits at 0x2A, immediately after the field. Reading eleven bytes
		// instead of ten would drag the V in with it.
		check(!strchr(ser, 'V'), "with the version beside it left where it is");

		memset(ser, 0, sizeof(ser));
		check(disc_serial_for(DISC_T_SATURN, L, ser, sizeof(ser)) > 0 && !strcmp(ser, "GS-9061"),
			"and the typed reader routes a Saturn disc to it");
	}

	{
		// A third-party code, which is the common case: T for a licensee, then a
		// publisher number and a region letter.
		fake_disc d; memset(&d, 0, sizeof(d));
		fake_put(&d, L, 0, "SEGA SEGASATURN ", 16, 0);
		fake_put(&d, L, 0, "T-1809G   ", 10, 0x20);
		disc_set_reader(fake_read, &d);

		memset(ser, 0, sizeof(ser));
		check(disc_saturn_serial_at(L, ser, sizeof(ser)) > 0 && !strcmp(ser, "T-1809G"),
			"a licensee's Saturn code comes out whole, region letter included");
	}

	{
		// Sega's own catalogue number. Redump writes this one three ways and the disc
		// only ever writes it this way - see key_forms() in tools/disctitles.py.
		fake_disc d; memset(&d, 0, sizeof(d));
		fake_put(&d, L, 0, "SEGA SEGASATURN ", 16, 0);
		fake_put(&d, L, 0, "MK-81088  ", 10, 0x20);
		disc_set_reader(fake_read, &d);

		memset(ser, 0, sizeof(ser));
		check(disc_saturn_serial_at(L, ser, sizeof(ser)) > 0 && !strcmp(ser, "MK-81088"),
			"and so does a Sega-published one");
	}

	{
		// Not a Saturn disc: the reader must refuse rather than return whatever
		// happens to sit at 0x20 of somebody else's sector.
		fake_disc d; memset(&d, 0, sizeof(d));
		fake_put(&d, L, 0, "SEGADISCSYSTEM", 14, 0);
		fake_put(&d, L, 0, "NOTASATURN", 10, 0x20);
		disc_set_reader(fake_read, &d);

		memset(ser, 0, sizeof(ser));
		check(disc_saturn_serial_at(L, ser, sizeof(ser)) == 0 && !ser[0],
			"the Saturn reader refuses a disc that is not one, rather than reading 0x20 blind");
	}

	/* ------------------------------------------------------------ Mega CD --- */

	{
		/*
		  "GM MK-4407 -00" is Sonic CD as the disc spells it. Redump spells the same
		  disc "MK-4407-50". Both ends of that disagreement have to be dealt with or
		  the table is a table of misses, and this is the end that lives here.
		*/
		fake_disc d; memset(&d, 0, sizeof(d));
		fake_put(&d, L, 0, "SEGADISCSYSTEM  ", 16, 0);
		fake_put(&d, L, 0, "SEGA MEGA DRIVE ", 16, 0x100);
		fake_put(&d, L, 0, "GM MK-4407 -00", 14, 0x180);
		disc_set_reader(fake_read, &d);

		check(disc_identify_at(L) == DISC_T_MEGACD, "a Mega CD header is still a Mega CD disc");

		memset(ser, 0, sizeof(ser));
		check(disc_megacd_serial_at(L, ser, sizeof(ser)) > 0 && !strcmp(ser, "MK-4407"),
			"and \"GM MK-4407 -00\" at 0x180 becomes MK-4407: the media type and the revision go");

		memset(ser, 0, sizeof(ser));
		check(disc_serial_for(DISC_T_MEGACD, L, ser, sizeof(ser)) > 0 && !strcmp(ser, "MK-4407"),
			"and the typed reader routes a Mega CD disc to it");
	}

	{
		/*
		  Every other spelling this field is known to take on a real disc, from the
		  match table of an emulator that keys on it. Two of these six are outright
		  malformed, and they are in here because a fixed-offset parse gets exactly
		  those two wrong while looking perfectly correct on the other four.
		*/
		static const struct { const char *field; const char *want; const char *why; } sp[] =
		{
			{ "GM MK-4407-00 ", "MK-4407",
			  "Sonic CD (Europe) writes the code flush left and pads the other end" },
			{ "GM  T-81025-00", "T-81025",
			  "Mortal Kombat pads in front of the code instead, and must not come back empty" },
			{ "GM T-127015-00", "T-127015",
			  "a nine-character code fills the field and keeps every digit" },
			{ "GM T-111065 -0", "T-111065",
			  "and one whose revision fell off the end still yields the code before it" },
		};

		for (size_t i = 0; i < sizeof(sp) / sizeof(sp[0]); i++)
		{
			fake_disc d; memset(&d, 0, sizeof(d));
			fake_put(&d, L, 0, "SEGADISCSYSTEM  ", 16, 0);
			fake_put(&d, L, 0, sp[i].field, 14, 0x180);
			disc_set_reader(fake_read, &d);

			memset(ser, 0, sizeof(ser));
			int n = disc_megacd_serial_at(L, ser, sizeof(ser));
			check(n > 0 && !strcmp(ser, sp[i].want), sp[i].why);
		}
	}

	{
		/*
		  A European pressing, where the two digits are a country code (50 = Europe)
		  rather than a revision. They occupy the same slot, and they come off the same
		  way - which is right rather than merely convenient: the generator strips the
		  same two digits off the Redump side, so both ends land on the same key. A
		  reader that kept them here would key on MK156950 while the table said MK1569.
		*/
		fake_disc d; memset(&d, 0, sizeof(d));
		fake_put(&d, L, 0, "SEGADISCSYSTEM  ", 16, 0);
		fake_put(&d, L, 0, "GM MK-1569 -50", 14, 0x180);
		disc_set_reader(fake_read, &d);

		memset(ser, 0, sizeof(ser));
		check(disc_megacd_serial_at(L, ser, sizeof(ser)) > 0 && !strcmp(ser, "MK-1569"),
			"a country code sits in the revision's two digits and comes off with it");
	}

	{
		// A header that is all padding, and one mangled so badly that only a prefix
		// survives. Either would go out as the disc's name and spend a request.
		static const char *const junk[] = { "              ", "GM MK- 4430  -" };

		for (size_t i = 0; i < sizeof(junk) / sizeof(junk[0]); i++)
		{
			fake_disc d; memset(&d, 0, sizeof(d));
			fake_put(&d, L, 0, "SEGADISCSYSTEM  ", 16, 0);
			fake_put(&d, L, 0, junk[i], 14, 0x180);
			disc_set_reader(fake_read, &d);

			memset(ser, 0, sizeof(ser));
			check(disc_megacd_serial_at(L, ser, sizeof(ser)) == 0 && !ser[0],
				i ? "a code mangled down to \"MK-\" is refused rather than sent as a name"
				  : "and a blank product code is no code at all");
		}
	}

	/* ------------------------------------ the ones we deliberately refuse --- */

	/*
	  PC Engine CD and Neo Geo CD carry no product code anywhere in their data. Redump
	  keys them on a catalogue number read off the disc's printed ring, which is not
	  something a drive can hand us.

	  So they must ask for nothing. This is the same call the existing code already makes
	  for a .npc in ss_system_id() and for the same reason: a confidently wrong match is
	  worse than no match, and here it is also paid for out of a daily allowance.
	*/
	{
		// Both sectors, because the search spans the pair and gives up if either
		// read fails - see the PC Engine case in disc_identify_at().
		fake_disc d; memset(&d, 0, sizeof(d));
		fake_put(&d, L, 0, "NOTHINGUSEFUL", 13, 0);
		fake_put(&d, L + 1, 0, "PC Engine CD-ROM SYSTEM", 23, 40);
		disc_set_reader(fake_read, &d);

		check(disc_identify_at(L) == DISC_T_PCECD, "a PC Engine CD is identified");

		memset(ser, 0xAA, sizeof(ser));
		check(disc_serial_for(DISC_T_PCECD, L, ser, sizeof(ser)) == 0 && !ser[0],
			"and is asked for no serial at all: nothing in its data is one");
	}

	{
		fake_disc d; memset(&d, 0, sizeof(d));
		static const char *const files[] = { "IPL.TXT", "ABS.TXT" };
		fake_iso(&d, L, "NEO GEO CD", "NGCD", files, 2);
		disc_set_reader(fake_read, &d);

		check(disc_identify_at(L) == DISC_T_NEOGEO, "a Neo Geo CD is identified");

		memset(ser, 0xAA, sizeof(ser));
		check(disc_serial_for(DISC_T_NEOGEO, L, ser, sizeof(ser)) == 0 && !ser[0],
			"and it too is asked for nothing rather than sent under a volume label");
	}

	{
		// 3DO and CD-i have no shelf row, so nothing would scrape them anyway - but the
		// dispatcher is the layer that has to say so, not the caller.
		fake_disc d; memset(&d, 0, sizeof(d));
		fake_put(&d, L, 0, "\x01\x5A\x5A\x5A\x5A\x5A", 6, 0);
		disc_set_reader(fake_read, &d);

		memset(ser, 0xAA, sizeof(ser));
		check(disc_serial_for(DISC_T_3DO, L, ser, sizeof(ser)) == 0 && !ser[0],
			"a 3DO disc is asked for nothing");
	}

	/* ------------------------------ PlayStation, unchanged and still there --- */

	{
		// The path that is in the field. It must behave exactly as it did, including
		// through the new dispatcher.
		fake_disc d; memset(&d, 0, sizeof(d));
		static const char *const none[] = { "" };
		fake_iso(&d, L, "PLAYSTATION", "PLAYSTATION", none, 0);
		fake_put(&d, L + 20, 0, "BOOT = cdrom:\\SLUS_006.26;1", 27, 100);
		disc_set_reader(fake_read, &d);

		memset(ser, 0, sizeof(ser));
		check(disc_serial_for(DISC_T_PSX, L, ser, sizeof(ser)) > 0 && !strcmp(ser, "SLUS-00626"),
			"a PlayStation disc still reads SLUS-00626, through the dispatcher");

		// An unidentified disc keeps the loose Sony scan, which is the one case where
		// looking for a serial we have no signature for is still worth the reads.
		memset(ser, 0, sizeof(ser));
		check(disc_serial_for(DISC_T_UNKNOWN, L, ser, sizeof(ser)) > 0 && !strcmp(ser, "SLUS-00626"),
			"and an unidentified disc is still scanned for one");
	}

	/* ------------------------------------ which platform gets asked about --- */

	/*
	  The shelf row a disc is filed under and the platform the database knows it by are
	  different answers, and sending the first one was three consoles' worth of requests
	  that could not match: Mega-CD is systeme 20 and Mega Drive is systeme 1.
	*/
	check(disc_scrape_id(DISC_T_MEGACD) && !strcmp(disc_scrape_id(DISC_T_MEGACD), "megacd"),
		"a Mega CD disc is scraped as megacd, not as the md shelf row it is filed under");
	check(disc_scrape_id(DISC_T_PCECD) && !strcmp(disc_scrape_id(DISC_T_PCECD), "pcecd"),
		"a PC Engine CD as pcecd, not tg16");
	check(disc_scrape_id(DISC_T_NEOGEO) && !strcmp(disc_scrape_id(DISC_T_NEOGEO), "neogeocd"),
		"a Neo Geo CD as neogeocd, not neogeo");
	check(disc_scrape_id(DISC_T_SATURN) && !strcmp(disc_scrape_id(DISC_T_SATURN), "saturn"),
		"and Saturn has a platform to be scraped as, though no core can be handed the drive");
	check(disc_system_id(DISC_T_SATURN) == 0,
		"which is exactly the answer disc_system_id() must keep giving instead");
	check(disc_scrape_id(DISC_T_MDPLUS) && !strcmp(disc_scrape_id(DISC_T_MDPLUS), "md"),
		"a Mega Drive+ disc is a Mega Drive game and is scraped as one");
	check(disc_scrape_id(DISC_T_3DO) == 0 && disc_scrape_id(DISC_T_CDI) == 0 &&
		disc_scrape_id(DISC_T_AUDIO) == 0 && disc_scrape_id(DISC_T_UNKNOWN) == 0,
		"and the discs with no shelf row are scraped as nothing");

	/*
	  Every platform this now names has to exist in both tables, or the fix is a string
	  nothing resolves. ss_system_id() is where a name becomes a systemeid.
	*/
	{
		const int types[] = { DISC_T_PSX, DISC_T_SATURN, DISC_T_MEGACD,
			DISC_T_PCECD, DISC_T_NEOGEO, DISC_T_MDPLUS, DISC_T_SNES };
		int named = 0, resolved = 0;
		for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++)
		{
			const char *id = disc_scrape_id(types[i]);
			if (!id) continue;
			named++;

			int onshelf = 0;
			for (int s = 0; s < lib_sys_count(); s++)
			{
				const chome_sys *c = lib_sys(s);
				if (c && !strcmp(c->id, id)) { onshelf = 1; break; }
			}
			if (onshelf && ss_system_id(id, 0)) resolved++;
		}
		check(named == 7 && resolved == 7,
			"and all seven are shelf systems with a ScreenScraper id behind them");
	}

	/* --------------------------------- and what is put in the request ---- */

	/*
	  disc_scrape_name(), which is the string that actually goes out - and is not the
	  string on screen. Driven through the state machine rather than called on its own,
	  because the whole point is what it answers for a disc that has just been read.
	*/
	{
		const char *path = ROOT "/classicui/disctitles.txt";
		mkpath(ROOT "/classicui");

		FILE *f = fopen(path, "wb");
		if (f)
		{
			fprintf(f, "#classicui-disctitles 1\n");
			fprintf(f, "GS9061\tHideo Nomo World Series Baseball\n");
			fprintf(f, "MK4407\tSonic the Hedgehog CD\n");
			fclose(f);
		}
		disc_titles_forget();

		{
			// A Saturn disc the table knows: the proper title goes out, not the label.
			fake_disc d; memset(&d, 0, sizeof(d));
			fake_put(&d, L, 0, "SEGA SEGASATURN ", 16, 0);
			fake_put(&d, L, 0, "GS-9061   ", 10, 0x20);
			disc_set_reader(fake_read, &d);

			disc_ingest_present(1);
			disc_ingest_identify(L);
			check(!strcmp(disc_serial(), "GS-9061"), "a Saturn disc arrives carrying GS-9061");
			check(disc_scrape_name() && !strcmp(disc_scrape_name(), "Hideo Nomo World Series Baseball"),
				"and is asked about by its proper name");

			disc_ingest_present(0);
			(void)disc_take_dirty();
		}

		{
			/*
			  The disc that started this, and the assertion is now the opposite of what it
			  was, on measurement rather than on preference.

			  It used to require that the serial go out and the label never did, reasoning
			  that "no volume label was ever indexed as a rom name". That is not true. Asked
			  about all 37 Saturn discs on the card on 2026-08-11:

			      romnom = ISO volume label     23/37 matched
			      romnom = disc header title    30/37 matched, none wrong
			      romnom = product number        0/37 - "MK-81207" and its kind miss
			      serialnum = product number    11/37

			  So the old order sent the one string that cannot work in place of two that
			  can. It survived because the serial was empty on every physical Saturn disc
			  (see disc_serial_for() in chome_disc.cpp), so this branch was unreachable on
			  real hardware and only ever ran here.
			*/
			fake_disc d; memset(&d, 0, sizeof(d));
			fake_put(&d, L, 0, "SEGA SEGASATURN ", 16, 0);
			fake_put(&d, L, 0, "MK-81088  ", 10, 0x20);
			fake_iso(&d, L, "SEGARALLY_CHAMPIONSHIP", 0, 0, 0);
			disc_set_reader(fake_read, &d);

			disc_ingest_present(1);
			disc_ingest_identify(L);

			check(!strcmp(disc_label(), "SEGARALLY CHAMPIONSHIP"),
				"an unknown Saturn disc still shows its volume label on screen");
			check(!strcmp(disc_display_name(), "SEGARALLY CHAMPIONSHIP"),
				"which is what the player sees, and is right");
			check(disc_scrape_name() && !strcmp(disc_scrape_name(), "SEGARALLY CHAMPIONSHIP"),
				"and the database is asked about that name, because a product number matches nothing");
			check(strcmp(disc_scrape_name(), "MK-81088") != 0,
				"never the bare product number, which is the one string measured to always miss");

			disc_ingest_present(0);
			(void)disc_take_dirty();
		}

		{
			// A Mega CD disc, end to end: header field to key to title.
			fake_disc d; memset(&d, 0, sizeof(d));
			fake_put(&d, L, 0, "SEGADISCSYSTEM  ", 16, 0);
			fake_put(&d, L, 0, "GM MK-4407 -00", 14, 0x180);
			disc_set_reader(fake_read, &d);

			disc_ingest_present(1);
			disc_ingest_identify(L);
			check(!strcmp(disc_serial(), "MK-4407"), "a Mega CD disc arrives carrying MK-4407");
			check(!strcmp(disc_display_name(), "Sonic the Hedgehog CD"),
				"and is named from the table, which is what this console could never do before");
			check(disc_scrape_name() && !strcmp(disc_scrape_name(), "Sonic the Hedgehog CD"),
				"and is asked about under that name");

			disc_ingest_present(0);
			(void)disc_take_dirty();
		}

		{
			/*
			  A Neo Geo CD disc. Its volume label is per-game and looks almost usable -
			  and of fifteen real discs read, "DD_CD" is one of the better ones: the set
			  also holds "B4CD", "CR2CD", "C205", a PowerISO timestamp and a flat
			  "UNTITLED". None of them is a rom name and no serial exists in the data at
			  all, so this console asks for nothing.
			*/
			fake_disc d; memset(&d, 0, sizeof(d));
			static const char *const files[] = { "IPL.TXT" };
			fake_iso(&d, L, "DD_CD", "NGCD", files, 1);
			disc_set_reader(fake_read, &d);

			disc_ingest_present(1);
			disc_ingest_identify(L);

			check(disc_type() == DISC_T_NEOGEO && !disc_serial()[0],
				"a Neo Geo CD disc arrives with no serial");
			// "DD CD" rather than "DD_CD": disc_label_at() reads an ISO label's
			// underscores as the spaces they stand in for, which for a house code
			// makes an already-unusable string one step further from a game's name.
			check(!strcmp(disc_display_name(), "DD CD"),
				"its label is still what the player is shown, for want of anything better");
			check(disc_scrape_name() == 0,
				"and nothing at all is asked of the database, which is the considered answer");

			disc_ingest_present(0);
			(void)disc_take_dirty();
		}

		{
			// A PC Engine CD disc has no ISO filesystem, so not even a label - which is
			// the same refusal arrived at one step earlier.
			fake_disc d; memset(&d, 0, sizeof(d));
			fake_put(&d, L, 0, "NOTHINGUSEFUL", 13, 0);
			fake_put(&d, L + 1, 0, "PC Engine CD-ROM SYSTEM", 23, 40);
			disc_set_reader(fake_read, &d);

			disc_ingest_present(1);
			disc_ingest_identify(L);

			check(disc_type() == DISC_T_PCECD && !disc_serial()[0] && !disc_label()[0],
				"a PC Engine CD disc has neither serial nor label - it has no filesystem");
			check(disc_scrape_name() == 0, "so it too asks for nothing");

			disc_ingest_present(0);
			(void)disc_take_dirty();
		}

		unlink(path);
		disc_titles_forget();
	}

	disc_reset_reader();
}

/*
  The disc title table: chome_titles.cpp, and disc_display_name() on top of it.

  The fixture is built here rather than committed, and it is deliberately *big* -
  four thousand rows, ~90 KB. A table that fits in one window would be answered by
  the linear scan at the bottom of tdb_search() and would say nothing at all about
  the binary search above it, which is the part with the invariant in it. At this
  size a lookup takes half a dozen probes, so a mistake in how the range narrows
  shows up as a miss on some keys and not others - which is what the first and last
  row are checked for.

  What this cannot test, and it is the interesting half:

    - That a *missing* file costs no work per frame. The contract is "opened once,
      and once the answer is no, never again", and there is no way to count open()
      calls from in here without instrumenting the module for the test's benefit.
      What is checked instead is the consequence: with the verdict cached, putting a
      perfectly good file on the card is *not* noticed until disc_titles_forget().
      A version that re-opened every call would see the new file and fail that check,
      so the caching is proved by what it gets wrong on purpose.

    - Mega CD, which is generated for and not yet reachable. The identifier is the
      product code at 0x180 of the disc header ("GM MK-4407 -00"), and
      disc_serial_at() only digs out PlayStation serials - so the row for MK4407 is
      checked through disc_title_for() directly and not through a disc. When
      identification learns to read that header this becomes a real end-to-end case.

    - Timing on the real thing. This runs off a Docker overlayfs with everything in
      page cache; the card is exFAT on SD behind a 400 MHz-class ARM. The probe
      *count* is what was designed against and it is the same in both places, but
      the milliseconds are not measurable here.
*/
static void assert_disc_titles()
{
	printf("\n== physical disc: the serial gets a name ==\n");

	const int L = 0;
	const char *path = ROOT "/classicui/disctitles.txt";

	mkpath(ROOT "/classicui");

	/*
	  Rows in the order the device compares them - byte order over A-Z0-9 keys, which
	  puts MK4407 first, the SLES block next and SUPERGAME last. Written by hand here
	  rather than by tools/disctitles.py, so that a change to the generator cannot make
	  this pass for the wrong reason.
	*/
	{
		FILE *f = fopen(path, "wb");
		check(f != 0, "the fixture table can be written");
		if (f)
		{
			fprintf(f, "#classicui-disctitles 1\n");
			fprintf(f, "MK4407\tSonic the Hedgehog CD\n");
			for (int i = 0; i < 4000; i++)
			{
				if (i == 1506) fprintf(f, "SLES%05d\tMetal Gear Solid\n", i);
				else fprintf(f, "SLES%05d\tFiller Title %d\n", i, i);
			}
			fprintf(f, "SUPERGAME\tSuper Game\n");
			fclose(f);
		}
	}

	disc_titles_forget();

	/* ------------------------------------------------------------- a hit --- */

	{
		const char *t = disc_title_for("SLES-01506");
		check(t && !strcmp(t, "Metal Gear Solid"),
			"a serial in the middle of the table is found");
	}

	{
		// The disc says SLES_015.06, Redump says SLES-01506, a Japanese serial has a
		// space in it. All three normalise to one key, or the table matches nothing.
		const char *a = disc_title_for("sles 01506");
		const char *b = disc_title_for("SLES_015.06");
		check(a && b && !strcmp(a, "Metal Gear Solid") && !strcmp(b, "Metal Gear Solid"),
			"case, spaces and punctuation in the key are normalised away");
	}

	// The row immediately after the magic line, and the very last row: the two the
	// search's boundaries would drop.
	check(disc_title_for("MK-4407") && !strcmp(disc_title_for("MK-4407"), "Sonic the Hedgehog CD"),
		"the first row is not shadowed by the magic line above it");
	check(disc_title_for("Super Game!") && !strcmp(disc_title_for("Super Game!"), "Super Game"),
		"and the last row is reachable");

	/* ------------------------------------------------------------ a miss --- */

	check(disc_title_for("SLES-09999") == 0, "a serial that is not there returns nothing");
	check(disc_title_for("SLES-01506X") == 0, "and neither does a near miss");
	check(disc_title_for("") == 0, "an empty key is not a lookup");
	check(disc_title_for("-  .") == 0, "nor is one that normalises to nothing");

	/* ------------------------------------------- what the disc prompt shows --- */

	{
		fake_disc d; memset(&d, 0, sizeof(d));
		static const char *const none[] = { "" };
		fake_iso(&d, L, "PLAYSTATION", "PLAYSTATION", none, 0);
		fake_put(&d, L + 20, 0, "BOOT = cdrom:\\SLES_015.06;1", 27, 100);
		disc_set_reader(fake_read, &d);

		disc_ingest_present(1);
		disc_ingest_identify(L);
		check(!strcmp(disc_serial(), "SLES-01506"), "a disc carrying SLES-01506 is identified");
		check(!strcmp(disc_display_name(), "Metal Gear Solid"),
			"and is shown by name instead of by serial");

		disc_ingest_present(0);
		(void)disc_take_dirty();
	}

	{
		// The same disc with a serial nothing knows about must read exactly as it did
		// before any of this existed.
		fake_disc d; memset(&d, 0, sizeof(d));
		static const char *const none[] = { "" };
		fake_iso(&d, L, "PLAYSTATION", "PLAYSTATION", none, 0);
		fake_put(&d, L + 20, 0, "BOOT = cdrom:\\SLES_099.99;1", 27, 100);
		disc_set_reader(fake_read, &d);

		disc_ingest_present(1);
		disc_ingest_identify(L);
		check(!strcmp(disc_display_name(), "SLES-09999"),
			"an unknown serial still falls back to the serial itself");

		disc_ingest_present(0);
		(void)disc_take_dirty();
	}

	{
		// A disc with no serial at all - which is every system here except PlayStation.
		// Its volume label is the only key it has, so the label is offered too.
		fake_disc d; memset(&d, 0, sizeof(d));
		static const char *const none[] = { "" };
		fake_iso(&d, L, "SUPER_GAME", 0, none, 0);
		disc_set_reader(fake_read, &d);

		disc_ingest_present(1);
		disc_ingest_identify(L);
		check(!disc_serial()[0] && !strcmp(disc_label(), "SUPER GAME"),
			"a disc with no serial still has a volume label");
		check(!strcmp(disc_display_name(), "Super Game"),
			"which is looked up too, so the label's own casing is not what gets drawn");

		disc_ingest_present(0);
		(void)disc_take_dirty();
	}

	/* ------------------------------------------------------- the caching --- */

	{
		// Delete the table without telling anyone. An answer already given must still
		// come back, which is only possible if it was not re-read.
		unlink(path);
		const char *t = disc_title_for("SLES-01506");
		check(t && !strcmp(t, "Metal Gear Solid"), "an answer already given survives the file going away");
		check(disc_title_for("SLES-00002") == 0, "while a new question now has no answer");
	}

	/* ------------------------------------------------- no file at all: silence --- */

	disc_titles_forget();
	check(disc_title_for("SLES-01506") == 0, "with no table on the card there are no titles");

	{
		fake_disc d; memset(&d, 0, sizeof(d));
		static const char *const none[] = { "" };
		fake_iso(&d, L, "PLAYSTATION", "PLAYSTATION", none, 0);
		fake_put(&d, L + 20, 0, "BOOT = cdrom:\\SLES_015.06;1", 27, 100);
		disc_set_reader(fake_read, &d);

		disc_ingest_present(1);
		disc_ingest_identify(L);
		check(!strcmp(disc_display_name(), "SLES-01506"),
			"and the prompt shows precisely what it showed before the table existed");

		disc_ingest_present(0);
		(void)disc_take_dirty();
	}

	/*
	  See the section's comment: this is how "it does not look again" is observed.
	  Restoring the file must NOT be noticed until something asks for it to be.

	  The four throwaway keys are load-bearing, and the check below said nothing without
	  them. TDB_CACHE is four, so SLES-01506 was still sitting in the answer cache from
	  the lookups above with a miss recorded against it - and a version of this module
	  that re-opened the card on every single call would have passed anyway, by reading
	  the cache. Asking four other things first is what evicts it and makes the verdict
	  the only thing that can produce this answer.
	*/
	put_file(path, "#classicui-disctitles 1\nSLES01506\tMetal Gear Solid\n");
	(void)disc_title_for("QQQQ0001");
	(void)disc_title_for("QQQQ0002");
	(void)disc_title_for("QQQQ0003");
	(void)disc_title_for("QQQQ0004");
	check(disc_title_for("SLES-01506") == 0,
		"a table appearing after the verdict is not re-opened on the next call");
	disc_titles_forget();
	check(disc_title_for("SLES-01506") != 0, "and forgetting the verdict is what picks it up");

	// A player who opens the file in a Windows editor gets CRLF back, and the card is
	// exFAT so nothing on the way in converts it. Stripping it is what keeps a stray
	// carriage return off the end of every title that gets drawn.
	put_file(path, "#classicui-disctitles 1\r\nSLES01506\tMetal Gear Solid\r\n");
	disc_titles_forget();
	check(disc_title_for("SLES-01506") && !strcmp(disc_title_for("SLES-01506"), "Metal Gear Solid"),
		"a table saved with CRLF line endings reads the same");

	/* --------------------------------------------------- files that are wrong --- */

	// Somebody else's file under our name. One log line, no titles, and no guessing
	// at a layout we do not recognise.
	put_file(path, "<?xml version=\"1.0\"?>\n<datafile>\n<game name=\"x\"/>\n</datafile>\n");
	disc_titles_forget();
	check(disc_title_for("SLES-01506") == 0, "a file without our magic line is refused");

	// A version we do not know. Same answer: refuse, rather than read a format that
	// has changed in some way this build cannot see.
	put_file(path, "#classicui-disctitles 2\nSLES01506\tMetal Gear Solid\n");
	disc_titles_forget();
	check(disc_title_for("SLES-01506") == 0, "and so is a version this build does not know");

	put_file(path, "");
	disc_titles_forget();
	check(disc_title_for("SLES-01506") == 0, "an empty file is a table with nothing in it");

	// And so is one with nothing but the magic line, which is the other way to say it and
	// the one a hand-written file arrives as while somebody is starting it. The search
	// gets a range containing only the magic line, which is not a row, so it must come
	// back with no title rather than compare against it.
	put_file(path, "#classicui-disctitles 1\n");
	disc_titles_forget();
	check(disc_title_for("SLES-01506") == 0, "as is one holding only its magic line");

	// Truncated mid-row, which is what a card pulled out during a copy leaves.
	put_file(path,
		"#classicui-disctitles 1\n"
		"MK4407\tSonic the Hedgehog CD\n"
		"SLES01506\tMetal Gear Solid\n"
		"SLES0299");
	disc_titles_forget();
	check(disc_title_for("SLES-01506") && !strcmp(disc_title_for("SLES-01506"), "Metal Gear Solid"),
		"a torn last row does not cost the rows above it");
	check(disc_title_for("SLES-0299") == 0, "and the torn row itself matches nothing");

	/*
	  The one that has to be timed rather than merely returned from: our magic followed
	  by a quarter of a megabyte with no line breaks in it. The search narrows by
	  finding line boundaries, so with none to find it degrades from ~6 probes to one
	  line-buffer at a time - which is not a crash and not an infinite loop, but a
	  front-end that has stopped, which is the same thing to whoever is holding the
	  controller. TDB_PROBE_MAX is what bounds it.

	  Five seconds is a very loose bound for what should be microseconds; it is set
	  loose on purpose so this cannot fail for being run on a busy machine, while still
	  failing if the bound in the module is ever removed.
	*/
	{
		FILE *f = fopen(path, "wb");
		if (f)
		{
			fprintf(f, "#classicui-disctitles 1\n");
			for (int i = 0; i < 256 * 1024; i++) fputc('A', f);
			fclose(f);
		}
		disc_titles_forget();

		struct timeval t0, t1;
		gettimeofday(&t0, 0);
		const char *t = disc_title_for("SLES-01506");
		gettimeofday(&t1, 0);

		double secs = (double)(t1.tv_sec - t0.tv_sec) + (t1.tv_usec - t0.tv_usec) / 1e6;
		check(t == 0, "a file with our magic and no rows in it yields no title");
		check(secs < 5.0, "and gives up rather than searching it a line-buffer at a time");
	}

	/* --------------------------------------- settling the verdict up front --- */

	/*
	  disc_titles_preload() asks "is there a table on this card at all?" when the drive
	  is found, rather than on the frame a serial first needs an answer. In the firmware
	  the caller is disc_watch_start(), which is stubbed here - so the function is driven
	  directly, which is the only part of it that is not plumbing anyway.

	  What has to be shown is that it genuinely *settles* the verdict, because that is
	  the whole of its effect: it caches no answers and reads no rows, and a version that
	  quietly did nothing would look identical from every other angle. So the verdict is
	  settled against a card with no table, a table is then put back, and the lookup must
	  still find nothing - which is only possible if the question had already been
	  answered. The stickiness itself is not new and is checked further up; what is new
	  is that preload is what can now do the sticking.
	*/
	unlink(path);
	disc_titles_forget();
	disc_titles_preload();

	{
		FILE *f = fopen(path, "wb");
		if (f)
		{
			fprintf(f, "#classicui-disctitles 1\n");
			fprintf(f, "SLES01506\tMetal Gear Solid\n");
			fclose(f);
		}
	}

	check(disc_title_for("SLES-01506") == 0,
		"preloading settles the verdict, so a table arriving after it is not picked up");

	disc_titles_forget();
	check(disc_title_for("SLES-01506") != 0, "and forgetting the verdict is what picks it up");

	/*
	  The other order, which is the one that actually ships: a table that is already on
	  the card when the drive is found. Preload must leave it perfectly readable - a
	  verdict settled as "good" is the case where nothing may change.
	*/
	disc_titles_forget();
	disc_titles_preload();
	{
		const char *t = disc_title_for("SLES-01506");
		check(t && !strcmp(t, "Metal Gear Solid"),
			"while preloading a table that is there leaves every lookup working");
	}

	/*
	  Restore. Every section after this one draws or logs a disc name, so a table left
	  on the card would change what they see - and the two disc sections that assert on
	  disc_display_name() would fail a long way from here.
	*/
	unlink(path);
	disc_titles_forget();
	disc_ingest_present(0);
	(void)disc_take_dirty();
	disc_reset_reader();
}

/*
  What the disc dialog actually says, at each of the three ends a disc can come to.

  The complaint this answers: a disc went in, the front-end showed "PlayStation" and no
  game name, and it did not read as a machine that was working - it read as one that had
  finished and had nothing to say. The dialog's own words are the only place that
  distinction lives, and until this section they were checked by dumping the canvas,
  which cannot tell a sentence from a different sentence.

  Three states, and the third is the one that matters most. A disc still being read must
  say so; an identified disc must show the name a player recognises; and a disc that is
  never going to be identified must *stop* saying it is reading and admit it. A reading
  state that no disc ever leaves is worse than a wrong answer, because the player waits
  for it.

  Driven through disc_ingest_present()/disc_ingest_identify() rather than through the
  drive, which is the split chome_disc.h exists for: the whole drive layer is stubbed
  here, so every one of these is reachable without a disc, a drive, or a fork.

  Its own table, installed and removed, exactly as assert_disc_titles() above does - and
  it leaves the same state that one does, so it sits between that section and the next
  without moving anything either of them depends on.
*/
static void assert_disc_dialog_words()
{
	printf("\n== physical disc: what the dialog says at each end ==\n");

	const char *path = ROOT "/classicui/disctitles.txt";
	const int L = 0;

	mkpath(ROOT "/classicui");
	{
		FILE *f = fopen(path, "wb");
		if (f)
		{
			fprintf(f, "#classicui-disctitles 1\n");
			fprintf(f, "SLES01506\tMetal Gear Solid\n");
			fclose(f);
		}
	}
	disc_titles_forget();

	char t[DISC_TITLE_LEN], s[64];

	/* ------------------------------------- present, and not yet identified --- */

	/*
	  No reader is installed and no identify is ingested, which is exactly the helper's
	  first write: "there is a disc" published before the slow read starts, so the
	  front-end has something to show while the drive seeks. On a dual-layer PAL disc
	  that window is seconds long.
	*/
	disc_ingest_present(1);
	check(disc_state() == DISC_SPINNING, "a disc that has arrived is being read");

	disc_test_dlg_text(t, sizeof(t), s, sizeof(s));
	check(!strcmp(t, "Reading the disc"), "and the dialog says so in its largest line");
	check(t[0] != 0, "which is never blank, whatever else is or is not known");

	/* -------------------------------------------- identified, and in the table --- */

	{
		fake_disc d; memset(&d, 0, sizeof(d));
		static const char *const none[] = { "" };
		fake_iso(&d, L, "PLAYSTATION", "PLAYSTATION", none, 0);
		fake_put(&d, L + 20, 0, "BOOT = cdrom:\\SLES_015.06;1", 27, 100);
		disc_set_reader(fake_read, &d);

		disc_ingest_identify(L);
		check(disc_state() == DISC_READY, "reading it finishes");

		disc_test_dlg_text(t, sizeof(t), s, sizeof(s));
		check(!strcmp(t, "Metal Gear Solid"), "and the dialog shows the name, not the serial");
		check(strcmp(t, "Reading the disc"),
			"the reading state is left behind rather than lingering under a known disc");
		check(!strcmp(s, "PlayStation"), "with the console named underneath it");

		disc_reset_reader();
		disc_ingest_present(0);
		(void)disc_take_dirty();
	}

	/* ------------------------------------------ read, and recognised as nothing --- */

	/*
	  The one that must not hang. A reader that answers every sector with bytes matching
	  no signature is a disc this firmware genuinely cannot place - and the required
	  behaviour is that identification *completes* and says so. If this ever comes back
	  "Reading the disc", the front-end has a spinner with no exit, which is the failure
	  the player would sit in front of rather than one they would report.
	*/
	{
		fake_disc d; memset(&d, 0, sizeof(d));
		static const char *const none[] = { "" };
		fake_iso(&d, L, "NOT A CONSOLE", 0, none, 0);
		disc_set_reader(fake_read, &d);

		disc_ingest_present(1);
		disc_ingest_identify(L);

		check(disc_state() == DISC_UNKNOWN, "a disc matching no signature is identified as unknown");
		check(!disc_identify_due(), "and identification is finished, not still pending");

		disc_test_dlg_text(t, sizeof(t), s, sizeof(s));
		check(strcmp(t, "Reading the disc") && strcmp(s, "Reading the disc"),
			"so the dialog stops saying it is reading");
		check(!strcmp(t, "Unrecognised disc") || !strcmp(s, "Unrecognised disc"),
			"and says the disc was not recognised instead");

		disc_reset_reader();
		disc_ingest_present(0);
		(void)disc_take_dirty();
	}

	// Restore, exactly as assert_disc_titles() does: no table, empty drive, no reader.
	unlink(path);
	disc_titles_forget();
	disc_ingest_present(0);
	(void)disc_take_dirty();
	disc_reset_reader();
}

/*
  The disc UI, checked where it can be checked honestly.

  The badge is drawing, so it is verified by reading pixels back out of the corner it
  occupies rather than by asserting a function was called: the failure this catches is
  "the state changed and nothing appeared", which is the only failure a player would
  notice. The two dumps are for eyes - a hash proves something is there, not that it
  looks like a disc.

  Not checked here: that the menu bar's Disc entry can be reached by pressing right the
  right number of times. That count depends on which other entries are visible at this
  profile, so a test asserting it would be asserting the profile, and it would pass
  while the entry led nowhere. What is checked is the thing underneath: the entry
  appears and disappears with the disc, via the same predicate the bar uses.
*/
/*
  How far the badge's recorded rectangle reaches, in cells of the resting radius, and
  stated here rather than borrowed so that this file has its own opinion about it.

  Eighteen cells is what gfx_disc paints with a focus ring - sixteen of disc and two of
  ring - and the breath grows the radius the ring hangs off by GFX_DISC_BREATH_16
  sixteenths, so the crest reaches 18 * 18/16 = 20.25 cells and the box has to be 21. It
  is DISC_BADGE_CELLS in chome_ui.cpp, arrived at the same way; the two agreeing is the
  point, and a change to one that this does not follow shows up as a stale ring in the
  section below.
*/
#define BADGE_CELLS ((18 * (16 + GFX_DISC_BREATH_16) + 15) / 16)

/*
  The badge's focus ring, at the pixel level: two cells thick just outside the disc,
  and breathing on the millisecond clock. A one-cell ring of one steady colour was too
  subtle to see on a real TV at 240p, which is what this pins. Callable at any profile,
  since the cell size scales with it; the caller must already be on the disc tier.

  Sampled on the row through the disc's centre, where the ring is cells 16 and 17 out
  from it and cell 18 is the shelf again. The clock is parked first: at the trough of
  the pulse the ring is exactly the selection blue - disc_focus_col() eases through
  zero there - which is what makes an exact-colour count usable on a colour that
  animates. The frame() after parking moves the clock 16ms past the trough, where the
  eased pulse still rounds to zero.
*/
static void check_focus_ring()
{
	const chome_profile *p = theme_get();
	int r = (p->ts_ui >= 2) ? 32 : 16;
	int cell = r / 16;
	int cy = p->safe_y + p->inset + r;
	int rx0 = p->safe_x + p->inset + r + 16 * cell;
	int rx1 = rx0 + 2 * cell;

	harness_advance(GFX_DISC_PULSE_MS - harness_now() % GFX_DISC_PULSE_MS);
	frame(1);

	check(box_pixels(rx0, cy, rx1, cy + 1, COL_BLUE) == 2 * cell,
		"the focus ring is two cells thick and selection blue at the pulse's trough");
	check(box_pixels(rx1, cy, rx1 + cell, cy + 1, COL_BLUE) == 0,
		"and stops there");

	unsigned long trough = harness_fb_hash_box(rx0, cy, rx1, cy + 1);
	harness_advance(GFX_DISC_PULSE_MS / 2 - 32);
	frame(1);
	check(harness_fb_hash_box(rx0, cy, rx1, cy + 1) != trough,
		"half a pulse later the ring has changed: it breathes on the clock");
	check(box_pixels(rx0, cy, rx1, cy + 1, COL_BLUE) == 0,
		"and at the crest it is no longer the resting blue");
}

/*
  The badge's size animation, and the rectangle it is not allowed to leave.

  Two vertical bands beside the badge, each as tall as its whole recorded box, read at the
  trough of the breath and again at the crest half a period later:

    the sweep, cells 18 to 21 out from the centre. At the resting size nothing is drawn
    there at all - gfx_disc stops at 18 cells with a ring and 16 without - so this band
    changing between the two instants is the growth itself, measured rather than assumed.

    the shelf, cells 21 to 24, which is outside the rectangle disc_note_rect() records.
    Every pixel of it has to read the same at both instants. The spin repaint clips to
    that rectangle, so a badge that painted out here would be leaving pixels nothing ever
    paints back - the ring of stale edge that is the whole hazard of animating a size
    inside a partial repaint.

  Hashes rather than colours: the ring's colour and the disc's rotation are both moving at
  the same time, and the only question here is whether pixels in those bands moved at all.

  The clock is parked on the trough first and stepped to the crest exactly as
  check_focus_ring does, so the two instants are the two ends of the swing and not two
  arbitrary points on it. Callable at any profile; the caller says whether the badge is
  supposed to be breathing where it has left the UI.
*/
static void check_breath(int expect_growth, const char *what)
{
	const chome_profile *p = theme_get();
	int r = (p->ts_ui >= 2) ? 32 : 16;
	int cell = r / 16;
	int cx = p->safe_x + p->inset + r;
	int cy = p->safe_y + p->inset + r;

	int sx0 = cx + 18 * cell, sx1 = cx + BADGE_CELLS * cell;
	int ox1 = cx + (BADGE_CELLS + 3) * cell;
	int y0 = cy - BADGE_CELLS * cell, y1 = cy + BADGE_CELLS * cell;

	// Let whatever the caller just pressed finish moving: the menu bar slides across
	// these rows on its way out, and a frame caught mid-slide is not the badge.
	frame(6);

	harness_advance(GFX_DISC_PULSE_MS - harness_now() % GFX_DISC_PULSE_MS);
	frame(1);
	unsigned long sweep = harness_fb_hash_box(sx0, y0, sx1, y1);
	unsigned long out = harness_fb_hash_box(sx1, y0, ox1, y1);

	harness_advance(GFX_DISC_PULSE_MS / 2 - 32);
	frame(1);

	if (expect_growth) check(harness_fb_hash_box(sx0, y0, sx1, y1) != sweep, what);
	else check(harness_fb_hash_box(sx0, y0, sx1, y1) == sweep, what);

	check(harness_fb_hash_box(sx1, y0, ox1, y1) == out,
		"and the badge paints nothing outside the rectangle the spin repaint clips to");
}

static void assert_disc_ui()
{
	printf("\n== physical disc: the badge and the prompt ==\n");

	// The corner the badge occupies, at whatever profile is current.
	int w = gfx_w(), h = gfx_h();
	int bx = w / 5, by = h / 5;

	disc_ingest_present(0);
	(void)disc_take_dirty();
	frame(6);
	unsigned long empty = harness_fb_hash_box(0, 0, bx, by);

	// A disc arrives: the badge should appear without anything having been read.
	disc_ingest_present(1);
	frame(6);
	unsigned long spinning = harness_fb_hash_box(0, 0, bx, by);
	check(disc_state() == DISC_SPINNING, "a disc arriving is SPINNING");
	check(spinning != empty, "and puts a badge in the corner of the shelf");
	dump("disc-1-spinning");

	/*
	  Identified. The badge stays - it is an indicator, not a notification - and the
	  corner changes again because the label beside it now names the console.
	*/
	fake_disc d; memset(&d, 0, sizeof(d));
	static const char *const none[] = { "" };
	fake_iso(&d, 0, "PLAYSTATION", "PLAYSTATION", none, 0);
	fake_put(&d, 20, 0, "BOOT = cdrom:\\SLUS_006.26;1", 27, 100);
	disc_set_reader(fake_read, &d);
	disc_ingest_identify(0);
	frame(6);

	check(disc_state() == DISC_READY && disc_type() == DISC_T_PSX,
		"identifying it reaches READY as a PlayStation disc");
	unsigned long ready = harness_fb_hash_box(0, 0, bx, by);
	check(ready != empty, "the badge is still there once the disc is known");
	dump("disc-2-ready");

	/*
	  Ejecting has to take the badge away. This is the check that matters most for a
	  drawing that is not part of any screen's own draw path: something that appears on
	  an event and is never removed is the classic version of this bug.
	*/
	disc_ingest_present(0);
	frame(6);
	check(harness_fb_hash_box(0, 0, bx, by) == empty,
		"ejecting takes the badge away and leaves the corner as it was");

	/*
	  Which cores could take a disc. Derived from the type map rather than listed
	  twice, so this also pins that the two Mega Drive disc types collapse to one core
	  rather than offering it to the player twice.
	*/
	const char *ids[16];
	int n = disc_capable_systems(ids, 16);
	check(n >= 4, "there are several disc-capable cores");

	int md = 0, psx = 0, tg = 0;
	for (int i = 0; i < n; i++)
	{
		if (!strcmp(ids[i], "md")) md++;
		if (!strcmp(ids[i], "psx")) psx++;
		if (!strcmp(ids[i], "tg16")) tg++;
	}
	check(psx == 1 && tg == 1, "psx and tg16 are each offered once");
	check(md == 1, "and the Mega Drive core once, though two disc types map to it");

	int cap = disc_capable_systems(ids, 2);
	check(cap == 2, "and the caller's limit is respected");

	/* --------------------------------------------------- the focus tier --- */

	/*
	  Up from the shelf reaches the disc first and the menu bar second, but only while
	  there is a disc. Checked by pressing keys rather than by reading `screen`, because
	  the thing that would break is the order, and the order is only visible from the
	  outside.

	  The menu bar and the disc tier both draw over the top of the shelf, so the top
	  strip of the canvas tells them apart: the bar fills it with entries, the disc tier
	  puts a plate and a name in the corner.
	*/
	disc_ingest_present(1);
	fake_disc d2; memset(&d2, 0, sizeof(d2));
	static const char *const none2[] = { "" };
	fake_iso(&d2, 0, "PLAYSTATION", "PLAYSTATION", none2, 0);
	fake_put(&d2, 20, 0, "BOOT = cdrom:\\SLUS_006.26;1", 27, 100);
	disc_set_reader(fake_read, &d2);
	disc_ingest_identify(0);
	frame(6);

	/*
	  Asserted on the screen id rather than on pixels. Both of these screens animate, so
	  two visits to the same one hash differently and two different ones might not -
	  which is exactly the trap the first version of this test fell into.
	*/
	enum { S_HOME = 0, S_MENUBAR = 1, S_DISC = 17, S_DISCBAR = 18 };

	check(chome_screen_id() == S_HOME, "starting on the shelf");

	press(KEY_UP);
	check(chome_screen_id() == S_DISCBAR, "up from the shelf focuses the disc, not the menu bar");
	dump("disc-3-focused");
	check_focus_ring();

	/*
	  And the same ring at 240p, which is where the defect lived: a cell is one pixel
	  there, so this is the profile on which the one-cell ring was invisible on a real
	  TV. The screen survives the resolution change, so the tier is still focused.
	*/
	harness_set_fb(320, 240);
	gfx_shutdown();
	theme_update(320, 240, 3);
	/*
	  Nothing here marks the UI dirty - on hardware the mode change does - so a bare
	  switch leaves the new, zeroed framebuffers repainted only where the disc spins.
	  Bounce to the menu bar and back: two full repaints at the new size, landing on
	  the tier again.
	*/
	press(KEY_UP);
	press(KEY_DOWN);
	check(chome_screen_id() == S_DISCBAR, "still on the disc tier at 240p");
	check_focus_ring();
	dump("disc-8-focused-240p");

	harness_set_fb(1280, 720);
	gfx_shutdown();
	theme_update(1280, 720, 1);
	press(KEY_UP);
	press(KEY_DOWN);
	check(chome_screen_id() == S_DISCBAR, "and back at 720p");

	press(KEY_UP);
	check(chome_screen_id() == S_MENUBAR, "a second up carries on to the menu bar");

	press(KEY_DOWN);
	check(chome_screen_id() == S_DISCBAR, "coming back down lands on the disc again");

	press(KEY_DOWN);
	check(chome_screen_id() == S_HOME, "and once more is the shelf");

	// A on the tier is what opens the prompt, so the tier is not a dead end.
	press(KEY_UP);
	press(KEY_ENTER);
	check(chome_screen_id() == S_DISC, "confirming on the disc opens its prompt");
	dump("disc-4-panel");

	press(KEY_ESC);
	check(chome_screen_id() == S_DISCBAR, "and back returns to the disc, not to the shelf");
	press(KEY_ESC);

	/*
	  Ejected while the prompt is up. Both disc screens describe something that is no
	  longer in the drive, so staying on one would leave the player offering to play
	  nothing.
	*/
	press(KEY_UP);
	press(KEY_ENTER);
	check(chome_screen_id() == S_DISC, "on the prompt again");
	disc_ingest_present(0);
	frame(6);
	check(chome_screen_id() == S_HOME, "taking the disc out leaves the prompt rather than stranding it");

	/*
	  With no disc there is no tier: one press has to reach the menu bar, or a player
	  with an empty drive pays for a feature they are not using.
	*/
	press(KEY_UP);
	check(chome_screen_id() == S_MENUBAR, "with no disc, one up reaches the menu bar as it always did");
	press(KEY_DOWN);
	check(chome_screen_id() == S_HOME, "and down is the shelf, with no tier in between");

	disc_reset_reader();
	disc_ingest_present(0);
	(void)disc_take_dirty();
}

/*
  The badge breathes when it has focus, and nowhere else.

  His instruction, and a section of its own rather than two lines in the one above,
  because animating a *size* inside the partial repaint is not like animating a colour:
  the rectangle the repaint clips to comes from the previous frame, so a badge that grew
  past it would paint its new edge outside the clip and leave the old edge standing -
  in the corner, permanently, since nothing else ever repaints there.

  Three states, because "only on the tier" is half the requirement: the shelf, the tier,
  and the tier with the dialog open over it. And then the frame as a whole at the crest,
  against a full repaint of the same instant, which is the assertion that actually catches
  a rectangle a cell too small.
*/
static void assert_disc_breath()
{
	printf("\n== physical disc: the focused badge breathes ==\n");

	enum { S_HOME = 0, S_DISC = 17, S_DISCBAR = 18, S_MENUBAR = 1 };

	// A known PlayStation disc, on the shelf, as the sections either side of this build one.
	disc_ingest_present(1);
	fake_disc d; memset(&d, 0, sizeof(d));
	static const char *const none[] = { "" };
	fake_iso(&d, 0, "PLAYSTATION", "PLAYSTATION", none, 0);
	fake_put(&d, 20, 0, "BOOT = cdrom:\\SLUS_006.26;1", 27, 100);
	disc_set_reader(fake_read, &d);
	disc_ingest_identify(0);
	frame(8);
	check(chome_screen_id() == S_HOME && disc_state() == DISC_READY,
		"on the shelf with a known disc");

	check_breath(0, "unfocused on the shelf, the badge holds its size");

	press(KEY_UP);
	check(chome_screen_id() == S_DISCBAR, "up focuses the disc");
	check_breath(1, "and focused it swells over the pulse and comes back");

	/*
	  The frame check_breath() has just left on screen is the crest, drawn by the spin
	  repaint - which is what makes this comparison possible at all: the clock has not moved
	  since, so two full repaints of the same instant have the same rotation and the same
	  size. Up to the menu bar and back down is those two full repaints, landing on the tier
	  again. A rectangle one cell short shows up right here and in no other check: the
	  partial frame would carry a rim of the previous, smaller badge that the full frame
	  drew over.
	*/
	{
		const chome_profile *p = theme_get();
		int cell = ((p->ts_ui >= 2) ? 32 : 16) / 16;
		int w = gfx_w(), h = gfx_h();

		check(gfx_damage_rows() <= 2 * BADGE_CELLS * cell,
			"the crest frame was drawn by the partial path");
		unsigned long crest = harness_fb_hash_box(0, 0, w, h);

		chome_handle(KEY_UP);
		chome_handle(KEY_UP | UPSTROKE);
		check(chome_screen_id() == S_MENUBAR, "the menu bar, a full repaint");
		chome_handle(KEY_DOWN);
		chome_handle(KEY_DOWN | UPSTROKE);
		check(chome_screen_id() == S_DISCBAR, "and the tier again, with the clock still");
		check(gfx_damage_rows() == h, "that one repainted every row");

		check(harness_fb_hash_box(0, 0, w, h) == crest,
			"a partial frame at the crest of the breath is byte-identical to a full "
			"repaint of the same instant");
	}

	/*
	  A pair for eyes, the two ends of the same breath: a hash can say the badge changed
	  size, and only the pictures can say whether it looks intentional. Parked on the clock
	  rather than taken a frame apart, or the pair would be two arbitrary points on the
	  swing and the difference would look like noise.
	*/
	harness_advance(GFX_DISC_PULSE_MS - harness_now() % GFX_DISC_PULSE_MS);
	frame(1);
	dump("disc-breath-1-tier-trough");
	harness_advance(GFX_DISC_PULSE_MS / 2 - 32);
	frame(1);
	dump("disc-breath-2-tier-crest");

	/*
	  With the dialog open the badge is behind the scrim and has no focus, so it must be
	  still: something growing in the corner would pull the eye off the panel that has just
	  opened, and the ring is gone for the same reason.
	*/
	press(KEY_ENTER);
	check(chome_screen_id() == S_DISC, "its dialog opens over the badge");
	check_breath(0, "with the dialog up the badge behind it holds its size");
	dump("disc-breath-3-dialog");

	press(KEY_ESC);
	check(chome_screen_id() == S_DISCBAR, "back on the tier");

	/*
	  And at 240p, which is the profile the breath exists for and the one it is hardest on:
	  a cell is one pixel there, so the swell is three whole sizes - 32, 34 and 36 pixels
	  across - rather than a continuum. Before gfx_disc mapped its grid onto the pixel box
	  there were no sizes in between at all.

	  Nothing here marks the UI dirty - on hardware the mode change does - so the bounce to
	  the menu bar and back is what gives the new framebuffers two full repaints at the new
	  size, as in the section above.
	*/
	harness_set_fb(320, 240);
	gfx_shutdown();
	theme_update(320, 240, 3);
	press(KEY_UP);
	press(KEY_DOWN);
	check(chome_screen_id() == S_DISCBAR, "still on the tier at 240p");
	check_breath(1, "and the badge breathes there too, in whole pixels");

	harness_advance(GFX_DISC_PULSE_MS - harness_now() % GFX_DISC_PULSE_MS);
	frame(1);
	dump("disc-breath-4-tier-trough-240p");
	harness_advance(GFX_DISC_PULSE_MS / 2 - 32);
	frame(1);
	dump("disc-breath-5-tier-crest-240p");

	/*
	  And one frame from the middle of the swing, at 240p, which is the picture worth
	  actually looking at: the size in between is the one the grid-to-pixel mapping
	  invented, drawn with one cell in sixteen a pixel wider than its neighbours. If that
	  read as a dented circle rather than as a disc, this is where it would show. A quarter
	  of the way along the eased triangle - x = 128 of 256 - is a radius of 17 at this
	  profile, one pixel between the two frames above.
	*/
	harness_advance(GFX_DISC_PULSE_MS - harness_now() % GFX_DISC_PULSE_MS);
	frame(1);
	harness_advance(GFX_DISC_PULSE_MS * 128 / 512 - 32);
	frame(1);
	dump("disc-breath-6-tier-mid-240p");

	press(KEY_DOWN);
	check(chome_screen_id() == S_HOME, "down leaves the tier for the shelf");
	check_breath(0, "where it is still again");

	harness_set_fb(1280, 720);
	gfx_shutdown();
	theme_update(1280, 720, 1);
	press(KEY_UP);
	press(KEY_DOWN);
	check(chome_screen_id() == S_HOME, "and back at 720p on the shelf");

	disc_reset_reader();
	disc_ingest_present(0);
	(void)disc_take_dirty();
	frame(6);
}

/*
  The partial repaint path: while a disc spins on the shelf, only its rectangle is
  recomposed and copied, and everything else on the presented frame is byte-identical
  frame to frame. Byte-identical is checkable here because the framebuffers alternate
  and gfx_end() unions each frame's damage with the previous frame's - so if that union
  were wrong, these very hashes would flicker between a current and a stale buffer.

  The trap the file's own comments warn about - two hashes of a region containing the
  disc differ with no bug present - is why every equality below is of a region that
  excludes the badge's box, and the box itself is only ever asserted to have *changed*.
*/
/*
  Tick until the spin path actually paints, bounded.

  The spin timer fires every GFX_DISC_PART_MS but the repaint now follows the *angle*: a
  tick on which the disc's 64-position step has not moved paints nothing at all (see
  disc_spin_sig() in chome_ui.cpp). At the slow rate a step is 62.5ms, so "advance 60ms
  and expect a frame" - which is what these sections did - is a coin flip over where the
  phase happens to sit. This waits for the paint the way the device would: tick by tick,
  up to a bit over one full step.
*/
static int spin_paint()
{
	for (int i = 0; i < 8; i++)
	{
		int flips = harness_present_count();
		harness_advance(16);
		chome_handle(0);
		if (harness_present_count() != flips) return 1;
	}
	return 0;
}

static void assert_partial_repaint()
{
	printf("\n== partial repaint: the disc turns without repainting the world ==\n");

	// A PSX disc in the drive, identified, sitting on the shelf.
	disc_ingest_present(1);
	fake_disc d; memset(&d, 0, sizeof(d));
	static const char *const none[] = { "" };
	fake_iso(&d, 0, "PLAYSTATION", "PLAYSTATION", none, 0);
	fake_put(&d, 20, 0, "BOOT = cdrom:\\SLUS_006.26;1", 27, 100);
	disc_set_reader(fake_read, &d);
	disc_ingest_identify(0);
	frame(8);
	check(chome_screen_id() == 0 && disc_state() == DISC_READY, "on the shelf with a known disc");

	// The badge's box, from the same numbers draw_disc_badge() and disc_note_rect() use:
	// centre at safe_x+inset+r, BADGE_CELLS of r/16 pixels each side.
	const chome_profile *p = theme_get();
	int r = (p->ts_ui >= 2) ? 32 : 16;
	int cell = r / 16;
	int bx0 = p->safe_x + p->inset + r - BADGE_CELLS * cell;
	int by0 = p->safe_y + p->inset + r - BADGE_CELLS * cell;
	int bx1 = bx0 + 2 * BADGE_CELLS * cell, by1 = by0 + 2 * BADGE_CELLS * cell;
	int w = gfx_w(), h = gfx_h();

	// Everything outside the box, in four hashes; and the box itself.
	unsigned long L = harness_fb_hash_box(0, 0, bx0, h);
	unsigned long R = harness_fb_hash_box(bx1, 0, w, h);
	unsigned long T = harness_fb_hash_box(bx0, 0, bx1, by0);
	unsigned long B = harness_fb_hash_box(bx0, by1, bx1, h);
	unsigned long box = harness_fb_hash_box(bx0, by0, bx1, by1);
	int flips = harness_present_count();

	/*
	  Half a second of nothing but the spin timer. At the slow rate that is ~8 of the 64
	  rotation steps, so the disc must have turned; and no key arrived and no state
	  changed, so nothing else may have.
	*/
	frame(32);

	check(harness_present_count() > flips, "spin repaints still reach the framebuffer");
	check(harness_fb_hash_box(0, 0, bx0, h) == L, "left of the badge is byte-identical");
	check(harness_fb_hash_box(bx1, 0, w, h) == R, "and right of it");
	check(harness_fb_hash_box(bx0, 0, bx1, by0) == T, "and above it");
	check(harness_fb_hash_box(bx0, by1, bx1, h) == B, "and below it");
	check(harness_fb_hash_box(bx0, by0, bx1, by1) != box, "while the disc itself has turned");
	check(gfx_damage_rows() <= 2 * BADGE_CELLS * cell, "a spin frame damages only the disc's rows");

	/*
	  The strongest thing that can be said about the partial path: a frame it finishes
	  is byte-for-byte the frame a full repaint of the same instant produces. The disc's
	  rotation is memoised on the clock (see disc_step()), so two repaints in the same
	  millisecond draw the same rotation - which lets a full repaint be forced without
	  the subject of the comparison moving: up onto the disc tier and straight back
	  down, no clock in between, is two full repaints that end on the very frame the
	  partial one drew.
	*/
	check(spin_paint(), "a spin frame arrives within one rotation step");
	check(gfx_damage_rows() <= 2 * BADGE_CELLS * cell, "the frame under comparison took the partial path");
	unsigned long partial_frame = harness_fb_hash_box(0, 0, w, h);

	chome_handle(KEY_UP);                      // the disc tier: a structural change
	check(gfx_damage_rows() == h, "a structural change still repaints every row");
	chome_handle(KEY_UP | UPSTROKE);
	chome_handle(KEY_DOWN);                    // and back, still at the same instant
	chome_handle(KEY_DOWN | UPSTROKE);
	check(chome_screen_id() == 0, "back on the shelf without the clock moving");

	check(harness_fb_hash_box(0, 0, w, h) == partial_frame,
		"a partially repainted frame is byte-identical to a full repaint of the same instant");

	/*
	  The ticks between angles paint nothing at all. The spin timer still fires every
	  GFX_DISC_PART_MS, but at the slow rate the 64-position step moves every 62.5ms -
	  so after the frame a step change just earned, the next 16ms tick cannot cross
	  another boundary, and repainting on it would copy the disc's rectangle into the
	  framebuffer byte-identical. That copy at 60fps was most of what a disc on a 720p
	  canvas cost. The skipped tick has to leave the shown frame exactly alone, which
	  presenting nothing does by construction.
	*/
	{
		check(spin_paint(), "a spin frame lands on a step change");
		unsigned long shown = harness_fb_hash_box(0, 0, w, h);
		int flips = harness_present_count();

		harness_advance(16);
		chome_handle(0);
		check(harness_present_count() == flips,
			"the tick after it, inside the same rotation step, paints nothing");
		check(harness_fb_hash_box(0, 0, w, h) == shown, "and the screen is untouched by it");

		check(spin_paint(), "the tick that crosses the next step paints again");
		check(harness_fb_hash_box(0, 0, w, h) != shown, "and the disc has turned on it");
	}

	/*
	  The buffer-alternation carry, provoked head on: a full frame (the tier, whose
	  legend and ring differ from the shelf's) followed by one partial frame. The
	  partial lands in the buffer the full frame never touched, so unless its copy
	  carries the previous frame's damage across, that buffer still shows the shelf
	  everywhere the disc is not.
	*/
	chome_handle(KEY_UP);
	chome_handle(KEY_UP | UPSTROKE);
	unsigned long tier_right = harness_fb_hash_box(bx1, 0, w, h);
	check(tier_right != R, "the tier reads differently to the shelf outside the badge");

	check(spin_paint(), "one partial, into the other buffer");
	check(gfx_damage_rows() <= 2 * BADGE_CELLS * cell, "and it was partial");
	check(harness_fb_hash_box(bx1, 0, w, h) == tier_right,
		"one partial frame later the other buffer shows the tier, not the stale shelf");

	press(KEY_DOWN);

	/*
	  The same equivalence on the disc prompt, which is the hard case for reconstructing
	  what is underneath: there the region sits on a scrim over the shelf and on a
	  panel, and the badge and the prompt's disc are both on screen, so the repainted
	  rectangle is the union of the two. A partial repaint that guessed at any layer -
	  or drew the scrim's checkerboard out of register with the clip edge - diverges
	  from the full frame here.
	*/
	press(KEY_UP);
	press(KEY_ENTER);
	check(chome_screen_id() == 17, "on the disc prompt");

	check(spin_paint(), "one spin frame, the partial path");
	unsigned long prompt_partial = harness_fb_hash_box(0, 0, w, h);

	chome_handle(KEY_ESC);                     // back to the tier: a full repaint
	chome_handle(KEY_ESC | UPSTROKE);
	chome_handle(KEY_ENTER);                   // and onto the prompt again, same instant
	chome_handle(KEY_ENTER | UPSTROKE);
	check(chome_screen_id() == 17, "on the prompt again without the clock moving");
	check(harness_fb_hash_box(0, 0, w, h) == prompt_partial,
		"a partial repaint over the scrim and panel matches a full repaint of the same instant");

	press(KEY_ESC);
	press(KEY_ESC);

	// Eject through the UI's own path this time, so the badge's removal is drawn.
	disc_reset_reader();
	disc_ingest_present(0);
	frame(6);
	check(harness_fb_hash_box(bx0, by0, bx1, by1) != box, "ejecting repaints the corner");
}

/*
  The card band a slide clips to, from the same numbers draw_card(), draw_shelf(),
  draw_pips() and draw_position() note into slide_note_rows(). A copy on purpose, and the
  two agreeing is the point - the same arrangement BADGE_CELLS above is in. A test that
  asked the front-end for its own band would assert nothing at all about where the band
  ought to be, and where it ought to be is the only thing that can go wrong here: too
  small by a row and the partial path leaves a line of an older frame behind that nothing
  ever comes back to repaint.
*/
#define CARD_RING 2
static int card_shadow_off(int h) { int sd = h / 40; return sd < 2 ? 2 : sd; }

static void slide_band_expect(const chome_profile *p, int *y0, int *y1)
{
	// The tallest card the shelf can draw, its focus ring above it and its drop shadow
	// below - the crest of the growth, not whatever size a card happens to be right now.
	*y0 = p->y_shelf - p->sel_h - CARD_RING;
	*y1 = p->y_shelf + card_shadow_off(p->sel_h);

	// The pips, which stay live during a scroll and are therefore in the band.
	int pips = p->y_pips + 6 * ((p->id == PROF_HD) ? 2 : 1);
	if (pips > *y1) *y1 = pips;

	// And the position line, which stays live for the same reason. 240p has none.
	if (p->id != PROF_LO)
	{
		int pos = p->y_pos + 8 * p->ts_tiny;
		if (pos > *y1) *y1 = pos;
	}
}

/*
  A full repaint of the instant already on screen, with the clock held still.

  The comparison the section below is built on needs two repaints of one moment: the
  partial frame, and the frame a full repaint would have produced instead. Moving the clock
  to get the second is not allowed - the ease would advance and the cards would be
  somewhere else - so the full frame has to be provoked by something that changes what is
  drawn without changing when it is drawn.

  The menu bar toggle is that: it marks the screen dirty, and with dt == 0 no animation can
  advance, so bar_y is still 0 and selF is exactly where the partial frame left it. Going
  there and straight back is two full repaints of one instant, the second of them of the
  shelf. The legend differs between the two screens, which is what the return value checks:
  without that, a broken forcing function would make every comparison below pass by
  comparing a frame with itself.

  The menu button is deliberately not one of the keys whose release commits the chrome (see
  chome_handle), so this can be used in the middle of a held arrow without committing the
  very deferral under test.
*/
static int force_full_repaint()
{
	int w = gfx_w(), h = gfx_h();
	unsigned long was = harness_fb_hash_box(0, 0, w, h);

	chome_handle(KEY_MENU);                    // the menu bar: dirty, and a different legend
	chome_handle(KEY_MENU | UPSTROKE);
	unsigned long other = harness_fb_hash_box(0, 0, w, h);

	chome_handle(KEY_MENU);                    // and back to the shelf, same instant
	chome_handle(KEY_MENU | UPSTROKE);

	return other != was;
}

/*
  Browsing the shelf, which is the thing this front-end is for and was the most expensive
  frame it drew.

  Moving the cursor changes the cards *and* the title, the system line, the file line, the
  prompts, the pips and the position line, and the union of all that spans most of the
  height of the screen - so every frame of every slide was a full repaint, twelve of them
  per tap and one for every frame of a hold. What this section asserts is the two
  halves of the way out. The cards follow the eased selF and are one contiguous band of
  rows, so their frames can be clipped to it; the title and the prompts follow a committed
  selection that does not move while the shelf does, so there is nothing outside the band
  for those frames to have to repaint.

  Both claims need proving in the same direction, because the failure mode is silent. A
  band one row short does not draw anything wrong - it draws nothing at all in that row,
  and the row keeps whatever it had until something else asks for a full frame. So the
  strongest check here is not "the damage is small" but "the frame is exactly the frame a
  full repaint would have produced", asserted on every frame of a slide including the last
  one - where the selected card is at its full size and its focus ring is on the top row of
  the band. That check is what found the snap in animate(): see the comment there.
*/
/*
  Where the stack badge lands on the selected card. Computed rather than hunted for: the
  selected card is sel_w x sel_h and centred, and draw_card() insets the badge by 4, so
  there is nothing to search and no chance of matching something else that happens to be
  the same colour elsewhere on the shelf.

  Kept in step with draw_card() by hand. That is a copy, deliberately - a check that asked
  the drawing code where it drew would agree with it for ever while both drifted away from
  the picture.
*/
static void sel_badge_box(int *bx, int *by, int *bw)
{
	const chome_profile *p = theme_get();
	int box = p->sel_h / 5;
	if (box > 16) box = 16;
	if (box < 6) box = 6;
	*bx = (p->w - p->sel_w) / 2 + 4;
	*by = p->y_shelf - p->sel_h + 4;
	*bw = box + 4;
}

// The entry index of a game whose title contains `want`, found the same way
// select_titled() finds it so the two cannot disagree about which card is meant.
static int entry_titled(const char *want)
{
	for (int i = 0; i < lib_view_count(); i++)
	{
		const chome_entry *e = lib_view_entry(i);
		if (e->kind != ENT_GAME) continue;
		chome_item *it = lib_item(e->game);
		if (it && strstr(it->title, want)) return i;
	}
	return -1;
}

static int badge_outline_px()
{
	int bx, by, bw;
	sel_badge_box(&bx, &by, &bw);
	const uint32_t *fb = harness_fb_shown();
	int w = gfx_w(), h = gfx_h(), n = 0;
	if (!fb) return -1;
	for (int y = by; y < by + bw && y < h; y++)
	{
		for (int x = bx; x < bx + bw && x < w; x++)
		{
			if (fb[(size_t)y * w + x] == COL_PANELHI) n++;
		}
	}
	return n;
}

/*
  A card holding several files says so on its face.

  The reader who asked for this had a NES romset split into USA/ and Europe/ folders, and
  wanted the card to admit it carries both - the grouping itself was already fixed, but
  nothing on the card showed it. A stack of cards rather than a count, because at 240p a
  card is 84 px wide and its bottom band already holds the title.

  Checked as pixels and by difference: the same screen with a multi-file card selected and
  with a single-file card selected, at the same position, so anything else on the shelf is
  common to both and cancels out.
*/
/*
  The shoulders jump by first letter.

  This replaced paging, and the reason is measurable rather than aesthetic: a page is
  p->visible cards - three at 240p, five at 720p - so on the owner's 1431-game shelf it
  took roughly fifty presses to cross the letter M looking for one game. A letter is the
  unit a person searching actually holds in their head.

  Checked against the *view*, not the pixels: what matters is which entry is selected, and
  the assertions below are about the initial of the name at that index. Reading it off the
  screen would tie the check to the layout of a card.
*/
// The selected index, and the letter the front-end would file that entry under.
static int view_sel() { return chome_sel_index(); }

/*
  A deliberate second implementation of jump_initial(). The two agreeing is the point: a
  helper that asked the front-end which letter it chose would agree with it for ever, including
  when both were wrong.
*/
static char view_initial(int i)
{
	const chome_entry *e = lib_view_entry(i);
	if (!e) return 0;

	const char *s = 0;
	if (e->kind == ENT_GAME)
	{
		chome_item *it = lib_item(e->game);
		s = it ? it->title : 0;
	}
	else s = e->label;

	if (!s || !*s) return 0;
	unsigned char c = (unsigned char)*s;
	return isalpha(c) ? (char)tolower(c) : '#';
}

static void assert_letter_jump()
{
	printf("\n== the shoulders jump by letter, not by page ==\n");

	chome_leave();
	press(KEY_MENU, 20);
	for (int i = 0; i < 80 && lib_scanning(); i++) frame(2);
	frame(30);

	check(chome_screen_id() == 0, "on the shelf");

	int n = lib_view_count();
	check(n > 20, "with enough entries for a jump to mean something");

	// The initial of whatever is selected, read the way the front-end reads it.
	int at = -1;
	shelf_rewind();
	frame(20);
	at = view_sel();
	check(at == 0, "rewound to the first entry");

	/*
	  Forward: every jump must land on a *different* letter from the one it left, and the
	  entry before it must share the old letter - that is what "the first entry of the next
	  letter" means, and it is the property a naive "skip N" would fail.
	*/
	int jumps = 0, bad_start = 0, bad_prev = 0;
	char seen[64];
	int nseen = 0;
	for (int guard = 0; guard < 60; guard++)
	{
		char before = view_initial(view_sel());
		int was = view_sel();
		press(KEY_EQUAL, 4);
		frame(12);
		int now = view_sel();
		if (now == was) break;               // ran out of shelf

		jumps++;
		if (view_initial(now) == before) bad_start++;
		if (now > 0 && view_initial(now - 1) != before) bad_prev++;

		/*
		  Only the games are collected for the monotonic check below. The shelf opens with
		  folders - Favourites, Systems and the rest - in a curated order that is
		  deliberately not alphabetical, so the first few jumps legitimately go s, c, #
		  before the sorted games begin. Asserting monotonicity across them failed the code
		  for doing the right thing.
		*/
		if (now >= leading_folders() && nseen < (int)sizeof(seen)) seen[nseen++] = view_initial(now);
	}

	printf("  %d forward jumps, letters:", jumps);
	for (int i = 0; i < nseen && i < 20; i++) printf(" %c", seen[i] ? seen[i] : '?');
	printf("\n");

	check(jumps >= 5, "the shoulder crosses several letters on this shelf");
	check(bad_start == 0, "every jump lands on a letter different from the one it left");
	check(bad_prev == 0, "and on the FIRST entry of that letter, not into the middle of it");

	/*
	  And what the right shoulder does once there is no next letter.

	  It used to refuse, which from the sofa reads as a dead button rather than as the end
	  of the alphabet - and the end of the list is plainly what the press was asking for.
	  Driven all the way to the end rather than computed, because the answer depends on the
	  view's real contents: leading folders are in curated order and the last entry is
	  whatever the shelf actually finishes with.
	*/
	{
		int last = lib_view_count() - 1;

		for (int i = 0; i < 80 && chome_sel_index() != last; i++) press(KEY_EQUAL, 3);
		check(chome_sel_index() == last,
			"pressing right past the last letter lands on the last entry");

		press(KEY_EQUAL, 3);
		check(chome_sel_index() == last, "and pressing it again there stays put");

		for (int i = 0; i < 80 && chome_sel_index() != 0; i++) press(KEY_MINUS, 3);
		check(chome_sel_index() == 0, "left still walks back to the first entry");
	}

	/*
	  Monotonic: the shelf is sorted, so the letters a forward jump visits must not go
	  backwards. A jump that overshoots and wraps would satisfy every check above.
	*/
	int backwards = 0;
	for (int i = 1; i < nseen; i++) if (seen[i] < seen[i - 1]) backwards++;
	check(backwards == 0, "and the letters it visits only ever move forwards");

	/*
	  Back: from the middle of a letter, one press goes to the head of *that* letter rather
	  than the previous one. A mistimed press should cost one press, not a whole letter.
	*/
	press(KEY_RIGHT, 4);
	frame(12);
	int mid = view_sel();
	char midc = view_initial(mid);
	if (view_initial(mid - 1) == midc)          // genuinely mid-letter
	{
		press(KEY_MINUS, 4);
		frame(12);
		int head = view_sel();
		check(view_initial(head) == midc && (head == 0 || view_initial(head - 1) != midc),
			"back from mid-letter goes to the head of that same letter");

		// And only the second press leaves it.
		press(KEY_MINUS, 4);
		frame(12);
		check(view_initial(view_sel()) != midc, "and the next press leaves for the one before");
	}
	else printf("  (selected entry was already at a letter head; mid-letter case skipped)\n");

	dump("letterjump-1-shelf");
}

static void assert_stack_badge()
{
	printf("\n== a card with several files behind it wears a stack ==\n");

	disc_reset_reader();
	disc_ingest_present(0);
	(void)disc_take_dirty();

	/*
	  Re-entered from scratch rather than trusting whichever view the section before this
	  one left up. select_titled() searches the *current* view and only presses LEFT, which
	  cannot climb out of a system folder - so inheriting a sub-view makes every check here
	  fail for a reason that has nothing to do with the badge. The carousel section below
	  learned this the same way.
	*/
	chome_leave();
	press(KEY_MENU, 20);
	for (int i = 0; i < 80 && lib_scanning(); i++) frame(2);
	frame(40);

	check(chome_screen_id() == 0, "on the shelf");

	// Final Fantasy VII is three discs behind one card in the fake library.
	check(select_titled("Final Fantasy VII") == 1, "selected a game with several files");
	int multi = entry_titled("Final Fantasy VII");
	check(entry_nvar(multi) == 3, "and it really does have three");
	int with = badge_outline_px();
	printf("  badge outline pixels, three files: %d\n", with);
	check(with > 0, "the badge is drawn on it");
	dump("stack-1-three-files");

	// Bonk's Adventure is one file, and the shelf puts it in the same place.
	check(select_titled("Bonk") == 1, "selected a game with one file");
	int single = entry_titled("Bonk");
	check(entry_nvar(single) == 1, "and it really does have one");
	int without = badge_outline_px();
	printf("  badge outline pixels, one file:    %d\n", without);
	check(without == 0, "and no badge is drawn on that one");
	dump("stack-2-one-file");

	/*
	  The badge must sit inside the card, because draw_card() records the damage band from
	  the card's own geometry. A badge hanging outside it would be drawn into rows nothing
	  repaints, which shows up as dirt that survives a slide - so assert the band is what
	  it was rather than trusting the arithmetic.
	*/
	const chome_profile *p = theme_get();
	int bx, by, bw;
	sel_badge_box(&bx, &by, &bw);
	check(by >= p->y_shelf - p->sel_h && by + bw <= p->y_shelf,
		"and it is inside the card's own rows, so the slide band still covers it");

	// It must also not collide with the favourite star, which is the top right corner.
	check(bx + bw < (p->w + p->sel_w) / 2 - 16 - 6,
		"and clear of the favourite star opposite it");

	/*
	  And again at 240p, which is the canvas that actually matters: the owner's MiSTer is
	  analog only, so this is the size he will judge it at. A card there is 61 rows tall, so
	  the badge is 12 px with 1 px gaps - the size where a picto would have turned to
	  porridge and the reason this is drawn as rectangles.
	*/
	harness_set_fb(320, 240);
	chome_leave();
	press(KEY_MENU, 20);
	for (int i = 0; i < 80 && lib_scanning(); i++) frame(2);
	frame(40);

	check(select_titled("Final Fantasy VII") == 1, "240p: on the multi-file card");
	int lo_px = badge_outline_px();
	const chome_profile *lp = theme_get();
	int lb = lp->sel_h / 5; if (lb > 16) lb = 16; if (lb < 6) lb = 6;
	printf("  240p card %dx%d, badge %d px, outline pixels %d\n",
		lp->sel_w, lp->sel_h, lb, lo_px);
	check(lo_px > 0, "240p: the badge is drawn");
	check(lb >= 6 && lb <= lp->sel_h / 3, "240p: and it is a badge, not a third of the card");
	dump("stack-3-240p");

	harness_set_fb(1280, 720);
	chome_leave();
	press(KEY_MENU, 20);
	for (int i = 0; i < 80 && lib_scanning(); i++) frame(2);
	frame(20);
}

static void assert_carousel_slide()
{
	printf("\n== the carousel: a slide repaints the card row and nothing else ==\n");

	// No disc, so nothing else on screen has a clock of its own.
	disc_reset_reader();
	disc_ingest_present(0);
	(void)disc_take_dirty();
	frame(6);

	int w = gfx_w(), h = gfx_h();
	check(chome_screen_id() == 0 && w == 1280 && h == 720, "on the shelf at 720p");

	/*
	  Re-entered from scratch, which is the idiom the profile walk uses, rather than trusted
	  to whichever view the section before this one left up.

	  Worth the four lines. One attempt at this ran on the shelf it inherited, which turned
	  out to be a twelve-card Systems view that B would not pop out of - so every scroll below
	  reached the end of the shelf, and a scroll that reaches the end nudges. A nudge is a
	  full repaint and it stays due for 160 ms, so the section measured nothing but nudges and
	  every check failed for a reason that had nothing to do with the band. The assertion on
	  the size of the shelf is there so that can never pass quietly again.
	*/
	chome_leave();
	press(KEY_MENU, 20);
	for (int i = 0; i < 80 && lib_scanning(); i++) frame(2);
	frame(40);

	int entries = lib_view_count();
	printf("  the root shelf has %d entries\n", entries);
	check(entries > 20, "out on the root shelf, which has room to scroll in");

	// Onto the games, past the folders that lead the shelf, and one further in so that
	// nothing below runs into either end.
	select_first_game();
	press(KEY_RIGHT, 6);
	frame(30);

	/*
	  And one tap each way, settled, before anything is measured. art_step() decodes one
	  cover per call and force_full_repaint() below calls into the front-end four times, so a
	  cover landing between a partial frame and the full repaint it is compared against would
	  fail the comparison for a reason that is not a bug: walking the neighbouring cards first
	  leaves their art already in the cache.
	*/
	press(KEY_RIGHT);
	frame(30);
	press(KEY_LEFT);
	frame(30);

	const chome_profile *p = theme_get();
	int by0, by1;
	slide_band_expect(p, &by0, &by1);
	int band_rows = by1 - by0 + 1;

	printf("  the card band is rows %d..%d - %d of %d\n", by0, by1, band_rows, h);
	check(band_rows > 0 && band_rows < h / 2, "the band is under half the height of the screen");

	/*
	  A tap: press, release, and let the ease run out on its own.

	  The release is what commits the chrome, so that frame - and only that frame - has the
	  new title in it and repaints the world for it. Everything after it is cards.
	*/
	chome_handle(KEY_RIGHT);
	harness_advance(16);
	chome_handle(KEY_RIGHT | UPSTROKE);
	check(gfx_damage_rows() == h, "the release of a tap repaints the world once, for the title");

	unsigned long above = harness_fb_hash_box(0, 0, w, by0);
	unsigned long below = harness_fb_hash_box(0, by1 + 1, w, h);

	int painted = 0, worst = 0, outside = 0;
	for (int i = 0; i < 40; i++)
	{
		int flips = harness_present_count();
		harness_advance(16);
		chome_handle(0);
		if (harness_present_count() == flips) break;      // the shelf has come to rest
		painted++;
		if (gfx_damage_rows() > worst) worst = gfx_damage_rows();
		if (harness_fb_hash_box(0, 0, w, by0) != above) outside++;
		if (harness_fb_hash_box(0, by1 + 1, w, h) != below) outside++;
	}

	printf("  the slide after a tap: %d frames, worst damage %d rows\n", painted, worst);
	check(painted >= 8, "a tap slides the shelf over several frames");
	check(worst <= band_rows, "and every one of them damages only the band");
	check(worst < h, "which is less than the whole screen");
	check(!outside, "nothing above or below the band changed while the cards moved");

	/*
	  The same slide again, this time with every frame of it compared against a full repaint
	  of its own instant. This is the check that would catch a band that was too small, a
	  layer that had been cached, or a partial frame drawn out of register with the clip.
	*/
	/*
	  Leftwards, deliberately, and this took a run to work out. art_step() decodes or gives
	  up on one cover per call, and force_full_repaint() calls into the front-end four times -
	  so a card that was still waiting for its art when the partial frame was drawn can have
	  resolved by the time the full frame is, and the comparison fails on a card that changed
	  for an honest reason. Scrolling back over cards this section has already visited and
	  settled on asks for nothing new: request_visible_art() covers the same span from the
	  entry to its left, and every card in it is already decided.
	*/
	frame(30);
	chome_handle(KEY_LEFT);
	harness_advance(16);
	chome_handle(KEY_LEFT | UPSTROKE);

	int frames = 0, forced = 0, differed = 0;
	int art_was = art_cache_count();
	for (int i = 0; i < 40; i++)
	{
		int flips = harness_present_count();
		harness_advance(16);
		chome_handle(0);
		if (harness_present_count() == flips) break;
		frames++;

		unsigned long partial = harness_fb_hash_box(0, 0, w, h);
		if (force_full_repaint()) forced++;
		if (harness_fb_hash_box(0, 0, w, h) != partial) differed++;
	}
	check(art_cache_count() == art_was, "no cover landed during the comparison to spoil it");

	check(frames >= 8, "the slide ran long enough to reach the card's full size");
	check(forced == frames, "a full repaint of the same instant was forced on every frame");
	check(!differed,
		"every partial frame of a slide is byte-identical to a full repaint of the same "
		"instant, the last of them with the card at its largest");

	/*
	  And now the case the deferral exists for: an arrow held down.

	  Delivered the way menu_key_get() delivers one - the press, then the same keycode again
	  every REPEATRATE, and *nothing at all* on the frames in between. That shape matters:
	  the front-end sees key == 0 on most frames of a hold, so anything that treated an idle
	  frame as the end of the hold would commit between every pair of repeats and defer
	  nothing.
	*/
	press(KEY_LEFT);
	frame(8);

	unsigned long title_was = harness_fb_hash_box(0, 0, w, by0);
	unsigned long legend_was = harness_fb_hash_box(0, by1 + 1, w, h);
	unsigned long pos_was = harness_fb_hash_box(0, p->y_pos, w, p->y_pos + 8 * p->ts_tiny);

	chome_handle(KEY_RIGHT);
	int held = 0, held_worst = 0;
	for (int r = 0; r < 4; r++)
	{
		// Three frames of nothing at 16 ms each, which is about the 50 ms of REPEATRATE.
		for (int f = 0; f < 3; f++)
		{
			int flips = harness_present_count();
			harness_advance(16);
			chome_handle(0);
			if (harness_present_count() == flips) continue;
			held++;
			if (gfx_damage_rows() > held_worst) held_worst = gfx_damage_rows();
		}
		chome_handle(KEY_RIGHT);                          // the repeat
		held++;
		if (gfx_damage_rows() > held_worst) held_worst = gfx_damage_rows();
	}

	printf("  the held scroll: %d frames, worst damage %d rows\n", held, held_worst);
	check(held >= 12, "a held arrow keeps the shelf moving");
	check(held_worst <= band_rows, "and every frame of the hold stays inside the band");
	check(harness_fb_hash_box(0, 0, w, by0) == title_was,
		"the title has not moved once while the arrow was held");
	check(harness_fb_hash_box(0, by1 + 1, w, h) == legend_was, "nor have the button prompts");
	check(harness_fb_hash_box(0, p->y_pos, w, p->y_pos + 8 * p->ts_tiny) != pos_was,
		"while the position line, which is in the band, has been counting all along");

	// The deferred frame has to be honest as well as cheap: a full repaint of this instant
	// draws the same old title, because the selection has not committed yet.
	unsigned long mid_hold = harness_fb_hash_box(0, 0, w, h);
	check(force_full_repaint(), "a full repaint can be forced mid-hold without committing it");
	check(harness_fb_hash_box(0, 0, w, h) == mid_hold,
		"a partial frame mid-hold is byte-identical to a full repaint of the same instant");

	chome_handle(KEY_RIGHT | UPSTROKE);
	check(gfx_damage_rows() == h, "releasing the arrow repaints the world");
	check(harness_fb_hash_box(0, 0, w, by0) != title_was,
		"and the title is the one under the cursor again");
	frame(10);

	/*
	  The other commit point, which is what keeps a lost release from freezing the title
	  until the next keypress: a press whose upstroke never arrives at all. The chrome
	  commits when the shelf comes to rest, and not before.
	*/
	unsigned long lost_was = harness_fb_hash_box(0, 0, w, by0);
	chome_handle(KEY_RIGHT);
	for (int i = 0; i < 4; i++) { harness_advance(16); chome_handle(0); }
	check(harness_fb_hash_box(0, 0, w, by0) == lost_was,
		"mid-slide with no release, the title is still the old one");
	for (int i = 0; i < 24; i++) { harness_advance(16); chome_handle(0); }
	check(harness_fb_hash_box(0, 0, w, by0) != lost_was,
		"and it commits when the shelf comes to rest, with no release at all");
	chome_handle(KEY_RIGHT | UPSTROKE);
	frame(6);

	/*
	  And the invariant from the other side. A structural change must still take the full
	  path: opening a folder rebuilds the view, which changes the cards, the title, the
	  prompts and the count all at once, and no band could contain that.
	*/
	shelf_rewind();
	frame(6);

	int was_n = lib_view_count();
	check(lib_view_entry(0) && lib_view_entry(0)->kind == ENT_FOLDER,
		"the leftmost card of the root shelf is a folder");

	chome_handle(KEY_ENTER);
	check(gfx_damage_rows() == h, "entering a folder repaints every row");
	chome_handle(KEY_ENTER | UPSTROKE);
	frame(10);
	check(lib_view_count() != was_n, "and it really did open one");

	press(KEY_ESC);
	frame(10);
	check(lib_view_count() == was_n, "back out of it");
}

/*
  Launching a physical disc, end to end up to the MGL - the part a fake drive can
  prove. The sentinel and the slot in the written MGL are the two things menu.cpp and
  psx.cpp key on, so they are checked as text; whether the core then reads sectors is
  hardware's question, not this file's.

  And the refusal: a disc whose core is not wired (SNES MSU-1 here) is named on the
  prompt but marked "(not yet)", and pressing it must launch nothing - the row that
  offered and then refused is the bug this pins shut.
*/
static void assert_disc_launch()
{
	printf("\n== physical disc: launching ==\n");

	enum { S_HOME = 0, S_DISC = 17 };

	cfg.classicui_disc = 1;

	/* ------------------------------------------ the unwired core refuses --- */

	disc_ingest_present(1);
	fake_disc dm; memset(&dm, 0, sizeof(dm));
	static const char *const sfc[] = { "GAME.SFC;1" };
	fake_iso(&dm, 0, "MSU1", 0, sfc, 1);
	disc_set_reader(fake_read, &dm);
	disc_ingest_identify(0);
	frame(6);
	check(disc_state() == DISC_READY && disc_type() == DISC_T_SNES,
		"an MSU-1 SNES disc is identified");

	press(KEY_UP);
	press(KEY_ENTER);
	check(chome_screen_id() == S_DISC, "and its prompt is up");
	dump("disc-5-not-yet");

	harness_clear_launch();
	press(KEY_ENTER);
	check(harness_last_launch()[0] == 0, "its \"(not yet)\" row launches nothing");
	check(chome_screen_id() == S_DISC, "and the prompt stays put rather than pretending");
	press(KEY_ESC);
	press(KEY_ESC);

	disc_ingest_present(0);
	frame(6);

	/* ------------------------------------------------ the wired one plays --- */

	disc_ingest_present(1);
	fake_disc dp; memset(&dp, 0, sizeof(dp));
	static const char *const none[] = { "" };
	fake_iso(&dp, 0, "PLAYSTATION", "PLAYSTATION", none, 0);
	fake_put(&dp, 20, 0, "BOOT = cdrom:\\SLUS_006.26;1", 27, 100);
	disc_set_reader(fake_read, &dp);
	disc_ingest_identify(0);
	frame(6);
	check(disc_type() == DISC_T_PSX, "a PlayStation disc is identified");

	press(KEY_UP);
	press(KEY_ENTER);
	check(chome_screen_id() == S_DISC, "and its prompt is up");
	dump("disc-6-play");

	harness_clear_launch();
	press(KEY_ENTER, 4);
	frame(80);                         // let the launch curtain elapse

	check(strstr(harness_last_launch(), ".mgl") != 0, "\"Play on PlayStation\" launches");

	FILE *f = fopen("/tmp/classicui_launch.mgl", "rt");
	check(f != 0, "and wrote the MGL");
	if (f)
	{
		char buf[1024] = {};
		size_t n = fread(buf, 1, sizeof(buf) - 1, f);
		buf[n] = 0;
		fclose(f);
		printf("---- /tmp/classicui_launch.mgl ----\n%s-----------------------------------\n", buf);
		check(strstr(buf, "_Console/PSX") != 0, "the MGL names the PSX core");
		check(strstr(buf, PHYSICAL_DISC_SENTINEL) != 0, "the file is the sentinel, not a path");
		check(strstr(buf, "type=\"s\" index=\"1\"") != 0,
			"and it goes into SD slot 1 - \"H7S1,CUECHD,Load CD\" in PSX.sv");
	}

	/* ------------------------------------- and Mega CD, with its own core --- */

	/*
	  Mega CD is the case where the disc's core is not the shelf system's own: the
	  "md" shelf launches the Genesis core, whose config string has no disc slot at
	  all, so the MGL has to name the separate MegaCD core. A wrong rbf here would
	  look on hardware like a broken disc, which is why the core line is asserted
	  as text alongside the sentinel and the slot.
	*/

	// The PSX launch closed the UI; come back the way a player would.
	disc_reset_reader();
	disc_ingest_present(0);
	(void)disc_take_dirty();
	chome_leave();
	press(KEY_MENU, 20);
	frame(10);

	disc_ingest_present(1);
	fake_disc dmc; memset(&dmc, 0, sizeof(dmc));
	fake_put(&dmc, 0, 0, "SEGADISCSYSTEM", 14, 0);
	disc_set_reader(fake_read, &dmc);
	disc_ingest_identify(0);
	frame(6);
	check(disc_state() == DISC_READY && disc_type() == DISC_T_MEGACD,
		"a Mega CD disc is identified");

	press(KEY_UP);
	press(KEY_ENTER);
	check(chome_screen_id() == S_DISC, "and its prompt is up");
	dump("disc-7-megacd-play");

	harness_clear_launch();
	press(KEY_ENTER, 4);
	frame(80);                         // let the launch curtain elapse

	check(strstr(harness_last_launch(), ".mgl") != 0, "\"Play on Mega Drive\" launches");

	FILE *fm = fopen("/tmp/classicui_launch.mgl", "rt");
	check(fm != 0, "and wrote the MGL");
	if (fm)
	{
		char buf[1024] = {};
		size_t n = fread(buf, 1, sizeof(buf) - 1, fm);
		buf[n] = 0;
		fclose(fm);
		printf("---- /tmp/classicui_launch.mgl ----\n%s-----------------------------------\n", buf);
		check(strstr(buf, "_Console/MegaCD") != 0,
			"the MGL names the MegaCD core - \"S0,CUECHD,Insert Disk\" in MegaCD.sv");
		check(strstr(buf, "_Console/Genesis") == 0,
			"and not the shelf's Genesis core, which has no disc slot");
		check(strstr(buf, PHYSICAL_DISC_SENTINEL) != 0, "the file is the sentinel, not a path");
		check(strstr(buf, "type=\"s\" index=\"0\"") != 0,
			"and it goes into SD slot 0, the MegaCD core's only S entry");
	}

	/* ------------------------------------------------- and so does Neo Geo --- */

	/*
	  Neo Geo CD, the romset system. Its shelf entries are board-named archives
	  resolved through romsets.xml, but a disc takes none of that path: disc_launch()
	  writes the sentinel with the slot swapped to the core's CD input, and menu.cpp
	  hands any 's' mount on this core to neocd_set_image(). What can be proven here
	  is exactly what the blocks above prove - the right core, the sentinel, the right
	  slot.

	  Its sectors come from the same shared cdd_t as Mega CD, which is why that port
	  had to land first; this block would pass either way, since the MGL is written
	  before any daemon is involved.
	*/
	disc_reset_reader();
	disc_ingest_present(0);
	(void)disc_take_dirty();
	chome_leave();
	press(KEY_MENU, 20);
	frame(10);
	check(chome_screen_id() == S_HOME, "the shelf is back after the Mega CD launch");

	disc_ingest_present(1);
	fake_disc dn; memset(&dn, 0, sizeof(dn));
	fake_iso(&dn, 0, "NEOGEO CD", "NGCD", none, 0);
	disc_set_reader(fake_read, &dn);
	disc_ingest_identify(0);
	frame(6);
	check(disc_type() == DISC_T_NEOGEO, "a Neo Geo CD disc is identified");

	press(KEY_UP);
	press(KEY_ENTER);
	check(chome_screen_id() == S_DISC, "and its prompt is up");

	harness_clear_launch();
	press(KEY_ENTER, 4);
	frame(80);

	check(strstr(harness_last_launch(), ".mgl") != 0, "\"Play on Neo Geo\" launches");

	f = fopen("/tmp/classicui_launch.mgl", "rt");
	check(f != 0, "and wrote the MGL");
	if (f)
	{
		char buf[1024] = {};
		size_t n = fread(buf, 1, sizeof(buf) - 1, f);
		buf[n] = 0;
		fclose(f);
		printf("---- /tmp/classicui_launch.mgl ----\n%s-----------------------------------\n", buf);
		check(strstr(buf, "_Console/NeoGeo") != 0, "the MGL names the NeoGeo core");
		check(strstr(buf, PHYSICAL_DISC_SENTINEL) != 0, "the file is the sentinel, not a path");
		check(strstr(buf, "type=\"s\" index=\"1\"") != 0,
			"and it goes into SD slot 1 - \"S1,CUECHD,Load CD Image\" in neogeo.sv, "
			"the 's' slot, not the FS1 romset slot");
	}

	// Leave things as the sections after this expect: no disc, the flag back off,
	// and the UI reopened on the shelf - the launch closed it. Re-entered the way
	// every other section re-enters, leave then menu key.
	disc_reset_reader();
	disc_ingest_present(0);
	(void)disc_take_dirty();
	cfg.classicui_disc = 0;
	chome_leave();
	press(KEY_MENU, 20);
	frame(10);
	check(chome_screen_id() == S_HOME, "the shelf is back for whatever comes next");
}

/*
  The dialog's two buttons.

  A is the default: it launches without the player moving the cursor at all, which is the
  whole point of replacing the row list - "Play on PlayStation" needed a press to read and
  a press to confirm. Options is the second button, and it opens the core chooser the
  removed "Use a different core" row used to offer.

  The two modes of this screen share one screen id, and the offer's disc animates, so a
  pixel hash of the panel differs between two visits with nothing wrong. The legend does
  not animate and says something different in each mode, so that is what is read here -
  and the launches are asserted on the MGL, which is the only thing that cannot lie about
  which core the disc went to.
*/
static void assert_disc_dialog()
{
	printf("\n== physical disc: the dialog and its two buttons ==\n");

	enum { S_HOME = 0, S_DISC = 17, S_DISCBAR = 18 };

	int w = gfx_w(), h = gfx_h();
	int ly0 = h * 9 / 10;                  // inside the legend strip at every profile

	cfg.classicui_disc = 1;

	/*
	  Openable before the disc is known, which is a real thirty-second window on a slow
	  drive: nothing has a name for it yet, so the dialog has to say what it is doing rather
	  than draw its largest line blank.
	*/
	disc_ingest_present(1);
	frame(6);
	check(disc_state() == DISC_SPINNING, "a disc has arrived and is still being read");
	press(KEY_UP);
	press(KEY_ENTER);
	check(chome_screen_id() == S_DISC, "its dialog opens before anything knows what it is");

	/*
	  And what it says while it is there, which is the complaint this section grew out of.

	  A disc that is in the drive and not yet identified used to be describable only as a
	  picture: the dump below proved something was drawn, not that it said anything. It
	  has to read as work in progress rather than as an answer, so all three of these are
	  failures - a blank largest line, the console's name on its own, and "Unrecognised
	  disc", which is the finished verdict and the opposite of this state.

	  disc_display_name() has nothing to offer here - no serial, no label, and
	  disc_type_name(DISC_T_NONE) is empty - so what lands in the title is the subtitle,
	  promoted by disc_dlg_get() precisely so that this line is never drawn blank.
	*/
	{
		char t[DISC_TITLE_LEN] = {}, s[64] = {};
		disc_test_dlg_text(t, sizeof(t), s, sizeof(s));

		check(t[0] != 0, "and its largest line is not left blank while the drive is still reading");
		check(!strcmp(t, "Reading the disc"), "it says the disc is being read");
		check(strcmp(t, "Unrecognised disc") && strcmp(s, "Unrecognised disc"),
			"and does not deliver a verdict it has not reached yet");
	}

	dump("disc-8b-dialog-reading");
	press(KEY_ESC);
	press(KEY_ESC);

	fake_disc dp; memset(&dp, 0, sizeof(dp));
	static const char *const none[] = { "" };
	fake_iso(&dp, 0, "PLAYSTATION", "PLAYSTATION", none, 0);
	fake_put(&dp, 20, 0, "BOOT = cdrom:\\SLUS_006.26;1", 27, 100);
	disc_set_reader(fake_read, &dp);
	disc_ingest_identify(0);
	frame(6);
	check(disc_type() == DISC_T_PSX, "a PlayStation disc is in the drive");

	press(KEY_UP);
	press(KEY_ENTER);
	check(chome_screen_id() == S_DISC, "the dialog opens from the badge");
	dump("disc-9-dialog");

	/* ---------------------- the scan belongs to the dialog and not to the badge --- */

	/*
	  His decision, and the reason it is checked from the shelf rather than in a game: the
	  badge is a press away rather than a whole state away, so one fixture answers for both.
	  The badge is thirty-two pixels at 240p, where a photograph is mud and the drawing says
	  "there is a disc" better than any picture could.

	  Counted by an exact colour, not by a hash: everything involved animates, and two hashes
	  of a turning disc differ with nothing wrong. 0xff20c020 is in no palette this UI draws
	  with, so any of it on screen came out of the fixture.
	*/
	mkpath(ROOT "/classicui/discart");
	make_cover(ROOT "/classicui/discart/SLUS-00626.png", 400, 400, 0xff20c020);
	frame(12);

	check(box_pixels(w / 4, h / 2 - h / 6, (3 * w) / 4, h / 2 + h / 6, 0xff20c020u) > 100,
		"a scan filed under the disc identity is what the dialog draws");
	dump("disc-9c-dialog-scan");

	/*
	  And the badge, read where the badge is.

	  This used to count the fixture colour in the top-left fifth with the dialog still up, and
	  it had stopped meaning anything: the dialog was given the whole screen, so at 720p the
	  panel covers the corner the badge sits in and there is no badge in that box to be right or
	  wrong about. A check that cannot fail is worse than no check, so it steps back to the
	  badge tier - one press - where the badge is on the shelf and visible.

	  The panel no longer covers that corner at 720p, being sized to its contents now, so the
	  original reading would work again on this canvas. It stays on the badge tier anyway: that
	  is where the badge belongs, and it is the one place the check is right at every profile
	  rather than at whichever ones the dialog happens not to reach.

	  Two halves, because "no fixture colour here" is also what an absent badge looks like: the
	  scan did not reach it, and there is something there that did.
	*/
	press(KEY_ESC);
	check(chome_screen_id() == S_DISCBAR, "back on the badge, with the scan still on the card");

	{
		const chome_profile *p = theme_get();
		int br = (p->ts_ui >= 2) ? 32 : 16;
		int bx0 = p->safe_x + p->inset, by0 = p->safe_y + p->inset;

		check(box_pixels(bx0, by0, bx0 + 2 * br, by0 + 2 * br, 0xff20c020u) == 0,
			"and the badge keeps the drawn disc rather than the scan");
		check(box_pixels(bx0, by0, bx0 + 2 * br, by0 + 2 * br, COL_WHITE) > 0,
			"having drawn one at all - that box holds a disc, not empty shelf");
	}

	press(KEY_ENTER);
	check(chome_screen_id() == S_DISC, "and the dialog comes back for the rest of this");

	unlink(ROOT "/classicui/discart/SLUS-00626.png");
	frame(12);
	check(box_pixels(w / 4, h / 2 - h / 6, (3 * w) / 4, h / 2 + h / 6, 0xff20c020u) == 0,
		"and taking the scan away puts the drawn disc back rather than leaving a stale one");

	/* --------------------------------------------- left and right, and Options --- */

	unsigned long leg_play = harness_fb_hash_box(0, ly0, w, h);

	press(KEY_RIGHT);
	unsigned long leg_opts = harness_fb_hash_box(0, ly0, w, h);
	check(leg_opts != leg_play, "moving onto the second button changes what A is offered for");
	dump("disc-9b-dialog-options");

	press(KEY_LEFT);
	check(harness_fb_hash_box(0, ly0, w, h) == leg_play,
		"and moving back onto the first restores it");

	/*
	  Right *held* against the end of the button row, which is where it now stops.

	  This check used to read "two taps of right, and it stops at the second button" and
	  asserted the clamp the dialog had before task 54 - the button row wraps on a fresh press
	  now, like every other list here, so the second tap would land back on Play and launch
	  the disc. What survives the change, and is the more useful claim, is that *holding* the
	  key cannot do that: auto-repeat walks to the last button and stays, so nothing a player
	  leans on can launch a disc they were only trying to scroll past. The wrap itself is
	  asserted in assert_uniform_wrap().

	  Asserted through A rather than through the legend, deliberately: a refused press paints
	  the line above the legend red, and this UI only repaints what changed, so that line is
	  still red the next time anything reads those pixels. What A does is not ambiguous like
	  that - if the hold had wrapped round to the first button the disc would have launched
	  and this screen would be gone.
	*/
	hold_dir(KEY_RIGHT, 8, 12);
	press(KEY_ENTER);
	check(chome_screen_id() == S_DISC,
		"a held right stops at the second button, and A there opens the core chooser "
		"rather than launching");
	dump("disc-10-dialog-cores");

	press(KEY_ESC);
	check(chome_screen_id() == S_DISC, "back out of the chooser returns to the dialog");
	check(harness_fb_hash_box(0, ly0, w, h) == leg_play,
		"with the cursor back on the action, not left on Options");

	press(KEY_ESC);
	check(chome_screen_id() == S_DISCBAR, "and a second back leaves the dialog for the badge");

	/* --------------------------------------- A alone, with nothing else pressed --- */

	press(KEY_ENTER);
	check(chome_screen_id() == S_DISC, "the dialog is up again");

	harness_clear_launch();
	press(KEY_ENTER, 4);
	frame(80);                             // let the launch curtain elapse

	check(strstr(harness_last_launch(), ".mgl") != 0,
		"A on the dialog launches the disc with no navigation at all");

	FILE *f = fopen("/tmp/classicui_launch.mgl", "rt");
	check(f != 0, "and wrote the MGL");
	if (f)
	{
		char buf[1024] = {};
		size_t n = fread(buf, 1, sizeof(buf) - 1, f);
		buf[n] = 0;
		fclose(f);
		check(strstr(buf, "_Console/PSX") != 0, "on the core the disc was identified as");
		check(strstr(buf, PHYSICAL_DISC_SENTINEL) != 0, "with the sentinel as the file");
	}

	/* ------------------------------- and A through the chooser, on a hand pick --- */

	/*
	  Row 0 of the chooser is the Mega Drive, which cannot read a PlayStation disc and is
	  marked "(not yet)"; row 1 is PlayStation. The order is disc_capable_systems()'s, which
	  walks the disc types in their own order - Mega CD before PlayStation - so this is one
	  press down and not a guess.
	*/
	disc_reset_reader();
	disc_ingest_present(0);
	(void)disc_take_dirty();
	chome_leave();
	press(KEY_MENU, 20);
	frame(10);

	disc_ingest_present(1);
	disc_set_reader(fake_read, &dp);
	disc_ingest_identify(0);
	frame(6);

	press(KEY_UP);
	press(KEY_ENTER);
	press(KEY_RIGHT);                      // Options
	press(KEY_ENTER);                      // the chooser
	press(KEY_DOWN);                       // past the Mega Drive row

	harness_clear_launch();
	press(KEY_ENTER, 4);
	frame(80);

	check(strstr(harness_last_launch(), ".mgl") != 0, "a core picked by hand launches too");

	f = fopen("/tmp/classicui_launch.mgl", "rt");
	check(f != 0, "and wrote its MGL");
	if (f)
	{
		char buf[1024] = {};
		size_t n = fread(buf, 1, sizeof(buf) - 1, f);
		buf[n] = 0;
		fclose(f);
		check(strstr(buf, "_Console/PSX") != 0, "naming the core the chooser was pointing at");
		check(strstr(buf, PHYSICAL_DISC_SENTINEL) != 0, "with the sentinel as the file");
	}

	// As assert_disc_launch leaves things: no disc, the flag off, the shelf back up.
	disc_reset_reader();
	disc_ingest_present(0);
	(void)disc_take_dirty();
	cfg.classicui_disc = 0;
	chome_leave();
	press(KEY_MENU, 20);
	frame(10);
	check(chome_screen_id() == S_HOME, "the shelf is back for whatever comes next");
}

/*
  The dialog's disc is resolved to the display, and the badge's is still a sprite.

  What this is about. gfx_disc() draws 32 cells of r/16 pixels, which is the look at badge
  size and nothing but blocks at dialog size: the dialog fills the panel, so a cell came out
  fifteen pixels square at 720p. That mattered because the dialog is also where a scanned
  disc label goes once something has fetched one, and fifteen-pixel blocks becoming a
  photograph does not read as one object at two moments. It reads as a fault.

  So the dialog renders the disc once into a buffer at exactly the size it draws at and turns
  that through the same path the scan goes through. The three things worth pinning are
  therefore not what it looks like - the PNGs are for the eyes - but that the buffer is the
  size of the disc on screen, that it is made once per size rather than once per frame, and
  that none of it reached the badge.

  Left as assert_disc_dialog() leaves things: no disc, the flag off, the shelf up and the
  canvas back where it was.
*/
static void assert_disc_hires()
{
	printf("\n== physical disc: the dialog draws it at the screen's resolution ==\n");

	enum { S_HOME = 0, S_DISC = 17 };

	int was_prof = cfg.classicui_profile;
	int was_w = gfx_w(), was_h = gfx_h();

	cfg.classicui_disc = 1;

	fake_disc dp; memset(&dp, 0, sizeof(dp));
	static const char *const none[] = { "" };
	fake_iso(&dp, 0, "PLAYSTATION", "PLAYSTATION", none, 0);
	fake_put(&dp, 20, 0, "BOOT = cdrom:\\SLUS_006.26;1", 27, 100);

	disc_ingest_present(1);
	disc_set_reader(fake_read, &dp);
	disc_ingest_identify(0);
	frame(6);

	press(KEY_UP);
	press(KEY_ENTER);
	check(chome_screen_id() == S_DISC, "the dialog is up over a PlayStation disc");

	/* ------------------------------------ the face is made for the disc on screen --- */

	int dia = disc_drawn_dia();
	printf("  the disc is %d px across on the %s canvas (%dx%d), face buffer %d\n",
		dia, theme_get()->name, gfx_w(), gfx_h(), gfx_disc_face_dia());

	/*
	  Pinned as a number, because this is the answer to the question that started the whole
	  exercise: how much resolution the dialog actually has to fill.

	  288 is the box, and the disc measures a pixel or two under it. Two reasons, both by
	  design and neither worth pinning to the pixel: gfx_disc_face() keeps a pixel of
	  background inside the buffer so that rotating it cannot sample off the end, and the
	  rotation's 8.8 sine can carry an edge pixel about a pixel back out again, by an amount
	  that depends on the angle it happens to be caught at. What the tolerance is still tight
	  enough to catch is the thing worth catching: a face generated at some other size and
	  stretched by the blit, which is the failure this whole design exists to make impossible.

	  It was 480 while the disc took whatever height the panel had been handed, which is what
	  made the panel 1216x644 of a 1280x720 screen. It is two fifths of the canvas height
	  again, capped at DISC_DLG_MAX_CELLS, and 720p is the canvas the cap first bites on - see
	  disc_layout_for() and assert_disc_dialog_size(), which measures every profile.
	*/
	check(gfx_disc_face_dia() == 288,
		"at 720p the panel gives the disc a 288 pixel box, and the face is made at exactly that");
	check(dia <= 288 && dia >= 284, "and blitted 1:1, so the disc measures the same bar a pixel");

	/* ---------------------------------- once per size, and not once per frame --- */

	/*
	  The disc is turning throughout this, which is the point: the rotation is recomputed
	  every time the angle moves - sixteen times a second at the slow rate, sixty at the
	  smooth one - and the face it is rotating must not be. Sixty frames is several turns
	  and several dozen angles.
	*/
	int gens = gfx_disc_face_gens();
	check(gens > 0, "the face has been generated");

	frame(60);
	check(gfx_disc_face_gens() == gens,
		"sixty frames of it turning generate no new face: the rotation is per angle, the "
		"face is per size");

	/* ------------------------ the same kind of edge, generated and photographed --- */

	/*
	  The seam this whole exercise is about, measured on both sides of it.

	  The dialog is where a scanned disc label goes once something has fetched one, so the two
	  discs that appear here have to be the same class of object - and an edge is most of what
	  "class of object" means at 480 pixels. A smooth generated disc beside a hard-edged scan is
	  the same fault as a blocky generated disc beside a smooth scan; it is the fault with the
	  two sides swapped, and the scan is the side Dinofly sees once his credentials fetch real art.

	  Both are measured the same way and reported side by side: how many pixels within two of a
	  boundary are neither of the colours it lies between. Zero is a staircase. The scan's mask
	  answered zero at every boundary before gfx_disc_cover() was shared with it.

	  Only the outer rim and the hub hole can be measured, and that is a property of the picture
	  rather than of the code: the disc's other two boundaries have the photograph on one side,
	  where a blended pixel and a pixel of the scan are indistinguishable to a colour test. They
	  go through the identical gfx_disc_cover() call in the identical loop, one line apart from
	  the two that are checked.
	*/
	{
		uint32_t edge = disc_edge_col();
		int cx, cy, tot;

		/*
		  Judged against the boundary's own circumference, not against the pixels in the band:
		  a one-pixel ramp puts a blend on roughly every pixel the arc passes through, which is
		  2*pi*r of them, and a staircase puts one on none. The floor is a sixth of that, which
		  no hard edge can reach and no drawn ramp can miss.
		*/
		int gd = disc_drawn_box(&cx, &cy);
		int g_rr = gd / 2, g_hr = (gd / 2) * GFX_DISC_HOLE_PCT / 100;

		int g_rim = ring_blends(cx, cy, g_rr, COL_PANEL, edge, &tot);
		int g_rimtot = tot;
		int g_hub = ring_blends(cx, cy, g_hr, edge, COL_BGDARK, &tot);
		int g_hubtot = tot;

		printf("  generated: rim %d/%d blended (%d%% of its circumference), "
			"hub %d/%d (%d%%)\n",
			g_rim, g_rimtot, g_rim * 100 / (g_rr * 6), g_hub, g_hubtot, g_hub * 100 / (g_hr * 6));

		check(g_rim > g_rr, "the generated disc fades its outer rim into the panel");
		check(g_hub > g_hr, "and its spindle hole into the hub ring");

		// The fixture is not flat - make_cover() puts a dark band and two white diagonals in
		// it - which is exactly why only the two flat boundaries are read.
		mkpath(ROOT "/classicui/discart");
		make_cover(ROOT "/classicui/discart/SLUS-00626.png", 400, 400, 0xff20c020);
		frame(12);

		int sd = disc_drawn_box(&cx, &cy);
		check(box_pixels(cx - sd / 4, cy - sd / 4, cx + sd / 4, cy + sd / 4, 0xff20c020u) > 100,
			"the scan is what the dialog is drawing now");

		int s_rr = sd / 2, s_hr = (sd / 2) * GFX_DISC_HOLE_PCT / 100;

		int s_rim = ring_blends(cx, cy, s_rr, COL_PANEL, edge, &tot);
		int s_rimtot = tot;
		int s_hub = ring_blends(cx, cy, s_hr, edge, COL_BGDARK, &tot);
		int s_hubtot = tot;

		printf("  scanned:   rim %d/%d blended (%d%% of its circumference), "
			"hub %d/%d (%d%%)\n",
			s_rim, s_rimtot, s_rim * 100 / (s_rr * 6), s_hub, s_hubtot, s_hub * 100 / (s_hr * 6));

		check(s_rim > s_rr, "and the scan fades its outer rim into the panel too");
		check(s_hub > s_hr, "and its spindle hole into the hub ring");

		/*
		  Within a third of each other rather than to the pixel. Both are the same radius and
		  the same ramp, so they are close by construction - but the scan's centre sits half a
		  pixel off the generated one (disc_rot measures from a pixel, the face from between
		  two), and the rotation nudges an edge pixel by up to one, so a tight bound would fail
		  on the angle it was caught at rather than on the drawing.
		*/
		check(s_rim * 3 > g_rim && g_rim * 3 > s_rim,
			"and to the same degree: neither is the smooth one beside a staircase");

		unlink(ROOT "/classicui/discart/SLUS-00626.png");
		frame(12);
		check(gfx_disc_face_gens() == gens,
			"a scan arriving and leaving does not rebuild the face either - it is per size");
	}

	/* --------------------------------------------- and again when the size moves --- */

	/*
	  The canvas is changed the way walk_profile() changes it, which means leaving and
	  re-entering - and chome_leave() calls disc_watch_stop(), so the drive has to be told
	  about the disc again on the way back in. Nothing here is testing the watcher; without
	  these three lines the dialog simply has no disc to be about.
	*/
	cfg.classicui_profile = 2;
	harness_set_fb(640, 480);
	gfx_shutdown();
	theme_update(640, 480, 2);
	chome_leave();
	press(KEY_MENU, 20);
	frame(10);

	disc_ingest_present(1);
	disc_set_reader(fake_read, &dp);
	disc_ingest_identify(0);
	frame(6);

	press(KEY_UP);
	press(KEY_ENTER);
	check(chome_screen_id() == S_DISC, "the dialog is up again on a 640x480 canvas");

	int sd_dia = disc_drawn_dia();
	printf("  the disc is %d px across on the %s canvas (%dx%d), face buffer %d\n",
		sd_dia, theme_get()->name, gfx_w(), gfx_h(), gfx_disc_face_dia());

	check(gfx_disc_face_dia() == 192, "which gives it a 192 pixel box instead, and the face followed");
	check(sd_dia <= 192 && sd_dia >= 188, "still blitted 1:1");
	check(gfx_disc_face_gens() == gens + 1, "regenerated once for the new size, and once only");

	frame(40);
	check(gfx_disc_face_gens() == gens + 1, "and not again while it sits there turning");

	/* ------------------------------------------- none of which reached the badge --- */

	/*
	  Read on the shelf and not with the dialog open, which is worth saying because the obvious
	  thing to do was the wrong one. The dialog was given the whole screen (draw_panel_at from
	  the inset), so at 720p the panel covered the corner the badge sits in entirely and there
	  was no badge on screen to read - and every test of it there passed on the panel's flat
	  grey without noticing. The panel is sized to its contents now and leaves that corner
	  alone, but the badge is still checked where it exists on every canvas rather than where
	  one canvas happens to allow it.

	  The badge is gfx_disc() still, and that is checked as the sprite's own defining property
	  rather than by hashing pixels: on the row through its centre the colour may only change
	  on a cell boundary, cells being r/16 pixels wide. Neither a smooth circle nor a blit of
	  the resolved face can satisfy that at 2x2 cells.

	  Then a count of the distinct colours on that row, because "changes only on a cell edge"
	  is also true of flat grey - and flat grey is exactly what a missing badge looks like. The
	  sprite crosses the panel colour, the outer edge, the rim, the bands, the ring and the
	  hole, so a handful is the floor.

	  The badge is unfocused here, which is what makes the run exact rather than approximate:
	  draw_disc_badge only breathes on SCR_DISCBAR, so the radius is the resting one and every
	  cell is the same width.
	*/
	cfg.classicui_profile = was_prof;
	harness_set_fb(was_w, was_h);
	gfx_shutdown();
	theme_update(was_w, was_h, was_prof);
	chome_leave();
	press(KEY_MENU, 20);
	frame(10);

	disc_ingest_present(1);
	disc_set_reader(fake_read, &dp);
	disc_ingest_identify(0);
	frame(6);
	check(chome_screen_id() == S_HOME, "back on the shelf, on the canvas this section started on");

	const chome_profile *p = theme_get();
	int br = (p->ts_ui >= 2) ? 32 : 16;          // disc_radius()'s answer, as check_focus_ring has it
	int cell = br / 16;
	int bcx = p->safe_x + p->inset + br;
	int bcy = p->safe_y + p->inset + br;

	const uint32_t *fb = harness_fb_shown();
	int w = gfx_w();
	int stepped = 1;

	uint32_t seen[64];
	int nseen = 0;

	for (int gx = -16; gx < 16; gx++)
	{
		int x = bcx + gx * cell;
		uint32_t c = fb[(size_t)bcy * w + x] | 0xff000000u;

		for (int i = 1; i < cell; i++)
			if ((fb[(size_t)bcy * w + x + i] | 0xff000000u) != c) stepped = 0;

		int have = 0;
		for (int i = 0; i < nseen; i++) if (seen[i] == c) have = 1;
		if (!have && nseen < 64) seen[nseen++] = c;
	}

	printf("  badge row: %d cells of %d px, %d distinct colours\n", 32, cell, nseen);

	check(cell >= 2, "this canvas draws the badge with cells more than one pixel wide");
	check(nseen >= 5, "there is a badge on that row and it is not flat panel");
	check(stepped, "and it is still the sprite: its colour only changes on a cell edge");
	check(gfx_disc_face_dia() != 2 * br,
		"the resolved face was never made at badge size, so nothing blitted one there");

	press(KEY_UP);
	press(KEY_ENTER);
	check(chome_screen_id() == S_DISC, "and the dialog opens over it as before");
	dump("disc-9d-dialog-hires");

	// As assert_disc_dialog leaves things, which is what the section after this starts from.
	disc_reset_reader();
	disc_ingest_present(0);
	(void)disc_take_dirty();
	cfg.classicui_disc = 0;
	chome_leave();
	press(KEY_MENU, 20);
	frame(10);
	check(chome_screen_id() == S_HOME, "the shelf is back for whatever comes next");
}

/*
  The dialog is sized for the canvas, at every canvas, measured off the screen.

  Dinofly: "we might still need to fix it for higher resolutions if they show more of the
  background behind the dialog (they probably should not have an immense dialog covering most
  of the screen at high res)". It was 1216x644 of a 1280x720 display - 85% of the picture -
  and 912x452 of 960x540, because the disc was taking whatever height the band above the
  legend had and the panel was taking the whole width whatever it held.

  Three things are pinned here and the third is the reason the other two are safe to change:

    - the width and the disc at each of the five canvases the walk covers, as numbers, with
      what each is made of written beside it;
    - the rule those numbers come from, so a canvas nobody tested is covered too: a panel
      whose disc has reached DISC_DLG_READABLE is sized to its own longest line and must not
      fill the width, and one whose disc has not is the full width by design;
    - 240p, which must be what it was to the pixel. It is Dinofly's own screen, he likes the
      dialog as it is there, and nothing above was worth breaking it for.

  The fixture is walk_disc_dialog's, deliberately: the panel is as wide as its longest line
  and on this disc that line is the title, so the pictures in test/out and the numbers here
  are about the same dialog. "SLES-01506" is ten characters and "Metal Gear Solid" is sixteen.
*/
static void assert_disc_dialog_size()
{
	printf("\n== the disc dialog is sized for the canvas, not for the screen ==\n");

	enum { S_HOME = 0, S_DISC = 17 };

	int was_prof = cfg.classicui_profile;
	int was_w = gfx_w(), was_h = gfx_h();

	const char *tdb = ROOT "/classicui/disctitles.txt";
	mkpath(ROOT "/classicui");
	{
		FILE *f = fopen(tdb, "wb");
		if (f)
		{
			fprintf(f, "#classicui-disctitles 1\n");
			fprintf(f, "SLES01506\tMetal Gear Solid\n");
			fclose(f);
		}
	}
	disc_titles_forget();

	cfg.classicui_disc = 1;

	fake_disc d; memset(&d, 0, sizeof(d));
	static const char *const none[] = { "" };
	fake_iso(&d, 0, "PLAYSTATION", "PLAYSTATION", none, 0);
	fake_put(&d, 20, 0, "BOOT = cdrom:\\SLES_015.06;1", 27, 100);

	/*
	  What each expected width is made of, at 12*ts_ui of margin either side:

	    hd      1280x720  432 = a 16-character title at 3x (384) + 48. Disc 288: two fifths
	                            of 720 is 288 and the cap is 288, so this canvas is where the
	                            two meet. Was 1216 wide with a 480 disc.
	    sd       640x480  280 = the same title at 2x (256) + 24. Disc 192, two fifths of 480.
	                            Was 564 wide with a 288 disc.
	    lo       320x240  282 = the canvas less its inset either side, unchanged. Disc 96,
	                            below DISC_DLG_READABLE, which is why it fills the width.
	    auto960  960x540  432 = the title at 3x again, the text scales being HD's. Disc 224,
	                            two fifths of 540 rounded to the nearest whole cell. Was 912.
	    tv640    640x240  564 = the canvas less its inset, which on this canvas is 38 rather
	                            than 19 because the inset is a fraction of the width. Disc 96:
	                            240 lines is 240 lines however wide the canvas says it is, so
	                            a stretched TV canvas keeps the 240p dialog. Unchanged.
	*/
	struct { const char *tag; int prof, w, h, pw, dia; } cases[] =
	{
		{ "hd",      1, 1280, 720, 432, 288 },
		{ "sd",      2,  640, 480, 280, 192 },
		{ "lo",      3,  320, 240, 282,  96 },
		{ "auto960", 0,  960, 540, 432, 224 },
		{ "tv640",   0,  640, 240, 564,  96 },
	};

	for (unsigned c = 0; c < sizeof(cases) / sizeof(cases[0]); c++)
	{
		char what[192];

		cfg.classicui_profile = (uint8_t)cases[c].prof;
		harness_set_fb(cases[c].w, cases[c].h);
		gfx_shutdown();
		theme_update(cases[c].w, cases[c].h, cases[c].prof);
		chome_leave();
		press(KEY_MENU, 20);
		frame(10);

		disc_ingest_present(1);
		disc_set_reader(fake_read, &d);
		disc_ingest_identify(0);
		frame(6);

		press(KEY_UP);
		press(KEY_ENTER);
		snprintf(what, sizeof(what), "%s: the dialog is up over a PlayStation disc", cases[c].tag);
		check(chome_screen_id() == S_DISC, what);

		const chome_profile *p = theme_get();
		int hdr = 10 * p->ts_ui + 6;                 // draw_panel_at's title bar
		int maxw = p->w - 2 * p->inset;              // all the width the dialog may have
		int legend_top = p->y_legend - 6 * p->ts_ui; // where draw_legend starts filling

		int ox = 0, oy = 0, ow = 0, oh = 0;
		int got = panel_plate_seen(&ox, &oy, &ow, &oh);

		snprintf(what, sizeof(what), "%s: there is a plate on screen to measure", cases[c].tag);
		check(got, what);
		if (!got) continue;

		// The plate is inside a two-pixel frame and below the title bar; the panel is what
		// disc_layout_for() sized. See panel_plate_seen().
		int pw = ow + 4, ph = oh + hdr + 2;
		int px = ox - 2, py = oy - hdr;
		int dcx = 0, dcy = 0;
		int dia = disc_drawn_box(&dcx, &dcy);

		printf("  %-7s %4dx%-4d panel %dx%d at %d,%d - %d%% of the picture, disc %d,"
			" bottom %d clears the legend at %d\n",
			cases[c].tag, p->w, p->h, pw, ph, px, py,
			(pw * ph * 100) / (p->w * p->h), dia, py + ph, legend_top);

		snprintf(what, sizeof(what), "%s: the panel is %d wide", cases[c].tag, cases[c].pw);
		check(pw == cases[c].pw, what);

		snprintf(what, sizeof(what), "%s: and its disc is %d across", cases[c].tag, cases[c].dia);
		check(dia >= cases[c].dia - 4 && dia <= cases[c].dia, what);

		/*
		  The rule, which is what covers the canvases nobody listed. A disc that has reached
		  the readable diameter is doing the job the width was ever for, so the panel is its
		  own longest line and nothing more - and half the canvas is a generous bound on that,
		  which the full width fails by construction.
		*/
		if (dia >= 144)
		{
			snprintf(what, sizeof(what),
				"%s: its disc reads on its own, so the panel is its contents and not the width",
				cases[c].tag);
			check(pw < maxw, what);

			/*
			  Half the canvas is a bound on this disc and not on every disc: the panel is as
			  wide as its longest line, and a forty-character title would be wider than this.
			  It is worth checking anyway, on the name it is checked with - sixteen characters
			  is a perfectly ordinary game title, and a panel that spent half the screen on one
			  would mean the width had stopped following the content again.
			*/
			snprintf(what, sizeof(what),
				"%s: which leaves the background either side - a title this long fits in half",
				cases[c].tag);
			check(pw * 2 <= p->w, what);
		}
		else
		{
			snprintf(what, sizeof(what),
				"%s: too small a disc to carry the dialog, so it keeps the full width",
				cases[c].tag);
			check(pw == maxw, what);
		}

		snprintf(what, sizeof(what), "%s: the disc is never past the cap", cases[c].tag);
		check(dia <= 288, what);

		if (p->h >= 360)
		{
			snprintf(what, sizeof(what),
				"%s: and on a canvas this tall it is big enough to read a label on",
				cases[c].tag);
			check(dia >= 144, what);
		}

		/*
		  The bottom, at every profile including the stretched one. This is the whole reason
		  draw_panel_at() exists: the plate and the legend used to end up a single row apart
		  at 240p - plate to row 209, legend from row 211, measured on the device - and two
		  things a row apart read as one crowded thing.
		*/
		snprintf(what, sizeof(what), "%s: and the bottom of it stays clear of the legend",
			cases[c].tag);
		check(py + ph < legend_top, what);

		snprintf(what, sizeof(what), "%s: and the top of it inside the overscan margin",
			cases[c].tag);
		check(py >= p->safe_y, what);

		/*
		  240p to the pixel, hashed rather than described.

		  The number is this dialog on the build before it was resized, and it is here because
		  everything else in this section is a licence to move the dialog about: the one canvas
		  that must not move is the one he looks at. If a deliberate change to the 240p dialog
		  ever lands, this moves with it - and the PNGs in test/out are the record of what it
		  used to be.

		  The plate rather than the frame: the shelf either side of it is still easing cards
		  about, which is not what this is about. The disc inside it is turning, so this leans
		  on the harness clock being stepped in fixed increments - it is the reason this is
		  taken here, in a section whose presses are fixed, rather than in the walk.
		*/
		if (cases[c].w == 320 && cases[c].h == 240)
		{
			/*
			  Everything but the disc, which turns. The disc's box is cut out with margin,
			  and its diameter and centre are asserted above - so the disc is still pinned,
			  just not the frame of its rotation.

			  The first version of this hashed the disc too and broke when a section was added
			  elsewhere in the harness: the extra clock advances caught the disc at a different
			  angle. The pixels that moved were the disc's 96x96 box exactly, measured, which is
			  how we know the dialog itself had not moved.

			  The cut-out is placed from the *plate*, not from disc_drawn_box(). The measured
			  box breathes by a pixel with the rotation - the anti-aliased rim reaches a
			  fraction further at some angles - so an exclusion rectangle built from it moved
			  whenever anything changed how many spin frames ran before this line, and the
			  pinned number broke with the plate pixel-for-pixel intact. That is exactly the
			  history-dependence the cut-out exists to remove, arriving through the back door.
			  So: horizontally the disc is centred on the plate, vertically it sits below the
			  title and the sub-line by draw_disc()'s own arithmetic (a copy, deliberately,
			  like every layout number in this section), and the expected diameter comes from
			  the case table with six pixels of margin for the rim and the wobble.
			*/
			int s = p->ts_ui;
			int ecx = ox + ow / 2;
			int ecy = oy + 6 * s + 8 * p->ts_title + 4 * s + 16 * s + cases[c].dia / 2;
			int half = cases[c].dia / 2 + 6;

			// The rectangle really does contain the drawn disc, wobble and all - the one
			// thing the fixed placement has to keep true.
			check(dcx - dia / 2 >= ecx - half && dcx + dia / 2 <= ecx + half &&
				dcy - dia / 2 >= ecy - half && dcy + dia / 2 <= ecy + half,
				"the fixed cut-out covers the drawn disc");

			unsigned long hash = harness_fb_hash_box_except(ox, oy, ox + ow, oy + oh,
				ecx - half, ecy - half, ecx + half, ecy + half);
			printf("  240p dialog plate hash (disc cut out) %lu\n", hash);

			/*
			  The number is this dialog on the build before it was resized, re-anchored when
			  the cut-out moved to the fixed rectangle: it was computed by running this very
			  hash over the frame the *baseline* build (bedc959) draws, so it still pins the
			  plate to the pixels he approved. If a deliberate change to the 240p dialog ever
			  lands, this moves with it - and the PNGs in test/out are the record.
			*/
			check(hash == 14061375912984922619UL, "240p is pixel for pixel the dialog it was");
		}

		disc_reset_reader();
		disc_ingest_present(0);
		(void)disc_take_dirty();
		frame(6);
	}

	// As assert_disc_hires leaves things, canvas included.
	unlink(tdb);
	disc_titles_forget();
	cfg.classicui_disc = 0;

	cfg.classicui_profile = (uint8_t)was_prof;
	harness_set_fb(was_w, was_h);
	gfx_shutdown();
	theme_update(was_w, was_h, was_prof);
	chome_leave();
	press(KEY_MENU, 20);
	frame(10);
	check(chome_screen_id() == S_HOME, "the shelf is back for whatever comes next");
}

/*
  A disc's suspend points, reached before the disc has ever been played.

  The strip used to be reachable over a *running* disc only, because a disc in the drive
  has no published identity until the mount writes one - and for most discs that is still
  true and still the right answer: physical_disc.cpp names them from the volume label or
  from a hash of the table of contents, and this front-end has neither.

  A PlayStation disc is the exception this section is about. Its save name is its serial,
  the serial is readable while the disc merely sits there, and both sides derive it by the
  same algorithm - so the name its states will carry under savestates/PSX is knowable from
  the shelf, and Down can be offered honestly.

  Both halves are checked, because "offer it when it is derivable" is worthless without
  "and not otherwise": the wrong key would list some other disc's states and then quietly
  fail to resume, which is the kind of failure nobody reports because nothing on screen
  says anything is wrong.

  The fixture state is named SLUS-00626_1.ss, which is what physical_disc_save_name() would
  publish for this disc: sanitize_name() leaves a serial alone. What cannot be checked here
  is that claim itself - no call in this harness reaches the mount - so the front-end
  refuses to guess whenever sanitising the serial *could* change it. See disc_susp_bind().
*/
static void assert_disc_shelf_slots()
{
	printf("\n== physical disc: its suspend points, from the shelf ==\n");

	enum { S_HOME = 0, S_SUSPEND = 2, S_DISC = 17 };

	const char *rec = ROOT "/classicui/suspend.txt";
	unlink(rec);

	cfg.classicui_disc = 1;

	mkpath(ROOT "/savestates/PSX");
	touch(ROOT "/savestates/PSX", "SLUS-00626_1.ss", 256);

	harness_set_menu_core(1);
	chome_leave();
	press(KEY_MENU, 20);
	frame(10);

	/* ------------------------------------------- the key is derivable: offer it --- */

	disc_ingest_present(1);
	fake_disc dp; memset(&dp, 0, sizeof(dp));
	static const char *const none[] = { "" };
	fake_iso(&dp, 0, "PLAYSTATION", "PLAYSTATION", none, 0);
	fake_put(&dp, 20, 0, "BOOT = cdrom:\\SLUS_006.26;1", 27, 100);
	disc_set_reader(fake_read, &dp);
	disc_ingest_identify(0);
	frame(6);
	check(disc_type() == DISC_T_PSX && !strcmp(disc_serial(), "SLUS-00626"),
		"a PlayStation disc in the drive, with its serial read off it");

	press(KEY_UP);
	press(KEY_ENTER);
	check(chome_screen_id() == S_DISC, "its dialog is up, and nothing has been launched");

	press(KEY_DOWN);
	check(chome_screen_id() == S_SUSPEND,
		"Down reaches the disc's suspend points from the shelf, with no mount involved");
	dump("disc-shelf-1-slots");

	press(KEY_ESC);
	check(chome_screen_id() == S_DISC, "and back returns to the dialog it was opened from");

	/* ------------------------------- A on a slot: start the disc at that point --- */

	press(KEY_DOWN);
	check(chome_screen_id() == S_SUSPEND, "on the strip again, cursor on the filled slot");

	harness_clear_launch();
	press(KEY_ENTER, 4);
	frame(20);

	char body[256] = {};
	FILE *f = fopen(rec, "rb");
	if (f) { if (fread(body, 1, sizeof(body) - 1, f)) {} fclose(f); }
	printf("  suspend record: %s", body[0] ? body : "(none)\n");

	check(strstr(body, "SLUS-00626") != 0,
		"A on it arms the resume record under the name the mount will publish");
	check(strncmp(body, "psx\n", 4) == 0, "on the PlayStation core");

	/*
	  And the launch itself is the disc, not the shelf's cursor. This is the half that made
	  SCR_LAUNCH the wrong path to reuse: its curtain ends in launch_selected(), and a disc
	  has no card under the cursor to select.
	*/
	check(strstr(harness_last_launch(), ".mgl") != 0, "and hands the disc to that core");

	f = fopen("/tmp/classicui_launch.mgl", "rt");
	check(f != 0, "with an MGL written for it");
	if (f)
	{
		char buf[1024] = {};
		size_t n = fread(buf, 1, sizeof(buf) - 1, f);
		buf[n] = 0;
		fclose(f);
		check(strstr(buf, "_Console/PSX") != 0, "naming the PlayStation core");
		check(strstr(buf, PHYSICAL_DISC_SENTINEL) != 0, "with the sentinel as the file");
	}

	unlink(rec);

	/* --------------------------- and a disc whose key cannot be derived: hide it --- */

	/*
	  A PC Engine CD disc. Playable - tg16 is one of the wired cores - so this cannot pass
	  by accident on "the disc is not launchable": what it lacks is a name. There is no
	  serial on it, and physical_disc.cpp would fall back to the volume label or to a hash
	  of a table of contents this side never sees.
	*/
	disc_reset_reader();
	disc_ingest_present(0);
	(void)disc_take_dirty();
	chome_leave();
	press(KEY_MENU, 20);
	frame(10);

	disc_ingest_present(1);
	fake_disc dc; memset(&dc, 0, sizeof(dc));
	fake_put(&dc, 0, 0, "NOTHINGUSEFUL", 13, 0);
	fake_put(&dc, 1, 0, "xx PC Engine CD-ROM SYSTEM xx", 29, 40);
	disc_set_reader(fake_read, &dc);
	disc_ingest_identify(0);
	frame(6);
	check(disc_type() == DISC_T_PCECD && !disc_serial()[0],
		"a PC Engine CD disc in the drive, which carries no serial");

	press(KEY_UP);
	press(KEY_ENTER);
	check(chome_screen_id() == S_DISC, "its dialog opens the same way");
	dump("disc-shelf-2-no-slots");

	harness_clear_launch();
	press(KEY_DOWN);
	check(chome_screen_id() == S_DISC,
		"Down offers nothing for a disc whose save name cannot be worked out");
	check(harness_last_launch()[0] == 0, "and launches nothing on the way to refusing");

	{
		FILE *g = fopen(rec, "rb");
		check(g == 0, "with no resume record armed for a name nobody could look up");
		if (g) fclose(g);
	}

	// As the sections around this one leave things: no disc, the flag off, the shelf up.
	disc_reset_reader();
	disc_ingest_present(0);
	(void)disc_take_dirty();
	cfg.classicui_disc = 0;
	unlink(ROOT "/savestates/PSX/SLUS-00626_1.ss");
	chome_leave();
	press(KEY_MENU, 20);
	frame(10);
	check(chome_screen_id() == S_HOME, "and the shelf is back for whatever comes next");
}

/*
  When the disc's scan is asked for, and what happens when it arrives late.

  His instruction, in his words: the art for the disc should be fetched and sized as soon
  as we detect the disc, before the user opens the dialog, so the picture is ready when
  they open it - and if it is not ready, the dialog listens for it and swaps the generated
  disc for the scan. Two claims, and neither of them shows in a pixel on its own.

  How the asking is observed. No request is made by this suite and none can be:
  classicui_artfetch is off throughout, so disc_art_request() returns before it forks
  anything, and assert_disc_art() above pins down every one of those refusals. What is
  counted here is the *ask* - disc_art_asks() - because "once, when the disc turned up"
  and "on every frame the dialog draws" are the same picture on screen and a different
  feature, and the counter is the smallest thing that tells them apart.

  How the arrival is observed. disc_art_scale() is called directly, which is exactly what
  ss_fetch_poll() does with the file curl brought down - so the fixture lands the way a
  finished download lands, including the signal disc_art_take_ready() hands over. Then the
  frame is driven by hand rather than through frame(): one millisecond at a time, so the
  spin timer is never due and a repaint can only have come from the scan. That is the
  whole hazard - nothing about a download finishing arrives on a keypress, and this UI
  paints when it is told to. A picture that landed correctly and stayed invisible until
  the player pressed something is the bug this pair of checks is here to catch, and it is
  the same one that made the disc badge appear and vanish without a repaint.

  What cannot be reached from a host test: core_holds_disc(), the half of "the drive is
  the core's" that survives a re-exec. It caches on the first frame of the process by
  design, so it answers 0 for this whole run. The other half - the latch this process sets
  when it hands the drive over - is reachable, and it is the same gate: with it set, the
  loop does not even take the drive's dirty flag, so the disc in there is not asked about.
*/
static void assert_disc_art_arrives()
{
	printf("\n== the disc scan: asked for on detection, and picked up when it lands late ==\n");

	enum { S_HOME = 0, S_DISC = 17 };

	cfg.classicui_disc = 1;
	harness_set_menu_core(1);
	chome_leave();
	press(KEY_MENU, 20);
	frame(10);
	check(chome_screen_id() == S_HOME && disc_state() == DISC_ABSENT,
		"on the shelf with an empty drive");

	// The same PlayStation disc the sections around this one use: SLUS-00626 off the boot
	// configuration, which is the key its scan and its savestates are both filed under.
	fake_disc dp; memset(&dp, 0, sizeof(dp));
	static const char *const none[] = { "" };
	fake_iso(&dp, 0, "PLAYSTATION", "PLAYSTATION", none, 0);
	fake_put(&dp, 20, 0, "BOOT = cdrom:\\SLUS_006.26;1", 27, 100);

	/* --------------------------------- asked for on the detection transition ----- */

	unsigned asked = disc_art_asks();

	disc_ingest_present(1);
	frame(6);
	check(disc_state() == DISC_SPINNING, "a disc has arrived and is still being read");
	check(disc_art_asks() == asked,
		"nothing is asked for while it is still being read: there is no key to file it under");

	disc_set_reader(fake_read, &dp);
	disc_ingest_identify(0);
	frame(1);
	check(disc_type() == DISC_T_PSX && !strcmp(disc_serial(), "SLUS-00626"),
		"and now it is a PlayStation disc with a serial");
	check(disc_art_asks() == asked + 1,
		"its scan is asked for on the very frame it is identified, with no dialog open");

	/*
	  And once. The dialog's own call is per draw and is meant to be - it is what recovers
	  an ask refused for a passing reason - but the shelf must not ask on every pass of a
	  loop that runs sixty times a second, because the whole point of doing this on the
	  transition is that it is one piece of work at one moment.
	*/
	frame(90);
	check(disc_art_asks() == asked + 1, "and once only, not again on every frame after it");

	/* ---------------------------- and not while a core owns the drive ------------- */

	/*
	  Which is done from inside the in-game menu, and the reason is worth writing down
	  because the obvious way round does not work.

	  The latch that says "this process handed the drive over" is cleared at the top of the
	  loop whenever we are in the menu core, and that is right: a launch that did not happen
	  must not leave the drive disowned for ever. But the core has not changed yet in the
	  frame the launch goes out - a disc played from the shelf sets the latch and clears it
	  in the same pass - so from the menu core this state cannot be observed at all. From the
	  in-game menu it can: is_menu() is false, so the latch stands, and the loop keeps
	  running because the menu is open over the game. That is the state the device is in
	  behind a playing disc, and it is where a request for a disc scan would be reaching past
	  a core for a drive that is not ours.

	  The other half of that decision - core_holds_disc(), which is what the re-exec'd
	  process reads - cannot be reached from here at all: it caches on the first frame of the
	  process by design, so it answers 0 for this whole run.
	*/
	chome_leave();
	harness_set_menu_core(0);
	press(KEY_MENU, 14);
	frame(8);
	check(chome_ingame_active(), "the in-game menu is up in a game core");
	check(chome_screen_id() == S_HOME, "on its shelf");

	// The disc goes back in: leaving the front-end stops the detection helper, which
	// forgets what was in the drive - see chome_leave() - and the helper is what the
	// harness stands in for.
	disc_ingest_present(1);
	disc_set_reader(fake_read, &dp);
	disc_ingest_identify(0);
	frame(6);
	check(disc_state() == DISC_READY, "and there is a disc in the drive again");

	press(KEY_UP);
	press(KEY_ENTER);
	check(chome_screen_id() == S_DISC, "whose dialog opens here as it does on the shelf");

	/*
	  A, delivered without a settle frame between the press and its release. The disc's Play
	  hands over immediately rather than through the launch curtain - see the SCR_SUSPEND
	  case for why a disc cannot use that curtain - so the launch is inside this one call.
	*/
	harness_clear_launch();
	chome_handle(KEY_ENTER);
	chome_handle(KEY_ENTER | UPSTROKE);
	check(strstr(harness_last_launch(), ".mgl") != 0, "A hands the disc to a core");

	// Off the dialog: its own per-draw ask is the fallback, and it is not what this part is
	// about. The dirty flag the hand-over raised is left standing, because nothing consumes
	// it while the drive is not ours - which is exactly the point.
	press(KEY_ESC);
	press(KEY_ESC);
	check(chome_screen_id() == S_HOME, "and the shelf is back, with the drive the core's");

	disc_ingest_present(1);
	disc_set_reader(fake_read, &dp);
	disc_ingest_identify(0);

	asked = disc_art_asks();
	frame(20);
	check(disc_state() == DISC_READY, "a disc reads as identified again");
	check(disc_art_asks() == asked,
		"but with the drive handed to a core, nothing asks about what is in it");

	// And that gate is the only thing that was stopping it: the menu core takes the drive
	// back, and the change that was left standing is picked up.
	harness_set_menu_core(1);
	frame(4);
	check(disc_art_asks() == asked + 1, "and it is asked for as soon as the drive is ours again");

	/* ------------------------- a scan that lands while the dialog is open --------- */

	if (chome_ingame_active()) press(KEY_MENU, 14);
	disc_reset_reader();
	disc_ingest_present(0);
	(void)disc_take_dirty();
	chome_leave();
	press(KEY_MENU, 20);
	frame(10);

	disc_ingest_present(1);
	disc_set_reader(fake_read, &dp);
	disc_ingest_identify(0);
	frame(6);
	check(disc_state() == DISC_READY, "the same disc is back in the drive");

	char dst[1024];
	check(disc_art_path("SLUS-00626", dst, sizeof(dst)) == 1, "and its scan has a path to land at");
	unlink(dst);

	press(KEY_UP);
	press(KEY_ENTER);
	check(chome_screen_id() == S_DISC, "its dialog is up with no scan on the card");

	int w = gfx_w(), h = gfx_h();
	int bx0 = w / 4, by0 = h / 2 - h / 6, bx1 = (3 * w) / 4, by1 = h / 2 + h / 6;

	// The fixture's own colour, which is in no palette this UI draws with - so any of it
	// on screen came out of the scan and nothing else. Same licence as the sections above.
	const uint32_t fixcol = 0xff000000u | DISC_FIX_RGB;
	check(box_pixels(bx0, by0, bx1, by1, fixcol) == 0,
		"and nothing of the fixture colour is on screen yet");

	/*
	  A quiet frame first, so that the repaint below cannot be something else's. One
	  millisecond after the last one: the disc's spin timer is not due at GFX_DISC_MS, the
	  selection has settled, and nothing on this screen animates - so this frame paints
	  nothing at all and never reaches the framebuffer.
	*/
	harness_advance(1);
	chome_handle(0);
	int flips = harness_present_count();
	harness_advance(1);
	chome_handle(0);
	check(harness_present_count() == flips, "a frame with nothing happening on it paints nothing");

	// The fetch finishing, exactly as ss_fetch_poll() finishes it: the downloaded scan
	// scaled into the sprite the dialog reads.
	const char *src = "/tmp/chome_disc_scan.png";
	make_disc_scan(src);
	check(disc_art_scale(src, dst) == 1, "the fetch lands: the sprite is written to the card");

	harness_advance(1);
	chome_handle(0);
	check(harness_present_count() == flips + 1,
		"the scan arriving repaints the screen by itself, with no key pressed");
	check(gfx_damage_rows() == h, "and it is the full repaint a changed screen asks for");
	check(box_pixels(bx0, by0, bx1, by1, fixcol) > 100,
		"the dialog is now drawing the scan instead of the generated disc");
	dump("disc-scan-arrives-late");

	/*
	  And exactly one. The signal is handed over once - see disc_art_take_ready() - so the
	  frame after it is quiet again. A per-frame check of the card would repaint here too,
	  which is the cost this is written to avoid.
	*/
	harness_advance(1);
	chome_handle(0);
	check(harness_present_count() == flips + 1, "and that cost one repaint, not one per frame");

	// As the sections around this one leave things: no disc, the flag off, the shelf up.
	unlink(dst);
	unlink(src);
	disc_reset_reader();
	disc_ingest_present(0);
	(void)disc_take_dirty();
	cfg.classicui_disc = 0;
	chome_leave();
	press(KEY_MENU, 20);
	frame(10);
	check(chome_screen_id() == S_HOME, "and the shelf is back for whatever comes next");
}

/* --------------------------------------------------------- screenscraper --- */

/*
  What can honestly be tested here, and what cannot.

  Can: the system-id table and its two extension special cases, the override file,
  the URL builder including that it never leaks a password into a loggable string,
  the error classifier over both HTTP codes and the French sentences, the reply
  parser, and the media picker's ordering rules.

  Cannot: that any of it matches what the server really sends. No request is made
  by this suite and none can be - the tree carries no devid. The XML *placement* of
  type/region/url is the acknowledged soft spot (see the comment at the top of
  chome_ss.cpp), so the parser is tested against all three placements rather than
  against one assumed to be right. When a real reply finally arrives, the thing to
  do is add it here as a fixture, not to trust these.

  The fixtures below are hand-written from the documented shape. That is stated
  plainly because a fixture that looks captured but was invented is the worst kind
  of test evidence.
*/
/*
  A running disc is a game, not just a mounted sentinel.

  What shipped mounted the disc as PHYSICAL_DISC_SENTINEL and used that string as the
  running game's identity too, which broke everything keyed on a game's path. The visible
  half was this: the in-game menu could not park on the disc - no card on the shelf carries
  that path - so Down landed on whatever the shelf happened to be showing, which for Dinofly
  was a folder, and folders have no suspend points. It nudged, and the savestate strip was
  unreachable. The invisible half was that savestates/PSX/*PHYSICAL_DISC*_1.ss is not a
  filename exFAT can hold, so the slots could never have been found anyway.

  The mount now publishes who the disc is and the front-end reads it. Checked here: Down
  reaches the strip, and a core that publishes nothing still gets a working menu instead of
  no identity at all.

  Not checked here: that the name is byte-for-byte the one the core's save files use.
  Nothing in this harness can see it - screenshot_thumbnail() is a stub and no public call
  exposes a slot path - and the assertion that matters is whether a state written by the
  running core turns up in the strip, which is a hardware test.
*/
static void assert_disc_identity()
{
	printf("\n== physical disc: the running disc has an identity ==\n");

	enum { S_SUSPEND = 2, S_DISC = 17 };

	// A state named the way physical_disc_save_name() names a PlayStation disc.
	mkpath(ROOT "/savestates/PSX");
	touch(ROOT "/savestates/PSX", "SLES-01506_1.ss", 256);

	{
		FILE *f = fopen("/tmp/classicui_current", "wt");
		if (f) { fprintf(f, "psx\n%s\n", PHYSICAL_DISC_SENTINEL); fclose(f); }
	}
	{
		FILE *f = fopen(PHYSICAL_DISC_IDENT_FILE, "wt");
		if (f) { fprintf(f, "SLES-01506\nMETAL GEAR SOLID\n"); fclose(f); }
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

	press(KEY_MENU, 14);
	frame(10);
	check(chome_ingame_active(), "the in-game menu opens over the disc");

	/*
	  On the disc's own dialog, and built entirely from the published identity: the drive
	  belongs to the core, so the detection helper was stopped at the launch and never
	  restarted. disc_state() has said ABSENT ever since, which is exactly why a dialog
	  keyed on the drive would have been blank here.
	*/
	check(chome_screen_id() == S_DISC,
		"the menu opens on the disc dialog rather than on the shelf");
	check(disc_state() == DISC_ABSENT,
		"and the drive is telling us nothing, because it is the core's now");
	dump("disc-identity-1-menu");

	{
		// It is turning, too. The repaint that advances it used to be armed off
		// disc_state(), so in a game the disc drew once and then sat there stopped.
		unsigned long a = pt_panel_hash();
		frame(20);
		unsigned long b = pt_panel_hash();
		check(a != b, "the disc on it is turning, with no drive state to arm the repaint");
	}

	press(KEY_DOWN, 18);
	frame(8);
	check(chome_screen_id() == S_SUSPEND,
		"Down reaches the disc's savestates instead of nudging on a folder");
	dump("disc-identity-2-slots");

	press(KEY_ESC, 14);
	frame(6);
	check(chome_screen_id() == S_DISC,
		"and back from the strip returns to the dialog it was opened from");

	/*
	  A on the dialog over a running disc goes back to the game, which is what A means on
	  the running game's own card. There is nothing to launch: the disc is already loaded,
	  and its type is gone with the drive, so a Play here could only guess at a core.
	*/
	press(KEY_ENTER, 14);
	frame(8);
	check(!chome_ingame_active(), "A on it resumes the game rather than relaunching the disc");

	/* --------------------------- the scan, drawn in the dialog only ---------- */

	/*
	  A picture filed under the disc's identity replaces the drawn disc here and nowhere
	  else - his decision: the badge on the shelf keeps the drawing, because at badge size a
	  photograph is thirty-two pixels of mud.

	  Square, because that is the shape of a disc scan. decode_into() letterboxes anything
	  else with black, and inside a circular mask that would read as a fault rather than as
	  a picture that does not fit.

	  Counted by an exact colour rather than by a hash: both states animate, and two hashes
	  of a turning disc differ with nothing wrong - the trap this file's own comments warn
	  about. 0xff20c020 is in no palette this UI draws with, so any of it on screen came out
	  of the fixture.
	*/
	mkpath(ROOT "/classicui/discart");

	// The band pt_panel_hash() reads, which at this canvas holds the whole disc and
	// nothing of the shelf behind the panel.
	int bx0 = 1280 / 4, by0 = 720 / 2 - 720 / 6;
	int bx1 = (3 * 1280) / 4, by1 = 720 / 2 + 720 / 6;

	press(KEY_MENU, 14);
	frame(10);
	check(chome_screen_id() == S_DISC, "back on the dialog");
	check(box_pixels(bx0, by0, bx1, by1, 0xff20c020u) == 0,
		"with no scan on the card, nothing of the fixture is on screen");

	make_cover(ROOT "/classicui/discart/SLES-01506.png", 400, 400, 0xff20c020);
	frame(10);
	check(box_pixels(bx0, by0, bx1, by1, 0xff20c020u) > 100,
		"and once one is there it is what the dialog draws");
	dump("disc-identity-3-scan");

	{
		// Rotating on the fly, inside the recorded rectangle. A scan that stopped turning
		// the moment it acquired a picture would read as a bug in the animation.
		unsigned long a = harness_fb_hash_box(bx0, by0, bx1, by1);
		frame(30);
		unsigned long b = harness_fb_hash_box(bx0, by0, bx1, by1);
		check(a != b, "and it turns like the drawn disc does");
	}

	unlink(ROOT "/classicui/discart/SLES-01506.png");

	/* --------------- a core that published no name still gets a menu --------- */

	unlink(PHYSICAL_DISC_IDENT_FILE);
	press(KEY_MENU, 14);                  // close, so reopening reloads the identity
	frame(8);
	press(KEY_MENU, 14);
	frame(10);
	check(chome_ingame_active(), "the menu still opens with no name published");

	press(KEY_DOWN, 18);
	frame(8);
	check(chome_screen_id() == S_SUSPEND, "and the disc is still what Down is about");

	press(KEY_ESC, 14);
	frame(6);
	if (chome_ingame_active()) press(KEY_MENU, 14);
	frame(6);

	unlink("/tmp/classicui_current");
	unlink(ROOT "/savestates/PSX/SLES-01506_1.ss");

	/*
	  Put the menu core back. The sections after this one start from the shelf without
	  declaring so - assert_launch() just calls chome_leave() and presses menu - so a
	  game core left loaded here makes them launch nothing and fail three sections later,
	  a long way from the cause.
	*/
	harness_set_menu_core(1);
	chome_leave();
	frame(6);
}

/*
  The dim ig_build_background() applies to the still, repeated here on purpose.

  It is the only way to name a pixel that came out of the still: the value on screen is the
  captured colour after this arithmetic, so counting it means doing the same arithmetic. If
  the dim in chome_ui.cpp ever changes, this moves with it and the counts below go to zero
  loudly rather than drifting quietly.
*/
static uint32_t still_dimmed(uint32_t c)
{
	uint32_t r = ((c >> 16) & 0xff) * 6 / 16;
	uint32_t g = ((c >> 8) & 0xff) * 6 / 16;
	uint32_t b = (c & 0xff) * 6 / 16;
	return 0xff000000u | (r << 16) | (g << 8) | b;
}

/*
  A still colour that appears in no palette, no icon and no font here, so any of it on
  screen came out of the capture and out of nothing else.
*/
#define STILL_FLAT 0xff60c0f0u

// How many pixels of the still survived onto the whole frame.
static int still_on_screen()
{
	return box_pixels(0, 0, gfx_w(), gfx_h(), still_dimmed(STILL_FLAT));
}

// One pixel of it, in the corner the fit reaches only when the picture spans the width.
static int still_in_corner()
{
	return box_pixels(0, 0, 8, 8, still_dimmed(STILL_FLAT));
}

/*
  Which in-game screens show the still of the game, counted rather than looked at.

  Dinofly reported the in-game menu over a physical disc as a black background where a
  file-launched game in the same core, same build and same profile showed the still of the
  game correctly, and the obvious reading of that is that the capture failed on the disc
  launch path. It is the wrong reading, and this section is what says so: the capture is the
  same call on both paths, and the still is built and drawn over a running disc here.

  What differs is the screen the menu opens on. Over a disc it opens on the disc's own
  dialog - deliberately, see ig_open() - and that dialog is the width of the canvas less the
  inset and reaches from the top margin to the button legend. So the only background it
  leaves showing is a narrow strip down each side, and on a stretched canvas those strips
  were entirely inside the black bars the fit left: the still was in the middle half of the
  width, underneath the dialog, and every pixel the player could see was a bar. A capture
  that worked perfectly and a capture that returned nothing look exactly the same from the
  sofa, which is why screenshot_grab_why() now says which it was.

  The bars are the half that was a bug. shot_fit() fitted 4:3 by raw pixel count, and a
  15 kHz TV canvas arrives as 640x240 whenever the framebuffer takeover is not held, so the
  picture came out 320 wide in a 640-wide canvas. Checked below on three canvases: the
  stretched one, where the corner must now be picture; a 4:3 one, where it always was; and a
  16:9 one, where it must still be a bar, because a 4:3 picture on a wide screen is supposed
  to be letterboxed and stretching it would be a different bug.
*/
static void assert_ingame_still()
{
	printf("\n== the still of the game, and which screens show it ==\n");

	enum { S_HOME = 0, S_DISC = 17 };

	harness_set_grab_flat(STILL_FLAT);
	harness_set_menu_core(0);
	harness_set_fb_supported(1);
	harness_set_osd_visible(0);

	/* ----------------------------------------- a file-launched game, on the shelf --- */

	{
		FILE *f = fopen("/tmp/classicui_current", "wt");
		if (f) { fprintf(f, "gb\nTetris (World).gb\n"); fclose(f); }
	}

	struct { const char *name; int w, h; int corner; } canv[] = {
		// A stretched 15 kHz canvas: px=2, and the whole width is one 4:3 picture.
		{ "640x240", 640, 240, 1 },
		// A square-pixelled 240p canvas: px=1, 4:3 already, and always worked.
		{ "320x240", 320, 240, 1 },
		// And 16:9, where the corner is meant to be a black bar and not picture.
		{ "1280x720", 1280, 720, 0 },
	};

	for (unsigned c = 0; c < sizeof(canv) / sizeof(canv[0]); c++)
	{
		harness_set_fb(canv[c].w, canv[c].h);
		gfx_shutdown();
		theme_update(canv[c].w, canv[c].h, 0);

		chome_handle(0);
		if (chome_ingame_active()) press(KEY_MENU, 14);
		frame(6);
		press(KEY_MENU, 20);
		for (int i = 0; i < 40 && lib_scanning(); i++) frame(2);
		frame(12);

		char what[128];
		snprintf(what, sizeof(what), "%s: the menu opens over the game on the shelf", canv[c].name);
		check(chome_ingame_active() && chome_screen_id() == S_HOME, what);

		int on = still_on_screen();
		int corner = still_in_corner();
		printf("  %s: %d pixels of the still on screen, %d of 64 in the corner\n",
			canv[c].name, on, corner);

		snprintf(what, sizeof(what), "%s: the shelf shows the still of the game", canv[c].name);
		check(on > 0, what);

		if (canv[c].corner)
		{
			snprintf(what, sizeof(what),
				"%s: and the picture reaches the corner, so it spans the width", canv[c].name);
			check(corner == 64, what);
		}
		else
		{
			snprintf(what, sizeof(what),
				"%s: and a 4:3 picture on a wide canvas leaves the corner a black bar", canv[c].name);
			check(corner == 0, what);
		}

		press(KEY_MENU, 14);
		frame(6);
	}

	/* ------------------------------------------------ and a disc, on its own dialog --- */

	{
		FILE *f = fopen("/tmp/classicui_current", "wt");
		if (f) { fprintf(f, "psx\n%s\n", PHYSICAL_DISC_SENTINEL); fclose(f); }
	}
	{
		FILE *f = fopen(PHYSICAL_DISC_IDENT_FILE, "wt");
		if (f) { fprintf(f, "SLES-01506\nMETAL GEAR SOLID\n"); fclose(f); }
	}

	/*
	  Both the canvases a television gives, because the two together are the reported symptom
	  and the bug behind it. The numbers this prints, of 153600 and 76800 pixels:

	    canvas    dialog   shelf     dialog before shot_fit took px
	    640x240     3180   89666     640 of the 3180, and 42417 of the 89666
	    320x240     1431   43796     unchanged - a 4:3 canvas never had bars

	  So on the stretched canvas the dialog left the player 640 pixels of game out of 153600,
	  four tenths of one per cent, and every one of them was in the sliver of full-width rows
	  below the panel: the strips down the sides, which are what the dialog leaves, were
	  inside the black bars the fit had put there. That is a black background by any
	  reasonable description, and it is the case shot_fit() now gets right.
	*/
	int on_disc = 0, on_home = 0;
	struct { const char *name; int w, h; } dcanv[] = { { "640x240", 640, 240 }, { "320x240", 320, 240 } };

	for (unsigned c = 0; c < sizeof(dcanv) / sizeof(dcanv[0]); c++)
	{
		harness_set_fb(dcanv[c].w, dcanv[c].h);
		gfx_shutdown();
		theme_update(dcanv[c].w, dcanv[c].h, 0);

		chome_handle(0);
		if (chome_ingame_active()) press(KEY_MENU, 14);
		frame(6);
		press(KEY_MENU, 20);
		frame(12);

		char what[128];
		snprintf(what, sizeof(what), "%s: over a disc the menu opens on the disc dialog instead",
			dcanv[c].name);
		check(chome_ingame_active() && chome_screen_id() == S_DISC, what);

		on_disc = still_on_screen();
		dump(c == 0 ? "still-1-disc-dialog-tv640" : "still-2-disc-dialog-240p");

		press(KEY_ESC, 14);
		frame(10);
		on_home = still_on_screen();
		if (c == 1) dump("still-3-disc-shelf");

		printf("  %s: %d pixels of the still behind the disc dialog, %d behind the same menu on the shelf\n",
			dcanv[c].name, on_disc, on_home);

		/*
		  The capture is not what is wrong over a disc, and this is the check that says so:
		  the still is on screen, in the strips the dialog leaves, on the very path that was
		  reported black. What is wrong is how little of it there is - the dialog is the
		  canvas less the inset by Dinofly's own decision, and those strips are then dimmed a
		  second time by the overlay scrim compose() lays over every panel screen. Both of
		  those are his calls and neither is changed here.
		*/
		snprintf(what, sizeof(what),
			"%s: the still is drawn over a disc too - the capture is not the fault", dcanv[c].name);
		check(on_disc > 0, what);

		snprintf(what, sizeof(what),
			"%s: and the shelf behind the same menu shows far more of it", dcanv[c].name);
		check(on_home > on_disc * 8, what);

		snprintf(what, sizeof(what), "%s: back from the dialog reaches the shelf", dcanv[c].name);
		check(chome_screen_id() == S_HOME, what);

		press(KEY_MENU, 14);
		frame(6);
	}

	harness_set_fb(320, 240);
	gfx_shutdown();
	theme_update(320, 240, 0);

	/* ------------------------------------------------- and when there is no capture --- */

	/*
	  The other half of the report, so the log line can be trusted: a grab that really does
	  refuse leaves no still at all and the grid shows through, which is a different picture
	  from the one above and not one anybody has seen on the device.
	*/
	harness_set_grab(0);
	press(KEY_MENU, 20);
	frame(12);
	check(chome_ingame_active(), "a refused capture still opens the menu");
	check(still_on_screen() == 0, "with no still anywhere on the frame");
	dump("still-4-no-capture");
	harness_set_grab(1);

	press(KEY_MENU, 14);
	frame(6);

	harness_set_grab_flat(0);
	unlink(PHYSICAL_DISC_IDENT_FILE);
	unlink("/tmp/classicui_current");

	/*
	  Exactly as assert_disc_identity() above leaves things, canvas included. The section
	  after this one reads a box scaled to the canvas and expects the 720p one; walking
	  three canvases here and leaving the last of them behind failed it two hundred lines
	  from the cause.
	*/
	harness_set_fb(1280, 720);
	gfx_shutdown();
	theme_update(1280, 720, 1);
	harness_set_menu_core(1);
	chome_leave();
	frame(6);
}

/*
  Close the in-game menu, put a different flat colour in the capture, and open it again.

  The dim is applied once, when the menu opens - ig_build_background() - so a colour changed
  under an open menu would be measured through the previous one's arithmetic.
*/
static void ig_menu_reopen(uint32_t flat)
{
	if (chome_ingame_active()) press(KEY_MENU, 14);
	frame(6);
	harness_set_grab_flat(flat);
	press(KEY_MENU, 20);
	frame(12);
}

/*
  How much of the paused game shows behind a menu, in pixel values.

  The question, and it is not the same one as "is the still there at all" that the section
  above answers. The still is dimmed to 6/16 when the menu opens, and then compose() lays
  gfx_scrim()'s 50% checkerboard over the whole canvas on every panel screen. A checkerboard
  replaces half the pixels outright, so with COL_BGDARK it is a floor and not a dim: anything
  in the game darker than COL_BGDARK came out as COL_BGDARK. Measured on the device over a
  paused PlayStation core, the framebuffer outside the dialog held exactly two luma values, 0
  and 31 - the game was mid-fade, every pixel of it was under the floor, and the result was a
  flat grey-black rectangle. A bright game survived the same treatment and merely dimmed.

  So the scrim over a still is COL_BLACK, which multiplies rather than floors, and the dim
  moved 5/16 -> 6/16 to keep the brightest case from getting dimmer than it already was. Both
  are single numbers in chome_ui.cpp and both are worth exactly nothing unasserted, because
  "the dark scene reads better now" is the kind of claim an eye on a television invents.

  Measured on a *file-launched* game, deliberately: this is not about discs. Every panel screen
  over every paused game got the same double treatment.

  What the four rows below come to, as the mean of the three channels over one box of
  background - which is what the eye reads off a checkerboard, the two values alternating:

    capture   screen           before (5/16, COL_BGDARK)   now (6/16, COL_BLACK)
    white     shelf, no scrim  79        mean 79.0         95       mean 95.0
    white     panel, scrim     79 and 33 mean 55.8         95 and 0 mean 47.5
    grey 32   panel, scrim     10 and 33 mean 21.3         12 and 0 mean  6.0
    black     panel, scrim      0 and 33 mean 16.3          0 and 0 mean  0.0

  The middle two are the point. Nothing behind a panel is brighter than it was - 47.5 against
  55.8 - so no text over it reads worse; but the range the picture has to work in is 0..47.5
  instead of 16.3..47.5, and a dark scene is dark instead of being lifted into the same flat
  grey a black one was.
*/
static void assert_ingame_dim()
{
	printf("\n== the dim and the scrim over a paused game ==\n");

	enum { S_HOME = 0, S_SORT = 3 };

	int was_prof = cfg.classicui_profile;

	/*
	  A 4:3 canvas, so shot_fit() gives the picture the whole of it and every pixel in the box
	  below is game rather than letterbox. See assert_ingame_still().

	  The forced profile goes back to `auto` for the duration, which the sections around this
	  one do not bother with because nothing in them depends on it. Here it decides where the
	  Sort panel lands, and the box below is chosen to sit above it: left forced to HD on a
	  320x240 canvas the panel is a different size on a canvas the log then calls "hd", which
	  is a confusing thing to leave in a section that is about pixel values.
	*/
	cfg.classicui_profile = 0;
	harness_set_fb(320, 240);
	gfx_shutdown();
	theme_update(320, 240, 0);

	harness_set_menu_core(0);
	harness_set_fb_supported(1);
	harness_set_osd_visible(0);
	harness_set_grab(1);

	{
		FILE *f = fopen("/tmp/classicui_current", "wt");
		if (f) { fprintf(f, "gb\nTetris (World).gb\n"); fclose(f); }
	}
	chome_handle(0);

	/*
	  The box every mean below is taken over: the rows between the menu bar and the panel.

	  Both of those edges are drawn things and both had to be found the hard way. The bar slides
	  in whenever a panel is open - compose() hands draw_menubar() the same `overlay` flag it
	  scrims on - so the rows just under the top margin are COL_PANEL rather than game, and a
	  box that started at a fixed row 30 read three rows of bar at 240p and a mean half again
	  too bright. Below, the panel is centred on the canvas. What is left in between is
	  background on every screen this reads, which is the only kind of box worth a mean.

	  Above the bar would do as well on paper and is worse in practice: draw_running_warning()
	  puts a red band there on a core that cannot pause, and that is a property of the fixture
	  rather than of this question.

	  An even width, because half of a 50% checkerboard is only exactly half of an even number
	  of pixels.
	*/
	const chome_profile *pr = theme_get();
	int panel_top = (pr->h - pr->panel_h) / 2;       // draw_panel_ex() centres it; checked below
	const int bx0 = pr->inset + 4, bx1 = pr->w - pr->inset - 4;
	const int by0 = pr->safe_y + pr->bar_h + 2, by1 = panel_top - 2;
	const int area = (bx1 - bx0) * (by1 - by0);

	printf("  reading %dx%d at %d,%d - between the bar at %d and the panel at %d\n",
		bx1 - bx0, by1 - by0, bx0, by0, pr->safe_y + pr->bar_h, panel_top);

	check((bx1 - bx0) % 2 == 0 && by1 - by0 >= 4, "there is a box of background to measure in");

	/* ------------------------------------------- white: the brightest case there is --- */

	ig_menu_reopen(0xffffffffu);
	check(chome_ingame_active() && chome_screen_id() == S_HOME,
		"the menu is open over the game, on the shelf");

	uint32_t lit = still_dimmed(0xffffffffu);
	int mean_shelf = box_mean_x100(bx0, by0, bx1, by1);

	printf("  white capture: shelf mean %d.%02d, every pixel %06x\n",
		mean_shelf / 100, mean_shelf % 100, lit & 0xffffffu);

	check(box_pixels(bx0, by0, bx1, by1, lit) == area,
		"with no panel open the still is dimmed once and nothing else touches it");
	check(mean_shelf == 9500, "which is 6/16 of white: a mean of 95");

	press(KEY_GRAVE, 20);
	check(chome_screen_id() == S_SORT, "a panel screen over the same still");
	dump("dim-1-panel-over-white");

	// The one copied piece of arithmetic in this section, checked rather than trusted: if the
	// panel is not where the box was measured against, every mean below is over the wrong
	// pixels and would fail in a way that named the dim instead.
	{
		int ox = 0, oy = 0, ow = 0, oh = 0;
		int got = panel_plate_seen(&ox, &oy, &ow, &oh);
		check(got && oy - (10 * pr->ts_ui + 6) == panel_top,
			"the panel is where the box was measured against");
	}

	int mean_lit = box_mean_x100(bx0, by0, bx1, by1);
	int n_lit = box_pixels(bx0, by0, bx1, by1, lit);
	int n_black = box_pixels(bx0, by0, bx1, by1, COL_BLACK);
	int n_bgdark = box_pixels(bx0, by0, bx1, by1, COL_BGDARK);

	printf("  white capture: panel mean %d.%02d, %d lit + %d black of %d, %d COL_BGDARK\n",
		mean_lit / 100, mean_lit % 100, n_lit, n_black, area, n_bgdark);

	check(n_lit + n_black == area,
		"the scrim over a still leaves two values and no third one: the game, and black");
	check(n_lit == area / 2, "half of each, which is what a 50% checkerboard is");
	check(n_bgdark == 0, "and no COL_BGDARK anywhere in it - that colour was the floor");
	check(mean_lit == 4750, "so the mean is half of 95");

	/*
	  Legibility, which is the half of this that wins if the two ever disagree: whatever the
	  game is doing, the background behind a panel is no brighter than it was before this
	  change. 5583 is the pair it replaces - 79 of dimmed white alternating with COL_BGDARK,
	  whose own three channels mean 32.67.
	*/
	check(mean_lit < 5583, "and no brighter than the pair it replaces, so no text reads worse");

	/* ----------------------------------------------- black: the reported symptom --- */

	ig_menu_reopen(0xff000000u);
	press(KEY_GRAVE, 20);
	check(chome_screen_id() == S_SORT, "a panel screen over a game that is drawing black");
	dump("dim-2-panel-over-black");

	int mean_dark = box_mean_x100(bx0, by0, bx1, by1);
	n_bgdark = box_pixels(bx0, by0, bx1, by1, COL_BGDARK);

	printf("  black capture: panel mean %d.%02d, %d COL_BGDARK, %d black of %d\n",
		mean_dark / 100, mean_dark % 100, n_bgdark,
		box_pixels(bx0, by0, bx1, by1, COL_BLACK), area);

	check(box_pixels(bx0, by0, bx1, by1, COL_BLACK) == area,
		"a black scene reads as black rather than as a lift to flat grey");
	check(n_bgdark == 0, "which is the two luma values, 0 and 31, gone from the frame");
	check(mean_dark == 0, "a mean of nothing, where the pair before it meant 16.3");

	/* -------------------------------- and a dark scene that is not actually black --- */

	/*
	  The difference between a floor and a dim, as one number. 32 through 6/16 is 12, halved
	  by the checkerboard is 6; the same 32 used to come out at 10 alternating with the floor's
	  33, a mean of 21.3 that sat a third of the way up the whole range the picture had. What
	  is on screen now is proportional to what the game drew, which is what makes a dark scene
	  a dark scene instead of the same grey a black one gave.
	*/
	ig_menu_reopen(0xff202020u);
	press(KEY_GRAVE, 20);
	check(chome_screen_id() == S_SORT, "and over a dark scene that is not black");

	int mean_dim = box_mean_x100(bx0, by0, bx1, by1);
	printf("  grey 32 capture: panel mean %d.%02d\n", mean_dim / 100, mean_dim % 100);

	check(mean_dim == 600, "a dark scene is dimmed in proportion, not raised to a floor");
	check(mean_dim > 0 && mean_dim < mean_lit,
		"so it is between black and bright rather than indistinguishable from black");

	/* ------------------------------------- and the scrim is unchanged without a still --- */

	/*
	  The other side of the condition, which is most of the front-end: with no still under it
	  the scrim is COL_BGDARK exactly as it always was. That colour is the shelf's own bottom
	  band, and dimming the grid towards it is what it is for - there is nothing under
	  COL_BGDARK there to be floored.
	*/
	if (chome_ingame_active()) press(KEY_MENU, 14);
	frame(6);
	harness_set_grab_flat(0);
	unlink("/tmp/classicui_current");
	harness_set_menu_core(1);
	chome_leave();
	press(KEY_MENU, 20);
	frame(12);
	check(!chome_ingame_active(), "out of the game, on the shelf");

	press(KEY_GRAVE, 20);
	check(chome_screen_id() == S_SORT, "with the same panel open over the grid instead");
	dump("dim-3-panel-no-still");

	int grid_bgdark = box_pixels(bx0, by0, bx1, by1, COL_BGDARK);
	printf("  no still: %d COL_BGDARK of %d in the same box\n", grid_bgdark, area);
	check(grid_bgdark == area / 2,
		"the scrim over the front-end's own background is COL_BGDARK, unchanged");

	press(KEY_ESC, 12);

	/*
	  Exactly as assert_ingame_still() leaves things, canvas included: the section after it
	  reads a box scaled to the canvas and expects the 720p one.
	*/
	cfg.classicui_profile = (uint8_t)was_prof;
	harness_set_fb(1280, 720);
	gfx_shutdown();
	theme_update(1280, 720, 1);
	harness_set_menu_core(1);
	chome_leave();
	frame(6);
}

/*
  Fixture credentials and a fixture host.

  Every one of these is obviously fake, and that is not decoration. The measured reply
  this fixture imitates carried our real devid, our real devpassword and Dinofly's own
  ScreenScraper login inside all 133 of its media URLs, so committing anything resembling
  the real thing would put the account in the repository permanently.

  The host is under .invalid, which RFC 2606 reserves so that it can never resolve. If a
  bug ever does let this suite hand a fixture URL to curl, the request fails in the
  resolver instead of reaching a third party under a credential.
*/
#define FIX_HOST    "https://fixture.invalid/media"
#define FIX_DEVID   "FIXTURE_NOT_A_DEVID"
#define FIX_DEVPASS "FIXTURE_NOT_A_DEVPASSWORD"
#define FIX_SSID    "fixture_not_a_user"
#define FIX_SSPASS  "FIXTURE_NOT_A_PASSWORD"

/*
  A reply in the shape and the size the real one turned out to have.

  Hand-built from what was measured on 2026-08-05 rather than captured, for the reason
  above - and the shape is the measured shape, not the one this parser was written
  against: type, region, format, crc, md5, sha1 and size are attributes, and the URL is
  the element text. There is no url attribute anywhere in a real reply.

  Two properties of the real reply are reproduced because they are what broke:

    133 media in total, against a store of 64. Everything past the cap was discarded.

    the server groups by type, and the types the pickers want are not all near the
    front. Measured first occurrences were sstitle 0, ss 1, wheel 6, box-2D 17,
    box-3D 47, mixrbv1 89, mixrbv2 97 and support-2D past 64 - so the overflow took
    mixrbv1, mixrbv2 and the disc scan out of every reply, and the mixrbv fallbacks in
    ss_pick() had literally never been reachable.

  The group sizes below are not the measured ones - only the first indices were recorded
  - but they preserve the ordering and put support-2D at raw index 128, well past the old
  cap. wheel is given eleven regions on purpose, two more than SS_MAX_PER_TYPE, so the
  per-type ceiling is exercised as well as the total.

  support-2D gets exactly the five regions the real reply carried for support media - de,
  eu, uk, jp, sp - and it matters that "us" is not among them: that is what makes the
  first-in-reply fallback testable against real data rather than against a case invented
  to suit it.
*/
static void write_measured_reply(const char *path)
{
	FILE *f = fopen(path, "wb");
	if (!f) { printf("  cannot write %s\n", path); return; }

	fprintf(f,
		"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
		"<Data>\n"
		"  <ssuser><id>%s</id><maxthreads>1</maxthreads>"
		"<requeststoday>12</requeststoday><maxrequestsperday>20000</maxrequestsperday></ssuser>\n"
		"  <jeu id=\"12345\">\n"
		"    <noms><nom region=\"wor\">Fixture Game</nom></noms>\n"
		"    <medias>\n", FIX_SSID);

	static const char *const any_region[] =
		{ "wor", "us", "eu", "jp", "de", "uk", "sp", "fr", "it", "br", "kr", "cn" };
	static const char *const sup_region[] = { "de", "eu", "uk", "jp", "sp" };

	static const struct { const char *type; int n; } groups[] =
	{
		{ "sstitle",         1  },
		{ "ss",              1  },
		{ "video",           4  },
		{ "wheel",           11 },
		{ "box-2D",          12 },
		{ "box-texture",     18 },
		{ "box-3D",          12 },
		{ "support-texture", 20 },
		{ "bezel-16-9",      9  },
		{ "mixrbv1",         4  },
		{ "mixrbv2",         4  },
		{ "figurine",        6  },
		{ "pictomonochrome", 12 },
		{ "themehs",         14 },
		{ "support-2D",      5  },
	};

	int total = 0;

	for (size_t g = 0; g < sizeof(groups) / sizeof(groups[0]); g++)
	{
		int is_sup = !strcmp(groups[g].type, "support-2D");
		const char *const *rl = is_sup ? sup_region : any_region;
		int nrl = is_sup ? 5 : 12;

		for (int i = 0; i < groups[g].n; i++)
		{
			const char *rg = rl[i % nrl];

			/*
			  The separators are written as &amp;, which is what a well-formed reply has
			  to write them as. sxmlc decodes entities in attribute values and explicitly
			  does not decode them in text, so this is also what proves url_unescape()
			  runs: without it the URL keeps its "&amp;" and curl sees one parameter whose
			  value swallowed the password.
			*/
			fprintf(f,
				"      <media type=\"%s\" region=\"%s\" format=\"png\""
				" crc=\"0000dead\" md5=\"00000000000000000000000000000000\""
				" sha1=\"0000000000000000000000000000000000000000\" size=\"417000\">"
				"%s/%s-%s.png?devid=%s&amp;devpassword=%s&amp;ssid=%s&amp;sspassword=%s"
				"</media>\n",
				groups[g].type, rg, FIX_HOST, groups[g].type, rg,
				FIX_DEVID, FIX_DEVPASS, FIX_SSID, FIX_SSPASS);
			total++;
		}
	}

	fprintf(f, "    </medias>\n  </jeu>\n</Data>\n");
	fclose(f);

	printf("  fixture reply: %d media elements\n", total);
}

// How many stored media carry this type.
static int media_count_of(const ss_result *r, const char *type)
{
	int n = 0;
	for (int i = 0; i < r->nmedia; i++) if (!strcasecmp(r->media[i].type, type)) n++;
	return n;
}

static void assert_screenscraper()
{
	printf("\n== screenscraper (inert: no devid in this tree) ==\n");

	// The gate. The harness build defines a dummy devid, which is the only reason
	// anything below is reachable at all.
	check(ss_available() == 1, "the harness build carries a dummy devid, so the module is live here");

	/* ------------------------------------------------- loaded at start-up --- */

	/*
	  main() wrote classicui/ss-systems.cfg on the fake card before this process drew its
	  first frame, with a psx override nowhere near the built-in 57 or the "999" this
	  section loads further down. If chome_ui.cpp's own start-up path ever stops calling
	  ss_systems_load(), this reads back 57 instead and catches it - which a test that
	  called ss_systems_load() itself could not, since that would only prove the function
	  works, not that anything wires it up.
	*/
	const char *id = ss_system_id("psx", "x.cue");
	check(id && !strcmp(id, "8675309"),
		"the override file main() seeded before frame one is already active here");
	ss_systems_forget();

	/* -------------------------------------------------------- system ids --- */

	id = ss_system_id("psx", "Destruction Derby (USA).cue");
	check(id && !strcmp(id, "57"), "psx maps to systemeid 57");

	id = ss_system_id("nes", "Zelda.nes");
	check(id && !strcmp(id, "3"), "nes maps to 3");

	id = ss_system_id("SNES", "Metroid.sfc");
	check(id && !strcmp(id, "4"), "the system id is matched case-insensitively");

	// The shelves that hold two platforms, told apart only by extension.
	id = ss_system_id("gb", "Tetris.gb");
	check(id && !strcmp(id, "9"), "a .gb in the Game Boy shelf is 9");

	id = ss_system_id("gb", "Zelda DX.gbc");
	check(id && !strcmp(id, "10"), "and a .gbc in the same shelf is 10, not 9");

	/*
	  Game Gear used to return nothing here: its systemeid was not one of the values a
	  live client's own source could verify, and the tempting alternative - fall back to
	  the Master System id - would have scraped .gg games as Master System and put the
	  wrong covers on the shelf silently. It is verified now (see chome_ss.cpp), so a .gg
	  gets its own id instead of staying uncovered forever.
	*/
	id = ss_system_id("sms", "Sonic.gg");
	check(id && !strcmp(id, "21"), "a .gg in the Master System shelf is 21, not 2");
	id = ss_system_id("sms", "Sonic.sms");
	check(id && !strcmp(id, "2"), "while a real .sms is still 2");

	id = ss_system_id("ws", "Rockman EXE WS.ws");
	check(id && !strcmp(id, "45"), "a .ws in the WonderSwan shelf is 45");
	id = ss_system_id("ws", "Rockman EXE WS.wsc");
	check(id && !strcmp(id, "46"), "and a .wsc in the same shelf is 46, not 45");

	id = ss_system_id("ngp", "SNK vs Capcom.ngp");
	check(id && !strcmp(id, "25"), "a plain .ngp is 25");
	id = ss_system_id("ngp", "SNK vs Capcom.ngc");
	check(id && !strcmp(id, "82"), "and a .ngc in the same shelf is 82, not 25");
	/*
	  .npc is the third extension chome_lib.cpp's Neo Geo Pocket row accepts, and neither
	  reference client this table was cross-checked against names it with confidence as
	  either the mono or the Color hardware - so, like .gg before it was verified, this
	  returns nothing rather than guess between two real platforms.
	*/
	check(ss_system_id("ngp", "SNK vs Capcom.npc") == 0,
		"a .npc is refused: it is genuinely ambiguous between mono and Color");
	id = ss_system_id("ngp", 0);
	check(id && !strcmp(id, "25"), "and with no extension to go on at all, ngp defaults to mono");

	// Saturn is why this table's gaps got closed - the system the bug report was about.
	id = ss_system_id("saturn", "Panzer Dragoon (USA).cue");
	check(id && !strcmp(id, "22"), "saturn maps to systemeid 22");

	check(ss_system_id("", "x.nes") == 0, "an empty system id is refused");

	/*
	  Every system chome_lib.cpp's library table carries either has a systemeid or is
	  named here as a deliberate exception - so a future system added to the library
	  table without a line in chome_ss.cpp's builtin[] fails loudly instead of just
	  quietly never showing art. There are no deliberate exceptions at the row level
	  today: every row resolves with no extension hint (romnom 0, so the ngp/gg/ws-style
	  extension cases above do not apply) - the only gap left is the .npc extension case
	  just above, which is not a row of its own.
	*/
	{
		static const char *const deliberately_absent[] = { 0 };  // none, today

		int n = lib_sys_count();
		check(n > 20, "the library table still has the systems this loop is walking");

		for (int i = 0; i < n; i++)
		{
			const chome_sys *s = lib_sys(i);
			const char *mapped = ss_system_id(s->id, 0);

			int excused = 0;
			for (int e = 0; deliberately_absent[e]; e++)
				if (!strcasecmp(deliberately_absent[e], s->id)) excused = 1;

			char what[96];
			snprintf(what, sizeof(what), "%s has a systemeid or is a named exception", s->id);
			check(mapped != 0 || excused, what);
		}
	}

	/* ---------------------------------------------------- override file --- */

	put_file("/tmp/chome_ss_sys.cfg",
		"# a comment\n"
		"\n"
		"homebrew = 999999\n"       // no library row named this at all
		"psx=999\n"                 // deliberately overrides a built-in
		"bogus=notanumber\n"        // must be ignored: this goes into a URL
		"noequals\n");

	check(ss_systems_load("/tmp/chome_ss_sys.cfg") == 2,
		"the override file takes two good lines and drops the junk");

	id = ss_system_id("homebrew", "whatever.rom");
	check(id && !strcmp(id, "999999"),
		"an override can name a system with no built-in entry at all");

	id = ss_system_id("psx", "x.cue");
	check(id && !strcmp(id, "999"), "and can correct a built-in that has gone stale");

	check(ss_system_id("bogus", "x.rom") == 0,
		"a non-numeric override is dropped, not passed into a URL");

	ss_systems_forget();
	id = ss_system_id("psx", "x.cue");
	check(id && !strcmp(id, "57"), "forgetting the overrides restores the built-in");

	check(ss_systems_load("/tmp/chome_ss_nothing_here.cfg") == 0,
		"a missing override file is simply no overrides");

	/* --------------------------------------------------------- the URL ---- */

	strcpy(cfg.classicui_ss_user, "dune");
	strcpy(cfg.classicui_ss_pass, "s3cret&pass");

	ss_query q;
	memset(&q, 0, sizeof(q));
	q.systemeid = "57";
	q.romnom = "Destruction Derby (USA).cue";
	q.romtaille = 1234567;
	q.md5 = "d41d8cd98f00b204e9800998ecf8427e";

	char url[1024];
	int n = ss_build_url(&q, 0, url, sizeof(url));
	check(n > 0, "a query with a system and a rom name builds a URL");
	check(strstr(url, "jeuInfos.php") != 0, "it goes to jeuInfos.php");
	check(strstr(url, "output=xml") != 0, "and asks for xml, which is what sxmlc can read");
	check(strstr(url, "romnom=Destruction%20Derby%20%28USA%29.cue") != 0,
		"the rom name is percent-encoded, spaces and brackets included");
	check(strstr(url, "romtaille=1234567") != 0, "the size is sent");
	check(strstr(url, "md5=d41d8cd98f00b204e9800998ecf8427e") != 0, "so is the hash");
	check(strstr(url, "sha1=") == 0, "a hash we do not have is left out entirely");
	check(strstr(url, "ssid=dune") != 0, "the user's own account is sent");

	/*
	  The one that matters more than the rest of this section. Everything that logs
	  or reports a URL has to use the redacted form, because /tmp/debug.txt is
	  world-readable and a password in it is a password published.
	*/
	char red[1024];
	check(ss_build_url(&q, 1, red, sizeof(red)) > 0, "the redacted form builds too");
	check(strstr(url, "s3cret") != 0, "the live URL does carry the password");
	check(strstr(red, "s3cret") == 0, "the redacted one does not");
	check(strstr(red, "testpass") == 0, "nor the dev password");
	check(strstr(red, "sspassword=***") != 0, "it is replaced rather than dropped");
	check(strstr(red, "romnom=Destruction%20Derby%20%28USA%29.cue") != 0,
		"and everything not secret survives redaction, or the log would be useless");

	// Refusals.
	q.systemeid = 0;
	check(ss_build_url(&q, 0, url, sizeof(url)) == 0, "no systemeid, no URL");
	q.systemeid = "57";
	q.romnom = "";
	check(ss_build_url(&q, 0, url, sizeof(url)) == 0, "no rom name, no URL");
	q.romnom = "x.cue";

	char tiny[32];
	check(ss_build_url(&q, 0, tiny, sizeof(tiny)) == 0,
		"a buffer too small refuses rather than sending a truncated request");
	check(tiny[0] == 0, "and leaves nothing behind in it");

	/*
	  The optional parameters are appended only if they fit, so a URL that is long
	  enough to lose its hash still has to be a valid request rather than a
	  half-written one.
	*/
	q.md5 = "d41d8cd98f00b204e9800998ecf8427e";
	q.romtaille = 999;
	char snug[260];
	int m = ss_build_url(&q, 1, snug, sizeof(snug));
	if (m > 0)
	{
		check(strstr(snug, "romnom=x.cue") != 0,
			"when the optional parameters do not fit, the required ones are still intact");
		check((int)strlen(snug) == m, "and the returned length matches the string");
	}
	else
	{
		check(snug[0] == 0, "or it refuses outright and empties the buffer");
		check(1, "(the required parameters alone did not fit this buffer)");
	}

	// A user with no account is off, not degraded: the API has no anonymous tier.
	cfg.classicui_screenscraper = 1;
	cfg.classicui_ss_user[0] = 0;
	check(ss_enabled() == 0, "turned on with no account is still off");
	strcpy(cfg.classicui_ss_user, "dune");
	check(ss_enabled() == 1, "on, with an account, is on");
	cfg.classicui_screenscraper = 0;
	check(ss_enabled() == 0, "and the option itself turns it off again");

	/* ------------------------------------------------------ classifying --- */

	check(ss_http_class(200) == SS_OK, "200 is fine");
	check(ss_http_class(404) == SS_ERR_NOTFOUND, "404 is a game we do not have");
	check(ss_http_class(403) == SS_ERR_CREDENTIALS, "403 is our credentials");
	check(ss_http_class(429) == SS_ERR_THREADS, "429 is too many at once");
	check(ss_http_class(430) == SS_ERR_QUOTA, "430 is the daily quota");
	// 431 used to answer SS_ERR_BLACKLISTED, on the reasoning that the response is the same
	// as a ban. The response still is - SS_ERR_UNMATCHED stands the module down for the
	// session - but the word was wrong: this lifts at midnight and is caused by our match
	// rate, not by our softname. See ss_http_class().
	check(ss_http_class(431) == SS_ERR_UNMATCHED, "431 is too many unmatched roms");
	check(ss_http_class(401) == SS_ERR_CLOSED, "401 is the API shut off under load");
	check(ss_http_class(503) == SS_ERR_TRANSPORT, "a 5xx is transport, not a verdict");
	check(ss_http_class(0) == SS_ERR_TRANSPORT, "and so is no response at all");

	/*
	  Matched on accent-free fragments on purpose: the body is UTF-8 French, and
	  depending on the exact bytes of "trouvée" would be depending on an encoding
	  nobody promised us.
	*/
	check(ss_body_class("Erreur : Rom/Iso/Dossier non trouvée !") == SS_ERR_NOTFOUND,
		"the French not-found sentence is recognised through its accent");
	check(ss_body_class("Votre quota de scrape est ecoule pour aujourd'hui") == SS_ERR_QUOTA,
		"so is the quota sentence");
	check(ss_body_class("API totalement fermé!") == SS_ERR_CLOSED, "and the API-closed one");
	check(ss_body_class("Le logiciel est blacklisté") == SS_ERR_BLACKLISTED, "and a blacklisting");
	check(ss_body_class("Erreur de login : Verifiez vos identifiants") == SS_ERR_CREDENTIALS,
		"and a login failure");
	check(ss_body_class("<Data><jeu id=\"1\"/></Data>") == SS_OK,
		"an ordinary reply is not mistaken for an error");
	check(ss_body_class(0) == SS_OK, "and a null body does not crash the classifier");

	/* ---------------------------------------------------------- parsing --- */

	/*
	  Placement 1: everything in attributes, which is what the JSON shape implies
	  and the most likely form.
	*/
	put_file("/tmp/chome_ss_attr.xml",
		"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
		"<Data>\n"
		"  <ssuser>\n"
		"    <id>dinofly</id>\n"
		"    <maxthreads>1</maxthreads>\n"
		"    <requeststoday>417</requeststoday>\n"
		"    <maxrequestsperday>20000</maxrequestsperday>\n"
		"  </ssuser>\n"
		"  <jeu id=\"4321\">\n"
		"    <noms><nom region=\"wor\">Destruction Derby</nom></noms>\n"
		"    <medias>\n"
		"      <media type=\"ss\" region=\"wor\" format=\"png\" url=\"https://ss/shot.png\"/>\n"
		"      <media type=\"box-3D\" region=\"us\" format=\"png\" url=\"https://ss/3d-us.png\"/>\n"
		"      <media type=\"box-2D\" region=\"jp\" format=\"png\" url=\"https://ss/2d-jp.png\"/>\n"
		"      <media type=\"box-2D\" region=\"eu\" format=\"png\" url=\"https://ss/2d-eu.png\"/>\n"
		"      <media type=\"wheel\" format=\"png\" url=\"https://ss/wheel.png\"/>\n"
		"    </medias>\n"
		"  </jeu>\n"
		"</Data>\n");

	ss_result r;
	check(ss_parse_file("/tmp/chome_ss_attr.xml", &r) == SS_OK, "an attribute-form reply parses");
	check(r.nmedia == 5, "all five media are taken");
	check(!strcmp(r.gameid, "4321"), "the game id comes off the jeu element");
	check(r.requests_today == 417 && r.max_requests_day == 20000,
		"the quota counters are read from the game reply, costing no extra request");
	check(r.max_threads == 1, "and the thread limit with them");

	static const char *const eu_first[] = { "eu", "wor", "us", "jp", 0 };
	static const char *const jp_first[] = { "jp", "wor", "us", "eu", 0 };

	const ss_media *m2 = ss_pick(&r, SS_KIND_COVER, eu_first);
	check(m2 && !strcmp(m2->url, "https://ss/2d-eu.png"),
		"the cover picker honours the region preference");

	m2 = ss_pick(&r, SS_KIND_COVER, jp_first);
	check(m2 && !strcmp(m2->url, "https://ss/2d-jp.png"), "and a different one changes the answer");

	/*
	  Type before region, which is the deliberate difference from Skyscraper: a
	  box-3D is a photograph of a box at an angle and looks wrong in a flat shelf
	  card, so a 2D cover in the wrong region beats a 3D one in the right region.
	*/
	static const char *const us_first[] = { "us", 0 };
	m2 = ss_pick(&r, SS_KIND_COVER, us_first);
	check(m2 && !strcmp(m2->type, "box-2D"),
		"a 2D cover in the wrong region still beats a 3D one in the right region");

	m2 = ss_pick(&r, SS_KIND_SCREEN, eu_first);
	check(m2 && !strcmp(m2->url, "https://ss/shot.png"), "a screenshot is picked by its own type list");

	m2 = ss_pick(&r, SS_KIND_WHEEL, eu_first);
	check(m2 && !strcmp(m2->url, "https://ss/wheel.png"),
		"a media carrying no region at all is used rather than dropped");

	/*
	  Placement 2: child elements and the URL as element text. This is the form we
	  could not confirm, and the reason the parser reads it at all - if the live
	  reply turns out to look like this, nothing has to change.
	*/
	put_file("/tmp/chome_ss_child.xml",
		"<Data><jeu id=\"7\"><medias>"
		"<media><type>box-2D</type><region>us</region><format>png</format>"
		"<url>https://ss/child-2d.png</url></media>"
		"</medias></jeu></Data>\n");

	check(ss_parse_file("/tmp/chome_ss_child.xml", &r) == SS_OK, "a child-element reply parses too");
	check(r.nmedia == 1, "and yields its one media");
	m2 = ss_pick(&r, SS_KIND_COVER, us_first);
	check(m2 && !strcmp(m2->url, "https://ss/child-2d.png"), "with the URL taken from element text");

	// Placement 3: attributes for the metadata, text for the URL.
	put_file("/tmp/chome_ss_mixed.xml",
		"<Data><jeu id=\"8\"><medias>"
		"<media type=\"box-2D\" region=\"us\">https://ss/mixed.png</media>"
		"</medias></jeu></Data>\n");

	check(ss_parse_file("/tmp/chome_ss_mixed.xml", &r) == SS_OK, "so does the mixed form");
	m2 = ss_pick(&r, SS_KIND_COVER, us_first);
	check(m2 && !strcmp(m2->url, "https://ss/mixed.png"), "taking the URL from the text");

	/*
	  A reply that parses and names no game. This has to be distinguishable from a
	  broken reply, because the right response differs: remember the miss, versus
	  try again later.
	*/
	put_file("/tmp/chome_ss_nogame.xml", "<Data><ssuser><id>dinofly</id></ssuser></Data>\n");
	check(ss_parse_file("/tmp/chome_ss_nogame.xml", &r) == SS_ERR_NOTFOUND,
		"a well-formed reply with no game is not-found, not malformed");

	// An error body that is not XML at all, which is a thing this API does.
	put_file("/tmp/chome_ss_err.txt", "Erreur : Rom/Iso/Dossier non trouvée !\n");
	check(ss_parse_file("/tmp/chome_ss_err.txt", &r) == SS_ERR_NOTFOUND,
		"an error body that is not XML is still classified, not just rejected");

	put_file("/tmp/chome_ss_broken.xml", "<Data><jeu id=\"9\"><medias><media type=\"box-2D\"\n");
	int broken = ss_parse_file("/tmp/chome_ss_broken.xml", &r);
	check(broken == SS_ERR_MALFORMED || broken == SS_ERR_NOTFOUND,
		"a truncated reply fails without yielding a media");
	check(ss_pick(&r, SS_KIND_COVER, eu_first) == 0, "and the picker refuses a failed result");

	check(ss_parse_file("/tmp/chome_ss_does_not_exist.xml", &r) == SS_ERR_TRANSPORT,
		"a reply that never landed is transport, not a verdict on the game");

	// A media with no URL, and one whose URL is not a URL, are both useless.
	put_file("/tmp/chome_ss_nourl.xml",
		"<Data><jeu id=\"10\"><medias>"
		"<media type=\"box-2D\" region=\"us\"/>"
		"<media type=\"box-2D\" region=\"eu\" url=\"/etc/passwd\"/>"
		"<media type=\"box-2D\" region=\"jp\" url=\"https://ss/ok.png\"/>"
		"</medias></jeu></Data>\n");
	check(ss_parse_file("/tmp/chome_ss_nourl.xml", &r) == SS_OK, "a reply with unusable media parses");
	check(r.nmedia == 1, "and keeps only the one with a real URL");
	check(!strcmp(r.media[0].url, "https://ss/ok.png"), "which is the http one");

	// Nothing of the kind asked for.
	check(ss_pick(&r, SS_KIND_WHEEL, eu_first) == 0, "no media of that kind is no media");

	/* ------------------------------------------------ the wanted-type filter --- */

	/*
	  The filter, on its own, in the real element shape. Only the types some kind list
	  can ask for are stored; the rest are dropped before the cap is even consulted,
	  which is the whole reason a small store is enough for a 133-media reply.
	*/
	check(ss_type_wanted("box-2D") && ss_type_wanted("mixrbv1") && ss_type_wanted("wheel-hd"),
		"every type the pickers list is wanted");
	check(ss_type_wanted("support-2D"), "and so is the disc scan, which no picker listed before");
	check(ss_type_wanted("SUPPORT-2D"), "matched without regard to case, as the rest of this file is");
	check(!ss_type_wanted("bezel-16-9") && !ss_type_wanted("support-texture") &&
		!ss_type_wanted("figurine") && !ss_type_wanted("pictomonochrome"),
		"and the bezels, textures, figurines and pictos are not");
	check(!ss_type_wanted("") && !ss_type_wanted(0), "an empty or absent type is not wanted either");

	put_file("/tmp/chome_ss_filter.xml",
		"<Data><jeu id=\"12\"><medias>"
		"<media type=\"bezel-16-9\" region=\"wor\" format=\"png\" size=\"1\">"
			FIX_HOST "/bezel.png</media>"
		"<media type=\"support-2D\" region=\"eu\" format=\"png\" size=\"1\">"
			FIX_HOST "/disc.png</media>"
		"<media type=\"figurine\" region=\"jp\" format=\"png\" size=\"1\">"
			FIX_HOST "/figurine.png</media>"
		"<media type=\"box-2D\" region=\"eu\" format=\"png\" size=\"1\">"
			FIX_HOST "/box.png</media>"
		"<media type=\"support-texture\" region=\"eu\" format=\"png\" size=\"1\">"
			FIX_HOST "/texture.png</media>"
		"<media type=\"wheel-hd\" region=\"wor\" format=\"png\" size=\"1\">"
			FIX_HOST "/wheel.png</media>"
		"</medias></jeu></Data>\n");

	check(ss_parse_file("/tmp/chome_ss_filter.xml", &r) == SS_OK, "a mixed reply parses");
	check(r.nmedia == 3, "and only the three wanted types are stored out of six");
	check(media_count_of(&r, "support-2D") == 1 && media_count_of(&r, "box-2D") == 1 &&
		media_count_of(&r, "wheel-hd") == 1, "which are the disc scan, the box and the wheel");
	check(media_count_of(&r, "bezel-16-9") == 0 && media_count_of(&r, "support-texture") == 0,
		"a bezel and a support texture never reach the store at all");

	/*
	  A second entry with the same type and region is unreachable: ss_pick() returns the
	  first match, so storing it would cost 552 bytes to hold something nothing can read.
	*/
	put_file("/tmp/chome_ss_dup.xml",
		"<Data><jeu id=\"13\"><medias>"
		"<media type=\"support-2D\" region=\"eu\" format=\"png\">" FIX_HOST "/first.png</media>"
		"<media type=\"support-2D\" region=\"eu\" format=\"png\">" FIX_HOST "/second.png</media>"
		"<media type=\"support-2D\" region=\"EU\" format=\"png\">" FIX_HOST "/third.png</media>"
		"<media type=\"support-2D\" region=\"jp\" format=\"png\">" FIX_HOST "/jp.png</media>"
		"</medias></jeu></Data>\n");

	check(ss_parse_file("/tmp/chome_ss_dup.xml", &r) == SS_OK, "a reply with repeated regions parses");
	check(r.nmedia == 2, "a repeated type-and-region pair is dropped, whatever its case");
	check(!strcmp(r.media[0].url, FIX_HOST "/first.png"), "and it is the first of the pair that is kept");

	/* --------------------------------------------- the reply that overflowed --- */

	write_measured_reply("/tmp/chome_ss_big.xml");
	check(ss_parse_file("/tmp/chome_ss_big.xml", &r) == SS_OK,
		"the 133-media reply, in the shape a real one has, parses");

	printf("  stored %d of 133 media, cap %d\n", r.nmedia, SS_MAX_MEDIA);
	check(r.nmedia < SS_MAX_MEDIA, "and fits inside the store with room left, rather than filling it");

	{
		int unwanted = 0;
		for (int i = 0; i < r.nmedia; i++) if (!ss_type_wanted(r.media[i].type)) unwanted++;
		check(unwanted == 0, "nothing the pickers cannot ask for was stored");
	}

	/*
	  The headline. support-2D is the 129th media in this reply; under the cap that
	  shipped it was thrown away with 68 others, so the disc dialog could never have had a
	  picture however the fetch was written.
	*/
	check(media_count_of(&r, "support-2D") == 5,
		"the disc scan survives, though it arrives past the old cap of 64");
	check(media_count_of(&r, "mixrbv1") == 4 && media_count_of(&r, "mixrbv2") == 4,
		"and so do the mixrbv fallbacks, which could never once have fired before");

	// Eleven wheels were sent; no type may take more than its share of the store.
	check(media_count_of(&r, "wheel") == SS_MAX_PER_TYPE,
		"a type with more regions than SS_MAX_PER_TYPE keeps that many and no more");

	{
		int pairs = 0;
		for (int i = 0; i < r.nmedia; i++)
		{
			for (int j = i + 1; j < r.nmedia; j++)
			{
				if (!strcasecmp(r.media[i].type, r.media[j].type) &&
					!strcasecmp(r.media[i].region, r.media[j].region)) pairs++;
			}
		}
		check(pairs == 0, "no two stored media share a type and a region");
	}

	/*
	  sxmlc decodes entities in attribute values and deliberately not in text, and the
	  URL is text. Without url_unescape() this URL keeps its "&amp;" and curl reads the
	  whole tail as one parameter value - the devpassword and the player's password
	  silently never sent, and a 401 with nothing in the log to explain it.
	*/
	{
		const ss_media *d = ss_pick(&r, SS_KIND_DISC, 0);
		check(d != 0, "the disc scan can be picked out of the store");
		check(d && strstr(d->url, "&devpassword=") != 0,
			"a URL taken from element text has its entities decoded");
		check(d && strstr(d->url, "&amp;") == 0, "and carries no &amp; into curl");
	}

	/* ------------------------------------------------------ region from disc --- */

	const char *rg;
	rg = ss_region_from_serial("SLES-01506");
	check(rg && !strcmp(rg, "eu"), "SLES is Europe");
	rg = ss_region_from_serial("SCES-00001");
	check(rg && !strcmp(rg, "eu"), "and so is SCES");
	rg = ss_region_from_serial("SLUS-00594");
	check(rg && !strcmp(rg, "us"), "SLUS is the US");
	rg = ss_region_from_serial("SCUS-94900");
	check(rg && !strcmp(rg, "us"), "and so is SCUS");
	rg = ss_region_from_serial("SLPS-00123");
	check(rg && !strcmp(rg, "jp"), "SLPS is Japan");
	rg = ss_region_from_serial("SLPM-86300");
	check(rg && !strcmp(rg, "jp"), "and so are SLPM");
	rg = ss_region_from_serial("SCPS-10001");
	check(rg && !strcmp(rg, "jp"), "and SCPS");

	// The forms this tree passes the same serial around in.
	rg = ss_region_from_serial("SLES_015.06");
	check(rg && !strcmp(rg, "eu"), "the raw on-disc form works too");
	rg = ss_region_from_serial("SLPS 01204");
	check(rg && !strcmp(rg, "jp"), "and the spaced form the title lists use");
	rg = ss_region_from_serial("sles01506");
	check(rg && !strcmp(rg, "eu"), "and it is case-insensitive");

	// A key that is not a PlayStation serial: a Mega CD header id, a volume label, a
	// TOC hash. None of them says anything about a region and none is guessed at.
	check(ss_region_from_serial("MK-4123") == 0, "a Mega CD header id implies no region");
	check(ss_region_from_serial("SONIC_CD") == 0, "nor does a volume label");
	check(ss_region_from_serial("") == 0 && ss_region_from_serial(0) == 0,
		"and an empty or absent key does not crash the lookup");

	{
		const char *regs[4];

		int nr = ss_regions_for_serial("SLES-01506", regs, 4);
		check(nr == 1 && !strcmp(regs[0], "eu") && regs[1] == 0,
			"a European serial yields exactly one preference, terminated");

		const ss_media *d = ss_pick(&r, SS_KIND_DISC, nr ? regs : 0);
		check(d && !strcmp(d->region, "eu"), "and the disc scan picked for it is the European one");

		/*
		  The fallback Dinofly asked for, on the data that motivated it. The real reply
		  carried support media for de, eu, uk, jp and sp - and no us - so an American
		  disc has no scan of its own pressing, and the answer is the first support-2D in
		  reply order rather than nothing.
		*/
		nr = ss_regions_for_serial("SLUS-00594", regs, 4);
		check(nr == 1 && !strcmp(regs[0], "us"), "an American serial asks for us");
		d = ss_pick(&r, SS_KIND_DISC, nr ? regs : 0);
		check(d && !strcmp(d->region, "de"),
			"and with no us scan in the reply it falls back to the first one sent, not to nothing");

		// No region derivable at all: the same fallback, reached the other way.
		nr = ss_regions_for_serial("MK-4123", regs, 4);
		check(nr == 0, "a key with no region in it yields no preference rather than a house default");
		check(regs[0] == 0, "and leaves the list empty rather than half-written");
		d = ss_pick(&r, SS_KIND_DISC, nr ? regs : 0);
		check(d && !strcmp(d->region, "de"), "so it too takes the first scan in reply order");

		// A preference for a region the per-type ceiling clipped behaves the same way.
		static const char *const kr_only[] = { "kr", 0 };
		const ss_media *w = ss_pick(&r, SS_KIND_WHEEL, kr_only);
		check(w && !strcmp(w->region, "wor"),
			"a region dropped by the per-type ceiling falls back to the first of its type");

		const char *one[1];
		check(ss_regions_for_serial("SLES-01506", one, 1) == 0,
			"a list with no room for a terminator is refused rather than overrun");
		check(one[0] == 0, "and is left terminated rather than holding a region it did not report");
	}

	/* --------------------------------------------------- redacting a reply --- */

	/*
	  The measured discovery that made this necessary: every URL the server hands back
	  carries devid, devpassword, ssid and sspassword. ss_build_url() has redacted the
	  request from the start; the reply needed the same discipline, and a media URL in
	  /tmp/debug.txt or in a cache file is the account published.
	*/
	{
		const ss_media *d = ss_pick(&r, SS_KIND_DISC, 0);
		char safe[SS_URL_LEN];

		check(d && ss_redact_url(d->url, safe, sizeof(safe)) > 0, "a reply URL redacts");
		check(strstr(safe, FIX_DEVPASS) == 0, "the dev password is gone");
		check(strstr(safe, FIX_SSPASS) == 0, "the player's password is gone");
		check(strstr(safe, FIX_DEVID) == 0, "so is the devid, which identifies the application");
		check(strstr(safe, FIX_SSID) == 0, "and the player's account name");
		check(strstr(safe, "devpassword=***") != 0, "each one is replaced rather than dropped");
		check(strstr(safe, "sspassword=***") != 0, "including the player's");
		check(strstr(safe, "support-2D-de.png") != 0,
			"and everything not secret survives, or the log line could not be matched to a reply");

		// ssid is a prefix of nothing here, but a value that contains one of the key
		// names must not be mistaken for a key.
		check(ss_redact_url("https://x.invalid/a.png?romnom=my_ssid=notakey&devid=SECRET",
			safe, sizeof(safe)) > 0, "a URL whose value looks like a key redacts");
		check(strstr(safe, "my_ssid=notakey") != 0, "the value that only looks like a key is untouched");
		check(strstr(safe, "SECRET") == 0, "while the real key beside it is not");

		check(ss_redact_url("https://x.invalid/a.png?type=box-2D", safe, sizeof(safe)) > 0,
			"a URL with no credentials in it redacts to itself");
		check(!strcmp(safe, "https://x.invalid/a.png?type=box-2D"), "unchanged");

		char snug[24];
		check(ss_redact_url(d ? d->url : "", snug, sizeof(snug)) == 0,
			"a buffer too small refuses rather than truncate");
		check(snug[0] == 0, "and empties itself, since the half that survived could be the secret half");
		check(ss_redact_url(0, safe, sizeof(safe)) == 0, "a null URL redacts to nothing");
	}

	cfg.classicui_ss_user[0] = 0;
	cfg.classicui_ss_pass[0] = 0;
}

/* ------------------------------------------------------------- disc art --- */

/*
  What can honestly be tested, and what cannot, for the disc scan.

  Can: where the file goes, that a request refuses in every configuration this tree
  ships, and - the part that took the longest to get right - that scaling one down does
  not dirty its edges.

  Cannot: the fetch. Two curls, one for the reply and one for the picture, and this suite
  makes no network request and must not: the account is limited to one thread and the
  fixture URLs point at .invalid precisely so that a bug cannot turn into a request. So
  every call below is made with the fetch disabled and asserted to have started nothing,
  and disc_art_scale() is driven directly on a generated file instead.

  The darkening check is the interesting one. A plain RGBA box average of one of these
  scans looks fine in a thumbnail and wrong on a TV: the transparent pixels are RGB
  (0,0,0), so every pixel straddling the rim or the hub gets those zeros mixed into its
  colour in proportion to how much of it is transparent. On a flat-coloured fixture that
  is exactly measurable - a half-covered rim pixel comes out at half the disc's colour -
  and the assertion below is that it does not.
*/
static void assert_disc_art()
{
	printf("\n== the disc scan: where it goes, and scaling it without dirtying it ==\n");

	/* ------------------------------------------------------------ the path --- */

	char p[1024];
	check(disc_art_path("SLES-01506", p, sizeof(p)) == 1, "a disc identity has a path for its scan");
	check(strstr(p, "/classicui/discart/SLES-01506.png") != 0,
		"under classicui/discart, named by the identity that names its savestates");
	check(disc_art_path("", p, sizeof(p)) == 0, "an empty identity has none");
	check(disc_art_path(0, p, sizeof(p)) == 0, "nor does an absent one");

	// A volume label can be a disc's identity, and a volume label is free text.
	check(disc_art_path("SONIC/CD", p, sizeof(p)) == 1 && strstr(p, "SONIC_CD.png") != 0,
		"a slash in an identity is sanitised rather than making a directory");

	/* ----------------------------------------------------- the refusals ----- */

	/*
	  Every configuration this tree ships, and the one the harness runs in. ss_available()
	  is compile-time false in a shipped build, so nothing here can reach the network
	  there; here the devid is a dummy, and cfg.classicui_artfetch is what holds the line.
	*/
	cfg.classicui_artfetch = 0;
	cfg.classicui_screenscraper = 1;
	strcpy(cfg.classicui_ss_user, "dune");

	check(disc_art_request("SLES-01506", "psx", 0) == 0, "with the fetch off, nothing is asked for");
	check(disc_art_active() == 0, "and no download was started");

	cfg.classicui_artfetch = 1;
	cfg.classicui_screenscraper = 0;
	check(disc_art_request("SLES-01506", "psx", 0) == 0,
		"with ScreenScraper off, nothing is asked for either");
	check(disc_art_active() == 0, "and still no download");

	cfg.classicui_screenscraper = 1;
	cfg.classicui_ss_user[0] = 0;
	check(disc_art_request("SLES-01506", "psx", 0) == 0, "nor with no account to ask under");
	check(disc_art_active() == 0, "and still none");

	strcpy(cfg.classicui_ss_user, "dune");
	check(disc_art_request("SLES-01506", "doesnotexist", 0) == 0,
		"nor for a system with no systemeid at all");
	check(disc_art_active() == 0, "and still none");

	check(disc_art_request("", "psx", 0) == 0, "an empty identity is refused");
	check(disc_art_request(0, "psx", 0) == 0, "and an absent one");
	check(disc_art_active() == 0, "none of the refusals forked anything");

	// Back to the state the rest of the suite expects. Left set, the next section that
	// scrolls a shelf would start downloading covers.
	cfg.classicui_artfetch = 0;
	cfg.classicui_screenscraper = 0;
	cfg.classicui_ss_user[0] = 0;

	/* -------------------------------------------------------- the scaling --- */

	const char *src = "/tmp/chome_disc_scan.png";
	make_disc_scan(src);

	int sw = 0, sh = 0;
	png_pixel(src, 0, 0, &sw, &sh);
	check(sw == DISC_FIX_PX && sh == DISC_FIX_PX, "the fixture scan is the measured 600x600");

	char dst[1024];
	check(disc_art_path("SLES-01506", dst, sizeof(dst)) == 1, "and it has somewhere to go");
	unlink(dst);

	check(disc_art_scale(src, dst) == 1, "a scan scales down and is stored");

	int w = 0, h = 0;
	png_pixel(dst, 0, 0, &w, &h);
	printf("  scaled %dx%d -> %dx%d\n", sw, sh, w, h);
	check(w == DISC_ART_PX && h == DISC_ART_PX,
		"at DISC_ART_PX, which is where the title round the disc becomes legible");

	// The 417 KB original is what this is for. The sprite has to be a fraction of it.
	{
		struct stat a, b;
		int have = (!stat(src, &a) && !stat(dst, &b));
		printf("  fixture %lld bytes, sprite %lld bytes\n",
			have ? (long long)a.st_size : -1, have ? (long long)b.st_size : -1);
		check(have && b.st_size < a.st_size / 2,
			"and much smaller than the source, which is the whole point of scaling at fetch time");
	}

	/*
	  Transparency has to survive the write. imlib2 will happily save a PNG with the alpha
	  channel flattened, and the result on the dialog is a black square with a disc printed
	  on it rather than a disc.
	*/
	check((png_pixel(dst, 0, 0, 0, 0) >> 24) == 0, "the corner outside the disc is transparent");
	check((png_pixel(dst, w - 1, h - 1, 0, 0) >> 24) == 0, "and so is the opposite corner");
	check((png_pixel(dst, w / 2, h / 2, 0, 0) >> 24) == 0,
		"and the hub hole in the middle, which is transparent in the source out to 15% of the radius");

	// The disc body itself, well inside the rim, must be exactly the colour it was.
	{
		uint32_t body = png_pixel(dst, w / 2, h / 4, 0, 0);
		check((body >> 24) == 0xff, "the disc body is opaque");
		check((body & 0xffffffu) == DISC_FIX_RGB, "and exactly the colour it was drawn in");
	}

	/*
	  The measurement this section exists for.

	  Along the middle row, every pixel with partial coverage is a rim pixel. Premultiplied,
	  its colour is the disc's colour whatever its alpha, because the only thing contributing
	  colour is the disc. Averaged naively, its colour is the disc's scaled by its coverage -
	  so a pixel at alpha 128 would read (112,32,8) instead of (224,64,16), and the sprite
	  gets the grey rim Dinofly saw.
	*/
	{
		int partial = 0, darkened = 0, worst = 0, half_covered = 0;
		unsigned lowest = 255;
		int mid = h / 2;

		for (int x = 0; x < w; x++)
		{
			uint32_t v = png_pixel(dst, x, mid, 0, 0);
			unsigned a = (v >> 24) & 0xff;
			if (!a || a == 0xff) continue;

			partial++;
			if (a < lowest) lowest = a;

			// Substantially partial, so that the check below can discriminate. A row whose
			// only partial pixels were alpha 254 would pass under a naive average too -
			// the error there is 1/255 - and would be a test that had quietly stopped
			// testing anything.
			if (a >= 32 && a <= 223) half_covered++;

			int dr = (int)((v >> 16) & 0xff) - (int)((DISC_FIX_RGB >> 16) & 0xff);
			int dg = (int)((v >> 8) & 0xff)  - (int)((DISC_FIX_RGB >> 8) & 0xff);
			int db = (int)(v & 0xff)         - (int)(DISC_FIX_RGB & 0xff);
			if (dr < 0) dr = -dr;
			if (dg < 0) dg = -dg;
			if (db < 0) db = -db;

			int err = dr > dg ? dr : dg;
			if (db > err) err = db;
			if (err > worst) worst = err;
			if (err > 6) darkened++;
		}

		printf("  %d partly-covered pixels on the middle row, %d of them substantially so"
			" (lowest alpha %u), worst colour error %d/255\n",
			partial, half_covered, lowest, worst);
		check(partial > 0, "the rim really does produce partly-covered pixels to check");
		check(half_covered > 0,
			"at least one of them is covered enough for a naive average to be visibly wrong");
		check(darkened == 0,
			"and not one of them was dragged toward black: the average is premultiplied by alpha");
	}

	/* ---------------------------------------------- and once it is on the card --- */

	/*
	  A scan already on the card is an answer, not a reason to fetch. Checked with the
	  fetch turned back on, because this is the branch that has to return before any of
	  the gates below it are reached.
	*/
	cfg.classicui_artfetch = 1;
	cfg.classicui_screenscraper = 1;
	strcpy(cfg.classicui_ss_user, "dune");

	check(disc_art_request("SLES-01506", "psx", 0) == 1,
		"a scan already on the card needs no fetch and says so");
	check(disc_art_active() == 0, "and nothing was downloaded to find that out");

	cfg.classicui_artfetch = 0;
	cfg.classicui_screenscraper = 0;
	cfg.classicui_ss_user[0] = 0;

	// The dialog reads it back through art_thumb(), which is the seam that was already
	// committed. It has to be able to decode what was written.
	{
		const uint32_t *px = art_thumb(dst, 96, 96);
		check(px != 0, "and the dialog can decode it through art_thumb()");
	}

	check(disc_art_scale("/tmp/chome_no_such_scan.png", dst) == 0,
		"scaling a file that is not there fails rather than writing a broken sprite");
	check(disc_art_scale(0, 0) == 0, "and null paths are refused");

	unlink(dst);
	unlink(src);
}

/* --------------------------------------------------------- the art ladder --- */

/*
  ScreenScraper first, the libretro pack second, and the card above both of them - and,
  above everything else in this section, the two refusals kept apart.

  What cannot be tested here: a request. Not one is made by this suite and not one may be.
  Dinofly's account allows a single thread, so a stray fetch is not a slow test but a request
  spent against a real quota under a real devid. cfg.classicui_artfetch is 0 for the rest of
  the run, the fixture URLs point at .invalid, and the block below that turns the fetch on
  calls nothing but art_next_source() and art_ss_settle() - both pure, one stats the card
  and the other reads a file - and draws no frame at all, because art_step() with the fetch
  enabled and ScreenScraper on is the one combination in this suite that could reach a
  network. It is off again before anything else runs, and the last checks here are that
  nothing was forked.

  What can: the order, and the memory.

  The order is art_next_source(), which is the same function art_step() walks rather than a
  description of it. That matters more than it looks: a ladder reordered by accident leaves
  no trace in a pixel - a game with a cover on the card looks identical whether we used that
  cover or fetched a new one over it - so a test that could only see the end state would
  pass either way.

  The memory is art_ss_settle(), the live reply-handling path driven on a file instead of on
  a download, exactly as disc_art_scale() is driven on a generated PNG rather than a fetched
  one.

  The crux is the last three blocks, and they are asserted on different games side by side
  because either half alone passes under the bug. A client that remembers every refusal
  passes "a miss is remembered"; a client that remembers none passes "a quota is not". Only
  both together say the two are told apart - so a game the database has no cover for and a
  game passed over while the quota was gone are checked against each other, after the hold
  is lifted, in one pair of assertions.

  The fixtures are hand-written from the documented shape and say so, because a fixture that
  looks captured but was invented is the worst kind of test evidence. The one exception is
  the ssuser block, which is copied from the shape of the reply Dinofly read on 2026-08-05.
*/

// The ssuser block, which rides along with every reply and is where the quota is read from
// for free. `today` and `limit` are what the two crux blocks below vary.
static void put_ssuser_reply(const char *path, const char *medias, int today, int limit)
{
	char xml[2048];
	snprintf(xml, sizeof(xml),
		"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
		"<Data>\n"
		"  <ssuser><id>" FIX_SSID "</id><maxthreads>1</maxthreads>"
		"<requeststoday>%d</requeststoday><maxrequestsperday>%d</maxrequestsperday>"
		"<requestskotoday>3</requestskotoday><maxrequestskoperday>4000</maxrequestskoperday>"
		"</ssuser>\n"
		"  <jeu id=\"991\">\n"
		"    <noms><nom region=\"wor\">Ladder Fixture</nom></noms>\n"
		"    <medias>\n%s    </medias>\n"
		"  </jeu>\n"
		"</Data>\n", today, limit, medias);

	put_file(path, xml);
}

static void assert_art_ladder()
{
	printf("\n== the art ladder: ScreenScraper first, and the two refusals told apart ==\n");

	int metroid = item_by_path("SNES", "Super Metroid (Europe).sfc");
	int smwjp   = item_by_path("SNES", "Super Mario World (Japan).sfc");
	int ddragon = item_by_path("Genesis", "Double Dragon (Europe).bin");
	int bonk    = item_by_path("TGFX16", "Bonk's Adventure (USA).pce");
	int fusion  = item_by_path("GBA", "Metroid Fusion (Europe).gba");
	int npc     = item_by_path("NGP", "SNK vs Capcom (USA).npc");

	check(metroid >= 0 && smwjp >= 0 && ddragon >= 0 && bonk >= 0 && fusion >= 0 && npc >= 0,
		"the six games this section needs are all in the index");

	/*
	  Where they start from, which is what assert_gamelist() left behind: one with a cover
	  off the card and the rest with none at all. Asserted rather than assumed, because
	  every ordering check below is only worth as much as this is.

	  "Not READY" rather than "MISSING", deliberately. MISSING means the ladder was walked
	  for that game and ran out of rungs, and a game may honestly not have got that far: the
	  decode queue holds QUEUE_MAX entries and art_request() will not displace an equal
	  priority, so on a card with more games than that the ones nothing has drawn are still
	  ART_NONE. Either state is the precondition this section wants, which is that no cover
	  has been found - and the rung checks below prove the stronger thing, that find_local_art()
	  answers nothing for them.
	*/
	check(art_state(metroid) == ART_READY, "Super Metroid has a cover on the card already");
	check(art_state(smwjp) != ART_READY && art_state(ddragon) != ART_READY &&
		art_state(bonk) != ART_READY && art_state(fusion) != ART_READY &&
		art_state(npc) != ART_READY, "and the other five have no cover anywhere on it");

	ss_forget_state();
	check(ss_hold_reason() == SS_OK, "and nothing is holding ScreenScraper off yet");
	check(art_ss_asks() == 0, "nor has anything asked it for a cover in this whole run");

	/* ---------------------------------------------- off, and therefore inert --- */

	cfg.classicui_artfetch = 1;
	cfg.classicui_screenscraper = 0;
	cfg.classicui_ss_user[0] = 0;

	check(ss_enabled() == 0, "with the option off, ScreenScraper is not enabled");
	check(art_next_source(ddragon) == ART_SRC_LIBRETRO,
		"so a coverless game goes straight to the pack, as it always did");

	cfg.classicui_screenscraper = 1;
	check(ss_enabled() == 0, "turned on with no account is still off - the API has no anonymous tier");
	check(art_next_source(ddragon) == ART_SRC_LIBRETRO, "and the pack is still what answers");

	/* --------------------------------------------- on, and therefore first --- */

	strcpy(cfg.classicui_ss_user, FIX_SSID);
	strcpy(cfg.classicui_ss_pass, FIX_SSPASS);
	check(ss_enabled() == 1, "on, with an account, is on");

	check(art_next_source(ddragon) == ART_SRC_SS,
		"and now a coverless game is asked of ScreenScraper before the pack");

	/*
	  The other half of Dinofly's instruction, and the half his words did not say outright.
	  "Use it for all arts as first priority" taken literally would re-fetch over a cover
	  that is already on the card - including one the player scraped themselves and pointed
	  a gamelist.xml at, which find_local_art() deliberately honours above everything else.
	  So the card stays above both network sources, and this is the check that says so.
	*/
	check(art_next_source(metroid) == ART_SRC_LOCAL,
		"while a game whose cover is already on the card is not re-fetched over");

	/*
	  A game with no systemeid cannot be asked at all, and must fall to the pack rather
	  than becoming a rung that is offered and then quietly refuses. Every system on the
	  shelf has a systemeid now (see chome_ss.cpp's builtin[]), so the only game left that
	  can produce this is a .npc in the Neo Geo Pocket shelf - the one extension neither
	  reference client that table was checked against can name with confidence.
	*/
	check(art_next_source(npc) == ART_SRC_LIBRETRO,
		"a game on a system with no systemeid skips ScreenScraper and uses the pack");

	/* ------------------------------------------- a reply that names a cover --- */

	put_ssuser_reply("/tmp/chome_ladder_ok.xml",
		"      <media type=\"box-2D\" region=\"eu\" format=\"png\" size=\"1\">"
		FIX_HOST "/box-2D-eu.png</media>\n"
		"      <media type=\"support-2D\" region=\"eu\" format=\"png\" size=\"1\">"
		FIX_HOST "/support-2D-eu.png</media>\n", 12, 20000);

	check(art_ss_settle(fusion, "/tmp/chome_ladder_ok.xml") == 1,
		"a reply naming a box-2D yields a cover to fetch");
	check(art_ss_absent(fusion) == 0, "and nothing is written off about that game");

	/*
	  The scar, guarded where it can bite again. Every successful reply carries <maxthreads>
	  in its ssuser block, and classifying replies by searching the body for "threads" once
	  marked every good game reply as a thread-limit error and threw the game away. A hold
	  raised here would be that bug returning.
	*/
	check(ss_hold_reason() == SS_OK,
		"and a successful reply does not read as a thread error, though ssuser carries maxthreads");
	check(ss_may_request() == 1, "so the module is still free to ask about the next game");

	/* --------------------------------------- a genuine absence, both shapes --- */

	/*
	  Shape one: the database has never heard of the game. A well-formed reply naming no
	  <jeu> at all, which ss_parse_file() reports as SS_ERR_NOTFOUND rather than as a broken
	  reply - the distinction exists precisely so this can be remembered.
	*/
	put_file("/tmp/chome_ladder_nogame.xml",
		"<Data><ssuser><id>" FIX_SSID "</id><maxthreads>1</maxthreads>"
		"<requeststoday>13</requeststoday><maxrequestsperday>20000</maxrequestsperday>"
		"</ssuser></Data>\n");

	check(art_ss_settle(ddragon, "/tmp/chome_ladder_nogame.xml") == 0,
		"a reply naming no game at all yields no cover");
	check(art_ss_absent(ddragon) == 1, "and that game is remembered as having none");
	check(ss_hold_reason() == SS_OK, "while the module itself is not held off by it");
	check(art_next_source(ddragon) == ART_SRC_LIBRETRO,
		"so the ladder drops that game to the pack, which is the fallback working");

	/*
	  Shape two: the game is in the database with pictures, but no cover among them. No
	  amount of asking again fills that gap either, so it is the same answer.
	*/
	put_ssuser_reply("/tmp/chome_ladder_nocover.xml",
		"      <media type=\"support-2D\" region=\"eu\" format=\"png\" size=\"1\">"
		FIX_HOST "/support-2D-eu.png</media>\n"
		"      <media type=\"wheel\" region=\"wor\" format=\"png\" size=\"1\">"
		FIX_HOST "/wheel-wor.png</media>\n", 14, 20000);

	check(art_ss_settle(bonk, "/tmp/chome_ladder_nocover.xml") == 0,
		"a reply with media but no cover type yields no cover either");
	check(art_ss_absent(bonk) == 1, "and is remembered the same way");
	check(art_next_source(bonk) == ART_SRC_LIBRETRO, "and falls back the same way");

	/* ------------------------------- THE CRUX: a quota is not an absence --- */

	/*
	  The refusal that must not be remembered. One bad afternoon at the wrong end of the
	  allowance would otherwise mark every card the shelf touched as having no art -
	  permanently, silently, and with no route back that a player would think of.

	  Driven through the real path: the French sentence the API answers with, in a body that
	  is not XML at all, which is a thing this API does.
	*/
	put_file("/tmp/chome_ladder_quota.txt",
		"Erreur : Votre quota de scrape est ecoule pour aujourd'hui !\n");

	check(art_next_source(smwjp) == ART_SRC_SS, "a game not yet asked about is on the ScreenScraper rung");
	check(art_ss_settle(smwjp, "/tmp/chome_ladder_quota.txt") == 0, "the quota refusal yields no cover");

	check(art_ss_absent(smwjp) == 0,
		"and - the crux - the game is NOT remembered as having no art");
	check(ss_hold_reason() == SS_ERR_QUOTA, "the module is what remembers, and it says quota");
	check(ss_may_request() == 0, "so nothing asks again while the hold stands");
	check(ss_verdict(SS_ERR_QUOTA) == 0, "because a quota is not a verdict about any game");

	// For now it uses the pack, which is what "fall back to libretro" means in the moment.
	check(art_next_source(smwjp) == ART_SRC_LIBRETRO,
		"so for now that game falls back to the pack rather than waiting");

	/*
	  And the two halves against each other, which is the only pair of assertions that
	  actually distinguishes the behaviour. The hold goes - a new session, a core change,
	  tomorrow - and the game refused over quota is asked again while the game the database
	  genuinely had nothing for is not.
	*/
	ss_forget_state();
	check(ss_hold_reason() == SS_OK && ss_may_request() == 1, "the hold lifts");
	check(art_next_source(smwjp) == ART_SRC_SS,
		"the game refused over quota is asked about again");
	check(art_next_source(ddragon) == ART_SRC_LIBRETRO,
		"while the game it genuinely has no cover for is not asked again");
	check(art_next_source(bonk) == ART_SRC_LIBRETRO, "and nor is the one with no cover among its media");

	/* ------------------------------ THE CRUX: a rate limit is not one either --- */

	/*
	  HTTP 429, which is the documented rate limit and the one refusal there is no other
	  evidence for. It has no body match on purpose - see ss_body_class() and the scar above
	  - so it arrives as a status, and on the device curl_spawn() captures that status
	  precisely so this classification can happen at all.
	*/
	check(ss_http_class(429) == SS_ERR_THREADS, "429 is the thread limit");
	check(ss_verdict(SS_ERR_THREADS) == 0, "which is not a verdict about a game");

	check(art_next_source(fusion) == ART_SRC_SS, "a game with no verdict against it is on the rung");
	ss_note_result(SS_ERR_THREADS, 0);

	check(art_ss_absent(fusion) == 0, "a rate limit writes off no game");
	check(ss_hold_reason() == SS_ERR_THREADS, "it holds the module instead");
	check(art_next_source(fusion) == ART_SRC_LIBRETRO, "which falls that game back to the pack for now");

	ss_forget_state();
	check(art_next_source(fusion) == ART_SRC_SS, "and it is asked about again once the limit is not the reason");

	/*
	  The same for a dropped connection, which is the third way of not answering. Told apart
	  from the two above in one respect only: it does not hold the module, because a single
	  failed request is not the account refusing us and holding on it would cost a session
	  its covers over one lost packet.
	*/
	ss_note_result(SS_ERR_TRANSPORT, 0);
	check(ss_hold_reason() == SS_OK, "a dropped request does not stand the module down");
	check(ss_verdict(SS_ERR_TRANSPORT) == 0, "and writes off no game either");
	check(art_next_source(fusion) == ART_SRC_SS, "so that game is still on the rung");

	/* -------------------------------- the quota that costs no extra request --- */

	/*
	  The counters ride along with the game data, so the last reply the allowance covers
	  says so itself. Standing down on that means the next request is never made, rather
	  than being made, refused, and counted against us.

	  This reply is a *success* carrying a spent counter, which is the case that only works
	  if the counters are read from every reply rather than from refusals.
	*/
	put_ssuser_reply("/tmp/chome_ladder_spent.xml",
		"      <media type=\"box-2D\" region=\"eu\" format=\"png\" size=\"1\">"
		FIX_HOST "/box-2D-eu.png</media>\n", 20000, 20000);

	check(art_ss_settle(fusion, "/tmp/chome_ladder_spent.xml") == 1,
		"a successful reply still names its cover");
	check(art_ss_absent(fusion) == 0, "and writes off nothing");
	check(ss_hold_reason() == SS_ERR_QUOTA,
		"but its own counters say the allowance is spent, so the module stands down");
	check(ss_may_request() == 0, "without a request having been spent to discover it");

	/* ------------------------------------------------------------- and out --- */

	/*
	  Nothing was fetched, which is the assertion this whole section is written around. Both
	  predicates, because they answer for different things now: art_fetch_active() is the
	  libretro pack fetch and disc_art_active() is a ScreenScraper fetch of the disc kind.
	*/
	check(art_fetch_active() == 0, "no pack fetch was started anywhere in this section");
	check(disc_art_active() == 0, "and no ScreenScraper download either");
	check(art_ss_asks() == 0,
		"and the ScreenScraper rung was never actually acted on - every reply above was a fixture");

	ss_forget_state();
	cfg.classicui_artfetch = 0;
	cfg.classicui_screenscraper = 0;
	cfg.classicui_ss_user[0] = 0;
	cfg.classicui_ss_pass[0] = 0;

	unlink("/tmp/chome_ladder_ok.xml");
	unlink("/tmp/chome_ladder_nogame.xml");
	unlink("/tmp/chome_ladder_nocover.xml");
	unlink("/tmp/chome_ladder_quota.txt");
	unlink("/tmp/chome_ladder_spent.xml");

	/*
	  And the slots back the way assert_gamelist() left them, absent flags and all. Every
	  section after this one runs with the fetch off, so none of them could be misled by a
	  stale flag - but a section that leaves state behind for the next one to trip over is
	  how this suite would stop being trustworthy.
	*/
	art_redo();
	check(art_state(metroid) == ART_READY, "and the shelf is decoded again for whatever comes next");
	check(art_ss_absent(ddragon) == 0, "with nothing remembered from the fixtures above");

	/*
	  And the card store the fixtures above wrote into, which the section below owns and
	  starts from empty. This is not tidiness for its own sake: the two settles above are
	  genuine misses and are now genuinely written down, so leaving the file behind would
	  make the next section's "nothing is remembered yet" a lie that happened to pass.
	*/
	unlink(art_ss_miss_path());
	art_ss_miss_reload();
}

/*
  ==========================================================================
  Not burning somebody's ScreenScraper allowance, and not getting them banned.
  ==========================================================================

  What this is about, because none of it is visible from the code it guards.

  On 2026-08-05 one request from the owner's machine came back with

      Faite du tri dans vos fichiers roms et repassez demain !

  which is not "no such game" - it is the server refusing the account for the rest of the
  day because too many of the day's searches had matched nothing. His counters that
  morning read "200 of 20000 requests today, 60 of 2000 unmatched": the ordinary budget
  was barely touched, and the unmatched one is the one this front-end spends.

  It was spending it in a loop. art_ss_absent() is a field on an in-memory slot, and this
  device restarts the firmware on every core change, so every boot asked the database
  about every coverless game again - 1469 of them on his card, through a matcher that
  misses often. The answer never changed and was never written down.

  Four things come out of that, and this section is the four of them:

    a miss is remembered on the card, for a week, and survives a restart
    the throttle sentence is classified as a refusal and NEVER as a per-game miss
    the shelf stands down before the unmatched ceiling, leaving the last of it for the
      disc dialog, which is the one a player is actually watching
    requests have a floor on how often they leave the box

  The load-bearing pair is the first two together, and they are checked against each
  other for the same reason the crux in the section above is: a client that remembers
  every outcome passes "a miss is remembered", and a client that remembers none passes "a
  throttle is not a miss". Only both at once say the two are told apart - and here the
  cost of getting it backwards is a week rather than a session, because the wrong answer
  is now on the card.

  Every reply below is a fixture. Nothing here contacts ScreenScraper; the account was in
  the penalty box when this was written, and a probe would have made it worse.
*/

// The ssuser block again, with the two ko counters exposed - which put_ssuser_reply()
// above pins at 3 of 4000 because nothing up there varies them. Everything about the
// reserve is a question about those two numbers.
static void put_ss_ko_reply(const char *path, int ko_today, int ko_max)
{
	char xml[1024];
	snprintf(xml, sizeof(xml),
		"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
		"<Data>\n"
		"  <ssuser><id>" FIX_SSID "</id><maxthreads>1</maxthreads>"
		"<requeststoday>200</requeststoday><maxrequestsperday>20000</maxrequestsperday>"
		"<requestskotoday>%d</requestskotoday><maxrequestskoperday>%d</maxrequestskoperday>"
		"</ssuser>\n"
		"  <jeu id=\"991\">\n"
		"    <noms><nom region=\"wor\">Reserve Fixture</nom></noms>\n"
		"    <medias>\n"
		"      <media type=\"box-2D\" region=\"eu\" format=\"png\" size=\"1\">"
		FIX_HOST "/box-2D-eu.png</media>\n"
		"    </medias>\n"
		"  </jeu>\n"
		"</Data>\n", ko_today, ko_max);

	put_file(path, xml);
}

/*
  Move every entry in the miss store `days` further into the past, by rewriting the file.

  The window is measured in days and there is no clock to wind forward, so the only honest
  way to test an expiry is to age the evidence rather than the observer. It edits the day
  field and nothing else - the key each record is matched on is untouched - so what comes
  back is the same set of misses, recorded earlier.

  Returns how many record lines it re-dated.
*/
static int miss_backdate(int days)
{
	FILE *in = fopen(art_ss_miss_path(), "rt");
	if (!in) return -1;

	char out[64 * 1024];
	int pos = 0, n = 0;
	char line[256];

	while (fgets(line, sizeof(line), in))
	{
		unsigned long day = 0;
		int used = 0;

		if (line[0] == '#' || sscanf(line, "%lu %n", &day, &used) < 1 || !used)
		{
			pos += snprintf(out + pos, sizeof(out) - (size_t)pos, "%s", line);
			continue;
		}

		unsigned long aged = (day > (unsigned long)days) ? day - (unsigned long)days : 0;
		pos += snprintf(out + pos, sizeof(out) - (size_t)pos, "%lu %s", aged, line + used);
		n++;
	}

	fclose(in);
	put_file(art_ss_miss_path(), out);
	return n;
}

// Record lines in the store file, ignoring the comment header. The bound is a property of
// the file as well as of the array, so both are counted.
static int miss_file_records()
{
	FILE *f = fopen(art_ss_miss_path(), "rt");
	if (!f) return -1;

	int n = 0;
	char line[256];
	while (fgets(line, sizeof(line), f)) if (line[0] != '#' && line[0] != '\n') n++;

	fclose(f);
	return n;
}

static void assert_ss_throttle()
{
	printf("\n== ScreenScraper: a miss remembered for a week, and a throttle honoured ==\n");

	int ddragon = item_by_path("Genesis", "Double Dragon (Europe).bin");
	int smwjp   = item_by_path("SNES", "Super Mario World (Japan).sfc");
	int fusion  = item_by_path("GBA", "Metroid Fusion (Europe).gba");
	check(ddragon >= 0 && smwjp >= 0 && fusion >= 0, "the three games this section needs are indexed");

	// What the shelf would actually ask about that game: the API's platform id and the ROM
	// file name. Taken from the same tables the fetcher reads rather than written out here,
	// so a test that passed against a hand-typed key while the fetcher used a different one
	// is not a thing that can happen.
	chome_item *dd = lib_item(ddragon);
	const chome_sys *ddsys = lib_sys(dd->sysidx);
	const char *dd_sys = ss_system_id(ddsys->id, "Double Dragon (Europe).bin");
	check(dd_sys != 0, "and the Mega Drive has a systemeid, so its games can be asked about at all");

	/* -------------------------------------------------- a clean card, first --- */

	unlink(art_ss_miss_path());
	art_ss_miss_reload();
	check(art_ss_miss_count() == 0, "the miss store starts empty");

	cfg.classicui_artfetch = 1;
	cfg.classicui_screenscraper = 1;
	strcpy(cfg.classicui_ss_user, FIX_SSID);
	strcpy(cfg.classicui_ss_pass, FIX_SSPASS);
	ss_forget_state();

	check(ss_enabled() == 1, "with the option on and an account, ScreenScraper is enabled");
	check(art_next_source(ddragon) == ART_SRC_SS,
		"and a coverless game with nothing remembered against it is on the ScreenScraper rung");

	/* --------------------------------- a miss, remembered across a restart --- */

	/*
	  The defect, in one block. A well-formed reply naming no game is the database saying
	  it has never heard of this one, which is the answer it will give tomorrow as well.
	*/
	put_file("/tmp/chome_miss_nogame.xml",
		"<Data><ssuser><id>" FIX_SSID "</id><maxthreads>1</maxthreads>"
		"<requeststoday>201</requeststoday><maxrequestsperday>20000</maxrequestsperday>"
		"<requestskotoday>61</requestskotoday><maxrequestskoperday>2000</maxrequestskoperday>"
		"</ssuser></Data>\n");

	check(art_ss_settle(ddragon, "/tmp/chome_miss_nogame.xml") == 0, "a reply naming no game yields no cover");
	check(art_ss_absent(ddragon) == 1, "the slot remembers it, as it always did");
	check(art_ss_miss_count() == 1, "and now the card remembers it too");
	check(art_ss_miss_known(dd_sys, "Double Dragon (Europe).bin") == 1,
		"under the query that was actually asked, not under the shelf's index");
	check(miss_file_records() == 1, "with one record written to the file");

	/*
	  And the restart, which is the whole point. A core change kills this firmware and the
	  next one starts with empty slots and a re-read of the card - so the slot flag is gone
	  and only the file can answer.

	  art_redo() with the fetch off first: it walks the ladder for every item, and walking
	  it with ScreenScraper enabled would fork a curl at a live API. Nothing in this suite
	  is allowed to do that.
	*/
	cfg.classicui_artfetch = 0;
	art_redo();
	cfg.classicui_artfetch = 1;
	ss_forget_state();
	art_ss_miss_reload();

	check(art_ss_absent(ddragon) == 0, "after a restart the slot has forgotten, as it must");
	check(art_ss_miss_count() == 1, "but the card has not");
	check(art_next_source(ddragon) == ART_SRC_LIBRETRO,
		"so the game is NOT asked about again - which is the bug this whole section is about");
	check(art_next_source(smwjp) == ART_SRC_SS,
		"while a game nothing was ever learnt about is still on the rung");

	/* ------------------------------------------------ and it expires, later --- */

	check(miss_backdate(ART_SS_MISS_DAYS - 1) == 1, "age that record to one day inside the window");
	art_ss_miss_reload();
	check(art_ss_miss_count() == 1, "a miss inside the window is still remembered");
	check(art_next_source(ddragon) == ART_SRC_LIBRETRO, "and the game is still not asked about");

	check(miss_backdate(1) == 1, "age it one more day, to the edge of the window");
	art_ss_miss_reload();
	check(art_ss_miss_count() == 0, "and it stops counting as a miss");
	check(art_next_source(ddragon) == ART_SRC_SS,
		"so the game is asked about again - a database people add to is worth re-asking");

	/*
	  And the record itself is still there, which is the half that is new.

	  It says nothing about the game any more - the line above proves the rung is back - and
	  it is kept for one purpose only: telling "asked about once, a while ago" apart from
	  "never asked about at all", which is what the fetch order downstream is built on. Delete
	  the record when it stops counting and that distinction cannot be made, because the only
	  evidence of the first ask was the thing that was deleted.
	*/
	check(art_ss_miss_held() == 1, "but the record is still held, now saying only that it was asked");
	check(miss_file_records() == 1, "and is still on the card, so a restart can still tell");
	check(art_ss_miss_stale(dd_sys, "Double Dragon (Europe).bin") == 1,
		"which is what a re-ask reads: eligible again, and not a game nobody has asked about");
	check(art_ss_miss_known(dd_sys, "Double Dragon (Europe).bin") == 0,
		"while the miss itself is emphatically not still known - the two are different questions");

	/*
	  And the second horizon, where it is genuinely forgotten. A game asked about once and
	  left alone for a month is as good as new: nothing on the card says otherwise, and
	  nothing needs to.
	*/
	check(miss_backdate(ART_SS_MISS_KEEP_DAYS - ART_SS_MISS_DAYS) == 1,
		"age it past the keep horizon as well");
	art_ss_miss_reload();
	check(art_ss_miss_held() == 0, "and now it is gone for good");
	check(miss_file_records() == 0, "the file having been rewritten without it");
	check(art_ss_miss_stale(dd_sys, "Double Dragon (Europe).bin") == 0,
		"the game reading as one nobody has ever asked about, which after a month it is");
	check(art_next_source(ddragon) == ART_SRC_SS, "and it is still on the rung either way");

	/* ------------- THE CRUX: the throttle is a refusal, not a verdict --- */

	/*
	  The sentence the owner's account actually got, as the entire body of a reply. It is
	  not XML, which is a thing this API does.

	  Read as a per-game miss it would be the worst bug this file has: the game in flight
	  when it arrives is written off, on the card, for a week, over a condition that was
	  never about that game. And it would arrive for every game the shelf touched for the
	  rest of the day.
	*/
	const char *throttle = "Faite du tri dans vos fichiers roms et repassez demain !\n";

	check(ss_body_class(throttle) == SS_ERR_UNMATCHED,
		"the throttle sentence classifies as the unmatched allowance being gone");
	check(ss_body_class(throttle) != SS_ERR_NOTFOUND,
		"and specifically NOT as this game not being in the database");
	check(ss_verdict(SS_ERR_UNMATCHED) == 0, "it is not a verdict about any game");
	check(ss_http_class(431) == SS_ERR_UNMATCHED, "and 431 is the same condition by status");

	put_file("/tmp/chome_miss_throttle.txt", throttle);
	ss_forget_state();
	art_ss_miss_reload();

	int before = art_ss_miss_count();
	check(art_next_source(smwjp) == ART_SRC_SS, "a game with nothing against it is on the rung");
	check(art_ss_settle(smwjp, "/tmp/chome_miss_throttle.txt") == 0, "the throttle reply yields no cover");

	check(art_ss_absent(smwjp) == 0, "and the game is not written off in the slot");
	check(art_ss_miss_count() == before, "nor on the card - the store is untouched");
	check(art_ss_miss_known(ss_system_id(lib_sys(lib_item(smwjp)->sysidx)->id,
		"Super Mario World (Japan).sfc"), "Super Mario World (Japan).sfc") == 0,
		"that game is left exactly as unasked as it was found");
	check(ss_hold_reason() == SS_ERR_UNMATCHED, "the module is what stands down, for the session");
	check(ss_may_request() == 0, "so nothing asks again until something restarts us");

	/* ---------------------------- nor does a quota, nor a dropped request --- */

	ss_forget_state();
	before = art_ss_miss_count();

	put_file("/tmp/chome_miss_quota.txt", "Erreur : Votre quota de scrape est ecoule pour aujourd'hui !\n");
	check(art_ss_settle(fusion, "/tmp/chome_miss_quota.txt") == 0, "a quota refusal yields no cover");
	check(art_ss_miss_count() == before, "and writes nothing to the card");
	check(ss_hold_reason() == SS_ERR_QUOTA, "it holds the module, as it always did");

	ss_forget_state();
	ss_note_result(SS_ERR_TRANSPORT, 0);
	check(art_ss_miss_count() == before, "a dropped request writes nothing to the card either");
	check(ss_hold_reason() == SS_OK, "and does not even hold the module");

	/* ------------------------------------------ a corrupt file fails safe --- */

	/*
	  Fail safe means ask again. The opposite failure - trusting a mangled record - would
	  write a real game off for a week on the strength of a byte that got flipped on a card
	  that was pulled out mid-write, and there would be nothing to see.
	*/
	art_ss_miss_record(dd_sys, "Double Dragon (Europe).bin");
	check(art_ss_miss_count() == 1, "one good record, to have something to corrupt");

	/*
	  Today's date in every fixture below, not a date typed out here. A hardcoded day would
	  expire, and an expired record is dropped for the *right* reason - so this whole block
	  would go on passing while testing nothing about corruption at all. That is exactly
	  what it did on its first run.

	  No embedded NUL either, tempting as one is: put_file() writes strlen() bytes, so a NUL
	  in the middle of the fixture silently truncates the rest of it away.
	*/
	char today[16];
	snprintf(today, sizeof(today), "%lu", (unsigned long)(time(0) / 86400));

	char rubbish[512];
	snprintf(rubbish, sizeof(rubbish),
		"# ClassicUI: games ScreenScraper had no cover for\n"
		"this is not a record at all\n"
		"\xff\xfe\x01 binary rubbish\n"
		"%s\n"
		"notanumber deadbeefdeadbeef 57/Something.bin\n"
		"%s 0000000000000000 57/A zero key is not a key\n", today, today);

	put_file(art_ss_miss_path(), rubbish);

	art_ss_miss_reload();
	check(art_ss_miss_count() == 0, "a file of nonsense yields no remembered misses rather than a crash");
	check(miss_file_records() == 0, "and it is rewritten without any of it");
	check(art_next_source(ddragon) == ART_SRC_SS,
		"so every game it should have covered is asked about again, which is the safe direction");

	// Truncation is the other shape of corruption, and the one a card pulled mid-write
	// actually produces: well-formed records, and then the file simply stops - here in the
	// middle of the third field, with no newline after it.
	char cut[128];
	snprintf(cut, sizeof(cut), "%s 1122334455667788 57/Half A Rec", today);
	put_file(art_ss_miss_path(), cut);

	art_ss_miss_reload();
	check(art_ss_miss_count() == 1,
		"a file that stops mid-line keeps what parsed and does not run off the end of it");
	check(art_ss_miss_known(dd_sys, "Double Dragon (Europe).bin") == 0,
		"and a half-written record cannot write off a real game");

	/* --------------------------------------------- and it stays bounded --- */

	{
		/*
		  More records than the store may hold, all valid. A shelf cannot produce this - the
		  library index is bounded well below ART_SS_MISS_MAX - but a file that has been
		  concatenated, edited or shared between cards can, and "it cannot happen" is not a
		  bound.
		*/
		FILE *f = fopen(art_ss_miss_path(), "wt");
		check(f != 0, "a store file can be written by hand");
		if (f)
		{
			unsigned long today = (unsigned long)(time(0) / 86400);
			for (int i = 0; i < ART_SS_MISS_MAX + 500; i++)
				fprintf(f, "%lu %016llx 57/Overflow %d.bin\n", today, (unsigned long long)(i + 1), i);
			fclose(f);
		}

		art_ss_miss_reload();
		check(art_ss_miss_count() == ART_SS_MISS_MAX,
			"a store of more than the cap is held at the cap rather than growing");
		check(miss_file_records() == ART_SS_MISS_MAX,
			"and the file is rewritten down to it, so it does not grow again on the next load");

		// One more on top of a full store: it fits, by dropping the oldest, and the store is
		// still the size it was. An out-of-order insert here would break the binary search
		// silently and answer "not remembered" for everything - see ss_miss_put().
		art_ss_miss_record(dd_sys, "Double Dragon (Europe).bin");
		check(art_ss_miss_count() == ART_SS_MISS_MAX, "recording into a full store does not grow it");
		check(art_ss_miss_known(dd_sys, "Double Dragon (Europe).bin") == 1,
			"and the record that was just paid for is the one that is kept");
	}

	/* -------------------- the reserve: the shelf stands down, the dialog does not --- */

	/*
	  The unmatched allowance is the one that runs out, and the disc dialog is the only
	  caller a player is actually watching. So the last tenth of it is the dialog's.

	  Read from a *successful* reply's counters, which is what makes this free: the moment
	  the allowance gets low is knowable without spending a request to discover it.
	*/
	unlink(art_ss_miss_path());
	art_ss_miss_reload();
	ss_forget_state();

	put_ss_ko_reply("/tmp/chome_miss_ko_ok.xml", 60, 2000);
	check(art_ss_settle(fusion, "/tmp/chome_miss_ko_ok.xml") == 1, "60 of 2000 unmatched: a normal reply");
	check(ss_ko_reserved() == 0, "nothing is held back");
	check(ss_may_request() == 1, "the shelf may scrape");
	check(ss_may_request_for(SS_ASK_DELIBERATE) == 1, "and so may the disc dialog");

	put_ss_ko_reply("/tmp/chome_miss_ko_low.xml", 1900, 2000);
	check(art_ss_settle(fusion, "/tmp/chome_miss_ko_low.xml") == 1, "1900 of 2000 unmatched: still a good reply");
	check(ss_ko_reserved() == 1, "but past the 90% mark, so the reserve is on");
	check(ss_hold_reason() == SS_OK, "nothing has gone wrong, so nothing is held off");
	check(ss_may_request() == 0, "the shelf stops scraping speculatively");
	check(ss_may_request_for(SS_ASK_DELIBERATE) == 1,
		"while the disc dialog - which a player is watching - still may");
	check(art_next_source(smwjp) == ART_SRC_LIBRETRO,
		"so the shelf quietly uses the pack instead of spending the last of the allowance");

	// And it lifts by itself when a later reply says the day has rolled over, without a
	// restart and without anyone having to notice.
	put_ss_ko_reply("/tmp/chome_miss_ko_reset.xml", 4, 2000);
	check(art_ss_settle(fusion, "/tmp/chome_miss_ko_reset.xml") == 1, "tomorrow's first reply arrives");
	check(ss_ko_reserved() == 0, "the reserve lifts on its own");
	check(ss_may_request() == 1, "and the shelf may scrape again");

	// The hard stop is still underneath it.
	put_ss_ko_reply("/tmp/chome_miss_ko_spent.xml", 2000, 2000);
	check(art_ss_settle(fusion, "/tmp/chome_miss_ko_spent.xml") == 1, "a reply whose counters say the ko allowance is gone");
	check(ss_hold_reason() == SS_ERR_UNMATCHED, "stands the module down outright");
	check(ss_may_request_for(SS_ASK_DELIBERATE) == 0, "for the deliberate caller as well as the shelf");
	check(art_ss_miss_count() == 0, "and none of those replies wrote a miss to the card");

	/* ---------------------------------------- a floor between requests --- */

	/*
	  maxthreads is 1 for an ordinary account and this module already serialises, so nothing
	  can overlap. What it could still do is fire a request the instant the last one lands,
	  for as long as somebody holds the stick on an unscraped shelf.

	  The gap is deliberately not part of ss_may_request(): the ladder's answer decides which
	  rung a game sits on, so a game drawn 300 ms after a request would be diverted to the
	  libretro pack over a timer. That pair of checks is the interesting one here.
	*/
	ss_forget_state();
	check(ss_gap_wait_ms() == 0, "with nothing asked yet there is nothing to wait for");
	check(ss_may_ask_now(SS_ASK_SPECULATIVE) == 1, "and a request may go out now");

	ss_note_request();
	int wait = ss_gap_wait_ms();
	check(wait > 0 && wait <= SS_MIN_REQUEST_GAP_MS,
		"immediately after one, there is a wait, and it is inside the gap");
	check(ss_may_ask_now(SS_ASK_SPECULATIVE) == 0, "so the shelf may not ask again yet");
	check(ss_may_ask_now(SS_ASK_DELIBERATE) == 0, "and nor may the disc dialog - the floor is about their server");

	check(ss_may_request() == 1,
		"but the ladder's own question is unaffected, so no game is diverted to the pack over a timer");
	check(art_next_source(smwjp) == ART_SRC_SS, "and that game stays on the ScreenScraper rung");

	ss_forget_state();
	check(ss_gap_wait_ms() == 0, "and a restart clears the gap with everything else");

	/* ------------------------------------------------------------- and out --- */

	check(art_fetch_active() == 0, "no pack fetch was started anywhere in this section");
	check(disc_art_active() == 0, "and no ScreenScraper download either");

	ss_forget_state();
	cfg.classicui_artfetch = 0;
	cfg.classicui_screenscraper = 0;
	cfg.classicui_ss_user[0] = 0;
	cfg.classicui_ss_pass[0] = 0;

	unlink("/tmp/chome_miss_nogame.xml");
	unlink("/tmp/chome_miss_throttle.txt");
	unlink("/tmp/chome_miss_quota.txt");
	unlink("/tmp/chome_miss_ko_ok.xml");
	unlink("/tmp/chome_miss_ko_low.xml");
	unlink("/tmp/chome_miss_ko_reset.xml");
	unlink("/tmp/chome_miss_ko_spent.xml");

	unlink(art_ss_miss_path());
	art_ss_miss_reload();
	check(art_ss_miss_count() == 0, "and the card is left with nothing remembered on it");

	art_redo();
	check(art_ss_absent(ddragon) == 0, "with the slots back the way the section above left them");
}

/*
  ==========================================================================
  The order covers are asked for, and where a cover came from.
  ==========================================================================

  Two features, one section each, and they share this note because they share a premise:
  the store written yesterday made it possible to stop asking, and the two things asked for
  next are about what happens *instead* of asking, and *when*.

  Neither shows in a pixel. A shelf whose covers were fetched in the worst possible order
  looks exactly like one fetched in the best, because the end state is the same and only the
  bill differs - and the bill is the unmatched allowance, which is 2000 a day and which a
  filename matcher against regional variants spends fast. So both sections assert against
  the functions the live path uses: art_queue_next() is the order art_step() pops with, and
  art_pack_landed() is the half of the pack fetch that runs on the device.

  Not one request leaves this process. Every section that turns ScreenScraper on holds the
  minimum gap closed with ss_note_request() first, which makes cover_ss_start() refuse at
  the last gate before the fork - so the ladder, the ordering and the counters are all
  exercised on the live code and the curl is never reached. That is asserted rather than
  hoped for, in both sections, because "no request went out" is the one property here that
  cannot be checked by reading the output afterwards.
*/

// Fresh slots, an empty queue and nothing walked yet - a boot, without the stepping
// art_redo() does. Every ordering check below needs items that are ART_NONE rather than the
// ART_MISSING a walked ladder leaves, because art_request() refuses to queue a missing item.
static void art_fresh_slots()
{
	art_shutdown();
	art_init(theme_get()->sel_w, theme_get()->sel_h);
}

// Record lines in the pack store, the same way miss_file_records() counts the other one.
// Two counts of two files, because "the two stores stay distinct" is a claim about the card.
static int pack_file_records()
{
	FILE *f = fopen(art_pack_path(), "rt");
	if (!f) return -1;

	int n = 0;
	char line[256];
	while (fgets(line, sizeof(line), f)) if (line[0] != '#' && line[0] != '\n') n++;

	fclose(f);
	return n;
}

static void assert_art_fetch_order()
{
	printf("\n== the fetch order: a first ask before a re-ask ==\n");

	int ddragon = item_by_path("Genesis", "Double Dragon (Europe).bin");
	int smwjp   = item_by_path("SNES", "Super Mario World (Japan).sfc");
	int bonk    = item_by_path("TGFX16", "Bonk's Adventure (USA).pce");
	int metroid = item_by_path("SNES", "Super Metroid (Europe).sfc");
	check(ddragon >= 0 && smwjp >= 0 && bonk >= 0 && metroid >= 0,
		"the four games this section needs are indexed");

	chome_item *dd = lib_item(ddragon);
	const char *dd_sys = ss_system_id(lib_sys(dd->sysidx)->id, "Double Dragon (Europe).bin");
	chome_item *sm = lib_item(smwjp);
	const char *sm_sys = ss_system_id(lib_sys(sm->sysidx)->id, "Super Mario World (Japan).sfc");
	check(dd_sys != 0 && sm_sys != 0, "and both of the two systems in question can be asked about");

	cfg.classicui_artfetch = 1;
	cfg.classicui_screenscraper = 1;
	strcpy(cfg.classicui_ss_user, FIX_SSID);
	strcpy(cfg.classicui_ss_pass, FIX_SSPASS);
	ss_forget_state();

	unlink(art_ss_miss_path());
	art_ss_miss_reload();
	unlink(art_pack_path());
	art_pack_reload();

	/* ------------------------- one game asked about a week ago, one never --- */

	/*
	  The two states the whole section is about, set up on two real games: a miss recorded
	  and then aged out of the window, and a game with nothing whatsoever against it.
	*/
	art_ss_miss_record(dd_sys, "Double Dragon (Europe).bin");
	check(miss_backdate(ART_SS_MISS_DAYS) == 1, "one recorded miss, aged out of its window");
	art_ss_miss_reload();

	check(art_ss_miss_count() == 0, "so nothing counts as a miss any more");
	check(art_ss_miss_stale(dd_sys, "Double Dragon (Europe).bin") == 1,
		"but the Mega Drive game reads as a re-ask: asked about once, eligible again");
	check(art_ss_miss_stale(sm_sys, "Super Mario World (Japan).sfc") == 0,
		"while the SNES one has never been asked about at all");

	check(art_next_source(ddragon) == ART_SRC_SS, "both are on the ScreenScraper rung");
	check(art_next_source(smwjp) == ART_SRC_SS, "which is what makes the order a question");

	/*
	  And the floor, closed for the rest of the section. cover_ss_start() consults it at the
	  last gate before it forks a curl, so from here the whole ladder runs and no request can
	  possibly leave this process - see the note at the top of this section.
	*/
	ss_note_request();
	check(ss_may_ask_now(SS_ASK_SPECULATIVE) == 0,
		"with the minimum gap held closed, no request can leave this process at all");
	check(ss_may_request() == 1, "while the ladder's own question is unaffected, as it must be");

	/* ----------------------------------------- and the re-ask waits its turn --- */

	/*
	  The re-ask is the card under the cursor and the first ask is four cards away, which is
	  the arrangement where the two rules disagree - and the one the owner asked to be got
	  right. Before either has been walked all that is known is the priority, so priority is
	  what decides: nothing has yet said that this game is about to spend a request.
	*/
	art_fresh_slots();
	art_request(ddragon, 0);
	art_request(smwjp, 4);
	check(art_queue_next() == ddragon,
		"before any ladder has been walked, the nearer card is next, exactly as it always was");

	unsigned asks0 = art_ss_asks();
	art_step();

	check(art_ss_asks() == asks0 + 1, "one pass reaches the ScreenScraper rung exactly once");
	check(art_ss_last_ask() == smwjp,
		"and it is spent on the game nobody has ever asked about, not on the on-screen re-ask");

	check(art_state(ddragon) == ART_NONE,
		"the re-ask is stood down, not written off - ART_NONE is re-requestable");
	check(art_next_source(ddragon) == ART_SRC_SS,
		"and it is still on the ScreenScraper rung, not diverted to the pack over a deferral");
	check(art_ss_miss_count() == 0, "nothing was recorded against either game - nothing was learnt");

	/*
	  And now that the ladder has said it out loud, the ordering sticks: the same pair queued
	  the same way comes out the other way round, without the ladder being walked again.
	*/
	art_request(ddragon, 0);
	art_request(smwjp, 4);
	check(art_queue_next() == smwjp,
		"queued again, the first ask is taken ahead of the re-ask from four cards away");

	/* ------------------------------- on-screen priority still beats both --- */

	// A second game nobody has asked about, nearer the cursor than the first one.
	art_request(bonk, 1);
	check(art_queue_next() == bonk,
		"between two games nobody has asked about, the nearer one still wins - priority is intact");

	/*
	  A picture that can be painted now beats a request every time, however far off screen it
	  is. Super Metroid has a cover on the card five cards away; the re-ask is under the
	  cursor. One pass defers the one and decodes the other.
	*/
	art_fresh_slots();
	art_request(ddragon, 0);
	art_request(metroid, 5);
	check(art_queue_next() == ddragon, "the card under the cursor is next, before anything is walked");

	unsigned asks1 = art_ss_asks();
	art_step();
	check(art_state(metroid) == ART_READY,
		"one pass stands the re-ask down and spends itself on the cover it can actually paint");
	check(art_ss_asks() == asks1, "reaching the rung for nobody at all");

	/*
	  Deferred and not starved, which is the other half of "not written off". With nothing
	  else waiting there is nobody to be unfair to, so the re-ask goes.
	*/
	art_request(ddragon, 0);
	check(art_queue_next() == ddragon, "with the queue otherwise empty, the re-ask is what is left");

	art_step();
	check(art_ss_asks() == asks1 + 1, "so it is asked about after all");
	check(art_ss_last_ask() == ddragon, "and it is the re-ask that got that turn");

	/* ------------------------------------------ first asked, first served --- */

	/*
	  The fairness the owner asked for by name. Two cards the same distance from the
	  selection: the one that has been waiting goes first. It used to be the other way about
	  - queue_pop() filled the hole it made with the last entry, so equals came out roughly
	  last-in-first-out - which cost nothing while every entry was a local decode and costs
	  an allowance now that an entry can be a request.
	*/
	art_fresh_slots();
	art_request(smwjp, 2);
	art_request(bonk, 2);
	check(art_queue_next() == smwjp, "of two cards the same distance away, the one asked for first");

	art_request(bonk, 1);
	check(art_queue_next() == bonk,
		"unless the player scrolls towards one of them, which is the whole point of priority");

	// And a promotion does not send a card to the back of its own queue: bonk was promoted
	// above, so putting smwjp back on level terms leaves bonk in front of it.
	art_request(smwjp, 1);
	check(art_queue_next() == smwjp,
		"and when the other catches up on priority the earlier arrival leads again - a promotion"
		" changes what a card is worth, not where it stands in the line");

	/* ------------------------------------------------------------- and out --- */

	check(art_fetch_active() == 0, "no pack fetch was started anywhere in this section");
	check(disc_art_active() == 0, "and no ScreenScraper download either");
	check(art_ss_miss_count() == 0, "with nothing written to the miss store by any of it");
	check(art_pack_count() == 0, "and nothing to the pack store, which this section never touched");

	ss_forget_state();
	cfg.classicui_artfetch = 0;
	cfg.classicui_screenscraper = 0;
	cfg.classicui_ss_user[0] = 0;
	cfg.classicui_ss_pass[0] = 0;

	unlink(art_ss_miss_path());
	art_ss_miss_reload();
	art_redo();
}

static void assert_pack_provenance()
{
	printf("\n== a cover from the pack: remembered as such, retried only if asked ==\n");

	int smwjp   = item_by_path("SNES", "Super Mario World (Japan).sfc");
	int ddragon = item_by_path("Genesis", "Double Dragon (Europe).bin");
	int bonk    = item_by_path("TGFX16", "Bonk's Adventure (USA).pce");
	check(smwjp >= 0 && ddragon >= 0 && bonk >= 0, "the three games this section needs are indexed");

	chome_item *sm = lib_item(smwjp);
	const chome_sys *smsys = lib_sys(sm->sysidx);
	const char *sm_sys = ss_system_id(smsys->id, "Super Mario World (Japan).sfc");
	chome_item *dd = lib_item(ddragon);
	const char *dd_sys = ss_system_id(lib_sys(dd->sysidx)->id, "Double Dragon (Europe).bin");
	check(sm_sys != 0 && dd_sys != 0, "and both systems have a ScreenScraper id");

	// Where the pack's cover for that game lands: the libretro layout, in our own artdir,
	// which is the file both fetchers write and the one rung one finds afterwards.
	char landed[1024];
	snprintf(landed, sizeof(landed), "%s/boxart/%s/Named_Boxarts/%s.png",
		ROOT, smsys->lr, "Super Mario World (Japan)");

	cfg.classicui_artfetch = 1;
	cfg.classicui_screenscraper = 1;
	strcpy(cfg.classicui_ss_user, FIX_SSID);
	strcpy(cfg.classicui_ss_pass, FIX_SSPASS);
	cfg.classicui_ss_replace_pack = 0;
	ss_forget_state();

	unlink(art_ss_miss_path());
	art_ss_miss_reload();
	unlink(art_pack_path());
	art_pack_reload();
	unlink(landed);

	check(strcmp(art_pack_path(), art_ss_miss_path()) != 0,
		"the two stores are two files, and not the same one under two names");
	check(art_pack_count() == 0 && art_ss_miss_count() == 0, "and both start empty");

	/* --------------------------------------- the pack's cover, and the mark --- */

	/*
	  The live landing path, driven on a file the way art_ss_settle() is driven on a reply.
	  The curl cannot run here and must not, so the harness writes the PNG the pack would
	  have sent and hands it to the function the fetcher calls with it.
	*/
	make_cover("/tmp/chome_pack_cover.png", 400, 600, 0xff20c0c0);
	check(art_pack_landed(smwjp, "/tmp/chome_pack_cover.png") == 1,
		"a pack download lands, and is stored on the card");
	struct stat lst;
	check(!stat(landed, &lst) && lst.st_size > 0,
		"in the libretro layout, where rung one will find it");

	check(art_pack_count() == 1, "and the card now records where that cover came from");
	check(art_pack_marked(sm_sys, "Super Mario World (Japan).sfc") == 1,
		"under the query ScreenScraper would be asked, not under the shelf's index - so a rescan cannot lose it");
	check(pack_file_records() == 1, "with one record in the pack store's own file");

	/*
	  THE distinction. A cover from the pack is not a game the database has nothing for, and
	  writing one down must not have written the other.
	*/
	check(art_ss_miss_count() == 0, "and it wrote no miss - a pack cover is not a refusal");
	check(art_ss_miss_held() == 0, "not even a record of one having been asked about");
	check(art_ss_miss_known(sm_sys, "Super Mario World (Japan).sfc") == 0,
		"and the mark is emphatically not readable as a miss");
	check(art_ss_miss_stale(sm_sys, "Super Mario World (Japan).sfc") == 0, "nor as an aged-out one");

	// A restart re-reads it off the card, which is the property a session flag did not have.
	art_pack_reload();
	check(art_pack_count() == 1, "the mark survives a restart");
	check(art_pack_marked(sm_sys, "Super Mario World (Japan).sfc") == 1, "and still answers for that game");

	/* -------------------------- and by itself it does nothing whatsoever --- */

	/*
	  The setting is off, which is how it ships. The cover is drawn, the mark sits there, and
	  nothing is asked about - which is the property yesterday's work exists to protect and
	  the one this feature could most easily have broken.
	*/
	ss_note_request();
	unsigned asks0 = art_ss_asks();

	art_fresh_slots();
	art_request(smwjp, 0);
	for (int i = 0; i < 8; i++) art_step();

	check(art_state(smwjp) == ART_READY, "the pack's cover decodes as any other local file does");
	check(cover_is(smwjp, 0xff20c0c0, "pack cover") == 1, "and it is the pack's cover that is on screen");
	check(art_next_source(smwjp) == ART_SRC_LOCAL,
		"the ladder is untouched by the mark: the card still wins rung one");
	check(art_pack_retry_pending() == 0, "nothing is queued for a retry");
	check(art_ss_asks() == asks0, "and the ScreenScraper rung is not reached for it at all");

	/* ------------------------------------ until the player asks for it --- */

	cfg.classicui_ss_replace_pack = 1;

	/*
	  One pass, deliberately. That pass decodes the cover and arms the retry, and it does not
	  spend itself on the retry as well - a pass that has done a decode is finished. So this is
	  also the check that the two happen in that order and not in one go, which is what "the
	  picture first, the preference afterwards" has to mean to be worth anything.
	*/
	art_fresh_slots();
	art_request(smwjp, 0);
	art_step();

	check(art_state(smwjp) == ART_READY, "with the setting on, the cover on the card is still decoded first");
	check(cover_is(smwjp, 0xff20c0c0, "pack cover, retry armed") == 1,
		"and still drawn - a retry never withholds the picture the player already has");
	check(art_next_source(smwjp) == ART_SRC_LOCAL, "rung one still answers for it");
	check(art_pack_retry_pending() == 1, "but the retry is armed now, behind everything else");

	unsigned asks1 = art_ss_asks();
	ss_note_request();
	art_step();

	check(art_ss_asks() == asks1 + 1, "an idle pass spends itself on it");
	check(art_ss_last_ask() == smwjp, "on that game");
	check(art_pack_retry_pending() == 0, "and the retry is off the list");

	ss_note_request();
	for (int i = 0; i < 20; i++) art_step();
	check(art_ss_asks() == asks1 + 1, "one attempt each, and twenty more passes do not find a second one");

	/* ------------------------- and the settle hands the game to the other store --- */

	/*
	  The reply says the database has never heard of it. That is a verdict, so the miss store
	  takes the game - and the mark comes off, because the question the player wanted asked
	  has been asked and the pack's cover is the answer after all.

	  One game, two stores, opposite claims, and this is the check that they went the right
	  way round.
	*/
	put_file("/tmp/chome_pack_nogame.xml",
		"<Data><ssuser><id>" FIX_SSID "</id><maxthreads>1</maxthreads>"
		"<requeststoday>210</requeststoday><maxrequestsperday>20000</maxrequestsperday>"
		"<requestskotoday>70</requestskotoday><maxrequestskoperday>2000</maxrequestskoperday>"
		"</ssuser></Data>\n");

	check(art_ss_settle(smwjp, "/tmp/chome_pack_nogame.xml") == 0, "the reply names no game, so no cover");
	check(art_ss_miss_count() == 1, "the miss store takes it: they have not got it");
	check(art_ss_miss_known(sm_sys, "Super Mario World (Japan).sfc") == 1, "under that game's query");
	check(art_pack_count() == 0, "and the mark comes off - the retry has been had");
	check(art_pack_marked(sm_sys, "Super Mario World (Japan).sfc") == 0, "for that game specifically");
	check(pack_file_records() == 0, "the pack store's file having been rewritten without it");
	check(miss_file_records() == 1, "while the miss store's file has the one record, and only it");

	check(art_next_source(smwjp) == ART_SRC_LOCAL,
		"and the cover the player has is still the cover they have");

	/* ------------------------------------ neither store reads the other's mind --- */

	art_pack_mark(dd_sys, "Double Dragon (Europe).bin");
	check(art_pack_marked(dd_sys, "Double Dragon (Europe).bin") == 1, "a mark for a second game");
	check(art_ss_miss_known(dd_sys, "Double Dragon (Europe).bin") == 0,
		"which is not a miss for it - a mark must never suppress a request");
	check(art_ss_miss_stale(dd_sys, "Double Dragon (Europe).bin") == 0, "and not an aged-out one either");
	check(art_next_source(ddragon) == ART_SRC_SS,
		"so that game is still asked about, mark and all");

	art_ss_miss_record(ss_system_id(lib_sys(lib_item(bonk)->sysidx)->id, "Bonk's Adventure (USA).pce"),
		"Bonk's Adventure (USA).pce");
	check(art_ss_miss_known(ss_system_id(lib_sys(lib_item(bonk)->sysidx)->id, "Bonk's Adventure (USA).pce"),
		"Bonk's Adventure (USA).pce") == 1, "a miss for a third game");
	check(art_pack_marked(ss_system_id(lib_sys(lib_item(bonk)->sysidx)->id, "Bonk's Adventure (USA).pce"),
		"Bonk's Adventure (USA).pce") == 0,
		"which is not a mark for it - a miss must never authorise a retry");

	// And off the card, both of them, because the claim is about the two files and not about
	// two arrays that happen to be in step this run.
	art_ss_miss_reload();
	art_pack_reload();
	check(art_ss_miss_count() == 2, "read back off the card: two misses");
	check(art_pack_count() == 1, "and one mark");
	check(art_pack_marked(dd_sys, "Double Dragon (Europe).bin") == 1, "still for the game it was written for");
	check(art_ss_miss_known(dd_sys, "Double Dragon (Europe).bin") == 0, "and still not a miss for it");

	/* ------------------------------------------------------------- and out --- */

	cfg.classicui_ss_replace_pack = 0;
	art_step();
	check(art_pack_retry_pending() == 0, "switch the setting off and the retry list goes with it");

	check(art_fetch_active() == 0, "no pack download was started anywhere in this section");
	check(disc_art_active() == 0, "and no ScreenScraper one either");

	ss_forget_state();
	cfg.classicui_artfetch = 0;
	cfg.classicui_screenscraper = 0;
	cfg.classicui_ss_user[0] = 0;
	cfg.classicui_ss_pass[0] = 0;

	unlink("/tmp/chome_pack_nogame.xml");
	unlink(art_ss_miss_path());
	art_ss_miss_reload();
	unlink(art_pack_path());
	art_pack_reload();

	// The cover this section put on the card goes too: every section after it is entitled to
	// the shelf assert_gamelist() left behind.
	unlink(landed);
	art_redo();
	check(art_state(smwjp) != ART_READY, "and that game has no cover on the card again");
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

/* ------------------------------------------------- collecting the children --- */

/*
  A modelled process table for the fake waitpid in the section below.

  `survives` is how many more attempts this pid answers "still running" to before it
  exits. A pid that is not in here at all is not this process's child, which is what a
  real waitpid() reports as ECHILD.
*/
static struct { pid_t pid; int survives; } proc_tbl[8];
static int proc_n = 0;

static void proc_reset() { proc_n = 0; }

static void proc_add(pid_t pid, int survives)
{
	if (proc_n >= (int)(sizeof(proc_tbl) / sizeof(proc_tbl[0]))) return;
	proc_tbl[proc_n].pid = pid;
	proc_tbl[proc_n].survives = survives;
	proc_n++;
}

static int proc_fake_reap(pid_t pid)
{
	for (int i = 0; i < proc_n; i++)
	{
		if (proc_tbl[i].pid != pid) continue;
		if (proc_tbl[i].survives > 0) { proc_tbl[i].survives--; return 0; }
		return 1;
	}
	return -1;
}

static pid_t sig_pid = 0;
static int sig_sig = 0, sig_group = 0, sig_calls = 0;

static void proc_fake_sig(pid_t pid, int sig, int group)
{
	sig_pid = pid;
	sig_sig = sig;
	sig_group = group;
	sig_calls++;
}

/*
  chome_proc.cpp, and the guard inside bt_pair_start().

  What this section can and cannot say, plainly, because the temptation here is to write a
  test that proves a stub.

  There is no fork() in this harness and no children, so a real waitpid() has nothing to
  return about any pid this file can invent, and a real kill() would signal whichever host
  process happens to own that number. Both syscalls therefore go through the seam
  chome_proc.h documents, and what is asserted below is the *decision*: after a stop, does
  the firmware still believe it has a child to collect; is it asked again on the next
  sweep; is it dropped once it comes back; and is the second-start guard closed. That the
  real waitpid(WNOHANG) then behaves as the module assumes is not verifiable here at all -
  it wants a device, a drive, and `ps` showing no Z after an evening of disc swaps.

  The first block is the finding itself restated as a test: a child signalled a moment ago
  is still running, so the WNOHANG that the three sites used to do right there came back
  empty - and every one of them then dropped the pid, which is what made the zombie
  permanent.
*/
static void assert_children_are_collected()
{
	printf("\n== stopped children are collected, not dropped ==\n");

	chome_proc_test_clear();
	chome_proc_test_hooks(proc_fake_sig, proc_fake_reap);

	{
		proc_reset();
		proc_add(4001, 3);                     // takes three sweeps to die, as a real one would

		sig_calls = 0;
		chome_child_stop(4001, SIGKILL, 0);

		check(sig_calls == 1 && sig_pid == 4001 && sig_sig == SIGKILL && !sig_group,
			"a stopped child is signalled once, on its own pid rather than its group");
		check(chome_child_pending() == 1 && chome_child_outstanding(4001),
			"and is still outstanding the instant after - which is the whole finding: "
			"nothing has died yet, so the reap that used to be here could not work");

		chome_child_reap();
		check(chome_child_outstanding(4001) && chome_child_tries(4001) == 1,
			"one sweep later it is still tracked, and it was asked about exactly once");

		chome_child_reap();
		chome_child_reap();
		check(chome_child_outstanding(4001) && chome_child_tries(4001) == 3,
			"it keeps being asked, once per sweep, for as long as it is still running");

		chome_child_reap();
		check(!chome_child_outstanding(4001) && chome_child_pending() == 0,
			"and it is collected on the first sweep after it actually exits");
	}

	{
		// The ECHILD case. It should not happen - nothing else in this firmware reaps -
		// but retrying it for ever would be a syscall a frame until the process is
		// replaced, so it is dropped instead.
		proc_reset();
		chome_child_stop(4002, SIGKILL, 0);
		check(chome_child_outstanding(4002),
			"a pid is tracked before anything is known about it");
		chome_child_reap();
		check(!chome_child_outstanding(4002) && chome_child_pending() == 0,
			"a pid the kernel says was never ours is dropped, not retried for ever");
	}

	{
		/*
		  Handing the same child over twice, which is what bt_pair_start() does when the
		  polite SIGINT has not been acted on: the signal goes again and harder, and there
		  is still one entry. Two entries would mean two collections of one child, and the
		  second of them reaping whatever the pid had been recycled onto.
		*/
		proc_reset();
		proc_add(4003, 99);

		chome_child_stop(4003, SIGINT, 1);
		check(sig_sig == SIGINT && sig_group == 1,
			"the pairing agent is signalled on its group, because it made one of its own");

		int was = sig_calls;
		chome_child_stop(4003, SIGKILL, 1);
		check(sig_calls == was + 1 && sig_sig == SIGKILL && sig_group == 1,
			"a second stop of the same child signals it again, harder");
		check(chome_child_pending() == 1,
			"and does not add a second entry for one child");

		chome_proc_test_clear();
	}

	{
		// Signal 0 is "already on its way out, just collect it".
		proc_reset();
		proc_add(4004, 1);
		int was = sig_calls;
		chome_child_stop(4004, 0, 0);
		check(sig_calls == was && chome_child_outstanding(4004),
			"a child handed over with no signal is collected without being signalled");
		chome_child_reap();
		chome_child_reap();
		check(chome_child_pending() == 0, "and collected all the same");
	}

	{
		/*
		  Two children that exit on the same sweep. The set is compacted by moving the last
		  entry into the vacated slot, so a forwards walk would step straight over whatever
		  landed there - one leaked pid, on the one frame two modules were collected
		  together, which is not something anybody would ever have noticed on a device.
		*/
		proc_reset();
		proc_add(4005, 0);
		proc_add(4006, 0);
		chome_child_stop(4005, SIGKILL, 0);
		chome_child_stop(4006, SIGKILL, 0);
		check(chome_child_pending() == 2, "two children can be outstanding at once");

		chome_child_reap();
		check(chome_child_pending() == 0,
			"and both are collected by the one sweep - compacting the set must not make "
			"the walk skip the entry moved into the hole");
	}

	{
		/*
		  A child that can never be collected - the stuck-in-an-ioctl case the whole
		  arrangement exists for - must not grow the set without bound, and must not stop
		  the ones already in it from being retried. No reap hook at all here, which is
		  exactly "the kernel never gives it back".
		*/
		proc_reset();
		chome_proc_test_clear();
		chome_proc_test_hooks(proc_fake_sig, 0);

		sig_calls = 0;
		const int over = CHOME_CHILD_MAX + 4;
		for (int i = 0; i < over; i++) chome_child_stop(5000 + i, SIGKILL, 0);

		check(chome_child_pending() == CHOME_CHILD_MAX,
			"children that never die fill the set and cannot push it past its bound");
		check(sig_calls == over,
			"every one of them was still signalled, including the ones there was no room "
			"to remember - being stopped matters more than being counted");
		check(chome_child_outstanding(5000) && !chome_child_outstanding(5000 + over - 1),
			"the ones already tracked are kept and the overflow is given up on out loud, "
			"rather than one of them being silently forgotten to make room");

		for (int i = 0; i < 200; i++) chome_child_reap();
		check(chome_child_pending() == CHOME_CHILD_MAX,
			"and two hundred sweeps of stuck children neither collect one nor lose one");
	}

	/*
	  And the guard the same finding turned up in chome_bt: bt_pair_start() tested
	  `pairing`, which pair_child_stop() clears in the same breath as it sends its SIGINT.
	  btctl takes a moment over that signal, so a Done followed straight away by an Add
	  forked a second agent onto the adapter the first one was still using - and overwrote
	  the pid, so the first was unreapable as well.

	  The decision only, because bt_pair_start() itself forks. Whether it is wired to this
	  answer is a device question; nothing in this harness may call it.
	*/
	check(bt_pair_start_due(0, 0) == 1,
		"Add controller starts an agent when there is nothing in the way");
	check(bt_pair_start_due(1, 0) == 0,
		"and does nothing at all while discovery is already up");
	check(bt_pair_start_due(1, 1) == 0,
		"nor while discovery is up and the last child is still around");
	check(bt_pair_start_due(0, 1) == 2,
		"and when the last agent has not gone yet it is stopped before a new one starts - "
		"the window that used to put two btctl agents on one adapter");

	// Leave nothing behind: no hooks, so nothing later in this run can signal a host pid,
	// and no entries, so a later count belongs to whoever made it.
	chome_proc_test_clear();
	chome_proc_test_hooks(0, 0);
	proc_reset();
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

/*
  chome_core_idle() - the predicate main() ANDs with menu_mgl_busy() to decide whether the
  poll loop may usleep(1000) instead of spinning. It is pure and does not touch mgl at all,
  so unlike menu_mgl_busy() (ARM-only, not linked into this harness) it can be driven and
  checked directly here.

  The one guard that must never weaken: a core that is still running - no usable pause, and
  classicui_freeze=0 so it is not held with a state either (the SNES/Battletoads case) - is
  the red "STILL PLAYING" band, and it must keep reporting not-idle even though our menu is
  drawn on top of it.
*/
static void assert_core_idle_predicate()
{
	printf("\n== chome_core_idle() ==\n");

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
	check(!chome_core_idle(), "not idle before the in-game menu ever opens");

	// A core that pauses for real: genuinely held still.
	harness_set_confstr(4);
	harness_reset_status();
	press(KEY_MENU, 20);
	check(chome_ingame_active(), "menu opens over the paused core");
	check(chome_core_idle(), "really paused - safe to back off");
	press(KEY_MENU, 16);
	frame(8);
	check(!chome_core_idle(), "and not once the menu closes and the core is live again");

	// Savestates but no usable pause, freeze left on (the default): held by a state instead.
	harness_set_confstr(2);
	press(KEY_MENU, 20);
	check(chome_ingame_active(), "menu opens over the frozen core");
	check(chome_core_idle(), "frozen with a state - also safe to back off");
	press(KEY_MENU, 16);
	frame(8);

	/*
	  Freeze turned off, so nothing holds the core still: the game runs on behind the menu.

	  This used to be excluded, on the grounds that a running game is still making sound.
	  That was wrong and Dinofly pointed it out: ig_mute_engage() is called unconditionally
	  when the in-game menu opens, before any pause or freeze decision, so every core is
	  muted - and the HPS framebuffer replaces the scaler's input rather than blending over
	  it, so the game is not on screen either. Neither seen nor heard means there is nothing
	  to be prompt for, so this backs off too.
	*/
	cfg.classicui_freeze = 0;
	harness_reset_status();
	press(KEY_MENU, 20);
	check(chome_ingame_active(), "the menu still opens");
	check(chome_core_idle(),
		"a running core behind the menu backs off too - it is muted and not composited");
	press(KEY_MENU, 16);
	frame(8);
	cfg.classicui_freeze = 1;
	harness_set_confstr(1);

	// cfg.classicui off disables the whole idea, regardless of pause state. The default
	// confstr (set just above) already has a real "Pause when OSD is open" option.
	press(KEY_MENU, 20);
	check(chome_ingame_active(), "menu open over a genuinely paused core, for the next check");
	cfg.classicui = 0;
	check(!chome_core_idle(), "the feature is off, so never idle from here");
	cfg.classicui = 1;
	press(KEY_MENU, 16);
	frame(8);
	harness_set_confstr(1);

	// The shelf, in the menu core: no core to be careful of, so this mirrors chome_active().
	harness_set_menu_core(1);
	chome_leave();
	press(KEY_MENU, 20);
	frame(6);
	check(chome_active(), "the shelf is up");
	check(chome_core_idle(), "shelf up in the menu core - safe to back off");
	for (int i = 0; i < 4; i++) press(KEY_ESC, 6);
	harness_set_menu_core(0);
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
/*
  A core that leaves config-string index 1 empty, which is the shape of the real Saturn
  core and the reason its options were invisible.

  The stock OSD starts reading options at index 2 (menu.cpp:2023) and index 1 is a slot it
  never touches, so a core may leave it empty. user_io_get_confstr() returns NULL for an
  empty entry, and our scanner treats NULL as end-of-string - so starting at 1 ended the
  scan before it began and the core looked like it had no options at all.

  What that cost, measured on the device on 2026-08-11: the Saturn core's Region option
  (O[35:33], default Japan) was unreachable, so the machine refused every USA and European
  disc with "Game disc unsuitable for this system" - our own rip and a known-good Redump
  dump alike - and the menu bar showed no core-options entry to fix it with, because
  mb_visible(MB_CORE) asks core_opts_tier_count() and every tier was 0.

  The entries below are the real ones, in the real order, read off the device.
*/
static const char *confstr_saturn[] =
{
	"Saturn",
	"",                                    /* the empty slot that ended the scan */
	"S0,CUECHD,Insert Disc",
	"FS2,BIN,Load bios",
	"FS3,BIN,Load cartridge",
	"O[4],Reset on insert,Yes,No",
	"-",
	"O[23:21],Cartridge,None,ROM 2M,DRAM 1M,DRAM 4M,DRAM 6M DEV,BACKUP",
	"O[35:33],Region,Japan,Taiwan,USA,Brazil,Korea,Asia,Europe,Auto",
	"-",
	"S1,SAV,Mount Backup RAM",
	0
};

static void assert_saturn_options()
{
	harness_set_confstr_table(confstr_saturn);
	harness_set_osd_mask(0);

	int n = core_opts_scan();
	check(n > 0, "a core that leaves config index 1 empty still has its options read");

	const core_opt *region = 0, *cart = 0;
	for (int i = 0; i < core_opts_count(); i++)
	{
		const core_opt *o = core_opt_at(i);
		if (!strcasecmp(o->name, "Region")) region = o;
		if (!strcasecmp(o->name, "Cartridge")) cart = o;
	}

	check(region != 0, "Region is one of them, which is what makes a Saturn disc bootable");
	check(cart != 0, "and so is Cartridge, so this is not one option arriving by luck");
	check(region && region->nvals == 8 && !strcmp(region->vals[0], "Japan")
		&& !strcmp(region->vals[7], "Auto"),
		"with all eight regions, Japan first and Auto last, as the core spells them");

	/*
	  And the count the menu bar actually consults. mb_visible(MB_CORE) shows the entry only
	  when one of these three tiers is non-empty, so this is the assertion that the screen
	  can be reached at all - the failure everybody actually saw.
	*/
	check(core_opts_tier_count(CO_TIER_PICTURE)
		|| core_opts_tier_count(CO_TIER_SYSTEM)
		|| core_opts_tier_count(CO_TIER_RISKY),
		"and a tier the menu bar counts is non-empty, so the Core entry is offered");

	/*
	  Put the shared fixture back before returning. The checks after the call site read
	  whatever the last scan left in core_opts_count(), and this core carries a real
	  "Reset on insert" *setting* - Yes/No, not a momentary trigger - which the loose
	  strcasestr(name, "Reset") test below would otherwise report as a trigger being
	  offered. That test is a proxy for "no T/R spec is ever accepted", which the parser
	  enforces by only taking O and o; the name match is not the property it means.
	*/
	harness_set_confstr_table(0);
	harness_set_confstr(6);
	core_opts_scan();
}

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

/*
  Which row a list has highlighted, as the y of the selection bar.

  By its own colour rather than by hashing the panel, because a hash cannot tell "the cursor
  moved" from "a cover finished decoding" - and this is used to check exactly that the cursor
  moved. -1 when nothing is highlighted.
*/
static int sel_bar_y()
{
	const uint32_t *fb = harness_fb_shown();
	if (!fb) return -1;

	int w = gfx_w(), h = gfx_h();
	for (int y = 0; y < h; y++)
	{
		int n = 0;
		for (int x = 0; x < w; x++) if (fb[(size_t)y * w + x] == 0xff2e6fb8u) n++;
		if (n > w / 8) return y;
	}
	return -1;
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
	assert_saturn_options();

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

	/*
	  Up and down have to move the visible cursor, not just the variable behind it.

	  A user reported the selection never appearing to change until they pressed left or
	  right. That is exactly what happens when the row moves and nothing repaints: move_v()
	  has no trailing mark_dirty() - each case calls it - and this screen's case did not,
	  while move_h() does. So changing a value revealed a cursor that had silently moved
	  several rows earlier.
	*/
	int bar0 = sel_bar_y();
	check(bar0 >= 0, "the selected row is highlighted");
	press(KEY_DOWN, 14);
	frame(10);
	int bar1 = sel_bar_y();
	printf("  selection bar y: %d -> %d\n", bar0, bar1);
	check(bar1 > bar0, "down moves the highlight without needing another keypress");
	press(KEY_UP, 14);
	frame(10);
	check(sel_bar_y() == bar0, "and up moves it back");

	int before = first ? core_opt_value(first) : 0;
	int saves = harness_cfg_saves();
	press(KEY_RIGHT, 14);
	frame(8);
	check(first && core_opt_value(first) != before, "right changes the value in the core");

	/*
	  Where it is *kept* changed with per-game overrides, and this is the check that used
	  to say the opposite.

	  A game is running here, so the change belongs to that game: writing it into
	  <CORE>.CFG would apply it to every game the core ever loads, which is the thing
	  per-game settings exist to stop. So the shared config must be left alone, and the
	  choice must be findable against the game instead.
	  assert_per_game_core_options() covers the rest of it.
	*/
	check(harness_cfg_saves() == saves, "and leaves the core's shared config alone");
	check(first && core_opt_per_game(first), "because it belongs to the game that is running");

	press(KEY_ESC, 12);
	frame(6);
	press(KEY_MENU, 16);
	frame(8);
	harness_set_confstr(1);
}

// How many pixels of exactly this colour are on screen. Used to prove a marker is
// really drawn rather than only computed: a hash says "something changed", this says
// "the per-game green is there".
static int px_count(uint32_t col)
{
	const uint32_t *fb = harness_fb_shown();
	if (!fb) return 0;

	int n = 0, w = gfx_w(), h = gfx_h();
	for (int i = 0; i < w * h; i++) if (fb[i] == col) n++;
	return n;
}

static int sysidx_of(const char *id)
{
	for (int i = 0; i < lib_sys_count(); i++) if (!strcmp(lib_sys(i)->id, id)) return i;
	return -1;
}



/*
  The SNAC-ownership rows are staged until the menu closes, and the reason is the pad.

  Choosing SNAC-port1 is how you tell the core to read the SNAC port itself - so writing it
  the instant it is selected destroys the uinput pad the player just used to select it, and
  they cannot move off the value they landed on. Reported from hardware: the earlier fix
  (releasing held buttons before the device goes) stopped the value cycling by itself, but
  could not fix this, because the pad is genuinely gone by design. The problem was *when*.

  What has to hold: the row shows the player's choice immediately, the core is NOT told
  while the menu is up, and it is told on the way out.
*/
static void assert_snac_row_is_staged()
{
	printf("\n== the SNAC rows wait for the way out ==\n");

	harness_set_menu_core(0);
	harness_set_confstr_table(0);
	harness_set_confstr(6);
	harness_set_osd_mask(0x0000);
	core_opts_scan();
	core_opts_pending_clear();

	// A Pad1-shaped row on the fixture core, and an ordinary row to contrast with.
	static const char *psx[] =
	{
		"PSXSTAGE", "FS1,BIN,Load ROM",
		"O[48:45],Pad1,Dualshock,Off,Digital,Analog,GunCon,NeGcon,Wheel-NegCon,"
			"Wheel-Analog,Mouse,Justifier,SNAC-port1,Analog Joystick,Popn",
		"O[40:39],System Type,Auto,NTSC,PAL",
		0
	};
	harness_set_confstr_table(psx);
	core_opts_scan();

	const core_opt *pad1 = 0, *sys = 0;
	for (int i = 0; i < core_opts_count(); i++)
	{
		const core_opt *o = core_opt_at(i);
		if (!strcasecmp(o->name, "Pad1")) pad1 = o;
		if (!strcasecmp(o->name, "System Type")) sys = o;
	}
	check(pad1 && sys, "the fixture offers Pad1 and an ordinary row beside it");
	if (!pad1 || !sys) { harness_set_confstr(1); return; }

	harness_set_opt("[48:45]", 0, 0);
	harness_set_opt("[40:39]", 0, 0);

	// An ordinary row still applies at once - that is what the footer promises.
	core_opt_set(sys, 2);
	check(harness_opt_val("[40:39]", 0) == 2, "an ordinary core option is still written at once");

	// Pad1 is not.
	core_opt_set(pad1, 10);                    // SNAC-port1
	check(harness_opt_val("[48:45]", 0) == 0,
		"choosing SNAC-port1 does not reach the core while the menu is open");
	check(core_opt_value(pad1) == 10,
		"but the row shows the choice, so the player sees what they picked");
	check(core_opts_pending(), "and it is held as pending");

	/*
	  Walking on through the values must not leak any of them to the core either - that is
	  the whole point, since every one of them would have been a separate hand-over.
	*/
	core_opt_set(pad1, 11);
	core_opt_set(pad1, 12);
	check(harness_opt_val("[48:45]", 0) == 0, "nor does walking past it through other values");
	check(core_opt_value(pad1) == 12, "the row still tracks the cursor");

	// The way out is what applies it.
	core_opts_pending_apply();
	check(harness_opt_val("[48:45]", 0) == 12, "closing the menu writes the staged value");
	check(!core_opts_pending(), "and nothing is left pending");

	// And abandoning it leaves the core untouched.
	harness_set_opt("[48:45]", 0, 0);
	core_opt_set(pad1, 10);
	core_opts_pending_clear();
	core_opts_pending_apply();
	check(harness_opt_val("[48:45]", 0) == 0, "a discarded choice never reaches the core");

	harness_set_confstr(1);
	harness_set_menu_core(1);
}

/*
  The exact shape a player reported from a television: SMS, a game running, 16 rows on the
  System & Sound page, and the cursor walking off the bottom into nothing.

  Worth its own section rather than another size in the loop above, because the failure is
  length-specific. With 28 rows the window jumps past the cursor on the first press and the
  scroll looks perfect; the defect only shows where list_fit()'s count and the number of
  rows the panel can actually draw differ by one, which on a 240p panel is around sixteen.
  His screenshots: fifteen rows drawn, no scrollbar, and after one DOWN no row highlighted
  at all - the cursor on row 15 with the window still at 0.
*/
static void assert_sms_shaped_page_scrolls()
{
	printf("\n== the page length that broke on a television ==\n");

	{
		FILE *f = fopen("/tmp/classicui_current", "wt");
		if (f) { fprintf(f, "gb\nTetris (World).gb\n"); fclose(f); }
	}
	harness_set_confstr(10);
	harness_set_menu_core(0);
	harness_set_fb_supported(1);
	harness_set_osd_visible(0);

	const int was_prof = cfg.classicui_profile;
	cfg.classicui_profile = 3;                     // 240p, the profile he is on
	harness_set_fb(320, 240);
	gfx_shutdown();
	theme_update(320, 240, 3);

	chome_leave();
	chome_handle(0);
	if (chome_ingame_active()) press(KEY_MENU, 14);
	frame(6);
	press(KEY_MENU, 20);
	for (int i = 0; i < 40 && lib_scanning(); i++) frame(2);
	frame(16);

	press(KEY_UP, 14);
	for (int i = 0; i < 6; i++) press(KEY_RIGHT, 8);
	frame(8);
	press(KEY_ENTER, 18);
	frame(10);

	int pic = core_opts_tier_count(CO_TIER_PICTURE);
	int sys = core_opts_tier_count(CO_TIER_SYSTEM);
	printf("  picture %d, system %d, risky %d\n", pic, sys, core_opts_tier_count(CO_TIER_RISKY));
	check(pic == 6 && sys == 16, "the fixture reproduces his page lengths");

	// Off the Picture page onto System & Sound, the way the "More" row does it.
	for (int i = 0; i < pic; i++) press(KEY_DOWN, 6);
	press(KEY_ENTER, 14);
	frame(10);

	/*
	  Now walk down one row at a time and require the highlight to stay on screen at every
	  single step. One press at a time on purpose: his report is that the *first* press past
	  the fold loses the cursor, and a loop that jumps to the end would step over it.
	*/
	int lost = -1;
	for (int r = 1; r <= sys; r++)
	{
		press(KEY_DOWN, 6);
		frame(6);
		if (sel_bar_y() < 0 && lost < 0) lost = r;
	}
	if (lost >= 0) printf("  the highlight disappeared on press %d of %d\n", lost, sys);
	dump("core-opts-sms-shape-240p-bottom");
	check(lost < 0, "every row of a 16-row page keeps the cursor on screen at 240p");

	press(KEY_ESC, 12);
	frame(6);
	cfg.classicui_profile = (uint8_t)was_prof;
	harness_set_confstr(1);
	harness_set_menu_core(1);
	chome_leave();
}

/*
  A core-options list longer than the panel, at every profile.

  The row-drop guard at the end of this file is worth nothing without a list that can
  actually overflow, and until now no fixture had one - every modelled core is short enough
  to fit at 240p, so the guard would have reported a clean sweep over a defect that was
  live on hardware. fake_confstr_long is the real PSX list, 30-odd settings on one page
  against roughly fifteen that fit at 240p.

  What has to hold: the last row is reachable, the highlight is on screen when the cursor is
  on it, and nothing is drawn past the panel edge. The first two are what a player does; the
  third is what the guard sees. All three at 240p especially, which is the television this
  front-end is for and the profile where the panel is smallest.
*/
static void assert_long_core_list_scrolls()
{
	printf("\n== a core list longer than its panel ==\n");

	{
		FILE *f = fopen("/tmp/classicui_current", "wt");
		if (f) { fprintf(f, "gb\nTetris (World).gb\n"); fclose(f); }
	}
	harness_set_confstr(9);
	harness_set_menu_core(0);
	harness_set_fb_supported(1);
	harness_set_osd_visible(0);

	struct { int w, h, force; const char *name; } canv[] = {
		{ 1280, 720, 1, "hd" },
		{  640, 480, 2, "sd" },
		{  320, 240, 3, "lo" },
	};

	const int was_prof = cfg.classicui_profile;

	for (int c = 0; c < 3; c++)
	{
		cfg.classicui_profile = (uint8_t)canv[c].force;
		harness_set_fb(canv[c].w, canv[c].h);
		gfx_shutdown();
		theme_update(canv[c].w, canv[c].h, canv[c].force);

		/*
		  Close it if the previous profile left it open. Without this the MENU press below
		  toggles the already-open menu shut and the profile is silently never tested - which
		  is what happened at 640x480, and it read as a drawing bug rather than a test bug.
		*/
		chome_leave();
		chome_handle(0);
		if (chome_ingame_active()) press(KEY_MENU, 14);
		frame(6);
		press(KEY_MENU, 20);
		for (int i = 0; i < 40 && lib_scanning(); i++) frame(2);
		frame(16);

		// To the bar, then along it to the running core's entry, as a player would.
		press(KEY_UP, 14);
		for (int i = 0; i < 6; i++) press(KEY_RIGHT, 8);
		frame(8);
		press(KEY_ENTER, 18);
		frame(10);

		int n = core_opts_tier_count(CO_TIER_SYSTEM);
		printf("  %s: %d rows on the System page\n", canv[c].name, n);

		/*
		  NOT cleared here, though it was. Clearing per profile threw away every record from
		  the two profiles before it AND everything earlier in the run, leaving the run-wide
		  guard at the end of this file judging only the last profile - most of its evidence
		  wiped by the section that needed it most. The per-profile assertion below reads the
		  count as a total instead, which is the same statement for a log that only ever grows.
		*/

		/*
		  Down past the end. RIGHT is not used here on purpose - it would change values on
		  the way through, and this is about reaching rows, not setting them.
		*/
		for (int i = 0; i < n + 4; i++) press(KEY_DOWN, 6);
		frame(10);

		/*
		  Park the cursor on each SNAC row and draw, so their footer help goes through
		  gfx_clip() and the run-wide clipped-copy guard measures it. That help is the only
		  copy of ours chosen by a *value* rather than by a screen, and nothing had ever drawn
		  it - so a sentence wider than the panel was invisible to every check we have.
		*/
		for (int pass = 0; pass < 2; pass++)
		{
			for (int i = 0; i < core_opts_tier_count(CO_TIER_SYSTEM); i++)
			{
				const core_opt *o = core_opt_tier_at(CO_TIER_SYSTEM, i);
				if (!o) break;
				if (strcasecmp(o->name, "Pad1") && strcasecmp(o->name, "Pad2")
					&& strcasecmp(o->name, "SNAC MemCard")) continue;

				// pass 0 draws the core's own default; pass 1 draws the SNAC value.
				harness_set_opt(o->spec, pass ? (uint32_t)(o->nvals - 4) : 0, o->ex);

				// Home, then down to it - no test-only accessor, just the keys a player has.
				for (int u = 0; u < core_opts_tier_count(CO_TIER_SYSTEM) + 2; u++) press(KEY_UP, 3);
				for (int d = 0; d < i; d++) press(KEY_DOWN, 3);
				frame(4);
			}
		}

		int bar = sel_bar_y();
		printf("  %s: highlight at y=%d after walking to the bottom\n", canv[c].name, bar);
		check(bar >= 0, "the cursor on the last row is drawn, not left below the panel");
		check(chome_rowdrop_n() == 0, "and no row of the list is drawn past the panel edge");

		{ char nm[64]; snprintf(nm, sizeof(nm), "core-options-long-%s", canv[c].name); dump(nm); }

		press(KEY_ESC, 14);
		frame(6);
	}

	cfg.classicui_profile = (uint8_t)was_prof;
	harness_set_confstr(1);
	harness_set_menu_core(1);
	chome_leave();
	/*
	  The log is NOT cleared here. Anything this section recorded belongs to the run-wide
	  guard at the end - clearing it on the way out would erase exactly the evidence that
	  guard exists to report, and did: with the windowing reverted this section failed while
	  the global check still said every list was clean.
	*/
}

/*
  Who owns the SNAC port, which is now derived from the core's own options rather than set.

  The rule: our reader drives the port only when the running core has not claimed it. That
  is the whole arbitration, and it has to be right for two quite different reasons. Get it
  wrong towards "we own it" and two readers drive the same pins at once - the one outcome
  here that can damage hardware, since our reader actively drives clock, command and
  attention at 250 kHz and another console's adapter maps those pins somewhere else. Get it
  wrong towards "the core owns it" and a player merely loses a pad until they look at the
  option, which is why that is the direction to fail in.

  All six SNAC-capable cores are modelled below because they spell it two different ways
  and a table of core names was the thing worth avoiding. PSX/N64/SMS name SNAC in the
  *value*; NES/Mega Drive/SNES make the option itself the switch. Note every one of them
  defaults to its first value, which is never SNAC - so the no-configuration case is
  "our reader owns the port", which is exactly what makes a PlayStation pad work in every
  core without the player setting anything.

  The ID check is here too. The reader in the fabric accepts any answer that is not 0xFF
  and never checks the 0x5A a real pad sends, and the buttons reach us inverted - so a
  device answering with zeroes would arrive as a pad with every button held down for ever,
  driving the menu. That is what another console's adapter on the same port can look like.

  What this cannot reach: whether the fabric and a real pad agree. snacpad.cpp is in this
  build now, but the reader it talks to is modelled here, and on this project a
  keyboard-driven host test has already proved nothing about a pad once. The arbitration is
  covered; the electrical half still wants the device.
*/
/*
  One SNAC poll, with the clock moved past both of the poll's own back-offs.

  snacpad_poll() rate-limits itself - SNAC_POLL_MS while it is reading pads, SNAC_RETRY_MS
  once it wants nothing - so two calls in the same millisecond are one poll and a silent
  no-op. Advancing past the longer of the two is what makes each call below mean a poll.
*/
static void snac_tick()
{
	harness_advance(1100);
	snacpad_poll();
}

static void assert_snac_ownership()
{
	printf("\n== who owns the SNAC port ==\n");

	// Not the menu core: core_owns_snac() answers 0 there, so this must not inherit it.
	harness_set_menu_core(0);

	// A core publishing each of the two idioms, alongside settings that must not match.
	static const char *snac_psx[] =
	{
		"PSX", "FS1,BIN,Load ROM",
		"D8O[48:45],Pad1,Dualshock,Off,Digital,Analog,GunCon,NeGcon,Wheel-NegCon,"
			"Wheel-Analog,Mouse,Justifier,SNAC-port1,Analog Joystick,Popn",
		"D8h0O[66],SNAC MemCard,Virtual,Real",
		0
	};
	static const char *snac_nes[] =
	{
		"NES", "FS1,BIN,Load ROM",
		"P2oJK,SNAC,Off,Controllers,Zapper,3D Glasses",
		0
	};
	static const char *snac_n64[] =
	{
		"N64", "FS1,BIN,Load ROM",
		"O[51:49],Pad 1 Type,N64Pad,None,ControllerPak,RumblePak,SNAC,TransferPak,Keyboard",
		"P3O[91],SNAC Compare,Off,On",
		0
	};
	static const char *snac_sms[] =
	{
		"SMS", "FS1,BIN,Load ROM",
		"P2oNO,USERIO,Off,SNAC,Gear2Gear",
		0
	};
	static const char *snac_snes[] = { "SNES", "FS1,BIN,Load ROM", "P2O8,SNAC,No,Yes", 0 };
	static const char *snac_none[] = { "GAMEBOY", "FS1,BIN,Load ROM", "OEF,System,Auto,GB", 0 };

	struct { const char **tbl; const char *spec; int ex; int snacval; const char *what; } cores[] =
	{
		{ snac_psx,  "[48:45]", 0, 10, "the PSX naming SNAC-port1 in Pad1" },
		{ snac_nes,  "JK",      1, 1,  "the NES making the option itself the switch" },
		{ snac_n64,  "[51:49]", 0, 4,  "the N64 offering SNAC among its pad types" },
		{ snac_sms,  "NO",      1, 1,  "the SMS calling the option USERIO" },
		{ snac_snes, "8",       0, 1,  "the SNES with a plain No/Yes" },
	};

	cfg.snac_pad = 1;
	cfg.snac_device = 0;

	for (unsigned c = 0; c < sizeof(cores) / sizeof(cores[0]); c++)
	{
		harness_reset_snac();
		harness_set_confstr_table(cores[c].tbl);

		// The core's own default: its first value, which is never SNAC.
		harness_set_opt(cores[c].spec, 0, cores[c].ex);
		snacpad_init();
		snac_tick();
		check(harness_snac_last_want() == 1, cores[c].what);

		/*
		  Now the player picks SNAC in the core's own options, mid-session and without a
		  relaunch - which is the case that matters and the one the old probe-and-settle
		  code could not do at all. No snacpad_init() here on purpose: the reader is
		  already enabled, so this asserts the *release*, that we actively tell the
		  fabric to let go rather than leaving two readers driving the port.
		*/
		harness_set_opt(cores[c].spec, (uint32_t)cores[c].snacval, cores[c].ex);
		snac_tick();
		check(harness_snac_last_want() == 0, "  and lets go when it is chosen mid-session");

		// And back again, without a relaunch either way.
		harness_set_opt(cores[c].spec, 0, cores[c].ex);
		snac_tick();
		check(harness_snac_last_want() == 1, "  and takes it back when it is unchosen");
	}

	/*
	  The two decoys, which is where an over-eager name match would go wrong: both begin
	  with "SNAC" and neither hands the port over.
	*/
	harness_reset_snac();
	harness_set_confstr_table(snac_psx);
	harness_set_opt("[48:45]", 0, 0);
	harness_set_opt("[66]", 1, 0);            // SNAC MemCard = Real
	snacpad_init();
	snac_tick();
	check(harness_snac_last_want() == 1, "\"SNAC MemCard\" does not hand the port over");

	harness_reset_snac();
	harness_set_confstr_table(snac_n64);
	harness_set_opt("[51:49]", 0, 0);
	harness_set_opt("[91]", 1, 0);            // SNAC Compare = On
	snacpad_init();
	snac_tick();
	check(harness_snac_last_want() == 1, "nor does \"SNAC Compare\"");

	// A core with no SNAC option at all: nothing claims it, so we read it.
	harness_reset_snac();
	harness_set_confstr_table(snac_none);
	snacpad_init();
	snac_tick();
	check(harness_snac_last_want() == 1, "a core with no SNAC option leaves the port to us");

	/*
	  snac_device, the one thing that cannot be derived. A SuperDock's bypass switch routes
	  the bus to an extension port taking any console's adapter and nothing readable moves,
	  so this is asked once and believed - and when it says "not a PSX pad" we must not
	  drive the port in any core, or in the menu.
	*/
	harness_reset_snac();
	harness_set_confstr_table(snac_none);
	snacpad_init();
	snac_tick();
	check(harness_snac_last_want() == 1, "with a PSX pad on the port we read it");

	// Told what is really on the port, we let go of it and stay off - no relaunch needed.
	cfg.snac_device = 1;
	snac_tick();
	check(harness_snac_last_want() == 0, "snac_device=1 keeps us off the port entirely");
	snac_tick();
	check(harness_snac_last_want() == 0, "and off it on every poll after that");
	cfg.snac_device = 0;

	// And snac_pad=0 is still the master switch.
	cfg.snac_pad = 0;
	harness_reset_snac();
	harness_set_confstr_table(snac_none);
	snacpad_init();
	snac_tick();
	check(harness_snac_last_want() == -1, "snac_pad=0 does not touch SPI at all");
	cfg.snac_pad = 1;

	/* ------------------------------------------------ the ID sanity check --- */

	harness_reset_snac();
	harness_set_confstr_table(snac_none);
	snacpad_init();

	harness_set_snac_pad(0, 1, 0x41, 0);      // a digital pad
	snac_tick();
	check(snacpad_test_present(0), "a digital pad (id 41) is accepted");

	harness_set_snac_pad(0, 1, 0x73, 0);      // an analog pad
	snac_tick();
	check(snacpad_test_present(0), "an analog pad (id 73) is accepted");

	harness_set_snac_pad(0, 1, 0x53, 0);
	snac_tick();
	check(snacpad_test_present(0), "and analog mode 2 (id 53)");

	/*
	  id 00 with every button set is the shape the inversion produces from a device that
	  answers with zeroes - another console's adapter, on the reading where its lines idle
	  low. Before the check this was a pad holding all sixteen buttons down for ever.
	*/
	harness_set_snac_pad(0, 1, 0x00, 0xFFFF);
	snac_tick();
	check(!snacpad_test_present(0), "id 00 is refused, not taken as every button held down");

	harness_set_snac_pad(0, 1, 0x12, 0);      // a PSX mouse: real, but not decodable here
	snac_tick();
	check(!snacpad_test_present(0), "and so is an id this reader cannot decode");

	harness_set_snac_pad(0, 1, 0x41, 0);
	snac_tick();
	check(snacpad_test_present(0), "a real pad after one is still accepted");

	// A core built from an older sys, which answers without the magic word.
	harness_reset_snac();
	harness_set_snac_reader(0);
	snacpad_init();
	snac_tick();
	check(!snacpad_test_present(0) && !snacpad_test_present(1),
		"a core with no reader in its sys reports no pads");

	/* ------------------------------------------- and they are reachable now --- */

	/*
	  The rows the arbitration reads have to be rows the player can get at, or "set Pad1 to
	  SNAC-port1" is advice about a screen we do not offer. They were hidden as ours while
	  snac_psx existed; with that gone they are the control itself.
	*/
	harness_set_confstr_table(snac_psx);
	harness_set_osd_mask(0x0001);              // so the h0-masked SNAC MemCard row applies
	core_opts_scan();

	int has_pad1 = 0, has_memcard = 0, has_compare = 0;
	for (int i = 0; i < core_opts_count(); i++)
	{
		const char *nm = core_opt_at(i)->name;
		if (!strcasecmp(nm, "Pad1")) has_pad1 = 1;
		if (!strcasecmp(nm, "SNAC MemCard")) has_memcard = 1;
		if (!strcasecmp(nm, "SNAC Compare")) has_compare = 1;
	}
	check(has_pad1, "Pad1 is offered in the core screen, not hidden as ours");
	check(has_memcard, "and so is SNAC MemCard");

	harness_set_confstr_table(snac_n64);
	core_opts_scan();
	has_compare = 0;
	for (int i = 0; i < core_opts_count(); i++)
		if (!strcasecmp(core_opt_at(i)->name, "SNAC Compare")) has_compare = 1;
	check(!has_compare, "while SNAC Compare stays hidden, being a debug aid that hands over nothing");

	harness_set_osd_mask(0x0000);
	harness_reset_snac();
	harness_set_confstr(1);

	/*
	  And left quiet, which matters now in a way it did not before.

	  This section ends with a core that has no reader in it and cfg.snac_pad on - which is
	  precisely the state the Controllers screen has been taught to put a line on the glass
	  about. Left standing, every later section that opens that screen would compose it with
	  an amber warning on it, and the section that would have found that out is the clipped-
	  copy sweep near the end of the run - twenty sections and several thousand checks away
	  from the code that caused it.

	  A settled module in this tree is one whose state the next section can ignore. Since
	  snacpad.cpp acquired readers outside itself, that now includes cfg.snac_pad.
	*/
	cfg.snac_pad = 0;
	snacpad_init();
}

/*
  "O" and "o" name different status words, and this screen has to keep them apart.

  Found while wiring the SNAC ownership rule, which needs to read the NES's "P2oJK" SNAC
  row. Every bit-addressing call in chome_core.cpp passed ex=0, so an "o" option was read
  from and written to the bits 32 *below* the ones it names. The stock OSD derives ex from
  the letter (menu.cpp:2593); this file did not.

  Why it survived this suite for so long, which is the part worth keeping: the fixture core
  had no "o" option at all, and the stub's option map was keyed on the spec string alone.
  With ex dropped the wrong slot was then used *consistently* - every read agreed with every
  write, so the screen was self-coherent and no assertion could tell. It takes a core that
  publishes both forms of the same letter to make the collision visible, which real cores do
  constantly: the SMS lists Z80 Speed as "H8o8" beside its "O" settings.

  What it cost a player: the Genesis "Vertical Crop" row, the SNES "SuperFX FastROM" row,
  "SMS BIOS", "Mapper", "Orientation" and a dozen more showed a value belonging to some
  other setting, and changing the row changed that other setting instead. Silent both ways.
  Bracket specs were never affected - they are absolute and take no ex.
*/
static void assert_core_option_word_forms()
{
	printf("\n== the two status words ==\n");

	// core_opts_scan() returns nothing on the menu core, so say which core this is.
	harness_set_menu_core(0);
	harness_set_confstr(8);
	harness_set_osd_mask(0x0000);
	core_opts_scan();

	const core_opt *lock = 0, *z80 = 0, *mapper = 0, *systype = 0;
	for (int i = 0; i < core_opts_count(); i++)
	{
		const core_opt *o = core_opt_at(i);
		if (!strcasecmp(o->name, "Region Lock")) lock = o;
		if (!strcasecmp(o->name, "Z80 Speed")) z80 = o;
		if (!strcasecmp(o->name, "Mapper")) mapper = o;
		if (!strcasecmp(o->name, "System Type")) systype = o;
	}

	check(lock && z80 && mapper && systype, "a core publishing both spec forms is read");
	if (!lock || !z80 || !mapper || !systype) { harness_set_confstr(1); return; }

	check(lock->ex == 0, "an \"O\" option is tagged as the lower word");
	check(z80->ex == 1, "an \"o\" option is tagged as the upper word");
	check(mapper->ex == 1, "and so is a multi-bit \"o\" option");
	check(systype->ex == 0, "a bracket spec stays the lower word, brackets being absolute");

	// The crux. "O8" and "o8" arrive at the bit layer as the same string, "8".
	harness_set_opt("8", 0, 0);
	harness_set_opt("8", 0, 1);

	core_opt_set(z80, 1);
	check(core_opt_value(z80) == 1, "setting an \"o\" option reads back as itself");
	check(core_opt_value(lock) == 0, "and leaves the \"O\" option sharing its letter alone");
	check(harness_opt_val("8", 1) == 1, "the upper word is the one that moved");
	check(harness_opt_val("8", 0) == 0, "and the lower word did not move at all");

	core_opt_set(lock, 1);
	check(core_opt_value(lock) == 1, "setting the \"O\" option reads back as itself too");
	check(core_opt_value(z80) == 1, "with the \"o\" option still on the value it was given");

	harness_set_confstr(1);
}

/*
  Core settings kept for one game instead of for the whole core.

  <CORE>.CFG is the core's global config: PSX's Widescreen Hack written there flatters
  a 3D racer and ruins every 2D game on the card. So a change made while a game is
  running is remembered against that game and re-applied when it next starts.

  What has to hold, and what each of these would catch if it stopped holding:

  - the change goes to the game, not to <CORE>.CFG;
  - it is visible as such, and undoable from the same screen;
  - it reaches the core at the next launch of *that* game and no other;
  - it survives a core update that renumbers the core's own value lists;
  - it can never be created for anything the front-end owns itself.
*/
static void assert_per_game_core_options()
{
	printf("\n== core settings kept for one game ==\n");

	int gb = sysidx_of("gb");
	check(gb >= 0, "the Game Boy system is in the table");

	const char *game_a = "Tetris (World).gb";
	const char *game_b = "Zelda - Oracle of Ages (Europe).gbc";

	{
		FILE *f = fopen("/tmp/classicui_current", "wt");
		if (f) { fprintf(f, "gb\n%s\n", game_a); fclose(f); }
	}

	harness_set_menu_core(0);
	harness_set_fb_supported(1);
	harness_set_fb(1280, 720);
	gfx_shutdown();
	theme_update(1280, 720, 1);
	harness_set_confstr(6);
	harness_set_osd_mask(0x0000);
	harness_set_osd_visible(0);

	/*
	  Start from no overrides for this game. Deleting the file is not enough - it is
	  read once per session and earlier sections have already put a record in it - so
	  the store is cleared through the same call the screen's X uses, which also proves
	  it clears something.
	*/
	core_opts_scan();
	core_opts_bind_game(core_opts_game_key(gb, game_a));
	for (int i = 0; i < core_opts_count(); i++) core_opt_drop_for_game(core_opt_at(i));

	// A known starting point for the option under test, as if the core had just read it
	// out of its own config.
	harness_set_opt("[54:53]", 0);                     // Widescreen Hack = Off

	chome_handle(0);
	if (chome_ingame_active()) press(KEY_MENU, 14);
	frame(6);
	press(KEY_MENU, 20);
	for (int i = 0; i < 40 && lib_scanning(); i++) frame(2);
	frame(16);
	check(chome_ingame_active(), "the menu is up over the running game");

	press(KEY_UP, 14);
	for (int i = 0; i < 6; i++) press(KEY_RIGHT, 8);
	press(KEY_ENTER, 18);
	frame(10);

	/*
	  Widescreen Hack is the second row of the picture page, and it is the option Dinofly
	  named: the one that is right for one game and wrong for the next.
	*/
	press(KEY_DOWN, 14);
	frame(8);
	const core_opt *ws = core_opt_tier_at(CO_TIER_PICTURE, 1);
	check(ws && !strcasecmp(ws->name, "Widescreen Hack"), "the cursor is on Widescreen Hack");

	int green0 = px_count(COL_GREEN);
	int saves = harness_cfg_saves();

	press(KEY_RIGHT, 14);
	press(KEY_RIGHT, 14);
	frame(10);

	ws = core_opt_tier_at(CO_TIER_PICTURE, 1);
	check(ws && core_opt_value(ws) == 2, "two presses put it on 16:9");
	check(harness_cfg_saves() == saves, "and nothing was written to the core's shared config");
	check(ws && core_opt_per_game(ws), "the game keeps the choice instead");

	/*
	  And says so. Without a mark the screen shows a setting that applies to one game
	  and one that applies to all of them in exactly the same ink, and the player has no
	  way to tell which of their games they just changed.
	*/
	int green1 = px_count(COL_GREEN);
	printf("  per-game green pixels: %d -> %d\n", green0, green1);
	check(green1 > green0, "and the row is marked on screen as this game's own");

	// It is a file, not a session's memory.
	{
		struct stat st;
		check(!stat(ROOT "/config/classicui_coreopts.cfg", &st) && st.st_size > 0,
			"the choice is on the card, not just in this session");
	}

	dump("core-options-per-game");

	/*
	  Undo, from the screen that made it. X puts the shared value back and clears the
	  mark - an override that can only be removed by knowing which file it lives in is
	  a trap, and one that clears without the row visibly moving is an undo nobody
	  believes happened.
	*/
	press(KEY_TAB, 14);
	frame(10);
	ws = core_opt_tier_at(CO_TIER_PICTURE, 1);
	check(ws && !core_opt_per_game(ws), "X gives the setting back to every game");
	check(ws && core_opt_value(ws) == 0, "and puts the value the core booted with back");
	check(px_count(COL_GREEN) == green0, "with the mark gone from the row");

	// Set it up again, this time to keep: the launch checks below need an override.
	press(KEY_RIGHT, 14);
	press(KEY_RIGHT, 14);
	frame(10);
	ws = core_opt_tier_at(CO_TIER_PICTURE, 1);
	check(ws && core_opt_per_game(ws) && core_opt_value(ws) == 2, "and it can be set again");

	press(KEY_ESC, 12);
	frame(6);
	press(KEY_MENU, 16);
	frame(8);

	/*
	  Now the launches. core_opts_apply_for_game() is what chome_core_boot() calls once
	  the core is up and its own config has been read, so the core is put back to that
	  state first - Widescreen Hack Off, as <CORE>.CFG has it.
	*/
	/*
	  The wiring first, because it is the part that cannot be checked twice.
	  chome_core_boot() is what HandleUI() calls once per core on the device, and it
	  runs at most once per process - so this is the only place it can be exercised.
	  Without it every other check here still passes and the feature is inert on
	  hardware.

	  It also applies a video look armed at launch, and an earlier section left one
	  armed. That belongs to the section that tests it, so it is taken out of the way
	  rather than fired from here.
	*/
	unlink("/tmp/classicui_preset");
	harness_set_opt("[54:53]", 0);
	chome_core_boot();
	check(harness_opt_val("[54:53]") == 2, "a core coming up applies it from chome_core_boot()");

	harness_set_opt("[54:53]", 0);
	int moved = core_opts_apply_for_game(gb, game_a);
	check(moved == 1, "starting that game again applies its own setting");
	check(harness_opt_val("[54:53]") == 2, "the core comes up on 16:9 for it");

	/*
	  The whole point, in one check. Another game on the same core must come up on the
	  core's own value: a per-game setting that leaked into the next game would be
	  indistinguishable from writing <CORE>.CFG, which is what this replaces.
	*/
	harness_set_opt("[54:53]", 0);
	check(core_opts_apply_for_game(gb, game_b) == 0, "another game on the same core has none");
	check(harness_opt_val("[54:53]") == 0, "so it comes up on the core's own value");

	/*
	  A core update that inserts a value into one of its own lists. PSX's Widescreen Hack
	  grew from two entries to four; anything that stored the choice as an index would
	  now re-apply the wrong setting. fake_confstr_opts_v2 puts 5:3 in front of 16:9, so
	  16:9 moves from index 2 to index 3 and the check is that the player still gets 16:9.
	*/
	harness_set_confstr(7);
	harness_set_opt("[54:53]", 0);
	check(core_opts_apply_for_game(gb, game_a) == 1, "the setting survives a core update");
	check(harness_opt_val("[54:53]") == 3, "and still means 16:9 after its list was renumbered");
	harness_set_confstr(6);

	/*
	  Nothing the front-end owns may become per-game, whatever asks. These never reach
	  the option table so the screen cannot offer them - this is the second lock, on the
	  store itself, because "Pause when OSD is open" is the mechanism the in-game menu's
	  freeze depends on and a per-game copy of it would change what the menu button does
	  on one game only.
	*/
	{
		core_opt owned;
		memset(&owned, 0, sizeof(owned));
		snprintf(owned.name, sizeof(owned.name), "Pause when OSD is open");
		snprintf(owned.spec, sizeof(owned.spec), "Q");
		snprintf(owned.vals[0], sizeof(owned.vals[0]), "Off");
		snprintf(owned.vals[1], sizeof(owned.vals[1]), "On");
		owned.nvals = 2;

		core_opts_bind_game(core_opts_game_key(gb, game_a));
		core_opt_keep_for_game(&owned, 1, 0);
		check(!core_opt_per_game(&owned), "a setting the front-end owns cannot be made per-game");
	}

	/*
	  And the shelf case: a core loaded behind our back, so the launch record names no
	  game we can trust. There is nothing to hang a choice on, so it goes into the
	  core's own config exactly as it did before any of this.
	*/
	unlink("/tmp/classicui_current");
	press(KEY_MENU, 20);
	frame(12);
	check(chome_ingame_active(), "the menu opens over an unidentified core");
	check(core_opts_bound_game() == 0, "with no game to keep settings for");

	press(KEY_UP, 14);
	for (int i = 0; i < 6; i++) press(KEY_RIGHT, 8);
	press(KEY_ENTER, 18);
	frame(10);

	saves = harness_cfg_saves();
	press(KEY_RIGHT, 14);
	frame(10);
	check(harness_cfg_saves() > saves, "a change there still goes to the core's own config");

	/*
	  And not against the game that happened to be running last. If the binding were
	  simply never cleared, the change above would have been filed under Tetris - which
	  looks like nothing at all until the player starts Tetris and finds a setting they
	  made in another core's menu.
	*/
	const core_opt *any = core_opt_tier_at(CO_TIER_PICTURE, 0);
	core_opts_bind_game(core_opts_game_key(gb, game_a));
	check(any && !core_opt_per_game(any), "and not against the game that was running before");
	core_opts_bind_game(0);

	press(KEY_ESC, 12);
	frame(6);
	press(KEY_MENU, 16);
	frame(8);

	harness_set_confstr(1);
}

/*
  One option's value as the shared config on the card holds it, or -1 when there is no
  shared config at all.

  Read out of the file with the firmware's own bit parser, so the placement is checked and
  not assumed: a promotion that put the right number in the wrong bits would come back as
  the wrong number here, and one that rebuilt the file instead of editing it would come
  back as zero for everything it did not mean to write.
*/
static int shared_cfg_field(const char *spec)
{
	unsigned char img[64];
	memset(img, 0, sizeof(img));

	char p[1024];
	snprintf(p, sizeof(p), "%s/config/%s", ROOT, user_io_create_config_name(1));
	FILE *f = fopen(p, "rb");
	if (!f) return -1;
	int len = (int)fread(img, 1, sizeof(img), f);
	fclose(f);

	int start = 0, end = 0;
	int size = user_io_status_bits(spec, &start, &end);
	if (!size || end / 8 >= len) return -1;

	uint32_t x = ((uint32_t)img[end / 8] << 8) | img[start / 8];
	x >>= start % 8;
	return (int)(x & ~(0xffffffffu << size));
}

static void forget_shared_cfg()
{
	char p[1024];
	snprintf(p, sizeof(p), "%s/config/%s", ROOT, user_io_create_config_name(1));
	unlink(p);
}

/*
  A core setting handed to the whole system, from inside a game.

  Dinofly asked for both halves: a change made while a game is running belongs to that game -
  which the section above is entirely about keeping true - and one deliberate press makes it
  the value every game on that core gets instead.

  Why that needed a mechanism rather than a call is what most of these checks are about.
  <CORE>.CFG is written by handing user_io_status_save() the *live* status word, and while a
  game with overrides is running that word carries all of them. Saving it to share one
  setting would push the rest out to every game on the card - exactly the leak per-game
  settings exist to stop, and the reason README.md carried this direction as impossible. So
  the promotion edits the file: this option's bits, nothing else.

  What has to hold:

  - the promoted value is really in the shared config, in that option's own bits;
  - the running game's *other* overrides are not - the crux, and why two of them are set up
    before anything is promoted;
  - those other overrides are still the game's afterwards, not quietly lost;
  - promoting a second option keeps the first, so the file is edited and not rebuilt;
  - the whole status word is never written, which is the mechanism check rather than an
    effect of it;
  - per-game is still what a plain value change does, for anyone who never presses it;
  - the prompt exists only where the press does something;
  - and the player is told, because a star going away is also what undoing one looks like.
*/
static void assert_core_option_for_all_games()
{
	printf("\n== a core setting given to the whole system ==\n");

	int gb = sysidx_of("gb");
	const char *game_a = "Tetris (World).gb";
	const char *game_b = "Zelda - Oracle of Ages (Europe).gbc";

	{
		FILE *f = fopen("/tmp/classicui_current", "wt");
		if (f) { fprintf(f, "gb\n%s\n", game_a); fclose(f); }
	}

	harness_set_menu_core(0);
	harness_set_fb_supported(1);
	harness_set_fb(1280, 720);
	gfx_shutdown();
	theme_update(1280, 720, 1);
	harness_set_confstr(6);
	harness_set_osd_mask(0x0000);
	harness_set_osd_visible(0);
	harness_set_input_pad(1);

	/*
	  A pad whose layout is known, because the check on the prompt counts one button's
	  colour in the legend band. A pad that cannot be placed draws its letters with no
	  colour at all, and the count would be zero whether the prompt was offered or not -
	  which would make the check pass for the wrong reason and keep passing if the prompt
	  were removed.
	*/
	harness_set_pad_name("Nintendo Switch Pro Controller");

	/*
	  No shared config on the card to begin with. That is not just a clean slate: it is the
	  case the promotion has to read as sixteen zero bytes rather than as a failure, because
	  zeros are what the core itself boots from when there is no config.
	*/
	forget_shared_cfg();
	check(shared_cfg_field("[54:53]") == -1, "the core has no shared config yet");

	core_opts_scan();
	core_opts_bind_game(core_opts_game_key(gb, game_a));
	for (int i = 0; i < core_opts_count(); i++) core_opt_drop_for_game(core_opt_at(i));

	harness_set_opt("FH", 0);              // Palette = Kitrinx
	harness_set_opt("[54:53]", 0);         // Widescreen Hack = Off

	chome_handle(0);
	if (chome_ingame_active()) press(KEY_MENU, 14);
	frame(6);
	press(KEY_MENU, 20);
	for (int i = 0; i < 40 && lib_scanning(); i++) frame(2);
	frame(16);
	check(chome_ingame_active(), "the menu is up over the running game");

	press(KEY_UP, 14);
	for (int i = 0; i < 6; i++) press(KEY_RIGHT, 8);
	press(KEY_ENTER, 18);
	frame(10);

	const core_opt *pal = core_opt_tier_at(CO_TIER_PICTURE, 0);
	check(pal && !strcasecmp(pal->name, "Palette"), "the screen opens on Palette");

	/*
	  Nothing to hand over yet, so nothing may be offered. The green west button is the Y
	  prompt on a Nintendo pad, and this screen draws no other green one.
	*/
	check(legend_colour(COL_SNES_Y) == 0,
		"on a row this game does not override, no button is offered to share it");

	/*
	  Two overrides, on two different options, both belonging to the one game. This is what
	  makes the crux checkable at all: with only one there is nothing that could leak.
	*/
	press(KEY_RIGHT, 14);                   // Palette -> Smooth
	frame(8);
	pal = core_opt_tier_at(CO_TIER_PICTURE, 0);
	check(pal && core_opt_value(pal) == 1 && core_opt_per_game(pal),
		"a change is still kept for the game alone, as it always was");

	press(KEY_DOWN, 14);
	press(KEY_RIGHT, 14);
	press(KEY_RIGHT, 14);                   // Widescreen Hack -> 16:9
	frame(10);
	const core_opt *ws = core_opt_tier_at(CO_TIER_PICTURE, 1);
	check(ws && !strcasecmp(ws->name, "Widescreen Hack") && core_opt_value(ws) == 2
		&& core_opt_per_game(ws), "and so is a second one, on another option");

	int saves = harness_cfg_saves();
	check(legend_colour(COL_SNES_Y) > 0,
		"on a row it does override, the button to share it is offered");
	dump("core-options-for-all-games");

	/*
	  Let anything still easing finish before the confirmation is measured. Sections before
	  this one leave the suspend strip on its way out, and its tiles are framed in the same
	  green - so an animation ending mid-measurement would read as the message appearing or
	  going away, and the check would pass whether the message existed or not.
	*/
	harness_advance(2500);
	frame(8);
	int green_before = px_count(COL_GREEN);

	// Y. One press, on Widescreen Hack, with Palette overridden and untouched.
	press(KEY_BACKSPACE, 14);
	frame(10);

	check(shared_cfg_field("[54:53]") == 2,
		"the shared config now holds the promoted value, in that option's own bits");

	/*
	  The crux. Palette is this game's own choice and must not have been carried out with
	  the setting that was promoted - which is precisely what saving the status word would
	  have done, and it is the reason this direction did not exist before.
	*/
	check(shared_cfg_field("FH") == 0,
		"and the other override this game holds did NOT go into it");
	check(harness_cfg_saves() == saves,
		"because the whole status word was never written - only those bits");

	// Nor was it thrown away in the other direction: it is still the game's.
	pal = core_opt_tier_at(CO_TIER_PICTURE, 0);
	check(pal && core_opt_per_game(pal) && core_opt_value(pal) == 1,
		"the other override is still this game and still on its own value");

	/*
	  And the promoted one stops being this game's, which is the decision made in
	  core_opt_promote_to_core(): the override has become a copy of the shared value, and a
	  row still starred would be claiming to differ from a value it now equals.
	*/
	ws = core_opt_tier_at(CO_TIER_PICTURE, 1);
	check(ws && !core_opt_per_game(ws),
		"the promoted setting is no longer kept for this game, being the shared one now");
	check(ws && core_opt_value(ws) == 2,
		"and the core was left on it rather than reverted under the player");

	// Which also means there is nothing left to hand over on that row.
	check(legend_colour(COL_SNES_Y) == 0, "so the button is no longer offered there");

	/*
	  The player has to be told, because the only other visible effect is the star going -
	  and X produces the same disappearance while meaning the opposite.

	  The direction of the first count is what makes it a check rather than an observation.
	  Promoting *removes* green from the screen: the row it happened to loses both its star
	  and the green its value was drawn in. So if the total green goes up across that press,
	  something green was added that is larger than what was taken away, and the only thing
	  on this screen that can be is the confirmation. It is on a timer, so the second count
	  is that it goes again on its own.
	*/
	int green_now = px_count(COL_GREEN);
	dump("core-options-shared-now");
	harness_advance(4000);
	frame(6);
	int green_later = px_count(COL_GREEN);
	printf("  green pixels: %d before, %d while it says so, %d after\n",
		green_before, green_now, green_later);
	check(green_now > green_before, "the screen says the promotion happened, in green");
	check(green_now > green_later, "and stops saying it on its own a moment later");
	check(green_later > 0, "while the row that is still this game only keeps its mark");

	/*
	  A second promotion, on the option that was left alone. If the file were rebuilt from
	  anything rather than edited, the first one would vanish here.
	*/
	press(KEY_UP, 14);
	frame(8);
	press(KEY_BACKSPACE, 14);
	frame(10);
	check(shared_cfg_field("FH") == 1, "a second option can be promoted too");
	check(shared_cfg_field("[54:53]") == 2, "and the first one is still in the file");
	check(harness_cfg_saves() == saves, "still without writing the status word");

	/*
	  What the whole thing was for: the next game on the same core has no overrides left to
	  find, so it comes up on values that are now the core's own. The core reading the file it
	  boots from is the firmware's own business and cannot be exercised here, so this checks
	  the half that is ours - that nothing is waiting to be re-applied per game.
	*/
	check(core_opts_apply_for_game(gb, game_b) == 0, "another game on the core overrides none");
	check(core_opts_apply_for_game(gb, game_a) == 0, "and neither does the game it came from");

	/*
	  And with no game to hang a choice on there is nothing to promote: a change there
	  already goes into the shared config, as it did before any of this existed. The press
	  must do nothing rather than write something.
	*/
	core_opts_bind_game(0);
	frame(8);
	check(legend_colour(COL_SNES_Y) == 0, "with no game identified the button is not offered");

	int before_fh = shared_cfg_field("FH");
	press(KEY_BACKSPACE, 14);
	frame(8);
	check(shared_cfg_field("FH") == before_fh, "and pressing it anyway writes nothing");
	check(harness_cfg_saves() == saves, "nor does it fall back to writing the status word");

	press(KEY_ESC, 12);
	frame(6);
	press(KEY_MENU, 16);
	frame(8);

	forget_shared_cfg();
	harness_set_confstr(1);
	harness_set_pad_name("Generic USB Gamepad");
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

	/*
	  Core Settings is the tenth row, above About - it was above Close Game until that moved
	  to the menu bar, and the two swapped without disturbing the count. Counted downwards
	  from the top, so a row added anywhere above it moves this count - which is exactly what
	  happened when Online Covers was inserted under Cover Art, and the four checks after this
	  walk are what said so. Left counting downwards on purpose: opt_ingame_pass() reaches the
	  last row by wrapping upwards instead, so between the two of them an inserted row is
	  certain to break one rather than sliding quietly past both.
	*/
	for (int i = 0; i < 9; i++) press(KEY_DOWN, 8);
	frame(8);
	dump("core-options-row");

	press(KEY_ENTER, 18);
	frame(10);

	check(!chome_ingame_active(), "choosing it gives the screen back to the core");
	check(harness_last_menu_key() == (KEY_F12 | UPSTROKE),
		"and asks for the classic OSD the way menu.cpp opens it - on the release");

	/*
	  The key that asks for the OSD must not be taken by us on the way past.

	  This is the bug Dinofly hit: Core Settings closed our menu, menu_key_set(KEY_F12) queued
	  the key, and then this handler saw F12 first and read it as "open Classic Home". The
	  OSD never appeared and the swallowed press is why the next one seemed to do nothing.

	  Here the OSD has not appeared yet, which is the exact window that went wrong - so a
	  menu press in it must leave us shut.
	*/
	press(KEY_MENU, 20);
	frame(8);
	check(!chome_ingame_active(), "and a press before the OSD appears is not taken by us");

	/*
	  And once it is up the button is the OSD's. Pressing it must not reopen the front-end
	  over the settings screen the player just asked for.
	*/
	harness_set_osd_visible(1);
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

/*
  What the Options panel holds, said here rather than read from chome_ui.cpp: those two
  counts are private to the front-end, and a test that shared the constant with the code
  it is checking would agree with a wrong one.

  Eleven rows either way now. The shelf's list was ten and gained About, which came off the
  menu bar to make room for Close Game; the in-game list was already eleven, lost Close
  Game to that bar and gained About in its place. So the shelf's panel is a row longer than
  it has ever been, and whether it still reaches its last row at 240p is a live question
  rather than a formality - which is what opt_menu_pass() below is for.
*/
#define OPT_ROWS_MENU_T 11
#define OPT_ROWS_GAME_T 11

/*
  The SCR_* ids these sections assert on, spelled out here for the same reason the row
  counts are: they are private to chome_ui.cpp, and sharing the enum with the code under
  test would make a renumbering agree with itself. The sections below use them by name so a
  walk that lands on the wrong screen says which one it landed on.
*/
enum {
	S_HOME_T    = 0,
	S_MENUBAR_T = 1,
	S_DISPLAY_T = 4,
	S_OPTIONS_T = 5,
	S_ABOUT_T   = 6,
	S_POWER_T   = 12,
	S_CORE_T    = 16,
	S_CLOSE_T   = 20
};

/*
  The Options panel's rows, read off the screen rather than recomputed.

  draw_rows_c() fills the selected row's plate in COL_BLUE and writes its label over it,
  and the panel is drawn opaque over the shelf - so within the panel rectangle that colour
  is the selected row and nothing else. What comes back is the band it occupies, which is
  the only honest answer to "is that row on the screen": the arithmetic deciding which
  rows get drawn is exactly what was wrong, so a test that recomputed it would have passed
  on the build where Close Game had never once appeared on a 240p television.
*/
static int opt_sel_band(int *oy0, int *oy1)
{
	int x0, y0, x1, y1;
	panel_rect(&x0, &y0, &x1, &y1);

	const uint32_t *fb = harness_fb_shown();
	int w = gfx_w(), h = gfx_h();
	int top = -1, bot = -1;

	if (oy0) *oy0 = 0;
	if (oy1) *oy1 = 0;
	if (!fb || w < 1 || h < 1) return 0;

	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (x1 > w) x1 = w;
	if (y1 > h) y1 = h;

	for (int y = y0; y < y1; y++)
	{
		int on = 0;
		for (int x = x0; x < x1 && !on; x++)
			if ((fb[(size_t)y * w + x] | 0xff000000u) == COL_BLUE) on = 1;

		if (!on) continue;
		if (top < 0) top = y;
		bot = y;
	}

	if (top < 0) return 0;
	if (oy0) *oy0 = top;
	if (oy1) *oy1 = bot;
	return 1;
}

// The panel's body: the rectangle draw_panel() hands the rows, under its own title bar.
// draw_panel_at()'s two constants, and they have to move with it.
static void opt_body(int *bx, int *by, int *bw, int *bh)
{
	const chome_profile *p = theme_get();
	int x0, y0, x1, y1;
	panel_rect(&x0, &y0, &x1, &y1);

	int hdr = 10 * p->ts_ui + 6;
	*bx = x0;
	*by = y0 + hdr;
	*bw = p->panel_w;
	*bh = p->panel_h - hdr;
}

// Ink in the scrollbar's column, which is the value column's right margin - clear of the
// widest value the rows can hold, so anything COL_INK in it is the thumb.
static int opt_scrollbar_ink()
{
	const chome_profile *p = theme_get();
	int bx, by, bw, bh;
	opt_body(&bx, &by, &bw, &bh);
	return box_pixels(bx + bw - 3 * p->ts_ui, by, bx + bw - p->ts_ui, by + bh, COL_INK);
}

/*
  The help line's own band, the way draw_options_panel() reserves it: two lines of the
  tiny face, 20 units up from the bottom of the panel.

  Inside the panel's frame rather than edge to edge. draw_panel_at() draws that frame in
  COL_PANELLO, which is also the colour of the line being looked for - so a box taken to
  the panel's own edges answers "there is a help line here" on every screen that has a
  panel at all, including the shelf's, which has no such line.
*/
static int opt_foot_top()
{
	int bx, by, bw, bh;
	opt_body(&bx, &by, &bw, &bh);
	return by + bh - 20 * theme_get()->ts_tiny;
}

static int opt_foot_pixels(uint32_t want)
{
	const chome_profile *p = theme_get();
	int bx, by, bw, bh;
	opt_body(&bx, &by, &bw, &bh);

	int s2 = p->ts_tiny;
	return box_pixels(bx + 4 * s2, opt_foot_top(), bx + bw - 4 * s2, by + bh - 2 * s2, want);
}

/*
  How many rows of the footer band have any of that colour in them, which is how a
  sentence drawn on two lines is told from one cut off at the panel's edge: a line of the
  tiny face is 8 units of ink, so one line answers about 8 and two answer about 16.

  It is the check that matters for the unarmed message. gfx_clip() shortens a string that
  will not fit and marks the cut with a '>', so "it is drawn" and "all of it is drawn" are
  different questions, and the second is the one that was failing - at every profile, not
  only the small one.
*/
static int opt_foot_ink_rows(uint32_t want)
{
	const chome_profile *p = theme_get();
	int bx, by, bw, bh;
	opt_body(&bx, &by, &bw, &bh);

	int s2 = p->ts_tiny;
	int n = 0;
	for (int y = opt_foot_top(); y < by + bh - 2 * s2; y++)
		if (box_pixels(bx + 4 * s2, y, bx + bw - 4 * s2, y + 1, want) > 0) n++;

	return n;
}

// Walks into Options from the shelf or from a running game. Display drops out of the menu
// bar at 240p, so Options is the first entry there and the second everywhere else.
static void opt_open()
{
	press(KEY_UP, 14);
	if (theme_get()->id != PROF_LO) press(KEY_RIGHT, 10);
	press(KEY_ENTER, 16);
	frame(8);
}

/*
  In a game, at one profile: walk to the last row and look at it.

  Everything here is asked of the picture. The row index is worked back out of where the
  highlight landed, so at 720p - where all eleven rows fit - the last row has to be drawn
  eleventh, and at 240p, where nine fit, it has to be drawn inside the window with the
  list scrolled under it. Either way it has to be on the screen, above the help line, with
  its label really written in it.
*/
static void opt_ingame_pass(const char *tag, int force, int w, int h)
{
	char what[160];

	/*
	  The profile is forced through cfg, not only through theme_update(): chome_handle()
	  re-measures the canvas every frame and passes cfg.classicui_profile back in, so a
	  profile set here alone is undone by the next frame. It cost a whole run - the "lo"
	  pass ran at 320x240 under hd metrics, which is a real arrangement but not the one
	  the section is named after.
	*/
	cfg.classicui_profile = (uint8_t)force;

	harness_set_fb(w, h);
	gfx_shutdown();
	theme_update(w, h, force);
	frame(6);

	if (chome_ingame_active()) press(KEY_MENU, 14);
	frame(6);
	press(KEY_MENU, 20);
	for (int i = 0; i < 40 && lib_scanning(); i++) frame(2);
	frame(12);

	snprintf(what, sizeof(what), "%s: the menu is up over the running game", tag);
	check(chome_ingame_active(), what);

	opt_open();

	const chome_profile *p = theme_get();
	int s = p->ts_ui, rowh = p->row_h;

	int bx, by, bw, bh;
	opt_body(&bx, &by, &bw, &bh);
	int foot_y = opt_foot_top();

	int a0 = 0, a1 = 0;
	snprintf(what, sizeof(what), "%s: the Options panel is up with a row selected", tag);
	check(opt_sel_band(&a0, &a1), what);

	// The last row, reached by wrapping upwards off the first - so a row inserted
	// anywhere above it cannot quietly move what this is about.
	press(KEY_UP, 10);
	frame(6);

	int y0 = 0, y1 = 0;
	int seen = opt_sel_band(&y0, &y1);

	snprintf(what, sizeof(what), "%s: the last row of Options is drawn on the screen", tag);
	check(seen, what);

	snprintf(what, sizeof(what), "%s: and inside the panel, clear of its bottom edge", tag);
	check(seen && y0 >= by && y1 < by + bh, what);

	// A plate with nothing written on it would satisfy the two above. The label is drawn
	// in white over the selected row, so this is the row's text really being there.
	snprintf(what, sizeof(what), "%s: with its label written in it", tag);
	check(seen && box_pixels(bx, y0, bx + bw, y1 + 1, COL_WHITE) > 0, what);

	int drawn = seen ? (y0 + 2 * s - by - 5 * s) / rowh : -1;
	printf("  %s: panel body %dx%d at %d,%d; last row drawn %d rows down, y %d..%d, help line at %d\n",
		tag, bw, bh, bx, by, drawn, y0, y1, foot_y);

	snprintf(what, sizeof(what), "%s: it is below the row the panel opened on", tag);
	check(seen && y0 > a0, what);

	/*
	  Eleven rows in a game. Where they all fit the last one is drawn eleventh and nothing
	  scrolls; where they do not, the window has moved down the list and the scrollbar says
	  so. Both are correct and which one applies is the profile's business - but a panel
	  drawing the last row eleventh while only nine rows fit is the original bug, and one
	  claiming to scroll when everything fits is the other way to get this wrong.
	*/
	int fits = (drawn == OPT_ROWS_GAME_T - 1);
	snprintf(what, sizeof(what), "%s: %s", tag,
		fits ? "every row fits, so nothing scrolled" : "the list scrolled to bring it into view");
	check(seen && (fits ? opt_scrollbar_ink() == 0 : (drawn >= 0 && drawn < OPT_ROWS_GAME_T - 1
		&& opt_scrollbar_ink() > 0)), what);

	/*
	  And no help line under them any more, at any profile.

	  There used to be one, and it was about Close Game: "THE GAME STAYS LOADED UNTIL YOU
	  CLOSE IT", with "UNSAVED PROGRESS WILL BE LOST" in red once the row was armed. Close
	  Game is on the menu bar now and the sentences went with it - they are checked on the
	  screen that carries them, in assert_close_game_on_the_bar(). What is checked here is
	  that the room they took was given back to the list rather than left as a reserved band
	  captioning a row that is no longer in the panel.
	*/
	snprintf(what, sizeof(what), "%s: and no help line under them, now the row it captioned has gone", tag);
	check(opt_foot_pixels(COL_PANELLO) == 0 && opt_foot_pixels(COL_RED) == 0, what);
	(void)foot_y;

	{
		char name[64];
		snprintf(name, sizeof(name), "options-ingame-%s-last-row", p->name);
		dump(name);
	}

	/*
	  And the last row is About, asked of the front-end by opening it rather than by reading
	  the label: what matters is where the row goes. This is also half of "About left the bar
	  and became a row" - the other half, that no bar entry opens it, is checked on the bar.
	*/
	press(KEY_ENTER, 14);
	frame(8);
	snprintf(what, sizeof(what), "%s: and it is About, which opens its panel", tag);
	check(chome_screen_id() == S_ABOUT_T, what);

	// Back to the row it was chosen from, not out to the bar: About is a row of this list now.
	press(KEY_ESC, 12);
	frame(6);
	snprintf(what, sizeof(what), "%s: and backing out of it returns to Options", tag);
	check(chome_screen_id() == S_OPTIONS_T, what);

	press(KEY_ESC, 10);
	press(KEY_MENU, 16);
	frame(6);
}

/*
  And on the shelf, where the list used to be ten rows and to fit.

  It is eleven now - About came down off the menu bar - so this is no longer the "a fix for
  a panel that is one row too long has no business moving a panel that is not" pass it was
  written as. The shelf's panel is now exactly the shape the in-game one was when Close Game
  was invisible on a television: one row longer than 240p can hold. So it is checked the
  same way, by walking to the last row and looking for it on the screen, and the "drawn
  where it always was" arithmetic is gone because where it always was is no longer where it
  belongs.
*/
static void opt_menu_pass(const char *tag, int force, int w, int h)
{
	char what[160];

	cfg.classicui_profile = (uint8_t)force;   // see opt_ingame_pass()

	harness_set_fb(w, h);
	gfx_shutdown();
	theme_update(w, h, force);
	chome_leave();
	press(KEY_MENU, 20);
	for (int i = 0; i < 40 && lib_scanning(); i++) frame(2);
	frame(12);

	opt_open();

	const chome_profile *p = theme_get();
	int s = p->ts_ui, rowh = p->row_h;

	int bx, by, bw, bh;
	opt_body(&bx, &by, &bw, &bh);

	// The last row, reached by wrapping upwards off the first - so a row inserted anywhere
	// above it cannot quietly move what this is about.
	press(KEY_UP, 10);
	frame(6);

	int y0 = 0, y1 = 0;
	int seen = opt_sel_band(&y0, &y1);

	int drawn = seen ? (y0 + 2 * s - by - 5 * s) / rowh : -1;

	printf("  %s: panel body %dx%d at %d,%d; last shelf row drawn %d rows down, y %d..%d; "
		"foot ink %d/%d, bar %d\n",
		tag, bw, bh, bx, by, drawn, y0, y1,
		opt_foot_pixels(COL_PANELLO), opt_foot_pixels(COL_RED), opt_scrollbar_ink());

	{
		char name[64];
		snprintf(name, sizeof(name), "options-shelf-%s", p->name);
		dump(name);
	}

	snprintf(what, sizeof(what), "%s: the last row of Options is drawn on the screen", tag);
	check(seen, what);

	snprintf(what, sizeof(what), "%s: and inside the panel, clear of its bottom edge", tag);
	check(seen && y0 >= by && y1 < by + bh, what);

	// A plate with nothing written on it would satisfy the two above.
	snprintf(what, sizeof(what), "%s: with its label written in it", tag);
	check(seen && box_pixels(bx, y0, bx + bw, y1 + 1, COL_WHITE) > 0, what);

	/*
	  Eleven rows on the shelf too. Where they fit, the last is drawn eleventh and nothing
	  scrolls; where they do not - 240p, which is the case this whole section exists for -
	  the window has moved down the list and the scrollbar says so.
	*/
	int fits = (drawn == OPT_ROWS_MENU_T - 1);
	snprintf(what, sizeof(what), "%s: %s", tag,
		fits ? "every row fits, so nothing scrolled" : "the list scrolled to bring it into view");
	check(seen && (fits ? opt_scrollbar_ink() == 0 : (drawn >= 0 && drawn < OPT_ROWS_MENU_T - 1
		&& opt_scrollbar_ink() > 0)), what);

	snprintf(what, sizeof(what), "%s: and no help line, which no panel carries any more", tag);
	check(opt_foot_pixels(COL_PANELLO) == 0 && opt_foot_pixels(COL_RED) == 0, what);

	// And the shelf's last row is About as well - the one row the two lists share.
	press(KEY_ENTER, 14);
	frame(8);
	snprintf(what, sizeof(what), "%s: and it is About here too", tag);
	check(chome_screen_id() == S_ABOUT_T, what);

	press(KEY_ESC, 12);
	frame(6);
	snprintf(what, sizeof(what), "%s: returning to Options rather than out to the bar", tag);
	check(chome_screen_id() == S_OPTIONS_T, what);

	press(KEY_ESC, 10);
	press(KEY_MENU, 16);
	frame(6);
}

/*
  Options has a row nobody could reach.

  "We also need a close game option somewhere" - it was there all along: rows_game[] had
  ended with Close Game for as long as the in-game menu had existed, and the cursor
  counted to it. The panel is a fixed rectangle out of the theme and draw_rows_c() stops
  the moment a row would cross its bottom edge, so at 240p the eleventh row was dropped
  without a word and the help line under it was drawn through the tenth.

  Close Game has since been promoted to the menu bar and About has come down into its
  place, which does not retire this section - it doubles it. The in-game list is still
  eleven rows, and the shelf's, which used to be ten and to fit, is eleven now too. Both
  are walked below. The list scrolls
  now, the way More Settings already did, and this section is the proof - at all three
  profiles, by walking to the row and looking for it.
*/
static void assert_options_panel_scrolls()
{
	printf("\n== every row of Options is on the screen, at every profile ==\n");

	{
		FILE *f = fopen("/tmp/classicui_current", "wt");
		if (f) { fprintf(f, "gb\nTetris (World).gb\n"); fclose(f); }
	}

	uint8_t was_profile = cfg.classicui_profile;

	harness_set_menu_core(0);
	harness_set_fb_supported(1);
	harness_set_confstr(1);
	harness_set_osd_visible(0);
	chome_handle(0);

	opt_ingame_pass("hd", 1, 1280, 720);
	opt_ingame_pass("sd", 2, 640, 480);
	opt_ingame_pass("lo", 3, 320, 240);

	harness_set_menu_core(1);
	opt_menu_pass("shelf hd", 1, 1280, 720);
	opt_menu_pass("shelf sd", 2, 640, 480);
	opt_menu_pass("shelf lo", 3, 320, 240);

	// Back the way this section found things: a game running, our menu shut, 720p.
	cfg.classicui_profile = was_profile;
	harness_set_menu_core(0);
	harness_set_fb(1280, 720);
	gfx_shutdown();
	theme_update(1280, 720, cfg.classicui_profile);
	chome_leave();
	chome_handle(0);
	frame(6);
}

/* ------------------------------------------- Close Game, up on the menu bar --- */

/*
  Which screen a menu-bar entry opens, found by walking the bar rather than by asking the
  front-end what is on it.

  There is no accessor for the entries and there should not need to be one: what a player
  can reach is the claim, and walking is what a player does. mb_idx survives leaving the
  bar, so every walk starts by pressing hard against the left-hand stop - move_h() nudges
  rather than wrapping at both ends, which is what makes eight presses a reliable "as far
  as it goes" in either direction.

  Left on whatever it opened. The caller reads the id and puts the screen back, because
  what "back" means differs per entry and doing it here would hide that.
*/
static int bar_slot_opens(int slot)
{
	press(KEY_UP, 14);                        // the menu bar
	for (int i = 0; i < 8; i++) press(KEY_LEFT, 5);
	for (int i = 0; i < slot; i++) press(KEY_RIGHT, 5);
	press(KEY_ENTER, 16);
	frame(8);
	return chome_screen_id();
}

// Back out to the shelf from wherever the walk above landed, without pressing B on the
// shelf itself - that is a navigation key there and would jump the selection to the left.
static void bar_walk_home()
{
	for (int i = 0; i < 5 && chome_screen_id() != S_HOME_T; i++) press(KEY_ESC, 10);
	frame(6);
}

// Defined with the typography section, which is the other place that reads a particular
// letter off the screen rather than counting ink.
static int glyph_seen(int x, int y, int s, unsigned char code, uint32_t col);

/*
  Is any label on the menu bar cut off?

  gfx_clip() shortens a string that will not fit its cell and marks the cut with a '>', so
  the question "is CLOSE GAME drawn on the bar" and "is CLOSE G> drawn on the bar" have the
  same answer to anything that only looks for ink. This looks for the marker itself, in the
  one row of the canvas the bar draws its words in - scanned across rather than computed per
  cell, because the cell a label lands in depends on how many entries are visible.

  The bar has to be fully out for this: draw_menubar() slides it in from above and its text
  row is only at safe_y once bar_y has settled at 1.
*/
static int bar_label_clipped(uint32_t col)
{
	const chome_profile *p = theme_get();
	int s = (p->id == PROF_HD) ? 2 : 1;
	int gy = p->safe_y + (p->bar_h - 8 * s) / 2;

	for (int x = 0; x + 8 * s <= p->w; x++)
		if (glyph_seen(x, gy, s, '>', col)) return 1;

	return 0;
}

/*
  Closing a game was the eleventh row of the Options panel - two presses in, at the bottom
  of a list that until this week did not even draw its last row at 240p. About, which a
  person reads once and never again, had a permanent slot on the menu bar. That is the two
  of them exactly the wrong way round, and this section is the swap.

  The half that matters is not the promotion but what came with it. The row it replaced had
  a two-press confirm on a three-second timer, and a bar entry that closed the game on one
  press would have been a regression dressed as an improvement: unsaved progress, gone, to
  save somebody a press. So the entry opens a screen and the confirm lives there, the way
  MB_POWER has always opened SCR_POWER rather than restarting the machine where it stands.

  Everything below is asked of the front-end by pressing keys at it and reading what came
  back - the screen it landed on, the pixels in the panel, whether a core was loaded.
*/
static void assert_close_game_on_the_bar()
{
	printf("\n== Close Game is on the menu bar, and About is not ==\n");

	uint8_t was_profile = cfg.classicui_profile;

	{
		FILE *f = fopen("/tmp/classicui_current", "wt");
		if (f) { fprintf(f, "gb\nTetris (World).gb\n"); fclose(f); }
	}

	harness_set_fb_supported(1);
	harness_set_confstr(1);
	harness_set_osd_visible(0);

	/* --------------------------------------------------------- on the shelf --- */

	/*
	  Nothing is running, so there is nothing to close and the entry is not there at all -
	  not drawn and refusing, which would leave a player wondering what the destructive-
	  sounding thing on their menu bar had been about to do.
	*/
	harness_set_menu_core(1);
	cfg.classicui_profile = 1;
	harness_set_fb(1280, 720);
	gfx_shutdown();
	theme_update(1280, 720, 1);
	chome_leave();
	press(KEY_MENU, 20);
	for (int i = 0; i < 40 && lib_scanning(); i++) frame(2);
	frame(12);

	{
		int seen_close = 0, seen_about = 0, ids[6];

		for (int slot = 0; slot < 6; slot++)
		{
			ids[slot] = bar_slot_opens(slot);
			if (ids[slot] == S_CLOSE_T) seen_close = 1;
			if (ids[slot] == S_ABOUT_T) seen_about = 1;
			bar_walk_home();
		}

		printf("  shelf bar opens: %d %d %d %d %d %d (close=%d about=%d)\n",
			ids[0], ids[1], ids[2], ids[3], ids[4], ids[5], S_CLOSE_T, S_ABOUT_T);

		check(ids[0] == S_DISPLAY_T && ids[1] == S_OPTIONS_T && ids[2] == S_POWER_T,
			"the shelf's bar is Display, Options, Power");

		/*
		  And stops there. Walking past the end nudges rather than wrapping, so slots 3, 4
		  and 5 all land back on Power - which is the same statement as "there is no fourth
		  entry", made without an accessor for how many there are.
		*/
		check(ids[3] == S_POWER_T && ids[4] == S_POWER_T && ids[5] == S_POWER_T,
			"and stops there: walking past the end stays on Power");

		check(!seen_close, "Close Game is not on the shelf's bar, where there is nothing to close");
		check(!seen_about, "and About is not on it either, at any slot");
	}

	{
		press(KEY_UP, 16);
		frame(8);
		dump("closegame-1-bar-shelf");
		check(!bar_label_clipped(COL_INK) && !bar_label_clipped(COL_WHITE),
			"and every word the shelf's bar does carry is drawn in full");
		bar_walk_home();
	}

	/* ------------------------------------------------------------- in a game --- */

	harness_set_menu_core(0);
	harness_set_core_name("GAMEBOY");
	chome_handle(0);
	if (chome_ingame_active()) press(KEY_MENU, 14);
	press(KEY_MENU, 20);
	for (int i = 0; i < 40 && lib_scanning(); i++) frame(2);
	frame(12);
	check(chome_ingame_active(), "and with a game up, the menu is over it");

	{
		int seen_close = 0, seen_about = 0, ids[6];

		for (int slot = 0; slot < 6; slot++)
		{
			ids[slot] = bar_slot_opens(slot);
			if (ids[slot] == S_CLOSE_T) seen_close = 1;
			if (ids[slot] == S_ABOUT_T) seen_about = 1;
			bar_walk_home();
		}

		printf("  in-game bar opens: %d %d %d %d %d %d\n",
			ids[0], ids[1], ids[2], ids[3], ids[4], ids[5]);

		check(ids[0] == S_DISPLAY_T && ids[1] == S_OPTIONS_T && ids[2] == S_POWER_T
			&& ids[3] == S_CLOSE_T,
			"in a game the bar is Display, Options, Power, Close Game");

		check(seen_close, "so Close Game is one press from the game, not eleven rows down");
		check(!seen_about, "and About is still not on the bar");
	}

	/* ------------------------------ the confirm, at 240p where the row was lost --- */

	/*
	  240p on purpose. This is the television the front-end was written for and the canvas
	  the old row was invisible on, so it is the one worth proving the replacement legible
	  and reachable on - and the bar is at its most crowded there, where Display is dropped
	  and "CLOSE GAME" is the longest word on it.
	*/
	cfg.classicui_profile = 3;
	harness_set_fb(320, 240);
	gfx_shutdown();
	theme_update(320, 240, 3);
	if (chome_ingame_active()) press(KEY_MENU, 14);
	press(KEY_MENU, 20);
	frame(12);

	press(KEY_UP, 16);
	frame(8);
	dump("closegame-2-bar-240p");
	check(!bar_label_clipped(COL_INK) && !bar_label_clipped(COL_WHITE),
		"at 240p the bar still spells Close Game out rather than cutting it short");

	// Display is dropped at 240p, so Close Game is the third entry here and not the fourth.
	for (int i = 0; i < 8; i++) press(KEY_LEFT, 5);
	press(KEY_RIGHT, 6);
	press(KEY_RIGHT, 6);
	press(KEY_ENTER, 16);
	frame(8);
	check(chome_screen_id() == S_CLOSE_T, "and Close Game opens from it at 240p too");

	{
		int x0, y0, x1, y1;
		panel_rect(&x0, &y0, &x1, &y1);

		dump("closegame-3-unarmed");

		/*
		  Nothing red before a press. The panel is opened by the bar entry and the entry is
		  named after a destructive act, so a screen that arrived already armed would close
		  the game on the first press made on it.
		*/
		check(box_pixels(x0, y0, x1, y1, COL_RED) == 0,
			"the screen opens disarmed, with nothing red on it");

		// And it says what closing costs, before the player has committed to anything.
		check(box_pixels(x0, y0, x1, y1, COL_PANELHI) > 0,
			"and carries the sentence about the game staying loaded");

		/*
		  The core loaded before any of this, kept so "nothing was loaded" can be said as
		  "the same thing is loaded as before". harness_clear_launch() would not do: it
		  empties the launch record and leaves last_rbf holding whatever a previous section
		  loaded, so an emptiness check on it asserts about that section and not this one.
		*/
		char rbf_before[256];
		snprintf(rbf_before, sizeof(rbf_before), "%s", harness_last_rbf());
		harness_clear_launch();

		press(KEY_ENTER, 12);
		frame(8);
		dump("closegame-4-armed");

		check(chome_ingame_active(), "one press does not close the game");
		check(!strcmp(harness_last_rbf(), rbf_before), "and loads nothing");
		check(box_pixels(x0, y0, x1, y1, COL_RED) > 0,
			"it arms instead, and says so in red");

		/*
		  Moving off disarms, which is the behaviour this screen inherited rather than the
		  one it invented: the timer used to run on while the cursor was elsewhere, so a
		  press, a look away and a press back inside three seconds closed the game on what
		  the player had counted as the first of two.
		*/
		press(KEY_DOWN, 10);
		frame(8);
		check(box_pixels(x0, y0, x1, y1, COL_RED) == 0, "moving off it disarms it");

		press(KEY_UP, 10);
		frame(8);
		check(box_pixels(x0, y0, x1, y1, COL_RED) == 0, "and coming back finds it disarmed");

		press(KEY_ENTER, 12);
		frame(8);
		check(chome_ingame_active(),
			"so the press after that arms it again rather than closing the game");
		check(!strcmp(harness_last_rbf(), rbf_before), "with still nothing loaded");

		// B cancels the arm before it leaves the screen, the same way Power does.
		press(KEY_ESC, 10);
		frame(8);
		check(chome_screen_id() == S_CLOSE_T && box_pixels(x0, y0, x1, y1, COL_RED) == 0,
			"and B cancels the arming before it leaves the screen");

		// Two deliberate presses, with nothing in between. This one has to work.
		press(KEY_ENTER, 12);
		press(KEY_ENTER, 12);
		frame(10);
		printf("  loaded rbf: %s\n", harness_last_rbf());
		check(strstr(harness_last_rbf(), "menu.rbf") != 0,
			"and two deliberate presses do close the game");
	}

	// Back the way this section found things: the shelf, our menu shut, 720p.
	cfg.classicui_profile = was_profile;
	harness_set_menu_core(1);
	harness_set_fb(1280, 720);
	gfx_shutdown();
	theme_update(1280, 720, cfg.classicui_profile);
	chome_leave();
	chome_handle(0);
	frame(6);
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

	check(select_folder("Systems"), "the Systems card is on the root shelf");
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
	// "o01" in this core's CONF_STR, so the upper word - see harness_opt_val().
	check(harness_opt_val("01", 1) != 0, "the savestate slot option is the one that moved");

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

/*
  How much ink the button bar at the bottom is using. A hash says "different"; this says
  "smaller", which is the claim when a prompt is meant to have disappeared and every other
  prompt on the screen is meant to be untouched.
*/
static long legend_ink()
{
	const uint32_t *fb = harness_fb_shown();
	int w = gfx_w(), h = gfx_h();
	if (!fb || w < 1 || h < 1) return -1;

	long n = 0;
	for (int y = (h * 4) / 5; y < h; y++)
		for (int x = 0; x < w; x++)
		{
			uint32_t px = fb[(size_t)y * w + x];
			if (px != COL_BG && px != COL_BGDARK && px != COL_BTN_CHIP) n++;
		}
	return n;
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
  Which slot the strip's cursor is on, read off the framebuffer the same way
  strip_pixels() reads the tiles: COL_FOCUS is the cursor ring, and in the strip's own
  band nothing else is that colour. Returns the tile the ring surrounds, using
  draw_suspend()'s own geometry (tiles at x0 + i * (tw + gap), the ring 3 px outside
  its tile), or -1 when no ring is drawn.

  Read back rather than trusted, because this is exactly what the device report got
  wrong by eye: a filled tile's green frame reads as "selected" in a capture, and the
  actual ring was two tiles away on an empty slot.
*/
static int strip_focused_slot(int nslots)
{
	const chome_profile *p = theme_get();
	uint32_t *fb = harness_fb_shown();
	int w = gfx_w(), h = gfx_h();
	if (!fb || w < 1 || h < 1 || nslots < 1) return -1;

	int top = p->h - p->safe_y - p->strip_h;
	int bot = p->y_legend - 6 * p->ts_ui;
	if (top < 0) top = 0;
	if (bot > h) bot = h;

	int minx = -1;
	for (int y = top; y < bot; y++)
		for (int x = 0; x < w; x++)
			if ((fb[(size_t)y * w + x] | 0xff000000u) == COL_FOCUS)
				if (minx < 0 || x < minx) minx = x;

	if (minx < 0) return -1;

	int x0 = (p->w - (nslots * p->thumb_w + (nslots - 1) * p->thumb_gap)) / 2;
	return (minx + 3 - x0) / (p->thumb_w + p->thumb_gap);
}

/*
  The row the strip's panel starts on, read off the framebuffer rather than from the
  metrics: its top edge is a rule in COL_PANELLO across the whole width, and with the menu
  bar closed - which it is, on the way down into the strip - nothing else on the screen
  draws one. The shelf's cards and their frames are that colour too, hence the whole width
  rather than a colour match on one pixel.

  Searched from the top of the canvas and not from the middle: the panel rests above the
  overscan margin, so it starts higher up the further the margin is raised, and a search
  from h/2 found it only at a margin of zero - which is exactly the case that does not
  need testing.
*/
static int strip_top_row()
{
	const uint32_t *fb = harness_fb_shown();
	int w = gfx_w(), h = gfx_h();
	if (!fb || w < 1 || h < 1) return -1;

	for (int y = 0; y < h; y++)
	{
		int n = 0;
		for (int x = 0; x < w; x++) if ((fb[(size_t)y * w + x] | 0xff000000u) == COL_PANELLO) n++;
		if (n >= w - w / 10) return y;
	}
	return -1;
}

/*
  Backlog 6: a system whose core has no save states says so before the player has spent
  an hour on it, from the shelf as well as from inside the game. On the shelf no core is
  loaded and no CONF_STR has been read, so the answer can only come from the measured
  table in chome_lib - and that table must never outrank a core that is actually running,
  in either direction.
*/
// Is the warning band on this row? Its own colour, so this says what it says.
static int band_red_at(int y)
{
	const uint32_t *fb = harness_fb_shown();
	if (!fb || y < 0 || y >= gfx_h()) return 0;

	int w = gfx_w(), red = 0;
	for (int x = 0; x < w; x++) if (fb[(size_t)y * w + x] == 0xffc4353cu) red++;
	return red > w / 2;
}

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

	// Into Systems, where Game Boy has two games - so a shelf of two is unmistakably
	// that view and not the unfiltered root.
	check(select_folder("Systems"), "the Systems card is on the root shelf");
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

/* --------------------------------------------------------- recently played */

static chome_item *find_titled(const char *title)
{
	for (int i = 0; i < lib_item_count(); i++)
	{
		chome_item *it = lib_item(i);
		if (!strcmp(it->title, title)) return it;
	}
	return 0;
}

// The game at a position of the view last built, or 0. Compared by pointer rather than
// by title everywhere below: two systems on the fake card hold a "Sonic The Hedgehog 2".
static chome_item *entry_game(int i)
{
	const chome_entry *e = lib_view_entry(i);
	if (!e || e->kind != ENT_GAME) return 0;
	return lib_item(e->game);
}

/*
  Recently Played.

  Everything here is about the one property this view has that no other view has: its
  order is its content. So each check is written to fail if the order were left to the
  shelf sort, if a game were listed twice, if the list grew without bound, if it did not
  survive the re-exec a launch performs, or if it offered a ROM that has left the card.

  Driven through lib_note_play() rather than by launching twenty games through the UI: a
  launch is a curtain, an MGL and a re-exec, none of which is what this is about, and
  do_launch() calling lib_note_play() is covered by assert_launch. Two launches have
  happened by the time this runs, which is why nothing here assumes an empty list; that
  the card is *absent* before the first one is asserted in assert_views, which runs
  before any launch.
*/
/*
  Starting a game *at* one of its suspend points, from the shelf, with no core loaded.

  This had no coverage at all, which is why it silently did nothing for however long: the
  shelf path threw the chosen slot away and launched the game from the beginning. The player
  saw a game start, so it looked like it had worked.

  What is checked is the record the resume machinery reads - the same one Resume writes -
  because that is the whole of what this side can do. resume_poll() consuming it happens in
  the next process, after a core load the harness cannot perform.
*/
static void assert_launch_into_state()
{
	printf("\n== starting a game at a suspend point ==\n");

	const char *rec = ROOT "/classicui/suspend.txt";
	unlink(rec);

	harness_set_menu_core(1);
	harness_set_fb(1280, 720);
	gfx_shutdown();
	theme_update(1280, 720, 1);
	chome_leave();
	press(KEY_MENU, 20);
	for (int i = 0; i < 40 && lib_scanning(); i++) frame(2);
	frame(12);

	// A game with a state on the card, reached by name so the shelf order cannot matter.
	int found = select_titled("Tetris");
	check(found, "the shelf can be parked on a game that has a suspend point");

	press(KEY_DOWN, 18);                  // into its suspend strip
	frame(8);

	{
		FILE *f = fopen(rec, "rb");
		check(f == 0, "nothing is armed before the player chooses a slot");
		if (f) fclose(f);
	}

	press(KEY_ENTER, 20);                 // A on the slot: start the game there
	frame(10);

	char body[256] = {};
	FILE *f = fopen(rec, "rb");
	if (f) { if (fread(body, 1, sizeof(body) - 1, f)) {} fclose(f); }
	printf("  suspend record: %s", body[0] ? body : "(none)\n");

	check(body[0] != 0, "choosing a slot arms the resume record so the state is loaded");
	check(strstr(body, "Tetris") != 0, "naming the game that was chosen");

	unlink(rec);
	press(KEY_ESC, 12);
	frame(6);

	/*
	  The same start when the first slot is empty, which is the device report of
	  2026-08-11 verbatim: states in slots 2 and 4 (files _2 and _4), slot 1 empty. The
	  strip drew slot 2's thumbnail, the legend offered Play, and ENT did nothing at all -
	  three times, from a cold boot, with disc support on and then off. The missing piece
	  was the cursor: the strip opened it on slot 1, EMPTY, whose refusal is a 160ms
	  flash no capture can hold, while slot 2's green frame read as the selection.

	  So this drives exactly what the player did - Down onto the strip, then ENT with no
	  slot movement - and expects the state to be armed. The cursor's position is read
	  back off the framebuffer first, because "which slot is selected" is the fact the
	  device captures could not settle by eye.
	*/
	printf("\n== starting at a suspend point when the first slot is empty ==\n");

	check(select_titled("Link to the Past"),
		"the shelf can be parked on a game whose first slot is empty");

	press(KEY_DOWN, 18);                  // into its suspend strip
	frame(8);
	dump("suspend-first-slot-empty");

	check(strip_focused_slot(3) == 1,
		"the strip opens with its cursor on the slot that actually holds a state");

	/*
	  The legend follows the cursor's slot, not the strip: on the empty first slot
	  neither Play nor Lock nor Delete can do anything, so none of them may be offered.
	  Compared as the legend band's pixels across the move rather than by naming
	  glyphs, because "it visibly changed when I stepped onto EMPTY" is the whole of
	  what a player can see. Left is a legal move (no nudge), so nothing else below
	  the strip differs between the two frames.
	*/
	{
		const chome_profile *p = theme_get();
		int ly = p->y_legend - 6 * p->ts_ui;
		unsigned long on_filled = harness_fb_hash_box(0, ly, gfx_w(), gfx_h());
		press(KEY_LEFT, 8);
		frame(6);
		unsigned long on_empty = harness_fb_hash_box(0, ly, gfx_w(), gfx_h());
		check(on_filled != on_empty,
			"the legend stops offering Play/Lock/Delete on an empty slot");
		check(strip_focused_slot(3) == 0, "with the cursor now on the empty first slot");
	}

	// ENT there refuses: an empty slot has nothing to start, and this must stay a
	// refusal rather than become a launch from the beginning.
	press(KEY_ENTER, 20);
	frame(10);
	{
		FILE *f = fopen(rec, "rb");
		check(f == 0, "ENT on the empty slot arms nothing");
		if (f) fclose(f);
	}

	// Back to the slot the strip opened on, and the press the player made.
	press(KEY_RIGHT, 8);
	press(KEY_ENTER, 20);
	frame(10);

	body[0] = 0;
	f = fopen(rec, "rb");
	if (f) { if (fread(body, 1, sizeof(body) - 1, f)) {} fclose(f); }
	printf("  suspend record: %s", body[0] ? body : "(none)\n");

	check(body[0] != 0, "ENT on the slot the strip opened on starts the game");
	check(strstr(body, "Link to the Past") != 0, "naming the game whose slot it was");
	check(strstr(body, "\n1\n") != 0,
		"and the slot line says index 1 - the file with the _2 suffix, not a renumbering");

	unlink(rec);
	press(KEY_ESC, 12);
	frame(6);
}

static void assert_recent()
{
	printf("\n== recently played ==\n");

	chome_item *metroid = find_titled("Super Metroid");
	chome_item *fusion  = find_titled("Metroid Fusion");
	chome_item *chip    = find_titled("Chip's Challenge");
	if (!metroid || !fusion || !chip) { check(0, "this section's fixtures are indexed"); return; }

	/*
	  Launch order, and deliberately the reverse of both other orders this view could
	  come out in: "Metroid Fusion" sorts before "Super Metroid", and after one play each
	  their play counts are equal, so a count sort falls back to the title as well.
	*/
	lib_note_play(fusion);
	lib_note_play(metroid);

	int n = lib_view_build(VIEW_RECENT, -1, SORT_TITLE);
	printf("  the recent view holds %d games\n", n);
	check(n >= 2, "the recent view lists what has been played");
	check(entry_game(0) == metroid && entry_game(1) == fusion,
		"most recently launched first, not sorted by title or by play count");

	// Playing an older one again brings it back to the front and must not list it twice.
	lib_note_play(fusion);
	int again = lib_view_build(VIEW_RECENT, -1, SORT_TITLE);
	check(entry_game(0) == fusion, "playing a game again moves it back to the front");
	check(again == n, "and does not give it a second place on the shelf");

	/*
	  No sort may touch it. The player's chosen sort applies to every other shelf and is
	  handed to this one too, so this is the check that the order is held back from it.
	*/
	{
		chome_item *want[32];
		int wn = lib_view_build(VIEW_RECENT, -1, SORT_TITLE);
		if (wn > 32) wn = 32;
		for (int i = 0; i < wn; i++) want[i] = entry_game(i);

		int same = 1;
		for (int s = 0; s < SORT_COUNT; s++)
		{
			if (lib_view_build(VIEW_RECENT, -1, s) != wn) { same = 0; continue; }
			for (int i = 0; i < wn; i++) if (entry_game(i) != want[i]) same = 0;
		}
		check(same, "every shelf sort leaves the recent order alone");
	}

	/*
	  Across the re-exec a launch performs. In memory the list is index positions; on the
	  card it is keys, so reloading the whole library has to bring the same games back in
	  the same order even though a rescan can hand any of them a different index.
	*/
	int before_reload = lib_view_build(VIEW_RECENT, -1, SORT_TITLE);
	lib_init();
	while (lib_scanning()) lib_scan_step();

	metroid = find_titled("Super Metroid");
	fusion  = find_titled("Metroid Fusion");
	chip    = find_titled("Chip's Challenge");
	if (!metroid || !fusion || !chip) { check(0, "the reload found this section's fixtures"); return; }

	int reloaded = lib_view_build(VIEW_RECENT, -1, SORT_TITLE);
	check(reloaded == before_reload, "the list survives a reload of the library");
	check(entry_game(0) == fusion && entry_game(1) == metroid,
		"in the same order, resolved from the stored keys rather than from index positions");

	/*
	  The cap. Play everything on the card: the list has to stop at twenty, keep the
	  newest and drop the oldest, or "recent" is the library again with extra steps.
	*/
	chome_item *oldest = 0, *newest = 0;
	int played = 0;
	for (int i = 0; i < lib_item_count(); i++)
	{
		chome_item *it = lib_item(i);
		const chome_sys *s = lib_sys(it->sysidx);
		if (!s || s->computer) continue;

		lib_note_play(it);
		if (!played) oldest = it;
		newest = it;
		played++;
	}
	printf("  played %d games in index order\n", played);
	check(played > 20, "the fake card holds more games than the cap, so the cap is reachable");

	int capped = lib_view_build(VIEW_RECENT, -1, SORT_TITLE);
	printf("  and the recent view holds %d of them\n", capped);
	check(capped == 20, "the list is capped at twenty");
	check(entry_game(0) == newest, "with the game played last at the front");

	int still_there = 0;
	for (int i = 0; i < capped; i++) if (entry_game(i) == oldest) still_there = 1;
	check(!still_there, "and the oldest pushed off the end");

	// The card on the shelf: where it sits, and what it says is behind it.
	{
		int rootn = lib_view_build(VIEW_ROOT, -1, SORT_TITLE);
		int at = -1, cnt = -1, opens = -1;
		for (int i = 0; i < rootn; i++)
		{
			const chome_entry *e = lib_view_entry(i);
			if (e->kind == ENT_GAME || strcmp(e->label, "Recently Played")) continue;
			at = i;
			cnt = e->count;
			opens = e->view;
			break;
		}
		printf("  the shelf card is entry %d, counting %d, opening view %d\n", at, cnt, opens);
		check(at == 1, "the card sits immediately after Favourites, ahead of Systems");
		check(opens == VIEW_RECENT, "and opens the recent view");
		check(cnt == capped, "its count is the capped number, not the whole library");
		check(!strcmp(lib_view_title(VIEW_RECENT, -1), "Recently Played"), "the view is titled");
	}

	/*
	  A ROM that has left the card. The index is deliberately not rescanned, so the item
	  is still in it and what this proves is the presence check in the list itself rather
	  than the scanner noticing - which is the case that matters, since this list is read
	  on every boot while a scan happens only when a directory's mtime says to.
	*/
	lib_note_play(chip);
	lib_view_build(VIEW_RECENT, -1, SORT_TITLE);
	check(entry_game(0) == chip, "the Lynx game is at the front of the list");

	char lynx[1024];
	snprintf(lynx, sizeof(lynx), "%s/games/AtariLynx/Chip's Challenge (USA).lnx", ROOT);
	check(unlink(lynx) == 0, "its ROM can be taken off the fake card");

	lib_note_play(metroid);                   // any play re-resolves the list
	int after = lib_view_build(VIEW_RECENT, -1, SORT_TITLE);

	int listed = 0;
	for (int i = 0; i < after; i++) if (entry_game(i) == chip) listed = 1;
	check(!listed, "a game whose ROM has gone is not on the recent shelf");
	check(find_titled("Chip's Challenge") == chip,
		"while the index still holds it, so it is the presence check that dropped it");

	// Put the card back as it was found, and the same check the other way round.
	touch(ROOT "/games/AtariLynx", "Chip's Challenge (USA).lnx", 2048);
	lib_note_play(chip);
	lib_view_build(VIEW_RECENT, -1, SORT_TITLE);
	check(entry_game(0) == chip, "and it is offered again once the ROM is back");
}

/* ------------------------------------------- title groups, through the UI -- */

/*
  Rewinds the shelf and steps right until the card carrying this file is selected, then
  returns that card's index - which is also where the cursor now is, since it got there
  one press at a time from zero. The front-end keeps its selection to itself, so this is
  how a check knows which card the presses below are landing on.
*/
static int shelf_go(const char *sysid, const char *relpath)
{
	shelf_rewind();

	int target = entry_carrying(item_at(sysid, relpath));
	if (target < 0) return -1;

	for (int i = 0; i < target; i++) press(KEY_RIGHT, 3);
	frame(10);
	return target;
}

// Presses the cycle button until the selected card is showing this file. One full turn
// and no more, so a card that never gets there fails rather than hanging.
static int shelf_cycle_to(int card, int idx)
{
	const chome_entry *e = lib_view_entry(card);
	if (!e) return 0;

	for (int i = 0; i <= e->nvar; i++)
	{
		if (lib_view_entry(card)->game == idx) return 1;
		press(KEY_TAB, 10);
	}
	return 0;
}

// The one line of the title block that names the file: between the system line and the
// top of the cards, which is background everywhere else.
static int filename_line_pixels()
{
	const chome_profile *p = theme_get();
	int y = p->y_meta + 10 * p->ts_ui;
	return box_pixels(0, y, p->w, y + 8 * p->ts_tiny, COL_PANELLO);
}

/*
  The same feature through the real key handler, on the real shelf.

  The five things that key state by game are what this is for. Favourites, play counts,
  Recently Played, suspend points and per-game core options all have to act on the file
  the player is looking at - and the whole risk of one card standing for several files is
  that one of them silently attaches one ROM's state to another. Each is checked here
  against the file that is *not* the one the card came up on, because that is the case a
  wrong implementation gets wrong.

  Runs after the recent-list section so that its own launch cannot disturb what that one
  asserts about the order.
*/
static void assert_variant_ui()
{
	printf("\n== title groups on the shelf ==\n");

	const char *eu = "Super Mario World (Europe).sfc";
	const char *jp = "Super Mario World (Japan).sfc";
	const char *us = "Super Mario World (USA).sfc";

	harness_set_menu_core(1);
	harness_set_fb_supported(1);
	harness_set_input_pad(1);
	harness_set_pad_name("Nintendo Switch Pro Controller");
	harness_set_fb(1280, 720);
	gfx_shutdown();
	theme_update(1280, 720, 1);

	chome_leave();
	press(KEY_MENU, 20);
	for (int i = 0; i < 4; i++) press(KEY_ESC, 6);        // out to the root shelf
	frame(10);

	int i_eu = item_at("snes", eu), i_jp = item_at("snes", jp), i_us = item_at("snes", us);
	if (i_eu < 0 || i_jp < 0 || i_us < 0) { check(0, "this section's fixtures are indexed"); return; }

	int card = shelf_go("snes", eu);
	if (card < 0) { check(0, "the grouped card is on the root shelf"); return; }

	/*
	  The prompt exists only where it acts. On a Nintendo pad the north button is a blue X,
	  and the shelf's other lettered prompts are the east button (red) and the west one
	  (green) - so a count of that blue in the legend rows is a count of this prompt.
	*/
	check(legend_colour(COL_SNES_X) > 0, "a card with several files offers a button to cycle them");
	dump("variants-legend-grouped");

	int lone = shelf_go("snes", "Super Metroid (Europe).sfc");
	if (lone < 0) { check(0, "a card with one file is on the root shelf"); return; }
	check(legend_colour(COL_SNES_X) == 0, "and a card with one file does not");

	// X there must do nothing rather than something invisible.
	int was = lib_view_entry(lone)->game;
	press(KEY_TAB, 10);
	check(lib_view_entry(lone)->game == was, "pressing it on such a card changes nothing");

	// Back to the grouped card, and cycle it with the real key.
	card = shelf_go("snes", eu);
	if (card < 0) { check(0, "back on the grouped card"); return; }
	check(lib_view_entry(card)->game == i_eu, "the card comes up on the first of its files");

	press(KEY_TAB, 10);
	check(lib_view_entry(card)->game == i_jp, "X moves it to the next file");
	press(KEY_TAB, 10);
	check(lib_view_entry(card)->game == i_us, "and to the one after");

	/*
	  Suspend points. Only the USA dump has a state on the fake card, so this fails if the
	  strip is read off the card rather than off the file on show - which would offer the
	  player a save state belonging to a different ROM.
	*/
	check((lib_item(i_us)->slots & 3) == 1, "the selected file's own suspend point is found");
	check(lib_item(i_eu)->slots == 0 && lib_item(i_jp)->slots == 0,
		"and the other files behind the card have none");

	// Favourites.
	press(KEY_BACKSPACE, 10);
	check(lib_item(i_us)->fav == 1, "Y favourites the file on show");
	check(lib_item(i_eu)->fav == 0 && lib_item(i_jp)->fav == 0,
		"and not the card, nor the file it came up on");

	/*
	  Per-game core options are keyed by system and path, so the key is per file by
	  construction - and the path it is given comes from what the launch wrote, checked
	  below. This is the construction half.
	*/
	check(core_opts_game_key(lib_item(i_us)->sysidx, us) !=
		core_opts_game_key(lib_item(i_eu)->sysidx, eu),
		"two files of one title have different per-game option keys");

	// The 240p title block: the line that names the file, and that it is only there when
	// the title above does not say which file this is.
	cfg.classicui_profile = 3;
	harness_set_fb(320, 240);
	gfx_shutdown();
	theme_update(320, 240, 3);
	frame(10);
	if (shelf_go("snes", us) < 0) { check(0, "the grouped card is on the 240p shelf"); return; }
	int named = filename_line_pixels();
	dump("variants-240p-named");
	check(named > 0, "at 240p the title block names the file on show");

	if (shelf_go("snes", "Super Metroid (Europe).sfc") < 0)
	{
		check(0, "a card with one file is on the 240p shelf");
		return;
	}
	check(filename_line_pixels() == 0, "and says nothing extra about a card with one file");

	/*
	  A card left sharing a title because grouping it would have been wrong is named too:
	  the Master System and Game Gear Sonic 2 are two cards with one title, and without
	  this the shelf is back to the fault this feature exists to fix.
	*/
	if (shelf_go("sms", "Sonic The Hedgehog 2 (Europe).sms") >= 0)
	{
		check(filename_line_pixels() > 0, "so is a card that only looks like a duplicate");
	}
	else check(0, "the Master System Sonic is on the shelf");

	cfg.classicui_profile = 1;
	harness_set_fb(1280, 720);
	gfx_shutdown();
	theme_update(1280, 720, 1);
	frame(10);

	// Undone before the launch below, so which file the rebuilt shelf lands on is decided
	// by the play count and by nothing else. The tie-break is checked in assert_variants().
	lib_toggle_fav(lib_item(i_us));

	/*
	  The launch. Everything above is state on the card; this is the one that hands a path
	  to the FPGA, and handing over the wrong dump of a game is the most visible way this
	  could be wrong.

	  Cycled to the first file rather than assumed to be there: the shelf has not been
	  rebuilt since the presses above, so it is still showing whatever they left it on -
	  and a check that reads better than it tests is worse than none.
	*/
	card = shelf_go("snes", eu);
	if (card < 0) { check(0, "the grouped card is back"); return; }
	if (!shelf_cycle_to(card, i_eu)) { check(0, "the card can be turned to its first file"); return; }

	press(KEY_TAB, 10);
	press(KEY_TAB, 10);
	check(lib_view_entry(card)->game == i_us, "two presses from the first file reach the third");

	int was_eu = lib_item(i_eu)->plays;
	int was_jp = lib_item(i_jp)->plays;
	int was_us = lib_item(i_us)->plays;

	harness_clear_launch();
	press(KEY_ENTER, 4);
	frame(80);                                  // let the curtain elapse

	char buf[2048] = {};
	if (slurp_file("/tmp/classicui_launch.mgl", buf, sizeof(buf)) > 0)
	{
		check(strstr(buf, us) != 0, "the MGL names the file the card was showing");
		check(strstr(buf, eu) == 0, "and not the one it came up on");
	}
	else check(0, "an MGL was written");

	// What the next core is told it is running, which is where the per-game option key
	// and the reference shot both come from.
	char cur[1024] = {};
	if (slurp_file("/tmp/classicui_current", cur, sizeof(cur)) > 0)
		check(strstr(cur, us) != 0, "and the running-game record names it too");
	else check(0, "the running-game record was written");

	// As a delta, because the section above this one plays every game on the card: an
	// absolute count would pass on a leftover.
	check(lib_item(i_us)->plays == was_us + 1, "the play count went to that file");
	check(lib_item(i_eu)->plays == was_eu && lib_item(i_jp)->plays == was_jp, "and to no other");

	lib_view_build(VIEW_RECENT, -1, SORT_TITLE);
	check(entry_game(0) == lib_item(i_us), "and Recently Played holds that file, not the card");

	/*
	  And the card remembers it. Not in a file of its own: the play count is already
	  per-file and already survives a rescan and a reboot, so the file the player actually
	  plays is the one the card offers from then on.
	*/
	check(lib_item(i_us)->plays > lib_item(i_eu)->plays && lib_item(i_us)->plays > lib_item(i_jp)->plays,
		"that file now has more plays than the others behind its card");

	lib_view_build(VIEW_ALL, -1, SORT_TITLE);
	int back = entry_carrying(i_us);
	check(back >= 0 && lib_view_entry(back)->game == i_us && lib_view_entry(back)->vsel == 2,
		"a rebuilt shelf comes up on the file that was played");

	/*
	  And Times Played has to order the shelf by the file each card is showing. The files
	  behind one card do not share a play count, so a sort that ran before the card chose
	  which of them it stands for would place this one among the games with one play while
	  showing a file with two - which is exactly what this ordering would then not be.
	*/
	int n = lib_view_build(VIEW_ALL, -1, SORT_PLAYS);
	int ordered = 1;
	for (int i = 1; i < n; i++)
	{
		chome_item *a = entry_game(i - 1), *b = entry_game(i);
		if (a && b && a->plays < b->plays) ordered = 0;
	}
	check(ordered, "a card sorts on the play count of the file it is showing");
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
	  This core pauses only while the OSD is open, which used to be unusable here - so the
	  menu held it still with a savestate instead, and these two checks asserted exactly
	  that. Both now say the opposite, because the menu holds OSD_STATUS asserted with no
	  overlay where it shows, so the core's own pause works and no state is needed.

	  Worth being clear that this is a behaviour change and not a test being bent to fit:
	  the freeze is still the right answer for a core with no pause at all, and that path
	  keeps its own coverage under the no-pause fixture.
	*/
	check(harness_osd_status_held(), "OSD_STATUS is held so the core can honour its pause");
	check(harness_pause_val() != 0, "an OSD-gated pause option is used, not written off");
	check(harness_pulses_on("S") == 0, "and no savestate is taken to hold the game still");
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
	  Everything from here to the end of this section is the freeze-and-copy path, which
	  needs a core that cannot pause at all - so it switches to that fixture.

	  It used to run on the fixture above, whose pause is the OSD-gated kind. That was
	  unusable, so the menu froze the game with a state instead and these checks described
	  it. OSD_STATUS is held without an overlay now, so that core pauses properly and never
	  freezes - which is the point of the change, and it is why these had to move rather
	  than be rewritten. The freeze is still exactly right for SNES, SMS, TG16, N64 and
	  Neo Geo, which have no pause in the core at all, and this is now the coverage for it.

	  With no pause, the moment the player wants is already being written by the freeze, so
	  saving is a copy of it rather than a second save: the core is asked for nothing and
	  the menu stays up.
	*/
	press(KEY_MENU, 16);
	frame(6);
	harness_set_confstr(2);
	press(KEY_MENU, 20);
	for (int i = 0; i < 40 && lib_scanning(); i++) frame(2);
	frame(16);
	check(!harness_osd_status_held() || harness_pause_val() == 0,
		"a core with no pause is not claimed to be paused");
	press(KEY_DOWN, 20);

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

	/*
	  Close Game: the menu bar, then two presses on the screen it opens.

	  It was the eleventh row of Options, reached here by wrapping upwards off the first.
	  Reached from the bar now - walked to from the left-hand stop rather than counted from
	  Options, so an entry inserted anywhere in the bar breaks this rather than moving it
	  quietly onto whatever took its place. Which is the menu-bar version of exactly what
	  happened to this walk when Wi-Fi was added to the panel.
	*/
	press(KEY_MENU, 20);
	press(KEY_UP, 14);                        // the menu bar
	for (int i = 0; i < 8; i++) press(KEY_LEFT, 6);
	press(KEY_RIGHT, 8);                      // Options
	press(KEY_RIGHT, 8);                      // Power
	press(KEY_RIGHT, 8);                      // Close Game
	press(KEY_ENTER, 16);
	frame(8);
	check(chome_screen_id() == S_CLOSE_T, "Close Game is on the bar and opens its own screen");
	check(chome_ingame_active(), "and opening it has not closed anything");

	press(KEY_ENTER, 10);
	frame(6);
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
	/*
	  Read at the band's own row, which is theme safe_y and not zero. This used to scan row
	  0 and passed anyway, because the harness runs at 720p where safe_y is 0 - so it could
	  not have caught the band being drawn outside the safe area, and it did not. A CRT did,
	  where 6% of 240 lines put the whole band behind the bezel.
	*/
	frame(6);
	check(band_red_at(theme_get()->safe_y + theme_get()->bar_h),
		"and warns across the top that the game is still playing");
	/*
	  Under the bar, not over it. The band used to be drawn at safe_y - exactly where
	  draw_menubar() puts the bar - so the warning covered every entry on it and the player
	  could read the message but not reach the navigation beneath it. Both rows are asserted
	  because only the pair states the placement: red where it belongs, and no red where the
	  bar's own entries are.
	*/
	check(!band_red_at(theme_get()->safe_y + theme_get()->bar_h / 2),
		"and it does not cover the menu bar it used to be drawn on top of");
	dump("still-playing-warning");

	/*
	  The converse, and the point of the whole band: it is a statement about the *game*, not
	  about the core's feature list. A core that cannot pause but can hold the game still with
	  a save state is, as far as the player is concerned, paused - so it must say nothing.
	  Dinofly's point, and worth a check of its own because the two conditions are set in
	  different places and could drift apart.
	*/
	harness_set_confstr(2);                   // savestates, no pause: the freeze applies
	press(KEY_MENU, 16);
	frame(6);
	press(KEY_MENU, 20);
	for (int i = 0; i < 40 && lib_scanning(); i++) frame(2);
	frame(16);
	check(chome_ingame_active(), "the menu opens on a core that freezes instead of pausing");
	check(!band_red_at(theme_get()->safe_y),
		"a game held still by a save state is not called still playing");
	press(KEY_MENU, 16);
	frame(6);
	harness_set_confstr(0);
	press(KEY_MENU, 20);
	frame(10);
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

/* ------------------------------------------------ library scan slices ----- */

/*
  An order-sensitive fingerprint of the whole index: system, kind, path, title and
  grouping key of every item, in the order the scan produced them.

  This is the load-bearing safety property of the re-slicing, and it is the same
  shape as the byte-identity checks elsewhere in this file. A scan cut into smaller
  slices must produce *exactly* the library the old whole-system slice produced -
  same games, same order, same system assignment - because the order is what the
  shelf's own order is derived from, and every pinned frame in this run would move
  if it changed. The player would see their shelf reorder for no reason they could
  see, which is worse than the stall being fixed.

  32-bit arithmetic on purpose: the number is quoted as a literal below and has to
  mean the same thing on the ARM build as on the host.
*/
static uint32_t lib_fingerprint()
{
	uint32_t h = 2166136261u;
	for (int i = 0; i < lib_item_count(); i++)
	{
		chome_item *it = lib_item(i);
		char rec[CH_PATH_LEN + CH_TITLE_LEN + 64];
		snprintf(rec, sizeof(rec), "%d|%d|%s|%s|%u|",
			(int)it->sysidx, (int)it->kind, it->path, it->title, (unsigned)it->key);
		for (const char *p = rec; *p; p++) { h ^= (unsigned char)*p; h *= 16777619u; }
	}
	h ^= (uint32_t)lib_item_count();
	return h * 16777619u;
}

/*
  And the shelf the index turns into, which is where the dedup grouping lives: the
  item fingerprint above cannot see whether three dumps of one game collapsed to one
  card, or which of them the card settled on. Both views the player actually lands
  on, at the default sort.
*/
static uint32_t view_fingerprint(int v, int sysidx, int sort)
{
	uint32_t h = 2166136261u;
	int n = lib_view_build(v, sysidx, sort);
	for (int i = 0; i < n; i++)
	{
		const chome_entry *e = lib_view_entry(i);
		char rec[CH_TITLE_LEN + 96];
		snprintf(rec, sizeof(rec), "%d|%s|%d|%d|%d|%d|%d|%d|",
			(int)e->kind, e->label, e->view, e->sysidx, e->count,
			e->nvar, e->vsel, (int)e->dup);
		for (const char *p = rec; *p; p++) { h ^= (unsigned char)*p; h *= 16777619u; }
	}
	h ^= (uint32_t)n;
	return h * 16777619u;
}

static unsigned long scan_us()
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (unsigned long)ts.tv_sec * 1000000UL + (unsigned long)(ts.tv_nsec / 1000);
}

/*
  How far past the budget one slice may legitimately land. The bound is checked before
  a unit of work is started, not after, so a slice that was one unit under the budget
  still finishes whatever it began: at worst reading romsets.xml and opening the folder
  it belongs to, which is the most expensive thing the walk starts in one go. Anything
  beyond this is a slice that ignored the bound, which is the freeze coming back.
*/
#define SCAN_SLICE_SLOP 300

/*
  A card the size the review's R3 entry names: a SNES folder of a couple of thousand
  zips, which is the shape that made the stall worth fixing. Every zip is a real
  archive, because opening the central directory of each one is the cost being
  measured - a stand-in for the zip reader would price the wrong thing.

  It is built here and torn down at the end of the section, the same way
  assert_index_cache() borrows the card: the item fingerprint of the ordinary fixture
  is pinned by several dozen other sections and must come back unchanged.
*/
#define STRESS_DIR   ROOT "/games/SNES/Stress"
#define STRESS_ZIPS  2000
#define STRESS_LOOSE 300

static void stress_build()
{
	mkpath(STRESS_DIR);
	mkpath(STRESS_DIR "/B");

	char name[64], inner[64];
	for (int i = 0; i < STRESS_ZIPS; i++)
	{
		snprintf(name, sizeof(name), "Zipped Game %04d (USA).zip", i);
		snprintf(inner, sizeof(inner), "Zipped Game %04d (USA).sfc", i);
		make_zip((i & 3) ? STRESS_DIR : STRESS_DIR "/B", name, inner, 64);
	}
	for (int i = 0; i < STRESS_LOOSE; i++)
	{
		snprintf(name, sizeof(name), "Loose Game %04d (USA).sfc", i);
		touch((i & 1) ? STRESS_DIR : STRESS_DIR "/B", name, 64);
	}
}

static void stress_remove()
{
	char cmd[512];
	snprintf(cmd, sizeof(cmd), "rm -rf %s", STRESS_DIR);
	if (system(cmd)) {}
}

/*
  One full scan, driven straight through lib_scan_step() with no frame loop, timed
  per slice. Returns the fingerprint; fills in the slice count and the worst and
  total slice times.

  Real microseconds, not the virtual clock, and reported as ratios rather than
  device milliseconds - the same discipline as assert_repaint_costs(). The number
  that matters is the *worst* slice, because that is the length of the freeze a
  player sees; an average hides it completely.
*/
/*
  `run` is the longest stretch of consecutive slices spent inside one system, and it is
  the number R3 is actually about. "More slices than there are systems" can be true of a
  walk that still does one system per slice, on a card with more systems than the check
  guessed; this cannot. A walk whose slice is a whole system has a longest run of 1,
  whatever the card looks like, and a card whose entire library sits in one folder - the
  card that made this a freeze - is precisely the one where yielding between systems buys
  nothing.
*/
static uint32_t scan_timed(long budget, int *slices, unsigned long *worst,
                           unsigned long *total, int *run)
{
	lib_scan_test_budget(budget);
	lib_rescan();

	int n = 0, best = 0, cur = 0, prev = -2;
	unsigned long w = 0, t = 0;

	while (lib_scanning() && n < 2000000)
	{
		int sys = lib_scan_sys();
		if (sys >= 0 && sys == prev) cur++;
		else cur = (sys >= 0) ? 1 : 0;
		if (cur > best) best = cur;
		prev = sys;

		unsigned long t0 = scan_us();
		lib_scan_step();
		unsigned long dt = scan_us() - t0;
		if (dt > w) w = dt;
		t += dt;
		n++;
	}

	if (slices) *slices = n;
	if (worst) *worst = w;
	if (total) *total = t;
	if (run) *run = best;
	return lib_fingerprint();
}

/*
  The same scan through the real frame loop instead, which is the number that decides
  the budget: chome_handle() rebuilds the shelf view after every slice, and that is
  O(n log n) in the library, so a budget small enough to be invisible to a player is
  also a budget that pays for a re-sort thousands of times. Returns frames spent, and
  fills in the worst single frame - a frame here is a slice plus everything the front
  end does with it, which is what actually freezes the picture.
*/
static int scan_framed(long budget, unsigned long *worst, unsigned long *total)
{
	lib_scan_test_budget(budget);
	lib_rescan();

	int frames = 0;
	unsigned long w = 0, t = 0;
	while (lib_scanning() && frames < 200000)
	{
		unsigned long t0 = scan_us();
		harness_advance(16);
		chome_handle(0);
		unsigned long dt = scan_us() - t0;
		if (dt > w) w = dt;
		t += dt;
		frames++;
	}

	if (worst) *worst = w;
	if (total) *total = t;
	return frames;
}

static void assert_scan_slices()
{
	printf("\n== library scan slices (host times: read ratios, not milliseconds) ==\n");

	/*
	  Where the cursor was, before anything here moves it. The shelf position is inherited
	  by design - "re-entry keeps the previous shelf position", see select_first_game() -
	  and this section is the only one that drives the shelf during a scan, so it is the
	  only one that has to walk the cursor back.

	  Not tidiness. assert_config_check() presses DOWN on the shelf and asserts the shelf is
	  still up, which is only true over a *folder*: down over a game opens the suspend strip.
	  A section that moved the cursor by one and left it there breaks a check eight thousand
	  lines away with nothing on screen to connect the two, which is how this note came to
	  be written.
	*/
	int was_sel = chome_sel_index();
	int was_screen = chome_screen_id();

	// The fixture card as every other section has it, so the section can prove it
	// handed it back.
	lib_scan_test_budget(0);
	lib_rescan();
	for (int i = 0; i < 40000 && lib_scanning(); i++) lib_scan_step();
	int fixture_items = lib_item_count();
	uint32_t fixture_fp = lib_fingerprint();
	uint32_t fixture_root = view_fingerprint(VIEW_ROOT, -1, SORT_TITLE);
	uint32_t fixture_all = view_fingerprint(VIEW_ALL, -1, SORT_TITLE);
	printf("  fixture: %d items, index fp %08x, root view fp %08x\n",
		fixture_items, fixture_fp, fixture_root);

	/*
	  The fixture card's own fingerprint, as a literal.

	  This number was read off the build *before* the walk was re-sliced - the one whose
	  slice was a whole system and whose recursion was the C stack. It is the only check
	  in this section that ties the new walk to the old code rather than to itself, so it
	  is the one that would catch a re-slicing that reordered the library consistently at
	  every slice size. If a fixture is deliberately added to build_fake_sd() this has to
	  be re-read from a whole-system build, not merely updated to whatever comes out.
	*/
	/*
	  Re-read after assert_screenscraper()'s Neo Geo Pocket fixture (a .npc, added so that
	  section has a real "no systemeid" game once every other system on the shelf got one -
	  see chome_ss.cpp) changed what build_sd() puts on the fake card, exactly as the note
	  above says to do rather than leaving the old literal to fail here forever.
	*/
	check(fixture_fp == 0x98342fd1,
		"the sliced walk produces the library the whole-system walk produced, item for item");
	check(fixture_root == 0xdbc6df21, "and the shelf it builds, card for card and group for group");

	stress_build();

	/*
	  The sweep. Every slice size the walk can be driven at, on a card the shape the
	  review's R3 entry names - a couple of thousand zipped ROMs in one folder, which is
	  where the cost is, because each one has its central directory opened and read.

	  The first row is the walk with no bound at all: one call does the entire scan, which
	  is what the old code did per system and is the "before" figure. The last is a bound
	  of one cost unit, which yields at every opportunity there is.
	*/
	printf("  budget  slices  worst slice  walk total    frames  worst frame  wall total\n");

	static const long budgets[] = { 1 << 28, 12000, 6000, 3000, 1500, 600, 1 };
	uint32_t fp0 = 0;
	int rows = 0;
	unsigned long whole_walk = 0;

	for (size_t i = 0; i < sizeof(budgets) / sizeof(budgets[0]); i++)
	{
		int slices = 0;
		unsigned long worst = 0, total = 0;
		uint32_t fp = scan_timed(budgets[i], &slices, &worst, &total, 0);

		if (!i) whole_walk = total;

		/*
		  The frame-loop figure only for the sizes worth shipping. A budget of 1 through
		  chome_handle() is tens of thousands of shelf re-sorts over a 2300-game library
		  and would dominate this section's own runtime - which is the finding, not an
		  obstacle to it: see the walk-total column instead.
		*/
		unsigned long fworst = 0, ftotal = 0;
		int frames = 0;
		if (budgets[i] >= 600) frames = scan_framed(budgets[i], &fworst, &ftotal);

		if (frames)
		{
			printf("  %6ld  %6d  %11lu  %10lu  %8d  %11lu  %10lu\n",
				budgets[i], slices, worst, total, frames, fworst, ftotal);
		}
		else
		{
			printf("  %6ld  %6d  %11lu  %10lu         -            -           -\n",
				budgets[i], slices, worst, total);
		}

		if (!rows++) fp0 = fp;
		else if (fp != fp0)
		{
			char what[160];
			snprintf(what, sizeof(what), "budget %ld produces the same library as one unbounded slice",
				budgets[i]);
			check(0, what);
		}
	}

	/*
	  One assertion for the whole sweep, because it is the property the budget being a
	  free parameter rests on: slicing the walk finer cannot change what it finds or the
	  order it finds it in. Fifty-something separate checks saying the same thing would
	  bury it.
	*/
	check(rows == (int)(sizeof(budgets) / sizeof(budgets[0])),
		"every slice size from unbounded down to one entry produces an identical library");

	/*
	  And the shipped budget against the walk it divides, which is the finding R3 named
	  stated as a check rather than as a column of numbers.

	  Two claims, both about the *shape* of the division rather than about host
	  milliseconds - a machine four times as fast would move every number in the table
	  above and neither of these:

	    the worst slice is a small fraction of the whole walk, so the walk really is
	    divided and not merely renamed. Before this change the worst slice *was* the walk,
	    because the slice was a whole system and one system held every game;

	    and it is divided *inside* a system. One slice per system is what R3 found; a card
	    whose whole library sits in one folder is exactly the card that made it a freeze,
	    and it is not helped at all by yielding between systems.
	*/
	int shipped_slices = 0, shipped_run = 0;
	unsigned long shipped_worst = 0, shipped_total = 0;
	scan_timed(0, &shipped_slices, &shipped_worst, &shipped_total, &shipped_run);

	printf("  shipped: %d slices, worst slice %lu us against %lu us for the whole walk;"
		" longest run inside one system %d, over %d systems\n",
		shipped_slices, shipped_worst, whole_walk, shipped_run, lib_sys_count());

	check(shipped_worst * 8 < whole_walk,
		"the worst slice is a fraction of the walk rather than the whole of it");
	check(shipped_run > 1,
		"and the walk is divided inside a system, not merely between systems");

	// And the shelf built from it, at the shipped budget, against one unbounded slice.
	scan_timed(1 << 28, 0, 0, 0, 0);
	uint32_t whole_root = view_fingerprint(VIEW_ROOT, -1, SORT_TITLE);
	uint32_t whole_sys = view_fingerprint(VIEW_SYSTEMS, -1, SORT_TITLE);
	scan_timed(0, 0, 0, 0, 0);
	check(view_fingerprint(VIEW_ROOT, -1, SORT_TITLE) == whole_root,
		"the shelf a sliced scan builds is the shelf a whole-system scan built");
	check(view_fingerprint(VIEW_SYSTEMS, -1, SORT_TITLE) == whole_sys,
		"and so is the systems list, with the same counts behind each folder");

	/*
	  Responsiveness, which is the whole point and is not implied by the scan finishing:
	  the pad has to be serviced *between* slices and the picture has to keep arriving.

	  Driven through chome_handle() at the shipped budget, on the big card, with the scan
	  deliberately left running. A press that lands while the walk is mid-folder has to
	  move the selection on the frame it lands, and the framebuffer has to be flipped
	  while the scan is still going - a shelf that only repainted at the end would look
	  exactly like the freeze this replaced.
	*/
	lib_scan_test_budget(0);
	harness_set_menu_core(1);
	harness_set_fb(1280, 720);
	gfx_shutdown();
	theme_update(1280, 720, 0);
	chome_leave();
	lib_rescan();
	press(KEY_MENU, 0);

	int scan_slices_seen = 0;
	long cost_max = 0, budget_now = 0;

	/*
	  Frame by frame from the first one, because the scan is short in host time even on
	  this card - 2300 games in a couple of dozen slices - and a fixed wait long enough to
	  be interesting would be long enough to miss the whole thing. Everything asserted
	  here is asserted from inside the loop, while lib_scanning() is true.
	*/
	int flips0 = harness_present_count();
	int painted_mid = 0, alive = 0, moved_at = -1, frames = 0;

	while (lib_scanning() && frames < 200000)
	{
		if (harness_present_count() > flips0) painted_mid = 1;

		if (!alive && lib_view_count() >= 3)
		{
			int before = chome_sel_index();
			chome_handle(KEY_RIGHT);
			harness_advance(16);
			chome_handle(KEY_RIGHT | UPSTROKE);
			harness_advance(16);
			chome_handle(0);
			frames += 2;
			if (chome_sel_index() != before) { alive = 1; moved_at = frames; }
			continue;
		}

		harness_advance(16);
		chome_handle(0);
		frames++;
	}

	lib_scan_stats(&scan_slices_seen, &cost_max, &budget_now);

	check(frames > 1, "a big card's scan spans many frames rather than one long one");
	check(painted_mid, "the picture keeps arriving while the scan runs");
	check(alive, "and a press mid-scan moves the selection on the frame it lands");
	printf("  %d frames, %d slices; the pad was answered on frame %d of the scan\n",
		frames, scan_slices_seen, moved_at);

	printf("  shipped budget %ld: %d slices, worst slice %ld cost units\n",
		budget_now, scan_slices_seen, cost_max);
	check(cost_max <= budget_now + SCAN_SLICE_SLOP,
		"no slice overran the budget by more than the one entry it was allowed to start");

	stress_remove();
	lib_scan_test_budget(0);
	lib_rescan();
	for (int i = 0; i < 40000 && lib_scanning(); i++) lib_scan_step();

	/*
	  What the player is told while it happens, at 240p - the one profile with nothing to
	  tell them.

	  At 720p and 480p draw_position() already reads "n / m SCANNING" and the count climbs,
	  which is honest progress and became legible for the first time here: while a slice was
	  a whole system the frame loop never ran during a walk, so nobody ever saw it move. At
	  240p that line does not exist at all (draw_position() returns before it), and the
	  re-slicing is what made that a gap: the shelf is now live *during* a scan, so a CRT
	  player watches cards appear one at a time with nothing on screen to say why.

	  Read inside the Computers view, on one of its browse cards, and both of those choices
	  are what make the check mean anything.

	  A browse card's meta line is the fixed sentence "BROWSE DISKS AND TAPES" - no count in
	  it - so a difference in that band cannot be the library growing. And the Computers view
	  is built from the systems table rather than from the index (see lib_view_build()), so it
	  does not change at all while a scan runs: the same cards, in the same order, whatever
	  the walk has found. Every other shelf fails one of those two tests. The first attempt
	  read the *root* Computers folder, whose line is "n SYSTEMS", and it passed against a
	  build with the notice removed - the count alone was moving it.

	  Parked before the rescan rather than after it, too: the whole fixture card is 132 slices
	  at the finest slice there is, and select_folder() rewinds with forty presses, so
	  navigating during a scan finishes the scan. The cursor keeps its index across a rescan.
	*/
	harness_set_fb(320, 240);
	gfx_shutdown();
	theme_update(320, 240, 0);
	chome_leave();
	press(KEY_MENU, 6);

	const chome_profile *lp = theme_get();
	int m0 = lp->y_meta, m1 = lp->y_meta + 8 * lp->ts_ui;
	int t0 = lp->y_title, t1 = lp->y_title + 8 * lp->ts_title;

	check(select_folder("Computers"), "the Computers folder is on the 240p shelf");
	press(KEY_ENTER, 10);
	frame(8);

	const chome_entry *be = lib_view_entry(chome_sel_index());
	check(be && be->kind == ENT_BROWSE, "and behind it a browse card, whose line carries no count");
	check(!lib_scanning(), "nothing is being scanned, which is the quiet reading");
	unsigned long meta_quiet = harness_fb_hash(m0, m1);
	unsigned long title_quiet = harness_fb_hash(t0, t1);

	lib_scan_test_budget(1);            // the finest slice there is, so the scan lasts
	lib_rescan();
	frame(4);

	check(lib_scanning(), "a scan is running under the same card");
	check(harness_fb_hash(m0, m1) != meta_quiet,
		"320x240: the shelf says so, on the one canvas with no position line to say it");
	check(harness_fb_hash(t0, t1) == title_quiet,
		"and the line above it is untouched, so that difference is the notice and not the shelf moving");

	lib_scan_test_budget(0);
	for (int i = 0; i < 40000 && lib_scanning(); i++) frame(1);
	frame(8);
	check(harness_fb_hash(m0, m1) == meta_quiet, "and it stops saying it when the scan ends");
	press(KEY_ESC, 10);

	// The fixture card, one more time, against the numbers taken at the top.
	lib_scan_test_budget(0);
	lib_rescan();
	for (int i = 0; i < 40000 && lib_scanning(); i++) lib_scan_step();
	check(lib_item_count() == fixture_items, "the stress card is off the fixture again");
	check(lib_fingerprint() == fixture_fp, "and the fixture library is byte-identical to before");
	check(view_fingerprint(VIEW_ROOT, -1, SORT_TITLE) == fixture_root, "the root shelf too");
	check(view_fingerprint(VIEW_ALL, -1, SORT_TITLE) == fixture_all, "and every game in one list");

	/*
	  And everything the section borrowed, handed back: the canvas, the shelf position, and
	  the entry list itself.

	  The last of those is not obvious. view_fingerprint() builds views through
	  lib_view_build(), which writes the one entry array the front-end draws from - so the
	  checks above leave the shelf holding a list of every game while the front-end still
	  believes it is showing the root. A folder opened and closed puts that right, because
	  nav_push() and nav_pop() each rebuild the view from the front-end's own state, and
	  nav_pop() restores the cursor it saved on the way in.
	*/
	harness_set_fb(1280, 720);
	gfx_shutdown();
	theme_update(1280, 720, 0);
	chome_leave();
	press(KEY_MENU, 6);

	// Rewound first, so the folder opened below is entry 0 - Favourites, which is always a
	// folder. Opening whatever `was_sel` happens to be would launch a game.
	shelf_rewind();
	press(KEY_ENTER, 8);
	press(KEY_ESC, 8);
	for (int i = 0; i < was_sel; i++) press(KEY_RIGHT, 1);
	frame(6);

	check(chome_sel_index() == was_sel, "the section hands the shelf position back");
	if (was_screen != chome_screen_id())
	{
		printf("  note: the front-end was on screen %d on the way in and is on %d on the way out\n",
			was_screen, chome_screen_id());
	}
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

	/*
	  And the save-state strip, at the two ends of the margin and at the one he runs.

	  The strip is the element the margin does the most to: it rests above the margin and is
	  filled down through it to the bottom edge, so the panel starts higher up as the margin
	  grows and the strip keeps its room. The tiles inside it are sized from that room
	  (chome_theme.cpp), which is why this walks three margins rather than trusting one: the
	  same code lays the row out three different sizes, and the row has to stay inside the
	  panel and out of the legend and the margin at every one of them.

	  The panel top is read off the pixels, like the rest of this section, because that is
	  where a bug in this element would be - the metric it comes from can be right while the
	  draw call puts the panel somewhere else.
	*/
	{
		uint8_t was_over = cfg.classicui_overscan;
		static const uint8_t margins[3] = { 0, 6, 15 };

		for (int i = 0; i < 3; i++)
		{
			cfg.classicui_overscan = margins[i];
			theme_invalidate();
			theme_update(320, 240, 3);
			chome_leave();
			press(KEY_MENU, 20);
			for (int j = 0; j < 40 && lib_scanning(); j++) frame(2);
			frame(12);

			check(select_titled("Super Metroid") != 0, "a game with suspend points at 240p");
			press(KEY_DOWN, 25);

			int top = strip_top_row();

			// Where the legend takes the band over, and how far down the tile row and the
			// slot number under it reach - draw_suspend() draws the caption 5 px below the
			// tile. The row is centred, and the focus frame is 3 px outside the tile it is
			// drawn around, so this is the leftmost pixel the row can put on screen.
			int band = p->y_legend - 6 * p->ts_ui;
			int row_bottom = top + p->thumb_y + p->thumb_h + 5 + 8 * p->ts_tiny;
			int row_left = (p->w - (3 * p->thumb_w + 2 * p->thumb_gap)) / 2 - 3;

			printf("  overscan %d: margin %d, strip top %d, panel %d tall, tile %dx%d\n",
				margins[i], p->safe_y, top, p->h - top, p->thumb_w, p->thumb_h);

			check(top == p->h - p->safe_y - p->strip_h, "the strip rests above the margin");
			check(top + p->strip_h + p->safe_y == p->h,
				"and is filled down through it to the bottom edge");
			check(row_bottom <= band && row_left >= p->inset,
				"the tiles and their slot numbers stay inside the panel");
			check(margin_bright(p, 0) == 0, "and out of the margin at the bottom");

			char nm[64];
			snprintf(nm, sizeof(nm), "overscan-strip-%d", margins[i]);
			dump(nm);
			press(KEY_ESC, 12);
		}

		// Back to the margin the rest of this section was written against.
		cfg.classicui_overscan = was_over;
		theme_invalidate();
		theme_update(320, 240, 3);
		chome_leave();
		press(KEY_MENU, 20);
		for (int j = 0; j < 40 && lib_scanning(); j++) frame(2);
		frame(12);
	}

	/*
	  And the still-playing band, which is the one thing that got this wrong: drawn at row 0
	  it sat entirely behind a CRT's bezel, and every check for it passed because they all
	  ran at 720p where safe_y is 0. This is the profile where the difference exists.
	*/
	/*
	  This needs a game core, not the menu core: the band only exists over a running game,
	  which is why a first attempt at putting these checks here silently skipped them behind
	  an if - coverage that looked like coverage and was not.
	*/
	{
		FILE *f = fopen("/tmp/classicui_current", "wt");
		if (f) { fprintf(f, "gb\nTetris (World).gb\n"); fclose(f); }
	}
	harness_set_menu_core(0);
	harness_set_confstr(0);                 // no pause and no savestates: the band applies
	chome_leave();
	chome_handle(0);
	if (chome_ingame_active()) { press(KEY_MENU, 16); frame(6); }
	press(KEY_MENU, 20);
	for (int i = 0; i < 40 && lib_scanning(); i++) frame(2);
	frame(16);

	check(chome_ingame_active(), "the in-game menu opens at 240p");
	check(band_red_at(p->safe_y + p->bar_h), "the still-playing band is inside the safe area");
	check(!band_red_at(p->safe_y + p->bar_h / 2),
		"and clears the menu bar rather than covering its entries");
	check(!band_red_at(0), "and not at the very top, where a television hides it");
	dump("overscan-still-playing");

	press(KEY_MENU, 16);
	frame(6);
	harness_set_menu_core(1);
	harness_set_confstr(1);

	// Hand the canvas back as it was found: this section is the only one that pins
	// a small one, and what follows should not have to know that.
	cfg.classicui_profile = 0;
	harness_set_fb(1280, 720);
	gfx_shutdown();
	theme_update(1280, 720, 0);
}

/*
  The canvas whose pixels are not square.

  A 15 kHz TV mode is 640x240 (or 640x288 with menu_pal=1) and the scaler stretches the
  framebuffer across the whole of it, so those pixels are twice as tall as they are
  wide. video.cpp halves the width before handing it over - but only while the front-end
  holds the analog output itself. With vga_scaler=1, or with direct_video, there is no
  takeover and the full width arrives.

  That is the second half of a user's report: "i managed to get some correct ish output
  when enabled vga_scaler to 1 but colours disappeared and interface was smushed
  together with buttons overlaping". The colour is not ours to fix (see
  assert_analog_report()); the smushing was. At 640x240 the width alone chose the SD
  profile, SD makes a card a quarter of the width, the 228x167 ratio turned 160 px into
  117 lines, and the selected card's 152 lines did not fit in a 240-line canvas at all -
  so both clamps in theme_update() saturated and the position line landed *below* the
  button legend and was drawn over it.

  Measured against the 240p layout rather than against constants: 320x240 is the shape
  that has been on Dinofly's CRT since the beginning, and what the stretched canvas has to
  produce is that same layout, twice as wide. Every vertical metric must match it
  exactly; every horizontal one must be double, within the rounding two integer
  divisions can differ by.
*/
static void assert_stretched_canvas()
{
	printf("\n== a stretched 15 kHz canvas ==\n");

	cfg.classicui_profile = 0;
	theme_invalidate();
	theme_update(320, 240, 0);
	const chome_profile lo = *theme_get();

	theme_invalidate();
	theme_update(640, 240, 0);
	const chome_profile *p = theme_get();

	printf("  320x240: px %d, %s, card %dx%d, y_pos %d, y_legend %d\n",
		lo.px, lo.name, lo.card_w, lo.card_h, lo.y_pos, lo.y_legend);
	printf("  640x240: px %d, %s, card %dx%d, y_pos %d, y_legend %d\n",
		p->px, p->name, p->card_w, p->card_h, p->y_pos, p->y_legend);

	check(lo.px == 1, "a 320x240 canvas has square pixels");
	check(p->px == 2, "a 640x240 canvas is recognised as half-width pixels");
	check(p->id == PROF_LO, "and takes the 240p profile, not the one its width suggests");

	// The bug itself. Before the fix this was y_pos 198 against y_legend 194.
	check(p->y_pos < p->y_legend, "the position line stays above the button legend");
	check(p->y_pos + 8 * p->ts_ui <= p->y_legend - 6 * p->ts_ui,
		"clear of the legend's own band, not merely above its text");

	check(p->ts_title == lo.ts_title && p->ts_ui == lo.ts_ui && p->ts_tiny == lo.ts_tiny,
		"the text scales are the 240p ones");
	check(p->visible == lo.visible, "and the shelf shows the same number of cards");

	check(p->card_h == lo.card_h && p->sel_h == lo.sel_h,
		"a card is exactly as tall as it is at 240p");
	check(p->card_w == lo.card_w * 2 && p->sel_w == lo.sel_w * 2,
		"and exactly twice as wide, which is the same picture");

	check(p->y_title == lo.y_title && p->y_meta == lo.y_meta && p->y_shelf == lo.y_shelf &&
		p->y_pips == lo.y_pips && p->y_pos == lo.y_pos && p->y_legend == lo.y_legend,
		"every band sits on the same line as it does at 240p");
	check(p->bar_h == lo.bar_h && p->strip_h == lo.strip_h && p->row_h == lo.row_h &&
		p->panel_h == lo.panel_h, "and every height that is not derived from a width");
	check(p->thumb_h == lo.thumb_h, "the suspend tiles keep their 240p height");

	// Two integer divisions can disagree by a pixel about half of an odd number.
	check(abs(p->pitch - 2 * lo.pitch) <= 2, "the shelf pitch is doubled");
	check(abs(p->inset - 2 * lo.inset) <= 2 && abs(p->gap - 2 * lo.gap) <= 2,
		"so are the inset and the gap");
	check(abs(p->thumb_w - 2 * lo.thumb_w) <= 2 && abs(p->panel_w - 2 * lo.panel_w) <= 2,
		"and the tile and panel widths");

	/*
	  The PAL member of the pair, which is what menu_pal=1 gives, and which is wider
	  relative to its height by less - so it is the one that would fall the wrong side of
	  the test if the test were a fixed ratio.
	*/
	theme_invalidate();
	theme_update(640, 288, 0);
	check(theme_get()->px == 2 && theme_get()->id == PROF_LO,
		"a 640x288 PAL canvas is stretched too");
	check(theme_get()->y_pos < theme_get()->y_legend, "and lays out without a collision");

	/*
	  And the canvases that are not stretched, because the shape test must not fire on
	  16:9. This is the regression guard for every layout that already worked: the four
	  the profile walk uses, plus the two 15 kHz modes seen through the takeover, which
	  arrive halved.
	*/
	{
		static const struct { int w, h; const char *what; } square[] =
		{
			{ 1280, 720, "1280x720" },
			{  960, 540, "960x540 (1080p at fb_size=2)" },
			{  640, 480, "640x480" },
			{  720, 480, "720x480" },
			{  320, 240, "320x240 (a halved NTSC TV mode)" },
			{  320, 288, "320x288 (a halved PAL TV mode)" },
		};

		int all = 1;
		for (size_t i = 0; i < sizeof(square) / sizeof(square[0]); i++)
		{
			theme_invalidate();
			theme_update(square[i].w, square[i].h, 0);
			if (theme_get()->px != 1)
			{
				printf("  %s came out stretched\n", square[i].what);
				all = 0;
			}
		}
		check(all, "no canvas up to 16:9 is treated as stretched");
	}

	/*
	  A forced profile still outranks the canvas - classicui_profile is there to be
	  believed - but the surface is not a matter of opinion, so the shapes are corrected
	  under it as well. This is the case the reporter was in when they tried hd, sd and
	  lo and none of them changed anything.
	*/
	theme_invalidate();
	theme_update(640, 240, 2);
	check(theme_get()->id == PROF_SD, "classicui_profile=2 still forces SD on a TV canvas");
	check(theme_get()->px == 2 && theme_get()->y_pos < theme_get()->y_legend,
		"and the forced layout fits, where before it overlapped");

	theme_invalidate();
	theme_update(1280, 720, 0);
}

/*
  What the front-end tells somebody on a television, and what it refuses to tell
  everybody else.

  vp_analog_facts() is a pure function of cfg and of whether an HDMI sink is attached,
  so it is driven directly here rather than through the UI: what matters is which facts
  a configuration produces, and the panel only draws whatever comes back.
*/
static void assert_analog_report()
{
	printf("\n== the analog video report ==\n");

	const uint8_t was_dv = cfg.direct_video;
	const uint8_t was_vs = cfg.vga_scaler;
	const uint8_t was_fs = cfg.forced_scandoubler;
	const uint8_t was_pal = cfg.menu_pal;
	const char was_vm = cfg.vga_mode_int;

	cfg.direct_video = 0;
	cfg.vga_scaler = 0;
	cfg.forced_scandoubler = 0;
	cfg.menu_pal = 0;
	cfg.vga_mode_int = 0;

	// The ordinary machine: HDMI, nothing set about the analog port. Silence.
	check(vp_analog_facts(1) == 0, "an HDMI machine on defaults is told nothing");
	check(vp_analog_facts(-1) == 0, "and so is one whose HDMI state cannot be read");

	/*
	  No sink on HDMI is itself a reason to speak, even on a default vga_mode: the
	  framebuffer is being put on the analog port, and if there is no television there
	  either then nobody sees the report anyway.
	*/
	check(vp_analog_facts(0) == VP_AN_60HZ,
		"with no HDMI sink the takeover's 60 Hz is named");
	cfg.menu_pal = 1;
	check(vp_analog_facts(0) == 0, "and menu_pal=1 answers that, so nothing is said");
	cfg.menu_pal = 0;

	// S-Video with a display on HDMI: the front-end is not on the CRT at all, which is
	// the whole of what the player needs to know and the one thing nothing said before.
	cfg.vga_mode_int = 2;
	check(vp_analog_facts(1) == VP_AN_NOTUS,
		"S-Video plus HDMI: the CRT is showing the core, not this menu");
	check(!(vp_analog_facts(1) & VP_AN_MONO),
		"and nothing about colour, because the menu is not on that wire");

	// The same card with the HDMI lead pulled out: now it is, and now it is grey.
	check(vp_analog_facts(0) == (VP_AN_MONO | VP_AN_60HZ),
		"S-Video alone: black and white, at 60 Hz");

	// Composite is the same wire as far as this is concerned.
	cfg.vga_mode_int = 3;
	check(vp_analog_facts(0) & VP_AN_MONO, "composite loses the colour the same way");

	// An external encoder taking a subcarrier from the FPGA: sys_top gates that with
	// ~vgas_en, so it goes the same way as the built-in one.
	cfg.vga_mode_int = 4;
	check(vp_analog_facts(0) & VP_AN_MONO, "so does an external encoder");

	// Component carries no subcarrier, so there is no colour to lose - only the 60 Hz
	// that the first report of this class was about.
	cfg.vga_mode_int = 1;
	check(vp_analog_facts(0) == VP_AN_60HZ, "component keeps its colour and is told so");

	/*
	  vga_scaler=1 routes the analog port to the scaler permanently, so there is no
	  takeover and no TV mode of ours - the refresh is the player's own - but the colour
	  is gone for exactly the same reason.
	*/
	cfg.vga_mode_int = 2;
	cfg.vga_scaler = 1;
	check(vp_analog_facts(1) == VP_AN_MONO,
		"vga_scaler=1 is black and white on S-Video, HDMI or not");
	check(vp_analog_facts(0) == VP_AN_MONO, "and says nothing about a refresh it did not set");
	cfg.vga_scaler = 0;

	/*
	  direct_video puts the framebuffer on the analog port through the same scaler leg,
	  and video_mode_load() still scandoubles it when forced_scandoubler is set - which
	  on an S-Video output is a 31 kHz signal no television can lock to. tv_fb_mode()
	  no longer does that to the takeover; this path is not the front-end's to change,
	  so it is named.
	*/
	cfg.direct_video = 1;
	check(vp_analog_facts(1) == VP_AN_MONO, "direct_video on S-Video: black and white");
	cfg.forced_scandoubler = 1;
	check(vp_analog_facts(1) == (VP_AN_31K | VP_AN_MONO),
		"and with forced_scandoubler=1 the 31 kHz output is called out");

	// The severe one has to come first, because the panel shows the first three.
	check(VP_AN_31K < VP_AN_NOTUS && VP_AN_NOTUS < VP_AN_MONO && VP_AN_MONO < VP_AN_60HZ,
		"the facts are ordered worst first");

	// Nothing can produce more than the panel has room for.
	check(VP_AN_MAX >= 3, "the panel has room for every set that can hold at once");

	// Every bit has a line, and every line fits the 240p panel: 33 characters at
	// ts_ui 1 with the inset the 240p profile uses. Measured, not assumed.
	{
		int all = 1;
		for (int bit = 1; bit <= VP_AN_LAST; bit <<= 1)
		{
			const char *t = vp_analog_text(bit);
			if (!t || !*t || strlen(t) > 33)
			{
				printf("  fact 0x%02x: %s\n", bit, t ? t : "(no line)");
				all = 0;
			}
		}
		check(all, "every fact has a line that fits a 240p panel");
	}
	check(!vp_analog_text(VP_AN_31K | VP_AN_MONO), "and a pair of bits is not a line");

	cfg.direct_video = was_dv;
	cfg.vga_scaler = was_vs;
	cfg.forced_scandoubler = was_fs;
	cfg.menu_pal = was_pal;
	cfg.vga_mode_int = was_vm;
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

/* ------------------------------------------------------------ typography -- */

/*
  One glyph, read off the screen and compared with the raster charfont holds for it.

  Both directions, and the second is what makes it a test rather than a coincidence: every
  inked pixel of the cell has to be the colour asked for, and every pixel the glyph does not
  ink has to be something else. A cell filled solid passes the first half of that on any
  glyph, and fails the second on all of them.

  charfont is column-major - byte n is column n, bit y is row y - which is draw_glyph()'s own
  reading of it. Deliberately the same source rather than a hard-coded bitmap: the question
  asked here is "which character was drawn", and taking the shape from the table is how that
  question stays answerable after somebody loads a different font.
*/
static int glyph_seen(int x, int y, int s, unsigned char code, uint32_t col)
{
	const uint32_t *fb = harness_fb_shown();
	int w = gfx_w(), h = gfx_h();
	if (!fb || s < 1) return 0;

	int ink = 0;
	for (int gx = 0; gx < 8; gx++)
	{
		for (int gy = 0; gy < 8; gy++)
		{
			int on = (charfont[code][gx] & (1 << gy)) ? 1 : 0;
			int px = x + gx * s, py = y + gy * s;
			if (px < 0 || py < 0 || px + s > w || py + s > h) return 0;

			uint32_t got = fb[(size_t)py * w + px] | 0xff000000u;
			if (on && got != col) return 0;
			if (!on && got == col) return 0;
			if (on) ink++;
		}
	}

	// A blank cell matches every blank cell, so it is not evidence of anything.
	return ink > 0;
}

/*
  A .pf on the card, generated rather than shipped.

  768 bytes, which is the size LoadFont() treats as "chars 32 upwards and no header": every
  printable character becomes a solid six-column block, so the transposed result is columns
  0..5 set and 6..7 clear for all of them. Six rather than eight because that is what the
  ROM font really uses (no printable stock glyph inks column 8) and a test font that filled
  the cell would be testing a shape the front-end's own layout was never fitted to.

  Space is left blank. A font whose space was a solid block would make every screen in the
  harness unreadable in a way no assertion below is about.
*/
static void make_pf(const char *path)
{
	unsigned char buf[768];
	for (int c = 32; c < 128; c++)
		for (int r = 0; r < 8; r++)
			buf[(c - 32) * 8 + r] = (c == 32) ? 0x00 : 0xFC;

	FILE *f = fopen(path, "wb");
	if (!f) return;
	fwrite(buf, 1, sizeof(buf), f);
	fclose(f);
}

/*
  Letter spacing, the capitals switch, and the font.

  Three settings that all mean the same thing to this file: they change the size, the shape
  or the case of every string the front-end draws. So what is checked is not that they look
  right - nothing here can judge that - but the four properties that make them safe to ship:

  1. gfx_text_w() and gfx_text_cols() are exact inverses at every tracking value and every
     scale. That is the invariant the disc dialog rests on, and the one whose absence once
     produced "Super Nintendo (n>": the panel is widened to hold a measured title and the
     title is then clipped against the same width by a different sum.

  2. A panel sized from its own text still contains that text, measured off the screen,
     at each of the five tracking values.

  3. The capitals switch is exactly a case change: on, the frame is bit for bit the one the
     rest of this harness pins; off, the glyph drawn in a known position is the lowercase
     letter and not the uppercase one.

  4. A font can be put back. LoadFont() overwrites the only copy of the glyph table in the
     process, so "restore" has to be byte-exact over all 2048 of them, and a file that will
     not load has to leave the previous font alone rather than blanking the screen.

  5. Asking "does this string fit in this many pixels" and asking "is this string longer
     than the number of characters that fit" are the same question of the built-in font.
     That equivalence is what makes every screen in chome_ui.cpp safe to have moved from the
     second form to the first: it says the move is a change of what the code means and not
     of what it draws. It is asserted over every string length, both sides of every
     boundary, at every scale and every letter-spacing value - because the interesting cases
     are exactly the ones where a panel is one pixel too narrow, and no screenshot suite
     lands on those on purpose.

  Run last, and it puts the built-in font, tracking 0 and the capitals back on the way out:
  every pixel assertion above this line was taken under those three.
*/
static void assert_typography()
{
	printf("\n== typography: letter spacing, capitals, and the font ==\n");

	int8_t was_track = cfg.classicui_tracking;
	uint8_t was_caps = cfg.classicui_caps;

	/* ------------------------------------------------- the advance model --- */

	/*
	  Tracking 0 is the model every panel in the front-end was measured against, so it is
	  asserted as an identity rather than as a range: strlen * 8 * scale, which is what
	  gfx_text_w() was before there was a setting.
	*/
	cfg.classicui_tracking = 0;
	{
		static const char *w[] = { "", "A", "SNES", "Super Nintendo (not yet)" };
		int same = 1;
		for (unsigned i = 0; i < sizeof(w) / sizeof(w[0]); i++)
			for (int s = 1; s <= 3; s++)
				if (gfx_text_w(w[i], s) != (int)strlen(w[i]) * 8 * s) same = 0;

		check(same, "at zero tracking a string measures exactly what it always did");
		check(gfx_adv(1) == 8 && gfx_adv(2) == 16 && gfx_adv(3) == 24,
			"and one character advances by the glyph cell");
	}

	/*
	  Monotonic, and strictly so: a setting whose middle two values measured the same would
	  be a dial with a flat spot on it. Checked on a run long enough for one font pixel per
	  gap to be visible at every scale.
	*/
	{
		const char *t = "Super Nintendo (not yet)";
		int mono = 1;

		for (int s = 1; s <= 3; s++)
		{
			int prev = -1;
			for (int k = -2; k <= 2; k++)
			{
				cfg.classicui_tracking = (int8_t)k;
				int got = gfx_text_w(t, s);
				if (prev >= 0 && got <= prev) mono = 0;
				prev = got;
			}
		}
		check(mono, "text measures strictly wider at every step of the dial, at every scale");

		cfg.classicui_tracking = 0;
		int at0 = gfx_text_w(t, 1);
		cfg.classicui_tracking = -1;
		int atm1 = gfx_text_w(t, 1);
		cfg.classicui_tracking = 2;
		int atp2 = gfx_text_w(t, 1);

		// 24 characters: 23 gaps, because the last glyph still rasterises its full cell.
		check(atm1 == at0 - 23 && atp2 == at0 + 46,
			"and by one gap per pair of characters - the last glyph keeps its whole cell");
	}

	/*
	  The invariant. gfx_text_cols(px) must be the largest n whose n-character string still
	  measures no more than px, so a panel widened to hold a measured line cannot then clip
	  that line - and a paragraph wrapped to a column count cannot exceed the pixels the
	  count came from.
	*/
	{
		int bad = 0, checked = 0;
		char run[64];
		for (int i = 0; i < 63; i++) run[i] = 'M';
		run[63] = 0;

		for (int k = -2; k <= 2; k++)
		{
			cfg.classicui_tracking = (int8_t)k;
			for (int s = 1; s <= 3; s++)
			{
				for (int px = 0; px <= 400; px++)
				{
					int n = gfx_text_cols(px, s);
					if (n < 0 || n > 62) continue;
					checked++;

					char buf[64];
					snprintf(buf, sizeof(buf), "%.*s", n, run);
					if (gfx_text_w(buf, s) > px) bad++;

					snprintf(buf, sizeof(buf), "%.*s", n + 1, run);
					if (gfx_text_w(buf, s) <= px) bad++;
				}
			}
		}
		printf("  %d pixel spans checked in both directions\n", checked);
		check(checked > 5000 && !bad,
			"gfx_text_cols is the exact inverse of gfx_text_w at every tracking value and scale");
	}

	/*
	  And gfx_clip(), which is built on that pair and is where the bug actually showed. A
	  string that fits has to come back whole - no defensive '>' on a line the panel was
	  widened for - and one that does not has to come back inside the budget.
	*/
	{
		int bad = 0, floored = 0;
		const char *t = "Super Nintendo (not yet)";

		for (int k = -2; k <= 2; k++)
		{
			cfg.classicui_tracking = (int8_t)k;
			for (int s = 1; s <= 2; s++)
			{
				int need = gfx_text_w(t, s);
				if (strcmp(gfx_clip(t, s, need), t)) bad++;
				if (strcmp(gfx_clip(t, s, need + 100), t)) bad++;

				for (int px = 1; px < need; px++)
				{
					/*
					  Except where there is not room for one character, which gfx_clip has
					  always answered with one character anyway - a single glyph says more
					  about what was there than an empty string does, and no panel in the
					  front-end is that narrow. Counted rather than ignored, so the exception
					  stays an exception.
					*/
					if (gfx_text_cols(px, s) < 1) { floored++; continue; }
					if (gfx_text_w(gfx_clip(t, s, px), s) > px) bad++;
				}
			}
		}
		printf("  %d spans too narrow for one character, floored to one\n", floored);
		check(!bad, "a line that fits is never clipped, and a clipped one always fits");
		check(floored > 0, "and below one character of room it still draws one, as it always did");
	}

	/*
	  And the equivalence the layout refactor rests on: measuring the string and counting the
	  columns decide the same way, for every length and on both sides of every boundary.

	  Every fit test in chome_ui.cpp used to read `strlen(s) > gfx_text_cols(px, scale)` and
	  now reads `gfx_text_w(s, scale) > px`. The first form only has a meaning while one
	  number describes every glyph; the second is a measurement of the string in front of it.
	  This is what says the two agree today - so a wrapped paragraph, a menu-bar word and a
	  slot caption all break in the same place they did before, at every profile.

	  Stronger than the inverse property above rather than a restatement of it. That one
	  checks a run of `n` and `n + 1` characters against the span the count came from; this
	  one checks every length against every span, which is where a caller comparing the wrong
	  way round - a `>=` where a `>` belongs - would show and the inverse check would not.
	*/
	{
		int bad = 0, checked = 0, decided_both_ways = 0;
		char run[64];
		for (int i = 0; i < 63; i++) run[i] = 'M';
		run[63] = 0;

		for (int k = -2; k <= 2; k++)
		{
			cfg.classicui_tracking = (int8_t)k;
			for (int s = 1; s <= 3; s++)
			{
				for (int n = 0; n <= 48; n++)
				{
					char buf[64];
					snprintf(buf, sizeof(buf), "%.*s", n, run);
					int w = gfx_text_w(buf, s);

					for (int px = 0; px <= 400; px++)
					{
						int by_width = (w > px);
						int by_count = (n > gfx_text_cols(px, s));
						checked++;
						if (by_width != by_count) bad++;
						else if (by_width) decided_both_ways++;
					}
				}
			}
		}
		printf("  %d (string, span) pairs decided by width and by column count\n", checked);
		check(checked > 250000 && !bad,
			"measuring a string and counting the columns give the same fit answer, "
			"at every length, scale and tracking value");
		check(decided_both_ways > 100000,
			"and both answers really occur, so the agreement is not one constant matching another");
	}

	cfg.classicui_tracking = 0;

	/* ------------------- a panel sized from its text still holds its text --- */

	/*
	  The disc dialog, which is the one panel in the front-end whose width comes from the
	  longest line it has to draw, and therefore the one that fails if the measuring and the
	  clipping disagree. Its plate is measured off the screen rather than recomputed here -
	  for the same reason assert_disc_dialog_size() measures it - and the title is then run
	  through the very gfx_clip() call draw_disc() makes, against the width the panel really
	  came out.
	*/
	{
		enum { S_DISC = 17 };

		const char *tdb = ROOT "/classicui/disctitles.txt";
		mkpath(ROOT "/classicui");
		{
			FILE *f = fopen(tdb, "wb");
			if (f)
			{
				fprintf(f, "#classicui-disctitles 1\n");
				fprintf(f, "SLES01506\tMetal Gear Solid\n");
				fclose(f);
			}
		}
		disc_titles_forget();

		uint8_t was_disc = cfg.classicui_disc;
		cfg.classicui_disc = 1;

		fake_disc fd; memset(&fd, 0, sizeof(fd));
		static const char *const none[] = { "" };
		fake_iso(&fd, 0, "PLAYSTATION", "PLAYSTATION", none, 0);
		fake_put(&fd, 20, 0, "BOOT = cdrom:\\SLES_015.06;1", 27, 100);

		for (int k = -2; k <= 2; k++)
		{
			char what[192];
			cfg.classicui_tracking = (int8_t)k;

			chome_leave();
			press(KEY_MENU, 20);
			frame(10);

			disc_ingest_present(1);
			disc_set_reader(fake_read, &fd);
			disc_ingest_identify(0);
			frame(6);

			press(KEY_UP);
			press(KEY_ENTER);
			frame(8);

			snprintf(what, sizeof(what), "tracking %+d: the disc dialog is up", k);
			check(chome_screen_id() == S_DISC, what);

			const chome_profile *p = theme_get();
			int ox = 0, oy = 0, ow = 0, oh = 0;
			int got = panel_plate_seen(&ox, &oy, &ow, &oh);

			snprintf(what, sizeof(what), "tracking %+d: there is a plate to measure", k);
			check(got, what);

			if (got)
			{
				// panel_plate_seen() gives the plate inside the two-pixel frame; the panel
				// draw_disc() clips against is four wider, less 12*ts_ui either side.
				int pw = ow + 4;
				int inner = pw - 24 * p->ts_ui;
				const char *title = "Metal Gear Solid";
				const char *drawn = gfx_clip(title, p->ts_title, inner);

				printf("  tracking %+d: panel %d wide, title measures %d in %d of room\n",
					k, pw, gfx_text_w(title, p->ts_title), inner);

				snprintf(what, sizeof(what),
					"tracking %+d: the panel it sized for its title still holds it whole", k);
				check(!strcmp(drawn, title), what);
			}

			disc_reset_reader();
			disc_ingest_present(0);
			(void)disc_take_dirty();
			press(KEY_ESC, 10);
			frame(6);
		}

		unlink(tdb);
		disc_titles_forget();
		cfg.classicui_disc = was_disc;
		cfg.classicui_tracking = 0;
	}

	/* ---------------------------- 240p at +2, where the copy runs out of room --- */

	/*
	  The canvas the front-end's wording was fitted to, at the value that costs it the most.

	  A full-width 240p row holds 35 characters at zero tracking and 28 at +2, and a dozen
	  lines in the front-end were written to within a character or two of 35 - see the
	  suspend header's fallback, and the note on the Letter Spacing row. Nothing here can
	  make that copy shorter. What it can check is that the shortfall is *graceful*: the
	  panel is still sized in characters, so it grows with the advance, and the text is still
	  wrapped and clipped against that panel, so nothing spills onto the shelf behind it.

	  Read as pixels of the body text's own colour outside the plate, which is what a line
	  wrapped to a column count that no longer matched the pixels would leave there.
	*/
	{
		enum { S_POWER = 12 };

		int was_prof = cfg.classicui_profile;
		int was_w = gfx_w(), was_h = gfx_h();

		for (int k = 0; k <= 2; k++)
		{
			char what[192];
			cfg.classicui_tracking = (int8_t)k;

			cfg.classicui_profile = 3;
			harness_set_fb(320, 240);
			gfx_shutdown();
			theme_invalidate();
			theme_update(320, 240, 3);

			chome_leave();
			press(KEY_MENU, 20);
			frame(12);
			press(KEY_UP, 10);                // the menu bar; Display is dropped at 240p
			press(KEY_RIGHT, 10);             // ...so Options is first and Power is next
			press(KEY_ENTER, 14);
			frame(8);

			snprintf(what, sizeof(what), "240p tracking +%d: the Power panel is up", k);
			check(chome_screen_id() == S_POWER, what);

			const chome_profile *p = theme_get();
			int ox = 0, oy = 0, ow = 0, oh = 0;
			int got = panel_plate_seen(&ox, &oy, &ow, &oh);

			snprintf(what, sizeof(what), "240p tracking +%d: with a plate to measure", k);
			check(got, what);
			if (!got) continue;

			int spill = box_pixels(0, oy, ox, oy + oh, COL_PANELHI)
				+ box_pixels(ox + ow, oy, p->w, oy + oh, COL_PANELHI);

			printf("  240p tracking +%d: plate %d wide holds %d characters, %d body pixels "
				"outside it\n", k, ow + 4, gfx_text_cols(ow + 4 - 16 * p->ts_ui, p->ts_ui), spill);

			snprintf(what, sizeof(what),
				"240p tracking +%d: the wrapped note stays inside its panel", k);
			check(spill == 0, what);

			press(KEY_ESC, 10);
			press(KEY_ESC, 10);
			frame(6);
		}

		cfg.classicui_tracking = 0;
		cfg.classicui_profile = (uint8_t)was_prof;
		harness_set_fb(was_w, was_h);
		gfx_shutdown();
		theme_invalidate();
		theme_update(was_w, was_h, cfg.classicui_profile);
		frame(6);
	}

	/* ------------------------------------------------------- the capitals --- */

	/*
	  Read off the About panel, whose header draw_panel_at() draws at a position this file
	  can name: 6*ts_ui in from the plate's left edge and four rows down from the top of the
	  title bar. So the second character of "About" is one advance further along, and the
	  question is whether the glyph there is 'b' or 'B'.

	  That is the whole claim - not "the panel changed", which a switch that blanked the
	  screen would also satisfy.
	*/
	{
		enum { S_ABOUT = 6 };

		harness_set_menu_core(1);
		chome_leave();
		press(KEY_MENU, 20);
		frame(8);
		press(KEY_UP, 10);                    // the menu bar, on Display
		press(KEY_RIGHT, 10);                 // Options
		press(KEY_ENTER, 14);
		// About is the last row of Options now rather than the fourth entry on the bar,
		// so it is reached by wrapping upwards off the first row.
		press(KEY_UP, 10);
		press(KEY_ENTER, 14);
		frame(8);
		check(chome_screen_id() == S_ABOUT, "the About panel is up, which has a header to read");

		int x0, y0, x1, y1;
		panel_rect(&x0, &y0, &x1, &y1);
		const chome_profile *p = theme_get();
		int s = p->ts_ui;

		int gx = x0 + 6 * s + gfx_adv(s);      // the second character of the header
		int gy = y0 + 4;

		/*
		  Closed and reopened rather than repainted in place, and it is worth saying why:
		  the compositor only recomposes when something has told it the screen is stale, and
		  poking a cfg field from outside the front-end tells it nothing. Every other section
		  here that changes cfg under the menu does the same thing - see the overscan walk.
		*/
		cfg.classicui_caps = 1;
		press(KEY_ESC, 10);
		press(KEY_ENTER, 14);
		frame(8);
		unsigned long on_hash = panel_hash();
		int saw_upper = glyph_seen(gx, gy, s, 'B', COL_PANELHI);

		cfg.classicui_caps = 0;
		press(KEY_ESC, 10);
		press(KEY_ENTER, 14);
		frame(8);
		dump("caps-off-panel");
		unsigned long off_hash = panel_hash();
		int saw_lower = glyph_seen(gx, gy, s, 'b', COL_PANELHI);

		check(saw_upper, "with capitals on, a panel header is drawn in capitals");
		check(saw_lower, "and with them off, the same header is drawn as it is written");
		check(on_hash != off_hash, "which is a visible difference, not a silent one");

		cfg.classicui_caps = 1;
		press(KEY_ESC, 10);
		press(KEY_ENTER, 14);
		frame(8);
		check(panel_hash() == on_hash,
			"and putting the switch back reproduces the frame bit for bit");

		// Two now rather than one: About backs out to Options, and Options to the bar.
		press(KEY_ESC, 10);
		press(KEY_ESC, 10);
		frame(6);
	}

	/* ------------------------------------------------------------ the font --- */

	{
		static unsigned char before[256][8];
		memcpy(before, charfont, sizeof(before));

		mkpath(ROOT "/" "font");
		make_pf(ROOT "/font/harness.pf");

		char rel[64];
		snprintf(rel, sizeof(rel), "font/harness.pf");
		check(LoadFont(rel) == 1, "a generated .pf on the card loads");

		check(memcmp(before, charfont, sizeof(before)) != 0, "and replaces the glyph table");

		// The transpose, checked on one character: six solid columns and two clear, which is
		// what a row-major 0xFC becomes. A load that got the axes the wrong way round would
		// give six solid rows instead and pass any "it changed" test.
		int shaped = 1;
		for (int c = 33; c < 127; c++)
		{
			for (int col = 0; col < 6; col++) if (charfont[c][col] != 0xFF) shaped = 0;
			for (int col = 6; col < 8; col++) if (charfont[c][col] != 0x00) shaped = 0;
		}
		check(shaped, "and transposes it: byte n is column n, as draw_glyph reads it");

		/*
		  A missing file. LoadFont() returns early without touching the table, which is what
		  lets the font row say "still on the old one" instead of drawing a screen of blanks -
		  and is why it has a return value at all.
		*/
		static unsigned char loaded[256][8];
		memcpy(loaded, charfont, sizeof(loaded));

		char gone[64];
		snprintf(gone, sizeof(gone), "font/not-here.pf");
		check(LoadFont(gone) == 0, "a font that is not there refuses");
		check(!memcmp(loaded, charfont, sizeof(loaded)),
			"and leaves the one already loaded exactly as it was");

		// And the way back, which is the reason charrom.cpp keeps 2KB aside.
		FontRestoreBuiltin();
		check(!memcmp(before, charfont, sizeof(before)),
			"Built-in restores all 2048 bytes of the compiled-in font, byte for byte");

		/*
		  Then the same thing through the screen: the Font row writes `font=` into MiSTer.ini
		  and tells cfg, because cfg is what the next LoadFont at boot reads. Driven with keys
		  rather than by calling the writer, so the row, the save and the file are one test.
		*/
		char path[1024], bak[1024];
		snprintf(path, sizeof(path), "%s/MiSTer.ini", ROOT);
		snprintf(bak, sizeof(bak), "%s.bak", path);
		put_file(path, "[MiSTer]\r\ndisable_autofire=1\r\ncontroller_info=0\r\n");
		cfg.font[0] = 0;

		chome_leave();
		press(KEY_MENU, 20);
		frame(8);
		press(KEY_UP, 10);                    // the menu bar
		press(KEY_RIGHT, 10);                 // Options
		press(KEY_ENTER, 14);
		press(KEY_UP, 8);                     // wrap to the last row, which is About now
		press(KEY_UP, 8);                     // Advanced Settings
		press(KEY_UP, 8);                     // More Settings
		press(KEY_ENTER, 14);
		frame(8);

		// Up once from the first row is Save Changes; up again is the Font row above it.
		press(KEY_UP, 8);
		press(KEY_UP, 8);
		frame(6);

		press(KEY_RIGHT, 10);                 // Built-in -> the only .pf on the card
		frame(8);
		dump("font-row-picked");
		check(memcmp(before, charfont, sizeof(before)) != 0,
			"stepping the Font row loads the font at once, without waiting for a save");

		static char now[8192];
		check(slurp_file(path, now, sizeof(now)) > 0 && !strstr(now, "font"),
			"and writes nothing on its own");

		press(KEY_DOWN, 8);                   // down to Save Changes
		press(KEY_ENTER, 12);                 // arms
		press(KEY_ENTER, 12);                 // writes
		frame(8);

		check(slurp_file(path, now, sizeof(now)) > 0
			&& strstr(now, "font=font/harness.pf") != 0,
			"saving puts font= in MiSTer.ini");
		check(!strcmp(cfg.font, "font/harness.pf"),
			"and tells the running firmware, which is what the next boot reads back");

		int bare_lf = 0;
		for (int i = 0; now[i]; i++) if (now[i] == '\n' && (!i || now[i - 1] != '\r')) bare_lf++;
		check(!bare_lf, "in CRLF, through the same writer every other setting goes through");

		/*
		  And back to Built-in through the row, which writes the key empty - exactly what
		  user_io.cpp tests for when it decides whether to load a font at all.
		*/
		press(KEY_UP, 8);                     // back up to the Font row
		press(KEY_LEFT, 10);
		frame(8);
		check(!memcmp(before, charfont, sizeof(before)),
			"stepping back to Built-in puts the compiled-in glyphs back on screen");

		press(KEY_DOWN, 8);
		press(KEY_ENTER, 12);
		press(KEY_ENTER, 12);
		frame(8);
		check(slurp_file(path, now, sizeof(now)) > 0 && strstr(now, "font=\r\n") != 0,
			"and saves it as an empty font=, which is how the firmware spells no font");
		check(!cfg.font[0], "with cfg agreeing");

		/*
		  And the way off this screen that is not B. The menu button goes straight to the menu
		  bar from wherever it is pressed, walking past the "again to lose the changes" prompt
		  - so a font chosen and not saved has to be put back on that path too, or it would
		  stay on screen until the next reboot with nothing on any screen saying why.
		*/
		press(KEY_UP, 8);                     // the Font row again
		press(KEY_RIGHT, 10);
		frame(8);
		check(memcmp(before, charfont, sizeof(before)) != 0, "a font is staged again");

		press(KEY_MENU, 14);
		frame(8);
		check(!memcmp(before, charfont, sizeof(before)),
			"and the menu button off the screen puts it back, not only B");

		press(KEY_ESC, 10);
		press(KEY_ESC, 10);
		press(KEY_ESC, 10);

		unlink(ROOT "/font/harness.pf");
		unlink(path);
		unlink(bak);
		cfg.font[0] = 0;

		// Whatever the presses above left staged, the table is the built-in one from here on.
		FontRestoreBuiltin();
		check(!memcmp(before, charfont, sizeof(before)),
			"and the section leaves the front-end on the font every frame above was drawn in");
	}

	cfg.classicui_tracking = was_track;
	cfg.classicui_caps = was_caps;
	theme_invalidate();
	theme_update(gfx_w(), gfx_h(), cfg.classicui_profile);
	gfx_damage_all();
	frame(6);
}

/* ------------------------------------------------------------------ main -- */

/* ================================================== ripping a disc to the card ===== */

/*
  A disc that does not exist, driven through the ripper, and then the bytes it wrote.

  This is the whole of what a host test can say about a rip and it is more than it sounds:
  everything about the *format* - which files, how long, what the sheet says, where the
  pregap went - is decided by code that never touches a drive, so all of it is checkable
  here. What is not checkable here is the drive: the helper process, the speed cap, the
  re-attach after a USB reset and every timing question are hardware and are called out as
  unproven in the guide rather than faked into a green tick.

  The fixture is three tracks chosen so that each of them is a case that has its own bug:

    01  data, and MODE2 on the surface while the table of contents says MODE1 - which is
        what every PlayStation disc looks like, and is the case rip_plan_build() reads a
        sector to settle. A plan that trusted the TOC would write MODE1/2352 here.
    02  audio with a 150-sector pregap, so INDEX 00 and INDEX 01 are both exercised and
        the pregap is inside the track's own file where the multi-FILE readers expect it.
    03  audio with no pregap, so the INDEX 00 line has to be *absent* rather than zero -
        a sheet that emitted it unconditionally would claim a pregap on every track.
*/

#define RF_T1_END  1000          // track 1: LBA 0..999, data
#define RF_T2_IX0  1000          // track 2: INDEX 00 here...
#define RF_T2_PRE  150           // ...INDEX 01 150 sectors later, audio
#define RF_T2_END  1600
#define RF_T3_END  2000          // track 3: audio, no pregap. Leadout at 2000.

struct rip_fake
{
	int fail_lba;                // a sector that will never read, or -1
	int cancel_after;            // cancel once this many sectors have been asked for, or -1
	int reads;
};

// A sector whose every byte is derived from its LBA, so a file can be checked rather than
// just weighed. Data tracks carry a real sync pattern and a mode byte, because that is what
// rip_plan_build() reads the mode out of.
static void rip_fake_sector(int lba, uint8_t *dst)
{
	memset(dst, 0, PHYSICAL_DISC_RAW);

	if (lba < RF_T1_END)
	{
		dst[0] = 0x00;
		for (int i = 1; i <= 10; i++) dst[i] = 0xFF;
		dst[11] = 0x00;
		dst[12] = (uint8_t)(lba / 4500);
		dst[13] = (uint8_t)((lba / 75) % 60);
		dst[14] = (uint8_t)(lba % 75);
		dst[15] = 0x02;                              // MODE2, which the TOC cannot say
	}

	for (int i = 16; i < PHYSICAL_DISC_RAW; i++)
		dst[i] = (uint8_t)((lba * 7 + i * 3) & 0xff);
}

static int rip_fake_read(int lba, uint8_t *dst, void *ctx)
{
	rip_fake *f = (rip_fake*)ctx;
	f->reads++;

	if (lba < 0 || lba >= RF_T3_END) return -1;
	if (f->fail_lba >= 0 && lba == f->fail_lba) return -1;

	rip_fake_sector(lba, dst);
	return 0;
}

static int rip_fake_cancelled(void *ctx)
{
	rip_fake *f = (rip_fake*)ctx;
	return (f->cancel_after >= 0 && f->reads >= f->cancel_after);
}

static void rip_fake_toc(toc_t *toc)
{
	memset(toc, 0, sizeof(*toc));

	toc->last = 3;
	toc->end = RF_T3_END;
	toc->sectorSize = PHYSICAL_DISC_RAW;
	toc->phys = 1;

	for (int i = 0; i < 3; i++)
	{
		toc->tracks[i].sector_size = PHYSICAL_DISC_RAW;
		toc->tracks[i].index_num = 2;
	}

	// Exactly what physical_disc_load_toc() produces - every data track TT_MODE1, because a
	// TOC entry has one data bit and no mode - followed by what
	// physical_disc_psx_enrich_toc() does to track 2 when the drive can report its pregap.
	toc->tracks[0].start = 0;             toc->tracks[0].end = RF_T1_END;  toc->tracks[0].type = TT_MODE1;
	toc->tracks[1].start = RF_T2_IX0;     toc->tracks[1].end = RF_T2_END;  toc->tracks[1].type = TT_CDDA;
	toc->tracks[1].indexes[1] = RF_T2_PRE;
	toc->tracks[2].start = RF_T2_END;     toc->tracks[2].end = RF_T3_END;  toc->tracks[2].type = TT_CDDA;
}

static long long file_bytes(const char *path)
{
	struct stat st;
	if (stat(path, &st)) return -1;
	return (long long)st.st_size;
}

static int dir_is_there(const char *path)
{
	struct stat st;
	return !stat(path, &st) && S_ISDIR(st.st_mode);
}

// The whole of a small file, as text. Returns 0 when it is not there.
static int slurp(const char *path, char *out, int outsz)
{
	out[0] = 0;
	FILE *f = fopen(path, "rb");
	if (!f) return 0;
	size_t n = fread(out, 1, (size_t)outsz - 1, f);
	fclose(f);
	out[n] = 0;
	return (int)n;
}

// One sector out of a track file, so the bytes can be compared against what the reader
// handed over rather than merely counted.
static int track_sector(const char *path, int index, uint8_t *dst)
{
	FILE *f = fopen(path, "rb");
	if (!f) return 0;
	if (fseek(f, (long)index * PHYSICAL_DISC_RAW, SEEK_SET)) { fclose(f); return 0; }
	size_t n = fread(dst, 1, PHYSICAL_DISC_RAW, f);
	fclose(f);
	return n == PHYSICAL_DISC_RAW;
}

static void assert_rip_format()
{
	printf("\n== ripping a disc: the plan, the sheet and the bytes ==\n");

	const char *psx = ROOT "/games/PSX";

	/* ------------------------------------------------------------- the arithmetic --- */

	{
		int m, s, f;
		rip_msf(0, &m, &s, &f);
		check(!m && !s && !f, "no pregap is 00:00:00");
		rip_msf(150, &m, &s, &f);
		check(!m && s == 2 && !f, "the standard 150-sector pregap is 00:02:00");
		rip_msf(75 * 60 + 1, &m, &s, &f);
		check(m == 1 && !s && f == 1, "and a minute and a frame carries into the minutes");
	}

	{
		char n[96];
		check(rip_folder_name("Metal Gear Solid", n, sizeof(n)) && !strcmp(n, "Metal Gear Solid"),
			"a title that is already a legal folder name is left alone");
		check(rip_folder_name("Wing Commander III: Heart", n, sizeof(n)) &&
			!strcmp(n, "Wing Commander III_ Heart"),
			"and a colon becomes one underscore, not one per character");
		check(rip_folder_name("Tomb Raider (Europe)", n, sizeof(n)) &&
			!strcmp(n, "Tomb Raider (Europe)"),
			"brackets survive, because every romset on the card has them");
		check(!rip_folder_name("///", n, sizeof(n)),
			"and a title with nothing legal in it is refused rather than made into a dot");
		check(!rip_folder_name("", n, sizeof(n)), "as is an empty one");
	}

	/*
	  Free space, decided separately from asking the filesystem so it can be tested without
	  one of a chosen size. The margin is the point: a rip that exactly fits must refuse,
	  because a card with no room left cannot save a state or write an index afterwards.
	*/
	{
		long long mb = 1024 * 1024;
		check(rip_space_ok(100 * mb, 400 * mb), "a rip with room to spare goes ahead");
		check(!rip_space_ok(100 * mb, 100 * mb), "one that exactly fits does not");
		check(!rip_space_ok(100 * mb, (100 + RIP_SPARE_MB - 1) * mb),
			"nor one that would leave less than the spare behind");
		check(rip_space_ok(100 * mb, (100 + RIP_SPARE_MB) * mb), "and one that would leave exactly it does");
		check(rip_space_ok(700 * mb, 0),
			"a filesystem statvfs could not read is allowed through rather than refusing every rip");
	}

	/* ------------------------------------------------------------------- the plan --- */

	toc_t toc;
	rip_fake fk = { -1, -1, 0 };
	rip_plan plan;

	rip_fake_toc(&toc);
	check(rip_plan_build(&toc, &plan, rip_fake_read, &fk) == 3, "three tracks are planned");

	check(plan.sectors == RF_T3_END, "and the sector count is the whole disc to the leadout");
	check(plan.t[0].num == 1 && plan.t[0].start == 0 && plan.t[0].sectors == RF_T1_END,
		"track 1 runs from the first sector to where track 2 begins");
	check(plan.t[1].start == RF_T2_IX0 && plan.t[1].sectors == RF_T2_END - RF_T2_IX0,
		"track 2 starts at its INDEX 00, so its pregap is inside its own file");
	check(plan.t[1].pregap == RF_T2_PRE, "and the pregap's length came through");
	check(plan.t[2].pregap == 0, "track 3 has none");
	check(plan.t[2].sectors == RF_T3_END - RF_T2_END,
		"and the last track runs to the leadout, not to one short of it");

	/*
	  The measurement that the table of contents cannot make. This is the check that fails if
	  rip_plan_build() ever stops reading a sector and starts believing the TOC's TT_MODE1.
	*/
	check(plan.t[0].type == TT_MODE2,
		"the data track is MODE2 because a sector says so, though the TOC said MODE1");
	check(plan.t[1].type == TT_CDDA && plan.t[2].type == TT_CDDA,
		"and the audio tracks are audio, with no sector read to decide it");

	check(rip_bytes_needed(&plan) > (long long)RF_T3_END * PHYSICAL_DISC_RAW,
		"the space asked for covers the sectors and then some, for the clusters they round up to");

	/* ------------------------------------------------------------------ the sheet --- */

	/*
	  Byte for byte, because this is the deliverable. Every one of the parsers in this tree
	  compares uppercase keywords with memcmp and skips indent with `while (*lptr == 0x20)`,
	  so the capitals and the spaces in here are load-bearing and a test that matched loosely
	  would pass a tab-indented sheet that no core can read.
	*/
	static const char cue_true[] =
		"FILE \"Track 01.bin\" BINARY\n"
		"  TRACK 01 MODE2/2352\n"
		"    INDEX 01 00:00:00\n"
		"FILE \"Track 02.bin\" BINARY\n"
		"  TRACK 02 AUDIO\n"
		"    INDEX 00 00:00:00\n"
		"    INDEX 01 00:02:00\n"
		"FILE \"Track 03.bin\" BINARY\n"
		"  TRACK 03 AUDIO\n"
		"    INDEX 01 00:00:00\n";

	char cue[4096];
	check(rip_cue_text(&plan, 0, cue, sizeof(cue)) == (int)strlen(cue_true) &&
		!strcmp(cue, cue_true), "the sheet is exactly what a PlayStation rip should say");

	/*
	  And the same disc for a core whose parser knows no MODE2 token - Mega CD, Neo Geo CD and
	  PC Engine CD. megacdd.cpp only looks for MODE1/2048 and MODE1/2352 and only on track 1,
	  and a MODE2 token there leaves its sector size unset and drops it into a byte sniff. So
	  the one line changes and nothing else does.
	*/
	static const char cue_m1[] =
		"FILE \"Track 01.bin\" BINARY\n"
		"  TRACK 01 MODE1/2352\n"
		"    INDEX 01 00:00:00\n"
		"FILE \"Track 02.bin\" BINARY\n"
		"  TRACK 02 AUDIO\n"
		"    INDEX 00 00:00:00\n"
		"    INDEX 01 00:02:00\n"
		"FILE \"Track 03.bin\" BINARY\n"
		"  TRACK 03 AUDIO\n"
		"    INDEX 01 00:00:00\n";

	check(rip_cue_text(&plan, 1, cue, sizeof(cue)) && !strcmp(cue, cue_m1),
		"and for a parser with no MODE2 token the data track is written MODE1/2352");

	/*
	  Saturn is the other kind, and it is worth pinning because the guess goes the wrong way.
	  Its neighbours in the table are the three Sega/NEC/SNK parsers that read MODE1 only, so
	  copying the Mega CD row would have looked reasonable and written MODE1/2352 over a
	  MODE2 track. saturncdd.cpp:169-192 reads MODE1/2048, MODE1/2352 AND MODE2/2352, per
	  track, keeping each track's sector size - so it takes the measured mode, exactly like
	  psx.cpp, and the sheet a Saturn rip writes is the true one.

	  Asserted against cue_true rather than against a Saturn-shaped copy of it: if these two
	  ever stop being the same string, the table's saturn row is wrong.
	*/
	check(rip_cue_text(&plan, 0, cue, sizeof(cue)) && !strcmp(cue, cue_true),
		"a Saturn sheet keeps the measured MODE2/2352, which is what saturncdd.cpp reads");

	check(!rip_cue_text(&plan, 0, cue, 40),
		"a sheet that will not fit its buffer is refused, not truncated into an unparseable one");

	/* ------------------------------------------------------------------ the bytes --- */

	const char *name = "Ripped Test Disc";
	char dir[1024], stage[1024], path[1024];
	snprintf(dir, sizeof(dir), "%s/%s", psx, name);
	rip_stage_path(psx, name, stage, sizeof(stage));

	rip_rmdir_flat(dir);
	rip_rmdir_flat(stage);

	rip_io io;
	memset(&io, 0, sizeof(io));
	io.read = rip_fake_read;
	io.read_ctx = &fk;
	io.cancelled = rip_fake_cancelled;
	io.cancel_ctx = &fk;

	int bad = -1;
	fk.reads = 0;
	check(rip_perform(&plan, psx, name, 0, 0, &io, &bad) == RIP_DONE, "the rip finishes");
	check(bad == 0, "with nothing unreadable on a disc with nothing wrong with it");
	check(!dir_is_there(stage), "and the staging folder it was built in is gone");
	check(dir_is_there(dir), "the folder is at the name the shelf will look for");

	snprintf(path, sizeof(path), "%s/%s.cue", dir, name);
	{
		char got[4096];
		check(slurp(path, got, sizeof(got)) && !strcmp(got, cue_true),
			"the sheet on the card is the sheet that was promised, byte for byte");
	}

	// Sizes: sectors times 2352 and not a byte more, which is what a reader deriving track
	// lengths from file sizes depends on.
	snprintf(path, sizeof(path), "%s/Track 01.bin", dir);
	check(file_bytes(path) == (long long)RF_T1_END * PHYSICAL_DISC_RAW,
		"track 1 is its sector count times 2352");

	{
		// ...and the bytes are the disc's. The first sector of the file must be the first
		// sector of the track, which is where an off-by-one in the start LBA would show.
		uint8_t got[PHYSICAL_DISC_RAW], want[PHYSICAL_DISC_RAW];
		check(track_sector(path, 0, got), "track 1's first sector can be read back");
		rip_fake_sector(0, want);
		check(!memcmp(got, want, PHYSICAL_DISC_RAW), "and it is LBA 0's 2352 bytes");

		check(track_sector(path, RF_T1_END - 1, got), "and its last one");
		rip_fake_sector(RF_T1_END - 1, want);
		check(!memcmp(got, want, PHYSICAL_DISC_RAW),
			"which is the sector before track 2, so no sector was dropped at the seam");
	}

	snprintf(path, sizeof(path), "%s/Track 02.bin", dir);
	check(file_bytes(path) == (long long)(RF_T2_END - RF_T2_IX0) * PHYSICAL_DISC_RAW,
		"track 2 is as long as its pregap plus its audio");
	{
		uint8_t got[PHYSICAL_DISC_RAW], want[PHYSICAL_DISC_RAW];
		check(track_sector(path, 0, got), "track 2's first sector can be read back");
		rip_fake_sector(RF_T2_IX0, want);
		check(!memcmp(got, want, PHYSICAL_DISC_RAW),
			"and it is INDEX 00 - the pregap really is at the head of the file the sheet says it is");

		check(track_sector(path, RF_T2_PRE, got), "the sector at the pregap's length can be read");
		rip_fake_sector(RF_T2_IX0 + RF_T2_PRE, want);
		check(!memcmp(got, want, PHYSICAL_DISC_RAW),
			"and it is INDEX 01, which is where INDEX 01 00:02:00 points");
	}

	snprintf(path, sizeof(path), "%s/Track 03.bin", dir);
	check(file_bytes(path) == (long long)(RF_T3_END - RF_T2_END) * PHYSICAL_DISC_RAW,
		"and track 3 runs to the leadout");

	snprintf(path, sizeof(path), "%s/%s", dir, RIP_BADFILE);
	check(file_bytes(path) < 0, "a clean rip leaves no list of unreadable sectors");

	/* ------------------------------------ the folder the scanner makes of it --- */

	/*
	  The interaction commit 73b0f71 exists for, end to end rather than assumed: a folder
	  holding a .cue and its Track NN.bin parts is ONE game named after the folder. This is
	  the reason the rip is laid out the way it is, so it is checked against the scanner
	  rather than against the commit message.
	*/
	lib_rescan();
	for (int i = 0; i < 400 && lib_scanning(); i++) frame(2);
	frame(10);

	{
		char rel[256];
		snprintf(rel, sizeof(rel), "%s/%s.cue", name, name);
		int it = item_at("psx", rel);
		check(it >= 0, "the finished rip is on the shelf");
		check(it >= 0 && !strcmp(lib_item(it)->title, name),
			"under the disc's own name, which is what the folder was called after");

		snprintf(rel, sizeof(rel), "%s/Track 01.bin", name);
		check(item_at("psx", rel) < 0, "and its tracks are not games of their own");

		int n = 0;
		for (int i = 0; i < lib_item_count(); i++)
		{
			chome_item *item = lib_item(i);
			if (item && item->kind == IT_GAME && !strncmp(item->path, name, strlen(name))) n++;
		}
		check(n == 1, "so the whole rip is exactly one card");
	}

	/* --------------------------------------------------------- a scratched disc --- */

	/*
	  The read-error policy, which is: retry, then write 2352 zero bytes, count it, list it
	  and tell the player. Not abort - one bad frame in an audio track is a click and
	  throwing away a finished 700 MB copy over it is the worse answer - and above all not
	  *skip*, because a short track file puts every sector after the hole at the wrong offset
	  and desynchronises the rest of the disc.
	*/
	const char *bname = "Scratched Test Disc";
	char bdir[1024];
	snprintf(bdir, sizeof(bdir), "%s/%s", psx, bname);
	rip_rmdir_flat(bdir);

	fk.fail_lba = RF_T2_IX0 + RF_T2_PRE + 10;      // inside track 2's audio
	fk.reads = 0;
	bad = -1;
	check(rip_perform(&plan, psx, bname, 0, 0, &io, &bad) == RIP_DONE,
		"a disc with an unreadable sector still finishes");
	check(bad == 1, "and says how many sectors it could not read");

	snprintf(path, sizeof(path), "%s/Track 02.bin", bdir);
	check(file_bytes(path) == (long long)(RF_T2_END - RF_T2_IX0) * PHYSICAL_DISC_RAW,
		"the track is still exactly the length the sheet says, so nothing after the hole moved");
	{
		uint8_t got[PHYSICAL_DISC_RAW], zero[PHYSICAL_DISC_RAW];
		memset(zero, 0, sizeof(zero));
		check(track_sector(path, fk.fail_lba - RF_T2_IX0, got), "the bad sector is readable back");
		check(!memcmp(got, zero, PHYSICAL_DISC_RAW), "and it is 2352 zero bytes, not a gap");

		uint8_t want[PHYSICAL_DISC_RAW];
		check(track_sector(path, fk.fail_lba - RF_T2_IX0 + 1, got), "and the one after it");
		rip_fake_sector(fk.fail_lba + 1, want);
		check(!memcmp(got, want, PHYSICAL_DISC_RAW),
			"is still its own sector, which is what zero-filling instead of skipping buys");
	}

	snprintf(path, sizeof(path), "%s/%s", bdir, RIP_BADFILE);
	{
		char got[2048], lba[32];
		snprintf(lba, sizeof(lba), "%d", fk.fail_lba);
		check(slurp(path, got, sizeof(got)) > 0, "the folder carries a list of what did not read");
		check(strstr(got, lba) != 0, "naming the sector by its LBA");
		check(strstr(got, "audio") != 0, "and which kind of track it was in");
	}

	rip_rmdir_flat(bdir);
	fk.fail_lba = -1;

	/* --------------------------------------------------------------- cancelling --- */

	/*
	  A cancelled rip leaves nothing at all, and by construction rather than by cleanup: the
	  copy is assembled in a folder whose name begins with a dot, which scan_dir() skips, and
	  it is only renamed to the finished name once every byte is written. So there is no
	  instant at which a half-written folder looks like a game.
	*/
	const char *cname = "Cancelled Test Disc";
	char cdir[1024], cstage[1024];
	snprintf(cdir, sizeof(cdir), "%s/%s", psx, cname);
	rip_stage_path(psx, cname, cstage, sizeof(cstage));
	rip_rmdir_flat(cdir);
	rip_rmdir_flat(cstage);

	fk.cancel_after = 100;
	fk.reads = 0;
	bad = -1;
	check(rip_perform(&plan, psx, cname, 0, 0, &io, &bad) == RIP_CANCELLED,
		"a rip that is asked to stop, stops");
	check(fk.reads < RF_T3_END, "well before the end of the disc");
	check(!dir_is_there(cdir), "and there is no folder at the name a game would have");
	check(!dir_is_there(cstage), "nor the hidden one it was being built in");

	fk.cancel_after = -1;

	/* ------------------------------------------------- never over the top, silently --- */

	/*
	  An existing folder is refused outright, and the confirmation for replacing it is the
	  front-end's - see the two presses in assert_rip_screen(). What is checked here is the
	  guard behind that, and the property that makes the confirmation safe to give: with
	  `overwrite` set the old folder is not touched until the new copy is finished, so a
	  confirmed replacement that is then cancelled costs nothing.
	*/
	const char *ename = "Existing Test Disc";
	char edir[1024], estage[1024], keep[1024];
	snprintf(edir, sizeof(edir), "%s/%s", psx, ename);
	rip_stage_path(psx, ename, estage, sizeof(estage));
	rip_rmdir_flat(edir);
	rip_rmdir_flat(estage);

	mkpath(edir);
	touch(edir, "already-here.txt", 11);
	snprintf(keep, sizeof(keep), "%s/already-here.txt", edir);

	fk.reads = 0;
	check(rip_perform(&plan, psx, ename, 0, 0, &io, &bad) == RIP_EXISTS,
		"a rip onto a folder that is already there refuses");
	check(fk.reads == 0, "without having read a single sector off the disc");
	check(file_bytes(keep) == 11, "and what was there is untouched");
	check(!dir_is_there(estage), "with no staging folder left behind either");

	fk.cancel_after = 100;
	fk.reads = 0;
	check(rip_perform(&plan, psx, ename, 0, 1, &io, &bad) == RIP_CANCELLED,
		"a confirmed replacement can still be cancelled");
	check(file_bytes(keep) == 11,
		"and the folder it was going to replace is still exactly as it was");
	check(!dir_is_there(estage), "and nothing hidden is left over");

	fk.cancel_after = -1;
	fk.reads = 0;
	check(rip_perform(&plan, psx, ename, 0, 1, &io, &bad) == RIP_DONE,
		"and a confirmed replacement that finishes, replaces");
	check(file_bytes(keep) < 0, "the old contents are gone");
	snprintf(path, sizeof(path), "%s/%s.cue", edir, ename);
	check(file_bytes(path) > 0, "and the new sheet is in its place");

	/* ------------------------------------------- disc 2 of a game is not a replacement --- */

	/*
	  The bug this section exists for, in the shape it was reported in.

	  The owner ripped disc 1 of the PAL Metal Gear Solid, put disc 2 in the drive, and was
	  offered the chance to REPLACE what was on the card. Both discs are titled "Metal Gear
	  Solid" in the shipped table - SLES-01506 and SLES-11506 return the byte-identical
	  string - so a folder named from the title alone collided, and saying yes would have
	  destroyed disc 1.

	  What tells them apart is the serial, and the naming is checked first because every
	  other property here rests on it.
	*/
	{
		char reg[16];
		check(rip_region_of("SLES-01506", reg, sizeof(reg)) && !strcmp(reg, "Europe"),
			"an SLES serial is a European disc");
		check(rip_region_of("SLUS-00594", reg, sizeof(reg)) && !strcmp(reg, "USA"),
			"an SLUS one is American");
		check(rip_region_of("SLPS-00123", reg, sizeof(reg)) && !strcmp(reg, "Japan"),
			"and an SLPS one Japanese");

		// A prefix whose territory is not settled says nothing rather than guessing, and a
		// volume label is not a serial at all.
		check(!rip_region_of("PCPX-96001", reg, sizeof(reg)),
			"a promo prefix this cannot place is left unplaced");
		check(!rip_region_of("PLAYSTATION", reg, sizeof(reg)),
			"and a volume label is not a serial");

		char b1[160], b2[160], f1[96];
		check(rip_disc_base("Metal Gear Solid", "SLES-01506", b1, sizeof(b1))
			&& !strcmp(b1, "Metal Gear Solid (Europe) (SLES-01506)"),
			"disc 1 is named by its serial");
		check(rip_disc_base("Metal Gear Solid", "SLES-11506", b2, sizeof(b2))
			&& !strcmp(b2, "Metal Gear Solid (Europe) (SLES-11506)"),
			"and disc 2 by its own, which is what makes them two things");
		check(strcmp(b1, b2), "so the two discs of one game do not share a name");

		check(rip_game_folder("Metal Gear Solid", "SLES-11506", f1, sizeof(f1))
			&& !strcmp(f1, "Metal Gear Solid (Europe)"),
			"while the folder is the game, and the same for both");

		// No disc number anywhere in it: nothing at rip time knows which disc this is, and
		// the header says at length why a guess would be worse than the serial.
		check(!strstr(b2, "Disc") && !strstr(b2, "disc"),
			"and nothing in the name claims to know which disc it is");

		// A disc with no serial keeps exactly the single-disc shape rips had before any of
		// this - PC Engine and Neo Geo discs carry no serial to tell copies apart with.
		char nb[160];
		check(rip_disc_base("Sonic CD", "", nb, sizeof(nb)) && !strcmp(nb, "Sonic CD"),
			"a disc with no serial is still named by its title alone");
	}

	{
		const char *mgs_t = "Metal Gear Solid";
		const char *d1 = "SLES-01506", *d2 = "SLES-11506";

		char gdir[1024], b1[160], b2[160], fold[96];
		rip_disc_base(mgs_t, d1, b1, sizeof(b1));
		rip_disc_base(mgs_t, d2, b2, sizeof(b2));
		rip_game_folder(mgs_t, d1, fold, sizeof(fold));
		snprintf(gdir, sizeof(gdir), "%s/%s", psx, fold);
		rip_rmdir_flat(gdir);

		char t1[1024], t2[1024], c1[1024], c2[1024];
		snprintf(c1, sizeof(c1), "%s/%s.cue", gdir, b1);
		snprintf(c2, sizeof(c2), "%s/%s.cue", gdir, b2);
		snprintf(t1, sizeof(t1), "%s/%s - Track 01.bin", gdir, b1);
		snprintf(t2, sizeof(t2), "%s/%s - Track 01.bin", gdir, b2);

		// Disc 1, into a folder that is not there yet.
		fk.cancel_after = -1;
		fk.reads = 0;
		check(rip_perform_disc(&plan, psx, fold, b1, 0, 0, &io, &bad) == RIP_DONE,
			"disc 1 copies into a folder named for the game");
		check(file_bytes(c1) > 0, "its sheet is there");
		check(file_bytes(t1) > 0, "and its tracks are named after it, not just Track 01");

		long long d1_cue = file_bytes(c1), d1_trk = file_bytes(t1);

		/*
		  And now the disc that was offered as a replacement. It must be ADDED, and the
		  refusal that guards the replace prompt must not fire: same game, different disc.
		*/
		fk.reads = 0;
		check(rip_perform_disc(&plan, psx, fold, b2, 0, 0, &io, &bad) == RIP_DONE,
			"disc 2 of the same game copies in beside it without being told to overwrite");
		check(file_bytes(c2) > 0, "disc 2 has its own sheet");
		check(file_bytes(t2) > 0, "and its own tracks");
		check(file_bytes(c1) == d1_cue && file_bytes(t1) == d1_trk,
			"and disc 1 is still there, byte for byte - which is the bug this is about");

		// Both sheets in one folder is what psx.cpp's same-directory compare needs to swap
		// discs without resetting the console or switching memory card.
		check(dir_is_there(gdir), "both discs are in the one folder the core swaps within");

		/*
		  A re-rip of the SAME disc is the one case that IS a replacement, and it still asks.
		  Same serial, so same base, so the sheet it would write is already there.
		*/
		fk.reads = 0;
		check(rip_perform_disc(&plan, psx, fold, b2, 0, 0, &io, &bad) == RIP_EXISTS,
			"re-ripping the same serial refuses until it is confirmed");
		check(fk.reads == 0, "without reading a sector");
		check(file_bytes(c1) == d1_cue,
			"and the refusal costs the other disc nothing either");

		// Confirmed, it replaces that disc and leaves its sibling alone.
		fk.reads = 0;
		check(rip_perform_disc(&plan, psx, fold, b2, 0, 1, &io, &bad) == RIP_DONE,
			"a confirmed re-rip of that disc goes ahead");
		check(file_bytes(c2) > 0, "the disc it was about is back");
		check(file_bytes(c1) == d1_cue && file_bytes(t1) == d1_trk,
			"and the disc it was NOT about was never touched");

		/*
		  A PAL copy and an NTSC copy of one title are different games with different saves,
		  and the region in the folder name is what keeps them apart. Before this they were
		  one folder called "Metal Gear Solid" and the second one ripped replaced the first.
		*/
		char ufold[96], ub[160], udir[1024], ucue[1024];
		rip_game_folder(mgs_t, "SLUS-00594", ufold, sizeof(ufold));
		rip_disc_base(mgs_t, "SLUS-00594", ub, sizeof(ub));
		check(strcmp(ufold, fold), "the USA copy of a title is a different folder to the PAL one");

		snprintf(udir, sizeof(udir), "%s/%s", psx, ufold);
		snprintf(ucue, sizeof(ucue), "%s/%s.cue", udir, ub);
		rip_rmdir_flat(udir);

		fk.reads = 0;
		check(rip_perform_disc(&plan, psx, ufold, ub, 0, 0, &io, &bad) == RIP_DONE,
			"so the NTSC copy rips without being offered the PAL one to replace");
		check(file_bytes(ucue) > 0, "it has its own sheet");
		check(file_bytes(c1) == d1_cue, "and the PAL copy is untouched");

		/*
		  The sheet a per-disc rip writes is the same sheet in every respect the six cue
		  parsers in this tree care about - uppercase keywords, space indent, one FILE per
		  track, the same mode tokens and INDEX lines. The only difference is the filename
		  each FILE names, which is the whole point.
		*/
		{
			// Built the same way cue_true is, so the two can be read side by side: every
			// line is identical except the name inside each FILE.
			char want[8192];
			snprintf(want, sizeof(want),
				"FILE \"%s - Track 01.bin\" BINARY\n"
				"  TRACK 01 MODE2/2352\n"
				"    INDEX 01 00:00:00\n"
				"FILE \"%s - Track 02.bin\" BINARY\n"
				"  TRACK 02 AUDIO\n"
				"    INDEX 00 00:00:00\n"
				"    INDEX 01 00:02:00\n"
				"FILE \"%s - Track 03.bin\" BINARY\n"
				"  TRACK 03 AUDIO\n"
				"    INDEX 01 00:00:00\n", b2, b2, b2);

			char got[8192];
			check(slurp(c2, got, sizeof(got)) && !strcmp(got, want),
				"disc 2's sheet is the format the six parsers accept, naming its own tracks");
			check(!strstr(got, "FILE \"Track 01.bin\""),
				"and names none of the bare track files that would be disc 1's");

			/*
			  And the sheet of a single-disc rip is byte for byte what it always was: the
			  format tests above pin cue_true, and rip_cue_text() still produces it.
			*/
			char plain[8192];
			check(rip_cue_text(&plan, 0, plain, sizeof(plain)) && !strcmp(plain, cue_true),
				"and a single-disc sheet is unchanged, byte for byte");
		}

		/*
		  A folder ripped before any of this existed is adopted rather than orphaned.

		  This is the owner's actual card: disc 1 sitting in "Metal Gear Solid/" under the
		  bare title. Disc 2 has to join it, because two folders would defeat the
		  same-directory compare psx.cpp swaps on.
		*/
		char legacy[1024], picked[96];
		snprintf(legacy, sizeof(legacy), "%s/%s", psx, mgs_t);
		rip_rmdir_flat(gdir);
		rip_rmdir_flat(legacy);
		mkpath(legacy);
		touch(legacy, "Metal Gear Solid.cue", 20);

		check(rip_target_folder(psx, mgs_t, d2, picked, sizeof(picked))
			&& !strcmp(picked, mgs_t),
			"a disc whose game is already on the card under the bare title joins it");

		fk.reads = 0;
		check(rip_perform_disc(&plan, psx, picked, b2, 0, 0, &io, &bad) == RIP_DONE,
			"and copying into it adds a disc rather than replacing the folder");

		char lkeep[1024], lnew[1024];
		snprintf(lkeep, sizeof(lkeep), "%s/Metal Gear Solid.cue", legacy);
		snprintf(lnew, sizeof(lnew), "%s/%s.cue", legacy, b2);
		check(file_bytes(lkeep) == 20, "the rip that was already there is exactly as it was");
		check(file_bytes(lnew) > 0, "with the new disc beside it");

		// And with the game's folder gone again, the region-qualified name is what a fresh
		// rip creates - the legacy name is adopted, never invented.
		rip_rmdir_flat(legacy);
		check(rip_target_folder(psx, mgs_t, d2, picked, sizeof(picked))
			&& !strcmp(picked, "Metal Gear Solid (Europe)"),
			"but with nothing on the card, a fresh rip gets the region-qualified folder");

		rip_rmdir_flat(gdir);
		rip_rmdir_flat(udir);
	}

	/* -------------------------------- what the other three cores' folders DO get now --- */

	/*
	  A Mega CD copy, end to end: the bytes, and then the card.

	  This used to be the check that a Mega CD rip appeared nowhere. The copy was written
	  into games/Genesis, `md` accepts "md,bin,gen" and not "cue" so the sheet was not a game
	  to it, and 73b0f71's rule then correctly hid the tracks beside it - so the folder was
	  correct, loadable from the core's own browser, and invisible to the shelf. That was the
	  one place this feature was knowingly incomplete and the check said so out loud.

	  It is not the case any more, and the fix is not the one that was tempting. "cue" was
	  never going to be added to the md row - that row launches the Genesis core with a
	  load-to-memory mount, and a .cue card there would draw and sort like a game and fail
	  the moment it was pressed. Mega CD is its own shelf system, reading games/MegaCD with
	  an 's'/0 mount into the MegaCD core, so a copy written there is a card.

	  Which folder the front-end chooses is asserted where that choice is made, in
	  assert_rip_screen(); what is asserted here is that a copy in that folder becomes
	  exactly one properly named card with its tracks hidden - the shape 73b0f71 promised and
	  the thing a player will actually look for.
	*/
	{
		const char *gname = "Mega Test Disc";
		char gdir[1024];
		snprintf(gdir, sizeof(gdir), "%s/%s", ROOT "/games/MegaCD", gname);
		rip_rmdir_flat(gdir);

		fk.reads = 0;
		check(rip_perform(&plan, ROOT "/games/MegaCD", gname, 1, 0, &io, &bad) == RIP_DONE,
			"a Mega CD rip writes its folder");

		snprintf(path, sizeof(path), "%s/%s.cue", gdir, gname);
		{
			char got[4096];
			check(slurp(path, got, sizeof(got)) && !strcmp(got, cue_m1),
				"in the MODE1/2352 form Mega CD's parser can read");
		}

		lib_rescan();
		for (int i = 0; i < 400 && lib_scanning(); i++) frame(2);
		frame(10);

		char rel[256];
		snprintf(rel, sizeof(rel), "%s/%s.cue", gname, gname);
		int card_item = item_at("megacd", rel);
		check(card_item >= 0,
			"and it IS on the shelf: Mega CD reads cue and mounts it into the MegaCD core, "
			"so the copy is a card rather than something only the core's browser can find");

		snprintf(rel, sizeof(rel), "%s/Track 01.bin", gname);
		check(item_at("megacd", rel) < 0, "with its tracks hidden rather than listed as games");

		lib_view_build(VIEW_ALL, -1, SORT_TITLE);
		int card = entry_carrying(card_item);
		check(card >= 0 && entry_nvar(card) == 1, "so the copy is exactly one card");
		if (card >= 0)
		{
			chome_item *ci = lib_item(card_item);
			check(ci && !strcmp(ci->title, gname), "under the name the disc was copied as");
		}

		rip_rmdir_flat(gdir);
	}

	/*
	  And the same for the other two, on the folder alone. Their sheets are byte-identical to
	  Mega CD's - neogeocd.cpp calls megacdd's own parser and pcecdd reads the same two mode
	  tokens - so what is worth checking separately is only that each one's folder is scanned
	  by a system that accepts a .cue, which is the part that was missing.
	*/
	{
		static const struct { const char *sysid, *games; } cd[] =
		{
			{ "pcecd",    ROOT "/games/TGFX16-CD" },
			{ "neogeocd", ROOT "/games/NeoGeo-CD" },
		};

		for (unsigned i = 0; i < sizeof(cd) / sizeof(cd[0]); i++)
		{
			const char *gname = "Disc Test";
			char gdir[1024];
			snprintf(gdir, sizeof(gdir), "%s/%s", cd[i].games, gname);
			rip_rmdir_flat(gdir);

			fk.reads = 0;
			check(rip_perform(&plan, cd[i].games, gname, 1, 0, &io, &bad) == RIP_DONE,
				"a rip writes its folder");

			lib_rescan();
			for (int j = 0; j < 400 && lib_scanning(); j++) frame(2);
			frame(10);

			char rel[256], msg[160];
			snprintf(rel, sizeof(rel), "%s/%s.cue", gname, gname);
			snprintf(msg, sizeof(msg), "and %s shows it as a card", cd[i].sysid);
			check(item_at(cd[i].sysid, rel) >= 0, msg);

			snprintf(rel, sizeof(rel), "%s/Track 01.bin", gname);
			snprintf(msg, sizeof(msg), "with %s's tracks not listed as games", cd[i].sysid);
			check(item_at(cd[i].sysid, rel) < 0, msg);

			rip_rmdir_flat(gdir);
		}
	}

	/*
	  And Saturn, which is the one that used to be refused.

	  It is the only destination in the table whose core cannot be handed the pressed disc,
	  so this is the block that says the copy does not care: the same helper reads the same
	  sectors, writes the sheet in the form saturncdd.cpp parses, and games/Saturn turns it
	  into a card like any other folder the scanner walks. The sheet is checked here rather
	  than only in the rip_cue_text() block above because the mode flag reaches it through
	  the rip_targets row, and a wrong row there would still produce a valid-looking sheet.
	*/
	{
		const char *gname = "Saturn Test Disc";
		char gdir[1024];
		snprintf(gdir, sizeof(gdir), "%s/%s", ROOT "/games/Saturn", gname);
		rip_rmdir_flat(gdir);

		fk.reads = 0;
		check(rip_perform(&plan, ROOT "/games/Saturn", gname, 0, 0, &io, &bad) == RIP_DONE,
			"a Saturn rip writes its folder");

		snprintf(path, sizeof(path), "%s/%s.cue", gdir, gname);
		{
			char got[4096];
			check(slurp(path, got, sizeof(got)) && !strcmp(got, cue_true),
				"with the measured mode in it, which Saturn's parser reads and Mega CD's does not");
		}

		lib_rescan();
		for (int i = 0; i < 400 && lib_scanning(); i++) frame(2);
		frame(10);

		char rel[256];
		snprintf(rel, sizeof(rel), "%s/%s.cue", gname, gname);
		check(item_at("saturn", rel) >= 0,
			"and the copy is a card on the Saturn shelf - the disc we cannot play from the "
			"drive plays from the card");

		snprintf(rel, sizeof(rel), "%s/Track 01.bin", gname);
		check(item_at("saturn", rel) < 0, "with its tracks hidden rather than listed as games");

		rip_rmdir_flat(gdir);
	}

	/* ------------------------------------------------------------------ tidy up --- */

	// The card goes back as it was, because everything after this section scans it.
	rip_rmdir_flat(dir);
	rip_rmdir_flat(edir);

	lib_rescan();
	for (int i = 0; i < 400 && lib_scanning(); i++) frame(2);
	frame(10);

	{
		char rel[256];
		snprintf(rel, sizeof(rel), "%s/%s.cue", name, name);
		check(item_at("psx", rel) < 0, "and the card is back as this section found it");
	}
}

/* ------------------------------------------------ the screen a rip is watched on --- */

/*
  The dialog while a copy runs: what it offers, what it says, and the pie.

  There is no child and no drive here, so what drives this is rip_test_set(), which writes
  the same fields the child's published line writes - see chome_rip.h. Everything downstream
  of those fields is the shipping code: the row that starts it, the snapshot the screen is
  drawn from, the fraction, the two presses that stop it and the rectangle the partial
  repaint clips to.
*/
/*
  One frame of the rip screen at a chosen progress, composed WITHOUT letting the clock move.

  That last part is the whole method. The disc is turning, so two frames taken a moment apart
  differ in nearly every pixel of it whatever the pie is doing - which is how the first
  version of this measurement reported 90% of the disc dimmed at every fraction. With the
  clock held, disc_step() answers from its memo and the rotation is bit-for-bit the same, so
  the *only* difference between two shots is the mask, and counting changed pixels counts
  exactly the area the pie covers.

  Out to the badge and back is what forces a full repaint at an instant the clock has not
  moved through - the same trick assert_partial_repaint() uses, and it works here for the
  same reason: a screen change marks dirty, and no key press of its own advances time.
*/
static void rip_pie_shot(int done, int total, int x0, int y0, int x1, int y1, uint32_t *out)
{
	rip_test_set(RIP_RUNNING, done, total, 0);

	chome_handle(KEY_ESC);
	chome_handle(KEY_ESC | UPSTROKE);
	chome_handle(KEY_ENTER);
	chome_handle(KEY_ENTER | UPSTROKE);

	const uint32_t *fb = harness_fb_shown();
	int w = gfx_w();
	for (int y = y0, i = 0; y < y1; y++)
		for (int x = x0; x < x1; x++, i++) out[i] = fb[(size_t)y * w + x];
}

// How many pixels of one shot differ from another, which is how much of the disc is dimmed.
static int rip_pie_diff(const uint32_t *a, const uint32_t *b, int n)
{
	int d = 0;
	for (int i = 0; i < n; i++) if (a[i] != b[i]) d++;
	return d;
}

/*
  Publish what a rip has got to, and then ask for the repaint that publishing it would have
  asked for.

  In the firmware chome_handle() folds the state, the percentage and the unreadable count into
  one number as rip_poll() reads each line, and marks dirty when it changes - so the line
  under the disc and the button beside it are only ever drawn by a full frame. That is
  deliberate: everything except the pie is outside the disc's rectangle, and the spin's
  partial repaint does not touch it.

  rip_test_set() writes those fields without going through rip_poll(), so a test that only
  advanced time afterwards would be looking at the *previous* line under a moving pie - which
  is exactly what the first version of this section dumped, and what made a finished rip's
  screen still say "Stop". So every use of it here comes through this.
*/
static void rip_show(int state, int done, int total, int bad)
{
	rip_test_set(state, done, total, bad);

	chome_handle(KEY_ESC);
	chome_handle(KEY_ESC | UPSTROKE);
	chome_handle(KEY_ENTER);
	chome_handle(KEY_ENTER | UPSTROKE);
	frame(2);
}

// Pixels of one exact colour anywhere on the shown frame. Exact is legitimate for the
// same reason panel_rows_pixels() says it is: gfx writes colours through unblended.
static int count_shown(uint32_t want)
{
	const uint32_t *fb = harness_fb_shown();
	int w = gfx_w(), h = gfx_h();
	if (!fb) return 0;

	int n = 0;
	for (int i = 0; i < w * h; i++) if (fb[i] == want) n++;
	return n;
}

/*
  The half-resolution canvas: the option classicui_halfres stands for, on by default on
  the device and exercised here against the model in stubs.cpp.

  What is being claimed, in order: the request halves the canvas; the layout profile
  and the overscan follow the *display* rather than the shrunken canvas, so a 720p
  screen keeps the hd layout instead of dropping to sd with a CRT margin on an HDMI
  panel; the text scales halve to the same glass size; turning the option off mid-
  session restores the full canvas on the next frame with nothing pressed; the request
  is refused below 320x240, which is what protects the analog takeover's TV canvas; the
  in-game still survives the resize instead of falling through to the grid; and leaving
  the front-end releases the request, because the framebuffer after us belongs to
  somebody else.
*/
static void assert_half_canvas()
{
	printf("\n== the half-resolution canvas: quarter the pixels, the same layout ==\n");

	// A clean shelf at 720p, the option off, exactly as the suite runs everywhere else.
	harness_set_menu_core(1);
	harness_set_fb(1280, 720);
	gfx_shutdown();
	theme_update(1280, 720, 0);
	chome_leave();
	press(KEY_MENU, 20);
	for (int i = 0; i < 80 && lib_scanning(); i++) frame(2);
	frame(20);

	check(chome_screen_id() == 0 && gfx_w() == 1280 && gfx_h() == 720,
		"on the shelf at the full 720p canvas");

	const chome_profile *p = theme_get();
	check(p->id == PROF_HD && p->visible == 5, "which lays out hd");
	int full_tt = p->ts_title, full_tu = p->ts_ui;

	/*
	  The option lands with no key pressed: the frame loop re-asserts the request the
	  way it re-asserts the analog claim, and the resize branch picks the new canvas up.
	*/
	cfg.classicui_halfres = 1;
	frame(6);

	check(gfx_w() == 640 && gfx_h() == 360, "turning the option on halves the canvas by itself");
	check(video_menu_fb_div() == 2, "and the divisor in force says so");

	p = theme_get();
	check(p->id == PROF_HD && p->visible == 5,
		"the layout is still hd: the profile is chosen from the display, not the canvas");
	check(p->safe_x == 0 && p->safe_y == 0,
		"and no overscan margin appears - this is still an HDMI display showing every pixel");
	check(p->ts_ui == 1 && full_tu == 2,
		"ui text halves in canvas pixels, which is the same size on the glass");
	check(p->ts_title == (full_tt + 1) / 2,
		"the title rounds up to 2, the one scale that cannot halve exactly");

	frame(6);
	dump("half-1-shelf-360");

	// The partial machinery is the same code at this size; prove the strongest thing
	// about it once here: a forced full repaint reproduces the shown frame exactly.
	{
		unsigned long shown = harness_fb_hash_box(0, 0, gfx_w(), gfx_h());
		check(force_full_repaint(), "a full repaint can be forced at the half canvas");
		check(harness_fb_hash_box(0, 0, gfx_w(), gfx_h()) == shown,
			"and it reproduces the shown frame byte for byte");
	}

	// The row is in the option table with the shipped default, so More Settings
	// carries it: the generic table checks cover the rest.
	{
		int i = opt_find("classicui_halfres");
		check(i >= 0, "the option is on the More Settings screen");
		check(i >= 0 && opt_at(i)->def == 1 && opt_at(i)->rec == 1,
			"fast is both the firmware default and the recommendation");
		check(i >= 0 && opt_at(i)->when == OW_NOW, "and it says it takes effect now");
	}

	// Off again mid-session: the same no-key path, the other direction.
	cfg.classicui_halfres = 0;
	frame(6);
	check(gfx_w() == 1280 && gfx_h() == 720, "turning it off restores the full canvas live");

	/*
	  The floor. A 320x240 canvas is the analog takeover's, and half of it would be
	  smaller than the smallest layout this front-end ships - so the request is refused
	  and the canvas arrives whole, option or no option.
	*/
	cfg.classicui_halfres = 1;
	harness_set_fb(320, 240);
	gfx_shutdown();
	theme_update(320, 240, 0);
	frame(6);
	check(gfx_w() == 320 && gfx_h() == 240, "a 240p canvas is never halved");
	check(video_menu_fb_div() == 1, "the request was refused, not applied and clamped");
	check(theme_get()->id == PROF_LO, "and it lays out lo, exactly as it always did");

	harness_set_fb(1280, 720);
	gfx_shutdown();
	theme_update(1280, 720, 0);
	frame(10);

	/*
	  In a game: the menu opens at the half canvas with the still of the game behind it,
	  and toggling the option under the open menu rebuilds the still at the new size
	  rather than dropping it. The still is painted flat here so it can be counted: the
	  6/16 dim of one known colour is one exact other colour, and a single pixel of it
	  proves the still survived where the grid background would have none.
	*/
	{
		FILE *f = fopen("/tmp/classicui_current", "wt");
		if (f) { fprintf(f, "gb\nTetris (World).gb\n"); fclose(f); }

		harness_set_grab_flat(0xff204060);        // dims to 0xff0c1824, counted below
		harness_set_menu_core(0);
		frame(2);

		press(KEY_MENU, 20);
		check(chome_ingame_active(), "the in-game menu opens with the option on");
		check(gfx_w() == 640 && gfx_h() == 360, "at the half canvas");
		frame(10);

		int at_half = count_shown(0xff0c1824);
		check(at_half > 1000, "the still of the game is behind the shelf at that size");

		cfg.classicui_halfres = 0;
		frame(6);
		check(gfx_w() == 1280 && gfx_h() == 720, "the canvas grows back under the open menu");
		int at_full = count_shown(0xff0c1824);
		check(at_full > at_half * 3,
			"and the still is rebuilt at the new size instead of falling through to the grid");

		press(KEY_MENU, 10);
		check(!chome_ingame_active(), "the menu closes back into the game");

		harness_set_grab_flat(0);
		harness_set_menu_core(1);
		frame(4);
	}

	/*
	  Leaving releases the request. What runs after this front-end - the classic menu's
	  wallpaper, the F9 terminal - shares the one framebuffer, and a half-size console
	  because a menu had been open would be this front-end scribbling on somebody
	  else's screen.
	*/
	cfg.classicui_halfres = 1;
	frame(6);
	check(gfx_w() == 640, "at the half canvas again");
	chome_leave();
	check(video_menu_fb_div() == 1, "handing off to the classic menu releases the request");

	// And back to the state every later section assumes: option off, 720p, on the shelf.
	cfg.classicui_halfres = 0;
	press(KEY_MENU, 20);
	frame(10);
	check(gfx_w() == 1280 && gfx_h() == 720, "the suite continues at the full canvas");
}

/*
  Repaint cost, measured rather than asserted.

  This section prints a table and checks almost nothing: its output is the before/after
  evidence for the two performance changes (the half-resolution canvas and the spin
  repaint following the angle), and a check that baked today's numbers in would fail on
  the next machine for no reason. Host times under Docker are indicative for *ratios* -
  the device's uncached framebuffer makes its copies far dearer than a host memcpy - so
  what to read off this table is paints-per-tick and how each column scales with the
  canvas, not the microseconds themselves.

  Kept under 200 painted frames per row: gfx_end() flushes and resets the counters at
  GFX_STAT_EVERY, and a row that crossed it would quietly report only the tail.
*/
static unsigned long perf_ticks;

static void perf_row(const char *canvas, const char *state)
{
	unsigned long fcomp, fcopy, frows, pcomp, pcopy, prows;
	unsigned long fc = gfx_stat_get(0, &fcomp, &fcopy, &frows);
	unsigned long pc = gfx_stat_get(1, &pcomp, &pcopy, &prows);
	unsigned long n = fc + pc;

	printf("  %-16s %-18s %4lu paints (%lu full, %lu partial) / %lu ticks, "
		"compose avg %4lu us, copy avg %4lu us, rows avg %4lu\n",
		canvas, state, n, fc, pc, perf_ticks,
		n ? (fcomp + pcomp) / n : 0,
		n ? (fcopy + pcopy) / n : 0,
		n ? (frows + prows) / n : 0);
}

static void perf_run(int ticks)
{
	gfx_stat_reset();
	perf_ticks = (unsigned long)ticks;
	for (int i = 0; i < ticks; i++) { harness_advance(16); chome_handle(0); }
}

static void measure_repaint_costs(const char *canvas)
{
	// A clean shelf, no disc, parked away from either end for the slides.
	disc_reset_reader();
	disc_ingest_present(0);
	(void)disc_take_dirty();
	chome_leave();
	press(KEY_MENU, 20);
	for (int i = 0; i < 80 && lib_scanning(); i++) frame(2);
	frame(30);
	select_first_game();
	press(KEY_RIGHT, 20);
	press(KEY_RIGHT, 20);
	frame(60);                                 // covers decoded, nothing left to land

	// Idle: nothing due, so nothing painted - the number the whole front-end rests on.
	perf_run(150);
	perf_row(canvas, "idle shelf");

	/*
	  And an idle shelf with a title too long for it, which is the resting state the marquee
	  introduced and therefore the one that had to be priced.

	  This row is the argument for the band. Read it against "full repaints" below: the paints
	  per tick tell you how often a marquee asks, and the rows column tells you what each ask
	  costs. If the two rows' rows-per-paint were close, the feature would be five full
	  repaints a second on a DE10-Nano for as long as a player left the cursor on a long name,
	  and it would not be shippable at 1080p.

	  Two and a half seconds of ticks, which is a whole opening hold plus a run of steps -
	  short enough to stay under the 200-frame ceiling perf_row() needs and long enough that
	  the hold does not dominate the average.
	*/
	select_titled("Yoshi");
	frame(20);
	perf_run(150);
	perf_row(canvas, "shelf, longest title");
	select_first_game();
	press(KEY_RIGHT, 20);
	press(KEY_RIGHT, 20);
	frame(40);

	// A full repaint per tick: the menu bar toggled with the clock moving, which marks
	// dirty on every pass the way any structural change does.
	gfx_stat_reset();
	perf_ticks = 120;
	for (int i = 0; i < 60; i++)
	{
		chome_handle(KEY_MENU);
		harness_advance(16);
		chome_handle(KEY_MENU | UPSTROKE);
		harness_advance(16);
	}
	perf_row(canvas, "full repaints");
	frame(20);                                 // an even count of toggles: the shelf, settling

	// The carousel: taps each way, the ease's frames clipped to the card band.
	gfx_stat_reset();
	perf_ticks = 0;
	for (int k = 0; k < 6; k++)
	{
		int dir = (k & 1) ? KEY_LEFT : KEY_RIGHT;
		chome_handle(dir);
		harness_advance(16);
		chome_handle(dir | UPSTROKE);
		for (int i = 0; i < 20; i++) { harness_advance(16); chome_handle(0); }
		perf_ticks += 21;
	}
	perf_row(canvas, "carousel slides");

	// A known disc on the shelf: the badge turns through the partial path, repainting
	// only when its 64-position step moves.
	disc_ingest_present(1);
	static fake_disc d;
	memset(&d, 0, sizeof(d));
	static const char *const none[] = { "" };
	fake_iso(&d, 0, "PLAYSTATION", "PLAYSTATION", none, 0);
	fake_put(&d, 20, 0, "BOOT = cdrom:\\SLUS_006.26;1", 27, 100);
	disc_set_reader(fake_read, &d);
	disc_ingest_identify(0);
	frame(10);

	perf_run(150);
	perf_row(canvas, "badge spinning");

	/*
	  The dialog over it: the full-size disc at 256 positions a turn, so nearly every
	  tick paints. Two rows, because the dialog has two steady states now: the first
	  turn after a picture arrives resamples each kept angle once as it fills the
	  rotation cache, and every turn after that is served from it - the difference
	  between the rows' compose columns is what the cache buys. See disc_rot().
	*/
	press(KEY_UP, 6);
	press(KEY_ENTER, 6);
	frame(4);
	perf_run(150);
	perf_row(canvas, "dialog, first turn");

	perf_run(150);
	perf_row(canvas, "dialog disc, warm");

	// And a rip on that dialog: the disc at the focus rate, a new angle every tick,
	// the reveal riding it - and the angles backed off to the cached set, so every
	// contended frame is a blit. See disc_step_fine().
	rip_test_set(RIP_RUNNING, 900, 2000, 0);
	chome_handle(KEY_ESC);
	chome_handle(KEY_ESC | UPSTROKE);
	chome_handle(KEY_ENTER);
	chome_handle(KEY_ENTER | UPSTROKE);
	frame(4);
	perf_run(150);
	perf_row(canvas, "dialog disc, rip");
	rip_test_reset();

	press(KEY_ESC, 6);
	press(KEY_ESC, 6);
	disc_reset_reader();
	disc_ingest_present(0);
	(void)disc_take_dirty();
	frame(10);
}

static void assert_repaint_costs()
{
	printf("\n== repaint costs, measured (host times: read ratios, not milliseconds) ==\n");

	int was_debug = cfg.debug;
	cfg.debug = 1;                             // what gates the collection

	harness_set_menu_core(1);
	cfg.classicui_halfres = 0;
	harness_set_fb(1280, 720);
	gfx_shutdown();
	theme_update(1280, 720, 0);
	measure_repaint_costs("1280x720");

	// The same display through the half-resolution option: the canvas the device
	// defaults to at 720p.
	cfg.classicui_halfres = 1;
	measure_repaint_costs("640x360 (half)");
	cfg.classicui_halfres = 0;

	harness_set_fb(320, 240);
	gfx_shutdown();
	theme_update(320, 240, 0);
	measure_repaint_costs("320x240");

	cfg.debug = was_debug;

	// One assertion, because it is the claim the idle column stands for and it must
	// never regress into "the front-end paints when nothing changed".
	harness_set_fb(1280, 720);
	gfx_shutdown();
	theme_update(1280, 720, 0);
	chome_leave();
	press(KEY_MENU, 20);
	// Long enough for every visible cover to decode: art_step() lands one per frame,
	// and a cover arriving mid-measurement is a legitimate repaint, not idle work.
	frame(80);

	gfx_stat_reset();
	int flips = harness_present_count();
	for (int i = 0; i < 100; i++) { harness_advance(16); chome_handle(0); }
	check(harness_present_count() == flips, "an idle shelf with no disc paints nothing at all");
}

/*
  The disc dialog at 60fps, which is the owner's ask in his own words: "i would like the
  disc spinning animation to be as close as possible to 60FPS when disc dialog is open".

  Four properties, each of which has a way to be quietly false:

    distinct angles per second, counted rather than adjectived - the spin path only
    paints when the step moved, so every page flip in a quiet dialog is a new angle;

    the cache is invisible in the pixels: a frame served by quadrant composition, a
    frame resampled directly at the full angle, and a frame from a dropped-and-refilled
    cache are the same bytes, at the same instant, on both the generated face and a
    scan. This is the property that lets the partial repaint stay byte-identical to a
    full one whatever the cache happens to hold;

    the cost profile: a warm turn at the sizes the cache holds whole resamples nothing,
    and the resample counter is the only witness - the pixels are identical by design;

    the backoff: under a rip the angles shown are exactly the cached set, the disc
    never turns backwards through the transition, a contended second resamples nothing,
    and the full count is back within a few frames of the rip ending - a disc that
    stayed coarse after the work finished would read as a bug.

  Byte-identity comparisons here lean on the same discipline as every other section
  that does them: presses without harness_advance() do not move the clock, and both
  step quantisations are memoised on the millisecond, so "the same instant" always
  draws the same angle whichever path composes it.
*/
static unsigned long disc_smooth_fullhash()
{
	// A full repaint of the pinned instant: out to the tier and straight back, no
	// clock in between - the pair the other byte-identity sections use.
	chome_handle(KEY_ESC);
	chome_handle(KEY_ESC | UPSTROKE);
	chome_handle(KEY_ENTER);
	chome_handle(KEY_ENTER | UPSTROKE);
	return harness_fb_hash_box(0, 0, gfx_w(), gfx_h());
}

// One pinned instant, three ways: as shown, resampled directly, and through a cache
// that was dropped and refilled. `tag` names the canvas in the failure text.
static void disc_smooth_identity(const char *tag)
{
	char what[192];

	check(spin_paint(), "a spin frame lands to compare");
	unsigned long shown = harness_fb_hash_box(0, 0, gfx_w(), gfx_h());
	int step_was = disc_test_shown_step();

	disc_test_rot_direct(1);
	unsigned long direct = disc_smooth_fullhash();

	disc_test_rot_direct(0);
	disc_test_rot_drop();
	unsigned long refill = disc_smooth_fullhash();

	snprintf(what, sizeof(what),
		"%s step %d: the direct resample matches the cache-served frame byte for byte",
		tag, step_was);
	check(direct == shown, what);

	snprintf(what, sizeof(what),
		"%s step %d: and a dropped, refilled cache reproduces it again", tag, step_was);
	check(refill == shown, what);
}

static void measure_disc_smoothness(const char *canvas, int want_dia, int want_stride)
{
	enum { S_HOME = 0, S_DISC = 17 };
	char what[192];

	// A PSX disc, identified, exactly as the cost table sets one up.
	chome_leave();
	press(KEY_MENU, 20);
	for (int i = 0; i < 80 && lib_scanning(); i++) frame(2);
	frame(30);

	disc_ingest_present(1);
	static fake_disc d;
	memset(&d, 0, sizeof(d));
	static const char *const none[] = { "" };
	fake_iso(&d, 0, "PLAYSTATION", "PLAYSTATION", none, 0);
	fake_put(&d, 20, 0, "BOOT = cdrom:\\SLUS_006.26;1", 27, 100);
	disc_set_reader(fake_read, &d);
	disc_ingest_identify(0);
	frame(10);

	press(KEY_UP, 6);
	press(KEY_ENTER, 6);
	snprintf(what, sizeof(what), "%s: the dialog is up", canvas);
	check(chome_screen_id() == S_DISC, what);
	frame(4);

	// What the budget allowed at this size, read from the code rather than recomputed.
	int slots = 0, stride = 0;
	long bytes = 0;
	int dia = disc_test_rot_info(&slots, &stride, &bytes);

	printf("  %-16s disc %3d px: %2d slots at stride %d, %7ld bytes of cache\n",
		canvas, dia, slots, stride, bytes);

	snprintf(what, sizeof(what), "%s: the disc is the %d px the layout table pins", canvas, want_dia);
	check(dia == want_dia, what);
	snprintf(what, sizeof(what), "%s: the kept angles divide the turn evenly", canvas);
	check(slots > 0 && 64 % slots == 0 && 64 / slots == stride, what);
	snprintf(what, sizeof(what), "%s: the stride is %d", canvas, want_stride);
	check(stride == want_stride, what);
	check(bytes <= 6553600L, "and the slots stay inside DISC_ROT_CACHE_BYTES");

	/*
	  Warm the cache: the quadrant index revisits every kept angle within a quarter of
	  a turn, which at the slow rate is one second. Then the count he asked for: two
	  exact seconds of ticks, every page flip a distinct angle by construction.
	*/
	frame(80);

	int renders0 = disc_test_rot_renders();
	int flips0 = harness_present_count();
	frame(125);
	int angles = harness_present_count() - flips0;
	int renders = disc_test_rot_renders() - renders0;

	printf("  %-16s %d distinct angles in 125 ticks (%d/s), %d resamples behind them\n",
		canvas, angles, angles / 2, renders);

	snprintf(what, sizeof(what),
		"%s: at least 120 of 125 ticks showed a new angle - 60fps, not an adjective", canvas);
	check(angles >= 120, what);

	if (want_stride == 1)
	{
		snprintf(what, sizeof(what),
			"%s: and every one was a blit - a warm turn resamples nothing", canvas);
		check(renders == 0, what);
	}
	else
	{
		snprintf(what, sizeof(what),
			"%s: and the kept angles were blits - fewer resamples than frames", canvas);
		check(renders > 0 && renders < angles, what);
	}

	// The cache against the pixels, on the generated face, across all four quarters:
	// a quarter of a turn is a second at this rate, 63 ticks.
	for (int k = 0; k < 4; k++)
	{
		disc_smooth_identity(canvas);
		frame(63);
	}

	press(KEY_ESC, 6);
	press(KEY_ESC, 6);
	disc_reset_reader();
	disc_ingest_present(0);
	(void)disc_take_dirty();
	frame(10);
}

static void assert_disc_smoothness()
{
	printf("\n== the dialog disc at 60fps: angles counted, cache invisible, backoff honest ==\n");

	enum { S_HOME = 0, S_DISC = 17 };

	harness_set_menu_core(1);
	cfg.classicui_disc = 1;
	rip_test_reset();

	/*
	  The three canvases the cost table measures, with what each must get: the pinned
	  diameter from assert_disc_dialog_size()'s table, and the stride the budget forces.
	  At 96 and 160 px the budget holds the whole turn (stride 1); at the full-resolution
	  288 px it holds every fourth angle, which is exactly the 64-position turn that
	  shipped - so "backed off" can never mean "coarser than it ever was".
	*/
	cfg.classicui_halfres = 0;
	harness_set_fb(1280, 720);
	gfx_shutdown();
	theme_update(1280, 720, 0);
	measure_disc_smoothness("1280x720", 288, 4);

	cfg.classicui_halfres = 1;
	measure_disc_smoothness("640x360 (half)", 160, 1);
	cfg.classicui_halfres = 0;

	harness_set_fb(320, 240);
	gfx_shutdown();
	theme_update(320, 240, 0);
	measure_disc_smoothness("320x240", 96, 1);

	/*
	  The masked path and the backoff, on the canvas where both bite: full-resolution
	  720p, the 288 px disc, stride 4. A scan is the picture he actually looks at once
	  his credentials fetch art, and its mask is composited per angle - so it is the
	  masked bytes the quadrant composition most needs checking against.
	*/
	harness_set_fb(1280, 720);
	gfx_shutdown();
	theme_update(1280, 720, 0);
	chome_leave();
	press(KEY_MENU, 20);
	for (int i = 0; i < 80 && lib_scanning(); i++) frame(2);
	frame(30);

	disc_ingest_present(1);
	static fake_disc d;
	memset(&d, 0, sizeof(d));
	static const char *const none[] = { "" };
	fake_iso(&d, 0, "PLAYSTATION", "PLAYSTATION", none, 0);
	fake_put(&d, 20, 0, "BOOT = cdrom:\\SLUS_006.26;1", 27, 100);
	disc_set_reader(fake_read, &d);
	disc_ingest_identify(0);
	frame(10);

	mkpath(ROOT "/classicui/discart");
	make_cover(ROOT "/classicui/discart/SLUS-00626.png", 400, 400, 0xff20c020);

	press(KEY_UP, 6);
	press(KEY_ENTER, 6);
	check(chome_screen_id() == S_DISC, "the dialog is up over the scan");
	frame(12);

	{
		int cx = 0, cy = 0;
		int sd = disc_drawn_box(&cx, &cy);
		check(sd > 200 && box_pixels(cx - sd / 4, cy - sd / 4, cx + sd / 4, cy + sd / 4,
			0xff20c020u) > 100, "and the scan is what it is drawing");
	}

	frame(80);                                   // a warm cache of masked frames
	for (int k = 0; k < 4; k++)
	{
		disc_smooth_identity("scan");
		frame(63);
	}

	/* ------------------------------------------------ the backoff, under a real rip --- */

	{
		rip_test_set(RIP_RUNNING, 900, 2000, 0);
		frame(4);

		int coarse_ok = 1, forward_ok = 1, prev = -1;
		for (int i = 0; i < 12; i++)
		{
			if (!spin_paint()) break;
			int s = disc_test_shown_step();
			if (s % 4) coarse_ok = 0;
			if (prev >= 0 && ((s - prev) & 255) >= 128) forward_ok = 0;
			prev = s;
		}
		check(prev >= 0, "the rip screen is painting");
		check(coarse_ok, "under a rip every angle shown is one the cache holds whole");
		check(forward_ok, "and the backoff never runs the disc backwards");

		/*
		  A quarter turn of warm-up before counting resamples, because the identity block
		  above ends by deliberately dropping the cache - so the first contended frames
		  are legitimately refilling slots, one resample each, exactly as the first turn
		  after a scan lands would. The claim under test is the steady state.
		*/
		frame(30);

		int renders1 = disc_test_rot_renders();
		int flips1 = harness_present_count();
		frame(60);
		printf("  contended: %d paints in 60 ticks, %d resamples\n",
			harness_present_count() - flips1, disc_test_rot_renders() - renders1);
		check(disc_test_rot_renders() == renders1,
			"a contended second of spinning resamples nothing at all");

		/*
		  Recovery, which his refinement asks about by name: the moment the rip ends the
		  count is fine again. Within a few paints, because the quantisation is per
		  millisecond and the very next new step may land on a multiple of four honestly.
		*/
		rip_test_reset();
		frame(2);
		int fine_again = 0;
		for (int i = 0; i < 8 && !fine_again; i++)
		{
			if (!spin_paint()) break;
			if (disc_test_shown_step() % 4) fine_again = 1;
		}
		check(fine_again, "the full angle count is back within a few frames of the rip ending");
	}

	// And leave nothing behind: the cover, the disc, the rip state, the canvas.
	unlink(ROOT "/classicui/discart/SLUS-00626.png");
	press(KEY_ESC, 6);
	press(KEY_ESC, 6);
	rip_test_reset();
	disc_reset_reader();
	disc_ingest_present(0);
	(void)disc_take_dirty();
	frame(10);
}

static void assert_rip_screen()
{
	printf("\n== ripping a disc: the screen it is watched on ==\n");

	enum { S_HOME = 0, S_DISC = 17, S_DISCBAR = 18 };

	int was_prof = cfg.classicui_profile;
	int was_w = gfx_w(), was_h = gfx_h();

	cfg.classicui_profile = 1;
	harness_set_fb(1280, 720);
	frame(6);

	cfg.classicui_disc = 1;
	rip_test_reset();

	// A PlayStation disc, identified, exactly as the other disc sections set one up.
	fake_disc dp; memset(&dp, 0, sizeof(dp));
	static const char *const none[] = { "" };
	fake_iso(&dp, 0, "PLAYSTATION", "PLAYSTATION", none, 0);
	fake_put(&dp, 20, 0, "BOOT = cdrom:\\SLUS_006.26;1", 27, 100);

	disc_ingest_present(1);
	disc_set_reader(fake_read, &dp);
	disc_ingest_identify(0);
	frame(6);

	press(KEY_UP);
	press(KEY_ENTER);
	check(chome_screen_id() == S_DISC, "the dialog is up over a PlayStation disc");

	/* ------------------------------------------------------- the row that starts it --- */

	press(KEY_RIGHT);                                  // onto Options
	press(KEY_ENTER);
	int rows_shot = (int)pt_panel_hash();
	check(rows_shot != 0, "the Options list is up");

	/*
	  Down to the last row, which is the copy. The rows above it are the cores, and the copy
	  row is deliberately last: it is not what most players open this list for.

	  Held rather than tapped, and that is not a style choice: the list wraps at its ends now
	  (see wrap_step in chome_ui.cpp), so eight taps down a list of four rows hand the disc to
	  whichever core the count happens to land on - which this section did, and launched
	  TurboGrafx-16 off a PlayStation disc. Holding the key walks to the last row and stays
	  there whatever the list is worth, which is what "down to the last row" always meant.
	*/
	hold_dir(KEY_DOWN, 12, 8);
	dump("rip-01-options");

	check(rip_test_starts() == 0, "nothing has been started yet");
	press(KEY_ENTER, 8);

	check(rip_test_starts() == 1, "pressing the copy row starts a rip");
	check(!strcmp(rip_test_last_dir(), ROOT "/games/PSX"),
		"into the PlayStation games folder, which is where the shelf looks for PlayStation games");
	/*
	  The folder is the game and the region it came from, and the sheet inside it is named
	  by the serial. Asserted by shape rather than by a literal title: whether the disc
	  resolves to "Ridge Racer" depends on whether the title table is installed at this
	  point, and the naming rule is the thing under test.
	*/
	{
		const char *nm = rip_test_last_name();
		const char *bs = rip_test_last_base();
		int nlen = (int)strlen(nm);

		check(nlen > 6 && !strcmp(nm + nlen - 6, " (USA)"),
			"in a folder named for the game and the region its serial places it in");

		char want_base[256];
		snprintf(want_base, sizeof(want_base), "%s (SLUS-00626)", nm);
		check(!strcmp(bs, want_base),
			"and the sheet inside it carries the serial, so another disc of the set cannot collide");
	}
	check(rip_test_last_mode1() == 0,
		"and told that this core's parser understands MODE2/2352, so the sheet may say it");
	check(rip_test_last_overwrite() == 0, "with nothing to replace, so no confirmation was needed");

	check(chome_screen_id() == S_DISC, "the dialog stays up, now showing the copy");

	/* ------------------------------------------------------------ what it says --- */

	rip_show(RIP_RUNNING, 0, 0, 0);
	dump("rip-02-reading");

	rip_show(RIP_RUNNING, 500, 2000, 0);
	check(rip_percent(rip_state()) == 25, "a quarter of the sectors written reads as 25%");
	dump("rip-03-quarter");

	rip_show(RIP_RUNNING, 1000, 2000, 0);
	dump("rip-04-half");

	rip_show(RIP_RUNNING, 1990, 2000, 0);
	check(rip_percent(rip_state()) == 99,
		"and the last few sectors read as 99, not 100 - the sheet is not written yet");

	/*
	  Progress is the sectors and nothing else. A fraction taken from the track index would
	  sit at a third through a PlayStation disc's one huge data track for twenty minutes; a
	  fraction taken from the clock would be a guess. This is the check that fails if either
	  ever creeps in.
	*/
	rip_test_set(RIP_RUNNING, 0, 2000, 0);
	check(rip_percent(rip_state()) == 0, "nothing written is 0%");
	rip_test_set(RIP_DONE, 2000, 2000, 0);
	check(rip_percent(rip_state()) == 100, "and only a finished rip is 100%");

	/*
	  And what the panel actually SAYS while it copies, read back through the same
	  disc_dlg_get() the drawing calls.

	  The owner saw "Copying to the cardc" on a real 240p screen at the start of a rip -
	  a stray character after "card" where the percentage should be. The format string in
	  the binary is right and rip_percent() is bounded 0..99, so whatever produces that
	  is between the snprintf and the glyphs. These read the line itself, so the next
	  person does not have to photograph a television to find out what it said.
	*/
	{
		char t[128], sub[128];

		rip_test_set(RIP_RUNNING, 0, 2000, 0);
		disc_test_dlg_text(t, sizeof(t), sub, sizeof(sub));
		check((int)strlen(sub) <= 20, "the copying line fits the narrow panel, so it cannot be cut");
		check(strstr(sub, "0%") != 0, "it carries the percentage it is at");

		rip_test_set(RIP_RUNNING, 320, 2000, 0);
		disc_test_dlg_text(t, sizeof(t), sub, sizeof(sub));
		check(strstr(sub, "16%") != 0, "and follows it up");
		check((int)strlen(sub) <= 20, "and still fits once it is under way");
	}

	/* ------------------------------------------------------------------- the pie --- */

	/*
	  The reveal, measured off the screen rather than asked of the code.

	  The disc has no scan on the card here, so it is the generated face: twelve bands of
	  known colour turning. That makes "is this pixel dimmed" answerable without knowing the
	  picture - the dim is a multiply by DISC_REVEAL_DIM/255 against black, so a dimmed pixel
	  is strictly darker than the same pixel undimmed, and the *count* of pixels that changed
	  between two reveals of the same instant is the area the pie covers.

	  Counted between two composes with the clock held still, so the rotation is identical
	  and the only difference between the two frames is the mask. disc_step() memoises on the
	  millisecond clock, which is what makes that possible at all.
	*/
	int cx = 0, cy = 0;
	rip_show(RIP_RUNNING, 2000, 2000, 0);            // fully revealed
	frame(20);                                       // and any scan on the card loaded
	int dia = disc_drawn_box(&cx, &cy);
	check(dia > 200, "the disc is drawn at the dialog's own size while the rip runs");
	printf("  the disc is %d px across at (%d,%d) during a rip\n", dia, cx, cy);

	int r = dia / 2;
	int x0 = cx - r, y0 = cy - r, x1 = cx + r, y1 = cy + r;
	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (x1 > gfx_w()) x1 = gfx_w();
	if (y1 > gfx_h()) y1 = gfx_h();

	int nbox = (x1 - x0) * (y1 - y0);
	uint32_t *full = (uint32_t*)malloc((size_t)nbox * 4);
	uint32_t *shot = (uint32_t*)malloc((size_t)nbox * 4);
	check(full && shot, "buffers for two shots of the same instant");

	if (full && shot)
	{
		rip_pie_shot(2000, 2000, x0, y0, x1, y1, full);

		/*
		  The disc's own pixel count, which is what a fraction is a fraction of. Taken as the
		  pixels the pie at zero changes: at nothing copied every pixel of the disc is dimmed
		  and every pixel outside it is left exactly as it was - which is itself the check
		  that the pie does not spill onto the panel behind the disc.
		*/
		rip_pie_shot(0, 2000, x0, y0, x1, y1, shot);
		int all = rip_pie_diff(shot, full, nbox);

		printf("  the disc covers %d of the %d pixels in its box\n", all, nbox);
		check(all > nbox / 2, "at nothing copied the whole disc is dimmed");

		/*
		  And the corners are not, which is the panel behind it. A pie that darkened those -
		  the corners of the square buffer the rotation lives in, which disc_rot() fills with
		  the panel's own colour - would put a dark wedge across the plate, and it would only
		  show at some angles.
		*/
		int corner_changed = 0;
		for (int k = 0; k < 6; k++)
		{
			int xs[4] = { x0 + k, x1 - 1 - k, x0 + k, x1 - 1 - k };
			int ys[4] = { y0 + k, y0 + k, y1 - 1 - k, y1 - 1 - k };
			for (int q = 0; q < 4; q++)
			{
				int i = (ys[q] - y0) * (x1 - x0) + (xs[q] - x0);
				if (shot[i] != full[i]) corner_changed++;
				(void)xs; (void)ys;
			}
		}
		check(corner_changed == 0,
			"and the panel showing through the corners of the disc's box is untouched by it");

		/*
		  Then the fractions. The pie fills clockwise from twelve o'clock, so a quarter copied
		  leaves three quarters dimmed - and the arithmetic is exact at the quadrants whatever
		  the arctangent's precision, which is why these are the three values pinned.

		  A tolerance of a fortieth of the disc rather than a pixel: the rim and the hub are
		  anti-aliased, and the wedge's edge crosses pixels that are a blend of two bands,
		  some of which the dim leaves within rounding of where they already were.
		*/
		static const int frac[] = { 25, 50, 75 };
		for (unsigned k = 0; k < sizeof(frac) / sizeof(frac[0]); k++)
		{
			rip_pie_shot(2000 * frac[k] / 100, 2000, x0, y0, x1, y1, shot);
			int dim = rip_pie_diff(shot, full, nbox);

			int want = all * (100 - frac[k]) / 100;
			int slack = all / 40;

			char what[160];
			snprintf(what, sizeof(what),
				"%d%% copied leaves %d%% of the disc dimmed (%d of %d, wanted about %d)",
				frac[k], 100 - frac[k], dim, all, want);
			check(dim >= want - slack && dim <= want + slack, what);

			snprintf(what, sizeof(what), "rip-05-pie-%d", frac[k]);
			dump(what);
		}

		free(full);
		free(shot);
	}

	/* ------------------------------------------ the rectangle the pie has to fit in --- */

	/*
	  The invariant the DISC_BADGE_CELLS comment is about: a frame drawn by the partial path
	  has to be byte-identical to a full repaint of the same instant, and it can only be if
	  everything drawn lands inside the rectangle the previous frame recorded.

	  The pie is drawn inside the disc's own radius and adds nothing outside it, which is the
	  reason there is no ring around the disc - see the report. This check is what would fail
	  if one were added without growing disc_note_rect()'s cell count to cover it.
	*/
	/*
	  A full repaint at this progress first, and the reason is worth writing down because the
	  first version of this check failed on it.

	  The percentage under the disc is drawn *outside* the disc's rectangle, so a partial
	  repaint does not touch it - and it must not need to. In the firmware that is handled by
	  chome_handle(): rip_poll() folds the state, the percentage and the unreadable count into
	  one number and marks dirty when it changes, so the line under the disc is only ever
	  redrawn by a full frame. rip_test_set() writes those fields without going through
	  rip_poll(), so the test has to ask for that full repaint itself; without it the partial
	  frame carries a percentage from several frames ago and the comparison measures the stale
	  text rather than the pie.
	*/
	rip_test_set(RIP_RUNNING, 900, 2000, 0);
	chome_handle(KEY_ESC);
	chome_handle(KEY_ESC | UPSTROKE);
	chome_handle(KEY_ENTER);
	chome_handle(KEY_ENTER | UPSTROKE);

	harness_advance(60);
	chome_handle(0);                                  // one spin frame, the partial path
	check(gfx_damage_rows() < gfx_h(),
		"the frame under comparison was drawn by the partial path");
	unsigned long partial = harness_fb_hash_box(0, 0, gfx_w(), gfx_h());

	chome_handle(KEY_ESC);                            // out and back, a full repaint...
	chome_handle(KEY_ESC | UPSTROKE);
	chome_handle(KEY_ENTER);                          // ...at the same instant, clock unmoved
	chome_handle(KEY_ENTER | UPSTROKE);
	check(chome_screen_id() == S_DISC, "back on the rip screen without the clock moving");
	check(harness_fb_hash_box(0, 0, gfx_w(), gfx_h()) == partial,
		"a partial repaint of a rip in progress is byte-identical to a full one of the same instant");

	/* ---------------------------------------------------------------- stopping it --- */

	check(rip_busy(), "the rip is still running");
	press(KEY_ENTER, 8);
	check(rip_busy(), "one press on Stop does not stop it");
	dump("rip-06-armed");

	press(KEY_ENTER, 8);
	check(!rip_busy(), "the second one does");
	check(rip_state()->state == RIP_CANCELLED, "and it is recorded as cancelled, not failed");
	check(rip_reportable(), "with something still to say to the player");
	dump("rip-07-stopped");

	// And the arming expires rather than leaving the console one press from cancelling.
	rip_show(RIP_RUNNING, 400, 2000, 0);
	press(KEY_ENTER, 8);
	check(rip_busy(), "the arming is up again");
	harness_advance(4000);
	frame(4);
	press(KEY_ENTER, 8);
	check(rip_busy(), "and a press after it has expired only arms it again rather than stopping");
	press(KEY_ENTER, 8);
	check(!rip_busy(), "two in a row still stop it");

	/* -------------------------------------------------- leaving it running --- */

	/*
	  B leaves and the copy carries on, because the helper is a process of its own. The badge
	  in the corner is what says so and what leads back - and it exists while a rip runs even
	  though the drive reports no disc, which is the whole of disc_or_rip_present().
	*/
	rip_show(RIP_RUNNING, 800, 2000, 0);
	disc_reset_reader();
	disc_ingest_present(0);                           // the drive is the rip helper's now
	(void)disc_take_dirty();
	frame(6);

	check(chome_screen_id() == S_DISC,
		"a running rip is not thrown off its own screen when the drive reports no disc");

	press(KEY_ESC, 8);
	check(chome_screen_id() == S_DISCBAR,
		"B leaves the copy running and lands on the badge, which is the way back to it");
	check(rip_busy(), "and the copy is indeed still running");
	dump("rip-08-badge");

	press(KEY_ENTER, 8);
	check(chome_screen_id() == S_DISC, "and the badge opens the progress screen again");

	/* ------------------------------------------------- what a finished one says --- */

	rip_show(RIP_DONE, 2000, 2000, 0);
	check(!rip_busy() && rip_reportable(), "a finished rip waits to be acknowledged");
	dump("rip-09-done");

	/*
	  And the case this whole path exists for. A rip with unreadable sectors must not report
	  "Done": the player has to be told the copy is imperfect here, not by a core that hangs
	  weeks later.
	*/
	rip_show(RIP_DONE, 2000, 2000, 12);
	dump("rip-10-imperfect");

	rip_show(RIP_NOSPACE, 0, 2000, 0);
	dump("rip-11-nospace");

	press(KEY_ENTER, 8);
	check(!rip_reportable(), "OK dismisses it");

	/* ------------------------------------------ where a Mega CD copy actually goes --- */

	/*
	  The folder a copy lands in is not always the folder of the console the dialog settled
	  on, and this is the check for the case where it is not.

	  A Mega CD disc plays on the "md" row - Mega Drive is where a player looks for Sega, and
	  that row's launch hands the disc to the separate MegaCD core. But games/Genesis is a
	  Mega Drive folder, `md` accepts no .cue, and a copy written there was correct and
	  invisible: no card, and nothing on screen to say why. games/MegaCD is a shelf system of
	  its own now, so that is where the copy goes and where the card comes from.

	  Asserted on the folder rather than on the row's wording because the folder is the thing
	  that decides whether a card appears. rip_target::dest and rip_dest_sys().
	*/
	rip_test_reset();
	disc_reset_reader();
	disc_ingest_present(0);
	(void)disc_take_dirty();
	chome_leave();
	press(KEY_MENU, 20);
	frame(10);

	disc_ingest_present(1);
	fake_disc dmc; memset(&dmc, 0, sizeof(dmc));
	fake_put(&dmc, 0, 0, "SEGADISCSYSTEM", 14, 0);
	disc_set_reader(fake_read, &dmc);
	disc_ingest_identify(0);
	frame(6);
	check(disc_type() == DISC_T_MEGACD, "a Mega CD disc is in the drive");

	press(KEY_UP);
	press(KEY_ENTER);
	check(chome_screen_id() == S_DISC, "and its dialog is up");

	press(KEY_RIGHT);
	press(KEY_ENTER);
	hold_dir(KEY_DOWN, 12, 8);                         // to the copy row; see rip-01 above
	dump("rip-12-megacd-options");

	press(KEY_ENTER, 8);
	check(rip_test_starts() == 1, "the copy row starts a rip for a Mega CD disc too");
	check(!strcmp(rip_test_last_dir(), ROOT "/games/MegaCD"),
		"into games/MegaCD, which is the shelf system that reads .cue and launches the "
		"MegaCD core - not games/Genesis, where the card would never have appeared");
	check(rip_test_last_mode1() == 1,
		"and told MODE1 only, which is all megacdd's parser reads");

	/* ------------------------------- a disc we cannot play, and can still copy --- */

	/*
	  The Saturn disc, which is the case this screen used to get wrong.

	  The owner put SEGARALLY CHAMPIONSHIP in the drive. It was identified - the log said so
	  - and the dialog offered no way to copy it, because the copy row was derived from the
	  console the *Play* rows had settled on and there is no Play row for Saturn. Those are
	  different questions: no core here can be handed the spinning drive, and none needs to
	  be, because the copy is our own helper reading sectors into games/Saturn where the
	  Saturn core reads them back off the card.

	  So both halves are asserted together, and the order matters - the refusal first, so a
	  Copy row that only appeared because something had quietly started claiming Saturn could
	  play would fail here rather than pass.
	*/
	rip_test_reset();
	disc_reset_reader();
	disc_ingest_present(0);
	(void)disc_take_dirty();
	chome_leave();
	press(KEY_MENU, 20);
	frame(10);

	disc_ingest_present(1);
	fake_disc dsa; memset(&dsa, 0, sizeof(dsa));
	fake_put(&dsa, 0, 0, "SEGA SEGASATURN", 15, 0);
	disc_set_reader(fake_read, &dsa);
	disc_ingest_identify(0);
	frame(6);
	check(disc_type() == DISC_T_SATURN, "a Saturn disc is in the drive, and identified");

	press(KEY_UP);
	press(KEY_ENTER);
	check(chome_screen_id() == S_DISC, "and its dialog is up");
	dump("rip-13-saturn");

	/*
	  Play, pressed on purpose.

	  LEFT first, and it is not decoration: disc_dlg_enter() opens the cursor on the first
	  button that is NOT dim, so over a Saturn disc the dialog comes up on Options - see
	  rip-13-saturn.png - and a bare press here would open the Options list and prove
	  nothing about Play at all. A *held* LEFT walks to button 0 and stops there, so after it
	  the cursor is on Play whichever button it opened on, and nothing else has been
	  pressed that could have opened anything. A single tap would not do it any more: the
	  buttons wrap at their ends now, so a tap on a dialog that opened on Play would take the
	  cursor to the far end of the row - see the disc case of move_h().
	*/
	harness_clear_launch();
	hold_dir(KEY_LEFT, 8, 12);
	press(KEY_ENTER, 4);
	frame(80);
	check(harness_last_launch()[0] == 0,
		"Play refuses it, because saturncdd.cpp cannot read the drive");
	check(chome_screen_id() == S_DISC, "and the dialog stays put rather than pretending");

	press(KEY_RIGHT);
	press(KEY_ENTER);
	hold_dir(KEY_DOWN, 12, 8);                         // to the copy row; see rip-01 above
	dump("rip-14-saturn-options");

	press(KEY_ENTER, 8);
	check(rip_test_starts() == 1, "and the copy row is there anyway, and starts a rip");
	check(!strcmp(rip_test_last_dir(), ROOT "/games/Saturn"),
		"into games/Saturn, the shelf system that reads .cue and launches the Saturn core");
	check(rip_test_last_mode1() == 0,
		"and told the measured mode may be written, which is what saturncdd.cpp parses");

	/* -------------------------- ...and a disc with nowhere to put a copy, still not --- */

	/*
	  The other half of the rule, which widening the first one must not have broken. An
	  MSU-1 SNES disc is identified and its console IS a shelf system, so the loosened test
	  would offer a copy if it asked nothing more - but the sfc the core wants is a file on
	  the disc, not a folder of tracks, and a cue sheet in games/SNES is a folder that never
	  loads. There is no snes row in rip_targets and so there is no Copy row.

	  Pressed on the LAST row of the Options list, which is the row the two blocks above
	  used to start a rip. Here it is the "(not yet)" snes row, and it starts nothing and
	  launches nothing.
	*/
	rip_test_reset();
	disc_reset_reader();
	disc_ingest_present(0);
	(void)disc_take_dirty();
	chome_leave();
	press(KEY_MENU, 20);
	frame(10);

	disc_ingest_present(1);
	fake_disc dsn; memset(&dsn, 0, sizeof(dsn));
	static const char *const sfc_only[] = { "GAME.SFC;1" };
	fake_iso(&dsn, 0, "MSU1", 0, sfc_only, 1);
	disc_set_reader(fake_read, &dsn);
	disc_ingest_identify(0);
	frame(6);
	check(disc_type() == DISC_T_SNES, "an MSU-1 SNES disc is in the drive");

	press(KEY_UP);
	press(KEY_ENTER);
	check(chome_screen_id() == S_DISC, "and its dialog is up");

	press(KEY_RIGHT);
	press(KEY_ENTER);
	hold_dir(KEY_DOWN, 12, 8);                         // to the copy row; see rip-01 above
	dump("rip-15-msu1-options");

	harness_clear_launch();
	press(KEY_ENTER, 8);
	frame(20);
	check(rip_test_starts() == 0,
		"a disc whose console has no cue reader gets no copy row, so the last row starts nothing");
	check(harness_last_launch()[0] == 0, "and launches nothing either");
	check(chome_screen_id() == S_DISC, "the list stays up rather than pretending");
	press(KEY_ESC);

	/* ------------------------------------------------------------------ tidy up --- */

	rip_test_reset();
	disc_reset_reader();
	disc_ingest_present(0);
	(void)disc_take_dirty();
	frame(6);
	cfg.classicui_disc = 0;

	cfg.classicui_profile = was_prof;
	harness_set_fb(was_w, was_h);
	frame(6);
}

/*
  The boot-time configuration check - support/classicui/chome_cfgrec.h.

  Driven through the recording calls themselves rather than through a fake ini, and that
  is the point rather than a shortcut: those three functions are exactly what cfg.cpp's
  parser calls, in the order it calls them, so a record built here is the record a real
  file produces. Building one from a parser of the harness's own would prove that the
  second parser agrees with the analysis and say nothing at all about the first.

  Everything below asserts the *wording*. The product here is a paragraph read by
  somebody who has already lost an evening, on a card in a PC, with no idea what a
  section is; "1 problem found" would be a check that passes while the file remains
  useless. So the checks are for the sentences.
*/
static char cc_buf[8192];

static int cc_has(const char *s) { return strstr(cc_buf, s) != 0; }

static void cc_build()
{
	cfgrec_report(cc_buf, sizeof(cc_buf));
}

static void assert_config_check()
{
	printf("\n== the boot-time configuration check ==\n");

	/* --------------------------------------------------- a config with nothing wrong --- */

	{
		cfgrec_begin("MiSTer.ini", "1280x720@60.0", "MENU");
		cfgrec_section("MiSTer", 1);
		cfgrec_line("classicui=1", 4, 1);
		cfgrec_line("classicui_disc=1", 5, 1);
		cfgrec_line("video_mode=8", 6, 1);          // not ours: dropped without a word
		cfg.classicui = 1;
		cc_build();

		check(cfgrec_problems() == 0, "a clean config has nothing to report");
		check(cc_has("PROBLEMS: none"), "and the report says so in the heading");
		check(cc_has("Every classicui setting was read from [MiSTer], spelled correctly and set"),
			"and in a sentence under it, so the heading is not the only place to look");
		check(!cc_has("SKIPPED"), "with nothing marked skipped");
		check(cc_has("  line     4  [MiSTer]") && cc_has("classicui=1  read"),
			"the file-order list gives the line, the section, the assignment and the outcome");
		check(!cc_has("video_mode"),
			"an upstream option is not listed at all - this check has no opinion about those");
		check(cc_has("Only classicui* settings and debug are checked."),
			"and the report says out loud that it looked at nothing else");
	}

	/* ------------------------------------------------------------------- the header --- */

	{
		check(cc_has("ini file read    : MiSTer.ini"), "the header names the ini that was read");
		check(cc_has("parsed for core  : MENU"), "and the core it was parsed for");
		check(cc_has("[video=...] matched against : 1280x720@60.0"),
			"and what a video section would have been matched against");
		check(!cc_has("0x0 means no core video"),
			"with no note about zeros when there was a real mode");

		cfgrec_begin("MiSTer_Alt_1.ini", "0x0@0.0", "MENU");
		cfgrec_section("MiSTer", 1);
		cfgrec_line("classicui=1", 4, 1);
		cc_build();

		check(cc_has("ini file read    : MiSTer_Alt_1.ini"),
			"an alt ini is named as the alt ini, because altcfg() decides which file this is about");
		check(cc_has("0x0 means no core video had been measured yet, so on this pass no"),
			"and a zero video mode is explained rather than printed and left");
	}

	/* -------------------------------------------------- a key in a core section --- */

	{
		cfgrec_begin("MiSTer.ini", "1280x720@60.0", "MENU");
		cfgrec_section("MiSTer", 1);
		cfgrec_line("classicui_caps=1", 4, 1);
		cfgrec_section("Gameboy", 0);
		cfgrec_line("classicui=1", 41, 0);
		cc_build();

		check(cfgrec_problems() == 1, "a classicui key in a core section is one problem");
		check(cc_has("1. classicui=1  -  line 41, section [Gameboy]"),
			"named with its value, its line and the section it was found in");
		check(cc_has("Read from a core section, so it applies only while that core is loaded."),
			"and told what a core section does");
		check(cc_has("The shelf is the menu core, so the front-end never sees this setting."),
			"and why that means the front-end never gets it");
		check(cc_has("It was NOT read: this pass skipped that section entirely, so the value"),
			"and that this particular line was skipped, not merely could be");
		check(cc_has("Fix: move the line into the [MiSTer] section at the top of MiSTer.ini."),
			"and what to do about it, in one sentence with the section named");
		check(cc_has("  line    41  [Gameboy]") && cc_has("classicui=1  SKIPPED"),
			"with the same line marked SKIPPED in the file-order list");
		check(cc_has("  line     4  [MiSTer]") && cc_has("classicui_caps=1  read"),
			"and the [MiSTer] line beside it marked read, so the two can be compared");
	}

	/* ----------------------------------------------- the same key, section applied --- */

	{
		cfgrec_begin("MiSTer.ini", "1280x720@60.0", "Gameboy");
		cfgrec_section("Gameboy", 1);
		cfgrec_line("classicui=1", 41, 1);
		cc_build();

		check(cfgrec_problems() == 1,
			"a core-section key is still a problem when that core is the one running");
		check(cc_has("It WAS read this time, because this pass parsed for that section."),
			"but the report says it was read, rather than claiming a skip that did not happen");
		check(!cc_has("It was NOT read"), "and does not say both");
	}

	/* --------------------------------------------------------- a [video=...] key --- */

	{
		cfgrec_begin("MiSTer.ini", "0x0@0.0", "MENU");
		cfgrec_section("video=1920x1080", 0);
		cfgrec_line("classicui=1", 84, 0);
		cc_build();

		check(cfgrec_problems() == 1, "a classicui key in a video section is one problem");
		check(cc_has("1. classicui=1  -  line 84, section [video=1920x1080]"),
			"named with the video section it was found in");
		check(cc_has("live inside") && cc_has("games and dead in the menu"),
			"and told the failure mode in those words: live inside games, dead in the menu");
		check(cc_has("MiSTer.ini is re-read on a video change only"),
			"with the reason - the ini is only re-read on a video change");
		check(cc_has("while a core is running, never in the menu"),
			"and never in the menu, which is where the front-end lives");
		check(cc_has("matched against the resolution the CORE is"),
			"and that the match is on the core's output, not on what the television is doing");
		check(cc_has("Fix: move the line into the [MiSTer] section at the top of MiSTer.ini."),
			"and the same one-sentence fix");
	}

	/*
	  A malformed video header. ini_get_section() compares only as far as the '=', so the
	  parser treats this as a video section - and so must the report, or the one header
	  most likely to be typed by hand would get the wrong explanation.
	*/
	{
		cfgrec_begin("MiSTer.ini", "0x0@0.0", "MENU");
		cfgrec_section("vid=1920x1080", 0);
		cfgrec_line("classicui=1", 84, 0);
		cc_build();

		check(cc_has("games and dead in the menu"),
			"[vid=...] gets the video explanation, because that is what the parser calls it");
	}

	/*
	  Several keys under one bad header, which is the common shape of this mistake:
	  somebody pastes a block in. The explanation is given once and pointed at after
	  that - a page that says the same five lines four times is a page nobody finishes.
	*/
	{
		cfgrec_begin("MiSTer.ini", "0x0@0.0", "MENU");
		cfgrec_section("video=1920x1080", 0);
		cfgrec_line("classicui=1", 84, 0);
		cfgrec_line("classicui_profile=2", 85, 0);
		cfgrec_line("classicui_caps=0", 86, 0);
		cc_build();

		check(cfgrec_problems() == 3, "three keys under one bad header are three problems");

		const char *para = "A [video=...] section is matched against the resolution";
		const char *first = strstr(cc_buf, para);
		check(first && !strstr(first + 1, para),
			"but the long explanation is written once, not once per key");
		check(cc_has("The same [video=1920x1080] section as problem 1: live inside games,"),
			"the second key points back at the first by number, and repeats the failure mode");
		check(cc_has("It was NOT read either."),
			"and says it was skipped too, without repeating why a skip matters");
		check(cc_has("2. classicui_profile=2  -  line 85, section [video=1920x1080]") &&
			cc_has("3. classicui_caps=0  -  line 86, section [video=1920x1080]"),
			"while every one of them still gets its own numbered entry with its own line");
	}

	/* ------------------------------------------------- a key above every section --- */

	{
		cfgrec_begin("MiSTer.ini", "1280x720@60.0", "MENU");
		cfgrec_line("classicui=1", 1, 0);
		cfgrec_section("MiSTer", 1);
		cc_build();

		check(cfgrec_problems() == 1, "a key above the first section header is one problem");
		check(cc_has("1. classicui=1  -  line 1, before any [section] header"),
			"and is placed by saying there is no section rather than by naming one");
		check(cc_has("[section] header is read for no core at all."),
			"and told that such a line is read for no core at all");
		check(cc_has("Fix: put a line reading [MiSTer] above it."),
			"with a different fix, because moving it is not what this one needs");
		check(cc_has("  line     1  (no section)"),
			"and the file-order list says (no section) rather than an empty pair of brackets");
	}

	/* ------------------------------------------------------------------ duplicates --- */

	{
		cfgrec_begin("MiSTer.ini", "1280x720@60.0", "MENU");
		cfgrec_section("MiSTer", 1);
		cfgrec_line("classicui_profile=1", 5, 1);
		cfgrec_line("classicui_profile=2", 9, 1);
		cc_build();

		check(cfgrec_problems() == 1, "the same key twice is one problem, not two");
		check(cc_has("1. classicui_profile is assigned 2 times  -  lines 5, 9"),
			"reported once, with every line it appears on");
		check(cc_has("The last assignment the parser reaches wins, silently"),
			"and told which one the parser keeps");
		check(cc_has("Line 9 won, with classicui_profile=2."),
			"and which line that is here, with the value it won with");
	}

	{
		cfgrec_begin("MiSTer.ini", "1280x720@60.0", "MENU");
		cfgrec_section("Gameboy", 0);
		cfgrec_line("classicui_profile=1", 5, 0);
		cfgrec_line("classicui_profile=2", 9, 0);
		cc_build();

		check(cc_has("None of them won: every one is in a section this pass skipped, so the"),
			"and when every copy was skipped it says none of them won, rather than naming a winner");
	}

	/* ------------------------------------------------------------- an unknown key --- */

	{
		cfgrec_begin("MiSTer.ini", "1280x720@60.0", "MENU");
		cfgrec_section("MiSTer", 1);
		cfgrec_line("classicui_dsic=1", 12, 1);
		cc_build();

		check(cfgrec_problems() == 1, "a misspelled classicui key is one problem");
		check(cc_has("1. classicui_dsic=1  -  line 12, section [MiSTer]"),
			"named with the spelling that was actually in the file");
		check(cc_has("Not an option this firmware has."),
			"and told it is not an option this firmware has");
		check(cc_has("recognise without a word, so a typo looks exactly like a setting that does"),
			"and why nothing said so at the time");
		check(cc_has("Fix: check the spelling against the resolved values at the bottom of this"),
			"and pointed at the list of real ones in this same file");
	}

	{
		cfgrec_begin("MiSTer.ini", "1280x720@60.0", "MENU");
		cfgrec_section("Gameboy", 0);
		cfgrec_line("classicui_dsic=1", 12, 0);
		cc_build();

		check(cfgrec_problems() == 1,
			"a misspelled key in the wrong section is one problem, because one line gets one fix");
		check(cc_has("And even spelled correctly it would not be read here: [Gameboy] is"),
			"and both faults are said in that one entry, not split across two");
		check(cc_has("this file, and move the line into the [MiSTer] section at the top."),
			"with a fix that does both, so doing only the half you read first is not possible");
		check(!cc_has("Read from a core section, so it applies only while that core is loaded."),
			"and it is not also reported as a placement problem - the fix there would be to "
			"carefully relocate a line that does nothing wherever it goes");
	}

	/* ----------------------------------------------------------- resolved values --- */

	{
		uint8_t was_ss = cfg.classicui_screenscraper;
		char was_pass[64];
		snprintf(was_pass, sizeof(was_pass), "%s", cfg.classicui_ss_pass);

		cfg.classicui = 1;
		cfg.classicui_overscan = 6;
		cfg.classicui_screenscraper = 1;
		snprintf(cfg.classicui_ss_pass, sizeof(cfg.classicui_ss_pass), "hunter2");

		cfgrec_begin("MiSTer.ini", "1280x720@60.0", "MENU");
		cfgrec_section("MiSTer", 1);
		cfgrec_line("classicui=1", 4, 1);
		cc_build();

		check(cc_has("RESOLVED VALUES - what the front-end is really using"),
			"the report ends with what every option actually resolved to");
		check(cc_has("  CLASSICUI=1"), "including the switch the whole front-end hangs off");
		check(cc_has("  CLASSICUI_OVERSCAN=6"), "and the ones nobody wrote a line for");
		check(cc_has("  CLASSICUI_SCREENSCRAPER=1"), "and the ones somebody did");
		check(cc_has("  CLASSICUI_SS_PASS=***") && !cc_has("hunter2"),
			"and the ScreenScraper password as three stars - this file gets posted to forums");
		check(cc_has("  DEBUG="),
			"and debug, because it is checked here for the same reason it cannot be trusted to log");

		cfg.classicui_screenscraper = was_ss;
		snprintf(cfg.classicui_ss_pass, sizeof(cfg.classicui_ss_pass), "%s", was_pass);
	}

	/* ------------------------------------------------------------------ and debug --- */

	{
		cfgrec_begin("MiSTer.ini", "1280x720@60.0", "MENU");
		cfgrec_section("Gameboy", 0);
		cfgrec_line("debug=2", 77, 0);
		cc_build();

		check(cfgrec_problems() == 1, "debug in the wrong section is a problem too");
		check(cc_has("And debug is the setting that hides its own failure:"),
			"and is the one key that gets a second paragraph");
		check(cc_has("no log to look in. That is why this report is a file on the card and not"),
			"which says why this report is a file rather than a line in the log it would have written");
	}

	{
		cfgrec_begin("MiSTer.ini", "1280x720@60.0", "MENU");
		cfgrec_section("MiSTer", 1);
		cfgrec_line("debug=2", 3, 1);
		cc_build();

		check(cfgrec_problems() == 0, "and debug in [MiSTer] is not");
	}

	/* ------------------------------------------------------------- an empty file --- */

	{
		cfgrec_begin("MiSTer.ini", "1280x720@60.0", "MENU");
		cc_build();

		check(cfgrec_problems() == 0, "an ini with no classicui line in it reports no problem");
		check(cc_has("(none - the file has no classicui setting in it at all)"),
			"and says the file had none, which is the answer for a card that was never set up");
	}

	/* ---------------------------------------------------------------- the ceiling --- */

	{
		cfgrec_begin("MiSTer.ini", "1280x720@60.0", "MENU");
		cfgrec_section("MiSTer", 1);
		for (int i = 0; i < CFGREC_MAX + 5; i++) cfgrec_line("classicui_caps=1", i + 1, 1);
		cc_build();

		check(cc_has("classicui/debug lines seen  : 37"),
			"every line is counted even past the cap, so the total is the file's and not the buffer's");
		check(cc_has("5 of them were past this check's 32-line limit and are NOT below."),
			"and the ones that did not fit are said out loud - a cut record must not read as a clean one");
	}

	/* ------------------------------------------------------- the file on the card --- */

	{
		cfgrec_begin("MiSTer.ini", "1280x720@60.0", "MENU");
		cfgrec_section("Gameboy", 0);
		cfgrec_line("classicui=1", 41, 0);
		cc_build();

		cfgrec_write_report();

		check(!strcmp(cfgrec_report_path(), ROOT "/classicui/config-report.txt"),
			"the report goes to classicui/config-report.txt on the card");

		static char onfile[8192];
		int n = slurp_file(cfgrec_report_path(), onfile, sizeof(onfile));
		check(n > 0, "and is really there after a boot with a broken config");
		check(n > 0 && !strcmp(onfile, cc_buf), "byte for byte the report the analysis built");
		check(strstr(onfile, "Written on every boot, whether or not the front-end is switched on, and without") != 0,
			"and it opens by saying it is written whether or not the front-end is on");
		check(strstr(onfile, "consulting debug=") != 0,
			"and that it did not consult debug, which is the reason it exists");
	}

	/* ------------------------------------------------- and never the player's password --- */

	/*
	  Read off a real card before this section existed: the resolved-values table printed
	  `***` while the file-order table three lines above it printed the password in full.
	  Both are printers of the same record, and only one of them asked ini_loggable().

	  This file is written to the card on every boot and is the file we ask people to send
	  us when something is wrong, so a password in it travels further than one in a log.
	  The check is on the *whole report* rather than on either table, because the next
	  printer somebody adds will be a third place to forget.
	*/
	{
		cfgrec_begin("MiSTer.ini", "1280x720@60.0", "MENU");
		cfgrec_section("MiSTer", 1);
		cfgrec_line("classicui_ss_user=dune", 5, 1);
		cfgrec_line("classicui_ss_pass=hunt3rSecret", 6, 1);
		cc_build();

		check(strstr(cc_buf, "hunt3rSecret") == 0,
			"the password is nowhere in the report, in any of its tables");
		check(strstr(cc_buf, "classicui_ss_pass=***") != 0,
			"the line is still listed, with the value replaced rather than the line dropped");
		check(strstr(cc_buf, "classicui_ss_user=dune") != 0,
			"while the login, which is not a secret, is still printed as itself");
	}

	/* ------------------------------------------------------------ and on the screen --- */

	{
		int was_prof = cfg.classicui_profile;

		cfg.classicui_profile = 0;
		harness_set_fb(1280, 720);
		gfx_shutdown();
		theme_update(1280, 720, 0);
		frame(8);

		// Clean first, so the amber below is the difference and not the decor. The record
		// is set before the panel opens, so nothing here needs a forced repaint.
		cfgrec_begin("MiSTer.ini", "1280x720@60.0", "MENU");
		cfgrec_section("MiSTer", 1);
		cfgrec_line("classicui=1", 4, 1);

		opt_open();
		frame(6);
		check(opt_foot_ink_rows(COL_YELLOW) == 0,
			"a machine with nothing wrong is told nothing: the Options panel has no notice on it");

		unsigned long clean = harness_fb_hash(0, 720);

		/*
		  Back to the shelf, and then *only* down if that has not already got there.

		  The down was unconditional and the section passed for months, on an accident: it
		  is there to come down off the menu bar, and it happened to be harmless on the
		  shelf because the cursor happened to be parked on a folder, where down does
		  nothing (see the SCR_HOME arm of the up/down handler - down over a *game* opens
		  the suspend strip and the check below then fails, complaining about a
		  configuration notice).

		  Sizing the library scan's slice is what found it. That changed how many frames a
		  scan spends, which changed how far several sections above this one get, which
		  moved the cursor - and the failure arrived here, eight thousand lines from
		  anything to do with either. Made conditional rather than re-parked, because
		  "get back to the shelf" is what the two lines are for and neither of them should
		  care what is under the cursor when they run.
		*/
		for (int i = 0; i < 4 && chome_screen_id() != 0; i++) press(KEY_ESC, 10);
		if (chome_screen_id() != 0) press(KEY_DOWN, 14);
		frame(6);

		cfgrec_begin("MiSTer.ini", "1280x720@60.0", "MENU");
		cfgrec_section("Gameboy", 0);
		cfgrec_line("classicui=1", 41, 0);

		/*
		  Not on the shelf, and this is the design rule in chome_ini.h rather than an
		  omission: a panel of technical text about a configuration file, over somebody's
		  cover art, before they have pressed anything, is the thing this front-end exists
		  to remove. So the notice is checked for on the shelf too, and its absence there
		  is the assertion.
		*/
		check(chome_screen_id() == 0, "the shelf is back up with a broken config recorded");
		check(opt_foot_ink_rows(COL_YELLOW) == 0, "and carries no notice of its own");

		opt_open();
		frame(6);

		check(harness_fb_hash(0, 720) != clean, "and a machine with a misplaced setting is told");
		check(opt_foot_ink_rows(COL_YELLOW) >= 12,
			"in amber at the foot of Options, on two lines - the count and the file name");

		dump("config-check-options");

		cfg.classicui_profile = (uint8_t)was_prof;
		frame(6);
	}

	/*
	  Left clean. Every other section draws the Options panel, and a record with a problem
	  in it would put a notice at the foot of all of them.
	*/
	cfgrec_begin("MiSTer.ini", "1280x720@60.0", "MENU");
	cfgrec_section("MiSTer", 1);
	cfgrec_line("classicui=1", 4, 1);
}

/* ===================================================================== marquee ===

  Text that does not fit, scrolled instead of only being cut, and marked with an ellipsis.

  Three things have to be true for this to be a feature rather than a liability, and each of
  them has failed somewhere in this front-end before:

    - It has to move. An animation whose only frames are the ones something else asked for
      is a still picture: the disc badge and the arriving cover art both landed correctly and
      sat there, and both were found by eye rather than here. So the checks below never
      accept a hash change on its own - they demand that the framebuffer was flipped as well
      as that the pixels differ, which is the grab_seq lesson in stubs.cpp arrived at from
      the drawing side.

    - It has to stop. A marquee on every clipped string on screen, running whether anybody
      is looking at it or not, is a full repaint five times a second on a DE10-Nano. So a
      string only scrolls where the caller says it has focus, and a resting screen with
      nothing focused-and-long on it must paint exactly as often as it did before this
      existed, which is not at all.

    - It has to be honest at rest. The window at offset 0 is byte-for-byte what gfx_clip()
      returned before any of this, so every pinned frame in this file that does not move the
      clock is unaffected by design rather than by luck.

  gfx_marquee() is tested before the screens are, because everything the screens do rests on
  it being a pure function of its arguments: two composes of one instant have to draw the
  same window, or the partial-repaint comparisons elsewhere in this file - which force a full
  repaint of a moment already on screen and demand byte identity - would start failing for a
  reason that has nothing to do with the disc.
*/

// The mark, as one character, so a test can look for it inside a returned buffer.
static int has_ellipsis(const char *s)
{
	for (const char *p = s; *p; p++) if (*p == CH_ELLIPSIS[0]) return 1;
	return 0;
}

/*
  How many pixels one string inks, drawn on a blank canvas at a given scale.

  The only way to answer "is this glyph legible at 240p" with a number rather than an
  opinion. gfx_begin()/gfx_end() around a direct draw is what the icon sheet further up this
  file does; the full-canvas fill is what makes the count trustworthy, since gfx_end() only
  copies rows something damaged.
*/
static int ink_of(const char *s, int scale)
{
	if (!gfx_begin()) return -1;
	gfx_fill(0, 0, gfx_w(), gfx_h(), COL_BLACK);
	gfx_text(s, 8, 8, scale, COL_WHITE, 0);
	gfx_end();
	return px_count(COL_WHITE);
}

/*
  A .pf that claims every code from 0 upwards, which is the case CH_ELLIPSIS's degradation
  story is about.

  make_pf() above is 768 bytes, and LoadFont() reads that size as "chars 32 upwards" - so it
  cannot reach code 5 and cannot test this. 1024 bytes with ink in the first 256 makes
  LoadFont() start at code 0 instead (charrom.cpp), and every glyph including code 5 becomes
  a solid six-column block. If the front-end were reading its ellipsis out of charfont[], the
  mark would become that block and every cut string in the front-end would end in a domino.
*/
static void make_pf_from_zero(const char *path)
{
	unsigned char buf[1024];
	for (int c = 0; c < 128; c++)
		for (int r = 0; r < 8; r++)
			buf[c * 8 + r] = (c == 32) ? 0x00 : 0xFC;

	FILE *f = fopen(path, "wb");
	if (!f) return;
	fwrite(buf, 1, sizeof(buf), f);
	fclose(f);
}

/*
  A file on the card with a name far longer than any panel, and the browser it shows up in.

  The vehicle for every on-screen check below, chosen after two others were tried and found
  wanting. An SSID cannot do it: NET_SSID is 33 bytes, and 32 characters still fit the Wi-Fi
  panel at 720p, so the row would simply not scroll and the test would pass by drawing
  nothing. The fixture's own game titles cannot do it either - the longest is 36 characters,
  which overruns a 240p title line by two, and a two-character marquee proves very little and
  photographs as nothing at all.

  A file name has no such ceiling, and a browser row is the widest text run in the front-end:
  the whole canvas less the inset, at 720p and at 240p both. It is also exactly the kind of
  string this feature is for - the end of a No-Intro name is where two dumps of one game
  differ, and it is the end that gets cut.

  Written into the Amiga folder, which is a *computer* system: those are excluded from the
  shelf and only ever reached through the browser, which lists its directory when it opens
  rather than from the library scan. So this file appears for exactly as long as the section
  needs it and nothing above or below has to be rescanned to make it go away.
*/
#define MARQ_LONG_NAME \
	"A Cracked Demo Disk With An Absurdly Long Name, For Testing (Europe) [a][!].adf"

static void marq_put_long_file()
{
	mkpath(ROOT "/games/Amiga");
	touch(ROOT "/games/Amiga", MARQ_LONG_NAME, 2048);
}

static void marq_drop_long_file()
{
	unlink(ROOT "/games/Amiga/" MARQ_LONG_NAME);
}

/*
  Open the browser on that folder: the Computers card, the one computer system behind it, and
  ENTER into its files.

  By label rather than by a count of cards, for the reason select_folder() exists - the row of
  leading folders grows as soon as a game has been played, and a hardcoded position would
  quietly open something else instead of failing.
*/
static int marq_open_browser()
{
	harness_set_menu_core(1);
	chome_leave();
	press(KEY_MENU, 20);
	for (int i = 0; i < 80 && lib_scanning(); i++) frame(2);
	frame(10);

	if (!select_folder("Computers")) return 0;
	press(KEY_ENTER, 14);                 // the Computers view: one card per computer system
	frame(8);
	press(KEY_ENTER, 14);                 // and into its files
	frame(8);
	return 1;
}

/*
  How far past the start of the marquee's cycle the clock is standing.

  marq_epoch moves on every mark_dirty(), so how far through the cycle a screen is after a
  press() depends on that press's settle count - and a test that hardcoded a number there
  would go intermittent the day somebody tuned one. Every duration below is measured against
  this instead of assumed.
*/
static unsigned long marq_since() { return harness_now() - chome_marq_epoch(); }

// What is left of the opening hold, less a margin - or 0 if the hold is already over, which
// is a caller's cue that there was nothing here to measure rather than a licence to measure
// the wrong thing.
static unsigned long marq_hold_left(unsigned long margin)
{
	unsigned long since = marq_since();
	if (since + margin >= GFX_MARQ_HOLD_MS) return 0;
	return GFX_MARQ_HOLD_MS - since - margin;
}

/*
  Let the clock run without pressing anything, and report whether the screen moved.

  Two answers rather than one, and both are needed. A hash change on its own would accept a
  repaint that redrew the same pixels somewhere else; a paint count on its own would accept a
  repaint that changed nothing. So `*paints` is how many times the framebuffer was flipped,
  the return value is whether the pixels in the band differ, and a marquee has to produce
  both while a still screen must produce neither.
*/
static int marq_run(unsigned long ms, int y0, int y1, int *paints, int *worst_rows)
{
	unsigned long before = harness_fb_hash(y0, y1);
	int flips = harness_present_count();
	int worst = 0;

	for (unsigned long t = 0; t < ms; t += 16)
	{
		harness_advance(16);
		chome_handle(0);
		if (harness_present_count() != flips && gfx_damage_rows() > worst) worst = gfx_damage_rows();
	}

	if (paints) *paints = harness_present_count() - flips;
	if (worst_rows) *worst_rows = worst;
	return harness_fb_hash(y0, y1) != before;
}

static void assert_marquee()
{
	printf("\n== marquee: text that does not fit scrolls, and the cut is an ellipsis ==\n");

	enum { S_BROWSE = 8 };
	const int was_prof = cfg.classicui_profile;

	/* ------------------------------------------------------- the mark --- */

	{
		const char *t = "Super Mario World 2 - Yoshi's Island";
		int narrow = gfx_text_w(t, 1) / 2;

		const char *cut = gfx_clip(t, 1, narrow);
		check(has_ellipsis(cut), "a string that does not fit is marked with the ellipsis");
		check(!strchr(cut, '>'), "and not with the '>' it used to be marked with");
		check(!strcmp(gfx_clip(t, 1, gfx_text_w(t, 1)), t),
			"a string that fits gains no mark at all");

		/*
		  Legible at scale 1, which is the profile this front-end is actually for: 240p on a
		  television. Three 2x2 dots is twelve pixels, the same ink as three of the ROM
		  font's own full stops, and the reason the mark is not three single pixels - one
		  pixel at 240p is a smudge, not punctuation. Asserted as the exact count, because
		  "some ink" would also pass for a mark that had lost two dots to a bad bit pattern.
		*/
		int ink1 = ink_of(CH_ELLIPSIS, 1);
		int ink2 = ink_of(CH_ELLIPSIS, 2);
		printf("  the ellipsis inks %d pixels at scale 1 and %d at scale 2\n", ink1, ink2);
		check(ink1 == 12, "the ellipsis is three 2x2 dots at scale 1 - punctuation, not three lit pixels");
		check(ink2 == 48, "and it scales with the text around it");
	}

	/*
	  And the mark under somebody else's font, which is the whole reason it lives in
	  extra_glyphs[] rather than in charrom. See CH_ELLIPSIS in chome_gfx.h.
	*/
	{
		static unsigned char before[256][8];
		memcpy(before, charfont, sizeof(before));

		mkpath(ROOT "/font");
		make_pf_from_zero(ROOT "/font/fromzero.pf");

		char rel[64];
		snprintf(rel, sizeof(rel), "font/fromzero.pf");
		check(LoadFont(rel) == 1, "a .pf that claims code 5 loads");
		check(memcmp(before, charfont, sizeof(before)) != 0, "and really replaced the glyph table");

		int ink1 = ink_of(CH_ELLIPSIS, 1);
		printf("  under a font that fills every cell, the ellipsis still inks %d pixels\n", ink1);
		check(ink1 == 12, "a loaded font cannot take the ellipsis away - it is ours, not charrom's");

		FontRestoreBuiltin();
		unlink(ROOT "/font/fromzero.pf");
		check(!memcmp(before, charfont, sizeof(before)),
			"and the built-in glyphs are back for every frame below this line");
	}

	/* ------------------------------------------------ gfx_marquee() --- */

	/*
	  The window, swept over a whole cycle at every tracking value and both text scales.

	  What is established here is the contract the screens rely on: the window is always
	  exactly as wide as the space, it starts as gfx_clip()'s answer, it walks the string one
	  character at a time without skipping or overshooting, it ends on the tail with no mark
	  on it, and it comes back to the beginning.
	*/
	{
		const char *t = "Flat 3 Upstairs Back Bedroom 5GHz Guest Network";
		int len = (int)strlen(t);
		int bad_width = 0, bad_order = 0, bad_first = 0, bad_last = 0, bad_pure = 0;
		int combos = 0, ends_seen = 0;

		for (int k = -2; k <= 2; k++)
		{
			cfg.classicui_tracking = (int8_t)k;
			for (int s = 1; s <= 2; s++)
			{
				// Two thirds of the room it wants, which cuts a third of the string off.
				int maxpx = gfx_text_w(t, s) * 2 / 3;
				int max = gfx_text_cols(maxpx, s);
				if (max < 2 || max >= len) continue;
				combos++;

				int steps = len - max;
				unsigned long cycle = GFX_MARQ_HOLD_MS
					+ (unsigned long)steps * GFX_MARQ_STEP_MS + GFX_MARQ_HOLD_MS;

				int sc = 0;
				unsigned long nx = 0;
				char zero[256];
				snprintf(zero, sizeof(zero), "%s", gfx_marquee(t, s, maxpx, 0, &sc, &nx));
				if (strcmp(zero, gfx_clip(t, s, maxpx))) bad_first++;
				if (!sc) bad_first++;

				int last_off = -1;
				for (unsigned long ms = 0; ms < cycle; ms += 20)
				{
					sc = 0;
					char keep[256];
					snprintf(keep, sizeof(keep), "%s", gfx_marquee(t, s, maxpx, ms, &sc, &nx));

					// Purity: the same instant twice is the same window.
					if (strcmp(gfx_marquee(t, s, maxpx, ms, &sc, &nx), keep)) bad_pure++;

					// Never wider than the space it was given, at any offset.
					if (gfx_text_w(keep, s) > maxpx) bad_width++;

					/*
					  Which offset this is, read off the string rather than trusted: the
					  window's first character is t[off], and the window is `max` cells at
					  every offset - so its body is max-1 while the mark is there and max
					  once the mark is gone.
					*/
					int off = -1;
					for (int o = 0; o <= steps; o++)
					{
						int body = (o < steps) ? max - 1 : max;
						if (!strncmp(keep, t + o, (size_t)body)) { off = o; break; }
					}
					if (off < 0) { bad_order++; continue; }

					if (off == steps)
					{
						ends_seen++;
						if (has_ellipsis(keep)) bad_last++;
						if (strcmp(keep, t + steps)) bad_last++;
					}
					else if (!has_ellipsis(keep)) bad_order++;

					// One character at a time, forwards only. The single backward step in a
					// cycle is the snap home at the end of it, which is past `cycle`.
					if (last_off >= 0 && off != last_off && off != last_off + 1) bad_order++;
					last_off = off;
				}

				// And back where it started, one whole cycle on.
				if (strcmp(gfx_marquee(t, s, maxpx, cycle, &sc, &nx), zero)) bad_order++;
			}
		}
		cfg.classicui_tracking = 0;

		printf("  %d width/scale combinations swept, %d instants ended on the tail\n",
			combos, ends_seen);
		check(combos >= 8, "there were combinations to sweep");
		check(!bad_first, "the window at offset 0 is byte-for-byte what gfx_clip() returns");
		check(!bad_width, "no offset ever draws wider than the space it was given");
		check(!bad_order, "the window walks the string one character at a time and returns to the start");
		check(ends_seen > 0 && !bad_last,
			"and the last offset is the tail of the string, with no mark on it");
		check(!bad_pure, "composing one instant twice gives the same window - gfx_marquee is pure");
	}

	/*
	  And `next_in`, which is the whole reason the repaint can be exact rather than a poll.

	  The claim is strong and worth checking as one: the window is the same at ms and at
	  ms + next_in - 1, and different at ms + next_in. A next_in that was merely a safe
	  under-estimate would pass a weaker test and would then repaint frames identical to the
	  one before them, which is the cost this design exists to avoid.
	*/
	{
		const char *t = "Flat 3 Upstairs Back Bedroom 5GHz Guest Network";
		int maxpx = gfx_text_w(t, 1) * 2 / 3;
		int early = 0, late = 0, n = 0;

		for (unsigned long ms = 0; ms < 12000; ms += 37)
		{
			int sc = 0;
			unsigned long nx = 0;
			char now[256];
			snprintf(now, sizeof(now), "%s", gfx_marquee(t, 1, maxpx, ms, &sc, &nx));
			if (!sc || !nx) continue;
			n++;

			int sc2 = 0;
			unsigned long nx2 = 0;
			if (strcmp(now, gfx_marquee(t, 1, maxpx, ms + nx - 1, &sc2, &nx2))) early++;
			if (!strcmp(now, gfx_marquee(t, 1, maxpx, ms + nx, &sc2, &nx2))) late++;
		}

		printf("  %d instants checked against their own next_in\n", n);
		check(n > 200, "there were instants to check");
		check(!early, "nothing changes before next_in says it will");
		check(!late, "and it does change exactly then - a marquee repaint is never a wasted frame");
	}

	/* ------------------------------------------- a focused row, both profiles --- */

	/*
	  The browser, at 720p and then at 240p, with one file name that overruns the row by
	  dozens of characters. Both profiles rather than one, because the two answers that matter
	  scale differently: how much of the name is missing scales with the canvas, and what a
	  repaint costs scales with it the other way.
	*/
	marq_put_long_file();

	struct { int w, h, force; const char *name; } canv[] = {
		{ 1280, 720, 1, "720p" },
		{  320, 240, 3, "240p" },
	};

	for (int c = 0; c < 2; c++)
	{
		cfg.classicui_profile = (uint8_t)canv[c].force;
		harness_set_fb(canv[c].w, canv[c].h);
		gfx_shutdown();
		theme_update(canv[c].w, canv[c].h, canv[c].force);

		char what[192];
		int paints = 0, rows = 0;

		snprintf(what, sizeof(what), "%s: the browser opens on the Amiga folder", canv[c].name);
		check(marq_open_browser() && chome_screen_id() == S_BROWSE, what);

		// After the screen is up, not before: gfx_shutdown() above leaves gfx_h() at 0 until
		// something composes a frame, and a band compared against rows 0..-1 compares nothing
		// at all - which passes for "still" and fails for "moved".
		int h = gfx_h();

		/*
		  Down onto the long name. The folder sorts before the files, so one press lands on
		  it - and row 0, the folder, is the short row this test needs afterwards.
		*/
		press(KEY_DOWN, 8);
		frame(4);

		snprintf(what, sizeof(what), "%s: with the cursor on the long name, it is scrolling", canv[c].name);
		check(chome_marq_live(), what);

		snprintf(what, sizeof(what), "marquee-browser-%s-rest", canv[c].name);
		dump(what);

		/*
		  The opening hold. Nothing may move for GFX_MARQ_HOLD_MS: a row that started
		  scrolling the instant it gained focus would never show its beginning, which is the
		  one part of a name a player is certain to want. Measured against the epoch and
		  stopped two steps short of it, because the frame the hold ends on is the frame the
		  first step lands on.
		*/
		unsigned long hold = marq_hold_left(2 * GFX_MARQ_STEP_MS);
		snprintf(what, sizeof(what), "%s: the press left room inside the hold to measure", canv[c].name);
		check(hold > 0, what);

		int moved = marq_run(hold, 0, h - 1, &paints, &rows);
		printf("  %s browser: %d paints during the opening hold\n", canv[c].name, paints);
		snprintf(what, sizeof(what),
			"%s: a newly focused row holds still long enough to read its beginning", canv[c].name);
		check(!moved && !paints, what);

		// And then it moves. Four steps' worth, so this cannot pass on one frame that
		// happened to be repainted for some other reason.
		moved = marq_run(5 * GFX_MARQ_STEP_MS, 0, h - 1, &paints, &rows);
		printf("  %s browser: %d paints once it starts, worst %d rows of %d\n",
			canv[c].name, paints, rows, h);

		snprintf(what, sizeof(what),
			"%s: the focused row's text really moves - the pixels differ, not only a hash", canv[c].name);
		check(moved, what);
		snprintf(what, sizeof(what),
			"%s: and it moved four times over four steps, so it is a scroll and not a stray frame",
			canv[c].name);
		check(paints >= 4, what);

		snprintf(what, sizeof(what), "marquee-browser-%s-mid", canv[c].name);
		dump(what);

		/*
		  Every one of those paints was a band, not a frame. This is the check the whole design
		  exists for: a marquee that repainted the screen would be five full repaints a second
		  for as long as a player left the cursor on a long name, which on a DE10-Nano at 1080p
		  is most of the loop.
		*/
		snprintf(what, sizeof(what),
			"%s: and each paint was a band of rows, not the whole screen", canv[c].name);
		check(rows > 0 && rows < h / 4, what);

		/*
		  Up onto the folder's short name. Nothing on screen is now both focused and too long,
		  so the screen has to go completely still - including the row that was scrolling a
		  moment ago, which is the half of "focus-driven" that is easy to get wrong.
		*/
		press(KEY_UP, 8);
		frame(4);
		snprintf(what, sizeof(what),
			"%s: with the cursor off it, nothing on the screen is scrolling", canv[c].name);
		check(!chome_marq_live(), what);

		snprintf(what, sizeof(what), "marquee-browser-%s-focus-left", canv[c].name);
		dump(what);

		moved = marq_run(2 * GFX_MARQ_HOLD_MS + 8 * GFX_MARQ_STEP_MS, 0, h - 1, &paints, &rows);
		printf("  %s browser: %d paints over two full holds with focus on a name that fits\n",
			canv[c].name, paints);
		snprintf(what, sizeof(what),
			"%s: moving the cursor off the long row stops it scrolling, and the screen goes still",
			canv[c].name);
		check(!moved && !paints, what);

		/*
		  And back onto the long name, which has to show its beginning again rather than
		  resuming where it had got to - then run a whole cycle and find that frame again.
		*/
		press(KEY_DOWN, 8);
		snprintf(what, sizeof(what),
			"%s: the press that regained focus restarted the cycle at its beginning", canv[c].name);
		check(marq_since() < GFX_MARQ_HOLD_MS, what);

		unsigned long rest = harness_fb_hash(0, h - 1);

		marq_run(GFX_MARQ_HOLD_MS + 4 * GFX_MARQ_STEP_MS, 0, h - 1, &paints, &rows);
		snprintf(what, sizeof(what), "%s: and mid-cycle it is somewhere else", canv[c].name);
		check(harness_fb_hash(0, h - 1) != rest, what);

		/*
		  One whole cycle of this particular string, worked out the way the front-end does it:
		  the characters that did not fit are the steps, and the cycle is the two holds plus
		  one step each. Three of them looked for, so a cycle whose length this recomputed
		  slightly differently still finds the frame.
		*/
		unsigned long cycle_ms;
		{
			const chome_profile *p = theme_get();
			char nm[192];
			snprintf(nm, sizeof(nm), "  %s", MARQ_LONG_NAME);
			gfx_shout(nm);
			int lost = (int)strlen(nm) - gfx_text_cols(p->w - p->inset * 2, p->ts_ui);
			if (lost < 1) lost = 1;
			cycle_ms = 2 * GFX_MARQ_HOLD_MS + (unsigned long)lost * GFX_MARQ_STEP_MS;
			printf("  %s browser: %d characters overrun the row, so a cycle is %lu ms\n",
				canv[c].name, lost, cycle_ms);
		}

		int came_home = 0;
		for (unsigned long t = 0; t < 3 * cycle_ms && !came_home; t += 16)
		{
			harness_advance(16);
			chome_handle(0);
			if (harness_fb_hash(0, h - 1) == rest) came_home = 1;
		}
		snprintf(what, sizeof(what),
			"%s: the scroll returns to the start - the resting frame comes back", canv[c].name);
		check(came_home, what);

		press(KEY_ESC, 10);                   // out of the browser
		press(KEY_ESC, 10);                   // out of the Computers view
		frame(6);
	}

	marq_drop_long_file();

	/* ------------------------------------------------- 240p, the shelf --- */

	/*
	  The shelf's title line at 240p, which is the string this front-end cuts most often: the
	  name of the game under the cursor, on the canvas where a No-Intro dump is wider than
	  the screen. It is drawn from the committed selection and there is no version of the
	  shelf where it is not the focused thing, so it scrolls unconditionally.

	  240p rather than 720p because that is where it actually clips - at 720p the title line
	  holds fifty characters and most of the fixture fits inside them.
	*/
	{
		cfg.classicui_profile = 3;
		harness_set_fb(320, 240);
		gfx_shutdown();
		theme_update(320, 240, 3);

		harness_set_menu_core(1);
		chome_leave();
		press(KEY_MENU, 20);
		for (int i = 0; i < 80 && lib_scanning(); i++) frame(2);
		frame(20);

		check(select_titled("Yoshi"), "the longest-named fixture game is on the shelf");
		frame(10);

		const chome_profile *p = theme_get();
		int h = gfx_h();
		int ty0 = p->y_title, ty1 = p->y_title + 9 * p->ts_title;
		int paints = 0, rows = 0;

		check(chome_marq_live(), "and its title is too long for 240p, so it scrolls");
		dump("marquee-240p-shelf-rest");

		check(marq_since() < GFX_MARQ_HOLD_MS, "the clock is standing inside the title's opening hold");
		unsigned long rest = harness_fb_hash(ty0, ty1);

		int moved = marq_run(GFX_MARQ_HOLD_MS + 4 * GFX_MARQ_STEP_MS, ty0, ty1, &paints, &rows);
		printf("  240p title line: %d paints, worst %d rows of %d\n", paints, rows, gfx_h());
		check(moved, "the shelf title scrolls at 240p");
		check(rows > 0 && rows <= 2 + 9 * p->ts_title,
			"and it repaints the title line only - not the cards, not the screen");
		dump("marquee-240p-shelf-mid");

		/*
		  The travel of this particular title is one character, and that is the fixture rather
		  than the code: the longest name on this card overruns a 240p title line by one. So the
		  way to show that the shelf keeps cycling is to let two whole cycles pass and count the
		  window changes in them - a step and a snap home in each. The browser above is where a
		  long marquee is exercised; what this block is for is the two claims only the shelf can
		  make, that its title needs nobody to tell it that it has focus and that its repaint is
		  the title line rather than the cards.

		  The cycle is estimated from the name the file carries, which is longer than the title
		  the library files it under - so this over-counts, and everything below simply waits a
		  little longer than it strictly has to.
		*/
		unsigned long cycle_ms;
		{
			int lost = (int)strlen("Super Mario World 2 - Yoshi's Island (Europe)")
				- gfx_text_cols(p->w - p->inset * 2, p->ts_title);
			if (lost < 1) lost = 1;
			cycle_ms = 2 * GFX_MARQ_HOLD_MS + (unsigned long)lost * GFX_MARQ_STEP_MS;
		}

		int cycling = 0;
		marq_run(2 * cycle_ms, ty0, ty1, &cycling, &rows);
		printf("  240p title line: %d window changes over two whole cycles\n", cycling);
		check(cycling >= 3, "and it keeps cycling - a step and a snap home in each turn of it");

		/*
		  And home again, compared against the resting frame taken before the clock was moved at
		  all. The whole title line, pixel for pixel: the strongest form of "the scroll returns
		  to the start" available from here.
		*/
		int came_home = 0;
		for (unsigned long t = 0; t < 3 * cycle_ms && !came_home; t += 16)
		{
			harness_advance(16);
			chome_handle(0);
			if (harness_fb_hash(ty0, ty1) == rest) came_home = 1;
		}
		check(came_home, "and the title line comes back to the frame it started from");

		/*
		  A card whose name fits scrolls nothing, and the shelf then costs exactly what it
		  always cost. This is assert_repaint_costs()'s idle claim restated in the one place
		  this feature could have broken it: the leading folders are named in words we chose,
		  they fit at every profile, and parked on one the front-end has to be as still as it
		  was before any of this existed.
		*/
		shelf_rewind();
		frame(20);
		check(!chome_marq_live(), "a card whose name fits is not scrolled");
		dump("marquee-240p-title-fits");

		moved = marq_run(2 * GFX_MARQ_HOLD_MS + 10 * GFX_MARQ_STEP_MS, 0, h - 1, &paints, &rows);
		printf("  parked on a name that fits: %d paints over two full holds\n", paints);
		check(!moved && !paints,
			"a title that fits gains no ellipsis and is never scrolled - an idle shelf still paints nothing");
	}

	cfg.classicui_profile = (uint8_t)was_prof;
	harness_set_fb(1280, 720);
	gfx_shutdown();
	theme_update(1280, 720, was_prof);
	chome_leave();
	press(KEY_MENU, 20);
	frame(8);
}

/* ================================================================= clipped copy ===

  Text cut off on screen, as a class rather than one sentence at a time.

  gfx_clip() is the only thing in the front-end that knows a string did not fit, and it
  used to keep that to itself - it wrote a '>' over the last character and returned. Three
  cut sentences were found by eye in a single day, one of them the only line explaining
  what a menu does ("THE GAME STAYS LOADED UNTIL YOU CL>"), which is a poor way to find
  out. Under CHOME_HOST_TEST it now records every truncation (chome_gfx.h), and this
  section reads the record the whole suite left behind.

  ------------------------------------------------- what counts as a bug -------------

  Not every clip is one. gfx_clip() exists because a game called "Super Mario World 2 -
  Yoshi's Island" does not fit on a card, and cutting it there is the right answer - the
  player knows what the game is called, the card is the size the artwork makes it, and
  nothing is being explained. What is never right is cutting a sentence *we* wrote: those
  lines are the only explanation anybody gets, and half of one is worse than none.

  So the line drawn here is between a name that came off the card and words we chose:

    - Words we chose are in our own source. text_is_ours() looks the string up in
      support/classicui/chome_*.{cpp,h} - the string as handed to gfx_clip(), which is
      how "Everything this menu wants is already set." is found, and failing that its
      longest run of leading words, which is how a line assembled with snprintf() -
      "Old file kept as MiSTer.ini.bak" out of "Old file kept as %s" - is found too.
      Nothing has to be registered anywhere: a sentence added to the front-end tomorrow
      is covered the moment it is written, which is the entire point of the exercise.

    - A game title, a file name, a network name, an account name, the label a player
      typed - none of those are in our source, so they are data, and clipping them is
      gfx_clip() doing its job. They are counted and reported, never failed on.

  The one thing the source lookup cannot tell apart is our own wording drawn *as* data -
  a shelf's name on a shelf card, a system's name on a card. Those clip by design, since
  a card is sized by its artwork and not by its label. They are the allow-list below, and
  it is (site, text) pairs rather than whole functions on purpose: exempting draw_card
  wholesale would also exempt whatever sentence gets added to a card next year.

  Calls the harness makes itself are skipped - assert_typography() sweeps gfx_clip()
  across two hundred widths to test the function, and those are measurements, not screens.
*/

static char *clipsrc = 0;                  // our own source, uppercased, concatenated
static long  clipsrc_n = 0;

static void clipsrc_load()
{
	if (clipsrc) return;

	long cap = 4 * 1024 * 1024;
	clipsrc = (char *)malloc(cap);
	clipsrc_n = 0;
	if (!clipsrc) return;

	DIR *d = opendir("support/classicui");
	if (!d) { printf("  cannot open support/classicui - is the harness running from /mister?\n"); clipsrc[0] = 0; return; }

	struct dirent *e;
	while ((e = readdir(d)))
	{
		const char *n = e->d_name;
		int len = (int)strlen(n);
		if (strncmp(n, "chome", 5)) continue;
		int cpp = (len > 4 && !strcmp(n + len - 4, ".cpp"));
		int hdr = (len > 2 && !strcmp(n + len - 2, ".h"));
		if (!cpp && !hdr) continue;

		char p[512];
		snprintf(p, sizeof(p), "support/classicui/%s", n);
		FILE *f = fopen(p, "rb");
		if (!f) continue;
		size_t got = fread(clipsrc + clipsrc_n, 1, (size_t)(cap - clipsrc_n - 2), f);
		fclose(f);
		clipsrc_n += (long)got;
		clipsrc[clipsrc_n++] = '\n';
	}
	closedir(d);

	clipsrc[clipsrc_n] = 0;
	for (long i = 0; i < clipsrc_n; i++) clipsrc[i] = (char)toupper((unsigned char)clipsrc[i]);
}

/*
  Is this string words we wrote, or a name that came off the card?

  Uppercased on both sides because gfx_shout() shouts nearly everything on its way to the
  screen, so the string gfx_clip() is handed is rarely spelled the way the source spells
  it. Twelve characters is the floor for a partial match: shorter than that and a run of
  leading words stops being evidence of anything - "SUPER" is in our source, and it is
  also the first word of half the fixtures.
*/
static int text_is_ours(const char *t)
{
	clipsrc_load();
	if (!clipsrc || !clipsrc_n) return 0;

	char up[256];
	snprintf(up, sizeof(up), "%s", t);
	for (char *q = up; *q; q++) *q = (char)toupper((unsigned char)*q);

	int n = (int)strlen(up);
	if (n < 4) return 0;
	if (strstr(clipsrc, up)) return 1;

	// The longest run of leading words that is still long enough to mean something. A
	// format string contributes its literal head, which is where a composed line matches.
	for (int i = n - 1; i >= 12; i--)
	{
		if (up[i] != ' ') continue;
		up[i] = 0;
		if (strstr(clipsrc, up)) return 1;
		up[i] = ' ';
	}
	return 0;
}

/*
  Our own wording that is allowed to be cut, with the reason. See the section note: these
  are words of ours drawn where a name belongs, on a surface sized by something other than
  the text. Anything added here should be a sentence somebody has looked at on a screen.
*/
struct clip_allowed_t { const char *site; const char *text; const char *why; };
static const clip_allowed_t clip_allowed[] = {
	{ "draw_card",          "RECENTLY PLAYED",  "a shelf's name on a card, which is sized by its artwork" },
	{ "draw_card",          "RECENTLY ADDED",   "the same" },
	{ "draw_card",          "FAVOURITES",       "the same" },
	{ "draw_card",          0,                  "every other card label is a system's name or a game's" },
	{ "draw_fallback_card", 0,                  "a game's name on a card with no artwork" },
	{ "draw_title_block",   0,                  "the hero's title and the file it came from" },
	{ 0, 0, 0 }
};

static const char *clip_allowance(const gfx_clip_rec *r)
{
	for (int i = 0; clip_allowed[i].site; i++)
	{
		if (strcmp(clip_allowed[i].site, r->site)) continue;
		if (!clip_allowed[i].text) return clip_allowed[i].why;
		if (!strcasecmp(clip_allowed[i].text, r->text)) return clip_allowed[i].why;
	}
	return 0;
}

/* ------------------------------------------- one boundary rule for every list --- */

/*
  Every list wraps at its ends, and only on a press the player made.

  What was wrong. Some lists wrapped and some clamped. Options, More Settings, Online Covers,
  Sort, Power and Close Game wrapped by modulo; core options, Controllers, the disc dialog's
  buttons and the disc's core chooser clamped with a nudge; and Wi-Fi and the file browser
  clamped *silently*, pinning the row at the end with no answer of any kind. Worse than the
  disagreement, every one of the wrapping ones wrapped on auto-repeat too - so a held Down on
  a two-row screen flipped between its rows fifty times a second for as long as the key was
  held, and a held Down on Options cycled the whole panel round and round for ever.

  What holds now, for every list on both axes: holding a direction steps one entry per repeat
  to the end of the list and stops there, and a *fresh* press at that end - one with a release
  before it - jumps to the other end. See wrap_step() in chome_ui.cpp for the rule and
  held_key above it for how a press is told from a repeat.

  The menu bar is the one exemption and it is asserted here as one rather than left untested;
  move_h()'s SCR_MENUBAR case carries the argument, and assert_close_game_on_the_bar() is what
  rests on it.
*/

// Open a menu-bar entry by the screen it opens rather than by counting cells, which differ per
// profile and per whether a game is running. Leaves that screen up.
static int bar_open(int want)
{
	for (int slot = 0; slot < 6; slot++)
	{
		if (bar_slot_opens(slot) == want) return 1;
		bar_walk_home();
	}
	return 0;
}

/*
  Hold a direction and watch where the cursor goes, repeat by repeat.

  The trajectory and not only the destination, because a destination can be reached by
  coincidence. On a list of ten, a hold of nineteen events finishes on the last entry whether
  the end clamps or wraps by modulo - nineteen modulo ten is nine, and nine is the last entry -
  so a check that read the finishing position alone would pass on the very behaviour this
  replaces. What cannot coincide is the shape: every move exactly one entry, and then nothing.

  `*steps` counts the moves and `*jumps` counts those that were not a single entry. Returns
  where it finished. Callers start the hold away from a boundary, because the first event of a
  hold is a real press and is entitled to wrap.
*/
static int hold_walk(int axis, int key, int repeats, int *steps, int *jumps)
{
	int prev = chome_list_cursor(axis, 0);
	int st = 0, jp = 0;

	chome_handle(key);
	for (int r = 0; r <= repeats; r++)
	{
		if (r)
		{
			// Three frames of nothing at 16 ms each, which is about the 50 ms of REPEATRATE.
			for (int f = 0; f < 3; f++) { harness_advance(16); chome_handle(0); }
			chome_handle(key);
		}

		int now = chome_list_cursor(axis, 0);
		if (now != prev)
		{
			st++;
			if (now - prev != 1 && now - prev != -1) jp++;
		}
		prev = now;
	}

	harness_advance(16);
	chome_handle(key | UPSTROKE);
	frame(6);

	if (steps) *steps = st;
	if (jumps) *jumps = jp;
	return prev;
}

/*
  The whole rule, applied to whatever list is on screen, on one axis. `fwd` and `back` are the
  two keys that walk it - Down and Up for a column, Right and Left for a row - and the screen
  must already be up with its cursor somewhere on the list.

  Leaves the cursor on the first entry, which is where every screen here opens, so a caller can
  go on driving the screen afterwards.
*/
static void wrap_rules(const char *name, int axis, int fwd, int back)
{
	char what[256];
	int n = 0;
	int cur = chome_list_cursor(axis, &n);

	snprintf(what, sizeof(what), "%s: a list with a cursor and at least two entries on it", name);
	check(cur >= 0 && n >= 2, what);
	if (cur < 0 || n < 2) return;
	printf("  %s: %d entries, cursor at %d\n", name, n, cur);

	/*
	  To the first entry. A hold and not a run of presses: taps wrap now, so a count of them
	  cannot arrive anywhere known from an unknown start. Nothing is measured about this walk,
	  because the cursor may begin on the boundary where the hold's own first press may wrap.
	*/
	hold_dir(back, n + 8, 5);
	snprintf(what, sizeof(what), "%s: holding back reaches the first entry", name);
	check(chome_list_cursor(axis, 0) == 0, what);

	/* Down the list under a held key, one entry per repeat, and stopping at the bottom. */
	int steps = 0, jumps = 0;
	int end = hold_walk(axis, fwd, n + 8, &steps, &jumps);
	printf("  %s: held forward, %d step(s), %d jump(s), finished on %d of %d\n",
		name, steps, jumps, end, n - 1);
	snprintf(what, sizeof(what),
		"%s: a held forward steps once per entry to the last and then stops, with nine "
		"repeats to spare", name);
	check(end == n - 1 && steps == n - 1 && jumps == 0, what);

	/* The one thing a press may do there that a repeat may not. */
	press(fwd, 5);
	snprintf(what, sizeof(what), "%s: and a fresh press there wraps to the first entry", name);
	check(chome_list_cursor(axis, 0) == 0, what);

	/* The same from the other end. */
	press(back, 5);
	snprintf(what, sizeof(what), "%s: a fresh press at the first entry wraps to the last", name);
	check(chome_list_cursor(axis, 0) == n - 1, what);

	/* Which leaves a held back the whole list to walk, ending at the top. */
	steps = jumps = 0;
	end = hold_walk(axis, back, n + 8, &steps, &jumps);
	snprintf(what, sizeof(what),
		"%s: a held back steps once per entry to the first and then stops", name);
	check(end == 0 && steps == n - 1 && jumps == 0, what);

	/*
	  And the count in discrete presses, which is the check a clamp cannot satisfy: n - 1 taps
	  from the first entry reach the last, and the nth is back at the first. A clamp fails the
	  second of those, an off-by-one in either direction fails one of them, and no nudge or
	  repaint can make either true by accident.

	  Skipped on a list too long to tap through, where the two holds above have already counted
	  every entry twice.
	*/
	if (n <= 30)
	{
		for (int i = 0; i < n - 1; i++) press(fwd, 3);
		snprintf(what, sizeof(what),
			"%s: %d press(es) from the first entry reach the last", name, n - 1);
		check(chome_list_cursor(axis, 0) == n - 1, what);

		press(fwd, 3);
		snprintf(what, sizeof(what),
			"%s: and press %d is back at the first, so the walk is exactly %d long",
			name, n, n);
		check(chome_list_cursor(axis, 0) == 0, what);
	}
	else
	{
		hold_dir(back, n + 8, 5);
	}
}

static void assert_uniform_wrap()
{
	printf("\n== every list wraps, and only on a deliberate press ==\n");

	enum {
		W_HOME = 0, W_MENUBAR = 1, W_SUSPEND = 2, W_SORT = 3, W_DISPLAY = 4,
		W_OPTIONS = 5, W_BROWSE = 8, W_WIFI = 10, W_PADS = 11, W_POWER = 12,
		W_SET = 15, W_CORE = 16, W_DISC = 17, W_COVERS = 19, W_CLOSE = 20
	};

	const uint8_t was_prof = cfg.classicui_profile;

	cfg.classicui_profile = 1;
	harness_set_fb(1280, 720);
	gfx_shutdown();
	theme_update(1280, 720, 1);
	harness_set_fb_supported(1);
	harness_set_confstr(1);
	harness_set_osd_visible(0);
	// Declared visible so Display is on the bar at all - draw_menubar drops the entry when
	// the scaler output is not what is on screen.
	harness_set_scaler_visible(1);

	/* ------------------------------------------------- the shelf and its panels --- */

	harness_set_menu_core(1);
	chome_leave();
	press(KEY_MENU, 20);
	for (int i = 0; i < 80 && lib_scanning(); i++) frame(2);
	frame(12);
	bar_walk_home();
	check(chome_screen_id() == W_HOME, "on the shelf, at 720p");

	// The carousel: the one list here long enough for a wrap to be worth something to a
	// player rather than merely consistent with the rest.
	wrap_rules("the shelf carousel", 1, KEY_RIGHT, KEY_LEFT);

	/*
	  Down from the shelf has never been a list - it opens the suspend strip - and the strip's
	  own list is the row of slots. Asserted rather than assumed, because "which axis is a
	  list" is exactly the decision chome_list_cursor() is there to record: an axis that
	  answers -1 is one move_v() or move_h() has deliberately left alone.
	*/
	check(chome_list_cursor(0, 0) < 0, "the shelf has no list on its vertical axis");

	shelf_rewind();
	select_first_game();
	press(KEY_DOWN, 14);
	check(chome_screen_id() == W_SUSPEND, "Down on a game card opens its suspend strip");
	wrap_rules("the suspend strip", 1, KEY_RIGHT, KEY_LEFT);
	check(chome_list_cursor(0, 0) < 0,
		"while Down on the strip locks a slot rather than walking one");
	press(KEY_ESC, 12);
	frame(6);

	// Sort, which is Select on the shelf.
	bar_walk_home();
	press(KEY_GRAVE, 14);
	check(chome_screen_id() == W_SORT, "Select on the shelf opens Sort");
	wrap_rules("Sort", 0, KEY_DOWN, KEY_UP);
	press(KEY_ESC, 12);

	/* ------------------------------------------------------ the one exemption --- */

	bar_walk_home();
	press(KEY_UP, 14);
	check(chome_screen_id() == W_MENUBAR, "Up from the shelf reaches the menu bar");
	{
		int n = 0;
		int cur = chome_list_cursor(1, &n);
		printf("  the menu bar: %d entries, cursor at %d\n", n, cur);
		check(cur >= 0 && n >= 2, "the bar is a row with a cursor on it");

		hold_dir(KEY_LEFT, n + 8, 5);
		check(chome_list_cursor(1, 0) == 0, "a held left reaches the first entry");
		press(KEY_LEFT, 6);
		check(chome_list_cursor(1, 0) == 0,
			"and a fresh press there stays put - the bar is the one cursor still clamped");

		hold_dir(KEY_RIGHT, n + 8, 5);
		check(chome_list_cursor(1, 0) == n - 1, "a held right reaches the last entry");
		press(KEY_RIGHT, 6);
		check(chome_list_cursor(1, 0) == n - 1, "and a fresh press there stays put too");

		check(chome_list_cursor(0, 0) < 0, "and the bar has no list on its vertical axis");
		hold_dir(KEY_LEFT, n + 8, 5);
	}

	/* ------------------------------------------------ the rest of the bar's own --- */

	check(bar_open(W_DISPLAY), "Display opens from the bar");
	wrap_rules("Display", 1, KEY_RIGHT, KEY_LEFT);
	check(chome_list_cursor(0, 0) < 0, "and it is one row of tiles, with nothing above or below");
	bar_walk_home();

	check(bar_open(W_POWER), "Power opens from the bar");
	wrap_rules("Power", 0, KEY_DOWN, KEY_UP);
	bar_walk_home();

	/* ------------------------------------------------------ the Options family --- */

	check(bar_open(W_OPTIONS), "Options opens from the bar");
	wrap_rules("Options", 0, KEY_DOWN, KEY_UP);

	// Each of these is counted down from row 0, which is where Options always opens - and
	// wrap_rules() leaves it there.
	press(KEY_DOWN, 6);
	press(KEY_ENTER, 14);
	check(chome_screen_id() == W_COVERS, "Online Covers opens from its row");
	wrap_rules("Online Covers", 0, KEY_DOWN, KEY_UP);
	press(KEY_ESC, 12);

	// Controllers, with the same three-pad fixture the controllers section uses.
	harness_clear_pads();
	harness_add_pad(1, PAD_WIRED, 0x054C, 0x09CC, "Sony Computer Entertainment Wireless Controller", "");
	harness_add_pad(2, PAD_SNAC,  0x0000, 0x0000, "MiSTer SNAC Pad 1", "");
	harness_add_pad(3, PAD_BT,    0x054C, 0x09CC, "Wireless Controller", "DC:2C:26:1B:9A:71");
	frame(8);

	list_goto(0, 5, KEY_DOWN, KEY_UP, 5);
	press(KEY_ENTER, 14);
	check(chome_screen_id() == W_PADS, "Controllers opens from its row");
	wrap_rules("Controllers", 0, KEY_DOWN, KEY_UP);
	press(KEY_ESC, 12);

	/*
	  Wi-Fi, with a radio and a finished scan asserted the way the wi-fi section does it -
	  there is none in the container. That section also leaves a fake `iw` in PATH, which is
	  what stops the link refresher answering "not connected" and wiping the ingested link out
	  from under the list; this runs after it for that reason.
	*/
	net_force_present(1);
	net_force_scanning(0);
	net_force_join(JOIN_IDLE, "");
	net_ingest_scan(SCAN_TEXT);
	net_ingest_link(LINK_TEXT);
	frame(10);

	list_goto(0, 6, KEY_DOWN, KEY_UP, 5);
	press(KEY_ENTER, 14);
	check(chome_screen_id() == W_WIFI, "Wi-Fi opens from its row");
	wrap_rules("Wi-Fi", 0, KEY_DOWN, KEY_UP);
	press(KEY_ESC, 12);

	// More Settings, which is the longest list in the front-end and always scrolls - so this
	// is also where "the window follows a wrap" is exercised, since list_track() is asked for
	// a jump of the whole list twice.
	list_goto(0, 8, KEY_DOWN, KEY_UP, 5);
	press(KEY_ENTER, 16);
	check(chome_screen_id() == W_SET, "More Settings opens from its row");
	wrap_rules("More Settings", 0, KEY_DOWN, KEY_UP);
	press(KEY_ESC, 14);
	press(KEY_ESC, 12);
	frame(6);

	net_force_present(-1);

	/* -------------------------------------------------------- the file browser --- */

	/*
	  Three files in the Amiga folder. Amiga is a computer system, which is off the shelf
	  altogether and reached only through the browser - and the browser lists the directory as
	  it finds it, so the fixture exists for exactly as long as this needs it and nothing has
	  to be rescanned to make it go away. Same bargain, and the same folder, as the marquee
	  section's own long-name file.
	*/
	mkpath(ROOT "/games/Amiga");
	touch(ROOT "/games/Amiga", "Wrap One.adf", 2048);
	touch(ROOT "/games/Amiga", "Wrap Two.adf", 2048);
	touch(ROOT "/games/Amiga", "Wrap Three.adf", 2048);

	check(marq_open_browser() && chome_screen_id() == W_BROWSE,
		"the browser opens on the Amiga folder");
	wrap_rules("the file browser", 0, KEY_DOWN, KEY_UP);
	check(chome_list_cursor(1, 0) < 0, "and it is a column, with nothing on the other axis");
	press(KEY_ESC, 12);

	unlink(ROOT "/games/Amiga/Wrap One.adf");
	unlink(ROOT "/games/Amiga/Wrap Two.adf");
	unlink(ROOT "/games/Amiga/Wrap Three.adf");

	/* ---------------------------------------------------------- the disc dialog --- */

	/*
	  A PlayStation disc in the fake drive, set up the way assert_disc_dialog() sets one up.

	  The one screen in scope with a list on each axis: its buttons are a row and the core
	  chooser behind them is a column. It is also the screen whose row used to clamp on the
	  argument that two entries which wrap make Left and Right the same key - which Close Game
	  and Power, both two-row lists, have always disproved.

	  Everything is put back on the way out. The sections after this one draw the menu bar,
	  and a disc in the drive puts a whole tier between the shelf and the bar.
	*/
	const int disc_was = cfg.classicui_disc;
	cfg.classicui_disc = 1;

	static fake_disc dw;
	memset(&dw, 0, sizeof(dw));
	static const char *const dw_none[] = { "" };
	fake_iso(&dw, 0, "PLAYSTATION", "PLAYSTATION", dw_none, 0);
	fake_put(&dw, 20, 0, "BOOT = cdrom:\\SLUS_006.26;1", 27, 100);

	disc_ingest_present(1);
	disc_set_reader(fake_read, &dw);
	disc_ingest_identify(0);
	frame(8);
	check(disc_type() == DISC_T_PSX, "a PlayStation disc is in the drive");

	press(KEY_UP, 12);                        // the badge in the corner
	press(KEY_ENTER, 14);                     // and its dialog
	check(chome_screen_id() == W_DISC, "the disc dialog opens over it");
	wrap_rules("the disc dialog's buttons", 1, KEY_RIGHT, KEY_LEFT);

	/*
	  And the core chooser behind the Options button, reached by *holding* Right to the last
	  button: A on the first button plays the disc, so a tap that wrapped round onto it would
	  launch a game rather than open a list. The same reasoning assert_disc_dialog() now
	  spells out where it makes the same move.
	*/
	hold_dir(KEY_RIGHT, 8, 10);
	press(KEY_ENTER, 14);
	check(chome_screen_id() == W_DISC && chome_list_cursor(0, 0) >= 0,
		"and A on its last button opens the core chooser, which is a column");
	wrap_rules("the disc's core chooser", 0, KEY_DOWN, KEY_UP);

	press(KEY_ESC, 12);                       // out of the chooser, back to the buttons
	press(KEY_ESC, 12);                       // and off the dialog

	disc_ingest_present(0);
	disc_set_reader(0, 0);
	frame(8);
	cfg.classicui_disc = (uint8_t)disc_was;
	bar_walk_home();

	/* ------------------------------------------------------ over a running game --- */

	{
		FILE *f = fopen("/tmp/classicui_current", "wt");
		if (f) { fprintf(f, "gb\nTetris (World).gb\n"); fclose(f); }
	}

	harness_set_menu_core(0);
	harness_set_core_name("GAMEBOY");
	chome_handle(0);
	if (chome_ingame_active()) press(KEY_MENU, 14);
	press(KEY_MENU, 20);
	for (int i = 0; i < 40 && lib_scanning(); i++) frame(2);
	frame(14);
	check(chome_ingame_active(), "the menu is up over a running game");

	// Options again, because the panel is a different length in a game: OPT_ROWS_GAME and
	// OPT_ROWS_MENU are separate counts and only one of them was walked above.
	check(bar_open(W_OPTIONS), "Options opens from the in-game bar");
	wrap_rules("Options, in a game", 0, KEY_DOWN, KEY_UP);
	bar_walk_home();

	check(bar_open(W_CLOSE), "Close Game opens from the in-game bar");
	wrap_rules("Close Game", 0, KEY_DOWN, KEY_UP);
	bar_walk_home();

	/*
	  The core's own options, where the last row of every page is the page switch.

	  The thing worth proving here is that wrapping down off that row does not fight the page
	  change: the page turns on A and on nothing else, so Down off the switch row must land on
	  row 0 of the page the player is already on. The list length before and after is what says
	  so - a page turn would change it under the walk, and the tally in wrap_rules() would not
	  come out either.

	  Navigation only. Nothing here presses Left or Right on this screen, which is where a
	  value would be written to the core.
	*/
	core_opts_scan();
	frame(6);
	check(bar_open(W_CORE), "the running core's options open from the in-game bar");
	int co_was = 0;
	{
		/*
		  Which page it opened on, worked out the way the MB_CORE case of accept() works it
		  out: Picture, or the first page after it that has anything. The fixture's Picture
		  tier is empty, so hardcoding it would be asserting against a page the screen never
		  shows.
		*/
		int tier = core_opts_tier_count(CO_TIER_PICTURE);
		if (!tier) tier = core_opts_tier_count(CO_TIER_SYSTEM);
		if (!tier) tier = core_opts_tier_count(CO_TIER_RISKY);

		chome_list_cursor(0, &co_was);
		printf("  core options: %d row(s) for %d option(s) on the page it opened\n", co_was, tier);
		check(co_was == tier + 1,
			"the page switch is the last row, so the list is one longer than the page");
	}
	wrap_rules("core options", 0, KEY_DOWN, KEY_UP);
	{
		int n = 0;
		chome_list_cursor(0, &n);
		check(n == co_was,
			"and the page never turned under it: same page, same length after the whole walk");
	}
	bar_walk_home();

	if (chome_ingame_active()) press(KEY_MENU, 16);
	frame(8);

	/* ------------------------------------------- and the state, put back as found --- */

	harness_set_menu_core(1);
	cfg.classicui_profile = was_prof;
	harness_set_fb(1280, 720);
	gfx_shutdown();
	theme_update(1280, 720, was_prof);
	chome_leave();
	press(KEY_MENU, 20);
	frame(10);
	bar_walk_home();
	/*
	  Out of the Amiga folder the browser was opened from, which enter() does not undo - it
	  puts the *screen* back to the shelf and leaves the view where it was. A section that
	  hands on a sub-view hands on a shelf with no Favourites or Systems card on it, and the
	  next section to look for one by name simply does not find it.
	*/
	shelf_root();
	frame(6);
}

/*
  The screens the rest of the suite does not put through gfx_clip() in every profile.

  Most of what this section reads was recorded by the sections above it - between them
  they open every panel, and at 720p and 240p both. Two gaps are left, and both are where
  a cut sentence would land hardest:

    - The 480p profile. One harness_set_fb(640, 480) exists in the whole file, so "sd" is
      a layout almost nothing has been drawn at, and its panels are narrower than hd's.

    - Best Settings with nothing to change. Every section that opens it opens it on a
      plan, and the sentence that is only drawn when there is no plan - the one this task
      started from - had never been composed by anything.
*/
static void sweep_screens()
{
	const int was_prof = cfg.classicui_profile;

	/*
	  Nothing here is timing-dependent or state-dependent on purpose. The scaler is
	  declared visible so the Display entry is on the bar at hd and sd and off it at lo
	  (draw_menubar drops it at 240p), which is what makes "Options is the second slot,
	  or the first at 240p" true rather than hopeful.
	*/
	harness_set_scaler_visible(1);

	/*
	  And an ini with nothing left to fix, which is the state Best Settings had never been
	  drawn in. Put back byte for byte afterwards - the sections that follow read this
	  file, and one of them compares it against what it wrote.
	*/
	char inipath[1024];
	snprintf(inipath, sizeof(inipath), "%s/MiSTer.ini", ROOT);
	static char ini_was[65536];
	int ini_had = slurp_file(inipath, ini_was, sizeof(ini_was));
	ini_apply(inipath);

	struct { int w, h, force; const char *name; int display; } canv[] = {
		{ 1280, 720, 1, "hd", 1 },
		{  640, 480, 2, "sd", 1 },
		{  320, 240, 3, "lo", 0 },
	};

	for (int c = 0; c < 3; c++)
	{
		cfg.classicui_profile = (uint8_t)canv[c].force;
		harness_set_fb(canv[c].w, canv[c].h);
		gfx_shutdown();
		theme_update(canv[c].w, canv[c].h, canv[c].force);

		harness_set_menu_core(1);
		chome_leave();
		press(KEY_MENU, 20);
		frame(8);

		/*
		  The menu bar, and every entry on it. RIGHT clamps at the last entry rather than
		  wrapping, so five presses reach the end of a bar of any length and the extra
		  ones cost a nudge; ESC from any of these panels comes back to the bar.
		*/
		press(KEY_UP, 10);
		for (int e = 0; e < 5; e++)
		{
			press(KEY_ENTER, 14);
			frame(6);
			char nm[64];
			snprintf(nm, sizeof(nm), "clipsweep-%s-bar%d", canv[c].name, e);
			dump(nm);
			press(KEY_ESC, 10);
			press(KEY_RIGHT, 6);
		}

		/*
		  And the Options rows that open a panel of their own, counted down from the top -
		  Online Covers, Controllers, Wi-Fi, Best Settings, More Settings. Counted from
		  the top because opening Options puts the cursor back on row 0 every time, and
		  because the rows that come and go with a running game are all below these.

		  Nothing between Rescan Library and Menu Layout is opened on purpose: those act
		  rather than open, and this section is here to compose screens, not to rescan the
		  card underneath the sections that follow.
		*/
		for (int i = 0; i < 5; i++) press(KEY_LEFT, 6);       // the first slot
		if (canv[c].display) press(KEY_RIGHT, 8);             // Options

		static const int rows[] = { 1, 5, 6, 7, 8 };
		static const char *rowname[] = { "covers", "pads", "wifi", "best", "more" };
		for (int r = 0; r < 5; r++)
		{
			press(KEY_ENTER, 14);                             // Options, cursor on row 0
			for (int i = 0; i < rows[r]; i++) press(KEY_DOWN, 4);
			press(KEY_ENTER, 14);
			frame(8);

			char nm[64];
			snprintf(nm, sizeof(nm), "clipsweep-%s-%s", canv[c].name, rowname[r]);
			dump(nm);

			press(KEY_ESC, 10);                               // back to Options
			press(KEY_ESC, 10);                               // back to the bar
		}

		// And the save/suspend strip, which is reached from the shelf rather than the bar.
		press(KEY_ESC, 10);
		frame(6);
		press(KEY_DOWN, 12);
		frame(8);
		char nm[64];
		snprintf(nm, sizeof(nm), "clipsweep-%s-suspend", canv[c].name);
		dump(nm);
		press(KEY_ESC, 10);
		frame(6);
	}

	if (ini_had > 0) put_file(inipath, ini_was);

	cfg.classicui_profile = (uint8_t)was_prof;
	harness_set_fb(1280, 720);
	gfx_shutdown();
	theme_update(1280, 720, was_prof);
	chome_leave();
	press(KEY_MENU, 20);
	frame(8);
}

static void assert_no_clipped_copy()
{
	printf("\n== clipped copy ==\n");

	sweep_screens();

	int ours = 0, data = 0, allowed = 0, skipped = 0;
	int n = gfx_clip_log_n();

	printf("  %d distinct truncations recorded\n", n);

	for (int i = 0; i < n; i++)
	{
		const gfx_clip_rec *r = gfx_clip_log(i);

		// The harness testing gfx_clip() itself, not a screen drawing text.
		if (!strncmp(r->site, "assert_", 7)) { skipped++; continue; }

		/*
		  Printed rather than counted, because this is where the section could go wrong
		  quietly: a sentence of ours that text_is_ours() failed to recognise would be
		  waved through as a game's name, and the only way anybody would notice is by
		  reading the list. It is short - these are names off the card.
		*/
		if (!text_is_ours(r->text))
		{
			data++;
			printf("  name  %dx%d  %-20s s%d  %3dpx (-%d)  \"%s\"\n",
				r->cw, r->ch, r->site, r->scale, r->maxpx, r->lost, r->text);
			continue;
		}

		const char *why = clip_allowance(r);
		if (why) { allowed++; continue; }

		ours++;
		printf("  CUT   %dx%d  %-20s s%d  %3dpx (-%d)  \"%s\"\n",
			r->cw, r->ch, r->site, r->scale, r->maxpx, r->lost, r->text);
	}

	printf("  %d data, %d allowed, %d harness, %d of our own words cut\n",
		data, allowed, skipped, ours);

	check(ours == 0, "no sentence of ours is cut off on any screen at any profile");
	check(n < 4000, "the clip log did not overflow, so nothing went unexamined");
}

/*
  Rows a player cannot see, which is the same defect as a cut sentence one dimension over.

  draw_rows_c() stops when a row would cross the bottom of its panel. That is correct
  drawing and a silent loss: the row is still in the list, still selectable, and never
  appears. It has bitten twice - Close Game in the Options panel, and every row past the
  fifteenth on the PSX's 27-row core-options page - both times found by eye on a television
  rather than here.

  A list that can outgrow its panel must window itself: list_fit(), list_track() and
  list_scrollbar() in chome_ui.cpp, which is what the Options panel, More Settings, the
  browser, Wi-Fi, Controllers and the core-options page all now do. This asserts it for all
  of them at once, at every profile, and names the screen when it fails - so the next list
  to grow is caught by a test instead of by a player.
*/
static void assert_no_row_is_hidden()
{
	printf("\n== rows below the fold ==\n");

	int n = chome_rowdrop_n();
	for (int i = 0; i < n; i++)
		printf("  LOST  %-24s %d row(s) drawn past the panel edge\n",
			chome_rowdrop_site(i), chome_rowdrop_lost(i));

	printf("  %d list(s) truncated a row somewhere\n", n);
	check(n == 0, "no list loses a row off the bottom of its panel, at any profile");

	/*
	  And the fixture plumbing itself. An exhausted option map answers every later read with
	  a default and drops every write, which does not fail here - it fails somewhere else,
	  as an unrelated feature apparently not working. It cost a while to trace once.
	*/
	check(!harness_optmap_full(), "the stub option map never ran out, so no write was dropped");
}

int main()
{
	printf("Classic Home host harness\n\n");

	build_sd();
	harness_set_root(ROOT);

	/*
	  Seeded before the shelf's very first frame, so ss_system_id()'s first call in this
	  whole process runs under an override this file never asked to be loaded - the only
	  way to tell "chome_ui.cpp's start-up path calls ss_systems_load()" apart from "the
	  loader works when a test calls it directly", which assert_screenscraper() already
	  covers on its own. See its "loaded at start-up" check, near the top of that section,
	  for where this gets read back and forgotten again.
	*/
	mkpath(ROOT "/classicui");
	put_file(ROOT "/classicui/ss-systems.cfg", "psx=8675309\n");

	cfg.classicui = 1;
	cfg.classicui_artfetch = 0;                       // no network in tests
	cfg.classicui_freeze = 1;                         // as cfg.cpp defaults it
	cfg.classicui_overscan = 6;                       // as cfg_parse() defaults it
	/*
	  The two typography keys, at the values cfg_parse() gives them. Both matter to every
	  pixel assertion in this file: zero tracking is the 8-pixel advance every panel width
	  was measured against, and caps on is the case every pinned frame was captured in. A
	  zero-initialised cfg would have turned the capitals off for the whole run.
	*/
	cfg.classicui_tracking = 0;
	cfg.classicui_caps = 1;
	snprintf(cfg.classicui_artdir, sizeof(cfg.classicui_artdir), "boxart");
	cfg.classicui_gamelist = 1;                       // as cfg.cpp defaults it
	cfg.osd_timeout = 0;

	/*
	  Off for the suite, though cfg.cpp defaults it on: every harness_set_fb() below
	  states the canvas a section runs at, and with the halving in force each of those
	  statements would quietly mean something else - 1280x720 arriving as 640x360 under
	  every pinned frame in the file. The option gets its own section
	  (assert_half_canvas), which turns it on against the model in stubs.cpp and
	  checks the layout, the live toggle and the floor.
	*/
	cfg.classicui_halfres = 0;

	harness_set_fb(1280, 720);

	// First entry needs no key at all: any HandleUI call takes over.
	frame(3);
	for (int i = 0; i < 200 && lib_scanning(); i++) frame(2);
	frame(60);

	assert_index();
	assert_views();
	// Before anything has been launched, so the choice of which file a card shows is not
	// yet under the influence of a play count. See the section's own comment.
	assert_variants();
	// Directly after it, because it asserts the same grouping on the CD systems' own
	// folders and needs the same untouched play counts: a card with nothing played shows
	// its first file, which is what makes "one card, two discs" checkable.
	assert_cd_systems();
	assert_slots();
	assert_art();
	assert_gamelist();
	assert_screenscraper();
	// Directly after it: it drives the same module, and it leaves classicui_artfetch and
	// classicui_screenscraper off again on the way out, which every section that scrolls a
	// shelf afterwards depends on.
	assert_disc_art();
	/*
	  And then the cover ladder, which needs the art slots exactly as assert_gamelist() left
	  them - one game with a cover off the card and five with none - and asserts that before
	  it does anything else. It leaves the same three settings off that the section above
	  does, and calls art_redo() on the way out so the slots go back untouched.
	*/
	assert_art_ladder();
	/*
	  Directly after it, because it starts from the shelf that one leaves and its whole
	  subject is the same module's two refusals - told apart now against a store on the card
	  rather than against a flag that a core change threw away. It leaves the same three
	  settings off, deletes the store it wrote, and calls art_redo() on its way out.
	*/
	assert_ss_throttle();
	/*
	  And the two that build on that store, in this order because the second uses what the
	  first proves. Both start from the shelf the sections above leave, both put the settings
	  back off and delete what they wrote, and both call art_redo() on their way out - the
	  second one also takes a cover it put on the card back off it, because every section
	  after this is entitled to the shelf assert_gamelist() built.
	*/
	assert_art_fetch_order();
	assert_pack_provenance();
	assert_physical_disc();
	assert_disc_serials();
	// Directly after it, because it drives the same state machine with the same fake
	// discs, and before every section that draws or logs a disc name: it puts a title
	// table on the card and takes it away again, and anything running in between would
	// see disc_display_name() answer differently. See its own comment.
	assert_disc_titles();
	// Directly after it, because it does the same thing: installs a title table, drives the
	// same state machine with the same fake discs, and takes the table away again, leaving
	// the empty drive and the reset reader that one leaves.
	assert_disc_dialog_words();
	assert_disc_ui();
	// Directly after it: it leaves the same state that one does - the shelf, an empty drive
	// and the HD canvas - and it puts the badge back under the same fake disc.
	assert_disc_breath();
	assert_partial_repaint();
	// Directly after it, because it is the same mechanism on the other region of the screen
	// and it starts from the state that one leaves: the shelf at 720p with an empty drive.
	assert_letter_jump();
	assert_stack_badge();
	assert_carousel_slide();
	assert_disc_launch();
	// After it, because it leaves the same state that one does and starts from it: a disc
	// in the drive, the flag on, and the shelf up.
	assert_disc_dialog();
	// Directly after it, and before anything moves the canvas: it leaves exactly the state
	// that one does, and it is about the size the dialog gives the disc at 720p.
	assert_disc_hires();
	// Directly after it, because it walks the canvas about and puts it back the way that one
	// does: the same dialog measured at every profile, which is where the panel's own size is
	// pinned rather than the disc's.
	assert_disc_dialog_size();
	// And after that one, which leaves the drive empty and the flag off: this needs both
	// back, and it launches a disc of its own on the way out.
	assert_disc_shelf_slots();
	assert_video();
	assert_index_cache();
	// Directly after it: it borrows and hands back the fake card exactly the way that
	// one does, and it needs the library that one leaves to prove it did.
	assert_scan_slices();

	walk_profile("hd", 1, 1280, 720);
	walk_profile("sd", 2, 640, 480);
	walk_profile("lo", 3, 320, 240);

	// Non-nominal canvas: 1080p output with fb_size=2.
	walk_profile("auto960", 0, 960, 540);

	/*
	  And the stretched one: a 15 kHz TV mode that reached us unhalved, which is what
	  vga_scaler=1 and direct_video both hand over. The metrics are checked in
	  assert_stretched_canvas(); these are for the eyes, and they are twice as wide as
	  they will look on the television - every pixel of them is half as wide as it is
	  tall once the mode stretches it back over a 4:3 screen.
	*/
	walk_profile("tv640", 0, 640, 240);

	walk_looks();
	assert_launch();
	// Before every other in-game section: the session record is read once per process,
	// and this is the one that cares which process read it. See its own comment.
	assert_ingame_view();
	// After it, and not up with the other disc sections, for the same reason: opening the
	// in-game menu over a disc consumes the once-per-process session read, and the section
	// above is the one that cares who consumed it.
	assert_disc_identity();
	// Directly after it: it needs the same running disc and the same consumed session read,
	// and it is about what that section's own screen does and does not show of the game.
	assert_ingame_still();
	// And directly after that one, because it is the other half of the same question: that
	// section asks whether the still is on screen at all, this one asks how much of it the two
	// dims leave. It leaves the canvas exactly as that one does.
	assert_ingame_dim();
	// And after that one, for the third time for the same reason: this section opens the
	// in-game menu too, to reach the one state where "a core owns the drive" can be seen
	// from a host test. Everything else in it would run anywhere.
	assert_disc_art_arrives();
	// After the launches above, so there is a recent list to be wrong about, and before
	// the shelf sections that now see a fourth card on the root shelf.
	assert_launch_into_state();
	assert_recent();
	// After it, because this one launches a game of its own and the section above is about
	// the order of the list a launch writes to.
	assert_variant_ui();
	assert_ingame();
	assert_save_on_pausing_core();
	assert_freeze_off();
	assert_core_idle_predicate();
	assert_core_options_screen();
	assert_long_core_list_scrolls();
	assert_sms_shaped_page_scrolls();
	assert_snac_ownership();
	assert_snac_row_is_staged();
	assert_core_option_word_forms();
	assert_per_game_core_options();
	assert_core_option_for_all_games();
	assert_core_options_are_reachable();
	// Directly after it, because the two are about the same panel from opposite ends: that
	// one counts down to Core Settings, this one walks past it to the row underneath.
	assert_options_panel_scrolls();
	// And directly after that, because it is the other end of the same move: that section
	// walks the panel Close Game left, this one walks the bar it arrived on.
	assert_close_game_on_the_bar();
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
	// Both about the analog output, and both pure: they read cfg and the theme and put
	// everything back, so they can sit anywhere the canvas is not mid-transition.
	assert_stretched_canvas();
	assert_analog_report();

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
		press(KEY_UP, 8);                     // wrap to the last row, which is About now
		press(KEY_UP, 8);                     // Advanced Settings
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

		/*
		  Moving off the row disarms it, so a stray press cannot be completed later - and a
		  move that is *refused* disarms it too, which is the half worth stating: the code
		  clears the arm before it tests the boundary, because reaching for a row that is not
		  there still means the player has stopped meaning to forget this one.

		  The refused move is a held UP against the top of the list, and that is the only shape
		  it now has: a tap at the first row wraps to the last, so tapping would move the cursor
		  rather than being refused, and the arm would be cleared by the ordinary path instead
		  of by the one this line is about.
		*/
		list_goto(0, 1, KEY_DOWN, KEY_UP, 10);
		press(KEY_TAB, 10);                   // arm the second row
		press(KEY_UP, 10);                    // ...and off it again, onto the first
		hold_dir(KEY_UP, 8, 10);              // a refused move counts too
		press(KEY_DOWN, 10);
		press(KEY_TAB, 10);                   // so this arms rather than forgets
		hold_dir(KEY_UP, 8, 10);
		frame(4);
		check(bt_count() == 3, "moving off an armed row disarms it, refused moves included");

		/*
		  A paired controller that is not connected is the case worth acting on, so the
		  legend offers waking it - and only for that one. Row 2 is paired-not-connected
		  in the fixture; row 1 is connected and must not offer it.

		  Placed rather than stepped, here and at every A below: the walks above end at the top
		  of the list now instead of being stopped short by a clamp, so "one more Down" no
		  longer names a row.
		*/
		list_goto(0, 1, KEY_DOWN, KEY_UP, 10);   // the second row, counting from one
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

		// Row 1 is the wired DualShock, player 1. A on it is "test it", not "scan". Placed
		// rather than assumed: the pairing panel above left the cursor wherever it left it,
		// and harness_set_pad_state() below is about *this* pad and no other.
		list_goto(0, 0, KEY_DOWN, KEY_UP, 10);
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

		  Reached by holding Down rather than by counting five of them, which is both more
		  honest and now necessary. It always meant "the bottom of the list"; counting only
		  worked because the fifth press was absorbed by a clamp when the walk started a row
		  in - and with the ends wrapping, that fifth press lands on row 0 instead and A there
		  opens a tester rather than a scan.
		*/
		hold_dir(KEY_DOWN, 12, 8);

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
	  A PlayStation pad on the SNAC port that this core cannot read.

	  The reader lives in the core's sys framework (psx_snac_pad.sv, over UIO_SNAC_PAD). A
	  core built from an older sys has none, and the whole of the player's experience of that
	  was: nothing happens. The one signal was a line in the log - "no SNAC pad reader in this
	  core (sys update needed)" - which a person sitting in front of a television will never
	  see, and which is invisible on this front-end even over ssh, because the front-end's own
	  Controllers screen lists a working SNAC pad and simply omits a broken one. Silence is
	  indistinguishable from the pad being unplugged.

	  Two halves. First what the module will say when asked, which is where the four quiet
	  cases live: three of them are cases where "no reader" is *true and still wrong*, and one
	  is a core nobody has looked at yet. Then the screen, at all three profiles, because the
	  sentence is long enough that saying it at 240p takes a second wording.

	  The quiet cases are the point of the section, not its edge cases. A warning that appears
	  on hardware which is working teaches a player to ignore warnings, and every one of these
	  would have done it: the player who never asked for SNAC pads, the player who told us a
	  Mega Drive adapter is on the port, and - worst of the three - the player whose old PSX
	  core reads the port natively and whose pad works perfectly.
	*/
	printf("\n== a snac pad this core cannot read ==\n");
	{
		uint8_t was_pad = cfg.snac_pad;
		uint8_t was_dev = cfg.snac_device;
		uint8_t was_prof = cfg.classicui_profile;

		// The PSX, as it publishes itself, with Pad1 among its thirteen values. The same
		// fixture assert_snac_ownership() uses, for the one case that needs a real core.
		static const char *snac_psx[] =
		{
			"PSX", "FS1,BIN,Load ROM",
			"D8O[48:45],Pad1,Dualshock,Off,Digital,Analog,GunCon,NeGcon,Wheel-NegCon,"
				"Wheel-Analog,Mouse,Justifier,SNAC-port1,Analog Joystick,Popn",
			"D8h0O[66],SNAC MemCard,Virtual,Real",
			0
		};

		/*
		  And the pads, without the working SNAC pad the section above left in the fixture.

		  Those two states cannot coexist: no reader means snacpad.cpp destroys the uinput
		  devices, so there is no SNAC row for the list to hold. Left as it was, every frame
		  dumped below would show the front-end saying a SNAC pad cannot work in this core
		  directly under a SNAC pad that is working in it - which as a picture is worse than no
		  picture, and these frames are release-note material. Put back on the way out, because
		  the sections after this one are entitled to the fixture they were written against.
		*/
		harness_clear_pads();
		harness_add_pad(1, PAD_WIRED, 0x054C, 0x09CC, "Sony Computer Entertainment Wireless Controller", "");
		harness_add_pad(3, PAD_BT,    0x054C, 0x09CC, "Wireless Controller", "DC:2C:26:1B:9A:71");

		/* ------------------------------------------------- what the module answers --- */

		// The menu core, where core_owns_snac() answers 0 - so what is being varied below is
		// the player's two settings and the core's sys, one at a time.
		harness_set_menu_core(1);
		harness_set_confstr(1);

		cfg.snac_pad = 0;
		cfg.snac_device = 0;
		harness_reset_snac();
		harness_set_snac_reader(0);
		snacpad_init();

		check(snacpad_reader() == SNAC_UNPROBED,
			"a core nothing has looked at yet reports neither reader nor no reader");
		check(!snacpad_wanted(), "and with the feature off, the port is not ours to want");

		snac_tick();
		check(!snacpad_wanted() && snacpad_reader() == SNAC_UNPROBED,
			"a poll with the feature off never finds out whether there is a reader");

		/*
		  Which is the whole argument for UNPROBED being a state and not a synonym for 0. The
		  poll returns before it touches SPI when it wants nothing, so in every quiet case
		  below `supported` stays at UNPROBED for the life of the core - and a caller that
		  read that as "no reader" would announce a fault on all of them, for ever.
		*/

		// The one case that speaks: asked for, a PlayStation pad on the port, nobody else
		// driving it, and the core has been looked at and has no reader.
		cfg.snac_pad = 1;
		snacpad_init();
		snac_tick();
		check(snacpad_wanted(), "with the feature on and nothing else claiming it, the port is ours");
		check(snacpad_reader() == SNAC_NO_READER, "and this core has no reader in it");

		/*
		  And now the three quiet cases, every one of them driven *without* snacpad_init() -
		  which is the whole of what makes them worth asserting, and is what the first version
		  of this section got wrong.

		  snacpad_init() is a core load. Turning a setting off is not: the core on the FPGA is
		  the same core, so `supported` keeps the NO_READER the probe above established. Drive
		  these with an init in between and every one of them lands on UNPROBED instead, where
		  the reader test alone is already enough to keep the screen quiet - so the checks pass
		  with or without the snacpad_wanted() gate they are supposed to be about, and the gate
		  could be deleted with the suite still green. Proved by doing exactly that.
		*/

		// Turned off mid-session. No relaunch: the next poll answers differently, and the
		// reader's own answer has not changed at all - which is the trap.
		cfg.snac_pad = 0;
		snac_tick();
		check(!snacpad_wanted(), "turning the feature off stops us wanting the port at once");
		check(snacpad_reader() == SNAC_NO_READER,
			"while the core is still the same readerless core, so the reader alone would speak");
		cfg.snac_pad = 1;

		/*
		  snac_device: the player has told us what is physically on the port, because nothing
		  readable changes when a SuperDock's bypass switch moves it (see cfg.h). With a
		  Mega Drive adapter plugged in, a framework reader for PlayStation pads would change
		  nothing - so "no reader" is true and is not the interesting fact, and sending that
		  player off to update a core would be sending them nowhere.
		*/
		cfg.snac_device = 1;
		snac_tick();
		check(!snacpad_wanted() && snacpad_reader() == SNAC_NO_READER,
			"a non-PlayStation adapter on the port is not ours either, readerless core and all");
		cfg.snac_device = 0;

		/*
		  And the case that made snacpad_wanted() have to exist rather than being recomputed
		  from the two cfg fields above: a core reading the SNAC port through its own option.
		  It needs no framework reader and works without one, so on an old PSX core in native
		  mode the sentence would be true and the player's pad would be fine - the one place
		  the message would be a flat lie about working hardware.

		  Picked mid-session, on the core already known to have no reader, because that is the
		  order a player does it in: they plug a pad in, nothing happens, they go and find
		  Pad1 in the core's own options. Nothing resets `supported` on the way.
		*/
		harness_set_menu_core(0);
		harness_set_confstr_table(snac_psx);
		harness_set_opt("[48:45]", 10, 0);         // Pad1 = SNAC-port1
		snac_tick();
		check(!snacpad_wanted(), "a core that reads the port itself is not missing our reader");
		check(snacpad_reader() == SNAC_NO_READER,
			"even though the probe already found none in it - the reader is not the question");

		// The player unpicks it, still mid-session. Now the port is ours, and now the core's
		// age matters - so this is the transition the notice has to appear across.
		harness_set_opt("[48:45]", 0, 0);          // Pad1 = Dualshock
		snac_tick();
		check(snacpad_wanted() && snacpad_reader() == SNAC_NO_READER,
			"unpicking it hands the port back to us, and the reader is missing after all");

		// And a core that has genuinely never been looked at, which is every core for its
		// first couple of milliseconds and every core whose port belongs to somebody else.
		snacpad_init();
		check(snacpad_reader() == SNAC_UNPROBED && !snacpad_wanted(),
			"a core load puts both answers back to knowing nothing");

		harness_set_confstr(1);
		harness_set_menu_core(1);

		// And a current core, which is the state most cards are in and must stay silent.
		harness_reset_snac();                      // a reader in the fabric again
		snacpad_init();
		snac_tick();
		check(snacpad_wanted() && snacpad_reader() == SNAC_READER,
			"a core built from a current sys has the reader and nothing needs saying");

		/* -------------------------------------------------------------- and on screen --- */

		struct { int w, h, force; const char *name; } canv[] = {
			{ 1280, 720, 1, "hd" },
			{  640, 480, 2, "sd" },
			{  320, 240, 3, "lo" },
		};

		for (int c = 0; c < 3; c++)
		{
			cfg.classicui_profile = (uint8_t)canv[c].force;
			harness_set_fb(canv[c].w, canv[c].h);
			gfx_shutdown();
			theme_update(canv[c].w, canv[c].h, canv[c].force);

			char nm[64];

			// The current core first, so what follows is a change and not a starting state.
			cfg.snac_pad = 1;
			cfg.snac_device = 0;
			harness_reset_snac();
			snacpad_init();
			snac_tick();

			open_controllers();
			snprintf(nm, sizeof(nm), "snacgap-%s-1-quiet", canv[c].name);
			dump(nm);
			check(pads_amber() == 0,
				"the Controllers screen says nothing about SNAC when the core can read it");

			// The old core.
			harness_set_snac_reader(0);
			snacpad_init();
			snac_tick();

			open_controllers();
			snprintf(nm, sizeof(nm), "snacgap-%s-2-said", canv[c].name);
			dump(nm);
			int said = pads_amber();
			check(said > 0, "  and says so when there is no reader in it");

			/*
			  A core load, straight out of the state that speaks and before any poll has run.
			  This is the flicker: a fresh core is UNPROBED for its first couple of
			  milliseconds, and the in-game menu can be open across a core change.

			  Ordered here on purpose. Two things independently keep this quiet - snacpad_init()
			  clearing `wanted`, and snac_gap_note() testing for NO_READER rather than "not
			  READER" - and either one alone is enough, so neither can be made to fail by
			  itself. What can be made to fail is the pair, and only from a preceding state
			  where the port *was* wanted: run this after one of the quiet cases below and
			  `wanted` is already 0 for an unrelated reason, and the check passes with both
			  guards deleted. Measured, in that order, both ways.
			*/
			snacpad_init();
			open_controllers();
			snprintf(nm, sizeof(nm), "snacgap-%s-3-unprobed", canv[c].name);
			dump(nm);
			check(snacpad_reader() == SNAC_UNPROBED && pads_amber() == 0,
				"  and nothing before the port has been looked at even once");

			// Back to the readerless core the quiet cases below are all changes to.
			snac_tick();
			open_controllers();
			check(snacpad_wanted() && snacpad_reader() == SNAC_NO_READER && pads_amber() == said,
				"  the first poll of that core finds no reader, and it says so again");

			/*
			  And the quiet cases on the glass rather than at the accessor, because a screen can
			  get this wrong in a way the module cannot. Each at every profile, since the wording
			  is chosen per profile and a condition written into one branch of that choice would
			  only fail at one canvas.

			  Every one of them a change to the *same* readerless core, with no snacpad_init()
			  anywhere: see the note in the half above. An init here is a core load, it puts
			  snacpad_reader() back to UNPROBED, and the reader test alone then keeps the screen
			  quiet - so these would pass with the snacpad_wanted() gate deleted, which is what
			  the first version of this section did.
			*/
			cfg.snac_pad = 0;
			snac_tick();
			open_controllers();
			snprintf(nm, sizeof(nm), "snacgap-%s-4-feature-off", canv[c].name);
			dump(nm);
			check(snacpad_reader() == SNAC_NO_READER && pads_amber() == 0,
				"  nothing is said when SNAC pads are switched off, readerless core and all");
			cfg.snac_pad = 1;

			cfg.snac_device = 1;
			snac_tick();
			open_controllers();
			check(snacpad_reader() == SNAC_NO_READER && pads_amber() == 0,
				"  nor when the player says the port is not a PlayStation pad");
			cfg.snac_device = 0;

			/*
			  And the one that matters most: the core claims the port with its own option, which
			  is what a player does *after* finding their pad dead. It reads the port natively, so
			  the pad now works - and the probe's NO_READER is still standing behind it.
			*/
			harness_set_menu_core(0);
			harness_set_confstr_table(snac_psx);
			harness_set_opt("[48:45]", 10, 0);     // Pad1 = SNAC-port1
			snac_tick();
			open_controllers();
			snprintf(nm, sizeof(nm), "snacgap-%s-6-core-owns-it", canv[c].name);
			dump(nm);
			check(snacpad_reader() == SNAC_NO_READER && pads_amber() == 0,
				"  nor about a core that has taken the port over and works without our reader");

			// The player puts it back, still mid-session. The notice returns, which is what
			// proves it was following the state rather than the order these were driven in.
			harness_set_opt("[48:45]", 0, 0);
			snac_tick();
			harness_set_confstr(1);
			harness_set_menu_core(1);
			open_controllers();
			check(said > 0 && pads_amber() == said, "  and the same notice comes back, unchanged");

			press(KEY_ESC, 10);
			press(KEY_ESC, 10);
			press(KEY_ESC, 10);
			frame(6);
		}

		/*
		  Left off, and both halves of it. A section that walked away with cfg.snac_pad on and
		  a readerless core behind it would put an amber warning on the Controllers screen for
		  every later section that composes one - including the clipped-copy sweep, thousands
		  of checks later, which is a long way from the code that caused it.
		*/
		cfg.snac_pad = was_pad;
		cfg.snac_device = was_dev;
		harness_reset_snac();
		snacpad_init();

		// And the fixture the section above built, SNAC pad included.
		harness_clear_pads();
		harness_add_pad(1, PAD_WIRED, 0x054C, 0x09CC, "Sony Computer Entertainment Wireless Controller", "");
		harness_add_pad(2, PAD_SNAC,  0x0000, 0x0000, "MiSTer SNAC Pad 1", "");
		harness_add_pad(3, PAD_BT,    0x054C, 0x09CC, "Wireless Controller", "DC:2C:26:1B:9A:71");

		cfg.classicui_profile = was_prof;
		harness_set_fb(1280, 720);
		gfx_shutdown();
		theme_update(1280, 720, 1);
		chome_leave();
		press(KEY_MENU, 20);
		frame(8);
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
		press(KEY_UP, 8);                     // wrap to the last row, which is About now
		press(KEY_UP, 8);                     // Advanced Settings
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
		press(KEY_UP, 8);                     // wrap to the last row, which is About now
		press(KEY_UP, 8);                     // Advanced Settings
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
		press(KEY_UP, 8);                     // wrap to About, the last row
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
		  - classicui is not there either, so the switch is appended like the rest. The
		    file that has it in the wrong section, and the eleven sibling keys our key
		    is a prefix of, get a fixture of their own below
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
		check(crlf == 18, "and the file gained only the lines it had to");   // 12 + a 6-line block

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

		// The three that were absent, in a section of their own - the file ends inside
		// [video=], where bare keys would have applied to that one video mode. The
		// switch matters most here: classicui=1 scoped to one video mode is a front-end
		// that appears on the HDMI set and not on the CRT.
		check(strstr(out, "[MiSTer]\r\nclassicui=1\r\ncontroller_info=0\r\ndisable_autofire=1\r\n") != 0,
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
			check(strstr(lfout, "[MiSTer]\nclassicui=1\ncontroller_info=0\n") != 0,
				"and the added block follows it");
		}

		// A file with nothing in it at all - a fresh card, or an ini somebody emptied.
		{
			static char eout[2048];
			int en = ini_rewrite("", 0, eout, sizeof(eout));
			eout[en] = 0;
			check(en > 0 && strstr(eout, "[MiSTer]\r\nclassicui=1\r\nvideo_info=0\r\n") != 0,
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
		check(np == 4, "all four settings are reported as needing a change");
		check(!strcmp(plan[1].had, "4") && plan[1].present,
			"the value shown is the last one in the file, which is the one in force");
		check(!plan[2].present && !plan[2].had[0], "a key that is only a comment reads as absent");
		check(!plan[0].present && !strcmp(plan[0].want->key, "classicui"),
			"and the front-end's own switch is the first thing the plan offers");

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
		check(wrote == 4, "writing reports what it changed");

		static char now[8192], saved[8192];
		check(slurp_file(path, now, sizeof(now)) > 0, "the ini is still readable afterwards");
		check(slurp_file(bak, saved, sizeof(saved)) > 0, "and a backup was left");
		check(!strcmp(saved, SRC_CRLF), "the backup is the old file, byte for byte");
		check(!strcmp(now, out), "and the new one is what the rewrite said it would be");

		// The session that is already running parsed the ini before any of this was
		// true, so it has to be told as well - that is what makes "no restart" honest.
		check(cfg.video_info == 0 && cfg.controller_info == 0 && cfg.disable_autofire == 1
			&& cfg.classicui == 1,
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
		press(KEY_UP, 8);                     // wrap to the last row, which is About now
		press(KEY_UP, 8);                     // Advanced Settings
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
		check(ini_plan(path, plan, INI_WANT_MAX) == 4, "and shows the plan rather than acting");

		/*
		  The same panel with something to say about the analog output, on the canvas
		  where saying it is hardest: four settings stacked over two lines each, the
		  undo note wrapped over three, and the video block under all of it. This is
		  the arrangement to look at - whether it fits is measured (the block is
		  6 + 9 + 2*11 px on top of 160, against a 212 px cap at 240p), but whether it
		  reads as a report rather than as a fifth setting is not.

		  direct_video rather than an unplugged HDMI lead, so the stub does not resize
		  the canvas underneath the capture. Same two facts either way.
		*/
		{
			const uint8_t was_dv = cfg.direct_video;
			const uint8_t was_fs = cfg.forced_scandoubler;
			const char was_vm = cfg.vga_mode_int;

			cfg.direct_video = 1;
			cfg.forced_scandoubler = 1;
			cfg.vga_mode_int = 2;                 // svideo

			press(KEY_ESC, 10);                   // back to Options, still on this row
			press(KEY_ENTER, 14);                 // and in again, now with the block
			frame(8);
			dump("ini-7-analog-240p");
			check(vp_analog_facts(1) == (VP_AN_31K | VP_AN_MONO),
				"the 240p panel is drawn with both video facts on it");

			cfg.direct_video = was_dv;
			cfg.forced_scandoubler = was_fs;
			cfg.vga_mode_int = was_vm;
		}

		press(KEY_ESC, 10);
		press(KEY_ESC, 10);
		press(KEY_ESC, 10);
		frame(6);

		harness_set_fb(1280, 720);
		gfx_shutdown();
		theme_update(1280, 720, 1);
		frame(6);

		/* ------------------------------------------------------ the switch itself --- */

		/*
		  classicui=1 is the newest member of the set and the only one that is the
		  front-end rather than a pop-up, and it brings two problems the other three do
		  not.

		  Sections is the first. This fixture is the card that actually generates the
		  bug report: [MiSTer] enables the front-end, so the shelf is there and looks
		  right, and [NES] turns it off again - so the menu button inside an NES game
		  gives the player the classic OSD instead of this. Nothing on the shelf can
		  hint at it. The rewriter sets every assignment of the key, which is what makes
		  the answer stop depending on which core is loaded.

		  Its own family is the second. "classicui" is a prefix of eleven keys, all of
		  which can be in this same file and none of which is this screen's to touch:
		  the player's television margin, their art folder, their deliberate
		  classicui_freeze=0, their ScreenScraper login. ini_stray_lines() forgives a
		  line whose key merely starts with one of ours - that is exactly the hole a
		  prefix bug would hide in - so they are named here one at a time instead.
		*/
		static const char *SRC_SW =
			"[MiSTer]\r\n"
			"classicui=1\r\n"
			"classicui_freeze=0     ; the SNES core dies if asked for a state\r\n"
			"classicui_overscan=9\r\n"
			"classicui_artdir=covers\r\n"
			"classicui_disc=1\r\n"
			"classicui_screenscraper=1\r\n"
			"classicui_ss_user=someone\r\n"
			"video_info=0\r\n"
			"controller_info=0\r\n"
			"disable_autofire=1\r\n"
			"\r\n"
			"[NES]\r\n"
			"classicui=0\r\n";

		{
			static char swout[8192];
			int swlen = (int)strlen(SRC_SW);
			int sn = ini_rewrite(SRC_SW, swlen, swout, sizeof(swout));
			check(sn > 0, "the switch fixture can be rewritten");
			swout[sn] = 0;

			int sw_lf = 0;
			for (int i = 0; i < sn; i++)
				if (swout[i] == '\n' && (!i || swout[i - 1] != '\r')) sw_lf++;
			check(!sw_lf, "and it is still CRLF afterwards");

			check(strstr(swout, "[NES]\r\nclassicui=0") == 0,
				"a core section that switches the front-end off is switched back on");
			check(strstr(swout, "[NES]\r\nclassicui=1\r\n") != 0, "in place, in its own section");
			check(strstr(swout, "[MiSTer]\r\nclassicui=1\r\n") != 0,
				"and the one that was already right is left saying the same thing");

			// Nothing was missing, so nothing is appended: a file that only needed a
			// value changed does not gain a section or a comment from us.
			check(!strstr(swout, "Written by Classic Home"),
				"a file with every key present gains no block at the end");

			/*
			  The nine siblings, one by one and with their spacing, because
			  ini_stray_lines() cannot see a change to a key that starts with ours. A
			  1 written over classicui_freeze=0 would be this screen breaking somebody's
			  SNES core to tidy their ini.
			*/
			check(strstr(swout, "classicui_freeze=0     ; the SNES core dies if asked for a state") != 0,
				"a deliberate classicui_freeze=0 survives untouched, comment and spacing and all");
			check(strstr(swout, "classicui_overscan=9") != 0, "so does a television margin the player chose");
			check(strstr(swout, "classicui_artdir=covers") != 0, "and their art folder");
			check(strstr(swout, "classicui_disc=1") != 0, "and an opt-in they made themselves");
			check(strstr(swout, "classicui_screenscraper=1") != 0, "and the scraper switch");
			check(strstr(swout, "classicui_ss_user=someone") != 0, "and their login");
			check(ini_stray_lines(SRC_SW, swout) == 0, "and no line that is not ours was touched");

			// And the same thing through the plan and the real writer, since that is the
			// path the screen takes.
			put_file(path, SRC_SW);

			int nsw = ini_plan(path, plan, INI_WANT_MAX);
			check(nsw == 1 && !strcmp(plan[0].want->key, "classicui"),
				"a card with the pop-ups already off has only the switch left to offer");
			check(plan[0].present && !strcmp(plan[0].had, "0"),
				"and it is shown as the 0 that is really in the file");

			check(ini_apply(path) == 1, "writing it changes one setting");

			static char swnow[8192];
			check(slurp_file(path, swnow, sizeof(swnow)) > 0 && !strcmp(swnow, swout),
				"and the file on the card is what the rewrite said it would be");
			check(ini_plan(path, plan, INI_WANT_MAX) == 0, "with nothing left over");
		}

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
		  And the disc flag, which this block now writes through the table. It has to go back
		  for a stronger reason than the four above: left on, every later section would have a
		  front-end that probes for a drive and can put a badge on the shelf, so a section
		  about something else entirely would be composing a different screen.
		*/
		uint8_t was_disc = cfg.classicui_disc;

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
			// rec is checked separately below, because one row is allowed to have none.
			if (o->rec != OPT_NO_REC && (o->rec < o->lo || o->rec > o->hi)) bad_range++;
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

		/*
		  And a recommendation is either a value the row can hold or the sentinel that says
		  there isn't one. Worth its own check because opt_rec_text() would hand INT_MIN to
		  text_for() and print it, which is only unreachable while every caller is guarded by
		  opt_is_rec() - a guard that lives in a different file from this table.

		  The count is asserted at exactly one, and that is a deliberate ratchet rather than
		  a fact worth knowing: "no opinion" is the easy way out of writing a recommendation,
		  and the argument for the row that has it (chome_opt.h, OPT_NO_REC) is that its value
		  is a measurement of the player's hardware rather than a preference. A second row
		  claiming that should have to come and say so here.
		*/
		int rec_oor = 0, rec_none = 0;
		for (int i = 0; i < opt_count(); i++)
		{
			const opt_def *o = opt_at(i);
			if (o->rec == OPT_NO_REC) { rec_none++; continue; }
			if (o->rec < o->lo || o->rec > o->hi) rec_oor++;
		}
		check(!rec_oor, "every recommendation is a value its own row can actually hold");
		check(rec_none == 1, "and exactly one row says it has no opinion, which is snac_device");

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

		check(opt_apply(path, 0, 0) == 2, "and writing reports both");

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
		check(!opt_dirty() && opt_apply(path, 0, 0) == 0, "and there is nothing left to write");

		/* ------------------------------------------------- physical disc support --- */

		/*
		  classicui_disc, which had no row anywhere until now.

		  The whole optical-disc feature was gated on it and offered on no screen, so the
		  only way to turn it on was to edit MiSTer.ini with a keyboard - on a front-end
		  whose entire purpose is that a television and a controller are enough. A feature
		  reachable only by its author is not opt-in, and a setting that exists only in a
		  file fails the objective this front-end is measured against.

		  Written through this table rather than through a screen of its own because it is
		  exactly what the table is for: a MiSTer.ini key whose value is 0 or 1 inside the
		  range cfg.cpp declares, with a uint8_t behind it. See chome_opt.h's three rules.

		  The fixture is what makes the last check here worth having. SRC *ends inside
		  [NES]*, so a writer that appended classicui_disc to the end of the file would put
		  it in a core section, where cfg.cpp's parser would read it for the NES core and
		  never for the menu - and the front-end would report the setting as on while the
		  disc feature stayed off on the shelf, with nothing on screen able to explain it.
		  That is the single most expensive trap in this project and it has cost it several
		  bugs; the assertion is that this key lands under a [MiSTer] header of its own.
		*/
		{
			int i_disc = opt_find("classicui_disc");
			check(i_disc >= 0, "physical disc support has a row on the settings screen");

			const opt_def *od = (i_disc >= 0) ? opt_at(i_disc) : 0;
			check(od && od->kind == OPT_LIST && od->lo == 0 && od->hi == 1,
				"offered as a two-value list inside the range cfg.cpp declares");
			check(od && od->def == 0 && od->rec == 0,
				"off is both the firmware default and what this menu recommends");
			/*
			  Which is what puts a machine with a drive in amber, deliberately: chome_disc.h
			  states the opinion the colour is reporting - the feature stays opt-in until it
			  has been proven against a range of drives and discs.
			*/
			check(od && od->when == OW_NOW,
				"and it says it takes effect now, which disc_poll() is what makes true");
			/*
			  Not scaler_only, and that is not a detail: the machine most likely to have a
			  USB optical drive attached is a SuperStation One on an analog set, which is
			  precisely the machine that drops every scaler_only row from this list.
			*/
			check(od && !od->scaler_only, "and it is offered on a machine with no scaler");

			put_file(path, SRC);
			opt_load(path);

			check(!opt_present(i_disc) && opt_value(i_disc) == 0,
				"a card that has never heard of the setting reads it as off");
			check(opt_is_rec(i_disc), "and is not flagged for it");

			check(opt_step_by(i_disc, 1) && opt_value(i_disc) == 1,
				"one press turns physical disc support on");
			check(!opt_is_rec(i_disc), "which is away from the default, and says so");
			check(opt_dirty() == 1 && cfg.classicui_disc == 0,
				"staged only - nothing has been written and nothing is live yet");

			check(opt_apply(path, 0, 0) == 1, "and saving writes it");
			check(slurp_file(path, now, sizeof(now)) > 0
				&& strstr(now, "[MiSTer]\r\nclassicui_disc=1\r\n") != 0,
				"under a [MiSTer] header of its own, not appended into the [NES] section the file ends in");
			check(!strstr(now, "[NES]\r\ncontroller_info=0\r\nclassicui_disc"),
				"which is the one place it would have been silently ignored");
			check(cfg.classicui_disc == 1 && opt_wrote_live(),
				"the running front-end is told, so the drive is looked for without a relaunch");

			// Back off again through the same row, which is the direction disc_poll() had
			// no answer for until disc_release_due() - see assert_physical_disc().
			check(opt_step_by(i_disc, -1) && opt_apply(path, 0, 0) == 1 && cfg.classicui_disc == 0,
				"and turning it off again goes through the same row and the same writer");
		}

		/* ------------------------------------------------------------ the screen --- */

		harness_set_menu_core(1);
		chome_leave();
		press(KEY_MENU, 20);
		frame(8);

		press(KEY_UP, 10);                    // the menu bar
		press(KEY_RIGHT, 10);                 // Options
		press(KEY_ENTER, 14);
		press(KEY_UP, 8);                     // wrap to the last row, which is About now
		press(KEY_UP, 8);                     // Advanced Settings
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
		press(KEY_UP, 8);                     // About, the last row
		press(KEY_UP, 8);                     // Advanced Settings
		press(KEY_UP, 8);                     // More Settings
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

		static char written[8192];
		snprintf(written, sizeof(written), "%s", now);

		/*
		  Leaving with edits that were never written throws them away, so it asks first.
		  Read off the panel, because there is nothing else to ask: staying put and
		  leaving look identical from outside until the panel is gone.
		*/
		press(KEY_DOWN, 8);                   // back round to the first setting
		frame(6);

		/*
		  Asserted here rather than on the frame above, and the move is the reason: with the
		  font and the two typography rows the list is longer than a 720p panel holds, so
		  saving leaves the cursor on the last row and the amber ones scrolled off the top.
		  The claim is about what the colour means, not about which rows happen to be in
		  view, so it is made where the coloured row is on screen.
		*/
		check(panel_rows_pixels(COL_YELLOW) > 0,
			"the colour is about the default, not about being unsaved, so it stays");

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
		press(KEY_UP, 8);                     // About, the last row
		press(KEY_UP, 8);                     // Advanced Settings
		press(KEY_UP, 8);                     // More Settings
		press(KEY_ENTER, 14);
		frame(8);
		dump("set-8-240p");
		check(gfx_w() == 320, "the list lays out on a 240p canvas");

		unsigned long h_top = panel_hash();
		press(KEY_UP, 8);                     // wrap to the last row, which is off the bottom
		frame(6);
		dump("set-9-240p-scrolled");
		check(panel_hash() != h_top, "a list too long for the panel scrolls to the cursor");

		/*
		  And the Physical Disc row itself, on the canvas his CRT gets, with its sentence
		  under it - the frame that is the whole answer to "the only way to turn disc support
		  on is a keyboard".

		  Walked to by index from the top of the list, which is only the table's index while
		  every row is in the view, so that is asserted rather than hoped for: the scaler-only
		  rows are dropped on a machine with no scaler and the walk would land elsewhere.
		*/
		{
			int i_disc = opt_find("classicui_disc");
			int vw[OPT_MAX];
			check(opt_view(vw, OPT_MAX, 1) == opt_count() && i_disc >= 0
				&& vw[i_disc] == i_disc, "the row's place in the list is the table's own order");

			press(KEY_DOWN, 8);               // wrap from Save Changes to the first row
			for (int i = 0; i < i_disc; i++) press(KEY_DOWN, 6);
			frame(8);
			dump("set-10-240p-physical-disc");
			check(panel_hash() != h_top, "and the disc row is reachable at 240p");

			/*
			  And Replace Pack Art, for the same reason and with one extra thing to hold: it
			  must default to OFF and recommend OFF, which is the only row in the table where
			  those two agree on the *unhelpful* value. It is off because turning it on spends
			  one ScreenScraper request per pack-supplied cover, against the allowance the miss
			  store exists to protect - so a future edit that "tidied" rec up to match the
			  other rows would quietly start spending it.
			*/
			int i_pack = opt_find("classicui_ss_replace_pack");
			check(i_pack >= 0, "Replace Pack Art is offered as a row, not only as an ini key");
			if (i_pack >= 0)
			{
				const opt_def *o = opt_at(i_pack);
				check(o && o->def == 0 && o->rec == 0,
					"and it both defaults to and recommends Off, because On costs requests");
				check(o && o->help && o->help[0],
					"and it carries a sentence, since which covers it touches is unguessable");

				for (int i = 0; i < i_pack - i_disc; i++) press(KEY_DOWN, 6);
				frame(8);
				dump("set-11-240p-replace-pack-art");
				check(panel_hash() != h_top, "and it is reachable at 240p too");
			}
		}

		/*
		  SNAC Adapter, which is the same omission as Physical Disc made a second time: the
		  key shipped reachable only by editing MiSTer.ini with a keyboard.

		  The interesting assertion is the one about the amber. Every other row on this
		  screen is a preference, so "away from what we recommend" is worth colouring - but
		  this one says what is physically plugged into a port, and both answers are correct
		  for the person giving them. A player who set Other Console because they own an N64
		  adapter must not be told they are off-spec, so the row carries OPT_NO_REC and
		  neither value is flagged. That is checked on both values rather than on the
		  sentinel, because what matters is what the footer does, not how it is spelled.

		  Also that it is not scaler_only. The machine this setting exists for is a
		  SuperStation One with a SuperDock - analog, and so exactly the machine that drops
		  every scaler_only row from this list.
		*/
		{
			int i_snac = opt_find("snac_device");
			check(i_snac >= 0, "the SNAC adapter has a row, not only an ini key");

			const opt_def *od = (i_snac >= 0) ? opt_at(i_snac) : 0;
			check(od && od->group == OG_PADS,
				"in Controllers, where somebody goes when a pad is not behaving");
			check(od && od->kind == OPT_LIST && od->lo == 0 && od->hi == 1,
				"as a two-value list inside the range cfg.cpp declares");
			check(od && od->nchoices == 2 && od->choices
				&& !strcmp(od->choices[0].label, "PlayStation")
				&& !strcmp(od->choices[1].label, "Other Console"),
				"named for the plug the player can see, not Off and On");
			check(od && od->when == OW_NOW,
				"and it takes effect now, which snacpad_poll() re-deriving ownership is what makes true");
			check(od && !od->scaler_only,
				"and it is offered on an analog machine, which is the one it is for");
			check(od && od->help && od->help[0], "and it carries a sentence of its own");

			put_file(path, SRC);
			opt_load(path);

			check(!opt_present(i_snac) && opt_value(i_snac) == 0,
				"a card that has never heard of it reads as a PlayStation pad");
			check(opt_is_rec(i_snac), "and is not flagged");
			check(opt_step_by(i_snac, 1) && opt_value(i_snac) == 1, "one press says Other Console");
			check(opt_is_rec(i_snac),
				"and that is not flagged either - it is a fact about the desk, not a preference");

			check(opt_apply(path, 0, 0) == 1, "saving writes it");
			check(slurp_file(path, now, sizeof(now)) > 0
				&& strstr(now, "[MiSTer]\r\nsnac_device=1\r\n") != 0,
				"under a [MiSTer] header of its own, not into the [NES] section the file ends in");
			check(cfg.snac_device == 1 && opt_wrote_live(),
				"and the running poll is told, so the port is let go without a relaunch");

			/*
			  And X, which is the trap the amber was only half of. opt_set() clamps, so
			  resetting to a sentinel of INT_MIN would land on lo - X on "Other Console"
			  would quietly write PlayStation and the legend across the bottom would have
			  called that the usual value. The press is refused instead, and because it is
			  refused the prompt is not drawn at all: the screen does not offer a key that
			  does nothing but shake the panel.
			*/
			check(opt_value(i_snac) == 1 && !opt_reset(i_snac) && opt_value(i_snac) == 1,
				"X is refused rather than clamping the sentinel onto PlayStation");
			check(!opt_has_rec(i_snac), "because the row has no usual value to go back to");

			int i_rum2 = opt_find("rumble");
			check(i_rum2 >= 0 && opt_has_rec(i_rum2), "which is not true of an ordinary row");

			/*
			  ...and the prompt is gone from the bar, checked in pixels rather than by asking
			  the code that decides it. Focus is on Replace Pack Art from the block above, so
			  two presses up reach SNAC Adapter and a third reaches Button Pop-Up - an
			  ordinary row, on the same screen, at the same size, differing in nothing but
			  whether X is offered. Less ink is the assertion; a hash would only say the bar
			  changed, which it would also do if the wording had shifted.
			*/
			press(KEY_UP, 6);
			press(KEY_UP, 6);
			frame(8);
			dump("set-12-240p-snac-adapter");
			long ink_snac = legend_ink();

			press(KEY_UP, 6);
			frame(8);
			long ink_ord = legend_ink();

			check(ink_snac > 0, "the bar still offers Change and Back on the SNAC row");
			check(ink_snac < ink_ord,
				"and one prompt fewer than an ordinary row, because X is not offered at all");

			cfg.snac_device = 0;
		}

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
		cfg.classicui_disc = was_disc;
		theme_invalidate();
		theme_update(1280, 720, 1);

		unlink(path);
		unlink(bak);
	}

	/*
	  Online Covers: the ScreenScraper account, set up from the front-end rather than by
	  editing MiSTer.ini over ssh.

	  Three things are worth failing over here and none of them is the layout.

	  The password must not reach the screen. That is checked by pixels rather than by
	  looking for a substring, because a substring search can only look where somebody
	  thought to look: the panel is drawn twice with two different passwords of the same
	  length, and once more with one of a different length, and all three have to be
	  identical to the byte. A screen that put any function of the password on the glass -
	  the text, a run of asterisks, even its length - fails that.

	  The values must survive the trip. cfg is what the running firmware reads and the file
	  is what the next core load reads, so both are checked, and the file has to come back
	  CRLF with the new keys under a [MiSTer] header of their own rather than appended into
	  whatever section it happens to end inside.

	  And a value MiSTer.ini cannot carry must be refused at the keyboard. See
	  ini_value_ok() in chome_ini.cpp for the bug: cfg.cpp drops characters it does not
	  recognise and treats a ';' as a comment, so a password with one in it is written
	  correctly, read back mangled, and fails on credentials for ever with nothing on any
	  screen able to show the difference.
	*/
	printf("\n== online covers ==\n");
	{
		uint8_t was_ss = cfg.classicui_screenscraper;
		char was_user[64], was_pass[64];
		snprintf(was_user, sizeof(was_user), "%s", cfg.classicui_ss_user);
		snprintf(was_pass, sizeof(was_pass), "%s", cfg.classicui_ss_pass);

		char path[1024], bak[1024];
		snprintf(path, sizeof(path), "%s/MiSTer.ini", ROOT);
		snprintf(bak, sizeof(bak), "%s.bak", path);

		/* ------------------------------------------------ the five states --- */

		/*
		  The one state a shipped build is always in, and the only one this binary cannot
		  draw: the dummy devid in run.sh makes ss_available() true for the whole harness.
		  So it is asked of the predicate instead, which is why the predicate takes the
		  answer as an argument. The gate binary is what proves a shipped build has no
		  credential; this is what proves the screen says so when it has not.
		*/
		check(!strcmp(chome_covers_state(0, 1, "dinofly", 1), "Not Available"),
			"a build with no application credential says so, however set up the account is");
		check(!strcmp(chome_covers_state(0, 0, "", 0), "Not Available"),
			"and says the same thing on a machine with nothing configured at all");

		check(!strcmp(chome_covers_state(1, 0, "dinofly", 1), "Off"),
			"switched off is off, account or no account");
		check(!strcmp(chome_covers_state(1, 1, "", 0), "No Account"),
			"on with no account is reported as incomplete, not as on");
		check(!strcmp(chome_covers_state(1, 1, "dinofly", 0), "No Password"),
			"and so is an account with no password");
		check(!strcmp(chome_covers_state(1, 1, "dinofly", 1), "On"),
			"on, with both halves of an account, is on");

		/*
		  ...and that last one is the same line ss_enabled() draws, which is what makes the
		  screen honest rather than merely consistent with itself. Checked against the real
		  thing rather than asserted in a comment.
		*/
		cfg.classicui_screenscraper = 1;
		strcpy(cfg.classicui_ss_user, "dune");
		check(ss_enabled() == 1 && !strcmp(chome_covers_state(ss_available(),
			cfg.classicui_screenscraper, cfg.classicui_ss_user, 1), "On"),
			"the state the screen calls On is the state a request is really made in");
		cfg.classicui_ss_user[0] = 0;
		check(ss_enabled() == 0 && !strcmp(chome_covers_state(ss_available(),
			cfg.classicui_screenscraper, cfg.classicui_ss_user, 1), "No Account"),
			"and the state it calls incomplete is one no request is made in");
		cfg.classicui_screenscraper = 0;

		/* --------------------------------------- what MiSTer.ini can hold --- */

		/*
		  The character class is cfg.cpp's, copied into chome_ini.cpp because the macros are
		  private to the parser - so what is checked here is the copy, not the original. The
		  firmware's own parser is not linked into this harness; if CHAR_IS_SPECIAL ever
		  gains a character, this passes while refusing something that would have worked,
		  which is the harmless direction.
		*/
		check(ini_value_ok("dinofly"), "a plain login survives MiSTer.ini");
		check(ini_value_ok("p4ss-w0rd_!@#$*+=,.:~[]()/"), "so does every punctuation mark it keeps");
		check(ini_value_ok("two words"), "and a space in the middle");
		check(ini_value_ok(""), "an empty value is not an unusable one - it means there is none");

		check(!ini_value_ok("hunter&2"), "an ampersand does not, and the keyboard types one");
		check(!ini_value_ok("50%off"), "nor a percent sign");
		check(!ini_value_ok("semi;colon"), "nor a semicolon, which would comment out the rest of the line");
		check(!ini_value_ok("say\"hi\""), "nor a quote");
		check(!ini_value_ok("it's"), "nor an apostrophe");
		check(!ini_value_ok("back\\slash"), "nor a backslash");
		check(!ini_value_ok("what?"), "nor a question mark");
		check(!ini_value_ok("a`b"), "nor a backtick");
		check(!ini_value_ok("{}<>|^"), "nor the brackets and bars the symbol page also offers");

		check(!ini_value_ok(" lead"), "a leading space is trimmed off, so it is refused");
		check(!ini_value_ok("trail "), "and so is a trailing one");
		check(!ini_value_ok("=eaten"), "and a leading = , which ini_parse_var() skips");

		/* ----------------------------------------------------- the screen --- */

		/*
		  Reached the way a person reaches it: the menu bar, Options, one row down from
		  Cover Art. The row it sits under is the argument for where it lives, so the walk
		  asserts the position rather than counting from the end of the list.
		*/
		cfg.classicui_screenscraper = 0;
		cfg.classicui_ss_user[0] = 0;
		cfg.classicui_ss_pass[0] = 0;

		harness_set_menu_core(1);
		chome_leave();
		press(KEY_MENU, 20);
		frame(8);
		press(KEY_UP, 10);                    // the menu bar
		press(KEY_RIGHT, 10);                 // Options
		press(KEY_ENTER, 14);
		frame(6);
		dump("covers-1-options-row");

		int on_options = chome_screen_id();

		press(KEY_DOWN, 8);                   // Online Covers, under Cover Art
		press(KEY_ENTER, 14);
		frame(8);
		check(chome_screen_id() != on_options, "A on the row opens a screen of its own");
		dump("covers-2-nothing-set");

		unsigned long h_nothing = covers_hash();

		/* ------------------------------------------- the password stays off --- */

		/*
		  Three renders of the same screen, differing only in the password the staging
		  buffer was filled from. The first pair is the same length as each other, so a row
		  drawing the text would differ; the third is a different length, so a row drawing
		  asterisks would differ too.
		*/
		press(KEY_ESC, 10);
		strcpy(cfg.classicui_ss_pass, "hunter2");
		press(KEY_ENTER, 14);
		frame(8);
		unsigned long h_pass_a = covers_hash();
		dump("covers-3-password-set");

		check(h_pass_a != h_nothing, "a password that is set makes the row say something else");

		press(KEY_ESC, 10);
		strcpy(cfg.classicui_ss_pass, "ZZZZZZZ");
		press(KEY_ENTER, 14);
		frame(8);
		check(covers_hash() == h_pass_a, "and not one pixel of it depends on what the password is");

		press(KEY_ESC, 10);
		strcpy(cfg.classicui_ss_pass, "q");
		press(KEY_ENTER, 14);
		frame(8);
		check(covers_hash() == h_pass_a, "nor on how long it is");

		press(KEY_ESC, 10);

		/* ------------------------------------------------- typing an account --- */

		/*
		  A file that ends inside a core section, which is the shape that makes the append
		  rule matter: bare keys added at the end of this would be scoped to [NES] and would
		  silently do nothing in the menu core, where this front-end runs.
		*/
		static const char *SRC_COV =
			"[MiSTer]\r\n"
			"; my own notes, which have to survive this\r\n"
			"classicui=1\r\n"
			"classicui_screenscraper=0\r\n"
			"video_contrast=50\r\n"
			"\r\n"
			"[NES]\r\n"
			"video_info=1\r\n";

		put_file(path, SRC_COV);

		cfg.classicui_screenscraper = 0;
		cfg.classicui_ss_user[0] = 0;
		cfg.classicui_ss_pass[0] = 0;

		press(KEY_ENTER, 14);                 // back into Online Covers
		frame(8);

		press(KEY_RIGHT, 10);                 // the switch, on the first row
		frame(6);
		check(cfg.classicui_screenscraper == 0, "turning it on does not touch cfg until it is saved");
		dump("covers-4-switched-on");

		press(KEY_DOWN, 8);                   // Account Name
		press(KEY_ENTER, 10);
		check(osk_active(), "A on the account row asks for the name");
		dump("covers-5-keyboard");

		// Typed rather than driven around the grid: the grid is the keyboard's own test
		// further up this file, and what is being checked here is where the text lands.
		harness_set_input_pad(0);
		press(KEY_D, 6);
		press(KEY_U, 6);
		press(KEY_N, 6);
		press(KEY_E, 6);
		press(KEY_ENTER, 8);
		harness_set_input_pad(1);
		frame(6);

		check(!osk_active(), "and Enter closes it");
		check(cfg.classicui_ss_user[0] == 0, "the name is staged, not written - there is a Save row");

		press(KEY_DOWN, 8);                   // Password
		press(KEY_ENTER, 10);
		check(osk_active(), "A on the password row asks for one too");

		harness_set_input_pad(0);
		press(KEY_H, 6);
		press(KEY_U, 6);
		press(KEY_N, 6);
		press(KEY_T, 6);
		press(KEY_3, 6);
		press(KEY_ENTER, 8);
		harness_set_input_pad(1);
		frame(6);

		check(!osk_active(), "and Enter closes that one");
		dump("covers-6-three-to-save");

		/* -------------------------------------------------------- the write --- */

		press(KEY_DOWN, 8);                   // Save Changes
		press(KEY_ENTER, 12);
		frame(6);
		dump("covers-7-armed");

		static char now[8192];
		check(slurp_file(path, now, sizeof(now)) > 0 && !strcmp(now, SRC_COV),
			"one press of A does not touch the file");

		/*
		  The second press writes - and it is captured, because this is where the password
		  did once escape. Not from the screen: from ini_apply_set(), a shared writer three
		  files away that logged every key = value pair it was handed and had never been
		  handed a secret before. /tmp/debug.txt on the device is this stream and it is
		  world-readable, so a password in it is a password published.

		  Read back rather than reasoned about. stdout is redirected only around the press
		  that writes; everything printed by the front-end while it saves lands in the file,
		  which is exactly the set of lines that would have landed on the card.
		*/
		fflush(stdout);
		int saved_out = dup(fileno(stdout));
		const char *logpath = "/tmp/chome_covers_log.txt";

		if (!freopen(logpath, "wb", stdout)) printf("  could not capture the log\n");
		press(KEY_ENTER, 12);
		frame(8);
		fflush(stdout);

		dup2(saved_out, fileno(stdout));
		close(saved_out);
		clearerr(stdout);

		static char logtext[8192];
		int loglen = slurp_file(logpath, logtext, sizeof(logtext));
		check(loglen > 0, "the front-end said something while it wrote");
		check(strstr(logtext, "hunt3") == 0, "and not one word of it was the password");
		check(strstr(logtext, "classicui_ss_pass = ***") != 0,
			"the line for it is there, with the value replaced rather than the line dropped");
		check(strstr(logtext, "classicui_ss_user = dune") != 0,
			"while the login, which is not a secret, is logged as itself");
		unlink(logpath);

		// And the mechanism on its own, since a future caller of the writer inherits it
		// without knowing it exists.
		check(!strcmp(ini_loggable("classicui_ss_pass", "hunter2"), "***"),
			"the password key is unloggable whoever writes it");
		check(!strcmp(ini_loggable("CLASSICUI_SS_PASS", "hunter2"), "***"),
			"and however MiSTer.ini happens to spell it");
		check(!strcmp(ini_loggable("classicui_ss_user", "dune"), "dune"),
			"and nothing else is redacted, or the log would stop being worth reading");
		check(!strcmp(ini_loggable("vscale_mode", "1"), "1"), "least of all a number");

		dump("covers-8-saved");

		check(cfg.classicui_screenscraper == 1, "the second press tells the running firmware");
		check(!strcmp(cfg.classicui_ss_user, "dune"), "the account name reaches cfg");
		check(!strcmp(cfg.classicui_ss_pass, "hunt3"), "and so does the password");
		check(ss_enabled() == 1, "which is the whole point: a request would now be made");

		int n_now = slurp_file(path, now, sizeof(now));
		check(n_now > 0, "and the file was rewritten");

		int lone_lf = 0;
		for (int i = 0; i < n_now; i++)
			if (now[i] == '\n' && (!i || now[i - 1] != '\r')) lone_lf++;
		check(!lone_lf, "still CRLF, every line of it");

		check(strstr(now, "[MiSTer]\r\nclassicui_screenscraper=0") == 0
			&& strstr(now, "classicui_screenscraper=1\r\n") != 0,
			"the switch was set in the line that was already there");
		check(strstr(now, "; Written by Classic Home - Options > Online Covers.") != 0,
			"the block at the end says which screen added it");
		check(strstr(now, "[MiSTer]\r\nclassicui_ss_user=dune\r\nclassicui_ss_pass=hunt3\r\n") != 0,
			"and the two new keys are under a [MiSTer] header, not left in [NES]");

		const char *nes = strstr(now, "[NES]");
		const char *acct = strstr(now, "classicui_ss_user=dune");
		const char *hdr = strstr(now, "; Written by Classic Home - Options > Online Covers.");
		check(nes && acct && hdr && nes < hdr && hdr < acct,
			"in that order, so nothing lands in the core section the file ended in");

		check(strstr(now, "; my own notes, which have to survive this") != 0,
			"the player's own comment is still there");
		check(strstr(now, "video_contrast=50") != 0, "and a setting this screen knows nothing about");
		check(strstr(now, "[NES]\r\nvideo_info=1") != 0, "and their core section, intact");
		check(ini_stray_lines(SRC_COV, now) == 0, "and no line that is not ours was touched");

		static char backed[8192];
		check(slurp_file(bak, backed, sizeof(backed)) > 0 && !strcmp(backed, SRC_COV),
			"the old file is kept beside it, byte for byte");

		/* ------------------------------------ a password MiSTer.ini cannot keep --- */

		/*
		  A semicolon, which is the worst of them: the parser treats it as the start of a
		  comment, so the value written would be correct and the value read back would be
		  the part before it. Typed on the row, refused at the keyboard, and the proof is
		  that Save then has nothing to do - if the text had been staged, the two presses
		  below would have written it.
		*/
		press(KEY_UP, 8);                     // Password
		press(KEY_ENTER, 10);
		check(osk_active(), "the password row opens again");

		harness_set_input_pad(0);
		press(KEY_H, 6);
		press(KEY_SEMICOLON, 6);
		press(KEY_A, 6);
		press(KEY_ENTER, 8);
		harness_set_input_pad(1);
		frame(6);
		dump("covers-9-refused");

		press(KEY_DOWN, 8);                   // Save Changes
		press(KEY_ENTER, 12);
		press(KEY_ENTER, 12);
		frame(8);

		check(!strcmp(cfg.classicui_ss_pass, "hunt3"),
			"a password MiSTer.ini cannot store is refused rather than half-written");

		static char after[8192];
		check(slurp_file(path, after, sizeof(after)) > 0 && !strcmp(after, now),
			"and the file is exactly as the good save left it");

		press(KEY_ESC, 10);
		press(KEY_ESC, 10);
		press(KEY_ESC, 10);

		/* --------------------------------------------------------- at 240p --- */

		/*
		  The canvas his CRT actually gets. Four rows, a value column right-aligned against
		  a label column, and three lines of footer under them - the arrangement most likely
		  to run into itself at this size, and the reason the phrases in cov_state_of() are
		  measured rather than chosen.
		*/
		strcpy(cfg.classicui_ss_user, "dune");
		cfg.classicui_ss_pass[0] = 0;
		cfg.classicui_screenscraper = 1;

		harness_set_fb(320, 240);
		gfx_shutdown();
		theme_update(320, 240, 3);
		frame(12);

		press(KEY_UP, 10);                    // Display is dropped at 240p: Options is first
		press(KEY_ENTER, 14);
		frame(8);

		// Options with the extra row on the narrowest canvas: ten rows have to fit inside
		// the panel, and the value column has to stay clear of the label column - which is
		// the measurement the phrases in cov_state_of() were cut to.
		dump("covers-10-240p-options-row");

		press(KEY_DOWN, 8);
		press(KEY_ENTER, 14);
		frame(8);
		check(gfx_w() == 320, "the panel lays out on a 240p canvas");
		dump("covers-11-240p-no-password");

		press(KEY_ESC, 10);
		press(KEY_ESC, 10);
		press(KEY_ESC, 10);

		harness_set_fb(1280, 720);
		gfx_shutdown();
		theme_update(1280, 720, 1);
		frame(6);

		cfg.classicui_screenscraper = was_ss;
		snprintf(cfg.classicui_ss_user, sizeof(cfg.classicui_ss_user), "%s", was_user);
		snprintf(cfg.classicui_ss_pass, sizeof(cfg.classicui_ss_pass), "%s", was_pass);

		unlink(path);
		unlink(bak);
	}

	/*
	  The icons. Two things worth failing over: a system that has lost its icon
	  because an id was renamed on one side and not the other - which shows up on
	  screen as a system quietly falling back to the generic art - and any icon that
	  came out empty or nearly solid, which is what a bad source file or a bad
	  threshold looks like.

	  Asked of chome_sysicon_id() rather than of the table, because "has an icon" is no
	  longer the same question as "has a row in chome_icons32.h": the CD systems added beside
	  their cartridge siblings borrow those siblings' drawings, since only tools/icons32.py
	  may put artwork in that header. Working it out here a second way is exactly how a
	  renamed id would keep passing while the shelf had already fallen back.

	  Saturn is named, because it is the one system with nothing to borrow. If a Saturn icon
	  is ever generated this list gets shorter, and a system arriving in it that was not
	  there before is a regression.
	*/
	printf("\n== system icons ==\n");
	{
		static const char *const no_icon[] = { "saturn" };

		int missing = 0, unexpected = 0;
		for (int i = 0; i < lib_sys_count(); i++)
		{
			const chome_sys *sy = lib_sys(i);
			if (!sy) continue;

			const char *icon = chome_sysicon_id(sy->id);
			if (icon && strcasecmp(icon, sy->id))
				printf("  system \"%s\" draws \"%s\"\n", sy->id, icon);
			if (icon) continue;

			int allowed = 0;
			for (size_t k = 0; k < sizeof(no_icon) / sizeof(no_icon[0]); k++)
				if (!strcasecmp(no_icon[k], sy->id)) allowed = 1;

			printf("  no icon for system \"%s\"%s\n", sy->id, allowed ? " (known)" : "");
			if (allowed) missing++; else unexpected++;
		}
		check(!unexpected, "every system draws an icon, or is one of the known few that cannot");
		check(missing == (int)(sizeof(no_icon) / sizeof(no_icon[0])),
			"and the systems falling back to the folder are exactly the ones listed here");

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
			{ "dpad_up", COL_WHITE }, { "dpad_down", COL_WHITE },
			{ "dpad_ud", COL_WHITE }, { "dpad_lr", COL_WHITE },
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
		shelf_rewind();
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

	// The half-resolution option, and then the cost table - in that order, because the
	// table measures the 640x360 column through the very option the section proves.
	assert_half_canvas();
	assert_repaint_costs();

	// Straight after the cost table, because its rows are the money this section spends:
	// the angles-per-second claim is only honest next to what each painted frame costs.
	assert_disc_smoothness();

	assert_rip_format();
	// Directly after it, because it is the other half of the same feature and it needs the
	// state that one leaves: no rip in flight and the games folders back as they were.
	assert_rip_screen();

	assert_config_check();

	// After the two rip sections, because the copier is one of the three children this is
	// about; it touches no screen and needs no card, so it can sit anywhere after them.
	assert_children_are_collected();

	/*
	  The marquee, before the clipped-copy sweep rather than after it.

	  It has to be before, because the sweep reads the truncation log every section above it
	  left behind and this section's own clips belong in that reading: a marquee is still a
	  cut string, and the day a sentence of ours is switched over to one the guard has to
	  fail rather than shrug. It also has to be before assert_typography(), which moves the
	  advance the whole front-end draws at - its clips would be recorded against widths no
	  screen actually has.

	  The clips it makes on a screen arrive under the drawing function's name, exactly as they
	  would if a player had been looking - marq_fit() passes the caller's __func__ down for
	  that reason - so a long SSID lands as data under draw_listrow and the shelf title lands
	  in draw_title_block's existing allowance. Only the direct sweeps of gfx_marquee() itself
	  are skipped, under a site beginning "assert_", the same as assert_typography()'s two
	  hundred measurements.
	*/
	/*
	  Task 54's own section: one boundary rule for every list.

	  Here rather than beside the screens it drives, and after the wi-fi and controllers
	  sections rather than before them, because it needs what those two leave behind: a fake
	  `iw` and a fake bluetoothctl in PATH, without which the refresh children answer
	  "nothing there" and empty the two lists out from under the cursor. It sets up its own
	  radio, scan and pads on top of that and puts all three back.

	  Before the marquee and the clipped-copy sweep, like every other section that opens a
	  panel: the clips its walks make belong in the reading those two do.
	*/
	assert_uniform_wrap();

	assert_marquee();

	/*
	  Second to last: it reads the truncations every section above it recorded, so it has
	  to run after all of them - and before assert_typography(), which shifts the advance
	  the whole front-end draws at and then sweeps gfx_clip() across two hundred widths
	  of its own.
	*/
	assert_no_clipped_copy();
	// After the sweep above, which is what visits every screen at every profile.
	assert_no_row_is_hidden();

	// Last, because it changes text rendering globally. Anything measuring a width after
	// this runs would be measuring whatever tracking the section left behind.
	assert_typography();

	printf("\n== presents ==\n");
	printf("  page flips: %d\n", harness_present_count());
	check(harness_present_count() > 10, "the framebuffer was actually flipped");

	printf("\n%d checks, %d failures\n", checks, fails);
	printf("PNGs in %s\n", OUT);
	return fails ? 1 : 0;
}
