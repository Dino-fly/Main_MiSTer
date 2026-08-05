/*
  Classic Home - systems table, game index and shelf views.

  The systems table is what makes the UI game-oriented rather than core-oriented:
  it is the only place a core is named, and the user never sees any of it. The
  built-in defaults cover the common systems; a data file on the SD card
  (classicui_systems.txt) overrides them so index/type values can be corrected
  without rebuilding.

  IMPORTANT: the MGL <file> type and index for each core come from that core's
  CONF_STR (see the MGL docs). The built-in values below are the commonly used
  ones but have NOT been verified against every core on hardware - a wrong index
  loads the file into the wrong slot. Verify per core, or override in the file.
*/

#ifndef CHOME_LIB_H
#define CHOME_LIB_H

#include <inttypes.h>

#define CH_MAX_ITEMS   6000
#define CH_MAX_SYS     64
#define CH_TITLE_LEN   64
#define CH_PATH_LEN    192

/*
  Whether a system's core has save states. Only knowable for certain from a loaded
  core's CONF_STR, which the shelf has not got - so the table carries what has been
  measured, and the three states are kept apart on purpose: a system nobody has
  measured must not be told either way. UNKNOWN is zero, so anything that arrives
  without an answer - a system from classicui_systems.txt - gets that one.
*/
#define CH_SS_UNKNOWN  0
#define CH_SS_YES      1
#define CH_SS_NO       2

struct chome_sys
{
	char id[16];
	char name[40];
	char badge[8];
	char rbf[64];        // e.g. "_Console/SNES", relative to SD root, no datecode
	char dir[32];        // games folder name, e.g. "SNES"
	char ext[64];        // comma separated, lower case
	char lr[80];         // libretro thumbnail system name, for art fetch
	char type;           // MGL file type: 'f' load to memory, 's' mount
	int  index;          // MGL slot index
	int  delay;          // MGL delay in seconds
	int  computer;       // 1: lives under Computers and uses the file browser
	int  mra;            // 1: launch .mra files directly (arcade)
	/*
	  1: the games are romsets, not ROM files - a Neo Geo archive is loaded whole and
	  is named for the board (mslug.zip), so titles come from romsets.xml through the
	  firmware's own lookup rather than from the filename.
	*/
	int  romset;
	int  savestates;     // CH_SS_*: what is known before the core is loaded
	int  vclass;         // VC_* in chome_video.h: picks the default video look
	uint32_t tint;       // fallback-card plate colour
};

#define IT_GAME   0
#define IT_FOLDER 1

struct chome_item
{
	uint8_t  kind;
	int16_t  sysidx;
	uint8_t  slots;              // 2 bits per suspend slot: 0 empty 1 saved 2 locked
	uint8_t  fav;
	uint16_t plays;
	uint32_t key;                // hash of system + path, for the state file
	char     title[CH_TITLE_LEN];
	char     path[CH_PATH_LEN];  // relative to the system's games dir
};

// Views. A view is a flat list of entries drawn as one shelf.
#define VIEW_ROOT      0
#define VIEW_ALL       1
#define VIEW_SYSTEMS   2
#define VIEW_SYS       3
#define VIEW_FAV       4
#define VIEW_COMPUTERS 5
#define VIEW_RECENT    6

#define ENT_GAME   0
#define ENT_FOLDER 1
#define ENT_BROWSE 2   // computer system: opens the file browser

/*
  One entry is one card. For a game that means one *title*, which can be several files:
  see "title groups" in chome_lib.cpp. `game` is the file on show, and every per-game
  thing in the UI - favourite, play count, suspend points, per-game core options, the
  launch itself - reads it, so all of them act on the file the player is looking at
  rather than on the group or on whichever variant happened to be indexed first.

  Downstream code must not assume one entry means one file: walk the variants with
  lib_view_variant() where it matters, and use lib_view_select_key() /
  lib_view_select_path() rather than comparing only against `game`.
*/
struct chome_entry
{
	uint8_t kind;
	int     game;      // index into the item array: the variant currently on show
	int     view;      // target view for folders
	int     sysidx;    // system for the target view / browse
	int     count;     // titles behind a folder, 0 for games
	char    label[CH_TITLE_LEN];
	const char *icon;

	int     nvar;      // files behind this card; 1 for an ordinary game, 0 for a folder
	int     vsel;      // which of them `game` is, 0-based
	int     vhead;     // first item of the chain, in filename order
	uint8_t dup;       // another entry in this view carries the same title
};

#define SORT_RECENT  0
#define SORT_PLAYS   1
#define SORT_TITLE   2
#define SORT_SYSTEM  3
#define SORT_ADDED   4
#define SORT_COUNT   5

const char *lib_sort_name(int sort);

// Loads the systems table and the play-state file. Cheap: no allocation of the
// index, no scanning. The in-game pause menu only needs this much.
void lib_load_systems();

/*
  Everything above plus the index. Loads the cached index when it is still valid
  and skips scanning entirely; otherwise starts the background scan and writes the
  cache when it finishes. The cache is what makes opening the menu inside a game
  instant instead of costing a rescan on every core switch.
*/
void lib_init();

// Forces a fresh scan, ignoring any cache. Options > Rescan Library.
void lib_rescan();

// 1 when this session's index came from the cache rather than a scan.
int  lib_index_cached();

// Advances the background scan by one slice. Returns 1 while still scanning.
int  lib_scan_step();
int  lib_scanning();
int  lib_scan_progress();     // items found so far

int  lib_sys_count();
const chome_sys *lib_sys(int i);

int  lib_item_count();
chome_item *lib_item(int i);

// Builds the entry list for a view. Returns the entry count.
int  lib_view_build(int view, int sysidx, int sort);
int  lib_view_count();
const chome_entry *lib_view_entry(int i);
const char *lib_view_title(int view, int sysidx);

/*
  Title groups: several files sharing one title, drawn as one card the player cycles.

  lib_view_cycle() moves a card to its next file and returns 1 when it moved. The
  entry's `game` follows, so nothing else has to be told.

  lib_view_variant() is the item index of one of a card's files, -1 out of range, and
  lib_view_variant_file() names it the way the title block shows it: the part of the
  path that differs from the other files behind the same card. That one answers into a
  static buffer, like gfx_clip() does, so a second call overwrites the first.
*/
int  lib_view_cycle(int entry, int dir);
int  lib_view_variant(int entry, int which);
const char *lib_view_variant_file(int entry, int which);

/*
  Finds a game anywhere in the view *including behind a card*, selects that variant and
  returns the entry index, or -1. Both callers - coming back to where the player was,
  and parking the shelf on the running game - would otherwise miss a game that is not
  the variant its card happens to be showing.
*/
int  lib_view_select_key(uint32_t key);
int  lib_view_select_path(int sysidx, const char *relpath);

// Suspend-point slots for a game, refreshed from disk. Lock flags come from the
// state file: MiSTer's savestate files have no lock concept, it is ours.
void lib_refresh_slots(chome_item *it);
void lib_set_lock(chome_item *it, int slot, int on);

// Deletes the savestate file backing a slot. Destructive; the UI confirms first.
int  lib_delete_slot(chome_item *it, int slot);

// Absolute path of a slot's thumbnail PNG, written by process_ss() at save time.
int  lib_slot_thumb(const chome_item *it, int slot, char *out, int len);

/*
  Absolute path a slot's state file would occupy, whether or not one is there. For
  writing, where lib_slot_thumb() and the internal lookup both want an existing file.
*/
int  lib_slot_target(const chome_item *it, int slot, char *out, int len);

/*
  Favourites and play counts, persisted in classicui_state.cfg. Noting a play also
  moves the game to the front of the Recently Played list, which is its own file
  (classicui_recent.cfg) because the order is what is stored, not a per-game field.
*/
void lib_toggle_fav(chome_item *it);
void lib_note_play(chome_item *it);
void lib_state_save();

// Full path of a game's containing games dir, and of the game itself.
int  lib_sys_games_dir(int sysidx, char *out, int len);

// Fix a core path ("_Console/MegaCD") for this card's regional naming, in place.
// Returns 1 when it was rewritten. For cores that are not a shelf system's own.
int  lib_resolve_rbf(char *rbf, int size);

#endif
