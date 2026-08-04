/*
  The running core's own options, read out of its CONF_STR.

  The option table itself is a view onto data the core publishes and the firmware
  already parses for the classic OSD; nothing about it is stored. See CORE-OPTIONS.md
  for the grammar and for why each option ends up in the tier it does.

  What *is* stored is the per-game part at the bottom of this file: which of those
  options a single game keeps its own value for.
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

/* ------------------------------------------------------------- per game ---- */

/*
  Per-game overrides.

  core_opts_save() writes <CORE>.CFG, which is the core's *global* config: a setting
  changed there applies to every game that core will ever load. Plenty of settings do
  not want that. PSX's Widescreen Hack is the example Derek gave - it flatters a
  3D racer and ruins a 2D game on the same core - and the same is true of region,
  dithering and the overclocks.

  So a change made while a known game is running is remembered against that game and
  re-applied the next time it starts, instead of being written into the core's config.
  With no game identified - a core somebody else loaded, so there is nothing to hang
  the choice on - the change stays global, exactly as it was.

  Keyed the way the rest of the front-end keys per-game state: the FNV-1a hash of
  "<system id>/<relative path>", the same string chome_lib.cpp hashes into
  chome_item.key. Stored in classicui_coreopts.cfg, next to the video looks'
  classicui_video.cfg.

  Option *and* value are stored by name rather than by index, which is what
  snacpad.cpp does with the same data and for the same reason: a core update that
  adds an option or a value shifts every index after it, and the front-end would
  then re-apply the wrong setting. A name that no longer exists is simply dropped.
*/
uint32_t core_opts_game_key(int sysidx, const char *relpath);

/*
  Which game the screen is editing. 0 means "none identified", and then changes go to
  the core's global config as before. Set once when the in-game menu opens; the
  screen never has to ask again.
*/
void core_opts_bind_game(uint32_t key);
uint32_t core_opts_bound_game();

// 1 when the bound game keeps its own value for this option.
int  core_opt_per_game(const core_opt *o);

/*
  Remember value for the bound game. global is the value the option held immediately
  before the change, which - the first time an option is overridden - is what the
  core's own config says, and is what core_opt_drop_for_game() puts back.
*/
void core_opt_keep_for_game(const core_opt *o, int value, int global);

/*
  Forget the override and put the shared value back on the core. Returns 1 when there
  was one to forget, so the caller can say "nothing to undo" rather than pretending.
*/
int  core_opt_drop_for_game(const core_opt *o);

/*
  Applies everything a game has remembered to the core that has just booted. Rescans,
  so the option table is left describing the core as it now is. Returns how many
  options were moved.
*/
int  core_opts_apply_for_game(int sysidx, const char *relpath);

/*
  Short name for the running system, for the menu bar. Pass the system index of the
  running game, or -1 when that is not known: the badge is preferred, the core's own name
  is the fallback. Empty when no core is loaded.
*/
const char *core_short_name(int sysidx);

#endif
