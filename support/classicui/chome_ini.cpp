#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "chome_ini.h"

#include "../../cfg.h"
#include "../../file_io.h"
#include "../../user_io.h"

/* ------------------------------------------------------------- the set ---- */

/*
  Four settings. One is this front-end itself; the other three are here because
  without them a classic-OSD panel appears over the player's game - which is the one
  thing this front-end exists to prevent. Two of those Dinofly named; the third is the
  same complaint with a button combo in front of it.

  classicui       the front-end. It is what the whole screen is for, and a player who
                  has just moved an existing card onto this firmware reasonably
                  expects "Best Settings" to mean "set the front-end up", not "tidy
                  three of MiSTer's pop-ups".
                  It looks tautological - this panel cannot be reached unless
                  cfg.classicui is already 1 - and it is not, because cfg.classicui is
                  the answer for *one core on one display*, while the file holds one
                  answer per section. The reachable case is a later section that says
                  0: [MiSTer] enables the front-end, [NES] or [video=800x600] turns it
                  off again, and the player meets the classic OSD every time they press
                  the menu button in that game or on that television - from a shelf
                  where everything looks correct. That is the report this fixes, and it
                  is the one nobody can diagnose from the front-end itself.
                  The writer sets every assignment of a key wherever it appears and
                  appends under a [MiSTer] header of its own, so after a write the
                  answer no longer depends on which sections happened to match.
                  For the ordinary card - classicui=1 in [MiSTer], which is what the
                  install step asks for - the plan finds nothing to change and the
                  screen is exactly as quiet as it is today.
                  What it cannot repair is the file that never enables the front-end,
                  or enables it only in the section that is not being read: neither
                  machine is running this code, so neither can be shown this screen.
                  That case belongs to the install step in GUIDE.md, and the note about
                  first-run help in chome_ini.h says why it has to.

  video_info      the mode banner. Every core prints its resolution and refresh over
                  the picture for a few seconds when the mode changes, which for a
                  console UI is a fault report nobody asked for. Fires on the first
                  frame of a launched game, so it is the very first thing seen.
  controller_info the button map. input.cpp already suppresses this while the
                  front-end owns the screen, but the front-end is not up in a game
                  core, so plugging a pad mid-session still draws it over the game.
  disable_autofire held face button + menu button toggles autofire and announces it
                  in the same panel. It is reachable by accident, invisible once on
                  (the button just starts repeating), and there is no way back that
                  a novice would find. Not a feature this audience has lost.

  The front-end has thirteen other options and none of them is here. That is one test
  applied thirteen times, and it is worth writing down because "our own options belong in
  our own screen" is the obvious answer and the wrong one:

    A setting whose absence already gives the front-end what it wants does not belong
    in a set that writes lines into somebody's file.

  Every classicui_* option other than the switch passes that test - cfg_parse() already
  defaults classicui_freeze to 1, classicui_gamelist to 1, classicui_artfill to 1,
  classicui_overscan to 6,
  classicui_halfres to 1, classicui_artdir to boxart, classicui_arturl to the libretro
  thumbnail server and classicui_profile to auto - so writing them would add lines that
  change nothing today
  and that the player then owns and has to maintain. Worse, a written line cannot tell
  "I never set this" from "I set this on purpose": classicui_freeze=0 is precisely the
  deliberate choice of somebody whose SNES core dies when asked for a save state, and
  this screen putting a 1 back over it would be the front-end breaking a game to tidy
  an ini. Those eight are personal taste or per-television anyway, and five of them
  (overscan, freeze, halfres, tracking, caps) already have a row in Options > More
  Settings, where a value is *offered* with its recommendation beside it rather than
  assumed.

  The remaining three are off by default for reasons that are not this screen's to
  overrule:

  classicui_artfetch    sends the names of the player's ROMs to a third-party server.
                        Consent to that is not something to collect by writing 1 into
                        a file on their behalf.
  classicui_screenscraper the same, and it needs the player's own account before it
                        can do anything at all - see chome_ss.h. Turning it on for
                        somebody who has no account sets a flag and nothing else.
  classicui_disc        needs an optical drive most machines do not have, so writing a 1
                        would enable a feature that can do nothing on the great majority
                        of cards. It is *offered* instead, on Options > More Settings,
                        which is the distinction this whole list is about - and the
                        difference between offering it and asserting it is why that row
                        could be added while this ruling stands.
                        Not "the console stops if it is wrong" any more: that was true of
                        the design where the drive was read on the drawing thread, and it
                        is what a helper *process* was introduced to fix - a wedged drive
                        now costs a stuck helper. See chome_disc.h. The default is still 0,
                        for the reason above and because it is somebody else's hardware.

  And classicui_ss_user / classicui_ss_pass are somebody's login. There is no value
  to write.

  What was considered and deliberately left out, so it is not re-argued:

  bootcore        clearing it would make the machine boot into this front-end, which
                  is certainly what the front-end assumes - and is exactly the kind
                  of change to power-on behaviour a player did not ask for.
  fb_terminal=0   kills the vga_nag panel, but also the Scripts and Help entries in
                  the classic menu, which Options deliberately still hands off to.
  vga_scaler,     video routing. Getting these wrong is a black screen, and the
  direct_video    front-end takes the analog output by itself anyway.
  gamepad_defaults name-versus-positional button mapping. Changing it silently moves
                  every button in every core.
  vscale_mode     stays out of this list, but for a new reason: integer scaling
                  became the firmware DEFAULT on Dinofly's call (cfg.cpp), so
                  there is nothing left for a Best Settings row to do except
                  fight a player who explicitly wrote vscale_mode=0 - and an
                  explicit choice is exactly what this screen does not override.

  Every key here is one ini_parse() knows: an unknown key raises a cfg_error(), and
  those are shown as an OSD panel on the next boot. Adding one would be self-defeating.
*/
static const ini_want wants[] =
{
	/*
	  The outcome text is 30 characters and that is a measurement, not a style: at HD
	  the panel is 46 columns wide, and an outcome long enough that it plus its
	  key=value no longer fit on one line puts *every* setting on two lines - see the
	  `stacked` decision in draw_ini(). Room for 32 here, so this has two to spare.
	*/
	{ "classicui",        "1", "Make this menu the main screen", &cfg.classicui, 1 },
	{ "video_info",       "0", "Hide the resolution pop-up", &cfg.video_info,       0 },
	{ "controller_info",  "0", "Hide the button map pop-up", &cfg.controller_info,  0 },
	{ "disable_autofire", "1", "Stop accidental autofire",   &cfg.disable_autofire, 1 },
};

#define NWANTS ((int)(sizeof(wants) / sizeof(wants[0])))

int ini_want_count() { return NWANTS; }

const ini_want *ini_want_at(int i)
{
	if (i < 0 || i >= NWANTS) return 0;
	return &wants[i];
}

static char last_error[128] = {};
const char *ini_last_error() { return last_error; }

const char *ini_path()
{
	static char p[1024];
	snprintf(p, sizeof(p), "%s/%s", getRootDir(), cfg_get_name(altcfg()));
	return p;
}

/*
  Not "MiSTer_backup.ini": cfg_get_name() scans the SD root for MiSTer_*.ini and
  presents whatever it finds as a selectable alternate configuration, so a backup
  named that way would appear in the classic menu as a fourth ini to boot from.
*/
const char *ini_backup_path()
{
	static char p[1024];
	snprintf(p, sizeof(p), "%s.bak", ini_path());
	return p;
}

/* ------------------------------------------------------------ the parse --- */

#define IS_BLANK(c) ((c) == ' ' || (c) == '\t')

/*
  Whether the line src[ls..be) assigns `key`, and where its value sits.

  This mirrors ini_parse_var() in cfg.cpp rather than being a tidier parser, because
  the value we hand the player has to be the value the firmware will read. Two of its
  quirks matter: a space works in place of the '=', and a ';' anywhere ends the line -
  so `video_info 1 ; note` really does set 1, and a line we rewrite has to keep the
  note.
*/
static int line_assign(const char *src, int ls, int be, const char *key, int *vs, int *ve)
{
	int p = ls;
	while (p < be && IS_BLANK(src[p])) p++;
	if (p >= be || src[p] == '[' || src[p] == '+' || src[p] == ';') return 0;

	int klen = (int)strlen(key);
	if (be - p < klen || strncasecmp(src + p, key, klen)) return 0;

	int q = p + klen;
	if (q < be && src[q] == '=') q++;
	else if (q < be && IS_BLANK(src[q]))
	{
		int r = q;
		while (r < be && IS_BLANK(src[r])) r++;
		if (r >= be || src[r] == ';') return 0;         // a bare key assigns nothing
		q = (src[r] == '=') ? r + 1 : r;
	}
	else return 0;                                      // a longer key, or a bare key

	while (q < be && IS_BLANK(src[q])) q++;

	int e = q;
	while (e < be && src[e] != ';') e++;
	while (e > q && IS_BLANK(src[e - 1])) e--;

	*vs = q;
	*ve = e;
	return 1;
}

// Splits off the next line. Returns its end (exclusive of any CR), and advances *i
// past the terminator. *iscrlf says which terminator it had, for the append below.
static int next_line(const char *src, int srclen, int *i, int *iscrlf)
{
	int nl = *i;
	while (nl < srclen && src[nl] != '\n') nl++;

	int be = nl;
	*iscrlf = 0;
	if (nl < srclen)
	{
		if (be > *i && src[be - 1] == '\r') { be--; *iscrlf = 1; }
		*i = nl + 1;
	}
	else *i = nl;

	return be;
}

/*
  Whether a value survives the round trip. See chome_ini.h for why anything asks.

  The character class is cfg.cpp's CHAR_IS_VALID, written out rather than included:
  the header those macros live in is not one this file includes, and a copy that is
  visibly a copy - with the original named - is better here than a dependency on a
  private macro of the parser. If cfg.cpp ever widens the class, this only refuses
  values that would in fact have worked, which is the safe direction to be wrong in.

  Deliberately one character stricter: cfg.cpp counts a tab as a space and would keep one
  in the middle of a value, and this does not. Nothing that reaches here can type a tab,
  and a value with one in it is a value nobody could read back off their own screen.
*/
int ini_value_ok(const char *v)
{
	if (!v) return 0;

	for (const char *p = v; *p; p++)
	{
		char c = *p;

		if (c >= 'a' && c <= 'z') continue;
		if (c >= 'A' && c <= 'Z') continue;
		if (c >= '0' && c <= '9') continue;
		if (strchr("[]()-+/=#$@_,.!*:~ ", c)) continue;

		return 0;
	}

	// And the edges, which survive the class and not the trimming.
	int n = (int)strlen(v);
	if (n && (v[0] == ' ' || v[0] == '=' || v[n - 1] == ' ')) return 0;

	return 1;
}

/* ------------------------------------------------------------- the plan --- */

int ini_plan(const char *path, ini_change *out, int max)
{
	char had[NWANTS][INI_VAL_MAX];
	int present[NWANTS];
	memset(had, 0, sizeof(had));
	memset(present, 0, sizeof(present));

	FILE *f = fopen(path, "rb");
	if (f)
	{
		char line[1024];
		while (fgets(line, sizeof(line), f))
		{
			int len = (int)strlen(line);
			int i = 0, crlf = 0;
			int be = next_line(line, len, &i, &crlf);

			for (int w = 0; w < NWANTS; w++)
			{
				int vs, ve;
				if (!line_assign(line, 0, be, wants[w].key, &vs, &ve)) continue;

				/*
				  The last active assignment wins, wherever it is. The firmware's own
				  answer depends on which sections match the core being loaded, which
				  is not knowable from here - but after a write every assignment of
				  the key holds our value, so the effective one certainly does too.
				*/
				int n = ve - vs;
				if (n > INI_VAL_MAX - 1) n = INI_VAL_MAX - 1;
				memcpy(had[w], line + vs, n);
				had[w][n] = 0;
				present[w] = 1;
			}
		}
		fclose(f);
	}

	int n = 0;
	for (int w = 0; w < NWANTS && n < max; w++)
	{
		if (present[w] && !strcasecmp(had[w], wants[w].value)) continue;

		out[n].want = &wants[w];
		out[n].present = present[w];
		snprintf(out[n].had, sizeof(out[n].had), "%s", had[w]);
		n++;
	}
	return n;
}

int ini_plan_restart(const ini_change *c, int n)
{
	for (int i = 0; i < n; i++) if (c[i].want && !c[i].want->live) return 1;
	return 0;
}

int ini_value_of(const char *path, const char *key, char *out, int max)
{
	if (max) out[0] = 0;

	FILE *f = fopen(path, "rb");
	if (!f) return 0;

	int present = 0;
	char line[1024];
	while (fgets(line, sizeof(line), f))
	{
		int len = (int)strlen(line);
		int i = 0, crlf = 0;
		int be = next_line(line, len, &i, &crlf);

		int vs, ve;
		if (!line_assign(line, 0, be, key, &vs, &ve)) continue;

		// Last one wins, as in ini_plan(): which assignment the firmware actually
		// obeys depends on the sections that matched the core being loaded, and that
		// is not knowable from here.
		int n = ve - vs;
		if (n > max - 1) n = max - 1;
		if (n < 0) n = 0;
		memcpy(out, line + vs, (size_t)n);
		out[n] = 0;
		present = 1;
	}
	fclose(f);
	return present;
}

/* ---------------------------------------------------------- the rewrite --- */

int ini_rewrite_set(const char *src, int srclen, char *dst, int dstmax,
	const ini_set *set, int n, const char *note)
{
	if (n > INI_SET_MAX) return -1;

	int seen[INI_SET_MAX];
	memset(seen, 0, sizeof(seen));

	int o = 0;
	int ncrlf = 0, nlf = 0;

#define PUT(p, n) do { \
		if (o + (n) > dstmax) return -1; \
		memcpy(dst + o, (p), (n)); \
		o += (n); \
	} while (0)
#define PUTS(p) PUT((p), (int)strlen(p))

	int i = 0;
	while (i < srclen)
	{
		int ls = i, crlf = 0;
		int be = next_line(src, srclen, &i, &crlf);
		if (crlf) ncrlf++; else if (i > be) nlf++;

		int done = 0;
		for (int w = 0; w < n && !done; w++)
		{
			int vs, ve;
			if (!line_assign(src, ls, be, set[w].key, &vs, &ve)) continue;

			// Everything before the value and everything after it is the player's:
			// indentation, the spelling of the key, the spacing, the trailing note.
			PUT(src + ls, vs - ls);
			PUTS(set[w].value);
			PUT(src + ve, i - ve);
			seen[w] = 1;
			done = 1;
		}

		if (!done) PUT(src + ls, i - ls);
	}

	int missing = 0;
	for (int w = 0; w < n; w++) if (!seen[w]) missing++;
	if (!missing) return o;

	/*
	  A [MiSTer] header of our own rather than hunting for the existing one. The file
	  may end inside a core or video section, in which case appending bare keys would
	  quietly scope them to that core; and [MiSTer] may legitimately appear more than
	  once, so adding another is not a change to what the file means.
	*/
	const char *eol = (ncrlf >= nlf) ? "\r\n" : "\n";

	if (o && dst[o - 1] != '\n') PUTS(eol);
	PUTS(eol);
	PUTS(note);
	PUTS(eol);
	PUTS("[MiSTer]");
	PUTS(eol);

	for (int w = 0; w < n; w++)
	{
		if (seen[w]) continue;
		PUTS(set[w].key);
		PUTS("=");
		PUTS(set[w].value);
		PUTS(eol);
	}

#undef PUTS
#undef PUT
	return o;
}

int ini_rewrite(const char *src, int srclen, char *dst, int dstmax)
{
	ini_set set[NWANTS];
	for (int w = 0; w < NWANTS; w++)
	{
		set[w].key = wants[w].key;
		set[w].value = wants[w].value;
	}

	return ini_rewrite_set(src, srclen, dst, dstmax, set, NWANTS,
		"; Written by Classic Home - Options > Best Settings.");
}

/* ------------------------------------------------------------ the write --- */

static int slurp(const char *path, char **buf, int *len)
{
	*buf = 0;
	*len = 0;

	FILE *f = fopen(path, "rb");
	if (!f) return 1;                       // no ini at all is a valid starting point

	fseek(f, 0, SEEK_END);
	long sz = ftell(f);
	fseek(f, 0, SEEK_SET);

	if (sz < 0 || sz > 4 * 1024 * 1024) { fclose(f); return 0; }

	char *b = (char*)malloc((size_t)sz + 1);
	if (!b) { fclose(f); return 0; }

	size_t got = fread(b, 1, (size_t)sz, f);
	fclose(f);

	b[got] = 0;
	*buf = b;
	*len = (int)got;
	return 1;
}

static int spill(const char *path, const char *buf, int len)
{
	FILE *f = fopen(path, "wb");            // binary: CRLF is the file's, not ours
	if (!f) return 0;

	int ok = (len == 0) || (fwrite(buf, 1, (size_t)len, f) == (size_t)len);
	if (fflush(f)) ok = 0;
	if (fclose(f)) ok = 0;
	return ok;
}

/*
  Back up, rewrite and rename, for whatever set of assignments is handed in. 1 on
  success; 0 leaves the file untouched and says why in last_error.
*/
static int ini_write_set(const char *path, const ini_set *set, int n, const char *note)
{
	char *src = 0;
	int srclen = 0;
	if (!slurp(path, &src, &srclen))
	{
		snprintf(last_error, sizeof(last_error), "Could not read %s", cfg_get_name(altcfg()));
		return 0;
	}

	/*
	  The backup goes first and a failure to write it stops everything. Rewriting
	  somebody's configuration with no way back is not a thing to do on a best-effort
	  basis, and a full card is exactly when it would happen.
	*/
	if (srclen && !spill(ini_backup_path(), src, srclen))
	{
		snprintf(last_error, sizeof(last_error), "Could not save a backup - nothing was changed");
		free(src);
		return 0;
	}

	int dstmax = srclen * 2 + 1024 + n * 128;
	char *dst = (char*)malloc((size_t)dstmax);
	int dstlen = dst ? ini_rewrite_set(src, srclen, dst, dstmax, set, n, note) : -1;
	free(src);

	if (dstlen < 0)
	{
		free(dst);
		snprintf(last_error, sizeof(last_error), "Could not prepare the new settings");
		return 0;
	}

	// Written beside the original and renamed over it, so a power cut during the write
	// leaves the old file intact rather than half of a new one.
	char tmp[1024];
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);

	int ok = spill(tmp, dst, dstlen);
	free(dst);

	if (!ok || rename(tmp, path))
	{
		unlink(tmp);
		snprintf(last_error, sizeof(last_error), "Could not write %s - the old one is unchanged",
			cfg_get_name(altcfg()));
		return 0;
	}
	sync();
	return 1;
}

/*
  What a written setting may say about itself in the log.

  Found the moment the ScreenScraper screen became the first caller to write a value that
  is not a number: the two printf()s below reported every key = value pair, so the first
  save of somebody's password put it in clear into /tmp/debug.txt, which is world-readable.
  The screen itself never draws it - and it was still published, by a shared writer three
  files away that had never had a secret handed to it before.

  A property of the key rather than a flag on the assignment, deliberately. A flag would
  have to be set by every caller and would default to "not a secret" when a future one
  forgot, and forgetting is silent; a key that is a credential is a credential no matter
  who writes it. The list is the keys in MiSTer.ini that hold one, which today is a list
  of one - and any new caller of this writer is covered by it without knowing it exists.

  chome_ss.cpp does the same thing for a URL - see ss_redact_url() and the "***" in
  ss_build_url() - and for the same reason, which is why the replacement reads the same.

  cfg.cpp's own debug log had the identical gap, one level up: cfg_print() walked
  ini_vars[] and printed every STRING value unconditionally, and the parser's own
  line-by-line trace printed the raw text of every VAR before cfg_print() ever ran -
  so CLASSICUI_SS_PASS reached /tmp/debug.txt through either of two lines in a file
  this one had never been asked to protect against. Both now call this instead of
  reimplementing the rule, for the reason above: a second rule is a rule that can
  drift from this one and nobody would notice until it did.
*/
const char *ini_loggable(const char *key, const char *value)
{
	if (!key || !value) return "";
	if (!strcasecmp(key, "classicui_ss_pass")) return "***";
	return value;
}

int ini_apply_set(const char *path, const ini_set *set, int n, const char *note)
{
	last_error[0] = 0;
	if (n <= 0) return 0;
	if (!ini_write_set(path, set, n, note)) return -1;

	for (int i = 0; i < n; i++)
		printf("ClassicUI: %s = %s\n", set[i].key, ini_loggable(set[i].key, set[i].value));
	printf("ClassicUI: %d setting(s) written, old file kept as %s\n", n, ini_backup_path());
	return n;
}

int ini_apply(const char *path)
{
	last_error[0] = 0;

	ini_change plan[INI_WANT_MAX];
	int n = ini_plan(path, plan, INI_WANT_MAX);
	if (!n) return 0;

	/*
	  The whole set is handed to the writer, not just the ones that disagree. A key
	  already holding our value rewrites to the same bytes, and passing them all means
	  one that appears twice - once right, once wrong, in a core section - is fixed in
	  both places rather than only where the plan happened to look.
	*/
	ini_set set[NWANTS];
	for (int w = 0; w < NWANTS; w++)
	{
		set[w].key = wants[w].key;
		set[w].value = wants[w].value;
	}

	if (!ini_write_set(path, set, NWANTS, "; Written by Classic Home - Options > Best Settings."))
		return -1;

	/*
	  And tell this process, which parsed the ini before any of this was true. The
	  firmware re-execs on every core switch so the next game would have read the file
	  anyway; this is for the session already running.
	*/
	for (int i = 0; i < n; i++)
	{
		const ini_want *w = plan[i].want;
		// ini_loggable() on both halves: `had` came out of the player file and is a value of
		// the same key, so whatever makes one of them unloggable makes the other one too.
		printf("ClassicUI: %s = %s (was %s)\n", w->key, ini_loggable(w->key, w->value),
			plan[i].present ? ini_loggable(w->key, plan[i].had) : "unset");
		if (w->live) *w->live = w->lval;
	}
	printf("ClassicUI: %d setting(s) written, old file kept as %s\n", n, ini_backup_path());

	return n;
}
