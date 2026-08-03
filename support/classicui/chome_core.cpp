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
	"State Ld/Sv", "Rewind Capture", "SNAC", "USERIO", "SNAC MemCard", "SNAC Compare",
	"Pad1", "Pad2", "Pad 1 Type", "Pad 2 Type", "Pad 3 Type", "Pad 4 Type",
	"Automount Memory Card 1", "Storage", "SPU RAM select",
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
  The overclock and timing family. Derek asked for it and it goes in, but on its own page:
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

	for (int i = 1; i < 64 && nopts < CO_MAX; i++)
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

int core_opt_value(const core_opt *o)
{
	if (!o) return 0;
	uint32_t v = user_io_status_get(o->spec);
	return (v < o->nvals) ? (int)v : 0;
}

void core_opt_set(const core_opt *o, int value)
{
	if (!o || o->nvals < 2) return;
	if (value < 0) value = o->nvals - 1;
	if (value >= o->nvals) value = 0;

	user_io_status_set(o->spec, (uint32_t)value);
	printf("ClassicUI: core option %s = %s\n", o->name, o->vals[value]);
}

void core_opts_save()
{
	char *name = user_io_create_config_name(1);
	if (!name || !name[0]) return;

	user_io_status_save(name);
	printf("ClassicUI: wrote the core's config (%s)\n", name);
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
