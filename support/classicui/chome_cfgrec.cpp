#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <inttypes.h>
#include <sys/stat.h>

#include "chome_cfgrec.h"
#include "chome_ini.h"

#include "../../cfg.h"
#include "../../file_io.h"

/* ------------------------------------------------------------- the record --- */

struct rec_entry
{
	char section[CFGREC_SECT_MAX];
	char key[CFGREC_KEY_MAX];
	char value[CFGREC_VAL_MAX];
	int  lineno;
	uint8_t kind;               // CFGREC_SEC_*
	uint8_t applied;            // the parser really read the value
};

static rec_entry rec[CFGREC_MAX];
static int nrec = 0;
static int ndropped = 0;        // lines past the cap - counted, never silent
static int nseen = 0;

static char rec_ini[64];
static char rec_vmode[64];
static char rec_core[64];

static char cur_section[CFGREC_SECT_MAX];
static uint8_t cur_kind = CFGREC_SEC_NONE;

static int problems_cache = -1;

static void copy_into(char *dst, int max, const char *src)
{
	snprintf(dst, max, "%s", src ? src : "");
}

void cfgrec_begin(const char *ini_name, const char *vmode, const char *core_name)
{
	nrec = 0;
	ndropped = 0;
	nseen = 0;
	cur_section[0] = 0;
	cur_kind = CFGREC_SEC_NONE;
	problems_cache = -1;

	copy_into(rec_ini, sizeof(rec_ini), ini_name);
	copy_into(rec_vmode, sizeof(rec_vmode), vmode);
	copy_into(rec_core, sizeof(rec_core), core_name);
}

/*
  Which kind of section this is, decided by ini_get_section()'s own rules rather than
  by rules that merely look like them.

  The video test in particular: upstream compares the first eq_pos characters of the
  name against "video", where eq_pos is wherever the '=' fell. So "[vid=1920x1080]" is
  a video section to the parser, and it has to be one here too - a check that insisted
  on the full word would call it a core section and print the wrong explanation for the
  exact malformed header most likely to be typed by hand.
*/
static uint8_t kind_of(const char *name)
{
	if (!name || !name[0]) return CFGREC_SEC_NONE;
	if (!strcasecmp(name, "MiSTer")) return CFGREC_SEC_MISTER;

	const char *eq = strchr(name, '=');
	if (eq && eq != name && !strncasecmp(name, "video", (size_t)(eq - name))) return CFGREC_SEC_VIDEO;

	return CFGREC_SEC_CORE;
}

void cfgrec_section(const char *name, int applied)
{
	(void)applied;      // whether it applied is recorded per line, from the parser's flag
	copy_into(cur_section, sizeof(cur_section), name);
	cur_kind = kind_of(cur_section);
}

// The key is what ini_parse_var() would take: everything up to the first '=' or blank.
static int key_of(const char *line, char *out, int max)
{
	int i = 0;
	while (line[i] && line[i] != '=' && line[i] != ' ' && line[i] != '\t')
	{
		if (i < max - 1) out[i] = line[i];
		i++;
	}
	out[(i < max - 1) ? i : max - 1] = 0;
	return i;
}

// And the value, skipping the '=' and the blanks around it exactly as ini_parse_var() does.
static void value_of(const char *line, int klen, char *out, int max)
{
	int i = klen;
	while (line[i] == '=' || line[i] == ' ' || line[i] == '\t') i++;
	copy_into(out, max, line + i);
}

static int key_is_ours(const char *key)
{
	return !strncasecmp(key, "classicui", 9) || !strcasecmp(key, "debug");
}

void cfgrec_line(const char *line, int lineno, int applied)
{
	if (!line || !line[0]) return;

	char key[CFGREC_KEY_MAX];
	int klen = key_of(line, key, sizeof(key));
	if (!key[0]) return;
	if (!key_is_ours(key)) return;

	nseen++;
	problems_cache = -1;

	if (nrec >= CFGREC_MAX) { ndropped++; return; }

	rec_entry *e = &rec[nrec++];
	copy_into(e->section, sizeof(e->section), cur_section);
	copy_into(e->key, sizeof(e->key), key);
	value_of(line, klen, e->value, sizeof(e->value));
	e->lineno = lineno;
	e->kind = cur_kind;
	e->applied = applied ? 1 : 0;
}

/* ----------------------------------------------------------- the analysis --- */

static int is_known_option(const char *key)
{
	for (int i = 0; i < cfg_var_count(); i++)
		if (!strcasecmp(key, cfg_var_name(i))) return 1;
	return 0;
}

static int count_same_key(const char *key)
{
	int n = 0;
	for (int i = 0; i < nrec; i++) if (!strcasecmp(rec[i].key, key)) n++;
	return n;
}

// The first record for a key, so a duplicate is reported once, where the file first says it.
static int first_of_key(const char *key)
{
	for (int i = 0; i < nrec; i++) if (!strcasecmp(rec[i].key, key)) return i;
	return -1;
}

/*
  What is wrong with one recorded line, if anything. Placement and spelling are
  separate answers to separate questions - a misspelled key in the wrong section is
  two problems and is reported as two, because fixing either one alone leaves a
  setting that still does nothing.
*/
enum { P_NONE = 0, P_VIDEO, P_CORE, P_NOSEC };

static int placement_problem(const rec_entry *e)
{
	switch (e->kind)
	{
	case CFGREC_SEC_VIDEO: return P_VIDEO;
	case CFGREC_SEC_CORE:  return P_CORE;
	case CFGREC_SEC_NONE:  return P_NOSEC;
	default: return P_NONE;
	}
}

int cfgrec_problems()
{
	if (problems_cache >= 0) return problems_cache;

	int n = 0;
	for (int i = 0; i < nrec; i++)
	{
		/*
		  A misspelled key that is also in the wrong section is one problem and not two,
		  because it gets one entry with one fix - see report_unknown(). Counting it
		  twice would put a number on the Options panel that no amount of reading the
		  file could account for.
		*/
		if (!is_known_option(rec[i].key)) n++;
		else if (placement_problem(&rec[i]) != P_NONE) n++;

		if (first_of_key(rec[i].key) == i && count_same_key(rec[i].key) > 1) n++;
	}

	problems_cache = n;
	return n;
}

/* ------------------------------------------------------------- the report --- */

/*
  Appending with a running position, checked once per call. Everything below writes
  through this, so a report that would not fit is cut at a line boundary instead of
  producing a half-written sentence - and the caller is told, because a truncated
  report that looks complete is worse than no report.
*/
static int put(char *out, int max, int pos, const char *fmt, ...) __attribute__((format(printf, 4, 5)));

static int put(char *out, int max, int pos, const char *fmt, ...)
{
	if (pos >= max - 1) return pos;

	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(out + pos, max - pos, fmt, ap);
	va_end(ap);

	if (n < 0) return pos;
	if (n >= max - pos) return max - 1;
	return pos + n;
}

static const char *section_desc(const rec_entry *e, char *buf, int max)
{
	if (e->kind == CFGREC_SEC_NONE) snprintf(buf, max, "before any [section] header");
	else snprintf(buf, max, "section [%s]", e->section);
	return buf;
}

// The one sentence a person actually needs, for every placement problem there is.
static const char *fix_line(int prob)
{
	if (prob == P_NOSEC) return "   Fix: put a line reading [MiSTer] above it.\n";
	return "   Fix: move the line into the [MiSTer] section at the top of MiSTer.ini.\n";
}

/*
  Which section the long explanation has already been given for, and under which
  number.

  A file with four settings under one [video=...] header is the common shape of this
  mistake - somebody pastes a block in - and printing the same five-line paragraph four
  times turns the one page that has to be read into a page nobody reads. The second and
  later lines in a section get a sentence pointing at the first. Records arrive in file
  order and a section's lines are contiguous, so comparing against the last explained
  name is enough.
*/
static char explained[CFGREC_SECT_MAX];
static int explained_num = 0;
static int explained_any = 0;

static int report_placement(char *out, int max, int pos, int *num, const rec_entry *e)
{
	int prob = placement_problem(e);
	if (prob == P_NONE) return pos;

	/*
	  A key that is not an option at all is reported once, by report_unknown(), which
	  says where it is as well. Reported here too it would be two entries for one line
	  and - worse - the fix on this one would be "move classicui_dsic into [MiSTer]",
	  which is advice to carefully relocate a line that does nothing wherever it goes.
	*/
	if (!is_known_option(e->key)) return pos;

	char sd[CFGREC_SECT_MAX + 16];
	pos = put(out, max, pos, "\n%d. %s=%s  -  line %d, %s\n\n",
		++(*num), e->key, e->value, e->lineno, section_desc(e, sd, sizeof(sd)));

	int again = explained_any && !strcmp(explained, e->section);

	if (again)
	{
		if (prob == P_VIDEO)
			pos = put(out, max, pos,
				"   The same [%s] section as problem %d: live inside games,\n"
				"   dead in the menu.\n", e->section, explained_num);
		else if (prob == P_CORE)
			pos = put(out, max, pos,
				"   The same [%s] section as problem %d: live only while that\n"
				"   core is loaded.\n", e->section, explained_num);
		else
			pos = put(out, max, pos,
				"   Also above the first [section] header, like problem %d.\n", explained_num);
	}
	else if (prob == P_VIDEO)
	{
		pos = put(out, max, pos,
			"   A [video=...] section is matched against the resolution the CORE is\n"
			"   putting out, and it is the one kind of section that can be live inside\n"
			"   games and dead in the menu. MiSTer.ini is re-read on a video change only\n"
			"   while a core is running, never in the menu, so the front-end - which IS\n"
			"   the menu core - cannot pick a setting up from here.\n");
	}
	else if (prob == P_CORE)
	{
		pos = put(out, max, pos,
			"   Read from a core section, so it applies only while that core is loaded.\n"
			"   The shelf is the menu core, so the front-end never sees this setting.\n");
	}
	else
	{
		pos = put(out, max, pos,
			"   MiSTer.ini starts outside every section, and a line above the first\n"
			"   [section] header is read for no core at all.\n");
	}

	if (!again)
	{
		copy_into(explained, sizeof(explained), e->section);
		explained_num = *num;
		explained_any = 1;
	}

	if (e->applied)
		pos = put(out, max, pos,
			"   It WAS read this time, because this pass parsed for that section.\n");
	else if (again)
		pos = put(out, max, pos, "   It was NOT read either.\n");
	else
		pos = put(out, max, pos,
			"   It was NOT read: this pass skipped that section entirely, so the value\n"
			"   never reached the firmware and nothing anywhere said so.\n");

	if (!strcasecmp(e->key, "debug"))
		pos = put(out, max, pos,
			"   And debug is the setting that hides its own failure: /tmp/debug.txt is\n"
			"   opened while this very line is parsed, so a debug= that is skipped leaves\n"
			"   no log to look in. That is why this report is a file on the card and not\n"
			"   a line in that log.\n");

	return put(out, max, pos, "%s", fix_line(prob));
}

static int report_unknown(char *out, int max, int pos, int *num, const rec_entry *e)
{
	if (is_known_option(e->key)) return pos;

	char sd[CFGREC_SECT_MAX + 16];
	pos = put(out, max, pos, "\n%d. %s=%s  -  line %d, %s\n\n",
		++(*num), e->key, e->value, e->lineno, section_desc(e, sd, sizeof(sd)));

	pos = put(out, max, pos,
		"   Not an option this firmware has. MiSTer.ini ignores a key it does not\n"
		"   recognise without a word, so a typo looks exactly like a setting that does\n"
		"   not work.\n");

	int prob = placement_problem(e);
	if (prob == P_NONE)
		return put(out, max, pos,
			"   Fix: check the spelling against the resolved values at the bottom of this\n"
			"   file - every option the front-end has is listed there.\n");

	/*
	  Both wrong at once, said in one entry. Two entries for one line would be two
	  fixes for one line, and somebody who did only the one they read first would be
	  back where they started with the same silence.
	*/
	if (prob == P_NOSEC)
		pos = put(out, max, pos,
			"   And it is above the first [section] header, so even spelled correctly it\n"
			"   would be read for no core at all.\n"
			"   Fix: correct the spelling against the resolved values at the bottom of\n"
			"   this file, and put a line reading [MiSTer] above it.\n");
	else
		pos = put(out, max, pos,
			"   And even spelled correctly it would not be read here: [%s] is\n"
			"   %s.\n"
			"   Fix: correct the spelling against the resolved values at the bottom of\n"
			"   this file, and move the line into the [MiSTer] section at the top.\n",
			e->section,
			prob == P_VIDEO ? "live inside games and dead in the menu"
			                : "live only while that core is loaded");

	return pos;
}

static int report_duplicate(char *out, int max, int pos, int *num, const rec_entry *e)
{
	int n = count_same_key(e->key);
	if (n < 2) return pos;

	pos = put(out, max, pos, "\n%d. %s is assigned %d times  -  lines",
		++(*num), e->key, n);

	int listed = 0;
	for (int i = 0; i < nrec; i++)
		if (!strcasecmp(rec[i].key, e->key))
			pos = put(out, max, pos, listed++ ? ", %d" : " %d", rec[i].lineno);

	pos = put(out, max, pos, "\n\n"
		"   The last assignment the parser reaches wins, silently: there is no warning\n"
		"   and no clue on screen about which one the machine is running on.\n");

	int won = -1;
	for (int i = 0; i < nrec; i++) if (!strcasecmp(rec[i].key, e->key) && rec[i].applied) won = i;

	if (won < 0)
		pos = put(out, max, pos,
			"   None of them won: every one is in a section this pass skipped, so the\n"
			"   option is on its built-in default.\n");
	else
		pos = put(out, max, pos,
			"   Line %d won, with %s=%s.\n", rec[won].lineno, rec[won].key, rec[won].value);

	return put(out, max, pos, "   Fix: keep one of them and delete the rest.\n");
}

int cfgrec_report(char *out, int max)
{
	if (max <= 0) return 0;
	out[0] = 0;
	int pos = 0;

	explained[0] = 0;
	explained_num = 0;
	explained_any = 0;

	pos = put(out, max, pos,
		"Classic Home - configuration check\n"
		"==================================\n\n"
		"Written on every boot, whether or not the front-end is switched on, and without\n"
		"consulting debug= - a setting in the wrong section cannot switch this file off.\n"
		"It is rewritten each time, so edits to it are lost.\n\n"
		"Only classicui* settings and debug are checked. Everything else in MiSTer.ini is\n"
		"upstream's and is none of this front-end's business.\n\n");

	pos = put(out, max, pos,
		"  ini file read    : %s\n"
		"  parsed for core  : %s\n"
		"  [video=...] matched against : %s\n",
		rec_ini[0] ? rec_ini : "(none)",
		rec_core[0] ? rec_core : "(unknown)",
		rec_vmode[0] ? rec_vmode : "(none)");

	/*
	  Said only when it is true, and it is observed rather than assumed: the mode above
	  is whatever the parser was really handed. At boot cfg_parse() runs before any
	  frame has been measured, so it is zeros, and then no [video=...] section can match
	  however carefully it was written.
	*/
	if (!strncmp(rec_vmode, "0x0", 3))
		pos = put(out, max, pos,
			"      0x0 means no core video had been measured yet, so on this pass no\n"
			"      [video=...] section could match at all.\n");

	pos = put(out, max, pos, "  classicui/debug lines seen  : %d\n", nseen);
	if (ndropped)
		pos = put(out, max, pos,
			"      %d of them were past this check's %d-line limit and are NOT below.\n",
			ndropped, CFGREC_MAX);

	int total = cfgrec_problems();

	if (!total)
	{
		pos = put(out, max, pos,
			"\nPROBLEMS: none\n"
			"--------------\n"
			"Every classicui setting was read from [MiSTer], spelled correctly and set\n"
			"once. Nothing here needs your attention.\n");
	}
	else
	{
		pos = put(out, max, pos, "\nPROBLEMS: %d\n-----------\n", total);

		int num = 0;
		for (int i = 0; i < nrec; i++)
		{
			pos = report_placement(out, max, pos, &num, &rec[i]);
			pos = report_unknown(out, max, pos, &num, &rec[i]);
			if (first_of_key(rec[i].key) == i)
				pos = report_duplicate(out, max, pos, &num, &rec[i]);
		}
	}

	pos = put(out, max, pos,
		"\nEVERY classicui/debug LINE, IN FILE ORDER\n"
		"-----------------------------------------\n");

	if (!nrec) pos = put(out, max, pos, "  (none - the file has no classicui setting in it at all)\n");

	for (int i = 0; i < nrec; i++)
	{
		char where[CFGREC_SECT_MAX + 4];
		if (rec[i].kind == CFGREC_SEC_NONE) snprintf(where, sizeof(where), "(no section)");
		else snprintf(where, sizeof(where), "[%s]", rec[i].section);

		/*
		  Through ini_loggable(), the same redaction the resolved-values table below
		  uses and the same one the debug log uses.

		  This line prints what the file literally says, which is exactly why it has to
		  ask: one of the keys we record is classicui_ss_pass, and a file written to the
		  card on every boot - the file we tell people to send us when something is
		  wrong - is the worst possible place to repeat a password. Measured on a real
		  card before this call was here: the resolved-values table said `***` while this
		  one printed the password in full, three lines apart. Redacting in one printer
		  and not the other is not a smaller bug than not redacting at all; it is the
		  same bug with a witness.
		*/
		pos = put(out, max, pos, "  line %5d  %-22s %s=%s  %s\n",
			rec[i].lineno, where, rec[i].key,
			ini_loggable(rec[i].key, rec[i].value),
			rec[i].applied ? "read" : "SKIPPED");
	}

	pos = put(out, max, pos,
		"\nRESOLVED VALUES - what the front-end is really using\n"
		"----------------------------------------------------\n");

	for (int i = 0; i < cfg_var_count(); i++)
	{
		const char *nm = cfg_var_name(i);
		if (strncasecmp(nm, "classicui", 9) && strcasecmp(nm, "debug")) continue;

		char v[CFGREC_VAL_MAX];
		cfg_var_text(i, v, sizeof(v));

		/*
		  The ScreenScraper password is a setting like any other and is not printed like
		  one. This file goes on a card that gets handed around and posted to forums when
		  somebody is stuck, which is the exact journey that leaked one during
		  development - see ini_loggable() in chome_ini.h for the same rule at the other
		  writer.
		*/
		if (!strcasecmp(nm, "CLASSICUI_SS_PASS") && v[0]) snprintf(v, sizeof(v), "***");

		pos = put(out, max, pos, "  %s=%s\n", nm, v);
	}

	if (pos >= max - 1)
		pos = put(out, max, pos, "\n[report truncated]\n");

	return pos;
}

/* -------------------------------------------------------------- the write --- */

const char *cfgrec_report_path()
{
	static char path[512];
	snprintf(path, sizeof(path), "%s/classicui/config-report.txt", getRootDir());
	return path;
}

/*
  8 KB, static: the worst case is CFGREC_MAX findings of about a paragraph each, and a
  buffer this size is a rounding error next to one cover thumbnail. It is static rather
  than on the stack because this runs during startup on a 1 GB ARM box where the stack
  is the one thing worth being careful with.
*/
static char report_buf[8192];

void cfgrec_write_report()
{
	char dir[512];
	snprintf(dir, sizeof(dir), "%s/classicui", getRootDir());
	mkdir(dir, 0777);

	int n = cfgrec_report(report_buf, sizeof(report_buf));

	FILE *f = fopen(cfgrec_report_path(), "w");
	if (!f)
	{
		// Reported and dropped. A machine that cannot write to its own card has a
		// bigger problem than a misplaced ini line, and startup does not wait for it.
		printf("ClassicUI: cannot write %s\n", cfgrec_report_path());
		return;
	}

	fwrite(report_buf, 1, n, f);
	fclose(f);

	printf("ClassicUI: config check wrote %s (%d problem%s)\n",
		cfgrec_report_path(), cfgrec_problems(), cfgrec_problems() == 1 ? "" : "s");
}
