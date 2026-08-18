/*
  Classic Home - the MiSTer.ini settings this front-end assumes.

  The front-end is opinionated by design, and part of that opinion cannot be held in
  our own state file: it lives in MiSTer.ini, which the firmware re-reads on every
  core load. The settings here are the ones whose absence a player would experience
  as the front-end being broken - a classic-OSD panel appearing over their game, or
  the front-end not being the screen they get.

  Three rules decided what is in the set, and they are worth keeping.

  Only things that surprise a player are removed. A setting that changes how the
  machine *behaves* outside the front-end is not ours to rewrite, however much this
  UI might prefer it: see the rejected list in chome_ini.cpp for what that ruled out
  and why. The set is deliberately short.

  And a setting whose absence already gives the front-end what it wants does not
  belong in a set that writes lines into somebody's file. That rule is what keeps
  eleven of the front-end's own twelve options out of it - every one but the switch
  already defaults to what this UI wants, so writing them would plant values that
  change nothing and that the player then owns. The argument, option by option, is in
  chome_ini.cpp.

  And the file is the player's, not ours. Everything not being set is copied through
  byte for byte - line endings, comments, ordering, unknown keys, sections we have
  never heard of - and the old file is kept beside the new one. MiSTer.ini is CRLF
  and hand-edited; a rewrite that reflowed it would turn every later diff into noise
  and would lose the comments people leave themselves.

  ------------------------------------------------------- and no first-run prompt ---

  There is deliberately nothing that notices a fresh installation and offers this
  screen, and the reason is not that it would be hard.

  The genuinely unconfigured machine cannot be told anything. Until classicui=1 is in
  the ini in a place the menu core reads, this front-end is not running - the player
  is looking at the stock OSD browser and there is no surface here to put a prompt on.
  The one case that most wants first-run help is the one case that structurally cannot
  receive it, which is why the answer to it is the install step in GUIDE.md and the
  troubleshooting line under it, not code.

  What is left, once the front-end *is* running, is a card with a pop-up or two still
  enabled - and that is already reported without being asked: the Options row reads
  "Best Settings  3 To Change >", or "All Set" when there is nothing to do. That is the
  whole of a first-run notice, one press deep, in the place a player goes to look for
  settings, and it keeps working forever rather than only on the first boot.

  A shelf-level notice was considered and is exactly the thing this front-end exists
  to remove: a panel of technical text about a configuration file, over somebody's
  cover art, before they have pressed anything. It is the same objection as video_info
  in chome_ini.cpp, and it would be ours rather than MiSTer's.
*/

#ifndef CHOME_INI_H
#define CHOME_INI_H

#include <inttypes.h>

#define INI_WANT_MAX 8              // the set is short on purpose
#define INI_VAL_MAX  24

// One setting the front-end insists on.
struct ini_want
{
	const char *key;            // spelled as MiSTer.ini spells it
	const char *value;          // written verbatim
	const char *outcome;        // what the player is told will happen
	uint8_t *live;              /* the cfg field to update as well, so the machine does
	                               not have to be restarted. 0 where only startup reads
	                               it - which is what makes the restart notice honest
	                               rather than a hand-maintained flag. */
	uint8_t lval;               // ...and what to put in it
};

// What writing would do to one of them.
struct ini_change
{
	const ini_want *want;
	char had[INI_VAL_MAX];      // what is set now; empty when the key is absent
	int present;                // the key is there and not commented out
};

int ini_want_count();
const ini_want *ini_want_at(int i);

/*
  One assignment to make. The set above is the front-end's own opinion; this is the
  general form underneath it, so that a second screen - Options > More Settings, which
  lets the player set things this file has no opinion about at all - writes through the
  same parser and the same backup rather than growing a second ini writer beside it.
*/
#define INI_SET_MAX 32

struct ini_set
{
	const char *key;
	const char *value;
};

// The ini this machine is really using (alt inis included), absolute.
const char *ini_path();

// And where ini_apply() keeps the copy of the old one.
const char *ini_backup_path();

/*
  What a write would change, in the order the set is declared. Returns how many, and
  0 when the file already agrees. A missing file reads as "every key absent".
*/
int ini_plan(const char *path, ini_change *out, int max);

// 1 when any of them is one the firmware only reads at startup.
int ini_plan_restart(const ini_change *c, int n);

/*
  Rewrite a whole file image. Returns the length written, or -1 if it would not fit.
  Assignments of our keys are set to our values wherever they appear - including in a
  core or video section, which would otherwise override the one we fixed - and keys
  that appear nowhere are appended in a [MiSTer] section of their own.
*/
int ini_rewrite(const char *src, int srclen, char *dst, int dstmax);

/*
  The same, for an arbitrary set of assignments. `note` is the comment written above
  the appended block, so a file says which screen added the lines at the bottom of it.
*/
int ini_rewrite_set(const char *src, int srclen, char *dst, int dstmax,
	const ini_set *set, int n, const char *note);

// What is set now, from the file rather than from cfg - which holds the value after
// the core's own sections were applied. 1 when the key is there and not commented out.
int ini_value_of(const char *path, const char *key, char *out, int max);

/*
  1 when a string would come back out of MiSTer.ini as the same string it went in as.

  Every writer above this line has only ever written numbers, so nothing needed to ask.
  The ScreenScraper screen writes somebody's login and their password, and those are
  the first values here that a person chooses the characters of - which is when this
  stopped being theoretical.

  cfg.cpp's ini_getline() keeps a character only if it is CHAR_IS_VALID or a space, and
  silently drops everything else; a ';' does not even get that far, because it ends the
  line as a comment. So a password with a '&' or a '%' in it - both of which the
  on-screen keyboard happily types, and both of which are ordinary in a password - is
  written to the file correctly, read back mangled, and produces a credentials error on
  every request from then on with nothing anywhere saying why. The value that reaches
  the server is not the value on the screen and no screen can show the difference.

  Leading and trailing spaces go the same way, from two directions: ini_getline() trims
  the trailing ones and skips the leading ones, and line_assign() above trims both when
  we rewrite a line. A leading '=' is eaten too - ini_parse_var() skips '=' and blanks
  after the key, however many there are.

  So this is the round-trip test stated once, in the file that owns the format, rather
  than as a character class copied into a settings screen. A caller that gets 0 must
  refuse the value and say so: writing it would be writing a value that cannot work.
*/
int ini_value_ok(const char *v);

/*
  What a setting's value may say about itself in the log: the value, or "***" when the key
  is one that holds a credential.

  Public so the harness can check it, and used by both writers below rather than by their
  callers. See the comment on the definition for the bug - a screen that never draws a
  password, publishing it anyway through a shared writer three files away.
*/
const char *ini_loggable(const char *key, const char *value);

/*
  Back up and write an arbitrary set. Returns n, 0 when n is 0, or -1 with
  ini_last_error() set. The caller owns telling the running firmware: this knows
  nothing about which cfg field is behind a key.
*/
int ini_apply_set(const char *path, const ini_set *set, int n, const char *note);

/* ----------------------------------------------------- one core's own section --- */

/*
  Everything above this line is section-blind on purpose: ini_rewrite_set() sets a key
  wherever it appears - including inside a core or video section, which would otherwise
  override the one we fixed - and appends what is missing into a [MiSTer] of its own.
  That is right for the settings this front-end has an opinion about, which are opinions
  about the machine.

  A per-core setting is the opposite question, and needs the opposite rule. `video_mode`
  under `[GBA]` must not touch the `video_mode` under `[MiSTer]`, because that one is the
  mode every other core on the machine runs at, and rewriting it is precisely the black
  screen a per-core setting exists to avoid.

  So these three work inside exactly one section, `[<core>]`, and leave every other byte
  of the file alone - the same copy-through rule as the rewriter above.

  A note on why the section goes at the END of the file when it has to be created.
  cfg.cpp parses top to bottom and a later matching section wins, so a `[GBA]` appended
  after an existing `[MiSTer]` is the one that takes effect. The one thing that can still
  outrank it is a `[video=...]` section further down - see the trap written up in
  CLAUDE.md - and there is nothing to be done about that from here beyond not being the
  cause of it.
*/

// What `[<core>]` sets this key to, from the file. 1 when the key is there, in that
// section, and not commented out. Values elsewhere in the file are not this question.
int ini_core_value(const char *path, const char *core, const char *key, char *out, int max);

/*
  Rewrite an image with one key set inside `[<core>]`. A null `value` REMOVES the
  assignment, which is the only honest way to spell "no per-core setting": an empty
  `video_mode=` is not "unset" to cfg.cpp, it is a parse failure that falls back to
  1080p. Returns the length written, or -1 if it would not fit.
*/
int ini_rewrite_core(const char *src, int srclen, char *dst, int dstmax,
	const char *core, const char *key, const char *value, const char *note);

/*
  Back up and write it. Returns 1 when the file changed, 0 when it already said this,
  or -1 with ini_last_error() set.
*/
int ini_apply_core(const char *path, const char *core, const char *key,
	const char *value, const char *note);

/*
  Back up, rewrite, and tell the running firmware. Returns the number of settings
  written, 0 when there was nothing to do, or -1 if nothing was written - in which
  case ini_last_error() says why in one line a player can read.
*/
int ini_apply(const char *path);
const char *ini_last_error();

#endif
