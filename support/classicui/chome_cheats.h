/*
  Classic Home - the running game's cheats.

  The firmware already loads them: user_io.cpp calls cheats_init() on every ROM load
  behind user_io_use_cheats(), and by the time the in-game menu can be opened the store
  is full. What it does not have is a way to *read* that store - cheats.cpp is written
  around the OSD's own cursor and 32-column buffer - so cheats.h grew five accessors
  (see UPSTREAM.md) and everything below is built on those.

  Two jobs live here: folding the list into something a player can walk, and remembering
  what they chose.

  ## The fold, and the measurement behind it

  A cheat pack from GameHacking.org is a zip of flat entries - no folders anywhere on a
  card of 14,766 of them - and the typical game has between one and thirty. Those need no
  help at all. The tail does: Mega Man Battle Network (GBA) carries **31,128** entries,
  Turok (N64) 30,887, SLUS-00940 (PSX) 23,799, and a list that long is not navigable with
  a stick at any speed.

  What makes it foldable is that the pack numbers its repeats. Those 31,128 entries are
  249 distinct names, each appearing as "Sword.gg", "Sword (1).gg" ... "Sword (176).gg".
  The obvious move - throw the duplicates away - is wrong, and it was worth checking
  before building on it: 31,032 of the 31,128 files have *distinct contents*, so the 176
  Swords are 176 different codes that happen to share a label. They can be grouped and
  they cannot be dropped.

  So a group is a run of adjacent entries sharing a stem, and a stem is the name with a
  trailing " (N)" and the ".gg" taken off. Two consequences worth stating:

  - **A group of one is not a group.** A stem that occurs once is an ordinary row that
    toggles, so a twelve-entry pack has zero groups and the screen is exactly a flat
    list. One behaviour, degenerating to flat where the data is flat - rather than a
    second navigation model every game has to pay for.

  - **Adjacency, not a re-sort.** cheats_init() sorts with CheatComp, which compares over
    the shorter length and puts the shorter name first, and that already gathers a stem's
    variants together. This only folds runs it finds, which is also what makes it safe on
    the one store that is *not* sorted - MRA arcade cheats are pushed in file order by
    cheats_add_arcade() - where it simply folds less.

    Two consequences of sorting the name *with* its ".gg" on, both measured against the
    real pack rather than reasoned about, and both pinned by the harness:

    A space is 0x20 and a full stop is 0x2E, so "Sword (176).gg" sorts *before* "Sword.gg".
    The unnumbered entry is therefore the LAST row of its group, not the first, and the
    numbered ones run in text order - (1), (10), (100), (101) ... (2), (20). That is why a
    variant row is labelled with the pack's own number (ch_entry_tag) and never with its
    position in the list: the two disagree almost everywhere.

    And a stem can occur in more than one run. "Have Dex Add.gg" sorts between "Have Dex
    (N).gg" and "Have Dex.gg" - again on 0x20 against 0x2E - which splits "Have Dex" into
    two groups with "Have Dex Add" between them. On the 31,128-entry pack this happens to
    exactly three stems out of 249, giving 252 groups. It is left alone deliberately:
    merging them means holding every stem in a hash, and then reordering rows away from
    the order the store is in, to save a player three duplicate-looking rows in a list of
    252. The cost of the tidier answer is larger than the untidiness.

  ## The memory

  Explicit, not automatic. Toggling a cheat is live and forgotten, exactly as the classic
  OSD has always behaved; the set is only written when the player asks for it. That is the
  same bargain the core-options screen strikes with its per-game overrides, and for the
  same reason - a cheat switched on to look at something should not follow the game around
  for ever.

  Keyed with core_opts_game_key(), the FNV-1a of "<system id>/<relative path>", rather
  than with a hash of this module's own - one definition of "which game", shared, cannot
  drift. Records name their cheat rather than numbering it: a pack update inserts entries
  and shifts every index after them, and re-applying by index would then switch on
  something the player never chose.
*/

#ifndef CHOME_CHEATS_H
#define CHOME_CHEATS_H

#include <inttypes.h>

// The store's own field is 256; the longest name measured on a real card is 47
// ("Infinite Battery Power (Power Plant Network).gg"). A name that would not fit is
// refused rather than truncated - a truncated key matches the wrong cheat, which is
// worse than not remembering it. ch_keep_for_game() says how many it refused.
#define CH_NAME_LEN 128

// Records across every game, shared. CO_PG_MAX's neighbour and deliberately larger:
// one game can legitimately hold a hundred enabled cheats where it can hold only a
// few interesting options.
#define CH_PG_MAX 256

/* ---------------------------------------------------------------- the fold ---- */

/*
  Rebuild the grouping over whatever cheats.cpp currently holds. Cheap on the packs
  that matter and O(n) on the one that does not, so it is called when the screen opens
  and not per frame. Safe with no cheats loaded: everything below then reads as empty.
*/
void ch_build();

// 1 when the fold describes the store as it is now - the store is reloaded wholesale by
// cheats_init() on a ROM load, so a stale fold is a fold over another game's cheats.
int  ch_built_for(int navailable);

int  ch_groups();
int  ch_group_count(int g);              // entries in the group, 1 for an ordinary row
int  ch_group_on(int g);                 // how many of them are switched on
int  ch_group_index(int g, int v);       // store index of variant v, -1 out of range

/*
  The stem, written into the caller's buffer. Not stored: on the 31,128-entry pack that
  would be 31,128 names held to describe 249 groups, and the name is already in the store
  one dereference away.
*/
const char *ch_group_name(int g, char *out, int len);

// The name of one entry with its ".gg" taken off. Same buffer rule.
const char *ch_entry_name(int idx, char *out, int len);

/*
  The pack's own number for an entry - "176" from "Sword (176).gg" - and an empty string
  for the one entry of a group that carries no number at all.

  What a variant row is labelled with, and it has to be this rather than the row's position:
  the store is sorted as text with the extension on, so the group runs (1), (10), (100) ...
  and ends with the unnumbered entry. A row reading "Code 3" that was really the pack's
  "Sword (100)" would be a number the player could not check against any cheat list
  anywhere.
*/
const char *ch_entry_tag(int idx, char *out, int len);

/* -------------------------------------------------------------- the budget ---- */

/*
  A core takes a fixed number of cheat lines (cheat_max_active, 128 unless an MRA says
  otherwise) and a cheat is a whole number of them, so enabling one can simply be
  refused. On a 31,000-entry pack a player will meet that ceiling, and a screen that
  showed nothing happening would look broken rather than full.
*/
int  ch_lines_used();
int  ch_lines_max();

// Returns 1 when the cheat is on afterwards. Compare with what was asked for to tell a
// refusal from a success - see ch_lines_used().
int  ch_set(int idx, int on);

/* ------------------------------------------------------------ the per-game ---- */

/*
  Which game the screen is editing. 0 - no game identified - means the cheats still work
  for this session and nothing can be remembered, and the screen says so rather than
  offering a Keep that would write nothing.
*/
void ch_bind_game(uint32_t key);
uint32_t ch_bound_game();

// How many cheat names the bound game has remembered, 0 when it has none.
int  ch_kept_count();

// 1 when what is remembered is exactly what is switched on, which is what decides
// whether Keep is worth offering at all.
int  ch_kept_matches_live();

/*
  Write the live set as this game's. Returns how many names were stored, or -1 when the
  store is full and nothing was written. Storing an empty set is a legitimate answer and
  is not the same as forgetting the game: it means "this game starts with no cheats on",
  which is what a player who turned one off and kept that is asking for.
*/
int  ch_keep_for_game();

// Forget everything this game remembers. 1 when there was something to forget. Does not
// switch anything off: the session the player is in is theirs, and a Forget that also
// changed the game under them would be two actions on one press.
int  ch_forget_for_game();

/*
  Switch on what the game remembers, after a launch. Returns how many were switched on;
  names the pack no longer has are dropped silently, names the budget refuses are counted
  in the shortfall the caller can report. Binds the game as a side effect - the launch
  path is the one caller that knows which game this is before the screen does.
*/
int  ch_apply_for_game(uint32_t key);

// Only the harness needs this: it packs many sessions into one process, where a new
// firmware process gets a clean store by existing.
void ch_forget_all();

#endif
