/*
  Classic Home - the MiSTer.ini settings this front-end assumes.

  The front-end is opinionated by design, and part of that opinion cannot be held in
  our own state file: it lives in MiSTer.ini, which the firmware re-reads on every
  core load. The settings here are the ones whose absence a player would experience
  as the front-end being broken - a classic-OSD panel appearing over their game.

  Two rules decided what is in the set, and they are worth keeping.

  Only things that surprise a player are removed. A setting that changes how the
  machine *behaves* outside the front-end is not ours to rewrite, however much this
  UI might prefer it: see the rejected list in chome_ini.cpp for what that ruled out
  and why. The set is deliberately short.

  And the file is the player's, not ours. Everything not being set is copied through
  byte for byte - line endings, comments, ordering, unknown keys, sections we have
  never heard of - and the old file is kept beside the new one. MiSTer.ini is CRLF
  and hand-edited; a rewrite that reflowed it would turn every later diff into noise
  and would lose the comments people leave themselves.
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
  Back up, rewrite, and tell the running firmware. Returns the number of settings
  written, 0 when there was nothing to do, or -1 if nothing was written - in which
  case ini_last_error() says why in one line a player can read.
*/
int ini_apply(const char *path);
const char *ini_last_error();

#endif
