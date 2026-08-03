/*
  The running core's own options, read out of its CONF_STR.

  Everything here is a view onto data the core publishes and the firmware already
  parses for the classic OSD; nothing is stored. See CORE-OPTIONS.md for the grammar
  and for why each option ends up in the tier it does.
*/

#ifndef CHOME_CORE_H
#define CHOME_CORE_H

#include <stdint.h>

#define CO_MAX        128
#define CO_VALS       16
#define CO_NAME_LEN   40
#define CO_VAL_LEN    24

// Which screen an option belongs on. Tier 0 is "never show": either we own the
// setting ourselves or it is a debug control nobody wants in a front-end.
#define CO_TIER_HIDDEN 0
#define CO_TIER_PICTURE 1
#define CO_TIER_SYSTEM  2
#define CO_TIER_RISKY   3

struct core_opt
{
	char name[CO_NAME_LEN];
	char spec[12];                    // bit spec, as user_io_status_set() wants it
	char page[24];                    // the core's own page name, "" when it has none
	char vals[CO_VALS][CO_VAL_LEN];
	uint8_t nvals;
	uint8_t tier;
	uint8_t disabled;                 // the core says it does not apply right now
	uint8_t unsafe;                   // a value is marked (U): can crash
};

/*
  Re-read the CONF_STR and the core's hide/disable mask. Cheap enough to call when a
  screen opens; not cheap enough for every frame. Returns the number of options that
  survived the mask.
*/
int  core_opts_scan();

int  core_opts_count();
const core_opt *core_opt_at(int i);

// How many are on a tier, and the i-th of them.
int  core_opts_tier_count(int tier);
const core_opt *core_opt_tier_at(int tier, int i);

int  core_opt_value(const core_opt *o);
void core_opt_set(const core_opt *o, int value);

// Writes the core's own config, the same file the classic OSD writes.
void core_opts_save();

/*
  Short name for the running system, for the menu bar. Pass the system index of the
  running game, or -1 when that is not known: the badge is preferred, the core's own name
  is the fallback. Empty when no core is loaded.
*/
const char *core_short_name(int sysidx);

#endif
