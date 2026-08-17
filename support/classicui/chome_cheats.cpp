/*
  The fold over the running game's cheats, and what a game remembers of them.

  chome_cheats.h has the whole of the reasoning, including the measurements off a real
  card that decided the shape - what follows is only the part that has to be right.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "../../file_io.h"
#include "../../cheats.h"

#include "chome_cheats.h"

/* ---------------------------------------------------------------- the fold ---- */

/*
  A group is (first entry, how many). The stem is not stored - see the header. Held on
  the heap and grown rather than sized to a constant, because the two numbers this has to
  live between are 1 and 31,128 and no fixed array is honest about both: 31,128 groups is
  the degenerate case where nothing folds, and at eight bytes each that is 250 KB the
  ordinary game must not be made to carry.
*/
struct ch_group
{
	int first;
	int count;
};

static ch_group *groups = 0;
static int ngroups = 0;
static int cap = 0;
static int built_for = -1;

/*
  The display name: the store's name with a trailing ".gg" removed.

  cheats_print() does the same thing in the OSD, by length rather than by extension, and
  the reason is the pack rather than a preference - every entry in a GameHacking zip
  carries it and it says nothing to anybody. Kept as its own function so the group list,
  the variant list and the per-game record cannot disagree about what a cheat is called.
*/
static void ch_display_name(const char *name, char *out, int len)
{
	if (!out || len <= 0) return;
	out[0] = 0;
	if (!name) return;

	snprintf(out, len, "%s", name);

	int n = (int)strlen(out);
	if (n > 3 && !strncasecmp(out + n - 3, ".gg", 3)) out[n - 3] = 0;
}

/*
  The stem: the display name with a trailing " (N)" removed, N being digits only.

  Deliberately narrow. "Sword (176)" folds into "Sword"; "Infinite Battery Power (Power
  Plant Network)" does not fold into "Infinite Battery Power", because the parenthesis
  holds a word and not a number - that name is a real one from the GBA pack and the two
  shapes sit in the same zip. Anything looser would merge cheats that are only related by
  having a bracket in them.
*/
static void ch_stem(const char *name, char *out, int len)
{
	ch_display_name(name, out, len);

	int n = (int)strlen(out);
	if (n < 4 || out[n - 1] != ')') return;

	/*
	  The walk is the rule: it stops at the first character that is not a digit, and the
	  '(' has to be exactly there. "Infinite Battery Power (Power Plant Network)" stops on
	  the 'k' and goes no further - that name and "Sword (176)" are both real entries in
	  the same GBA zip, which is why this is a walk and not a search for a bracket.
	*/
	int i = n - 2;
	int digits = 0;
	while (i >= 0 && out[i] >= '0' && out[i] <= '9') { i--; digits++; }

	if (!digits) return;                       // "Name ()", which nothing sane produces
	if (i < 1 || out[i] != '(') return;
	if (out[i - 1] != ' ') return;             // "Thunder(3)" is a name, not a variant

	out[i - 1] = 0;
}

static int ch_grow()
{
	int want = cap ? cap * 2 : 64;
	ch_group *n = (ch_group *)realloc(groups, want * sizeof(ch_group));
	if (!n) return 0;
	groups = n;
	cap = want;
	return 1;
}

void ch_build()
{
	ngroups = 0;
	built_for = cheats_available();

	char prev[CH_NAME_LEN] = {};
	char cur[CH_NAME_LEN] = {};

	for (int i = 0; i < built_for; i++)
	{
		ch_stem(cheats_name(i), cur, sizeof(cur));

		// Adjacent and equal extends the run; anything else starts one. See the header
		// for why adjacency is enough and why it is also all that is safe.
		if (ngroups && !strcmp(cur, prev))
		{
			groups[ngroups - 1].count++;
			continue;
		}

		/*
		  Out of memory leaves a short list rather than none, and says so. Silence here
		  would be a truncation that reads on screen as "this pack has 200 cheats" -
		  DEVELOPMENT.md's rule about capped coverage applies to a list a player is
		  looking at at least as much as to a workflow. built_for is still set, so the
		  screen does not spin rebuilding a fold that will fail the same way.
		*/
		if (ngroups >= cap && !ch_grow())
		{
			printf("ClassicUI: out of memory folding %d cheats, stopping at %d groups\n",
				built_for, ngroups);
			return;
		}

		groups[ngroups].first = i;
		groups[ngroups].count = 1;
		ngroups++;
		memcpy(prev, cur, sizeof(prev));
	}
}

int ch_built_for(int navailable)
{
	return (built_for >= 0 && built_for == navailable) ? 1 : 0;
}

int ch_groups() { return ngroups; }

int ch_group_count(int g)
{
	if (g < 0 || g >= ngroups) return 0;
	return groups[g].count;
}

int ch_group_index(int g, int v)
{
	if (g < 0 || g >= ngroups) return -1;
	if (v < 0 || v >= groups[g].count) return -1;
	return groups[g].first + v;
}

int ch_group_on(int g)
{
	if (g < 0 || g >= ngroups) return 0;

	int n = 0;
	for (int v = 0; v < groups[g].count; v++)
		if (cheats_is_enabled(groups[g].first + v)) n++;
	return n;
}

const char *ch_group_name(int g, char *out, int len)
{
	if (!out || len <= 0) return "";
	out[0] = 0;
	if (g < 0 || g >= ngroups) return out;

	ch_stem(cheats_name(groups[g].first), out, len);
	return out;
}

const char *ch_entry_name(int idx, char *out, int len)
{
	if (!out || len <= 0) return "";
	ch_display_name(cheats_name(idx), out, len);
	return out;
}

const char *ch_entry_tag(int idx, char *out, int len)
{
	if (!out || len <= 0) return "";
	out[0] = 0;

	char disp[CH_NAME_LEN];
	ch_display_name(cheats_name(idx), disp, sizeof(disp));

	// The same shape ch_stem() takes off, read out instead of discarded - so the two
	// cannot come to disagree about what counts as a number.
	int n = (int)strlen(disp);
	if (n < 4 || disp[n - 1] != ')') return out;

	int i = n - 2;
	int digits = 0;
	while (i >= 0 && disp[i] >= '0' && disp[i] <= '9') { i--; digits++; }

	if (!digits) return out;
	if (i < 1 || disp[i] != '(') return out;
	if (disp[i - 1] != ' ') return out;

	disp[n - 1] = 0;
	snprintf(out, len, "%s", disp + i + 1);
	return out;
}

/* -------------------------------------------------------------- the budget ---- */

int ch_lines_used() { return cheats_loaded(); }
int ch_lines_max()  { return cheats_max_lines(); }

int ch_set(int idx, int on)
{
	return cheats_set_enabled(idx, on);
}

/* ------------------------------------------------------------ the per-game ---- */

#define CH_PG_FILE "classicui_cheats.cfg"

/*
  One record per (game, cheat). A uint32 followed by nothing but a char array, so there
  is no padding for a compiler to choose differently on the device and in the harness -
  the same rule chome_core.cpp's co_pg states, and the only thing that makes a raw struct
  dump a safe file format. Keep it that way if a field is ever added.
*/
struct ch_pg
{
	uint32_t key;                    // game: hash of "<system id>/<relative path>"
	char name[CH_NAME_LEN];          // the cheat, as the pack names it, with its ".gg" off
};

static ch_pg pgrecs[CH_PG_MAX];
static int npgrecs = 0;
static int pg_loaded = 0;
static uint32_t pg_bound = 0;

static void pg_load()
{
	if (pg_loaded) return;
	pg_loaded = 1;

	memset(pgrecs, 0, sizeof(pgrecs));
	int len = FileLoadConfig(CH_PG_FILE, pgrecs, sizeof(pgrecs));
	npgrecs = (len > 0) ? len / (int)sizeof(ch_pg) : 0;
	if (npgrecs > CH_PG_MAX) npgrecs = CH_PG_MAX;

	if (npgrecs) printf("ClassicUI: %d remembered cheats\n", npgrecs);
}

static void pg_save()
{
	FileSaveConfig(CH_PG_FILE, pgrecs, npgrecs * (int)sizeof(ch_pg));
}

void ch_bind_game(uint32_t key) { pg_bound = key; }
uint32_t ch_bound_game() { return pg_bound; }

int ch_kept_count()
{
	if (!pg_bound) return 0;
	pg_load();

	int n = 0;
	for (int i = 0; i < npgrecs; i++) if (pgrecs[i].key == pg_bound) n++;
	return n;
}

// 1 when this game remembers this cheat by name.
static int pg_has(uint32_t key, const char *disp)
{
	for (int i = 0; i < npgrecs; i++)
	{
		if (pgrecs[i].key != key) continue;
		if (!strcmp(pgrecs[i].name, disp)) return 1;
	}
	return 0;
}

int ch_kept_matches_live()
{
	if (!pg_bound) return 0;
	pg_load();

	int kept = ch_kept_count();
	int live = cheats_active();
	if (kept != live) return 0;

	/*
	  Equal counts are not equal sets - a player who switched one cheat off and another on
	  has the same number of records and a different game. So every live cheat has to be
	  found in the store, and the counts above are what makes that sufficient rather than
	  only necessary.
	*/
	char disp[CH_NAME_LEN];
	for (int i = 0; i < cheats_available(); i++)
	{
		if (!cheats_is_enabled(i)) continue;
		ch_display_name(cheats_name(i), disp, sizeof(disp));
		if (!pg_has(pg_bound, disp)) return 0;
	}
	return 1;
}

// Drop every record for one game, without writing. Callers write once when they are done.
static void pg_drop_game(uint32_t key)
{
	int w = 0;
	for (int i = 0; i < npgrecs; i++)
	{
		if (pgrecs[i].key == key) continue;
		if (w != i) pgrecs[w] = pgrecs[i];
		w++;
	}
	npgrecs = w;
}

int ch_keep_for_game()
{
	if (!pg_bound) return -1;
	pg_load();

	/*
	  How much room there would be with this game's old records gone, because keeping is a
	  replacement and not an addition. Counting against the store as it stands would refuse
	  a player who is re-saving the same set they saved a minute ago.
	*/
	int mine = ch_kept_count();
	int room = CH_PG_MAX - (npgrecs - mine);

	int want = 0;
	int refused = 0;

	for (int i = 0; i < cheats_available(); i++)
	{
		if (!cheats_is_enabled(i)) continue;

		/*
		  A name that would not fit is refused rather than stored short. The store matches
		  by name on the next launch, and a truncated key matches whatever else truncates to
		  the same thing - which is precisely the pack shape this front-end folds, where
		  hundreds of names share a prefix.
		*/
		if ((int)strlen(cheats_name(i)) >= CH_NAME_LEN)
		{
			printf("ClassicUI: cheat name too long to remember: %s\n", cheats_name(i));
			refused++;
			continue;
		}
		want++;
	}

	if (want > room)
	{
		printf("ClassicUI: no room to remember %d cheats (%d records, %d free)\n",
			want, CH_PG_MAX, room);
		return -1;
	}

	pg_drop_game(pg_bound);

	int n = 0;
	for (int i = 0; i < cheats_available(); i++)
	{
		if (!cheats_is_enabled(i)) continue;
		if ((int)strlen(cheats_name(i)) >= CH_NAME_LEN) continue;

		ch_pg *r = &pgrecs[npgrecs++];
		memset(r, 0, sizeof(*r));
		r->key = pg_bound;
		ch_display_name(cheats_name(i), r->name, sizeof(r->name));
		n++;
	}

	pg_save();

	printf("ClassicUI: %d cheat%s kept for this game%s\n", n, n == 1 ? "" : "s",
		refused ? " (some names were too long to remember)" : "");
	return n;
}

int ch_forget_for_game()
{
	if (!pg_bound) return 0;
	pg_load();

	if (!ch_kept_count()) return 0;

	pg_drop_game(pg_bound);
	pg_save();

	printf("ClassicUI: this game no longer remembers its cheats\n");
	return 1;
}

int ch_apply_for_game(uint32_t key)
{
	ch_bind_game(key);
	if (!key) return 0;

	pg_load();
	if (!npgrecs || !cheats_available()) return 0;

	/*
	  By name against the store, which is O(records x cheats) and is the one place that
	  matters: on the 31,128-entry pack, a hundred records is three million string
	  compares in a loop that runs once per launch, off the frame path entirely. Building
	  an index to save it would be a data structure that exists for a case nobody is
	  waiting on.
	*/
	int n = 0;
	int missing = 0;
	int refused = 0;
	char disp[CH_NAME_LEN];

	for (int i = 0; i < npgrecs; i++)
	{
		if (pgrecs[i].key != key) continue;

		int found = 0;
		for (int c = 0; c < cheats_available(); c++)
		{
			ch_display_name(cheats_name(c), disp, sizeof(disp));
			if (strcmp(disp, pgrecs[i].name)) continue;

			found = 1;
			if (ch_set(c, 1)) n++;
			else refused++;
			break;
		}
		if (!found) missing++;
	}

	if (n || missing || refused)
		printf("ClassicUI: %d remembered cheat%s switched on%s%s\n", n, n == 1 ? "" : "s",
			missing ? ", some are no longer in the pack" : "",
			refused ? ", some did not fit in the core" : "");

	return n;
}

void ch_forget_all()
{
	free(groups);
	groups = 0;
	ngroups = 0;
	cap = 0;
	built_for = -1;

	memset(pgrecs, 0, sizeof(pgrecs));
	npgrecs = 0;
	pg_loaded = 0;
	pg_bound = 0;
}
