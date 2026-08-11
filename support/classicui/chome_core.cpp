/*
  Reading the running core's own options out of its CONF_STR.

  The firmware already parses these strings for the classic OSD; this walks the same
  entries and keeps the ones a player should be offered. CORE-OPTIONS.md has the grammar
  and the reasoning behind the tiers - what follows is only the part that has to be right.
*/

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>

#include "../../user_io.h"
#include "../../spi.h"
#include "../../file_io.h"
#include "chome_lib.h"
#include "chome_core.h"

static core_opt opts[CO_MAX];
static int nopts = 0;

/* ------------------------------------------------------------- curation ---- */

/*
  Settings we own ourselves. Offering a second control for the same thing is worse than
  offering none: two screens would disagree and the player would have no way to tell
  which one won.

  "Pause when OSD is open" is the one to be careful about. Every core has it, and it is
  the mechanism the in-game menu's freeze depends on - a player switching it off would
  silently change what happens when they open the menu. We set it; we do not offer it.
*/
static const char *ours[] =
{
	"Savestate Slot", "SaveState Slot", "Savestates to SDCard", "Save state to SD",
	"Save to SDCard", "Autoincrement Slot", "Autosave", "Cheats Enabled", "Cheats enabled",
	"Pause when OSD is open", "Pause When OSD is Open", "Pause when OSD open",
	"State Ld/Sv", "Rewind Capture", "SNAC Compare",
	"Automount Memory Card 1", "Storage", "SPU RAM select",
	/*
	  The SNAC rows are NOT on this list any more, and that is the point of them.

	  They were ours while snac_psx decided who read the SNAC port. It does not exist:
	  core_owns_snac() in snacpad.cpp reads these very rows to decide, so offering them is
	  offering the control itself rather than a second one - and hiding them meant a player
	  who wanted a light gun, a wheel or real memory cards had to leave this front-end for
	  the classic OSD to get them. "SNAC Compare" stays hidden because it is a debug aid and
	  hands the port to nobody.

	  Each carries per-value help, because the consequence differs per value rather than per
	  row - see co_help_for() in chome_ui.cpp.
	*/
	/*
	  And the three that overlap Display looks. These are core-side video controls, and
	  the front-end already drives the scaler's filters, mask and gamma through presets.
	  Showing both would give one picture two competing controls; see the note in
	  CORE-OPTIONS.md, which wants deciding before either of these appears here.
	*/
	"Aspect ratio", "Aspect Ratio", "Scale", "Scandoubler Fx",
	0
};

// The reason someone opens this screen: a visible change, and safe.
static const char *picture[] =
{
	"Palette", "Custom Palette", "Super Game Boy", "Super Game Boy + GBC", "GBC Colors",
	"Inverted color", "Screen Shadow", "Frame blend", "Flickerblend", "Modify Colors",
	"Extra Sprites", "Extra sprites", "Spritelimit", "Sprites Per Line", "2XResolution",
	"Pseudo Transparency", "Force 256px", "Mask Edges", "Masked Left Column", "Overscan",
	"Border", "320x224 Aspect", "Composite Blend", "CRAM Dots", "Game Gear Res.",
	"Orientation", "Flip Screen", "Vertical Crop", "Crop Offset", "Crop Vertical",
	"Horizontal Crop", "Widescreen Hack", "Dithering", "Dither 24 Bit for VGA",
	"Texture Filter", "LOD Textures", "Deinterlacing", "Rotate", "Video Out",
	"VI Deblur", "VI Antialias", "VI Bilinear", "VI Divot", "VI Noisedither",
	"VI Dedither", "VI Gamma", "VI Colorbits", "Black Transitions",
	0
};

/*
  Worth reaching for, not worth meeting first: region, sound, and the hardware character
  options that change how a machine behaves rather than how it looks.
*/
static const char *system_tier[] =
{
	"System Type", "Video Region", "Region", "Auto Region", "TV System", "System",
	"Priority", "TMSS", "Mapper", "SMS BIOS", "GG BIOS", "ROM Header", "RAM Clear",
	"PPU Reset Behavior", "Initial WRAM", "Initial ARAM", "Audio Clock", "Audio mode",
	"Audio Enable", "Audio Filter", "FM Chip", "SMS FM Chip", "SMS FM Sound",
	"Stereo Mix", "Stereo mix", "Analog width", "Save Type", "RTC", "Fastboot",
	"Sync core to video", "Fixed Video Blanks", "Stabilize video(buffer)",
	"Sync 480i for HDMI", "480i to 480p Hack", "Disk Speed", "Disk Swap",
	0
};

/*
  The overclock and timing family. Dinofly asked for it and it goes in, but on its own page:
  this is the set that breaks games, and PSX says so itself - see the (U) detection below.
*/
static const char *risky[] =
{
	"Underclock CPU", "Z80 Speed", "Turbo(Cheats Off)", "CD Speed", "CD Fast Seek",
	"Limit Max CD Speed", "GPU Slowdown", "PAL 60Hz Hack", "RAM(Homebrew)", "RAM size",
	"Fast RAM access", "Fast ROM access", "Old GPU(CXD8514Q)", "Homebrew BIOS(Reset!)",
	"GPIO HACK(RTC+Rumble)", "Pause when CD slow", "VDPs", "PSGs",
	0
};

static int in_list(const char *const *list, const char *name)
{
	for (int i = 0; list[i]; i++) if (!strcasecmp(list[i], name)) return 1;
	return 0;
}

static int tier_for(const core_opt *o)
{
	if (in_list(ours, o->name)) return CO_TIER_HIDDEN;

	// The core's own word for "this can crash", so it lands on the risky page whatever
	// else we think of it. No list to keep in step with the cores.
	if (o->unsafe) return CO_TIER_RISKY;

	// Debug pages are why the classic OSD stays reachable.
	if (strcasestr(o->page, "debug")) return CO_TIER_HIDDEN;

	if (in_list(picture, o->name)) return CO_TIER_PICTURE;
	if (in_list(risky, o->name)) return CO_TIER_RISKY;
	if (in_list(system_tier, o->name)) return CO_TIER_SYSTEM;

	// Anything a core adds that we have never seen: offer it, but not first.
	return CO_TIER_SYSTEM;
}

/* -------------------------------------------------------------- parsing ---- */

/*
  Prefixes arrive in either order - "P1O[3:1]" and "D1P1O[104]" both occur in real cores -
  so both are stripped in a loop rather than in a fixed sequence. Assuming an order is how
  the first pass over this data lost every option that had a mask in front of its page,
  including the whole of the N64's VI filter group.
*/
static int split_prefix(const char *spec, uint32_t hdmask, int *page, int *hide, int *dis,
	const char **body)
{
	const char *p = spec;
	int h = 0, d = 0, pg = -1;

	for (;;)
	{
		if ((p[0] == 'H' || p[0] == 'h' || p[0] == 'D' || p[0] == 'd') && p[1] && p[2])
		{
			char m[2] = { p[1], 0 };
			int flg = (hdmask & (1u << user_io_hd_mask(m))) ? 1 : 0;
			if (p[0] == 'H') h |= flg;
			if (p[0] == 'h') h |= (flg ^ 1);
			if (p[0] == 'D') d |= flg;
			if (p[0] == 'd') d |= (flg ^ 1);
			p += 2;
			continue;
		}

		// A page prefix, as opposed to a page *definition* ("P1,Audio & Video").
		if (p[0] == 'P' && p[1] >= '0' && p[1] <= '9' && p[2] && p[2] != ',')
		{
			pg = p[1] - '0';
			p += 2;
			continue;
		}
		break;
	}

	*page = pg;
	*hide = h;
	*dis = d;
	*body = p;
	return 1;
}

// One comma-separated field, unescaped only as far as we need: cores do not quote.
static void field(const char *src, int idx, char *out, int len)
{
	out[0] = 0;
	int n = 0;
	const char *p = src;
	while (n < idx)
	{
		const char *c = strchr(p, ',');
		if (!c) return;
		p = c + 1;
		n++;
	}
	const char *e = strchr(p, ',');
	int l = e ? (int)(e - p) : (int)strlen(p);
	if (l > len - 1) l = len - 1;
	memcpy(out, p, l);
	out[l] = 0;
}

int core_opts_scan()
{
	nopts = 0;

	if (is_menu()) return 0;

	uint32_t hdmask = spi_uio_cmd16(UIO_GET_OSDMASK, 0);
	char pagename[10][24] = {};

	/*
	  From index 2, which is where the stock OSD starts (menu.cpp:2023, "add options as
	  requested by core": `int i = 2; do { p = user_io_get_confstr(i++); ...} while (p);`).

	  Index 1 is a slot the stock menu never reads, and a core is free to leave it empty.
	  user_io_get_confstr() returns NULL for an empty entry rather than "" (user_io.cpp:3094),
	  and this loop breaks on NULL - so starting at 1 meant that on any core with an empty
	  index 1 the scan ended before it began and the core appeared to have no options at all.

	  The Saturn core is exactly that core. Its config string reads

	      0 = Saturn
	      1 = <empty>                     <-- scan stopped here
	      2 = S0,CUECHD,Insert Disc
	      ...
	      8 = O[35:33],Region,Japan,Taiwan,USA,Brazil,Korea,Asia,Europe,Auto

	  so Region was unreachable from this front-end. That is not a cosmetic gap: a Saturn
	  with Region on its default of Japan refuses every USA and European disc with "Game
	  disc unsuitable for this system", and the one screen that could have fixed it was
	  hidden - mb_visible(MB_CORE) asks core_opts_tier_count(), which was 0 for every tier
	  because nothing had been parsed. Measured on the device on 2026-08-11: neither a disc
	  we ripped nor a known-good Redump dump would boot, and no core-options entry appeared
	  on the bar to say why.

	  Breaking on NULL is kept, because that is also what the stock menu does: the config
	  string is terminated by its first empty entry, so continuing past one would offer
	  options the stock OSD does not.
	*/
	for (int i = 2; i < 64 && nopts < CO_MAX; i++)
	{
		char *line = user_io_get_confstr(i);
		if (!line) break;
		if (!*line) continue;

		char spec[40];
		field(line, 0, spec, sizeof(spec));
		if (!spec[0]) continue;

		// A page definition: remember its name for the options that reference it.
		if (spec[0] == 'P' && spec[1] >= '0' && spec[1] <= '9' && !spec[2])
		{
			char nm[24];
			field(line, 1, nm, sizeof(nm));
			char *sc = strchr(nm, ';');
			if (sc) *sc = 0;
			int n = spec[1] - '0';
			if (n >= 0 && n < 10) snprintf(pagename[n], sizeof(pagename[n]), "%s", nm);
			continue;
		}

		int pg, hide, dis;
		const char *body;
		split_prefix(spec, hdmask, &pg, &hide, &dis, &body);

		// Only settings. Triggers, file selectors, labels and joystick names are not ours
		// to draw, and a momentary reset in a settings list is a trap.
		if (body[0] != 'O' && body[0] != 'o') continue;
		if (hide) continue;

		core_opt *o = &opts[nopts];
		memset(o, 0, sizeof(*o));

		snprintf(o->spec, sizeof(o->spec), "%s", body + 1);
		o->ex = (body[0] == 'o') ? 1 : 0;
		field(line, 1, o->name, sizeof(o->name));
		if (!o->name[0]) continue;

		if (pg >= 0 && pg < 10) snprintf(o->page, sizeof(o->page), "%s", pagename[pg]);
		o->disabled = dis ? 1 : 0;

		for (int v = 0; v < CO_VALS; v++)
		{
			char val[CO_VAL_LEN];
			field(line, 2 + v, val, sizeof(val));
			if (!val[0]) break;
			snprintf(o->vals[v], sizeof(o->vals[v]), "%s", val);
			if (strstr(val, "(U)")) o->unsafe = 1;
			o->nvals = (uint8_t)(v + 1);
		}

		// A single value is a label the core drew as an option; nothing to choose.
		if (o->nvals < 2) continue;

		o->tier = (uint8_t)tier_for(o);
		if (o->tier == CO_TIER_HIDDEN) continue;

		nopts++;
	}

	printf("ClassicUI: core options - %d offered", nopts);
	for (int t = 1; t <= 3; t++) printf(", tier%d=%d", t, core_opts_tier_count(t));
	printf(" (mask %04x)\n", hdmask);
	return nopts;
}

int core_opts_count() { return nopts; }

const core_opt *core_opt_at(int i)
{
	return (i >= 0 && i < nopts) ? &opts[i] : 0;
}

int core_opts_tier_count(int tier)
{
	int n = 0;
	for (int i = 0; i < nopts; i++) if (opts[i].tier == tier) n++;
	return n;
}

const core_opt *core_opt_tier_at(int tier, int idx)
{
	int n = 0;
	for (int i = 0; i < nopts; i++)
	{
		if (opts[i].tier != tier) continue;
		if (n == idx) return &opts[i];
		n++;
	}
	return 0;
}

/* ------------------------------------------------------------ read/write --- */

/*
  o->ex is not optional, on any of the four calls in this file that address the bits.

  "O" and "o" are different status words: user_io_status_bits() adds 32 to the start and
  end when ex is set, so "oJK" is bits 41-42 and not bits 9-10. Passing 0 for an "o"
  option therefore reads and writes a perfectly valid *different* option - the NES's
  SNAC row would show, and set, whatever lives at bits 9-10 instead. It is silent both
  ways, which is why it survived: the row draws, the press is accepted, and the damage
  lands on a setting the player was not looking at. The stock OSD gets this right at
  menu.cpp:2593 (int ex = (p[0] == 'o')) and this file did not.
*/
/*
  The rows that decide who reads the SNAC port, and why they are not written immediately.

  Every other core option applies the moment it is changed, which is the right behaviour and
  what the screen's footer promises. These four cannot, because of what they do to the pad
  the player is holding: choosing SNAC-port1 is how you tell the core to read the port
  itself, so the instant it is written our reader lets go, the uinput pad disappears, and
  the player has just used that pad to arrive at a value they can no longer move off. To
  correct a mistake they would have to find a second controller.

  Reported from hardware, and the earlier fix - releasing held buttons before destroying the
  device - only cured the stuck direction. It could not fix this, because the pad is
  genuinely gone by design; the problem is *when*.

  So these are staged and written when the menu closes. Browsing the values costs nothing,
  the choice takes effect on the way out, and one press of B abandons it.

  Matched by name rather than by value: a change *away* from SNAC is staged too. That is
  deliberate - it keeps the rule "this row applies when you leave" true in both directions
  rather than only in the interesting one, and a player who is on a USB pad because the core
  owns the port loses nothing by the wait.
*/
static int co_is_snac_owner_row(const core_opt *o)
{
	if (!o) return 0;
	return !strcasecmp(o->name, "Pad1") || !strcasecmp(o->name, "Pad2")
		|| !strncasecmp(o->name, "Pad ", 4) || !strcasecmp(o->name, "SNAC")
		|| !strcasecmp(o->name, "USERIO");
}

#define CO_PEND_MAX 8
static struct { char spec[12]; uint8_t ex; uint8_t set; int value; } co_pend[CO_PEND_MAX];

static int co_pend_find(const core_opt *o)
{
	for (int i = 0; i < CO_PEND_MAX; i++)
	{
		if (co_pend[i].set && co_pend[i].ex == o->ex && !strcmp(co_pend[i].spec, o->spec)) return i;
	}
	return -1;
}

void core_opts_pending_clear()
{
	for (int i = 0; i < CO_PEND_MAX; i++) co_pend[i].set = 0;
}

int core_opts_pending()
{
	for (int i = 0; i < CO_PEND_MAX; i++) if (co_pend[i].set) return 1;
	return 0;
}

/*
  Write the staged rows and forget them. Called when the menu closes, so the core learns
  about the change at the moment the player stops needing the pad to navigate with.
*/
void core_opts_pending_apply()
{
	for (int i = 0; i < CO_PEND_MAX; i++)
	{
		if (!co_pend[i].set) continue;
		user_io_status_set(co_pend[i].spec, (uint32_t)co_pend[i].value, co_pend[i].ex);
		printf("ClassicUI: applying staged core option [%s] = %d on the way out\n",
			co_pend[i].spec, co_pend[i].value);
		co_pend[i].set = 0;
	}
}

int core_opt_value(const core_opt *o)
{
	if (!o) return 0;

	// A staged value is what the player chose, so it is what the row must show.
	int p = co_pend_find(o);
	if (p >= 0) return (co_pend[p].value < o->nvals) ? co_pend[p].value : 0;

	uint32_t v = user_io_status_get(o->spec, o->ex);
	return (v < o->nvals) ? (int)v : 0;
}

void core_opt_set(const core_opt *o, int value)
{
	if (!o || o->nvals < 2) return;
	if (value < 0) value = o->nvals - 1;
	if (value >= o->nvals) value = 0;

	// Staged, not written - see co_is_snac_owner_row() above for why these four wait.
	if (co_is_snac_owner_row(o))
	{
		int p = co_pend_find(o);
		if (p < 0)
		{
			for (int i = 0; i < CO_PEND_MAX && p < 0; i++) if (!co_pend[i].set) p = i;
		}
		if (p >= 0)
		{
			snprintf(co_pend[p].spec, sizeof(co_pend[p].spec), "%s", o->spec);
			co_pend[p].ex = o->ex;
			co_pend[p].value = value;
			co_pend[p].set = 1;
			printf("ClassicUI: core option %s = %s (staged until the menu closes)\n",
				o->name, o->vals[value]);
			return;
		}
		// No room to stage it. Writing it now is worse than losing it: it would take the
		// player's pad away mid-list. Refuse, and say so rather than appearing to work.
		printf("ClassicUI: cannot stage %s - too many pending, left unchanged\n", o->name);
		return;
	}

	user_io_status_set(o->spec, (uint32_t)value, o->ex);
	printf("ClassicUI: core option %s = %s\n", o->name, o->vals[value]);
}

void core_opts_save()
{
	char *name = user_io_create_config_name(1);
	if (!name || !name[0]) return;

	user_io_status_save(name);
	printf("ClassicUI: wrote the core's config (%s)\n", name);
}

/* --------------------------------------------------------- per-game opts --- */

/*
  See chome_core.h for what this is and why the records name their option and their
  value rather than numbering them.
*/
#define CO_PG_MAX  128
#define CO_PG_FILE "classicui_coreopts.cfg"

/*
  One record per (game, option). A uint32 followed by nothing but char arrays, so the
  compiler has no padding to insert and the file means the same thing on the device as
  it does in the host harness - which is the only reason a raw struct dump is a safe
  file format at all. Keep it that way if a field is ever added.
*/
struct co_pg
{
	uint32_t key;                 // game: hash of "<system id>/<relative path>"
	char name[CO_NAME_LEN];       // the option, as the core names it
	char val[CO_VAL_LEN];         // the value this game keeps
	char base[CO_VAL_LEN];        // what the core's own config held, so undo can undo
};

static co_pg pgrecs[CO_PG_MAX];
static int npgrecs = 0;
static int pg_loaded = 0;
static uint32_t pg_bound = 0;

static void pg_load()
{
	if (pg_loaded) return;
	pg_loaded = 1;

	memset(pgrecs, 0, sizeof(pgrecs));
	int len = FileLoadConfig(CO_PG_FILE, pgrecs, sizeof(pgrecs));
	npgrecs = (len > 0) ? len / (int)sizeof(co_pg) : 0;
	if (npgrecs > CO_PG_MAX) npgrecs = CO_PG_MAX;

	if (npgrecs) printf("ClassicUI: %d per-game core settings\n", npgrecs);
}

static void pg_save()
{
	FileSaveConfig(CO_PG_FILE, pgrecs, npgrecs * (int)sizeof(co_pg));
}

uint32_t core_opts_game_key(int sysidx, const char *relpath)
{
	const chome_sys *s = lib_sys(sysidx);
	if (!s || !relpath || !relpath[0]) return 0;

	/*
	  Deliberately the same string chome_lib.cpp hashes for chome_item.key, and the same
	  FNV-1a over it, so the two cannot drift apart and mean different games by the same
	  number. Not quite identical: 0 is this module's "no game bound" sentinel, so a string
	  hashing to 0 is stored as 1 here where the library would keep 0. Nothing
	  cross-references the two values, so that is a difference without a consequence -
	  noted because a comment claiming they are the same would be the sort of thing someone
	  later relies on.
	*/
	char buf[CH_PATH_LEN + 32];
	snprintf(buf, sizeof(buf), "%s/%s", s->id, relpath);

	uint32_t h = 2166136261u;
	for (const char *p = buf; *p; p++) { h ^= (unsigned char)*p; h *= 16777619u; }
	return h ? h : 1;
}

void core_opts_bind_game(uint32_t key) { pg_bound = key; }
uint32_t core_opts_bound_game() { return pg_bound; }

static co_pg *pg_find(uint32_t key, const char *name)
{
	if (!key || !name || !name[0]) return 0;
	for (int i = 0; i < npgrecs; i++)
	{
		if (pgrecs[i].key != key) continue;
		if (!strcasecmp(pgrecs[i].name, name)) return &pgrecs[i];
	}
	return 0;
}

// The index of a value by its name, -1 when the core no longer publishes it.
static int pg_val_index(const core_opt *o, const char *val)
{
	if (!o || !val || !val[0]) return -1;
	for (int v = 0; v < o->nvals; v++) if (!strcmp(o->vals[v], val)) return v;
	return -1;
}

/*
  Nothing the front-end owns may ever become per-game, whatever a record says.

  The screen cannot offer them - they never reach the option table, tier_for() drops
  them - but the file is a file, and a stale record from an older build or an edited
  card must not be able to reach one either. "Pause when OSD is open" is the reason
  this is a check and not a comment: it is the mechanism the in-game menu's freeze
  depends on, and a per-game copy of it would silently change what the menu button
  does on one game only.
*/
static int pg_allowed(const char *name)
{
	return (name && name[0] && !in_list(ours, name)) ? 1 : 0;
}

int core_opt_per_game(const core_opt *o)
{
	if (!o || !pg_bound) return 0;
	pg_load();
	return pg_find(pg_bound, o->name) ? 1 : 0;
}

void core_opt_keep_for_game(const core_opt *o, int value, int global)
{
	if (!o || !pg_bound) return;
	if (value < 0 || value >= o->nvals) return;
	if (!pg_allowed(o->name)) return;

	pg_load();

	co_pg *r = pg_find(pg_bound, o->name);
	if (!r)
	{
		if (npgrecs >= CO_PG_MAX)
		{
			printf("ClassicUI: no room for another per-game core setting (%d)\n", CO_PG_MAX);
			return;
		}
		r = &pgrecs[npgrecs++];
		memset(r, 0, sizeof(*r));
		r->key = pg_bound;
		snprintf(r->name, sizeof(r->name), "%s", o->name);

		/*
		  Only on the first override: this is the one moment the value being replaced is
		  known to be the core's own, because nothing of ours has touched it yet. Later
		  changes to the same option must not overwrite it with another per-game value,
		  or "put the shared value back" would put back the player's own last choice.
		*/
		if (global >= 0 && global < o->nvals)
			snprintf(r->base, sizeof(r->base), "%s", o->vals[global]);
	}

	snprintf(r->val, sizeof(r->val), "%s", o->vals[value]);
	pg_save();

	printf("ClassicUI: %s = %s kept for this game only (shared value %s)\n",
		o->name, r->val, r->base[0] ? r->base : "unknown");
}

// Drop a record and write the store. The list has no order that means anything - every
// lookup is by (key, name) - so the hole is filled from the end rather than shifted.
static void pg_forget(co_pg *r)
{
	int i = (int)(r - pgrecs);
	if (i < npgrecs - 1) pgrecs[i] = pgrecs[npgrecs - 1];
	npgrecs--;
	pg_save();
}

int core_opt_drop_for_game(const core_opt *o)
{
	if (!o || !pg_bound) return 0;
	pg_load();

	co_pg *r = pg_find(pg_bound, o->name);
	if (!r) return 0;

	/*
	  Put the shared value back on the core before forgetting where it was, so the row
	  changes under the player's cursor. A clear that leaves the screen exactly as it
	  was until the next launch is the kind of undo nobody believes happened.
	*/
	int back = pg_val_index(o, r->base);
	if (back >= 0) core_opt_set(o, back);

	printf("ClassicUI: %s is no longer kept for this game%s\n", o->name,
		(back >= 0) ? ", shared value restored" : " (its shared value is gone from the core)");

	pg_forget(r);
	return 1;
}

/* -------------------------------------------------- one option, shared ----- */

/*
  How big the shared config is. <CORE>.CFG is a raw dump of user_io.cpp's cur_status[16],
  so sixteen bytes is what user_io_status_save() always writes and what the core reads back
  at boot. The buffer is larger and the length actually read is what gets written again, so
  a firmware that one day grows the status word cannot have its file truncated by this.
*/
#define CO_CFG_BYTES 16
#define CO_CFG_MAX   64

int core_opt_can_promote(const core_opt *o)
{
	if (!o || !pg_bound) return 0;
	if (!pg_allowed(o->name)) return 0;

	// A spec this firmware cannot address and a core with no config name are both
	// "promotion is impossible", and the screen asks this before it offers the press.
	if (!user_io_status_bits(o->spec, 0, 0, o->ex)) return 0;

	char *name = user_io_create_config_name(1);
	if (!name || !name[0]) return 0;

	pg_load();
	return pg_find(pg_bound, o->name) ? 1 : 0;
}

/*
  Making one option shared, from inside a game, without writing the status word.

  The trap this exists to avoid: user_io_status_save() persists by dumping the *live*
  status word, and while a game with overrides is running that word is that game's. Save it
  and PSX's widescreen hack, its region and its dithering all become the core's own values
  because one of them was meant to. That is why README.md carried "no way to make a value
  the shared one from in there" as a limitation rather than a bug, and it is the reason the
  classic OSD is still a trap when it is opened over a game with overrides: its save is the
  same call.

  What makes the narrow write possible is that <CORE>.CFG has no format. It is cur_status[]
  memcpy'd to a file (see user_io_status_save), so the file *is* the shared status word,
  byte for byte and bit for bit - and the running core's copy of it need not be involved at
  all. Read the file, put this option's bits in it, write it back. Every other option in
  the result is the bytes that were already there, so the shared value of everything this
  game overrides survives untouched. Nothing is asked of the core and nothing is asked of
  the classic OSD.

  A missing file is sixteen zero bytes rather than a failure. That is not a guess: it is
  what the core boots from when there is no config, because user_io.cpp memsets cur_status
  before it tries to load one. So zeros *are* the shared config when there is none.

  The bit arithmetic below is user_io_status_set()'s, deliberately: the same start%8 shift
  and the same "at most two adjacent bytes" span, and the same o->ex that core_opt_set()
  passes. If it drifted from that pair the file would disagree with the core it is meant to
  describe, which is a bug nothing on screen could show.

  ex matters here twice over. It picks the word the option lives in, so an "o" option
  promoted with ex=0 would write this game's value over a different option's shared bits -
  wrong setting changed, in a file the player cannot inspect, and persistent. Note the
  span check below stays in bytes and so needs no adjustment: ex has already moved start
  and end into the 4..7 byte range by the time they get here, and CO_CFG_BYTES is 16.
*/
int core_opt_promote_to_core(const core_opt *o)
{
	if (!core_opt_can_promote(o)) return 0;

	int start = 0, end = 0;
	int size = user_io_status_bits(o->spec, &start, &end, o->ex);
	if (!size) return 0;

	char *name = user_io_create_config_name(1);
	if (!name || !name[0]) return 0;

	uint8_t img[CO_CFG_MAX];
	memset(img, 0, sizeof(img));

	int len = FileLoadConfig(name, img, sizeof(img));
	if (len < CO_CFG_BYTES) len = CO_CFG_BYTES;
	if (len > CO_CFG_MAX) len = CO_CFG_MAX;
	if (end / 8 >= len) return 0;

	int value = core_opt_value(o);

	uint32_t mask = ~(0xffffffffu << size);
	mask <<= start % 8;
	uint32_t x = ((uint32_t)img[end / 8] << 8) | img[start / 8];
	x = (x & ~mask) | (((uint32_t)value << (start % 8)) & mask);

	img[start / 8] = (uint8_t)x;
	if (end / 8 != start / 8) img[end / 8] = (uint8_t)(x >> 8);

	if (!FileSaveConfig(name, img, len)) return 0;

	printf("ClassicUI: %s = %s is now the shared value, in %s (bits %d..%d only)\n",
		o->name, o->vals[value], name, start, end);

	/*
	  And this game stops overriding it, because the override has just become a copy of
	  the shared value.

	  The other choice is defensible and was rejected on two counts. Keeping the record
	  would store one value in two places with the per-game copy winning, so the row would
	  keep its green star - and that star says "kept for this game, not for the core",
	  which would no longer be true of a value the player has just asked every game to
	  have. Worse, if the shared value were later moved again from the classic OSD, this
	  one game would silently stay behind on the value it had asked to *share*, which is
	  the opposite of what the press meant. Dropping it leaves one value in one place, and
	  the star going is how the screen says the promotion landed.

	  Nothing else this game overrides is touched: only this option's record goes.

	  Not core_opt_drop_for_game(): that puts the record's base - the value we have just
	  replaced - back on the core, so the picture would revert at the moment the player
	  asked to keep it everywhere.
	*/
	co_pg *r = pg_find(pg_bound, o->name);
	if (r) pg_forget(r);

	return 1;
}

// Passes over the record list. More than one because an option can reveal another:
// the N64's whole VI group is masked off until Video Out is Original(VI), so an
// override on Video Out has to be applied before the group is even in the table.
#define CO_PG_PASSES 3

int core_opts_apply_for_game(int sysidx, const char *relpath)
{
	uint32_t k = core_opts_game_key(sysidx, relpath);
	if (!k) return 0;

	pg_load();

	int mine = 0;
	for (int i = 0; i < npgrecs; i++) if (pgrecs[i].key == k) mine++;
	if (!mine) return 0;

	int applied = 0, dirty = 0;

	for (int pass = 0; pass < CO_PG_PASSES; pass++)
	{
		core_opts_scan();

		int moved = 0;
		for (int i = 0; i < npgrecs; i++)
		{
			co_pg *r = &pgrecs[i];
			if (r->key != k) continue;
			if (!pg_allowed(r->name)) continue;

			const core_opt *o = 0;
			for (int j = 0; j < nopts; j++)
			{
				if (!strcasecmp(opts[j].name, r->name)) { o = &opts[j]; break; }
			}
			if (!o) continue;

			int want = pg_val_index(o, r->val);
			if (want < 0) continue;                  // the core dropped that value

			int now = core_opt_value(o);
			if (now == want) continue;

			/*
			  The core has just loaded <CORE>.CFG, so what it holds now is the shared
			  value - refreshed here rather than trusted from whenever the override was
			  made, so that changing the setting for every game in the classic OSD is
			  still undoable per game afterwards.
			*/
			if (now < o->nvals && strcmp(r->base, o->vals[now]))
			{
				snprintf(r->base, sizeof(r->base), "%s", o->vals[now]);
				dirty = 1;
			}

			core_opt_set(o, want);
			applied++;
			moved = 1;
		}

		if (!moved) break;
	}

	if (dirty) pg_save();

	// Leave the table describing the core as it now is, not as it booted.
	core_opts_scan();

	printf("ClassicUI: applied %d of %d per-game core settings\n", applied, mine);
	return applied;
}

/* ------------------------------------------------------------ short name --- */

const char *core_short_name(int sysidx)
{
	if (is_menu()) return "";

	/*
	  The library's badge, already sized for tight places - NES, SNES, GB, GBA, N64, MD,
	  SMS, TG16, PSX. Taken from the system the running game belongs to, which the caller
	  knows, rather than matched against the core name: the two do not reliably agree.
	  A card can carry MegaDrive_*.rbf where the table says Genesis, and the firmware
	  reports "MegaDrive" - the regional naming the launcher already has to resolve.

	  Anything else - an arcade .mra, a core loaded behind our back - falls back to the
	  name the firmware reports, which is always something rather than nothing.
	*/
	const chome_sys *s = (sysidx >= 0) ? lib_sys(sysidx) : 0;
	if (s && s->badge[0]) return s->badge;

	const char *cn = user_io_get_core_name();
	return (cn && *cn) ? cn : "";
}
