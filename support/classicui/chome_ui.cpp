#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>

#include "chome.h"
#include "chome_gfx.h"
#include "chome_theme.h"
#include "chome_lib.h"
#include "chome_core.h"
#include "chome_art.h"
#include "chome_gamelist.h"
#include "chome_video.h"
#include "chome_icons32.h"
#include "chome_icons16.h"
#include "chome_btn12.h"
#include "chome_osk.h"
#include "chome_net.h"
#include "chome_disc.h"
#include "chome_bt.h"
#include "chome_ini.h"
#include "chome_opt.h"

#include "../../cfg.h"
#include "../../user_io.h"
#include "../../recent.h"
#include "../../input.h"
#include "../../osd.h"
#include "../../video.h"
#include "../../hardware.h"
#include "../../file_io.h"
#include "../../menu.h"
#include "../arcade/mra_loader.h"
#include "../physical_disc/physical_disc.h"
#include "../../scaler.h"
#include "../../audio.h"
#include "../../fpga_io.h"

#define CURRENT_FILE "/tmp/classicui_current"

/* ------------------------------------------------------------- in-game ---- */

static int ig_active = 0;
static int ig_fb = 1;                 // framebuffer we page-flip in-game
static int ig_have_item = 0;
static chome_item ig_item;            // the running game, from CURRENT_FILE

static uint32_t *ig_shot = 0;         // live capture, core resolution
static int ig_shot_w = 0, ig_shot_h = 0;

static uint32_t *ig_bg = 0;           // capture scaled to the canvas and dimmed
static int ig_bg_w = 0, ig_bg_h = 0;

static int ig_paused = 0;                    // the core is actually halted
static int ig_frozen = 0;                    // held still by a state instead of a pause
static unsigned long ig_freeze_at = 0;       // wall clock when it was, so a stale state is not mistaken for it

/*
  A save the player has asked for that is waiting on the core to write the held state.
  Declared here because the legend and the slot strip are drawn further up the file and
  both have to say so - see chome_pend_poll() for what it is waiting on.
*/
static int pend_slot = -1;
static int pend_failed = -1;                 // slot whose save gave up, for one message
static int ig_selected_running = 0;          // shelf parked on the running game
/*
  The in-game menu's own "nothing is in memory yet". Separate from first_entry
  because on the device the two never live in the same process: the menu core's
  shelf is one MiSTer, and a game core's is the one that re-exec'd over it, so each
  has to read the session record once for itself.
*/
static int ig_first_open = 1;
static unsigned long ig_close_until = 0;     // "press A again to close the game"
static int wifi_row = 0;                     // which network is picked
static int wifi_top = 0;                     // first one on screen
static char wifi_pick[NET_SSID];             // ...and its name, kept across the keyboard

static int pwr_row = 0;                      // Restart / Shut Down
static int pwr_arm = -1;                     // ...and which one is one press from happening
static unsigned long pwr_until = 0;

/*
  Best Settings: the MiSTer.ini keys this front-end assumes, and what writing
  them would change. Read from the card when the screen that shows it is opened -
  never per frame, because the answer only moves when we move it.
*/
static ini_change ini_list[INI_WANT_MAX];
static int ini_n = 0;                        // settings that disagree with the file
static int ini_armed = 0;                    // one press from rewriting the file
static unsigned long ini_until = 0;
static int ini_wrote = -1;                   // -1 idle, >= 0 how many were written, < -1 failed
static int ini_needs_restart = 0;            // ...and whether that is enough on its own

// Re-reads the ini. Cheap enough to do on every entry to a screen that shows it, and
// nowhere near cheap enough to do per frame.
static void ini_refresh()
{
	ini_n = ini_plan(ini_path(), ini_list, INI_WANT_MAX);
	ini_armed = 0;
	ini_wrote = -1;
}

/*
  More Settings: the curated slice of MiSTer.ini a player is allowed to edit. The table
  and the file handling are chome_opt.cpp's; what lives here is where the cursor is and
  the two-press arming, the same as every other screen that writes something.

  The view is rebuilt on entry rather than per frame - it depends on the video path,
  which cannot change while a menu is up over it - and the row indices below index the
  view, not the table, so a hidden option never has a row.
*/
static int set_view[OPT_MAX];
static int set_nview = 0;
static int set_row = 0;
static int set_top = 0;                      // first row drawn, for a list that scrolls
static int set_arm = 0;                      // one press from writing
static unsigned long set_arm_until = 0;
static int set_quit_arm = 0;                 // ...and one from throwing the edits away
static unsigned long set_quit_until = 0;
static int set_wrote = -1;                   // -1 nothing written yet, else how many
static int set_failed = 0;
static int set_odd = 0;                      // options not at their recommended value

static int pads_row = 0;                     // which controller is picked
static int pads_forget_arm = -1;             // ...and whether forgetting it is armed
static unsigned long pads_forget_until = 0;

/*
  One row of the Controllers screen, wireless or not. Declared here rather than beside
  the screen because the legend is built further up the file and asks what is selected.
*/
#define PADS_MAX (BT_MAX + 6)

struct pad_row
{
	int  player;                     // 0 when paired but not connected
	int  kind;                       // PAD_*
	int  connected;
	int  is_add;                     // the "Add a Controller" entry, always last
	uint16_t vid, pid;               // 0 for a paired pad that is not connected
	char name[64];
	char mac[24];                    // Bluetooth only
};

static int pads_build(pad_row *out, int max);
static int pads_sel(pad_row *out);
static int pads_count();

/*
  The controller tester holds on to what *names* a pad, not to the row it was opened
  from. A pad that was asleep when the player picked it wakes up when they press a
  button on it - which is the whole point of the screen - and is a different row by
  then, with a player number it did not have. So the row is looked up again every
  frame and only the identity survives.
*/
static char padtest_name[64];
static char padtest_mac[24];
static int  padtest_kind = PAD_WIRED;
static int  padtest_back_arm = 0;               // B once lights B; B again leaves
static unsigned long padtest_back_until = 0;
static unsigned padtest_seen = 0;               // signature of the live state on screen
static int wifi_pick_secure = 0;
static unsigned wifi_seen = 0;               // signature of the network state on screen

// Which screen is waiting for the text the keyboard is collecting.
#define OSKD_NONE 0
#define OSKD_WIFI 1
static int osk_dest = OSKD_NONE;

static int ig_muted = 0;                     // game silenced while the menu is up
static int ig_mute_was = 0;                  // ...and what it was before, so his own mute survives

#define REF_DELAY_MS 20000
// Long enough for the core to have the ROM in before a state lands on top of it.
#define RESUME_DELAY_MS 4000

/*
  Reference frame for the look previews: "classicui/refshots/<system>/<rom>.png".
  A real, unfiltered capture of the game, taken in the game core. The looks are
  applied over it in software - see the note on vp_preview().
*/
static void ref_shot_path(const char *sysid, const char *rompath, char *out, int len)
{
	const char *fn = strrchr(rompath, '/');
	fn = fn ? fn + 1 : rompath;

	char base[CH_PATH_LEN];
	snprintf(base, sizeof(base), "%s", fn);
	char *dot = strrchr(base, '.');
	if (dot) *dot = 0;

	snprintf(out, len, "%s/classicui/refshots/%s/%s.png", getRootDir(), sysid, base);
}

/* --------------------------------------------------------------- state ---- */

#define SCR_HOME    0
#define SCR_MENUBAR 1
#define SCR_SUSPEND 2
#define SCR_SORT    3
#define SCR_DISPLAY 4
#define SCR_OPTIONS 5
#define SCR_ABOUT   6
#define SCR_BROWSE  8
#define SCR_LAUNCH  9
#define SCR_WIFI    10
#define SCR_PADS    11
#define SCR_POWER   12
#define SCR_INI     13
#define SCR_PADTEST 14
#define SCR_SET     15
#define SCR_CORE    16
#define SCR_DISC    17
#define SCR_DISCBAR 18

// Rows on the Options panel. Several places step over them.
/*
  Options has one more row inside a game than on the shelf. Both end with a way out -
  Advanced Settings hands the shelf to the classic menu, Close Game puts the game away -
  but in a game there is also Core Settings, which is the only route to the options that
  belong to the core itself rather than to this front-end. A player reported that as the
  one thing the front-end had taken away from them, and they were right: widescreen on
  PSX, or a core's own video and audio settings, live in the classic OSD and nowhere
  else, and the OSD is only reachable while that core is running.
*/
#define OPT_ROWS_MENU 9
#define OPT_ROWS_GAME 10
#define OPT_ROWS    9

/*
  Savestate slots.

  The framework in sys/ gives a core four, and the last one is not the player's: it
  is where suspend and freeze put the moment you walked away from, because the core
  cannot really be paused while this menu is up (see freeze_engage). That slot is
  plumbing, so the strip, the shelf pips and the slot numbering stop at three and it
  is never drawn, counted or offered.

  A core declaring fewer than four slots reserves its own last one - susp_slot() -
  which is what actually gets written; the strip is a fixed four wide and has always
  shown slots such a core does not have, which is cosmetic and predates this.
*/
/*
  How much of the frame a look preview shows, as a percentage of its width.

  Fitting the whole frame into a tile a couple of hundred pixels wide destroys the very
  thing these looks change: a scanline, a shadow mask and a soft filter all live at the
  scale of single pixels, and squeezed down to fit they average out into a faint tint
  that looks the same for every preset. Showing the middle of the frame instead trades
  away picture the player already knows for the detail they are choosing between.
*/
#define VP_ZOOM_PCT   45

/*
  The shape a captured frame is meant to be shown in - see shot_fit(). 4:3 because that is
  what every console core here is, and what MiSTer's own "Original" aspect gives them.
*/
#define SHOT_AR_W 4
#define SHOT_AR_H 3

#define CH_SLOTS      4
#define CH_SLOTS_USER 3

#define MB_DISPLAY  0
#define MB_OPTIONS  1
#define MB_POWER    2
#define MB_ABOUT    3
#define MB_CORE     4
#define MB_COUNT    5

/*
  Language and Manuals are gone. The first opened a panel with nothing behind it,
  and the second only handed the screen to the classic OSD - which is exactly what
  the front-end is not supposed to do on its own.
*/
static const char *mb_label[MB_COUNT] = { "Display", "Options", "Power", "About", "Core" };

/*
  The core entry is labelled with the running system rather than the word "Core": a player
  looking for the PlayStation's widescreen hack is looking for "PSX". Defined further down,
  where the running game's identity is in scope.
*/
static const char *mb_text(int i);

/*
  Every Display option lives in the scaler - filters, shadow mask, gamma - so the
  whole entry is dropped when the scaler's output is not what reaches the screen:
  direct_video, or an analog-only setup without vga_scaler. Showing a CRT filter
  picker to somebody already looking at a real CRT would be daft.
*/
static int mb_visible(int i)
{

	/*
	  The core's own options, which only exist while a core is running - and only if it
	  published something we would offer. A core with nothing but debug toggles gets no
	  entry rather than an empty screen.
	*/
	if (i == MB_CORE)
	{
		if (!ig_active) return 0;
		return core_opts_tier_count(CO_TIER_PICTURE)
			|| core_opts_tier_count(CO_TIER_SYSTEM)
			|| core_opts_tier_count(CO_TIER_RISKY);
	}

	if (i != MB_DISPLAY) return 1;

	// Nothing in Display applies when the scaler is bypassed.
	if (!video_scaler_is_visible()) return 0;

	/*
	  Nor is it worth showing at 240p: a canvas that small means an analog CRT in
	  practice, and even on HDMI the preview tiles come out around 66px wide, which
	  is far too small to judge a filter by. The looks still apply from their class
	  defaults; force classicui_profile=1 if you want the screen back.
	*/
	if (theme_get()->id == PROF_LO) return 0;

	return 1;
}

static int mb_count_visible()
{
	int n = 0;
	for (int i = 0; i < MB_COUNT; i++) if (mb_visible(i)) n++;
	return n;
}

// Maps a slot in the visible bar to a menu-bar entry.
static int mb_at(int slot)
{
	int n = 0;
	for (int i = 0; i < MB_COUNT; i++)
	{
		if (!mb_visible(i)) continue;
		if (n == slot) return i;
		n++;
	}
	return MB_OPTIONS;
}

#define NAV_DEPTH 8

struct nav_rec { int view, sysidx, sel; };

static int inited = 0;
static int first_entry = 1;
static int active = 0;
static int screen = SCR_HOME;

static int view = VIEW_ROOT;
static int viewsys = -1;
static int sel = 0;
static double selF = 0;
static int sort_mode = SORT_TITLE;

static nav_rec navstack[NAV_DEPTH];
static int navdepth = 0;

/*
  Where the player was. All of the above is in memory, and launching a game re-execs
  MiSTer, so without this the shelf comes back on the unfiltered root - a long walk
  back to whichever system you were browsing.

  The selected entry is remembered by its key rather than its index: a rescan can
  move a game up or down the list, and an index would then land on a neighbour.
*/
struct session_rec
{
	uint32_t magic;
	int view, viewsys, sort_mode;
	uint32_t sel_key;                 // 0 when the selection was not a game
	int sel_idx;                      // fallback for folders and system cards
	int navdepth;
	nav_rec nav[NAV_DEPTH];
};

#define SESSION_MAGIC 0x53484348u     // "CHHS"
#define SESSION_FILE  "classicui_session.cfg"

static void session_save()
{
	session_rec r;
	memset(&r, 0, sizeof(r));
	r.magic = SESSION_MAGIC;
	r.view = view;
	r.viewsys = viewsys;
	r.sort_mode = sort_mode;
	r.sel_idx = sel;
	r.navdepth = navdepth;
	for (int i = 0; i < navdepth && i < NAV_DEPTH; i++) r.nav[i] = navstack[i];

	const chome_entry *e = lib_view_entry(sel);
	if (e && e->kind == ENT_GAME)
	{
		chome_item *it = lib_item(e->game);
		if (it) r.sel_key = it->key;
	}

	FileSaveConfig(SESSION_FILE, &r, sizeof(r));
}

/*
  Rebuilds the view as it was. Returns 0 when there is nothing usable saved - the
  first boot, or a stale record - and the caller then has to build a view itself:
  leaving without doing either is how the shelf came up empty with a full index.
*/
static int session_restore()
{
	session_rec r;
	memset(&r, 0, sizeof(r));

	if (FileLoadConfig(SESSION_FILE, &r, sizeof(r)) != (int)sizeof(r)) return 0;
	if (r.magic != SESSION_MAGIC) return 0;
	if (r.view < 0 || r.view > VIEW_RECENT) return 0;
	if (r.viewsys >= lib_sys_count()) return 0;

	view = r.view;
	viewsys = r.viewsys;
	sort_mode = r.sort_mode;
	navdepth = (r.navdepth >= 0 && r.navdepth <= NAV_DEPTH) ? r.navdepth : 0;
	for (int i = 0; i < navdepth; i++) navstack[i] = r.nav[i];

	lib_view_build(view, viewsys, sort_mode);

	int n = lib_view_count();
	sel = 0;

	/*
	  Through the view rather than over the entries, so a game that is one of several
	  files behind one card is still found - and the card comes back showing *that* file.
	  This is the only thing that remembers a cycled card across the re-exec a launch
	  performs, which is the case that matters: choose the European dump, play it, and
	  the shelf is still on the European dump when the menu comes back.
	*/
	if (r.sel_key)
	{
		int at = lib_view_select_key(r.sel_key);
		if (at >= 0) sel = at;
	}
	if (!sel && r.sel_idx > 0 && r.sel_idx < n) sel = r.sel_idx;

	selF = sel;
	printf("ClassicUI: back where you were - view %d, entry %d of %d\n", view, sel + 1, n);
	return 1;
}

static int mb_idx = 0;
static int slot_idx = 0;
static int sort_idx = 0;
static int opt_row = 0;
static int look_row = 0;
static int co_row = 0;                       // the core-options list
static int co_tier = CO_TIER_PICTURE;

static double bar_y = 0, strip_y = 0, curtain = 0;

// The last position an activity indicator was painted at, so a busy screen repaints
// when the ring moves and not once per frame. See ui_busy().
static unsigned long anim_seen = 0;
static unsigned long launch_at = 0;
static unsigned long nudge_until = 0;
static unsigned long last_ms = 0;

static int dirty = 1;

static uint32_t last_key = 0;
static int key_run = 0;

// Set once the user hands off to the classic menu, so we do not immediately
// grab the screen back. The OSD/menu button brings us back.
static int handed_off = 0;
/*
  The in-game equivalent: Core Settings has given the screen to the classic OSD, so the
  menu button belongs to that until it comes back.

  A flag rather than asking menu_present(), which was the first attempt and was wrong twice
  over. Too broad: the classic menu's state machine is briefly busy after a savestate load,
  and standing down then sent the player to the classic OSD when they wanted us. And it
  cannot help with the other half of the problem - menu_key_set(KEY_F12) is read by *this*
  handler before menu.cpp ever sees it, so the key that was meant to open the OSD just
  reopened our own menu instead.
*/
#define OSDH_NONE    0
#define OSDH_WAITING 1      // asked for the OSD, it has not appeared yet
#define OSDH_UP      2      // it is on screen; the button is its until it closes
static int osd_handoff = OSDH_NONE;
static unsigned long osd_handoff_until = 0;

// Two-press confirmation for deleting a suspend point.
static int del_arm_slot = -1;
static unsigned long del_arm_until = 0;

// Browser state for computer systems.
#define BROWSE_MAX 512
struct browse_ent { char name[128]; int isdir; };
static browse_ent bent[BROWSE_MAX];
static int nbent = 0;
static int browse_sel = 0, browse_top = 0;
static int browse_sys = -1;
static char browse_rel[CH_PATH_LEN] = {};


static void mark_dirty() { dirty = 1; }

static const uint32_t *ig_live_ref(int w, int h);
static void ig_close(int restore_video);
static int user_slots();
static void ig_select_running();
static int ss_can_save();
static int susp_matches(const chome_item *it);
static int ig_load_item();
static void quit_to_home(int suspend);
static int ss_can_load();
static void draw_running_warning(const chome_profile *p);
static int susp_arm(const chome_item *it, int slot);
static void core_opts_save_unpaused();
static int ss_do_save(int slot);
static int ss_do_load(int slot);
static void ss_pause_release(int engaged);
static int ss_pause_engage();

/* ------------------------------------------------------------- helpers ---- */

static const chome_entry *cur_entry()
{
	return lib_view_entry(sel);
}

static chome_item *cur_game()
{
	const chome_entry *e = cur_entry();
	if (!e || e->kind != ENT_GAME) return 0;
	return lib_item(e->game);
}

/*
  Video class of a game. Refined by extension: a .gbc cartridge runs in the Game
  Boy core but is a Game Boy Color game, and should be offered GBC and GBA screens
  rather than DMG ones. The Video Look panel and the launch path both go through
  here so they can never disagree.
*/
static int class_of(int sysidx, const char *path)
{
	const chome_sys *s = lib_sys(sysidx);
	if (!s) return VC_CONSOLE;

	const char *ext = path ? strrchr(path, '.') : 0;
	if (ext) ext++;

	if (ext)
	{
		// Cores that host more than one machine are told apart by extension.
		if (s->vclass == VC_GB && !strcasecmp(ext, "gbc")) return VC_GBC;
		if (s->vclass == VC_CONSOLE && !strcasecmp(ext, "gg")) return VC_GG;
		if (s->vclass == VC_WS && !strcasecmp(ext, "wsc")) return VC_WSC;
	}

	return s->vclass;
}

// Class of whatever is selected on the shelf, for the Display and Look panels.
static int sel_class()
{
	chome_item *it = cur_game();
	if (!it) return VC_CONSOLE;
	return class_of(it->sysidx, it->path);
}

/*
  Best real frame we have of this game, for the look previews:
    1. the reference shot captured while it was running
    2. failing that, a suspend-point thumbnail, which is also a real capture
  Returns 0 when we have neither and the synthetic pattern should be used.
*/
static int ref_shot_for(const chome_item *it, char *out, int len)
{
	if (!it) return 0;

	const chome_sys *sy = lib_sys(it->sysidx);
	if (!sy) return 0;

	struct stat st;

	ref_shot_path(sy->id, it->path, out, len);
	if (!stat(out, &st) && S_ISREG(st.st_mode)) return 1;

	for (int n = 0; n < 4; n++)
	{
		if (!lib_slot_thumb(it, n, out, len)) continue;
		if (!stat(out, &st) && S_ISREG(st.st_mode)) return 1;
	}

	out[0] = 0;
	return 0;
}

// True when the given item is the game currently running behind the menu.
static int ig_is_running(const chome_item *it)
{
	if (!ig_active || !ig_have_item || !it) return 0;
	return (it->sysidx == ig_item.sysidx) && !strcmp(it->path, ig_item.path);
}

/*
  True when this game's core is known to have no save states, so the strip can say so
  instead of offering slots that can never fill.

  Two sources, in this order on purpose. Inside its own running core the CONF_STR is
  the truth and the table is not consulted at all: a core rebuilt from a newer upstream
  can gain save states, and a stale "no" in chome_lib would then be a lie about the very
  core that is answering. From the shelf there is no core loaded to ask, so the measured
  table is all there is - and a system nobody measured (CH_SS_UNKNOWN) is left alone,
  which shows the slots as before rather than guessing.
*/
static int no_savestates_for(const chome_item *it)
{
	if (!it) return 0;
	if (ig_is_running(it)) return !ss_can_save() && !ss_can_load();

	const chome_sys *s = lib_sys(it->sysidx);
	return (s && s->savestates == CH_SS_NO);
}

static int slot_state(const chome_item *it, int n)
{
	if (!it) return 0;
	return (it->slots >> (n * 2)) & 3;
}

static void nudge()
{
	nudge_until = GetTimer(160);
	mark_dirty();
}

/*
  Keeps the shelf pips honest: re-stat the selected game's savestate slots whenever the
  selection moves. Four stat() calls, only on change.

  The game index is part of what "moved" means, not just the shelf position: cycling a
  grouped card to another file leaves sel and view alone while putting a different ROM -
  with its own savestates - under the cursor.
*/
static void sync_sel_slots()
{
	static int last_sel = -1;
	static int last_view = -1;
	static int last_game = -1;

	const chome_entry *ce = lib_view_entry(sel);
	int game = (ce && ce->kind == ENT_GAME) ? ce->game : -1;

	if (sel == last_sel && view == last_view && game == last_game) return;
	last_sel = sel;
	last_view = view;
	last_game = game;

	del_arm_slot = -1;

	chome_item *it = cur_game();
	if (it) lib_refresh_slots(it);
	mark_dirty();
}

static void view_rebuild(int keep_sel)
{
	int old = sel;
	lib_view_build(view, viewsys, sort_mode);
	int n = lib_view_count();
	sel = keep_sel ? old : 0;
	if (sel >= n) sel = n ? n - 1 : 0;
	if (sel < 0) sel = 0;
	selF = sel;
	mark_dirty();
}

static void nav_push(int v, int s)
{
	if (navdepth < NAV_DEPTH)
	{
		navstack[navdepth].view = view;
		navstack[navdepth].sysidx = viewsys;
		navstack[navdepth].sel = sel;
		navdepth++;
	}
	view = v;
	viewsys = s;
	view_rebuild(0);
}

static int nav_pop()
{
	if (!navdepth) return 0;
	navdepth--;
	view = navstack[navdepth].view;
	viewsys = navstack[navdepth].sysidx;
	lib_view_build(view, viewsys, sort_mode);
	sel = navstack[navdepth].sel;
	if (sel >= lib_view_count()) sel = lib_view_count() ? lib_view_count() - 1 : 0;
	selF = sel;
	mark_dirty();
	return 1;
}

/* ------------------------------------------------------------ pictograms -- */

/*
  Badges and prompts. Not drawn here either: see chome_icons16.h, which is generated
  from the same licensed sets as the system icons.

  Sampled rather than scaled by whole pixels, for the same reason as draw_sysicon:
  the box available differs between profiles and an integer scale would be too small
  on one and clipped on the other.
*/
static void picto(const char *name, int x, int y, int box, uint32_t col)
{
	if (box < 4) return;

	/*
	  Two sizes of the same shape, and the small box takes the small one. Sampling a 16x16
	  mask down to 8 pixels drops every other row and column, which a filled silhouette
	  survives and a one-pixel outline does not - an outlined triangle reduced that way
	  loses two of its three sides. See outline() in tools/icons32.py.
	*/
	if (box < ICON16)
	{
		for (size_t i = 0; i < sizeof(pictos8) / sizeof(pictos8[0]); i++)
		{
			if (strcmp(pictos8[i].name, name)) continue;

			const picto8_def *d8 = &pictos8[i];
			for (int oy = 0; oy < box; oy++)
			{
				const char *row = d8->rows[oy * ICON8 / box];
				for (int ox = 0; ox < box; ox++)
				{
					if (row[ox * ICON8 / box] == '#') gfx_fill(x + ox, y + oy, 1, 1, col);
				}
			}
			return;
		}
	}

	const picto_def *d = 0;
	for (size_t i = 0; i < sizeof(pictos) / sizeof(pictos[0]); i++)
	{
		if (!strcmp(pictos[i].name, name)) { d = &pictos[i]; break; }
	}
	if (!d) return;

	for (int oy = 0; oy < box; oy++)
	{
		const char *row = d->rows[oy * ICON16 / box];
		for (int ox = 0; ox < box; ox++)
		{
			if (row[ox * ICON16 / box] == '#') gfx_fill(x + ox, y + oy, 1, 1, col);
		}
	}
}

/*
  A padlock, from MiSTer's own OSD font rather than from rectangles: the ROM has a
  closed one at 0x17 and an open one at 0x18, and using them means one less drawing
  of mine on screen.
*/
#define CH_LOCK   "\x17"
#define CH_UNLOCK "\x18"

/*
  Per-system icon, if this system has one. Sampled rather than scaled by whole
  pixels: the box a card can spare is 60-odd pixels at HD but barely 20 at 240p, so
  an integer scale would be either too small on one or clipped on the other.
*/
static const sysicon_def *sysicon_find(const char *id)
{
	if (!id || !id[0]) return 0;
	for (size_t i = 0; i < sizeof(sysicons) / sizeof(sysicons[0]); i++)
	{
		if (!strcasecmp(sysicons[i].id, id)) return &sysicons[i];
	}
	return 0;
}

static void draw_sysicon(const sysicon_def *d, int x, int y, int box, uint32_t col)
{
	if (box < 4) return;

	for (int oy = 0; oy < box; oy++)
	{
		const char *row = d->rows[oy * ICON_SYS / box];
		for (int ox = 0; ox < box; ox++)
		{
			if (row[ox * ICON_SYS / box] == '#') gfx_fill(x + ox, y + oy, 1, 1, col);
		}
	}
}

/* ------------------------------------------------------------- the card --- */

static void draw_fallback_card(const chome_item *it, int x, int y, int w, int h)
{
	const chome_sys *s = lib_sys(it->sysidx);
	uint32_t tint = s ? s->tint : COL_PANELLO;
	int u = h / 24; if (u < 1) u = 1;

	gfx_fill(x, y, w, h, tint);
	gfx_scrim(x, y, w, h, COL_SHADOW, 4);
	gfx_frame_rect(x, y, w, h, COL_PANELLO, 1);

	int ts = (w > 180) ? 2 : 1;
	int maxc = (w - 6 * u) / (GLYPH_W * ts);
	if (maxc < 4) maxc = 4;

	// Wrap the title on word boundaries.
	char words[8][CH_TITLE_LEN];
	int nw = 0;
	{
		char tmp[CH_TITLE_LEN];
		snprintf(tmp, sizeof(tmp), "%s", it->title);
		char *p = tmp;
		while (*p && nw < 8)
		{
			while (*p == ' ') p++;
			if (!*p) break;
			char *st = p;
			while (*p && *p != ' ') p++;
			int len = (int)(p - st);
			if (len > CH_TITLE_LEN - 1) len = CH_TITLE_LEN - 1;
			memcpy(words[nw], st, len);
			words[nw][len] = 0;
			nw++;
		}
	}

	char lines[5][CH_TITLE_LEN];
	int nl = 0;
	lines[0][0] = 0;
	for (int i = 0; i < nw && nl < 5; i++)
	{
		char cand[CH_TITLE_LEN * 2];
		if (lines[nl][0]) snprintf(cand, sizeof(cand), "%s %s", lines[nl], words[i]);
		else snprintf(cand, sizeof(cand), "%s", words[i]);

		if ((int)strlen(cand) > maxc && lines[nl][0])
		{
			nl++;
			if (nl >= 5) break;
			snprintf(lines[nl], CH_TITLE_LEN, "%s", words[i]);
		}
		else
		{
			snprintf(lines[nl], CH_TITLE_LEN, "%s", cand);
		}
	}
	nl++;

	int lh = 10 * ts;
	int ty = y + (h - nl * lh) / 2 - 2 * u;
	for (int i = 0; i < nl; i++)
	{
		char up[CH_TITLE_LEN];
		snprintf(up, sizeof(up), "%s", lines[i]);
		for (char *p = up; *p; p++) *p = (char)toupper((unsigned char)*p);
		gfx_text_c(gfx_clip(up, ts, w - 4), x + w / 2, ty + i * lh, ts, COL_WHITE, COL_SHADOW);
	}

	if (s)
	{
		int bw = gfx_text_w(s->badge, 1) + 4;
		gfx_fill(x + 2 * u, y + h - 2 * u - 11, bw, 11, COL_SHADOW);
		gfx_text(s->badge, x + 2 * u + 2, y + h - 2 * u - 9, 1, COL_PANELHI, 0);
	}
}

static void draw_card(const chome_entry *e, int cx, int bottom, int w, int h, int selected)
{
	int x = cx - w / 2, y = bottom - h;
	int sd = h / 40; if (sd < 2) sd = 2;

	gfx_fill(x + sd, y + sd, w, h, COL_SHADOW);

	if (e->kind != ENT_GAME)
	{
		gfx_fill(x, y, w, h, COL_PANEL);
		int hh = h * 15 / 100; if (hh < 6) hh = 6;
		gfx_fill(x, y, w, hh, COL_PANELLO);
		gfx_frame_rect(x, y, w, h, COL_PANELLO, 1);

		/*
		  Bands, so the icon can never land on the label: header, then the icon
		  centred in the space above the text, then the label, then the count.
		*/
		int fs = (w > 180) ? 2 : 1;
		int label_y = y + h - (h * 30 / 100);
		int icon_top = y + hh;
		int icon_space = label_y - icon_top - 2 * fs;

		int is = icon_space / 10;                 // 8 rows plus breathing room
		if (is < 1) is = 1;
		if (is * 8 > w / 3) is = (w / 3) / 8;     // never wider than a third
		if (is < 1) is = 1;

		const sysicon_def *si = sysicon_find(e->icon);
		if (si)
		{
			int box = icon_space;
			if (box > w / 2) box = w / 2;
			draw_sysicon(si, x + (w - box) / 2, icon_top + (icon_space - box) / 2, box, COL_INK);
		}
		else
		{
			// A folder, or a system this build has never heard of.
			const sysicon_def *fb = sysicon_find("folder");
			if (fb)
			{
				int box = icon_space;
				if (box > w / 2) box = w / 2;
				draw_sysicon(fb, x + (w - box) / 2, icon_top + (icon_space - box) / 2, box, COL_INK);
			}
		}

		char up[CH_TITLE_LEN];
		snprintf(up, sizeof(up), "%s", e->label);
		for (char *p = up; *p; p++) *p = (char)toupper((unsigned char)*p);
		gfx_text_c(gfx_clip(up, fs, w - 8), x + w / 2, label_y, fs, COL_INK, 0);

		if (h > 80)
		{
			char cnt[32];
			snprintf(cnt, sizeof(cnt), "%d", e->count);
			gfx_text_c(e->count ? cnt : "", x + w / 2, label_y + 11 * fs, 1, COL_PANELLO, 0);
		}
	}
	else
	{
		chome_item *it = lib_item(e->game);
		if (!it) return;

		int aw = 0, ah = 0;
		const uint32_t *art = art_get(e->game, &aw, &ah);
		if (art) gfx_blit(art, aw, ah, x, y, w, h);
		else if (art_state(e->game) == ART_MISSING) draw_fallback_card(it, x, y, w, h);
		else
		{
			// Still loading: plate plus badge, so the shelf never shows a hole.
			const chome_sys *s = lib_sys(it->sysidx);
			gfx_fill(x, y, w, h, s ? s->tint : COL_PANELLO);
			gfx_scrim(x, y, w, h, COL_SHADOW, 2);
			gfx_frame_rect(x, y, w, h, COL_PANELLO, 1);
		}

		// Cover title band, like the front of a real box.
		if (art)
		{
			int bs = (w > 180) ? 2 : 1;
			int bandh = 9 * bs + 5;
			gfx_blend(x, y + h - bandh, w, bandh, COL_SHADOW, 190);
			char up[CH_TITLE_LEN];
			snprintf(up, sizeof(up), "%s", it->title);
			for (char *p = up; *p; p++) *p = (char)toupper((unsigned char)*p);
			gfx_text_c(gfx_clip(up, bs, w - 4), x + w / 2, y + h - bandh + 3, bs, COL_PANELHI, 0);
		}

		if (it->fav)
		{
			int box = 16;
			gfx_fill(x + w - box - 6, y + 4, box + 4, box + 4, COL_SHADOW);
			picto("star", x + w - box - 4, y + 6, box, COL_YELLOW);
		}
	}

	if (selected)
	{
		gfx_frame_rect(x - 2, y - 2, w + 4, h + 4, COL_FOCUS, 2);
	}
	else
	{
		gfx_scrim(x, y, w, h, COL_BGDARK, 2);
		gfx_frame_rect(x, y, w, h, COL_SHADOW, 1);
	}
}

/* ------------------------------------------------------------ the shelf --- */

static void request_visible_art(const chome_profile *p)
{
	int n = lib_view_count();
	int span = p->visible + 2;
	for (int d = 0; d <= span; d++)
	{
		for (int sgn = 0; sgn < 2; sgn++)
		{
			int i = sel + (sgn ? -d : d);
			if (i < 0 || i >= n) continue;
			const chome_entry *e = lib_view_entry(i);
			if (e && e->kind == ENT_GAME) art_request(e->game, d);
			if (!d) break;
		}
	}
}

static void draw_shelf(const chome_profile *p)
{
	gfx_fill(0, p->y_shelf + 1, p->w, 1, COL_GRID);

	int n = lib_view_count();
	int first = (int)selF - p->visible;
	int last = (int)selF + p->visible + 1;

	for (int i = first; i <= last; i++)
	{
		if (i < 0 || i >= n) continue;
		const chome_entry *e = lib_view_entry(i);
		if (!e) continue;

		double d = i - selF;
		int x = p->w / 2 + (int)(d * p->pitch);
		if (x < -p->sel_w || x > p->w + p->sel_w) continue;

		double t = 1.0 - (d < 0 ? -d : d);
		if (t < 0) t = 0;
		int w = p->card_w + (int)((p->sel_w - p->card_w) * t);
		int h = p->card_h + (int)((p->sel_h - p->card_h) * t);
		draw_card(e, x, p->y_shelf, w, h, t > 0.5);
	}
}

/* ------------------------------------------------------------ the chrome -- */

static void draw_background(const chome_profile *p)
{
	// Paused inside a game, the menu sits over a still of it.
	if (ig_active && ig_bg && ig_bg_w == p->w && ig_bg_h == p->h)
	{
		gfx_blit(ig_bg, p->w, p->h, 0, 0, p->w, p->h);
		gfx_fill(0, p->h - p->h * 24 / 100, p->w, p->h * 24 / 100, COL_BGDARK);
		return;
	}

	gfx_fill(0, 0, p->w, p->h, COL_BG);

	int step = p->w / 40; if (step < 8) step = 8;
	for (int x = 0; x < p->w; x += step) gfx_fill(x, 0, 1, p->h, COL_GRID);
	for (int y = 0; y < p->h; y += step) gfx_fill(0, y, p->w, 1, COL_GRID);

	gfx_fill(0, p->h - p->h * 24 / 100, p->w, p->h * 24 / 100, COL_BGDARK);

}

static void draw_title_block(const chome_profile *p)
{
	const chome_entry *e = cur_entry();
	int avail = p->w - p->inset * 2;

	if (!e)
	{
		gfx_text_c(lib_scanning() ? "SCANNING..." : "NO GAMES FOUND", p->w / 2, p->y_title, p->ts_title, COL_WHITE, COL_SHADOW);
		if (!lib_scanning())
		{
			gfx_text_c(gfx_clip("PUT ROMS IN /MEDIA/FAT/GAMES", p->ts_ui, avail), p->w / 2, p->y_meta, p->ts_ui, COL_DIM, 0);
		}
		return;
	}

	char up[CH_TITLE_LEN * 2];
	if (e->kind == ENT_GAME)
	{
		chome_item *it = lib_item(e->game);
		snprintf(up, sizeof(up), "%s", it ? it->title : "");
	}
	else
	{
		snprintf(up, sizeof(up), "%s", e->label);
	}
	for (char *q = up; *q; q++) *q = (char)toupper((unsigned char)*q);
	gfx_text_c(gfx_clip(up, p->ts_title, avail), p->w / 2, p->y_title, p->ts_title, COL_WHITE, COL_SHADOW);

	char meta[128] = {};
	if (e->kind == ENT_GAME)
	{
		chome_item *it = lib_item(e->game);
		const chome_sys *s = it ? lib_sys(it->sysidx) : 0;
		if (s)
		{
			if (p->id == PROF_LO) snprintf(meta, sizeof(meta), "%s", s->badge);
			else if (it->plays) snprintf(meta, sizeof(meta), "%s  -  PLAYED %d", s->name, it->plays);
			else snprintf(meta, sizeof(meta), "%s", s->name);
		}
	}
	else if (e->kind == ENT_BROWSE)
	{
		snprintf(meta, sizeof(meta), "BROWSE DISKS AND TAPES");
	}
	else if (e->view == VIEW_SYSTEMS || e->view == VIEW_COMPUTERS)
	{
		snprintf(meta, sizeof(meta), "%d SYSTEMS", e->count);
	}
	else
	{
		snprintf(meta, sizeof(meta), "%d GAMES", e->count);
	}

	for (char *q = meta; *q; q++) *q = (char)toupper((unsigned char)*q);
	gfx_text_c(gfx_clip(meta, p->ts_ui, avail), p->w / 2, p->y_meta, p->ts_ui, COL_DIM, 0);

	/*
	  A third line naming the file, for the cards where the title above does not.

	  Two of those: a grouped card, which is several files and says which of them with a
	  counter, and a card whose title appears on another card of the same shelf - a hack
	  in its own folder, the .sms beside the .gg, Recently Played holding two dumps of one
	  game. See "title groups" in chome_lib.cpp for why those are deliberately not merged.

	  Left in the file's own case while everything above it is upper-cased: this is the
	  name on the card, and "MEGA MAN (E).NES" is not it. MiSTer's OSD ROM font has
	  lower case, so nothing is lost by saying so exactly.

	  Drawn at the tiny scale, clipped to the same width as the lines above, and skipped
	  outright if it would reach the cards - at 240p the whole block has about twenty
	  pixels under the meta line and a long No-Intro name is far wider than the canvas.
	*/
	if (e->kind == ENT_GAME && (e->nvar > 1 || e->dup))
	{
		int y = p->y_meta + 10 * p->ts_ui;
		if (y + 8 * p->ts_tiny <= p->y_shelf - p->sel_h)
		{
			char line[CH_PATH_LEN + 16];
			const char *file = lib_view_variant_file(sel, e->vsel);

			if (e->nvar > 1) snprintf(line, sizeof(line), "%d/%d  %s", e->vsel + 1, e->nvar, file);
			else snprintf(line, sizeof(line), "%s", file);

			gfx_text_c(gfx_clip(line, p->ts_tiny, avail), p->w / 2, y, p->ts_tiny, COL_PANELLO, 0);
		}
	}
}

static void draw_pips(const chome_profile *p)
{
	chome_item *it = cur_game();
	if (!it) return;

	int s = (p->id == PROF_HD) ? 2 : 1;
	int d = 6 * s, gap = 5 * s, n = user_slots();
	int x0 = p->w / 2 - (n * d + (n - 1) * gap) / 2;

	for (int i = 0; i < n; i++)
	{
		int st = slot_state(it, i);
		int x = x0 + i * (d + gap);
		if (!st) gfx_frame_rect(x, p->y_pips, d, d, COL_DIM, 1);
		else if (st == 1) gfx_fill(x, p->y_pips, d, d, COL_GREEN);
		else
		{
			gfx_fill(x, p->y_pips, d, d, COL_YELLOW);
			gfx_fill(x + 2 * s, p->y_pips + 2 * s, d - 4 * s, d - 4 * s, COL_INK);
		}
	}
}

static void draw_position(const chome_profile *p)
{
	if (p->id == PROF_LO) return;
	int n = lib_view_count();
	if (!n) return;

	char buf[64];
	if (lib_scanning()) snprintf(buf, sizeof(buf), "%d / %d  SCANNING", sel + 1, n);
	else snprintf(buf, sizeof(buf), "%d / %d", sel + 1, n);

	gfx_text_c(buf, p->w / 2, p->y_pos, p->ts_tiny, COL_DIM, 0);

	int half = gfx_text_w(buf, p->ts_tiny) / 2;
	if (sel > 0) gfx_text(CH_LEFT, p->w / 2 - half - GLYPH_W * p->ts_tiny, p->y_pos, p->ts_tiny, COL_DIM, 0);
	if (sel < n - 1) gfx_text(CH_RIGHT, p->w / 2 + half, p->y_pos, p->ts_tiny, COL_DIM, 0);
}

/*
  Button prompts follow whatever the user last touched. A gamepad's buttons reach
  the menu as synthetic key events carrying the same codes a keyboard sends
  (input.cpp translates them for the OSD), so the code alone cannot tell them
  apart - input_menu_key_from_pad() is what does.

  The keyboard column is exactly that translation read backwards, so what the
  legend shows is what the key actually does:
      A = Enter    B = Esc    X = Tab    Y = Backspace
      Select = `   L / R = - and =       OSD button = F12
*/
// Named LBL_* rather than BTN_*: linux/input.h already owns BTN_A, BTN_X and
// friends as gamepad event codes.
#define LBL_A      0
#define LBL_B      1
#define LBL_X      2
#define LBL_Y      3
#define LBL_SELECT 4
#define LBL_COUNT  5

static const char *btn_pad[LBL_COUNT]      = { "A", "B", "X", "Y", "SEL" };
static const char *btn_kbd[LBL_COUNT]      = { "ENTER", "ESC", "TAB", "BKSP", "`" };
static const char *btn_kbd_lo[LBL_COUNT]   = { "ENT", "ESC", "TAB", "BSP", "`" };

static int using_pad = 1;

static const char *btn(int which)
{
	if (which < 0 || which >= LBL_COUNT) return "?";
	if (using_pad) return btn_pad[which];

	// The 240p legend is tight, so the keyboard names get shorter forms there.
	return (theme_get()->id == PROF_LO) ? btn_kbd_lo[which] : btn_kbd[which];
}

/*
  Prompts that match the controller in the player's hands.

  A PlayStation pad has no A or B on it, so telling somebody to "press A" is asking
  them to translate. Naming the shape they can see is the whole point of a prompt.
  There are four sets - keyboard, Nintendo, PlayStation, Xbox - plus a fifth for a pad
  we cannot place; pad_layout() picks between them from the last device used.

  Which shape or letter goes with which menu button is looked up, not assumed: SYS_BTN_A
  is whatever button that pad has mapped to it, so remap the pad and the prompts follow.
  How a mapped code becomes a shape or a letter is the next comment down.

  The four shapes are drawn, not lettered - see chome_btn12.h. Twelve pixels, which is
  the floor Derek set: below that a filled triangle and a filled square are both blobs,
  so the shapes are outlined, and an outline needs the room.
*/

/*
  POSITION OR LETTER - the decision the Xbox set turns on, since Xbox swaps A/B and X/Y
  round from Nintendo and the two answers therefore disagree.

  What we follow is the *reported button code*, which is the only thing actually available:
  input_menu_key_btn() gives the code the user's own map binds to a menu button, so a
  remapped pad stays correctly described. A code is then read as a POSITION on the pad, and
  the letter comes from that layout's diamond - not from the code's legacy letter name.

  Why position and not the letter name: input-event-codes.h gives each of these four
  numbers two names, BTN_SOUTH/BTN_A, BTN_EAST/BTN_B, BTN_NORTH/BTN_X, BTN_WEST/BTN_Y. The
  positional name is the definition; the lettered one is an older alias, and its letters
  spell out an *Xbox* pad - which is why BTN_NORTH is also BTN_X even though X sits west on
  an Xbox pad. Take the letter name literally and every pad in the world is described as
  though it were an Xbox, which is exactly the bug this feature exists to remove.

  So each layout has its own diamond:

      Nintendo:  N=X  E=A  S=B  W=Y          Xbox:  N=Y  E=B  S=A  W=X

  and the honest consequence, which is worth stating because it looks like a mistake: the
  two tables differ in A/B only. X and Y come out the same on both, because xpad puts an
  Xbox pad's X and Y on BTN_X and BTN_Y - the alias names - and those aliases are the
  positions Nintendo's X and Y sit on. Two departures cancelling, not a copy-paste slip.

  Where a pad's layout is unknown there is no diamond to read a position through, so
  PAD_PLAIN quotes the code's own legacy letter (the Xbox table) rather than inventing a
  geometry, and draws it with no colour - see COL_BTN_PLAIN.

  A PlayStation pad needs no diamond: it has no letters, so position is the only readable
  answer and it is the right one. snacpad.cpp emits BTN_NORTH for triangle, BTN_EAST for
  circle, BTN_SOUTH for cross and BTN_WEST for square. With MiSTer's default map
  (def_mmap in input.cpp binds SYS_BTN_A to BTN_EAST) that lands on circle to confirm and
  cross to go back - the Japanese convention, and the one the physical layout implies.
  The same default is why a Nintendo pad's confirm prompt reads A and an Xbox pad's reads
  B: both are the east button, and the two pads print different letters on it.
*/
#define BTN_CODE_SOUTH 0x130
#define BTN_CODE_EAST  0x131
#define BTN_CODE_NORTH 0x133
#define BTN_CODE_WEST  0x134

/*
  Which family of pad is in the player's hands, and so which set of prompts to draw.

  Only the name is available to tell them apart - a SNAC pad has no vid/pid of its own,
  it is a uinput device snacpad.cpp creates - so this matches on names that *state a
  layout*. A name that only states a brand is not enough: 8BitDo alone sells pads with
  Nintendo lettering and pads with Xbox lettering, so "8BitDo" deliberately does not
  match anything here and falls through to PAD_PLAIN. Guessing wrong is worse than
  declining: a legend that paints Nintendo colours on an Xbox pad has told the player
  something false about the thing in their hands, whereas PAD_PLAIN still names the
  right button and merely has no colour for it.
*/
#define PAD_KBD   0     // no pad was used last: named keys on a light chip, no colour
#define PAD_PSX   1
#define PAD_SNES  2
#define PAD_XBOX  3
#define PAD_PLAIN 4

/*
  By name, so the controller tester can ask about the pad it is showing rather than the
  one the last menu key came from - on that screen they are usually the same pad and the
  one time they are not is the one time it matters.
*/
/*
  Who made it, which is in the USB identity even when it is nowhere in the name.

  This is the part a name cannot do. Sony, Nintendo and Microsoft each put one set of
  markings on every pad they ship, so their vendor id settles the question outright -
  and it keeps working for a pad nobody has added a name pattern for yet. Third-party
  makers are deliberately absent: Hori and PDP and 8BitDo all ship both letterings, so
  their vendor id says nothing and the name has to.
*/
static int vendor_layout(uint16_t vid)
{
	switch (vid)
	{
	case 0x054c: return PAD_PSX;      // Sony
	case 0x057e: return PAD_SNES;     // Nintendo
	case 0x045e: return PAD_XBOX;     // Microsoft
	}
	return -1;
}

static int pad_layout_of(const char *n)
{
	if (!n || !*n) return PAD_PLAIN;

	// A SNAC pad is a uinput device snacpad.cpp names; a USB Sony pad says so too,
	// and the same labels are right for it.
	if (strcasestr(n, "SNAC") || strcasestr(n, "PlayStation")
		|| strcasestr(n, "DualShock") || strcasestr(n, "DualSense")
		|| strcasestr(n, "Sony")) return PAD_PSX;

	// "Microsoft X-Box 360 pad" is what the kernel's xpad driver calls one; the wireless
	// ones report "Xbox Wireless Controller". XInput is checked too because a third-party
	// pad in that mode presents Xbox lettering whatever else its name says.
	if (strcasestr(n, "Xbox") || strcasestr(n, "X-Box")
		|| strcasestr(n, "XInput")) return PAD_XBOX;

	// Nintendo lettering, and the same four colours on all of them. "Nintendo" covers the
	// Switch Pro Controller, whose kernel name begins with it.
	if (strcasestr(n, "SNES") || strcasestr(n, "Super Nintendo")
		|| strcasestr(n, "Super Famicom") || strcasestr(n, "Famicom")
		|| strcasestr(n, "Nintendo") || strcasestr(n, "Joy-Con")
		|| strcasestr(n, "Switch Pro")) return PAD_SNES;

	/*
	  What these pads actually call themselves over Bluetooth, which is neither their
	  maker nor their model. Taken from Derek's own pairing records:

	    Name=Wireless Controller     - a DualShock 4
	    Name=Pro Controller          - a Switch Pro

	  Both fell through to PAD_PLAIN, so two of the three pads he plays with were being
	  offered lettered prompts on a machine that knows perfectly well what they are. Only
	  the USB names carry a maker; every one of the patterns above needs one.

	  Matched in full rather than as substrings, and last. "Xbox Wireless Controller" is
	  a real name that contains the first of these and is not one, and it is already
	  caught above - but a substring test here would still be a trap for the next name
	  somebody adds.
	*/
	if (!strcasecmp(n, "Wireless Controller")) return PAD_PSX;
	if (!strcasecmp(n, "Pro Controller")) return PAD_SNES;

	return PAD_PLAIN;
}

// The identity first, the name second - see vendor_layout() and pad_layout_of().
static int pad_layout_for(uint32_t vidpid, const char *name)
{
	int v = vendor_layout((uint16_t)(vidpid >> 16));
	if (v >= 0) return v;
	return pad_layout_of(name);
}

static int pad_layout()
{
	if (!using_pad) return PAD_KBD;
	return pad_layout_for(input_menu_key_vidpid(), input_menu_key_devname());
}

// Indexed by LBL_A..LBL_Y, which is also the letter order.
static const char *letter_pic[4] = { "btn_a", "btn_b", "btn_x", "btn_y" };

/*
  Which letter is printed on the button this code came from - see the diamonds above.
  -1 for anything that is not one of the four faces: Select, a shoulder, or a button the
  pad reports as something else entirely.
*/
static int code_letter(int layout, uint16_t code)
{
	int pos;
	switch (code)
	{
	case BTN_CODE_SOUTH: pos = 0; break;
	case BTN_CODE_EAST:  pos = 1; break;
	case BTN_CODE_NORTH: pos = 2; break;
	case BTN_CODE_WEST:  pos = 3; break;
	default: return -1;
	}

	/*
	  south, east, north, west - by position, because the position is what the player's
	  thumb knows and the letter printed there is what the pad says.

	  Xbox needs its own row, and the reason is a trap in the kernel's own names. In
	  input.h BTN_X is an alias of BTN_NORTH and BTN_Y of BTN_WEST, which is backwards
	  for the pad those letters come from: an Xbox controller has Y at the top and X on
	  the left. Reading the aliases as geometry put a Y on the west button and an X on
	  the north one - the wrong letter *and* the wrong colour, since the colour follows
	  the letter.

	  An unknown pad keeps the alias order. It is a guess either way, and this is not
	  the place to change what unrecognised hardware has always drawn.
	*/
	static const int nintendo[4] = { LBL_B, LBL_A, LBL_X, LBL_Y };
	static const int xbox[4]     = { LBL_A, LBL_B, LBL_Y, LBL_X };
	static const int legacy[4]   = { LBL_A, LBL_B, LBL_X, LBL_Y };

	if (layout == PAD_SNES) return nintendo[pos];
	if (layout == PAD_XBOX) return xbox[pos];
	return legacy[pos];
}

/*
  The colour of a letter on this pad. Keyed to the letter rather than to the menu button
  it is bound to, for the same reason a circle is red however it is mapped: what the
  player sees is the plastic, not the binding.
*/
static uint32_t letter_col(int layout, int letter)
{
	static const uint32_t snes[4] = { COL_SNES_A, COL_SNES_B, COL_SNES_X, COL_SNES_Y };
	static const uint32_t xbox[4] = { COL_XBOX_A, COL_XBOX_B, COL_XBOX_X, COL_XBOX_Y };

	if (letter < 0 || letter > 3) return COL_BTN_PLAIN;

	if (layout == PAD_SNES) return snes[letter];
	if (layout == PAD_XBOX) return xbox[letter];
	return COL_BTN_PLAIN;
}

struct prompt { const char *text; const char *pic; uint32_t col; };

// Which menu button each LBL_* is, in LBL order. The tester walks it too.
static const int lbl_sysbtn[LBL_COUNT] =
	{ SYS_BTN_A, SYS_BTN_B, SYS_BTN_X, SYS_BTN_Y, SYS_BTN_SELECT };

/*
  Split from btn_prompt() so the controller tester can ask the same question about a
  button code it read from a different pad. Everything about which shape goes where is
  in here; btn_prompt() only supplies the pad.
*/
static prompt prompt_for_code(int layout, int which, uint16_t code)
{
	prompt out = { btn(which), 0, 0 };   // no shape and no colour: a named key, not a button
	if (which < 0 || which >= LBL_COUNT) return out;
	if (layout == PAD_KBD) return out;

	if (layout == PAD_PSX)
	{
		switch (code)
		{
		case BTN_CODE_EAST:  out.pic = "psx_circle";   out.col = COL_BTN_CIRCLE;   break;
		case BTN_CODE_NORTH: out.pic = "psx_triangle"; out.col = COL_BTN_TRIANGLE; break;
		case BTN_CODE_WEST:  out.pic = "psx_square";   out.col = COL_BTN_SQUARE;   break;
		case BTN_CODE_SOUTH: out.pic = "psx_cross";     out.col = COL_BTN_CROSS;    break;
		default: break;                                // Select keeps its own name
		}
		return out;
	}

	// A lettered pad: where the button sits says which letter, and the layout says both
	// which diamond to read that through and what colour the letter is.
	int letter = code_letter(layout, code);

	if (letter >= 0)
	{
		out.pic = letter_pic[letter];
		out.col = letter_col(layout, letter);
	}
	return out;
}

static prompt btn_prompt(int which)
{
	if (which < 0 || which >= LBL_COUNT) return prompt_for_code(PAD_KBD, which, 0);
	return prompt_for_code(pad_layout(), which, input_menu_key_btn(lbl_sysbtn[which]));
}

// dim marks a prompt that is on screen but not available - a save already registered and
// waiting, say. Shown rather than removed, so the row does not reshuffle under the player.
// col is the pad's own colour for this button, 0 for a keyboard key - which has none.
struct legend_pair { const char *key; const char *pic; const char *label; const char *shortl; int dim; uint32_t col; };

static legend_pair lp(int which, const char *label, const char *shortl);


/*
  One button, drawn as it looks on the pad: the shape or letter in the pad's own colour on
  a near-black chip, or the old light chip with dark lettering for a keyboard key, which
  has no colour of its own.

  Shared with the dialogs rather than living inside the legend, because a dialog that says
  "press X" while the legend below it shows a square is telling the player two different
  things - which is what he found.
*/
static const btn12_def *btn12_find(const char *name)
{
	if (!name) return 0;
	for (size_t i = 0; i < sizeof(btn12s) / sizeof(btn12s[0]); i++)
	{
		if (!strcmp(btn12s[i].name, name)) return &btn12s[i];
	}
	return 0;
}

/*
  A button, drawn rather than lettered. Twelve pixels tall at 240p and a whole multiple of
  that above: below twelve the four PlayStation shapes stop telling each other apart, and a
  fractional scale would put their one-pixel outlines on half pixels.

  Width comes from the row, because Start and Select are pills carrying a word while the
  face buttons are square. The glyph brings its own chip - its corners are transparent,
  which is what makes it read as a rounded button instead of a box - so nothing is filled
  behind it.
*/
static int btn12_w(const btn12_def *d)
{
	return (int)strlen(d->rows[0]);
}

static void btn12_draw(const btn12_def *d, int x, int y, int s, uint32_t accent, int dim)
{
	uint32_t chip = dim ? COL_BGDARK : COL_BTN_CHIP;
	if (dim) accent = COL_DIM;

	int w = btn12_w(d);
	for (int gy = 0; gy < BTN12; gy++)
	{
		const char *row = d->rows[gy];
		for (int gx = 0; gx < w; gx++)
		{
			char c = row[gx];
			if (c == '.') continue;
			gfx_fill(x + gx * s, y + gy * s, s, s, (c == 'c') ? accent : chip);
		}
	}
}

static int btn_chip_w(const char *key, const char *pic, int s)
{
	const btn12_def *d = btn12_find(pic);
	if (d) return btn12_w(d) * s;

	return (pic ? 8 * s : gfx_text_w(key, s)) + 4 * s;
}

/*
  A centred line of prose with one button in it, so a dialog names the same thing the
  legend does. lp() already resolves which shape or letter this pad uses for that button,
  so this asks it rather than hard-coding a letter.
*/
static void btn_hint_c(int cx, int y, int s, uint32_t col, const char *pre, int which, const char *post);

static void btn_chip(const char *key, const char *pic, uint32_t col, int dim, int x, int y, int s)
{
	const btn12_def *d = btn12_find(pic);
	if (d)
	{
		// Centred on the row of text beside it: the glyph is taller than the 8-pixel font,
		// so it hangs two pixels either side of the baseline rather than dragging the row.
		btn12_draw(d, x - 2 * s, y - 2 * s, s, col ? col : COL_WHITE, dim);
		return;
	}

	int kw = pic ? 8 * s : gfx_text_w(key, s);

	gfx_fill(x - 2 * s, y - 2 * s, kw + 4 * s, 8 * s + 4 * s, col ? COL_BTN_CHIP : COL_PANEL);

	uint32_t kcol = col ? col : (dim ? COL_DIM : COL_INK);
	if (pic) picto(pic, x, y, 8 * s, kcol);
	else gfx_text(key, x, y, s, kcol, 0);
}

// A prompt for one of the face buttons, whatever it is called on this controller.
static legend_pair lp(int which, const char *label, const char *shortl)
{
	prompt pr = btn_prompt(which);

	uint32_t col = pr.col;
	const char *pic = pr.pic;

	/*
	  Select gets its pill on every pad - none of the four families gives it a colour or a
	  shape, only a word - and a face button whose code the pad did not report as one of
	  the four letters falls back to the name the firmware uses for it, drawn the same way.
	  Not the font's letter on a chip: a drawn A beside a drawn circle is the only way the
	  two read as the same kind of thing.

	  The fallback is a real path, not belt-and-braces: an unmapped button reads back as 0,
	  and a pad that reports its faces as BTN_TRIGGER or BTN_THUMB - some HID gamepads do -
	  reaches it too. Better to draw MiSTer's own name for the button than nothing at all.
	*/
	int layout = pad_layout();

	if (!pic && layout != PAD_KBD)
	{
		if (which == LBL_SELECT) { pic = "btn_select"; col = 0; }
		else if (which >= LBL_A && which <= LBL_Y)
		{
			pic = letter_pic[which];
			col = letter_col(layout, which);
		}
	}
	else if (!pic && layout == PAD_KBD)
	{
		col = 0;                               // a keyboard key keeps the plain chip
	}

	legend_pair out = { pr.text, pic, label, shortl, 0, col };
	return out;
}

/*
  A sentence with a button drawn into it - "Press [O] again to restart".

  Three quarters of a character either side of the glyph. Two pixels was enough when the
  button was a letter on a chip; a drawn one is a dark rounded button, and on the light
  panel of a dialog it is a solid block - set close to the words it read as "Press[A]again".
*/
#define BTN_HINT_PAD 6

static int btn_hint_w(int s, const char *pre, int which, const char *post)
{
	legend_pair b = lp(which, "", "");
	return (pre && *pre ? gfx_text_w(pre, s) : 0)
		+ (post && *post ? gfx_text_w(post, s) : 0)
		+ btn_chip_w(b.key, b.pic, s) + BTN_HINT_PAD * 2 * s;
}

static void btn_hint_l(int x, int y, int s, uint32_t col, const char *pre, int which, const char *post)
{
	legend_pair b = lp(which, "", "");
	int pad = BTN_HINT_PAD * s;

	if (pre && *pre) { gfx_text(pre, x, y, s, col, 0); x += gfx_text_w(pre, s); }
	x += pad;

	btn_chip(b.key, b.pic, b.col, 0, x + 2 * s, y, s);
	x += btn_chip_w(b.key, b.pic, s) + pad;

	if (post && *post) gfx_text(post, x, y, s, col, 0);
}

static void btn_hint_c(int cx, int y, int s, uint32_t col, const char *pre, int which, const char *post)
{
	btn_hint_l(cx - btn_hint_w(s, pre, which, post) / 2, y, s, col, pre, which, post);
}

static int build_legend(legend_pair *out, int max)
{
	int n = 0;
	const chome_entry *e = cur_entry();

	switch (screen)
	{
	case SCR_SUSPEND:
	{
		// Inside that very game the slots become live: A restores, Y writes.
		int here = ig_is_running(cur_game());

		// Nothing to offer for a core with no savestates - see draw_suspend(). Also from
		// the shelf, where the answer comes from the table instead of the core.
		if (no_savestates_for(cur_game()))
		{
			// From the shelf A starts the game rather than going back to it, and the
			// only prompt on screen must not say otherwise.
			if (n < max) { out[n++] = lp(LBL_A, here ? "Resume" : "Start", here ? "Play" : "Start"); }
			if (n < max) { out[n++] = lp(LBL_B, "Back", "Back"); }
			break;
		}
		if (here && ss_can_load() && n < max) { out[n++] = lp(LBL_A, "Load", "Load"); }
		else if (n < max) { out[n++] = lp(LBL_A, "Resume", "Play"); }
		if (here && ss_can_save() && n < max)
		{
			int busy = (pend_slot >= 0);
			out[n] = lp(LBL_Y, busy ? "Saving" : "Save", busy ? "Saving" : "Save");
			out[n].dim = busy;
			n++;
		}
		else if (n < max) { out[n++] = { CH_DOWN, "dpad_down", "Lock", "Lock", 0, COL_WHITE }; }
		if (n < max) { out[n++] = lp(LBL_X, "Delete", "Del"); }
		if (n < max) { out[n++] = lp(LBL_B, "Back", "Back"); }
		break;
	}
	case SCR_POWER:
		if (n < max) { out[n++] = lp(LBL_A, "Choose", "OK"); }
		if (n < max) { out[n++] = lp(LBL_B, "Back", "Back"); }
		break;
	case SCR_SORT:
		if (n < max) { out[n++] = lp(LBL_A, "Apply", "OK"); }
		if (n < max) { out[n++] = lp(LBL_B, "Back", "Back"); }
		break;
	case SCR_WIFI:
		if (net_join_state() != JOIN_IDLE)
		{
			if (net_join_state() != JOIN_WORK && n < max) { out[n++] = lp(LBL_A, "OK", "OK"); }
			break;
		}
		if (net_count() && n < max) { out[n++] = lp(LBL_A, "Join", "Join"); }
		if (n < max) { out[n++] = lp(LBL_X, "Look Again", "Scan"); }
		if (n < max) { out[n++] = lp(LBL_B, "Back", "Back"); }
		break;
	case SCR_PADS:
		/*
		  While pairing is running there is exactly one thing to offer: stopping. The
		  pad in the player's hands is busy being paired, and a second action here
		  would be a second thing to explain.
		*/
		if (bt_pairing())
		{
			if (n < max) { out[n++] = lp(LBL_B, "Done", "Done"); }
			break;
		}
		if (bt_pair_state() != BTP_IDLE)
		{
			// A finished result. "Add Another" rather than "Try Again" once one worked.
			int ok = (bt_pair_state() == BTP_OK);
			if (n < max) { out[n++] = lp(LBL_A, ok ? "Add Another" : "Try Again", ok ? "Add" : "Retry"); }
			if (n < max) { out[n++] = lp(LBL_B, "Done", "Done"); }
			break;
		}
		{
			/*
			  A now says what the row under the cursor does, because adding a controller is
			  a row of its own rather than a shortcut. Everything else here is a thing you
			  can only do to a wireless pad; a wired one is listed so the player can see it
			  is there, and offering nothing for it is how the screen says so.
			*/
			pad_row sel;
			if (pads_sel(&sel))
			{
				if (sel.is_add)
				{
					if (n < max) { out[n++] = lp(LBL_A, "Add a Controller", "Add"); }
				}
				else
				{
					if (n < max) { out[n++] = lp(LBL_A, "Test It", "Test"); }

					if (sel.kind == PAD_BT)
					{
						if (!sel.connected && n < max) { out[n++] = lp(LBL_Y, "Wake It Up", "Wake"); }
						if (n < max) { out[n++] = lp(LBL_X, "Forget", "Forget"); }
					}
				}
			}
		}
		if (n < max) { out[n++] = lp(LBL_B, "Back", "Back"); }
		break;

	case SCR_PADTEST:
		/*
		  Nothing but the way out: every button on the pad is the thing being tested, so
		  none of them may also mean something. B does not either until it has been
		  pressed once and seen to light up - hence "twice", and hence the panel's own
		  footer saying it a second time, since that is where the player is looking.
		*/
		if (n < max) { out[n++] = lp(LBL_B, "Back Twice", "Back"); }
		break;
	case SCR_DISPLAY:
		if (n < max) { out[n++] = { CH_LEFT CH_RIGHT, "dpad_lr", "Choose", "Sel", 0, COL_WHITE }; }
		if (n < max) { out[n++] = lp(LBL_A, "Apply", "OK"); }
		if (n < max) { out[n++] = lp(LBL_B, "Back", "Back"); }
		break;
	case SCR_OPTIONS:
		if (n < max) { out[n++] = { CH_LEFT CH_RIGHT, "dpad_lr", "Change", "Chg", 0, COL_WHITE }; }
		if (n < max) { out[n++] = lp(LBL_A, "Select", "OK"); }
		if (n < max) { out[n++] = lp(LBL_B, "Back", "Back"); }
		break;
	case SCR_CORE:
		/*
		  Left and right on every row but the last, which turns the page. No Save: a
		  change is written to the core as it is made.
		*/
		if (co_row < core_opts_tier_count(co_tier))
		{
			if (n < max) { out[n++] = { CH_LEFT CH_RIGHT, "dpad_lr", "Change", "Chg", 0, COL_WHITE }; }

			/*
			  And on a row this game keeps its own value for, the way to give it back.
			  Offered only there: on any other row X would have nothing to do, and a
			  prompt for a press that does nothing is worse than no prompt.
			*/
			const core_opt *co = core_opt_tier_at(co_tier, co_row);
			if (co && core_opt_per_game(co) && n < max)
			{
				out[n++] = lp(LBL_X, "Shared Value", "Shared");
			}
		}
		else if (n < max) out[n++] = lp(LBL_A, "More", "More");
		if (n < max) out[n++] = lp(LBL_B, "Back", "Back");
		break;

	case SCR_SET:
		/*
		  The Save row is the one row where A means something, so it is the only row that
		  offers it. Everywhere else the pair that matters is left/right to change and X
		  to put the value back - and at 240p only the first three survive, which is why
		  those three are first.
		*/
		if (set_row >= set_nview)
		{
			// ...and only while there is something to save, or the row would keep
			// offering a press that does nothing but shake the panel.
			if (opt_dirty() && n < max) { out[n++] = lp(LBL_A, "Save", "Save"); }
			if (n < max) { out[n++] = lp(LBL_B, "Back", "Back"); }
			break;
		}
		if (n < max) { out[n++] = { CH_LEFT CH_RIGHT, "dpad_lr", "Change", "Chg", 0, COL_WHITE }; }
		if (n < max) { out[n++] = lp(LBL_X, "Usual Value", "Usual"); }
		if (n < max) { out[n++] = lp(LBL_B, "Back", "Back"); }
		break;
	case SCR_ABOUT:
		if (n < max) { out[n++] = lp(LBL_B, "Back", "Back"); }
		break;
	case SCR_DISCBAR:
		if (n < max) { out[n++] = lp(LBL_A, "Use Disc", "Disc"); }
		if (n < max) { out[n++] = { CH_UP CH_DOWN, "dpad_ud", "Move", "Move", 0, COL_WHITE }; }
		if (n < max) { out[n++] = lp(LBL_B, "Back", "Back"); }
		break;
	case SCR_DISC:
		if (n < max) { out[n++] = lp(LBL_A, "Choose", "Choose"); }
		if (n < max) { out[n++] = lp(LBL_B, "Back", "Back"); }
		break;
	case SCR_MENUBAR:
		if (n < max) { out[n++] = lp(LBL_A, "Open", "Open"); }
		if (n < max) { out[n++] = { CH_LEFT CH_RIGHT, "dpad_lr", "Move", "Move", 0, COL_WHITE }; }
		if (n < max) { out[n++] = lp(LBL_B, "Back", "Back"); }
		break;
	case SCR_BROWSE:
		if (n < max) { out[n++] = lp(LBL_A, "Open", "Open"); }
		if (n < max) { out[n++] = lp(LBL_B, "Back", "Back"); }
		break;
	default:
		if (e && e->kind != ENT_GAME)
		{
			if (n < max) { out[n++] = lp(LBL_A, "Open", "Open"); }
			if (n < max) { out[n++] = { CH_UP, "dpad_up", "Menu", "Menu", 0, COL_WHITE }; }
			if (n < max) { out[n++] = lp(LBL_SELECT, "Sort", "Sort"); }
		}
		else
		{
			int running = ig_is_running(cur_game()) || susp_matches(cur_game());
			if (n < max) { out[n++] = lp(LBL_A, running ? "Resume" : "Start", running ? "Play" : "Start"); }
			/*
			  X was the one face button the shelf had nothing for, which is what makes it
			  the button that cycles a card's files. Everything else was taken and none of
			  it could be given up: A starts, B jumps back to the folders, Y favourites,
			  Select sorts, the shoulders page the shelf, up is the menu bar and down is
			  the suspend points. Taking one of those would have cost an action to gain
			  one - and X already means "the other thing you can do to this row"
			  elsewhere here (a shared value on core options, the usual value in
			  Settings), so this is that meaning on the shelf rather than a new one.

			  Offered only on a card that has something to cycle, like X on the core
			  options rows: a prompt for a press that does nothing is worse than no prompt.

			  Second, because order is priority - the 240p legend keeps only the first
			  three - and on a grouped card starting the wrong region is a wrong outcome
			  while not favouriting is merely a missing convenience. On every other card
			  the order is unchanged.
			*/
			if (e && e->nvar > 1 && n < max) { out[n++] = lp(LBL_X, "Version", "Ver"); }
			if (n < max) { out[n++] = { CH_DOWN, "dpad_down", "Suspend Points", "Saves", 0, COL_WHITE }; }
			// And favouriting a game is worth more on a CRT than re-sorting the shelf.
			// The action itself was always here - it just never appeared there.
			if (n < max) { out[n++] = lp(LBL_Y, "Favourite", "Fav"); }
			if (n < max) { out[n++] = lp(LBL_SELECT, "Sort", "Sort"); }
		}
		break;
	}
	return n;
}

static void draw_legend(const chome_profile *p)
{
	legend_pair pairs[6];
	int n = build_legend(pairs, 6);
	int lo = (p->id == PROF_LO);
	if (lo && n > 3) n = 3;

	int s = p->ts_ui;
	int top = p->y_legend - 6 * s;
	gfx_fill(0, top, p->w, p->h - top, COL_BGDARK);
	gfx_fill(0, top, p->w, 1, CheckTimer(nudge_until) ? COL_GRID : COL_RED);

	int widths[6], total = 0;
	const char *labels[6];
	for (int i = 0; i < n; i++)
	{
		labels[i] = lo ? pairs[i].shortl : pairs[i].label;
		// Measured by whatever will draw it: a drawn button is twelve pixels and Select
		// is a pill half again as wide, so a fixed glyph cell put the labels wrong.
		widths[i] = btn_chip_w(pairs[i].key, pairs[i].pic, s)
			+ 5 * s + gfx_text_w(labels[i], s);
		total += widths[i];
	}

	int avail = p->w - p->inset * 2;
	int gap = 3 * GLYPH_W * s;
	if (n > 1)
	{
		int fit = (avail - total) / (n - 1);
		if (fit < gap) gap = fit;
		if (gap < 2 * GLYPH_W * s) gap = 2 * GLYPH_W * s;
		total += gap * (n - 1);
	}

	int x = (p->w - total) / 2;
	if (x < p->inset) x = p->inset;

	for (int i = 0; i < n; i++)
	{
		int kw = btn_chip_w(pairs[i].key, pairs[i].pic, s);
		btn_chip(pairs[i].key, pairs[i].pic, pairs[i].dim ? 0 : pairs[i].col,
			pairs[i].dim, x, p->y_legend, s);

		uint32_t lcol = pairs[i].dim ? COL_DIM : COL_PANELHI;

		char up[64];
		snprintf(up, sizeof(up), "%s", labels[i]);
		for (char *q = up; *q; q++) *q = (char)toupper((unsigned char)*q);
		gfx_text(up, x + kw + 5 * s, p->y_legend, s, lcol, 0);

		x += widths[i] + gap;
	}
}

static void draw_menubar(const chome_profile *p, int focused)
{
	if (bar_y <= 0.002) return;

	int h = p->bar_h;

	// Slides in to the safe margin, not to the edge: on a CRT the top few percent
	// of the picture is behind the bezel, and the bar was landing in it.
	int y = (int)(-h + h * bar_y) + p->safe_y;
	gfx_fill(0, y, p->w, h, COL_PANEL);
	gfx_fill(0, y + h - 2, p->w, 2, COL_PANELLO);

	int s = (p->id == PROF_HD) ? 2 : 1;
	int nvis = mb_count_visible();
	if (nvis < 1) nvis = 1;
	int cellw = (p->w - p->inset * 2) / nvis;

	for (int slot = 0; slot < nvis; slot++)
	{
		int i = mb_at(slot);
		int cx = p->inset + cellw * slot + cellw / 2;
		int on = focused && slot == mb_idx;

		/*
		  The word, and nothing else. There used to be a pictogram beside it, drawn
		  by hand; the room it took is why this said "OPTI" at 240p rather than
		  "OPTIONS". The name is the clearer label of the two anyway.
		*/
		char up[32];
		snprintf(up, sizeof(up), "%s", mb_text(i));
		for (char *q = up; *q; q++) *q = (char)toupper((unsigned char)*q);

		if (on) gfx_fill(cx - cellw / 2 + 2, y + 2, cellw - 4, h - 6, COL_BLUE);
		gfx_text_c(gfx_clip(up, s, cellw - 8), cx, y + (h - 8 * s) / 2, s,
			on ? COL_WHITE : COL_INK, 0);
	}
}

/* ------------------------------------------------------------- panels ----- */

struct panel_box { int x, y, w, h, s; };

static panel_box draw_panel_ex(const chome_profile *p, int w, int h, const char *title)
{
	panel_box b;
	int x = (p->w - w) / 2, y = (p->h - h) / 2;
	int s = p->ts_ui;
	int hdr = 10 * s + 6;

	gfx_fill(x + 4, y + 4, w, h, COL_SHADOW);
	gfx_fill(x, y, w, h, COL_PANEL);
	gfx_frame_rect(x, y, w, h, COL_PANELLO, 2);
	gfx_fill(x, y, w, hdr, COL_INK);

	char up[64];
	snprintf(up, sizeof(up), "%s", title);
	for (char *q = up; *q; q++) *q = (char)toupper((unsigned char)*q);
	gfx_text(up, x + 6 * s, y + 4, s, COL_PANELHI, 0);

	b.x = x; b.y = y + hdr; b.w = w; b.h = h - hdr; b.s = s;
	return b;
}

static panel_box draw_panel(const chome_profile *p, const char *title)
{
	return draw_panel_ex(p, p->panel_w, p->panel_h, title);
}

// Word-wraps into at most `maxlines` lines of `cols` characters.
static int wrap_text(const char *src, int cols, char out[4][64], int maxlines)
{
	int n = 0;
	out[0][0] = 0;

	const char *p2 = src;
	while (*p2 && n < maxlines)
	{
		while (*p2 == ' ') p2++;
		if (!*p2) break;

		const char *st = p2;
		while (*p2 && *p2 != ' ') p2++;
		int wl = (int)(p2 - st);
		if (wl > 63) wl = 63;

		int cur = (int)strlen(out[n]);
		if (cur && cur + 1 + wl > cols)
		{
			if (++n >= maxlines) break;
			out[n][0] = 0;
			cur = 0;
		}
		if (cur) { out[n][cur] = ' '; cur++; }
		memcpy(out[n] + cur, st, wl);
		out[n][cur + wl] = 0;
	}

	// The break above leaves n at maxlines when the text did not fit, and returning
	// n + 1 there told the caller about a line that was never written - which drew
	// whatever was in the array. Text that overflows is cut, not garnished.
	return (n < maxlines) ? n + 1 : maxlines;
}

/*
  vcol is per-row and optional: 0 there, or a null array, leaves a value in the colour
  it would have had. It exists for the settings screen, which colours a value that is
  not the recommended one - and that colour has to survive the row being selected, or
  the one row the player is looking at would be the one that stopped saying so.
*/
static void draw_rows_c(const panel_box *b, const char *const *rows, const char *const *vals,
	const uint32_t *vcol, int n, int idx)
{
	int rowh = 12 * b->s;
	for (int i = 0; i < n; i++)
	{
		int y = b->y + 5 * b->s + i * rowh;
		if (y + rowh > b->y + b->h) break;

		int on = (i == idx);
		if (on) gfx_fill(b->x + 3, y - 2 * b->s, b->w - 6, rowh - 2 * b->s, COL_BLUE);

		char up[64];
		snprintf(up, sizeof(up), "%s", rows[i]);
		for (char *q = up; *q; q++) *q = (char)toupper((unsigned char)*q);
		gfx_text(gfx_clip(up, b->s, b->w / 2), b->x + 6 * b->s, y, b->s, on ? COL_WHITE : COL_INK, 0);

		if (vals && vals[i])
		{
			char v[48];
			snprintf(v, sizeof(v), "%s", vals[i]);
			for (char *q = v; *q; q++) *q = (char)toupper((unsigned char)*q);
			int vw = gfx_text_w(v, b->s);
			uint32_t col = (vcol && vcol[i]) ? vcol[i] : (on ? COL_WHITE : COL_PANELLO);
			gfx_text(v, b->x + b->w - 6 * b->s - vw, y, b->s, col, 0);
		}
	}
}

static void draw_rows(const panel_box *b, const char *const *rows, const char *const *vals, int n, int idx)
{
	draw_rows_c(b, rows, vals, 0, n, idx);
}

/* ------------------------------------------------------- lists that wait -- */

/*
  The furniture the two screens where the player has to *wait* are built from: Wi-Fi
  and Controllers. Both were a column of text rows, which is the right shape for a
  settings list and the wrong one here - a row on these screens is a thing in the room
  with a state, not a value.

  Kept general rather than written twice, and general on purpose: the same treatment
  is what the rest of Options wants, and a second copy of it would be a second set of
  paddings to get out of step.
*/

// The animation clock. One place, so every indicator on screen turns together.
static unsigned long anim_ms() { return GetTimer(0); }

/*
  How fast the disc turns, which is the entire state indicator: fast while the drive
  is still working out what the disc is, slow once it is known. Nothing else about the
  drawing changes between the two, so there is exactly one place to get this wrong.
*/
/*
  How fast the disc should be turning, as phase units per second rather than as a period,
  because a speed is what gets eased and 1/period does not interpolate sensibly.

  A full turn is DISC_TURN units. Three rates, three meanings: focused is faster than
  either state, because reusing the "still identifying" rate for focus would make a
  focused known disc and an unfocused unknown one identical, and the rate is the only
  thing that distinguishes those.

  Only the badge is ever focused; the prompt's disc turns at the state rate.
*/
#define DISC_TURN     1024UL
#define DISC_RAMP_MS  300UL

static unsigned long disc_target_speed()
{
	unsigned long period = (screen == SCR_DISCBAR) ? GFX_DISC_FOCUS_MS
		: (disc_state() == DISC_SPINNING) ? GFX_DISC_FAST_MS : GFX_DISC_SLOW_MS;

	return DISC_TURN * 1000UL / period;
}

static unsigned long disc_spd_from = 0;
static unsigned long disc_spd_to = 0;
static unsigned long disc_ramp_t0 = 0;
static unsigned long disc_phase = 0;          // 0..DISC_TURN-1
static unsigned long disc_phase_ms = 0;
static unsigned long disc_step_ms = 0;        // when disc_step() last recomputed
static int disc_step_cached = 0;

/*
  Where the ramp has got to. Smoothstep rather than linear, which is the difference
  between "accelerating" and "changing speed instantly to a new constant" - the eye reads
  a linear ramp as two kinks with a straight line between them.
*/
static unsigned long disc_speed_now(unsigned long now)
{
	unsigned long dt = now - disc_ramp_t0;
	if (dt >= DISC_RAMP_MS) return disc_spd_to;

	// smoothstep in 0..1024 fixed point: x*x*(3-2x)
	unsigned long x = dt * 1024UL / DISC_RAMP_MS;
	unsigned long e = (x * x / 1024UL) * (3072UL - 2UL * x) / 1024UL;
	if (e > 1024UL) e = 1024UL;

	if (disc_spd_to >= disc_spd_from)
		return disc_spd_from + (disc_spd_to - disc_spd_from) * e / 1024UL;

	return disc_spd_from - (disc_spd_from - disc_spd_to) * e / 1024UL;
}

/*
  The disc's rotation, 0-63.

  Accumulates phase from elapsed time at the current speed, so a speed change moves the
  speed and nothing else. Memoised on the clock because two discs can be on screen in one
  frame - the badge and the prompt's - and advancing the phase once per *draw* would spin
  it at double rate on that screen.
*/
static int disc_step()
{
	unsigned long now = anim_ms();

	if (!disc_spd_to)
	{
		disc_spd_from = disc_spd_to = disc_target_speed();
		disc_ramp_t0 = now;
		disc_phase_ms = now;
		disc_step_ms = now;
	}

	if (now == disc_step_ms && disc_phase_ms) return disc_step_cached;

	unsigned long target = disc_target_speed();
	if (target != disc_spd_to)
	{
		// From wherever the ramp had reached, not from the old target: changing the
		// target mid-ramp must not snap the speed back.
		disc_spd_from = disc_speed_now(now);
		disc_spd_to = target;
		disc_ramp_t0 = now;
	}

	unsigned long dt = now - disc_phase_ms;
	/*
	  Capped. The front-end only draws when something changed, so the gap across a closed
	  menu or a long stall can be seconds - and an uncapped catch-up would spin the disc
	  through several turns the instant it came back.
	*/
	if (dt > 250) dt = 250;
	disc_phase_ms = now;

	disc_phase = (disc_phase + disc_speed_now(now) * dt / 1000UL) % DISC_TURN;

	disc_step_ms = now;
	disc_step_cached = (int)(disc_phase * 64UL / DISC_TURN);
	return disc_step_cached;
}

/*
  The iridescence, as a short ramp the wedges are taken from.

  Cool hues on purpose rather than a full rainbow: a real disc throws cyan through
  violet far more than it throws red, and this UI is dark and blue, so a saturated
  spectrum would look like a parrot landed on the shelf. One near-white wedge acts as
  the specular streak, which is what the eye actually tracks as it turns.
*/
static const uint32_t disc_bands[12] =
{
	0xffeef6ff,      // the specular streak, and the one either side of it
	0xffc8e8fa,
	0xff8fd8f0,
	0xff64bce0,
	0xff4894cc,
	0xff4878c0,
	0xff5866c4,
	0xff6858c0,
	0xff8058bc,
	0xff9860b4,
	0xff7a5cb0,
	0xff4f6aa8,
};

#define DISC_BANDS_N ((int)(sizeof(disc_bands) / sizeof(disc_bands[0])))

/*
  Radius in the units gfx_disc() wants: a multiple of 8, so the cells come out whole
  pixels. 8 gives a 16px icon at 240p; 16 gives a 32px one with 2x2 cells where there
  is room for it.
*/
static int disc_radius(const chome_profile *p)
{
	// Multiples of 16: gfx_disc draws a 32-cell sprite, so this is one pixel per cell
	// at 240p and 2x2 above it.
	return (p->ts_ui >= 2) ? 32 : 16;
}

/*
  Two lines and the air round them, in units of s. Wider than a settings row (12)
  because the second line is the whole point: it is where "paired, not awake" and
  "needs a password" go, which is what the old screens said in a column of symbols and
  abbreviations.

  Twenty-one and not twenty: the second line sits at 9 and is 8 tall, so a highlight
  that stopped at 20 cut the bottom two rows of pixels off it - and the one row that
  looked wrong was the row the player had selected.
*/
#define LIST_ROWH 21
#define LIST_GUT  12                         // the icon column, in units of s

static int chip_w(const char *text, int s)
{
	return gfx_text_w(text, s) + 6 * s;
}

/*
  A state word in a filled box. The ink is chosen from how light the box is rather
  than fixed: these boxes run from COL_DIM to COL_GREEN and either ink alone is
  unreadable on half of them.
*/
static void draw_chip(int x, int y, int s, const char *text, uint32_t bg)
{
	int lum = (int)((((bg >> 16) & 0xff) * 77 + ((bg >> 8) & 0xff) * 151 + (bg & 0xff) * 28) >> 8);
	gfx_fill(x, y - 2 * s, chip_w(text, s), 12 * s, bg);
	gfx_text(text, x + 3 * s, y, s, (lum > 140) ? COL_INK : COL_WHITE, 0);
}

struct list_row
{
	const char *title;
	const char *sub;                     // the second line, or 0 for one line
	const char *chip;                    // one word about its state, or 0
	uint32_t chip_col;
	const char *icon;                    // pictogram name, or 0
	int gut;                             // reserve the icon column even with no icon
	int busy;                            // spin in the icon column: this row is working
	uint32_t accent;                     // left stripe, or 0
	uint32_t sel_col;                    // the highlight when selected; 0 means COL_BLUE
	int rule;                            // a separator above this row
	int rsvd;                            // pixels kept clear at the right for the caller
};

/*
  Draws one row and returns the left edge of the reserved area, so a caller with its
  own gauge to draw - the Wi-Fi list's padlock and signal bars - can put it there
  without duplicating the arithmetic.
*/
static int draw_listrow(const panel_box *b, int y, const list_row *r, int on, unsigned long ms)
{
	int s = b->s;
	int x = b->x + 4 * s;
	int w = b->w - 8 * s;
	int h = LIST_ROWH * s - s;               // covers both lines; one pixel of gap below

	if (r->rule) gfx_fill(x, y - 5 * s, w, s, COL_PANELLO);

	// The highlight is the caller's, so a row that is armed for something destructive
	// can be red under the cursor without a second code path drawing it.
	if (on) gfx_fill(x, y - 2 * s, w, h, r->sel_col ? r->sel_col : COL_BLUE);

	/*
	  The stripe survives selection, which is why the state is a stripe and not the
	  row colour: the one row the player is looking at must not be the one row that
	  stops saying what it is.
	*/
	if (r->accent) gfx_fill(x, y - 2 * s, 2 * s, h, r->accent);

	uint32_t ink = on ? COL_WHITE : COL_INK;
	uint32_t dim = on ? COL_PANELHI : COL_DIM;

	int tx = x + 5 * s;
	if (r->icon || r->busy || r->gut)
	{
		int box = 8 * s;
		int cy = y - 2 * s + h / 2;

		if (r->busy) gfx_spinner(tx + box / 2, cy, box / 2 + s, s, ms, on ? COL_WHITE : COL_BLUE,
			on ? COL_BLUE : COL_PANELLO);
		else if (r->icon) picto(r->icon, tx, cy - box / 2, box, on ? COL_WHITE : COL_PANELLO);

		tx += LIST_GUT * s;
	}

	int rx = x + w - 4 * s - r->rsvd;
	if (r->chip)
	{
		int cw = chip_w(r->chip, s);
		draw_chip(rx - cw, y + (r->sub ? s : 0), s, r->chip, r->chip_col);
		rx -= cw + 4 * s;
	}

	gfx_text(gfx_clip(r->title, s, rx - tx), tx, y, s, ink, 0);
	if (r->sub) gfx_text(gfx_clip(r->sub, s, rx - tx), tx, y + 9 * s, s, dim, 0);

	return x + w - 4 * s - r->rsvd;
}

// A heading over a group of rows: the word, then a rule out to the edge. Costs one
// line, which is what makes grouping affordable on a panel sized for six rows.
#define LIST_SECH 10

static void draw_section(const panel_box *b, int y, const char *text)
{
	int s = b->s;

	char up[48];
	snprintf(up, sizeof(up), "%s", text);
	for (char *q = up; *q; q++) *q = (char)toupper((unsigned char)*q);

	// The word darker than its rule: at COL_PANELLO on COL_PANEL the heading was fainter
	// than the second line of the rows under it, which inverts what leads what.
	gfx_text(up, b->x + 6 * s, y, s, COL_DIM, 0);

	int tw = 6 * s + gfx_text_w(up, s) + 4 * s;
	if (tw < b->w - 8 * s) gfx_fill(b->x + tw, y + 4 * s, b->w - tw - 6 * s, s, COL_PANELLO);
}

/*
  The panel a player is looking at while something is happening to their machine.

  Three things, in the order the questions arrive: what is going on (the mark and the
  headline), how far along it is (the track), and what they should do about it (the
  body). The mark orbits while - and only while - the work is really running, which is
  the difference between a screen that is thinking and a screen that has hung. Both
  callers pass a step that came out of the tool's own output, so the track stopping is
  the job stopping.
*/
#define PMARK_BT   0
#define PMARK_WIFI 1

static void draw_bars(int x, int y, int s, int dbm, uint32_t on, uint32_t off);

static void draw_progress(const panel_box *b, int mark, const char *head, const char *body,
	int nseg, int step, const char *stepname, int busy, uint32_t tone, unsigned long ms)
{
	int s = b->s;
	int cx = b->x + b->w / 2;
	int box = 16 * s;

	char lines[4][64];
	int nl = wrap_text(body, (b->w - 16 * s) / (8 * s), lines, 3);

	/*
	  Centred in what it was given rather than starting at the top. The panel is sized
	  for the longest message either caller can produce - a panel that resized as the
	  wording changed under it would be worse than the space it saved - so a short one
	  leaves room over, and room split above and below reads as a centred dialog where
	  the same room all at the bottom reads as a panel that has lost something.

	  Never above 10*s from the top: the ring is drawn wider than the mark it goes
	  round, and its top would otherwise cross the panel header.
	*/
	int used = 10 * s + box + 8 * s + 13 * s + 8 * s + 14 * s + nl * 10 * s;
	int y = b->y + ((b->h - 12 * s) - used) / 2;    // 12*s is the footer both callers keep
	if (y < b->y + 10 * s) y = b->y + 10 * s;

	/*
	  Wi-Fi has no pictogram - the set has none and the list's own signal bars are the
	  mark this screen already uses - so it is drawn at full strength, at the size the
	  Bluetooth rune occupies, and means "a wireless network" rather than "this much
	  signal". Nothing here is a new drawing: see ICONS.md on why the bars are not one.
	*/
	if (mark == PMARK_WIFI) draw_bars(cx - 12 * s, y, 2 * s, 0, tone, tone);
	else picto("bluetooth", cx - box / 2, y, box, tone);

	if (busy) gfx_spinner(cx, y + box / 2, box * 3 / 4 + 2 * s, 2 * s, ms, tone, COL_PANELLO);

	y += box + 8 * s;
	gfx_text_c(gfx_clip(head, s, b->w - 12 * s), cx, y, s, tone, 0);

	y += 13 * s;
	gfx_track(b->x + 12 * s, y, b->w - 24 * s, 4 * s, nseg, step, busy, ms,
		COL_GREEN, COL_PANELLO, COL_WHITE);

	y += 8 * s;
	if (stepname) gfx_text_c(stepname, cx, y, s, COL_INK, 0);

	y += 14 * s;
	// COL_DIM, the same weight the second line of a list row has, and for the same
	// reason: it is the explanation under the thing, not the thing. It also reads on
	// this panel, which COL_PANELHI - light grey on light grey - barely did.
	for (int i = 0; i < nl; i++) gfx_text_c(lines[i], cx, y + i * 10 * s, s, COL_DIM, 0);
}

// What draw_progress needs below the panel header: the sum of the steps above, with
// room for three wrapped lines of body. Kept as one number because both callers size
// their panel from it and the two must not drift apart.
#define PROGRESS_H(s) (100 * (s))

static void draw_suspend(const chome_profile *p)
{
	if (strip_y <= 0.002) return;

	chome_item *it = cur_game();
	int ph = p->strip_h;

	// Comes to rest above the overscan margin, and its panel is extended down into
	// it so the bottom of the screen stays filled rather than showing a seam.
	int y = p->h - p->safe_y - (int)(ph * strip_y);

	gfx_fill(0, y, p->w, ph + p->safe_y, COL_BGDARK);
	gfx_fill(0, y, p->w, 2, COL_PANELLO);

	int s = p->ts_ui;
	int armed = (del_arm_slot >= 0 && !CheckTimer(del_arm_until));

	char hdr[128];
	if (armed) snprintf(hdr, sizeof(hdr), "DELETE SLOT %d? PRESS", del_arm_slot + 1);
	else snprintf(hdr, sizeof(hdr), "%s - SUSPEND POINTS", it ? it->title : "");
	for (char *q = hdr; *q; q++) *q = (char)toupper((unsigned char)*q);

	// Armed, the header draws the button rather than naming it - the legend under it is
	// showing that same button, and one of them saying "X" while the other drew a square
	// was the two of them describing different controllers.
	if (armed) btn_hint_l(p->inset, y + 6 * s, s, COL_RED, hdr, LBL_X, "AGAIN");
	else gfx_text(gfx_clip(hdr, s, p->w - p->inset * 2), p->inset, y + 6 * s, s, COL_PANELHI, 0);

	/*
	  A core with no savestate entries at all - most arcade hardware - can never fill
	  these, and three slots marked EMPTY invite a player to try. Say it plainly instead:
	  being told there is nothing to do here is a different thing from being told nothing,
	  and pressing the button and having it silently refuse is the worst of the three.

	  This is worth more from the shelf than it is in the game, which is where it started:
	  a player who has not started the game yet is the one still deciding whether to trust
	  it with an hour of their evening. See no_savestates_for() for where the answer comes
	  from when there is no core loaded to ask.
	*/
	if (no_savestates_for(it))
	{
		/*
		  Two lines, not three: the strip is only as tall as the row of slot tiles it
		  normally holds, and a third line fell past the bottom of it - where the shelf
		  card behind showed through and put a stray game title under the message.
		*/
		int s2 = p->ts_ui;
		char lines[4][64];
		int nl = wrap_text("This system cannot save your place - it has no save states.",
			(p->w - p->inset * 2 - 16 * s2) / (8 * s2), lines, 2);
		for (int i = 0; i < nl; i++)
			gfx_text_c(lines[i], p->w / 2, y + 22 * s2 + i * 11 * s2, s2, COL_PANELHI, 0);
		return;
	}

	int n = user_slots(), tw = p->thumb_w, th = p->thumb_h, gap = p->thumb_gap;
	int x0 = (p->w - (n * tw + (n - 1) * gap)) / 2;
	int ty = y + 18 * s + 6;

	const chome_entry *e = cur_entry();
	int aw = 0, ah = 0;
	const uint32_t *art = (e && e->kind == ENT_GAME) ? art_get(e->game, &aw, &ah) : 0;

	for (int i = 0; i < n; i++)
	{
		int x = x0 + i * (tw + gap);
		int st = slot_state(it, i);

		gfx_fill(x + 3, ty + 3, tw, th, COL_SHADOW);

		if (i == pend_slot)
		{
			/*
			  Waiting on the core. The picture is already there - it was taken when the
			  button was pressed - so it shows, with the word over it.
			*/
			const uint32_t *shot = 0;
			char tp[1024];
			if (it && lib_slot_thumb(it, i, tp, sizeof(tp))) shot = art_thumb(tp, tw, th);

			if (shot) gfx_blit(shot, tw, th, x, ty, tw, th);
			else gfx_fill(x, ty, tw, th, COL_BG);

			gfx_scrim(x, ty, tw, th, COL_SHADOW, 2);
			gfx_frame_rect(x, ty, tw, th, COL_YELLOW, 2);
			gfx_text_c("SAVING", x + tw / 2, ty + th / 2 - 4 * p->ts_tiny, p->ts_tiny, COL_YELLOW, 0);
		}
		else if (!st)
		{
			gfx_fill(x, ty, tw, th, COL_BG);
			gfx_frame_rect(x, ty, tw, th, COL_DIM, 1);
			gfx_text_c(i == pend_failed ? "NOT SAVED" : "EMPTY",
				x + tw / 2, ty + th / 2 - 4 * p->ts_tiny, p->ts_tiny,
				i == pend_failed ? COL_RED : COL_DIM, 0);
		}
		else
		{
			// Prefer the real capture written beside the savestate at save time
			// (process_ss -> screenshot_thumbnail). Fall back to the cover.
			const uint32_t *shot = 0;
			char tp[1024];
			if (it && lib_slot_thumb(it, i, tp, sizeof(tp))) shot = art_thumb(tp, tw, th);

			if (shot) gfx_blit(shot, tw, th, x, ty, tw, th);
			else if (art) gfx_blit(art, aw, ah, x, ty, tw, th);
			else gfx_fill(x, ty, tw, th, COL_BG);

			if (!shot) gfx_scrim(x, ty, tw, th, COL_SHADOW, 4);
			if (st == 2)
				gfx_text(CH_LOCK, x + tw - 9 * p->ts_tiny, ty + 3, p->ts_tiny, COL_YELLOW, 0);
			gfx_frame_rect(x, ty, tw, th, st == 2 ? COL_YELLOW : COL_GREEN, 2);
		}

		if (i == slot_idx) gfx_frame_rect(x - 3, ty - 3, tw + 6, th + 6, armed ? COL_RED : COL_FOCUS, 2);

		char cap[16];
		snprintf(cap, sizeof(cap), "%d", i + 1);
		gfx_text_c(cap, x + tw / 2, ty + th + 5, p->ts_tiny, COL_DIM, 0);
	}
}

/*
  The Display screen, laid out like the SNES Classic's: a row of preview tiles for
  the looks that apply to this game, each with a radio underneath, and a frame
  strip below it with arrows. Up/Down moves between the two zones, Left/Right
  within one.
*/
static void draw_radio(int cx, int cy, int r, int on)
{
	gfx_frame_rect(cx - r, cy - r, r * 2, r * 2, COL_INK, 1);
	if (on) gfx_fill(cx - r / 2, cy - r / 2, r, r, COL_RED);
}

/*
  A stored picture, cropped the same way the live frame is.

  Decoded larger than the tile and then taken from the middle at 1:1, so a preset's
  detail survives at the size it is judged. Deliberately not cached here: art_thumb()
  already caches the decode, and a second cache in front of it would be one more place
  to hand back the picture a file used to have - which is the bug this module has
  already had once.
*/
static const uint32_t *ref_zoom(const char *path, int w, int h)
{
	static uint32_t *buf = 0;
	static int bw = 0, bh = 0;

	if (!path || !*path || w < 1 || h < 1) return 0;

	int fw = w * 100 / VP_ZOOM_PCT;
	int fh = h * 100 / VP_ZOOM_PCT;
	const uint32_t *full = art_thumb(path, fw, fh);
	if (!full) return 0;

	if (!buf || bw != w || bh != h)
	{
		free(buf);
		buf = (uint32_t*)malloc((size_t)w * h * 4);
		if (!buf) { bw = bh = 0; return 0; }
		bw = w;
		bh = h;
	}

	int ox = (fw - w) / 2;
	int oy = (fh - h) / 2;
	for (int y = 0; y < h; y++)
		memcpy(buf + (size_t)y * w, full + (size_t)(oy + y) * fw + ox, (size_t)w * 4);

	return buf;
}

static void draw_display_screen(const chome_profile *p)
{
	int s = p->ts_ui;
	int tiny = p->ts_tiny;

	chome_item *it = cur_game();
	int sysidx = it ? it->sysidx : -1;
	int vclass = sel_class();

	int opts[VP_MAX_OPTIONS];
	int n = vp_options_for(vclass, opts);
	if (n < 1) return;
	if (look_row >= n) look_row = n - 1;

	/*
	  Height is derived from the content rather than guessed, so the bands cannot
	  collide and there is no dead space at the bottom. Vertical order:
	  header, tiles, tile labels, radios, description.
	*/
	int pw = (p->w * 92) / 100;
	int gap = 6 * s;
	int pad = 6 * s;

	int tile_w = (pw - pad * 2 - gap * (n - 1)) / n;
	int tile_h = (tile_w * 3) / 4;

	int h_title = 10 * s + 6;          // draw_panel_ex's own title bar
	int h_header = 13 * tiny;
	int h_label = 11 * tiny;
	int h_radio = 12 * s;
	int h_blurb = (p->id == PROF_LO) ? 0 : 2 * (10 * tiny);

	int ph = h_title + pad + h_header + tile_h + 4 * s + h_label + h_radio + h_blurb + pad;
	if (ph > (p->h * 94) / 100)
	{
		// Too tall for the canvas: give the tiles back the difference.
		int over = ph - (p->h * 94) / 100;
		tile_h -= over;
		if (tile_h < 24) tile_h = 24;
		ph = h_title + pad + h_header + tile_h + 4 * s + h_label + h_radio + h_blurb + pad;
	}

	panel_box b = draw_panel_ex(p, pw, ph, "Display");

	int in_use = vp_effective(sysidx, vclass);

	/*
	  Preview over the user's own game. Paused in-game we have the live frame,
	  which is as current as it gets; from the menu we fall back to the reference
	  shot captured during play, then to a suspend thumbnail.
	*/
	const uint32_t *ref = ig_active ? ig_live_ref(tile_w, tile_h) : 0;
	if (!ref)
	{
		char rp[1024];
		if (ref_shot_for(it, rp, sizeof(rp))) ref = ref_zoom(rp, tile_w, tile_h);
	}

	// Which hardware these looks belong to.
	char hdr[64];
	snprintf(hdr, sizeof(hdr), "FOR %s", vp_class_label(vclass));
	gfx_text(gfx_clip(hdr, tiny, b.w - 12 * s), b.x + pad, b.y + 2 * s, tiny, COL_PANELLO, 0);

	int top = b.y + 2 * s + h_header;
	int label_y = top + tile_h + 4 * s;
	int radio_y = label_y + h_label + h_radio / 2 - 2 * s;
	int blurb_y = label_y + h_label + h_radio;

	int total = tile_w * n + gap * (n - 1);
	int tx = b.x + (b.w - total) / 2;

	for (int i = 0; i < n; i++)
	{
		int x = tx + i * (tile_w + gap);
		int on = (i == look_row);

		if (on)
		{
			gfx_fill(x - 3 * s, top - 2 * s, tile_w + 6 * s,
				tile_h + 2 * s + h_label + h_radio, COL_BLUE);
		}

		const uint32_t *img = vp_preview(opts[i], tile_w, tile_h, ref);
		if (img) gfx_blit(img, tile_w, tile_h, x, top, tile_w, tile_h);
		else gfx_fill(x, top, tile_w, tile_h, COL_BGDARK);
		gfx_frame_rect(x - 1, top - 1, tile_w + 2, tile_h + 2, on ? COL_WHITE : COL_INK, 1);

		char up[64];
		snprintf(up, sizeof(up), "%s", vp_name(opts[i]));
		for (char *q = up; *q; q++) *q = (char)toupper((unsigned char)*q);
		gfx_text_c(gfx_clip(up, tiny, tile_w), x + tile_w / 2, label_y, tiny,
			on ? COL_WHITE : COL_INK, 0);

		draw_radio(x + tile_w / 2, radio_y, 4 * s, opts[i] == in_use);
	}

	// What the highlighted look actually is.
	if (h_blurb)
	{
		char lines[4][64];
		int cols = (b.w - pad * 2) / (GLYPH_W * tiny);
		int nl = wrap_text(vp_blurb(opts[look_row]), cols, lines, 2);
		for (int i = 0; i < nl; i++)
		{
			char up[64];
			snprintf(up, sizeof(up), "%s", lines[i]);
			for (char *q = up; *q; q++) *q = (char)toupper((unsigned char)*q);
			gfx_text_c(up, b.x + b.w / 2, blurb_y + i * 10 * tiny, tiny, COL_INK, 0);
		}
	}

	if (!vp_available(opts[look_row]))
	{
		gfx_text("FILES MISSING - OPTIONS > REINSTALL LOOKS", b.x + pad,
			b.y + b.h - 10 * tiny, tiny, COL_RED, 0);
	}
}

static void draw_options_panel(const chome_profile *p)
{
	panel_box b = draw_panel(p, "Options");

	static const char *rows_menu[] = { "Cover Art", "Rescan Library", "Reinstall Looks", "Menu Layout", "Controllers", "Wi-Fi", "Best Settings", "More Settings", "Advanced Settings", 0 };
	static const char *rows_game[] = { "Cover Art", "Rescan Library", "Reinstall Looks", "Menu Layout", "Controllers", "Wi-Fi", "Best Settings", "More Settings", "Core Settings", "Close Game" };
	const char *const *rows = ig_active ? rows_game : rows_menu;
	char v1[32];
	if (lib_scanning()) snprintf(v1, sizeof(v1), "%d...", lib_scan_progress());
	else snprintf(v1, sizeof(v1), "%d games", lib_item_count());

	int closing = (ig_active && opt_row == OPT_ROWS_GAME - 1 && !CheckTimer(ig_close_until));

	// What the Wi-Fi row says without being opened: the network name is the one
	// piece of it anybody wants to check in passing.
	char v2[40];
	if (!net_present()) snprintf(v2, sizeof(v2), "No Adapter");
	else
	{
		const net_link *l = net_link_now();
		if (l->up && l->ssid[0]) snprintf(v2, sizeof(v2), "%s", l->ssid);
		else snprintf(v2, sizeof(v2), "Set Up >");
	}

	/*
	  And what the Controllers row says: how many are paired, the same way the Wi-Fi row
	  names the network. "USB Only" rather than "No Adapter" because a wired pad is a
	  complete answer to the question this row is about - nothing is missing.
	*/
	char v3[32];
	if (!bt_present()) snprintf(v3, sizeof(v3), "USB Only");
	else if (!bt_count()) snprintf(v3, sizeof(v3), "Set Up >");
	else snprintf(v3, sizeof(v3), "%d Wireless", bt_count());

	// And what the Best Settings row says: how many of them the ini disagrees
	// with, so a player who has already run it is told there is nothing to do here.
	char v4[32];
	if (!ini_n) snprintf(v4, sizeof(v4), "All Set");
	else snprintf(v4, sizeof(v4), "%d To Change >", ini_n);

	// And what the More Settings row says: how many of the options it offers are away
	// from their default - the same count the amber on that screen is made of.
	char v5[32];
	if (!set_odd) snprintf(v5, sizeof(v5), "All Default");
	else snprintf(v5, sizeof(v5), "%d Changed >", set_odd);

	const char *vals[] = {
		cfg.classicui_artfetch ? "Fetch Missing" : "Local Only",
		v1,
		"Write Files",
		cfg.classicui_profile == 0 ? "Auto" : theme_get()->name,
		v3,
		v2,
		v4,
		v5,
		ig_active ? "Core Options >" : "Classic Menu >",
		closing ? "Again To Confirm" : "Back To Menu"
	};

	draw_rows(&b, rows, vals, ig_active ? OPT_ROWS_GAME : OPT_ROWS_MENU, opt_row);

	if (ig_active)
	{
		int s2 = p->ts_tiny;
		gfx_text(gfx_clip(closing ? "UNSAVED PROGRESS WILL BE LOST"
		                          : "THE GAME STAYS LOADED UNTIL YOU CLOSE IT",
			s2, b.w - 12 * s2), b.x + 6 * s2, b.y + b.h - 11 * s2, s2,
			closing ? COL_RED : COL_PANELLO, 0);
	}
}

/*
  Signal as four bars rather than a number. -67 dBm means nothing to the person
  standing in the room; three bars out of four does.
*/
static void draw_bars(int x, int y, int s, int dbm, uint32_t on, uint32_t off)
{
	int lv = 1;
	if (dbm >= -55) lv = 4;
	else if (dbm >= -65) lv = 3;
	else if (dbm >= -75) lv = 2;

	for (int i = 0; i < 4; i++)
	{
		int bh = (i + 1) * 2 * s;
		gfx_fill(x + i * 3 * s, y + 8 * s - bh, 2 * s, bh, i < lv ? on : off);
	}
}

/*
  Five, not six: the rows carry two lines now, and a network's row says what it is -
  "connected", "needs a password", "open" - instead of leaving that to a padlock the
  player has to know the meaning of. The sixth row was worth less than the sentence.
*/
#define WIFI_VIS 5

/*
  The band across the top: where this machine stands, in the two facts anybody asks
  for. It is a band and not a line of text because it is not one of the rows - it is
  what the rows are for.
*/
static void draw_wifi_hero(const panel_box *b, unsigned long ms)
{
	int s = b->s;
	int h = 24 * s;

	gfx_fill(b->x, b->y, b->w, h, COL_PANELHI);
	gfx_fill(b->x, b->y + h - s, b->w, s, COL_PANELLO);

	const net_link *l = net_link_now();
	int bx = b->x + 8 * s;
	int tx = bx + 30 * s;                    // the bars are 22 wide; 26 left them touching

	const char *head, *sub;
	char buf[96];

	if (l->up)
	{
		head = l->ssid;
		if (l->ip[0]) { snprintf(buf, sizeof(buf), "%s", l->ip); sub = buf; }
		else sub = "Getting an address";
	}
	else if (!net_present()) { head = "No Wi-Fi adapter"; sub = "Plug one into the USB port"; }
	else if (net_scanning())  { head = "Looking for networks"; sub = "This takes a few seconds"; }
	else { head = "Not connected"; sub = net_count() ? "Pick a network below" : "No networks found"; }

	/*
	  The bars show the link and only the link. Greyed out when there is not one, so the
	  band reads at a glance from across the room without anybody parsing the words -
	  which is the same job the bars do in the list.
	*/
	draw_bars(bx, b->y + 6 * s, 2 * s, l->up ? l->signal : -100,
		l->up ? COL_GREEN : COL_PANELLO, COL_PANEL);

	/*
	  And the ring, only while a scan is really running. It is here rather than in the
	  footer because this band is where the eye already is, and a scan that finishes
	  changes the words right underneath it.
	*/
	if (net_scanning() && !l->up)
		gfx_spinner(bx + 12 * s, b->y + 10 * s, 14 * s, 2 * s, ms, COL_BLUE, COL_PANEL);

	gfx_text(gfx_clip(head, s, b->w - (tx - b->x) - 8 * s), tx, b->y + 4 * s, s, COL_INK, 0);
	gfx_text(gfx_clip(sub, s, b->w - (tx - b->x) - 8 * s), tx, b->y + 14 * s, s, COL_DIM, 0);
}

static void draw_wifi(const chome_profile *p)
{
	int s = p->ts_ui;
	int rowh = LIST_ROWH * s;                // a list aimed at with a pad, not a mouse
	unsigned long ms = anim_ms();

	/*
	  Sized for a fixed number of rows rather than for the screen or for however many
	  networks turned up. Filling four fifths of a 720p canvas to list three networks
	  looks broken, and a panel that changes size as more are found is worse.
	*/
	int w = p->w - 2 * p->inset;
	if (w > 44 * 8 * s) w = 44 * 8 * s;

	int hdr = 10 * s + 6;
	int foot = 12 * s;

	int js = net_join_state();

	/*
	  A join owns the whole panel, and the panel is sized for it rather than for the
	  list underneath: a progress screen squeezed into a list's height had its body text
	  running off the bottom at 240p.
	*/
	int h = (js != JOIN_IDLE) ? hdr + PROGRESS_H(s) + foot
	                          : hdr + 24 * s + 6 * s + WIFI_VIS * rowh + foot + 6 * s;
	if (h > p->h - 2 * p->safe_y) h = p->h - 2 * p->safe_y;

	panel_box b = draw_panel_ex(p, w, h, "Wi-Fi");

	// While a join is running, or as soon as it has finished, that is the only thing
	// worth saying: the list underneath is about to be wrong either way.
	if (js != JOIN_IDLE)
	{
		const char *head;
		uint32_t tone = COL_INK;

		if (js == JOIN_WORK) head = "Connecting";
		else if (js == JOIN_OK) { head = "Connected"; tone = COL_GREEN; }
		else { head = "Could not connect"; tone = COL_RED; }

		char body[128];
		if (js == JOIN_WORK)
			snprintf(body, sizeof(body), "Joining %s. This can take a minute.", net_join_detail());
		else if (js == JOIN_OK)
			snprintf(body, sizeof(body), "This MiSTer is on %s.", net_join_detail());
		else if (js == JOIN_LOST)
			snprintf(body, sizeof(body), "%s", net_join_detail());
		else
			snprintf(body, sizeof(body), "%s Your old network was put back.", net_join_detail());

		/*
		  The step comes from the child doing the work, so the track advancing is the job
		  advancing and the track sitting still is the job sitting still. Rolling back is
		  drawn as no progress at all rather than as a nearly-full bar: it is not step five
		  of joining, it is the opposite of it.
		*/
		int phase = net_join_phase();
		int step = (js == JOIN_OK) ? JOIN_STEPS : (phase == JOIN_ROLLBACK) ? 0 : phase;

		draw_progress(&b, PMARK_WIFI, head, body, JOIN_STEPS, step,
			net_join_phase_name(js == JOIN_OK ? JOIN_STEPS : phase),
			js == JOIN_WORK, tone, ms);

		if (js != JOIN_WORK)
			btn_hint_c(b.x + b.w / 2, b.y + b.h - foot, s, COL_PANELLO, "", LBL_A, "OK");
		return;
	}

	// Nothing to show *and* no adapter is the only case worth explaining hardware
	// for; a list on screen is proof enough that there is one.
	if (!net_present() && !net_count())
	{
		char lines[4][64];
		int nl = wrap_text("Plug a USB Wi-Fi adapter into the MiSTer and come back to this screen.",
			(b.w - 16 * s) / (8 * s), lines, 3);
		for (int i = 0; i < nl; i++)
			gfx_text_c(lines[i], b.x + b.w / 2, b.y + b.h / 2 + i * 10 * s, s, COL_INK, 0);
		return;
	}

	draw_wifi_hero(&b, ms);

	int y0 = b.y + 24 * s + 6 * s;
	int vis = (b.y + b.h - foot - 2 * s - y0) / rowh;
	if (vis > WIFI_VIS) vis = WIFI_VIS;
	if (vis < 1) vis = 1;

	int n = net_count();
	if (!n)
	{
		/*
		  An empty list with a radio present. The ring is the whole message here: a scan
		  takes seconds and the words alone gave no sign it had not simply given up.
		*/
		int cy = b.y + b.h / 2;
		if (net_scanning()) gfx_spinner(b.x + b.w / 2, cy - 14 * s, 9 * s, 2 * s, ms, COL_BLUE, COL_PANELLO);

		gfx_text_c(net_scanning() ? "Looking for networks" : "No networks found",
			b.x + b.w / 2, cy, s, COL_INK, 0);

		if (!net_scanning())
			btn_hint_c(b.x + b.w / 2, b.y + b.h - foot, s, COL_PANELLO, "", LBL_X, "LOOK AGAIN");
		return;
	}

	if (wifi_row >= n) wifi_row = n - 1;
	if (wifi_row < 0) wifi_row = 0;
	if (wifi_row < wifi_top) wifi_top = wifi_row;
	if (wifi_row >= wifi_top + vis) wifi_top = wifi_row - vis + 1;

	// The padlock and the four bars, kept clear of the text by the row helper.
	int rsvd = 23 * s;

	for (int i = 0; i < vis && wifi_top + i < n; i++)
	{
		const net_ap *a = net_at(wifi_top + i);
		if (!a) break;

		int on = (wifi_top + i == wifi_row);
		int y = y0 + i * rowh;

		const net_link *l = net_link_now();

		list_row r;
		memset(&r, 0, sizeof(r));
		r.title = a->ssid;
		r.rsvd = rsvd;

		/*
		  What the second line says is what the player would otherwise have had to work
		  out from a dot and a padlock. The one we are on gets its address, because that
		  is the fact somebody on this screen came looking for.
		*/
		if (a->current)
		{
			r.accent = COL_GREEN;
			r.sub = (l->up && l->ip[0]) ? l->ip : "Connected";
		}
		else r.sub = a->secure ? "Needs a password" : "Open - no password";

		// The row helper hands back the left edge of what it kept clear, so the padlock
		// and the bars land inside that and flush with the panel's own right margin.
		int rx = draw_listrow(&b, y, &r, on, ms);

		uint32_t ink = on ? COL_WHITE : COL_INK;
		if (a->secure) gfx_text(CH_LOCK, rx, y + 5 * s, s, ink, 0);
		draw_bars(rx + 11 * s, y + 5 * s, s, a->signal, ink, on ? COL_PANELLO : COL_PANEL);
	}

	/*
	  A scan running while there is already a list to look at. The ring goes beside the
	  words rather than replacing them: the list stays usable while more arrive, and the
	  only thing that needs saying is that more may still be coming.
	*/
	if (net_scanning())
	{
		const char *msg = "LOOKING FOR MORE";
		int tw = gfx_text_w(msg, s);
		int cx = b.x + b.w / 2;

		gfx_spinner(cx - tw / 2 - 8 * s, b.y + b.h - foot + 3 * s, 5 * s, s, ms, COL_BLUE, COL_PANELLO);
		gfx_text(msg, cx - tw / 2, b.y + b.h - foot, s, COL_PANELLO, 0);
	}
	else if (n > vis)
	{
		char more[48];
		snprintf(more, sizeof(more), "%d of %d", wifi_row + 1, n);
		gfx_text_c(more, b.x + b.w / 2, b.y + b.h - foot, s, COL_PANELLO, 0);
	}
}

/*
  The Controllers screen: what is paired, and a way to pair something new.

  Deliberately not a list of things found nearby. `btctl pair` adopts any input device
  that appears while it is running, so the flow a player already knows from a console -
  hold the buttons on the pad until it pairs - works without anyone reading a list of
  names, and there is no moment where they have to recognise their own controller among
  the neighbours'. Which means this screen has one action, not two.

  Wired pads are not listed. They need no setting up, they are not in bluetoothctl's
  paired list, and a screen that showed them would invite the question of what to do
  about them - which is nothing.
*/
/*
  Six, not five: adding a controller is a row of its own now, so five visible rows means
  a fifth controller pushes it out of sight - and an entry nobody can see is worse than
  the shortcut it replaced. Five controllers plus the entry is more than any living room
  has.
*/
/*
  Five row-heights of list, and the rows carry two lines each now.

  The sixth was there so that a fifth controller could not push "Add a Controller" off
  the bottom, an entry nobody can see being worse than the shortcut it replaced. That
  is solved properly here instead: the entry is **pinned** to the foot of the panel and
  the controllers scroll above it, so it is on screen at any number of pads rather than
  at up to five of them. Four controllers show at once and the rest scroll, which is
  the shape the panel would want anyway - the action belongs at the bottom, not in the
  middle of a list it is not part of.
*/
#define PADS_VIS 5

/*
  Which heading a row belongs under. Three groups, in the order somebody scanning the
  screen wants them: what is working, what is paired but asleep, and the way to add
  another. It is also the sort key pads_build() uses, so the two cannot disagree.
*/
#define PG_READY  0
#define PG_ASLEEP 1
#define PG_ADD    2

static int pads_group(const pad_row *r)
{
	if (r->is_add) return PG_ADD;
	return r->player ? PG_READY : PG_ASLEEP;
}

static const char *pads_group_name(int g)
{
	// The add row gets no heading - a rule above it is enough, and a heading would
	// read as the name of a group with one thing in it.
	return (g == PG_READY) ? "Ready to play" : (g == PG_ASLEEP) ? "Paired, not awake" : 0;
}

/*
  Wired pads have nothing to set up, which was the argument for leaving them out - but he
  was right that it is the wrong call: a screen called Controllers that omits the
  controller in your hands reads as though it has not noticed it. Being told there is
  nothing to do is a different thing from being told nothing.

  Two sources, joined on the Bluetooth address: the pads MiSTer has given player numbers
  to (which is everything actually working, wired or not), and the paired devices that
  are not among them (which is a wireless pad that is off or has wandered off).
*/
static int pads_build(pad_row *out, int max)
{
	int n = 0;

	pad_info pads[8];
	int np = input_pad_list(pads, 8);

	for (int i = 0; i < np && n < max; i++)
	{
		pad_row *r = &out[n++];
		memset(r, 0, sizeof(*r));
		r->player = pads[i].player;
		r->kind = pads[i].kind;
		r->connected = 1;
		r->vid = pads[i].vid;
		r->pid = pads[i].pid;
		snprintf(r->name, sizeof(r->name), "%s",
			bt_pad_label(pads[i].vid, pads[i].pid, pads[i].name));
		snprintf(r->mac, sizeof(r->mac), "%s", pads[i].mac);
	}

	for (int i = 0; i < bt_count() && n < max; i++)
	{
		const bt_dev *d = bt_at(i);
		if (!d) break;

		// Already above, as a working controller.
		int seen = 0;
		for (int j = 0; j < n; j++) if (out[j].mac[0] && !strcasecmp(out[j].mac, d->mac)) { seen = 1; break; }
		if (seen) continue;

		pad_row *r = &out[n++];
		memset(r, 0, sizeof(*r));
		r->kind = PAD_BT;
		r->connected = d->connected;
		snprintf(r->name, sizeof(r->name), "%s", d->name[0] ? d->name : "Controller");
		snprintf(r->mac, sizeof(r->mac), "%s", d->mac);
	}

	/*
	  Adding one is the last entry rather than a button on the legend. Derek's call, and
	  the right one: a shortcut is a thing you have to be told about, and the row above it
	  has just shown the player what a controller looks like on this screen. It also frees
	  A to mean the obvious thing on a controller row - open it - which is what the tester
	  needed.

	  Only with a radio to do it with. bt_count() alone is a card that still remembers
	  pairings from a dongle that is not plugged in; offering to pair with it would take
	  the player to a panel that can only fail.
	*/
	/*
	  Grouped before the entry that adds one goes on the end, so the screen can put a
	  heading over each group and the row indices the handlers use are the ones on
	  screen. It comes out grouped already on every arrangement seen so far - the pads
	  with player numbers are read from one source and the sleeping ones from another -
	  but "already sorted" is not a property of the two sources, it is a coincidence of
	  them, and a heading over a list that turns out not to be grouped is a lie.

	  Insertion sort, stable, on a handful of rows: within a group the order the two
	  sources gave them is worth keeping, because that is player order.
	*/
	for (int i = 1; i < n; i++)
	{
		pad_row tmp = out[i];
		int g = pads_group(&tmp);
		int j = i;
		while (j > 0 && pads_group(&out[j - 1]) > g) { out[j] = out[j - 1]; j--; }
		out[j] = tmp;
	}

	if (bt_present() && n < max)
	{
		pad_row *r = &out[n++];
		memset(r, 0, sizeof(*r));
		r->is_add = 1;
		snprintf(r->name, sizeof(r->name), "Add a Controller");
	}

	return n;
}

// The row under the cursor, for the handlers. Rebuilt rather than cached: it is a scan
// of thirty slots and a dozen paired devices, and a stale copy would act on the wrong pad.
static int pads_sel(pad_row *out)
{
	pad_row rows[PADS_MAX];
	int n = pads_build(rows, PADS_MAX);
	if (!n) return 0;

	if (pads_row >= n) pads_row = n - 1;
	if (pads_row < 0) pads_row = 0;

	*out = rows[pads_row];
	return 1;
}

static int pads_count()
{
	pad_row rows[PADS_MAX];
	return pads_build(rows, PADS_MAX);
}

static void draw_pads(const chome_profile *p)
{
	int s = p->ts_ui;
	int rowh = LIST_ROWH * s;
	unsigned long ms = anim_ms();

	int w = p->w - 2 * p->inset;
	if (w > 44 * 8 * s) w = 44 * 8 * s;

	int hdr = 10 * s + 6;
	int foot = 12 * s;

	int pairing = (bt_pairing() || bt_pair_state() != BTP_IDLE);

	/*
	  Two heights, because the two things this screen does want different shapes: a
	  list wants rows, a pairing wants room for a mark, a track and three lines of what
	  to do with your hands. The pairing panel used to be squeezed into the list's
	  height, which is why it had no footer.
	*/
	int h = pairing ? hdr + PROGRESS_H(s) + foot
	                : hdr + 18 * s + PADS_VIS * rowh + 2 * LIST_SECH * s + foot + 4 * s;
	if (h > p->h - 2 * p->safe_y) h = p->h - 2 * p->safe_y;

	panel_box b = draw_panel_ex(p, w, h, "Controllers");

	// Pairing mode owns the screen while it is on: the list underneath is what this is
	// about to change, and the player is holding a button down waiting to be told.
	/*
	  Any state but idle owns the screen, not just a running one: pairing mode now ends
	  itself the moment a controller works, and the result has to stay up to be read.
	*/
	if (pairing)
	{
		int st = bt_pair_state();

		const char *head = "Hold the buttons on your controller";
		uint32_t tone = COL_INK;

		if (st == BTP_WORKING || st == BTP_PIN) { head = bt_pair_name()[0] ? bt_pair_name() : "Found a controller"; }
		else if (st == BTP_OK) { head = "Paired"; tone = COL_GREEN; }
		else if (st == BTP_FAIL) { head = "Not paired"; tone = COL_RED; }

		/*
		  Which buttons, per pad, is a table this does not have - so it says the thing
		  that is true of all of them rather than guessing at one. The detail line
		  underneath is btctl's own progress, in words a player can act on.
		*/
		const char *body = bt_pair_detail();

		/*
		  ...unless btctl's commentary is the same words the track is already labelled
		  with, which "Found it" and "Pairing" both are. Two lines saying the same thing
		  is not progress, and what is worth saying while nothing new has happened is
		  what the player's hands should be doing.
		*/
		if (!body[0] || !strcasecmp(body, bt_pair_step_name(bt_pair_step())))
			body = "Most controllers pair by holding two buttons until the light flashes quickly.";

		/*
		  The track is read off btctl's own commentary (bt_pair_step), so it advances
		  when the pairing advances and stops when it stops. A failure is left showing
		  how far it got rather than emptied, because "it got as far as connecting" and
		  "it never saw the pad" are different things to try next.
		*/
		int step = bt_pair_step();
		int busy = (st == BTP_LOOKING || st == BTP_WORKING || st == BTP_PIN);

		draw_progress(&b, PMARK_BT, head, body, BTP_STEPS,
			(st == BTP_FAIL) ? step : step, bt_pair_step_name(step), busy, tone, ms);

		if (bt_pair_done() > 0)
		{
			char done[64];
			snprintf(done, sizeof(done), (bt_pair_done() == 1) ? "%d controller ready" : "%d controllers ready",
				bt_pair_done());
			gfx_text_c(done, b.x + b.w / 2, b.y + b.h - foot, s, COL_GREEN, 0);
		}
		return;
	}

	pad_row rows[PADS_MAX];
	int n = pads_build(rows, PADS_MAX);

	/*
	  An empty list now means no adapter *and* nothing plugged in, since the adapter puts
	  "Add a Controller" in the list on its own. So the two empty states collapse into the
	  one sentence that explains the hardware - and the wired pads, which used to be hidden
	  behind that sentence whenever no dongle was present, are listed as they should be.
	*/
	if (!n)
	{
		char lines[4][64];
		int nl = wrap_text("This MiSTer has no Bluetooth adapter. A controller plugged into "
			"the USB port works without any setting up.",
			(b.w - 16 * s) / (8 * s), lines, 3);
		for (int i = 0; i < nl; i++)
			gfx_text_c(lines[i], b.x + b.w / 2, b.y + b.h / 2 - 10 * s + i * 10 * s, s, COL_INK, 0);
		return;
	}

	// The count is of controllers; the entry that adds one is not a controller.
	int nreal = 0, nplay = 0;
	for (int i = 0; i < n; i++)
	{
		if (rows[i].is_add) continue;
		nreal++;
		if (rows[i].player) nplay++;
	}

	char hdrtext[96];
	if (!nreal) snprintf(hdrtext, sizeof(hdrtext), "No controllers found");
	else if (nplay == nreal) snprintf(hdrtext, sizeof(hdrtext), (nreal == 1) ? "%d controller ready" : "%d controllers ready", nreal);
	else snprintf(hdrtext, sizeof(hdrtext), "%d ready, %d asleep", nplay, nreal - nplay);
	gfx_text(gfx_clip(hdrtext, s, b.w - 12 * s), b.x + 6 * s, b.y + 3 * s, s, COL_INK, 0);
	gfx_fill(b.x + 6 * s, b.y + 13 * s, b.w - 12 * s, s, COL_PANELLO);

	int y0 = b.y + 18 * s;
	int ybot = b.y + b.h - foot - 2 * s;

	/*
	  The entry that adds a controller is pinned to the foot of the panel and the
	  controllers scroll above it, so it is reachable at any number of pads. pads_build()
	  always puts it last, which is also where the cursor reaches it from the bottom of
	  the list, so the pinning is only about where it is drawn.
	*/
	int addrow = (n && rows[n - 1].is_add) ? n - 1 : -1;
	int nlist = (addrow >= 0) ? n - 1 : n;
	if (addrow >= 0) ybot -= rowh;

	/*
	  How many rows fit, allowing for the headings, which cost a line each. Worked out
	  against the worst case of both headings being on screen rather than measured
	  against this particular list: the count is also what decides where the list is
	  scrolled to, and a window that changed size as the selection moved through it
	  would scroll under the player's cursor.
	*/
	int vis = (ybot - y0 - 2 * LIST_SECH * s) / rowh;
	if (vis > PADS_VIS - (addrow >= 0 ? 1 : 0)) vis = PADS_VIS - (addrow >= 0 ? 1 : 0);
	if (vis < 1) vis = 1;

	if (pads_row >= n) pads_row = n - 1;
	if (pads_row < 0) pads_row = 0;

	/*
	  Scrolled by the cursor when it is on a controller. The pinned entry is not in the
	  scrolling area at all, so when the cursor is on it the list holds at its end -
	  which is where the cursor came from, and which keeps the last controller beside
	  the entry that would add another.
	*/
	int top = 0;
	if (pads_row < nlist && pads_row >= vis) top = pads_row - vis + 1;
	else if (pads_row == addrow && nlist > vis) top = nlist - vis;

	int armed = (pads_forget_arm >= 0 && !CheckTimer(pads_forget_until));

	int y = y0;
	int lastg = -1;
	int shown = 0;

	/*
	  Capped at `vis` as well as at the room left, so the count in the footer is the
	  count on screen. Without the cap a list with only one heading in view drew a row
	  more than the scrolling arithmetic believed was there, and said "1 of 5" over five
	  visible rows.
	*/
	for (int i = top; i < nlist && shown < vis; i++)
	{
		const pad_row *r = &rows[i];
		int g = pads_group(r);

		int need = rowh + ((g != lastg && pads_group_name(g)) ? LIST_SECH * s : 0);
		if (y + need > ybot) break;

		if (g != lastg)
		{
			const char *nm = pads_group_name(g);
			if (nm) { draw_section(&b, y, nm); y += LIST_SECH * s; }
			lastg = g;
		}

		const char *how = (r->kind == PAD_BT) ? "Wireless"
			: (r->kind == PAD_SNAC) ? "SNAC port" : "Plugged in";

		list_row lr;
		memset(&lr, 0, sizeof(lr));
		lr.gut = 1;
		lr.icon = (r->kind == PAD_BT) ? "bluetooth" : 0;
		lr.title = r->name;

		char sub[80], chip[8];

		/*
		  The player number was a two-letter column on the left and "--" where there was
		  none, which is a symbol somebody has to be taught. It is a chip on the right
		  saying P1, or the word ASLEEP - and the second line says what to do about it,
		  which is the thing the old row could not say at all.
		*/
		if (r->player)
		{
			snprintf(chip, sizeof(chip), "P%d", r->player);
			snprintf(sub, sizeof(sub), "%s", how);
			lr.chip = chip;
			lr.chip_col = COL_GREEN;
			lr.accent = COL_GREEN;
		}
		else
		{
			// Short enough to survive the chip beside it at 240p, where the row is
			// twenty-odd characters wide: longer wordings came out clipped to
			// "Press a button to wake >".
			snprintf(sub, sizeof(sub), "Press a button on it");
			lr.chip = "ASLEEP";
			lr.chip_col = COL_PANELLO;
		}

		lr.sub = sub;

		// Armed to be forgotten: the row itself turns red, which is the same language
		// the suspend strip uses for the same two-press confirmation.
		if (armed && pads_forget_arm == i) lr.sel_col = COL_RED;

		draw_listrow(&b, y, &lr, i == pads_row, ms);
		y += rowh;
		shown++;
	}

	if (addrow >= 0)
	{
		/*
		  The wireless mark goes in the icon column, where every other row's mark says how
		  that controller is attached. Here it says what kind of controller this would
		  add, which is the same column doing the same job.
		*/
		list_row lr;
		memset(&lr, 0, sizeof(lr));
		lr.gut = 1;
		lr.icon = "bluetooth";
		lr.title = rows[addrow].name;
		lr.sub = "Put a controller into pairing mode";
		lr.accent = COL_BLUE;
		lr.rule = 1;                         // an action, not another controller

		draw_listrow(&b, ybot + 3 * s, &lr, pads_row == addrow, ms);
	}

	if (armed)
		btn_hint_c(b.x + b.w / 2, b.y + b.h - foot, s, COL_RED,
			"PRESS", LBL_X, "AGAIN TO FORGET IT");
	else if (nlist > vis)
	{
		// Counts controllers, not entries: the pinned one is always on screen, so
		// including it would say "5 of 6" while six things were visible.
		char more[48];
		int at = (pads_row < nlist) ? pads_row + 1 : nlist;
		snprintf(more, sizeof(more), "%d of %d", at, nlist);
		gfx_text_c(more, b.x + b.w / 2, b.y + b.h - foot, s, COL_PANELLO, 0);
	}
}

/*
  The controller tester.

  What a player wants here is not diagnostics, it is two questions: does this pad work,
  and which button is which. So it is a picture of a controller with every control on it,
  each one lighting up as it is pressed, and nothing to read.

  The face buttons are the pad's own shapes, resolved from the codes the device reports
  (input_pad_state) through the very tables the legend uses - so a DualShock shows the
  four shapes where they sit on a DualShock, and a Super Famicom pad shows its four
  coloured letters. Nothing on this screen is a new drawing: the faces and the two pills
  are the 12-pixel glyphs of chome_btn12.h, and the directions are the OSD font's own
  arrow codes.

  The pad is looked up again on every frame rather than captured on the way in, because
  the interesting case is a controller that was asleep: pressing a button on it is how it
  gets a player number, and it must be this screen that shows that happening.
*/
static int padtest_find(pad_row *out)
{
	pad_row rows[PADS_MAX];
	int n = pads_build(rows, PADS_MAX);

	for (int i = 0; i < n; i++)
	{
		if (rows[i].is_add) continue;

		/*
		  The address is what survives a wireless pad going to sleep and coming back; a
		  wired one has none, so its name and how it is attached are all there is - which
		  is enough, since two identical wired pads differ only by player number and
		  either one is a fair answer to "test this".
		*/
		int same = padtest_mac[0]
			? (rows[i].mac[0] && !strcasecmp(rows[i].mac, padtest_mac))
			: (rows[i].kind == padtest_kind && !strcmp(rows[i].name, padtest_name));

		if (!same) continue;
		*out = rows[i];
		return 1;
	}
	return 0;
}

// One control's cell. Twelve pixels, the same grid the button glyphs are drawn on.
#define PT_CELL 12

/*
  Lit is green behind the glyph, not a different glyph: the shape has to stay recognisable
  while it is pressed, or the screen answers "which button is this" differently depending
  on whether you are pressing it.
*/
static void pt_glyph(const char *pic, int x, int y, int s, uint32_t col, int held)
{
	const btn12_def *d = btn12_find(pic);
	if (!d) return;

	if (held) gfx_fill(x - s, y - s, btn12_w(d) * s + 2 * s, PT_CELL * s + 2 * s, COL_GREEN);
	btn12_draw(d, x, y, s, col ? col : COL_WHITE, !held);
}

// The controls with no glyph of their own: the four directions and the two shoulders.
static void pt_key(const char *text, int x, int y, int s, int held)
{
	gfx_fill(x - 2 * s, y - 2 * s, PT_CELL * s, PT_CELL * s, held ? COL_GREEN : COL_BTN_CHIP);
	gfx_text(text, x, y, s, held ? COL_WHITE : COL_DIM, 0);
}

// Where a face button sits on the pad: 0 north, 1 east, 2 south, 3 west. -1 for a code
// that is not one of the four, which is left to take whichever slot is still free.
static int code_pos(uint16_t code)
{
	switch (code)
	{
	case BTN_CODE_NORTH: return 0;
	case BTN_CODE_EAST:  return 1;
	case BTN_CODE_SOUTH: return 2;
	case BTN_CODE_WEST:  return 3;
	}
	return -1;
}

// A stick, as a box with the tip in it. -128..127 in, box coordinates out.
static void pt_stick(int x, int y, int box, int s, int sx, int sy, int live)
{
	gfx_fill(x, y, box, box, COL_BGDARK);
	gfx_frame_rect(x, y, box, box, live ? COL_PANELLO : COL_DIM, s);

	int dot = 4 * s;
	int span = box - dot - 2 * s;
	int cx = x + s + ((sx + 128) * span) / 255;
	int cy = y + s + ((sy + 128) * span) / 255;

	// Centred is at rest and says nothing; off centre is the player moving it.
	int moved = (sx < -8 || sx > 8 || sy < -8 || sy > 8);
	gfx_fill(cx, cy, dot, dot, moved ? COL_GREEN : COL_PANELLO);
}

static void draw_padtest(const chome_profile *p)
{
	int s = p->ts_ui;

	pad_row row;
	int have = padtest_find(&row);
	int player = have ? row.player : 0;

	pad_state st;
	int live = input_pad_state(player, &st);

	const char *nm = (have && row.name[0]) ? row.name : padtest_name;
	int layout = pad_layout_for(have ? (((uint32_t)row.vid << 16) | row.pid) : 0, nm);

	int w = p->w - 2 * p->inset;
	if (w > 44 * 8 * s) w = 44 * 8 * s;

	// The stick band is only there for a pad that has sticks. A SNAC pad is a digital
	// PlayStation pad and two empty boxes would be a question it cannot answer.
	int sticks = live && st.sticks;
	int h = (10 * s + 6) + 25 * s + 46 * s + (sticks ? 30 * s : 0) + 16 * s;
	if (h > p->h - 2 * p->safe_y) h = p->h - 2 * p->safe_y;

	panel_box b = draw_panel_ex(p, w, h, "Controller Test");

	int cy = b.y + 3 * s;
	gfx_text_c(gfx_clip(nm[0] ? nm : "Controller", s, b.w - 12 * s), b.x + b.w / 2, cy, s, COL_INK, 0);

	cy += 11 * s;
	if (live)
	{
		char pl[32];
		snprintf(pl, sizeof(pl), "Player %d", player);
		gfx_text_c(pl, b.x + b.w / 2, cy, s, COL_GREEN, 0);
	}
	else
	{
		gfx_text_c(gfx_clip("Asleep - press a button on it", s, b.w - 12 * s),
			b.x + b.w / 2, cy, s, COL_PANELLO, 0);
	}

	int y = cy + 14 * s;                       // top of the diagram band, 46*s tall
	int dcx = b.x + b.w / 6;                   // d-pad
	int mcx = b.x + b.w / 2;                   // Select and Start
	int fcx = b.x + (5 * b.w) / 6;             // face buttons

	uint32_t held = st.held;

	// Shoulders, along the top, where they are on the pad.
	pt_key("L", b.x + 10 * s, y, s, held & (1u << SYS_BTN_L));
	pt_key("R", b.x + b.w - 20 * s, y, s, held & (1u << SYS_BTN_R));

	pt_key(CH_UP,    dcx - 4 * s,  y + 6 * s,  s, held & (1u << SYS_BTN_UP));
	pt_key(CH_LEFT,  dcx - 16 * s, y + 18 * s, s, held & (1u << SYS_BTN_LEFT));
	pt_key(CH_RIGHT, dcx + 8 * s,  y + 18 * s, s, held & (1u << SYS_BTN_RIGHT));
	pt_key(CH_DOWN,  dcx - 4 * s,  y + 30 * s, s, held & (1u << SYS_BTN_DOWN));

	{
		const btn12_def *d = btn12_find("btn_select");
		if (d) pt_glyph("btn_select", mcx - btn12_w(d) * s / 2, y + 10 * s, s, 0,
			held & (1u << SYS_BTN_SELECT));

		d = btn12_find("btn_start");
		if (d) pt_glyph("btn_start", mcx - btn12_w(d) * s / 2, y + 26 * s, s, 0,
			held & (1u << SYS_BTN_START));
	}

	/*
	  The four faces, placed by where the pad says they are rather than in a fixed order.
	  A button whose code is none of the four - unmapped, or one of the HID pads that
	  report their faces as BTN_THUMB and friends - takes whichever slot is still free, so
	  every button the player can press is on the diagram even when its position is a
	  guess. Better a guessed position than a missing button on a screen whose whole job
	  is to prove the pad works.
	*/
	static const int slot_dx[4] = { -6, 8, -6, -20 };   // north, east, south, west
	static const int slot_dy[4] = { 2, 16, 30, 16 };

	int taken[4] = { -1, -1, -1, -1 };
	for (int i = LBL_A; i <= LBL_Y; i++)
	{
		int pos = code_pos(st.code[lbl_sysbtn[i]]);
		if (pos < 0 || taken[pos] >= 0)
		{
			pos = -1;
			for (int j = 0; j < 4; j++) if (taken[j] < 0) { pos = j; break; }
			if (pos < 0) continue;
		}
		taken[pos] = i;
	}

	for (int pos = 0; pos < 4; pos++)
	{
		int which = taken[pos];
		if (which < 0) continue;

		int sb = lbl_sysbtn[which];
		prompt pr = prompt_for_code(layout, which, st.code[sb]);

		const char *pic = pr.pic;
		uint32_t col = pr.col;
		if (!pic) { pic = letter_pic[which]; col = letter_col(layout, which); }

		pt_glyph(pic, fcx + slot_dx[pos] * s, y + slot_dy[pos] * s, s, col,
			held & (1u << sb));
	}

	if (sticks)
	{
		int box = 24 * s;
		int sy = y + 48 * s;
		pt_stick(b.x + b.w / 3 - box / 2, sy, box, s, st.lx, st.ly, 1);
		pt_stick(b.x + (2 * b.w) / 3 - box / 2, sy, box, s, st.rx, st.ry, 1);
	}

	if (padtest_back_arm && !CheckTimer(padtest_back_until))
		btn_hint_c(b.x + b.w / 2, b.y + b.h - 11 * s, s, COL_RED, "Press", LBL_B, "again to go back");
	else
		btn_hint_c(b.x + b.w / 2, b.y + b.h - 11 * s, s, COL_INK, "Press", LBL_B, "twice to go back");
}

/*
  Restart and Shut Down.

  A console is turned off by its switch, but a MiSTer is a computer with a card in it and
  pulling the power mid-write is how a library gets corrupted - so there has to be a way
  to ask. Two presses, like anything else here that cannot be undone, and it syncs first.
*/
#define PWR_ROWS 2

/* ------------------------------------------------------------------ disc --- */

/*
  The prompt for a disc in the drive.

  Two shapes, because there are two situations and they need different answers:

    - we recognised the disc and this firmware has a core for it: offer to play it,
      and offer to override the choice anyway (a Mega Drive+ disc is a real case
      where the player may want the other core).
    - we did not, or the core does not exist here (Saturn, 3DO and CD-i are all
      identified and all have no shelf system): ask which core to try.

  What is deliberately *not* here yet is playing. Handing a disc to a core needs each
  CD core's firmware-side daemon taught to read sectors from the drive rather than
  from a .cue, which is the piece that has not been merged - so the action says so
  rather than pretending, and records the choice for when it lands. A prompt that
  silently did nothing would be worse than one that explains itself.
*/

// Defined further down, with the rest of the navigation.
static void go_screen(int s);

#define DISC_ROW_MAX 8

#define DACT_PLAY   0
#define DACT_CHOOSE 1

static int disc_row = 0;
static int disc_picking = 0;                  // 0: the offer, 1: choosing a core
static char disc_rowtext[DISC_ROW_MAX][48];
static int  disc_rowact[DISC_ROW_MAX];
static int  disc_rowsys[DISC_ROW_MAX];        // system index, -1 when not a system
static int  disc_nrows = 0;

// Which chosen core the player last picked for this disc, so re-opening the prompt
// shows what they decided rather than starting over.
static int disc_chosen_sys = -1;

static int disc_sys_by_id(const char *id)
{
	if (!id) return -1;
	for (int i = 0; i < lib_sys_count(); i++)
	{
		const chome_sys *sc = lib_sys(i);
		if (sc && !strcasecmp(sc->id, id)) return i;
	}
	return -1;
}

static void disc_build_rows()
{
	disc_nrows = 0;

	if (disc_picking)
	{
		/*
		  Only systems that are actually in this library. A card offering to load the
		  Neo Geo core on a machine with no Neo Geo core would be a dead end.
		*/
		const char *ids[16];
		int n = disc_capable_systems(ids, 16);

		for (int i = 0; i < n && disc_nrows < DISC_ROW_MAX; i++)
		{
			int sx = disc_sys_by_id(ids[i]);
			if (sx < 0) continue;

			const chome_sys *sc = lib_sys(sx);
			snprintf(disc_rowtext[disc_nrows], sizeof(disc_rowtext[0]), "%s", sc->name);
			disc_rowact[disc_nrows] = DACT_PLAY;
			disc_rowsys[disc_nrows] = sx;
			disc_nrows++;
		}
		return;
	}

	int match = disc_chosen_sys;
	if (match < 0) match = disc_sys_by_id(disc_system_id(disc_type()));

	if (match >= 0)
	{
		const chome_sys *sc = lib_sys(match);
		snprintf(disc_rowtext[disc_nrows], sizeof(disc_rowtext[0]), "Play on %s", sc->name);
		disc_rowact[disc_nrows] = DACT_PLAY;
		disc_rowsys[disc_nrows] = match;
		disc_nrows++;
	}

	if (disc_nrows < DISC_ROW_MAX)
	{
		snprintf(disc_rowtext[disc_nrows], sizeof(disc_rowtext[0]),
			match >= 0 ? "Use a different core" : "Choose a core");
		disc_rowact[disc_nrows] = DACT_CHOOSE;
		disc_rowsys[disc_nrows] = -1;
		disc_nrows++;
	}
}

static int disc_rows()
{
	disc_build_rows();
	return disc_nrows;
}

static void disc_open_screen()
{
	disc_row = 0;
	disc_picking = 0;
	disc_build_rows();
	go_screen(SCR_DISC);
}

static void draw_disc(const chome_profile *p)
{
	disc_build_rows();

	int ps = p->ts_ui;

	/*
	  The disc is the subject of this screen, so the panel is as wide as the inset allows
	  rather than the usual 34 characters, and the disc is sized off the canvas height
	  rather than off the width: a sixth of the height reads as large at every profile
	  without a 240p panel swallowing the screen. Snapped to a multiple of 8 so
	  gfx_disc's cells stay whole pixels.
	*/
	int r = (p->h / 6) & ~7;
	if (r < 16) r = 16;

	/*
	  The panel is sized to its contents rather than to the screen: the disc plus a text
	  column wide enough for the longest row - "Play on PlayStation" is nineteen
	  characters. Stretching it to the full width instead left half the panel empty,
	  which looked like a mistake rather than a design.

	  If that will not fit the canvas the disc gives the room back, because a clipped row
	  is worse than a smaller disc.
	*/
	int tcol = 22 * 8 * ps;
	int maxw = p->w - 2 * p->inset;

	while (r > 16 && 2 * r + tcol + 24 * ps > maxw) r -= 8;

	int pw = 2 * r + tcol + 24 * ps;
	if (pw > maxw) { pw = maxw; tcol = pw - 2 * r - 24 * ps; }

	int nrows = disc_nrows ? disc_nrows : 1;
	int rows_h = 24 * ps + nrows * 14 * ps;

	int body_h = 2 * r + 8 * ps;
	if (body_h < rows_h + 8 * ps) body_h = rows_h + 8 * ps;

	int ph = (10 * ps + 6) + body_h + 8 * ps;

	panel_box b = draw_panel_ex(p, pw, ph, disc_picking ? "Which core?" : "Disc");
	int s = b.s;

	// Centred in the body, which is as tall as the disc or the rows, whichever is taller.
	int cx = b.x + 8 * s + r;
	int cy = b.y + body_h / 2;

	/*
	  Always the art when there is art. There is none yet - a physical disc has no
	  filename to match on, so it needs a serial-to-title table and a scraper that this
	  build has no credential for - and until then the drawn disc stands in. When art
	  arrives it belongs here, masked to the same circle.
	*/
	gfx_disc(cx, cy, r, disc_step(),
		disc_bands, DISC_BANDS_N, COL_WHITE, COL_PANELHI, COL_BGDARK, 0);

	int tx = cx + r + 10 * s;
	int tw = tcol;
	int y = b.y + 6 * s;

	const char *what = (disc_state() == DISC_SPINNING) ? "Reading the disc"
		: (disc_state() == DISC_UNKNOWN) ? "Unrecognised disc"
		: disc_type_name(disc_type());

	gfx_text(gfx_clip(what, s, tw), tx, y, s, COL_WHITE, 0);

	const char *name = disc_display_name();
	if (name && name[0] && strcmp(name, what))
	{
		gfx_text(gfx_clip(name, s, tw), tx, y + 10 * s, s, COL_PANELHI, 0);
	}

	if (disc_state() == DISC_SPINNING) return;

	int ry = y + 24 * s;
	int rowh = 14 * s;

	for (int i = 0; i < disc_nrows; i++)
	{
		int on = (i == disc_row);
		int yy = ry + i * rowh;

		if (on) gfx_fill(tx - 4 * s, yy - 3 * s, tw + 8 * s, rowh - 2 * s, COL_BLUE);
		gfx_text(gfx_clip(disc_rowtext[i], s, tw), tx, yy, s, on ? COL_WHITE : COL_INK, 0);
	}
}

/*
  The disc indicator: a spinning disc in the top-left corner whenever there is one in
  the drive, on the shelf and behind the in-game menu alike.

  Top-left because every other edge is taken - the title is centred at the top, the
  legend and the suspend strip are along the bottom, and the menu bar slides down over
  the top-centre. Inside the overscan margin, like everything else anchored to an
  edge; a badge a CRT crops is a badge nobody sees.

  Drawn after the scrim so it stays legible while a panel is open, which matches how
  the legend and the menu bar behave. It is only an indicator: the way to act on a
  disc is the Disc entry in the menu bar, which exists exactly as long as this badge
  does.
*/
static void draw_disc_badge(const chome_profile *p)
{
	if (disc_state() == DISC_ABSENT) return;

	int r = disc_radius(p);
	int cx = p->safe_x + p->inset + r;
	int cy = p->safe_y + p->inset + r;

	/*
	  The disc and nothing else. No plate behind it and no name beside it, at any
	  profile, focused or not.

	  It turns, which is already enough to notice, and a disc is self-explanatory in a
	  way a label is not - so the label was only ever repeating what the picture said,
	  while costing the room it needed and, at 240p, running across the shelf title.
	  What is *on* the disc belongs in the prompt, where there is room to say it
	  properly.

	  Focus is a ring one cell outside the disc, not a plate behind it, and it is the
	  same COL_BLUE this front-end uses for a selected row everywhere else - a white ring
	  merged with the disc's own white rim into one thick band that read as decoration.
	  Growing the radius instead would have shown nothing at all: the cell size is r/16
	  as an integer, so anything short of doubling renders identically.
	*/
	int focused = (screen == SCR_DISCBAR);

	gfx_disc(cx, cy, r, disc_step(),
		disc_bands, DISC_BANDS_N, COL_WHITE, COL_PANELHI, COL_BGDARK,
		focused ? COL_BLUE : 0);

	/*
	  No word beside it, focused or not.

	  An "Open" label was tried and dropped: at 240p it runs into the shelf title, and the
	  only way to fit it was to suppress that title - which is a side effect on the shelf
	  to buy a word the outline and the faster spin already convey. The legend along the
	  bottom names the action anyway.
	*/
	(void)focused;
}

static void draw_power(const chome_profile *p)
{
	/*
	  Sized for its two rows and the line underneath, rather than taking the default panel
	  and leaving two thirds of it empty grey.
	*/
	int ps = p->ts_ui;
	int pw = p->w - 2 * p->inset;
	if (pw > 34 * 8 * ps) pw = 34 * 8 * ps;
	int ph = (10 * ps + 6) + PWR_ROWS * 14 * ps + 30 * ps;

	panel_box b = draw_panel_ex(p, pw, ph, "Power");
	int s = b.s, rowh = 14 * s, y = b.y + 6 * s;

	static const char *rows[PWR_ROWS] = { "Restart", "Shut Down" };
	int armed = (pwr_arm >= 0 && !CheckTimer(pwr_until));

	for (int i = 0; i < PWR_ROWS; i++)
	{
		int on = (i == pwr_row);
		int ry = y + i * rowh;

		if (on) gfx_fill(b.x + 4 * s, ry - 3 * s, b.w - 8 * s, rowh - 2 * s,
			(armed && pwr_arm == i) ? COL_RED : COL_BLUE);

		gfx_text(rows[i], b.x + 10 * s, ry, s, on ? COL_WHITE : COL_INK, 0);
	}

	int ny = y + PWR_ROWS * rowh + 6 * s;

	if (armed)
	{
		btn_hint_c(b.x + b.w / 2, ny, s, COL_RED, "Press", LBL_A,
			(pwr_arm == 0) ? "again to restart" : "again to shut down");
	}
	else
	{
		char lines[4][64];
		int nl = wrap_text("Always shut down here rather than pulling the plug.",
			(b.w - 16 * s) / (8 * s), lines, 2);
		for (int i = 0; i < nl; i++)
			gfx_text_c(lines[i], b.x + b.w / 2, ny + i * 10 * s, s, COL_PANELHI, 0);
	}
}

/*
  Best Settings.

  Two levels on purpose. The left column is an outcome a player can judge - "hide the
  resolution pop-up over a game" - and the right column is the line that will really be
  written to their file. Nobody has to read the right column, and nobody who wants to
  know what is being done to their configuration should have to guess.

  It is a screen rather than an Options row that just acts, because this rewrites a file
  the player may well have edited themselves: they see the whole list first, and it still
  takes the two presses that everything unrecoverable takes here.
*/
#define INI_NOTE "A copy of the old file is kept, so this can be undone."

static void draw_ini(const chome_profile *p)
{
	int s = p->ts_ui;
	int rowh = 11 * s;
	int done = (ini_wrote != -1);

	int w = p->w - 2 * p->inset;
	if (w > 46 * 8 * s) w = 46 * 8 * s;
	int avail = w - 12 * s;

	/*
	  ts_tiny is ts_ui on every profile, so the two columns are told apart by colour
	  rather than by size - and at 240p they do not both fit on a line at all. Measured
	  rather than assumed: when the widest pair overflows, each setting takes two lines
	  with its key indented under it, which is the one arrangement that keeps the
	  written line visible on the canvas Derek's CRT actually gets.
	*/
	int stacked = 0;
	for (int i = 0; i < ini_n; i++)
	{
		char kv[48];
		snprintf(kv, sizeof(kv), "%s=%s", ini_list[i].want->key, ini_list[i].want->value);
		if (gfx_text_w(ini_list[i].want->outcome, s) + gfx_text_w(kv, s) + 8 * s > avail) stacked = 1;
	}

	// Sized for its content, like Power, rather than taking the default panel.
	int lines = done ? 3 : (ini_n ? ini_n * (stacked ? 2 : 1) : 1);
	char note[4][64];
	int nnote = done ? 0 : (ini_n ? wrap_text(INI_NOTE, avail / (8 * s), note, 3) : 0);

	int h = (10 * s + 6) + 5 * s + lines * rowh + 6 * s + nnote * 9 * s + 6 * s + 12 * s;
	if (h > p->h - 2 * p->safe_y) h = p->h - 2 * p->safe_y;

	panel_box b = draw_panel_ex(p, w, h, "Best Settings");
	int y = b.y + 5 * s;

	if (done)
	{
		int failed = (ini_wrote < 0);
		const char *bak = strrchr(ini_backup_path(), '/');
		char l1[96], l2[96];

		if (failed) snprintf(l1, sizeof(l1), "%s", ini_last_error());
		else snprintf(l1, sizeof(l1), "%d setting%s changed", ini_wrote, ini_wrote == 1 ? "" : "s");
		snprintf(l2, sizeof(l2), "Old file kept as %s", bak ? bak + 1 : ini_backup_path());

		gfx_text(gfx_clip(l1, s, avail), b.x + 6 * s, y, s, failed ? COL_RED : COL_INK, 0);

		if (!failed)
		{
			gfx_text(gfx_clip(l2, s, avail), b.x + 6 * s, y + rowh, s, COL_PANELLO, 0);

			/*
			  And the restart line, which has to be the truth rather than a
			  reassurance. Every setting shipped today has a cfg field ini_apply()
			  pokes as well as writing, so this session is already living under the
			  new values; one without a field makes this say the opposite.
			*/
			gfx_text(gfx_clip(ini_needs_restart ? "A restart is needed for these"
			                                    : "In effect now - no restart needed", s, avail),
				b.x + 6 * s, y + 2 * rowh, s, COL_PANELLO, 0);
		}

		btn_hint_c(b.x + b.w / 2, b.y + b.h - 11 * s, s, COL_INK, "Press", LBL_B, "to close");
		return;
	}

	if (!ini_n)
	{
		gfx_text_c(gfx_clip("Everything this menu wants is already set.", s, avail),
			b.x + b.w / 2, y, s, COL_INK, 0);
		btn_hint_c(b.x + b.w / 2, b.y + b.h - 11 * s, s, COL_INK, "Press", LBL_B, "to close");
		return;
	}

	for (int i = 0; i < ini_n; i++)
	{
		const ini_want *wt = ini_list[i].want;

		char kv[48];
		snprintf(kv, sizeof(kv), "%s=%s", wt->key, wt->value);

		if (stacked)
		{
			gfx_text(gfx_clip(wt->outcome, s, avail), b.x + 6 * s, y + 2 * i * rowh, s, COL_INK, 0);
			gfx_text(gfx_clip(kv, s, avail - 8 * s), b.x + 14 * s, y + (2 * i + 1) * rowh, s, COL_PANELLO, 0);
		}
		else
		{
			int kvw = gfx_text_w(kv, s);
			gfx_text(gfx_clip(wt->outcome, s, avail - kvw - 4 * s), b.x + 6 * s, y + i * rowh, s, COL_INK, 0);
			gfx_text(kv, b.x + b.w - 6 * s - kvw, y + i * rowh, s, COL_PANELLO, 0);
		}
	}

	int ny = y + lines * rowh + 6 * s;
	for (int i = 0; i < nnote; i++)
		gfx_text_c(note[i], b.x + b.w / 2, ny + i * 9 * s, s, COL_PANELHI, 0);

	// The note stays put while arming so the panel does not resize under the player;
	// only the line they are about to act on changes.
	int armed = (ini_armed && !CheckTimer(ini_until));
	if (armed) btn_hint_c(b.x + b.w / 2, b.y + b.h - 11 * s, s, COL_RED, "Press", LBL_A, "again to write");
	else btn_hint_c(b.x + b.w / 2, b.y + b.h - 11 * s, s, COL_INK, "Press", LBL_A, "to change them");
}

/* ------------------------------------------------------- more settings ---- */

#define SET_SAVE_NOTE "Writes the changes into MiSTer.ini. A copy is kept."

/*
  Read the card and count what is not at its recommended value. Rebuilt on the way in to
  Options and to the screen itself, never per frame: which options apply depends on the
  video path, which cannot change while a panel is up over it, and the values only move
  when we move them.
*/
static void set_summary()
{
	set_nview = opt_view(set_view, OPT_MAX, video_scaler_is_visible());
	opt_load(ini_path());

	set_odd = 0;
	for (int i = 0; i < set_nview; i++) if (!opt_is_rec(set_view[i])) set_odd++;
}

static void set_refresh()
{
	set_summary();

	set_row = 0;
	set_top = 0;
	set_arm = 0;
	set_quit_arm = 0;
	set_wrote = -1;
	set_failed = 0;
}

// A value moved. The last write's result stops being the news, and an armed save is
// no longer a save of what the player armed it for.
static void set_edited()
{
	set_wrote = -1;
	set_failed = 0;
	set_arm = 0;
	mark_dirty();
}

/* ------------------------------------------------------- core options ----- */

static int co_rows()
{
	int n = core_opts_tier_count(co_tier);
	// The last row switches page, so there is always one more than there are options.
	return n + 1;
}

static const char *co_tier_name(int t)
{
	if (t == CO_TIER_PICTURE) return "Picture";
	if (t == CO_TIER_SYSTEM) return "System & Sound";
	return "Risky";
}

/*
  The running core's own options.

  Flat within a tier, the same choice draw_settings() made and for the same reason: one
  list to scroll beats picking a category first. The tiers themselves are a page switch on
  the last row rather than a separate screen, so the risky set takes a deliberate step to
  reach without being hidden away.

  Values come from the core every time this draws. They have to: some of these options
  change what others mean - the N64's whole VI group applies only while Video Out is
  Original(VI) - and the core recomputes that mask itself. Caching would show the player a
  stale screen with no way to know.
*/
static void draw_core_opts(const chome_profile *p)
{
	int s = p->ts_ui;
	int n = core_opts_tier_count(co_tier);

	int pw = p->w - p->inset * 2;
	if (pw > 46 * 8 * s) pw = 46 * 8 * s;
	int ph = (10 * s + 6) + (n + 1) * 12 * s + 22 * s;
	if (ph > p->h - 2 * p->safe_y) ph = p->h - 2 * p->safe_y;

	char title[48];
	const char *sysn = core_short_name(ig_have_item ? ig_item.sysidx : -1);
	snprintf(title, sizeof(title), "%s - %s", (sysn && *sysn) ? sysn : "Core",
		co_tier_name(co_tier));

	panel_box b = draw_panel_ex(p, pw, ph, title);

	static const char *rows[CO_MAX + 1];
	static const char *vals[CO_MAX + 1];
	static uint32_t vcol[CO_MAX + 1];
	static char vbuf[CO_MAX + 1][CO_VAL_LEN + 8];

	int i = 0;
	int mine = 0;                     // rows on this page this game keeps its own value for
	for (; i < n && i < CO_MAX; i++)
	{
		const core_opt *o = core_opt_tier_at(co_tier, i);
		if (!o) break;

		/*
		  A star on a value this game keeps to itself, explained by the footer below.
		  Without a mark the screen would show a per-game setting and a shared one
		  identically, and the player would have no way to know which of their games
		  they had just changed.
		*/
		int own = core_opt_per_game(o);
		if (own) mine++;

		rows[i] = o->name;
		snprintf(vbuf[i], sizeof(vbuf[i]), "%s%s", o->vals[core_opt_value(o)], own ? " *" : "");
		vals[i] = vbuf[i];

		/*
		  Amber for a value the core marked (U), because the core is telling the player it
		  can crash - the same signal the risky page is built from. Dim for one the core
		  says does not apply right now, which is how the VI group reads under Clean HDMI:
		  still there, visibly not in effect. Both outrank the per-game green: a warning
		  from the core matters more than whose setting it is.
		*/
		if (o->disabled) vcol[i] = COL_DIM;
		else if (strstr(vbuf[i], "(U)")) vcol[i] = COL_YELLOW;
		else if (own) vcol[i] = COL_GREEN;
		else vcol[i] = COL_PANELLO;
	}

	// The page switch, always last.
	int nt = (co_tier == CO_TIER_PICTURE) ? CO_TIER_SYSTEM
		: (co_tier == CO_TIER_SYSTEM) ? CO_TIER_RISKY : CO_TIER_PICTURE;
	rows[i] = "More";
	snprintf(vbuf[i], sizeof(vbuf[i]), "%s >", co_tier_name(nt));
	vals[i] = vbuf[i];
	vcol[i] = COL_PANELHI;
	i++;

	draw_rows_c(&b, rows, vals, vcol, i, co_row);

	int fy = b.y + b.h - 12 * s;
	/*
	  Two wordings again: at 240p the panel is not wide enough for the long one, and a
	  footer that loses its end is worse than a short one that does not.
	*/
	int room = (b.w - 12 * s) / (8 * p->ts_tiny);
	const char *foot = (room >= 36) ? "Applied at once, kept with the core" : "Applied at once";

	/*
	  Three different truths, and the screen has to tell the player which one applies -
	  a change here goes to the core's own config, or to this game alone, and nothing
	  else on screen says which.

	  The star is explained where the player is already looking rather than in a legend:
	  it appears as soon as one value on the page carries it.
	*/
	if (core_opts_bound_game())
	{
		if (mine) foot = (room >= 36) ? "* kept for this game, not for the core" : "* this game only";
		else foot = (room >= 36) ? "Changes are kept for this game only" : "Kept for this game";
	}

	/*
	  The core's own warning wins on the risky page - it is the reason that page is a page -
	  but the star still has to mean something there, so where the line fits it says both.
	*/
	if (co_tier == CO_TIER_RISKY)
	{
		if (mine && room >= 36) foot = "* this game only - (U) can crash";
		else foot = (room >= 34) ? "(U) marked by the core: can crash" : "(U): can crash";
	}
	else if (!n) foot = "Nothing here on this core";
	gfx_text(gfx_clip(foot, p->ts_tiny, b.w - 12 * s), b.x + 6 * s, fy, p->ts_tiny,
		(co_tier == CO_TIER_RISKY) ? COL_YELLOW : COL_PANELLO, 0);
}

static void draw_settings(const chome_profile *p)
{
	int s = p->ts_ui;
	int rowh = 12 * s;

	// The group of the row under the cursor, in the header. The list is flat - a novice
	// scrolling one list beats a novice picking a category first - so this is how the
	// grouping shows at all, and it costs no rows.
	const opt_def *od = (set_row < set_nview) ? opt_at(set_view[set_row]) : 0;
	char title[64];
	if (od) snprintf(title, sizeof(title), "Settings - %s", opt_group_name(od->group));
	else snprintf(title, sizeof(title), "Settings");

	panel_box b = draw_panel(p, title);

	// Three lines are reserved at the bottom: two for the selected row's sentence, one
	// for whatever the screen has to say about it.
	int foot = 3 * 10 * s + 4 * s;
	int fit = (b.h - 5 * s - foot) / rowh;
	if (fit < 1) fit = 1;

	int nrows = set_nview + 1;                   // the options, then Save Changes
	if (fit > nrows) fit = nrows;

	if (set_row < set_top) set_top = set_row;
	if (set_row >= set_top + fit) set_top = set_row - fit + 1;
	if (set_top > nrows - fit) set_top = nrows - fit;
	if (set_top < 0) set_top = 0;

	const char *labels[OPT_MAX + 1];
	const char *vals[OPT_MAX + 1];
	uint32_t vcol[OPT_MAX + 1];
	static char vbuf[OPT_MAX + 1][24];

	int dirty = opt_dirty();
	int n = 0;

	for (int r = set_top; r < set_top + fit && n <= OPT_MAX; r++)
	{
		if (r < set_nview)
		{
			int i = set_view[r];
			labels[n] = opt_at(i)->label;
			vals[n] = opt_value_text(i, vbuf[n], sizeof(vbuf[n]));

			// The whole colour code, in one line: amber is "not the value this menu
			// recommends", which for most options is simply the machine's default.
			vcol[n] = opt_is_rec(i) ? 0 : COL_YELLOW;
		}
		else
		{
			labels[n] = "Save Changes";
			if (dirty) snprintf(vbuf[n], sizeof(vbuf[n]), "%d To Save", dirty);
			else if (set_wrote > 0) snprintf(vbuf[n], sizeof(vbuf[n]), "Saved");
			else snprintf(vbuf[n], sizeof(vbuf[n]), "Nothing To Save");
			vals[n] = vbuf[n];
			vcol[n] = (!dirty && set_wrote > 0) ? COL_GREEN : 0;
		}
		n++;
	}

	draw_rows_c(&b, labels, vals, vcol, n, set_row - set_top);

	// Where the cursor is in a list that does not fit. Inside the value column's right
	// margin, so it cannot land on a value.
	if (nrows > fit)
	{
		int tx = b.x + b.w - 3 * s, ty = b.y + 3 * s, th = fit * rowh;
		gfx_fill(tx, ty, 2 * s, th, COL_PANELLO);

		int hh = th * fit / nrows;
		if (hh < 4 * s) hh = 4 * s;
		gfx_fill(tx, ty + (th - hh) * set_top / (nrows - fit), 2 * s, hh, COL_INK);
	}

	int fy = b.y + b.h - foot + 2 * s;
	int cols = (b.w - 12 * s) / (8 * s);

	char wrapped[4][64];
	int nl = wrap_text(set_failed ? opt_error() : od ? od->help : SET_SAVE_NOTE, cols, wrapped, 2);
	for (int i = 0; i < nl; i++)
		gfx_text(wrapped[i], b.x + 6 * s, fy + i * 10 * s, s, set_failed ? COL_RED : COL_PANELLO, 0);

	/*
	  And the one line that changes. Order is by urgency: an armed action first, because
	  the player is one press from it; then the result of the last write; then what the
	  amber on the selected row means, which is the only place the colour is explained.
	*/
	int y3 = fy + 2 * 10 * s;

	if (set_arm && !CheckTimer(set_arm_until))
	{
		btn_hint_c(b.x + b.w / 2, y3, s, COL_RED, "Press", LBL_A, "again to save");
		return;
	}
	if (set_quit_arm && !CheckTimer(set_quit_until))
	{
		btn_hint_c(b.x + b.w / 2, y3, s, COL_RED, "Press", LBL_B, "again to lose the changes");
		return;
	}
	if (set_row == set_nview && dirty)
	{
		btn_hint_c(b.x + b.w / 2, y3, s, COL_INK, "Press", LBL_A, "to save");
		return;
	}
	if (!dirty && set_wrote > 0)
	{
		gfx_text_c(gfx_clip(opt_wrote_live() ? "SAVED - IN EFFECT NOW"
		                                     : "SAVED - FROM THE NEXT GAME ON", s, b.w - 12 * s),
			b.x + b.w / 2, y3, s, COL_GREEN, 0);
		return;
	}
	if (od && !opt_is_rec(set_view[set_row]))
	{
		char u[64], rv[24];
		snprintf(u, sizeof(u), "Usually %s", opt_rec_text(set_view[set_row], rv, sizeof(rv)));
		for (char *q = u; *q; q++) *q = (char)toupper((unsigned char)*q);
		gfx_text_c(gfx_clip(u, s, b.w - 12 * s), b.x + b.w / 2, y3, s, COL_YELLOW, 0);
	}
}

static void draw_about_panel(const chome_profile *p)
{
	panel_box b = draw_panel(p, "About");
	int s = b.s, rowh = 11 * s, y = b.y + 5 * s;

	char l[10][80];
	int n = 0;
	snprintf(l[n++], 80, "Classic Home for MiSTer");
	snprintf(l[n++], 80, "Profile %s  %dx%d", theme_get()->name, p->w, p->h);
	snprintf(l[n++], 80, "Systems %d   Games %d", lib_sys_count(), lib_item_count());
	snprintf(l[n++], 80, "Art cached %d  %d KB", art_cache_count(), art_cache_bytes() / 1024);
	snprintf(l[n++], 80, "Art fetch %s", cfg.classicui_artfetch ? "on" : "off");
	snprintf(l[n++], 80, "Font: MiSTer OSD 8x8 ROM");
	snprintf(l[n++], 80, "GPL v3.");
	/*
	  The icons are somebody else's work under CC BY 4.0, which asks to be credited
	  where a person can see it. Both sets name an About screen as the right place,
	  so this is the attribution, not a nicety - see ICONS.md. If the icon set
	  changes, these two lines change with it.
	*/
	snprintf(l[n++], 80, "Icons: RetroArch monochrome,");
	snprintf(l[n++], 80, "Twemoji. CC BY 4.0.");

	for (int i = 0; i < n; i++)
	{
		char up[80];
		snprintf(up, sizeof(up), "%s", l[i]);
		for (char *q = up; *q; q++) *q = (char)toupper((unsigned char)*q);
		gfx_text(gfx_clip(up, s, b.w - 12 * s), b.x + 6 * s, y + i * rowh, s, COL_INK, 0);
	}
}

static void draw_sort_panel(const chome_profile *p)
{
	panel_box b = draw_panel(p, "Sort by");
	const char *rows[SORT_COUNT];
	for (int i = 0; i < SORT_COUNT; i++) rows[i] = lib_sort_name(i);
	draw_rows(&b, rows, 0, SORT_COUNT, sort_idx);
}

/* ------------------------------------------------------------- browser ---- */

static int browse_cmp(const void *a, const void *b)
{
	const browse_ent *ea = (const browse_ent*)a;
	const browse_ent *eb = (const browse_ent*)b;
	if (ea->isdir != eb->isdir) return eb->isdir - ea->isdir;
	return strcasecmp(ea->name, eb->name);
}

static void browse_load(int sysidx, const char *rel)
{
	nbent = 0;
	browse_sel = 0;
	browse_top = 0;
	browse_sys = sysidx;
	snprintf(browse_rel, sizeof(browse_rel), "%s", rel ? rel : "");

	const chome_sys *s = lib_sys(sysidx);
	if (!s) return;

	char root[1024];
	if (!lib_sys_games_dir(sysidx, root, sizeof(root))) return;

	char full[1200];
	if (browse_rel[0]) snprintf(full, sizeof(full), "%s/%s", root, browse_rel);
	else snprintf(full, sizeof(full), "%s", root);

	DIR *d = opendir(full);
	if (!d) return;

	struct dirent *de;
	while ((de = readdir(d)) && nbent < BROWSE_MAX)
	{
		if (de->d_name[0] == '.') continue;

		int isdir;
		if (de->d_type == DT_DIR) isdir = 1;
		else if (de->d_type == DT_REG) isdir = 0;
		else
		{
			char cf[1400];
			snprintf(cf, sizeof(cf), "%s/%s", full, de->d_name);
			struct stat st;
			if (stat(cf, &st)) continue;
			isdir = S_ISDIR(st.st_mode) ? 1 : 0;
		}

		if (!isdir)
		{
			const char *dot = strrchr(de->d_name, '.');
			if (!dot) continue;
			char ext[16];
			snprintf(ext, sizeof(ext), "%s", dot + 1);
			int ok = 0;
			const char *p = s->ext;
			while (*p)
			{
				const char *c = strchr(p, ',');
				int len = c ? (int)(c - p) : (int)strlen(p);
				if (len == (int)strlen(ext) && !strncasecmp(p, ext, len)) { ok = 1; break; }
				if (!c) break;
				p = c + 1;
			}
			if (!ok) continue;
		}

		snprintf(bent[nbent].name, sizeof(bent[nbent].name), "%s", de->d_name);
		bent[nbent].isdir = isdir;
		nbent++;
	}
	if (nbent >= BROWSE_MAX) printf("ClassicUI: browser listing capped at %d entries\n", BROWSE_MAX);
	closedir(d);

	qsort(bent, nbent, sizeof(browse_ent), browse_cmp);
}

static void draw_browse(const chome_profile *p)
{
	const chome_sys *s = lib_sys(browse_sys);

	gfx_fill(0, 0, p->w, p->h, COL_BG);

	// Header sits below the overscan margin; the panel behind it still runs to the
	// edge, so the margin reads as part of the header rather than as a gap.
	int hy = p->safe_y;
	gfx_fill(0, 0, p->w, hy + p->bar_h, COL_PANEL);
	gfx_fill(0, hy + p->bar_h - 2, p->w, 2, COL_PANELLO);

	int s2 = p->ts_ui;
	char hdr[160];
	snprintf(hdr, sizeof(hdr), "%s  %s", s ? s->name : "", browse_rel);
	for (char *q = hdr; *q; q++) *q = (char)toupper((unsigned char)*q);
	gfx_text(gfx_clip(hdr, s2, p->w - p->inset * 2), p->inset, hy + (p->bar_h - 8 * s2) / 2, s2, COL_INK, 0);

	int rowh = 12 * s2;
	int top = hy + p->bar_h + 8;
	int avail = p->y_legend - 8 * s2 - top;
	int rows = avail / rowh;
	if (rows < 1) rows = 1;

	if (browse_sel < browse_top) browse_top = browse_sel;
	if (browse_sel >= browse_top + rows) browse_top = browse_sel - rows + 1;

	for (int i = 0; i < rows; i++)
	{
		int idx = browse_top + i;
		if (idx >= nbent) break;

		int y = top + i * rowh;
		int on = (idx == browse_sel);
		if (on) gfx_fill(p->inset - 4, y - 2 * s2, p->w - p->inset * 2 + 8, rowh - 2 * s2, COL_BLUE);

		char nm[160];
		snprintf(nm, sizeof(nm), "%s%s", bent[idx].isdir ? "[ " : "  ", bent[idx].name);
		if (bent[idx].isdir) strncat(nm, " ]", sizeof(nm) - strlen(nm) - 1);
		for (char *q = nm; *q; q++) *q = (char)toupper((unsigned char)*q);

		gfx_text(gfx_clip(nm, s2, p->w - p->inset * 2), p->inset, y, s2,
			on ? COL_WHITE : (bent[idx].isdir ? COL_PANELHI : COL_DIM), 0);
	}

	if (!nbent)
	{
		gfx_text_c("NOTHING HERE", p->w / 2, p->h / 2, p->ts_ui, COL_DIM, 0);
	}
}

/* -------------------------------------------------------------- launch ---- */

/*
  A slot other than the system's usual one.

  A shelf entry knows one slot per system, because a game is one file: TurboGrafx-16
  is 'f'/0, which is its "FS0,PCEBIN,Load TurboGrafx". A disc goes into a different
  slot of the same core - "S0,CUECHD,Insert CD" - so a disc launch is the system's
  rbf with the slot swapped, and nothing else about launching changes. Passed rather
  than stored in the systems table so there is still exactly one launch path.
*/
struct chome_slot
{
	char type;
	int  index;
};

static void launch_write_mgl(const chome_sys *s, const char *relpath, const chome_slot *slot)
{
	FILE *f = fopen("/tmp/classicui_launch.mgl", "wt");
	if (!f) return;

	char type = slot ? slot->type : s->type;
	int index = slot ? slot->index : s->index;

	fprintf(f, "<mistergamedescription>\n");
	fprintf(f, "\t<rbf>%s</rbf>\n", s->rbf);
	fprintf(f, "\t<file delay=\"%d\" type=\"%c\" index=\"%d\" path=\"%s\"/>\n",
		s->delay ? s->delay : 2, type == 's' ? 's' : 'f', index, relpath);
	fprintf(f, "</mistergamedescription>\n");
	fclose(f);
}

static void do_launch(int sysidx, const char *relpath, chome_item *it, const chome_slot *slot = 0)
{
	const chome_sys *s = lib_sys(sysidx);
	if (!s) return;

	if (it) lib_note_play(it);
	lib_state_save();
	session_save();                   // so quitting the game comes back to this shelf

	vp_arm_for_launch(sysidx, class_of(sysidx, relpath));

	// Tell the next core what it is running, so it can grab a reference frame.
	{
		FILE *f = fopen(CURRENT_FILE, "wt");
		if (f)
		{
			/*
			  Third line: the core this launch expects. Anything can load a core behind
			  our back - the classic menu, a script, /dev/MiSTer_cmd, a bootcore - and
			  then this file describes a game that is not running, which is how the
			  in-game menu ended up captioned with a game from a previous session.
			  Arcade writes "*" because an .mra picks its own core name.
			*/
			const char *core = "*";
			if (!s->mra && s->rbf[0])
			{
				const char *slash = strrchr(s->rbf, '/');
				core = slash ? slash + 1 : s->rbf;
			}
			fprintf(f, "%s\n%s\n%s\n", s->id, relpath, core);
			fclose(f);
		}
	}

	if (!s->mra && !s->rbf[0]) { nudge(); return; }

	// The game is about to own the screen, so hand the analog output back before
	// loading: the core brings its own video mode and must not inherit ours.
	video_menu_fb_analog(0);

	if (s->mra)
	{
		// Arcade: the .mra is the descriptor, hand it straight over.
		char abs[1200];
		snprintf(abs, sizeof(abs), "%s/%s/%s", getRootDir(), s->dir, relpath);
		printf("ClassicUI: launching arcade %s\n", abs);
		active = 0;
		xml_load(abs);
		return;
	}

	launch_write_mgl(s, relpath, slot);
	printf("ClassicUI: launching %s via %s\n", relpath, s->rbf);
	active = 0;
	xml_load("/tmp/classicui_launch.mgl");
}

static void launch_selected()
{
	const chome_entry *e = cur_entry();
	if (!e || e->kind != ENT_GAME) { nudge(); return; }

	chome_item *it = lib_item(e->game);
	if (!it) { nudge(); return; }

	do_launch(it->sysidx, it->path, it);
}

/* ------------------------------------------------------- launch a disc ---- */

/*
  Which core slot a physical disc goes into, by shelf system id, and only for the
  systems whose firmware-side daemon can read from the drive.

  PC Engine CD is first and for now the only one: the TurboGrafx16 core reads a real
  disc at full speed, and its daemon is the one that has the branches for it. The
  other CD daemons each need the same work done to them separately, so a system that
  is not in this table is still identified and still offered - it just says it cannot
  play the disc yet instead of loading a core that would find nothing in the slot.
*/
struct disc_playable
{
	const char *sysid;
	chome_slot slot;
};

static const disc_playable disc_playables[] =
{
	{ "tg16", { 's', 0 } },      // "S0,CUECHD,Insert CD" in TurboGrafx16.sv
};

static const chome_slot *disc_slot_for(int sysidx)
{
	const chome_sys *s = (sysidx >= 0) ? lib_sys(sysidx) : 0;
	if (!s) return 0;

	for (unsigned i = 0; i < sizeof(disc_playables) / sizeof(disc_playables[0]); i++)
	{
		if (!strcasecmp(disc_playables[i].sysid, s->id)) return &disc_playables[i].slot;
	}
	return 0;
}

/*
  Set once the drive has been handed to a core, and cleared only when the menu core is
  running again.

  The drive can have exactly one owner. Detection is a helper process that holds
  /dev/sr0 open and polls its status; the core's reader opens the same device in *this*
  process and streams sectors from it on the thread that also draws. Every ioctl on
  that device serialises behind whatever the drive is doing, so a status poll from the
  helper would put itself in front of a sector the core needs now - see chome_disc.h
  for the two freezes that measured this.

  So the helper is stopped before the launch and must not come back, and disc_poll()
  restarts it whenever it finds it stopped. Hence a latch rather than just stopping it:
  the in-game menu runs this same loop over the top of the running core, and that is
  where the helper would otherwise be resurrected mid-game.

  Cleared on is_menu() because the firmware process survives a core change: quitting
  the game loads the menu core, and at that point nothing can be holding a disc.
*/
static int disc_handed_to_core = 0;

static void disc_launch(int sysidx)
{
	const chome_slot *slot = disc_slot_for(sysidx);
	const chome_sys *s = (sysidx >= 0) ? lib_sys(sysidx) : 0;

	// The whole feature is off by default, and the only way here is through a screen
	// that only exists when it is on - but this is the point where the drive gets used
	// in earnest, so it does not rely on that.
	if (!cfg.classicui_disc || !slot || !s) { nudge(); return; }

	printf("ClassicUI: handing the disc to %s (%s)\n", s->name, disc_display_name());

	disc_watch_stop();
	disc_handed_to_core = 1;

	/*
	  The sentinel goes in as the file. It is not a path: menu.cpp keeps it out of the
	  games-folder resolution and pcecdd's Load() recognises it and reads the table of
	  contents off the disc instead of parsing a cue sheet.
	*/
	do_launch(sysidx, PHYSICAL_DISC_SENTINEL, 0, slot);
}

/* -------------------------------------------------------------- compose --- */

static void draw_launch(const chome_profile *p)
{
	int half = (int)(p->w / 2 * (curtain * 1.5 > 1 ? 1 : curtain * 1.5));
	gfx_fill(0, 0, half, p->h, COL_BLACK);
	gfx_fill(p->w - half, 0, half, p->h, COL_BLACK);

	if (curtain > 0.6)
	{
		const chome_entry *e = cur_entry();
		chome_item *it = (e && e->kind == ENT_GAME) ? lib_item(e->game) : 0;
		const chome_sys *s = it ? lib_sys(it->sysidx) : 0;
		int s2 = p->ts_ui;

		if (it)
		{
			char up[CH_TITLE_LEN];
			snprintf(up, sizeof(up), "%s", it->title);
			for (char *q = up; *q; q++) *q = (char)toupper((unsigned char)*q);
			gfx_text_c(gfx_clip(up, p->ts_title, p->w - p->inset * 2), p->w / 2, p->h / 2 - 14 * s2, p->ts_title, COL_WHITE, COL_SHADOW);
		}
		if (s)
		{
			char up[64];
			snprintf(up, sizeof(up), "%s", s->name);
			for (char *q = up; *q; q++) *q = (char)toupper((unsigned char)*q);
			gfx_text_c(up, p->w / 2, p->h / 2 + 4 * s2, s2, COL_DIM, 0);
		}
		gfx_text_c("LOADING", p->w / 2, p->h / 2 + 20 * s2, s2, COL_PANELLO, 0);
	}
}

static void render()
{
	gfx_stat_compose_begin();

	const chome_profile *p = theme_get();

	if (screen == SCR_BROWSE)
	{
		draw_browse(p);
		draw_legend(p);
		gfx_end();
		return;
	}

	draw_background(p);
	draw_title_block(p);
	draw_shelf(p);
	draw_pips(p);
	draw_position(p);

	int overlay = (screen == SCR_SORT || screen == SCR_DISPLAY || screen == SCR_OPTIONS ||
		screen == SCR_ABOUT || screen == SCR_WIFI || screen == SCR_PADS ||
		screen == SCR_POWER || screen == SCR_INI || screen == SCR_PADTEST ||
		screen == SCR_SET || screen == SCR_CORE || screen == SCR_DISC);
	if (overlay) gfx_scrim(0, 0, p->w, p->h, COL_BGDARK, 2);

	draw_suspend(p);
	draw_legend(p);
	draw_disc_badge(p);
	draw_menubar(p, screen == SCR_MENUBAR || overlay);

	switch (screen)
	{
	case SCR_SORT:    draw_sort_panel(p); break;
	case SCR_DISPLAY: draw_display_screen(p); break;
	case SCR_OPTIONS: draw_options_panel(p); break;
	case SCR_ABOUT:   draw_about_panel(p); break;
	case SCR_WIFI:    draw_wifi(p); break;
	case SCR_POWER:   draw_power(p); break;
	case SCR_DISC:    draw_disc(p); break;
	case SCR_INI:     draw_ini(p); break;
	case SCR_SET:     draw_settings(p); break;
	case SCR_CORE:    draw_core_opts(p); break;
	case SCR_PADS:    draw_pads(p); break;
	case SCR_PADTEST: draw_padtest(p); break;
	case SCR_LAUNCH:  draw_launch(p); break;
	default: break;
	}

	// Over the panels too: a game that is still playing is true whatever is on top of it.
	draw_running_warning(p);

	// Last, and over everything: while the keyboard is up it is the only thing the
	// player can act on.
	if (osk_active()) osk_draw(p, using_pad);

	gfx_end();
}

/* ---------------------------------------------------------------- input --- */

/*
  Hands a finished entry back to whoever opened the keyboard. Nothing opens it yet -
  the Wi-Fi screen is the reason it exists - so for now this only clears the result
  so a cancelled entry is not seen twice.
*/
static void osk_settle()
{
	int r = osk_result();
	if (!r) return;

	int dest = osk_dest;
	osk_dest = OSKD_NONE;
	osk_clear_result();
	mark_dirty();

	if (r < 0) return;                    // cancelled: the text is thrown away

	if (dest == OSKD_WIFI) net_join(wifi_pick, osk_text(), wifi_pick_secure);
}

static void go_screen(int s)
{
	screen = s;
	// Reading the link costs a process, so only do it while something is showing it.
	net_watch(s == SCR_OPTIONS || s == SCR_WIFI);
	// The tester counts: a wireless pad woken from inside it has to turn up in the list
	// underneath, and bluetoothctl is the only thing that knows it has.
	bt_watch(s == SCR_OPTIONS || s == SCR_PADS || s == SCR_PADTEST);
	// The Options rows say how many settings the ini disagrees with and how many are
	// away from their default, so both have to be read - once, on the way in, and not
	// on every frame the rows are on screen.
	if (s == SCR_OPTIONS) { ini_refresh(); set_summary(); }
	mark_dirty();
}

// Opening the tester on the row under the cursor. Only what names the pad is kept -
// see padtest_find() for why the row itself would go stale.
static void padtest_open(const pad_row *r)
{
	snprintf(padtest_name, sizeof(padtest_name), "%s", r->name);
	snprintf(padtest_mac, sizeof(padtest_mac), "%s", r->mac);
	padtest_kind = r->kind;
	padtest_back_arm = 0;
	padtest_seen = 0;
	go_screen(SCR_PADTEST);
}

/*
  Every key while the tester is up, and none of them does anything - that is the point.
  A screen that asks the player to press every button cannot also give those buttons
  meanings, so the only way out is B, and B is armed rather than immediate so that it too
  can be pressed and seen to light up. Same two-press idiom as forgetting a controller.
*/
static void padtest_key(uint32_t k)
{
	if (k == KEY_ESC || k == KEY_BACK)
	{
		if (padtest_back_arm && !CheckTimer(padtest_back_until))
		{
			padtest_back_arm = 0;
			go_screen(SCR_PADS);
			return;
		}

		padtest_back_arm = 1;
		padtest_back_until = GetTimer(2500);
	}

	// Not even a nudge for the rest: the button lighting up is the feedback, and a panel
	// that shook every time a button was tested would be unusable.
	mark_dirty();
}

static void move_h(int dir)
{
	const chome_profile *p = theme_get();

	switch (screen)
	{
	case SCR_MENUBAR:
	{
		int n = mb_idx + dir;
		if (n < 0 || n >= mb_count_visible()) { nudge(); return; }
		mb_idx = n;
		break;
	}
	case SCR_SUSPEND:
	{
		int n = slot_idx + dir;
		if (n < 0 || n >= user_slots()) { nudge(); return; }
		slot_idx = n;
		break;
	}
	case SCR_DISPLAY:
	{
		int opts[VP_MAX_OPTIONS];
		int nn = vp_options_for(sel_class(), opts);
		int next = look_row + dir;
		if (next < 0 || next >= nn) { nudge(); return; }
		look_row = next;
		break;
	}
	case SCR_OPTIONS:
		if (opt_row == 0) cfg.classicui_artfetch = cfg.classicui_artfetch ? 0 : 1;
		else if (opt_row == 3)
		{
			int v = cfg.classicui_profile + dir;
			if (v < 0) v = 3;
			if (v > 3) v = 0;
			cfg.classicui_profile = (uint8_t)v;
			theme_update(gfx_w(), gfx_h(), cfg.classicui_profile);
			art_init(theme_get()->sel_w, theme_get()->sel_h);
			gfx_damage_all();
		}
		else { nudge(); return; }
		break;
	/*
	  Left and right are how a setting is changed, which is why the value column is on
	  this axis at all. On the Save row there is nothing to move: A is what it is for.
	*/
	/*
	  Left and right change the value, which is the whole interaction on this screen. It
	  writes to the core immediately: there is no Save row, because a picture setting you
	  cannot see take effect is not worth having.

	  Where the change is *kept* depends on whether a game is running. With one, it is
	  remembered against that game; without one, it goes into the core's own config as it
	  always did. See the per-game section of chome_core.h.
	*/
	case SCR_CORE:
	{
		int n = core_opts_tier_count(co_tier);
		if (co_row >= n) { nudge(); return; }

		const core_opt *o = core_opt_tier_at(co_tier, co_row);
		if (!o) { nudge(); return; }

		// Read before the change: the first time an option is overridden this is the
		// value the core's own config holds, and the only chance to record it.
		int shared = core_opt_value(o);
		core_opt_set(o, shared + dir);

		if (core_opts_bound_game()) core_opt_keep_for_game(o, core_opt_value(o), shared);
		else core_opts_save_unpaused();

		// The core recomputes which options apply, so re-read rather than assume.
		core_opts_scan();
		if (co_row >= co_rows()) co_row = co_rows() - 1;
		break;
	}

	case SCR_SET:
		if (set_row >= set_nview) { nudge(); return; }
		if (!opt_step_by(set_view[set_row], dir)) { nudge(); return; }
		set_edited();
		return;

	case SCR_BROWSE:
		nudge();
		return;

	case SCR_HOME:
	{
		int n = lib_view_count();
		int next = sel + dir;
		if (next < 0 || next >= n) { nudge(); return; }

		// Hold to accelerate: after a few repeats, move a screenful.
		if (key_run > 8)
		{
			next = sel + dir * p->visible;
			if (next < 0) next = 0;
			if (next >= n) next = n - 1;
		}
		sel = next;
		slot_idx = 0;
		break;
	}

	/*
	  Anything else on screen is a panel, and a panel is modal: left and right belong to
	  it, not to the shelf behind it. This used to fall through to moving the shelf, so
	  Sort, About, Wi-Fi and Controllers all steered the browser behind them while their
	  own panel sat there - the selection, the title and the pips changing under a dialog
	  that had nothing to do with them. move_v() already ended at a nudge; this is that,
	  for the other axis.
	*/
	default:
		nudge();
		return;
	}
	mark_dirty();
}

static void move_v(int dir)
{
	switch (screen)
	{
	/*
	  Up and down walk the list; the last row is the page switch.

	  mark_dirty() is not optional here, and its absence is a real bug a user found: move_v()
	  has no trailing repaint - every case does its own - so the row moved and nothing was
	  drawn. The cursor then appeared to jump only when left or right changed a value, because
	  move_h() does repaint. "The selected item does not change until you press left or
	  right" was exactly right.
	*/
	case SCR_CORE:
	{
		int n = co_rows();
		int next = co_row + dir;
		if (next < 0 || next >= n) { nudge(); return; }
		co_row = next;
		mark_dirty();
		break;
	}

	case SCR_HOME:
		/*
		  Up goes to the disc before it goes to the menu bar, when there is a disc.

		  Somebody who has just pushed a disc in wants that disc, not Display settings,
		  so it gets the first press and the menu bar gets the second. It costs a press
		  only while a disc is actually in the drive, which is a state the player
		  created deliberately and can end by taking it out.
		*/
		if (dir < 0 && disc_state() != DISC_ABSENT) { go_screen(SCR_DISCBAR); }
		else if (dir < 0) { mb_idx = 0; go_screen(SCR_MENUBAR); }
		else
		{
			chome_item *it = cur_game();
			if (!it) { nudge(); return; }     // folders have no suspend points
			lib_refresh_slots(it);
			slot_idx = 0;
			go_screen(SCR_SUSPEND);
		}
		break;

	case SCR_MENUBAR:
		if (dir > 0) go_screen(disc_state() != DISC_ABSENT ? SCR_DISCBAR : SCR_HOME);
		else nudge();
		break;

	case SCR_DISCBAR:
		// Between the two: up carries on to the menu bar, down returns to the shelf.
		if (dir < 0) { mb_idx = 0; go_screen(SCR_MENUBAR); }
		else go_screen(SCR_HOME);
		break;

	case SCR_SUSPEND:
		if (dir < 0) go_screen(SCR_HOME);
		else
		{
			// Down on a slot locks or unlocks it. Locking is ours to track:
			// there is no lock concept in MiSTer's savestate files.
			chome_item *it = cur_game();
			if (!it) { nudge(); return; }
			int st = slot_state(it, slot_idx);
			if (!st) { nudge(); return; }
			lib_set_lock(it, slot_idx, st == 2 ? 0 : 1);
			del_arm_slot = -1;
			mark_dirty();
		}
		break;

	case SCR_POWER:
		pwr_row = (pwr_row + dir + PWR_ROWS) % PWR_ROWS;
		pwr_arm = -1;                    // moving off disarms, as everywhere else here
		mark_dirty();
		break;

	case SCR_SORT:
		sort_idx = (sort_idx + dir + SORT_COUNT) % SORT_COUNT;
		mark_dirty();
		break;

	case SCR_DISC:
	{
		/*
		  Clamped rather than wrapping, and mark_dirty() at the end: leaving that off
		  is the bug a user reported on the core options screen, where the cursor
		  moved and the screen did not.
		*/
		int n = disc_rows();
		if (n <= 0) { nudge(); break; }

		int next = disc_row + dir;
		if (next < 0 || next >= n) { nudge(); break; }

		disc_row = next;
		mark_dirty();
		break;
	}

	case SCR_DISPLAY:
		nudge();               // one row of tiles: nothing above or below
		break;

	case SCR_OPTIONS:
		{ int n = ig_active ? OPT_ROWS_GAME : OPT_ROWS_MENU; opt_row = (opt_row + dir + n) % n; }
		mark_dirty();
		break;

	case SCR_SET:
	{
		int n = set_nview + 1;
		set_row = (set_row + dir + n) % n;

		// Moving off disarms, as everywhere else here: reaching for another row means
		// the player has stopped meaning to do the thing this one offered.
		set_arm = 0;
		set_quit_arm = 0;
		mark_dirty();
		break;
	}

	case SCR_PADS:
	{
		// Nothing to move through while pairing, or when the list is empty.
		if (bt_pairing() || bt_pair_state() != BTP_IDLE) { nudge(); return; }

		int n = pads_count();
		if (!n) { nudge(); return; }

		/*
		  Disarmed before the bounds check, not after: reaching for another row and
		  finding there isn't one still means the player has stopped meaning to forget
		  this one. Leaving it armed there left a row sitting red and one press from
		  being forgotten.
		*/
		pads_forget_arm = -1;

		int next = pads_row + dir;
		if (next < 0 || next >= n) { nudge(); return; }

		pads_row = next;
		mark_dirty();          // move_v() has no trailing repaint; each case does its own
		break;
	}

	case SCR_WIFI:
	{
		if (net_join_state() != JOIN_IDLE) { nudge(); return; }

		int n = net_count();
		if (!n) { nudge(); return; }

		wifi_row += dir;
		if (wifi_row < 0) wifi_row = 0;
		if (wifi_row >= n) wifi_row = n - 1;
		mark_dirty();
		break;
	}

	case SCR_BROWSE:
		if (!nbent) { nudge(); return; }
		browse_sel += dir;
		if (browse_sel < 0) browse_sel = 0;
		if (browse_sel >= nbent) browse_sel = nbent - 1;
		mark_dirty();
		break;

	default:
		nudge();
		break;
	}
}

static void accept()
{
	switch (screen)
	{
	case SCR_HOME:
	{
		const chome_entry *e = cur_entry();
		if (!e) { nudge(); return; }

		if (e->kind == ENT_FOLDER) { nav_push(e->view, e->sysidx); return; }
		if (e->kind == ENT_BROWSE)
		{
			browse_load(e->sysidx, "");
			go_screen(SCR_BROWSE);
			return;
		}

		// Already inside this one: drop back into it rather than reloading it.
		if (ig_is_running(cur_game())) { ig_close(1); return; }

		curtain = 0;
		launch_at = GetTimer(0);
		go_screen(SCR_LAUNCH);
		break;
	}

	case SCR_MENUBAR:
		switch (mb_at(mb_idx))
		{
		case MB_DISPLAY:
		{
			chome_item *lit = cur_game();
			int vclass = sel_class();
			int cur = vp_effective(lit ? lit->sysidx : -1, vclass);

			int opts[VP_MAX_OPTIONS];
			int n = vp_options_for(vclass, opts);
			look_row = 0;
			for (int i = 0; i < n; i++) if (opts[i] == cur) { look_row = i; break; }

			go_screen(SCR_DISPLAY);
			break;
		}
		case MB_OPTIONS:  opt_row = 0; go_screen(SCR_OPTIONS); break;
		case MB_POWER:    pwr_row = 0; pwr_arm = -1; go_screen(SCR_POWER); break;
		case MB_ABOUT:    go_screen(SCR_ABOUT); break;
		case MB_CORE:
			core_opts_scan();
			co_tier = CO_TIER_PICTURE;
			// Land on a page that has something, so an empty Picture list is not the
			// first thing a player meets on a core whose options are all elsewhere.
			if (!core_opts_tier_count(co_tier)) co_tier = CO_TIER_SYSTEM;
			if (!core_opts_tier_count(co_tier)) co_tier = CO_TIER_RISKY;
			co_row = 0;
			go_screen(SCR_CORE);
			break;
		}
		break;

	case SCR_CORE:
	{
		// Only the last row does anything with A: it turns the page.
		int n = core_opts_tier_count(co_tier);
		if (co_row < n) { nudge(); break; }

		co_tier = (co_tier == CO_TIER_PICTURE) ? CO_TIER_SYSTEM
			: (co_tier == CO_TIER_SYSTEM) ? CO_TIER_RISKY : CO_TIER_PICTURE;
		co_row = 0;
		mark_dirty();
		break;
	}

	case SCR_SORT:
		sort_mode = sort_idx;
		view_rebuild(0);
		go_screen(SCR_HOME);
		break;

	case SCR_DISPLAY:
	{
		chome_item *lit = cur_game();
		if (!lit) { nudge(); break; }

		int vclass = sel_class();
		int opts[VP_MAX_OPTIONS];
		int n = vp_options_for(vclass, opts);
		if (look_row < 0 || look_row >= n) { nudge(); break; }

		vp_set(lit->sysidx, vclass, opts[look_row]);

		// The game it applies to is on screen behind this menu, so show it there now.
		if (ig_active && ig_is_running(lit)) vp_apply_now(lit->sysidx, vclass);

		printf("ClassicUI: %s (%s) now uses look \"%s\"\n",
			lib_sys(lit->sysidx) ? lib_sys(lit->sysidx)->name : "?",
			vp_class_label(vclass), vp_name(opts[look_row]));
		mark_dirty();
		break;
	}

	case SCR_OPTIONS:
		switch (opt_row)
		{
		case 0: cfg.classicui_artfetch = cfg.classicui_artfetch ? 0 : 1; mark_dirty(); break;
		// gl_forget() as well: a rescan is also how a player says "I have re-scraped",
		// and the parsed gamelists would otherwise still be the ones from before.
		case 1: lib_rescan(); gl_forget(); art_shutdown(); art_init(theme_get()->sel_w, theme_get()->sel_h); view_rebuild(0); break;
		case 2: vp_install(); mark_dirty(); break;
		case 3: nudge(); break;                       // Layout changes with left/right
		case 4:
			/*
			  This used to hand the player to MiSTer's own joystick setup, which meant
			  leaving the front-end for a classic-OSD panel that names buttons by
			  number. Pairing a controller is the one setup job a console cannot ask a
			  keyboard to do, so it belongs here.
			*/
			pads_row = 0;
			pads_forget_arm = -1;
			go_screen(SCR_PADS);
			break;

		case 5:
			wifi_row = 0;
			wifi_top = 0;
			go_screen(SCR_WIFI);
			if (net_present() && !net_count()) net_scan_start();
			break;

		case 6:
			ini_refresh();
			go_screen(SCR_INI);
			break;

		case 7:
			set_refresh();
			go_screen(SCR_SET);
			break;

		case 8:
			if (!ig_active) { chome_leave(); break; }

			/*
			  The core's own options, which live in the classic OSD and nowhere else.

			  Our menu goes away first - it owns the framebuffer and the OSD is composited
			  over the same screen - and then the OSD is opened the way the menu button
			  opens it. ig_close(1) also puts the video mode and the pause back, so the
			  core is running normally underneath it, which is what its own settings
			  screen expects.

			  Getting back here needs nothing: while the OSD is visible the menu button
			  belongs to it, and once it is closed the next press is ours again.
			*/
			/*
			  The flag goes up before the close, so the menu key that follows is left for
			  the classic menu instead of being taken as "open Classic Home".
			*/
			/*
			  The release, not the press. menu.cpp opens the classic menu on
			  KEY_F12 | UPSTROKE; its press branch only does anything when the OSD is
			  already on screen, which here it is not. Queueing the press did nothing at
			  all, and the handoff timed out four seconds later having achieved only a
			  closed menu - which is what Derek saw as "Core Settings just goes back to
			  the game". The comment above eat_menu_release has said the menu opens on the
			  release since the day it was written; I simply had not applied it here.
			*/
			printf("ClassicUI: handing the screen to the core's own options\n");
			osd_handoff = OSDH_WAITING;
			osd_handoff_until = GetTimer(4000);
			ig_close(1);
			menu_key_set(KEY_F12 | UPSTROKE);
			break;

		case 9:
			if (!ig_active) break;

			// Closing the game loses unsaved progress, so it takes two presses.
			if (!CheckTimer(ig_close_until))
			{
				quit_to_home(1);
			}
			else
			{
				ig_close_until = GetTimer(3000);
				mark_dirty();
			}
			break;
		}
		break;

	case SCR_DISCBAR:
		disc_open_screen();
		break;

	case SCR_DISC:
	{
		disc_build_rows();
		if (disc_row < 0 || disc_row >= disc_nrows) { nudge(); break; }

		if (disc_rowact[disc_row] == DACT_CHOOSE)
		{
			disc_picking = 1;
			disc_row = 0;
			disc_build_rows();
			mark_dirty();
			break;
		}

		/*
		  A core was chosen. Remembered so re-opening the prompt shows the decision
		  rather than starting from the guess.

		  It launches if that core's daemon can read from the drive - PC Engine CD is
		  the only one so far - and otherwise says why not, in the log and with a nudge.
		  A button that silently does nothing would be worse than one that refuses.
		*/
		disc_chosen_sys = disc_rowsys[disc_row];
		disc_picking = 0;
		disc_row = 0;

		{
			const chome_sys *sc = (disc_chosen_sys >= 0) ? lib_sys(disc_chosen_sys) : 0;

			if (disc_slot_for(disc_chosen_sys))
			{
				disc_launch(disc_chosen_sys);
				break;
			}

			printf("ClassicUI: disc -> %s (%s), not launched: that core's daemon does not read from the drive yet\n",
				sc ? sc->name : "?", disc_display_name());
			nudge();
		}

		mark_dirty();
		break;
	}

	case SCR_POWER:
		if (pwr_arm == pwr_row && !CheckTimer(pwr_until))
		{
			/*
			  Flush first. The card is mounted sync, but the library index and the state
			  file are ours and there is no reason to find out the hard way.
			*/
			printf("ClassicUI: %s\n", pwr_row ? "shutting down" : "restarting");
			lib_state_save();
			sync();
			system(pwr_row ? "poweroff" : "reboot");
			break;
		}

		pwr_arm = pwr_row;
		pwr_until = GetTimer(3000);
		mark_dirty();
		break;

	case SCR_SET:
	{
		/*
		  On a setting, A steps it forward - the same thing right does, so a player who
		  only ever presses A can still change everything on the screen. A number wraps
		  round at the top here where right stops, because A has no other meaning to
		  fall back on and a value that cannot be got back to would be a trap.
		*/
		if (set_row < set_nview)
		{
			int i = set_view[set_row];
			const opt_def *o = opt_at(i);

			if (!opt_step_by(i, 1))
			{
				if (o->kind != OPT_NUMBER || opt_value(i) == o->lo) { nudge(); break; }
				opt_set(i, o->lo);
			}
			set_edited();
			break;
		}

		if (!opt_dirty()) { nudge(); break; }

		// Rewriting the player's own ini takes two presses, as everything here that
		// touches a real file does.
		if (set_arm && !CheckTimer(set_arm_until))
		{
			set_arm = 0;
			int w = opt_apply(ini_path());

			if (w < 0) { set_failed = 1; set_wrote = -1; }
			else
			{
				set_wrote = w;
				set_failed = 0;

				/*
				  classicui_overscan is in the set and every layout metric is derived
				  from it, so the theme has to be recomputed even though the canvas is
				  exactly the size it was - which is the one case theme_update() skips.
				*/
				theme_invalidate();
				theme_update(gfx_w(), gfx_h(), cfg.classicui_profile);
				art_init(theme_get()->sel_w, theme_get()->sel_h);
				gfx_damage_all();
			}

			set_summary();
			if (set_row > set_nview) set_row = set_nview;
			mark_dirty();
			break;
		}

		set_arm = 1;
		set_arm_until = GetTimer(3000);
		mark_dirty();
		break;
	}

	case SCR_INI:
		// Once it has run, A is the way out as well as B - the result panel has nothing
		// else to act on and a player who pressed A to get here will press A again.
		if (ini_wrote != -1) { go_screen(SCR_OPTIONS); break; }
		if (!ini_n) { nudge(); break; }

		if (ini_armed && !CheckTimer(ini_until))
		{
			// Read before the write, because ini_apply() rewrites the file the plan was
			// made from and the answer has to describe what was actually changed.
			ini_needs_restart = ini_plan_restart(ini_list, ini_n);
			ini_wrote = ini_apply(ini_path());
			if (ini_wrote < 0) ini_wrote = -2;         // the result panel says why
			ini_armed = 0;
			mark_dirty();
			break;
		}

		ini_armed = 1;
		ini_until = GetTimer(3000);
		mark_dirty();
		break;

	case SCR_PADS:
	{
		// While it is running, A is not the way out - B is, and the legend says so.
		if (bt_pairing()) { nudge(); break; }

		/*
		  A finished pairing owns the screen, so A means what its panel offers - "Add
		  Another" or "Try Again" - and not whatever row is selected in the list behind it.
		  Checked before the row is read, or acknowledging a result would open the tester.
		*/
		if (bt_pair_state() == BTP_IDLE)
		{
			pad_row sel;
			if (!pads_sel(&sel)) { nudge(); break; }

			if (!sel.is_add) { padtest_open(&sel); break; }
		}

		if (!bt_present()) { nudge(); break; }

		bt_pair_start();
		if (!bt_pairing()) nudge();
		mark_dirty();
		break;
	}

	case SCR_WIFI:
	{
		int js = net_join_state();
		if (js == JOIN_WORK) { nudge(); break; }
		if (js != JOIN_IDLE) { net_join_ack(); mark_dirty(); break; }

		const net_ap *a = net_at(wifi_row);
		if (!a) { nudge(); break; }

		snprintf(wifi_pick, sizeof(wifi_pick), "%s", a->ssid);
		wifi_pick_secure = a->secure;

		if (!a->secure)
		{
			// An open network needs nothing typed, so do not make them type nothing.
			net_join(wifi_pick, "", 0);
			mark_dirty();
			break;
		}

		char pr[128];
		snprintf(pr, sizeof(pr), "Enter the password for %s", wifi_pick);
		osk_dest = OSKD_WIFI;
		osk_open("Wi-Fi password", pr, "", 1);
		mark_dirty();
		break;
	}

	case SCR_ABOUT:
		go_screen(SCR_MENUBAR);
		break;

	case SCR_SUSPEND:
	{
		chome_item *it = cur_game();
		if (!it || !slot_state(it, slot_idx)) { nudge(); return; }

		// Inside that very game we restore directly, through the same status bit
		// the OSD pulses, and drop straight back into play.
		if (ig_is_running(it) && ss_can_load())
		{
			// Loading a slot by hand replaces the moment we froze, so the freeze
			// must not be put back over the top of it on the way out.
			if (ss_do_load(slot_idx)) { ig_frozen = 0; ig_close(1); }
			else nudge();
			return;
		}

		/*
		  Otherwise the game is not running and this means "start it here". The comment that
		  used to be on this line said the core picks its slot up; nothing carried the slot,
		  so it never did - the game simply started from the beginning and the chosen state
		  was ignored without a word. Arming the resume record is what makes it true: it is
		  the same mechanism Resume uses, and resume_poll() loads it once the core is up.
		*/
		susp_arm(it, slot_idx);
		curtain = 0;
		launch_at = GetTimer(0);
		go_screen(SCR_LAUNCH);
		break;
	}

	case SCR_BROWSE:
	{
		if (!nbent) { nudge(); return; }
		browse_ent *b = &bent[browse_sel];
		if (b->isdir)
		{
			char rel[CH_PATH_LEN];
			if (browse_rel[0]) snprintf(rel, sizeof(rel), "%s/%s", browse_rel, b->name);
			else snprintf(rel, sizeof(rel), "%s", b->name);
			browse_load(browse_sys, rel);
			mark_dirty();
		}
		else
		{
			char rel[CH_PATH_LEN];
			if (browse_rel[0]) snprintf(rel, sizeof(rel), "%s/%s", browse_rel, b->name);
			else snprintf(rel, sizeof(rel), "%s", b->name);
			do_launch(browse_sys, rel, 0);
		}
		break;
	}
	}
}

static void back()
{
	switch (screen)
	{
	/*
	  Back out of the core chooser to the offer, rather than out of the disc prompt
	  altogether: the player who opened the chooser to look at their options should
	  not lose the prompt for doing so.
	*/
	case SCR_DISCBAR:
		go_screen(SCR_HOME);
		return;

	case SCR_DISC:
		if (disc_picking)
		{
			disc_picking = 0;
			disc_row = 0;
			disc_build_rows();
			mark_dirty();
			return;
		}

		// Back to the disc it belongs to, which is where the prompt was opened from -
		// falling through to the generic back dropped the player onto the shelf.
		go_screen(SCR_DISCBAR);
		return;

	case SCR_HOME:
		if (nav_pop()) break;

		/*
		  Top of the shelf: B is a navigation key, not a way out. What it saves is the
		  walk left to Favourites and Systems, which sit at the head of a row of
		  hundreds of games - so it jumps to the leftmost entry, and deliberately not to
		  the menu bar. In a game core it used to close the menu from here; going back to
		  the game is the menu button's job now, so that no longer competes with this.
		*/
		if (sel > 0) { sel = 0; slot_idx = 0; mark_dirty(); }
		else nudge();
		break;

	case SCR_SORT:
	case SCR_DISPLAY:
	case SCR_OPTIONS:
	case SCR_ABOUT:
		go_screen(SCR_MENUBAR);
		break;

	case SCR_POWER:
		if (pwr_arm >= 0) { pwr_arm = -1; mark_dirty(); break; }   // first B cancels
		go_screen(SCR_MENUBAR);
		break;

	case SCR_INI:
		if (ini_armed) { ini_armed = 0; mark_dirty(); break; }     // first B cancels
		go_screen(SCR_OPTIONS);
		break;

	case SCR_SET:
		if (set_arm) { set_arm = 0; mark_dirty(); break; }         // first B cancels

		/*
		  Leaving with edits that were never written throws them away, and nothing on
		  the way out would otherwise say so - the panel simply closes. So it asks, the
		  same way closing a game does, rather than saving them on the way out: a write
		  the player did not ask for is the worse of the two surprises.
		*/
		if (opt_dirty() && !(set_quit_arm && !CheckTimer(set_quit_until)))
		{
			set_quit_arm = 1;
			set_quit_until = GetTimer(3000);
			mark_dirty();
			break;
		}

		set_quit_arm = 0;
		go_screen(SCR_OPTIONS);
		break;

	case SCR_PADS:
		/*
		  B out of pairing mode stops it and stays here, so the controller that has just
		  paired is visible in the list rather than the player being returned to Options
		  wondering whether it worked. A second B leaves.
		*/
		if (bt_pairing()) { bt_pair_stop(); mark_dirty(); break; }
		if (bt_pair_state() != BTP_IDLE) { bt_pair_ack(); mark_dirty(); break; }
		go_screen(SCR_OPTIONS);
		break;

	case SCR_WIFI:
		// A join in flight is not cancellable - the child is already reconfiguring
		// the interface - so B acknowledges the result instead of abandoning it.
		if (net_join_state() != JOIN_IDLE && net_join_state() != JOIN_WORK) net_join_ack();
		go_screen(SCR_OPTIONS);
		break;

	case SCR_BROWSE:
		if (browse_rel[0])
		{
			char rel[CH_PATH_LEN];
			snprintf(rel, sizeof(rel), "%s", browse_rel);
			char *slash = strrchr(rel, '/');
			if (slash) *slash = 0;
			else rel[0] = 0;
			browse_load(browse_sys, rel);
			mark_dirty();
		}
		else go_screen(SCR_HOME);
		break;

	default:
		go_screen(SCR_HOME);
		break;
	}
}

/* ------------------------------------------------------ savestate hooks --- */

/*
  MiSTer's savestates are driven by the core, but the OSD reaches them the same way
  it reaches any core option: the core declares entries in its CONF_STR and the ARM
  pulses the status bit behind them. A momentary entry (T/t/R/r) is set to 1 then
  back to 0, exactly as the generic menu does in menu.cpp.

  So we scan CONF_STR for the framework's savestate entries and drive the same
  bits. This is label matching against strings the core author wrote, so it works
  for cores using the standard wording and simply finds nothing for the rest - in
  which case the buttons are not offered rather than doing nothing when pressed.
*/

struct ss_hooks
{
	int  found_save, found_load, found_slot;
	char save_opt[32], load_opt[32], slot_opt[32];
	int  save_ex, load_ex, slot_ex;
	int  slot_count;

	/*
	  Pause. There is no pause command in MiSTer: sys_top drives osd_status into the
	  core as OSD_STATUS (emu_ports.vh), and a core pauses on it only when its own
	  "Pause when OSD is open" option is On - an option that ships Off. So pausing
	  means two things at once: hold OSD_STATUS high, which ig_open/enter do by
	  leaving the OSD enabled, and force that option On for as long as we are up.

	  An earlier version of this dismissed that route and hunted for a pause command
	  instead, which no core offers - so nothing ever paused.
	*/
	int  found_pause;
	int  pause_is_option;         // O/o entry (set a value) vs T/R (pulse)
	int  pause_needs_osd;         // gated on OSD_STATUS, so useless to us
	char pause_opt[32];
	int  pause_ex;
	uint32_t pause_on_val;        // which value of that option means "paused"

	/*
	  Savestates to SDCard. When this is Off the core keeps states in memory and no
	  file is ever written, so a save from here would look like it did nothing. Its
	  default is On, but a core config on the card can have turned it off.
	*/
	int  found_sd;
	char sd_opt[32];
	int  sd_ex;
	uint32_t sd_on_val;
};

static ss_hooks ss_hk;
static int ss_hk_valid = 0;

static int label_has(const char *label, const char *needle)
{
	return strcasestr(label, needle) ? 1 : 0;
}

// Copies the option spec (everything up to the first comma) out of an entry.
static void ss_copy_opt(const char *p, char *out, int len)
{
	int i = 0;
	while (p[i] && p[i] != ',' && i < len - 1) { out[i] = p[i]; i++; }
	out[i] = 0;
}

static void ss_scan_hooks()
{
	memset(&ss_hk, 0, sizeof(ss_hk));
	ss_hk_valid = 1;

	// A terse "Slot" is only believed as a last resort - see the slot branch below.
	char slot_fb[32] = {};
	int slot_fb_ex = 0, slot_fb_count = 0, have_slot_fb = 0;

	for (int i = 2; i < 128; i++)
	{
		char *p = user_io_get_confstr(i);
		if (!p) break;

		/*
		  Strip the prefixes the generic menu strips: hide/disable flags, and the
		  P<n> page markers. Missing the page marker is why the pause option was
		  never found - the real one reads "P3OQ,Pause when OSD is open,Off,On",
		  and without this it parses as an entry of unknown type "P".
		*/
		while (strlen(p) > 2 &&
		       (p[0] == 'H' || p[0] == 'D' || p[0] == 'h' || p[0] == 'd' ||
		        (p[0] == 'P' && p[1] >= '0' && p[1] <= '9'))) p += 2;
		if (!p[0]) continue;

		char label[128] = {};
		substrcpy(label, p, 1);
		if (!label[0]) continue;

		char c = p[0];
		int momentary = (c == 'T' || c == 'R' || c == 't' || c == 'r');
		int option = (c == 'O' || c == 'o');
		int ex = (c == 't' || c == 'r' || c == 'o');

		if (momentary && !ss_hk.found_save && label_has(label, "save state"))
		{
			ss_copy_opt(p + 1, ss_hk.save_opt, sizeof(ss_hk.save_opt));
			ss_hk.save_ex = ex;
			ss_hk.found_save = 1;
		}
		else if (momentary && !ss_hk.found_load &&
			(label_has(label, "restore state") || label_has(label, "load state")))
		{
			ss_copy_opt(p + 1, ss_hk.load_opt, sizeof(ss_hk.load_opt));
			ss_hk.load_ex = ex;
			ss_hk.found_load = 1;
		}
		else if (!ss_hk.found_pause && label_has(label, "pause"))
		{
			/*
			  This is nearly always "Pause when OSD is open", an option rather than a
			  command, and it is the one that works: the core watches OSD_STATUS and
			  honours it only while this is On. Find which of its values says On, since
			  the order is the core's choice ("Off,On" here, but not guaranteed).
			*/
			if (momentary || option)
			{
				const char *spec = p + 1;
				if (spec[0] == 'X') spec++;
				ss_copy_opt(spec, ss_hk.pause_opt, sizeof(ss_hk.pause_opt));
				ss_hk.pause_ex = ex;
				ss_hk.pause_is_option = option;
				ss_hk.pause_needs_osd = label_has(label, "osd");
				ss_hk.pause_on_val = 1;

				if (option)
				{
					char v[64];
					for (int n = 0; n < 8 && substrcpy(v, p, (char)(2 + n)) && v[0]; n++)
					{
						if (!strcasecmp(v, "on") || !strcasecmp(v, "yes")) { ss_hk.pause_on_val = (uint32_t)n; break; }
					}
				}
				ss_hk.found_pause = 1;
			}
		}
		else if (option && !ss_hk.found_sd && label_has(label, "savestates to sd"))
		{
			const char *spec = p + 1;
			if (spec[0] == 'X') spec++;
			ss_copy_opt(spec, ss_hk.sd_opt, sizeof(ss_hk.sd_opt));
			ss_hk.sd_ex = ex;
			ss_hk.sd_on_val = 0;

			char v[64];
			for (int n = 0; n < 8 && substrcpy(v, p, (char)(2 + n)) && v[0]; n++)
			{
				if (!strcasecmp(v, "on") || !strcasecmp(v, "yes")) { ss_hk.sd_on_val = (uint32_t)n; break; }
			}
			ss_hk.found_sd = 1;
		}
		else if (option && !ss_hk.found_slot && label_has(label, "slot"))
		{
			const char *spec = p + 1;
			if (spec[0] == 'X') spec++;             // set-by-ARM marker

			// How many values the option offers, so we never select a missing slot.
			int n = 0;
			char v[64];
			while (n < 8 && substrcpy(v, p, (char)(2 + n)) && v[0]) n++;

			/*
			  "Slot" on its own is not necessarily ours. MSX means a cartridge slot by it
			  and Apple II an expansion slot - both were matched here, and in a core that
			  also had savestates, picking slot 2 would have switched hardware under the
			  player instead. Measured on the device: MSX reported slots=2 and Apple II
			  slots=3 with no save or load entry at all.

			  So take a label that says which kind of slot it means, and keep a bare one
			  only as a fallback - for a core that does have savestates and named the
			  option tersely.
			*/
			if (label_has(label, "state") || label_has(label, "save"))
			{
				ss_copy_opt(spec, ss_hk.slot_opt, sizeof(ss_hk.slot_opt));
				ss_hk.slot_ex = ex;
				ss_hk.slot_count = n ? n : 4;
				ss_hk.found_slot = 1;
			}
			else if (!have_slot_fb)
			{
				ss_copy_opt(spec, slot_fb, sizeof(slot_fb));
				slot_fb_ex = ex;
				slot_fb_count = n ? n : 4;
				have_slot_fb = 1;
			}
		}
	}

	/*
	  The fallback, and only where the core really does have savestates - otherwise a
	  cartridge slot would be adopted as a savestate selector in a core that has none.
	  Done after the loop rather than inside it, because the CONF_STR is free to list the
	  slot option before the save and restore entries.
	*/
	if (!ss_hk.found_slot && have_slot_fb && (ss_hk.found_save || ss_hk.found_load))
	{
		memcpy(ss_hk.slot_opt, slot_fb, sizeof(ss_hk.slot_opt));
		ss_hk.slot_ex = slot_fb_ex;
		ss_hk.slot_count = slot_fb_count;
		ss_hk.found_slot = 1;
	}

	printf("ClassicUI: core hooks - sdcard:%s save:%s load:%s slot:%s(%d) pause:%s%s\n",
		ss_hk.found_sd ? ss_hk.sd_opt : "-",
		ss_hk.found_save ? ss_hk.save_opt : "-",
		ss_hk.found_load ? ss_hk.load_opt : "-",
		ss_hk.found_slot ? ss_hk.slot_opt : "-", ss_hk.slot_count,
		ss_hk.found_pause ? ss_hk.pause_opt : "-",
		ss_hk.found_pause ? (ss_hk.pause_is_option ? " (option)" : " (pulse)") : "");
}

static const ss_hooks *ss_get()
{
	if (!ss_hk_valid) ss_scan_hooks();
	return &ss_hk;
}

// Pulses a momentary status bit, the way the generic menu does.
static void ss_pulse(const char *opt, int ex)
{
	user_io_status_set(opt, 1, ex);
	user_io_status_set(opt, 0, ex);
}

static int ss_select_slot(int slot)
{
	const ss_hooks *h = ss_get();
	if (!h->found_slot) return (slot == 0);          // single-slot core
	if (slot < 0 || slot >= h->slot_count) return 0;

	user_io_status_set(h->slot_opt, (uint32_t)slot, h->slot_ex);
	return 1;
}

static int ss_can_save() { return ss_get()->found_save; }

/* ------------------------------------------------------- suspend points --- */

/*
  Putting a game away and getting it back later. A core that cannot host the
  front-end has nowhere to draw a menu, so MENU means "put this away" - and doing
  that without keeping the moment would throw the session out, which is the one
  thing a shelf full of games must not do.

  The state itself is an ordinary savestate in the core's reserved last slot, which
  the player is never shown (see CH_SLOTS_USER). This file only records which game the
  last suspend belongs to, so the shelf can offer Resume and the core can restore
  itself once the ROM is up.
*/
#define SUSPEND_FILE "classicui/suspend.txt"

static int susp_read(char *sysid, int sysmax, char *relpath, int pathmax, int *slot)
{
	char full[1024];
	snprintf(full, sizeof(full), "%s/%s", getRootDir(), SUSPEND_FILE);

	FILE *f = fopen(full, "rt");
	if (!f) return 0;

	char sl[32] = {};
	int ok = (fgets(sysid, sysmax, f) && fgets(relpath, pathmax, f) && fgets(sl, sizeof(sl), f));
	fclose(f);
	if (!ok) return 0;

	for (char *q = sysid; *q; q++) if (*q == '\n') { *q = 0; break; }
	for (char *q = relpath; *q; q++) if (*q == '\n') { *q = 0; break; }
	*slot = atoi(sl);
	return (sysid[0] && relpath[0]);
}

static void susp_clear()
{
	char full[1024];
	snprintf(full, sizeof(full), "%s/%s", getRootDir(), SUSPEND_FILE);
	unlink(full);
}

// Does this game have a suspend point waiting?
static int susp_matches(const chome_item *it)
{
	if (!it) return 0;

	const chome_sys *sy = lib_sys(it->sysidx);
	if (!sy) return 0;

	char sysid[64], relpath[CH_PATH_LEN];
	int slot = 0;
	if (!susp_read(sysid, sizeof(sysid), relpath, sizeof(relpath), &slot)) return 0;

	return (!strcmp(sysid, sy->id) && !strcmp(relpath, it->path));
}

// The core's last slot: a suspend is automatic and frequent, so it stays out of the
// slots the player picked by hand.
/*
  How many slots the player actually gets.

  The last slot a core offers is reserved to hold the game still while the menu is open
  (susp_slot), so the player gets the ones before it - and a core does not have to offer
  four. PSX, GBA and WonderSwan offer **two**, which leaves exactly one.

  Showing three regardless meant the third slot *was* the reserved one: saving there
  copied the held state onto itself and looked like it had worked, and loading it restored
  the moment the menu was opened - which looks exactly like a load doing nothing. That is
  what Derek hit on PSX with Destruction Derby.

  Only knowable once the core is up, since the count comes from its CONF_STR. From the
  shelf the strip is a display of files that already exist, not somewhere to save into, so
  the full three are shown there as before.
*/
static int user_slots()
{
	if (!ig_active || !ss_hk_valid) return CH_SLOTS_USER;

	const ss_hooks *h = ss_get();
	if (!h->found_save && !h->found_load) return CH_SLOTS_USER;

	int total = h->found_slot ? h->slot_count : 1;
	int n = total - 1;
	if (n < 0) n = 0;
	return (n < CH_SLOTS_USER) ? n : CH_SLOTS_USER;
}

static int susp_slot()
{
	const ss_hooks *h = ss_get();
	int n = h->found_slot ? h->slot_count : 1;
	return (n > 0) ? n - 1 : 0;
}

/*
  Keeping the suspend slot quiet.

  A player who never asked for a save should not be told one happened, and two things
  in the firmware announce every state anyway: the core lists its own messages in
  CONF_STR ("Save to state 4") and raises one for the firmware to pop up as a
  classic-OSD panel, and process_ss() captures a thumbnail beside each state file so
  the strip has a picture. Neither belongs to a slot the player cannot see.

  Both arrive a poll or two after the pulse rather than during it - the core writes
  the state on its own schedule and the firmware only notices on its next poll - so
  this is a window opened around the write, not a flag held across it. Its width only
  has to cover those polls; over-suppressing costs nothing, because everything it can
  swallow is an OSD panel that would have been drawn over the front-end.
*/
#define SS_QUIET_MS 4000
static unsigned long ss_quiet_until = 0;

int chome_ss_quiet()
{
	return (ss_quiet_until && !CheckTimer(ss_quiet_until));
}

// Which slot the firmware should keep quiet about, or -1 when we have reserved none.
int chome_hidden_slot()
{
	if (!cfg.classicui) return -1;
	return susp_slot();
}

// Best effort: a core with no savestates just cannot be suspended, and quitting
// still has to work.
/*
  Point the resume machinery at a game and a slot, without saving anything.

  susp_write() below both saves a state and writes this record, which is right for putting a
  game away. Launching a game *into* an existing state needs only the record: the state is
  already on the card, and resume_poll() loads whatever this names once the core is up.
*/
static int susp_arm(const chome_item *it, int slot)
{
	if (!it) return 0;

	const chome_sys *sy = lib_sys(it->sysidx);
	if (!sy) return 0;

	char dir[1024];
	snprintf(dir, sizeof(dir), "%s/classicui", getRootDir());
	mkdir(dir, 0777);

	char full[1024];
	snprintf(full, sizeof(full), "%s/%s", getRootDir(), SUSPEND_FILE);
	FILE *f = fopen(full, "wt");
	if (!f) return 0;

	fprintf(f, "%s\n%s\n%d\n", sy->id, it->path, slot);
	fclose(f);

	printf("ClassicUI: %s will start from slot %d\n", it->title, slot + 1);
	return 1;
}

static int susp_write()
{
	/*
	  Quitting from a core that cannot host the menu never got as far as reading the
	  launch record, so it did not know which game it was putting away. Read it here:
	  it is the same record, and it now names its core so a stale one cannot mislead.
	*/
	if (!ig_have_item) ig_load_item();
	if (!ig_have_item || !ss_can_save()) return 0;

	int slot = susp_slot();
	if (!ss_do_save(slot)) return 0;

	const chome_sys *sy = lib_sys(ig_item.sysidx);
	if (!sy) return 0;

	char dir[1024];
	snprintf(dir, sizeof(dir), "%s/classicui", getRootDir());
	mkdir(dir, 0777);

	char full[1024];
	snprintf(full, sizeof(full), "%s/%s", getRootDir(), SUSPEND_FILE);
	FILE *f = fopen(full, "wt");
	if (!f) return 0;

	fprintf(f, "%s\n%s\n%d\n", sy->id, ig_item.path, slot);
	fclose(f);

	printf("ClassicUI: suspended %s into slot %d\n", ig_item.title, slot + 1);
	return 1;
}

/*
  Silencing the game while the menu is up.

  The core keeps running behind the still (see freeze_engage), and a game you can
  hear playing on without you reads as "something is wrong" in a way that a frozen
  frame does not. Muting costs nothing and removes the whole question.

  Not set_volume(): that draws an on-screen "Mute" popup, and the OSD composites over
  this UI. audio_mute() is the same register write without the message, and it leaves
  the saved volume alone - this lasts as long as the menu, it is not a preference.
*/
static void ig_mute_engage()
{
	ig_mute_was = audio_is_muted();
	if (ig_mute_was) return;                       // his own mute; nothing for us to undo

	audio_mute(1);
	ig_muted = 1;
	printf("ClassicUI: game muted\n");
}

static void ig_mute_release()
{
	if (!ig_muted) return;

	ig_muted = 0;
	audio_mute(0);
	printf("ClassicUI: sound back\n");
}

/*
  Holding a game still when the core cannot be paused.

  Most cores only pause while the OSD is on screen, and the OSD draws over this UI,
  so that route is closed (see ss_pause_engage). What is left is what he suggested:
  write a state on the way in and put it back on the way out. The game does keep
  running behind the still, but nothing that happens to it survives, so from the
  player's side the moment is kept - which is the point.

  Uses the same slot as a suspend point, because both mean the same thing: where you
  were when you left.
*/
static int freeze_engage()
{
	if (ig_paused) return 0;                       // a real pause is better

	/*
	  The one operation in this front-end that can take a core down with it.

	  Holding the game still means asking the core for a save state, and the SNES core dies
	  if it is asked during the Battletoads intro - black picture, no more save states,
	  nothing short of reloading the core recovers it. That is upstream, not ours: it
	  reproduces from MiSTer's own Alt-F1 hotkey with this front-end out of the loop, and
	  the same save five seconds earlier in the same intro is fine.

	  Since every menu open in a core without a real pause fires one of these, the setting
	  exists to take the whole class of risk away. Off means the game plays on behind the
	  menu - which is already what Neo Geo, Mega Drive and N64 do here.
	*/
	if (!cfg.classicui_freeze) return 0;

	if (!ss_can_save() || !ss_can_load()) return 0;

	if (!ss_do_save(susp_slot())) return 0;

	printf("ClassicUI: no pause in this core, holding it still with a state\n");
	return 1;
}

static void freeze_release(int engaged)
{
	if (!engaged) return;

	if (ss_do_load(susp_slot())) printf("ClassicUI: put the game back where it was\n");
	else printf("ClassicUI: could not put the game back\n");
}

/*
  Saving into a slot the player chose, from the state that is already on disk.

  The reserved slot holds this exact moment - freeze_engage() wrote it when the menu
  opened - so "save here" is a copy rather than a second save. That is not only
  cheaper. A save pulse has to be serviced by a running core, so the old path had to
  resume and close the menu, which stored a moment slightly *after* the one the player
  was looking at. A copy stores what is on screen and the menu stays up.

  The picture comes from ig_shot, the still this menu is drawn over, so thumbnail and
  state are the same instant. process_ss() cannot supply one here: it reads through the
  scaler, which is showing our framebuffer, so it would capture the menu itself.
*/
static int copy_file(const char *src, const char *dst)
{
	FILE *in = fopen(src, "rb");
	if (!in) return 0;

	FILE *out = fopen(dst, "wb");
	if (!out) { fclose(in); return 0; }

	static char buf[64 * 1024];
	int ok = 1;
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
	{
		if (fwrite(buf, 1, n, out) != n) { ok = 0; break; }
	}
	if (ferror(in)) ok = 0;

	fclose(in);
	if (fclose(out) && ok) ok = 0;
	if (!ok) unlink(dst);
	return ok;
}

// Where a slot's picture goes, given where its state goes.
static int slot_png_path(const chome_item *it, int slot, char *out, int len)
{
	if (!lib_slot_target(it, slot, out, len)) return 0;

	char *dot = strrchr(out, '.');
	if (!dot) return 0;

	strcpy(dot, ".png");
	return 1;
}

/*
  The picture, written the moment the player asks rather than when the state lands.

  ig_shot is the still this menu is drawn over, so it is the exact frame they were
  looking at - and taking it now means the picture does not depend on how long the core
  takes to write its state, or on the menu still being open by then.
*/
static int ss_write_thumb(const chome_item *it, int slot)
{
	if (!it || !ig_shot || ig_shot_w < 1 || ig_shot_h < 1) return 0;

	char png[1024];
	if (!slot_png_path(it, slot, png, sizeof(png))) return 0;

	/*
	  256 wide at the display aspect, not at the pixel aspect - see shot_fit(). By pixel
	  ratio a PSX frame came out 256x173 and a 240p one 256x96, both squashed.
	*/
	int ow = 256;
	int oh = ow * SHOT_AR_H / SHOT_AR_W;
	if (oh < 1) oh = 1;

	if (!write_screenshot(png, (const uint8_t *)ig_shot, ig_shot_w, ig_shot_h, ow, oh)) return 0;

	art_forget(png);        // the tile is looking at the moment we just replaced
	return 1;
}

/*
  Copies the held state into the chosen slot, if it is on disk yet.

  Returns 0 when there is nothing to copy *yet* as well as when there never will be, so
  the caller decides which it is - see the pending save below. The freshness test is what
  makes that distinction safe: the same game suspended in an earlier session leaves a file
  in that slot, and copying it would quietly store the wrong moment.
*/
static int ss_copy_state(const chome_item *it, int to_slot)
{
	if (!it || !ig_freeze_at) return 0;
	if (to_slot == susp_slot()) return 0;

	char src[1024], dst[1024];
	if (!lib_slot_target(it, susp_slot(), src, sizeof(src))) return 0;
	if (!lib_slot_target(it, to_slot, dst, sizeof(dst))) return 0;

	struct stat st;
	if (stat(src, &st)) return 0;
	if ((unsigned long)st.st_mtime + 2 < ig_freeze_at) return 0;

	if (!copy_file(src, dst))
	{
		printf("ClassicUI: could not copy the held state into slot %d\n", to_slot + 1);
		return 0;
	}

	/*
	  And in memory, which is the part that makes it loadable *now*. The slots the core
	  reads are buffers in DDR; the .ss files are a mirror read in once at ROM load time
	  and never read again. Copying only the file left a slot that looked saved on the
	  shelf and loaded nothing, because the core's buffer for it was still empty.
	*/
	if (!user_io_ss_copy_slot(susp_slot(), to_slot))
		printf("ClassicUI: slot %d has the file but not the memory - it will load next time\n", to_slot + 1);

	printf("ClassicUI: saved the held moment into slot %d\n", to_slot + 1);
	return 1;
}

/*
  A save the player has asked for and that is waiting on the core.

  The held state is written by the core and noticed by process_ss() on its next poll, so
  for about a second after the menu opens there is nothing to copy. Asking the player to
  wait for that, or hiding the option until it lands, both make them deal with an
  implementation detail. Instead the request is registered: the picture is taken at once,
  the slot says so, and the copy happens the moment the file appears.

  Falling back to a second save pulse is deliberately *not* done. That path has to resume
  the core to be serviced, so it closes the menu and stores a slightly later moment - and
  it is the path that re-entered HandleUI() through process_ss() and left the framebuffer
  in a state the next menu press drew garbage from.
*/
void chome_pend_poll();

static unsigned long pend_until = 0;

/*
  Held as paths rather than as a slot of the selected game, so the request describes
  itself. The player may press B a moment after asking - a save is a file copy and has no
  reason to be lost because the menu closed - and by then the selection, or the running
  game, may be something else entirely.
*/
static char pend_src[1024];
static char pend_dst[1024];
static char pend_png[1024];
static unsigned long pend_after = 0;     // the state must be newer than this
static int pend_reserved = -1;           // which slot holds it, for the copy in DDR

/*
  A core that pauses properly writes the chosen slot itself, so there is nothing to copy -
  only something to wait for. pend_reserved < 0 says so. What is waited on is the slot's own
  file changing, since the pause had to come off for the core to service the pulse at all
  and it has to go back on the moment the state lands.
*/
static int pend_repause = 0;             // the pause we took off, to put back
static int pend_pre_ok = 0;              // was there a file there before
static unsigned long pend_pre_mtime = 0;
static long long pend_pre_size = 0;

static void pend_start(const chome_item *it, int slot)
{
	if (!lib_slot_target(it, susp_slot(), pend_src, sizeof(pend_src))) return;
	if (!lib_slot_target(it, slot, pend_dst, sizeof(pend_dst))) return;
	if (!slot_png_path(it, slot, pend_png, sizeof(pend_png))) pend_png[0] = 0;

	pend_slot = slot;
	pend_reserved = susp_slot();
	pend_after = ig_freeze_at;
	pend_until = GetTimer(8000);
	pend_failed = -1;

	// The picture is of now, so it is taken now, whatever the state does.
	ss_write_thumb(it, slot);
	printf("ClassicUI: slot %d is waiting for the held state\n", slot + 1);
}

/*
  The same request on a core that pauses properly. No held state exists - there was nothing
  to hold still - so the core is asked directly, which means letting it run: a save pulse is
  only serviced by a running core. The menu stays up over the same still, the game is
  already muted, and the pause goes back on as soon as the state is there.

  Before this, Save on such a core resumed the game and closed the menu, which reads as the
  front-end throwing you out for pressing Save. SMS is the first core Derek has that pauses,
  which is how it surfaced.
*/
static int pend_direct_start(const chome_item *it, int slot)
{
	if (!lib_slot_target(it, slot, pend_src, sizeof(pend_src))) return 0;
	snprintf(pend_dst, sizeof(pend_dst), "%s", pend_src);
	if (!slot_png_path(it, slot, pend_png, sizeof(pend_png))) pend_png[0] = 0;

	// What the slot looks like now, so its replacement can be recognised.
	struct stat st;
	pend_pre_ok = !stat(pend_src, &st);
	pend_pre_mtime = pend_pre_ok ? (unsigned long)st.st_mtime : 0;
	pend_pre_size = pend_pre_ok ? (long long)st.st_size : 0;

	// Off with the pause, or the pulse is never serviced. ss_do_save() looks at ig_paused
	// to decide whether to lift it around the pulse, so it must not still be set here.
	int was = ig_paused;
	ss_pause_release(ig_paused);
	ig_paused = 0;

	if (!ss_do_save(slot))
	{
		if (was) ig_paused = ss_pause_engage();
		return 0;
	}

	pend_repause = was;
	pend_slot = slot;
	pend_reserved = -1;
	pend_after = (unsigned long)time(0);
	pend_until = GetTimer(8000);
	pend_failed = -1;

	ss_write_thumb(it, slot);              // the picture is of now, as in pend_start()
	printf("ClassicUI: slot %d is waiting for the core to write it\n", slot + 1);
	return 1;
}

/*
  The one case where the menu does not stop the game.

  Every other core is either paused or held still by a state, so the player can read a
  screen for as long as they like. A core with neither offers nothing to hold it with:
  the game plays on behind this menu and they can lose a life while deciding what to
  do. That is not something to infer from movement in the corner of the screen, so it
  says so across the top.

  Not while a save is in flight on a core that does pause: pend_direct_start() takes
  the pause off deliberately, because a save pulse is only serviced by a running core,
  and a red bar flashing up for that would be a lie about the core rather than a
  warning about it.
*/
static void draw_running_warning(const chome_profile *p)
{
	if (!ig_active || ig_paused || ig_frozen || pend_repause) return;

	int s = p->ts_ui;
	int h = 13 * s;

	/*
	  Inside the safe area, not at y=0. On a CRT the top of the canvas is behind the
	  bezel: at 240p with the default 6% overscan that is about 14 lines, and this band
	  is 13 tall - so drawn at the top it was entirely hidden bar one line of red, which
	  is how Derek found it. Every panel already respects safe_y; this did not.
	*/
	int y = p->safe_y;

	gfx_fill(0, y, p->w, h, COL_RED);
	gfx_fill(0, y + h, p->w, (s > 1) ? 2 : 1, COL_SHADOW);

	/*
	  Two wordings, because gfx_text_c() centres and a line too long for the canvas
	  loses both its ends. On the CRT the full sentence came out as "PLAYING - this
	  system cannot pause your", which drops the one word that carries the warning.
	*/
	const char *msg = "STILL PLAYING - this system cannot pause your game";
	int room = (p->w - 8 * s) / (8 * s);
	if ((int)strlen(msg) > room) msg = "STILL PLAYING - NOT PAUSED";

	gfx_text_c(gfx_clip(msg, s, p->w - 8 * s), p->w / 2, y + (h - 7 * s) / 2, s, COL_WHITE, 0);
}

// Runs every frame in every core, so a registered save finishes whether the menu is
// still open or not.
void chome_pend_poll()
{
	if (pend_slot < 0) return;

	struct stat st;

	if (pend_reserved < 0)
	{
		int done = (!stat(pend_src, &st)
			&& (!pend_pre_ok || (unsigned long)st.st_mtime != pend_pre_mtime
				|| (long long)st.st_size != pend_pre_size));

		if (!done && pend_until && !CheckTimer(pend_until)) return;

		/*
		  At the deadline a file stamped at or after the request still counts: a slot
		  overwritten inside the same second changes neither field.
		*/
		if (!done && !stat(pend_src, &st) && (unsigned long)st.st_mtime + 2 >= pend_after) done = 1;

		/*
		  Only while the menu is still up. The player may have closed it while this was in
		  flight, and pausing a core they are playing would freeze the game on them.
		*/
		if (pend_repause)
		{
			if (ig_active) ig_paused = ss_pause_engage();
			pend_repause = 0;
		}

		if (done)
		{
			printf("ClassicUI: the core wrote slot %d\n", pend_slot + 1);
			chome_item *sel = cur_game();
			if (sel) lib_refresh_slots(sel);
			if (ig_have_item) lib_refresh_slots(&ig_item);
		}
		else
		{
			if (pend_png[0]) unlink(pend_png);
			pend_failed = pend_slot;
			printf("ClassicUI: the core never wrote slot %d\n", pend_slot + 1);
		}

		pend_slot = -1;
		pend_until = 0;
		mark_dirty();
		return;
	}

	if (!stat(pend_src, &st) && (unsigned long)st.st_mtime + 2 >= pend_after)
	{
		if (copy_file(pend_src, pend_dst))
		{
			// See ss_copy_state(): the file is the mirror, DDR is what the core loads.
			if (!user_io_ss_copy_slot(pend_reserved, pend_slot))
				printf("ClassicUI: slot %d has the file but not the memory\n", pend_slot + 1);

			printf("ClassicUI: saved the held moment into slot %d\n", pend_slot + 1);

			chome_item *sel = cur_game();
			if (sel) lib_refresh_slots(sel);
			if (ig_have_item) lib_refresh_slots(&ig_item);

			pend_slot = -1;
			pend_until = 0;
			mark_dirty();
			return;
		}

		printf("ClassicUI: could not copy the held state into slot %d\n", pend_slot + 1);
		pend_until = 0;                  // fall through to the give-up below
	}

	if (!pend_until || CheckTimer(pend_until))
	{
		// No state arrived, so the picture would belong to a slot that is not there.
		if (pend_png[0]) unlink(pend_png);
		pend_failed = pend_slot;
		printf("ClassicUI: gave up waiting for the held state for slot %d\n", pend_slot + 1);

		pend_slot = -1;
		pend_until = 0;
		mark_dirty();
	}
}

/*
  Leaving a game for Classic Home. Suspending first where the core allows it, so
  closing a game is not the same as losing it.
*/
static void quit_to_home(int suspend)
{
	if (suspend) susp_write();

	ss_pause_release(ig_paused);
	ig_paused = 0;
	ig_mute_release();
	lib_state_save();
	ig_active = 0;
	unlink(CURRENT_FILE);
	printf("ClassicUI: leaving the game for Classic Home\n");
	fpga_load_rbf("menu.rbf");
}
static int ss_can_load() { return ss_get()->found_load; }

// Remembers what the pause option was, so leaving the menu restores it exactly.
static uint32_t ss_pause_prev = 0;

static int ss_pause_engage()
{
	const ss_hooks *h = ss_get();
	if (!h->found_pause) return 0;

	/*
	  A pause the core only honours while the OSD is on screen used to be a dead end here,
	  because winning it meant drawing the OSD over this UI. It is not any more: the menu
	  holds OSD_STATUS asserted with no overlay where it shows (OsdStatusHold), so such a
	  core pauses like any other and the option is simply forced on below.

	  That is worth more than the tidiness. It is what lets NES, Game Boy, GBA and Mega
	  Drive be paused properly instead of held still by a savestate - and on those cores it
	  retires freeze_engage(), which is the thing that exposes us to the SNES core's habit
	  of dying when asked for a state at a bad moment.
	*/
	if (h->pause_needs_osd) printf("ClassicUI: pausing through OSD_STATUS, held without the OSD\n");

	if (h->pause_is_option || h->pause_needs_osd)
	{
		ss_pause_prev = user_io_status_get(h->pause_opt, h->pause_ex);
		if (ss_pause_prev == h->pause_on_val) return 1;              // already paused
		user_io_status_set(h->pause_opt, h->pause_on_val, h->pause_ex);
	}
	else
	{
		ss_pulse(h->pause_opt, h->pause_ex);
	}

	printf("ClassicUI: paused the core via %s\n", h->pause_opt);
	return 1;
}

/*
  Write the core's config without our own pause in it.

  user_io_status_save() writes the whole status word, and while this menu is up that word
  carries the pause we forced on. Saved as-is it would persist a setting the player never
  chose - and being in the config, it would then apply on every later boot. So the pause is
  lifted for the write and put straight back.
*/
static void core_opts_save_unpaused()
{
	const ss_hooks *h = ss_get();
	int held = (ig_paused && h->found_pause
		&& (h->pause_is_option || h->pause_needs_osd));

	if (held) user_io_status_set(h->pause_opt, ss_pause_prev, h->pause_ex);
	core_opts_save();
	if (held) user_io_status_set(h->pause_opt, h->pause_on_val, h->pause_ex);
}

static void ss_pause_release(int engaged)
{
	const ss_hooks *h = ss_get();
	if (!engaged || !h->found_pause) return;

	/*
	  Symmetrical with engage, which is what this was not. Engage sets the option for an
	  OSD-gated pause as well as a plain one, but this only put a plain one back - so on
	  NES, Game Boy, GBA and Mega Drive the pause option was left switched on for good.
	  That is how MegaDrive.CFG came to have "Pause When OSD is Open" already set: we turned
	  it on and never turned it off. A setting the player did not choose, made permanent.
	*/
	if (h->pause_is_option || h->pause_needs_osd)
		user_io_status_set(h->pause_opt, ss_pause_prev, h->pause_ex);
	else
		ss_pulse(h->pause_opt, h->pause_ex);

	printf("ClassicUI: resumed the core\n");
}

static int ss_do_save(int slot)
{
	const ss_hooks *h = ss_get();

	// Say which half is missing: a silent no-op here is impossible to tell from a
	// core that simply has no savestates.
	if (!h->found_save) { printf("ClassicUI: this core declares no save-state entry\n"); return 0; }
	if (!ss_select_slot(slot)) { printf("ClassicUI: cannot select slot %d\n", slot + 1); return 0; }

	// Armed here rather than at the four suspend and freeze call sites, so a slot the
	// player chose by hand keeps its feedback and the reserved one never gets any.
	/*
	  Quiet for any slot this front-end drives, not just the reserved one.

	  The core announces its own states ("Save to state 4") and the firmware pops that up as
	  a classic-OSD panel over our screen. That was suppressed for the suspend slot, on the
	  grounds the player never asked for it - but the same is true of a slot they picked
	  here: the strip already shows the moment and says whether it landed, so MiSTer's own
	  notification is a second, uglier answer to a question already answered.
	*/
	ss_quiet_until = GetTimer(SS_QUIET_MS);

	/*
	  Two things have to be true for the core to actually write a file.

	  It needs clocks: a paused core never services the save, so pause comes off for
	  the pulse and goes back on afterwards. And "Savestates to SDCard" has to be On,
	  or the state stays in memory and no file appears - which looks exactly like
	  saving being broken.
	*/
	uint32_t sd_prev = 0;
	int sd_forced = 0;
	if (h->found_sd)
	{
		sd_prev = user_io_status_get(h->sd_opt, h->sd_ex);
		if (sd_prev != h->sd_on_val)
		{
			printf("ClassicUI: turning \"savestates to SD card\" on for the save\n");
			user_io_status_set(h->sd_opt, h->sd_on_val, h->sd_ex);
			sd_forced = 1;
		}
	}

	int was_paused = 0;
	if (ig_paused && h->found_pause && h->pause_is_option)
	{
		was_paused = 1;
		user_io_status_set(h->pause_opt, ss_pause_prev, h->pause_ex);
	}

	printf("ClassicUI: save state -> slot %d via %s\n", slot + 1, h->save_opt);
	ss_pulse(h->save_opt, h->save_ex);

	if (was_paused) user_io_status_set(h->pause_opt, h->pause_on_val, h->pause_ex);
	if (sd_forced) user_io_status_set(h->sd_opt, sd_prev, h->sd_ex);
	return 1;
}

static int ss_do_load(int slot)
{
	const ss_hooks *h = ss_get();
	if (!h->found_load || !ss_select_slot(slot)) return 0;

	/*
	  Quiet for any slot this front-end drives, not just the reserved one.

	  The core announces its own states ("Save to state 4") and the firmware pops that up as
	  a classic-OSD panel over our screen. That was suppressed for the suspend slot, on the
	  grounds the player never asked for it - but the same is true of a slot they picked
	  here: the strip already shows the moment and says whether it landed, so MiSTer's own
	  notification is a second, uglier answer to a question already answered.
	*/
	ss_quiet_until = GetTimer(SS_QUIET_MS);

	printf("ClassicUI: restore state <- slot %d\n", slot + 1);
	ss_pulse(h->load_opt, h->load_ex);
	return 1;
}

/* ------------------------------------------------------- in-game screens --- */

// Identity of the running game, written by do_launch() before the core switch.
/*
  What is running, according to the launch that started it - and only if that is
  still true. A record naming a core other than the one loaded is stale: something
  else changed cores since, so it is dropped rather than believed.
*/
static int cur_read(char *sysid, int syslen, char *rompath, int pathlen)
{
	FILE *f = fopen(CURRENT_FILE, "rt");
	if (!f) return 0;

	char core[64] = {};
	int ok = (fgets(sysid, syslen, f) && fgets(rompath, pathlen, f));
	int have_core = (fgets(core, sizeof(core), f) != 0);
	fclose(f);
	if (!ok) return 0;

	for (char *q = sysid; *q; q++) if (*q == '\n') { *q = 0; break; }
	for (char *q = rompath; *q; q++) if (*q == '\n') { *q = 0; break; }
	for (char *q = core; *q; q++) if (*q == '\n') { *q = 0; break; }
	if (!sysid[0] || !rompath[0]) return 0;

	if (have_core && core[0] && strcmp(core, "*"))
	{
		const char *running = user_io_get_core_name();
		if (running && running[0] && strcasecmp(running, core))
		{
			printf("ClassicUI: ignoring a launch record for %s while %s is running\n", core, running);
			unlink(CURRENT_FILE);
			return 0;
		}
	}
	return 1;
}

static int ig_load_item()
{
	ig_have_item = 0;

	char sysid[64] = {}, rompath[CH_PATH_LEN] = {};
	if (!cur_read(sysid, sizeof(sysid), rompath, sizeof(rompath))) return 0;

	lib_load_systems();

	int sysidx = -1;
	for (int i = 0; i < lib_sys_count(); i++)
	{
		if (!strcasecmp(lib_sys(i)->id, sysid)) { sysidx = i; break; }
	}
	if (sysidx < 0) return 0;

	memset(&ig_item, 0, sizeof(ig_item));
	ig_item.kind = IT_GAME;
	ig_item.sysidx = (int16_t)sysidx;
	snprintf(ig_item.path, sizeof(ig_item.path), "%s", rompath);

	const char *fn = strrchr(rompath, '/');
	fn = fn ? fn + 1 : rompath;
	snprintf(ig_item.title, sizeof(ig_item.title), "%s", fn);
	char *dot = strrchr(ig_item.title, '.');
	if (dot) *dot = 0;

	lib_refresh_slots(&ig_item);

	ig_have_item = 1;
	return 1;
}

/*
  The menu-bar label for the core entry: the library's badge for the running system, which
  is already sized for tight places, and the word "Core" only if we have nothing better.
*/
static const char *mb_text(int i)
{
	if (i != MB_CORE) return mb_label[i];

	const char *n = core_short_name(ig_have_item ? ig_item.sysidx : -1);
	return (n && *n) ? n : mb_label[MB_CORE];
}

/*
  The shape the captured frame is meant to be shown in, which is not the shape of its
  pixels.

  screenshot_grab() hands back the core's native frame, and those pixels are not square:
  PSX gave 352x239 and the 240p cores 640x240 - ratios of 1.47 and 2.67 for two pictures
  that are both 4:3 on the television. Fitting by the pixel ratio is what squashed the menu
  background into a letterboxed band, and squashed the suspend thumbnails with it.

  So the fit is done against the display aspect instead. 4:3 is assumed, which is right for
  every console core here and is what MiSTer's own "Original" aspect gives them. Handhelds
  are the known exception - a GBA panel is 3:2 and a Game Boy 10:9 - and if one of those
  looks wrong on a real screen this is the line to revisit; it is a deliberate assumption,
  not an oversight.
*/
// Fits SHOT_AR_W:SHOT_AR_H inside w x h, centred, and reports where it landed.
static void shot_fit(int w, int h, int *fw, int *fh, int *ox, int *oy)
{
	int aw, ah;
	if ((long long)SHOT_AR_W * h > (long long)SHOT_AR_H * w)
	{
		aw = w;
		ah = (int)((long long)w * SHOT_AR_H / SHOT_AR_W);
	}
	else
	{
		ah = h;
		aw = (int)((long long)h * SHOT_AR_W / SHOT_AR_H);
	}
	if (aw < 1) aw = 1;
	if (ah < 1) ah = 1;

	*fw = aw;
	*fh = ah;
	if (ox) *ox = (w - aw) / 2;
	if (oy) *oy = (h - ah) / 2;
}

// The live frame, resampled to a requested size and cached, for look previews.
static const uint32_t *ig_live_ref(int w, int h)
{
	static uint32_t *buf = 0;
	static int bw = 0, bh = 0;

	if (!ig_shot || w < 1 || h < 1) return 0;
	if (buf && bw == w && bh == h) return buf;

	free(buf);
	buf = (uint32_t*)malloc((size_t)w * h * 4);
	if (!buf) { bw = bh = 0; return 0; }
	bw = w; bh = h;

	// The middle of the frame, magnified - see VP_ZOOM_PCT.
	int rw = ig_shot_w * VP_ZOOM_PCT / 100;
	int rh = ig_shot_h * VP_ZOOM_PCT / 100;
	if (rw < 8) rw = ig_shot_w;
	if (rh < 8) rh = ig_shot_h;
	int ox = (ig_shot_w - rw) / 2;
	int oy = (ig_shot_h - rh) / 2;

	for (int y = 0; y < h; y++)
	{
		int sy = oy + (y * rh) / h;
		const uint32_t *srow = ig_shot + (size_t)sy * ig_shot_w;
		uint32_t *drow = buf + (size_t)y * w;
		for (int x = 0; x < w; x++) drow[x] = srow[ox + (x * rw) / w] | 0xff000000u;
	}

	return buf;
}

// Capture scaled to the canvas and dimmed, built once when the menu opens.
static void ig_build_background(const chome_profile *p)
{
	free(ig_bg);
	ig_bg = 0;
	ig_bg_w = ig_bg_h = 0;

	if (!ig_shot) return;

	ig_bg = (uint32_t*)malloc((size_t)p->w * p->h * 4);
	if (!ig_bg) return;
	ig_bg_w = p->w;
	ig_bg_h = p->h;

	/*
	  Fit, not stretch: a 4:3 core on a 16:9 canvas keeps its shape, with the
	  surround left black. Then dim, so panel text stays readable over anything.
	*/
	int fw, fh;
	shot_fit(p->w, p->h, &fw, &fh, 0, 0);

	int ox = (p->w - fw) / 2, oy = (p->h - fh) / 2;

	for (int i = 0; i < p->w * p->h; i++) ig_bg[i] = COL_BLACK;

	for (int y = 0; y < fh; y++)
	{
		int sy = (y * ig_shot_h) / fh;
		const uint32_t *srow = ig_shot + (size_t)sy * ig_shot_w;
		uint32_t *drow = ig_bg + (size_t)(oy + y) * p->w + ox;

		for (int x = 0; x < fw; x++)
		{
			uint32_t c = srow[(x * ig_shot_w) / fw];
			// Dim to about a third: the game stays recognisable, the menu readable.
			uint32_t r = ((c >> 16) & 0xff) * 5 / 16;
			uint32_t g = ((c >> 8) & 0xff) * 5 / 16;
			uint32_t b = (c & 0xff) * 5 / 16;
			drow[x] = 0xff000000u | (r << 16) | (g << 8) | b;
		}
	}
}

static void ig_close(int restore_video)
{
	if (!ig_active) return;
	ig_active = 0;

	ss_pause_release(ig_paused);
	ig_paused = 0;

	// Only when going back into the game: quitting keeps the state as the suspend
	// point instead, and loading a different slot has already moved things on.
	if (restore_video) freeze_release(ig_frozen);
	ig_mute_release();            // after the reload, so its audio glitch is not heard
	ig_frozen = 0;
	ig_selected_running = 0;

	free(ig_shot); ig_shot = 0; ig_shot_w = ig_shot_h = 0;
	free(ig_bg);   ig_bg = 0;   ig_bg_w = ig_bg_h = 0;

	OsdDisable();                 // keyboard back to the game

	// Release the analog output unconditionally, even when the framebuffer is being
	// left in place for a core switch: whatever runs next brings its own mode, and
	// leaving the mux pointed at the scaler would strand it.
	video_menu_fb_analog(0);
	if (restore_video) video_fb_enable(0);

	printf("ClassicUI: pause menu closed\n");
}

/*
  Park the shelf on the game that is running, so opening the menu lands where the
  player already is. The index may still be scanning, so this is retried until it
  succeeds or the scan finishes.
*/
static void ig_select_running()
{
	if (!ig_have_item || ig_selected_running) return;

	/*
	  Through the view, so the running game is found even when it is not the file its
	  card is showing - and the card is turned to it. Comparing only against each card's
	  selected file would leave the shelf parked somewhere else entirely whenever the
	  player started the second dump of a title.
	*/
	int at = lib_view_select_path(ig_item.sysidx, ig_item.path);
	if (at < 0) return;

	sel = at;
	selF = at;
	ig_selected_running = 1;
	mark_dirty();
}

// Returns 1 when the menu took over, 0 when the core cannot host it.
static int ig_open()
{
	const chome_profile *p;

	theme_update(video_menu_fb_width(), video_menu_fb_height(), cfg.classicui_profile);
	p = theme_get();
	if (p->w < 8 || p->h < 8) return 0;

	// Grab the running frame before we take the screen away from the core.
	int max_px = 2048 * 1024;
	free(ig_shot);
	ig_shot = (uint32_t*)malloc((size_t)max_px * 4);
	if (ig_shot && !screenshot_grab(ig_shot, max_px, &ig_shot_w, &ig_shot_h))
	{
		free(ig_shot);
		ig_shot = 0;
		ig_shot_w = ig_shot_h = 0;
	}

	/*
	  Opaque, once, here. The scaler hands back ARGB with nothing meaningful in the alpha
	  byte, and the drawing paths each worked around that by or-ing 0xff000000 in as they
	  read - so the menu background looked right while ss_write_thumb, which passes the
	  buffer straight to imlib2, wrote a fully transparent image. That is the black tile
	  Derek found on a PSX suspend point. Fixing it at the source means every consumer gets
	  a valid frame instead of each having to remember.
	*/
	if (ig_shot)
	{
		size_t n = (size_t)ig_shot_w * ig_shot_h;
		for (size_t i = 0; i < n; i++) ig_shot[i] |= 0xff000000u;
	}

	if (!gfx_begin()) { free(ig_shot); ig_shot = 0; return 0; }

	// Take the framebuffer. A core without support leaves us nothing to draw on.
	if (!video_menu_fb_present(ig_fb))
	{
		printf("ClassicUI: core has no HPS framebuffer, leaving the OSD to it\n");
		free(ig_shot);
		ig_shot = 0;
		return 0;
	}

	/*
	  With the framebuffer now ours, an analog-only setup still needs the scaler
	  routed to the analog port or none of this is on screen. That resizes the
	  canvas, so re-measure and re-take the compose buffer before the still of the
	  game is scaled into it.
	*/
	video_menu_fb_analog(1);
	theme_update(video_menu_fb_width(), video_menu_fb_height(), cfg.classicui_profile);
	p = theme_get();
	if (p->w < 8 || p->h < 8 || !gfx_begin())
	{
		video_menu_fb_analog(0);
		free(ig_shot);
		ig_shot = 0;
		return 0;
	}

	// As in the menu core: input keeps arriving and the overlay ends up off, because
	// it draws over this UI rather than under it.
	/*
	  OSD_STATUS held, no overlay drawn where it would be seen. This is what a core that
	  only pauses "while the OSD is open" needs - MiSTer has no pause command, that signal
	  is the pause, and the old pair here (OsdEnable then OsdMenuCtl(0)) raised it and threw
	  it straight back away. See OsdStatusHold().
	*/
	OsdStatusHold(1);

	ig_mute_engage();             // before the freeze state, which takes a moment to write

	ss_hk_valid = 0;              // re-read CONF_STR: it may not have been ready before

	/*
	  And read the core's own options, because the menu bar has to know whether there are
	  any before it can decide whether to show an entry for them. Doing it lazily when the
	  entry is opened cannot work: the entry would never appear to be opened.
	*/
	core_opts_scan();

	ig_load_item();

	/*
	  Whose per-game core settings the options screen edits, if anyone's.

	  ig_have_item is the whole test, and it is the honest one: it is set only when the
	  launch record names a game and still names the core that is loaded. A core somebody
	  else started - the classic menu, a script, a bootcore - leaves it clear, and then
	  there is no game to hang a setting on, so a change made on that screen goes into the
	  core's own config as it always did.
	*/
	core_opts_bind_game(ig_have_item ? core_opts_game_key(ig_item.sysidx, ig_item.path) : 0);

	ig_build_background(p);

	/*
	  The whole front-end runs here, not a cut-down pause panel: the library, art
	  and video layers have no dependency on the menu core. The index is built the
	  same way it is there, sliced across frames, so the first open after a core
	  switch shows "scanning" briefly and later opens are instant.
	*/
	if (!inited)
	{
		lib_init();
		vp_install();
		inited = 1;
	}

	/*
	  Where the player was, on the first open in this core. Launching re-execs MiSTer,
	  so the shelf statics here start at their defaults - the unfiltered root - and the
	  view the game was started from only exists in the session record. Without this
	  the menu came back on the root shelf with the running game selected, because
	  ig_select_running() below searches whatever view happens to be built and parks on
	  the game wherever it finds it: the selection looks right, so the wrong view is the
	  only symptom. Later opens keep what is in memory, since by then it is the player's
	  own browsing.
	*/
	if (ig_first_open)
	{
		ig_first_open = 0;
		if (!session_restore()) view_rebuild(1);
	}
	else view_rebuild(1);

	art_init(theme_get()->sel_w, theme_get()->sel_h);

	ig_paused = ss_pause_engage();
	ig_frozen = freeze_engage();
	ig_freeze_at = ig_frozen ? (unsigned long)time(0) : 0;

	ig_active = 1;
	screen = SCR_HOME;
	slot_idx = 0;
	del_arm_slot = -1;
	/*
	  A save that gave up marks its slot NOT SAVED, which is worth seeing once and not
	  worth seeing forever: it is only a slot number, so without this it would still be
	  there on the next visit, and on a different game's strip at that.
	*/
	pend_failed = -1;
	ig_close_until = 0;
	bar_y = 0;
	strip_y = 0;

	ig_select_running();

	gfx_damage_all();
	mark_dirty();                 // damage alone does not schedule a draw
	printf("ClassicUI: menu open over %s\n", ig_have_item ? ig_item.title : "the running game");
	return 1;
}

/* --------------------------------------------------------------- driver --- */

int chome_enabled()
{
	return cfg.classicui && is_menu();
}

/*
  Runs in every core, not just the menu. When a game was launched from Classic
  Home we left a video look armed for it; the core has booted by now, so apply it
  and forget it. video_loadPreset(save=true) makes it stick for this core.
*/
/*
  One reference capture per game. The delay is a heuristic: long enough to be past
  boot logos and into something representative, short enough that a quick session
  still gets one. Only written when missing, to spare the SD card.
*/
/*
  Resume, in the core that has just booted. The ROM has to be in before a state can
  go back on top of it, so this waits like the reference-frame capture does, then
  restores and drops the marker - once, whether it worked or not, so a core that
  cannot restore does not sit here retrying forever.
*/
static void resume_poll()
{
	static int done = 0;
	static unsigned long due = 0;

	if (done) return;

	if (!due) { due = GetTimer(RESUME_DELAY_MS); return; }
	if (!CheckTimer(due)) return;
	done = 1;

	char sysid[64] = {}, relpath[CH_PATH_LEN] = {};
	int slot = 0;
	if (!susp_read(sysid, sizeof(sysid), relpath, sizeof(relpath), &slot)) return;

	// Only for the game this core actually booted.
	char cur_sys[64] = {}, cur_path[CH_PATH_LEN] = {};
	if (!cur_read(cur_sys, sizeof(cur_sys), cur_path, sizeof(cur_path))) return;
	if (strcmp(cur_sys, sysid) || strcmp(cur_path, relpath)) return;

	if (!ss_hk_valid) ss_scan_hooks();

	if (ss_do_load(slot)) printf("ClassicUI: resumed %s from its suspend point\n", relpath);
	else printf("ClassicUI: could not resume %s\n", relpath);

	susp_clear();
}

void chome_core_poll()
{
	static int done = 0;
	static unsigned long due = 0;

	if (!cfg.classicui || is_menu()) return;

	// Before the early returns below: a registered save has to finish even in a core
	// whose reference frame was captured long ago.
	chome_pend_poll();

	resume_poll();

	if (done) return;

	if (!due)
	{
		due = GetTimer(REF_DELAY_MS);
		return;
	}
	if (!CheckTimer(due)) return;

	done = 1;

	char sysid[64] = {}, rompath[CH_PATH_LEN] = {};
	if (!cur_read(sysid, sizeof(sysid), rompath, sizeof(rompath))) return;

	char path[1024];
	ref_shot_path(sysid, rompath, path, sizeof(path));

	struct stat st;
	if (!stat(path, &st)) return;         // already have one

	// mkdir -p on the two levels we own
	char dir[1024];
	snprintf(dir, sizeof(dir), "%s/classicui", getRootDir());
	mkdir(dir, 0777);
	snprintf(dir, sizeof(dir), "%s/classicui/refshots", getRootDir());
	mkdir(dir, 0777);
	snprintf(dir, sizeof(dir), "%s/classicui/refshots/%s", getRootDir(), sysid);
	mkdir(dir, 0777);

	printf("ClassicUI: capturing a preview reference frame -> %s\n", path);
	screenshot_thumbnail(path, 480);
}

void chome_core_boot()
{
	static int done = 0;
	if (done) return;
	done = 1;

	if (!cfg.classicui || is_menu()) return;
	vp_apply_pending();

	/*
	  Tell MiSTer's own recents about this launch.

	  A user's point, and a fair one: the firmware has had a recents list since long before
	  this front-end - recent.cpp, config/<CORE>_recent_<idx>.cfg - and a game started from
	  here never appeared in it. Anything else that reads it, including MiSTer's own file
	  browser, was blind to everything the player actually played.

	  Here rather than in do_launch(), which is the obvious place and the wrong one: the
	  record is named after user_io_get_core_name(), and at launch time that is still MENU.
	  By the time this runs the core is up and answers for itself.

	  Our own Recently Played is a different shape and stays - MiSTer's is per core, capped
	  at sixteen, and has no ordering across cores, so it cannot answer "what did I play
	  last" for a shelf that spans systems. This is interoperability, not a replacement.

	  cfg.recents gates it inside recent_update(), so a player who has the feature switched
	  off gets nothing written, which is what they asked for.
	*/
	{
		char sysid[64] = {}, rel[CH_PATH_LEN] = {};
		if (cur_read(sysid, sizeof(sysid), rel, sizeof(rel)))
		{
			const chome_sys *sy = 0;
			for (int i = 0; i < lib_sys_count() && !sy; i++)
			{
				const chome_sys *c = lib_sys(i);
				if (c && !strcasecmp(c->id, sysid)) sy = c;
			}

			if (sy)
			{
				/*
				  Split as MiSTer stores it: the directory it was found in and the file's own
				  name. rel is relative to the system's games folder and may name a member
				  inside an archive, so the last slash is the wrong split for a zip - the
				  archive is the file, exactly as it is for the shelf.
				*/
				char full[CH_PATH_LEN + 64];
				snprintf(full, sizeof(full), "%s/%s", sy->dir, rel);

				const char *zip = strcasestr(full, ".zip/");
				const char *end = zip ? zip + 4 : full + strlen(full);
				char *slash = 0;
				for (char *q = full; q < full + (end - full); q++) if (*q == '/') slash = q;

				char dir[CH_PATH_LEN + 64] = {};
				char name[CH_PATH_LEN] = {};
				if (slash)
				{
					snprintf(dir, sizeof(dir), "%.*s", (int)(slash - full), full);
					snprintf(name, sizeof(name), "%s", slash + 1);
				}
				else snprintf(name, sizeof(name), "%s", full);

				char label[CH_PATH_LEN] = {};
				snprintf(label, sizeof(label), "%s", name);
				char *dot = strrchr(label, '.');
				if (dot) *dot = 0;

				recent_update(dir, name, label, 0);
				printf("ClassicUI: told MiSTer's recents about %s\n", name);
			}
		}
	}

	/*
	  And the core settings this game keeps to itself.

	  Here rather than later because here is where the values are still the core's own:
	  the firmware has just loaded <CORE>.CFG into the status word and nothing has
	  touched it since, so the value each override replaces is the shared one and can be
	  recorded as such. It is also before the MGL's delay has expired, which means before
	  the ROM is handed over - the right side of every option that only takes effect at
	  load time.

	  Nothing is written to <CORE>.CFG: the overrides live in our own file, so a game
	  with none of them boots with exactly the core's own settings and the next game on
	  the same core is unaffected by this one.
	*/
	if (!ig_load_item()) return;
	core_opts_apply_for_game(ig_item.sysidx, ig_item.path);
}

int chome_screen_id() { return screen; }

int chome_active()
{
	return active || ig_active;
}

void chome_text_entry(const char *title, const char *prompt, const char *initial, int mask)
{
	osk_open(title, prompt, initial, mask);
	mark_dirty();
}

int chome_ingame_active()
{
	return ig_active;
}

void chome_leave()
{
	if (!active) return;
	active = 0;
	handed_off = 1;
	printf("ClassicUI: handing off to the classic menu (OSD button returns)\n");
	session_save();

	// Give the analog output back: the classic menu is drawn by the core, not into
	// the framebuffer, so holding the scaler would leave it invisible instead.
	video_menu_fb_analog(0);

	lib_state_save();
	net_watch(0);
	disc_watch_stop();
	OsdMenuCtl(1);            // OSD overlay back on for the classic menu

	// Repaint the wallpaper over our UI: the classic menu only draws the
	// background on demand, so our last frame would otherwise stay behind it.
	video_menu_bg(user_io_status_get("[3:1]"));
	gfx_damage_all();
}

static void enter()
{
	if (!inited)
	{
		lib_init();
		vp_install();
		inited = 1;
	}

	active = 1;
	screen = SCR_HOME;
	dirty = 1;
	last_ms = GetTimer(0);

	/*
	  Keep osd_is_visible set so keyboard and gamepad input keeps arriving here
	  (input.cpp routes pad buttons to the menu only when the OSD is "visible"), then
	  switch the overlay itself off.

	  The overlay has to end up off: the classic OSD is composited over the
	  framebuffer, not replaced by it - that is how it appears over the menu core's
	  wallpaper - so leaving it enabled paints stale OSD rows on top of this UI.
	  Which is a shame, because OSD_CMD_ENABLE is also what sys_top turns into
	  OSD_STATUS, and that is the only thing a core's "pause when the OSD is open"
	  option watches. The two cannot both be had: see ss_pause_engage().
	*/
	/*
	  OSD_STATUS held, no overlay drawn where it would be seen. This is what a core that
	  only pauses "while the OSD is open" needs - MiSTer has no pause command, that signal
	  is the pause, and the old pair here (OsdEnable then OsdMenuCtl(0)) raised it and threw
	  it straight back away. See OsdStatusHold().
	*/
	OsdStatusHold(1);

	// On an analog-only setup the framebuffer reaches no screen until the scaler
	// output is routed there. Ask before measuring: this resizes the framebuffer to
	// the TV mode, and the theme profile follows whatever canvas it is handed.
	video_menu_fb_analog(1);

	theme_update(video_menu_fb_width(), video_menu_fb_height(), cfg.classicui_profile);

	/*
	  First entry after a boot or a core switch picks the session up where it was
	  left; later entries are a handoff to the classic menu and back, where the view
	  is still in memory and only the shelf position needs keeping.
	*/
	if (first_entry)
	{
		first_entry = 0;
		if (!session_restore()) view_rebuild(0);
	}
	else view_rebuild(1);

	art_init(theme_get()->sel_w, theme_get()->sel_h);
}

/*
  Is there a job running that the player is waiting on, on the screen they are looking
  at? Only these get an animated indicator, and only while they are really in flight -
  an indicator that spins whenever a screen is open teaches people to ignore it, and
  then it cannot do the one job it has, which is to say "this has not hung".
*/
static int ui_busy()
{
	if (screen == SCR_WIFI) return net_scanning() || net_join_state() == JOIN_WORK;

	if (screen == SCR_PADS)
	{
		int st = bt_pair_state();
		return bt_pairing() || st == BTP_LOOKING || st == BTP_WORKING || st == BTP_PIN;
	}

	return 0;
}

static void animate()
{
	unsigned long now = GetTimer(0);
	unsigned long dt = now - last_ms;
	last_ms = now;
	if (dt > 200) dt = 200;

	double k = dt / 90.0;
	if (k > 1) k = 1;

	double d = sel - selF;
	if (d < -0.003 || d > 0.003)
	{
		selF += d * (k * 2.2 > 1 ? 1 : k * 2.2);
		mark_dirty();
	}
	else selF = sel;

	double bt = (screen == SCR_MENUBAR || screen == SCR_SORT || screen == SCR_DISPLAY ||
		screen == SCR_OPTIONS || screen == SCR_ABOUT) ? 1 : 0;
	if (bar_y != bt)
	{
		bar_y += (bt - bar_y) * (k * 2.5 > 1 ? 1 : k * 2.5);
		if (bar_y > 0.998) bar_y = 1;
		if (bar_y < 0.002) bar_y = 0;
		mark_dirty();
	}

	double st = (screen == SCR_SUSPEND) ? 1 : 0;
	if (strip_y != st)
	{
		strip_y += (st - strip_y) * (k * 2.5 > 1 ? 1 : k * 2.5);
		if (strip_y > 0.998) strip_y = 1;
		if (strip_y < 0.002) strip_y = 0;
		mark_dirty();
	}

	if (screen == SCR_LAUNCH)
	{
		curtain += k * 0.6;
		if (curtain > 1) curtain = 1;
		mark_dirty();
		if (GetTimer(0) - launch_at > 900)
		{
			// Return home first: a launch that cannot proceed must not re-fire
			// on every frame.
			screen = SCR_HOME;
			curtain = 0;
			launch_selected();
			return;
		}
	}
	else curtain = 0;

	if (!CheckTimer(nudge_until)) mark_dirty();
	if (!CheckTimer(ig_close_until)) mark_dirty();

	/*
	  Keep the activity indicators turning - and only while something is really turning
	  them. Everything folded in below is a job that is running right now in a forked
	  child: a Wi-Fi scan, a join, a pairing conversation. Nothing here spins because a
	  screen is open, so a screen with nothing happening on it still costs nothing.

	  Throttled to the rate the ring actually moves rather than to the frame rate. The
	  ring has eight positions and a repaint of this UI is a full compose and a blit, so
	  painting between positions is work with nothing on the end of it - which matters
	  most for a pairing, where this can run for a minute.
	*/
	if (ui_busy())
	{
		unsigned long ph = GetTimer(0) / GFX_SPIN_MS;
		if (ph != anim_seen) { anim_seen = ph; mark_dirty(); }
	}
}

/*
  The classic menu opens on the *release* of the menu button, not the press
  (menu.cpp: case KEY_F12 | UPSTROKE), and it only skips that when the press before
  it was the one that opened it. So a press this front-end took for itself leaves
  its release behind to open the classic OSD - which is what closing the in-game
  menu did: back into the game, with MiSTer's own menu on top of it.

  Set when a menu-button press is consumed inside a game core, so the release that
  belongs to it can be consumed as well.
*/
static int eat_menu_release = 0;

int chome_handle(uint32_t key)
{
	uint32_t igk = key & ~UPSTROKE;
	int igpress = key && !(key & UPSTROKE);
	int igmenu = (igk == KEY_MENU || igk == KEY_F12);
	int was_ingame = ig_active;

	if (eat_menu_release && igmenu && (key & UPSTROKE))
	{
		eat_menu_release = 0;
		return 1;
	}

	/*
	  A menu-button press while the previous one's release is still owed can only be the
	  auto-repeat of the same physical press, so it is not a second thing the player did.

	  menu.cpp's menu_key_get() repeats a held key once its repeat timer expires, and one
	  of the conditions for repeating is chome_active() - true the moment this menu opens.
	  Opening it is not quick (a screenshot, the framebuffer, a video mode change, the
	  freeze state), so the repeat lands immediately afterwards, sees the menu up, and
	  closes what the press just opened. On the device that was "it takes two presses to
	  open the menu, and the first press flashes something for a split second".

	  Why it only happened after closing with B or by loading a slot: those closes never
	  deliver their own key's release to the menu, so menu_key stays latched on a press and
	  the repeat timer is left expired - armed to fire on the very next key. Closing with
	  the menu button delivers an UPSTROKE, which resets that timer. The deeper fault is
	  the swallowed release in user_io.cpp; this guard is the part that can be fixed
	  without touching the keyboard routing every core shares.

	  Both directions. A first attempt only ignored a repeat while the menu was up, which
	  fixed opening and broke closing: the repeat then arrived just after the close, found
	  no menu, and opened it again - so going back to the game needed two presses too, and
	  at REPEATRATE the open/close churn left the menu black and dead. The real repair is
	  in menu.cpp, which no longer repeats the menu button at all; this stays as the layer
	  that can be tested, and because one duplicate press must never toggle twice.

	  Safe against a stuck flag: user_io.cpp sends KEY_F12|UPSTROKE for a menu event
	  whatever happened to the press, which the trace confirms - it arrives even after the
	  menu has closed.
	*/
	if (eat_menu_release && igmenu && igpress) return 1;

	/*
	  Inside a game core the whole front-end runs, not a cut-down pause panel: the
	  shelf, folders, Display, Options, everything, drawn over a still of the
	  running game. Taking the HPS framebuffer also routes pad input here
	  (input.cpp gates on chome_active()); the OSD stays enabled but blanked so
	  the keyboard reaches us instead of the game.

	  From there it shares the menu core's frame loop below - same screens, same
	  keys - with only the differences that being inside a game implies.
	*/
	if (cfg.classicui && !is_menu())
	{
		if (!ig_active)
		{
			/*
			  While the screen has been handed to the core's own options the button belongs
			  to the classic menu - that is how the player closes it. Taking it back would
			  trap them in there with no way out but a reset.

			  The handoff ends when the classic menu is no longer showing anything, and only
			  then: watching menu_present() alone is what made this wrong before, because it
			  goes true for a moment after a savestate load as well.
			*/
			/*
			  Two steps, because one is not enough: on the frame after the handoff the
			  classic menu has not opened yet, so a single "clear when it is not present"
			  clears immediately and we take the very key we were standing down for.

			  The deadline matters as much as the states. If the OSD never appears - a core
			  that will not show it, or a key that goes nowhere - then without a timeout the
			  flag stays up and our menu becomes unreachable, which is exactly the trap this
			  is meant to prevent rather than create.
			*/
			if (osd_handoff == OSDH_WAITING)
			{
				if (menu_present()) osd_handoff = OSDH_UP;
				else if (CheckTimer(osd_handoff_until))
				{
					printf("ClassicUI: the core's options never opened, taking the screen back\n");
					osd_handoff = OSDH_NONE;
				}
			}
			else if (osd_handoff == OSDH_UP && !menu_present())
			{
				osd_handoff = OSDH_NONE;
			}

			if (igpress && igmenu && !osd_handoff)
			{
				eat_menu_release = 1;
				if (ig_open()) return 1;

				/*
				  No framebuffer in this core, so there is nowhere to draw the menu.
				  Put the game away and go back to Classic Home rather than opening the
				  classic OSD: the front-end is the only menu the player should meet,
				  and the suspend point means the game is still there afterwards.
				*/
				quit_to_home(1);
				return 1;
			}
			return 0;
		}
	}
	else
	{
		if (!chome_enabled())
		{
			if (active) active = 0;
			return 0;
		}

		// Yield while the fb terminal owns the framebuffer (F9 console, scripts).
		// It has its own claim on the analog output, so drop ours rather than
		// fight over the video mode.
		if (video_fb_state())
		{
			active = 0;
			video_menu_fb_analog(0);
			return 0;
		}
	}

	uint32_t k = key & ~UPSTROKE;
	int press = key && !(key & UPSTROKE);

	if (!active && !ig_active)
	{
		// After a handoff the classic menu has the screen; the OSD/menu button
		// brings us back. On first entry we simply take over.
		if (handed_off)
		{
			if (press && (k == KEY_MENU || k == KEY_F12))
			{
				handed_off = 0;
				enter();
				return 1;
			}
			return 0;
		}

		enter();
		if (!active) return 0;

		// Taking over consumes this frame: otherwise a first input of the menu
		// button would enter and immediately hand back out again.
		key = 0;
		k = 0;
		press = 0;
	}

	if (press)
	{
		// Relabel the prompts for whichever device this came from.
		int pad = input_menu_key_from_pad();
		if (pad != using_pad) { using_pad = pad; mark_dirty(); }

		/*
		  The keyboard is modal: while it is up every key belongs to it, including
		  MENU, which cancels the entry rather than closing the front-end.
		*/
		if (osk_active())
		{
			osk_key(k, pad);
			osk_settle();
			mark_dirty();
			return 1;
		}

		if (k == last_key) key_run++;
		else { key_run = 0; last_key = k; }

		/*
		  The tester is modal for the same reason the keyboard above it is: while it is up
		  every button on the pad is the thing being tested, so none of them may also mean
		  something. Intercepted here rather than case by case - move_h()'s default: moves
		  the shelf behind the panel, which is the trap every modal screen here has hit.
		*/
		if (screen == SCR_PADTEST)
		{
			padtest_key(k);
			return 1;
		}

		switch (k)
		{
		case KEY_LEFT:  move_h(-1); break;
		case KEY_RIGHT: move_h(1); break;
		case KEY_UP:    move_v(-1); break;
		case KEY_DOWN:  move_v(1); break;

		case KEY_ENTER:
		case KEY_KPENTER:
		case KEY_SPACE:
			accept();
			break;

		case KEY_BACK:
		case KEY_ESC:
			back();
			break;

		case KEY_BACKSPACE:      // pad Y
		{
			chome_item *it = cur_game();

			if (screen == SCR_SUSPEND && ig_is_running(it) && ss_can_save())
			{
				if (slot_state(it, slot_idx) == 2) { nudge(); break; }   // locked
				if (pend_slot >= 0) { nudge(); break; }                  // one at a time

				// Already on disk: a memory copy, a file copy and a picture.
				if (ss_copy_state(it, slot_idx))
				{
					ss_write_thumb(it, slot_idx);
					lib_refresh_slots(it);
					mark_dirty();
					break;
				}

				/*
				  Not yet - the core writes the held state on its own schedule. Register
				  the request and let the slot say so, rather than making the player wait
				  for something they have no way of knowing about.
				*/
				if (ig_frozen) { pend_start(it, slot_idx); mark_dirty(); break; }

				/*
				  And there are cores that never froze, because they pause properly. For
				  those no held state is ever coming, so the core has to be asked - which
				  means letting it run. That can be done with the menu still up (see
				  pend_direct_start); only if even that fails is the game resumed.
				*/
				if (ig_paused && pend_direct_start(it, slot_idx)) { mark_dirty(); break; }

				if (ss_do_save(slot_idx)) { ig_frozen = 0; ig_close(1); }
				else nudge();
				break;
			}

			if (screen == SCR_PADS)
			{
				if (bt_pairing() || bt_pair_state() != BTP_IDLE) { nudge(); break; }

				pad_row sel;
				if (!pads_sel(&sel) || sel.kind != PAD_BT || sel.connected) { nudge(); break; }

				bt_connect(sel.mac);
				mark_dirty();
				break;
			}

			if (screen == SCR_HOME && it) { lib_toggle_fav(it); mark_dirty(); }
			else nudge();
			break;
		}

		case KEY_GRAVE:          // pad Select
			if (screen == SCR_HOME) { sort_idx = sort_mode; go_screen(SCR_SORT); }
			else nudge();
			break;

		case KEY_TAB:            // pad X
		{
			if (screen == SCR_WIFI)
			{
				if (net_join_state() != JOIN_IDLE || net_scanning()) { nudge(); break; }
				net_scan_start();
				mark_dirty();
				break;
			}

			/*
			  Forgetting a controller means it stops working until it is paired again, so
			  it takes two presses like deleting a suspend point does.
			*/
			if (screen == SCR_PADS)
			{
				if (bt_pairing() || bt_pair_state() != BTP_IDLE) { nudge(); break; }

				pad_row sel;
				if (!pads_sel(&sel) || sel.kind != PAD_BT) { nudge(); break; }

				if (pads_forget_arm == pads_row && !CheckTimer(pads_forget_until))
				{
					pads_forget_arm = -1;
					bt_forget(sel.mac);
					if (pads_row > 0) pads_row--;
				}
				else
				{
					pads_forget_arm = pads_row;
					pads_forget_until = GetTimer(2500);
				}
				mark_dirty();
				break;
			}

			/*
			  X gives a per-game core setting back to every game: it drops the override,
			  puts the core's own value on the core and clears the star, so the row
			  visibly moves. An override that could not be undone from the screen that
			  made it would be a trap - the player would have to know which file it
			  lived in.

			  On a row with no star there is nothing to undo, so it nudges rather than
			  doing something invisible. The legend only offers X where it acts.
			*/
			if (screen == SCR_CORE)
			{
				if (co_row >= core_opts_tier_count(co_tier)) { nudge(); break; }

				const core_opt *o = core_opt_tier_at(co_tier, co_row);
				if (!o || !core_opt_drop_for_game(o)) { nudge(); break; }

				// Same reason as changing a value: the core recomputes its own mask.
				core_opts_scan();
				if (co_row >= co_rows()) co_row = co_rows() - 1;
				mark_dirty();
				break;
			}

			/*
			  X puts a setting back to the value this menu recommends, which is what the
			  amber on the row is pointing at. Without it, undoing a value somebody had
			  already put in the file means knowing what the default was.
			*/
			if (screen == SCR_SET)
			{
				if (set_row >= set_nview || !opt_reset(set_view[set_row])) { nudge(); break; }
				set_edited();
				break;
			}

			/*
			  On the shelf X turns a card to the next of the files behind it. One
			  direction only, and it wraps: a title has two or three dumps, so wrapping
			  reaches all of them, and there is no second free button on this screen to
			  spend on going back (see build_legend).

			  Nothing else needs telling. The entry's game index moves with it, and every
			  per-game path in this file reads that index - so the launch, the play count,
			  Recently Played, the favourite, the suspend points and the per-game core
			  options all follow to the file now on show. The slots are re-stat()ed by
			  sync_sel_slots(), which counts the game as part of the selection for exactly
			  this reason.
			*/
			if (screen == SCR_HOME)
			{
				if (!lib_view_cycle(sel, 1)) { nudge(); break; }
				mark_dirty();
				break;
			}

			// Deleting a suspend point removes a real savestate file, so it takes
			// two presses: the first arms it and says so on screen.
			if (screen != SCR_SUSPEND) { nudge(); break; }

			chome_item *it = cur_game();
			if (!it) { nudge(); break; }

			int st = slot_state(it, slot_idx);
			if (!st || st == 2) { nudge(); break; }   // empty, or locked

			if (del_arm_slot == slot_idx && !CheckTimer(del_arm_until))
			{
				del_arm_slot = -1;
				if (!lib_delete_slot(it, slot_idx)) nudge();
				mark_dirty();
			}
			else
			{
				del_arm_slot = slot_idx;
				del_arm_until = GetTimer(2500);
				mark_dirty();
			}
			break;
		}

		case KEY_MINUS:
		case KEY_EQUAL:
		{
			// The shoulders page the shelf, so they belong to the shelf. With a panel up
			// they were still paging it behind the dialog.
			if (screen != SCR_HOME) { nudge(); break; }

			const chome_profile *p = theme_get();
			int n = lib_view_count();
			int next = sel + ((k == KEY_MINUS) ? -p->visible : p->visible);
			if (next < 0) next = 0;
			if (next >= n) next = n ? n - 1 : 0;
			if (next != sel) { sel = next; mark_dirty(); } else nudge();
			break;
		}

		case KEY_MENU:
		case KEY_F12:
			/*
			  In a game core this is the way out of the pause menu, and the release that
			  follows has to be eaten or the classic OSD opens on it.

			  In the menu core it used to hand straight off to the classic OSD, which is
			  the one menu a player of this front-end should never meet by accident. It
			  opens our own menu bar instead - the same thing Up does. Advanced settings
			  are still one deliberate choice away, on Options.
			*/
			if (ig_active) { eat_menu_release = 1; ig_close(1); }
			else go_screen(screen == SCR_MENUBAR ? SCR_HOME : SCR_MENUBAR);
			return 1;

		default:
			break;
		}
	}
	else if (!key || (key & UPSTROKE))
	{
		// Reset the hold counter on release as well as on idle, otherwise a run of
		// discrete taps looks like a held key and triggers the screenful jump.
		if (!key || k == last_key) key_run = 0;
	}

	/*
	  That key closed the in-game menu, so stop here.

	  Everything below is the frame: it re-claims the analog output, re-measures the
	  canvas and paints. Running it after the menu has gone puts the front-end back over
	  the running game with no menu on it - the scaler pointed at our framebuffer, the
	  canvas re-measured to the core's mode - and the game is left unreachable behind it.
	  On the device that showed up as "load a state, land back in the game, and the pad
	  does nothing"; pressing the menu button appeared to fix it because that is the one
	  close path that already returned before reaching here (case KEY_MENU below).

	  Every other way out came through: loading a slot, B on the shelf, quitting a game,
	  saving-and-closing. The close itself was always correct - what followed it was not.
	*/
	if (was_ingame && !ig_active) return 1;

	/*
	  Networking. Cheap unless something is in flight: it reaps the child that runs
	  iw or ifup, and asks for the link again every few seconds while a screen is
	  showing it. The state has to be picked up even after leaving the screen,
	  because the child is still out there either way.
	*/
	net_poll();
	bt_poll();
	chome_pend_poll();

	/*
	  The optical drive, if there is one. One status ioctl per pass, and sectors are
	  only read on the pass after a disc turns up - see chome_disc.h for why that
	  matters on the thread that draws.

	  Not while a core has the disc. Once it has been handed over, the core's reader
	  owns /dev/sr0 and our detection helper must stay dead: this loop also runs behind
	  a running game, for the in-game menu, and disc_poll() restarts the helper
	  whenever it finds it stopped. See disc_launch().
	*/
	if (disc_handed_to_core && is_menu()) disc_handed_to_core = 0;

	if (!disc_handed_to_core) disc_poll();
	if (!disc_handed_to_core && disc_take_dirty())
	{
		/*
		  mark_dirty() is the point of the dirty flag, and leaving it off is how the
		  badge came and went without the screen ever repainting - the same omission
		  that made the core options cursor look stuck. Nothing about a disc arriving
		  comes through a keypress, so if this does not ask for a repaint, nothing will.
		*/
		mark_dirty();

		/*
		  Taken out while we were looking at it. Both disc screens describe a disc that
		  is no longer there, so they have to be left rather than sitting there
		  offering to play nothing.
		*/
		if (disc_state() == DISC_ABSENT && (screen == SCR_DISC || screen == SCR_DISCBAR))
		{
			go_screen(SCR_HOME);
		}

		printf("ClassicUI: disc state=%d type=%s name=\"%s\"\n",
			disc_state(), disc_type_name(disc_type()), disc_display_name());
	}

	/*
	  And a repaint while it is spinning, for the same reason the Wi-Fi screen repaints
	  while it scans: the badge animates, and this UI only draws when something says it
	  must. Rate-limited to the animation step so a spinning disc does not mean a full
	  repaint every pass of this loop.
	*/
	if (disc_state() != DISC_ABSENT)
	{
		/*
		  Faster than the other animations, because the disc travels further per frame -
		  see GFX_DISC_MS. Only while a disc is in the drive, so a machine with an empty
		  one repaints exactly as often as it did before any of this existed.
		*/
		static unsigned long disc_next_spin = 0;
		if (CheckTimer(disc_next_spin))
		{
			disc_next_spin = GetTimer(GFX_DISC_MS);
			mark_dirty();
		}
	}

	/*
	  Nothing about the network arrives on a keypress: the scan finishes, an address
	  turns up, a join ends. The screen only repaints when something marks it dirty,
	  so fold what is on it into one number and repaint when that changes - otherwise
	  a finished scan sits behind a "looking for networks" that never goes away.
	*/
	{
		const net_link *nl = net_link_now();
		unsigned sig = (unsigned)net_count()
			| ((unsigned)net_scanning() << 8)
			| ((unsigned)net_join_state() << 9)
			| ((unsigned)nl->up << 12)
			| ((unsigned)(nl->ip[0] ? 1 : 0) << 13)
			| ((unsigned)(strlen(nl->ssid) & 63) << 14);

		if (sig != wifi_seen)
		{
			wifi_seen = sig;
			mark_dirty();
		}
	}

	/*
	  The controller screen needs the same treatment, and needs it more: a pairing is a
	  running commentary from a child, so none of it arrives on a keypress at all. The
	  message text is folded in rather than its length, because the interesting changes
	  are between messages of similar length ("Pairing" to "Connecting"), and without it
	  a pairing in progress sits behind whatever the screen said when it started.
	*/
	{
		static unsigned bt_seen = 0;

		/*
		  bt_present() is in here because the list has an entry that exists only when there
		  is one: plug a dongle in with this screen open and nothing else about the state
		  changes, so without it "Add a Controller" would not turn up until something else
		  happened to repaint.
		*/
		unsigned sig = (unsigned)bt_count()
			| ((unsigned)bt_pairing() << 8)
			| ((unsigned)bt_pair_state() << 9)
			| ((unsigned)bt_pair_done() << 12)
			| ((unsigned)bt_present() << 15);

		for (const char *q = bt_pair_detail(); *q; q++) sig = sig * 31u + (unsigned char)*q;
		for (const char *q = bt_pair_name(); *q; q++) sig = sig * 31u + (unsigned char)*q;

		if (sig != bt_seen)
		{
			bt_seen = sig;
			mark_dirty();
		}
	}

	/*
	  And the tester, which needs it most of all: a stick moves without any key being
	  delivered at all, and the buttons that are delivered are swallowed by padtest_key()
	  before anything else looks at them. Folded rather than repainted every frame - an
	  idle menu costing nothing is the reason this whole front-end is affordable, and a
	  player looking at a pad they are not touching is idle.
	*/
	if (screen == SCR_PADTEST)
	{
		pad_row row;
		pad_state st;
		int have = padtest_find(&row);
		int live = input_pad_state(have ? row.player : 0, &st);

		/*
		  `live` is in here on its own account: a pad waking up while the tester is open
		  changes the whole panel from "asleep" to a diagram, and it can do that without
		  a single bit of `held` changing - the first press is over by the time anything
		  looks. The armed footer is in for a different reason: it expires on a timer, and
		  nothing else would notice it had.
		*/
		unsigned sig = (unsigned)st.held
			^ ((unsigned)(uint8_t)st.lx << 4) ^ ((unsigned)(uint8_t)st.ly << 12)
			^ ((unsigned)(uint8_t)st.rx << 18) ^ ((unsigned)(uint8_t)st.ry << 24)
			^ ((unsigned)(have ? row.player : 0) << 28)
			^ ((unsigned)(live ? 1 : 0) << 30)
			^ ((unsigned)(padtest_back_arm && !CheckTimer(padtest_back_until)) << 31);

		if (sig != padtest_seen)
		{
			padtest_seen = sig;
			mark_dirty();
		}
	}

	// Background work: one scan slice and one art decode per frame.
	if (lib_scanning())
	{
		lib_scan_step();
		art_init(theme_get()->sel_w, theme_get()->sel_h);
		view_rebuild(1);
		if (ig_active) ig_select_running();     // findable once its system is in
	}

	// Re-assert the claim on the analog output every frame. It is free once held,
	// and the fb terminal shares the mechanism and drops it when a script exits,
	// which would otherwise leave this UI drawing where nothing displays it.
	video_menu_fb_analog(1);

	if (!gfx_begin())
	{
		// No HPS framebuffer, or no memory for the compose buffer. Consuming keys
		// here would leave a black screen with no way out, so step aside for good.
		printf("ClassicUI: framebuffer unavailable, falling back to the classic menu\n");
		if (ig_active) { ig_close(1); return 0; }
		active = 0;
		handed_off = 1;
		OsdMenuCtl(1);
		return 0;
	}

	int w = gfx_w(), h = gfx_h();
	const chome_profile *p = theme_get();
	if (p->w != w || p->h != h)
	{
		theme_update(w, h, cfg.classicui_profile);
		art_init(theme_get()->sel_w, theme_get()->sel_h);
		gfx_damage_all();
		dirty = 1;
	}

	sync_sel_slots();
	request_visible_art(theme_get());

	int before = art_cache_count();
	art_step();
	if (art_cache_count() != before) mark_dirty();

	animate();

	if (dirty)
	{
		dirty = 0;
		render();
	}

	return 1;
}
