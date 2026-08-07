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

static void select_first_game()
{
	// Re-entry keeps the previous shelf position by design, so rewind to the
	// start before counting. LEFT clamps at index 0.
	for (int i = 0; i < 30; i++) press(KEY_LEFT, 2);

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
	for (int i = 0; i < 40; i++) press(KEY_LEFT, 2);

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
	  The ones that matter for the UI: identified, but this firmware has no shelf
	  system for them. "We know what it is" and "we can launch it" are different
	  questions, and this is the case that forces the player to be asked.
	*/
	check(disc_system_id(DISC_T_SATURN) == 0, "a Saturn disc is identified but has no core here");
	check(disc_system_id(DISC_T_CDI) == 0, "nor a CD-i disc");
	check(disc_system_id(DISC_T_AUDIO) == 0, "and an audio CD is nobody's game");
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
	harness_advance(60);                       // past the spin interval, nothing else due
	chome_handle(0);
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

	harness_advance(60);
	chome_handle(0);                           // one partial, into the other buffer
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

	harness_advance(60);
	chome_handle(0);                           // one spin frame, the partial path
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
	for (int i = 0; i < 40; i++) press(KEY_LEFT, 2);
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
	for (int i = 0; i < 40; i++) press(KEY_LEFT, 2);
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
	  Back onto Options, then right again - which has nowhere to go.

	  Asserted through A rather than through the legend, deliberately: a refused press paints
	  the line above the legend red, and this UI only repaints what changed, so that line is
	  still red the next time anything reads those pixels. What A does is not ambiguous like
	  that - if right had wrapped round to the first button the disc would have launched and
	  this screen would be gone.
	*/
	press(KEY_RIGHT);
	press(KEY_RIGHT);
	press(KEY_ENTER);
	check(chome_screen_id() == S_DISC,
		"right stops at the second button, and A there opens the core chooser "
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
			  Everything but the disc, which turns. The disc's box is cut out with two pixels
			  of margin for the soft rim, and its diameter and centre are asserted above - so
			  the disc is still pinned, just not the frame of its rotation.

			  The first version of this hashed the disc too and broke when a section was added
			  elsewhere in the harness: the extra clock advances caught the disc at a different
			  angle. The pixels that moved were the disc's 96x96 box exactly, measured, which is
			  how we know the dialog itself had not moved.
			*/
			unsigned long hash = harness_fb_hash_box_except(ox, oy, ox + ow, oy + oh,
				dcx - dia / 2 - 2, dcy - dia / 2 - 2,
				dcx + dia / 2 + 2, dcy + dia / 2 + 2);
			printf("  240p dialog plate hash (disc cut out) %lu\n", hash);
			check(hash == 8834752626955163003UL, "240p is pixel for pixel the dialog it was");
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

	/* -------------------------------------------------------- system ids --- */

	const char *id = ss_system_id("psx", "Destruction Derby (USA).cue");
	check(id && !strcmp(id, "57"), "psx maps to systemeid 57");

	id = ss_system_id("nes", "Zelda.nes");
	check(id && !strcmp(id, "3"), "nes maps to 3");

	id = ss_system_id("SNES", "Metroid.sfc");
	check(id && !strcmp(id, "4"), "the system id is matched case-insensitively");

	// The two that ride in another core's shelf and are a different platform to the API.
	id = ss_system_id("gb", "Tetris.gb");
	check(id && !strcmp(id, "9"), "a .gb in the Game Boy shelf is 9");

	id = ss_system_id("gb", "Zelda DX.gbc");
	check(id && !strcmp(id, "10"), "and a .gbc in the same shelf is 10, not 9");

	/*
	  Game Gear rides in the Master System shelf and its systemeid is not one of the
	  values we were able to verify. Returning nothing is the whole point: the
	  tempting alternative - fall back to the Master System id - would scrape .gg
	  games as Master System and put the wrong covers on the shelf silently.
	*/
	check(ss_system_id("sms", "Sonic.gg") == 0,
		"a .gg is refused rather than scraped as Master System");
	id = ss_system_id("sms", "Sonic.sms");
	check(id && !strcmp(id, "2"), "while a real .sms is still 2");

	check(ss_system_id("c64", "game.d64") == 0,
		"a system whose id we never verified returns nothing rather than a guess");
	check(ss_system_id("", "x.nes") == 0, "an empty system id is refused");

	/* ---------------------------------------------------- override file --- */

	put_file("/tmp/chome_ss_sys.cfg",
		"# a comment\n"
		"\n"
		"c64 = 66\n"
		"psx=999\n"                 // deliberately overrides a built-in
		"bogus=notanumber\n"        // must be ignored: this goes into a URL
		"noequals\n");

	check(ss_systems_load("/tmp/chome_ss_sys.cfg") == 2,
		"the override file takes two good lines and drops the junk");

	id = ss_system_id("c64", "game.d64");
	check(id && !strcmp(id, "66"), "an override fills a gap in the built-in table");

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

	strcpy(cfg.classicui_ss_user, "dinofly");
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
	check(strstr(url, "ssid=dinofly") != 0, "the user's own account is sent");

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
	strcpy(cfg.classicui_ss_user, "dinofly");
	check(ss_enabled() == 1, "on, with an account, is on");
	cfg.classicui_screenscraper = 0;
	check(ss_enabled() == 0, "and the option itself turns it off again");

	/* ------------------------------------------------------ classifying --- */

	check(ss_http_class(200) == SS_OK, "200 is fine");
	check(ss_http_class(404) == SS_ERR_NOTFOUND, "404 is a game we do not have");
	check(ss_http_class(403) == SS_ERR_CREDENTIALS, "403 is our credentials");
	check(ss_http_class(429) == SS_ERR_THREADS, "429 is too many at once");
	check(ss_http_class(430) == SS_ERR_QUOTA, "430 is the daily quota");
	check(ss_http_class(431) == SS_ERR_BLACKLISTED, "431 is too many unmatched roms");
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
	strcpy(cfg.classicui_ss_user, "dinofly");

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

	strcpy(cfg.classicui_ss_user, "dinofly");
	check(disc_art_request("SLES-01506", "c64", 0) == 0,
		"nor for a system whose systemeid we never verified");
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
	strcpy(cfg.classicui_ss_user, "dinofly");

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
	int lynx    = item_by_path("AtariLynx", "Chip's Challenge (USA).lnx");

	check(metroid >= 0 && smwjp >= 0 && ddragon >= 0 && bonk >= 0 && fusion >= 0 && lynx >= 0,
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
		art_state(lynx) != ART_READY, "and the other five have no cover anywhere on it");

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
	  A system whose systemeid we never verified cannot be asked at all, and must fall to
	  the pack rather than becoming a rung that is offered and then quietly refuses.
	  Atari Lynx has a libretro name and no ScreenScraper id, so it is exactly that case.
	*/
	check(art_next_source(lynx) == ART_SRC_LIBRETRO,
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
	  Core Settings is the row above Close Game. Counted downwards from the top, so a row
	  added anywhere above it moves this count - which is exactly what happened when Online
	  Covers was inserted under Cover Art, and the four checks after this walk are what
	  said so. Left counting downwards on purpose: assert_ingame() reaches the last row by
	  wrapping upwards instead, so between the two of them an inserted row is certain to
	  break one rather than sliding quietly past both.
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
	for (int i = 0; i < 60; i++) press(KEY_LEFT, 2);

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
	/*
	  Read at the band's own row, which is theme safe_y and not zero. This used to scan row
	  0 and passed anyway, because the harness runs at 720p where safe_y is 0 - so it could
	  not have caught the band being drawn outside the safe area, and it did not. A CRT did,
	  where 6% of 240 lines put the whole band behind the bezel.
	*/
	frame(6);
	check(band_red_at(theme_get()->safe_y),
		"and warns across the top that the game is still playing");
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
	check(band_red_at(p->safe_y), "the still-playing band is inside the safe area");
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
		press(KEY_RIGHT, 10);                 // Power
		press(KEY_RIGHT, 10);                 // About
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
		press(KEY_UP, 8);                     // wrap to the last row
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

	/* ---------------------------------- what the other three cores' folders do NOT get --- */

	/*
	  Ripping a Mega CD disc writes a folder the *core* can load and the *shelf* cannot see,
	  and that is worth a check rather than a footnote, because it is the one place this
	  feature is knowingly incomplete.

	  The md shelf entry accepts "md,bin,gen" and not "cue", so the sheet is not a game to
	  it; and 73b0f71's rule then correctly hides the tracks beside that sheet, because a
	  Mega CD track handed to the Genesis core as a cartridge was never going to boot. So a
	  Mega CD rip yields no card at all and has to be loaded from the core's own file
	  browser. Adding "cue" to that entry is not the fix - md launches the Genesis core with
	  a load-to-memory mount, and a .cue card there would fail when pressed. The fix is a
	  shelf route to the MegaCD core, which disc_playables already has the slot for and is
	  its own piece of work.

	  This check exists so that whoever does it finds out here rather than from a player.
	*/
	{
		const char *gname = "Mega Test Disc";
		char gdir[1024];
		snprintf(gdir, sizeof(gdir), "%s/%s", ROOT "/games/Genesis", gname);
		rip_rmdir_flat(gdir);

		fk.reads = 0;
		check(rip_perform(&plan, ROOT "/games/Genesis", gname, 1, 0, &io, &bad) == RIP_DONE,
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
		check(item_at("md", rel) < 0,
			"and it is NOT on the shelf: md accepts no cue, so this one has to be loaded "
			"from the core's own browser until md gains a route to the MegaCD core");
		snprintf(rel, sizeof(rel), "%s/Track 01.bin", gname);
		check(item_at("md", rel) < 0, "and its tracks are hidden rather than listed as games");

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
	*/
	for (int i = 0; i < 8; i++) press(KEY_DOWN, 6);
	dump("rip-01-options");

	check(rip_test_starts() == 0, "nothing has been started yet");
	press(KEY_ENTER, 8);

	check(rip_test_starts() == 1, "pressing the copy row starts a rip");
	check(!strcmp(rip_test_last_dir(), ROOT "/games/PSX"),
		"into the PlayStation games folder, which is where the shelf looks for PlayStation games");
	check(!strcmp(rip_test_last_name(), "Ridge Racer") ||
		!strcmp(rip_test_last_name(), "SLUS-00626"),
		"in a folder named for the disc by the best name anything knows it by");
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

int main()
{
	printf("Classic Home host harness\n\n");

	build_sd();
	harness_set_root(ROOT);

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
	assert_physical_disc();
	// Directly after it, because it drives the same state machine with the same fake
	// discs, and before every section that draws or logs a disc name: it puts a title
	// table on the card and takes it away again, and anything running in between would
	// see disc_display_name() answer differently. See its own comment.
	assert_disc_titles();
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
	assert_per_game_core_options();
	assert_core_option_for_all_games();
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
		strcpy(cfg.classicui_ss_user, "dinofly");
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
		press(KEY_E, 6);
		press(KEY_R, 6);
		press(KEY_E, 6);
		press(KEY_K, 6);
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
		check(strstr(logtext, "classicui_ss_user = dinofly") != 0,
			"while the login, which is not a secret, is logged as itself");
		unlink(logpath);

		// And the mechanism on its own, since a future caller of the writer inherits it
		// without knowing it exists.
		check(!strcmp(ini_loggable("classicui_ss_pass", "hunter2"), "***"),
			"the password key is unloggable whoever writes it");
		check(!strcmp(ini_loggable("CLASSICUI_SS_PASS", "hunter2"), "***"),
			"and however MiSTer.ini happens to spell it");
		check(!strcmp(ini_loggable("classicui_ss_user", "dinofly"), "dinofly"),
			"and nothing else is redacted, or the log would stop being worth reading");
		check(!strcmp(ini_loggable("vscale_mode", "1"), "1"), "least of all a number");

		dump("covers-8-saved");

		check(cfg.classicui_screenscraper == 1, "the second press tells the running firmware");
		check(!strcmp(cfg.classicui_ss_user, "dinofly"), "the account name reaches cfg");
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
		check(strstr(now, "[MiSTer]\r\nclassicui_ss_user=dinofly\r\nclassicui_ss_pass=hunt3\r\n") != 0,
			"and the two new keys are under a [MiSTer] header, not left in [NES]");

		const char *nes = strstr(now, "[NES]");
		const char *acct = strstr(now, "classicui_ss_user=dinofly");
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
		strcpy(cfg.classicui_ss_user, "dinofly");
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

	assert_rip_format();
	// Directly after it, because it is the other half of the same feature and it needs the
	// state that one leaves: no rip in flight and the games folders back as they were.
	assert_rip_screen();

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
