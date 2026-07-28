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

struct chome_entry
{
	uint8_t kind;
	int     game;      // index into the item array
	int     view;      // target view for folders
	int     sysidx;    // system for the target view / browse
	int     count;     // games behind a folder, 0 for games
	char    label[CH_TITLE_LEN];
	const char *icon;
};

#define SORT_RECENT  0
#define SORT_PLAYS   1
#define SORT_TITLE   2
#define SORT_SYSTEM  3
#define SORT_ADDED   4
#define SORT_COUNT   5

const char *lib_sort_name(int sort);

void lib_init();

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

// Suspend-point slots for a game, refreshed from disk. Lock flags come from the
// state file: MiSTer's savestate files have no lock concept, it is ours.
void lib_refresh_slots(chome_item *it);
void lib_set_lock(chome_item *it, int slot, int on);

// Deletes the savestate file backing a slot. Destructive; the UI confirms first.
int  lib_delete_slot(chome_item *it, int slot);

// Absolute path of a slot's thumbnail PNG, written by process_ss() at save time.
int  lib_slot_thumb(const chome_item *it, int slot, char *out, int len);

// Favourites and play counts, persisted in classicui_state.cfg.
void lib_toggle_fav(chome_item *it);
void lib_note_play(chome_item *it);
void lib_state_save();

// Full path of a game's containing games dir, and of the game itself.
int  lib_sys_games_dir(int sysidx, char *out, int len);

#endif
