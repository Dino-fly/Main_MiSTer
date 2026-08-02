#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "chome_opt.h"
#include "chome_ini.h"

#include "../../cfg.h"

/* ------------------------------------------------------------- the set ---- */

static const opt_choice ch_size[] =
{
	{ 0, "Fit Screen" },
	{ 1, "Whole Pixels" },
	{ 2, "Half Steps" },
	{ 3, "Quarter Steps" },
	{ 4, "Core Shape" },
	{ 5, "Screen Shape" },
};

static const opt_choice ch_black[] =
{
	{ 0, "Full" },
	{ 1, "TV 16-235" },
	{ 2, "TV 16-255" },
};

static const opt_choice ch_offon[]   = { { 0, "Off" },     { 1, "On" } };
static const opt_choice ch_autofire[] = { { 0, "Allowed" }, { 1, "Blocked" } };

/*
  controller_info is a number of seconds, 0 for never - but the only two answers a
  player has are "I want to see it" and "I do not". Six is what the shipped ini uses,
  so it stands for "shown"; a file that says 3 is left saying 3 until it is changed,
  and reads as its own number on the row.
*/
static const opt_choice ch_popup[] = { { 0, "Hidden" }, { 6, "Shown" } };

#define NCH(a) ((uint8_t)(sizeof(a) / sizeof(a[0])))

/*
  Ranges are cfg.cpp's, copied. Defaults are cfg_parse()'s, copied. Neither is
  reachable from here - ini_vars is static in cfg.cpp and the defaults are code - so
  the harness checks the handful it can and the rest is a two-file edit.
*/
static const opt_def opts[] =
{
	{ "vscale_mode", "Picture Size",
	  "Whole pixels is sharpest; fit screen is biggest.",
	  OG_PICTURE, OPT_LIST, 1, 0, 5, 1, 0, 0, 0, ch_size, NCH(ch_size), &cfg.vscale_mode, OW_GAME },

	{ "video_brightness", "Brightness",
	  "Brightness of the HDMI picture. 50 is neutral.",
	  OG_PICTURE, OPT_NUMBER, 1, 0, 100, 5, 0, 50, 50, 0, 0, &cfg.video_brightness, OW_GAME },

	{ "video_contrast", "Contrast",
	  "Contrast of the HDMI picture. 50 is neutral.",
	  OG_PICTURE, OPT_NUMBER, 1, 0, 100, 5, 0, 50, 50, 0, 0, &cfg.video_contrast, OW_GAME },

	{ "video_saturation", "Colour",
	  "How strong the colours are. 0 is black and white.",
	  OG_PICTURE, OPT_NUMBER, 1, 0, 100, 5, 0, 100, 100, 0, 0, &cfg.video_saturation, OW_GAME },

	{ "hdmi_limited", "Black Level",
	  "Try a TV range if blacks look grey on your set.",
	  OG_PICTURE, OPT_LIST, 1, 0, 2, 1, 0, 0, 0, ch_black, NCH(ch_black), &cfg.hdmi_limited, OW_GAME },

	{ "hdmi_game_mode", "TV Game Mode",
	  "Asks the television for its low-lag game mode.",
	  OG_PICTURE, OPT_LIST, 1, 0, 1, 1, 0, 0, 0, ch_offon, NCH(ch_offon), &cfg.hdmi_game_mode, OW_GAME },

	{ "rumble", "Rumble",
	  "Lets games shake a controller that can.",
	  OG_PADS, OPT_LIST, 0, 0, 1, 1, 0, 1, 1, ch_offon, NCH(ch_offon), &cfg.rumble, OW_NOW },

	/*
	  The two the front-end has an opinion about, so rec is not def. Both are in the
	  Best Settings set as well: that screen writes them without asking, this one shows
	  where they stand and lets them be put back. The amber says "not what this menu
	  wants", which for these two is the point rather than a side effect.
	*/
	{ "disable_autofire", "Autofire Toggle",
	  "Blocked stops a button combo turning autofire on.",
	  OG_PADS, OPT_LIST, 0, 0, 1, 1, 0, 0, 1, ch_autofire, NCH(ch_autofire), &cfg.disable_autofire, OW_NOW },

	{ "controller_info", "Button Pop-Up",
	  "The button list a pad shows the first time in a game.",
	  OG_PADS, OPT_LIST, 0, 0, 10, 1, 0, 6, 0, ch_popup, NCH(ch_popup), &cfg.controller_info, OW_NOW },

	{ "classicui_overscan", "TV Edge Margin",
	  "Keeps this menu clear of the edges of a TV.",
	  OG_MENU, OPT_NUMBER, 0, 0, 15, 1, "%", 6, 6, 0, 0, &cfg.classicui_overscan, OW_NOW },

	{ "classicui_freeze", "Pause In Menu",
	  "Holds the game still while this menu is open.",
	  OG_MENU, OPT_LIST, 0, 0, 1, 1, 0, 1, 1, ch_offon, NCH(ch_offon), &cfg.classicui_freeze, OW_NOW },
};

#define NOPTS ((int)(sizeof(opts) / sizeof(opts[0])))

int opt_count() { return NOPTS; }

const opt_def *opt_at(int i)
{
	if (i < 0 || i >= NOPTS) return 0;
	return &opts[i];
}

const char *opt_group_name(int g)
{
	switch (g)
	{
	case OG_PICTURE: return "Picture";
	case OG_PADS:    return "Controllers";
	case OG_MENU:    return "This Menu";
	}
	return "";
}

int opt_view(int *out, int max, int scaler_visible)
{
	int n = 0;
	for (int i = 0; i < NOPTS && n < max; i++)
	{
		if (opts[i].scaler_only && !scaler_visible) continue;
		out[n++] = i;
	}
	return n;
}

/* ----------------------------------------------------------- the model ---- */

static int cur[OPT_MAX];
static int was[OPT_MAX];
static int present[OPT_MAX];
static char last_error[128] = {};
static int wrote_live = 1;

const char *opt_error() { return last_error; }
int opt_wrote_live() { return wrote_live; }

void opt_load(const char *path)
{
	for (int i = 0; i < NOPTS; i++)
	{
		char v[24];
		present[i] = ini_value_of(path, opts[i].key, v, sizeof(v));

		/*
		  A value that is not a number is treated as though the key were absent. The
		  firmware does the same thing in the end - ini_parse_numeric() takes whatever
		  strtoul made of it and raises a cfg_error - and there is nothing this screen
		  could honestly show for it. Editing the row replaces it, which is the repair.
		*/
		char *end = v;
		long n = present[i] ? strtol(v, &end, 0) : 0;

		if (!present[i] || end == v) { cur[i] = opts[i].def; present[i] = 0; }
		else cur[i] = opt_set(i, (int)n);   // out of range shows as what the firmware
		                                    // will use, which is this same clamp

		was[i] = cur[i];
	}
	last_error[0] = 0;
}

int opt_value(int i)   { return (i >= 0 && i < NOPTS) ? cur[i] : 0; }
int opt_saved(int i)   { return (i >= 0 && i < NOPTS) ? was[i] : 0; }
int opt_present(int i) { return (i >= 0 && i < NOPTS) ? present[i] : 0; }
int opt_is_rec(int i)  { return (i >= 0 && i < NOPTS) ? (cur[i] == opts[i].rec) : 1; }
int opt_dirty_at(int i){ return (i >= 0 && i < NOPTS) ? (cur[i] != was[i]) : 0; }

int opt_set(int i, int v)
{
	if (i < 0 || i >= NOPTS) return 0;

	if (v < opts[i].lo) v = opts[i].lo;
	if (v > opts[i].hi) v = opts[i].hi;

	cur[i] = v;
	return v;
}

int opt_step_by(int i, int dir)
{
	if (i < 0 || i >= NOPTS || !dir) return 0;
	const opt_def *o = &opts[i];

	if (o->kind == OPT_NUMBER)
	{
		int v = cur[i] + dir * o->step;
		if (v < o->lo) v = o->lo;
		if (v > o->hi) v = o->hi;
		if (v == cur[i]) return 0;
		cur[i] = v;
		return 1;
	}

	/*
	  Which entry of the list we are on. A value none of them claims - somebody's own
	  controller_info=3 - is not one of the stops, so stepping off it goes to the first
	  or the last rather than pretending it was one of them.
	*/
	int at = -1;
	for (int c = 0; c < o->nchoices; c++) if (o->choices[c].val == cur[i]) at = c;

	int next;
	if (at < 0) next = (dir > 0) ? 0 : o->nchoices - 1;
	else next = (at + dir + o->nchoices) % o->nchoices;

	if (o->choices[next].val == cur[i]) return 0;
	opt_set(i, o->choices[next].val);
	return 1;
}

int opt_reset(int i)
{
	if (i < 0 || i >= NOPTS || cur[i] == opts[i].rec) return 0;
	opt_set(i, opts[i].rec);
	return 1;
}

static const char *text_for(const opt_def *o, int v, char *buf, int max)
{
	for (int c = 0; c < o->nchoices; c++)
		if (o->choices[c].val == v) return o->choices[c].label;

	snprintf(buf, (size_t)max, "%d%s", v, (o->kind == OPT_NUMBER && o->unit) ? o->unit : "");
	return buf;
}

const char *opt_value_text(int i, char *buf, int max)
{
	if (i < 0 || i >= NOPTS || max < 1) return "";
	return text_for(&opts[i], cur[i], buf, max);
}

const char *opt_rec_text(int i, char *buf, int max)
{
	if (i < 0 || i >= NOPTS || max < 1) return "";
	return text_for(&opts[i], opts[i].rec, buf, max);
}

int opt_dirty()
{
	int n = 0;
	for (int i = 0; i < NOPTS; i++) if (cur[i] != was[i]) n++;
	return n;
}

/* ----------------------------------------------------------- the write ---- */

int opt_apply(const char *path)
{
	last_error[0] = 0;

	ini_set set[OPT_MAX];
	static char vals[OPT_MAX][16];
	int which[OPT_MAX];
	int n = 0;

	for (int i = 0; i < NOPTS; i++)
	{
		if (cur[i] == was[i]) continue;

		snprintf(vals[n], sizeof(vals[n]), "%d", cur[i]);
		set[n].key = opts[i].key;
		set[n].value = vals[n];
		which[n] = i;
		n++;
	}
	if (!n) return 0;

	if (ini_apply_set(path, set, n, "; Written by Classic Home - Options > More Settings.") < 0)
	{
		snprintf(last_error, sizeof(last_error), "%s", ini_last_error());
		return -1;
	}

	/*
	  And tell the session that is already running, which parsed the ini before any of
	  this was true. Not every one of them is believed straight away - the scaler reads
	  its own settings when the video mode is next set - so what the screen says
	  afterwards is read off the set that was written rather than being a fixed line.
	*/
	wrote_live = 1;
	for (int i = 0; i < n; i++)
	{
		const opt_def *o = &opts[which[i]];
		if (o->live) *o->live = (uint8_t)cur[which[i]];
		if (o->when != OW_NOW) wrote_live = 0;

		was[which[i]] = cur[which[i]];
		present[which[i]] = 1;
	}

	return n;
}
