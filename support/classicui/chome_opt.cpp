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

// Named for what the player is choosing between, not for the mechanism: "Half" of what
// would mean nothing on this screen, and Off/On would not say which way is which.
static const opt_choice ch_res[] = { { 0, "Sharp" }, { 1, "Fast" } };

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
	  OG_PICTURE, OPT_LIST, 1, 0, 5, 1, 0, 0, 0, ch_size, NCH(ch_size), &cfg.vscale_mode, 0, OW_GAME },

	{ "video_brightness", "Brightness",
	  "Brightness of the HDMI picture. 50 is neutral.",
	  OG_PICTURE, OPT_NUMBER, 1, 0, 100, 5, 0, 50, 50, 0, 0, &cfg.video_brightness, 0, OW_GAME },

	{ "video_contrast", "Contrast",
	  "Contrast of the HDMI picture. 50 is neutral.",
	  OG_PICTURE, OPT_NUMBER, 1, 0, 100, 5, 0, 50, 50, 0, 0, &cfg.video_contrast, 0, OW_GAME },

	{ "video_saturation", "Colour",
	  "How strong the colours are. 0 is black and white.",
	  OG_PICTURE, OPT_NUMBER, 1, 0, 100, 5, 0, 100, 100, 0, 0, &cfg.video_saturation, 0, OW_GAME },

	{ "hdmi_limited", "Black Level",
	  "Try a TV range if blacks look grey on your set.",
	  OG_PICTURE, OPT_LIST, 1, 0, 2, 1, 0, 0, 0, ch_black, NCH(ch_black), &cfg.hdmi_limited, 0, OW_GAME },

	{ "hdmi_game_mode", "TV Game Mode",
	  "Asks the television for its low-lag game mode.",
	  OG_PICTURE, OPT_LIST, 1, 0, 1, 1, 0, 0, 0, ch_offon, NCH(ch_offon), &cfg.hdmi_game_mode, 0, OW_GAME },

	{ "rumble", "Rumble",
	  "Lets games shake a controller that can.",
	  OG_PADS, OPT_LIST, 0, 0, 1, 1, 0, 1, 1, ch_offon, NCH(ch_offon), &cfg.rumble, 0, OW_NOW },

	/*
	  The two the front-end has an opinion about, so rec is not def. Both are in the
	  Best Settings set as well: that screen writes them without asking, this one shows
	  where they stand and lets them be put back. The amber says "not what this menu
	  wants", which for these two is the point rather than a side effect.
	*/
	{ "disable_autofire", "Autofire Toggle",
	  "Blocked stops a button combo turning autofire on.",
	  OG_PADS, OPT_LIST, 0, 0, 1, 1, 0, 0, 1, ch_autofire, NCH(ch_autofire), &cfg.disable_autofire, 0, OW_NOW },

	{ "controller_info", "Button Pop-Up",
	  "The button list a pad shows the first time in a game.",
	  OG_PADS, OPT_LIST, 0, 0, 10, 1, 0, 6, 0, ch_popup, NCH(ch_popup), &cfg.controller_info, 0, OW_NOW },

	/*
	  Physical discs, and this row is the whole of how the feature is reached.

	  It was gated on classicui_disc and offered nowhere, so the only way to turn optical
	  disc support on was to edit MiSTer.ini with a keyboard - which is precisely the thing
	  this front-end exists to remove. A feature with no control is not an opt-in, it is a
	  feature only its author has.

	  First in the OG_MENU block rather than beside the typography rows: this is the one row
	  in the group that turns a *feature* on, and the four that follow it are all about how
	  the menu already looks - the last two of them are written to be read next to the Font
	  row underneath the table (see their own comments), so nothing may be inserted between
	  them and it.

	  In OG_MENU at all, and not in a group of its own, because every consequence of this
	  setting is something *this menu* does: a badge on the shelf, the screen behind it, and
	  a helper process the front-end owns. No core and no game behaves differently. A
	  one-row "Disc Drive" group would buy a caption and cost a section the player goes
	  looking for and does not find.

	  rec is def, which is 0, so a machine with the drive turned on reads amber and the
	  footer says "Usually Off". That is not a nag by accident - chome_disc.h states the
	  opinion this colour is reporting: the feature stays opt-in until it has been proven
	  against a range of drives and discs. A player who turned it on deliberately is being
	  told they are away from the shipped default, which they are.

	  OW_NOW, and it is honest in both directions rather than only in the interesting one.
	  disc_poll() re-reads cfg.classicui_disc every pass, so turning it on starts the drive
	  probe on the next frame with no relaunch and no rescan - the library is files on the
	  card and a disc was never in it. Turning it *off* used to leave the helper holding
	  /dev/sr0 with nobody reading what it wrote; disc_poll() now hands the drive back when
	  the flag goes away, which is what makes OW_NOW true rather than half true. See the
	  note at the top of disc_poll().
	*/
	{ "classicui_disc", "Physical Disc",
	  "Finds a game disc in a USB drive and offers to play it.",
	  OG_MENU, OPT_LIST, 0, 0, 1, 1, 0, 0, 0, ch_offon, NCH(ch_offon), &cfg.classicui_disc, 0, OW_NOW },

	/*
	  Re-asking ScreenScraper for the covers the fallback pack supplied.

	  Off, and off is the recommendation as well as the default, which is unusual here - most
	  rows recommend what they default to because that is the good value. This one is off
	  because of what it *spends*: the marks it acts on cost nothing to keep, and turning it
	  on turns each of them into one request against the account's unmatched allowance, which
	  is the scarce one and the reason the miss store exists at all.

	  It is a row rather than an ini-only key for the same reason Physical Disc above is: the
	  player it helps is the one who entered their account *after* their card was already
	  full, and asking that person to edit a file with a keyboard is asking them to leave the
	  front-end to fix the front-end.

	  OW_NOW. art_step() reads the flag on the pass that would queue the retry
	  (chome_art.cpp:3024), so it takes effect on the next frame with no rescan - and the
	  marks were being written all along whether it was set or not, so there is no history to
	  rebuild when it is switched on.

	  The help line names the *pack* rather than the account, because which covers this
	  touches is the part nobody can guess: not a gamelist.xml cover, not a scrape made with
	  another tool, not a pack copied on by hand - only what this front-end fetched itself.
	*/
	{ "classicui_ss_replace_pack", "Replace Pack Art",
	  "Re-asks ScreenScraper for covers the pack supplied.",
	  OG_MENU, OPT_LIST, 0, 0, 1, 1, 0, 0, 0, ch_offon, NCH(ch_offon),
	  &cfg.classicui_ss_replace_pack, 0, OW_NOW },

	{ "classicui_overscan", "TV Edge Margin",
	  "Keeps this menu clear of the edges of a TV.",
	  OG_MENU, OPT_NUMBER, 0, 0, 15, 1, "%", 6, 6, 0, 0, &cfg.classicui_overscan, 0, OW_NOW },

	{ "classicui_freeze", "Pause In Menu",
	  "Holds the game still while this menu is open.",
	  OG_MENU, OPT_LIST, 0, 0, 1, 1, 0, 1, 1, ch_offon, NCH(ch_offon), &cfg.classicui_freeze, 0, OW_NOW },

	/*
	  The half-resolution canvas, on by default and at the owner's request: a quarter of
	  the pixels to compose and copy, which on the device is the difference between a
	  spinning disc costing a full core at 720p and fitting in the frame budget. The
	  layout does not change with it - the profile is chosen from the display, see
	  theme_update() - so what "Fast" costs is sharpness alone, and the help line says
	  which side of the trade each value sits on. Applied by the frame loop's
	  fb_size_sync() the moment it is written, which is why it can be OW_NOW.
	*/
	{ "classicui_halfres", "Menu Resolution",
	  "Fast draws this menu with fewer pixels. Games are unaffected.",
	  OG_MENU, OPT_LIST, 0, 0, 1, 1, 0, 1, 1, ch_res, NCH(ch_res), &cfg.classicui_halfres, 0, OW_NOW },

	/*
	  Letter spacing, and the help line is doing real work rather than describing the row.

	  Every panel width and a good deal of the wording in this front-end was fitted to the
	  ROM font's 8-pixel advance: a full-width row at 240p holds 35 characters, and several
	  lines were written to exactly that. Positive spacing takes characters away - 31 at +1,
	  28 at +2 - so those lines start ending in a '>'. Nothing breaks, the clip is clean and
	  every panel still contains its own text, but a player who was not told would read it
	  as the front-end being buggy rather than as the price of the setting they just chose.
	  Hence "fits fewer words", in the sentence under the row, where they are looking.

	  Not clamped to -1 even though -2 makes the eight widest glyphs in the stock font
	  (& M W ^ _ m w ~) touch their neighbour. The range offered is the range cfg.cpp
	  declares, the point of the feature is a *custom* font - a five-wide one wants -2 - and
	  this is a dial the player is watching while they turn it.
	*/
	{ "classicui_tracking", "Letter Spacing",
	  "Space between letters. Positive fits fewer words.",
	  OG_MENU, OPT_NUMBER, 0, -2, 2, 1, 0, 0, 0, 0, 0, 0, &cfg.classicui_tracking, OW_NOW },

	/*
	  And whether this menu shouts. On, which is what it has always done, every title and
	  label is drawn in capitals; off, they are drawn as they are written. The reason it is
	  a setting at all is the font above it: somebody who put a .pf on the card usually
	  chose it for its lowercase, and a front-end that never draws a lowercase glyph makes
	  half of that font invisible.
	*/
	{ "classicui_caps", "Capital Letters",
	  "Off draws titles and labels as they are written.",
	  OG_MENU, OPT_LIST, 0, 0, 1, 1, 0, 1, 1, ch_offon, NCH(ch_offon), &cfg.classicui_caps, 0, OW_NOW },
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
int opt_present(int i) { return (i >= 0 && i < NOPTS) ? present[i] : 0; }
int opt_is_rec(int i)  { return (i >= 0 && i < NOPTS) ? (cur[i] == opts[i].rec) : 1; }

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

int opt_apply(const char *path, const ini_set *extra, int nextra)
{
	last_error[0] = 0;

	ini_set set[INI_SET_MAX];
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

	// The screen's own keys after ours, and counted separately: they are not in the model
	// below, so the loop that updates cfg and clears the dirty flags must not walk them.
	int nown = n;
	for (int i = 0; i < nextra && n < INI_SET_MAX; i++) set[n++] = extra[i];

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
	for (int i = 0; i < nown; i++)
	{
		const opt_def *o = &opts[which[i]];
		if (o->live) *o->live = (uint8_t)cur[which[i]];
		else if (o->live_s) *o->live_s = (int8_t)cur[which[i]];
		if (o->when != OW_NOW) wrote_live = 0;

		was[which[i]] = cur[which[i]];
		present[which[i]] = 1;
	}

	return n;
}
