#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <math.h>
#include <unistd.h>
#include <sys/stat.h>

#include "chome_video.h"
#include "chome_lib.h"
#include "../../file_io.h"
#include "../../video.h"

#define PREFIX "ClassicHome"
#define PENDING "/tmp/classicui_preset"

/* ------------------------------------------------------------- the table -- */

struct preset_def
{
	const char *id;
	const char *name;
	const char *blurb;
	const char *hfilter;      // 0 = leave alone, "off" = disable
	const char *vfilter;
	const char *sfilter;
	const char *mask;         // 0 = no mask line, "off" = explicitly off
	const char *maskmode;
	const char *gamma;        // 0 = no gamma line, "off" = explicitly off
};

#define F_SHARP   PREFIX " Sharp.txt"
#define F_SOFT    PREFIX " Soft.txt"
#define F_BLURRY  PREFIX " Blurry.txt"
#define F_SCAN    PREFIX " Scanlines.txt"
#define F_SCANLT  PREFIX " Scanlines Light.txt"
#define M_GRILLE  PREFIX " Grille.txt"
#define M_MATRIX  PREFIX " Dot Matrix.txt"
#define G_DMG     PREFIX " DMG.txt"
#define G_POCKET  PREFIX " GB Pocket.txt"
#define G_GBC     PREFIX " GBC.txt"
#define G_AGB001  PREFIX " GBA AGB-001.txt"
#define G_AGS001  PREFIX " GBA AGS-001.txt"
#define G_AGS101  PREFIX " GBA AGS-101.txt"
#define G_GG      PREFIX " Game Gear.txt"
#define G_GGMOD   PREFIX " Game Gear Backlit.txt"
#define G_LYNX    PREFIX " Lynx.txt"
#define G_WS      PREFIX " WonderSwan.txt"
#define G_WSC     PREFIX " WonderSwan Color.txt"
#define G_NGPC    PREFIX " Neo Geo Pocket Color.txt"

static const preset_def presets[] =
{
	{ "sharp", "Sharp", "No filtering. Square pixels, nothing added.",
	  "off", "off", "off", "off", "off", "off" },

	{ "pvm-rgb", "PVM RGB", "Sharp RGB monitor with fine scanlines and an aperture grille.",
	  F_SHARP, F_SHARP, F_SCAN, M_GRILLE, "1x", "off" },

	{ "pvm-svideo", "PVM S-Video", "Slight horizontal bleed, scanlines. Consoles on a good TV.",
	  F_SOFT, F_SHARP, F_SCAN, M_GRILLE, "1x", "off" },

	{ "composite", "Composite TV", "Soft and blurry, as an RF or composite hookup looked.",
	  F_BLURRY, F_SOFT, F_SCAN, M_GRILLE, "2x", "off" },

	{ "pal-tv", "PAL TV", "Softer still with lighter scanlines. Home computers on a telly.",
	  F_SOFT, F_SOFT, F_SCANLT, M_GRILLE, "2x", "off" },

	{ "vga", "VGA Monitor", "Clean and slightly smoothed. No scanlines: a 31 kHz monitor had none.",
	  F_SOFT, F_SOFT, "off", "off", "off", "off" },

	{ "dmg", "Game Boy DMG", "The original olive-green reflective LCD, with its pixel grid.",
	  F_SHARP, F_SHARP, "off", M_MATRIX, "2x", G_DMG },

	{ "pocket", "Game Boy Pocket", "Neutral grey reflective LCD, better contrast, finer grid.",
	  F_SHARP, F_SHARP, "off", M_MATRIX, "1x", G_POCKET },

	{ "gbc", "Game Boy Color", "Reflective colour LCD: darkish and a little muted.",
	  F_SHARP, F_SHARP, "off", M_MATRIX, "1x", G_GBC },

	{ "agb001", "GBA (AGB-001)", "The original unlit screen. Dim and washed out.",
	  F_SHARP, F_SHARP, "off", M_MATRIX, "1x", G_AGB001 },

	{ "ags001", "GBA SP (AGS-001)", "Frontlit SP: brighter than AGB, still washed out.",
	  F_SHARP, F_SHARP, "off", M_MATRIX, "1x", G_AGS001 },

	{ "ags101", "GBA SP (AGS-101)", "Backlit SP: bright with proper contrast and colour.",
	  F_SHARP, F_SHARP, "off", "off", "off", G_AGS101 },

	{ "gg", "Game Gear", "Backlit but murky, with the Game Gear's poor contrast.",
	  F_SHARP, F_SHARP, "off", M_MATRIX, "1x", G_GG },

	{ "gg-mod", "Game Gear (Backlit Mod)", "The common LED backlight mod: brighter, cleaner whites.",
	  F_SHARP, F_SHARP, "off", M_MATRIX, "1x", G_GGMOD },

	{ "lynx", "Atari Lynx", "Backlit colour LCD with a cool cast and washed blacks.",
	  F_SHARP, F_SHARP, "off", M_MATRIX, "1x", G_LYNX },

	{ "ws", "WonderSwan", "Reflective mono FSTN: warm grey, low contrast.",
	  F_SHARP, F_SHARP, "off", M_MATRIX, "1x", G_WS },

	{ "wsc", "WonderSwan Color", "Reflective colour panel: muted and slightly warm.",
	  F_SHARP, F_SHARP, "off", M_MATRIX, "1x", G_WSC },

	{ "ngpc", "Neo Geo Pocket Color", "Reflective pastel colour LCD, gentle contrast.",
	  F_SHARP, F_SHARP, "off", M_MATRIX, "1x", G_NGPC },
};

#define NPRESETS ((int)(sizeof(presets) / sizeof(presets[0])))

int vp_count() { return NPRESETS; }
const char *vp_name(int i) { return (i >= 0 && i < NPRESETS) ? presets[i].name : "?"; }
const char *vp_blurb(int i) { return (i >= 0 && i < NPRESETS) ? presets[i].blurb : ""; }

static const char *class_names[VC_COUNT] =
{
	"console", "arcade", "computer", "vga", "gb", "gbc", "gba",
	"gg", "lynx", "ws", "wsc", "ngpc"
};

const char *vp_class_name(int c) { return (c >= 0 && c < VC_COUNT) ? class_names[c] : "console"; }

static const char *class_labels[VC_COUNT] =
{
	"Consoles", "Arcade", "Home Computers", "VGA Monitor",
	"Game Boy", "Game Boy Color", "Game Boy Advance",
	"Game Gear", "Atari Lynx", "WonderSwan", "WonderSwan Color",
	"Neo Geo Pocket Color"
};

int vp_class_is_handheld(int c)
{
	return (c == VC_GB || c == VC_GBC || c == VC_GBA || c == VC_GG ||
		c == VC_LYNX || c == VC_WS || c == VC_WSC || c == VC_NGPC) ? 1 : 0;
}

const char *vp_class_label(int c) { return (c >= 0 && c < VC_COUNT) ? class_labels[c] : "Consoles"; }

int vp_class_from_name(const char *s)
{
	if (!s || !*s) return VC_CONSOLE;
	for (int i = 0; i < VC_COUNT; i++) if (!strcasecmp(s, class_names[i])) return i;
	return VC_CONSOLE;
}

// Preset indices, in table order.
enum
{
	P_SHARP = 0, P_PVM_RGB, P_PVM_SVIDEO, P_COMPOSITE, P_PAL_TV, P_VGA,
	P_DMG, P_POCKET, P_GBC, P_AGB001, P_AGS001, P_AGS101,
	P_GG, P_GGMOD, P_LYNX, P_WS, P_WSC, P_NGPC
};

/*
  What each class is allowed to choose from, most authentic first (so entry 0 is
  the default). The reasoning:

  - Consoles reached a TV by RGB, S-Video or composite depending on your luck, so
    all three plus an unfiltered escape hatch.
  - Arcade monitors were direct RGB; composite never entered the picture.
  - 15 kHz home computers lived on televisions, not monitors.
  - 31 kHz VGA had no visible scanline gaps at all, so no CRT looks belong here.
  - A DMG cartridge in this UI means the original hardware or a Pocket: both grey
    reflective panels, no colour, no backlight.
  - A GBC cartridge runs on a Game Boy Color or on either Game Boy Advance, so it
    gets the GBC panel plus all three GBA screen revisions.
  - A GBA cartridge runs on the three GBA screens and nothing else.

  Handhelds deliberately get no "Sharp" option: an unfiltered Game Boy is not a
  look anyone is after, and the LCD is the point.
*/
static const int opt_console[]  = { P_PVM_RGB, P_PVM_SVIDEO, P_COMPOSITE, P_SHARP };
static const int opt_arcade[]   = { P_PVM_RGB, P_PVM_SVIDEO, P_SHARP };
static const int opt_computer[] = { P_PAL_TV, P_COMPOSITE, P_PVM_SVIDEO, P_SHARP };
static const int opt_vga[]      = { P_VGA, P_SHARP };
static const int opt_gb[]       = { P_DMG, P_POCKET };
static const int opt_gbc[]      = { P_GBC, P_AGB001, P_AGS001, P_AGS101 };
static const int opt_gba[]      = { P_AGB001, P_AGS001, P_AGS101 };
static const int opt_gg[]       = { P_GG, P_GGMOD };
static const int opt_lynx[]     = { P_LYNX };
static const int opt_ws[]       = { P_WS };
static const int opt_wsc[]      = { P_WSC, P_WS };
static const int opt_ngpc[]     = { P_NGPC };

struct opt_set { const int *list; int n; };

static opt_set options_of(int vclass)
{
	opt_set r;
	switch (vclass)
	{
	case VC_ARCADE:   r.list = opt_arcade;   r.n = (int)(sizeof(opt_arcade) / sizeof(int)); break;
	case VC_COMPUTER: r.list = opt_computer; r.n = (int)(sizeof(opt_computer) / sizeof(int)); break;
	case VC_VGA:      r.list = opt_vga;      r.n = (int)(sizeof(opt_vga) / sizeof(int)); break;
	case VC_GB:       r.list = opt_gb;       r.n = (int)(sizeof(opt_gb) / sizeof(int)); break;
	case VC_GBC:      r.list = opt_gbc;      r.n = (int)(sizeof(opt_gbc) / sizeof(int)); break;
	case VC_GBA:      r.list = opt_gba;      r.n = (int)(sizeof(opt_gba) / sizeof(int)); break;
	case VC_GG:       r.list = opt_gg;       r.n = (int)(sizeof(opt_gg) / sizeof(int)); break;
	case VC_LYNX:     r.list = opt_lynx;     r.n = (int)(sizeof(opt_lynx) / sizeof(int)); break;
	case VC_WS:       r.list = opt_ws;       r.n = (int)(sizeof(opt_ws) / sizeof(int)); break;
	case VC_WSC:      r.list = opt_wsc;      r.n = (int)(sizeof(opt_wsc) / sizeof(int)); break;
	case VC_NGPC:     r.list = opt_ngpc;     r.n = (int)(sizeof(opt_ngpc) / sizeof(int)); break;
	default:          r.list = opt_console;  r.n = (int)(sizeof(opt_console) / sizeof(int)); break;
	}
	return r;
}

int vp_options_for(int vclass, int *out)
{
	opt_set o = options_of(vclass);
	int n = (o.n > VP_MAX_OPTIONS) ? VP_MAX_OPTIONS : o.n;
	if (out) for (int i = 0; i < n; i++) out[i] = o.list[i];
	return n;
}

int vp_default_for(int vclass)
{
	opt_set o = options_of(vclass);
	return o.n ? o.list[0] : P_PVM_RGB;
}

static int allowed_in(int vclass, int preset)
{
	opt_set o = options_of(vclass);
	for (int i = 0; i < o.n; i++) if (o.list[i] == preset) return 1;
	return 0;
}

/* ------------------------------------------------------------ generation -- */

static int exists_rel(const char *rel)
{
	struct stat st;
	char p[1024];
	snprintf(p, sizeof(p), "%s/%s", getRootDir(), rel);
	return (!stat(p, &st) && S_ISREG(st.st_mode)) ? 1 : 0;
}

static void ensure_dir(const char *rel)
{
	char p[1024];
	snprintf(p, sizeof(p), "%s/%s", getRootDir(), rel);
	mkdir(p, 0777);
}

// Opens a file for writing only if it does not exist yet: never clobber the
// user's own filters or presets.
static FILE *open_new(const char *dir, const char *name)
{
	char rel[1024];
	snprintf(rel, sizeof(rel), "%s/%s", dir, name);
	if (exists_rel(rel)) return 0;

	char p[1200];
	snprintf(p, sizeof(p), "%s/%s", getRootDir(), rel);
	FILE *f = fopen(p, "wt");
	if (f) printf("ClassicUI: wrote %s\n", rel);
	else printf("ClassicUI: could not write %s\n", rel);
	return f;
}

#define PHASES 32

/*
  4-tap polyphase coefficients, range -128..128, each line summing to at most
  128 (read_video_filter in video.cpp). Lines summing to less than 128 come out
  darker, which is exactly how a scanline filter works.
*/
static void write_filter(const char *name, int kind, double scan_depth)
{
	FILE *f = open_new("filters", name);
	if (!f) return;

	fprintf(f, "# generated by Classic Home\n");
	fprintf(f, "# range -128..128, 4 taps, %d phases\n\n", PHASES);

	for (int p = 0; p < PHASES; p++)
	{
		double x = (double)p / PHASES;      // position within the source pixel
		double w[4] = { 0, 0, 0, 0 };

		if (kind == 0)
		{
			// Nearest: hard switch at the halfway point.
			if (x < 0.5) w[1] = 1.0; else w[2] = 1.0;
		}
		else if (kind == 1)
		{
			// Linear.
			w[1] = 1.0 - x;
			w[2] = x;
		}
		else
		{
			// Wide tent over all four taps: the composite-ish smear.
			double d[4] = { -1.0 - x, -x, 1.0 - x, 2.0 - x };
			double sum = 0;
			for (int t = 0; t < 4; t++)
			{
				double a = fabs(d[t]) / 1.9;
				w[t] = (a >= 1.0) ? 0.0 : (1.0 - a);
				sum += w[t];
			}
			if (sum > 0) for (int t = 0; t < 4; t++) w[t] /= sum;
		}

		double gain = 1.0;
		if (scan_depth > 0)
		{
			// Bright in the middle of a source line, dark at its edges.
			double s = sin(M_PI * (x + 0.5 / PHASES));
			if (s < 0) s = 0;
			gain = (1.0 - scan_depth) + scan_depth * pow(s, 1.4);
		}

		int c[4];
		int total = 0;
		for (int t = 0; t < 4; t++)
		{
			c[t] = (int)lround(w[t] * gain * 128.0);
			if (c[t] > 128) c[t] = 128;
			if (c[t] < -128) c[t] = -128;
			total += c[t];
		}

		// Must not exceed the range: trim the biggest tap if rounding pushed over.
		while (total > 128)
		{
			int big = 0;
			for (int t = 1; t < 4; t++) if (c[t] > c[big]) big = t;
			c[big]--;
			total--;
		}

		fprintf(f, "%4d,%4d,%4d,%4d\n", c[0], c[1], c[2], c[3]);
	}

	fclose(f);
}

/*
  Shadow masks: "w,h" then h rows of w hex values. In the non-v2 form only the
  low three bits are used, one per colour channel (setShadowMask in video.cpp),
  so 7 passes everything and 0 is black.
*/
static void write_mask(const char *name, int kind)
{
	FILE *f = open_new("shadow_masks", name);
	if (!f) return;

	fprintf(f, "# generated by Classic Home\n");

	if (kind == 0)
	{
		// Aperture grille: R, G, B stripes.
		fprintf(f, "3,1\n");
		fprintf(f, "1,2,4\n");
	}
	else
	{
		// LCD pixel grid: a black gutter on two sides of each cell.
		fprintf(f, "4,4\n");
		fprintf(f, "7,7,7,0\n");
		fprintf(f, "7,7,7,0\n");
		fprintf(f, "7,7,7,0\n");
		fprintf(f, "0,0,0,0\n");
	}

	fclose(f);
}

// Gamma curves are 256 lines of "r,g,b" (setGamma in video.cpp), i.e. a full
// per-channel LUT - which is what makes the handheld tints possible at all.
struct lut_spec
{
	double lo[3], hi[3];      // output at input 0 and 255, per channel
	double gamma;
};

static void write_gamma(const char *name, const lut_spec *s)
{
	FILE *f = open_new("gamma", name);
	if (!f) return;

	fprintf(f, "# generated by Classic Home\n");
	for (int i = 0; i < 256; i++)
	{
		double t = pow(i / 255.0, s->gamma);
		int v[3];
		for (int c = 0; c < 3; c++)
		{
			double o = s->lo[c] + (s->hi[c] - s->lo[c]) * t;
			if (o < 0) o = 0;
			if (o > 255) o = 255;
			v[c] = (int)lround(o);
		}
		fprintf(f, "%d,%d,%d\n", v[0], v[1], v[2]);
	}
	fclose(f);
}

/*
  Handheld panels. These are per-channel curves: MiSTer's gamma format is three
  independent LUTs, so brightness, contrast and tint are all reachable, but a true
  colour-space correction matrix (the usual "GBA colour correction" approach, which
  mixes channels) is not expressible here.
*/
// DMG: deep olive shadow to pale yellow-green highlight, poor contrast.
static const lut_spec lut_dmg    = { { 15, 56, 15 },  { 155, 188, 15 },  1.00 };
// Game Boy Pocket: neutral grey with a faint cool cast, much better contrast.
static const lut_spec lut_pocket = { { 20, 22, 24 },  { 214, 219, 214 }, 1.00 };
// GBC: reflective colour panel, lifted blacks, slightly muted and warm.
static const lut_spec lut_gbc    = { { 34, 32, 28 },  { 226, 220, 205 }, 0.94 };
// AGB-001: no light at all. Very low contrast, dim, faintly green.
static const lut_spec lut_agb001 = { { 62, 66, 58 },  { 188, 190, 176 }, 0.86 };
// AGS-001: frontlight lifts it and warms it, still washed out.
static const lut_spec lut_ags001 = { { 48, 47, 42 },  { 214, 209, 196 }, 0.90 };
// AGS-101: backlit. Bright, near-neutral, proper black level - the good one.
static const lut_spec lut_ags101 = { { 14, 14, 17 },  { 247, 247, 250 }, 1.00 };
// Game Gear: backlit, but a murky panel with badly lifted blacks.
static const lut_spec lut_gg     = { { 46, 48, 52 },  { 206, 204, 196 }, 0.90 };
// Game Gear with the usual LED backlight mod: brighter, cleaner whites.
static const lut_spec lut_ggmod  = { { 26, 27, 30 },  { 240, 238, 234 }, 0.97 };
// Lynx: backlit colour, cool cast, washed blacks.
static const lut_spec lut_lynx   = { { 44, 48, 56 },  { 214, 216, 224 }, 0.90 };
// WonderSwan mono FSTN: warm grey, narrow range.
static const lut_spec lut_ws     = { { 40, 38, 32 },  { 208, 202, 186 }, 0.95 };
// WonderSwan Color: reflective colour, muted and warm.
static const lut_spec lut_wsc    = { { 34, 32, 28 },  { 222, 214, 198 }, 0.94 };
// Neo Geo Pocket Color: reflective pastel, gentle contrast.
static const lut_spec lut_ngpc   = { { 38, 38, 36 },  { 226, 224, 214 }, 0.96 };

static void write_preset(const preset_def *d)
{
	char name[256];
	snprintf(name, sizeof(name), "%s %s.ini", PREFIX, d->name);

	FILE *f = open_new("presets", name);
	if (!f) return;

	fprintf(f, "# %s - %s\n", d->name, d->blurb);
	fprintf(f, "# generated by Classic Home\n");
	if (d->hfilter)  fprintf(f, "hfilter=%s\n", d->hfilter);
	if (d->vfilter)  fprintf(f, "vfilter=%s\n", d->vfilter);
	if (d->sfilter)  fprintf(f, "sfilter=%s\n", d->sfilter);
	if (d->mask)     fprintf(f, "mask=%s\n", d->mask);
	if (d->maskmode) fprintf(f, "maskmode=%s\n", d->maskmode);
	if (d->gamma)    fprintf(f, "gamma=%s\n", d->gamma);
	fclose(f);
}

void vp_install()
{
	ensure_dir("filters");
	ensure_dir("shadow_masks");
	ensure_dir("gamma");
	ensure_dir("presets");

	write_filter(F_SHARP, 0, 0.0);
	write_filter(F_SOFT, 1, 0.0);
	write_filter(F_BLURRY, 2, 0.0);
	// Scanline depth is the single most subjective number here, and combined with
	// a full aperture grille it is easy to end up too dark. These are deliberately
	// moderate; tune them on real hardware and delete the files to regenerate.
	write_filter(F_SCAN, 1, 0.28);
	write_filter(F_SCANLT, 1, 0.15);

	write_mask(M_GRILLE, 0);
	write_mask(M_MATRIX, 1);

	write_gamma(G_DMG, &lut_dmg);
	write_gamma(G_POCKET, &lut_pocket);
	write_gamma(G_GBC, &lut_gbc);
	write_gamma(G_AGB001, &lut_agb001);
	write_gamma(G_AGS001, &lut_ags001);
	write_gamma(G_AGS101, &lut_ags101);
	write_gamma(G_GG, &lut_gg);
	write_gamma(G_GGMOD, &lut_ggmod);
	write_gamma(G_LYNX, &lut_lynx);
	write_gamma(G_WS, &lut_ws);
	write_gamma(G_WSC, &lut_wsc);
	write_gamma(G_NGPC, &lut_ngpc);

	for (int i = 0; i < NPRESETS; i++) write_preset(&presets[i]);
}

int vp_available(int i)
{
	if (i < 0 || i >= NPRESETS) return 0;
	const preset_def *d = &presets[i];

	char rel[1024];
	snprintf(rel, sizeof(rel), "presets/%s %s.ini", PREFIX, d->name);
	if (!exists_rel(rel)) return 0;

	const char *files[3] = { d->hfilter, d->sfilter, 0 };
	for (int k = 0; k < 2; k++)
	{
		if (!files[k] || !strcasecmp(files[k], "off")) continue;
		snprintf(rel, sizeof(rel), "filters/%s", files[k]);
		if (!exists_rel(rel)) return 0;
	}

	if (d->mask && strcasecmp(d->mask, "off"))
	{
		snprintf(rel, sizeof(rel), "shadow_masks/%s", d->mask);
		if (!exists_rel(rel)) return 0;
	}

	if (d->gamma && strcasecmp(d->gamma, "off"))
	{
		snprintf(rel, sizeof(rel), "gamma/%s", d->gamma);
		if (!exists_rel(rel)) return 0;
	}

	return 1;
}

/* ------------------------------------------------------- per system choice */

#define VP_MAX 64

struct vp_rec { uint32_t key; uint8_t preset; uint8_t pad[3]; };

static vp_rec vprecs[VP_MAX];
static int nvprecs = 0;
static int vp_loaded = 0;

// Keyed by system and class together: a DMG cart and a GBC cart share a core but
// must not share a remembered screen.
static uint32_t sys_key(int sysidx, int vclass)
{
	const chome_sys *s = lib_sys(sysidx);
	if (!s) return 0;

	uint32_t h = 2166136261u;
	for (const char *p = s->id; *p; p++) { h ^= (unsigned char)*p; h *= 16777619u; }
	h ^= (uint32_t)(vclass + 1);
	h *= 16777619u;
	return h ? h : 1;
}

static void vp_load()
{
	if (vp_loaded) return;
	vp_loaded = 1;

	memset(vprecs, 0, sizeof(vprecs));
	int len = FileLoadConfig("classicui_video.cfg", vprecs, sizeof(vprecs));
	nvprecs = (len > 0) ? len / (int)sizeof(vp_rec) : 0;
	if (nvprecs > VP_MAX) nvprecs = VP_MAX;
}

static int resolve_class(int sysidx, int vclass)
{
	if (vclass >= 0 && vclass < VC_COUNT) return vclass;
	const chome_sys *s = lib_sys(sysidx);
	return s ? s->vclass : VC_CONSOLE;
}

int vp_effective(int sysidx, int vclass)
{
	vp_load();
	vclass = resolve_class(sysidx, vclass);

	uint32_t k = sys_key(sysidx, vclass);
	for (int i = 0; i < nvprecs; i++)
	{
		if (vprecs[i].key != k) continue;
		int p = vprecs[i].preset;
		// A stored choice that is no longer offered for this class is ignored.
		if (p < NPRESETS && allowed_in(vclass, p)) return p;
		break;
	}

	return vp_default_for(vclass);
}

void vp_set(int sysidx, int vclass, int preset)
{
	if (preset < 0 || preset >= NPRESETS) return;
	vp_load();

	vclass = resolve_class(sysidx, vclass);
	if (!allowed_in(vclass, preset)) return;

	uint32_t k = sys_key(sysidx, vclass);
	if (!k) return;

	for (int i = 0; i < nvprecs; i++)
	{
		if (vprecs[i].key != k) continue;
		vprecs[i].preset = (uint8_t)preset;
		FileSaveConfig("classicui_video.cfg", vprecs, nvprecs * (int)sizeof(vp_rec));
		return;
	}

	if (nvprecs >= VP_MAX) return;
	vprecs[nvprecs].key = k;
	vprecs[nvprecs].preset = (uint8_t)preset;
	nvprecs++;
	FileSaveConfig("classicui_video.cfg", vprecs, nvprecs * (int)sizeof(vp_rec));
}

/* ------------------------------------------------------------ applying ---- */

int vp_preset_path(int i, char *out, int len)
{
	if (i < 0 || i >= NPRESETS) return 0;
	snprintf(out, len, "%s/presets/%s %s.ini", getRootDir(), PREFIX, presets[i].name);
	return 1;
}

void vp_arm_for_launch(int sysidx, int vclass_hint)
{
	int p = vp_effective(sysidx, vclass_hint);
	if (p < 0 || p >= NPRESETS) return;

	FILE *f = fopen(PENDING, "wt");
	if (!f) return;

	// video_loadPreset() takes a path it can open directly.
	fprintf(f, "%s/presets/%s %s.ini\n", getRootDir(), PREFIX, presets[p].name);
	fclose(f);

	printf("ClassicUI: armed video look \"%s\" for the next core\n", presets[p].name);
}

/*
  The same look, applied to the core that is already running.

  vp_arm_for_launch() leaves the choice for the next core to pick up. That is right for
  a game about to start and wrong for the one on screen: the player chose a look in
  order to see it, and having to reload the core first reads as the setting not working.

  Safe to do while the menu is up. A preset carries only the scaler's filters, mask and
  gamma - no mode, no timing - so nothing here disturbs the framebuffer this is drawn
  into, which is the thing that has broken before when video state moved underneath it.
*/
int vp_apply_now(int sysidx, int vclass_hint)
{
	int i = vp_effective(sysidx, vclass_hint);
	if (i < 0 || i >= NPRESETS) return 0;

	char path[1024];
	if (!vp_preset_path(i, path, sizeof(path))) return 0;

	printf("ClassicUI: applying video look \"%s\" to the running core\n", presets[i].name);
	video_loadPreset(path, true);
	return 1;
}

void vp_apply_pending()
{
	FILE *f = fopen(PENDING, "rt");
	if (!f) return;

	char path[1024] = {};
	if (fgets(path, sizeof(path), f))
	{
		char *nl = strchr(path, '\n');
		if (nl) *nl = 0;
	}
	fclose(f);
	unlink(PENDING);

	if (!path[0]) return;

	printf("ClassicUI: applying video look %s\n", path);
	video_loadPreset(path, true);
}

/* ------------------------------------------------------------- previews --- */

static uint32_t *pv_buf = 0;
static int pv_w = 0, pv_h = 0, pv_idx = -1;
static const uint32_t *pv_ref = 0;

static void lut_of(int i, uint8_t lut[3][256])
{
	const lut_spec *s = 0;
	if (presets[i].gamma)
	{
		if (!strcmp(presets[i].gamma, G_DMG)) s = &lut_dmg;
		else if (!strcmp(presets[i].gamma, G_POCKET)) s = &lut_pocket;
		else if (!strcmp(presets[i].gamma, G_GBC)) s = &lut_gbc;
		else if (!strcmp(presets[i].gamma, G_AGB001)) s = &lut_agb001;
		else if (!strcmp(presets[i].gamma, G_AGS001)) s = &lut_ags001;
		else if (!strcmp(presets[i].gamma, G_AGS101)) s = &lut_ags101;
		else if (!strcmp(presets[i].gamma, G_GG)) s = &lut_gg;
		else if (!strcmp(presets[i].gamma, G_GGMOD)) s = &lut_ggmod;
		else if (!strcmp(presets[i].gamma, G_LYNX)) s = &lut_lynx;
		else if (!strcmp(presets[i].gamma, G_WS)) s = &lut_ws;
		else if (!strcmp(presets[i].gamma, G_WSC)) s = &lut_wsc;
		else if (!strcmp(presets[i].gamma, G_NGPC)) s = &lut_ngpc;
	}

	for (int v = 0; v < 256; v++)
	{
		if (!s) { lut[0][v] = lut[1][v] = lut[2][v] = (uint8_t)v; continue; }
		double t = pow(v / 255.0, s->gamma);
		for (int c = 0; c < 3; c++)
		{
			double o = s->lo[c] + (s->hi[c] - s->lo[c]) * t;
			if (o < 0) o = 0;
			if (o > 255) o = 255;
			lut[c][v] = (uint8_t)lround(o);
		}
	}
}

/*
  Reference image: colour bars, a grey ramp and a chunky sprite shape, so both
  colour handling and pixel edges are visible. Then the preset's signature is
  applied in software. This illustrates the look; it is not a capture.
*/
static void ref_pixel(int x, int y, int w, int h, uint8_t out[3])
{
	static const uint8_t bars[8][3] =
	{
		{ 255,255,255 }, { 255,255,0 }, { 0,255,255 }, { 0,255,0 },
		{ 255,0,255 }, { 255,0,0 }, { 0,0,255 }, { 24,24,24 }
	};

	int band = h / 3;

	if (y < band)
	{
		int b = (x * 8) / (w ? w : 1);
		if (b > 7) b = 7;
		out[0] = bars[b][0]; out[1] = bars[b][1]; out[2] = bars[b][2];
		return;
	}

	if (y < band * 2)
	{
		// Grey ramp in eight steps, to show black lift and contrast.
		int step = (x * 8) / (w ? w : 1);
		if (step > 7) step = 7;
		uint8_t v = (uint8_t)(step * 36);
		out[0] = out[1] = out[2] = v;
		return;
	}

	// A blocky sprite on a mid background: shows edges and the mask.
	out[0] = 40; out[1] = 44; out[2] = 60;

	int cell = (h / 12) ? (h / 12) : 1;
	int sx = (x - w / 2) / cell;
	int sy = (y - (band * 2 + (h - band * 2) / 2)) / cell;

	if (sy >= -2 && sy <= 2 && sx >= -3 && sx <= 3)
	{
		int body = (sy * sy + sx * sx) <= 8;
		if (body)
		{
			out[0] = 230; out[1] = 90; out[2] = 60;
			if (sy == -1 && (sx == -1 || sx == 1)) { out[0] = 20; out[1] = 20; out[2] = 20; }
		}
	}
}

// Reads the source frame: the real capture when present, else the test pattern.
static void src_pixel(const uint32_t *ref, int x, int y, int w, int h, uint8_t out[3])
{
	if (!ref)
	{
		ref_pixel(x, y, w, h, out);
		return;
	}

	if (x < 0) x = 0;
	if (y < 0) y = 0;
	if (x >= w) x = w - 1;
	if (y >= h) y = h - 1;

	uint32_t c = ref[(size_t)y * w + x];
	out[0] = (uint8_t)((c >> 16) & 0xff);
	out[1] = (uint8_t)((c >> 8) & 0xff);
	out[2] = (uint8_t)(c & 0xff);
}

const uint32_t *vp_preview(int i, int w, int h, const uint32_t *ref)
{
	if (i < 0 || i >= NPRESETS || w < 8 || h < 8) return 0;
	if (pv_buf && pv_idx == i && pv_w == w && pv_h == h && pv_ref == ref) return pv_buf;

	if (!pv_buf || pv_w != w || pv_h != h)
	{
		free(pv_buf);
		pv_buf = (uint32_t*)malloc((size_t)w * h * 4);
		pv_w = w;
		pv_h = h;
	}
	if (!pv_buf) return 0;
	pv_idx = i;
	pv_ref = ref;

	const preset_def *d = &presets[i];

	uint8_t lut[3][256];
	lut_of(i, lut);

	int soft = (d->hfilter && !strcasecmp(d->hfilter, F_SOFT)) ? 1 : 0;
	int blur = (d->hfilter && !strcasecmp(d->hfilter, F_BLURRY)) ? 1 : 0;
	double scan = 0;
	if (d->sfilter && !strcasecmp(d->sfilter, F_SCAN)) scan = 0.28;
	if (d->sfilter && !strcasecmp(d->sfilter, F_SCANLT)) scan = 0.15;

	int grille = (d->mask && !strcasecmp(d->mask, M_GRILLE)) ? 1 : 0;
	int matrix = (d->mask && !strcasecmp(d->mask, M_MATRIX)) ? 1 : 0;

	/*
	  With a reference frame the source is that frame at 1:1 (it was decoded at
	  exactly w x h). Without one, the synthetic pattern is drawn at a chunky scale
	  so pixel edges and the mask stay visible.
	*/
	int scale = (h / 60) + 2;
	int sw = w / scale, sh = h / scale;
	if (sw < 8) sw = 8;
	if (sh < 8) sh = 8;

	if (ref) { sw = w; sh = h; scale = 1; }

	for (int y = 0; y < h; y++)
	{
		int syi = (y * sh) / h;

		for (int x = 0; x < w; x++)
		{
			int sxi = (x * sw) / w;

			uint8_t c[3];
			src_pixel(ref, sxi, syi, sw, sh, c);

			// Horizontal bleed: mix with the neighbours either side.
			if (soft || blur)
			{
				uint8_t l[3];
				src_pixel(ref, sxi > 0 ? sxi - 1 : 0, syi, sw, sh, l);
				uint8_t r[3];
				src_pixel(ref, sxi + 1 < sw ? sxi + 1 : sw - 1, syi, sw, sh, r);
				int wgt = blur ? 34 : 16;                  // percent to each side
				for (int k = 0; k < 3; k++)
					c[k] = (uint8_t)((c[k] * (100 - 2 * wgt) + l[k] * wgt + r[k] * wgt) / 100);
			}

			for (int k = 0; k < 3; k++) c[k] = lut[k][c[k]];

			if (scan > 0)
			{
				// Position within the source line drives the scanline envelope.
				double f = ((double)(y * sh) / h) - syi;
				double s = sin(M_PI * f);
				if (s < 0) s = 0;
				double gain = (1.0 - scan) + scan * pow(s, 1.4);
				for (int k = 0; k < 3; k++) c[k] = (uint8_t)(c[k] * gain);
			}

			if (grille)
			{
				int ph = x % 3;
				for (int k = 0; k < 3; k++) if (k != ph) c[k] = (uint8_t)(c[k] / 3);
			}
			else if (matrix)
			{
				/*
				  LCD cell grid. The period is a small constant in preview pixels,
				  deliberately not tied to the source scale: tying it there made the
				  cells up to 36px across, which read as giant blocks rather than as
				  a panel. A GBA pixel is only about one preview pixel at this size,
				  so some exaggeration is needed to be visible at all - but only a
				  little, and the gutter is a gentle shade rather than near-black.
				*/
				const int period = 3;
				if ((x % period) == period - 1 || (y % period) == period - 1)
				{
					for (int k = 0; k < 3; k++) c[k] = (uint8_t)((c[k] * 3) / 4);
				}
			}

			pv_buf[y * w + x] = 0xff000000u | (c[0] << 16) | (c[1] << 8) | c[2];
		}
	}

	return pv_buf;
}
