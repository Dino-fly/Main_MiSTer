#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <math.h>
#include <unistd.h>
#include <sys/stat.h>

#include "chome_video.h"
#include "chome_lib.h"
#include "chome_core.h"
#include "../../cfg.h"
#include "../../file_io.h"
#include "../../user_io.h"
#include "../../video.h"

#define PREFIX "ClassicHome"
#define PENDING "/tmp/classicui_preset"

/* ------------------------------------------------------------- the table -- */

/*
  A look drives two machines at once.

  The scaler half (filters, mask, gamma) goes through a preset file exactly as
  before. The core half is new: core_opts is a ;-separated list of
  "Option Name=Value Name" pairs matched against the running core's CONF_STR by
  NAME - a core that does not publish the option is simply left alone, which is
  what makes one "None" entry safe on every system. Names, not bit positions,
  for the same reason snacpad.cpp does it: a core update that inserts an option
  renumbers every bit after it.

  Nothing the core half does is ever saved into <CORE>.CFG. The look is applied
  on every launch from the shelf (and live from the Display screen), so a game
  started from the stock menu still gets the player's own core config untouched.

  Why the core owns the colour now: the Game Boy core colourises DMG games BY
  DEFAULT (Custom Palette=Auto, falling back to the olive/teal
  828214/517356/305A5F/1A3B49 baked into its RTL). Our old gamma LUT tinted that
  already-tinted picture green - the double-processed mess this replaces. The
  GBA core carries the Pokefan531 colour correction ("Modify Colors") in its own
  pixel pipeline, per hardware model, before scaling - strictly better than a
  per-channel LUT, which cannot mix channels at all. So for GB and GBA the
  colour belongs to the core and the scaler gamma stays off; the scaler keeps
  only what the core cannot do - the pixel grid.

  palette is a root-relative .gbp path sent to the Game Boy core's palette slot
  (FC3). It is only sent when the core publishes "Custom Palette", so it cannot
  land in a random index of some other core.
*/
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
	const char *core_opts;    // 0 = nothing, else "Name=Value;Name=Value"
	const char *palette;      // 0 = none, else root-relative .gbp path
};

#define F_SHARP   PREFIX " Sharp.txt"
#define F_SOFT    PREFIX " Soft.txt"
#define F_BLURRY  PREFIX " Blurry.txt"
#define F_SCAN    PREFIX " Scanlines.txt"
#define F_SCANLT  PREFIX " Scanlines Light.txt"
#define F_SCANDP  PREFIX " Scanlines Deep.txt"
#define F_GRID    PREFIX " LCD Grid.txt"
#define F_GRIDSH  PREFIX " LCD Grid Shadow.txt"
#define M_GRILLE  PREFIX " Grille.txt"
#define M_MATRIX  PREFIX " Dot Matrix.txt"
#define G_GG      PREFIX " Game Gear.txt"
#define G_GGMOD   PREFIX " Game Gear Backlit.txt"
#define G_LYNX    PREFIX " Lynx.txt"
#define G_WS      PREFIX " WonderSwan.txt"
#define G_WSC     PREFIX " WonderSwan Color.txt"
#define G_NGPC    PREFIX " Neo Geo Pocket Color.txt"

// Root-relative, where the Game Boy core's own palette browser looks.
#define PAL_DIR    "games/GAMEBOY/Palettes"
#define PAL_DMG    PAL_DIR "/" PREFIX " DMG Green.gbp"
#define PAL_POCKET PAL_DIR "/" PREFIX " Pocket.gbp"

/*
  The core-side halves, shared between entries.

  Every LCD look pins the framework's Scale to HV-integer: the grid filter puts
  its gutter at source-pixel boundaries whatever the scale, but at a fractional
  scale the CELLS come out unequal (4- and 5-wide columns alternating at 4.5x),
  which no real panel does. Integer scale is what makes the grid honest, and the
  core's own Scale option is the one place it can be set per game from here.
  "Narrower" rounds down, so the image always fits.

  CO_LCD_OFF is the reset half of "None" and covers GB and GBA in one string -
  unmatched names are skipped by name lookup, so the GBA sees only its own.
*/
#define CO_INTEGER  "Scale=Narrower HV-Integer"
/*
  Super Game Boy=On rides every DMG-family look: Dinofly runs Zelda with the
  SGB border around the Pocket screen and asked for it as the default -
  confirmed live off the core (the border in the capture; the stale CFG still
  said Off). The custom palette wins the colours, the SGB canvas keeps the
  border, and a player who disagrees flips it in Core options, where the
  per-game record outranks the look.
*/
#define CO_GB_DMG   "Super Game Boy=On;Custom Palette=On;Screen Shadow=Yes;Frame blend=On;" CO_INTEGER

static const preset_def presets[] =
{
	{ "sharp", "Sharp", "No filtering. Square pixels, nothing added.",
	  "off", "off", "off", "off", "off", "off", 0, 0 },

	/*
	  TrashUncle's "Sony PVM" from the distribution's Display Specific pack,
	  component for component - Dinofly picked it over our own recipe. The
	  vfilter is adaptive (dark 30%, bright 70%), which read_video_filter()
	  handles and simulate_look.py deliberately does not - lookshots for this
	  one come from captures, not the model.
	*/
	{ "pvm-rgb", "PVM RGB", "Sony PVM: adaptive scanlines, aperture grille, warm gamma.",
	  "Upscaling - Recommended/GS_Sharpness_050.txt",
	  "Scanlines - Adaptive/SLA_Dk_030_Br_070.txt",
	  "off",
	  "Simple (Monochrome)/Aperture Grille (No Scanlines) (1968).txt",
	  "1x",
	  "Pure_Gamma/gamma_110.txt", 0, 0 },

	/*
	  Short names on purpose: five tiles share a row on the console class since
	  BVM joined, and at 960x540 a longer label is cut off - the no-clipped-copy
	  gate is the arbiter. The blurb carries the longer story.
	*/
	{ "pvm-svideo", "S-Video", "Slight horizontal bleed, scanlines: a console on a good PVM over S-Video.",
	  F_SOFT, F_SHARP, F_SCAN, M_GRILLE, "1x", "off", 0, 0 },

	/*
	  The distribution's own Sony PVM mask at 1x, on Dinofly's call, in place of our
	  generated grille at 2x: a doubled cell is two output pixels per phosphor, which
	  at 1080p is a coarser stripe than any tube ever had, and the 1x PVM mask is the
	  structure he wants under the blur. Same file the PVM look uses for its grille
	  family, so the two reads as one set.
	*/
	{ "composite", "Composite", "Soft and blurry, as an RF or composite hookup looked.",
	  F_BLURRY, F_SOFT, F_SCAN,
	  "Simple (Monochrome)/Sony PVM (Generic) (~1980).txt", "1x", "off", 0, 0 },

	{ "pal-tv", "PAL TV", "Softer still with lighter scanlines. Home computers on a telly.",
	  F_SOFT, F_SOFT, F_SCANLT, M_GRILLE, "2x", "off", 0, 0 },

	{ "vga", "VGA Monitor", "Clean and slightly smoothed. No scanlines: a 31 kHz monitor had none.",
	  F_SOFT, F_SOFT, "off", "off", "off", "off", 0, 0 },

	/*
	  The Game Boy pair. Colour comes from a real .gbp through the core's own
	  palette slot - the scaler gamma that used to fake it tinted the core's
	  already-colourised picture. The grid is the polyphase filter (aligned to
	  core pixels by construction), the shadow is the core's own drop-shadow.
	*/
	{ "dmg", "Game Boy DMG", "Muted olive-green reflective LCD, pixel grid and shadow.",
	  F_GRIDSH, F_GRIDSH, "off", "off", "off", "off", CO_GB_DMG, PAL_DMG },

	{ "pocket", "Game Boy Pocket", "Neutral grey reflective LCD, finer grid, pixel shadow.",
	  F_GRIDSH, F_GRIDSH, "off", "off", "off", "off", CO_GB_DMG, PAL_POCKET },

	{ "gbc", "Game Boy Color", "Reflective colour LCD with its pixel grid.",
	  F_GRID, F_GRID, "off", "off", "off", "off",
	  "GBC Colors=Corrected;Screen Shadow=No;Frame blend=Off;" CO_INTEGER, 0 },

	/*
	  The three GBA screens map to the core's own "Modify Colors" profiles - the
	  Pokefan531 correction, authored per model, applied before scaling. AGS-101
	  is the backlit panel people mod their consoles towards: near-raw colour.
	*/
	{ "agb001", "GBA (AGB-001)", "The original unlit screen. Dim and washed out.",
	  F_GRID, F_GRID, "off", "off", "off", "off",
	  "Modify Colors=GBA 2.2;" CO_INTEGER, 0 },

	{ "ags001", "GBA SP (AGS-001)", "Frontlit SP: brighter than AGB, still washed out.",
	  F_GRID, F_GRID, "off", "off", "off", "off",
	  "Modify Colors=GBA 1.6;" CO_INTEGER, 0 },

	{ "ags101", "GBA SP (AGS-101)", "Backlit SP: bright with proper contrast and colour.",
	  F_GRID, F_GRID, "off", "off", "off", "off",
	  "Modify Colors=Off;" CO_INTEGER, 0 },

	/*
	  The rest of the handhelds, same split, measured against each core's own
	  CONF_STR on the device (2026-08-12): the SMS/Game Gear and NGP cores offer
	  no colour work at all, so their panels stay as scaler gamma LUTs - the one
	  case where the LUT is not fighting anybody. Lynx and WonderSwan publish
	  Flickerblend (Off / 2 Frames / 3 Frames): real temporal ghosting from the
	  core, which those smeary panels had in spades, so their looks turn it on.
	  All of them swap the misaligned Dot Matrix mask for the grid filter and
	  pin integer scale like the GB/GBA looks do.
	*/
	{ "gg", "Game Gear", "Backlit but murky, with the Game Gear's poor contrast.",
	  F_GRID, F_GRID, "off", "off", "off", G_GG, CO_INTEGER, 0 },

	{ "gg-mod", "Game Gear (Backlit Mod)", "The common LED backlight mod: brighter, cleaner whites.",
	  F_GRID, F_GRID, "off", "off", "off", G_GGMOD, CO_INTEGER, 0 },

	{ "lynx", "Atari Lynx", "Backlit colour LCD with a cool cast and washed blacks.",
	  F_GRID, F_GRID, "off", "off", "off", G_LYNX,
	  "Flickerblend=2 Frames;" CO_INTEGER, 0 },

	{ "ws", "WonderSwan", "Reflective mono FSTN: warm grey, low contrast.",
	  F_GRID, F_GRID, "off", "off", "off", G_WS,
	  "Flickerblend=2 Frames;" CO_INTEGER, 0 },

	{ "wsc", "WonderSwan Color", "Reflective colour panel: muted and slightly warm.",
	  F_GRID, F_GRID, "off", "off", "off", G_WSC,
	  "Flickerblend=2 Frames;" CO_INTEGER, 0 },

	{ "ngpc", "Neo Geo Pocket Color", "Reflective pastel colour LCD, gentle contrast.",
	  F_GRID, F_GRID, "off", "off", "off", G_NGPC, CO_INTEGER, 0 },

	/*
	  One switch to turn every layer of processing off: scaler filters, mask and
	  gamma explicitly off, and no core half at all - which means "restore what
	  the look before me changed" when picked live, and "touch nothing" at
	  launch, where the core has just booted from the player's own config (see
	  vp_apply_core_side). Appended at the END of the table - the stored choice
	  in classicui_video.cfg is a raw preset index, so table order is ABI.
	*/
	{ "none", "None", "Every effect off: the core's own picture, nothing added.",
	  "off", "off", "off", "off", "off", "off", 0, 0 },

	// Appended after None for the same ABI reason None sits where it does.
	{ "bvm-rgb", "BVM RGB", "Reference broadcast monitor: razor sharp, deep scanlines.",
	  F_SHARP, F_SHARP, F_SCANDP, M_GRILLE, "1x", "off", 0, 0 },
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
	P_GG, P_GGMOD, P_LYNX, P_WS, P_WSC, P_NGPC, P_NONE, P_BVM
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

  Every class ends with an escape hatch that switches the processing off. For
  the CRT classes that is Sharp (their filters are the only layer); handhelds
  get None, which also resets the core-side effects their looks drive. This
  replaces the old stance that "an unfiltered Game Boy is not a look anyone is
  after" - Dinofly's rule is now that every single system must offer it.

  The GBC list lost the three GBA screens: those looks now speak the GBA core's
  "Modify Colors" language, which the Game Boy core does not publish, so on a
  GBC cartridge they would silently do nothing. If a GBC-cart-on-GBA-screen look
  comes back it will be through the GB core's own GBC colour LUT slot (FC7).
*/
static const int opt_console[]  = { P_PVM_RGB, P_BVM, P_PVM_SVIDEO, P_COMPOSITE, P_SHARP };
static const int opt_arcade[]   = { P_PVM_RGB, P_BVM, P_PVM_SVIDEO, P_SHARP };
static const int opt_computer[] = { P_PAL_TV, P_COMPOSITE, P_PVM_SVIDEO, P_SHARP };
static const int opt_vga[]      = { P_VGA, P_SHARP };
static const int opt_gb[]       = { P_DMG, P_POCKET, P_NONE };
static const int opt_gbc[]      = { P_GBC, P_NONE };
static const int opt_gba[]      = { P_AGB001, P_AGS001, P_AGS101, P_NONE };
static const int opt_gg[]       = { P_GG, P_GGMOD, P_NONE };
static const int opt_lynx[]     = { P_LYNX, P_NONE };
static const int opt_ws[]       = { P_WS, P_NONE };
static const int opt_wsc[]      = { P_WSC, P_WS, P_NONE };
static const int opt_ngpc[]     = { P_NGPC, P_NONE };

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

/* ---- generated files, and when a new build may rewrite them ----

  The old rule was "never overwrite", which meant a card that had ever booted an
  earlier build kept its old filters and presets for ever - a shipped fix never
  reached anyone. The rule now is: a file WE generated may be regenerated when
  its content changes; a file the user made (or edited enough to remove the
  marker) is never touched. Ownership is the GEN_MARK line for text files; .gbp
  is a fixed binary layout with its tail documented as reserved-zero, so ours
  carry "CH" in bytes 12-13 (the Game Boy core keeps those bits off-screen).

  Everything is composed in memory first so "would it change" is one compare,
  and an unchanged file is not rewritten at all - vp_install() runs on every
  boot, and the SD card does not want 30 spurious writes each time.
*/
#define GEN_MARK "generated by Classic Home"

struct genbuf
{
	char b[24 * 1024];
	int len;
	int overflow;
};

static void gb_reset(genbuf *g) { g->len = 0; g->overflow = 0; }

static void gb_bytes(genbuf *g, const void *p, int n)
{
	if (g->len + n > (int)sizeof(g->b)) { g->overflow = 1; return; }
	memcpy(g->b + g->len, p, n);
	g->len += n;
}

static void gb_addf(genbuf *g, const char *fmt, ...)
{
	char line[512];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	if (n < 0) return;
	if (n > (int)sizeof(line) - 1) n = (int)sizeof(line) - 1;
	gb_bytes(g, line, n);
}

static int gen_owned(const char *data, int len, int gbp)
{
	if (gbp) return (len >= 14 && data[12] == 'C' && data[13] == 'H');

	int scan = (len < 1024) ? len : 1024;
	int mlen = (int)strlen(GEN_MARK);
	for (int i = 0; i + mlen <= scan; i++)
		if (!memcmp(data + i, GEN_MARK, mlen)) return 1;
	return 0;
}

/*
  Pristine-or-edited, decided by a content stamp rather than by the marker
  alone. The marker says a file was BORN ours; it cannot say nobody touched it
  since, and the whole point of leaving user files alone dies if hand-tuning a
  coefficient is reverted on the next boot (the first cut of this did exactly
  that). So every generated text file ends in "# build <hash>" over the bytes
  above it, and a .gbp carries the same idea in its reserved tail: 'C','H',
  then a 16-bit hash of the 14 bytes before it.

  Three states on read: no marker - the user's file, never touched; marker and
  a stamp that matches the content - our pristine output, regenerate freely;
  marker but the stamp is missing (files from the first stampless build) or
  does not match - somebody edited it, leave it and say so once.
*/
static uint32_t gen_hash(const char *p, int n)
{
	uint32_t h = 2166136261u;
	for (int i = 0; i < n; i++) { h ^= (unsigned char)p[i]; h *= 16777619u; }
	return h;
}

#define GEN_STAMP "# build "

// 1 = safe to rewrite (missing, or provably our pristine output).
static int gen_may_write(int gbp, const char *old, int olen, int more)
{
	if (more) return 0;                       // longer than the buffer: not ours
	if (!gen_owned(old, olen, gbp)) return 0;

	if (gbp)
	{
		if (olen != 16) return 0;
		uint16_t st = (uint16_t)(((unsigned char)old[14] << 8) | (unsigned char)old[15]);
		// 0 is the first build's stampless tail: pristine by construction.
		if (st && st != (uint16_t)gen_hash(old, 14)) return 0;
		return 1;
	}

	/*
	  Text: find the stamp line wherever it is - anchoring on "the last line"
	  would misread a file with lines APPENDED after the stamp as stampless and
	  overwrite exactly the hand-edit this exists to protect.
	*/
	int slen = (int)strlen(GEN_STAMP);
	int at = -1;
	for (int i = 0; i + slen <= olen; i++)
	{
		if ((i == 0 || old[i - 1] == '\n') && !strncmp(old + i, GEN_STAMP, slen)) at = i;
	}

	// A marked file without a stamp: the first build wrote these, and only on
	// the cards we ourselves deployed that day. Treated as pristine.
	if (at < 0) return 1;

	// Anything after the stamp's own line is a user addition.
	int eol = at;
	while (eol < olen && old[eol] != '\n') eol++;
	if (eol < olen && eol + 1 < olen) return 0;

	uint32_t st = (uint32_t)strtoul(old + at + slen, 0, 16);
	if (st != gen_hash(old, at)) return 0;
	return 1;
}

// Writes rel (root-relative) from g, honouring the ownership rule above.
static void gen_commit(const char *rel, genbuf *g, int gbp)
{
	if (g->overflow) { printf("ClassicUI: %s overflowed the compose buffer, not written\n", rel); return; }

	// Seal the buffer with its stamp before any comparison.
	if (gbp)
	{
		if (g->len == 16)
		{
			uint16_t h = (uint16_t)gen_hash(g->b, 14);
			g->b[14] = (char)(h >> 8);
			g->b[15] = (char)(h & 0xff);
		}
	}
	else
	{
		gb_addf(g, GEN_STAMP "%08x\n", gen_hash(g->b, g->len));
		if (g->overflow) { printf("ClassicUI: %s overflowed the compose buffer, not written\n", rel); return; }
	}

	char p[1200];
	snprintf(p, sizeof(p), "%s/%s", getRootDir(), rel);

	FILE *f = fopen(p, "rb");
	if (f)
	{
		static char old[sizeof(g->b)];
		int olen = (int)fread(old, 1, sizeof(old), f);
		int more = fgetc(f) != EOF;
		fclose(f);

		if (olen == g->len && !memcmp(old, g->b, g->len)) return;
		if (!gen_may_write(gbp, old, olen, more))
		{
			// Only marked-but-edited files are worth a line: a user's own file
			// being left alone is not news.
			if (gen_owned(old, olen, gbp))
				printf("ClassicUI: %s was hand-edited, keeping it\n", rel);
			return;
		}
	}

	f = fopen(p, "wb");
	if (!f) { printf("ClassicUI: could not write %s\n", rel); return; }
	fwrite(g->b, 1, g->len, f);
	fclose(f);
	printf("ClassicUI: wrote %s\n", rel);
}

/*
  64, and not fewer, because of a trap in read_video_filter() (video.cpp): a
  32-line file is taken for the LEGACY 16-phase format and only its first half
  is ever loaded - the half of the pixel BEFORE the grid filter's centered
  gutter, so the shipped grid degenerated into an asymmetric smear. Every
  ClassicHome filter had been 32 lines since the beginning, so all of them were
  being half-read; it went unseen because the harness stubs video.cpp and a
  device screenshot taps core video before the scaler. 64 lines take the
  ordinary path (and it is what the community filter packs use). The on-device
  check is the firmware's own log line: "Filter '...', phases: 64".
*/
#define PHASES 64

/*
  4-tap polyphase coefficients, 128 = 1.0. A line summing to less than 128 comes
  out darker - a scanline. A line summing to MORE is a brightness boost, and is
  legitimate: the shipped LCD_Effect filters run up to ~143, which is how the
  grid filter below can dim its gutters without dimming the whole picture. (An
  earlier comment here claimed 128 was a hard ceiling; the shipped filters
  disprove it.)
*/
static void filter_line(genbuf *g, const double w[4], double gain)
{
	int c[4];
	for (int t = 0; t < 4; t++)
	{
		c[t] = (int)lround(w[t] * gain * 128.0);
		if (c[t] > 255) c[t] = 255;
		if (c[t] < -255) c[t] = -255;
	}
	gb_addf(g, "%4d,%4d,%4d,%4d\n", c[0], c[1], c[2], c[3]);
}

static void write_filter(const char *name, int kind, double scan_depth)
{
	genbuf g;
	gb_reset(&g);

	gb_addf(&g, "# %s\n", GEN_MARK);
	gb_addf(&g, "# 4 taps, %d phases\n\n", PHASES);

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

		filter_line(&g, w, gain);
	}

	char rel[1024];
	snprintf(rel, sizeof(rel), "filters/%s", name);
	gen_commit(rel, &g, 0);
}

/*
  The LCD pixel grid, drawn by the polyphase filter instead of a shadow mask.

  A shadow mask repeats in OUTPUT pixels: it lines up with the core's pixels
  only at an integer scale AND an image offset the cell divides, and 1080p
  denies the Game Boy both (7x, x-offset 400). This filter is indexed by the
  position INSIDE each source pixel, so the gutter lands on every core pixel
  boundary at any scale and any offset - a grid that cannot disagree with the
  native pixel count, which is the requirement for having one at all.

  Nearest-neighbour switches source pixels at phase 0.5, so the gutter is
  centred there and the crossfade hides inside the dark line. The body is
  boosted to keep the average near 1.0, exactly like the community LCD_Effect
  filters.
*/
#define GRID_GUTTER 0.22   // dark line width, as a fraction of the cell
#define GRID_DEPTH  0.42   // how dark: 0 = invisible, 1 = black

/*
  The reflective LCD's drop shadow, as the old distribution "LCD Effect" filters
  drew it and Dinofly remembers it: every pixel casts faintly onto the leading
  band of the next cell, down and to the right. In a linear filter that is a
  ghost tap - the band takes SHADOW_MIX of the previous source pixel - plus a
  small unconditional dim so the shadow still reads where neighbours are equal.
  Localised to the band by phase, which a mask could never do: it rides the
  source pixel at any integer scale, like the gutter above it. The same file
  serves both axes, so the diagonal falls out of separability.
*/
#define SHADOW_WIDTH 0.30  // leading band of the cell that carries the shadow
#define SHADOW_MIX   0.35  // how much of the previous pixel the band starts with
#define SHADOW_DIM   0.10  // unconditional darkening at the band's start

static void write_filter_grid(const char *name, int shadow)
{
	genbuf g;
	gb_reset(&g);

	gb_addf(&g, "# %s\n", GEN_MARK);
	if (shadow)
		gb_addf(&g, "# LCD grid: gutter %.0f%%/%.0f%%, pixel shadow %.0f%% over the leading %.0f%%\n\n",
			GRID_GUTTER * 100, GRID_DEPTH * 100, SHADOW_MIX * 100, SHADOW_WIDTH * 100);
	else
		gb_addf(&g, "# LCD grid: gutter %.0f%% of the cell at %.0f%% depth\n\n",
			GRID_GUTTER * 100, GRID_DEPTH * 100);

	double half = GRID_GUTTER / 2;
	double soft = 1.0 / PHASES;                     // one-phase shoulders
	double boost = 1.0 / (1.0 - GRID_DEPTH * GRID_GUTTER);
	if (boost > 1.12) boost = 1.12;

	for (int p = 0; p < PHASES; p++)
	{
		double x = (double)p / PHASES;
		double d = fabs(x - 0.5);

		double e = 0;                               // gutter envelope
		if (d < half) e = 1.0;
		else if (d < half + soft) e = 1.0 - (d - half) / soft;

		double w[4] = { 0, 0, 0, 0 };
		if (x < 0.5 - half) w[1] = 1.0;
		else if (x > 0.5 + half) w[2] = 1.0;
		else
		{
			double t = (x - (0.5 - half)) / GRID_GUTTER;
			w[1] = 1.0 - t;
			w[2] = t;
		}

		double gain = boost * (1.0 - GRID_DEPTH * e);

		/*
		  The cell body runs from the gutter's far shoulder to the next gutter,
		  i.e. source pixel [2]'s territory: phases past 0.5+half. The shadow
		  band is its first SHADOW_WIDTH, fading linearly. Mixing toward tap [1]
		  keeps the row sum constant, so only the gain dim changes brightness.
		*/
		if (shadow && x > 0.5 + half)
		{
			double into = (x - (0.5 + half)) / (1.0 - (0.5 + half));
			if (into < SHADOW_WIDTH)
			{
				double f = 1.0 - into / SHADOW_WIDTH;
				w[1] += SHADOW_MIX * f * w[2];
				w[2] -= SHADOW_MIX * f * w[2];
				gain *= 1.0 - SHADOW_DIM * f;
			}
		}

		filter_line(&g, w, gain);
	}

	char rel[1024];
	snprintf(rel, sizeof(rel), "filters/%s", name);
	gen_commit(rel, &g, 0);
}

/*
  Shadow masks: "w,h" then h rows of w hex values. In the non-v2 form only the
  low three bits are used, one per colour channel (setShadowMask in video.cpp),
  so 7 passes everything and 0 is black.
*/
static void write_mask(const char *name, int kind)
{
	genbuf g;
	gb_reset(&g);

	gb_addf(&g, "# %s\n", GEN_MARK);

	if (kind == 0)
	{
		// Aperture grille: R, G, B stripes.
		gb_addf(&g, "3,1\n");
		gb_addf(&g, "1,2,4\n");
	}
	else
	{
		// LCD pixel grid: a black gutter on two sides of each cell.
		gb_addf(&g, "4,4\n");
		gb_addf(&g, "7,7,7,0\n");
		gb_addf(&g, "7,7,7,0\n");
		gb_addf(&g, "7,7,7,0\n");
		gb_addf(&g, "0,0,0,0\n");
	}

	char rel[1024];
	snprintf(rel, sizeof(rel), "shadow_masks/%s", name);
	gen_commit(rel, &g, 0);
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
	genbuf g;
	gb_reset(&g);

	gb_addf(&g, "# %s\n", GEN_MARK);
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
		gb_addf(&g, "%d,%d,%d\n", v[0], v[1], v[2]);
	}

	char rel[1024];
	snprintf(rel, sizeof(rel), "gamma/%s", name);
	gen_commit(rel, &g, 0);
}

/*
  Handheld panels still done as per-channel curves - the ones whose cores offer
  no colour work of their own. The GB and GBA entries that used to sit here are
  gone: their colour is now the core's (palette slot / Modify Colors), because a
  LUT cannot mix channels and, worse, it stacked on top of the core's own
  colourisation.
*/
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

/*
  Game Boy palettes, written as ordinary .gbp files the core (and the classic
  OSD) can load: 4 colours, lightest to darkest, 3 bytes each; the reserved tail
  carries the ownership mark (see gen_owned).

  DMG was the bgb green - the palette the emulation world settled on - until
  Dinofly judged it too vibrant against the real thing, which it is: bgb's
  lightest stop is a spring green no unlit reflective screen ever showed. These
  are the greyer olives measured off photographed DMG-01 screens. The Pocket is
  GrafxGray's neutral warm grey, the closest shipped match for that screen, and
  passed his eye as-is. Both also drive the preview rendering, so the numbers
  live here rather than only in the files.
*/
struct gbp_spec { const char *rel; uint8_t c[4][3]; };

static const gbp_spec pal_dmg =
{ PAL_DMG,    { { 0xC4, 0xCF, 0xA1 }, { 0x8B, 0x95, 0x6D }, { 0x4D, 0x53, 0x3C }, { 0x1F, 0x1F, 0x1F } } };

static const gbp_spec pal_pocket =
{ PAL_POCKET, { { 0xE0, 0xDB, 0xCD }, { 0xA8, 0x9F, 0x94 }, { 0x70, 0x6B, 0x66 }, { 0x2B, 0x2B, 0x26 } } };

static void write_gbp(const gbp_spec *s)
{
	genbuf g;
	gb_reset(&g);

	for (int i = 0; i < 4; i++) gb_bytes(&g, s->c[i], 3);
	gb_bytes(&g, "CH\0", 4);              // 16 bytes total, tail marks ownership

	gen_commit(s->rel, &g, 1);
}

static void write_preset(const preset_def *d)
{
	genbuf g;
	gb_reset(&g);

	gb_addf(&g, "# %s - %s\n", d->name, d->blurb);
	gb_addf(&g, "# %s\n", GEN_MARK);
	if (d->hfilter)  gb_addf(&g, "hfilter=%s\n", d->hfilter);
	if (d->vfilter)  gb_addf(&g, "vfilter=%s\n", d->vfilter);
	if (d->sfilter)  gb_addf(&g, "sfilter=%s\n", d->sfilter);
	if (d->mask)     gb_addf(&g, "mask=%s\n", d->mask);
	if (d->maskmode) gb_addf(&g, "maskmode=%s\n", d->maskmode);
	if (d->gamma)    gb_addf(&g, "gamma=%s\n", d->gamma);

	char rel[1024];
	snprintf(rel, sizeof(rel), "presets/%s %s.ini", PREFIX, d->name);
	gen_commit(rel, &g, 0);
}

void vp_install()
{
	ensure_dir("filters");
	ensure_dir("shadow_masks");
	ensure_dir("gamma");
	ensure_dir("presets");
	// The palette lives where the core's own browser looks. games/ exists on
	// any card that can play anything; the two below might not.
	ensure_dir("games");
	ensure_dir("games/GAMEBOY");
	ensure_dir(PAL_DIR);

	write_filter(F_SHARP, 0, 0.0);
	write_filter(F_SOFT, 1, 0.0);
	write_filter(F_BLURRY, 2, 0.0);
	// Scanline depth is the single most subjective number here, and combined with
	// a full aperture grille it is easy to end up too dark. These are deliberately
	// moderate; tune them on real hardware.
	write_filter(F_SCAN, 1, 0.28);
	write_filter(F_SCANLT, 1, 0.15);
	// The BVM's line structure: a reference monitor resolves the gaps a
	// consumer set smears over, which on real hardware reads as deep scanlines.
	write_filter(F_SCANDP, 0, 0.45);
	write_filter_grid(F_GRID, 0);
	write_filter_grid(F_GRIDSH, 1);

	write_mask(M_GRILLE, 0);
	write_mask(M_MATRIX, 1);

	write_gamma(G_GG, &lut_gg);
	write_gamma(G_GGMOD, &lut_ggmod);
	write_gamma(G_LYNX, &lut_lynx);
	write_gamma(G_WS, &lut_ws);
	write_gamma(G_WSC, &lut_wsc);
	write_gamma(G_NGPC, &lut_ngpc);

	write_gbp(&pal_dmg);
	write_gbp(&pal_pocket);

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

	if (d->palette && !exists_rel(d->palette)) return 0;

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

	/*
	  Choosing the default REMOVES the record rather than storing it, so a stored
	  choice always means "this system is deliberately not on its default".

	  That invariant is worth more than the byte it saves. Dinofly asked why Game
	  Boy was not on DMG by default when DMG is exactly what vp_default_for()
	  answers: his card carried a per-system record from an earlier session of
	  ours, the front-end honoured it, and nothing on screen could tell the two
	  apart. With this, a record is evidence of a decision, and the footer below
	  can name the default only when one was really overridden.
	*/
	if (preset == vp_default_for(vclass))
	{
		for (int i = 0; i < nvprecs; i++)
		{
			if (vprecs[i].key != k) continue;
			for (int j = i; j + 1 < nvprecs; j++) vprecs[j] = vprecs[j + 1];
			nvprecs--;
			FileSaveConfig("classicui_video.cfg", vprecs, nvprecs * (int)sizeof(vp_rec));
			return;
		}
		return;                                  // already on the default: nothing stored
	}

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

/*
  A static, distribution-provided preview beats a computed one: the real filters
  live in the FPGA scaler where nothing can read them back, so anything rendered
  at runtime is an approximation of an approximation. When the card carries
  classicui/lookshots/<id>.png - a good zoomed-in photo or render showing the
  phosphors or the grid - the Display screen shows that instead. Keyed by the
  preset id, which is stable; the computed illustration stays as the fallback so
  a card without the pack loses nothing.
*/
int vp_lookshot_path(int i, char *out, int len)
{
	if (i < 0 || i >= NPRESETS) return 0;

	/*
	  Never for None. Its honest preview is the player's own unprocessed frame,
	  and the shipped none.png made every system's None tile the same picture -
	  which is what Dinofly reported. Refused here rather than only unshipped,
	  so the copies already on cards go quiet too.
	*/
	if (!strcmp(presets[i].id, "none")) return 0;

	char rel[256];
	snprintf(rel, sizeof(rel), "classicui/lookshots/%s.png", presets[i].id);
	if (!exists_rel(rel)) return 0;

	snprintf(out, len, "%s/%s", getRootDir(), rel);
	return 1;
}

/*
  The core-side half of a look: named options, then the palette file.

  Everything here is by name against whatever core is running, so a look is a
  request, not a command - "Modify Colors" on a SNES matches nothing and nothing
  happens, which is what lets one None entry cover every system. Values go
  through core_opt_set(), which changes the live status word and deliberately
  never touches <CORE>.CFG: a game launched from the stock menu keeps the
  player's own core setup.

  The palette is gated on the core publishing "Custom Palette": index 3 is only
  known to mean "palette" on the Game Boy core, and a file pushed at some other
  core's index 3 would be loaded as who-knows-what.
*/
/*
  The look applied to the running core this session, for the delayed re-apply:
  chome_core_boot() runs on the first UI frame, but an MGL delivers the ROM a
  couple of seconds later and the core resets around it - late enough to undo
  status bits set at boot. chome_core_poll() calls vp_reapply_core_side() once,
  after its reference-shot delay, so the look wins whichever order the boot
  dance ran in. Idempotent and cheap: it is the same status writes again.
*/
static int vp_running_look = -1;

/*
  What the look changed and what stood there before, so it can all be undone.

  Two consumers. Picking a look with no core half (None, or any CRT look after
  an LCD one) restores these - which is the only correct meaning of "nothing
  added": putting back the PLAYER's values, not some table of core defaults
  that would trample a deliberate choice like GBC Colors=Raw. And
  user_io_status_save() swaps them in around its write, so a "Save settings"
  from any OSD captures the player's configuration, never the look's session
  values - the guarantee that nothing a look does outlives the session.

  Recorded once per option per session, before the first look touches it.
*/
struct vp_orig { char spec[12]; uint8_t ex; uint32_t val; };
#define VP_ORIG_MAX 24
static vp_orig vp_origs[VP_ORIG_MAX];
static int nvp_origs = 0;

static void vp_record_original(const char *name)
{
	char spec[12];
	int ex = 0;
	uint32_t val = 0;
	if (!core_opt_read_named(name, spec, sizeof(spec), &ex, &val)) return;

	for (int i = 0; i < nvp_origs; i++)
		if (vp_origs[i].ex == ex && !strcmp(vp_origs[i].spec, spec)) return;

	if (nvp_origs >= VP_ORIG_MAX) return;
	snprintf(vp_origs[nvp_origs].spec, sizeof(vp_origs[0].spec), "%s", spec);
	vp_origs[nvp_origs].ex = (uint8_t)ex;
	vp_origs[nvp_origs].val = val;
	nvp_origs++;
}

static void vp_restore_originals()
{
	for (int i = 0; i < nvp_origs; i++)
		user_io_status_set(vp_origs[i].spec, vp_origs[i].val, vp_origs[i].ex);
	if (nvp_origs) printf("ClassicUI: put back %d core option(s) a look had set\n", nvp_origs);
}

/*
  Forget the records. On hardware every core load re-execs the firmware, so a
  session and a process are the same thing and nobody needs this; the harness
  runs many sessions in one process and does.
*/
void vp_forget_originals()
{
	nvp_origs = 0;
	vp_running_look = -1;
}

static void vp_apply_core_side(int i, int with_palette)
{
	if (i < 0 || i >= NPRESETS) return;
	const preset_def *d = &presets[i];

	if (!d->core_opts && !d->palette)
	{
		// A look with no core half still undoes the one it replaces.
		vp_restore_originals();
		vp_running_look = -1;
		return;
	}

	if (core_opts_scan() <= 0) return;
	vp_running_look = i;

	if (d->core_opts)
	{
		char list[512];
		snprintf(list, sizeof(list), "%s", d->core_opts);

		char *save = 0;
		for (char *pair = strtok_r(list, ";", &save); pair; pair = strtok_r(0, ";", &save))
		{
			char *eq = strchr(pair, '=');
			if (!eq) continue;
			*eq = 0;
			vp_record_original(pair);
			if (!core_opt_set_named(pair, eq + 1))
				printf("ClassicUI: look \"%s\": core has no %s=%s, skipped\n",
					d->name, pair, eq + 1);
		}
	}

	if (d->palette)
	{
		vp_record_original("Custom Palette");
		if (with_palette && core_opt_set_named("Custom Palette", "On") && exists_rel(d->palette))
		{
			char full[1200];
			snprintf(full, sizeof(full), "%s/%s", getRootDir(), d->palette);
			printf("ClassicUI: look \"%s\": palette %s\n", d->name, d->palette);
			user_io_file_tx(full, 3 /* the GB core's FC3 slot */);
		}
		else if (!with_palette)
		{
			core_opt_set_named("Custom Palette", "On");
		}
	}
}

/*
  The save shield. user_io_status_save() calls these around its write so that
  <CORE>.CFG only ever receives the player's own values - see vp_origs above.
  The core sees the originals for the few milliseconds of the save; a save is a
  deliberate, rare act, and a one-frame revert of a scale or a palette flag is
  invisible next to persisting the wrong configuration forever. Resume skips
  the palette upload: the file is already in the core and no status was lost.
*/
void vp_core_side_suspend()
{
	vp_restore_originals();
}

void vp_core_side_resume()
{
	if (vp_running_look >= 0) vp_apply_core_side(vp_running_look, 0);
}

void vp_arm_for_launch(int sysidx, int vclass_hint)
{
	int p = vp_effective(sysidx, vclass_hint);
	if (p < 0 || p >= NPRESETS) return;

	FILE *f = fopen(PENDING, "wt");
	if (!f) return;

	// video_loadPreset() takes a path it can open directly. The second line
	// names the look so the core-side half can be applied once the core is up -
	// an older line-1-only file still works, it just carries no core half.
	fprintf(f, "%s/presets/%s %s.ini\n", getRootDir(), PREFIX, presets[p].name);
	fprintf(f, "look=%s\n", presets[p].id);
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
  The core half is status bits and a palette upload, none of which move video modes
  either.
*/
int vp_apply_now(int sysidx, int vclass_hint)
{
	int i = vp_effective(sysidx, vclass_hint);
	if (i < 0 || i >= NPRESETS) return 0;

	char path[1024];
	if (!vp_preset_path(i, path, sizeof(path))) return 0;

	printf("ClassicUI: applying video look \"%s\" to the running core\n", presets[i].name);
	video_loadPreset(path, true);
	vp_apply_core_side(i, 1);
	return 1;
}

void vp_apply_pending()
{
	FILE *f = fopen(PENDING, "rt");
	if (!f) return;

	char path[1024] = {};
	char look[128] = {};
	if (fgets(path, sizeof(path), f))
	{
		char *nl = strchr(path, '\n');
		if (nl) *nl = 0;

		char line[160] = {};
		if (fgets(line, sizeof(line), f) && !strncmp(line, "look=", 5))
		{
			snprintf(look, sizeof(look), "%s", line + 5);
			char *lnl = strchr(look, '\n');
			if (lnl) *lnl = 0;
		}
	}
	fclose(f);
	unlink(PENDING);

	if (!path[0]) return;

	printf("ClassicUI: applying video look %s\n", path);
	video_loadPreset(path, true);

	/*
	  The scaler half only. This runs from chome_core_boot(), which HandleUI()
	  reaches WHILE user_io is still feeding the core its boot files - applying
	  the core half here interleaved the palette upload with boot1.rom (one
	  transfer came back CRC 00000000) and the MGL's ROM was never sent at all:
	  a blank core wearing the right palette. The look is parked instead, and
	  chome_core_poll() plays it once menu_mgl_busy() says the launch is over.

	  A look with no core half is not parked at all: at launch the core has just
	  booted from its own config, which already IS "nothing added", and there is
	  nothing from any earlier session to undo - each launch is a new process.
	*/
	if (look[0])
	{
		for (int i = 0; i < NPRESETS; i++)
			if (!strcmp(presets[i].id, look) && (presets[i].core_opts || presets[i].palette))
			{
				vp_running_look = i;
				break;
			}
	}
}

void vp_reapply_core_side()
{
	if (vp_running_look >= 0) vp_apply_core_side(vp_running_look, 1);
}

/* -------------------------------------------------- the analog output ----- */

/*
  Where the front-end's framebuffer actually comes out, and what that costs. All of it
  is settled by three ini keys and by whether an HDMI sink is attached; none of it is
  measurable from here, so this is written from what the firmware and sys_top.v do.

  The framebuffer is composited into the *scaler* output. Four ways it reaches a
  television, and they are not equivalent:

    vga_scaler=1      the analog port carries the scaler output permanently.
    direct_video=1    video_fb_enable() calls set_vga_fb() when the framebuffer goes
                      up (video.cpp), so the analog port carries it in the TV mode
                      direct_video already runs.
    neither, no HDMI  video_menu_fb_analog() takes the port for as long as the
                      front-end holds the screen, in a 240p or 288p TV mode.
    neither, HDMI on  it does not reach the analog port at all. That is deliberate -
                      taking the port would drag the HDMI display down to 240p with
                      it - and it is why one reporter saw the stock menu correctly on
                      a CRT and this front-end not at all, with hd/sd/lo making no
                      difference: the layout was never on that wire.

  In the first three the colour goes. sys_top.v instantiates yc_out - the S-Video and
  composite encoder - on the direct core-video path only, and the analog pins select
  the scaler path whenever vga_fb or vga_scaler is set:

    wire vgas_en = vga_fb | vga_scaler;
    assign VGA_R = ... vgas_en ? vgas_o[23:18] : vga_o[23:18];

  where vga_o is the leg carrying `yc_en ? yc_o : vga_o_t` and vgas_o is not. The
  external-encoder subcarrier is gated with `& ~vgas_en` in the same file, so
  vga_mode=subcarrier is dead there too. It is worse than a missing burst: yc_out
  packs chroma into R and luma into G (`dout = {C, Y, 8'd0}`), so on the scaler path
  the set's chroma input is fed a plain red channel and its luma input a plain green
  one. Black and white at best. Nothing on the HPS side can change it - the wire is
  not there - and set_yc_mode() is unreachable during the takeover anyway, because
  video_mode_adjust() returns early while it is held.

  This is why the report says which of the four is happening rather than offering to
  change it. See README.md, "Analog video", for the whole chain.
*/
int vp_analog_facts(int hdmi)
{
	int enc = (cfg.vga_mode_int >= 2);         // svideo, cvbs, or an external encoder
	int dv  = cfg.direct_video ? 1 : 0;
	int vs  = cfg.vga_scaler ? 1 : 0;
	int seen = (hdmi != 0);                    // unknown counts as attached

	/*
	  Does anything suggest a television is connected? vga_mode=rgb with a display on
	  HDMI is the shipped default and the commonest machine there is, and telling that
	  player about composite encoders would be noise on every screen. A vga_mode the
	  player chose, either routing key set, or no HDMI sink at all are each a reason to
	  speak; nothing else is.
	*/
	if (cfg.vga_mode_int == 0 && !dv && !vs && seen) return 0;

	int takeover = (!dv && !vs && !seen);
	int on_analog = (dv || vs || takeover);

	int f = 0;

	/*
	  tv_fb_mode() no longer scandoubles the takeover on an encoded output, but
	  video_mode_load() still does for direct_video - and there it sets the mode every
	  core runs in, which is not the front-end's to overrule. So it is named instead.
	*/
	if (enc && dv && cfg.forced_scandoubler) f |= VP_AN_31K;

	if (!dv && !vs && seen) f |= VP_AN_NOTUS;

	/*
	  60 Hz whatever the set is: tv_fb_mode() chooses between the NTSC and PAL members
	  of tvmodes[] on cfg.menu_pal alone, and menu_pal defaults to 0. A 50 Hz-only
	  television rolls - which is exactly the earlier report, on component, where there
	  was no colour to lose and the rolling was all that was left. Only for the
	  takeover: under vga_scaler or direct_video the mode is the player's own.
	*/
	if (takeover && !cfg.menu_pal) f |= VP_AN_60HZ;

	if (enc && on_analog) f |= VP_AN_MONO;

	return f;
}

const char *vp_analog_text(int fact)
{
	switch (fact)
	{
	case VP_AN_31K:   return "forced_scandoubler=1: 31kHz out";
	case VP_AN_NOTUS: return "HDMI on: the CRT shows the core";
	case VP_AN_MONO:  return "Black and white on S-Video/CVBS";
	case VP_AN_60HZ:  return "60Hz out: try menu_pal=1 for PAL";
	}
	return 0;
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
		if (!strcmp(presets[i].gamma, G_GG)) s = &lut_gg;
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
  The core-side halves, simulated for the preview only.

  The Game Boy looks replace four shades with a palette, so the preview maps
  luminance into the same four colours the .gbp carries - over a real capture
  that recovers almost exactly what the core will show, because a DMG frame
  only ever holds four levels.

  The GBA looks run the core's "Modify Colors" - a 3x3 matrix over
  gamma-decoded channels (gba_gpu_colorshade.vhd, coefficients /1024). The
  matrix here is the core's own gba-color set; the gamma pair approximates its
  LUT sections: 2.2 encodes darker than it decodes (the unlit panel), 1.6
  meets it in the middle.
*/
static const gbp_spec *pv_palette(int i)
{
	if (!presets[i].palette) return 0;
	if (!strcmp(presets[i].palette, PAL_DMG)) return &pal_dmg;
	if (!strcmp(presets[i].palette, PAL_POCKET)) return &pal_pocket;
	return 0;
}

// 0 = none, else the decode gamma the profile is labeled with.
static double pv_gba_gamma(int i)
{
	if (!presets[i].core_opts) return 0;
	if (strstr(presets[i].core_opts, "Modify Colors=GBA 2.2")) return 2.2;
	if (strstr(presets[i].core_opts, "Modify Colors=GBA 1.6")) return 1.6;
	return 0;
}

static void pv_apply_palette(const gbp_spec *pal, uint8_t c[3])
{
	// Perceptual-ish luma, quantised to the four DMG levels, brightest first.
	int y = (c[0] * 299 + c[1] * 587 + c[2] * 114) / 1000;
	int lv = 3 - ((y * 4) / 256 > 3 ? 3 : (y * 4) / 256);
	c[0] = pal->c[lv][0];
	c[1] = pal->c[lv][1];
	c[2] = pal->c[lv][2];
}

static void pv_apply_gba(double g, uint8_t c[3])
{
	// gba-color: R'G'B' = M * RGB in light-linear-ish space, then re-encode.
	static const int m[3][3] =
	{
		{ 865, 174, -15 },
		{  92, 696, 236 },
		{ 164,  87, 773 },
	};

	double in[3], out[3];
	for (int k = 0; k < 3; k++) in[k] = pow(c[k] / 255.0, g);

	for (int r = 0; r < 3; r++)
	{
		double v = (m[r][0] * in[0] + m[r][1] * in[1] + m[r][2] * in[2]) / 1024.0;
		if (v < 0) v = 0;
		if (v > 1) v = 1;
		out[r] = pow(v, 1.0 / 2.2);      // back to the panel the player is on
	}

	for (int k = 0; k < 3; k++) c[k] = (uint8_t)lround(out[k] * 255.0);
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
	// One symbolic cell grid for both mechanisms: the mask the colour handhelds
	// still use, and the polyphase grid filter of the GB/GBA looks.
	int matrix = (d->mask && !strcasecmp(d->mask, M_MATRIX)) ? 1 : 0;
	if (d->hfilter && !strcasecmp(d->hfilter, F_GRID)) matrix = 1;

	const gbp_spec *pal = pv_palette(i);
	double gba_g = pv_gba_gamma(i);

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

			if (pal) pv_apply_palette(pal, c);
			else if (gba_g > 0) pv_apply_gba(gba_g, c);

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
