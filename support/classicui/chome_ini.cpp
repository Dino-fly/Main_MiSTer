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
  Three settings, and the reason each one is here is that without it a classic-OSD
  panel appears over the player's game - which is the one thing this front-end
  exists to prevent. Two of them Dinofly named; the third is the same complaint with
  a button combo in front of it.

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
  vscale_mode     how games are scaled. The most visible setting on the machine.

  Every key here is one ini_parse() knows: an unknown key raises a cfg_error(), and
  those are shown as an OSD panel on the next boot. Adding one would be self-defeating.
*/
static const ini_want wants[] =
{
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

/* ---------------------------------------------------------- the rewrite --- */

int ini_rewrite(const char *src, int srclen, char *dst, int dstmax)
{
	int seen[NWANTS];
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
		for (int w = 0; w < NWANTS && !done; w++)
		{
			int vs, ve;
			if (!line_assign(src, ls, be, wants[w].key, &vs, &ve)) continue;

			// Everything before the value and everything after it is the player's:
			// indentation, the spelling of the key, the spacing, the trailing note.
			PUT(src + ls, vs - ls);
			PUTS(wants[w].value);
			PUT(src + ve, i - ve);
			seen[w] = 1;
			done = 1;
		}

		if (!done) PUT(src + ls, i - ls);
	}

	int missing = 0;
	for (int w = 0; w < NWANTS; w++) if (!seen[w]) missing++;
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
	PUTS("; Written by Classic Home - Options > Best Settings.");
	PUTS(eol);
	PUTS("[MiSTer]");
	PUTS(eol);

	for (int w = 0; w < NWANTS; w++)
	{
		if (seen[w]) continue;
		PUTS(wants[w].key);
		PUTS("=");
		PUTS(wants[w].value);
		PUTS(eol);
	}

#undef PUTS
#undef PUT
	return o;
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

int ini_apply(const char *path)
{
	last_error[0] = 0;

	char *src = 0;
	int srclen = 0;
	if (!slurp(path, &src, &srclen))
	{
		snprintf(last_error, sizeof(last_error), "Could not read %s", cfg_get_name(altcfg()));
		return -1;
	}

	ini_change plan[INI_WANT_MAX];
	int n = ini_plan(path, plan, INI_WANT_MAX);
	if (!n) { free(src); return 0; }

	/*
	  The backup goes first and a failure to write it stops everything. Rewriting
	  somebody's configuration with no way back is not a thing to do on a best-effort
	  basis, and a full card is exactly when it would happen.
	*/
	if (srclen && !spill(ini_backup_path(), src, srclen))
	{
		snprintf(last_error, sizeof(last_error), "Could not save a backup - nothing was changed");
		free(src);
		return -1;
	}

	int dstmax = srclen * 2 + 1024;
	char *dst = (char*)malloc((size_t)dstmax);
	int dstlen = dst ? ini_rewrite(src, srclen, dst, dstmax) : -1;
	free(src);

	if (dstlen < 0)
	{
		free(dst);
		snprintf(last_error, sizeof(last_error), "Could not prepare the new settings");
		return -1;
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
		return -1;
	}
	sync();

	/*
	  And tell this process, which parsed the ini before any of this was true. The
	  firmware re-execs on every core switch so the next game would have read the file
	  anyway; this is for the session already running.
	*/
	for (int i = 0; i < n; i++)
	{
		const ini_want *w = plan[i].want;
		printf("ClassicUI: %s = %s (was %s)\n", w->key, w->value,
			plan[i].present ? plan[i].had : "unset");
		if (w->live) *w->live = w->lval;
	}
	printf("ClassicUI: %d setting(s) written, old file kept as %s\n", n, ini_backup_path());

	return n;
}
