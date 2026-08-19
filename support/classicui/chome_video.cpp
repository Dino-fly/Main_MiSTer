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
#include "chome_ini.h"
#include "../../cfg.h"
#include "../../file_io.h"
#include "../../user_io.h"
#include "../../video.h"
#include "../../scaler.h"
#include "../../hardware.h"

#define PREFIX "ClassicHome"
#define PENDING "/tmp/classicui_preset"

/* ------------------------------------------------------------- the table -- */

/*
  A look drives two machines at once.

  The scaler half (filters, mask, gamma) goes through a preset file exactly as
  before. The core half is new: core_colour and core_struct are ;-separated
  lists of "Option Name=Value Name" pairs matched against the running core's
  CONF_STR by NAME - a core that does not publish the option is simply left
  alone, which is what makes one "None" entry safe on every system. Names, not
  bit positions, for the same reason snacpad.cpp does it: a core update that
  inserts an option renumbers every bit after it.

  Why the core half is TWO columns and not one: what a look does divides into
  colour and structure, and only colour survives on an analog display.

    core_colour   the palette, the colour correction - what the panel's dyes and
                  filters did to the picture. A player with a Game Boy on a CRT
                  wants DMG green exactly as much as one on HDMI.
    core_struct   the pixel grid, the drop shadow, the panel's ghosting, and the
                  integer scale that only exists to keep the grid's cells equal:
                  everything that simulates the physical LCD. On a real CRT that
                  is not a simulation of anything, it is just damage.

  The same division runs through the scaler half - gamma is colour, the grid
  filter and the mask are structure - which is why every look with a core_struct
  gets a second preset file with the structure dropped. See look_is_panel() and
  vp_preset_path_for().

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
	const char *core_colour;  // 0 = nothing, else "Name=Value;Name=Value". Any output.
	const char *core_struct;  // the LCD's own structure. Never on an analog display.
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

  It is therefore part of the STRUCTURE half and not the colour one: its whole
  justification is the grid, and where there is no grid - an analog display -
  there is no reason to overrule the player's own scaling.
*/
#define CO_INTEGER  "Scale=Narrower HV-Integer"
/*
  Super Game Boy=On rides every DMG-family look: Dinofly runs Zelda with the
  SGB border around the Pocket screen and asked for it as the default -
  confirmed live off the core (the border in the capture; the stale CFG still
  said Off). The custom palette wins the colours, the SGB canvas keeps the
  border, and a player who disagrees flips it in Core options, where the
  per-game record outranks the look.

  It stays with the colour half: an SGB border is a second machine's canvas, not
  a simulation of the Game Boy's panel, and Super Game Boy on a television is
  how that machine was actually played.
*/
#define CO_GB_DMG_COLOUR "Super Game Boy=On;Custom Palette=On"

/*
  Screen Shadow is the core's own drop shadow under each LCD pixel and Frame
  blend is the panel's slow response. Both simulate the physical thing, so both
  are structure - and Frame blend / Flickerblend with them, for all that a
  flicker-heavy Lynx game looks rough without it. A CRT running raw is what the
  cartridge really put out; the ghosting was the panel's doing, and the panel is
  not there.
*/
#define CO_GB_DMG_PANEL  "Screen Shadow=Yes;Frame blend=On;" CO_INTEGER

static const preset_def presets[] =
{
	{ "sharp", "Sharp", "No filtering. Square pixels, nothing added.",
	  "off", "off", "off", "off", "off", "off", 0, 0, 0 },

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
	  "Pure_Gamma/gamma_110.txt", 0, 0, 0 },

	/*
	  Short names on purpose: five tiles share a row on the console class since
	  BVM joined, and at 960x540 a longer label is cut off - the no-clipped-copy
	  gate is the arbiter. The blurb carries the longer story.
	*/
	{ "pvm-svideo", "S-Video", "Slight horizontal bleed, scanlines: a console on a good PVM over S-Video.",
	  F_SOFT, F_SHARP, F_SCAN, M_GRILLE, "1x", "off", 0, 0, 0 },

	/*
	  The distribution's own Sony PVM mask at 1x, on Dinofly's call, in place of our
	  generated grille at 2x: a doubled cell is two output pixels per phosphor, which
	  at 1080p is a coarser stripe than any tube ever had, and the 1x PVM mask is the
	  structure he wants under the blur. Same file the PVM look uses for its grille
	  family, so the two reads as one set.
	*/
	{ "composite", "Composite", "Soft and blurry, as an RF or composite hookup looked.",
	  F_BLURRY, F_SOFT, F_SCAN,
	  "Simple (Monochrome)/Sony PVM (Generic) (~1980).txt", "1x", "off", 0, 0, 0 },

	{ "pal-tv", "PAL TV", "Softer still with lighter scanlines. Home computers on a telly.",
	  F_SOFT, F_SOFT, F_SCANLT, M_GRILLE, "2x", "off", 0, 0, 0 },

	{ "vga", "VGA Monitor", "Clean and slightly smoothed. No scanlines: a 31 kHz monitor had none.",
	  F_SOFT, F_SOFT, "off", "off", "off", "off", 0, 0, 0 },

	/*
	  The Game Boy pair. Colour comes from a real .gbp through the core's own
	  palette slot - the scaler gamma that used to fake it tinted the core's
	  already-colourised picture. The grid is the polyphase filter (aligned to
	  core pixels by construction), the shadow is the core's own drop-shadow.
	*/
	{ "dmg", "Game Boy DMG", "Muted olive-green reflective LCD, pixel grid and shadow.",
	  F_GRIDSH, F_GRIDSH, "off", "off", "off", "off",
	  CO_GB_DMG_COLOUR, CO_GB_DMG_PANEL, PAL_DMG },

	{ "pocket", "Game Boy Pocket", "Neutral grey reflective LCD, finer grid, pixel shadow.",
	  F_GRIDSH, F_GRIDSH, "off", "off", "off", "off",
	  CO_GB_DMG_COLOUR, CO_GB_DMG_PANEL, PAL_POCKET },

	/*
	  Super Game Boy on a colour cartridge takes two options, not one: the core
	  keeps "Super Game Boy + GBC" separate precisely because a GBC game on an SGB
	  is a combination the hardware never shipped, and it will not infer one from
	  the other. Both are asked for by name, so a core too old to have the second
	  simply skips it and the first still lands - Dinofly asked for the border on
	  Game Boy first and on Color after seeing it.
	*/
	{ "gbc", "Game Boy Color", "Reflective colour LCD with its pixel grid.",
	  F_GRID, F_GRID, "off", "off", "off", "off",
	  "Super Game Boy=On;Super Game Boy + GBC=On;GBC Colors=Corrected",
	  "Screen Shadow=No;Frame blend=Off;" CO_INTEGER, 0 },

	/*
	  The three GBA screens map to the core's own "Modify Colors" profiles - the
	  Pokefan531 correction, authored per model, applied before scaling. AGS-101
	  is the backlit panel people mod their consoles towards: near-raw colour.
	*/
	{ "agb001", "GBA", "The original unlit screen. Dim and washed out.",
	  F_GRID, F_GRID, "off", "off", "off", "off",
	  "Modify Colors=GBA 2.2", CO_INTEGER, 0 },

	{ "ags001", "GBA SP", "Frontlit SP: brighter than AGB, still washed out.",
	  F_GRID, F_GRID, "off", "off", "off", "off",
	  "Modify Colors=GBA 1.6", CO_INTEGER, 0 },

	{ "ags101", "GBA SP Brighter", "Backlit SP: bright with proper contrast and colour.",
	  F_GRID, F_GRID, "off", "off", "off", "off",
	  "Modify Colors=Off", CO_INTEGER, 0 },

	/*
	  The rest of the handhelds, same split, measured against each core's own
	  CONF_STR on the device (2026-08-12): the SMS/Game Gear and NGP cores offer
	  no colour work at all, so their panels stay as scaler gamma LUTs - the one
	  case where the LUT is not fighting anybody. Lynx and WonderSwan publish
	  Flickerblend (Off / 2 Frames / 3 Frames): real temporal ghosting from the
	  core, which those smeary panels had in spades, so their looks turn it on.
	  All of them swap the misaligned Dot Matrix mask for the grid filter and
	  pin integer scale like the GB/GBA looks do.

	  None of them carries a core_colour, and that is what decides their fate on
	  an analog display: their whole colour rendition is a scaler gamma LUT, so
	  they are worth offering there only while the scaler output still reaches
	  the screen (vga_scaler), and worth nothing at all on direct_video or an
	  analog takeover. look_shows_on_analog() is where that is decided, and the
	  Display screen simply does not list them where they would be inert.
	*/
	{ "gg", "Game Gear", "Backlit but murky, with the Game Gear's poor contrast.",
	  F_GRID, F_GRID, "off", "off", "off", G_GG, 0, CO_INTEGER, 0 },

	{ "gg-mod", "Game Gear (Backlit Mod)", "The common LED backlight mod: brighter, cleaner whites.",
	  F_GRID, F_GRID, "off", "off", "off", G_GGMOD, 0, CO_INTEGER, 0 },

	{ "lynx", "Atari Lynx", "Backlit colour LCD with a cool cast and washed blacks.",
	  F_GRID, F_GRID, "off", "off", "off", G_LYNX,
	  0, "Flickerblend=2 Frames;" CO_INTEGER, 0 },

	{ "ws", "WonderSwan", "Reflective mono FSTN: warm grey, low contrast.",
	  F_GRID, F_GRID, "off", "off", "off", G_WS,
	  0, "Flickerblend=2 Frames;" CO_INTEGER, 0 },

	{ "wsc", "WonderSwan Color", "Reflective colour panel: muted and slightly warm.",
	  F_GRID, F_GRID, "off", "off", "off", G_WSC,
	  0, "Flickerblend=2 Frames;" CO_INTEGER, 0 },

	{ "ngpc", "Neo Geo Pocket Color", "Reflective pastel colour LCD, gentle contrast.",
	  F_GRID, F_GRID, "off", "off", "off", G_NGPC, 0, CO_INTEGER, 0 },

	/*
	  One switch to turn every layer of processing off: scaler filters, mask and
	  gamma explicitly off, and no core half at all - which means "restore what
	  the look before me changed" when picked live, and "touch nothing" at
	  launch, where the core has just booted from the player's own config (see
	  vp_apply_core_side). Appended at the END of the table - the stored choice
	  in classicui_video.cfg is a raw preset index, so table order is ABI.
	*/
	{ "none", "None", "Every effect off: the core's own picture, nothing added.",
	  "off", "off", "off", "off", "off", "off", 0, 0, 0 },

	// Appended after None for the same ABI reason None sits where it does.
	{ "bvm-rgb", "BVM RGB", "Reference broadcast monitor: razor sharp, deep scanlines.",
	  F_SHARP, F_SHARP, F_SCANDP, M_GRILLE, "1x", "off", 0, 0, 0 },
};

#define NPRESETS ((int)(sizeof(presets) / sizeof(presets[0])))

/* ------------------------------------------------- the scaler, on the ARM ---

  The FPGA scaler's own arithmetic, run over a captured frame so the front-end
  can SHOW what the television is showing.

  Everything here was read out of the hardware rather than guessed, which is the
  only reason it is allowed to claim accuracy:

  - The coefficient files are the ones the scaler is actually loaded with. They
    carry integers at 128 scale; read_video_filter() in video.cpp multiplies by
    two (by one after a "10bit" header) into the 256-scale words the fabric
    stores, and a 64-phase file is DUPLICATED up to the 256 internal phases
    rather than interpolated (scale_phases). Both rules are reproduced below,
    so a filter we generate and a filter somebody else wrote are treated alike.

  - The multiply-accumulate is ascal.vhd's: poly_cvt shifts each coefficient
    left seven into 3.15, each tap multiplies a nine-bit zero-extended pixel,
    the taps sum IN PAIRS, each pair is truncated by eight bits (poly_final
    takes bits 26:8 - a floor, not a round), the two halves add, and bound()
    clamps - negative to zero, anything at or past bit 15 to 255, else bits
    14:7. The two separate truncations are why this cannot be folded into one
    shift, and folding them was the first thing that made a preview wrong.

  - The axis order is the fabric's: horizontal first, into line buffers the
    vertical stage then reads.

  What is deliberately NOT modelled: gamma (a LUT the scaler applies after
  this), and the adaptive filters, whose second coefficient set is chosen per
  pixel by luminance. A look wearing either is drawn without it and is honest
  about being a picture of the filters alone - see vp_render_exact()'s return.
*/

#define VP_HW_PHASES 256

struct vp_taps
{
	int t[VP_HW_PHASES][4];
	int ok;
};

static int vp_load_taps(const char *name, vp_taps *out)
{
	memset(out, 0, sizeof(*out));
	if (!name || !*name || !strcasecmp(name, "off")) return 0;

	char path[1024];
	snprintf(path, sizeof(path), "%s/filters/%s", getRootDir(), name);

	FILE *f = fopen(path, "rt");
	if (!f) return 0;

	int raw[VP_HW_PHASES][4];
	int n = 0, scale = 2, adaptive = 0;
	char line[256];

	while (fgets(line, sizeof(line), f))
	{
		char *p = line;
		while (*p == ' ' || *p == '\t') p++;

		if (!n && !strncasecmp(p, "10bit", 5)) { scale = 1; continue; }
		if (!n && !strncasecmp(p, "adaptive", 8)) { adaptive = 1; continue; }

		int a, b, c, d;
		if (sscanf(p, "%d,%d,%d,%d", &a, &b, &c, &d) != 4) continue;
		if (n >= VP_HW_PHASES) break;

		raw[n][0] = a * scale; raw[n][1] = b * scale;
		raw[n][2] = c * scale; raw[n][3] = d * scale;
		n++;
	}
	fclose(f);

	// An adaptive file's first half is the ordinary set; the second is chosen by
	// luminance, which this does not model. Half of it is still the right picture
	// of the sharpness, so it is used and the caller is told the look is partial.
	if (adaptive) n /= 2;
	if (n == 32) n = 16;                       // the legacy 16-phase pair form
	if (n < 1) return 0;

	int dup = VP_HW_PHASES / n;
	if (dup * n != VP_HW_PHASES) return 0;     // not a phase count the fabric can hold

	for (int i = 0; i < n; i++)
		for (int k = 0; k < dup; k++)
			memcpy(out->t[i * dup + k], raw[i], sizeof(raw[i]));

	out->ok = 1;
	return 1;
}

static inline int vp_bound(int v)
{
	if (v < 0) return 0;
	if (v >= (1 << 15)) return 255;
	return (v >> 7) & 0xff;
}


/*
  The shadow mask, as sys/shadowmask.sv applies it.

  A cell's word gives each channel a 1.4 fixed-point multiplier - bright cells
  take {1,lut[7:4]}, dim cells {0,lut[3:0]} - and the fabric applies it as a
  truncating shift-add, channel>>4 up to channel>>0, one term per set bit. The
  simple three-bit masks (one bit per channel) are the same thing with the
  multiplier fixed at 1.125 for a set bit and 0.625 for a clear one - video.cpp
  ORs a constant 0x2A into the low byte for those, so the file never gets to say.
  It is a gentle modulation and NOT an on/off switch; assuming the latter made
  three of the CRT looks preview far darker than the hardware draws them.

  Only the first table in a file is read. Files that carry several under
  "Resolution=" lines are choosing a cell size for the OUTPUT height, and the
  preview and the background are drawn at canvas sizes rather than at the
  scaler's, so picking by resolution here would be answering a question nobody
  asked. The first table is the one the file leads with.
*/
#define VP_MASK_MAX 32

struct vp_mask
{
	uint8_t mul[VP_MASK_MAX][VP_MASK_MAX][3];
	int w, h, ok;
};

static int vp_load_mask(const char *name, vp_mask *out)
{
	memset(out, 0, sizeof(*out));
	if (!name || !*name || !strcasecmp(name, "off")) return 0;

	char path[1024];
	snprintf(path, sizeof(path), "%s/shadow_masks/%s", getRootDir(), name);

	FILE *f = fopen(path, "rt");
	if (!f) return 0;

	char line[512];
	int rows = 0, cols = 0, want = 0;

	while (fgets(line, sizeof(line), f))
	{
		char *p = line;
		while (*p == ' ' || *p == '\t') p++;
		char *hash = strpbrk(p, "#;\r\n");
		if (hash) *hash = 0;
		if (!*p) continue;

		if (!strncasecmp(p, "resolution=", 11)) { if (want) break; continue; }
		if (!strcasecmp(p, "v2")) continue;

		int a, b;
		if (!want && sscanf(p, "%d,%d", &a, &b) == 2 && a > 0 && b > 0)
		{
			cols = (a > VP_MASK_MAX) ? VP_MASK_MAX : a;
			want = (b > VP_MASK_MAX) ? VP_MASK_MAX : b;
			continue;
		}
		if (!want) continue;

		int c = 0;
		for (char *tok = strtok(p, ","); tok && c < cols; tok = strtok(0, ","))
		{
			unsigned v = 0;
			if (sscanf(tok, "%x", &v) != 1) continue;

			for (int ch = 0; ch < 3; ch++)
			{
				int bit = 10 - ch;                       // r,g,b = bits 10,9,8
				int m;
				/*
				  A v1 cell does NOT switch a channel fully on and off, which is what
				  this assumed and what the note above still said. video.cpp's own
				  loader builds the word as ((p & 7) << 8) | 0x2A, so the low byte is
				  FIXED at 0x2A whatever the file says: lut[7:4] = 2 and lut[3:0] = A.
				  A set bit is therefore {1,2} = 1.125 and a clear bit is {0,A} =
				  0.625 - a gentle modulation, not a switch.

				  Measured on hardware 2026-08-19: our generated grille on a white
				  screen comes out 170/229/187 in studio luma. 1.125/0.625 predicts
				  that shape; 1.0/0.0 predicts 63/173/32, which is nothing like it.
				  See docs/SCALER-MODEL-2026-08-19.md.
				*/
				if (v <= 7) m = ((v >> (2 - ch)) & 1) ? 0x12 : 0x0A;
				else m = ((v >> bit) & 1) ? (0x10 | ((v >> 4) & 0xF)) : (int)(v & 0xF);
				out->mul[rows][c][ch] = (uint8_t)m;
			}
			c++;
		}
		if (c) rows++;
		if (rows >= want) break;
	}
	fclose(f);

	if (!rows || !cols) return 0;
	out->w = cols;
	out->h = rows;
	out->ok = 1;
	return 1;
}

static inline int vp_mask_mul(int ch, int mul)
{
	int s = 0;
	for (int k = 0; k < 5; k++) if ((mul >> k) & 1) s += ch >> (4 - k);
	return (s > 255) ? 255 : s;
}


/*
  A WINDOW of the full-size render, computed without rendering the rest.

  The preview needs a 1:1 crop of the picture as the television draws it - filter
  first at the television's magnification, zoom afterwards - and rendering a whole
  960x640 frame per tile only to throw away nine tenths of it would cost six
  full-canvas passes every time the cursor moves. A polyphase is separable and each
  output pixel reads four source pixels, so a window can be computed on its own:
  only the source rows the window reaches go through the horizontal pass.

  Written to be fast enough to sit in a menu open, which the first version was not:
  the per-pixel work is a table lookup and twelve multiplies, with the sampling plan
  for each axis computed ONCE rather than per pixel (a floor and a divide per pixel
  per row is most of a full-canvas pass), and the vertical stage reading the
  horizontal stage's rows directly instead of transposing the frame twice.

  x0,y0,w,h are in the coordinates of the virtual dw x dh output.
*/
struct vp_plan { int s[4]; const int *c; };

static void vp_plan_axis(int sw, int dw, int x0, int n, const vp_taps *taps,
	int soff, vp_plan *out)
{
	for (int x = 0; x < n; x++)
	{
		double u = ((double)(x0 + x) + 0.5) * sw / dw - 0.5;
		int i = (int)floor(u);
		int ph = (int)((u - i) * VP_HW_PHASES);
		if (ph < 0) ph = 0;
		if (ph >= VP_HW_PHASES) ph = VP_HW_PHASES - 1;

		for (int t = 0; t < 4; t++)
		{
			int si = i - 1 + t - soff;
			if (si < 0) si = 0;
			if (si >= sw - soff) si = sw - soff - 1;
			out[x].s[t] = si;
		}
		out[x].c = taps->t[ph];
	}
}

static inline uint32_t vp_tap4(const uint32_t p[4], const int *c)
{
	uint32_t out = 0xff000000u;
	for (int sh8 = 16; sh8 >= 0; sh8 -= 8)
	{
		int a = (c[0] << 7) * (int)((p[0] >> sh8) & 0xff)
		      + (c[1] << 7) * (int)((p[1] >> sh8) & 0xff);
		int b = (c[2] << 7) * (int)((p[2] >> sh8) & 0xff)
		      + (c[3] << 7) * (int)((p[3] >> sh8) & 0xff);
		out |= (uint32_t)vp_bound((a >> 8) + (b >> 8)) << sh8;
	}
	return out;
}

// The mask rides absolute output pixels, so a window has to say where it starts.
static void vp_apply_mask_at(uint32_t *px, int w, int h, int x0, int y0,
	const vp_mask *m, int twox)
{
	int step = twox ? 2 : 1;

	for (int y = 0; y < h; y++)
	{
		for (int x = 0; x < w; x++)
		{
			const uint8_t *mul = m->mul[((y0 + y) / step) % m->h][((x0 + x) / step) % m->w];
			uint32_t c = px[(size_t)y * w + x];
			uint32_t o = 0xff000000u;
			for (int ch = 0; ch < 3; ch++)
			{
				int sh8 = 16 - ch * 8;
				o |= (uint32_t)vp_mask_mul((int)((c >> sh8) & 0xff), mul[ch]) << sh8;
			}
			px[(size_t)y * w + x] = o;
		}
		if (!(y & 31)) user_io_core_alive_poll();
	}
}

int vp_render_exact_rect(int look, const uint32_t *src, int sw, int sh,
	int dw, int dh, int x0, int y0, int w, int h, uint32_t *dst)
{
	if (look < 0 || look >= NPRESETS) return 0;
	if (!src || !dst || sw < 1 || sh < 1 || dw < 1 || dh < 1 || w < 1 || h < 1) return 0;

	const preset_def *d = &presets[look];

	vp_taps hf, vf;
	int have_h = vp_load_taps(d->hfilter, &hf);
	int have_v = vp_load_taps(d->vfilter, &vf);

	vp_mask mask;
	int have_mask = vp_load_mask(d->mask, &mask);

	if (!have_h && !have_v && !have_mask) return 0;

	// What an axis with no filter of its own gets, which is what Sharp asks for.
	vp_taps nearest;
	memset(&nearest, 0, sizeof(nearest));
	for (int p = 0; p < VP_HW_PHASES; p++)
	{
		if (p < VP_HW_PHASES / 2) nearest.t[p][1] = 256;
		else nearest.t[p][2] = 256;
	}
	nearest.ok = 1;

	// The source rows this window reaches: the taps around its first and last rows.
	double u0 = ((double)y0 + 0.5) * sh / dh - 0.5;
	double u1 = ((double)(y0 + h - 1) + 0.5) * sh / dh - 0.5;
	int r0 = (int)floor(u0) - 1, r1 = (int)floor(u1) + 2;
	if (r0 < 0) r0 = 0;
	if (r1 > sh - 1) r1 = sh - 1;
	int rows = r1 - r0 + 1;

	vp_plan *hp = (vp_plan*)malloc(sizeof(vp_plan) * w);
	vp_plan *vp = (vp_plan*)malloc(sizeof(vp_plan) * h);
	uint32_t *hbuf = (uint32_t*)malloc((size_t)w * rows * 4);
	if (!hp || !vp || !hbuf) { free(hp); free(vp); free(hbuf); return 0; }

	vp_plan_axis(sw, dw, x0, w, have_h ? &hf : &nearest, 0, hp);
	vp_plan_axis(sh, dh, y0, h, have_v ? &vf : &nearest, r0, vp);

	for (int y = 0; y < rows; y++)
	{
		const uint32_t *srow = src + (size_t)(r0 + y) * sw;
		uint32_t *drow = hbuf + (size_t)y * w;
		for (int x = 0; x < w; x++)
		{
			const vp_plan *pl = &hp[x];
			uint32_t px[4] = { srow[pl->s[0]], srow[pl->s[1]], srow[pl->s[2]], srow[pl->s[3]] };
			drow[x] = vp_tap4(px, pl->c);
		}
		if (!(y & 31)) user_io_core_alive_poll();
	}

	for (int y = 0; y < h; y++)
	{
		const vp_plan *pl = &vp[y];
		const uint32_t *r[4];
		for (int t = 0; t < 4; t++)
		{
			int idx = pl->s[t];
			if (idx < 0) idx = 0;
			if (idx > rows - 1) idx = rows - 1;
			r[t] = hbuf + (size_t)idx * w;
		}

		uint32_t *drow = dst + (size_t)y * w;
		for (int x = 0; x < w; x++)
		{
			uint32_t px[4] = { r[0][x], r[1][x], r[2][x], r[3][x] };
			drow[x] = vp_tap4(px, pl->c);
		}
		if (!(y & 31)) user_io_core_alive_poll();
	}

	if (have_mask)
		vp_apply_mask_at(dst, w, h, x0, y0, &mask,
			d->maskmode && !strcasecmp(d->maskmode, "2x"));

	free(hp); free(vp); free(hbuf);
	return 1;
}

int vp_render_exact(int look, const uint32_t *src, int sw, int sh,
	uint32_t *dst, int dw, int dh)
{
	return vp_render_exact_rect(look, src, sw, sh, dw, dh, 0, 0, dw, dh, dst);
}

int vp_count() { return NPRESETS; }
const char *vp_name(int i) { return (i >= 0 && i < NPRESETS) ? presets[i].name : "?"; }
const char *vp_blurb(int i) { return (i >= 0 && i < NPRESETS) ? presets[i].blurb : ""; }

/* --------------------------------------------- which output are we on? ---- */

/*
  Is the picture on an analog display, and only there?

  The test is video_hdmi_connected() == 0: no HDMI sink attached. Nothing else is
  needed, and nothing else would be honest. All three analog routings put the
  picture on the VGA port - direct_video straight off the core's timing,
  vga_scaler permanently, and video_menu_fb_analog()'s takeover for as long as
  the front-end holds the screen - and none of them can be told apart from the
  ini alone: a machine with vga_scaler=1 AND an HDMI display is an ordinary HDMI
  machine. What settles it is whether there is an HDMI sink to be the display,
  and if there is neither an HDMI sink nor an analog one then nobody is looking
  at anything and this answer costs nothing.

  Unknown (-1, i2c not up yet) counts as attached, the same cautious answer
  video_scaler_is_visible() and vp_analog_facts() already give it: it is the
  reading taken before the machine is fully awake, and guessing "analog" there
  would drop the grid on an HDMI player for the first frames after boot.

  Deliberately NOT video_scaler_is_visible(). That answers "does the scaler
  output reach a screen", which is a different question: with vga_scaler=1 the
  scaler is very much in the path and its output is going down a VGA cable to a
  CRT - the one case where the grid filter would be drawn onto a real tube.

  Not cached. The i2c byte behind it is the same one the Display gate already
  reads every frame, and the two callers that are on a per-frame path (the menu
  bar, for a handheld class only, and vp_output_poll() only while a look owns
  core options) both check something cheaper first.
*/
int vp_output_is_analog()
{
	return (video_hdmi_connected() == 0) ? 1 : 0;
}

/*
  A panel look: one that simulates the physical LCD, and therefore the only kind
  an analog display has to be protected from. Derived from core_struct rather
  than from a flag of its own so the two cannot drift apart - a look that
  simulates a panel has panel options, and the CRT looks have none.
*/
static int look_is_panel(const preset_def *d)
{
	return d->core_struct ? 1 : 0;
}

/*
  Would this look show the player anything on an analog display?

  Only colour survives there, and a look's colour comes from one of two places.
  The core's own (a palette upload, GBC Colors, Modify Colors) reaches any output
  because it happens before the picture leaves the core. A scaler gamma LUT
  reaches the screen only while the scaler output does - so the Game Gear,
  Lynx, WonderSwan and NGPC looks, whose entire colour work is a LUT, are real on
  a vga_scaler CRT and inert on direct_video or the takeover.

  Non-panel looks (the CRT presets, Sharp, None) are not filtered at all: they
  are unchanged by this whole mechanism, and offering someone a look with no
  processing in it is never wasted.
*/
static int look_shows_on_analog(int i)
{
	const preset_def *d = &presets[i];

	if (!look_is_panel(d)) return 1;
	if (d->core_colour || d->palette) return 1;
	if (d->gamma && strcasecmp(d->gamma, "off") && video_scaler_is_visible()) return 1;

	return 0;
}

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

/*
  On an analog display the list is shortened to the looks that still do something
  there - see look_shows_on_analog(). Silently listing a look that cannot change
  a single pixel is the worst of the three options: the player picks it, nothing
  happens, and the front-end looks broken rather than honest.

  The list shrinking is also what tells the menu bar whether the Display entry is
  worth putting up at all: a class left with nothing but its off switch has no
  choice to offer. Everything BELOW this - vp_default_for(), allowed_in(),
  vp_effective() and the stored choice in classicui_video.cfg - walks the
  unfiltered list on purpose, so plugging an HDMI cable in never rewrites what
  the player chose.
*/
int vp_options_for(int vclass, int *out)
{
	opt_set o = options_of(vclass);
	int analog = vp_output_is_analog();

	int n = 0;
	for (int i = 0; i < o.n && n < VP_MAX_OPTIONS; i++)
	{
		if (analog && !look_shows_on_analog(o.list[i])) continue;
		if (out) out[n] = o.list[i];
		n++;
	}
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
  How many output pixels the scaler is currently giving each source pixel.

  The grid has to know. A polyphase filter is sampled only at the phases the
  current scale visits - N of them, evenly spaced - and a gutter narrower than
  that spacing can fall clean between two of them and darken NOTHING. That is not
  a theory: at 720p the Game Boy gets 5x and its grid is visible, the Game Boy
  Advance gets exactly 4x and Dinofly reported no grid at all. The phases 4x
  visits are 32, 96, 160 and 224 of 256, and a gutter centred on 128 spanning
  100..156 misses every one of them. 2x and 3x miss it too.

  Read from the scaler's own header rather than computed from the ini, so it is
  the magnification actually in force, whatever the mode, the aspect and the
  integer-scaling setting between them worked out to. 0 when there is nothing
  running to ask about, which is the harness and the menu.
*/
/*
  The rectangle the scaler is putting the GAME in, in panel pixels.

  Not the same as fitting the frame to the canvas, which is what the menu
  background used to do: with integer scaling a 256x224 core lands in 1170x896 on
  a 1080p panel, while an aspect fit of the same frame is 1440x1080. Dinofly saw
  exactly that - "the background image of mario is bigger than what the nes core
  rendered" - and the scanline filter, rendered over the wrong height, put its
  lines at a spacing the game never had.

  Read from the scaler's own header, so it is the geometry actually on the
  television rather than one computed from the ini and hoped for. 0 when nothing
  is running to ask.
*/
/*
  What the scaler last said about the GAME, read once a second and remembered.

  Two reasons it is a latch rather than a question asked when the answer is
  wanted. The first is cost: mister_scaler_init() opens /dev/mem, maps it, reads
  six words and unmaps - and, until it was asked not to, printed two lines of
  diagnostics while it was at it. Asked per frame that was sixty maps and a
  hundred and twenty SD-card writes a second, which Dinofly saw as the whole
  screen wobbling. Quiet now, but sixty maps a second is still sixty too many.

  The second is that by the time the answer is wanted, it is no longer available.
  Opening the in-game menu hands the screen to our framebuffer, and from that
  moment the scaler is describing US - 1920x1080 - not the game. The background
  built right after asked, believed the answer, and drew the still full-screen
  over a game that was really in 1170x896. So the read stands down whenever our
  framebuffer owns the output, and what is kept is the last thing the scaler said
  while the game still had the screen.
*/
static int vp_out_w = 0, vp_out_h = 0, vp_src_h = 0;

void vp_output_watch()
{
	static unsigned long next = 0;
	if (next && !CheckTimer(next)) return;
	next = GetTimer(1000);

	if (video_fb_state()) return;          // our framebuffer, not the game's picture

	/*
	  Forgotten when there is nothing to read, rather than kept. A core that has not
	  put a picture up yet answers the same as no core at all, and holding the last
	  answer would draw the next game's background at the last game's size - the
	  fallback fit is the honest thing to do when the geometry is not known.
	*/
	mister_scaler_quiet(1);
	mister_scaler *ms = mister_scaler_init();
	mister_scaler_quiet(0);
	if (!ms)
	{
		vp_out_w = vp_out_h = vp_src_h = 0;
		return;
	}

	vp_out_w = ms->output_width;
	vp_out_h = ms->output_height;
	vp_src_h = ms->height;
	mister_scaler_free(ms);
}

int vp_output_rect(int *w, int *h)
{
	if (vp_out_w < 16 || vp_out_h < 16) return 0;
	if (w) *w = vp_out_w;
	if (h) *h = vp_out_h;
	return 1;
}

int vp_game_height()
{
	return (vp_src_h > 0) ? vp_src_h : 0;
}

static int vp_output_scale()
{
	int n = (vp_src_h > 0) ? (vp_out_h / vp_src_h) : 0;
	return (n >= 2 && n <= 16) ? n : 0;
}

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

static void write_filter_grid(const char *name, int shadow, int scale)
{
	genbuf g;
	gb_reset(&g);

	/*
	  With the scale known, the gutter is put where a sample will actually land:
	  one output pixel per source pixel, dark, and the rest of the cell boosted to
	  hold the average - which is what an LCD grid is. The window is centred on the
	  visited phase nearest the cell boundary and made a little narrower than the
	  spacing between phases, so exactly one of them is inside it.

	  Without a scale (no core running, or a mode nobody can read) it falls back to
	  the fixed 22% gutter, which is right at 5x and above and is what shipped
	  before. See vp_output_scale().
	*/
	double centre = 0.5, half = GRID_GUTTER / 2;
	double duty = GRID_GUTTER;

	if (scale >= 2)
	{
		double step = 1.0 / scale;
		double best = 0, bestd = 2;
		for (int x = 0; x < scale; x++)
		{
			double u = ((double)x + 0.5) / scale - 0.5;
			double frac = u - floor(u);
			double d = fabs(frac - 0.5);
			if (d < bestd) { bestd = d; best = frac; }
		}
		centre = best;
		half = (step * 0.8) / 2;
		duty = step;                       // one output pixel in every `scale`
	}

	gb_addf(&g, "# %s\n", GEN_MARK);
	gb_addf(&g, "# LCD grid for %dx: one dark pixel per source pixel at %.0f%% depth%s\n\n",
		scale ? scale : 0, GRID_DEPTH * 100, shadow ? ", with the pixel shadow" : "");

	double soft = 1.0 / PHASES;                     // one-phase shoulders
	double boost = 1.0 / (1.0 - GRID_DEPTH * duty);
	if (boost > 1.30) boost = 1.30;

	for (int p = 0; p < PHASES; p++)
	{
		double x = (double)p / PHASES;
		// Distance to the gutter's centre, the short way round the cell.
		double d = fabs(x - centre);
		if (d > 0.5) d = 1.0 - d;

		double e = 0;                               // gutter envelope
		if (d < half) e = 1.0;
		else if (d < half + soft) e = 1.0 - (d - half) / soft;

		double w[4] = { 0, 0, 0, 0 };
		if (x < 0.5) w[1] = 1.0;
		else w[2] = 1.0;

		double gain = boost * (1.0 - GRID_DEPTH * e);

		/*
		  The cell body runs from the gutter's far shoulder to the next gutter,
		  i.e. source pixel [2]'s territory: phases past 0.5+half. The shadow
		  band is its first SHADOW_WIDTH, fading linearly. Mixing toward tap [1]
		  keeps the row sum constant, so only the gain dim changes brightness.
		*/
		if (shadow && x > 0.5)
		{
			double into = (x - 0.5) / 0.5;
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

/*
  Where a look's preset file lives, and its analog twin.

  A panel look gets a second file with the structure lines forced off and the
  gamma - the colour - kept: the same split the core half gets, applied to the
  scaler. It is a real file rather than a runtime edit because video_loadPreset()
  takes a path, and a file also means a player can read what we did to their
  scaler.

  It is loaded INSTEAD of the full preset on an analog display, never as well as,
  so a session that starts on HDMI and ends on a CRT clears the grid out of the
  scaler rather than leaving it there unseen. Only panel looks get one; a CRT
  look on a CRT is the player's own arrangement and none of our business.
*/
static void preset_rel(const preset_def *d, int crt, char *out, int len)
{
	snprintf(out, len, "presets/%s %s%s.ini", PREFIX, d->name, crt ? " (CRT)" : "");
}

static void write_preset(const preset_def *d, int crt)
{
	genbuf g;
	gb_reset(&g);

	if (crt) gb_addf(&g, "# %s on an analog display - colour only, no panel structure\n", d->name);
	else     gb_addf(&g, "# %s - %s\n", d->name, d->blurb);
	gb_addf(&g, "# %s\n", GEN_MARK);

	const char *off = "off";
	if (d->hfilter)  gb_addf(&g, "hfilter=%s\n", crt ? off : d->hfilter);
	if (d->vfilter)  gb_addf(&g, "vfilter=%s\n", crt ? off : d->vfilter);
	if (d->sfilter)  gb_addf(&g, "sfilter=%s\n", crt ? off : d->sfilter);
	if (d->mask)     gb_addf(&g, "mask=%s\n", crt ? off : d->mask);
	if (d->maskmode) gb_addf(&g, "maskmode=%s\n", crt ? off : d->maskmode);
	if (d->gamma)    gb_addf(&g, "gamma=%s\n", d->gamma);

	char rel[1024];
	preset_rel(d, crt, rel, sizeof(rel));
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
	write_filter_grid(F_GRID, 0, vp_output_scale());
	write_filter_grid(F_GRIDSH, 1, vp_output_scale());

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

	for (int i = 0; i < NPRESETS; i++)
	{
		write_preset(&presets[i], 0);
		if (look_is_panel(&presets[i])) write_preset(&presets[i], 1);
	}
}

int vp_available(int i)
{
	if (i < 0 || i >= NPRESETS) return 0;
	const preset_def *d = &presets[i];

	// On an analog display a panel look is the CRT twin, which needs neither the
	// grid filter nor a mask - so asking for them would refuse a look that is
	// perfectly usable there.
	int crt = vp_output_is_analog() && look_is_panel(d);

	char rel[1024];
	preset_rel(d, crt, rel, sizeof(rel));
	if (!exists_rel(rel)) return 0;

	const char *files[3] = { crt ? 0 : d->hfilter, crt ? 0 : d->sfilter, 0 };
	for (int k = 0; k < 2; k++)
	{
		if (!files[k] || !strcasecmp(files[k], "off")) continue;
		snprintf(rel, sizeof(rel), "filters/%s", files[k]);
		if (!exists_rel(rel)) return 0;
	}

	if (!crt && d->mask && strcasecmp(d->mask, "off"))
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
  The preset to load for the output actually in use: the CRT twin for a panel look
  on an analog display, the look's own file otherwise.

  A missing twin refuses rather than falling back to the full preset. The fallback
  is the one thing that must not happen - it would put the LCD grid on the tube,
  which is the whole fault being fixed here - and vp_install() writes the twin on
  every boot, so the only way to be here is a card somebody has taken files off.
*/
static int vp_preset_path_for(int i, int analog, char *out, int len)
{
	if (i < 0 || i >= NPRESETS) return 0;

	int crt = analog && look_is_panel(&presets[i]);

	char rel[1024];
	preset_rel(&presets[i], crt, rel, sizeof(rel));
	if (crt && !exists_rel(rel))
	{
		printf("ClassicUI: no analog preset for \"%s\" - leaving the scaler alone\n", presets[i].name);
		return 0;
	}

	snprintf(out, len, "%s/%s", getRootDir(), rel);
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
  ...and which output it was applied FOR, so vp_output_poll() can tell that the
  answer has gone stale. -1 is "no look owns anything", which is also what it
  reads as before the first apply.
*/
static int vp_applied_analog = -1;

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
	vp_applied_analog = -1;
}

// One ;-separated half of a look, by name against the running core.
static void vp_apply_opt_list(const preset_def *d, const char *opts)
{
	if (!opts) return;

	char list[512];
	snprintf(list, sizeof(list), "%s", opts);

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

static void vp_apply_core_side(int i, int with_palette)
{
	if (i < 0 || i >= NPRESETS) return;
	const preset_def *d = &presets[i];

	if (!d->core_colour && !d->core_struct && !d->palette)
	{
		// A look with no core half still undoes the one it replaces.
		vp_restore_originals();
		vp_running_look = -1;
		vp_applied_analog = -1;
		return;
	}

	if (core_opts_scan() <= 0) return;

	int analog = vp_output_is_analog();
	vp_running_look = i;
	vp_applied_analog = analog;

	/*
	  Put everything back first, then set what this look wants on THIS output.

	  Two things need that. A panel half applied on HDMI has to come off when the
	  same session moves to a CRT, and there is no other record of what it set;
	  and a look that follows another look only ever set the options the two have
	  in common, so switching DMG (Super Game Boy=On) to the GBC screen used to
	  leave the SGB border up. Restoring is a no-op when nothing was recorded,
	  which is every launch - the core has just booted from the player's config.
	*/
	vp_restore_originals();

	vp_apply_opt_list(d, d->core_colour);

	/*
	  The panel half, and only where there is a panel to simulate. On an analog
	  display the pixel grid, the drop shadow and the ghosting are not a Game Boy
	  screen, they are a defect - and the integer scale that exists to keep the
	  grid honest has nothing left to be honest about.
	*/
	if (!analog) vp_apply_opt_list(d, d->core_struct);
	else if (d->core_struct)
		printf("ClassicUI: look \"%s\": analog display, panel effects left off\n", d->name);

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
	//
	// The path here is the look's own. Which of the two files really gets loaded
	// is decided in vp_apply_pending(), on the far side of the core load, because
	// that is where the answer to "which output" is worth having: this runs on the
	// shelf and the core load re-execs the firmware in between.
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
/*
  The grid is rebuilt for the magnification in force before any look that uses
  one is loaded. Cheap: gen_commit() only writes when the bytes change, so this
  is a no-op on every apply after the first at a given scale, and the file is
  shared by every look that names it.
*/
int vp_grid_for_now(int force)
{
	/*
	  Asked at most once a second, and that rate limit is the whole point of this
	  guard rather than tidiness.

	  The scale comes from vp_output_watch(), which reads the scaler header at most
	  once a second for the reasons given there; this keeps its own limit on top so
	  that the comparison and the two file writes below are not attempted per frame
	  either. The scale only changes when a core or a video mode does, so once a
	  second is already far more often than the question can have a new answer.
	*/
	static unsigned long next_look = 0;
	if (!force && next_look && !CheckTimer(next_look)) return 0;
	next_look = GetTimer(1000);

	vp_output_watch();
	int n = vp_output_scale();
	if (n < 2) return 0;

	static int last = 0;
	if (n == last) return 0;
	last = n;

	printf("ClassicUI: the scaler is giving each pixel %dx, rebuilding the LCD grid\n", n);
	write_filter_grid(F_GRID, 0, n);
	write_filter_grid(F_GRIDSH, 1, n);
	return 1;
}

// Whether this look's picture depends on the grid, and therefore on the scale.
static int vp_uses_grid(int i)
{
	if (i < 0 || i >= NPRESETS) return 0;

	const preset_def *d = &presets[i];
	const char *f[2] = { d->hfilter, d->vfilter };
	for (int k = 0; k < 2; k++)
		if (f[k] && (!strcasecmp(f[k], F_GRID) || !strcasecmp(f[k], F_GRIDSH))) return 1;

	return 0;
}

int vp_apply_now(int sysidx, int vclass_hint)
{
	int i = vp_effective(sysidx, vclass_hint);
	if (i < 0 || i >= NPRESETS) return 0;

	vp_grid_for_now();

	char path[1024];
	int analog = vp_output_is_analog();

	printf("ClassicUI: applying video look \"%s\" to the running core%s\n",
		presets[i].name, analog ? " (analog display)" : "");

	if (vp_preset_path_for(i, analog, path, sizeof(path))) video_loadPreset(path, true);
	vp_apply_core_side(i, 1);
	return 1;
}

void vp_apply_pending()
{
	FILE *f = fopen(PENDING, "rt");
	if (!f) return;

	vp_grid_for_now();

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

	/*
	  The look is named, so the path is recomputed for the output this machine is
	  actually on rather than trusting the one the shelf wrote. On an analog display
	  that is the difference between the scaler getting the LCD grid and getting the
	  colour alone; a file from an older build carries no name and is loaded as it
	  stands, which is the behaviour it was written for.
	*/
	if (look[0])
	{
		for (int i = 0; i < NPRESETS; i++)
		{
			if (strcmp(presets[i].id, look)) continue;
			if (!vp_preset_path_for(i, vp_output_is_analog(), path, sizeof(path))) path[0] = 0;
			break;
		}
		if (!path[0]) return;
	}

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
			if (!strcmp(presets[i].id, look) &&
			    (presets[i].core_colour || presets[i].core_struct || presets[i].palette))
			{
				vp_running_look = i;
				// Parked for the output seen here, so vp_output_poll() does not
				// mistake "not applied yet" for "the output changed" and play the
				// core half early - the very interleaving the note above forbids.
				vp_applied_analog = vp_output_is_analog();
				break;
			}
	}
}

void vp_reapply_core_side()
{
	if (vp_running_look >= 0) vp_apply_core_side(vp_running_look, 1);
}

/*
  The output can change under a running game: an HDMI cable pulled or pushed in,
  or - the ordinary case - a machine that boots with no sink attached and only
  finds out once i2c is up. Whatever was set for the output before is then simply
  wrong, and the panel half is the half that matters: leave it and the player has
  a pixel grid and a drop shadow on a CRT, set by a session they have already left
  behind, with nothing in any menu admitting to it.

  So re-evaluate rather than re-assert: both halves of the look are applied again
  for the output that is there now, which for the core half means vp_origs puts
  the panel options back to the player's own values before the colour half goes
  on again. Cheap by construction - it costs an i2c byte only while a look owns
  core options at all, which is the handheld case and nothing else.
*/
void vp_output_poll()
{
	if (vp_running_look < 0) return;

	/*
	  The magnification is not knowable when a look is armed, and often not even at
	  the moment the core boots: the scaler is still describing the menu, or the core
	  has not put a picture up yet. The first GBA launch after the scale-aware grid
	  landed rebuilt it for 3x - the MENU's scale - and the game then ran at 4x with
	  a grid built for somebody else. So the scale is watched here, where the output
	  is already watched, and a change rebuilds the grid AND hands the fabric the new
	  coefficients: a filter file nobody reloads is a file nobody sees.
	*/
	if (vp_grid_for_now() && vp_uses_grid(vp_running_look))
	{
		char gpath[1024];
		if (vp_preset_path_for(vp_running_look, vp_applied_analog, gpath, sizeof(gpath)))
		{
			printf("ClassicUI: re-applying \"%s\" so the new grid reaches the scaler\n",
				presets[vp_running_look].name);
			video_loadPreset(gpath, true);
		}
	}

	int analog = vp_output_is_analog();
	if (analog == vp_applied_analog) return;

	printf("ClassicUI: video output is now %s - re-evaluating look \"%s\"\n",
		analog ? "analog" : "the scaler", presets[vp_running_look].name);

	char path[1024];
	if (vp_preset_path_for(vp_running_look, analog, path, sizeof(path)))
		video_loadPreset(path, true);

	vp_apply_core_side(vp_running_look, 1);
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
	if (!presets[i].core_colour) return 0;
	if (strstr(presets[i].core_colour, "Modify Colors=GBA 2.2")) return 2.2;
	if (strstr(presets[i].core_colour, "Modify Colors=GBA 1.6")) return 1.6;
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

const uint32_t *vp_preview(int i, int w, int h, const uint32_t *ref, int sw_native, int sh_native)
{
	if (i < 0 || i >= NPRESETS || w < 8 || h < 8) return 0;
	if (pv_buf && pv_idx == i && pv_w == w && pv_h == h && pv_ref == ref) return pv_buf;
	if (ref && (sw_native < 1 || sh_native < 1)) ref = 0;

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
	  With a real frame, the look is applied by the scaler's own arithmetic - the
	  same code that draws the in-game background - and the tile is a 1:1 CROP of
	  that render. Filter at the size the television draws, zoom afterwards.

	  The frame arrives at its NATIVE resolution and is resampled exactly once, on
	  the way out. The version before this took a frame that had already been
	  magnified to tile size, shrank it back down and blew it up again: two lossy
	  passes, which is why Dinofly said the Sharp tile looked nothing like the core's
	  own sharp pixels - it was a box-average of a magnification. Sharp asks the
	  scaler for no filter at all, so here it is nearest-neighbour from the native
	  pixels, which is exactly what square pixels are.
	*/
	if (ref)
	{
		/*
		  The picture the television is really drawing, in its real proportions, and the
		  window of it this tile shows - centred.

		  Asked of the scaler rather than worked out as native x an integer scale, because
		  that arithmetic silently assumes square pixels. The Super Nintendo emits 512x224
		  for A Link to the Past; the television draws that in 1170x896, while native x 4
		  is 2048 wide. The preview was therefore stretched sideways by three quarters -
		  which is what Dinofly saw, and reported as the screenshot being vertically
		  compressed. Every core with a doubled or halved axis had the same fault: the
		  Mega Drive's 320 and 256 modes, the PlayStation's 640, hi-res Super Nintendo.

		  The fallback is the old arithmetic, for when there is nothing running to ask -
		  which is also the only case where nothing better is knowable.
		*/
		int vw = 0, vh = 0;
		if (!vp_output_rect(&vw, &vh) || vw < 16 || vh < 16)
		{
			int n = vp_output_scale();
			if (n < 2) n = 4;                   // nothing running to ask: a 1080p-ish guess
			vw = sw_native * n;
			vh = sh_native * n;
		}

		// And never smaller than the tile, or the crop below would have nothing to take.
		// Doubled rather than stretched, so the proportions survive the growth.
		while ((vw < w || vh < h) && vw < 8192) { vw *= 2; vh *= 2; }

		int cw = (w < vw) ? w : vw;
		int ch2 = (h < vh) ? h : vh;
		int x0 = (vw - cw) / 2, y0 = (vh - ch2) / 2;

		// The core's half first, on the native pixels, because the core colours the
		// picture the scaler then filters - the order the hardware runs them in.
		uint32_t *coloured = (uint32_t*)malloc((size_t)sw_native * sh_native * 4);
		if (coloured)
		{
			for (int y = 0; y < sh_native; y++)
			{
				for (int x = 0; x < sw_native; x++)
				{
					uint32_t p = ref[(size_t)y * sw_native + x];
					uint8_t c[3] = { (uint8_t)((p >> 16) & 0xff), (uint8_t)((p >> 8) & 0xff),
						(uint8_t)(p & 0xff) };

					for (int k = 0; k < 3; k++) c[k] = lut[k][c[k]];
					if (pal) pv_apply_palette(pal, c);
					else if (gba_g > 0) pv_apply_gba(gba_g, c);

					coloured[(size_t)y * sw_native + x] =
						0xff000000u | (c[0] << 16) | (c[1] << 8) | c[2];
				}
			}

			uint32_t *win = (uint32_t*)malloc((size_t)cw * ch2 * 4);
			if (win)
			{
				if (!vp_render_exact_rect(i, coloured, sw_native, sh_native,
					vw, vh, x0, y0, cw, ch2, win))
				{
					/*
					  Nothing of this look lives in the scaler - Sharp, None - so the
					  window IS the native pixels repeated, which is the picture a
					  scaler with no filter puts on the screen.

					  Mapped through the real picture size rather than an integer scale,
					  for the same reason it is asked for above: the two axes do not
					  always magnify by the same amount.
					*/
					for (int y = 0; y < ch2; y++)
					{
						int sy = (int)((long)(y0 + y) * sh_native / vh);
						if (sy >= sh_native) sy = sh_native - 1;
						for (int x = 0; x < cw; x++)
						{
							int sx = (int)((long)(x0 + x) * sw_native / vw);
							if (sx >= sw_native) sx = sw_native - 1;
							win[(size_t)y * cw + x] = coloured[(size_t)sy * sw_native + sx];
						}
					}
				}

				// Centre the window in the tile; a tile larger than the render (a
				// tiny core, a huge tile) keeps the surround rather than stretching.
				int ox = (w - cw) / 2, oy = (h - ch2) / 2;
				for (int y = 0; y < h; y++)
					for (int x = 0; x < w; x++)
						pv_buf[(size_t)y * w + x] = 0xff000000u;

				for (int y = 0; y < ch2; y++)
					memcpy(pv_buf + (size_t)(oy + y) * w + ox, win + (size_t)y * cw, (size_t)cw * 4);

				free(win);
				free(coloured);
				return pv_buf;
			}
			free(coloured);
		}
	}

	/*
	  Without a frame, the synthetic pattern is drawn at a chunky scale so pixel
	  edges and the mask stay visible, and the impression below stands in for the
	  scaler: there is no real picture for the real arithmetic to be run over.
	*/
	int scale = (h / 60) + 2;
	int sw = w / scale, sh = h / scale;
	if (sw < 8) sw = 8;
	if (sh < 8) sh = 8;

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

/* ------------------------------------------------ the per-core video mode ----- */

/*
  The list. See chome_video.h for what this is and why it is this short.

  Every entry is a number from MiSTer.ini's own predefined table - the same numbers the
  comment block above `video_mode=` in that file lists - rather than a modeline of our
  own. Three things follow from that and all three matter:

  the line written into somebody's ini is the line the documentation describes, so a
  person reading their own file afterwards finds a setting they can look up; the timings
  are upstream's, not ours, so a mode that goes wrong is upstream's mode going wrong on
  that display rather than our arithmetic; and video_mode_cmd() now takes the identical
  string, so what the countdown shows IS what the file will say.

  60 Hz only, no pixel repeat. The 50 Hz entries (3, 7, 9) are missing on purpose: this
  board's EDID is empty, nothing can ask the display whether it takes 50 Hz, and a set
  that does not is a black screen with no warning. Somebody who needs one can still write
  it by hand - the row will read Automatic and leave their line alone.

  1366x768 (10) is missing for the other reason: it is the mode panels most often report
  and least often display correctly, and there is nothing an integer scale wants from it.
*/
struct vm_entry
{
	const char *ini;
	const char *label;
	int h;
};

static const vm_entry vm_list[] = {
	/*
	  Automatic first, so the way back is the first thing the cursor is on when the screen
	  opens on a core that has no setting - and the way back from a mode somebody cannot
	  see is one press up from wherever they are.
	*/
	{ "",  "Automatic",  0 },
	{ "6", "640x480",  480 },
	{ "2", "720x480",  480 },     // 3x of 160 lines, which is what was asked for
	{ "5", "800x600",  600 },
	{ "1", "1024x768", 768 },
	{ "0", "1280x720", 720 },
	{ "8", "1920x1080", 1080 },
};

#define VM_N ((int)(sizeof(vm_list) / sizeof(vm_list[0])))

int vm_count() { return VM_N; }

const char *vm_ini(int i)   { return (i > 0 && i < VM_N) ? vm_list[i].ini : ""; }
const char *vm_label(int i) { return (i >= 0 && i < VM_N) ? vm_list[i].label : "Automatic"; }
int vm_height(int i)        { return (i > 0 && i < VM_N) ? vm_list[i].h : 0; }

int vm_current(const char *core)
{
	if (!core || !core[0]) return 0;

	char had[INI_VAL_MAX];
	if (!ini_core_value(ini_path(), core, "video_mode", had, sizeof(had))) return 0;

	for (int i = 1; i < VM_N; i++) if (!strcmp(had, vm_list[i].ini)) return i;

	/*
	  A value this list does not offer - a modeline somebody wrote by hand, or a 50 Hz
	  number. Reported as Automatic, which is a lie about the file and the right answer
	  for the screen: the alternative is a row showing a value the player cannot select,
	  cannot get back to once they move off it, and did not put there through this menu.
	  Their line is left alone unless they choose something, which is the only part that
	  actually matters.
	*/
	printf("ClassicUI: [%s] video_mode=%s is not one this menu offers - showing Automatic\n",
		core, had);
	return 0;
}

/*
  Does a video mode change anything this machine is showing?

  video_mode shapes the SCALER's output and nothing else, so the question is whether that
  output reaches a screen - which is exactly what video_scaler_is_visible() answers, and
  the same test mb_visible() uses to decide whether the Display screen is worth having.
  Three configurations answer no, for three different reasons:

  - `direct_video=1`: video_mode_load() takes the TV-mode branch and never reads
    cfg.video_conf. The core's own timing goes straight out of the DAC.
  - analog only with `vga_scaler=0` (the default): the analog port carries raw,
    scandoubled core video, bypassing the scaler entirely. The scaler output goes to an
    HDMI socket with nothing in it.
  - and on such a machine the front-end is also holding the analog takeover while its menu
    is up, which is a video mode of its own that it puts back on the way out.

  It started as `cfg.direct_video ? 0 : 1` and that was wrong in the case that matters
  most: an analog-only machine is precisely the one this feature was asked for, and there
  the preview would have applied a mode nobody could see, run a countdown against it, and
  then written a setting on the strength of a confirmation that meant nothing. A preview
  that cannot be seen is worse than no preview - it is the confirmation step lying.

  Analog with `vga_scaler=1` is the case that says yes: the scaler output goes down the
  VGA cable, there is no takeover, and video_mode is exactly the knob. That is also the
  configuration to be in for the thing the report asked for.

  Said on the screen rather than by leaving the row out, which is this file's own ruling
  about the analog report applied again: naming what is happening costs nothing, and a
  player who cannot find the setting has been told less than one who is told why it would
  not work - especially when the fix is a one-word ini change.
*/
int vm_supported()
{
	return video_scaler_is_visible() ? 1 : 0;
}

// Which of the two reasons, so the screen can name the one that applies. 1 direct video,
// 0 the scaler output reaching no screen.
int vm_unsupported_is_direct()
{
	return cfg.direct_video ? 1 : 0;
}

void vm_apply_now(int i)
{
	if (i <= 0 || i >= VM_N) { vm_restore_now(); return; }

	char cmd[32];
	snprintf(cmd, sizeof(cmd), "%s", vm_list[i].ini);
	video_mode_cmd(cmd);
}

void vm_restore_now()
{
	video_mode_restore();
}

int vm_write(const char *core, int i)
{
	if (i < 0 || i >= VM_N) return -1;

	return ini_apply_core(ini_path(), core, "video_mode",
		i ? vm_list[i].ini : 0,
		"; Written by Classic Home - Options > Video Mode.");
}
