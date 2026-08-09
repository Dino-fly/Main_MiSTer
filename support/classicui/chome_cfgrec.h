/*
  Classic Home - the boot-time configuration check.

  ----------------------------------------------------------------- the problem ---

  MiSTer.ini is sectioned, and a setting only takes effect if the parser is inside a
  section that applies to the core being started (cfg.cpp, ini_parse() and
  ini_get_section()). A line appended to the end of the file lands in whatever section
  happens to be last, and is then read for that core and no other - silently. There is
  no error, no log line and nothing on screen; the setting simply is not there.

  For this front-end that failure is total rather than partial. classicui=1 in the
  wrong section means the shelf never appears at all, so there is no screen of ours to
  put a diagnosis on, and the player is looking at the stock OSD wondering what they
  installed. A user lost a day to it this week; we have hit it three times ourselves.
  It is in GUIDE.md and it still catches people, so a document is not the answer.

  ------------------------------------------- why this cannot be a debug log line ---

  The same user could not produce /tmp/debug.txt either, because debug=2 was itself in
  a section that never applied - cfg.cpp handles DEBUG while parsing the line, so a
  skipped section means the log is never opened. A warning that only exists in the
  debug log is missing from exactly the machine that needs it.

  So the rule this file is built on: the diagnostic must not depend on the thing being
  diagnosed. cfgrec_write_report() writes an ordinary text file to the card on every
  boot regardless of cfg.debug, regardless of cfg.classicui, and regardless of whether
  the word "classicui" was ever successfully parsed. Somebody with the card in a PC can
  always read it. That is the entire point, and it is why the write is unconditional in
  a codebase where almost nothing else is.

  --------------------------------------------------------- observe, do not change ---

  cfg.cpp is shared by every core and by the stock OSD, and the recording calls in it
  are three lines that copy strings into the buffer below. They read the parser's state;
  they never steer it. The one structurally new thing is a trailing `else` in ini_parse()
  that fires only on lines the parser already did nothing with - the skipped-section
  case, which is the invisible failure and the most valuable one to catch. Every branch
  that resolves a setting is untouched, so no option can resolve differently because
  this exists.

  ------------------------------------------------------------------ and the scope ---

  Only classicui* keys and debug. We do not own upstream's option semantics, and a
  front-end that warned about somebody's legitimate per-core settings would be a bug
  factory that trains people to ignore the file. Filtering at record time is also what
  bounds the memory: an ini of any size produces at most CFGREC_MAX entries, and the
  overflow is counted and reported rather than hidden.
*/

#ifndef CHOME_CFGREC_H
#define CHOME_CFGREC_H

/*
  The cap. Fifteen classicui options plus debug is sixteen lines in a sane file; 32
  leaves room for the duplicates and typos that are the whole reason for looking, and
  costs about 6 KB. Entries past it are counted and the report says so - a truncated
  record must never read as a clean one.
*/
#define CFGREC_MAX       32

#define CFGREC_SECT_MAX  40
#define CFGREC_KEY_MAX   40
#define CFGREC_VAL_MAX   72

/* What kind of section a line was found in. */
enum
{
	CFGREC_SEC_NONE = 0,       // before any [section] header at all
	CFGREC_SEC_MISTER,         // [MiSTer] - the one that always applies
	CFGREC_SEC_CORE,           // [Gameboy], [arcade], [MegaDrive*] ...
	CFGREC_SEC_VIDEO           // [video=1920x1080]
};

/* ---------------------------------------------------------------- recording ---

  Called from cfg.cpp's parser only. Also called directly by the harness, which is
  deliberate: the analysis is then driven over the same records the parser produces,
  rather than over a second model of them that could drift.
*/

// Start of one pass over one ini file. Clears the record.
void cfgrec_begin(const char *ini_name, const char *vmode, const char *core_name);

/*
  A [section] line, with the name as ini_get_section() left it (no brackets) and
  whether it applied. Must be called for skipped sections too - that is where the
  invisible keys live.
*/
void cfgrec_section(const char *name, int applied);

/*
  One non-section line, exactly as ini_getline() produced it and BEFORE
  ini_parse_var() writes a nul over its '='. `applied` is the parser's `section`
  flag: 1 when the value was really read, 0 when the line was skipped.

  Lines whose key is neither classicui* nor debug are dropped here.
*/
void cfgrec_line(const char *line, int lineno, int applied);

/* ----------------------------------------------------------------- reporting --- */

/*
  How many problems the record contains. 0 means every classicui line was read from
  [MiSTer], with no duplicates and no unknown keys - which is the normal answer and
  the one that must cost nothing to ask, so it is computed once and cached.
*/
int cfgrec_problems();

/*
  The report, as the text that goes in the file. Returns the number of characters
  written. Public so the harness can check the wording, which is the product here:
  the file is read by somebody who is already confused, and a report that says
  "warning: section mismatch" would leave them exactly where they were.
*/
int cfgrec_report(char *out, int max);

/*
  Write it to <root>/classicui/config-report.txt, overwriting. Unconditional - see
  the top of this file. Call after cfg_parse() in the menu core.
*/
void cfgrec_write_report();

// Where it went, absolute, for the sentence on screen and for the harness.
const char *cfgrec_report_path();

#endif
