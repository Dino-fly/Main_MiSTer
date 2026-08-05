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
#include "../chome_gamelist.h"
#include "../chome_ss.h"
#include "../chome_disc.h"
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
	select_folder("Systems");
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
	// centre at safe_x+inset+r, 17 cells of r/16 pixels each side.
	const chome_profile *p = theme_get();
	int r = (p->ts_ui >= 2) ? 32 : 16;
	int cell = r / 16;
	int bx0 = p->safe_x + p->inset + r - 17 * cell;
	int by0 = p->safe_y + p->inset + r - 17 * cell;
	int bx1 = bx0 + 34 * cell, by1 = by0 + 34 * cell;
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
	check(gfx_damage_rows() <= 34 * cell, "a spin frame damages only the disc's rows");

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
	check(gfx_damage_rows() <= 34 * cell, "the frame under comparison took the partial path");
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
	check(gfx_damage_rows() <= 34 * cell, "and it was partial");
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
  Launching a physical disc, end to end up to the MGL - the part a fake drive can
  prove. The sentinel and the slot in the written MGL are the two things menu.cpp and
  psx.cpp key on, so they are checked as text; whether the core then reads sectors is
  hardware's question, not this file's.

  And the refusal: a disc whose core is not wired (Mega CD here) is named on the
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
	fake_put(&dm, 0, 0, "SEGADISCSYSTEM", 14, 0);
	disc_set_reader(fake_read, &dm);
	disc_ingest_identify(0);
	frame(6);
	check(disc_state() == DISC_READY && disc_type() == DISC_T_MEGACD,
		"a Mega CD disc is identified");

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

	cfg.classicui_ss_user[0] = 0;
	cfg.classicui_ss_pass[0] = 0;
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

// The rows the legend occupies, which is where its button prompts and nothing else are.
static int legend_colour(uint32_t want)
{
	const chome_profile *p = theme_get();
	return box_pixels(0, p->y_legend - 6 * p->ts_ui, p->w, p->h, want);
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
	assert_physical_disc();
	assert_disc_ui();
	assert_partial_repaint();
	assert_disc_launch();
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
