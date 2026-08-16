#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "chome_lib.h"
#include "chome_video.h"
#include "../../file_io.h"
#include "../../lib/miniz/miniz.h"
#include "../neogeo/neogeo_loader.h"

static int is_dir_abs(const char *path)
{
	struct stat st;
	if (stat(path, &st)) return 0;
	return S_ISDIR(st.st_mode) ? 1 : 0;
}

/* ------------------------------------------------------------- systems ---- */

static chome_sys systems[CH_MAX_SYS];
static int nsys = 0;

struct sys_def
{
	const char *id, *name, *badge, *rbf, *dir, *ext, *lr;
	char type;
	int index, delay, computer, mra;
	uint32_t tint;
	int romset;          // last, so the rows above keep their positional layout
	int savestates;      // CH_SS_*, and after romset for the same reason
	/*
	  MGL <setname>: re-homes a shared core, so its games folder, config and
	  savestates take this name instead of the core's own. Game Gear is the case
	  that needs it: same SMS core, games in games/GameGear. Trailing so only
	  the rows that use it carry it - everything else aggregates to 0.
	*/
	const char *setname;
};

/*
  type/index follow each core's CONF_STR. Where a core exposes a single ROM slot
  the answer is 'f'/0, which covers most consoles. CD-based and computer cores
  are the ones worth double checking.

  The trailing pair is romset then savestates. The savestate answers were measured
  by loading each core and reading its CONF_STR (2026-08-02), so they describe the
  cores on the card rather than MiSTer in general - a core rebuilt from a newer
  upstream can gain save states without this file being touched, which is why a
  loaded core's own answer always outranks this one. Systems left at CH_SS_UNKNOWN
  were not measured; do not guess them from what the hardware "should" do.

  Master System was CH_SS_UNKNOWN and is CH_SS_YES: it gained save states upstream over
  May-July 2026 and the dump in docs/confstr/SMS.txt shows them - SS3E000000:18000, a
  SaveState Slot option, and Save/Load State entries.

  And why a loaded core is judged by its save and load *labels* rather than by the SS
  entry, which looks like the obvious test: N64 declares SS3C000000:1000000 and ships a
  whole savestates.vhd, but its save path is hardwired off in N64.sv - `.save_state(0)`,
  commented out, that way since the core's first release - and its CONF_STR carries no
  Save or Load entry at all. Trusting the SS entry would offer N64 save states that can
  never be written. docs/confstr/N64.txt is the evidence.
*/
static const sys_def defaults[] =
{
	{ "nes",   "Nintendo Entertainment System", "NES",  "_Console/NES",          "NES",     "nes,fds,nsf",  "Nintendo - Nintendo Entertainment System",       'f', 0, 2, 0, 0, 0x8a4b2b, 0, CH_SS_YES     },
	{ "snes",  "Super Nintendo",                "SNES", "_Console/SNES",         "SNES",    "sfc,smc",      "Nintendo - Super Nintendo Entertainment System", 'f', 0, 2, 0, 0, 0x5b4b8a, 0, CH_SS_YES     },
	{ "gb",    "Game Boy",                      "GB",   "_Console/Gameboy",      "GAMEBOY", "gb,gbc",       "Nintendo - Game Boy",                            'f', 0, 2, 0, 0, 0x3c6e4a, 0, CH_SS_YES     },
	{ "gba",   "Game Boy Advance",              "GBA",  "_Console/GBA",          "GBA",     "gba",          "Nintendo - Game Boy Advance",                    'f', 0, 2, 0, 0, 0x4a3c8a, 0, CH_SS_YES     },
	{ "n64",   "Nintendo 64",                   "N64",  "_Console/N64",          "N64",     "n64,z64,v64",  "Nintendo - Nintendo 64",                         'f', 0, 3, 0, 0, 0x2b5e8a, 0, CH_SS_NO      },
	{ "md",    "Mega Drive",                    "MD",   "_Console/Genesis",      "Genesis", "md,bin,gen",   "Sega - Mega Drive - Genesis",                    'f', 0, 2, 0, 0, 0x2b4c7e, 0, CH_SS_NO      },
	/*
	  The MGL slot index is the DIGIT in the core's file entry ("FS2,GG" wants
	  index 2), not the entry's position - and an index that matches no entry
	  falls through silently to the FIRST file entry, which is how Game Gear
	  games spent a while loading into the Master System slot and running in
	  the wrong video mode. Measured on the device: SMS publishes
	  "H8FS1,SMSSG SC" and "H8FS2,GG"; index 0 and 1 both land in FS1, only
	  index 2 reaches the GG slot (native 160x144). So the SMS row says 1
	  explicitly, and Game Gear gets a row of its own: same core, its own games
	  folder, the GG slot, its own art and video class. A .gg filed under
	  games/SMS is re-slotted at launch - see do_launch().
	*/
	{ "sms",   "Master System",                 "SMS",  "_Console/SMS",          "SMS",     "sms,gg,sg",    "Sega - Master System - Mark III",                'f', 1, 2, 0, 0, 0x7e3a2b, 0, CH_SS_YES     },
	{ "gg",    "Game Gear",                     "GG",   "_Console/SMS",          "GameGear","gg",           "Sega - Game Gear",                               'f', 2, 2, 0, 0, 0x24345e, 0, CH_SS_YES, "GameGear" },
	{ "tg16",  "TurboGrafx-16",                 "TG16", "_Console/TurboGrafx16", "TGFX16",  "pce,sgx",      "NEC - PC Engine - TurboGrafx 16",                'f', 0, 2, 0, 0, 0x8a6e2b, 0, CH_SS_NO      },
	{ "a7800", "Atari 7800",                    "A78",  "_Console/Atari7800",    "A7800",   "a78,a26,bin",  "Atari - 7800",                                   'f', 0, 2, 0, 0, 0x6e2b2b, 0, CH_SS_NO      },
	{ "psx",   "PlayStation",                   "PSX",  "_Console/PSX",          "PSX",     "cue,chd,exe",  "Sony - PlayStation",                             's', 1, 3, 0, 0, 0x4a4c58, 0, CH_SS_YES     },

	/*
	  The CD consoles, each one its own system rather than an extra extension on the
	  cartridge machine it sits inside.

	  Three of them share a shelf row's worth of hardware with an entry above - Mega CD
	  with Mega Drive, PC Engine CD with TurboGrafx-16, Neo Geo CD with Neo Geo - and
	  adding "cue" to those rows instead was tried and is wrong: `md` launches the Genesis
	  core with an 'f' load-to-memory mount, and a .cue card there would draw, sort and
	  scrape like a game and then fail the moment it was pressed. A disc is a different
	  core, a different slot and a different games folder, so it is a different row.

	  The games folders are the official MiSTer Distribution's, which is the whole point:
	  someone who downloads a romset drops it into games/MegaCD or games/TGFX16-CD without
	  being told to, and a disc copied off the drive has to land where a download would.
	  Two of them are also fixed in the firmware rather than chosen here - PCECD_DIR in
	  support/pcecd/pcecd.h and NEOCD_DIR in support/neogeo/neogeocd.h - and those two are
	  where the core itself looks for its BIOS.

	  `ext` is cue,chd and nothing else, and the temptation to widen it has a cost that is
	  not obvious: these folders hold the cores' BIOS images (TGFX16-CD/cd_bios.rom,
	  NeoGeo-CD/neocd.bin and top-sp1.bin, Saturn/boot.rom). Any extension that reaches one
	  of those puts a card on the shelf that looks like a game, scrapes like a game and
	  cannot boot. "bin" is the dangerous one and it is deliberately absent - a rip's
	  "Track 01.bin" is already hidden beside its sheet by dir_has_playlist(), so nothing
	  needs it.

	  Save states: pcecd and neogeocd load the *same rbf* as tg16 and neogeo above, whose
	  CH_SS_NO was read off the loaded core, so they carry the same measured answer rather
	  than a guess. MegaCD and Saturn are separate cores that nobody has loaded and read,
	  so they stay CH_SS_UNKNOWN and promise nothing either way.

	  The slots, each from the code that consumes them:

	    megacd    "S0,CUECHD,Insert Disk" in MegaCD.sv, the same slot disc_playables in
	              chome_ui.cpp hands a pressed disc to.
	    pcecd     "S0,CUECHD,Insert CD" in TurboGrafx16.sv. The core is shared with the
	              HuCard row above, which is 'f'/0 - the type is what tells them apart.
	    neogeocd  "S1,CUECHD,Load CD Image" in neogeo.sv. Index 1 is also the romset slot
	              ("FS1,*,Load ROM set") that `neogeo` above uses, and that one is type
	              'f': menu.cpp routes an 's' mount on this core to neocd_set_image() and
	              an 'f' one to the romset loader, so the two rows cannot collide.
	    saturn    index 0, from menu.cpp rather than from a .sv this tree does not carry:
	              MENU_GENERIC_IMAGE_SELECTED calls saturn_set_image() when ioctl_index is
	              0 and saturn_mount_save() for anything else, and the browser marks index
	              1 as SCANO_SAVES. Index 1 is the backup RAM, so a disc sent there would
	              be mounted as a save file.
	*/
	{ "megacd","Mega CD",                       "MCD",  "_Console/MegaCD",       "MegaCD",   "cue,chd",     "Sega - Mega-CD - Sega CD",                       's', 0, 3, 0, 0, 0x3a6e8a, 0, CH_SS_UNKNOWN },
	{ "pcecd", "PC Engine CD",                  "PCD",  "_Console/TurboGrafx16", "TGFX16-CD","cue,chd",     "NEC - PC Engine CD - TurboGrafx-CD",             's', 0, 3, 0, 0, 0x8a4e3a, 0, CH_SS_NO      },
	{ "neogeocd","Neo Geo CD",                  "NCD",  "_Console/NeoGeo",       "NeoGeo-CD","cue,chd",     "SNK - Neo Geo CD",                               's', 1, 3, 0, 0, 0x4a2030, 0, CH_SS_NO      },
	{ "saturn","Saturn",                        "SAT",  "_Console/Saturn",       "Saturn",   "cue,chd",     "Sega - Saturn",                                  's', 0, 3, 0, 0, 0x4a3a5e, 0, CH_SS_UNKNOWN },

	/*
	  Neo Geo games are romsets rather than ROM files: the archive is loaded whole and
	  named for the board, so `romset` sends titles through the firmware's romsets.xml
	  lookup. FS1 in the core's CONF_STR is why this is index 1, and the longer delay
	  is for the core to come up before a romset of tens of megabytes follows it.
	*/
	{ "neogeo","Neo Geo",                       "NEO",  "_Console/NeoGeo",       "NEOGEO",  "zip,neo",      "SNK - Neo Geo",                                  'f', 1, 3, 0, 0, 0x8a2b2b, 1, CH_SS_NO      },
	{ "arcade","Arcade",                        "ARC",  "",                      "_Arcade", "mra",          "MAME",                                           'f', 0, 0, 0, 1, 0x7e2b3a, 0, CH_SS_UNKNOWN },

	// Handhelds. Game Gear rides in the SMS core above (.gg), and GBC in the Game
	// Boy core (.gbc); both are told apart by extension at launch.
	{ "lynx",  "Atari Lynx",                    "LNX",  "_Console/AtariLynx",    "AtariLynx","lnx",         "Atari - Lynx",                                   'f', 0, 2, 0, 0, 0x2a2a2e, 0, CH_SS_YES     },
	{ "ws",    "WonderSwan",                    "WS",   "_Console/WonderSwan",   "WonderSwan","ws,wsc",     "Bandai - WonderSwan",                            'f', 0, 2, 0, 0, 0x2f3a5a, 0, CH_SS_YES     },
	{ "ngp",   "Neo Geo Pocket Color",          "NGP",  "_Console/NeoGeo-Pocket","NGP",     "ngp,ngc,npc",  "SNK - Neo Geo Pocket Color",                     'f', 0, 2, 0, 0, 0x20304a, 0, CH_SS_NO      },

	{ "amiga", "Amiga",                         "AMI",  "_Computer/Minimig",     "Amiga",   "adf,hdf",      "Commodore - Amiga",                              'f', 0, 3, 1, 0, 0x2e6e63, 0, CH_SS_NO      },
	{ "st",    "Atari ST",                      "ST",   "_Computer/AtariST",     "AtariST", "st,msa,img",   "Atari - ST",                                     's', 0, 3, 1, 0, 0x3a5e6e, 0, CH_SS_NO      },
	{ "c64",   "Commodore 64",                  "C64",  "_Computer/C64",         "C64",     "d64,g64,prg,crt","Commodore - 64",                               'f', 1, 3, 1, 0, 0x4a5e2b, 0, CH_SS_NO      },
	{ "spec",  "ZX Spectrum",                   "SPE",  "_Computer/ZX-Spectrum", "Spectrum","tap,tzx,z80,trd","Sinclair - ZX Spectrum",                       'f', 1, 3, 1, 0, 0x6e2e5e, 0, CH_SS_NO      },
	{ "cpc",   "Amstrad CPC",                   "CPC",  "_Computer/Amstrad",     "Amstrad", "dsk,cdt",      "Amstrad - CPC",                                  's', 0, 3, 1, 0, 0x2b3a6e, 0, CH_SS_NO      },
	{ "msx",   "MSX",                           "MSX",  "_Computer/MSX",         "MSX",     "rom,dsk,cas",  "Microsoft - MSX",                                'f', 1, 3, 1, 0, 0x6e4a2b, 0, CH_SS_NO      },
	{ "ao486", "PC / DOS",                      "DOS",  "_Computer/ao486",       "AO486",   "img,vhd,ima",  "DOS",                                            's', 0, 4, 1, 0, 0x4a4c58, 0, CH_SS_NO      },
	{ "apple2","Apple II",                      "AII",  "_Computer/Apple-II",    "Apple-II","dsk,nib,po",   "Apple - II",                                     'f', 0, 3, 1, 0, 0x5e5e5e, 0, CH_SS_NO      },
};

/*
  Video class, from the system id. Handhelds get their own LCD looks, arcade and
  consoles a PVM look, 15 kHz home computers a TV look, and 31 kHz machines a
  clean one. The systems file can override this with a 13th field.
*/
static int vclass_for(const char *id, int computer, int mra)
{
	if (!strcasecmp(id, "gb")) return VC_GB;
	if (!strcasecmp(id, "gbc")) return VC_GBC;
	if (!strcasecmp(id, "gba")) return VC_GBA;
	if (!strcasecmp(id, "gg")) return VC_GG;
	if (!strcasecmp(id, "lynx")) return VC_LYNX;
	if (!strcasecmp(id, "ws")) return VC_WS;
	if (!strcasecmp(id, "ngp")) return VC_NGPC;
	if (mra || !strcasecmp(id, "neogeo")) return VC_ARCADE;
	if (!strcasecmp(id, "ao486") || !strcasecmp(id, "x86") || !strcasecmp(id, "archie")) return VC_VGA;
	if (computer) return VC_COMPUTER;
	return VC_CONSOLE;
}

static void add_sys(const sys_def *d)
{
	if (nsys >= CH_MAX_SYS) return;
	chome_sys *s = &systems[nsys++];
	memset(s, 0, sizeof(*s));
	snprintf(s->id, sizeof(s->id), "%s", d->id);
	snprintf(s->name, sizeof(s->name), "%s", d->name);
	snprintf(s->badge, sizeof(s->badge), "%s", d->badge);
	snprintf(s->rbf, sizeof(s->rbf), "%s", d->rbf);
	snprintf(s->dir, sizeof(s->dir), "%s", d->dir);
	snprintf(s->ext, sizeof(s->ext), "%s", d->ext);
	snprintf(s->lr, sizeof(s->lr), "%s", d->lr);
	s->type = d->type;
	s->index = d->index;
	s->delay = d->delay;
	s->computer = d->computer;
	s->mra = d->mra;
	s->romset = d->romset;
	s->savestates = d->savestates;
	s->vclass = vclass_for(d->id, d->computer, d->mra);
	s->tint = 0xff000000u | d->tint;
	if (d->setname) snprintf(s->setname, sizeof(s->setname), "%s", d->setname);
}

static char *trim(char *s)
{
	while (*s == ' ' || *s == '\t') s++;
	char *e = s + strlen(s);
	while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
	return s;
}

// id | name | badge | rbf | dir | exts | lr name | type | index | delay | computer | tint
//   | look | savestates
static int load_systems_file()
{
	char path[1024];
	snprintf(path, sizeof(path), "%s/classicui_systems.txt", getRootDir());

	FILE *f = fopen(path, "rt");
	if (!f) return 0;

	printf("ClassicUI: loading systems from %s\n", path);
	nsys = 0;

	char line[1024];
	while (fgets(line, sizeof(line), f))
	{
		char *p = trim(line);
		if (!*p || *p == '#') continue;

		char *fld[14] = {};
		int n = 0;
		char *tok = p;
		while (n < 14)
		{
			char *bar = strchr(tok, '|');
			if (bar) *bar = 0;
			fld[n++] = trim(tok);
			if (!bar) break;
			tok = bar + 1;
		}
		if (n < 7) continue;

		sys_def d = {};
		d.id = fld[0]; d.name = fld[1]; d.badge = fld[2]; d.rbf = fld[3];
		d.dir = fld[4]; d.ext = fld[5]; d.lr = fld[6];
		d.type = (n > 7 && (fld[7][0] == 's' || fld[7][0] == 'S')) ? 's' : 'f';
		d.index = (n > 8) ? atoi(fld[8]) : 0;
		d.delay = (n > 9) ? atoi(fld[9]) : 2;
		d.computer = (n > 10) ? atoi(fld[10]) : 0;
		d.mra = 0;
		d.tint = (n > 11) ? (uint32_t)strtoul(fld[11], NULL, 16) : 0x4a4c58;
		if (!strcasecmp(d.dir, "_Arcade")) d.mra = 1;
		add_sys(&d);
		if (n > 12 && fld[12][0]) systems[nsys - 1].vclass = vp_class_from_name(fld[12]);

		/*
		  Spelled out rather than numbered, and anything else - including the field being
		  absent, which is every file written before this existed - leaves the system
		  unknown. A file that overrides the table replaces it wholesale, so without this
		  the built-in measurements would be lost along with the values being corrected.
		*/
		if (n > 13)
		{
			if (!strcasecmp(fld[13], "yes")) systems[nsys - 1].savestates = CH_SS_YES;
			else if (!strcasecmp(fld[13], "no")) systems[nsys - 1].savestates = CH_SS_NO;
		}
	}

	fclose(f);
	return nsys > 0;
}

int lib_sys_count() { return nsys; }
const chome_sys *lib_sys(int i) { return (i >= 0 && i < nsys) ? &systems[i] : 0; }

/*
  Where each system's games are, remembered once.

  Not tidiness: this is asked several times per shelf move - item_on_card() checks the
  twenty cards the view can show, the art ladder asks for every card it draws - and
  answering it means findGamesDir(), which walks a list of candidate roots with a stat
  each. Two of those candidates are `../network/...` and `/media/fat/cifs/...`, so a
  card with a stale mount pays a network timeout per question, and every hit prints
  "Found dir:" to a log that is on the SD card. Dinofly saw the sum of it as the picture
  wobbling while he moved along the shelf, and only while he moved.

  The answer cannot change without the library being told: a stick appearing or a
  systems file being reloaded both come through the two invalidation points below.
*/
static char gd_cache[CH_MAX_SYS][1024];
static char gd_state[CH_MAX_SYS];        // 0 unknown, 1 found, 2 no such directory

static int lib_sys_games_dir_uncached(int sysidx, char *out, int len);

void lib_forget_dirs()
{
	memset(gd_state, 0, sizeof(gd_state));
}

int lib_sys_games_dir(int sysidx, char *out, int len)
{
	const chome_sys *s = lib_sys(sysidx);
	if (!s) return 0;

	if (sysidx >= 0 && sysidx < CH_MAX_SYS && gd_state[sysidx])
	{
		if (gd_state[sysidx] == 2) return 0;
		snprintf(out, len, "%s", gd_cache[sysidx]);
		return 1;
	}

	int found = lib_sys_games_dir_uncached(sysidx, out, len);

	if (sysidx >= 0 && sysidx < CH_MAX_SYS)
	{
		gd_state[sysidx] = found ? 1 : 2;
		if (found) snprintf(gd_cache[sysidx], sizeof(gd_cache[sysidx]), "%s", out);
	}
	return found;
}

static int lib_sys_games_dir_uncached(int sysidx, char *out, int len)
{
	const chome_sys *s = lib_sys(sysidx);
	if (!s) return 0;

	if (s->mra)
	{
		snprintf(out, len, "%s/%s", getRootDir(), s->dir);
		return is_dir_abs(out);
	}

	char dir[1024];
	snprintf(dir, sizeof(dir), "%s", s->dir);
	if (findGamesDir(dir, sizeof(dir)))
	{
		/*
		  findGamesDir() answers relative to the SD root - "games/SNES", or "../usb0/..."
		  for a stick - because MiSTer's own file helpers prepend the root themselves.
		  Everything here hands the result to stat() and opendir(), which resolve against
		  the process working directory instead.

		  So it only ever worked when the firmware happened to have been started from
		  /media/fat. init starts it from / (`::sysinit:/media/fat/MiSTer &`), which is
		  why a real boot found every ROM system empty while Arcade - the branch above,
		  which builds an absolute path - was fine. Every apparently working scan was a
		  manual `cd /media/fat && ./MiSTer` restart.

		  Prepending the root also resolves the USB form correctly:
		  /media/fat/../usb0/games/SNES is /media/usb0/games/SNES.
		*/
		if (dir[0] == '/') snprintf(out, len, "%s", dir);
		else snprintf(out, len, "%s/%s", getRootDir(), dir);

		return is_dir_abs(out);
	}
	return 0;
}

/* --------------------------------------------------------------- items ---- */

static chome_item *items = 0;
static int nitems = 0;

int lib_item_count() { return nitems; }
chome_item *lib_item(int i) { return (i >= 0 && i < nitems) ? &items[i] : 0; }

static uint32_t hash32(const char *s)
{
	uint32_t h = 2166136261u;
	while (*s) { h ^= (unsigned char)*s++; h *= 16777619u; }
	return h;
}

/*
  Zipped ROMs need no unpacking. The firmware's file layer reads straight through
  an archive - fileTYPE carries an inflate iterator and FileSeek rewinds it when
  something seeks backwards - so a core is handed plain ROM bytes and never learns
  it was compressed. The classic browser exposes the same thing by walking into a
  .zip as though it were a folder, and the path it produces, "Game.zip/Game.sfc",
  is exactly what the loader takes.

  Indexing an archive therefore means looking inside for something this system can
  run. Only the top level is considered: MiSTer rejects nested archives, and a zip
  holding a folder tree is a romset rather than a game.
*/
#define ZIP_MAX_ENTRIES 8

static int ext_matches(const char *name, const char *list);

static int is_zip_name(const char *name)
{
	size_t l = strlen(name);
	return (l > 4 && !strcasecmp(name + l - 4, ".zip"));
}

// Systems whose games *are* archives - a Neo Geo romset is loaded whole - must not
// be peeked into: the zip itself is the item.
static int sys_takes_zip(int sysidx)
{
	return ext_matches("_.zip", systems[sysidx].ext);
}

static int zip_playables(const char *zippath, const char *extlist,
                         char inner[][CH_PATH_LEN], int max)
{
	mz_zip_archive z;
	memset(&z, 0, sizeof(z));
	if (!mz_zip_reader_init_file(&z, zippath, 0)) return 0;

	int n = 0;
	mz_uint total = mz_zip_reader_get_num_files(&z);
	for (mz_uint i = 0; i < total && n < max; i++)
	{
		if (mz_zip_reader_is_file_a_directory(&z, i)) continue;

		char name[CH_PATH_LEN];
		if (!mz_zip_reader_get_filename(&z, i, name, sizeof(name))) continue;
		if (strchr(name, '/')) continue;
		if (!ext_matches(name, extlist)) continue;

		snprintf(inner[n++], CH_PATH_LEN, "%s", name);
	}

	mz_zip_reader_end(&z);
	return n;
}

/*
  Romsets. The set is named for the board, so the shelf would otherwise read like a
  MAME listing; romsets.xml carries the real titles and the firmware already knows
  how to read it. Same convention as the classic browser (file_io.cpp): the key
  going in is the name without its extension, and the reply is the title, NULL when
  the set is unlisted - the board name then has to do - or -1 when the file marks it
  as one to hide, which is how the BIOS set stays off the shelf.

  Returns 0 for the hidden case, meaning "do not list this at all".
*/
static const char *romset_title(const char *dir, const char *name, char *buf, int len)
{
	snprintf(buf, len, "%s", name);
	char *dot = strrchr(buf, '.');
	if (dot && !strcasecmp(dot, ".zip")) *dot = 0;

	char *alt = neogeo_get_altname((char*)dir, (char*)name, buf);
	if (alt == (char*)-1) return 0;
	return alt ? alt : buf;
}

// Is this folder a romset rather than a folder of games? A Darksoft set is loose
// member files, and the loader asks for these two first (neogeo_loader.cpp).
static int dir_is_romset(const char *path)
{
	static const char *const member[] = { "prom", "p1rom", "romset.xml" };

	for (size_t i = 0; i < sizeof(member) / sizeof(member[0]); i++)
	{
		char p[1200];
		snprintf(p, sizeof(p), "%s/%s", path, member[i]);
		struct stat st;
		if (!stat(p, &st) && S_ISREG(st.st_mode)) return 1;
	}
	return 0;
}

static int ext_matches(const char *name, const char *list)
{
	const char *dot = strrchr(name, '.');
	if (!dot || !dot[1]) return 0;
	dot++;

	int elen = (int)strlen(dot);
	const char *p = list;
	while (*p)
	{
		const char *c = strchr(p, ',');
		int len = c ? (int)(c - p) : (int)strlen(p);
		if (len == elen && !strncasecmp(p, dot, len)) return 1;
		if (!c) break;
		p = c + 1;
	}
	return 0;
}

// "Super Metroid (Europe) [!].sfc" -> "Super Metroid"
/*
  A folder holding a playlist and its parts is one game, not a shelf of fragments.

  A CD rip is normally a .cue naming a set of tracks - Track 01.bin, Track 02.bin - and
  those tracks are parts of a game rather than games. Two things go wrong if they are
  listed. They do not load: a Mega CD track handed to the Genesis core as a cartridge was
  never going to boot, so each one is a card that fails when pressed. And their names are
  not titles, so once the directory left the grouping key in 97775f0, every "Track 01.bin"
  on the card grouped behind one card whatever game it came from.

  The test is a .cue or .m3u in the same folder, and deliberately not whether *this* system
  can load one - md accepts bin and not cue, which is exactly the case that breaks. A cue
  beside a bin means the bin is a track, whoever can read the cue.

  Deliberately not img/ima/vhd: ao486 and Atari ST load those as games in their own right
  and no rip layout needs them covered.

  One opendir per directory, done once before the entries are walked rather than per
  candidate file, which would make it quadratic in a folder of tracks.
*/
static int dir_has_playlist(const char *path)
{
	DIR *d = opendir(path);
	if (!d) return 0;

	int found = 0;
	struct dirent *de;
	while (!found && (de = readdir(d)))
	{
		if (de->d_name[0] == '.') continue;
		if (ext_matches(de->d_name, "cue,m3u")) found = 1;
	}

	closedir(d);
	return found;
}

static int ext_is_part(const char *name)
{
	return ext_matches(name, "bin,iso,wav,raw");
}

/*
  A name that is only a part designator is not a title: "Track 01", "Disc 2", "CD1". Where
  clean_title() reduces a filename to one of those, the file cannot name its own card and
  the folder does it instead - which stops two folders colliding and gets the card called
  "Sonic CD" rather than "Track 01".

  Note what this must NOT match. "Final Fantasy VII (USA) (Disc 1).cue" is a real title
  with a qualifier, and clean_title() dropping that qualifier is what makes its three discs
  one card; the bug is only when the *whole* name is the qualifier. A bare number is left
  alone on purpose too - 1942, 1943, 2048 and 720 are games.
*/
static int title_is_part_only(const char *t)
{
	static const char *const word[] = { "track", "disc", "disk", "cd", "side", "part" };

	char low[CH_TITLE_LEN];
	int n = 0;
	for (const char *p = t; *p && n < (int)sizeof(low) - 1; p++)
		low[n++] = (char)tolower((unsigned char)*p);
	low[n] = 0;

	for (size_t i = 0; i < sizeof(word) / sizeof(word[0]); i++)
	{
		size_t wl = strlen(word[i]);
		if (strncmp(low, word[i], wl)) continue;

		const char *p = low + wl;
		while (*p == ' ' || *p == '-' || *p == '_' || *p == '.') p++;
		if (!isdigit((unsigned char)*p)) continue;

		while (isdigit((unsigned char)*p)) p++;
		while (*p == ' ') p++;
		if (!*p) return 1;
	}
	return 0;
}

static void clean_title(const char *file, char *out, int len)
{
	char tmp[CH_TITLE_LEN * 2];
	snprintf(tmp, sizeof(tmp), "%s", file);

	char *dot = strrchr(tmp, '.');
	if (dot) *dot = 0;

	// Drop everything from the first bracket group onwards.
	for (char *p = tmp; *p; p++)
	{
		if (*p == '(' || *p == '[')
		{
			while (p > tmp && (p[-1] == ' ' || p[-1] == '_')) p--;
			*p = 0;
			break;
		}
	}

	for (char *p = tmp; *p; p++) if (*p == '_') *p = ' ';

	char *s = trim(tmp);
	if (!*s) snprintf(out, len, "%s", file);
	else snprintf(out, len, "%s", s);
}

/*
  The ceiling in force. A variable only so the harness can drive a card that is bigger than
  the index without building one - the same arrangement, and the same reason, as
  lib_scan_test_budget(): what is worth asserting is that a truncated library still behaves
  (it says so, and it still caches), and that is only checkable if a test can reach the
  ceiling. It never exceeds CH_MAX_ITEMS, which is what the arrays are sized for.
*/
static int item_cap = CH_MAX_ITEMS;

void lib_test_item_cap(int n)
{
	item_cap = (n > 0 && n < CH_MAX_ITEMS) ? n : CH_MAX_ITEMS;
}

static void add_item(int sysidx, const char *relpath, const char *filename)
{
	if (nitems >= item_cap)
	{
		static int warned = 0;
		if (!warned)
		{
			warned = 1;
			printf("ClassicUI: index full at %d items, the rest of the library is not listed\n", item_cap);
		}
		return;
	}

	chome_item *it = &items[nitems];
	memset(it, 0, sizeof(*it));
	it->kind = IT_GAME;
	it->sysidx = (int16_t)sysidx;
	snprintf(it->path, sizeof(it->path), "%s", relpath);
	clean_title(filename, it->title, sizeof(it->title));

	// A part designator cannot name a card; its folder can. See title_is_part_only().
	if (title_is_part_only(it->title))
	{
		const char *slash = strrchr(relpath, '/');
		if (slash && slash > relpath)
		{
			char dir[CH_PATH_LEN];
			snprintf(dir, sizeof(dir), "%.*s", (int)(slash - relpath), relpath);
			const char *last = strrchr(dir, '/');
			clean_title(last ? last + 1 : dir, it->title, sizeof(it->title));
		}
		// At the system root there is no folder to borrow from, so the name stands.
	}

	char keybuf[CH_PATH_LEN + 32];
	snprintf(keybuf, sizeof(keybuf), "%s/%s", systems[sysidx].id, relpath);
	it->key = hash32(keybuf);

	nitems++;
}

/* ------------------------------------------------------ recently played --- */

/*
  The order *is* the data: an MRU list of game keys, most recently launched first.

  A last-played time on each item is the obvious alternative and is wrong here twice
  over. The DE10-Nano has no battery-backed clock, so anything played before the
  network came up would be filed under 1970 and outrank every game played since. And
  the only per-game record that survives a launch is this config directory: index.bin
  is written once at the end of a scan, while a launch re-execs MiSTer long before the
  next one, so a time stamped into chome_item would never reach the card at all.

  Capped on the card as well as on the shelf - a "recent" list as long as the library
  is just the library again.
*/
#define RECENT_MAX     20
#define RECENT_FILE    "classicui_recent.cfg"
#define RECENT_MAGIC   0x50524843u       // "CHRP"
/*
  Bump when the record below changes. A file that does not answer to the current magic
  and version reads as "nothing played yet" and is rewritten by the next launch, which
  is the same trade IDX_VERSION makes for the index cache: a stale file is rebuilt
  rather than read at the wrong stride.
*/
#define RECENT_VERSION 1

struct recent_file
{
	uint32_t magic;
	uint32_t version;
	uint32_t count;
	uint32_t key[RECENT_MAX];
};

static uint32_t recent_keys[RECENT_MAX];
static int recent_nkeys = 0;

// Those keys resolved to index positions, same order, minus anything not on the card.
// What the view is built from, so no view build has to touch the disk.
static int recent_items[RECENT_MAX];
static int recent_n = 0;

/*
  Is the game still there? Asked only of the twenty, and only when the list changes,
  so it costs twenty stats per launch rather than anything per frame.

  A zipped ROM's path names the member inside the archive ("Game (USA).zip/Game.sfc"),
  which stat() cannot see, so the archive is what gets asked about - and a Darksoft
  romset is a folder, so a directory counts as present.
*/
static int item_on_card(const chome_item *it)
{
	char dir[1024];
	if (!lib_sys_games_dir(it->sysidx, dir, sizeof(dir))) return 0;

	char rel[CH_PATH_LEN];
	snprintf(rel, sizeof(rel), "%s", it->path);
	char *member = (char*)strcasestr(rel, ".zip/");
	if (member) member[4] = 0;

	char full[1024 + CH_PATH_LEN];
	snprintf(full, sizeof(full), "%s/%s", dir, rel);

	struct stat st;
	return stat(full, &st) ? 0 : 1;
}

/*
  A key the index knows nothing about is a game that has left the card, or one on a
  USB stick that is not plugged in this time. It drops out here instead of being
  carried as a hole in the list, so the twenty places hold twenty games the player
  can actually start.
*/
static void recent_resolve()
{
	recent_n = 0;

	for (int i = 0; i < recent_nkeys && recent_n < RECENT_MAX; i++)
	{
		for (int k = 0; k < nitems; k++)
		{
			if (items[k].key != recent_keys[i]) continue;
			if (item_on_card(&items[k])) recent_items[recent_n++] = k;
			break;
		}
	}
}

static void recent_load()
{
	recent_nkeys = 0;

	recent_file r;
	memset(&r, 0, sizeof(r));

	if (FileLoadConfig(RECENT_FILE, &r, sizeof(r)) == (int)sizeof(r) &&
		r.magic == RECENT_MAGIC && r.version == RECENT_VERSION && r.count <= RECENT_MAX)
	{
		recent_nkeys = (int)r.count;
		for (int i = 0; i < recent_nkeys; i++) recent_keys[i] = r.key[i];
	}

	// The systems are also reloaded by the in-game menu, which does it with the index
	// already up, so the resolved list has to be rebuilt from here as well.
	recent_resolve();
}

static void recent_save()
{
	recent_file r;
	memset(&r, 0, sizeof(r));
	r.magic = RECENT_MAGIC;
	r.version = RECENT_VERSION;
	r.count = (uint32_t)recent_nkeys;
	for (int i = 0; i < recent_nkeys; i++) r.key[i] = recent_keys[i];

	FileSaveConfig(RECENT_FILE, &r, sizeof(r));
}

// To the front, dropping any earlier appearance of the same game: one played twice
// takes one place on the shelf, not two.
static void recent_touch(const chome_item *it)
{
	uint32_t keys[RECENT_MAX];
	int n = 0;

	keys[n++] = it->key;
	for (int i = 0; i < recent_nkeys && n < RECENT_MAX; i++)
	{
		if (recent_keys[i] != it->key) keys[n++] = recent_keys[i];
	}

	recent_nkeys = n;
	memcpy(recent_keys, keys, sizeof(keys[0]) * (size_t)n);

	recent_save();
	recent_resolve();
}

/* ---------------------------------------------------------- play state ---- */

#define STATE_MAX 1024

struct state_rec
{
	uint32_t key;
	uint16_t plays;
	uint8_t  fav;
	uint8_t  locks;    // one bit per suspend slot
};

static state_rec state[STATE_MAX];
static int nstate = 0;
static int state_dirty = 0;

static void state_load()
{
	memset(state, 0, sizeof(state));
	nstate = 0;

	int len = FileLoadConfig("classicui_state.cfg", state, sizeof(state));
	if (len > 0)
	{
		nstate = len / (int)sizeof(state_rec);
		if (nstate > STATE_MAX) nstate = STATE_MAX;
	}
	printf("ClassicUI: %d state records\n", nstate);
}

void lib_state_save()
{
	if (!state_dirty) return;
	FileSaveConfig("classicui_state.cfg", state, nstate * (int)sizeof(state_rec));
	state_dirty = 0;
}

static state_rec *state_find(uint32_t key, int create)
{
	for (int i = 0; i < nstate; i++) if (state[i].key == key) return &state[i];
	if (!create) return 0;

	if (nstate < STATE_MAX)
	{
		state[nstate].key = key;
		state[nstate].plays = 0;
		state[nstate].fav = 0;
		return &state[nstate++];
	}

	// Full: reuse the least played non-favourite slot.
	state_rec *victim = 0;
	for (int i = 0; i < nstate; i++)
	{
		if (state[i].fav) continue;
		if (!victim || state[i].plays < victim->plays) victim = &state[i];
	}
	if (victim)
	{
		victim->key = key;
		victim->plays = 0;
		victim->fav = 0;
	}
	return victim;
}

static void state_apply(chome_item *it)
{
	state_rec *r = state_find(it->key, 0);
	it->fav = r ? r->fav : 0;
	it->plays = r ? r->plays : 0;
}

void lib_toggle_fav(chome_item *it)
{
	state_rec *r = state_find(it->key, 1);
	if (!r) return;
	r->fav = r->fav ? 0 : 1;
	it->fav = r->fav;
	state_dirty = 1;
	lib_state_save();
}

void lib_note_play(chome_item *it)
{
	state_rec *r = state_find(it->key, 1);
	if (!r) return;
	if (r->plays < 0xffff) r->plays++;
	it->plays = r->plays;
	state_dirty = 1;
	lib_state_save();

	recent_touch(it);
}

/* -------------------------------------------------------------- slots ----- */

// Builds "savestates/<core>/<rom base>_<n>.ss", matching FileGenerateSavestatePath().
// Says where the state would live, whether or not anything is there yet.
static int slot_path_raw(const chome_item *it, int n, char *out, int len)
{
	const chome_sys *s = lib_sys(it->sysidx);
	if (!s) return 0;

	const char *core = strrchr(s->rbf, '/');
	core = core ? core + 1 : s->rbf;
	if (s->mra) core = "Arcade";
	if (!*core) return 0;

	const char *fn = strrchr(it->path, '/');
	fn = fn ? fn + 1 : it->path;

	char base[CH_PATH_LEN];
	snprintf(base, sizeof(base), "%s", fn);
	char *dot = strrchr(base, '.');
	if (dot) *dot = 0;

	snprintf(out, len, "savestates/%s/%s_%d.ss", core, base, n);
	return 1;
}

// The same path, but only when a state is actually there.
static int slot_path(const chome_item *it, int n, char *out, int len)
{
	if (!slot_path_raw(it, n, out, len)) return 0;
	if (FileExists(out, 0)) return 1;

	if (n == 1)
	{
		// Slot 1 also has a legacy suffix-less form: the same path without the "_1".
		char *tail = out + strlen(out) - 5;         // "_1.ss"
		if (tail > out && !strcmp(tail, "_1.ss"))
		{
			strcpy(tail, ".ss");
			if (FileExists(out, 0)) return 1;
		}
	}
	return 0;
}

int lib_slot_target(const chome_item *it, int slot, char *out, int len)
{
	char rel[1024];
	if (!slot_path_raw(it, slot + 1, rel, sizeof(rel))) return 0;

	snprintf(out, len, "%s/%s", getRootDir(), rel);
	return 1;
}

int lib_slot_thumb(const chome_item *it, int slot, char *out, int len)
{
	char rel[1024];
	if (!slot_path(it, slot + 1, rel, sizeof(rel))) return 0;

	char *dot = strrchr(rel, '.');
	if (!dot) return 0;
	strcpy(dot, ".png");

	snprintf(out, len, "%s/%s", getRootDir(), rel);
	return 1;
}

void lib_set_lock(chome_item *it, int slot, int on)
{
	state_rec *r = state_find(it->key, 1);
	if (!r) return;

	if (on) r->locks |= (uint8_t)(1u << slot);
	else r->locks &= (uint8_t)~(1u << slot);

	state_dirty = 1;
	lib_state_save();
	lib_refresh_slots(it);
}

int lib_delete_slot(chome_item *it, int slot)
{
	char rel[1024];
	if (!slot_path(it, slot + 1, rel, sizeof(rel))) return 0;

	// Never remove a locked slot.
	state_rec *r = state_find(it->key, 0);
	if (r && (r->locks & (1u << slot))) return 0;

	char full[1200];
	snprintf(full, sizeof(full), "%s/%s", getRootDir(), rel);
	printf("ClassicUI: deleting %s\n", full);
	int ok = (remove(full) == 0);

	/*
	  The state's screenshot goes with it. MiSTer writes it beside the state as a .png
	  and the shelf shows it as the slot's picture, so leaving it behind means a slot
	  that reads as empty while its thumbnail lingers on the card - noticed when a
	  deleted slot 2 left Adventure Island 3 (USA)_2.png sitting there.
	*/
	char png[1200];
	snprintf(png, sizeof(png), "%s", full);
	char *dot = strrchr(png, '.');
	if (dot) { strcpy(dot, ".png"); remove(png); }

	lib_refresh_slots(it);
	return ok;
}

void lib_refresh_slots(chome_item *it)
{
	it->slots = 0;
	const chome_sys *s = lib_sys(it->sysidx);
	if (!s) return;

	state_rec *r = state_find(it->key, 0);
	uint8_t locks = r ? r->locks : 0;

	for (int n = 1; n <= 4; n++)
	{
		char p[1024];
		if (!slot_path(it, n, p, sizeof(p))) continue;

		int i = n - 1;
		int st = (locks & (1u << i)) ? 2 : 1;
		it->slots |= (uint8_t)((unsigned)st << (i * 2));
	}
}

/* -------------------------------------------------------- index cache ----- */

/*
  The index is cached on the SD card so opening the menu inside a game core does
  not cost a rescan. Every core switch re-execs the binary, so without this the
  first open after each switch would walk the whole library again.

  Validation records the mtime of every directory the scan visited. Adding or
  removing a file changes its parent directory's mtime, so stat()ing those
  directories catches library changes at a fraction of the cost of re-reading them
  (one stat per directory, versus a readdir of every entry). What it cannot catch
  is a change deeper than the recorded set when that set overflowed - hence the cap
  below is generous, and Options > Rescan Library forces a fresh scan regardless.
*/

#define IDX_MAGIC   0x58494843u        // "CHIX"
// 2: archives are indexed by looking inside them, so a cache written by 1 is
// missing every zipped ROM on the card and has to be rebuilt rather than trusted.
#define IDX_VERSION 2
#define IDX_MAX_DIRS 2048
#define IDX_PATH_LEN 304

struct idx_dir
{
	char     path[IDX_PATH_LEN];
	uint64_t mtime;
};

struct idx_header
{
	uint32_t magic;
	uint32_t version;
	uint32_t item_size;      // reject a cache written by a different layout
	uint32_t sys_sig;        // systems table signature
	uint32_t nitems;
	uint32_t ndirs;
	uint32_t truncated;      // 1 when the directory set overflowed
	uint32_t reserved;
};

static idx_dir idx_dirs[IDX_MAX_DIRS];
static int idx_ndirs = 0;
static int idx_truncated = 0;
static int idx_from_cache = 0;

int lib_index_cached() { return idx_from_cache; }

static void idx_note_dir(const char *full)
{
	if (idx_ndirs >= IDX_MAX_DIRS) { idx_truncated = 1; return; }
	if (strlen(full) >= IDX_PATH_LEN) { idx_truncated = 1; return; }

	struct stat st;
	if (stat(full, &st)) return;

	snprintf(idx_dirs[idx_ndirs].path, IDX_PATH_LEN, "%s", full);
	idx_dirs[idx_ndirs].mtime = (uint64_t)st.st_mtime;
	idx_ndirs++;
}

// Signature of the systems table: editing classicui_systems.txt invalidates.
static uint32_t idx_sys_sig()
{
	uint32_t h = 2166136261u;
	for (int i = 0; i < nsys; i++)
	{
		const char *parts[4] = { systems[i].id, systems[i].dir, systems[i].ext, systems[i].rbf };
		for (int k = 0; k < 4; k++)
			for (const char *p = parts[k]; *p; p++) { h ^= (unsigned char)*p; h *= 16777619u; }
		h ^= (uint32_t)systems[i].computer + 1u;
		h *= 16777619u;
	}
	h ^= (uint32_t)nsys;
	return h * 16777619u;
}

static const char *idx_path()
{
	static char p[1024];
	snprintf(p, sizeof(p), "%s/classicui/index.bin", getRootDir());
	return p;
}

static void idx_save()
{
	char dir[1024];
	snprintf(dir, sizeof(dir), "%s/classicui", getRootDir());
	mkdir(dir, 0777);

	FILE *f = fopen(idx_path(), "wb");
	if (!f) { printf("ClassicUI: cannot write the index cache\n"); return; }

	idx_header h = {};
	h.magic = IDX_MAGIC;
	h.version = IDX_VERSION;
	h.item_size = (uint32_t)sizeof(chome_item);
	h.sys_sig = idx_sys_sig();
	h.nitems = (uint32_t)nitems;
	h.ndirs = (uint32_t)idx_ndirs;
	h.truncated = (uint32_t)idx_truncated;

	int ok = (fwrite(&h, sizeof(h), 1, f) == 1);
	if (ok && nitems) ok = (fwrite(items, sizeof(chome_item), nitems, f) == (size_t)nitems);
	if (ok && idx_ndirs) ok = (fwrite(idx_dirs, sizeof(idx_dir), idx_ndirs, f) == (size_t)idx_ndirs);
	fclose(f);

	if (!ok) { unlink(idx_path()); printf("ClassicUI: index cache write failed\n"); return; }

	printf("ClassicUI: cached %d items and %d directories%s\n",
		nitems, idx_ndirs, idx_truncated ? " (directory set truncated)" : "");
}

// Returns 1 when a still-valid cache was loaded into the index.
static int idx_load()
{
	FILE *f = fopen(idx_path(), "rb");
	if (!f) return 0;

	idx_header h = {};
	if (fread(&h, sizeof(h), 1, f) != 1) { fclose(f); return 0; }

	if (h.magic != IDX_MAGIC || h.version != IDX_VERSION ||
		h.item_size != sizeof(chome_item) ||
		h.nitems > CH_MAX_ITEMS || h.ndirs > IDX_MAX_DIRS)
	{
		fclose(f);
		printf("ClassicUI: index cache is from another build, rescanning\n");
		return 0;
	}

	if (h.sys_sig != idx_sys_sig())
	{
		fclose(f);
		printf("ClassicUI: systems table changed, rescanning\n");
		return 0;
	}

	if (fread(items, sizeof(chome_item), h.nitems, f) != h.nitems) { fclose(f); return 0; }
	if (fread(idx_dirs, sizeof(idx_dir), h.ndirs, f) != h.ndirs) { fclose(f); return 0; }
	fclose(f);

	// Every recorded directory must still be there, unchanged.
	for (uint32_t i = 0; i < h.ndirs; i++)
	{
		struct stat st;
		if (stat(idx_dirs[i].path, &st) || (uint64_t)st.st_mtime != idx_dirs[i].mtime)
		{
			printf("ClassicUI: %s changed, rescanning\n", idx_dirs[i].path);
			return 0;
		}
	}

	/*
	  A system whose folder exists now must have been walked then, otherwise the
	  user has just added a whole system and the cache predates it. Directory
	  mtimes cannot catch that on their own: the new folder has no record to check.
	*/
	for (int i = 0; i < nsys; i++)
	{
		char root[1024];
		if (!lib_sys_games_dir(i, root, sizeof(root))) continue;

		int seen = 0;
		for (uint32_t k = 0; k < h.ndirs && !seen; k++) if (!strcmp(idx_dirs[k].path, root)) seen = 1;
		if (!seen)
		{
			printf("ClassicUI: %s appeared since the cache, rescanning\n", systems[i].name);
			return 0;
		}
	}

	nitems = (int)h.nitems;
	idx_ndirs = (int)h.ndirs;
	idx_truncated = (int)h.truncated;

	/*
	  The state file is authoritative for favourites and play counts, and savestate
	  slots are re-read per selection, so neither is trusted from the cache.
	*/
	for (int i = 0; i < nitems; i++)
	{
		items[i].slots = 0;
		state_apply(&items[i]);
	}
	recent_resolve();

	printf("ClassicUI: index cache hit, %d items%s\n",
		nitems, idx_truncated ? " (validation incomplete: use Rescan if a game is missing)" : "");
	return 1;
}

/* --------------------------------------------------------------- scan ----- */

static int scan_sys = 0;
static int scanning = 0;

/*
  The walk, sliced small enough that the frame loop keeps running through it.

  What this replaces, and why. lib_scan_step() is called once per frame and was
  documented as "one slice per frame", but the slice was a whole *system*: a full
  recursive walk of one games folder, a second opendir per directory for
  dir_has_playlist(), and - the expensive part - zip_playables() opening the central
  directory of every archive it passes. On a card with a couple of thousand zipped
  ROMs in one folder that is thousands of reads off an SD card inside one call from
  the frame loop, so the picture froze and the pad went dead for seconds at a time,
  on first boot and on every Options > Rescan Library.

  It was never felt in development because the index cache hides it on ordinary
  boots, and it cannot be seen in CPU time at all: the firmware busy-polls, so it
  sits at 100% of one core whether it is drawing, scanning or idle. The same trap
  the repaint counters in chome_gfx.cpp were written for - the only way to see this
  work is to time it.

  The recursion is now an explicit stack of *open* directories. That choice is the
  whole safety argument:

    Keeping the DIR* handles open across frames means a paused walk resumes exactly
    where it stopped, in the same readdir order, so the traversal is the same
    depth-first, in-place order the recursion produced - a subdirectory's games
    still land between the parent entries that bracket it. The library therefore
    comes out identical whatever the slice size is, which matters because the shelf
    is ordered from the index and a re-slicing that reordered games would make the
    player's shelf shuffle itself for no visible reason. assert_scan_slices() pins
    that: the same library fingerprint at seven slice sizes from unbounded down to
    one cost unit, and the same fingerprint again against a literal read off the
    whole-system build.

    The alternative - reopening the directory each slice and skipping N entries -
    would be both quadratic and unfounded, since readdir order is only guaranteed
    stable for one open handle.

  The stack is at most one frame per depth level, and the walk stops at depth 3 as
  it always did, so this holds at most four descriptors open between frames plus the
  transient one dir_has_playlist() takes. That is the cost of the design and it is
  the reason lib_init_common() has to unwind it: Rescan Library is reachable *from
  the shelf while a scan is running*, and restarting a scan over a live stack would
  leak every open handle.
*/
#define SCAN_DEPTH_MAX 3
#define SCAN_STACK_MAX (SCAN_DEPTH_MAX + 1)

/*
  What a slice may spend, in units weighted by what the work actually costs off an SD
  card rather than by entry count.

  The weights are read off what each branch does rather than timed one by one: a bare
  entry is a readdir out of a buffer the kernel already filled, while a zip is an open
  and two reads at the far end of a file, and a directory is an opendir plus a stat plus
  dir_has_playlist() sweeping the whole thing. Those are not the same size and counting
  entries alone would price a slice of 400 archives and a slice of 400 filenames the
  same. The ratio between the three is a judgement; what is measured is the *budget*
  they add up to, in assert_scan_slices().

  SCAN_BUDGET is sized by the number of *card reads* a slice may do, and deliberately
  not by how long a slice takes on a development host. The two disagree by two orders of
  magnitude and the host is the misleading one: a zip's central directory is about ten
  microseconds there, because the file was written a moment ago and is in the page cache,
  while on the device it is a seek and a read on an SD card through FAT. Nothing in this
  tree measures that latency - the review's R3 entry asks for it, and it is the one number
  that decides how bad the stall really was - so the budget is set on the quantity that
  *is* known: at 30 units an archive, 600 lets a slice open twenty of them. Twenty card
  reads is a few frames even at a pessimistic latency, and the whole scan still does
  exactly as many reads as it always did.

  It does not go lower than that because the per-slice overhead is real, and measuring it
  is what found the second half of this bug: chome_ui.cpp rebuilt the shelf view after
  every slice, which is O(n log n) in the whole library. On the host, at every budget from
  1500 down, the worst *frame* stopped falling at around seven milliseconds while the
  worst slice kept shrinking - the frame had stopped being about the walk. Slicing alone
  would have traded one long freeze for a permanently heavy frame; that rebuild is now
  throttled, and the two changes only work together.
*/
#define SCAN_COST_ENTRY   1      // readdir plus the name work for one entry
#define SCAN_COST_STAT    3      // one stat, for an entry readdir would not type
#define SCAN_COST_DIR    30      // opendir + stat + dir_has_playlist()'s own sweep
#define SCAN_COST_ZIP    30      // opening one archive's central directory
#define SCAN_COST_ROMSET  9      // up to three stats to tell a romset from a folder
#define SCAN_COST_SYS     8      // resolving a system's games folder, whether it exists
#define SCAN_COST_XML   200      // romsets.xml, read whole before the folder is walked
#define SCAN_BUDGET     600

// The budget in force. A variable only so the harness can drive the walk at every slice
// size it can be driven at and prove the library does not depend on which one. See
// lib_scan_test_budget().
static long scan_budget = SCAN_BUDGET;

struct scan_frame
{
	DIR *d;
	char full[1024];
	char rel[CH_PATH_LEN];
	int has_playlist;
	int depth;
};

static scan_frame scan_stack[SCAN_STACK_MAX];
static int scan_sp = 0;
static char scan_root[1024];     // the games folder the open frames belong to
static int scan_before = 0;      // nitems when this system's walk started
static long scan_cost = 0;       // spent in the current slice

// Reported so the device can size its own scan without a stopwatch, and so the
// harness can assert the frame loop is being let back in. See lib_scan_stats().
static int scan_slices = 0;
static long scan_cost_max = 0;

/*
  Push one directory: the equivalent of entering scan_dir(). Every reason the old
  function had to return immediately - too deep, index full, unreadable folder - is a
  reason not to push, and produces exactly the same library.
*/
static int scan_push(const char *rel, int depth)
{
	if (depth > SCAN_DEPTH_MAX || scan_sp >= SCAN_STACK_MAX) return 0;
	if (nitems >= item_cap) return 0;

	scan_frame *f = &scan_stack[scan_sp];

	if (rel[0]) snprintf(f->full, sizeof(f->full), "%s/%s", scan_root, rel);
	else snprintf(f->full, sizeof(f->full), "%s", scan_root);

	f->d = opendir(f->full);
	if (!f->d) return 0;

	snprintf(f->rel, sizeof(f->rel), "%s", rel);
	f->depth = depth;

	idx_note_dir(f->full);

	// Once per directory, not once per file: see dir_has_playlist().
	f->has_playlist = dir_has_playlist(f->full);
	scan_cost += SCAN_COST_DIR;

	scan_sp++;
	return 1;
}

static void scan_pop()
{
	if (scan_sp <= 0) return;
	scan_sp--;
	closedir(scan_stack[scan_sp].d);
	scan_stack[scan_sp].d = 0;
}

static void scan_unwind()
{
	while (scan_sp > 0) scan_pop();
}

/*
  Walk the open tree until the budget runs out or it is finished. The body is the old
  loop body unchanged, with the one recursive call replaced by a push - and the next
  pass then works on the child, which is what keeps the order depth-first in place.
*/
static void scan_walk()
{
	while (scan_sp > 0 && scan_cost < scan_budget)
	{
		scan_frame *f = &scan_stack[scan_sp - 1];

		struct dirent *de = readdir(f->d);
		if (!de) { scan_pop(); continue; }

		if (de->d_name[0] == '.') continue;

		/*
		  A full index stopped the old walk by breaking out of every loop on the way
		  home, so nothing further was read at any depth. Unwinding says the same
		  thing, and the whole scan then finishes on the next pass.

		  And it says so out loud, here rather than in add_item(): the guard there is
		  reached only when an archive's members fill the last places, so on a card that
		  is simply too big it never fires and the library was cut short in silence.
		  lib_index_full() carries the same fact to the UI.
		*/
		if (nitems >= item_cap)
		{
			// Once per scan: only the system the walk was inside when the ceiling was
			// reached has an open stack to unwind, and every system after it is refused
			// before it opens one.
			printf("ClassicUI: the index is full at %d files - the rest of %s, and every system "
				"after it, are not listed. Nothing is wrong with the card; the ceiling is ours.\n",
				item_cap, systems[scan_sys].name);
			scan_unwind();
			return;
		}

		scan_cost += SCAN_COST_ENTRY;

		char childrel[CH_PATH_LEN];
		if (f->rel[0]) snprintf(childrel, sizeof(childrel), "%s/%s", f->rel, de->d_name);
		else snprintf(childrel, sizeof(childrel), "%s", de->d_name);

		char childfull[1024];
		snprintf(childfull, sizeof(childfull), "%s/%s", f->full, de->d_name);

		int isdir;
		if (de->d_type == DT_DIR) isdir = 1;
		else if (de->d_type == DT_REG) isdir = 0;
		else
		{
			struct stat st;
			scan_cost += SCAN_COST_STAT;
			if (stat(childfull, &st)) continue;
			isdir = S_ISDIR(st.st_mode) ? 1 : 0;
		}

		if (isdir)
		{
			/*
			  Arcade packs keep alternate ROM revisions of the same games in
			  _alternatives, and unbuildable ones in similar underscore folders. Listing
			  them puts several near-identical entries in front of every game and buries
			  the ~950 real ones, so the shelf skips them; they are still on the card and
			  still reachable from the classic browser.
			*/
			if (systems[scan_sys].mra && de->d_name[0] == '_') continue;

			/*
			  A romset can be a folder of member files rather than an archive of them -
			  that is how the Darksoft packs ship - and then the folder is the game, not
			  something to walk into. The loader takes either: file_io opens
			  "romset/prom" and "romset.zip/prom" alike.
			*/
			if (systems[scan_sys].romset)
			{
				scan_cost += SCAN_COST_ROMSET;
				if (dir_is_romset(childfull))
				{
					char buf[CH_TITLE_LEN];
					const char *title = romset_title(f->full, de->d_name, buf, sizeof(buf));
					if (title) add_item(scan_sys, childrel, title);
					continue;
				}
			}

			// `f` is stale past here: the child is the top of the stack now.
			scan_push(childrel, f->depth + 1);
		}
		else if (ext_matches(de->d_name, systems[scan_sys].ext))
		{
			// A track beside its cue is part of a game, not a game. dir_has_playlist().
			if (f->has_playlist && ext_is_part(de->d_name)) continue;

			if (systems[scan_sys].romset)
			{
				char buf[CH_TITLE_LEN];
				const char *title = romset_title(f->full, de->d_name, buf, sizeof(buf));
				if (title) add_item(scan_sys, childrel, title);
			}
			else add_item(scan_sys, childrel, de->d_name);
		}
		else if (is_zip_name(de->d_name) && !sys_takes_zip(scan_sys))
		{
			char inner[ZIP_MAX_ENTRIES][CH_PATH_LEN];
			scan_cost += SCAN_COST_ZIP;
			int n = zip_playables(childfull, systems[scan_sys].ext, inner, ZIP_MAX_ENTRIES);

			for (int i = 0; i < n; i++)
			{
				char p[CH_PATH_LEN];
				snprintf(p, sizeof(p), "%s/%s", childrel, inner[i]);

				/*
				  One ROM per archive is the normal case, and there the archive carries
				  the good name - a No-Intro zip is named properly while its member may
				  not be - so the title comes from the zip. A multi-ROM archive has to
				  name each entry instead, or they would all read the same.
				*/
				add_item(scan_sys, p, (n == 1) ? de->d_name : inner[i]);
			}
		}
	}
}

// Drop any open walk. Called wherever a scan starts or is abandoned.
static void scan_reset()
{
	scan_unwind();
	scan_cost = 0;
	scan_before = 0;
}

int lib_scan_step()
{
	if (!scanning) return 0;

	scan_cost = 0;
	scan_slices++;

	for (;;)
	{
		// Resume, or finish, whatever tree is already open.
		if (scan_sp > 0)
		{
			scan_walk();
			if (scan_sp > 0) break;              // out of budget, mid-system

			printf("ClassicUI: %s -> %d items\n",
				systems[scan_sys].name, nitems - scan_before);
			scan_sys++;
		}

		if (scan_sys >= nsys)
		{
			scanning = 0;
			if (scan_cost > scan_cost_max) scan_cost_max = scan_cost;
			for (int i = 0; i < nitems; i++) state_apply(&items[i]);
			recent_resolve();
			printf("ClassicUI: scan complete, %d items in %d slices (worst slice %ld of %ld)\n",
				nitems, scan_slices, scan_cost_max, scan_budget);
			idx_save();
			return 0;
		}

		if (scan_cost >= scan_budget) break;

		/*
		  Start the next system. A system with no games folder on this card costs
		  almost nothing, so several of them are cleared in one slice rather than one
		  per frame - which is what the old code did, and the reason a card of mostly
		  absent systems took thirty frames to find that out.
		*/
		scan_cost += SCAN_COST_SYS;
		if (lib_sys_games_dir(scan_sys, scan_root, sizeof(scan_root)))
		{
			scan_before = nitems;

			// Romset titles come out of romsets.xml, which has to be read before the
			// folder is walked so every entry can be looked up as it is found.
			if (systems[scan_sys].romset)
			{
				neogeo_scan_xml(scan_root);
				scan_cost += SCAN_COST_XML;
			}

			if (scan_push("", 0)) continue;

			/*
			  The folder is here and was not walked - the index filled up, or it could not
			  be opened. Record it anyway, or idx_load()'s "a system appeared since the
			  cache" rule sees a games folder with no record and rejects the cache for as
			  long as the card stays that way. That is what made a card too big for the
			  index pay a full scan on every single boot: the truncation was permanent, so
			  the rejection was too, and Rescan Library could not clear it either.

			  Recording the root says "this folder existed and this is what it looked like",
			  which is exactly what the mtime check wants; that its contents are missing
			  from the index is what lib_index_full() is for.
			*/
			idx_note_dir(scan_root);

			// An unreadable games folder: the old code reported it as an empty one.
			printf("ClassicUI: %s -> %d items\n", systems[scan_sys].name, nitems - scan_before);
		}
		scan_sys++;
	}

	if (scan_cost > scan_cost_max) scan_cost_max = scan_cost;
	return 1;
}

int lib_scanning() { return scanning; }
int lib_scan_progress() { return nitems; }

/*
  Whether the library on the card is bigger than the index can hold. Asked of the index
  rather than of a flag left by the walk, so it answers the same after a cache load as
  after a scan - the cache carries the item count and nothing else would remember.
*/
int lib_index_full() { return nitems >= item_cap; }

/*
  Which system the walk is inside, for the shelf to name while it waits, and -1 when
  the scan is not running or is between systems. The item count on its own is not
  honest progress on a big card: it crawls for a minute with nothing to say how much
  is left, and it was never seen at all while a whole system was one frozen slice.
*/
int lib_scan_sys()
{
	if (!scanning || scan_sp <= 0 || scan_sys < 0 || scan_sys >= nsys) return -1;
	return scan_sys;
}

// Slices taken by the scan so far, and the worst slice's cost. The harness asserts
// on the first of these; the log line at the end of a scan reports both.
void lib_scan_stats(int *slices, long *cost_max, long *budget)
{
	if (slices) *slices = scan_slices;
	if (cost_max) *cost_max = scan_cost_max;
	if (budget) *budget = scan_budget;
}

void lib_scan_test_budget(long b)
{
	scan_budget = b > 0 ? b : SCAN_BUDGET;
}

/* ----------------------------------------------- installation naming ------ */

/*
  One machine, several names. The downloader's names.txt renames cores and their
  game folders to regional titles, so a European card carries _Console/MegaDrive
  and games/MegaDrive where the table above says Genesis - and the core lookup is
  a prefix match, so the mismatch is not a near miss, it is a system that quietly
  vanishes from the UI.

  Each row lists names that mean the same machine. The table's own name is tried
  first; if it is not on this card, the remaining ones are, and the core file and
  the games folder are resolved independently since a card can rename either.
*/
#define ALIAS_MAX 4
static const char *const name_alias[][ALIAS_MAX] =
{
	{ "Genesis",       "MegaDrive",       "Mega Drive",    0            },
	/*
	  Now a shelf system of its own as well as the core the physical-disc launch hands a
	  Mega CD disc to, and US naming renames both halves of it: the core file becomes
	  SegaCD.rbf and the games folder becomes games/SegaCD. Both are resolved through this
	  row, independently, because a card can carry either spelling of either.

	  Its two CD siblings need no row of their own. PC Engine CD and Neo Geo CD live in
	  folders the firmware names itself - PCECD_DIR and NEOCD_DIR - so a renamed core does
	  not move them; and their cores are found under the names their cartridge rows already
	  use, TurboGrafx16 through the row below and NeoGeo through no row at all, since
	  nothing renames it.
	*/
	{ "MegaCD",        "SegaCD",          "Sega CD",       0            },
	{ "TurboGrafx16",  "TGFX16",          "PCEngine",      "PC Engine"  },
	{ "NeoGeo-Pocket", "NeoGeoPocket",    "NGP",           0            },
	{ "Atari7800",     "A7800",           0,               0            },
	{ "ZX-Spectrum",   "Spectrum",        "ZXSpectrum",    0            },
	{ "MSX",           "MSX1",            0,               0            },
	{ "SMS",           "MasterSystem",    "Master System", 0            },
	{ "AtariLynx",     "Lynx",            0,               0            },
	{ "Minimig",       "Minimig-AGA",     "Amiga",         0            },
};

// Does <root>/<dir> hold a core for this base name? Same rule the MGL loader
// uses: the base, then '.' or '_' (the datecode), then ".rbf".
static int rbf_present(const char *dir, const char *base)
{
	char path[1200];
	snprintf(path, sizeof(path), "%s/%s", getRootDir(), dir);

	DIR *d = opendir(path);
	if (!d) return 0;

	size_t bl = strlen(base);
	int found = 0;
	struct dirent *e;
	while (!found && (e = readdir(d)) != NULL)
	{
		size_t l = strlen(e->d_name);
		if (l < bl + 4) continue;
		if (strcasecmp(e->d_name + l - 4, ".rbf")) continue;
		if (strncasecmp(e->d_name, base, bl)) continue;
		if (e->d_name[bl] == '.' || e->d_name[bl] == '_') found = 1;
	}

	closedir(d);
	return found;
}

// An alias of `name` that is actually installed, or 0. games: look for a games
// folder, otherwise a core file in `coredir`.
static const char *alias_pick(const char *name, const char *coredir, int games)
{
	for (size_t g = 0; g < sizeof(name_alias) / sizeof(name_alias[0]); g++)
	{
		int mine = 0;
		for (int k = 0; k < ALIAS_MAX && name_alias[g][k]; k++)
		{
			if (!strcasecmp(name, name_alias[g][k])) mine = 1;
		}
		if (!mine) continue;

		for (int k = 0; k < ALIAS_MAX && name_alias[g][k]; k++)
		{
			const char *cand = name_alias[g][k];
			if (!strcasecmp(cand, name)) continue;

			if (games)
			{
				char d[1024];
				snprintf(d, sizeof(d), "%s", cand);
				if (findGamesDir(d, sizeof(d))) return cand;
			}
			else if (rbf_present(coredir, cand)) return cand;
		}
		break;         // a name belongs to one group
	}
	return 0;
}

/*
  Rewrite a core path ("_Console/MegaCD") in place to the name this card actually
  carries, using the alias table above. 1 when it was rewritten. Exported because
  the physical-disc launch names a core that is not any shelf system's own - see
  disc_launch() in chome_ui.cpp - so it cannot ride on resolve_names() below.
*/
int lib_resolve_rbf(char *rbf, int size)
{
	if (!rbf || !rbf[0]) return 0;

	char dir[80];
	snprintf(dir, sizeof(dir), "%s", rbf);
	char *slash = strrchr(dir, '/');
	if (!slash) return 0;

	*slash = 0;
	const char *base = slash + 1;
	if (rbf_present(dir, base)) return 0;

	const char *alt = alias_pick(base, dir, 0);
	if (!alt) return 0;

	snprintf(rbf, size, "%s/%s", dir, alt);
	return 1;
}

static void resolve_names()
{
	for (int i = 0; i < nsys; i++)
	{
		chome_sys *s = &systems[i];

		if (lib_resolve_rbf(s->rbf, sizeof(s->rbf)))
		{
			printf("ClassicUI: %s core is %s on this card\n", s->id, s->rbf);
		}

		if (!s->mra)
		{
			char d[1024];
			snprintf(d, sizeof(d), "%s", s->dir);
			if (!findGamesDir(d, sizeof(d)))
			{
				const char *alt = alias_pick(s->dir, 0, 1);
				if (alt)
				{
					printf("ClassicUI: %s games are in %s on this card\n", s->id, alt);
					snprintf(s->dir, sizeof(s->dir), "%s", alt);
				}
			}
		}
	}
}

void lib_load_systems()
{
	nsys = 0;
	lib_forget_dirs();

	if (!load_systems_file())
	{
		for (size_t i = 0; i < sizeof(defaults) / sizeof(defaults[0]); i++) add_sys(&defaults[i]);
	}

	resolve_names();
	state_load();
	recent_load();
}

static void lib_init_common(int use_cache)
{
	if (!items) items = (chome_item*)calloc(CH_MAX_ITEMS, sizeof(chome_item));
	if (!items) { printf("ClassicUI: out of memory for the index\n"); return; }

	nitems = 0;
	idx_ndirs = 0;
	idx_truncated = 0;
	idx_from_cache = 0;

	/*
	  Before anything else, and unconditionally: Rescan Library is reachable from the
	  shelf *while a scan is running*, and the walk now holds open directory handles
	  between frames. Starting a second scan over a live stack would leak one
	  descriptor per open level and leave the new walk reading the old card's
	  directories, so the stack is dropped here whichever way this call arrived.
	*/
	scan_reset();
	scan_slices = 0;
	scan_cost_max = 0;

	lib_load_systems();

	if (use_cache && idx_load())
	{
		idx_from_cache = 1;
		scanning = 0;
		scan_sys = nsys;
		return;
	}

	scan_sys = 0;
	scanning = 1;
}

void lib_init()
{
	lib_init_common(1);
}

void lib_rescan()
{
	printf("ClassicUI: rescanning the library\n");
	lib_forget_dirs();
	unlink(idx_path());
	lib_init_common(0);
}

/* -------------------------------------------------------- title groups ---- */

/*
  Several files, one title.

  clean_title() strips the decoration, so "Mega Man (U).nes" and "Mega Man (E).nes" both
  read as "Mega Man" and the shelf drew two identical cards with nothing to tell them
  apart. They become one card instead, cycled with a button, and the title block names
  the file on show. Regional variants, revisions and the discs of one game all collide
  the same way and are all fixed by the same grouping.

  What counts as the same title is four things. The display title is one of them and the
  other three exist to stop a merge that would be *wrong*, because merging two genuinely
  different games hides one of them behind a button nobody knows to press - a worse fault
  than the duplicate cards this replaces:

    the system     Aladdin on the SNES and Aladdin on the Mega Drive are different games
                   that share a name.
    the directory  NOT part of the key. The argument for including it was that a player
                   who keeps hacks, translations or a region in a folder of its own is
                   saying those are separate collections, so
                   games/SNES/Hacks/Super Mario World.sfc is not a regional variant of
                   the game above it. The call went the other way: better that a hack
                   shares a card with the original than that multi-disc sets split up.

                   What that buys: Chrono Cross (Disc 1)/ and (Disc 2)/ become one card,
                   which is what a player wants and what the folder-based key could never
                   do - clean_title() already reduces both to "Chrono Cross". A library
                   filed as games/NES/USA + games/NES/Europe now groups too.

                   What it costs: games/SNES/Hacks/Super Mario World.sfc shares a card
                   with games/SNES/Super Mario World.sfc, and two files of the same name
                   in two folders become one card with two entries. Both are accepted -
                   the filename line names whichever is on show, so nothing is hidden,
                   and X cycles to the other.
    the extension  a shared core hosts more than one machine and is told apart by
                   extension exactly here - "Sonic The Hedgehog 2.sms" and
                   "Sonic The Hedgehog 2.gg" are different games with different levels,
                   and class_of() already treats them as different hardware. The cost is
                   that a .cue and a .chd of one dump stay two cards, which is what they
                   are today, so nothing regresses.
    clean_title()  the grouping key and the thing the player reads as the title cannot
                   then disagree. Anything looser - a prefix, a fuzzy match - could put
                   "Mega Man" and "Mega Man 2" on one card.

  Compared case-insensitively throughout, matching both the shelf sort (strcasecmp) and
  the FAT filesystem the paths came off.
*/
#define GRP_KEY_LEN 320
#define GRP_SLOTS   32768    // power of two, comfortably over CH_MAX_ITEMS

/*
  How much of a path is the directory a group would share.

  A zipped ROM's path names the member inside the archive
  ("Mega Man (U).zip/Mega Man (U).nes") and the archive is the file on the card, so the
  archive counts as the file rather than as a directory. Without that, two zipped
  regions of one game sit in two different "directories" and never group - and zipped
  ROMs are how most cards store them.
*/
static int group_dir_len(const char *path)
{
	const char *zip = strcasestr(path, ".zip/");
	const char *end = zip ? zip + 4 : path + strlen(path);

	const char *slash = 0;
	for (const char *p = path; p < end; p++) if (*p == '/') slash = p;
	return slash ? (int)(slash - path) : 0;
}

/*
  The extension is the innermost file's, not the archive's: what decides the machine is
  the ROM, and "Sonic.zip/Sonic.gg" is a Game Gear game however the archive is named. A
  romset archive has no inner file and answers "zip", which is right - the archive is
  the game there.
*/
static void group_key(const chome_item *it, char *out, int len)
{
	const char *dot = strrchr(it->path, '.');
	snprintf(out, len, "%d|%s|%s", (int)it->sysidx, dot ? dot + 1 : "", it->title);

	for (char *p = out; *p; p++) *p = (char)tolower((unsigned char)*p);
}

/*
  One open-addressed table, shared by the three passes below that each need to answer
  "have I seen this key already" in a single walk. The generation counter retires the
  whole table in one assignment, so a pass costs nothing to start and no pass can see
  another's entries - they run one after another over the same cells.

  The payload is an index and the caller confirms the hit itself, because a 32-bit hash
  over a few thousand keys collides often enough that trusting it would occasionally
  merge two unrelated games. That is the one failure this feature must not have.
*/
struct grp_cell { uint32_t gen; int val; };
static grp_cell grp_tab[GRP_SLOTS];
static uint32_t grp_gen = 0;

typedef int (*grp_same)(int val, const char *key);

static void group_open() { grp_gen++; }

// The cell this key belongs in: an occupied one `same()` accepts, or a free one with
// val < 0 for the caller to fill. 0 only if the table filled up, which cannot happen at
// CH_MAX_ITEMS keys in GRP_SLOTS cells but is checked rather than assumed.
static grp_cell *group_cell(const char *key, grp_same same)
{
	uint32_t h = hash32(key);
	for (uint32_t probe = 0; probe < GRP_SLOTS; probe++)
	{
		grp_cell *c = &grp_tab[(h + probe) & (GRP_SLOTS - 1)];
		if (c->gen != grp_gen) { c->gen = grp_gen; c->val = -1; }
		if (c->val < 0) return c;
		if (same(c->val, key)) return c;
	}
	return 0;
}

/* --------------------------------------------------------------- views ---- */

/*
  Every item can be a card of its own - nothing forces the grouping to collapse anything -
  plus the handful of folder cards a view leads with. Written against CH_MAX_ITEMS rather
  than as a number of its own so the two cannot drift apart: a view cap below the item cap
  would drop cards off the end of the shelf without saying anything, which is the fault
  push_folder() and push_game() below would commit silently.
*/
#define VIEW_MAX (CH_MAX_ITEMS + 100)
static chome_entry view[VIEW_MAX];
static int nview = 0;

/*
  The files behind each card, as a chain through the item array: one link per item,
  since an item belongs to at most one card in any one view. A chain rather than a
  packed list because the variants of a title are not adjacent in the index - a third
  file in the same folder, or a zip whose members are added when the zip is reached,
  lands between them - so a packed list would need a second flattening pass.

  Kept in filename order, so the cycle runs Disc 1, Disc 2, Disc 3 rather than in
  whatever order readdir() happened to return.
*/
static int var_next[CH_MAX_ITEMS];

int lib_view_count() { return nview; }
const chome_entry *lib_view_entry(int i) { return (i >= 0 && i < nview) ? &view[i] : 0; }

const char *lib_sort_name(int sort)
{
	switch (sort)
	{
	case SORT_RECENT: return "Recently Played";
	case SORT_PLAYS:  return "Times Played";
	case SORT_TITLE:  return "Title A-Z";
	case SORT_SYSTEM: return "System";
	case SORT_ADDED:  return "Recently Added";
	case SORT_FAVS:   return "Favourites First";
	}
	return "?";
}

static int cur_sort = SORT_TITLE;

/*
  How recently a game was played, as a rank: 0 is the most recent, RECENT_MAX means "not in
  the list at all".

  There is no timestamp anywhere in this front-end. state_rec holds a key, a play count, a
  favourite bit and the suspend locks - it has never recorded WHEN a game was played, nor
  for how long it ran. What it does have is recent_keys[], which recent_touch() moves the
  played game to the front of on every launch and which is saved to the card: an ordered
  most-recent-first list, which is the same information for the twenty games it covers.

  So "Recently Played" can be a real order after all. It used to fall through to the play
  count, which made it identical to "Times Played" - two names for one order, and the
  documentation went as far as claiming there was nothing to sort it by. There was; it was
  in the next file down.

  The twenty is the honest limit: past that a game has no recency to compare, so those sort
  after every game that does, by title. A player looking at this order is looking for what
  they played last, and that is exactly what the list holds.
*/
static int recent_rank(uint32_t key)
{
	for (int i = 0; i < recent_nkeys; i++) if (recent_keys[i] == key) return i;
	return RECENT_MAX;
}

static int cmp_entry(const void *a, const void *b)
{
	const chome_entry *ea = (const chome_entry*)a;
	const chome_entry *eb = (const chome_entry*)b;

	// Folders always lead, in insertion order.
	if (ea->kind != ENT_GAME || eb->kind != ENT_GAME)
	{
		if (ea->kind == ENT_GAME) return 1;
		if (eb->kind == ENT_GAME) return -1;
		return 0;
	}

	const chome_item *ia = &items[ea->game];
	const chome_item *ib = &items[eb->game];

	switch (cur_sort)
	{
	case SORT_PLAYS:
		if (ia->plays != ib->plays) return (int)ib->plays - (int)ia->plays;
		break;
	case SORT_SYSTEM:
		if (ia->sysidx != ib->sysidx) return ia->sysidx - ib->sysidx;
		break;
	case SORT_RECENT:
	{
		int ra = recent_rank(ia->key), rb = recent_rank(ib->key);
		if (ra != rb) return ra - rb;              // lower rank = played more recently
		break;                                     // both unplayed, or both off the list
	}
	/*
	  Favourites first, and a SORT rather than a filter - Dinofly asked which it should be.

	  A filter already exists: Favourites is the first card on the shelf, so a view holding
	  only them is one press away and duplicating it here would give the same thing two
	  controls that could disagree. What a sort adds is the thing the filter cannot - the
	  whole library still browsable, with the ones you care about at the front, so walking
	  right past the end of your favourites lands you in everything else rather than in a
	  dead end.

	  Ties fall through to the title compare below, so within each group the order is the
	  one a player can predict rather than whatever the scan happened to produce.
	*/
	case SORT_FAVS:
		if (!ia->fav != !ib->fav) return ia->fav ? -1 : 1;
		break;
	default:
		break;
	}

	return strcasecmp(ia->title, ib->title);
}

// Confirms a count-pass hit: the payload is the item that opened the group.
static int same_group_item(int val, const char *key)
{
	char other[GRP_KEY_LEN];
	group_key(&items[val], other, sizeof(other));
	return !strcmp(key, other);
}

/*
  Counts what sits behind a folder, so the card and title can say so.

  Counted in titles, not in files, because that is what the shelf behind the folder will
  show and because "12 GAMES" over a shelf of ten cards is the folder lying about itself.
  Three regional dumps of one game are one game by the same argument that put them on one
  card.
*/
static int count_behind(int tview, int tsys)
{
	// Already resolved and already capped, and the loop below could express neither.
	// Ungrouped as well - see lib_view_build - so a file count is the card count here.
	if (tview == VIEW_RECENT) return recent_n;

	group_open();

	int c = 0;
	for (int i = 0; i < nitems; i++)
	{
		const chome_sys *s = lib_sys(items[i].sysidx);
		int mine = 0;
		switch (tview)
		{
		case VIEW_FAV: mine = items[i].fav ? 1 : 0; break;
		case VIEW_SYS: mine = (items[i].sysidx == tsys); break;
		case VIEW_ALL: mine = (s && !s->computer) ? 1 : 0; break;
		default: break;
		}
		if (!mine) continue;

		char key[GRP_KEY_LEN];
		group_key(&items[i], key, sizeof(key));

		grp_cell *cell = group_cell(key, same_group_item);
		if (!cell) { c++; continue; }        // no table: over-count rather than under
		if (cell->val >= 0) continue;        // another file of a title already counted

		cell->val = i;
		c++;
	}

	if (tview == VIEW_SYSTEMS || tview == VIEW_COMPUTERS)
	{
		int want = (tview == VIEW_COMPUTERS) ? 1 : 0;
		for (int i = 0; i < nsys; i++)
		{
			if (systems[i].computer != want) continue;
			for (int j = 0; j < nitems; j++) if (items[j].sysidx == i) { c++; break; }
		}
	}

	return c;
}

static void push_folder(const char *label, int tview, int tsys, const char *icon, int kind)
{
	if (nview >= VIEW_MAX) return;
	chome_entry *e = &view[nview++];
	memset(e, 0, sizeof(*e));
	e->kind = (uint8_t)kind;
	e->view = tview;
	e->sysidx = tsys;
	e->icon = icon;
	e->count = count_behind(tview, tsys);
	snprintf(e->label, sizeof(e->label), "%s", label);
}

static void push_game(int idx)
{
	if (nview >= VIEW_MAX) return;
	chome_entry *e = &view[nview++];
	memset(e, 0, sizeof(*e));
	e->kind = ENT_GAME;
	e->game = idx;
	e->nvar = 1;
	e->vhead = idx;
	var_next[idx] = -1;
}

// Confirms a grouping hit: the payload is the entry, and every file behind it shares
// the key, so the chain head answers for all of them.
static int same_group_entry(int val, const char *key)
{
	char other[GRP_KEY_LEN];
	group_key(&items[view[val].vhead], other, sizeof(other));
	return !strcmp(key, other);
}

/*
  Adds a game to the shelf, or behind the card that already carries its title.

  The chain is kept in filename order by inserting into it, which costs nothing: a title
  has two or three files, not hundreds.

  The table's generation is opened here, on the first grouped push of a build, rather
  than at the top of lib_view_build(): the folder cards are pushed first and
  count_behind() runs a pass of its own over the same cells while they are, so opening
  earlier would leave the games sharing a generation with the counting.
*/
static int grp_open_done = 0;

static void push_game_grouped(int idx)
{
	if (!grp_open_done) { group_open(); grp_open_done = 1; }

	char key[GRP_KEY_LEN];
	group_key(&items[idx], key, sizeof(key));

	grp_cell *cell = group_cell(key, same_group_entry);
	if (!cell || cell->val < 0)
	{
		int before = nview;
		push_game(idx);
		if (cell && nview > before) cell->val = before;
		return;
	}

	chome_entry *e = &view[cell->val];

	int *link = &e->vhead;
	while (*link >= 0 && strcasecmp(items[*link].path, items[idx].path) < 0) link = &var_next[*link];
	var_next[idx] = *link;
	*link = idx;

	e->nvar++;
}

/*
  Which file a card shows when the view is built.

  The one with the highest play count, then a favourite, then the first in filename
  order. That is deliberately not "the first file found": the version the player
  actually plays is the version the card should offer, and play counts and favourites
  are already per-file in classicui_state.cfg, so this needs no new state and survives
  a re-exec, a rescan and a reboot on its own.

  Widening state_rec to hold an explicit choice was the alternative and was rejected:
  classicui_state.cfg is a bare array of records with no magic and no version, so
  growing the record would make every existing file parse at the wrong stride and lose
  the player's favourites. The exact file the player last stood on is pinned separately,
  by lib_view_select_key() from the session record.
*/
static void group_pick_default()
{
	for (int i = 0; i < nview; i++)
	{
		chome_entry *e = &view[i];
		if (e->kind != ENT_GAME || e->nvar < 2) continue;

		int best = e->vhead, best_pos = 0, pos = 0;
		for (int k = e->vhead; k >= 0; k = var_next[k], pos++)
		{
			const chome_item *a = &items[k];
			const chome_item *b = &items[best];
			if (a->plays > b->plays || (a->plays == b->plays && a->fav && !b->fav))
			{
				best = k;
				best_pos = pos;
			}
		}

		e->game = best;
		e->vsel = best_pos;
	}
}

/*
  Confirms a duplicate-title hit. Folded to lower case on both sides because the hash
  the table probes with is not case-insensitive: a confirm that was would put "MEGA MAN"
  and "Mega Man" in different cells and then never compare them.
*/
static void lower_title(const char *in, char *out, int len)
{
	snprintf(out, len, "%s", in);
	for (char *p = out; *p; p++) *p = (char)tolower((unsigned char)*p);
}

static int same_title_entry(int val, const char *key)
{
	char mine[CH_TITLE_LEN];
	lower_title(items[view[val].game].title, mine, sizeof(mine));
	return !strcmp(mine, key);
}

/*
  Marks the cards whose title alone does not say which file they are.

  A grouped card knows this about itself, but the cases grouping deliberately refuses -
  a hack in its own folder, the .sms and .gg of one name, the same title on two systems,
  and Recently Played, which is not grouped at all - still put two identical titles on
  one shelf. Those are exactly the cards the title block has to name the file for, so
  they are found here rather than left as the fault this feature exists to fix.
*/
static void group_mark_dups()
{
	group_open();

	for (int i = 0; i < nview; i++)
	{
		if (view[i].kind != ENT_GAME) continue;

		char key[CH_TITLE_LEN];
		lower_title(items[view[i].game].title, key, sizeof(key));

		grp_cell *cell = group_cell(key, same_title_entry);
		if (!cell) continue;

		if (cell->val < 0) { cell->val = i; continue; }

		view[cell->val].dup = 1;
		view[i].dup = 1;
	}
}

int lib_view_build(int v, int sysidx, int sort)
{
	nview = 0;
	cur_sort = sort;

	int nfolders = 0;
	grp_open_done = 0;

	switch (v)
	{
	case VIEW_ROOT:
		push_folder("Favourites", VIEW_FAV, -1, "star", ENT_FOLDER);
		/*
		  Beside Favourites, because both are lists of the player's own games while
		  Systems and Computers are ways of browsing the card - but only once there is
		  something in it. An entry that opens on "NOTHING HERE" is worse than no entry,
		  and this one sits where the thumb lands.
		*/
		if (recent_n) push_folder("Recently Played", VIEW_RECENT, -1, "clock", ENT_FOLDER);
		push_folder("Systems", VIEW_SYSTEMS, -1, "stack", ENT_FOLDER);
		push_folder("Computers", VIEW_COMPUTERS, -1, "disk", ENT_FOLDER);
		nfolders = nview;
		for (int i = 0; i < nitems; i++)
		{
			const chome_sys *s = lib_sys(items[i].sysidx);
			if (s && !s->computer) push_game_grouped(i);
		}
		break;

	case VIEW_ALL:
		for (int i = 0; i < nitems; i++)
		{
			const chome_sys *s = lib_sys(items[i].sysidx);
			if (s && !s->computer) push_game_grouped(i);
		}
		break;

	case VIEW_FAV:
		for (int i = 0; i < nitems; i++) if (items[i].fav) push_game_grouped(i);
		break;

	case VIEW_RECENT:
		/*
		  The one shelf that is *not* grouped. Its content is a list of launches, and two
		  launches of two different files are two things that happened - collapsing them
		  would either lose the ordering that is this view's whole point, or contradict
		  the group's own choice of which file to show. The duplicate titles that leaves
		  are named by their file instead: see group_mark_dups().
		*/
		for (int i = 0; i < recent_n; i++) push_game(recent_items[i]);
		/*
		  Here the order is the whole answer, so the sort at the end of this function
		  must not reach it - whichever sort the player last chose would otherwise
		  scatter the one thing this view is for. Counting these as leading entries is
		  how the other views keep their folders out of the sort, and it does the same
		  job here.
		*/
		nfolders = nview;
		break;

	case VIEW_SYSTEMS:
		for (int i = 0; i < nsys; i++)
		{
			if (systems[i].computer) continue;
			int count = 0;
			for (int j = 0; j < nitems; j++) if (items[j].sysidx == i) count++;
			// The id doubles as the icon key, so each system can have its own.
			if (count) push_folder(systems[i].name, VIEW_SYS, i, systems[i].id, ENT_FOLDER);
		}
		nfolders = nview;
		break;

	case VIEW_COMPUTERS:
		for (int i = 0; i < nsys; i++)
		{
			if (!systems[i].computer) continue;
			push_folder(systems[i].name, VIEW_SYS, i, systems[i].id, ENT_BROWSE);
		}
		nfolders = nview;
		break;

	case VIEW_SYS:
		for (int i = 0; i < nitems; i++) if (items[i].sysidx == sysidx) push_game_grouped(i);
		break;
	}

	/*
	  Both before the sort, and the first of them has to be: cmp_entry() reads the item
	  behind each entry, and the files behind one card do not share a play count - so
	  sorting by Times Played before the card has settled on which file it stands for
	  would order it by a file it is not showing. Neither pass cares about the order
	  itself, and both are done with the hash table before qsort makes its entry indices
	  meaningless.
	*/
	group_pick_default();
	group_mark_dups();

	if (nview > nfolders)
	{
		qsort(view + nfolders, nview - nfolders, sizeof(chome_entry), cmp_entry);
	}

	return nview;
}

/* --------------------------------------------------- variants of a card --- */

int lib_view_variant(int entry, int which)
{
	const chome_entry *e = lib_view_entry(entry);
	if (!e || e->kind != ENT_GAME || which < 0 || which >= e->nvar) return -1;

	int k = e->vhead;
	while (which-- > 0 && k >= 0) k = var_next[k];
	return k;
}

/*
  Names a variant by the part of its path the other variants do not share. For the
  ordinary case that is the ROM filename, which is what the player needs to read; for a
  ROM inside a multi-ROM archive it keeps the archive in front of it, because there the
  archive name is shared and the member name is what differs.

  The folder is dropped only when the card has files to share it with. A card of one file
  is named here because some *other* card carries the same title (see group_mark_dups),
  and the other card may well be the same filename in another folder - two collections
  of one library, filed apart. Dropping the folder there would print the same line under
  both cards and answer nothing.
*/
const char *lib_view_variant_file(int entry, int which)
{
	static char buf[CH_PATH_LEN];
	buf[0] = 0;

	const chome_entry *e = lib_view_entry(entry);
	int k = lib_view_variant(entry, which);
	if (!e || k < 0) return buf;

	const char *path = items[k].path;
	int skip = 0;

	/*
	  The folder is dropped only when every file behind this card is in the same one - it
	  would be noise repeated on each line. Once the directory left the grouping key a card
	  can span folders, and then the folder is the *only* thing telling two files apart:
	  games/MD/Streets of Rage 2 (Europe).bin and games/MD/Proto/Streets of Rage 2
	  (Europe).bin have the same filename, and without this both lines read identically and
	  the player cannot tell which they are about to start.
	*/
	if (e->nvar > 1)
	{
		int mine = group_dir_len(path);
		int shared = 1;

		for (int i = 0; i < e->nvar && shared; i++)
		{
			int o = lib_view_variant(entry, i);
			if (o < 0 || o == k) continue;

			int od = group_dir_len(items[o].path);
			if (od != mine || strncasecmp(items[o].path, path, (size_t)mine)) shared = 0;
		}

		if (shared)
		{
			skip = mine;
			if (skip && path[skip] == '/') skip++;
		}
	}

	snprintf(buf, sizeof(buf), "%s", path + skip);
	return buf;
}

int lib_view_cycle(int entry, int dir)
{
	if (entry < 0 || entry >= nview) return 0;

	chome_entry *e = &view[entry];
	if (e->kind != ENT_GAME || e->nvar < 2) return 0;

	// Wraps, and only one direction is ever asked for: see the legend in chome_ui.cpp
	// for why there is one button rather than two.
	int next = e->vsel + ((dir < 0) ? -1 : 1);
	while (next < 0) next += e->nvar;
	next %= e->nvar;

	int idx = lib_view_variant(entry, next);
	if (idx < 0) return 0;

	e->vsel = next;
	e->game = idx;
	return 1;
}

// Both lookups below want the same walk: every card, and every file behind it.
static int view_select(uint32_t key, int sysidx, const char *relpath)
{
	for (int i = 0; i < nview; i++)
	{
		chome_entry *e = &view[i];
		if (e->kind != ENT_GAME) continue;

		int pos = 0;
		for (int k = e->vhead; k >= 0; k = var_next[k], pos++)
		{
			int hit = relpath
				? (items[k].sysidx == sysidx && !strcmp(items[k].path, relpath))
				: (items[k].key == key);
			if (!hit) continue;

			e->game = k;
			e->vsel = pos;
			return i;
		}
	}
	return -1;
}

int lib_view_select_key(uint32_t key)
{
	return key ? view_select(key, -1, 0) : -1;
}

int lib_view_select_path(int sysidx, const char *relpath)
{
	if (!relpath || !relpath[0]) return -1;
	return view_select(0, sysidx, relpath);
}

const char *lib_view_title(int v, int sysidx)
{
	static char buf[64];
	switch (v)
	{
	case VIEW_ROOT:      return "Home";
	case VIEW_ALL:       return "All Games";
	case VIEW_FAV:       return "Favourites";
	case VIEW_RECENT:    return "Recently Played";
	case VIEW_SYSTEMS:   return "Systems";
	case VIEW_COMPUTERS: return "Computers";
	case VIEW_SYS:
	{
		const chome_sys *s = lib_sys(sysidx);
		snprintf(buf, sizeof(buf), "%s", s ? s->name : "System");
		return buf;
	}
	}
	return "";
}
