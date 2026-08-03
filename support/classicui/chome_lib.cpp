#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
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
	{ "sms",   "Master System",                 "SMS",  "_Console/SMS",          "SMS",     "sms,gg,sg",    "Sega - Master System - Mark III",                'f', 0, 2, 0, 0, 0x7e3a2b, 0, CH_SS_YES     },
	{ "tg16",  "TurboGrafx-16",                 "TG16", "_Console/TurboGrafx16", "TGFX16",  "pce,sgx",      "NEC - PC Engine - TurboGrafx 16",                'f', 0, 2, 0, 0, 0x8a6e2b, 0, CH_SS_NO      },
	{ "a7800", "Atari 7800",                    "A78",  "_Console/Atari7800",    "A7800",   "a78,a26,bin",  "Atari - 7800",                                   'f', 0, 2, 0, 0, 0x6e2b2b, 0, CH_SS_NO      },
	{ "psx",   "PlayStation",                   "PSX",  "_Console/PSX",          "PSX",     "cue,chd,exe",  "Sony - PlayStation",                             's', 1, 3, 0, 0, 0x4a4c58, 0, CH_SS_YES     },
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

int lib_sys_games_dir(int sysidx, char *out, int len)
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

static void add_item(int sysidx, const char *relpath, const char *filename)
{
	if (nitems >= CH_MAX_ITEMS)
	{
		static int warned = 0;
		if (!warned)
		{
			warned = 1;
			printf("ClassicUI: index full at %d items, the rest of the library is not listed\n", CH_MAX_ITEMS);
		}
		return;
	}

	chome_item *it = &items[nitems];
	memset(it, 0, sizeof(*it));
	it->kind = IT_GAME;
	it->sysidx = (int16_t)sysidx;
	snprintf(it->path, sizeof(it->path), "%s", relpath);
	clean_title(filename, it->title, sizeof(it->title));

	char keybuf[CH_PATH_LEN + 32];
	snprintf(keybuf, sizeof(keybuf), "%s/%s", systems[sysidx].id, relpath);
	it->key = hash32(keybuf);

	nitems++;
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

	printf("ClassicUI: index cache hit, %d items%s\n",
		nitems, idx_truncated ? " (validation incomplete: use Rescan if a game is missing)" : "");
	return 1;
}

/* --------------------------------------------------------------- scan ----- */

static int scan_sys = 0;
static int scanning = 0;

static void scan_dir(int sysidx, const char *root, const char *rel, int depth)
{
	if (depth > 3 || nitems >= CH_MAX_ITEMS) return;

	char full[1024];
	if (rel[0]) snprintf(full, sizeof(full), "%s/%s", root, rel);
	else snprintf(full, sizeof(full), "%s", root);

	DIR *d = opendir(full);
	if (!d) return;

	idx_note_dir(full);

	struct dirent *de;
	while ((de = readdir(d)))
	{
		if (de->d_name[0] == '.') continue;
		if (nitems >= CH_MAX_ITEMS) break;

		char childrel[CH_PATH_LEN];
		if (rel[0]) snprintf(childrel, sizeof(childrel), "%s/%s", rel, de->d_name);
		else snprintf(childrel, sizeof(childrel), "%s", de->d_name);

		char childfull[1024];
		snprintf(childfull, sizeof(childfull), "%s/%s", full, de->d_name);

		int isdir;
		if (de->d_type == DT_DIR) isdir = 1;
		else if (de->d_type == DT_REG) isdir = 0;
		else
		{
			struct stat st;
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
			if (systems[sysidx].mra && de->d_name[0] == '_') continue;

			/*
			  A romset can be a folder of member files rather than an archive of them -
			  that is how the Darksoft packs ship - and then the folder is the game, not
			  something to walk into. The loader takes either: file_io opens
			  "romset/prom" and "romset.zip/prom" alike.
			*/
			if (systems[sysidx].romset && dir_is_romset(childfull))
			{
				char buf[CH_TITLE_LEN];
				const char *title = romset_title(full, de->d_name, buf, sizeof(buf));
				if (title) add_item(sysidx, childrel, title);
				continue;
			}

			scan_dir(sysidx, root, childrel, depth + 1);
		}
		else if (ext_matches(de->d_name, systems[sysidx].ext))
		{
			if (systems[sysidx].romset)
			{
				char buf[CH_TITLE_LEN];
				const char *title = romset_title(full, de->d_name, buf, sizeof(buf));
				if (title) add_item(sysidx, childrel, title);
			}
			else add_item(sysidx, childrel, de->d_name);
		}
		else if (is_zip_name(de->d_name) && !sys_takes_zip(sysidx))
		{
			char inner[ZIP_MAX_ENTRIES][CH_PATH_LEN];
			int n = zip_playables(childfull, systems[sysidx].ext, inner, ZIP_MAX_ENTRIES);

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
				add_item(sysidx, p, (n == 1) ? de->d_name : inner[i]);
			}
		}
	}

	closedir(d);
}

int lib_scan_step()
{
	if (!scanning) return 0;

	if (scan_sys >= nsys)
	{
		scanning = 0;
		for (int i = 0; i < nitems; i++) state_apply(&items[i]);
		printf("ClassicUI: scan complete, %d items\n", nitems);
		idx_save();
		return 0;
	}

	char root[1024];
	if (lib_sys_games_dir(scan_sys, root, sizeof(root)))
	{
		int before = nitems;

		// Romset titles come out of romsets.xml, which has to be read before the
		// folder is walked so every entry can be looked up as it is found.
		if (systems[scan_sys].romset) neogeo_scan_xml(root);

		scan_dir(scan_sys, root, "", 0);
		printf("ClassicUI: %s -> %d items\n", systems[scan_sys].name, nitems - before);
	}

	scan_sys++;
	return 1;
}

int lib_scanning() { return scanning; }
int lib_scan_progress() { return nitems; }

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

static void resolve_names()
{
	for (int i = 0; i < nsys; i++)
	{
		chome_sys *s = &systems[i];

		if (s->rbf[0])
		{
			char dir[80];
			snprintf(dir, sizeof(dir), "%s", s->rbf);
			char *slash = strrchr(dir, '/');
			if (slash)
			{
				*slash = 0;
				const char *base = slash + 1;
				if (!rbf_present(dir, base))
				{
					const char *alt = alias_pick(base, dir, 0);
					if (alt)
					{
						printf("ClassicUI: %s core is %s/%s on this card\n", s->id, dir, alt);
						snprintf(s->rbf, sizeof(s->rbf), "%s/%s", dir, alt);
					}
				}
			}
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

	if (!load_systems_file())
	{
		for (size_t i = 0; i < sizeof(defaults) / sizeof(defaults[0]); i++) add_sys(&defaults[i]);
	}

	resolve_names();
	state_load();
}

static void lib_init_common(int use_cache)
{
	if (!items) items = (chome_item*)calloc(CH_MAX_ITEMS, sizeof(chome_item));
	if (!items) { printf("ClassicUI: out of memory for the index\n"); return; }

	nitems = 0;
	idx_ndirs = 0;
	idx_truncated = 0;
	idx_from_cache = 0;

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
	unlink(idx_path());
	lib_init_common(0);
}

/* --------------------------------------------------------------- views ---- */

#define VIEW_MAX 6100
static chome_entry view[VIEW_MAX];
static int nview = 0;

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
	}
	return "?";
}

static int cur_sort = SORT_TITLE;

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
		if (ia->plays != ib->plays) return (int)ib->plays - (int)ia->plays;
		break;
	default:
		break;
	}

	return strcasecmp(ia->title, ib->title);
}

// Counts what sits behind a folder, so the card and title can say so.
static int count_behind(int tview, int tsys)
{
	int c = 0;
	for (int i = 0; i < nitems; i++)
	{
		const chome_sys *s = lib_sys(items[i].sysidx);
		switch (tview)
		{
		case VIEW_FAV: if (items[i].fav) c++; break;
		case VIEW_SYS: if (items[i].sysidx == tsys) c++; break;
		case VIEW_ALL: if (s && !s->computer) c++; break;
		default: break;
		}
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
}

int lib_view_build(int v, int sysidx, int sort)
{
	nview = 0;
	cur_sort = sort;

	int nfolders = 0;

	switch (v)
	{
	case VIEW_ROOT:
		push_folder("Favourites", VIEW_FAV, -1, "star", ENT_FOLDER);
		push_folder("Systems", VIEW_SYSTEMS, -1, "stack", ENT_FOLDER);
		push_folder("Computers", VIEW_COMPUTERS, -1, "disk", ENT_FOLDER);
		nfolders = nview;
		for (int i = 0; i < nitems; i++)
		{
			const chome_sys *s = lib_sys(items[i].sysidx);
			if (s && !s->computer) push_game(i);
		}
		break;

	case VIEW_ALL:
		for (int i = 0; i < nitems; i++)
		{
			const chome_sys *s = lib_sys(items[i].sysidx);
			if (s && !s->computer) push_game(i);
		}
		break;

	case VIEW_FAV:
		for (int i = 0; i < nitems; i++) if (items[i].fav) push_game(i);
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
		for (int i = 0; i < nitems; i++) if (items[i].sysidx == sysidx) push_game(i);
		break;
	}

	if (nview > nfolders)
	{
		qsort(view + nfolders, nview - nfolders, sizeof(chome_entry), cmp_entry);
	}

	return nview;
}

const char *lib_view_title(int v, int sysidx)
{
	static char buf[64];
	switch (v)
	{
	case VIEW_ROOT:      return "Home";
	case VIEW_ALL:       return "All Games";
	case VIEW_FAV:       return "Favourites";
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
