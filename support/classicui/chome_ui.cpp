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
/*
  For ss_available() alone - whether this build carries a ScreenScraper application
  credential, which the Online Covers screen has to say out loud. Note that the ss_*
  functions defined *in this file* are savestates and have nothing to do with this
  header; see the comment on SCR_COVERS.
*/
#include "chome_ss.h"
#include "chome_video.h"
#include "chome_icons32.h"
#include "chome_icons16.h"
#include "chome_btn12.h"
#include "chome_osk.h"
#include "chome_net.h"
#include "chome_disc.h"
#include "chome_rip.h"
#include "chome_titles.h"
#include "chome_bt.h"
#include "chome_ini.h"
#include "chome_opt.h"
#include "chome_cfgrec.h"

#include "../../cfg.h"
#include "../../user_io.h"
#include "../../recent.h"
#include "../../input.h"
#include "../../osd.h"
#include "../../video.h"
#include "../../hardware.h"
#include "../../file_io.h"
#include "../../charrom.h"
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
static int ig_is_disc = 0;            // ...and it is the disc in the drive, not a file

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

// Close Game / Resume. What is armed is ig_close_until above; see draw_close().
static int cls_row = 0;

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

/*
  The two rows under the options: the font, then Save Changes. Named rather than written as
  set_nview and set_nview + 1 at a dozen sites, because "which row am I on" is asked by the
  drawing, the legend, left/right, A, X and B, and one of those left comparing against the
  wrong number is a key that acts on the row below the cursor.
*/
#define SET_ROW_FONT (set_nview)
#define SET_ROW_SAVE (set_nview + 1)
static int set_nrows();

// Everything staged and not yet written, from both models below. The Save row's count, the
// arm, and the question B asks on the way out are all this one number.
static int set_pending();

/*
  The Font row, which is on that same screen and is not one of the options above.

  It cannot be, and the reason is written out on opt_def in chome_opt.h: that table's
  safety property is that every value it can write is a number cfg.cpp will accept, and
  `font=` is a 1024-character path. So it is a row of its own, staged the way the rest of
  the screen is staged, and it writes its one key through the same ini_apply_set() the
  Save row uses for everything else.

  Entry 0 is always the built-in font and always reachable - see FontRestoreBuiltin() in
  charrom.cpp for why that took 2KB rather than being free. The rest are whatever .pf
  files are on the card, plus, if `font=` names something that is no longer there, that
  name as well: the row's job is to say what the machine is set to, and dropping a missing
  font from the list would show the player "Built-in" for a file they can see in their ini.
*/
#define FONT_MAX 33                          // the built-in, plus 32 files

/*
  As wide as cfg.font itself, on purpose. Anything narrower would truncate a long `font=`
  on the way in and then write the truncated path back out on the next save - a settings
  screen quietly corrupting the setting it was opened to look at.
*/
static char font_rel[FONT_MAX][sizeof(cfg.font)];   // "" for the built-in, else the path
static int font_n = 1;
static int font_sel = 0;                     // staged
static int font_was = 0;                     // ...and what the file says
static char font_note[96] = {};              // a load that did not happen, for the footer

/*
  Online Covers: the player's own ScreenScraper account.

  Staged rather than written as it is typed, which is the same shape as More Settings and
  chosen for the same reason - this writes lines into somebody's MiSTer.ini, so there is a
  Save row, two-press arming, and a way to leave without keeping any of it.

  cov_pass is the reason this file has a comment about a variable rather than only about a
  screen. It is a password in clear in the firmware's memory, exactly as cfg.classicui_ss_pass
  already is and as the line in MiSTer.ini already is; what this screen must not do is make
  that worse by putting it anywhere a person or a log can see it. Nothing below ever draws
  it, prints it, or hands it to the keyboard as an initial value - see cov_open_pass() and
  the comment on the row text in draw_covers() for how each of those is avoided.
*/
#define COV_ROWS 4
#define COV_ON   0                           // the rows, which three places step over
#define COV_USER 1
#define COV_PASS 2
#define COV_SAVE 3

static int  cov_row = 0;
static int  cov_on = 0;                      // staged copies of the three cfg fields
static char cov_user[64];
static char cov_pass[64];
static int  cov_arm = 0;                     // one press from writing
static unsigned long cov_arm_until = 0;
static int  cov_quit_arm = 0;                // ...and one from throwing the edits away
static unsigned long cov_quit_until = 0;
static int  cov_wrote = -1;                  // -1 nothing written yet, else how many
static int  cov_failed = 0;
static char cov_note[96];                    // one refused entry, for a few seconds
static unsigned long cov_note_until = 0;

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
#define OSKD_SS_USER 2                       // Online Covers: the account name
#define OSKD_SS_PASS 3                       // ...and its password
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
/*
  Online Covers, which is where a player sets up their ScreenScraper account.

  Named COVERS and not SS, and the state below is cov_* for the same reason: in this
  file `ss_` already means *savestate* - ss_can_save(), ss_do_load(), ss_copy_opt() -
  and a second meaning for the same two letters in the same file is a trap for whoever
  reads it next. The module this screen configures is chome_ss.cpp; nothing here
  borrows its prefix.
*/
#define SCR_COVERS  19

/*
  Putting the game away, which is now a menu-bar entry and so needs a screen of its own to
  ask on. See draw_close() for why it is a screen and not a press on the bar.
*/
#define SCR_CLOSE   20

// Rows on the Options panel. Several places step over them.
/*
  Eleven rows either way, and only the tenth differs: in a game it is Core Settings, the
  only route to the options belonging to the core itself rather than to this front-end,
  and on the shelf it is Advanced, which hands the shelf to the classic menu. A
  player reported the core's own options as the one thing the front-end had taken away
  from them, and they were right: widescreen on PSX, or a core's own video and audio
  settings, live in the classic OSD and nowhere else, and the OSD is only reachable while
  that core is running.

  Both lists end in About. It used to have a permanent slot on the menu bar - one of five,
  next to the things a player reaches for every session - for a panel that is read once and
  never again. Close Game took that slot and About came down here, which is the same trade
  in both directions: prominence for how often the thing is actually wanted.
*/
#define OPT_ROWS_MENU 11
#define OPT_ROWS_GAME 11

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

// The strip sizes its tiles for a full row of these, in chome_theme.cpp, which cannot see
// this number - so the two are checked against each other here rather than left to drift:
// a profile laying out four tiles for a screen that draws three would simply look wrong,
// with nothing to point at.
static_assert(CH_SLOTS_USER == CHOME_STRIP_SLOTS,
	"the strip lays out a different number of slots than it draws");

#define MB_DISPLAY  0
#define MB_OPTIONS  1
#define MB_POWER    2
#define MB_CLOSE    3
#define MB_CORE     4
#define MB_COUNT    5

/*
  Language and Manuals are gone. The first opened a panel with nothing behind it,
  and the second only handed the screen to the classic OSD - which is exactly what
  the front-end is not supposed to do on its own.

  About is gone from here too, and Close Game has its slot. Putting a game away was the
  eleventh row of a panel that is itself two presses in - so far down that at 240p it was
  off the end of the list entirely until the list learned to scroll - while About, which a
  person reads once, sat on the bar for every session after. That is the prominence of the
  two exactly the wrong way round. About is the last row of Options now.

  The count is unchanged, which is deliberate: MB_COUNT sizes mb_label[] and bounds the
  walks in mb_count_visible() and mb_at(), and a swap rather than an addition leaves all
  three alone.
*/
static const char *mb_label[MB_COUNT] = { "Display", "Options", "Power", "Close Game", "Core" };

/*
  A shorter word for a cell too narrow for the real one, and a null where there is no
  shorter word worth having.

  This is the legend's long/short pair - lp(LBL_A, "Look Again", "Scan") - applied to the
  bar, and Close Game is what made the bar need it. In a game at 240p the bar carries four
  entries across 320 pixels, so a cell holds about eight characters and "CLOSE GAME" is
  ten: gfx_clip() served it as "CLOSE G>". Every other entry here is seven characters or
  fewer and has never come close, which is why they get no second form rather than a
  duplicate of their first.

  The bar picks between them by measuring, not by profile - see draw_menubar(). With a core
  that publishes no options there are three entries and the full phrase fits at 240p too,
  and shortening it there on the strength of the canvas size alone would be giving up room
  the player actually has.
*/
static const char *mb_short[MB_COUNT] = { 0, 0, 0, "Close", 0 };

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

	/*
	  There is no game to close on the shelf, and an entry that did nothing there would be
	  worse than no entry: it is the one on the bar whose name promises something
	  destructive, so a player who found it greyed out would still have to wonder what it
	  had been about to do. Same test as MB_CORE above.
	*/
	if (i == MB_CLOSE) return ig_active ? 1 : 0;

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
// The selection the chrome is drawn from, which lags sel while the shelf is sliding.
// Declared with sel because the session restore below places both; see shown_entry().
static int sel_shown = 0;
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
	sel_shown = sel;                  // placed, not moved - see view_rebuild()
	printf("ClassicUI: back where you were - view %d, entry %d of %d\n", view, sel + 1, n);
	return 1;
}

static int mb_idx = 0;
static int slot_idx = 0;
static int sort_idx = 0;
static int opt_row = 0;
static int opt_top = 0;                      // first Options row drawn; the list scrolls in a game
static int look_row = 0;
static int co_row = 0;                       // the core-options list
static int co_top = 0;                       // first row drawn; this list scrolls too
static int co_tier = CO_TIER_PICTURE;

/*
  The last option handed to the whole system, and how long its footer line stays up.

  Giving a setting to every game writes a file the player cannot see and changes nothing
  on screen except a star going away - and a star going away is what *undoing* an override
  looks like too. So the promotion says so in words for a few seconds, the way the Settings
  screen reports its own write, and names the option so there is no doubt which row it was.
*/
static char co_promoted[CO_NAME_LEN] = {};
static unsigned long co_promoted_until = 0;

static double bar_y = 0, strip_y = 0, curtain = 0;

// The last position an activity indicator was painted at, so a busy screen repaints
// when the ring moves and not once per frame. See ui_busy().
static unsigned long anim_seen = 0;
static unsigned long launch_at = 0;
static unsigned long nudge_until = 0;
static unsigned long last_ms = 0;

static int dirty = 1;

/*
  The disc's periodic spin repaint, kept apart from `dirty` on purpose: dirty means
  "something about the frame changed, redraw it all", and the spinning disc is the one
  thing that changes without anything else doing so - it repaints through the partial
  path (render_region) instead, which redraws only the disc's rectangle. Anything that
  marks dirty in the same pass wins, because a full repaint repaints the disc too.
*/
static int disc_spin_due = 0;

/*
  The carousel's slide, kept apart from `dirty` for exactly the reason the disc's spin is:
  it means "the only thing that changed is the card row", and the card row is one
  contiguous band of full-width rows - which is the shape render_region() can clip to.

  A slide is the most expensive thing this front-end does and the least of it actually
  moves. Moving the cursor changes the cards, the title, the system line, the file line and
  the button prompts, and the union of those spans most of the height of the screen, so
  every frame of every slide was a full repaint - twelve of them per tap, and one per key
  repeat for as long as an arrow is held. A full repaint measures 5.1 ms on the device at
  240p and is essentially linear in pixels, which puts 1080p somewhere near 60 ms: a shelf
  that browses at 240p and crawls on the display most people own.

  What the band buys, measured in the harness over a 200-frame held scroll: at 720p the
  damaged rows fall from 720 to 304 and the compose from 679 to 427 us, and at 240p from 240
  rows to 96 and 116 to 93 us. The rows are the number that matters most on the device,
  because the copy goes into an uncached /dev/mem mapping shared with the FPGA and is charged
  by the row. The compose saves less than the row count suggests, and that is expected: the
  cards are the expensive part of the frame and the cards are what stays inside the band.

  What makes the band usable is that the two halves are already on different clocks: the
  cards follow the eased selF, and everything else follows the discrete selection. Freeze
  the second (see sel_shown) and the moving region is the card row and nothing else.

  Anything that marks dirty in the same pass wins, because a full repaint repaints the
  cards too. That is the invariant the whole thing rests on: anything that appears,
  disappears or moves outside the band must mark dirty, not this.
*/
static int slide_due = 0;

/*
  And the marquee's repaint, which is the third flag of this shape and the third version of
  the same argument. It means "the only thing that changed is that some scrolling text moved
  one character", and that text is a line or two of glyphs - the narrowest band any of these
  three has ever asked for.

  It has to be a band, not a full frame, and the numbers are the reason. A marquee steps
  every GFX_MARQ_STEP_MS, which is between five and six paints a second for as long as the
  player leaves a long name under the cursor - not a burst, a resting state. A full repaint
  measures 5.1ms on the device at 240p and scales with the canvas, so six a second is 3% of
  the loop at 240p, around 24% at 1080p and around 36% at 1080p with the half-resolution
  canvas turned off. That is a shelf that stutters because a title is long, on the display
  most people own, which is precisely the regression the partial-repaint work existed to
  stop. Two text lines out of 720 rows is under 2% of the copy.

  `marq_epoch` is what the phase is measured from, and mark_dirty() moves it. That is the
  whole of the focus story: every change of what is selected goes through mark_dirty() -
  it has to, or the row the cursor moved to would not be drawn - so a marquee is always
  measured from the moment the thing it is on became the thing it is on. A row gains focus
  and shows its beginning for GFX_MARQ_HOLD_MS before anything moves, which is the
  behaviour asked for; a row that loses focus stops being drawn through the marquee at all
  on the very same frame.

  Which is also why animate() must not use mark_dirty() for its timers - see mark_anim().
  A Wi-Fi scan repaints every GFX_SPIN_MS for as long as it runs, and if each of those
  repaints moved the epoch the marquee would be pinned at its first character on the one
  screen whose contents are other people's network names.
*/
static int marq_due = 0;
static unsigned long marq_epoch = 0;

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


static void mark_dirty() { dirty = 1; marq_epoch = GetTimer(0); }
static void mark_slide() { slide_due = 1; }

/*
  A full repaint that is not a change of state: an ease still easing, a timer that has run
  out, an activity ring one position further round.

  Identical to mark_dirty() except that it leaves marq_epoch alone, and that difference is
  the only reason it exists. Everything in animate() repaints because the clock moved, not
  because anything the player did changed what is on screen - and a marquee whose phase was
  reset by every clock-driven repaint would never leave its first character on any screen
  that has an animation running on it.

  The rule for choosing between the two: mark_dirty() if the answer to "what is selected,
  what screen is this, what does it say" changed, mark_anim() if only "how far through an
  animation are we" changed.
*/
static void mark_anim() { dirty = 1; }

/*
  The half-resolution framebuffer, asked for whenever this front-end owns the screen and
  the option says so.

  One call, made wherever the ownership changes and re-asserted every frame from the
  loop, the same idiom video_menu_fb_analog(1) already follows there: an unchanged
  request is free, and re-asserting is what makes the More Settings toggle take effect
  on the next frame with no plumbing of its own - the loop below notices the canvas
  changed and re-lays everything out, exactly as it does for the analog takeover.

  The request must NOT outlive the ownership. Everything after this front-end shares the
  same framebuffer: the classic menu's wallpaper, the F9 terminal, a script's console.
  Each hand-off below releases it (video_fb_size_request(0)), because a terminal that
  came up at half resolution because a menu had been open would be this front-end
  scribbling on somebody else's screen. See the release sites in chome_leave(),
  ig_close() and the video_fb_state() yield.
*/
static void fb_size_sync()
{
	video_fb_size_request(cfg.classicui_halfres ? 2 : 0);
}

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

// Defined with the disc dialog, where the state it reads lives: the disc sitting in the
// drive as an item the suspend strip can be about, or 0 when nothing here can say what
// name its states would be filed under.
static chome_item *disc_susp_item_get();

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
  The selection the chrome is drawn from, which is not always the one the cursor is on.

  This is the other half of the band clip (see slide_due). The cards are interpolated from
  selF every frame; the title, the system line, the file line and the button prompts read
  the discrete selection, and they sit above and below the card row - so a frame in which
  those changed too could not be clipped to the row at all. Under a held arrow the
  selection changes on every key repeat, which is every 50 ms (REPEATRATE), so that was
  every frame of a scroll.

  So the chrome follows sel_shown, which *commits* to sel at two moments: when the shelf
  comes to rest, and when the key that was scrolling it is let go. Between those the chrome
  is not merely allowed to be stale, it is required to be - nothing outside the card band is
  being repainted while a slide runs, so anything up there that changed would smear.

  What the player sees. A tap moves the cards and the title follows on the release, which on
  a real press is a few tens of milliseconds later; there is no timer involved and nothing
  to tune. Holding the arrow scrolls the shelf with the title left on the item the hold
  began on, and the whole chrome catches up the moment the arrow is released - or before
  that, if the shelf runs out of shelf and comes to rest with the key still down. The title
  therefore never disagrees with a stationary shelf, which is the only state a player reads
  it in.

  Two commit points rather than one, and either alone would do: a release that never arrives
  - and swallowed upstrokes are a real thing here, see chome_handle() - still commits when
  the ease settles, and a hold whose repeats outrun the ease still commits on the release.
  Nothing can leave the chrome stuck.

  Actions deliberately keep reading the live sel: what A launches is the card under the
  cursor, not whatever the title happens to be naming. The two can only differ while an
  arrow is held down, and by the time the shelf has stopped they agree again.
*/
static const chome_entry *shown_entry()
{
	return lib_view_entry(sel_shown);
}

static chome_item *shown_game()
{
	const chome_entry *e = shown_entry();
	if (!e || e->kind != ENT_GAME) return 0;
	return lib_item(e->game);
}

/*
  The letter a shelf entry files under, for the shoulder jump.

  Taken from the name as *shown*, which matters because the title stored for a game has
  already had its article rotated - "The Legend of Zelda" is held as "Legend of Zelda,
  The" and files under L, where a player looking for it will expect it.

  Everything that is not a letter folds together into one stop. A shelf that opens with
  "240p Test Suite", "3D WorldRunner" and "8 Eyes" should be one jump away from A, not
  three, and nobody thinks of those as separate sections.

  Returns 0 only when there is no name at all, which groups those together too rather
  than making them each their own stop.
*/
static char jump_initial(int i)
{
	const chome_entry *e = lib_view_entry(i);
	if (!e) return 0;

	const char *s = 0;
	if (e->kind == ENT_GAME)
	{
		chome_item *it = lib_item(e->game);
		s = it ? it->title : 0;
	}
	else s = e->label;

	if (!s || !*s) return 0;

	unsigned char c = (unsigned char)*s;
	return isalpha(c) ? (char)tolower(c) : '#';
}

static void sel_commit()
{
	if (sel_shown == sel) return;
	sel_shown = sel;
	// The title, the meta line, the file line and the prompts all change with it, and all
	// of them are outside the band - so this is the one thing in the slide path that has
	// to ask for the whole frame.
	mark_dirty();
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
  The running disc, when there is one, as something the screens can be handed.

  A disc is not on the shelf: nothing scanned it, so there is no card to walk onto and
  no card for the in-game menu to park on. Everything those screens need is in ig_item
  already, so where the shelf would have supplied a selection this supplies the disc
  instead. Returns nothing when a file-launched game is running, which leaves browsing
  another game's slots from the in-game menu exactly as it was.
*/
static chome_item *ig_running_disc()
{
	return (ig_active && ig_have_item && ig_is_disc) ? &ig_item : 0;
}

/*
  The item the suspend strip is about: the shelf's selection, or the running disc when
  there is no selection to be had.

  One accessor rather than a cur_game() call at each site, because that is how the first
  version of this went wrong on hardware. The strip opened on the disc - the disc was
  passed in explicitly - and then every question *about* it was asked of cur_game()
  independently, in six places. With a folder focused, which is what a disc launch leaves
  behind, all six answered "no game", so a running disc was offered "ENT PLAY" on its own
  savestate strip and neither saving nor loading was reachable.
*/
/*
  Set while the strip was opened from the disc dialog, and only then.

  The fallback below is the right rule from the shelf - Down there means the card under
  the cursor - and the wrong one from the disc's own dialog, where the shelf's selection
  has nothing to do with what is on screen. It is only *accidentally* right today: a disc
  launch leaves a folder focused, so cur_game() is 0 and the disc drops out of the
  fallback. Park the cursor on a game card, open the disc dialog, press Down, and the
  same code would have shown that game's slots under the disc's name.

  So the disc dialog says who its Down was for instead of relying on the shelf being
  parked somewhere harmless. Cleared by whoever opens the strip the ordinary way.
*/
static int susp_is_disc = 0;

static chome_item *susp_target()
{
	if (susp_is_disc)
	{
		chome_item *d = ig_running_disc();
		if (d) return d;

		/*
		  Or the disc that is only sitting in the drive. Reaching the strip from the shelf
		  needs an item as much as reaching it from a game does, and without this the disc
		  dropped straight into the fallback below - the shelf's own selection, under the
		  disc's name in the header. That is the confusion the paragraph above describes,
		  and it was only ever avoided by a launch happening to leave a folder focused.
		*/
		d = disc_susp_item_get();
		if (d) return d;
	}

	chome_item *it = cur_game();
	return it ? it : ig_running_disc();
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
	/*
	  The slide, not the world. What the re-stat can change is the row of pips, which is
	  inside the band a slide repaints; the disarmed delete belongs to the suspend strip,
	  which is a different screen. This used to be the reason a held scroll repainted every
	  frame in full even with the title deferred - it runs before animate() on every pass
	  and marks whatever the selection moving is worth, so it had to learn the difference
	  too. Off the shelf it is upgraded to a full repaint at the one place that decides,
	  because there the pips are tiles in the strip instead.
	*/
	mark_slide();
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
	/*
	  The shelf is placed here rather than moved, so there is nothing for the chrome to
	  defer - and sel_shown must not be left pointing into a view that no longer has that
	  many entries. Every site that assigns selF straight from sel is a site where the
	  chrome goes with it, for both of those reasons.
	*/
	sel_shown = sel;
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
	sel_shown = sel;                  // placed, not moved - see view_rebuild()
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
  A system with no drawing of its own, and the one whose drawing it borrows.

  chome_icons32.h is generated from a licensed set by tools/icons32.py and is the only
  file allowed to hold artwork, so a new system cannot be given an icon here - it either
  has one in that header or it does not. Three of the CD systems do not, and each of them
  is a peripheral bolted onto a machine that does: the drawing of a Mega Drive is the
  right picture for a Mega CD shelf, and it is the picture a player recognises. Without
  this they would all draw the folder, which says nothing about which console they are.

  Saturn is a machine of its own and has nothing to borrow, so it keeps the folder until
  the generator is run against a Saturn icon.
*/
static const struct { const char *id; const char *icon; } sysicon_alias[] =
{
	{ "megacd",   "md"     },
	{ "pcecd",    "tg16"   },
	{ "neogeocd", "neogeo" },
};

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

	for (size_t i = 0; i < sizeof(sysicon_alias) / sizeof(sysicon_alias[0]); i++)
	{
		if (strcasecmp(sysicon_alias[i].id, id)) continue;
		for (size_t j = 0; j < sizeof(sysicons) / sizeof(sysicons[0]); j++)
		{
			if (!strcasecmp(sysicons[j].id, sysicon_alias[i].icon)) return &sysicons[j];
		}
	}
	return 0;
}

// See chome.h: which drawing a system ends up with, asked of the resolver above rather
// than worked out again by whoever wants to know.
const char *chome_sysicon_id(const char *sysid)
{
	const sysicon_def *d = sysicon_find(sysid);
	return d ? d->id : 0;
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
	int maxc = gfx_text_cols(w - 6 * u, ts);
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
		gfx_shout(up);
		gfx_text_c(gfx_clip(up, ts, w - 4), x + w / 2, ty + i * lh, ts, COL_WHITE, COL_SHADOW);
	}

	if (s)
	{
		int bw = gfx_text_w(s->badge, 1) + 4;
		gfx_fill(x + 2 * u, y + h - 2 * u - 11, bw, 11, COL_SHADOW);
		gfx_text(s->badge, x + 2 * u + 2, y + h - 2 * u - 9, 1, COL_PANELHI, 0);
	}
}

/*
  The band a slide repaints, recorded as the row is drawn.

  It exists for the reason disc_note_rect() does, and it is worth restating because getting
  it wrong is silent: the honest source of the rectangle is the drawing itself. draw_shelf()
  interpolates each card's size from the profile and hands draw_card() a bottom edge and a
  height, draw_card() derives its shadow and its focus ring from that height - and a second
  copy of any of that arithmetic up here would drift the first time somebody changed the
  card ratio, the shadow or the ring. Drift in the direction of "too small" is exactly the
  smear a partial repaint cannot fix: it only ever repaints what it clips to, so a card that
  reaches one row above the band leaves that row holding an older frame for as long as the
  player keeps scrolling, and nothing ever comes back for it.

  Rows rather than a rectangle, because the cards slide the whole width of the canvas -
  there is no useful horizontal bound, and the copy in gfx_end() is charged by the row in
  any case.

  Read from the previous composed frame, which is safe for the same reason the disc's
  rectangle is: a partial slide frame only happens on a pass where nothing marked dirty, and
  everything that could change this - a resolution change, a view rebuild, a screen opening
  over the shelf - marks dirty and takes the full path, which re-records it.

  The pips and the position line note themselves into the same band, deliberately: see
  draw_pips().
*/
static struct { int y0, y1, on; } slide_rc;

static void slide_note_rows(int y0, int y1)
{
	if (!slide_rc.on) { slide_rc.y0 = y0; slide_rc.y1 = y1; slide_rc.on = 1; return; }
	if (y0 < slide_rc.y0) slide_rc.y0 = y0;
	if (y1 > slide_rc.y1) slide_rc.y1 = y1;
}

/*
  The band the last composed frame drew the card row into. 0 when it drew no row at all -
  the browser, which returns out of compose() before the shelf - in which case there is
  nothing to clip a slide to and the caller has to repaint the world.
*/
static int slide_band(int *y0, int *y1)
{
	if (!slide_rc.on) return 0;
	*y0 = slide_rc.y0;
	*y1 = slide_rc.y1;
	return 1;
}

/*
  The band the last composed frame scrolled text in, and how long until that text moves.

  Recorded the same way and for the same reason as the card row's band above: the repaint
  that follows a marquee step has to clip to something, and the only thing that knows where
  the scrolling text ended up is the code that drew it. Rows rather than a rectangle,
  because gfx_end() copies by the row in any case and because two lines of a list are two
  bands one row apart - a rectangle would have to be their union anyway.

  `next` is the earliest instant at which any of this frame's marquees changes its window.
  Not a poll interval: gfx_marquee() is a pure function of the clock and can therefore say
  exactly when it will next look different, so the repaint happens on the frame the text
  moves and on no other. This is the disc's lesson (see disc_spin_sig) arrived at from the
  other end - the disc asks every 16ms and throws away the ticks on which its angle has not
  moved; the marquee is asked once and says when to come back.
*/
static struct { int y0, y1, on; unsigned long next_in; } marq_rc;
static unsigned long marq_next = 0;

static void marq_note(int y0, int y1, unsigned long next_in)
{
	if (!marq_rc.on)
	{
		marq_rc.y0 = y0;
		marq_rc.y1 = y1;
		marq_rc.next_in = next_in;
		marq_rc.on = 1;
		return;
	}
	if (y0 < marq_rc.y0) marq_rc.y0 = y0;
	if (y1 > marq_rc.y1) marq_rc.y1 = y1;
	if (next_in < marq_rc.next_in) marq_rc.next_in = next_in;
}

/*
  Is one of this front-end's own panels sitting over the shelf?

  It was spelled out inline in compose(), where it decides whether to lay a scrim over the
  background. It is a function now because the marquee needs the same answer: the shelf's
  title block is drawn *behind* every one of these panels, and a marquee running back there
  is motion nobody can read that still asks for a repaint five times a second - which is the
  exact shape of the cost this whole design is arranged to avoid, arrived at by accident.

  The menu bar is deliberately not in the list, here as in compose(): the bar sits above the
  title rather than over it, the title is fully legible with the bar up, and a player on the
  bar is looking at the shelf.
*/
static int overlay_up()
{
	return (screen == SCR_SORT || screen == SCR_DISPLAY || screen == SCR_OPTIONS ||
		screen == SCR_ABOUT || screen == SCR_WIFI || screen == SCR_PADS ||
		screen == SCR_POWER || screen == SCR_INI || screen == SCR_PADTEST ||
		screen == SCR_SET || screen == SCR_CORE || screen == SCR_DISC ||
		screen == SCR_COVERS || screen == SCR_CLOSE);
}

static int marq_band(int *y0, int *y1)
{
	if (!marq_rc.on) return 0;
	*y0 = marq_rc.y0;
	*y1 = marq_rc.y1;
	return 1;
}

/*
  Fit a string to `maxpx`, scrolling it if the caller says this is the thing with focus.

  The one entry point every marquee in this front-end goes through, and it takes `focused`
  as an argument rather than working it out. That is the answer to "what does focusable mean
  here", and it is a deliberate choice against the obvious alternative: gfx_clip() is called
  from about sixty places buried in drawing code, and nothing down there knows whether it is
  drawing the selected row - draw_listrow() would have had to be told, and so would
  draw_rows_c_at(), and so would the browser. Threading a flag to all sixty would have put
  the decision in sixty places and made "is this focused" a property of a text run instead
  of a property of a list.

  Every caller that matters already has the answer one variable away. draw_listrow() takes
  `on`. draw_rows_c_at() computes `on`. draw_browse() computes `on`. The shelf's title block
  is drawn from the committed selection and is therefore *always* the focused thing - there
  is no unfocused version of it. So the information was never missing; it simply was not
  where gfx_clip() was standing. Asking the caller costs one argument and keeps the
  unfocused path byte-for-byte what it was.

  `y` is the top of the text and the scale gives its height; both are needed because this is
  also where the repaint band gets recorded, and recording it here rather than at each call
  site is what makes it impossible to add a marquee and forget the repaint that makes it
  move. A marquee with no band would scroll only when something else asked for a frame,
  which is the failure that has already been paid for twice in this file - the disc badge
  and the arriving cover art both landed correctly and stayed invisible.

  The `site` is threaded through by hand, and it has to be. The clip log keys on the name of
  the function that asked (chome_gfx.h), and assert_no_clipped_copy()'s allow-list is a
  table of (site, text) pairs - "draw_card may cut a shelf's name, and nothing else may".
  Left to the macro, every clip routed through here would arrive at that table calling
  itself "marq_fit_at", the allow-list would match nothing, and the guard would report every
  game title on the shelf as a sentence of ours that had been cut. So the caller's own name
  is passed on, which is exactly what the gfx_clip() macro does one level further up.
*/
#ifdef CHOME_HOST_TEST
#define marq_fit(s, scale, maxpx, focused, y) \
	marq_fit_at((s), (scale), (maxpx), (focused), (y), __func__)
#else
#define marq_fit(s, scale, maxpx, focused, y) \
	marq_fit_at((s), (scale), (maxpx), (focused), (y), 0)
#endif

static const char *marq_fit_at(const char *s, int scale, int maxpx, int focused, int y,
	const char *site)
{
	(void)site;

	if (!focused)
	{
#ifdef CHOME_HOST_TEST
		return gfx_clip_at(s, scale, maxpx, site);
#else
		return gfx_clip(s, scale, maxpx);
#endif
	}

	int scrolling = 0;
	unsigned long next_in = 0;
	unsigned long ms = GetTimer(0) - marq_epoch;

#ifdef CHOME_HOST_TEST
	const char *out = gfx_marquee_at(s, scale, maxpx, ms, &scrolling, &next_in, site);
#else
	const char *out = gfx_marquee(s, scale, maxpx, ms, &scrolling, &next_in);
#endif

	// Only a string that did not fit has anything to repaint for. gfx_text() damages one
	// pixel past the cell on both axes (see gfx_text_w there), so the band matches.
	if (scrolling) marq_note(y, y + 8 * scale + scale, next_in);
	return out;
}

/*
  A card's two decorations, as functions rather than as literals, because the band is
  derived from them: the drop shadow, offset down and right by a fortieth of the height,
  and the focus ring, which sits CARD_RING pixels outside the selected card on every side
  and is that thick. Change either and the band follows.
*/
#define CARD_RING 2
static int card_shadow(int h) { int sd = h / 40; return sd < 2 ? 2 : sd; }

static void draw_card(const chome_entry *e, int cx, int bottom, int w, int h, int selected)
{
	int x = cx - w / 2, y = bottom - h;
	int sd = card_shadow(h);

	// Recorded as it is drawn, and for every card: the row is as tall as its tallest card
	// and the ring and the shadow are part of it.
	slide_note_rows(y - CARD_RING, bottom + sd);

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
		gfx_shout(up);
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
			gfx_shout(up);
			gfx_text_c(gfx_clip(up, bs, w - 4), x + w / 2, y + h - bandh + 3, bs, COL_PANELHI, 0);
		}

		if (it->fav)
		{
			int box = 16;
			gfx_fill(x + w - box - 6, y + 4, box + 4, box + 4, COL_SHADOW);
			picto("star", x + w - box - 4, y + 6, box, COL_YELLOW);
		}

		/*
		  Several files behind one card, said as a stack of cards rather than as a number.
		  A count would need a digit legible at 240p, where a card is 84 wide and the band
		  along its bottom already has the title in it; the stack reads at a glance and at
		  any size, which is the whole reason the shape is conventional.

		  Top left, because the favourite star is top right and a game can be both.

		  Three rectangles rather than a picto, deliberately. A picto is a 1-bit mask
		  sampled to its box, and three one-pixel outlines with one-pixel gaps do not
		  survive being sampled to 12 pixels - they turn into a grey smudge. Drawn as
		  geometry it stays crisp at every size, and gfx_frame_rect is exactly the
		  primitive for it.

		  Back to front, each one filling before it frames, so the front card occludes the
		  two behind it and only their bottom-right edges show. Drawn the other way round
		  the outlines cross each other and it reads as a grid.

		  Not animated, and inside the card's own rectangle. Both matter: draw_card()
		  records the damage band from this card's geometry a few lines above, so anything
		  drawn within it is already covered, while a badge that breathed would want the
		  band grown to its maximum for a frame it has not drawn yet - the trap the disc
		  badge needed DISC_BADGE_CELLS for.
		*/
		if (e->nvar > 1)
		{
			int box = h / 5;
			if (box > 16) box = 16;              // never larger than the star opposite it
			if (box < 6) box = 6;                // below this the gaps close up anyway
			int step = (box >= 12) ? 2 : 1;
			int side = box - 2 * step;

			gfx_fill(x + 4, y + 4, box + 4, box + 4, COL_SHADOW);
			for (int i = 2; i >= 0; i--)
			{
				int rx = x + 6 + i * step, ry = y + 6 + i * step;
				gfx_fill(rx, ry, side, side, COL_SHADOW);
				gfx_frame_rect(rx, ry, side, side, COL_PANELHI, 1);
			}
		}
	}

	if (selected)
	{
		gfx_frame_rect(x - CARD_RING, y - CARD_RING, w + 2 * CARD_RING, h + 2 * CARD_RING,
			COL_FOCUS, CARD_RING);
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

	/*
	  And the band at the crest of the growth, whether or not a card is at it this frame.

	  This is the trap the disc badge's breath was built around and it is the same shape
	  here, so it is worth spelling out twice. The band is read one frame after it is
	  recorded. A band that only held the cards at the size they were drawn would therefore
	  be one frame behind the card growing towards the centre - and the row it grows into
	  would be clipped away by the very frame that wanted to draw it, leaving a line of the
	  previous, smaller card above the enlarged one that nothing ever repaints, because the
	  only thing painting that row is this same clipped path.

	  sel_h is the height the interpolation below reaches at t == 1 and the largest a card
	  can ever be, so the band is recorded at that height on every frame - the badge's
	  rectangle is recorded at the crest of its breath for exactly this reason. See
	  disc_note_rect() and DISC_BADGE_CELLS.
	*/
	slide_note_rows(p->y_shelf - p->sel_h - CARD_RING, p->y_shelf + card_shadow(p->sel_h));

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

/*
  Is this frame drawn over a still of the paused game?

  Asked in two places and therefore written once. draw_background() blits the still, and
  compose() has to dim what is on top of it - and the second is only right when the first
  actually happened, because the two want different scrims. A scrim chosen for a photograph,
  laid over the front-end's own grid instead, would be the wrong dim on the wrong thing.

  The size test is part of the question and not paranoia: ig_bg is allocated at the canvas
  size and chome_handle() can resize the canvas under an open menu without rebuilding it, in
  which case there is no still on this frame at all. draw_background() says so out loud.
*/
static int ig_still_shown(const chome_profile *p)
{
	return ig_active && ig_bg && ig_bg_w == p->w && ig_bg_h == p->h;
}

static void draw_background(const chome_profile *p)
{
	// Paused inside a game, the menu sits over a still of it.
	if (ig_still_shown(p))
	{
		gfx_blit(ig_bg, p->w, p->h, 0, 0, p->w, p->h);
		gfx_fill(0, p->h - p->h * 24 / 100, p->w, p->h * 24 / 100, COL_BGDARK);
		return;
	}

	/*
	  In a game and falling through to the grid instead, which is a fault every time: the
	  still is what this background is for. Named once per change rather than per frame,
	  because it is drawn sixty times a second and a log that scrolls is a log nobody reads.

	  The size test is the interesting half and the reason it is reported separately from
	  "no still at all". ig_bg is allocated at the canvas size, so a mismatch is not a
	  build that went wrong - it is the canvas having changed *since* the build, which the
	  resize path in chome_handle() does without rebuilding it. That is a real hole and this
	  is what would show it: a menu that came up over the game and lost the still when
	  something resized the framebuffer under it.
	*/
	if (ig_active)
	{
		static const uint32_t *said_bg = 0;
		static int said_w = -1, said_h = -1, said_cw = -1, said_ch = -1;

		if (ig_bg != said_bg || ig_bg_w != said_w || ig_bg_h != said_h
			|| p->w != said_cw || p->h != said_ch)
		{
			said_bg = ig_bg;
			said_w = ig_bg_w; said_h = ig_bg_h;
			said_cw = p->w;   said_ch = p->h;

			if (!ig_bg) printf("ClassicUI: no still to draw the menu over, so the grid is what shows\n");
			else printf("ClassicUI: the still is %dx%d and the canvas is now %dx%d, so it cannot be drawn\n",
				ig_bg_w, ig_bg_h, p->w, p->h);
		}
	}

	gfx_fill(0, 0, p->w, p->h, COL_BG);

	int step = p->w / 40; if (step < 8) step = 8;
	for (int x = 0; x < p->w; x += step) gfx_fill(x, 0, 1, p->h, COL_GRID);
	for (int y = 0; y < p->h; y += step) gfx_fill(0, y, p->w, 1, COL_GRID);

	gfx_fill(0, p->h - p->h * 24 / 100, p->w, p->h * 24 / 100, COL_BGDARK);

}

/*
  The block above the shelf: the title, the system line and, on the cards that need it, the
  file name.

  Drawn from the committed selection and not the live one. All three lines sit above the card
  row, so a slide that redrew them could not be clipped to that row - and there is nothing to
  read in a title that changes twenty times a second anyway. See shown_entry() for when it
  commits and what the player sees while it has not.
*/
static void draw_title_block(const chome_profile *p)
{
	const chome_entry *e = shown_entry();
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
	gfx_shout(up);
	/*
	  Scrolled, and unconditionally: this line is the name of the thing the cursor is on and
	  there is no version of this screen where it is not the focused element. It is also the
	  string this front-end cuts most often and cares most about - a shelf of No-Intro dumps
	  is a shelf of names longer than any canvas - and the card under the cursor cannot help,
	  because a card is sized by its artwork and cuts the same name harder.

	  Centred, which is why gfx_marquee() keeps the window the same number of characters at
	  every offset. See the note there.

	  Unconditionally except behind a panel, which is not a caveat about focus but about
	  visibility: with one of our own panels up this line is behind a scrim, and a marquee
	  there is motion nobody reads that still asks for a repaint. See overlay_up().
	*/
	gfx_text_c(marq_fit(up, p->ts_title, avail, !overlay_up(), p->y_title),
		p->w / 2, p->y_title, p->ts_title, COL_WHITE, COL_SHADOW);

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

	gfx_shout(meta);
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
			// sel_shown, to match the entry the two lines above were drawn from: asking
			// the live selection for the file name of a different card's variant is how a
			// deferred title block would come apart.
			const char *file = lib_view_variant_file(sel_shown, e->vsel);

			if (e->nvar > 1) snprintf(line, sizeof(line), "%d/%d  %s", e->vsel + 1, e->nvar, file);
			else snprintf(line, sizeof(line), "%s", file);

			/*
			  Scrolled on the same terms as the title above it, and it is the line that gains
			  the most by it: this is the only place a player can tell two dumps of one game
			  apart, and what tells them apart is at the very end of the name - "(Europe)" or
			  "(USA) (Disc 1)", which is exactly what gets cut. The clip log has it losing up
			  to thirty-two characters at 240p.
			*/
			gfx_text_c(marq_fit(line, p->ts_tiny, avail, !overlay_up(), y),
				p->w / 2, y, p->ts_tiny, COL_PANELLO, 0);
		}
	}
}

/*
  The save-slot pips under the shelf, drawn from the *live* selection - one of the two
  things below the cards that is not deferred with the title, and the reason both are in the
  band rather than out of it.

  The position line is the deliberate one. It is the single element on the screen whose
  entire content is the thing that is changing, so freezing it would not leave it stale but
  wrong: "12 / 320" under a shelf that is somewhere in the two hundreds says less than
  nothing, and the two arrows on that line are the end-of-list affordance - frozen, they
  would keep offering a direction the shelf has already run out of. It is what gives a fast
  scroll a sense of place once the title has stopped being readable, which is precisely when
  it earns its keep.

  The pips then come along for almost nothing: they sit between the cards and that line, so
  a band that reaches the line contains them anyway, and deferring them would put two
  different clocks inside one clipped region to save about thirty rows. The whole choice
  costs 71 rows at 720p - a band of 304 rows of 720 rather than 233 - which is the price of
  a scroll that still says where it is.

  Both note their rows unconditionally, before any early return: the pips appear the moment
  the cursor lands on a game, and a band that only covered them while they were on screen
  would clip away the very frame that first drew them. That is the growing-edge trap again,
  and it is why the note is above the "no game selected" return rather than below it.
*/
static void draw_pips(const chome_profile *p)
{
	int s = (p->id == PROF_HD) ? 2 : 1;
	int d = 6 * s, gap = 5 * s, n = user_slots();

	slide_note_rows(p->y_pips, p->y_pips + d);

	chome_item *it = cur_game();
	if (!it) return;

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
	// At 240p this line does not exist at all, so it never needs a row kept clear for it.
	if (p->id == PROF_LO) return;

	// Before the "no entries" return, and for the same reason the pips are: see draw_pips().
	slide_note_rows(p->y_pos, p->y_pos + 8 * p->ts_tiny);

	int n = lib_view_count();
	if (!n) return;

	char buf[64];
	if (lib_scanning()) snprintf(buf, sizeof(buf), "%d / %d  SCANNING", sel + 1, n);
	else snprintf(buf, sizeof(buf), "%d / %d", sel + 1, n);

	gfx_text_c(buf, p->w / 2, p->y_pos, p->ts_tiny, COL_DIM, 0);

	int half = gfx_text_w(buf, p->ts_tiny) / 2;
	if (sel > 0) gfx_text(CH_LEFT, p->w / 2 - half - gfx_adv(p->ts_tiny), p->y_pos, p->ts_tiny, COL_DIM, 0);
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
  the floor Dinofly set: below that a filled triangle and a filled square are both blobs,
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
	  maker nor their model. Taken from Dinofly's own pairing records:

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

/*
  Defined with the disc dialog further down, where the state it describes lives. The
  legend is assembled up here, but it must not hold a second opinion about what that
  screen does: the dialog answers for itself.
*/
static int disc_dlg_legend(legend_pair *out, int max);

/*
  Same reason, smaller scale: the Online Covers legend only offers Save while there is
  something to save, and the answer to that is the staging buffers, which live with the
  screen. Asking rather than keeping a flag up here is what stops the prompt and the
  panel from ever disagreeing about whether the file needs writing.
*/
static int cov_dirty();

static int build_legend(legend_pair *out, int max)
{
	int n = 0;
	/*
	  The committed selection, because the prompts change with the card under the cursor -
	  "Open" on a folder against "Start" on a game, Version only on a grouped card, Resume
	  only on a game that is already running - and the legend is drawn below everything the
	  band covers. Deferred with the title rather than given a rect of its own: it describes
	  the actions available on an item, so it belongs to whichever item the title is naming.
	  A legend that offered Version for a card that had scrolled away would also be a legend
	  that lied about what X would do.
	*/
	const chome_entry *e = shown_entry();

	switch (screen)
	{
	case SCR_SUSPEND:
	{
		// Inside that very game the slots become live: A restores, Y writes.
		int here = ig_is_running(susp_target());

		// Nothing to offer for a core with no savestates - see draw_suspend(). Also from
		// the shelf, where the answer comes from the table instead of the core.
		if (no_savestates_for(susp_target()))
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
	case SCR_CLOSE:
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

			/*
			  And the way out of "this game only" that keeps the value rather than
			  throwing it away: Y makes it the value every game on the core gets.

			  Y because the shoulders jump the shelf by letter and are not read here, Select sorts,
			  and the other three faces are spoken for on this screen - A turns the page, B
			  goes back, X hands a setting to every game *by discarding it*. Y is the one
			  face button this screen had nothing for, and it sits next to the X it is the
			  mirror of: both act on the row under the cursor, both appear only where that
			  row is this game's own. Nothing else in the front-end binds Y outside the
			  suspend strip and the shelf, so no habit is being broken.

			  Offered on exactly the rows X is, and for the same reason: with no game bound
			  a change already goes to the core's config, and on a row this game does not
			  override there is nothing to hand over. core_opt_can_promote() is the whole
			  condition.

			  It does push Back off the legend at 240p, where only three prompts survive.
			  That is the trade the suspend strip already makes with its four, and it is the
			  right way round here: B is the press every screen in this front-end answers
			  to, while a button the player has never seen does not exist until it is shown.
			*/
			if (co && core_opt_can_promote(co) && n < max)
			{
				out[n++] = lp(LBL_Y, "For All Games", "All");
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
		if (set_row == SET_ROW_SAVE)
		{
			// ...and only while there is something to save, or the row would keep
			// offering a press that does nothing but shake the panel.
			if (set_pending() && n < max) { out[n++] = lp(LBL_A, "Save", "Save"); }
			if (n < max) { out[n++] = lp(LBL_B, "Back", "Back"); }
			break;
		}
		if (n < max) { out[n++] = { CH_LEFT CH_RIGHT, "dpad_lr", "Change", "Chg", 0, COL_WHITE }; }
		// The Font row has no recommended value to go back to - see the row's colour - so X
		// puts back the one the file names, which is the undo somebody actually wants.
		if (n < max) { out[n++] = lp(LBL_X, (set_row == SET_ROW_FONT) ? "Saved Font" : "Usual Value",
			(set_row == SET_ROW_FONT) ? "Saved" : "Usual"); }
		if (n < max) { out[n++] = lp(LBL_B, "Back", "Back"); }
		break;

	/*
	  Online Covers. On a build with no application credential nothing but Back is offered:
	  every other key on the screen is refused there, and a prompt for a press that only
	  shakes the panel is worse than no prompt - the same rule the Controllers screen
	  follows for a wired pad it can do nothing with.
	*/
	case SCR_COVERS:
		if (ss_available())
		{
			if (cov_row == COV_ON && n < max)
			{
				out[n++] = { CH_LEFT CH_RIGHT, "dpad_lr", "Change", "Chg", 0, COL_WHITE };
			}
			if (cov_row == COV_USER && n < max) { out[n++] = lp(LBL_A, "Type It", "Type"); }
			if (cov_row == COV_PASS && n < max) { out[n++] = lp(LBL_A, "Type It", "Type"); }
			if (cov_row == COV_SAVE && cov_dirty() && n < max) { out[n++] = lp(LBL_A, "Save", "Save"); }
		}
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
	/*
	  Built by the dialog itself rather than here, because what it offers depends on
	  whether the disc is in the drive or already playing and on which of its two buttons
	  the cursor is on - and a legend naming a press the screen does not offer is worse
	  than no legend at all. See disc_dlg_legend().
	*/
	case SCR_DISC:
		n = disc_dlg_legend(out, max);
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
			int running = ig_is_running(shown_game()) || susp_matches(shown_game());
			if (n < max) { out[n++] = lp(LBL_A, running ? "Resume" : "Start", running ? "Play" : "Start"); }
			/*
			  X was the one face button the shelf had nothing for, which is what makes it
			  the button that cycles a card's files. Everything else was taken and none of
			  it could be given up: A starts, B jumps back to the folders, Y favourites,
			  Select sorts, the shoulders jump the shelf by letter, up is the menu bar and down is
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
	/*
	  Air between the prompts, in glyph cells rather than in advances: three characters of
	  space between "MOVE" and "CHOOSE" is what separates the pairs, and letter spacing is
	  about the gap *inside* a word. Following the advance here would widen the gaps at the
	  one moment the labels beside them got wider too, which is when the row is tightest.
	*/
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
		gfx_shout(up);
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
		gfx_shout(up);

		/*
		  The short form, where the long one would be served cut. Measured against exactly
		  what gfx_clip() measures against below, so the two cannot come to disagree about
		  whether the word fits - a test of that shape is what found "CLOSE G>" at 240p.
		  Shouted again because the short form has not been through it yet.
		*/
		if (mb_short[i] && (int)strlen(up) > gfx_text_cols(cellw - 8, s))
		{
			snprintf(up, sizeof(up), "%s", mb_short[i]);
			gfx_shout(up);
		}

		if (on) gfx_fill(cx - cellw / 2 + 2, y + 2, cellw - 4, h - 6, COL_BLUE);
		gfx_text_c(gfx_clip(up, s, cellw - 8), cx, y + (h - 8 * s) / 2, s,
			on ? COL_WHITE : COL_INK, 0);
	}
}

/* ------------------------------------------------------------- panels ----- */

struct panel_box { int x, y, w, h, s; };

/*
  A panel where the caller says where it goes.

  Centring is right for a dialog that is smaller than the screen, and wrong for one that
  is nearly all of it: the disc dialog is asked to keep a small margin at the top and to
  stop clear of the button legend at the bottom, and those two are not the same distance,
  so no centred rectangle expresses it. Centring a panel tall enough to reach the legend
  puts its bottom back over the legend, which is the thing being fixed.
*/
static panel_box draw_panel_at(const chome_profile *p, int x, int y, int w, int h, const char *title)
{
	panel_box b;
	int s = p->ts_ui;
	int hdr = 10 * s + 6;

	gfx_fill(x + 4, y + 4, w, h, COL_SHADOW);
	gfx_fill(x, y, w, h, COL_PANEL);
	gfx_frame_rect(x, y, w, h, COL_PANELLO, 2);
	gfx_fill(x, y, w, hdr, COL_INK);

	char up[64];
	snprintf(up, sizeof(up), "%s", title);
	gfx_shout(up);
	gfx_text(up, x + 6 * s, y + 4, s, COL_PANELHI, 0);

	b.x = x; b.y = y + hdr; b.w = w; b.h = h - hdr; b.s = s;
	return b;
}

static panel_box draw_panel_ex(const chome_profile *p, int w, int h, const char *title)
{
	return draw_panel_at(p, (p->w - w) / 2, (p->h - h) / 2, w, h, title);
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
/*
  Every list goes through here, so this is where "a row the player cannot see" is caught.

  The loop below stops when a row would cross the bottom edge, silently - which is the right
  thing to draw and the wrong thing to do without telling anyone. Twice now a list has grown
  past its panel and the rows past the fold simply vanished while staying selectable: Close
  Game in the Options panel, and every row past the fifteenth on the PSX's 27-row core-options
  page. Both were found by eye, on hardware, long after the fact.

  So a truncated list is recorded under the host test and asserted against at every profile.
  A caller whose list can outgrow its panel has to window it - list_fit(), list_track() and
  list_scrollbar() are right above - and this is what says whether it did. The site is the
  *calling* function, via the macro in the header, because "some list was cut" is not
  actionable and "draw_core_opts was cut" is.
*/
#ifdef CHOME_HOST_TEST
struct rowdrop_rec { char site[48]; int n; int drawn; };
static rowdrop_rec rowdrops[32];
static int nrowdrops = 0;

static void rowdrop_add(const char *site, int n, int drawn)
{
	for (int i = 0; i < nrowdrops; i++)
	{
		if (!strcmp(rowdrops[i].site, site ? site : "?"))
		{
			// Keep the worst case for this screen: the most rows it ever lost.
			if (n - drawn > rowdrops[i].n - rowdrops[i].drawn)
			{
				rowdrops[i].n = n;
				rowdrops[i].drawn = drawn;
			}
			return;
		}
	}
	if (nrowdrops >= (int)(sizeof(rowdrops) / sizeof(rowdrops[0]))) return;
	snprintf(rowdrops[nrowdrops].site, sizeof(rowdrops[nrowdrops].site), "%s", site ? site : "?");
	rowdrops[nrowdrops].n = n;
	rowdrops[nrowdrops].drawn = drawn;
	nrowdrops++;
}

// See the note in chome.h. Reads, no levers: nothing here can move the marquee.
unsigned long chome_marq_epoch() { return marq_epoch; }
int chome_marq_live() { return marq_rc.on; }

int chome_rowdrop_n() { return nrowdrops; }
void chome_rowdrop_clear() { nrowdrops = 0; }
const char *chome_rowdrop_site(int i)
{
	return (i >= 0 && i < nrowdrops) ? rowdrops[i].site : "";
}
int chome_rowdrop_lost(int i)
{
	return (i >= 0 && i < nrowdrops) ? rowdrops[i].n - rowdrops[i].drawn : 0;
}
#endif

static void draw_rows_c_at(const panel_box *b, const char *const *rows, const char *const *vals,
	const uint32_t *vcol, int n, int idx, const char *site)
{
	int rowh = 12 * b->s;
	int drawn = 0;
	for (int i = 0; i < n; i++)
	{
		int y = b->y + 5 * b->s + i * rowh;
		if (y + rowh > b->y + b->h) break;
		drawn++;

		int on = (i == idx);
		if (on) gfx_fill(b->x + 3, y - 2 * b->s, b->w - 6, rowh - 2 * b->s, COL_BLUE);

		char up[64];
		snprintf(up, sizeof(up), "%s", rows[i]);
		gfx_shout(up);

		char v[48];
		v[0] = 0;
		int vw = 0;
		if (vals && vals[i])
		{
			snprintf(v, sizeof(v), "%s", vals[i]);
			gfx_shout(v);
			vw = gfx_text_w(v, b->s);
		}

		/*
		  What the label may use: everything the value does not want, or half the row -
		  whichever is more.

		  Half the row flat was the rule here, and it cut labels that had room to spare.
		  "RESCAN LIBRARY" came out "RESCAN LIBRAR>" beside a value reading "52 GAMES"
		  with a third of the row empty between them, and a list with no value column at
		  all - the sort panel - lost the ends of "RECENTLY PLAYED" and "RECENTLY ADDED"
		  to a half it was never sharing with anything.

		  Never less than half, so no row is narrower than it was: a value long enough to
		  eat the label is a value that should be shortened, and taking the label's room
		  away to make space for it would be the wrong end to give.
		*/
		int lw = b->w - 12 * b->s - vw;
		if (lw < b->w / 2) lw = b->w / 2;

		// The row under the cursor scrolls its label; the rest are cut as they always were.
		// `on` is right here, which is the whole argument for marq_fit() taking it rather
		// than trying to work it out from underneath.
		gfx_text(marq_fit(up, b->s, lw, on, y), b->x + 6 * b->s, y, b->s, on ? COL_WHITE : COL_INK, 0);

		if (v[0])
		{
			uint32_t col = (vcol && vcol[i]) ? vcol[i] : (on ? COL_WHITE : COL_PANELLO);
			gfx_text(v, b->x + b->w - 6 * b->s - vw, y, b->s, col, 0);
		}
	}

#ifdef CHOME_HOST_TEST
	if (drawn < n) rowdrop_add(site, n, drawn);
#else
	(void)site;
#endif
}

static void draw_rows_at(const panel_box *b, const char *const *rows, const char *const *vals,
	int n, int idx, const char *site)
{
	draw_rows_c_at(b, rows, vals, 0, n, idx, site);
}

// The site is the caller's name, which is the only form of it worth reporting.
#define draw_rows_c(b, r, v, c, n, i) draw_rows_c_at((b), (r), (v), (c), (n), (i), __func__)
#define draw_rows(b, r, v, n, i)      draw_rows_at((b), (r), (v), (n), (i), __func__)

/*
  A list longer than its panel, in the three pieces every screen with one needs: how many
  rows fit, which row the window starts at, and the bar down the right-hand edge saying
  where in the list that window is.

  It lives here, next to draw_rows_c(), because draw_rows_c() is the reason it has to
  exist: that loop stops as soon as a row would cross the bottom of the panel, so a list
  handed more rows than fit simply loses the last of them - silently, and only on the
  profile where the panel happens to be short. Options lost its eleventh row that way and
  nobody could reach Close Game on a 240p television, while the same list on the same
  build was complete at 480p and 720p.

  Written once and used by both screens rather than copied into the second, so More
  Settings and Options cannot drift into scrolling by different rules. `foot` is whatever
  the caller draws under the rows and must therefore keep clear of; a screen with nothing
  down there passes 0 and gets the whole panel, which is the arrangement that was there
  before any of this and the reason a list that already fits is drawn exactly as it was.
*/
static int list_fit(const panel_box *b, int rowh, int foot, int nrows)
{
	int fit = (b->h - 5 * b->s - foot) / rowh;
	if (fit < 1) fit = 1;
	if (fit > nrows) fit = nrows;
	return fit;
}

// Keeps the selected row inside the window, and the window inside the list.
static void list_track(int *top, int sel, int nrows, int fit)
{
	if (sel < *top) *top = sel;
	if (sel >= *top + fit) *top = sel - fit + 1;
	if (*top > nrows - fit) *top = nrows - fit;
	if (*top < 0) *top = 0;
}

/*
  Where the cursor is in a list that does not fit. Inside the value column's right
  margin, so it cannot land on a value.

  Nothing at all when the list fits, which is not a nicety: a scrollbar beside a complete
  list tells the player there is more below when there is not.
*/
static void list_scrollbar(const panel_box *b, int rowh, int top, int fit, int nrows)
{
	if (nrows <= fit) return;

	int s = b->s;
	int tx = b->x + b->w - 3 * s, ty = b->y + 3 * s, th = fit * rowh;
	gfx_fill(tx, ty, 2 * s, th, COL_PANELLO);

	int hh = th * fit / nrows;
	if (hh < 4 * s) hh = 4 * s;
	gfx_fill(tx, ty + (th - hh) * top / (nrows - fit), 2 * s, hh, COL_INK);
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

// Defined with the rest of the rip, further down. Whether a copy is running, which is a
// disc that is genuinely being read from end to end.
static int rip_busy_ui();

static unsigned long disc_target_speed()
{
	/*
	  A rip turns it at the focus rate, which is the fastest rate this UI has.

	  Not a new, faster one, and that is measured rather than a preference: there are 64
	  positions in a turn and chome_gfx.h records that a disc advancing more than about four
	  of them between repaints strobes instead of spinning. GFX_DISC_FOCUS_MS is already
	  four per frame at the full-repaint rate, so it is the ceiling - the first attempt at
	  that constant used 400ms and looked like a juddering disc rather than a fast one.
	  "Spinning fast" therefore means the fastest thing here that still reads as spinning.

	  Ahead of the SCR_DISCBAR test because a rip can be running while the badge has focus,
	  and the rip is the more specific fact.
	*/
	unsigned long period = rip_busy_ui() ? GFX_DISC_FOCUS_MS
		: (screen == SCR_DISCBAR) ? GFX_DISC_FOCUS_MS
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
  The disc's phase, 0..DISC_TURN-1.

  Accumulates from elapsed time at the current speed, so a speed change moves the
  speed and nothing else. Memoised on the clock because two discs can be on screen in one
  frame - the badge and the prompt's - and advancing the phase once per *draw* would spin
  it at double rate on that screen.

  This is the one accumulator both quantisations below read, which is what keeps the
  badge and the dialog the same physical disc: the dialog resolves the same turn four
  times as finely, it does not turn at its own rate.
*/
static unsigned long disc_phase_now()
{
	unsigned long now = anim_ms();

	if (!disc_spd_to)
	{
		disc_spd_from = disc_spd_to = disc_target_speed();
		disc_ramp_t0 = now;
		disc_phase_ms = now;
		disc_step_ms = now;
	}

	if (now == disc_step_ms && disc_phase_ms) return disc_phase;

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
	return disc_phase;
}

// The badge's rotation, 0-63: what gfx_disc()'s 32-cell sprite can express, and all
// a 32-pixel disc has ever needed.
static int disc_step()
{
	disc_phase_now();
	return disc_step_cached;
}

/*
  Whether something the player is actually waiting on needs the loop's time more than
  the disc needs its angles. This is the owner's rule stated as code - "only reduce the
  animation steps when performance is required for the rest of the interface" - so the
  test is for named work in flight, not for how expensive the disc looks on paper.

  Three signals, all cheap reads of state something else already maintains:

    a rip. The copy runs in a helper process, but the sectors it writes share the memory
    bus with every rectangle this loop copies into the uncached framebuffer, and the
    whole point of the screen is that the copy finishes;

    a library scan, which runs a slice per pass through this same loop and is the one
    job the player sits watching a counter for;

    covers queued to decode - art_step() pays for one image decode per pass while any
    are, and a decode is milliseconds, the scale of an entire frame.

  Deliberately NOT a measurement of the previous frame's cost. That was considered and
  rejected: the harness drives these frames on a fake clock and asserts them byte for
  byte against full repaints, and an angle count that depended on how fast the host
  happened to run would make the drawn frame a function of wall-clock luck - the exact
  history-dependence the pinned hashes in the test suite exist to keep out.

  And NOT chome_core_idle(), which reads like the right signal and is the wrong one: it
  answers "may the scheduler sleep between passes", which is true on an idle disc dialog
  - precisely when the disc should be spending the budget, not trimming it.
*/
static int disc_contended()
{
	return rip_busy_ui() || lib_scanning() || art_pending();
}

// Defined with the rotation cache, further down: how coarsely disc_step_fine() quantises
// while disc_contended(), which is the spacing of the angles the cache holds whole.
static int disc_rot_stride();

/*
  The dialog's rotation, 0-255.

  Four times disc_step()'s resolution, because the dialog is where he asked for 60fps: at
  the slow rate a 64-position turn moves every 62.5ms, so of sixty frames a second only
  sixteen could show a new angle however often the screen repainted. 256 positions move
  every 15.6ms, which is just under the 16ms spin tick - the smallest count that gives
  every tick a new angle at every speed the disc turns.

  Quantised from the same phase as the badge's step, and the coarse table is exactly
  every fourth entry of the fine one, so the two discs cannot disagree about where the
  turn is - the dialog only resolves it more finely.

  Under contention it backs off to the angles the rotation cache holds whole - see
  disc_rot() - which makes every contended frame a cached blit and never a resample.
  Masking low bits rather than rounding, because masking floors within the same
  sequence: the step shown never exceeds the fine step, so entering or leaving
  contention can only pause the rotation for a slice of a frame, never run it backwards.

  Memoised on the same clock as the phase, and the memo carries the *quantised* answer
  deliberately: the contention signals move with work - art_step() can drain its queue
  between two asks at one instant - and byte-identity between a partial frame and a full
  repaint of the same instant is only checkable if the same millisecond always answers
  with the same step. The phase memo alone would leave the quantisation free to flip.
*/
static int disc_fine_cached = 0;
static unsigned long disc_fine_ms = (unsigned long)-1;

static int disc_step_fine()
{
	unsigned long phase = disc_phase_now();

	if (disc_fine_ms != disc_step_ms)
	{
		int fine = (int)(phase * 256UL / DISC_TURN);
		if (disc_contended()) fine &= ~(disc_rot_stride() - 1);

		disc_fine_cached = fine;
		disc_fine_ms = disc_step_ms;
	}

	return disc_fine_cached;
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
  The focus ring's pulse: the ring breathes between the selection blue and a lighter
  blue while the badge has focus, because on a real TV at 240p even a two-pixel ring of
  one steady colour is easy to miss in the corner - and brightness moving is what the
  eye is built to catch.

  Off the millisecond clock, like every other animation here (see chome_gfx.h): the UI
  only draws when something changed, so a frame counter would pulse at a different
  speed depending on what else was moving. Repaints already come every GFX_DISC_MS
  while a disc is on screen, so the pulse costs no extra frames - it rides the spin's.

  The bright end stops well short of white: a white ring against the disc's own white
  rim merged into one thick band that read as decoration, which is why the steady ring
  is blue in the first place.
*/
#define COL_DISC_FOCUS_HI 0xffa3bdddu    // COL_BLUE six-tenths of the way to COL_WHITE

/*
  Where the breath is: 0 at the trough, 256 at the crest.

  One accessor because the ring's colour and the badge's size both ride it, and they have
  to ride the *same* one. Two copies of this - or worse, two periods - would put the
  brightest ring and the biggest badge at different moments, and the corner would read as
  two things happening rather than as one thing alive.
*/
static unsigned long disc_pulse_e(void)
{
	unsigned long t = anim_ms() % GFX_DISC_PULSE_MS;

	// A triangle wave, 0..255..0 across the period...
	unsigned long x = t * 512UL / GFX_DISC_PULSE_MS;
	if (x > 255) x = 511 - x;

	// ...through a smoothstep, so the ends of the breath ease instead of bouncing.
	// Same 0..256 fixed point as disc_speed_now()'s ramp.
	unsigned long e = x * x * (768UL - 2UL * x) / (256UL * 256UL);
	return (e > 256) ? 256 : e;
}

static uint32_t disc_focus_col(void)
{
	unsigned long e = disc_pulse_e();

	uint32_t c = 0xff000000u;
	for (int sh = 0; sh <= 16; sh += 8)
	{
		unsigned long lo = (COL_BLUE >> sh) & 0xff;
		unsigned long hi = (COL_DISC_FOCUS_HI >> sh) & 0xff;
		c |= (uint32_t)(lo + (hi - lo) * e / 256UL) << sh;
	}
	return c;
}

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
  The badge's breath: the resting radius at the trough, GFX_DISC_BREATH_16 sixteenths more
  at the crest. An eighth, which at 240p is the 32px badge swelling to 36 and back.

  Upward from the resting size rather than either side of it, which is two decisions. The
  badge is anchored in the corner at p->inset and its centre is fixed - a breath that also
  shrank would leave the disc's edge drifting away from the edge it is aligned to - and
  every frame that is *not* focused then draws at exactly the size it drew before any of
  this existed, so the shelf and the dialog behind its scrim are untouched.
*/
static int disc_breath_r(int r)
{
	/*
	  Rounded to the nearest pixel rather than truncated, which is not a nicety: the eased
	  triangle tops out at 255 and not at 256 - it is folded at 255, so the crest is one
	  step short of the full swing - and truncating there costs the last pixel of the
	  growth. At 240p that pixel is half of the whole breath.
	*/
	unsigned long g = (unsigned long)r * GFX_DISC_BREATH_16 * disc_pulse_e();
	return r + (int)((g + 8UL * 256UL) / (16UL * 256UL));
}

/*
  Where the discs were drawn, recorded as they are drawn.

  The spin repaint needs the rectangle a disc occupies *before* it composes anything,
  and the honest source of that rectangle is the drawing itself: draw_disc() computes
  its centre from a panel layout that depends on the profile and the row count, and a
  second copy of that arithmetic would drift the first time someone resized the panel.
  So both disc draws note their bounds, compose() clears the notes first, and the
  partial repaint uses what the previous frame recorded.

  Using last frame's rectangles is safe, not merely convenient: a partial repaint only
  happens on a pass where nothing marked dirty, and everything that could move, add or
  remove a disc - a screen change, a disc arriving or leaving, a resolution change -
  marks dirty and takes the full path, which re-records these.

  Sized in cells of the *resting* radius, and the count is the caller's because the two
  discs do not reach the same distance. This must track what gfx_disc() actually paints:
  a disc drawn outside this rectangle is exactly the smear the partial repaint cannot
  fix, because it will never repaint those pixels.

  DISC_RECT_CELLS is the focus-ring variant of gfx_disc's grid - 16 cells of disc plus
  the ring's two - and it is passed whether or not the ring is on, because two cells of
  slack cost a few rows and the ring appearing is a screen change anyway.

  DISC_BADGE_CELLS is that same sprite at the crest of the badge's breath, rounded up: the
  ring sits at 18 cells of a radius that has grown by GFX_DISC_BREATH_16 sixteenths, so
  18 * 18/16 is 20.25 cells and this is 21. Which is the whole reason the count is a
  parameter - the badge's rectangle has to be recorded at a size the badge only reaches for
  an instant, and recorded at every size, because a rectangle that tracked the breath would
  be one frame behind the growing edge.
*/
#define DISC_RECT_CELLS  18
#define DISC_BADGE_CELLS ((DISC_RECT_CELLS * (16 + GFX_DISC_BREATH_16) + 15) / 16)

static struct { int x, y, w, h, on; } disc_rc[2];   // 0 the badge, 1 the prompt's

static void disc_note_rect(int i, int cx, int cy, int r, int cells)
{
	int ext = cells * r / 16;
	if (ext < cells) ext = cells;              // under one pixel per cell, keep it whole
	disc_rc[i].x = cx - ext;
	disc_rc[i].y = cy - ext;
	disc_rc[i].w = 2 * ext;
	disc_rc[i].h = 2 * ext;
	disc_rc[i].on = 1;
}

// The box round every disc the last composed frame drew. 0 when it drew none - the
// browser, say, or an empty drive - in which case nothing on screen is turning and
// the spin repaint has nothing to do.
static int disc_spin_rect(int *x, int *y, int *w, int *h)
{
	int x0 = 0x7fffffff, y0 = 0x7fffffff, x1 = -1, y1 = -1;

	for (int i = 0; i < 2; i++)
	{
		if (!disc_rc[i].on) continue;
		if (disc_rc[i].x < x0) x0 = disc_rc[i].x;
		if (disc_rc[i].y < y0) y0 = disc_rc[i].y;
		if (disc_rc[i].x + disc_rc[i].w > x1) x1 = disc_rc[i].x + disc_rc[i].w;
		if (disc_rc[i].y + disc_rc[i].h > y1) y1 = disc_rc[i].y + disc_rc[i].h;
	}

	if (x1 < 0) return 0;
	*x = x0; *y = y0; *w = x1 - x0; *h = y1 - y0;
	return 1;
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

	// Both lines of the selected row scroll: a Wi-Fi row's title is somebody's network name
	// and its second line can be a whole sentence about the state of the join, and neither is
	// a length this front-end chose.
	gfx_text(marq_fit(r->title, s, rx - tx, on, y), tx, y, s, ink, 0);
	if (r->sub) gfx_text(marq_fit(r->sub, s, rx - tx, on, y + 9 * s), tx, y + 9 * s, s, dim, 0);

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
	gfx_shout(up);

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
	int nl = wrap_text(body, gfx_text_cols(b->w - 16 * s, s), lines, 3);

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

/*
  A word across the middle of a slot tile - "EMPTY", "SAVING" - and nothing at all when
  the tile is too small to hold one.

  The longest of them, "NOT SAVED", is 72 px at the size the strip draws them, and a 240p
  tile has been narrower than that for as long as the strip has existed: centred in one it
  ran clean across its neighbours. The tiles are sized from the room the strip has now
  (chome_theme.cpp), so how narrow they get depends on the canvas and the margin, which is
  a reason to measure rather than to assume there is room. Clipped to the tile rather than
  shortened by hand, and dropped entirely below three characters - an empty frame already
  reads as empty, where "N>" over it reads as a fault in the drawing.
*/
static void slot_word(const char *word, int x, int y, int tw, int s, uint32_t col)
{
	int room = tw - 4 * s;
	if (gfx_text_cols(room, s) < 3) return;
	gfx_text_c(gfx_clip(word, s, room), x, y, s, col, 0);
}

static void draw_suspend(const chome_profile *p)
{
	if (strip_y <= 0.002) return;

	chome_item *it = susp_target();
	int ph = p->strip_h;

	// Comes to rest above the overscan margin, and its panel is extended down into
	// it so the bottom of the screen stays filled rather than showing a seam. The
	// strip therefore keeps ph of room whatever the margin is, and the tile row is
	// sized from that room - see chome_theme.cpp.
	int y = p->h - p->safe_y - (int)(ph * strip_y);

	gfx_fill(0, y, p->w, ph + p->safe_y, COL_BGDARK);
	gfx_fill(0, y, p->w, 2, COL_PANELLO);

	int s = p->ts_ui;
	int armed = (del_arm_slot >= 0 && !CheckTimer(del_arm_until));
	int room = p->w - p->inset * 2;

	char hdr[128];
	if (armed) snprintf(hdr, sizeof(hdr), "DELETE SLOT %d? PRESS", del_arm_slot + 1);
	else
	{
		/*
		  The game, and the label after it only when both fit. "LEGEND OF ZELDA, THE -
		  SUSPEND POINTS" is 37 characters where a 240p header holds 35, and gfx_clip cut
		  the tail - which is the label, except that reaching it had already eaten the end
		  of the title: "LEGEND OF ZELDA, THE - SUSPEND POI>". Dropping the label whole is
		  the better trade. Which screen this is the row of numbered tiles below says, and
		  the legend under them says it again; which game these slots belong to nothing
		  else on the screen says at all. A title too long even on its own is still
		  clipped below - there is nothing left to give it.
		*/
		const char *title = it ? it->title : "";
		snprintf(hdr, sizeof(hdr), "%s - SUSPEND POINTS", title);
		if (gfx_text_w(hdr, s) > room) snprintf(hdr, sizeof(hdr), "%s", title);
	}
	gfx_shout(hdr);

	// Armed, the header draws the button rather than naming it - the legend under it is
	// showing that same button, and one of them saying "X" while the other drew a square
	// was the two of them describing different controllers.
	if (armed) btn_hint_l(p->inset, y + 6 * s, s, COL_RED, hdr, LBL_X, "AGAIN");
	else gfx_text(gfx_clip(hdr, s, room), p->inset, y + 6 * s, s, COL_PANELHI, 0);

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
			gfx_text_cols(room - 16 * s2, s2), lines, 2);
		for (int i = 0; i < nl; i++)
			gfx_text_c(lines[i], p->w / 2, y + 22 * s2 + i * 11 * s2, s2, COL_PANELHI, 0);
		return;
	}

	int n = user_slots(), tw = p->thumb_w, th = p->thumb_h, gap = p->thumb_gap;
	int x0 = (p->w - (n * tw + (n - 1) * gap)) / 2;
	int ty = y + p->thumb_y;

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
			slot_word("SAVING", x + tw / 2, ty + th / 2 - 4 * p->ts_tiny, tw, p->ts_tiny, COL_YELLOW);
		}
		else if (!st)
		{
			gfx_fill(x, ty, tw, th, COL_BG);
			gfx_frame_rect(x, ty, tw, th, COL_DIM, 1);
			slot_word(i == pend_failed ? "NOT SAVED" : "EMPTY",
				x + tw / 2, ty + th / 2 - 4 * p->ts_tiny, tw, p->ts_tiny,
				i == pend_failed ? COL_RED : COL_DIM);
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
		gfx_shout(up);
		gfx_text_c(gfx_clip(up, tiny, tile_w), x + tile_w / 2, label_y, tiny,
			on ? COL_WHITE : COL_INK, 0);

		draw_radio(x + tile_w / 2, radio_y, 4 * s, opts[i] == in_use);
	}

	// What the highlighted look actually is.
	if (h_blurb)
	{
		char lines[4][64];
		int cols = gfx_text_cols(b.w - pad * 2, tiny);
		int nl = wrap_text(vp_blurb(opts[look_row]), cols, lines, 2);
		for (int i = 0; i < nl; i++)
		{
			char up[64];
			snprintf(up, sizeof(up), "%s", lines[i]);
			gfx_shout(up);
			gfx_text_c(up, b.x + b.w / 2, blurb_y + i * 10 * tiny, tiny, COL_INK, 0);
		}
	}

	if (!vp_available(opts[look_row]))
	{
		gfx_text("FILES MISSING - OPTIONS > REINSTALL LOOKS", b.x + pad,
			b.y + b.h - 10 * tiny, tiny, COL_RED, 0);
	}
}

/*
  Online Covers in one phrase, for the Options row and for the screen's own footer.

  There are five states and only one of them means "this works", which is the whole
  reason it is said in words instead of as On/Off. Taken as arguments rather than read
  from cfg because the Options row is asking about the machine and the screen is asking
  about the edits the player has not saved yet, and those are different questions with
  the same five answers.

  Order matters: the build comes first because nothing a player types can move it, then
  the switch, then the two ways of being switched on and still inert. ss_enabled() in
  chome_ss.cpp draws the same line - available, on, and an account - so "On" here is
  exactly the state in which a cover is really asked for.

  `avail` is a parameter and not a call to ss_available() for one reason, and it is a
  testing reason stated plainly: the devid is compile-time, so a build that can reach this
  code at all is a build in which ss_available() is a constant. The harness is compiled
  with a dummy credential - without one the URL builder would be unreachable dead code -
  and could therefore never see the one state that matters most. Handed the answer, it can
  check all five. See chome_covers_state() at the bottom of this file.

  Every phrase is short on purpose. draw_rows_c() right-aligns the value column without
  clipping it and gives the label whatever is left, so at 240p a long one takes the
  label's room away: the panel is 249 px and the widest of these ("Not Available", 13
  characters) leaves 125 for "ONLINE COVERS", which is exactly the thirteen it needs.
  "Not In This Build" was the first wording, and it left room for nine.
*/
static const char *cov_state_of(int avail, int on, const char *user, int has_pass)
{
	if (!avail) return "Not Available";
	if (!on) return "Off";
	if (!user || !user[0]) return "No Account";
	if (!has_pass) return "No Password";
	return "On";
}

static void draw_options_panel(const chome_profile *p)
{
	panel_box b = draw_panel(p, "Options");

	/*
	  "Advanced" and not "Advanced Settings": seventeen characters beside a value reading
	  "Classic Menu >" is the one pair in this list that cannot share a row, and it came
	  out "ADVANCED SETTING>" at 240p and "ADVANCED SETT>" on a halved 1080p canvas. The
	  value names where the row goes, so the label only has to say what kind of thing is
	  behind it - and this is the row nobody should be looking for by name anyway.
	*/
	static const char *rows_menu[] = { "Cover Art", "Online Covers", "Rescan Library", "Reinstall Looks", "Menu Layout", "Controllers", "Wi-Fi", "Best Settings", "More Settings", "Advanced", "About" };
	static const char *rows_game[] = { "Cover Art", "Online Covers", "Rescan Library", "Reinstall Looks", "Menu Layout", "Controllers", "Wi-Fi", "Best Settings", "More Settings", "Core Settings", "About" };
	const char *const *rows = ig_active ? rows_game : rows_menu;
	char v1[32];
	if (lib_scanning()) snprintf(v1, sizeof(v1), "%d...", lib_scan_progress());
	else snprintf(v1, sizeof(v1), "%d games", lib_item_count());

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

	/*
	  And what the Online Covers row says: whether a cover would really be fetched, which
	  is not the same question as whether the option is on. Read from cfg, not from the
	  staging buffers - this row is about the machine, and the screen behind it is where
	  unsaved edits live.
	*/
	const char *v6 = cov_state_of(ss_available(), cfg.classicui_screenscraper,
		cfg.classicui_ss_user, cfg.classicui_ss_pass[0] != 0);

	const char *vals[] = {
		cfg.classicui_artfetch ? "Fetch Missing" : "Local Only",
		v6,
		v1,
		"Write Files",
		cfg.classicui_profile == 0 ? "Auto" : theme_get()->name,
		v3,
		v2,
		v4,
		v5,
		ig_active ? "Core Options >" : "Classic Menu >",
		"This Menu >"
	};

	/*
	  The list, windowed - because eleven rows are more than the panel at 240p can hold, and
	  draw_rows_c() stops drawing as soon as a row would cross the bottom edge. That is how
	  Close Game came to be invisible on the television this front-end was written for: the
	  panel drew ten rows, no scrollbar, and no word about the eleventh. The window is why
	  the last row is reachable at every profile, and it matters on the shelf now too - that
	  list gained About and is eleven rows as well, where it used to be ten and to fit.
	*/
	/*
	  And what the boot-time configuration check found, if it found anything: a line at
	  the foot of this panel. See support/classicui/chome_cfgrec.h for the check itself.

	  Here rather than on the shelf, and this is the ruling in chome_ini.h applied rather
	  than a new opinion: a panel of technical text about a configuration file, over
	  somebody's cover art, before they have pressed anything, is the thing this
	  front-end exists to remove. Options is where a player goes to look for settings and
	  it is one press away, which is the same depth as the "Best Settings 3 To Change"
	  notice that this front-end already considers a sufficient first-run notice.

	  A line and not a row, for a reason the row above it demonstrates: the value column
	  is right-aligned and about thirteen characters before it walks into the label - it
	  can hold "3 To Change >" and it cannot hold a file name. A player who is told there
	  is a problem and not told where to read about it has been given the anxiety without
	  the fix, and the file name is the whole of the fix.

	  Never while a game is up. The record describes the parse that ran for the core
	  currently loaded, and cfg_parse() runs again per core; in a game it would be
	  answering a question about that game, and the question this check answers is about
	  the menu.
	*/
	int cc_n = ig_active ? 0 : cfgrec_problems();

	int s2 = p->ts_tiny;
	int nrows = ig_active ? OPT_ROWS_GAME : OPT_ROWS_MENU;
	/*
	  Room under the list only for the configuration notice now. The in-game help line went
	  with the row it was about: "THE GAME STAYS LOADED UNTIL YOU CLOSE IT" explained Close
	  Game, and Close Game is on the menu bar. It is said on that screen instead, where the
	  player is actually deciding - see draw_close(). Leaving the sentence here would have
	  cost a row of a list that is eleven rows at every profile, to caption a row that is no
	  longer in it.
	*/
	int foot = cc_n ? 22 * s2 : 0;

	int fit = list_fit(&b, p->row_h, foot, nrows);
	list_track(&opt_top, opt_row, nrows, fit);

	draw_rows(&b, rows + opt_top, vals + opt_top, fit, opt_row - opt_top);
	list_scrollbar(&b, p->row_h, opt_top, fit, nrows);

	if (cc_n)
	{
		/*
		  Amber, which on every other screen here means "away from what this menu
		  recommends" - and a setting the machine is silently not reading is as away from
		  it as a setting gets. Not red: nothing is broken and nothing is about to be
		  lost, which is what red is kept for here - see draw_close().

		  Worded to fit two lines at 240p, where the panel holds twenty-nine characters:
		  "MISTER.INI: 1 PROBLEM. SEE" is twenty-six and "CLASSICUI/CONFIG-REPORT.TXT" is
		  twenty-seven, so neither is ever the wrapped-away line. wrap_text() cuts rather
		  than hyphenates, and the half it would cut here is the file name.
		*/
		char msg[80];
		snprintf(msg, sizeof(msg), "MISTER.INI: %d PROBLEM%s. SEE CLASSICUI/CONFIG-REPORT.TXT",
			cc_n, cc_n == 1 ? "" : "S");

		char wrapped[4][64];
		int nl = wrap_text(msg, gfx_text_cols(b.w - 12 * s2, s2), wrapped, 2);

		int fy = b.y + b.h - foot + 2 * s2;
		for (int i = 0; i < nl; i++)
			gfx_text(wrapped[i], b.x + 6 * s2, fy + i * 10 * s2, s2, COL_YELLOW, 0);
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
	if (w > 44 * gfx_adv(s)) w = 44 * gfx_adv(s);

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
			gfx_text_cols(b.w - 16 * s, s), lines, 3);
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
	  Adding one is the last entry rather than a button on the legend. Dinofly's call, and
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
	if (w > 44 * gfx_adv(s)) w = 44 * gfx_adv(s);

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
			gfx_text_cols(b.w - 16 * s, s), lines, 3);
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
	if (w > 44 * gfx_adv(s)) w = 44 * gfx_adv(s);

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
  The dialog for a disc: the one screen this front-end has about a physical disc, whether
  the disc is sitting in the drive or already playing.

  It is the disc's card. A shelf game gets a cover, a title, A to start it and Down to its
  save states; a disc has no card because nothing scanned it, so this is where those four
  things live for it. Hence the shape: the game's name, a large disc under it, and the
  actions as buttons side by side - Play, and Options for the core choice. The rows it
  replaced ("Play on PlayStation", "Use a different core") said the same two things in a
  list, and put the disc - the only picture on the screen - in a column beside them at a
  third of the size the panel could afford.

  Two sources, one dialog. See disc_dlg_get(): from the shelf the drive answers, and over
  a running disc the drive is the core's and the mount's published identity answers
  instead. Everything below reads that struct, so the layout, the title, the scan and the
  save states are the same code either way.

  Playing is real core by core: a system whose firmware-side daemon has been taught to
  read sectors from the drive is in disc_playables below. A system whose daemon has not
  been is still named - the identification is this screen's one claim and hiding the
  console the disc actually belongs to would turn an honest answer into a lie - but the
  Play button draws dim and refuses, and the line under the title says "(not yet)". A
  button that silently did nothing would be worse than one that explains itself.
*/

// Defined further down, with the rest of the navigation.
static void go_screen(int s);

// Defined further down, with the launch path: whether this system's daemon can
// read from the drive - the disc_playables table has the details.
static int disc_wired(int sysidx);

#define DISC_ROW_MAX 8

#define DACT_PLAY   0
#define DACT_NONE   1   // named but not playable: the daemon work has not been done
#define DACT_RIP    2   // copy the disc into that system's games folder

static int disc_row = 0;                      // the Options cursor
static int disc_picking = 0;                  // 0: the offer, 1: the Options list
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

/* ------------------------------------------------------ ripping a disc to the card --- */

/*
  Which shelf systems a rip can be *for*, and how their cue parser has to be spoken to.

  This table is the set whose core loads a .cue and its tracks OFF THE CARD - which is a
  different set from the ones that can be handed the pressed disc, and deliberately so.
  Reading the sectors is this front-end's own work; the row a system gets here depends on
  nothing but whether its core can read back what we would write. The flag is the one
  thing about them that differs in what the sheet may say:

    psx      MODE1/2352 and MODE2/2352 are both understood and both map to 2352
             (psx.cpp:250), so the sheet says whichever mode the sectors actually are.
    md       Mega CD's parser knows MODE1/2048 and MODE1/2352 and nothing else, and only
             looks for a token on track 1 at all (megacdd.cpp:147-166). A MODE2/2352 token
             leaves the sector size unset and the loader falls through to sniffing the
             file's first bytes - right by luck rather than by contract.
    tg16     PC Engine CD's is the same two tokens, per track (pcecdd.cpp:157-174).
    neogeo   has no parser of its own: neocd_set_image() calls Mega CD's cdd_t
             (neogeocd.cpp:192), so it is md's rules exactly.
    saturn   reads MODE1/2048, MODE1/2352 and MODE2/2352, per track and per track's own
             sector size (saturncdd.cpp:169-192), so it is psx's rules and not Mega CD's -
             measured from that parser rather than assumed from the console's neighbours,
             because a Saturn disc's second session is where a wrong token would show.

  Saturn is here even though no core here can play a Saturn disc from the drive, and that
  is the whole point of the split described over disc_console_id(): a copy is written by
  us and read back by a core off the card, so saturncdd.cpp not being able to stream the
  drive is not a fact about copying. games/Saturn is a shelf system with a folder and a
  cue reader; that is everything a copy needs.

  A system that is not in this table still gets no Rip row, and that is a real answer
  rather than laziness: an MSU-1 SNES disc's core wants the .sfc off the disc and not a
  copy of the disc, so a cue sheet in SNES/ would be a folder that never loads. 3DO and
  CD-i have no shelf entry at all - no folder, so nowhere to put it.

  Where the rip goes is a games folder - lib_sys_games_dir(), the same directory the
  scanner walks - because a copy that the shelf cannot see is not worth making. For
  PlayStation and Saturn that is the system's own folder and `dest` is empty.

  For the other three it is not, and that is what `dest` is for. The row the dialog
  settles on for a Mega CD disc is "md", because Mega Drive is where a player looks for
  Sega and because that row's core is what plays the pressed disc; but a folder of tracks
  in games/Genesis is not a Mega Drive game, `md` accepts no .cue, and the copy was
  written correctly and then never appeared as a card - the one place this feature used to
  be knowingly incomplete. games/MegaCD is its own shelf system now, with its own core and
  its own mount slot, so that is where the copy belongs and where the card comes from. PC
  Engine CD and Neo Geo CD had the same hole and are redirected the same way.

  `dest` names a system rather than a folder so the destination is resolved through
  lib_sys_games_dir() like any other - a card whose systems file has dropped that system
  has nowhere to put the copy, and then there is no Rip row rather than a copy nothing can
  see. The destination is itself in this table, so the mode token is decided by the row
  that actually receives the sheet.
*/
struct rip_target
{
	const char *sysid;
	int mode1_only;
	const char *dest;      // the system whose games folder receives it; 0 = its own
};

static const rip_target rip_targets[] =
{
	{ "psx",      0, 0          },
	{ "saturn",   0, 0          },
	{ "md",       1, "megacd"   },
	{ "megacd",   1, 0          },
	{ "tg16",     1, "pcecd"    },
	{ "pcecd",    1, 0          },
	{ "neogeo",   1, "neogeocd" },
	{ "neogeocd", 1, 0          },
};

static const rip_target *rip_target_for(int sysidx)
{
	const chome_sys *s = (sysidx >= 0) ? lib_sys(sysidx) : 0;
	if (!s) return 0;

	for (unsigned i = 0; i < sizeof(rip_targets) / sizeof(rip_targets[0]); i++)
	{
		if (!strcasecmp(rip_targets[i].sysid, s->id)) return &rip_targets[i];
	}
	return 0;
}

/*
  The system a rip of this system's disc is written into, which is itself unless the row
  redirects it. -1 when there is nowhere: either the disc cannot be copied at all, or the
  folder it would go into belongs to a system this card's table does not carry.

  Everything downstream - the row's wording, the folder, the mode token and the card that
  appears afterwards - is asked about the answer to this rather than about what the dialog
  settled on, so there is one place where "which console" turns into "which folder".
*/
static int rip_dest_sys(int sysidx)
{
	const rip_target *rt = rip_target_for(sysidx);
	if (!rt) return -1;
	if (!rt->dest) return sysidx;

	int dx = disc_sys_by_id(rt->dest);
	if (dx < 0) printf("ClassicUI: rip: no %s system on this card to copy into\n", rt->dest);
	return dx;
}

/*
  Who the running rip is about, captured when it starts.

  This exists for the same reason the disc_dlg comment below gives for the *running* disc,
  and it is the same trap: a rip owns the drive, so the detection helper is stopped for the
  whole length of it and disc_state() is ABSENT while disc_type(), disc_serial() and
  disc_label() are all empty. A progress screen built from the drive would therefore be
  blank in exactly the case it exists for. So the three facts the screen needs - the name
  to show, the key the artwork is filed under, and which console's folder it went to - are
  taken once, at the press, and drawn from here afterwards.
*/
static char rip_title[DISC_TITLE_LEN];
static char rip_key[128];
static int  rip_sysidx = -1;

// One press from starting a rip over a folder that is already there, and when that offer
// expires. The same two-press shape as deleting a suspend point; see del_arm_slot.
static int rip_over_arm = 0;
static unsigned long rip_over_until = 0;

// Whether the disc dialog is showing a rip rather than a disc: one in progress, or one
// that has finished and has something to say that the player has not dismissed.
static int rip_showing()
{
	return rip_busy() || rip_reportable();
}

// The one disc_target_speed() forward-declares, so the animation above does not have to
// know where in this file the rip lives.
static int rip_busy_ui() { return rip_busy(); }

/*
  Whether there is a disc for the front-end to point at, which during a rip is a different
  question from whether the *drive* has one.

  Everywhere this replaces asked disc_state() directly, and every one of them was right
  until a rip could own the drive: the detection helper is stopped for the whole length of a
  copy, so disc_state() is ABSENT throughout. Left as it was, the badge would vanish the
  instant the rip started, Up from the shelf would go nowhere, and - worst of the three -
  the branch that leaves SCR_DISC when the disc goes would throw the player off the
  progress screen at the moment it appeared, with no way back to it and a helper still
  copying 700 MB in the background.
*/
static int disc_or_rip_present()
{
	return disc_state() != DISC_ABSENT || rip_showing();
}

/*
  How much of the disc is revealed, 0..1024, which is the pie's fraction.

  Straight from the sectors written, because that is the only quantity that advances at the
  rate the work does - see rip_percent(). Deliberately finer than the percentage under the
  disc: at 288 px across, a pie that moved in whole percent would visibly step, and the
  sectors are there to be counted.
*/
#define DISC_REVEAL_FULL 1024

static int rip_reveal()
{
	const rip_status *st = rip_state();

	if (!rip_busy()) return DISC_REVEAL_FULL;
	if (st->total <= 0) return 0;

	long long r = (long long)st->done * DISC_REVEAL_FULL / st->total;
	if (r < 0) r = 0;
	if (r > DISC_REVEAL_FULL) r = DISC_REVEAL_FULL;
	return (int)r;
}

/*
  Everything a disc's pixels are a function of, folded into one number, so the spin
  repaint can tell "the timer fired" from "the disc will actually look different".

  The spin tick fires every GFX_DISC_PART_MS, and which quantisation of the turn goes
  into the number is exactly the question "which disc is on screen". The dialog's disc
  resolves the turn to 256 positions (disc_step_fine), so with the dialog up nearly
  every tick is a genuinely new frame - that is the 60fps he asked for, and the skip
  correctly all but disappears there. Everywhere else the only turning disc is the
  badge, a 32-cell sprite that quantises to 64 positions whatever it is handed - so the
  badge screens keep the coarse step here, and with it the skip: at the slow rate three
  ticks in four would compose, blit and copy the badge's rectangle byte-identical, which
  on the device was most of what a disc on the shelf cost. A fine step in the number on
  those screens would repaint sprite frames that cannot differ.

  The reveal is in the number for the one thing inside the rectangle that moves between
  angles: the rip's pie follows the sectors, not the clock. Both step functions are
  memoised on the millisecond off one phase accumulator, so asking here and drawing a
  moment later cannot disagree - and the draw itself records this same number, which is
  what keeps the two honest whichever quantisation applied.

  What is NOT in the number is the badge's breath and its ring's pulse, which are
  continuous in the clock - so the skip is never taken while the badge has focus (the
  dispatch checks SCR_DISCBAR). On every other screen a disc's pixels are this number
  and nothing else. Byte-identity against a full repaint is untouched: a skipped tick
  presents nothing at all.
*/
static int disc_drawn_sig = -1;

static int disc_spin_sig()
{
	int step = (screen == SCR_DISC && !disc_picking) ? disc_step_fine() : disc_step();
	return step | (rip_reveal() << 8);
}

/*
  Who the dialog is about, gathered once per draw and per press.

  There are two moments a player wants this screen and only one of them can ask the drive.

  Sitting on the shelf, the detection helper owns /dev/sr0 and disc_state(), disc_type(),
  disc_serial() and disc_label() describe what is in it.

  Playing that disc, the drive belongs to the core: the helper was stopped before the
  launch and must not come back - see disc_launch() and core_holds_disc() - so
  disc_state() is DISC_ABSENT and the type and both identifiers are empty. A dialog built
  off the drive would therefore be *blank* in exactly the case that matters most, which is
  the trap this struct exists to avoid. The running disc is described from what the mount
  published instead: ig_item, whose path is the key disc_ident_read() lifted out of
  PHYSICAL_DISC_IDENT_FILE, which is deliberately the same name the core's save files use.

  Copied into buffers rather than kept as pointers on purpose: disc_title_for() hands back
  a slot in a small cache that a later query can reuse, and one frame asks it more than
  once.
*/
struct disc_dlg
{
	int running;                 // playing: the drive is the core's, not ours
	int sysidx;                  // the shelf system that takes it, or -1
	chome_item *susp;            // what Down is about, or 0 when there is nothing to reach
	char title[DISC_TITLE_LEN];  // the game, by the best name anything here knows
	char sub[64];                // what it is, or how the identification is getting on
	char key[128];               // what the scan and the savestates are filed under
};

/* ------------------------------------ the disc as an item, for the suspend strip --- */

/*
  A disc that has not been played yet, as something the suspend strip can be about.

  It is NOT on the shelf and it is not meant to be. This item is never added to items[] and
  never appears in a view, so no amount of Left, Right or Down on the shelf can land on it;
  the only way to it is the disc dialog, which is what Dinofly decided the disc's one entry
  point should be. susp_target() consults it solely behind the susp_is_disc latch, and that
  latch is only ever set by Down inside the dialog. These three were called disc_shelf_item,
  disc_shelf_bind() and disc_shelf_susp(), which said the opposite of all of that and sent
  every reader off to check whether the shelf had grown a disc card.

  Down from this dialog used to be offered over a *running* disc only, on the reasoning
  that a disc in the drive has no published identity until the mount writes one. For most
  discs that is exactly right: save_name_of() in physical_disc.cpp falls back to the volume
  label and, failing that, to a hash of the table of contents - and the front-end has no
  table of contents at all, because the detection helper owns the drive and hands back
  sectors rather than a TOC.

  PlayStation is the exception, and it is the case that matters here: for a PSX disc that
  name *is* the serial, and disc_serial() has the serial while the disc merely sits in the
  drive - dug out of the boot configuration by the same prefix list, the same length bounds
  and the same normalisation as physical_disc_disc_serial(). So savestates/PSX/<serial>_N.ss
  is derivable from the shelf, and Dinofly owns PSX originals, which is what this is for.

  Offered only where the name is *provable*, because the failure mode is silent. A key that
  is nearly right lists files that are not this disc's states, and resume_poll() - which
  compares the armed name against the one the mount publishes - then declines to resume
  without a word. So two conditions, and Down stays hidden unless both hold:

    The disc is a PlayStation disc and the core it would go to is the PlayStation one.
    psx.cpp asks physical_disc_save_name() for the PSX name; a disc forced onto some other
    core through Options gets that core's answer instead, which is a different key - so a
    hand-picked core withdraws the offer rather than filing states under a name nothing
    will ever look for again.

    sanitize_name() cannot change the serial. That function is what physical_disc.cpp runs
    the serial through, and it drops spaces and punctuation and puts an underscore where it
    dropped them. Rather than keep a second copy of that rule here - the copy that would
    rot the first time the real one changed - this accepts only a serial already in the
    form sanitize_name() would leave untouched. Every real serial is: four letters, a dash
    and five digits.
*/
static chome_item disc_susp_item;

// True when sanitize_name() would hand this string straight back, so it can be used as
// the save name without reproducing that function here. Deliberately stricter than it
// needs to be: it also refuses the dot sanitize_name() allows, and a serial never has one
// once disc_serial_at() has taken it out.
static int disc_name_is_sanitised(const char *s)
{
	if (!s || !s[0]) return 0;

	for (const char *q = s; *q; q++)
	{
		int ok = (*q >= '0' && *q <= '9') || (*q >= 'A' && *q <= 'Z')
			|| (*q >= 'a' && *q <= 'z') || *q == '-';
		if (!ok) return 0;
	}
	return 1;
}

/*
  Bound from the drive on every pass through the dialog, so the item follows whatever is
  actually in there. No slot refresh here: this runs per draw, and the strip's opener
  already asks lib_refresh_slots() once, where four stats of the card are worth paying for.
*/
static void disc_susp_bind(const disc_dlg *d)
{
	disc_susp_item.path[0] = 0;

	if (d->running || disc_type() != DISC_T_PSX) return;
	if (d->sysidx < 0 || d->sysidx != disc_sys_by_id(disc_system_id(DISC_T_PSX))) return;
	if (!disc_wired(d->sysidx)) return;
	if (!disc_name_is_sanitised(disc_serial())) return;

	memset(&disc_susp_item, 0, sizeof(disc_susp_item));
	disc_susp_item.kind = IT_GAME;
	disc_susp_item.sysidx = (int16_t)d->sysidx;
	snprintf(disc_susp_item.path, sizeof(disc_susp_item.path), "%s", disc_serial());
	snprintf(disc_susp_item.title, sizeof(disc_susp_item.title), "%s", d->title);
}

static chome_item *disc_susp_item_get()
{
	// The disc has to still be in there. Tying it to the drive rather than to a flag
	// somebody has to clear is what stops the strip outliving an eject with a stale item
	// under the previous disc's name.
	if (disc_state() == DISC_ABSENT || !disc_susp_item.path[0]) return 0;
	return &disc_susp_item;
}

/*
  The dialog, describing a rip instead of a disc.

  Everything here comes from the snapshot and from the child's published line, and nothing
  from the drive - see rip_title above for why there is nothing there to ask. The key is
  carried through so disc_draw_face() still finds the disc's scan: the picture is the same
  picture, and a rip that drew the generated face while the scan sat on the card would look
  like a different disc from the one the player pressed A on.
*/
static void disc_dlg_from_rip(disc_dlg *d)
{
	const rip_status *st = rip_state();

	d->sysidx = rip_sysidx;
	snprintf(d->title, sizeof(d->title), "%s", rip_title);
	snprintf(d->key, sizeof(d->key), "%s", rip_key);

	const chome_sys *sc = (rip_sysidx >= 0) ? lib_sys(rip_sysidx) : 0;

	if (rip_busy())
	{
		/*
		  The percentage, and the count of what would not read if there is one. Both in the
		  line under the title rather than on the disc: the pie is the shape of the answer
		  and the number is the answer, and the disc has no room for text at 240p.
		*/
		/*
		  Short enough that it cannot be clipped, which is why it no longer says where
		  it is copying to.

		  "Copying to the card - 0%" is twenty-four characters, and the owner
		  photographed it on a 240p television reading "Copying to the cardc" - the
		  line cut with gfx_clip()'s marker on the end. The odd part is that the LONGER
		  "Copying to the card - 16%" rendered whole a moment later, so the panel is
		  narrower in the first frames than it settles at, and a line that fits the
		  finished dialog can still be cut while it is arriving. Rather than chase which
		  frame, both of these now fit the narrow case: twenty characters, worst values
		  included.
		  The other one was worse and nobody had hit it yet: "Copying 45% - 7 unreadable
		  so far" is thirty-five characters and would have been cut on every profile.
		  The pie says what is being copied and where; these say how far along.
		*/
		if (st->bad) snprintf(d->sub, sizeof(d->sub), "%d%% - %d unreadable",
			rip_percent(st), st->bad);
		else if (!st->total) snprintf(d->sub, sizeof(d->sub), "Reading the disc");
		else snprintf(d->sub, sizeof(d->sub), "Copying - %d%%", rip_percent(st));
		return;
	}

	switch (st->state)
	{
	case RIP_DONE:
		/*
		  The count first when there is one. A rip with unreadable sectors that reported
		  "Done" is the failure this whole path is written to avoid: the player would find
		  out from a core that hangs, weeks later, with nothing to connect it to.
		*/
		if (st->bad) snprintf(d->sub, sizeof(d->sub), "Copied, but %d sectors would not read", st->bad);
		else if (sc) snprintf(d->sub, sizeof(d->sub), "Copied to the %s folder", sc->name);
		else snprintf(d->sub, sizeof(d->sub), "Copied to the card");
		break;

	case RIP_CANCELLED:
		snprintf(d->sub, sizeof(d->sub), "Stopped - nothing was kept");
		break;

	case RIP_NOSPACE:
		snprintf(d->sub, sizeof(d->sub), "No room: needs %d MB, %d MB free",
			st->need_mb, st->free_mb);
		break;

	case RIP_EXISTS:
		// This disc and not this game: another disc of the same game is added beside it,
		// so the only thing that gets this far is a copy of the disc in the drive.
		snprintf(d->sub, sizeof(d->sub), "This disc is already copied");
		break;

	default:
		snprintf(d->sub, sizeof(d->sub), "The copy failed - nothing was kept");
		break;
	}
}

static void disc_dlg_get(disc_dlg *d)
{
	memset(d, 0, sizeof(*d));
	d->sysidx = -1;

	// A rip first, because it owns the drive and everything below this reads the drive.
	if (rip_showing()) { disc_dlg_from_rip(d); return; }

	chome_item *run = ig_running_disc();
	if (run)
	{
		d->running = 1;
		d->susp = run;
		d->sysidx = run->sysidx;
		snprintf(d->key, sizeof(d->key), "%s", run->path);

		/*
		  The title table first, then the name the mount published. ig_item.title is the
		  volume label, which is whatever the mastering engineer typed - "PLAYSTATION" as
		  often as the game - so a real title beats it when the card has one. Exactly the
		  order disc_display_name() uses for the disc in the drive, so the same disc reads
		  the same before and after it is playing.
		*/
		const char *t = disc_title_for(d->key);
		snprintf(d->title, sizeof(d->title), "%s", (t && *t) ? t : run->title);

		const chome_sys *sc = lib_sys(run->sysidx);
		if (sc) snprintf(d->sub, sizeof(d->sub), "%s", sc->name);
	}
	else
	{
		/*
		  Serial first, label second, matching disc_display_name() and disc_art_path():
		  they are the only two handles a pressed disc gives us, and PlayStation discs
		  carry a serial while PC Engine and Neo Geo ones do not.
		*/
		snprintf(d->key, sizeof(d->key), "%s",
			disc_serial()[0] ? disc_serial() : disc_label());

		snprintf(d->title, sizeof(d->title), "%s", disc_display_name());

		snprintf(d->sub, sizeof(d->sub), "%s",
			(disc_state() == DISC_SPINNING) ? "Reading the disc"
			: (disc_state() == DISC_UNKNOWN) ? "Unrecognised disc"
			: disc_type_name(disc_type()));

		d->sysidx = (disc_chosen_sys >= 0) ? disc_chosen_sys
			: disc_sys_by_id(disc_system_id(disc_type()));

		/*
		  The console this firmware would load it on, when that is a different fact from
		  the console that pressed it - a Mega CD disc chosen onto the Mega Drive card, or
		  a core the player picked by hand through Options.
		*/
		const chome_sys *sc = (d->sysidx >= 0) ? lib_sys(d->sysidx) : 0;
		if (sc && !disc_wired(d->sysidx))
		{
			// Named, and marked. Hiding the console the disc belongs to would make the
			// identification - this screen's one claim - into a lie.
			snprintf(d->sub, sizeof(d->sub), "%s (not yet)", sc->name);
		}
	}

	/*
	  Nothing knows a name for it, which is the whole of the SPINNING state and is also what
	  an unidentified disc leaves behind: no title table hit, no volume label, no serial, and
	  disc_type_name(DISC_T_NONE) is the empty string. Promote what we do know into the title
	  rather than drawing the panel's largest line blank with the answer whispered under it.
	*/
	if (!d->title[0])
	{
		snprintf(d->title, sizeof(d->title), "%s", d->sub);
		d->sub[0] = 0;
	}

	// The same line twice reads as a drawing fault rather than as two facts, and it
	// happens whenever nothing knows the disc by any name but its console's.
	if (!strcmp(d->title, d->sub)) d->sub[0] = 0;

	/*
	  And what Down is about, when the disc is not playing. After the title is settled, so
	  the strip's header reads the same name the dialog above it does - and after `sub`, so
	  a disc still being read cannot be bound under a name nothing has yet.
	*/
	if (!d->running)
	{
		disc_susp_bind(d);
		d->susp = disc_susp_item_get();
	}

	/*
	  And ask for the disc's own artwork, which is the one thing on this screen nothing else
	  would ever set in motion.

	  Here rather than in draw_disc() because a draw should not start a network request, and
	  here rather than at the launch because the dialog is where the picture is wanted. It is
	  safe to call on every pass: disc_art_request() answers from the file when it already
	  has one, refuses outright unless ss_enabled() and classicui_artfetch are both on, and
	  tries at most once per key per session so a disc the database has never heard of is not
	  asked for again on the next frame.

	  This call is why it exists. The fetch and the drawing were written either side of
	  disc_art_path() - one writes that file, the other reads it through art_thumb() - and
	  both were complete and correct while nobody asked for anything, so the dialog quietly
	  drew the fallback disc for ever. A seam named by a path does not say who knocks.

	  No longer the *first* thing to ask, though, and it is worth being clear about why it
	  stays. disc_art_prefetch() below asks the moment the disc is identified, so by the time
	  this screen is opened the picture is usually already on the card. That ask is one ask at
	  one instant, and it can be refused for reasons that have nothing to do with this disc -
	  a cover download in flight, another disc's scan still downloading, or ScreenScraper
	  switched on in the settings *after* the disc went in. None of those mark the key as
	  tried, so the dialog asking again is what recovers them. It is also what covers the
	  running disc, whose key comes from the mount rather than the drive and which the
	  prefetch never sees at all.
	*/
	/*
	  The console the disc BELONGS to, not the one that would load it.

	  d->sysidx is the launch answer, and for a Saturn disc there is no launch answer -
	  saturncdd cannot stream from a drive, so the Play row is refused and sysidx is -1.
	  Asking for artwork through it therefore handed disc_art_request() a null system,
	  ss_system_id() returned nothing, and the request returned without so much as a log
	  line. A Sega Rally disc named itself correctly on screen and never asked anyone for
	  its picture; the owner reported it as "no art", which is exactly what it looked like.

	  Artwork is a question about which game this is. Whether a core can read the drive
	  has nothing to do with it, and the two answers were only ever the same by accident.
	  A disc the player has pointed at a core by hand still wins, because that is a
	  statement about what the disc IS.
	*/
	int art_sx = (disc_chosen_sys >= 0) ? disc_chosen_sys
		: (d->running ? d->sysidx : disc_sys_by_id(disc_console_id(disc_type())));
	if (art_sx < 0) art_sx = d->sysidx;

	if (d->key[0]) disc_art_request(d->key, lib_sys(art_sx) ? lib_sys(art_sx)->id : 0,
		d->title[0] ? d->title : d->key);
}

#ifdef CHOME_HOST_TEST
/*
  The two lines the disc dialog puts on screen, for the harness. Compiled out of the
  firmware, like the rotation hooks further up.

  Everything else about this dialog is checked by dumping the canvas and comparing
  pixels, which is the right test for a layout and the wrong one for a sentence: a
  picture cannot say whether a disc still being read says so or merely says nothing,
  and that distinction - "Reading the disc" against a bare console name against
  "Unrecognised disc" - is the one thing about this screen the player complained
  about. So the strings are read as strings.

  Not a second derivation of them either: it calls the same disc_dlg_get() the drawing
  does, so a check here cannot pass while the screen says something else.
*/
void disc_test_dlg_text(char *title, int tsz, char *sub, int ssz)
{
	disc_dlg d;
	disc_dlg_get(&d);

	if (title && tsz > 0) snprintf(title, (size_t)tsz, "%s", d.title);
	if (sub && ssz > 0) snprintf(sub, (size_t)ssz, "%s", d.sub);
}
#endif

/*
  Ask for the disc's scan as soon as the disc is known, rather than when the dialog opens.

  His instruction, and the shape of it is his too: the picture should be on its way the
  moment we know what the disc is, so that opening the dialog finds it there instead of
  showing the generated face for however long a query, a download and a scale take. If it
  is not ready in time the dialog still opens on the generated face and swaps to the scan
  when it lands - see disc_art_take_ready() - so this is about the common case being right,
  not about the late case being broken.

  Called from exactly one place: the branch in chome_handle() that runs when disc_poll()
  reports the drive's state has changed. That is the only moment "this disc has become
  identified" is a fact rather than something to re-derive by comparing against what we
  saw last frame, and it is the reason this is not in draw_disc() or in the dialog: both
  of those are per frame, and a request per frame is what da_already_tried() exists to
  paper over rather than something to rely on.

  It does not touch the drive and cannot block. Everything it reads - the state, the type,
  the serial, the label - the detection helper already wrote into a file in /tmp and
  disc_poll() has already read; there is no ioctl anywhere near this. That matters more
  here than it reads: every ioctl on /dev/sr0 serialises behind whatever the drive is
  doing, and going to the drive from the front-end is what froze the console twice. See
  the top of chome_disc.h. What this does start is network and card work in a forked curl,
  which is why it belongs on the state change and nowhere near the per-frame path.

  Nor while a core owns the drive: the caller is inside `drive_is_ours`, so a disc handed
  over to a core cannot bring us back here - and over a running disc there is nothing for
  this to ask about anyway, because the drive's state went with the handover and the
  running disc's identity comes from the mount instead.

  The key is disc_serial() else disc_label(), which is what disc_dlg_get() will use and
  what disc_art_path() files the picture under. Not a choice: a key that differs by one
  character from the dialog's puts the picture somewhere nothing will ever look for it,
  and it would look exactly like a fetch that had failed.
*/
static void disc_art_prefetch()
{
	// READY only. SPINNING has no identifiers yet, UNKNOWN has no system to scrape as,
	// and ABSENT is an eject.
	if (disc_state() != DISC_READY) return;

	const char *key = disc_serial()[0] ? disc_serial() : disc_label();
	if (!key[0]) return;

	// The system the disc would load on, exactly as the dialog derives it. No hand-picked
	// core can be involved: disc_chosen_sys is forgotten on every drive change, which is
	// the same event that got us here.
	int sysidx = disc_sys_by_id(disc_system_id(disc_type()));
	const chome_sys *sc = (sysidx >= 0) ? lib_sys(sysidx) : 0;

	// And the name to match on, which is the title table's answer when it has one - the
	// same order disc_display_name() and the dialog use. disc_art_request() refuses
	// everything else that has to hold: the fetch option, an account, a systemeid it
	// recognises, one attempt per key per session.
	const char *name = disc_display_name();

	disc_art_request(key, sc ? sc->id : 0, name[0] ? name : key);
}

/*
  Whether A will hand this disc to a core.

  Only ever from the drive. disc_play_for() picks its row by disc_type(), and over a
  running disc the type went with the rest of the drive state - so it would match only the
  any-disc rows and mark every typed one unplayable. A Mega CD disc, mid-game, would be
  told by its own dialog that its own core cannot read it. The running case does not offer
  the launch at all; A resumes instead, which is what pressing it inside a game means
  everywhere else in this front-end.
*/
static int disc_can_play(const disc_dlg *d)
{
	return !d->running && d->sysidx >= 0 && disc_wired(d->sysidx);
}

static void disc_build_rows()
{
	disc_nrows = 0;

	/*
	  Only systems that are actually in this library. A card offering to load the Neo Geo
	  core on a machine with no Neo Geo core would be a dead end.
	*/
	const char *ids[16];
	int n = disc_capable_systems(ids, 16);

	for (int i = 0; i < n && disc_nrows < DISC_ROW_MAX; i++)
	{
		int sx = disc_sys_by_id(ids[i]);
		if (sx < 0) continue;

		const chome_sys *sc = lib_sys(sx);
		int wired = disc_wired(sx);
		snprintf(disc_rowtext[disc_nrows], sizeof(disc_rowtext[0]),
			wired ? "%s" : "%s (not yet)", sc->name);
		disc_rowact[disc_nrows] = wired ? DACT_PLAY : DACT_NONE;
		disc_rowsys[disc_nrows] = sx;
		disc_nrows++;
	}

	/*
	  And "copy it to the card", for the console the disc belongs to.

	  One row rather than one per system, and it targets the console the disc was identified
	  as - or the one the player picked by hand through the rows above. A rip has to go into
	  some system's games folder in some core's format, and this screen already has an
	  answer to which; offering the same rip once per console so the player can pick the
	  wrong folder is not a choice worth giving them.

	  Which console the copy is for is asked of the DISC - disc_console_id() - and not of
	  the rows above, which answer a different question: whether a core here can be handed
	  the spinning drive. Those two used to be the same call, and the cost was a Saturn
	  disc. It is identified, games/Saturn is a shelf system, saturncdd.cpp reads the sheet
	  we would write - and the screen offered no Copy, because the copy was being asked
	  which core could play it. Copying is our own helper reading sectors and writing a cue
	  and some .bins; no daemon is involved and none of its limits belong here. A Saturn
	  disc now gets no Play row and a Copy row, which is not a contradiction: it is the two
	  questions giving their own answers.

	  disc_chosen_sys stays as an override, so a player who picked a core by hand for an
	  unidentified disc copies it into that console's folder rather than nowhere.

	  Absent rather than dim where there is still no answer, which is an unrecognised disc,
	  a disc whose console has no row in rip_targets, and one whose destination system is
	  missing from this card. A dim row says "this could work and does not"; here there is
	  nowhere to put the copy at all, so there is nothing for the row to be about and the
	  rows above already say so.

	  The row names the folder's console and not the disc's, which for three of the five is
	  not the same word: a Mega CD disc plays on the Mega Drive row above and copies into
	  Mega CD. Saying "Copy to Mega CD" is what tells the player where the card will turn
	  up, which is the only thing about the destination they can act on. rip_dest_sys().
	*/
	int rip_sx = rip_dest_sys((disc_chosen_sys >= 0) ? disc_chosen_sys
		: disc_sys_by_id(disc_console_id(disc_type())));

	const rip_target *rt = rip_target_for(rip_sx);

	if (rt && disc_nrows < DISC_ROW_MAX)
	{
		const chome_sys *sc = lib_sys(rip_sx);

		/*
		  Armed, the row says what the second press will do to what is already there. The
		  same shape the suspend strip's delete uses: the offer is written into the thing
		  the cursor is on rather than into a dialog of its own, and it expires.
		*/
		if (rip_over_arm && !CheckTimer(rip_over_until))
			snprintf(disc_rowtext[disc_nrows], sizeof(disc_rowtext[0]),
				"Replace this disc? Press again");
		else
			snprintf(disc_rowtext[disc_nrows], sizeof(disc_rowtext[0]),
				"Copy to %s", sc ? sc->name : "the card");

		disc_rowact[disc_nrows] = DACT_RIP;
		disc_rowsys[disc_nrows] = rip_sx;
		disc_nrows++;
	}
}

static int disc_rows()
{
	disc_build_rows();
	return disc_nrows;
}

/* --------------------------------------------------------- the two buttons --- */

/*
  Two, side by side, in place of the list of rows.

  A list of one or two entries is a list only in the code: on screen it was two lines of
  left-aligned text in a column beside the disc, which is why the disc could only have a
  third of the panel. Buttons stack horizontally, so the whole width above them is the
  disc's, and they say what they do in two words rather than in a sentence per row.
*/
#define DBTN_MAX 2

#define DBTN_ACT  0        // Play the disc, or Resume the game it is already running
#define DBTN_OPTS 1        // the Options list: which core, and copying it to the card
#define DBTN_STOP 2        // ...and, while a rip runs, the only press there is
#define DBTN_OK   3        // dismiss what a finished rip had to say

static int disc_btn = 0;
static char disc_btntext[DBTN_MAX][12];
static int  disc_btnact[DBTN_MAX];
static int  disc_btndim[DBTN_MAX];
static int  disc_nbtn = 0;

// One press from stopping the rip, and when the offer expires. A rip is minutes of work
// and the drive is spinning; asking twice is the same courtesy the suspend strip's delete
// pays a file that took a second to write.
static int rip_stop_arm = 0;
static unsigned long rip_stop_until = 0;

static void disc_build_btns(const disc_dlg *d)
{
	disc_nbtn = 0;

	/*
	  A rip has one button and it is not Play. Everything the offer's two buttons do needs
	  the drive, which the rip has, so leaving them there would be two presses that cannot
	  work over the one screen where the drive is definitely busy.
	*/
	if (rip_showing())
	{
		int armed = (rip_stop_arm && !CheckTimer(rip_stop_until));

		if (rip_busy())
			snprintf(disc_btntext[disc_nbtn], sizeof(disc_btntext[0]), "%s",
				armed ? "Sure?" : "Stop");
		else
			snprintf(disc_btntext[disc_nbtn], sizeof(disc_btntext[0]), "OK");

		disc_btnact[disc_nbtn] = rip_busy() ? DBTN_STOP : DBTN_OK;
		disc_btndim[disc_nbtn] = 0;
		disc_nbtn++;
		return;
	}

	/*
	  "Resume" rather than "Play" over a running disc, and it is the same word the shelf
	  and the savestate strip use for the same press: the game is already going, so A
	  closes the menu and drops back into it - see the SCR_HOME case of confirm().
	*/
	snprintf(disc_btntext[disc_nbtn], sizeof(disc_btntext[0]), "%s",
		d->running ? "Resume" : "Play");
	disc_btnact[disc_nbtn] = DBTN_ACT;
	disc_btndim[disc_nbtn] = (!d->running && !disc_can_play(d));
	disc_nbtn++;

	/*
	  And the core choice, from the drive only.

	  Not over a running disc, for the reason disc_can_play() gives: without the type,
	  every core whose entry names one would be listed "(not yet)", so the chooser would
	  be a screenful of wrong answers. The core is also already chosen at that point - it
	  is the one playing - and swapping it means restarting the game, which is what
	  closing the game and coming back is for.
	*/
	if (!d->running)
	{
		snprintf(disc_btntext[disc_nbtn], sizeof(disc_btntext[0]), "Options");
		disc_btnact[disc_nbtn] = DBTN_OPTS;
		disc_btndim[disc_nbtn] = 0;
		disc_nbtn++;
	}
}

static int disc_dlg_legend(legend_pair *out, int max)
{
	int n = 0;

	/*
	  A rip, before the Options list, because a rip can be started from that list and the
	  screen it leaves behind is this one. Leaving is offered and does not stop the rip: the
	  helper is a process of its own, the badge in the corner keeps turning while it works,
	  and coming back to the dialog finds the progress where it got to. A ten-minute copy
	  that pinned the player to one screen would be the worse of the two designs.
	*/
	if (rip_showing())
	{
		int armed = (rip_stop_arm && !CheckTimer(rip_stop_until));

		// The word changes and the pad glyph's colour does not: `col` here is the button's
		// own colour on the pad, and a red A chip would be this front-end inventing a
		// button. The armed state is said in the label, exactly as the delete hint says it.
		if (n < max)
		{
			out[n++] = rip_busy() ? lp(LBL_A, armed ? "Stop it" : "Stop", "Stop")
				: lp(LBL_A, "OK", "OK");
		}
		// "Leave it running" only while something is running. Over a finished rip there is
		// nothing to leave and B is the ordinary Back it is everywhere else - a legend that
		// said otherwise would be telling the player a copy was still going.
		if (n < max)
		{
			out[n++] = rip_busy() ? lp(LBL_B, "Leave it running", "Leave")
				: lp(LBL_B, "Back", "Back");
		}
		return n;
	}

	if (disc_picking)
	{
		if (n < max) { out[n++] = lp(LBL_A, "Choose", "Choose"); }
		if (n < max) { out[n++] = { CH_UP CH_DOWN, "dpad_ud", "Move", "Move", 0, COL_WHITE }; }
		if (n < max) { out[n++] = lp(LBL_B, "Back", "Back"); }
		return n;
	}

	disc_dlg d;
	disc_dlg_get(&d);
	disc_build_btns(&d);

	int on_opts = (disc_btn < disc_nbtn && disc_btnact[disc_btn] == DBTN_OPTS);

	/*
	  A names what the button under the cursor will do, and dims when that button does -
	  the legend showing a live "PLAY" over a Play button that refuses is the two of them
	  disagreeing about the same press.
	*/
	if (n < max)
	{
		out[n] = on_opts ? lp(LBL_A, "Options", "Opts")
			: lp(LBL_A, d.running ? "Resume" : "Play", "Play");
		out[n].dim = (!on_opts && !d.running && !disc_can_play(&d));
		n++;
	}

	// The same chip a shelf card gets, for the same press and the same destination, and
	// only while there is a strip to reach - see the SCR_DISC case of move_v().
	if (d.susp && n < max) { out[n++] = { CH_DOWN, "dpad_down", "Suspend Points", "Saves", 0, COL_WHITE }; }

	if (disc_nbtn > 1 && n < max) { out[n++] = { CH_LEFT CH_RIGHT, "dpad_lr", "Move", "Move", 0, COL_WHITE }; }
	if (n < max) { out[n++] = lp(LBL_B, "Back", "Back"); }
	return n;
}

// Where the cursor should start: the action, unless it cannot act - an unrecognised disc
// opens with Options under the cursor, because that is the only press that leads anywhere.
static void disc_dlg_enter()
{
	disc_dlg d;
	disc_dlg_get(&d);
	disc_build_btns(&d);

	disc_picking = 0;
	disc_row = 0;
	disc_btn = 0;
	for (int i = 0; i < disc_nbtn; i++)
	{
		if (!disc_btndim[i]) { disc_btn = i; break; }
	}
}

static void disc_open_screen()
{
	disc_dlg_enter();
	go_screen(SCR_DISC);
}

/* ------------------------------------------------------------- the layout --- */

/*
  What the disc is for, as two numbers, because the whole layout hangs off them.

  Measured on Dinofly's own set with a real scanned label in the dialog: at 32 px across the
  label is a featureless blob, at 64 it reads as artwork but the title on it is illegible,
  and at about 144 the title becomes readable. So 144 is the size at which the disc is doing
  the job the dialog exists for, and below it the dialog has nothing better to spend the
  canvas on than making the disc as large as the canvas allows.

  288 - twice that, and 9 of gfx_disc's cells - is the ceiling. Past it a bigger disc buys
  no legibility at all and costs real work twice over: a dearer resample per angle, and a
  rotation cache whose fixed budget holds fewer of the turn's angles whole (see disc_rot).
*/
#define DISC_DLG_READABLE  144
#define DISC_DLG_MAX_CELLS 9

/*
  The dialog's geometry, computed in one place because the drawing and the rectangle the
  spin repaint replays under a clip both depend on it, and a second copy of this
  arithmetic would drift the first time the panel was resized. See disc_note_rect().
*/
struct disc_layout
{
	int px, py;              // where the panel goes; see draw_panel_at()
	int pw, ph;              // the panel
	int r;                   // in gfx_disc's units: the disc is 2r pixels across
	int ts;                  // the title's text scale
	int bw, bh, gap;         // one button, and the air between two of them
};

static void disc_layout_for(const chome_profile *p, const disc_dlg *d, disc_layout *L)
{
	int s = p->ts_ui;

	L->ts = p->ts_title;
	L->bh = 12 * s + 6;
	L->gap = 8 * s;

	/*
	  Buttons wide enough for the longest label and no wider, then both the same width:
	  two buttons of different sizes read as one button and one label.
	*/
	L->bw = 40 * s;
	for (int i = 0; i < disc_nbtn; i++)
	{
		int w = gfx_text_w(disc_btntext[i], s) + 12 * s;
		if (w > L->bw) L->bw = w;
	}

	int nb = disc_nbtn ? disc_nbtn : 1;
	int btnrow = nb * L->bw + (nb - 1) * L->gap;

	// Everything above and below the disc, so the disc can be given what is left.
	int chrome = (10 * s + 6)                 // draw_panel_ex's own title bar
		+ 6 * s + 8 * L->ts                   // the game's name
		+ 4 * s + 8 * s                       // the line under it
		+ 8 * s                               // air above the disc
		+ 8 * s + L->bh                       // air, then the row of buttons
		+ 8 * s;                              // and the panel's bottom margin

	/*
	  The room the dialog has: the canvas less a margin either side, from the top margin
	  down to the button legend.

	  The bottom is the point of the exercise. It stops clear of the button legend rather
	  than reaching for the centre of the screen, because the two used to end up a single
	  row apart at 240p - measured on the device: plate to row 209, legend from row 211 -
	  and a dialog touching the prompts reads as one crowded thing instead of two.

	  Top and bottom margins are therefore NOT equal, which is why this cannot be centred
	  on the canvas and why draw_panel_at() exists. What the panel does with the room is
	  two decisions further down: how big the disc is, and whether the panel fills the
	  width or is only as wide as its own longest line.
	*/
	int left = p->inset;
	int top = p->safe_y + 4 * s;
	int bot = p->y_legend - 6 * s;          // the air between the panel and the prompts

	int maxw = p->w - 2 * left;
	int maxh = bot - top;
	if (maxh < 32) maxh = 32;               // a canvas too short for this is still drawn

	/*
	  Sized in whole cells rather than in pixels, because a radius that is not a multiple
	  of 16 buys nothing: gfx_disc draws a 32-cell sprite at r/16 pixels per cell, so the
	  cell size is an integer division and anything between two multiples renders as the
	  lower one with a fractional grid. Cells are also what a rotated scan is cached
	  against, so this is the number that decides how much work a turn costs.
	*/
	int side = 12 * s;                        // the margin either side of the widest line

	/*
	  Two fifths of the canvas height - 288 px at 720p, 224 at 960x540, 192 at 480p and 96
	  at 240p, against 224, 160, 160 and 64 for the column layout this replaced.

	  That is the figure this dialog was designed at, and it drifted: the disc was changed
	  to start from the height the *panel* had been handed instead, which on a big canvas is
	  most of the screen. At 720p the band above the legend is 652 lines, so the disc took
	  480 of them and the panel came out 1216x644 of a 1280x720 display - and at 1920x1080
	  the same sum gives a disc of 800. Nothing needs a 480 px disc, let alone an 800 px one.
	  Dinofly: "they probably should not have an immense dialog covering most of the screen at
	  high res". So the fraction of the canvas is what decides again, and the panel is sized
	  from the disc rather than the other way round.

	  DISC_DLG_MAX_CELLS is what stops the same drift arriving from above instead: two fifths
	  of 1080 lines is 432 px, which is past the point where a scan is fully readable and is
	  only more resampling per turn. It bites at 720 lines and higher, so 720p lands exactly
	  on the cap.

	  Rounded to the nearest whole cell rather than truncated, and 960x540 is the reason:
	  two fifths of 540 is 6.75 cells, truncating took it to 6, and the disc came out a
	  sixth smaller than the canvas had room for.
	*/
	int cell = (2 * p->h / 5 + 16) / 32;
	if (cell > DISC_DLG_MAX_CELLS) cell = DISC_DLG_MAX_CELLS;
	if (cell < 1) cell = 1;

	// Then walked down until the panel fits both ways, on a canvas that cannot afford even
	// that. A clipped button is worse than a smaller disc, which is the same order the old
	// layout gave way in.
	while (cell > 1 && (chrome + 32 * cell > maxh || 32 * cell + 2 * side > maxw)) cell--;

	L->r = 16 * cell;
	L->ph = chrome + 32 * cell;

	/*
	  And now the width, which is one decision: does the panel fill what it was given, or is
	  it only as wide as its own longest line?

	  Both answers have been right. It started as the widest line plus a margin, on the
	  grounds that stretching it to the inset left half of it empty. Then Dinofly asked for
	  nearly the full width, because at 240p there is no room for a disc big enough to show a
	  real scanned label and a large plate is the best that canvas can do. Neither is a
	  principle; the 240p compromise simply stopped being a compromise and became the rule at
	  every resolution, which is what made the panel immense.

	  So the disc decides, since the disc is what the width was ever for. Below
	  DISC_DLG_READABLE it is not yet doing its job - 96 px at 240p, a label there is a blob -
	  and the panel spends everything it has, which is what that canvas gets today and what it
	  keeps. At or past it the disc reads on its own, and a panel stretched to the inset is
	  1216 px of flat grey with a 288 px disc floating in the middle of it. The switch falls at
	  a canvas of 360 lines, two fifths of which is the readable diameter.

	  Read off the canvas rather than off the profile deliberately. `lo` forced onto a 720p
	  screen from the Display panel is a large canvas with large text, and it should get the
	  large-canvas dialog; a 15 kHz TV canvas arriving unhalved as 640x240 is the opposite
	  case and has to keep the 240p layout, which it does because its height is 240 whatever
	  its width says - see theme_update() on square units.

	  The lines are measured either way, and that is what stops a long name being clipped
	  silently: sizing to the disc alone once made "Super Nintendo (not yet)" come out as
	  "Super Nintendo (n>" on the one disc whose whole point is that line. gfx_clip() still
	  cuts either line when even the full width will not hold it.

	  Which does mean a disc with a very long name gets a wide dialog - a forty-character title
	  at 720p comes to 1008 of the 1216 it may have. That is deliberate and it is not what was
	  wrong before: the width is being *used* by the longest thing the screen has to say, rather
	  than spent on grey either side of a title that needed a third of it. The height, which is
	  most of what made the old panel immense, is bounded by the disc whatever the name is.
	*/
	int fill = (32 * cell < DISC_DLG_READABLE);

	int content = 32 * cell;
	if (btnrow > content) content = btnrow;

	int tw = gfx_text_w(d->title, L->ts);
	if (tw > content) content = tw;

	tw = gfx_text_w(d->sub, s);
	if (tw > content) content = tw;

	L->pw = content + 2 * side;
	if (fill && L->pw < maxw) L->pw = maxw;
	if (L->pw > maxw) L->pw = maxw;

	L->px = (p->w - L->pw) / 2;             // centred horizontally, which the margins are

	/*
	  Vertically it sits in the middle of the band it was given - between the top margin and
	  the legend - and not in the middle of the canvas, which is the same reason the width
	  cannot be centred: the legend owns the bottom and the two margins are different
	  distances.

	  A panel that fills the band is pinned at the top instead. That is the 240p case, where
	  the panel is nine rows short of the band and centring it would move it four rows down
	  for no reason; pinning it is what keeps that canvas pixel for pixel what it was. The
	  clamp is for a canvas so short that even one cell of disc does not fit, where the
	  centring arithmetic would otherwise push the panel up into the overscan margin.
	*/
	L->py = fill ? top : top + (maxh - L->ph) / 2;
	if (L->py < top) L->py = top;
}

/* -------------------------------------------------------------- the scan --- */

/*
  A real photograph of the disc, turning, when the card has one.

  Cosine at 256 positions to a turn, in 8.8 fixed point, quarter-turn table plus symmetry.
  256 because that is what disc_step_fine() quantises the phase accumulator to: the count
  the dialog's 60fps ask needs, chosen there. Every fourth entry is the old 64-position
  table exactly, so the angles the badge's sprite steps through are a strict subset of
  these and the two discs stay in register.

  The folds are not just a smaller table: they are what makes the quadrant cache in
  disc_rot() *exact*. Composing "the angle inside one quadrant, then a quarter turn" is
  byte-identical to rotating by the full angle only if cos(q+64) == -sin(q) holds in the
  table's own integers, which the folding guarantees by construction - both sides read
  the same entry. An independently rounded full-turn table would be off by one count on
  some entries, and one count is a moved pixel at the rim of a 288px disc.
*/
static const int disc_cos_tab[65] =
{
	256, 256, 256, 255, 255, 254, 253, 252,
	251, 250, 248, 247, 245, 243, 241, 239,
	237, 234, 231, 229, 226, 223, 220, 216,
	213, 209, 206, 202, 198, 194, 190, 185,
	181, 177, 172, 167, 162, 157, 152, 147,
	142, 137, 132, 126, 121, 115, 109, 104,
	98, 92, 86, 80, 74, 68, 62, 56,
	50, 44, 38, 31, 25, 19, 13, 6,
	0,
};

static int disc_cos_q8(int q)
{
	q &= 255;
	if (q <= 64) return disc_cos_tab[q];
	if (q <= 128) return -disc_cos_tab[128 - q];
	if (q <= 192) return -disc_cos_tab[q - 128];
	return disc_cos_tab[256 - q];
}

static int disc_sin_q8(int q)
{
	return disc_cos_q8(q + 192);
}

/*
  A square image, turned, cached until something about it changes.

  Rotation is inverse-mapped - for each destination pixel, where in the source it came
  from - which is the only way round that leaves no unwritten pixels. Done into a buffer
  and blitted rather than drawn pixel by pixel: gfx_fill() records damage per call, and
  20k to 80k one-pixel fills a frame would spend more time in the damage bookkeeping than
  in the resampling.

  Behind the per-frame answer sits a bounded cache of quadrant frames, because the dialog
  now asks for 256 angles a turn and at 60fps nearly every ask is a new one - see
  disc_step_fine(). An earlier note here rejected pre-rendering outright ("64 frames at
  288px would be 27MB"), and the arithmetic was right while a frame only had to exist
  every 62.5ms; sixty resamples a second is a different regime, and the two facts that
  make caching affordable now are the quarter turn and the budget below.

  The quarter turn: rotating this square buffer by exactly 90 degrees is a permutation of
  its pixels, and with the sampling and the mask both measured from the true centre - the
  point *between* the four middle pixels, where gfx_disc_face() has always measured from -
  composing "the angle inside one quadrant, then quarter turns" is byte-identical to
  rotating by the full angle. Identical because the trig folds guarantee cos(q+64) is
  read from the same table entry as -sin(q), so the integer sums inside disc_rot_pick()
  are equal term for term, not merely close. One cached frame therefore serves four
  angles of every turn, and 64 slots cover all 256.

  The budget: DISC_ROT_CACHE_BYTES bounds what the slots may hold, sized so the disc the
  default configuration actually draws - 160px, on the halved 720p canvas - caches a
  whole turn. Where a frame is bigger the slot count halves until it fits (it must stay
  a power of two, or the kept angles would not divide the turn evenly), and the angles
  between the kept ones are resampled on demand, exactly as every angle used to be. So
  memory decides how much of a turn is *cheap*, never how smooth it is - reducing the
  angle count is the contention rule's decision alone, and the set it reduces to is the
  cached set, which is what makes a contended frame a blit and never a resample.

  This was written for the scan and is now what draws the disc either way, which is the
  point of it: the generated face comes through here too (see disc_draw_face), so the dialog
  is always turning a bitmap and a scan landing cannot change the *kind* of thing on screen.

  `mask` is for a photograph and only for one. Outside the circle the buffer is filled with
  the panel colour rather than left alone, because the blit is square and the panel is what
  is behind it; the spindle hole and hub are then composited in at gfx_disc's own proportions,
  so that whatever the scan actually is - a disc face, a label, a square crop - the result
  still reads as a disc. Every one of those boundaries is anti-aliased through the same
  gfx_disc_cover() the generated face uses, which is what stops the two having different kinds
  of edge.

  A generated face passes 0. Not because the mask would be too coarse for it - it would not
  be, any more - but because the face already *has* all of this in it and more: its own bright
  rim and clear inner ring, drawn at the display's resolution. Masking it would paint the
  photograph's plainer rings over the disc's own.

  Everything here is kept when the dialog closes, deliberately, and the numbers are worth
  stating because the cache is most of them now: the scratch buffer and gfx_disc_face()'s
  together are 1.8 MB at 720p, 648 KB at 480p and on a 960x540 canvas, 72 KB at 240p; the
  slots on top of that reach DISC_ROT_CACHE_BYTES at worst - the exact figure per canvas is
  a slot count times a frame, printed by the harness. Only ever one size at a time - a
  canvas change frees and reallocates the lot.

  Why keeping them is right, buffer by buffer rather than as one answer:

    The face costs a quarter of a million pixels with a square root each to build, and it is
    keyed on nothing but the size. Freeing it would put that whole rebuild on the critical
    path of every dialog open - the one moment the player is waiting on this screen - to
    reclaim 900 KB that the very next open asks for again. Opening the disc dialog is not a
    rare event; it is the disc's only entry point.

    The scratch costs a malloc and nothing else, since the angle has always moved by the
    time the dialog is looked at again and the contents are rebuilt regardless. So freeing
    it would genuinely reclaim 900 KB for the price of one allocation - and buy a 900 KB
    mmap/munmap pair per visit, which glibc will do at that size, for a board with 1 GB
    where the compose buffer alone is 3.6 MB at 720p. Not worth the churn or the second
    code path.

    The slots are the same trade at a bigger number: they refill at one resample per frame
    - the misses of the first revolution after a flush, work the uncached design did every
    frame forever - so dropping them on close would cost nothing visible and reclaim a few
    megabytes, at the price of sixty-odd mmap/munmap pairs per dialog visit and a second
    lifecycle to hold in the head. On a board where a core launch re-execs this whole
    process, idle megabytes are reclaimed by the thing the player was leaving to do anyway.

  If that ever stops being true the release to write is one function that drops all of it,
  called from chome_leave() rather than from leaving SCR_DISC - the dialog is reopened far
  more often than the front-end is left.
*/
static uint32_t *disc_rot_buf = 0;
static int disc_rot_dia = 0;
static int disc_rot_step = -1;
static char disc_rot_path[1024] = {};
static const uint32_t *disc_rot_src = 0;
static const uint32_t *disc_rot_out = 0;   // what the memo above answers with

/*
  The slot budget. 64 quadrant frames of the 160px disc, which is the disc the shipped
  default draws: classicui_halfres is on, so a 720p display is a 640x360 canvas and its
  dialog disc is 160px - the one canvas where "the whole turn is a blit" matters most and
  costs a number this board does not miss. 6.4 MB against 1 GB, beside a compose buffer
  that is 3.6 MB whenever halfres is off.

  Not a setting. A knob for cache bytes is a promise to explain cache bytes to a player;
  the observable thing is the disc turning smoothly, and that is true at every size - the
  budget only moves where the smoothness is paid for.
*/
#define DISC_ROT_CACHE_BYTES (64UL * 160 * 160 * 4)

static uint32_t *disc_rot_slot[64];
static unsigned char disc_rot_slot_ok[64];
static int disc_rot_slots = 0;              // slots this diameter is allowed
static int disc_rot_stride_now = 4;         // quadrant angles per kept one: 64/slots
static int disc_rot_cache_dia = 0;
static const uint32_t *disc_rot_cache_src = 0;
static char disc_rot_cache_key[1024] = {};

#ifdef CHOME_HOST_TEST
static int disc_rot_force_direct = 0;       // see disc_test_rot_direct()
#endif

/*
  How coarsely a contended frame quantises, declared back beside disc_step_fine(): the
  spacing of the cached angles, so "contended" means "only frames that are a blit".

  4 before the first dialog frame has sized the cache, because 4 is the old 64-position
  turn: a contended frame on a cache that does not exist yet behaves exactly as every
  frame did before any of this. Clamped at 4 from the other side too - if the budget or
  the diameter cap ever moves and a size ends up with fewer than 16 slots, contention
  must not grind the disc below the turn that shipped.
*/
static int disc_rot_stride()
{
	int s = disc_rot_stride_now;
	return (s > 4) ? 4 : s;
}

/*
  Where a destination pixel comes from in the source, for a rotation about the centre of a
  dia-square image. 0 when that is off the end of the buffer.

  Off the end happens, and the caller has to mean something by it rather than clamp. A
  square's corners are further from the centre than its edges, so a destination pixel out in
  a corner asks for a source pixel that does not exist - and clamping answers with whatever is
  at the middle of an edge, which for a disc drawn to the buffer's rim is the rim itself.
  Under the mask that could never show, because every pixel that far out is background before
  the sampling. Unmasked it flung four grey smears off the disc at the diagonals, turning with
  it.

  gfx_disc_face() also keeps a pixel of background inside its own edge, for the same reason.
  Both, because one is a property of the source and this is a property of the map, and either
  alone leaves the other free to be wrong.

  `dx2`/`dy2` are in half-pixel units about the true centre - 2*(x-r)+1, odd numbers, the
  same frame gfx_disc_face() draws in - and not about pixel (r,r) as they used to be.
  Half a pixel is not a nicety here; it is the whole quadrant cache: about the true centre
  a quarter turn maps the pixel grid onto itself exactly, so the map at step q+64 *is* the
  map at q followed by that permutation, integer for integer. About a pixel the same
  composition lands one pixel off, and the disc jumped sideways four times a turn.
*/
static const uint32_t *disc_rot_pick(const uint32_t *src, int dia, int r, int dx2, int dy2,
	int cs, int sn)
{
	int sx2 = (dx2 * cs + dy2 * sn) >> 8;
	int sy2 = (dy2 * cs - dx2 * sn) >> 8;

	// Back from half-pixel units to the pixel whose centre is nearest: the arithmetic
	// shift floors, which pairs each even value with the odd one above it.
	int u = r + (sx2 >> 1), v = r + (sy2 >> 1);
	if (u < 0 || u >= dia || v < 0 || v >= dia) return 0;

	return src + (size_t)v * dia + u;
}

// How many times the resample below has actually run. The whole cost argument for the
// 256-position turn is that a steady-state frame is a blit and not a resample, and a
// counter is the only way a test can see the difference - the pixels are identical by
// design. Counted in the firmware too (it is one increment), read only by the harness.
static int disc_rot_renders = 0;

static void disc_rot_render(uint32_t *out, const uint32_t *src, int dia, int step, int mask)
{
	int r = dia / 2;
	disc_rot_renders++;

	/*
	  Turned by the negated angle, so that this and gfx_disc turn the same way.

	  An inverse map rotates the picture the opposite way round from the angle it is given,
	  and gfx_disc's wedges advance with theirs - so with the plain step the dialog's disc
	  turned backwards against the badge the player had just pressed A on. Nobody could see
	  it while this path only ever drew photographs, because a scan has no feature whose
	  direction is known. It became visible the moment the generated face came through here:
	  the same twelve bands, one after the other, going opposite ways.

	  Verified rather than argued: at step 0 the face and the sprite put the specular band at
	  the same angle, and stepping both forward moves it the same way round to within the
	  degree and a half the 8.8 sine costs.
	*/
	int cs = disc_cos_q8(-step), sn = disc_sin_q8(-step);

	/*
	  gfx_disc's radii, in the same 32nds of the radius it uses: the clear inner ring at
	  9/32, the hub ring below it, and the hole inside that at GFX_DISC_HOLE_PCT - which is
	  where these scans are actually transparent, and therefore exactly what has to be
	  covered. Punching at the sprite's 6/32 left the dark hole 4% of the radius wider than
	  the transparency it was there to hide, so a scan and a generated face put their holes
	  in visibly different places.

	  The outer edge is one 32nd rather than the drawn disc's five. There it is an edge and a
	  bright rim, which is what makes a flat circle of colour read as a pressed disc; a
	  photograph has its own printed edge and does not need lending one, but it does need
	  separating from the panel it sits on - without any ring at all the scan was a circular
	  crop rather than an object.
	*/
	int r_edge = r;
	int r_dark = r * 31 / 32;
	int r_ring = r * 9 / 32;
	int r_hub  = r * GFX_DISC_HOLE_PCT / 100;

	uint32_t edge = ((COL_WHITE >> 1) & 0x7f7f7f7f) + ((COL_BGDARK >> 1) & 0x7f7f7f7f);
	edge |= 0xff000000u;

	for (int y = 0; y < dia; y++)
	{
		int dy2 = 2 * (y - r) + 1;
		uint32_t *dst = out + (size_t)y * dia;

		for (int x = 0; x < dia; x++)
		{
			int dx2 = 2 * (x - r) + 1;
			int d2h = dx2 * dx2 + dy2 * dy2;

			if (mask)
			{
				/*
				  The mask, composited from the outside in with a coverage per boundary, which
				  is the same shape - and the same ramp, through gfx_disc_cover_h() - that
				  gfx_disc_face() draws its rings with, measured from the same between-pixels
				  centre. The rings have to be centred where the rotation is centred, or a
				  quarter-turned cache frame would carry its rings one pixel out of place; a
				  side effect worth having is that a scan's rings now sit exactly on the
				  face's, where they used to be half a pixel off.

				  It used to be four comparisons and four hard edges, and that was defensible
				  only for as long as the alternative in this dialog was a 32-cell sprite: a
				  hard circle is not worth remarking on next to fifteen-pixel blocks. Once the
				  generated disc was resolved to the screen it stopped being defensible, because
				  then the *scan* was the one with the staircase - the same seam as before with
				  the two sides swapped, and this is the side that shows once real art arrives.
				  Measured before the change: not one blended pixel at any of the four
				  boundaries.

				  Affordable because gfx_disc_cover_h() answers 0 or 255 from the squared
				  distance and only roots the pixels a boundary actually passes through -
				  circumference, not area. This loop runs per rotation angle, so that
				  distinction is the whole reason it can be done at all.
				*/
				int a_edge  = gfx_disc_cover_h(r_edge, d2h);
				if (!a_edge) { dst[x] = COL_PANEL; continue; }

				int a_photo = gfx_disc_cover_h(r_dark, d2h);
				int a_ring  = gfx_disc_cover_h(r_ring, d2h);
				int a_hole  = gfx_disc_cover_h(r_hub, d2h);

				uint32_t col = gfx_mix(COL_PANEL, edge, a_edge);

				if (a_photo)
				{
					const uint32_t *p = disc_rot_pick(src, dia, r, dx2, dy2, cs, sn);
					if (p) col = gfx_mix(col, *p, a_photo);
				}

				col = gfx_mix(col, edge, a_ring);
				col = gfx_mix(col, COL_BGDARK, a_hole);

				dst[x] = col;
				continue;
			}

			const uint32_t *p = disc_rot_pick(src, dia, r, dx2, dy2, cs, sn);
			dst[x] = p ? (*p | 0xff000000u) : COL_PANEL;
		}
	}
}

/*
  A quarter turn as the pixel permutation it is: out(x,y) = in(dia-1-y, x) per quarter,
  which about the between-pixels centre is exactly what the sampling map does at step
  q+64 - see disc_rot_pick(). This is the blit that lets one cached frame stand in for
  four resamples a turn.

  Derived once and checked in the harness rather than trusted: the smoothness section
  compares a frame served through here against the same step resampled directly, byte
  for byte, on both the face and a scan.

  The half turn reads backwards linearly; the odd quarters read a column per row, so
  they walk in 32-pixel tiles - the source lines a tile touches stay resident instead of
  being evicted dia times each, which is the difference between a copy and a copy that
  costs like a resample at 288px.
*/
static void disc_rot_quarter(uint32_t *out, const uint32_t *in, int dia, int k)
{
	size_t n = (size_t)dia * dia;

	if (k == 2)
	{
		for (size_t i = 0; i < n; i++) out[i] = in[n - 1 - i];
		return;
	}

	const int T = 32;
	for (int y0 = 0; y0 < dia; y0 += T)
	{
		int y1 = (y0 + T < dia) ? y0 + T : dia;
		for (int x0 = 0; x0 < dia; x0 += T)
		{
			int x1 = (x0 + T < dia) ? x0 + T : dia;
			for (int y = y0; y < y1; y++)
			{
				uint32_t *dst = out + (size_t)y * dia;
				if (k == 1)
				{
					// out(x,y) = in(dia-1-y, x): column dia-1-y, walking rows with x.
					const uint32_t *p = in + (size_t)x0 * dia + (dia - 1 - y);
					for (int x = x0; x < x1; x++, p += dia) dst[x] = *p;
				}
				else
				{
					// k == 3, the quarter the other way: out(x,y) = in(y, dia-1-x).
					const uint32_t *p = in + (size_t)(dia - 1 - x0) * dia + y;
					for (int x = x0; x < x1; x++, p -= dia) dst[x] = *p;
				}
			}
		}
	}
}

static const uint32_t *disc_rot(const uint32_t *src, const char *key, int dia, int step,
	int mask)
{
	if (!src || dia < 2) return 0;

	/*
	  The decode is in the key as well as the path, because art_thumb() hands back a fresh
	  allocation when it notices the file was rewritten - which is precisely what happens
	  when the fetcher lands a scan while this dialog is up. Keyed on the path alone, the
	  first angle after that would still be the picture that was there before.
	*/
	if (disc_rot_out && disc_rot_dia == dia && disc_rot_step == step
		&& disc_rot_src == src && !strcmp(disc_rot_path, key))
	{
		return disc_rot_out;
	}

	if (!disc_rot_buf || disc_rot_dia != dia)
	{
		free(disc_rot_buf);
		disc_rot_buf = (uint32_t*)malloc((size_t)dia * dia * 4);
		disc_rot_dia = disc_rot_buf ? dia : 0;
		if (!disc_rot_buf) { disc_rot_out = 0; return 0; }
	}

	/*
	  The cache generation: a different picture, a different decode of the same picture, or
	  a different size invalidates every slot at once. The slots themselves are only freed
	  when the size moves - a scan landing reuses the allocations with new contents.
	*/
	if (disc_rot_cache_dia != dia || disc_rot_cache_src != src
		|| strcmp(disc_rot_cache_key, key))
	{
		if (disc_rot_cache_dia != dia)
		{
			for (int i = 0; i < 64; i++) { free(disc_rot_slot[i]); disc_rot_slot[i] = 0; }

			/*
			  How many of the 64 quadrant angles the budget holds at this size, as a power
			  of two so the kept angles divide the turn evenly - 64ths at a stride of 1, the
			  old 64-position turn at a stride of 4. Every diameter the layout can produce
			  (96 to the 288 cap) lands between 16 and 64.
			*/
			int k = (int)(DISC_ROT_CACHE_BYTES / ((unsigned long)dia * dia * 4));
			if (k > 64) k = 64;
			if (k < 1) k = 1;
			while (k & (k - 1)) k &= k - 1;
			disc_rot_slots = k;
			disc_rot_stride_now = 64 / k;
		}
		memset(disc_rot_slot_ok, 0, sizeof(disc_rot_slot_ok));
		disc_rot_cache_dia = dia;
		disc_rot_cache_src = src;
		snprintf(disc_rot_cache_key, sizeof(disc_rot_cache_key), "%s", key);
	}

	snprintf(disc_rot_path, sizeof(disc_rot_path), "%s", key);
	disc_rot_step = step;
	disc_rot_src = src;

	/*
	  The angle inside one quadrant and the quarter turns outside it. A kept angle is
	  resampled once per generation into its slot and served as a blit ever after - as
	  itself when the quarter count is zero, through disc_rot_quarter() otherwise. An
	  angle between the kept ones is resampled directly at the full step, which is
	  byte-identical to what a slot would have produced for it: the trig folds make the
	  one-step map and the composed map the same integers, so the cache can never be
	  seen in the pixels, only in the time. The harness holds it to that.
	*/
	int q = step & 63;
	int quarters = (step >> 6) & 3;
	int stride = disc_rot_stride_now;
	const uint32_t *out = 0;

#ifdef CHOME_HOST_TEST
	if (disc_rot_force_direct) stride = 0;
#endif

	if (stride && !(q % stride))
	{
		int si = q / stride;
		if (!disc_rot_slot[si])
			disc_rot_slot[si] = (uint32_t*)malloc((size_t)dia * dia * 4);

		if (disc_rot_slot[si])
		{
			if (!disc_rot_slot_ok[si])
			{
				disc_rot_render(disc_rot_slot[si], src, dia, q, mask);
				disc_rot_slot_ok[si] = 1;
			}

			if (!quarters) out = disc_rot_slot[si];
			else
			{
				disc_rot_quarter(disc_rot_buf, disc_rot_slot[si], dia, quarters);
				out = disc_rot_buf;
			}
		}
	}

	if (!out)
	{
		// Between the kept angles, or a slot the allocator refused: the resample this
		// path always was, at the full angle.
		disc_rot_render(disc_rot_buf, src, dia, step, mask);
		out = disc_rot_buf;
	}

	disc_rot_out = out;
	return out;
}

#ifdef CHOME_HOST_TEST
/*
  Hooks for the harness's smoothness section, compiled out of the firmware. The cache is
  deliberately invisible in the pixels, so proving that takes levers no player needs:
  drop the slots without moving the clock, force the direct resample for the same
  instant, and read what the cache decided it may hold.
*/
void disc_test_rot_drop()
{
	memset(disc_rot_slot_ok, 0, sizeof(disc_rot_slot_ok));
	disc_rot_step = -1;
	disc_rot_out = 0;
}

void disc_test_rot_direct(int on)
{
	disc_rot_force_direct = on;
	disc_rot_step = -1;
	disc_rot_out = 0;
}

int disc_test_rot_info(int *slots, int *stride, long *bytes)
{
	if (slots) *slots = disc_rot_slots;
	if (stride) *stride = disc_rot_stride();
	if (bytes) *bytes = (long)disc_rot_slots * disc_rot_cache_dia * disc_rot_cache_dia * 4;
	return disc_rot_cache_dia;
}

int disc_test_shown_step()
{
	return disc_rot_step;
}

int disc_test_rot_renders()
{
	return disc_rot_renders;
}
#endif

/*
  The cache key the generated face goes under, in the slot a scan's path goes in. It cannot
  collide with a real one: disc_art_path() only ever answers with an absolute path under the
  card root. The buffer pointer is in that key too, so this is belt as well as braces.
*/
static const char disc_gen_key[] = "*generated*";

/*
  The scan if there is one, the generated disc if there is not - and the second is the normal
  case, not a fallback for a broken one: a physical disc has no filename to match on, so a
  picture only exists once something has fetched one against the disc's identity. Both are
  drawn at the same centre and the same radius, so the rectangle disc_note_rect() records
  covers whichever turned up.

  Both now go through the same rotate-and-blit, which is the whole design and not a tidying:
  this dialog is where a photograph of the disc label appears, and it used to draw the 32-cell
  sprite the rest of the time. At badge size that sprite is the right look; here it fills the
  panel, so each cell came out fifteen pixels square at 720p, and a scan landing swapped
  fifteen-pixel blocks for a photograph. Two different objects, not one object twice. So the
  dialog asks for the disc resolved to the size it is actually drawing at, and turns that
  exactly as it turns a scan.

  His decision, and it only applies here: the badge on the shelf keeps gfx_disc at every
  profile. At badge size a photograph is 32 pixels of mud and a smooth circle is a blurred
  icon, and the badge's job is to say "there is a disc", which the sprite does better.

  gfx_disc stays as the last resort, because both buffers above are a malloc that can fail
  and a disc dialog with no disc on it is worse than a blocky one.
*/
/* ------------------------------------------------- the disc as a progress pie --- */

/*
  arctangent, as a table, because this file has no math.h and does not want one.

  Same reasoning as disc_sin[] and gfx_disc_cover() in chome_gfx.cpp: the values are
  written out, and interpolating between them is cheaper and more predictable than pulling
  in libm for a curve that is used in exactly one place.

  tab[i] is atan(i/64) in units where a full turn is 4096, so an eighth of a turn - the
  octant this covers - is 512. Linear interpolation between the entries is accurate to
  0.044 degrees, which at the largest disc this dialog draws (288 px across) is under a
  fifth of a pixel at the rim.
*/
static const uint16_t disc_atan_tab[65] =
{
	   0,   10,   20,   31,   41,   51,   61,   71,
	  81,   91,  101,  111,  121,  131,  140,  150,
	 160,  169,  179,  188,  197,  207,  216,  225,
	 234,  243,  252,  260,  269,  277,  286,  294,
	 302,  310,  318,  326,  334,  342,  349,  357,
	 364,  371,  379,  386,  393,  399,  406,  413,
	 419,  426,  432,  439,  445,  451,  457,  463,
	 469,  474,  480,  486,  491,  496,  502,  507,
	 512,
};

// atan(a/b) in 4096ths of a turn, for 0 <= a <= b and b > 0. Answers 0..512.
static int disc_atan_q(int a, int b)
{
	int t = (int)((long)a * 4096 / b);
	if (t < 0) t = 0;
	if (t > 4096) t = 4096;

	int i = t >> 6, f = t & 63;
	if (i >= 64) return disc_atan_tab[64];
	return disc_atan_tab[i] + (disc_atan_tab[i + 1] - disc_atan_tab[i]) * f / 64;
}

/*
  The angle of a point clockwise from twelve o'clock, in 4096ths of a turn.

  `v` is how far right of the centre and `u` how far *up*, so a caller working in screen
  coordinates passes -dy for u. Clockwise from the top because that is the direction a
  progress pie fills and the direction this disc turns.
*/
static int disc_angle_q(int v, int u)
{
	if (!v && !u) return 0;

	int a = (v < 0) ? -v : v;
	int b = (u < 0) ? -u : u;

	// The angle inside its own quadrant, 0 at the vertical axis and 1024 at the horizontal
	// one. Folded at the diagonal so the table only has to cover an octant.
	int q = (a <= b) ? disc_atan_q(a, b) : 1024 - disc_atan_q(b, a);

	if (u >= 0 && v >= 0) return q;                    // up and right: 0 to a quarter turn
	if (u < 0 && v >= 0) return 2048 - q;              // down and right
	if (u < 0 && v < 0) return 2048 + q;               // down and left
	return 4096 - q;                                   // up and left
}

#define DISC_ANG_OUTSIDE 0xffff

/*
  Every pixel's angle, computed once per diameter and then only compared against.

  This is what keeps the pie off the rotation cache, which is the whole performance
  question here. disc_rot() caches the rotated disc against the rotation step so it is
  resampled once per angle rather than once per blit; baking a pie into that image would
  make the cache key depend on progress as well and resample a 288 px disc every frame of
  an operation that is already saturating the drive.

  So the mask is applied *after* the cached image, and the only per-pixel work in a frame
  is one 16-bit compare and, for the part not yet revealed, one gfx_mix. The angles
  themselves never change: they depend on the geometry and not on the rotation or the
  progress, so this buffer is built when the disc changes size and not again.

  Pixels outside the disc are marked DISC_ANG_OUTSIDE and left exactly as the cached image
  has them. That is not a nicety either: disc_rot() fills the corners of its square buffer
  with COL_PANEL, and a pie that darkened those would put a dark wedge across the panel
  behind the disc.
*/
static uint16_t *disc_ang_buf = 0;
static int disc_ang_dia = 0;

static const uint16_t *disc_angle_map(int dia)
{
	if (dia < 2) return 0;
	if (disc_ang_buf && disc_ang_dia == dia) return disc_ang_buf;

	uint16_t *nb = (uint16_t*)malloc((size_t)dia * dia * sizeof(uint16_t));
	if (!nb) return 0;

	free(disc_ang_buf);
	disc_ang_buf = nb;
	disc_ang_dia = dia;

	int r = dia / 2;
	int r2 = r * r;

	for (int y = 0; y < dia; y++)
	{
		int dy = y - r;
		uint16_t *row = disc_ang_buf + (size_t)y * dia;

		for (int x = 0; x < dia; x++)
		{
			int dx = x - r;
			if (dx * dx + dy * dy > r2) { row[x] = DISC_ANG_OUTSIDE; continue; }

			// -dy for "up": the buffer's y grows downward and the angle is measured from
			// twelve o'clock.
			row[x] = (uint16_t)disc_angle_q(dx, -dy);
		}
	}

	return disc_ang_buf;
}

/*
  How dark the part not yet copied is, out of 255 - and the darkening is a *multiply*
  against black rather than a wash of a flat colour.

  That distinction has already cost this front-end once. Drawing a scrim of COL_BGDARK over
  a picture forces every dark pixel to one near-black value and takes the detail with it,
  which is why the in-game dim was changed to multiply against COL_BLACK instead. The
  unrevealed part of the disc has to stay recognisably the disc - the player is watching
  their own game's label appear - so every pixel keeps its own colour at three-eighths of
  its brightness, and a scanned label is still legible through it.
*/
#define DISC_REVEAL_DIM 96

// Where the masked frame is assembled, so the framebuffer still takes one blit and the
// cached rotation is never written to.
static uint32_t *disc_rev_buf = 0;
static int disc_rev_dia = 0;

/*
  The cached rotated disc, with everything past the pie's edge dimmed.

  Falls back to the plain blit whenever a buffer cannot be had: a fully bright disc with no
  progress on it is a worse screen than this one and a much better screen than no disc.
*/
static void disc_blit_reveal(const uint32_t *img, int dia, int x, int y, int reveal)
{
	if (reveal >= DISC_REVEAL_FULL) { gfx_blit(img, dia, dia, x, y, dia, dia); return; }

	const uint16_t *ang = disc_angle_map(dia);
	if (!ang) { gfx_blit(img, dia, dia, x, y, dia, dia); return; }

	if (!disc_rev_buf || disc_rev_dia != dia)
	{
		uint32_t *nb = (uint32_t*)malloc((size_t)dia * dia * 4);
		if (!nb) { gfx_blit(img, dia, dia, x, y, dia, dia); return; }
		free(disc_rev_buf);
		disc_rev_buf = nb;
		disc_rev_dia = dia;
	}

	// The pie's edge, in the same 4096ths the map is in. A reveal of 0 darkens the whole
	// disc; DISC_REVEAL_FULL never reaches here.
	int edge = reveal * 4096 / DISC_REVEAL_FULL;

	size_t n = (size_t)dia * dia;
	for (size_t i = 0; i < n; i++)
	{
		uint16_t a = ang[i];
		disc_rev_buf[i] = (a == DISC_ANG_OUTSIDE || a < edge)
			? img[i] : gfx_mix(COL_BLACK, img[i], DISC_REVEAL_DIM);
	}

	gfx_blit(disc_rev_buf, dia, dia, x, y, dia, dia);
}

static void disc_draw_face(const disc_dlg *d, int cx, int cy, int r, int reveal)
{
	// The fine step: this is the disc he asked to see at 60fps, so it gets the 256-position
	// turn. The sprite fallback below keeps the coarse one - 64 positions is all a 32-cell
	// sprite can express anyway.
	int step = disc_step_fine();
	int dia = 2 * r;
	char path[1024];

	// What this frame's disc is a function of, for the spin repaint's skip. Recorded
	// by the draw itself so the two can never disagree; see disc_spin_sig().
	disc_drawn_sig = disc_spin_sig();

	if (d->key[0] && disc_art_path(d->key, path, sizeof(path)))
	{
		const uint32_t *scan = art_thumb(path, dia, dia);
		const uint32_t *img = scan ? disc_rot(scan, path, dia, step, 1) : 0;
		if (img)
		{
			disc_blit_reveal(img, dia, cx - r, cy - r, reveal);
			return;
		}
	}

	const uint32_t *face = gfx_disc_face(dia, disc_bands, DISC_BANDS_N,
		COL_WHITE, COL_PANELHI, COL_BGDARK, COL_PANEL);

	const uint32_t *img = face ? disc_rot(face, disc_gen_key, dia, step, 0) : 0;
	if (img)
	{
		disc_blit_reveal(img, dia, cx - r, cy - r, reveal);
		return;
	}

	gfx_disc(cx, cy, r, disc_step(),
		disc_bands, DISC_BANDS_N, COL_WHITE, COL_PANELHI, COL_BGDARK, 0);
}

/* ------------------------------------------------------------ the drawing --- */

static void draw_disc_picker(const chome_profile *p, const disc_dlg *d)
{
	disc_build_rows();

	int s = p->ts_ui;
	int rowh = 14 * s;
	int nrows = disc_nrows ? disc_nrows : 1;

	/*
	  Twenty-four characters, or the widest row if one of them needs more.

	  The fixed width was fine while every row was a console's name plus at most "(not yet)".
	  The copy row is longer than that at some system names, and a row that does not fit is
	  clipped by gfx_clip() below - which turned "Copy to TurboGrafx-16" into a sentence
	  ending in a chevron. Grown rather than replaced so the existing rows are laid out
	  exactly as they were, and still capped by the canvas: a clipped row is bad and a panel
	  wider than the screen is worse.

	  In advances, not cells: with letter spacing on, 24 * 8 * s is no longer 24 characters.
	  gfx_text_w() already counts the tracking, so the widest-row arm needs no change.
	*/
	int tw = 24 * gfx_adv(s);
	for (int i = 0; i < disc_nrows; i++)
	{
		int w = gfx_text_w(disc_rowtext[i], s);
		if (w > tw) tw = w;
	}

	int maxw = p->w - 2 * p->inset;
	if (tw > maxw - 16 * s) tw = maxw - 16 * s;

	int ph = (10 * s + 6) + 6 * s + 8 * s + 8 * s + nrows * rowh + 8 * s;

	// "Which core?" while the rows were only cores. Now one of them copies the disc to the
	// card, and a list headed by a question none of its answers answers reads as a bug.
	panel_box b = draw_panel_ex(p, tw + 16 * s, ph, "Disc Options");

	int x = b.x + 8 * s;
	int y = b.y + 6 * s;

	/*
	  The disc's name at the head of the list, dim, because the question underneath it is
	  "which core for *this*" and the answer list names consoles rather than the disc.
	  No disc drawn here: the chooser is a question about the library, and the picture
	  belongs to the offer it came from.
	*/
	gfx_text(gfx_clip(d->title, s, tw), x, y, s, COL_PANELLO, 0);
	y += 16 * s;

	for (int i = 0; i < disc_nrows; i++)
	{
		int on = (i == disc_row);
		int yy = y + i * rowh;

		// A "(not yet)" row stays dim even under the selection bar: it can be read and
		// landed on, but nothing about it may look like it will launch.
		int dis = (disc_rowact[i] == DACT_NONE);

		// And the copy row, armed, is red under the cursor: the same colour the suspend
		// strip gives a slot that is one press from being deleted, for the same reason -
		// this press is about to replace a copy of a disc that is already on the card.
		// Only a re-rip of the same serial ever arms it; another disc of the same game is
		// added without asking, because there is nothing to lose by adding it.
		int armed = (disc_rowact[i] == DACT_RIP && rip_over_arm && !CheckTimer(rip_over_until));

		if (on) gfx_fill(x - 4 * s, yy - 3 * s, tw + 8 * s, rowh - 2 * s, armed ? COL_RED : COL_BLUE);
		gfx_text(gfx_clip(disc_rowtext[i], s, tw), x, yy, s,
			dis ? COL_DIM : (on ? COL_WHITE : COL_INK), 0);
	}
}

static void draw_disc(const chome_profile *p)
{
	disc_dlg d;
	disc_dlg_get(&d);

	if (disc_picking) { draw_disc_picker(p, &d); return; }

	disc_build_btns(&d);
	if (disc_btn >= disc_nbtn) disc_btn = disc_nbtn ? disc_nbtn - 1 : 0;

	disc_layout L;
	disc_layout_for(p, &d, &L);

	int s = p->ts_ui;
	panel_box b = draw_panel_at(p, L.px, L.py, L.pw, L.ph, "Disc");
	int cx = b.x + b.w / 2;
	int y = b.y + 6 * s;

	int inner = b.w - 24 * s;              // the margin disc_layout_for() sized it for

	// The game, above its disc, in the size the shelf gives a card's title.
	gfx_text_c(gfx_clip(d.title, L.ts, inner), cx, y, L.ts, COL_INK, 0);
	y += 8 * L.ts + 4 * s;

	gfx_text_c(gfx_clip(d.sub, s, inner), cx, y, s, COL_PANELLO, 0);
	y += 8 * s + 8 * s;

	int cy = y + L.r;
	disc_note_rect(1, cx, cy, L.r, DISC_RECT_CELLS);

	/*
	  The disc, and while a rip runs, how much of it has been copied.

	  rip_reveal() is DISC_REVEAL_FULL whenever there is no rip, so this is the same call
	  every other frame makes and the rectangle noted above is unchanged: the pie is drawn
	  inside the disc's own radius and adds nothing outside it, which is what lets the
	  partial repaint stay byte-identical to a full one. See disc_blit_reveal(), and see the
	  DISC_BADGE_CELLS comment for what happens when something is drawn outside the rect a
	  frame records.
	*/
	disc_draw_face(&d, cx, cy, L.r, rip_reveal());
	y = cy + L.r + 8 * s;

	/*
	  The row of buttons. The focused one is filled with the selection blue this front-end
	  uses for a selected row everywhere else; a dim one is drawn as a hollow outline
	  rather than a plate, so that "cannot" and "not selected" cannot be confused with
	  each other under a CRT's gamma.
	*/
	int total = disc_nbtn * L.bw + (disc_nbtn - 1) * L.gap;
	int bx = cx - total / 2;

	for (int i = 0; i < disc_nbtn; i++)
	{
		int x = bx + i * (L.bw + L.gap);
		int on = (i == disc_btn);
		int dim = disc_btndim[i];

		if (dim)
		{
			gfx_frame_rect(x, y, L.bw, L.bh, on ? COL_DIM : COL_PANELLO, 2);
		}
		else
		{
			gfx_fill(x, y, L.bw, L.bh, on ? COL_BLUE : COL_PANELLO);
			gfx_frame_rect(x, y, L.bw, L.bh, on ? COL_BLUE : COL_PANELLO, 2);
		}

		gfx_text_c(disc_btntext[i], x + L.bw / 2, y + (L.bh - 8 * s) / 2, s,
			dim ? COL_DIM : (on ? COL_WHITE : COL_INK), 0);
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
	// ...or while a rip is copying it, which is the one case where there is a disc to say
	// "there is a disc" about and the drive reports none. See disc_or_rip_present().
	if (!disc_or_rip_present()) return;

	int r = disc_radius(p);
	int cx = p->safe_x + p->inset + r;
	int cy = p->safe_y + p->inset + r;

	/*
	  Recorded at the crest, always, and at the resting centre.

	  This is the trap the whole breath is built around: the partial repaint clips to what
	  the *previous* frame recorded, so a rectangle sized to the radius being drawn now
	  would clip the next frame's larger edge away and leave a ring of the old, smaller
	  disc's pixels standing in the corner - pixels nothing would ever paint over, because
	  the only thing repainting that corner is this same clipped path. See disc_note_rect().
	*/
	disc_note_rect(0, cx, cy, r, DISC_BADGE_CELLS);

	/*
	  The disc and nothing else. No plate behind it and no name beside it, at any
	  profile, focused or not.

	  It turns, which is already enough to notice, and a disc is self-explanatory in a
	  way a label is not - so the label was only ever repeating what the picture said,
	  while costing the room it needed and, at 240p, running across the shelf title.
	  What is *on* the disc belongs in the prompt, where there is room to say it
	  properly.

	  Focus is a ring two cells outside the disc, not a plate behind it, and it breathes
	  between the COL_BLUE this front-end uses for a selected row everywhere else and a
	  lighter blue - see disc_focus_col(). Not white: a white ring merged with the disc's
	  own white rim into one thick band that read as decoration. One cell of ring and one
	  steady colour were both tried and both were too subtle on a real TV at 240p.

	  And the badge itself breathes with it, which is what focus finally reads as from the
	  sofa. A two-cell ring is two pixels at 240p, in the one corner of the screen the eye
	  is least likely to be watching when the player presses up; size is the property of a
	  32-pixel badge that can be seen from across a room. Growing the radius used to show
	  nothing at all - the cell size was r/16 as an integer, so anything short of doubling
	  rendered identically - so gfx_disc maps its grid onto the pixel box now and the sizes
	  in between exist.

	  What the breath quantises to is worth being plain about: at 240p a cell is a pixel
	  and an eighth of 16 is two of them, so there are three sizes (32, 34 and 36 across)
	  and not a continuum; a profile with 2x2 cells gets five. Whole pixels are all a 240p
	  canvas has. The ease is what makes three sizes read as a breath rather than as a
	  flicker - disc_pulse_e() holds near both ends of the swing and moves fastest through
	  the middle, so the eye sees a swell and not three steps.

	  Only on the tier, and `focused` is the whole condition: with the dialog open the
	  screen is SCR_DISC, so the badge behind the scrim neither rings nor breathes. A badge
	  pulsing under a panel would be movement drawing the eye away from the panel.
	*/
	int focused = (screen == SCR_DISCBAR);

	// As in disc_draw_face(): what the badge is a function of, for the spin repaint's
	// skip. The breath and the pulse are not in it, which is why the skip is never
	// taken while the badge has focus.
	disc_drawn_sig = disc_spin_sig();

	gfx_disc(cx, cy, focused ? disc_breath_r(r) : r, disc_step(),
		disc_bands, DISC_BANDS_N, COL_WHITE, COL_PANELHI, COL_BGDARK,
		focused ? disc_focus_col() : 0);

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
	if (pw > 34 * gfx_adv(ps)) pw = 34 * gfx_adv(ps);

	/*
	  36 and not 30: the line underneath is two lines at every profile - the panel is 34
	  characters wide at its widest and the sentence is fifty - and 30 left room for the
	  second one to the pixel, so its bottom row landed exactly on the panel's own edge
	  and the border drew through "than pulling the plug."

	  The same cut sentence as everywhere else, arriving the other way round: gfx_clip()
	  never saw this one, because nothing was too wide. It was too tall.
	*/
	int ph = (10 * ps + 6) + PWR_ROWS * 14 * ps + 36 * ps;

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
			gfx_text_cols(b.w - 16 * s, s), lines, 2);
		for (int i = 0; i < nl; i++)
			gfx_text_c(lines[i], b.x + b.w / 2, ny + i * 10 * s, s, COL_PANELHI, 0);
	}
}

/*
  Putting the game away.

  A screen and not an action on the menu bar, which is the whole reason promoting it is not
  a regression dressed as an improvement. Close Game came off the eleventh row of Options
  because that is far too deep for something a player wants every session - but the two
  presses it took down there were never the depth, they were the safety, and a bar entry
  that closed the game on one press would throw away somebody's afternoon to save them a
  press. So the bar opens this, exactly the way MB_POWER opens SCR_POWER rather than
  restarting the machine where it stands, and the arm-then-confirm lives here.

  Two rows rather than one. The second is what keeps "moving off disarms" a thing a player
  can actually do: that rule was fixed because the timer used to run on while the cursor was
  elsewhere, so press, look away, press again closed the game on what the player had counted
  as the first of two presses - and on a one-row screen the rule would still be in the code
  with nowhere to move to. It is also the way out for somebody who does not know that B goes
  back, on a front-end whose whole point is not requiring that knowledge. "Resume" because
  that is this front-end's word for going back into the running game, on the shelf and over
  a disc alike.

  The arm is ig_close_until, the same timer the Options row used rather than a second one
  beside it. chome_handle() already repaints while it runs and once more when it expires,
  and opening the menu already clears it; a parallel timer would have had to be added to
  both, and the one that got forgotten would be the one leaving a screen saying "Again To
  Confirm" three seconds after it stopped being true.
*/
#define CLS_ROWS 2

static void draw_close(const chome_profile *p)
{
	int ps = p->ts_ui;
	int pw = p->w - 2 * p->inset;
	if (pw > 34 * gfx_adv(ps)) pw = 34 * gfx_adv(ps);
	int ph = (10 * ps + 6) + CLS_ROWS * 14 * ps + 40 * ps;

	panel_box b = draw_panel_ex(p, pw, ph, "Close Game");
	int s = b.s, rowh = 14 * s, y = b.y + 6 * s;

	static const char *rows[CLS_ROWS] = { "Close Game", "Resume" };
	int armed = (cls_row == 0 && !CheckTimer(ig_close_until));

	for (int i = 0; i < CLS_ROWS; i++)
	{
		int on = (i == cls_row);
		int ry = y + i * rowh;

		if (on) gfx_fill(b.x + 4 * s, ry - 3 * s, b.w - 8 * s, rowh - 2 * s,
			(armed && i == 0) ? COL_RED : COL_BLUE);

		gfx_text(rows[i], b.x + 10 * s, ry, s, on ? COL_WHITE : COL_INK, 0);
	}

	int ny = y + CLS_ROWS * rowh + 6 * s;

	/*
	  The two sentences the Options row carried, moved to where the decision is now made.
	  Red for the armed one, which is the colour this front-end keeps for "something is
	  about to be lost", and the reason the plate above turns red with it.

	  Wrapped rather than written straight out: "THE GAME STAYS LOADED UNTIL YOU CLOSE IT"
	  is forty characters into a panel holding about thirty-four at 720p and fewer at 240p,
	  and it used to be served as "...UNTIL YOU CL>" at every profile - the one sentence
	  explaining the cost, cut off before it reached it.
	*/
	char lines[4][64];
	int nl = wrap_text(armed ? "UNSAVED PROGRESS WILL BE LOST"
	                         : "THE GAME STAYS LOADED UNTIL YOU CLOSE IT",
		gfx_text_cols(b.w - 16 * s, s), lines, 2);

	for (int i = 0; i < nl; i++)
		gfx_text_c(lines[i], b.x + b.w / 2, ny + i * 10 * s, s,
			armed ? COL_RED : COL_PANELHI, 0);

	if (armed)
		btn_hint_c(b.x + b.w / 2, ny + nl * 10 * s + 2 * s, s, COL_RED,
			"Press", LBL_A, "again to close the game");
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

/*
  The analog block under the plan: what the video routing is doing to this front-end,
  reported and never written. draw_ini() draws it on all three of its states - the
  plan, the "nothing to change" one and the result - because the machine it is about
  is most often the one with nothing to change, and that is the state where the player
  who came here looking for an explanation would otherwise be told only that
  everything is already set.

  Returns the y below the block, or y unchanged when there is nothing to say.
*/
static int draw_ini_analog(panel_box b, int s, int avail, int y, const int *an, int nan)
{
	if (!nan) return y;

	gfx_text(gfx_clip("Analog video", s, avail), b.x + 6 * s, y, s, COL_PANELLO, 0);
	y += 9 * s;

	for (int i = 0; i < nan; i++)
	{
		const char *t = vp_analog_text(an[i]);
		if (!t) continue;

		// Red only for the one that means no picture at all. The others describe a
		// picture that is limited rather than broken, and a panel of red lines would
		// make a working television look like a fault.
		gfx_text(gfx_clip(t, s, avail), b.x + 14 * s, y + i * 11 * s, s,
			(an[i] == VP_AN_31K) ? COL_RED : COL_INK, 0);
	}

	return y + nan * 11 * s;
}

static void draw_ini(const chome_profile *p)
{
	int s = p->ts_ui;
	int rowh = 11 * s;
	int done = (ini_wrote != -1);

	int w = p->w - 2 * p->inset;
	if (w > 46 * gfx_adv(s)) w = 46 * gfx_adv(s);
	int avail = w - 12 * s;

	/*
	  ts_tiny is ts_ui on every profile, so the two columns are told apart by colour
	  rather than by size - and at 240p they do not both fit on a line at all. Measured
	  rather than assumed: when the widest pair overflows, each setting takes two lines
	  with its key indented under it, which is the one arrangement that keeps the
	  written line visible on the canvas Dinofly's CRT actually gets.
	*/
	int stacked = 0;
	for (int i = 0; i < ini_n; i++)
	{
		char kv[48];
		snprintf(kv, sizeof(kv), "%s=%s", ini_list[i].want->key, ini_list[i].want->value);
		if (gfx_text_w(ini_list[i].want->outcome, s) + gfx_text_w(kv, s) + 8 * s > avail) stacked = 1;
	}

	/*
	  What the analog output is doing, worst first. Read here rather than cached: HDMI
	  can be unplugged while a panel is up, and the answer is three ini reads and one
	  i2c byte behind video_hdmi_connected(), which caches for a second of its own.
	*/
	int an[VP_AN_MAX];
	int nan = 0;
	{
		int facts = vp_analog_facts(video_hdmi_connected());
		for (int bit = 1; bit <= VP_AN_LAST && nan < VP_AN_MAX; bit <<= 1)
			if (facts & bit) an[nan++] = bit;
	}

	/*
	  The whole of what this screen says when there is nothing to do - wrapped, because it
	  is forty-two characters and the panel holds thirty-three at 240p. It read
	  "Everything this menu wants is al>" on the television, which reads as a screen that
	  has broken off mid-thought rather than as one with good news.

	  Wrapped rather than shortened: it is one plain sentence already, and the panel it
	  sits in is sized from these very lines, so a second one costs nothing but a row.
	*/
	char okmsg[4][64];
	int nok = wrap_text("Everything this menu wants is already set.",
		gfx_text_cols(avail, s), okmsg, 2);

	// Sized for its content, like Power, rather than taking the default panel.
	int lines = done ? 3 : (ini_n ? ini_n * (stacked ? 2 : 1) : nok);
	char note[4][64];
	int nnote = done ? 0 : (ini_n ? wrap_text(INI_NOTE, gfx_text_cols(avail, s), note, 3) : 0);

	int h = (10 * s + 6) + 5 * s + lines * rowh + 6 * s + nnote * 9 * s + 6 * s + 12 * s;
	if (nan) h += 6 * s + 9 * s + nan * rowh;
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

		draw_ini_analog(b, s, avail, y + 3 * rowh + 6 * s, an, nan);
		btn_hint_c(b.x + b.w / 2, b.y + b.h - 11 * s, s, COL_INK, "Press", LBL_B, "to close");
		return;
	}

	if (!ini_n)
	{
		for (int i = 0; i < nok; i++)
			gfx_text_c(okmsg[i], b.x + b.w / 2, y + i * rowh, s, COL_INK, 0);
		draw_ini_analog(b, s, avail, y + nok * rowh + 6 * s, an, nan);
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

	draw_ini_analog(b, s, avail, ny + nnote * 9 * s + 6 * s, an, nan);

	// The note stays put while arming so the panel does not resize under the player;
	// only the line they are about to act on changes.
	int armed = (ini_armed && !CheckTimer(ini_until));
	if (armed) btn_hint_c(b.x + b.w / 2, b.y + b.h - 11 * s, s, COL_RED, "Press", LBL_A, "again to write");
	else btn_hint_c(b.x + b.w / 2, b.y + b.h - 11 * s, s, COL_INK, "Press", LBL_A, "to change them");
}

/* ------------------------------------------------------------- the font --- */

#define FONT_DIR "font"
#define FONT_HELP "The glyphs this menu draws. Put .pf files in font/."

// The name on the row: "Built-in", or the file without its folder and extension. The
// extension is dropped because every entry has it, and a column of ".PF" says nothing.
static const char *font_label(int i, char *buf, int max)
{
	if (i <= 0 || i >= font_n) return "Built-in";

	const char *base = strrchr(font_rel[i], '/');
	base = base ? base + 1 : font_rel[i];
	snprintf(buf, (size_t)max, "%s", base);

	char *dot = strrchr(buf, '.');
	if (dot && !strcasecmp(dot, ".pf")) *dot = 0;
	return buf;
}

static int font_cmp(const void *a, const void *b)
{
	return strcasecmp((const char*)a, (const char*)b);
}

/*
  What is on the card, plus what the ini says, whether or not those are the same thing.

  Sorted, because readdir() order is whatever the filesystem felt like and a list that
  reorders itself between two visits is a list nobody can navigate from memory. The
  built-in stays at index 0 outside the sort - it is the way back, not one of the files.
*/
static void font_scan()
{
	font_n = 1;
	font_rel[0][0] = 0;

	char dir[1024];
	snprintf(dir, sizeof(dir), "%s/%s", getRootDir(), FONT_DIR);

	DIR *d = opendir(dir);
	if (d)
	{
		struct dirent *de;
		while ((de = readdir(d)) && font_n < FONT_MAX)
		{
			if (de->d_name[0] == '.') continue;

			const char *dot = strrchr(de->d_name, '.');
			if (!dot || strcasecmp(dot, ".pf")) continue;

			snprintf(font_rel[font_n], sizeof(font_rel[font_n]), "%s/%s", FONT_DIR, de->d_name);
			font_n++;
		}
		closedir(d);

		// Said out loud rather than silently, as the file browser's own cap is: a player with
		// more than this on the card would otherwise be looking for a font that is on it.
		if (font_n >= FONT_MAX)
			printf("ClassicUI: font list capped at %d files in %s/\n", FONT_MAX - 1, FONT_DIR);
	}

	if (font_n > 2) qsort(font_rel[1], (size_t)(font_n - 1), sizeof(font_rel[0]), font_cmp);

	/*
	  And whatever `font=` names, if the scan did not find it - a font in some other folder,
	  or one that has been deleted since the ini was written. Either way the row has to be
	  able to show it, because it is what the machine is set to.
	*/
	font_was = 0;
	if (cfg.font[0])
	{
		for (int i = 1; i < font_n; i++) if (!strcmp(font_rel[i], cfg.font)) font_was = i;

		if (!font_was && font_n < FONT_MAX)
		{
			snprintf(font_rel[font_n], sizeof(font_rel[font_n]), "%s", cfg.font);
			font_was = font_n++;
		}
	}

	font_sel = font_was;
}

static int font_dirty() { return (font_sel != font_was) ? 1 : 0; }

/*
  Put a choice on the screen now.

  The built-in comes from the copy charrom.cpp keeps; anything else is a fresh LoadFont(),
  which is the same call boot makes and the reason a custom font needed no code in this
  front-end at all. Restoring first is what makes a *failed* load harmless in the other
  direction too: LoadFont() leaves the table alone when the file will not read, so without
  the restore, stepping from font A to a missing font B would leave A on screen while the
  row said B.

  0 back when the file would not load, and the caller says so on the footer rather than
  leaving the player looking at a font that is not the one named. That case is the whole
  reason LoadFont() has a return value now.
*/
static int font_apply(int i)
{
	FontRestoreBuiltin();

	int ok = 1;
	if (i > 0 && i < font_n && font_rel[i][0])
	{
		// A copy, because LoadFont() takes a char* and the list is what the row is showing.
		char path[sizeof(font_rel[0])];
		snprintf(path, sizeof(path), "%s", font_rel[i]);
		ok = LoadFont(path);
	}

	// Every glyph on screen changed, and nothing in the compositor could know that from
	// the shape of any region: this is a full repaint or it is a screen of two fonts.
	gfx_damage_all();
	mark_dirty();
	return ok;
}

/* ------------------------------------------------------- more settings ---- */

#define SET_SAVE_NOTE "Writes the changes into MiSTer.ini. A copy is kept."

/*
  Read the card and count what is not at its recommended value. Rebuilt on the way in to
  Options and to the screen itself, never per frame: which options apply depends on the
  video path, which cannot change while a panel is up over it, and the values only move
  when we move them.
*/
static int set_nrows() { return set_nview + 2; }
static int set_pending() { return opt_dirty() + font_dirty(); }

static void set_summary()
{
	set_nview = opt_view(set_view, OPT_MAX, video_scaler_is_visible());
	opt_load(ini_path());
	font_scan();

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
	font_note[0] = 0;
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

/*
  Step the font, and show it at once.

  Live rather than on save, unlike every other row here, and the difference is the point:
  the value of this setting is what the letters look like, and a font chosen from a list of
  file names without seeing it is a guess. So the screen the player is reading is drawn in
  the font under the cursor - which also means leaving without saving has to put the old
  one back, and B already asks before it does that.
*/
static void set_font_step(int dir)
{
	int n = font_sel + dir;
	if (n < 0) n = font_n - 1;
	if (n >= font_n) n = 0;
	if (n == font_sel) return;

	font_sel = n;

	char nm[64];
	if (font_apply(font_sel)) font_note[0] = 0;
	else snprintf(font_note, sizeof(font_note), "%s will not load - still on the old font",
		font_label(font_sel, nm, sizeof(nm)));

	set_edited();
}

/* ------------------------------------------------------- core options ----- */

static int co_rows()
{
	int n = core_opts_tier_count(co_tier);
	// The last row switches page, so there is always one more than there are options.
	return n + 1;
}

/*
  The promotion confirmation stops being true the moment anything else touches the store:
  changing a value puts a per-game override back, X throws one away, and opening the screen
  again is a new visit. In each of those the last promotion is no longer the news, and a
  footer still claiming it would describe the state the player just left.
*/
static void co_news_clear()
{
	co_promoted[0] = 0;
	co_promoted_until = 0;
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
	if (pw > 46 * gfx_adv(s)) pw = 46 * gfx_adv(s);
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

	/*
	  Windowed, like every other list in here. This one needs it most: the PSX's System page
	  is 27 rows and about fifteen fit, and draw_rows_c() simply stops when a row would cross
	  the bottom edge - so the rows past the fold existed, were selectable with the stick, and
	  were never drawn. That is the same defect that hid Close Game in the Options panel, and
	  it is why the SNAC rows could not be offered here until now.

	  One row of footer to keep clear of. The promotion line above it is transient and shares
	  that space rather than claiming its own, which is deliberate: reserving a second row
	  permanently to caption something that appears for three seconds would cost a row of
	  every page, on every core, for ever.
	*/
	/*
	  The help line for the row the cursor is on, when that row is one of the SNAC controls.

	  Per *value*, not per row, which is the whole reason it exists: "Pad1" says nothing
	  useful, while "Pad1 = SNAC-port1" and "Pad1 = Dualshock" have opposite consequences and
	  neither is guessable. One of them costs the player the ability to open this menu with
	  the pad in their hands, which is not a thing to discover by trying it.

	  Two wordings, the same as every other line down here: at 240p the panel is not wide
	  enough for the long one, and a sentence that loses its end is worse than a short one.
	*/
	const char *cohelp = 0;
	if (co_row < n)
	{
		const core_opt *sel = core_opt_tier_at(co_tier, co_row);
		if (sel)
		{
			int wide = gfx_text_cols(b.w - 12 * s, p->ts_tiny) >= 36;
			const char *v = sel->vals[core_opt_value(sel)];
			int is_snac = (v && strcasestr(v, "SNAC"))
				|| (!strcasecmp(sel->name, "SNAC") && core_opt_value(sel) != 0)
				|| (!strcasecmp(sel->name, "USERIO") && v && strcasestr(v, "SNAC"));

			if (!strcasecmp(sel->name, "Pad1") || !strcasecmp(sel->name, "Pad2")
				|| !strncasecmp(sel->name, "Pad ", 4) || !strcasecmp(sel->name, "SNAC")
				|| !strcasecmp(sel->name, "USERIO"))
			{
				if (is_snac) cohelp = wide ? "The core reads the port: guns and real cards, no menu"
					: "Guns work - the pad cannot open this";
				else cohelp = wide ? "We read the port: Select+Start opens this, cards virtual"
					: "Select+Start opens this menu";
			}
			else if (!strcasecmp(sel->name, "SNAC MemCard"))
			{
				cohelp = wide ? "Real cards need Pad1 or Pad2 set to SNAC first"
					: "Needs Pad1 on SNAC";
			}
		}
	}

	int nrows = i;
	int cofoot = 22 * s;
	int cofit = list_fit(&b, 12 * b.s, cofoot, nrows);
	list_track(&co_top, co_row, nrows, cofit);

	draw_rows_c(&b, rows + co_top, vals + co_top, vcol + co_top, cofit, co_row - co_top);
	list_scrollbar(&b, 12 * b.s, co_top, cofit, nrows);

	int fy = b.y + b.h - 12 * s;
	/*
	  Two wordings again: at 240p the panel is not wide enough for the long one, and a
	  footer that loses its end is worse than a short one that does not.
	*/
	int room = gfx_text_cols(b.w - 12 * s, p->ts_tiny);
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

	/*
	  The SNAC help outranks all of the lines above, because it is about the row the cursor is
	  on rather than about the page. The star and the (U) warning describe a property of the
	  list; this describes what the highlighted value will *do*, and one of its values takes
	  the player's ability to reach this menu away. Ranked under the promotion below, which is
	  transient and reports something that just happened.
	*/
	if (cohelp) foot = cohelp;

	/*
	  ...and above all of them, for a few seconds, the one thing the screen cannot show any
	  other way.

	  Handing a setting to every game writes a file nobody can see and moves nothing on the
	  core - the value was already on it - so the only visible effect is the row's star going
	  away, and that is exactly what X does too. Two opposite actions with one appearance is
	  the sort of thing a player learns wrong once and never trusts again, so the promotion
	  says which it was, in words, and names the row it happened to.

	  Green because that is what this front-end already uses for a write that landed, in the
	  Settings screen's SAVED line. The name is clipped rather than dropped: on a narrow
	  panel "ALL GAMES:" is the part that carries the meaning, and the player has just moved
	  the cursor there.
	*/
	char promo[64];
	if (co_promoted[0] && !CheckTimer(co_promoted_until))
	{
		snprintf(promo, sizeof(promo), (room >= 36) ? "ALL GAMES NOW: %s" : "ALL GAMES: %s",
			co_promoted);
		gfx_shout(promo);
		gfx_text(gfx_clip(promo, p->ts_tiny, b.w - 12 * s), b.x + 6 * s, fy, p->ts_tiny,
			COL_GREEN, 0);
		return;
	}

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

	int nrows = set_nrows();                     // the options, then Font, then Save Changes
	int fit = list_fit(&b, rowh, foot, nrows);
	list_track(&set_top, set_row, nrows, fit);

	const char *labels[OPT_MAX + 2];
	const char *vals[OPT_MAX + 2];
	uint32_t vcol[OPT_MAX + 2];
	static char vbuf[OPT_MAX + 2][24];

	int dirty = set_pending();
	int n = 0;

	for (int r = set_top; r < set_top + fit && n <= OPT_MAX + 1; r++)
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
		else if (r == SET_ROW_FONT)
		{
			labels[n] = "Font";
			vals[n] = font_label(font_sel, vbuf[n], sizeof(vbuf[n]));

			/*
			  Never amber. The other rows' colour means "away from what this menu
			  recommends", and this menu has no opinion about which font somebody should
			  read their shelf in - the built-in is the one that ships, not the one that is
			  right. Red is different: it means the file named on the row would not load,
			  which is a fact about the card rather than a preference.
			*/
			vcol[n] = font_note[0] ? COL_RED : 0;
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
	list_scrollbar(&b, rowh, set_top, fit, nrows);

	int fy = b.y + b.h - foot + 2 * s;
	int cols = gfx_text_cols(b.w - 12 * s, s);

	const char *help = SET_SAVE_NOTE;
	if (od) help = od->help;
	else if (set_row == SET_ROW_FONT) help = FONT_HELP;
	if (set_failed) help = opt_error();

	char wrapped[4][64];
	int nl = wrap_text(help, cols, wrapped, 2);
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
	if (set_row == SET_ROW_SAVE && dirty)
	{
		btn_hint_c(b.x + b.w / 2, y3, s, COL_INK, "Press", LBL_A, "to save");
		return;
	}
	// A font that would not load outranks the rest of this line: the row above says a name
	// and the screen is not drawn in it, and nothing else here could explain that.
	if (font_note[0])
	{
		char u[96];
		snprintf(u, sizeof(u), "%s", font_note);
		gfx_shout(u);
		gfx_text_c(gfx_clip(u, s, b.w - 12 * s), b.x + b.w / 2, y3, s, COL_RED, 0);
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
		gfx_shout(u);
		gfx_text_c(gfx_clip(u, s, b.w - 12 * s), b.x + b.w / 2, y3, s, COL_YELLOW, 0);
	}
}

/* ------------------------------------------------------ online covers ----- */

/*
  Online Covers: the player's own ScreenScraper account, set up from the console.

  ------------------------------------------------------------- where it lives ---

  Under Options, in the row directly below Cover Art, and that placement is an argument
  rather than the nearest free slot:

  Cover Art is the row that decides whether a missing cover is looked for on the network
  at all (classicui_artfetch). This screen is where that lookup gets somewhere to look.
  The two are one chain - disc_art_request() in chome_art.cpp asks for classicui_artfetch
  AND ss_enabled() before it asks the database for anything, and a player who turns one on
  without the other gets nothing and is told nothing - so they are adjacent, and each row
  says enough about its own state that the pair can be read at a glance.

  It is deliberately not in Options > More Settings. That screen is a table of MiSTer.ini
  options whose values are numbers picked from a list or stepped within cfg.cpp's declared
  range, and whose `live` pointer is a uint8_t* - see the comment on opt_def in
  chome_opt.h, which says a wider cfg field needs that widened rather than silently
  scribbling on the field after it. A 64-character login is not a number in a range, and
  bending that table into holding one would cost the property that makes it safe: every
  value it can write is one cfg.cpp will accept.

  And not in Options > Best Settings either. That screen is the front-end's own opinion
  about four MiSTer.ini keys, written on the player's behalf; somebody else's account is not
  a value we have an opinion about. chome_ini.cpp says so already, in the list of keys it
  refuses to touch: "classicui_ss_user / classicui_ss_pass are somebody's login. There is
  no value to write."

  --------------------------------------------------------------- the password ---

  It is never drawn. The row says whether one is set, not what it is; there is no
  confirmation step that echoes it; nothing here prints it, and cov_open_pass() does not
  hand it to the keyboard as an initial value - the keyboard starts a masked field
  *visible* by design (see chome_osk.h), so pre-filling it would put a stored password on
  screen without anybody asking for it.

  What this does not fix, and is not trying to: the value lives in MiSTer.ini in clear,
  the way every MiSTer option does, and the row for it says so. Making that untrue is a
  change to the configuration format, not to a settings screen.
*/
#define COV_SAVE_NOTE "Writes your account into MiSTer.ini. A copy is kept."
#define COV_INI_NOTE  "; Written by Classic Home - Options > Online Covers."

// Staged from cfg on the way in, so the screen cannot change under the player and
// leaving without saving throws the edits away rather than half of them.
static void cov_refresh()
{
	cov_on = cfg.classicui_screenscraper ? 1 : 0;
	snprintf(cov_user, sizeof(cov_user), "%s", cfg.classicui_ss_user);
	snprintf(cov_pass, sizeof(cov_pass), "%s", cfg.classicui_ss_pass);

	cov_row = 0;
	cov_arm = 0;
	cov_quit_arm = 0;
	cov_wrote = -1;
	cov_failed = 0;
	cov_note[0] = 0;
	cov_note_until = 0;
}

static int cov_dirty()
{
	int n = 0;
	if (cov_on != (cfg.classicui_screenscraper ? 1 : 0)) n++;
	if (strcmp(cov_user, cfg.classicui_ss_user)) n++;
	if (strcmp(cov_pass, cfg.classicui_ss_pass)) n++;
	return n;
}

// A value moved: the last write stops being the news, and an armed save is no longer a
// save of what the player armed it for. Same rule as More Settings.
static void cov_edited()
{
	cov_wrote = -1;
	cov_failed = 0;
	cov_arm = 0;
	mark_dirty();
}

// One thing to say about the last press, for long enough to read and then gone.
static void cov_say(const char *msg)
{
	snprintf(cov_note, sizeof(cov_note), "%s", msg);
	cov_note_until = GetTimer(6000);
	mark_dirty();
}

/*
  Which character MiSTer.ini would not keep, so the refusal can name it instead of
  listing every character it might have been.

  Probed between two letters rather than on its own: ini_value_ok() also refuses a value
  that starts or ends with a space, and a bare " " would come back as "the space is the
  problem" for a password with a space in the middle of it, which is a lie. 0 means every
  character was fine and it was the ends that failed.
*/
static char cov_bad_char(const char *v)
{
	for (const char *p = v; *p; p++)
	{
		char probe[4] = { 'a', *p, 'a', 0 };
		if (!ini_value_ok(probe)) return *p;
	}
	return 0;
}

/*
  A value the player just typed, checked before it is staged.

  This is the bug that made ini_value_ok() exist, and it is worth stating in full because
  nothing on screen could ever have shown it. MiSTer.ini is read by cfg.cpp's
  ini_getline(), which keeps a character only if it is alphanumeric or one of a short list
  of punctuation and silently drops the rest; a ';' does not even get that far, because it
  ends the line as a comment. The on-screen keyboard offers & % ; ? quotes and backslashes
  on its symbol page, and & and % in particular are ordinary in a password.

  So the failure was: the player types their real password, the screen says it is set, the
  correct string is written to MiSTer.ini, and the firmware reads back a *different*
  string on the next core load. Every request from then on fails on credentials, and there
  is no screen anywhere that could show the difference, because the difference is between
  the file and the parser.

  Refusing at entry rather than at save is deliberate: the player is told while they still
  remember what they typed, and the staged value is left as it was rather than being
  replaced by something that cannot work.
*/
static int cov_accept_text(const char *v, const char *what)
{
	if (ini_value_ok(v)) return 1;

	char bad = cov_bad_char(v);
	char msg[96];

	if (bad) snprintf(msg, sizeof(msg), "MiSTer.ini cannot store %c - change your %s", bad, what);
	else snprintf(msg, sizeof(msg), "MiSTer.ini trims a space or = from the ends");

	cov_say(msg);
	return 0;
}

/*
  Write the three of them, and only the ones that moved.

  Through ini_apply_set(), which backs the file up, replaces our keys wherever they
  already appear - including in a core section, where a stale copy would override the one
  we just fixed - and appends what was missing under a [MiSTer] header of its own rather
  than at the end of whatever section the file happens to stop inside. None of that is
  this screen's to reimplement; see chome_ini.h.

  Then cfg, because cfg is what the running firmware reads: ss_enabled() is asked on every
  cover request and it looks at cfg, not at the file. Writing one without the other is the
  version of this that appears to work and does nothing until the next core load.
*/
static int cov_apply()
{
	ini_set set[3];
	int n = 0;

	const char *onv = cov_on ? "1" : "0";

	if (cov_on != (cfg.classicui_screenscraper ? 1 : 0))
	{
		set[n].key = "classicui_screenscraper";
		set[n].value = onv;
		n++;
	}
	if (strcmp(cov_user, cfg.classicui_ss_user))
	{
		set[n].key = "classicui_ss_user";
		set[n].value = cov_user;
		n++;
	}
	if (strcmp(cov_pass, cfg.classicui_ss_pass))
	{
		set[n].key = "classicui_ss_pass";
		set[n].value = cov_pass;
		n++;
	}

	if (!n) return 0;

	if (ini_apply_set(ini_path(), set, n, COV_INI_NOTE) < 0) return -1;

	cfg.classicui_screenscraper = (uint8_t)cov_on;
	snprintf(cfg.classicui_ss_user, sizeof(cfg.classicui_ss_user), "%s", cov_user);
	snprintf(cfg.classicui_ss_pass, sizeof(cfg.classicui_ss_pass), "%s", cov_pass);

	// The count, not the values. This is the one line in the front-end that could
	// casually put somebody's password in /tmp/debug.txt.
	printf("ClassicUI: online covers - %d setting%s written\n", n, n == 1 ? "" : "s");
	return n;
}

static void cov_open_user()
{
	osk_dest = OSKD_SS_USER;
	osk_open("Account name", "Your own account on screenscraper.fr", cov_user, 0);
}

/*
  The keyboard for the password, and the argument is about the empty string it is handed.

  osk_open()'s masked mode starts *visible* - chome_osk.h explains why, and it is the right
  call for somebody spelling a password out one letter at a time on a pad. But it means the
  initial value is displayed the instant the keyboard opens. Pre-filling this with the
  stored password would therefore print it on the television for a player who pressed A to
  see what the row said, which is precisely what "the password is never displayed" rules
  out. So the field starts empty and a password is re-typed rather than edited.
*/
static void cov_open_pass()
{
	osk_dest = OSKD_SS_PASS;
	osk_open("Password", "The password for that account", "", 1);
}

static void draw_covers(const chome_profile *p)
{
	int s = p->ts_ui;
	int rowh = 12 * s;
	int avail = ss_available();

	// Three lines at the bottom, as on More Settings: two for the sentence about the
	// selected row and one for whatever the screen has to say about the whole of it.
	int foot = 3 * 10 * s + 4 * s;

	int pw = p->w - p->inset * 2;
	if (pw > 46 * gfx_adv(s)) pw = 46 * gfx_adv(s);
	// A margin under the footer as well as over the first row. Without it the status line
	// rests on the panel border, which at 240p reads as text falling off the edge.
	int botpad = 6 * s;

	int ph = (10 * s + 6) + 5 * s + COV_ROWS * rowh + foot + botpad;
	if (ph > p->h - 2 * p->safe_y) ph = p->h - 2 * p->safe_y;

	panel_box b = draw_panel_ex(p, pw, ph, "Online Covers");

	int dirty = cov_dirty();

	static const char *labels[COV_ROWS] = { "Online Covers", "Account Name", "Password", "Save Changes" };
	const char *vals[COV_ROWS];
	uint32_t vcol[COV_ROWS];
	char vname[40], vsave[24];

	/*
	  The switch row carries the same five-state phrase the Options row does, rather than a
	  plain On/Off. It says more where it matters: "On" with no password is the state a
	  player would otherwise sit in believing they had finished, and the row they switched
	  it on with is where they will look.
	*/
	vals[COV_ON] = cov_state_of(avail, cov_on, cov_user, cov_pass[0] != 0);

	/*
	  The account name, clipped by us. draw_rows_c() right-aligns the value column and
	  does not clip it, and a ScreenScraper login can be 64 characters; an unclipped one
	  would be drawn straight through the label and off the left edge of the panel.
	*/
	snprintf(vname, sizeof(vname), "%s",
		cov_user[0] ? gfx_clip(cov_user, s, b.w / 2 - 12 * s) : "Not Set");
	vals[COV_USER] = vname;

	// Set, not the password. There is no state of this screen in which the value is text.
	vals[COV_PASS] = cov_pass[0] ? "Set" : "Not Set";

	if (dirty) snprintf(vsave, sizeof(vsave), "%d To Save", dirty);
	else if (cov_wrote > 0) snprintf(vsave, sizeof(vsave), "Saved");
	else snprintf(vsave, sizeof(vsave), "Nothing To Save");
	vals[COV_SAVE] = vsave;

	/*
	  Colour says which row is the one to act on. Dim throughout on a build with no
	  credential - the same "there, and visibly not in effect" the core options screen
	  uses for an option the core says does not apply - and amber on whichever half of
	  the account is missing while the switch is on, because that is the row that turns
	  "On" back into nothing being fetched.
	*/
	for (int i = 0; i < COV_ROWS; i++) vcol[i] = avail ? 0 : COL_DIM;

	if (avail && cov_on)
	{
		if (!cov_user[0]) vcol[COV_USER] = COL_YELLOW;
		else if (!cov_pass[0]) vcol[COV_PASS] = COL_YELLOW;
	}
	if (!dirty && cov_wrote > 0) vcol[COV_SAVE] = COL_GREEN;

	draw_rows_c(&b, labels, vals, vcol, COV_ROWS, cov_row);

	int fy = b.y + b.h - botpad - foot + 2 * s;
	int cols = gfx_text_cols(b.w - 12 * s, s);

	/*
	  What the selected row is for - or, ahead of it, the one thing that outranks every
	  row: a build with no application credential. Saying it here rather than only in the
	  status line is the difference between "this is off" and "nothing you type on this
	  screen can ever work", and a player who does not know which they are looking at will
	  keep typing.
	*/
	static const char *help[COV_ROWS] = {
		"Asks ScreenScraper for covers no local folder has.",
		"Your own free account on screenscraper.fr.",
		"Never shown here. MiSTer.ini keeps it in clear.",
		COV_SAVE_NOTE
	};

	int said = (cov_note[0] && !CheckTimer(cov_note_until));
	const char *body = help[cov_row];
	if (!avail) body = "This build carries no ScreenScraper credential.";
	if (cov_failed) body = ini_last_error();
	if (said) body = cov_note;

	char wrapped[4][64];
	int nl = wrap_text(body, cols, wrapped, 2);
	for (int i = 0; i < nl; i++)
		gfx_text(wrapped[i], b.x + 6 * s, fy + i * 10 * s, s,
			(said || cov_failed) ? COL_RED : COL_PANELLO, 0);

	/*
	  And the line that changes, by urgency: an armed press first because the player is one
	  press from it, then the build, then the result of the last write, then what is still
	  missing. Every wording here is 29 characters or fewer, which is what the panel holds
	  at 240p - the same measurement "SAVED - FROM THE NEXT GAME ON" was cut to next door.
	*/
	int y3 = fy + 2 * 10 * s;

	if (cov_arm && !CheckTimer(cov_arm_until))
	{
		btn_hint_c(b.x + b.w / 2, y3, s, COL_RED, "Press", LBL_A, "again to save");
		return;
	}
	if (cov_quit_arm && !CheckTimer(cov_quit_until))
	{
		btn_hint_c(b.x + b.w / 2, y3, s, COL_RED, "Press", LBL_B, "again to lose the changes");
		return;
	}
	if (!avail)
	{
		gfx_text_c(gfx_clip("NO CREDENTIAL IN THIS BUILD", s, b.w - 12 * s),
			b.x + b.w / 2, y3, s, COL_RED, 0);
		return;
	}
	if (cov_row == COV_SAVE && dirty)
	{
		btn_hint_c(b.x + b.w / 2, y3, s, COL_INK, "Press", LBL_A, "to save");
		return;
	}
	if (!dirty && cov_wrote > 0)
	{
		gfx_text_c(gfx_clip("SAVED - IN EFFECT NOW", s, b.w - 12 * s),
			b.x + b.w / 2, y3, s, COL_GREEN, 0);
		return;
	}

	// "On" with half an account is the state this whole screen exists to stop somebody
	// sitting in without knowing. Named as what is missing, not as a warning symbol.
	if (cov_on && !cov_user[0])
		gfx_text_c(gfx_clip("ON, BUT NO ACCOUNT NAME", s, b.w - 12 * s),
			b.x + b.w / 2, y3, s, COL_YELLOW, 0);
	else if (cov_on && !cov_pass[0])
		gfx_text_c(gfx_clip("ON, BUT NO PASSWORD", s, b.w - 12 * s),
			b.x + b.w / 2, y3, s, COL_YELLOW, 0);
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
		gfx_shout(up);
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
	gfx_shout(hdr);
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
		gfx_shout(nm);

		// File names off the card, which are the longest strings this front-end draws and the
		// ones a player most needs to read to the end: two dumps of a game differ in the
		// bracket at the very end of the name.
		gfx_text(marq_fit(nm, s2, p->w - p->inset * 2, on, y), p->inset, y, s2,
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

static void launch_write_mgl(const chome_sys *s, const char *relpath, const chome_slot *slot, const char *rbf)
{
	FILE *f = fopen("/tmp/classicui_launch.mgl", "wt");
	if (!f) return;

	char type = slot ? slot->type : s->type;
	int index = slot ? slot->index : s->index;

	fprintf(f, "<mistergamedescription>\n");
	fprintf(f, "\t<rbf>%s</rbf>\n", rbf ? rbf : s->rbf);
	fprintf(f, "\t<file delay=\"%d\" type=\"%c\" index=\"%d\" path=\"%s\"/>\n",
		s->delay ? s->delay : 2, type == 's' ? 's' : 'f', index, relpath);
	fprintf(f, "</mistergamedescription>\n");
	fclose(f);
}

/*
  `slot` and `rbf` are the two ways a launch can deviate from the system's shelf
  defaults, and both exist for the physical disc: the disc goes into the core's CD
  slot rather than the shelf's ROM slot, and - for Mega CD alone so far - into a
  different core than the shelf system's own, because the Genesis core has no disc
  input at all. Everything else about launching is identical, which is why these
  are parameters on the one launch path rather than a second one.
*/
static void do_launch(int sysidx, const char *relpath, chome_item *it, const chome_slot *slot = 0, const char *rbf = 0)
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
			const char *rbfpath = rbf ? rbf : s->rbf;
			if (!s->mra && rbfpath[0])
			{
				const char *slash = strrchr(rbfpath, '/');
				core = slash ? slash + 1 : rbfpath;
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

	launch_write_mgl(s, relpath, slot, rbf);
	printf("ClassicUI: launching %s via %s\n", relpath, rbf ? rbf : s->rbf);
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

  Four so far. PC Engine CD and Mega CD read a real disc at full speed upstream;
  PlayStation is wired the same way in psx.cpp but upstream reports it short of full
  speed from a drive, so expect FMV and CD audio to be the rough parts there. Neo Geo
  CD has no daemon of its own - neocd_set_image() recognises the sentinel and the
  sectors come from the same shared cdd_t in support/megacd that serves Mega CD, which
  is why that port had to land first. The other CD daemons each need the same work done
  to them separately; a system that is not in this table is still identified and still
  named by the prompt, but its row is marked "(not yet)" and refuses - see
  disc_build_rows() - instead of loading a core that would find nothing in the slot.

  The slot is the core's own SD-card index for its CD image and comes from the "S"
  entry in each core's config string; getting it wrong mounts the disc into the wrong
  core input, so each entry cites its source.

  Two qualifiers beyond the slot:

  - `dtype`: which identified disc the row is for, because one shelf system can host
    discs that need different cores - a Mega CD disc and a Mega Drive+ disc both map
    to "md". DISC_T_NONE means any disc, which keeps tg16 and psx behaving as before
    (their cores are also where an unknown or forced disc is allowed to go). A system
    whose entries all name types refuses discs of any other type, so an "md" row for
    a Mega Drive+ disc stays "(not yet)" until mdplus.cpp learns the drive.

  - `rbf`: the core that takes the disc, when it is not the shelf system's own.
    "md"'s shelf core is Genesis, whose config string has no "S" entry at all - a
    disc handed to it would mount into nothing - so Mega CD discs load the separate
    MegaCD core instead.
*/
struct disc_playable
{
	const char *sysid;
	int dtype;              // DISC_T_* this row serves; DISC_T_NONE = any disc
	chome_slot slot;
	const char *rbf;        // 0 = the system's own core takes the disc
};

static const disc_playable disc_playables[] =
{
	{ "tg16", DISC_T_NONE,   { 's', 0 }, 0 },   // "S0,CUECHD,Insert CD" in TurboGrafx16.sv
	{ "psx",  DISC_T_NONE,   { 's', 1 }, 0 },   // "H7S1,CUECHD,Load CD" in PSX.sv (H7 is a hide
	                                            // mask, not part of the slot; S2/S3 are its
	                                            // memory cards)
	{ "md",   DISC_T_MEGACD, { 's', 0 }, "_Console/MegaCD" },  // "S0,CUECHD,Insert Disk" in MegaCD.sv
	{ "md",   DISC_T_AUDIO,  { 's', 0 }, "_Console/MegaCD" },  // the Mega CD BIOS is a CD player,
	                                            // and megacd.cpp mounts an audio-only disc
	{ "neogeo", DISC_T_NONE, { 's', 1 }, 0 },   // "S1,CUECHD,Load CD Image" in neogeo.sv. Index 1
	                                            // is also its romset slot ("FS1,*,Load ROM set")
	                                            // but that one is type 'f' - menu.cpp routes any
	                                            // 's' mount on this core to neocd_set_image().
};

static const disc_playable *disc_play_for(int sysidx)
{
	const chome_sys *s = (sysidx >= 0) ? lib_sys(sysidx) : 0;
	if (!s) return 0;

	// The disc now in the drive decides among a system's rows; a typed row outranks
	// the any-disc one so a future specific entry can carve a type out of it.
	const disc_playable *any = 0;
	for (unsigned i = 0; i < sizeof(disc_playables) / sizeof(disc_playables[0]); i++)
	{
		const disc_playable *pl = &disc_playables[i];
		if (strcasecmp(pl->sysid, s->id)) continue;
		if (pl->dtype == disc_type()) return pl;
		if (pl->dtype == DISC_T_NONE) any = pl;
	}
	return any;
}

static int disc_wired(int sysidx)
{
	return disc_play_for(sysidx) != 0;
}

/*
  Set once the drive has been handed to a core, within the process that handed it over.

  The drive can have exactly one owner. Detection is a helper process that holds
  /dev/sr0 open and polls its status; the core's reader opens the same device in *this*
  process and streams sectors from it on the thread that also draws. Every ioctl on
  that device serialises behind whatever the drive is doing, so a status poll from the
  helper would put itself in front of a sector the core needs now - see chome_disc.h
  for the two freezes that measured this.

  So the helper is stopped before the launch and must not come back, and disc_poll()
  restarts it whenever it finds it stopped. Hence a latch rather than just stopping it.

  This latch is NOT enough on its own, and the comment here used to claim it was, on the
  grounds that "the firmware process survives a core change". It does not: loading a core
  re-execs the firmware, so every static in this file - including this one - is 0 again in
  the process that actually runs the game. disc_poll() then found nothing watching and
  forked a fresh helper straight onto the drive the core was reading. Measured on the
  device with Metal Gear Solid playing: the firmware held /dev/sr0, and a child of it held
  the same device on its own descriptor. See core_holds_disc() for the half that survives.
*/
static int disc_handed_to_core = 0;

/*
  Whether the drive belongs to the core rather than to us, decided from what this process
  can actually see rather than from what a previous one remembered.

  A game core with the sentinel mounted is playing the disc, and the drive is its own.
  Anything else - the menu core, or a game launched from a file - leaves the drive free
  and detection is welcome to it.

  Cached because it cannot change without another re-exec, and because cur_read() has a
  side effect: it unlinks a launch record naming a core that is not running.
*/
static int cur_read(char *sysid, int syslen, char *rompath, int pathlen);

static int core_holds_disc()
{
	static int cached = -1;
	if (cached >= 0) return cached;

	if (is_menu()) { cached = 0; return cached; }

	char sysid[64] = {}, rompath[CH_PATH_LEN] = {};
	cached = (cur_read(sysid, sizeof(sysid), rompath, sizeof(rompath))
		&& !strcmp(rompath, PHYSICAL_DISC_SENTINEL)) ? 1 : 0;
	return cached;
}

static void disc_launch(int sysidx)
{
	const disc_playable *pl = disc_play_for(sysidx);
	const chome_sys *s = (sysidx >= 0) ? lib_sys(sysidx) : 0;

	// The whole feature is off by default, and the only way here is through a screen
	// that only exists when it is on - but this is the point where the drive gets used
	// in earnest, so it does not rely on that.
	if (!cfg.classicui_disc || !pl || !s) { nudge(); return; }

	// A row's own core rather than the system's, renamed the way this card names it
	// (a US-named card carries SegaCD where the table says MegaCD).
	char rbf[64];
	if (pl->rbf)
	{
		snprintf(rbf, sizeof(rbf), "%s", pl->rbf);
		lib_resolve_rbf(rbf, sizeof(rbf));
	}

	printf("ClassicUI: handing the disc to %s (%s)\n", s->name, disc_display_name());

	disc_watch_stop();
	disc_handed_to_core = 1;

	/*
	  The sentinel goes in as the file. It is not a path: menu.cpp keeps it out of the
	  games-folder resolution and the CD daemons' load paths recognise it and read the
	  table of contents off the disc instead of parsing a cue sheet.
	*/
	do_launch(sysidx, PHYSICAL_DISC_SENTINEL, 0, &pl->slot, pl->rbf ? rbf : 0);
}

/*
  Copy the disc in the drive into a system's games folder.

  The drive has one owner, and this is the second thing in this file that takes it: the
  detection helper is stopped exactly as disc_launch() stops it, and disc_poll() is kept
  from forking a new one for as long as rip_busy() - see the drive_is_ours line in
  chome_handle(). Two processes on /dev/sr0 is not a race that shows up as wrong data; it
  shows up as a status ioctl sitting in state D behind a 700 MB read.

  `overwrite` is the player's second press, and it is only about the *offer*: the copy is
  assembled in a hidden staging folder either way and the old one is not touched until the
  new one is finished. So a confirmed overwrite that is then cancelled costs nothing.

  Everything the progress screen will need is captured here rather than read later, because
  a moment after this returns there is no detection helper left to ask. See rip_title.
*/
static void disc_rip_begin(const disc_dlg *d, int sysidx, int overwrite)
{
	const rip_target *rt = rip_target_for(sysidx);
	const chome_sys *s = (sysidx >= 0) ? lib_sys(sysidx) : 0;

	// The whole feature is behind classicui_disc, and the only way here is a screen that
	// only exists when it is on - but this is where a drive and the card both get used in
	// earnest, so it does not rely on that.
	if (!cfg.classicui_disc || !rt || !s || !d) { nudge(); return; }

	char games[1024];
	if (!lib_sys_games_dir(sysidx, games, sizeof(games)))
	{
		printf("ClassicUI: rip: %s has no games folder on this card\n", s->name);
		nudge();
		return;
	}

	/*
	  The folder is the GAME and the sheet inside it is the DISC.

	  Both are built from the name the dialog is showing - the title table's answer where
	  there is one, else the volume label, else the serial, which is disc_display_name()'s
	  order and what makes the finished card read "Metal Gear Solid" instead of
	  "SLES-01506" - and from d->key, which is the serial when the disc carries one.

	  The serial is what makes disc 2 a different thing from disc 1. Both discs of the PAL
	  Metal Gear Solid are titled "Metal Gear Solid" in the shipped table, so the title
	  alone cannot tell them apart and the folder named from it collides. See the naming
	  notes in chome_rip.h for why the name says SLES-11506 and not "Disc 2".
	*/
	const char *disc_name = d->title[0] ? d->title : d->key;

	char name[96], base[160];
	if (!rip_target_folder(games, disc_name, d->key, name, sizeof(name))
		|| !rip_disc_base(disc_name, d->key, base, sizeof(base)))
	{
		printf("ClassicUI: rip: nothing here is a usable folder name\n");
		nudge();
		return;
	}

	/*
	  This exact disc is already there, and this is the first press: the row says what the
	  second one does.

	  Deliberately rip_disc_present() and not rip_folder_exists(). A folder holding disc 1
	  is a game to add disc 2 to, and the replace prompt is reserved for a re-rip of the
	  same serial - offering to replace a game because another of its discs is already
	  copied is what would have destroyed disc 1.
	*/
	if (!overwrite && rip_disc_present(games, name, base))
	{
		rip_over_arm = 1;
		rip_over_until = GetTimer(3000);
		printf("ClassicUI: rip: %s/%s/%s.cue exists, asking before replacing it\n",
			games, name, base);
		mark_dirty();
		return;
	}

	if (rip_folder_exists(games, name))
		printf("ClassicUI: rip: adding %s to the %s already on the card\n", base, name);

	snprintf(rip_title, sizeof(rip_title), "%s", d->title);
	snprintf(rip_key, sizeof(rip_key), "%s", d->key);
	rip_sysidx = sysidx;

	disc_watch_stop();

	if (!rip_start(games, name, base, d->title, rt->mode1_only, overwrite))
	{
		printf("ClassicUI: rip: could not start the helper\n");
		nudge();
		return;
	}

	disc_picking = 0;
	disc_row = 0;
	rip_stop_arm = 0;
	mark_dirty();
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
			gfx_shout(up);
			gfx_text_c(gfx_clip(up, p->ts_title, p->w - p->inset * 2), p->w / 2, p->h / 2 - 14 * s2, p->ts_title, COL_WHITE, COL_SHADOW);
		}
		if (s)
		{
			char up[64];
			snprintf(up, sizeof(up), "%s", s->name);
			gfx_shout(up);
			gfx_text_c(up, p->w / 2, p->h / 2 + 4 * s2, s2, COL_DIM, 0);
		}
		gfx_text_c("LOADING", p->w / 2, p->h / 2 + 20 * s2, s2, COL_PANELLO, 0);
	}
}

/*
  One frame's drawing, back to front. Called bare for a full repaint and under
  gfx_clip_set() for a partial one: the partial case replays exactly this stack, so
  whatever sits underneath the repainted region - the shelf background, a scrim, the
  edge of a panel - is reconstructed by the same code in the same order as a full
  frame, rather than cached per element or read back from a previous frame. That is
  what makes a region repaint smear-proof: nothing here ever depends on what the
  region used to contain.
*/
static void compose()
{
	const chome_profile *p = theme_get();

	// Re-recorded by whichever discs draw this frame; see disc_note_rect().
	disc_rc[0].on = disc_rc[1].on = 0;

	// And by the card row, the pips and the position line; see slide_note_rows(). Cleared
	// here rather than accumulated, so that a screen with no shelf on it leaves no band
	// behind for a slide to clip to.
	slide_rc.on = 0;

	// And by whatever scrolling text this frame draws; see marq_note(). Cleared here for the
	// same reason - a screen with no marquee on it must leave no band behind, or a repaint
	// would be asked for on behalf of text that is no longer drawn.
	marq_rc.on = 0;

	if (screen == SCR_BROWSE)
	{
		draw_browse(p);
		draw_legend(p);
		return;
	}

	draw_background(p);
	draw_title_block(p);
	draw_shelf(p);
	draw_pips(p);
	draw_position(p);

	int overlay = overlay_up();
	// Black over a still of the game and COL_BGDARK over the front-end's own background, for
	// the reason spelled out at ig_build_background(): over a photograph this colour is a
	// floor and not a dim, and it was flattening every dark scene to grey.
	if (overlay) gfx_scrim(0, 0, p->w, p->h, ig_still_shown(p) ? COL_BLACK : COL_BGDARK, 2);

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
	case SCR_CLOSE:   draw_close(p); break;
	case SCR_DISC:    draw_disc(p); break;
	case SCR_INI:     draw_ini(p); break;
	case SCR_SET:     draw_settings(p); break;
	case SCR_COVERS:  draw_covers(p); break;
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
}

/*
  Turn the band marq_note() just recorded into a deadline.

  Called after compose() rather than inside it, and that is not tidiness: compose() has to be
  a pure function of the clock and the state - everything in this file that compares a partial
  repaint against a full repaint of the same instant rests on composing one moment twice
  drawing the same pixels - and a deadline is not a pixel. Setting it here keeps compose()
  answering only the question "what does this instant look like".

  Two composes of one instant set the same deadline, so this is idempotent as well as
  invisible.
*/
static void marq_arm()
{
	if (marq_rc.on) marq_next = GetTimer(marq_rc.next_in);
}

static void render()
{
	gfx_stat_compose_begin();
	compose();
	marq_arm();
	gfx_end();
}

/*
  Repaint only what intersects one rectangle - the spinning disc's, in practice.

  The frame is composed as usual but under a clip, so only the region's pixels are
  drawn or damaged, and gfx_end() copies only that region into the framebuffer. The
  copy is row-based and the disc spans ~36 rows of 240, so this is the difference
  between ~5ms a frame and well under 1ms.

  The alternating framebuffers need no special handling here, and that is worth
  spelling out because it is the likeliest place for a stale-disc bug: gfx_end()
  already unions this frame's damage with the previous frame's before copying, because
  the buffer it fills is two frames stale. The compose buffer always holds a complete
  current frame (a clip only limits what changes, never invalidates the rest), so that
  union is exactly the set of rows in which the stale buffer differs - a partial
  repaint following a full one carries the full frame's damage across to the second
  buffer, and a chain of partials carries the disc's rectangle. Neither buffer can be
  left holding an old rotation.
*/
static void render_region(int x, int y, int w, int h)
{
	gfx_stat_compose_begin();
	gfx_clip_set(x, y, w, h);
	compose();
	gfx_clip_clear();
	marq_arm();
	gfx_end();
}

/* ---------------------------------------------------------------- input --- */

/*
  Hands a finished entry back to whoever opened the keyboard. The Wi-Fi screen is the
  reason it exists; Online Covers is the second and third caller, and the shape is the
  same - the destination was recorded before the keyboard was opened, and a cancelled
  entry is dropped here rather than being seen twice by whoever asked for it.
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

	/*
	  The account name, and the password, staged rather than written - the Save row is
	  what touches the file.

	  An accepted empty entry means "there is none", not "leave it alone": DONE on an
	  empty field is how a player takes an account back off the machine, and the row
	  then reads "Not Set" so that it is visible rather than silent. Cancelling is what
	  leaves the value alone, which is the case above.
	*/
	if (dest == OSKD_SS_USER && cov_accept_text(osk_text(), "account name"))
	{
		snprintf(cov_user, sizeof(cov_user), "%s", osk_text());
		cov_edited();
	}
	if (dest == OSKD_SS_PASS && cov_accept_text(osk_text(), "password"))
	{
		snprintf(cov_pass, sizeof(cov_pass), "%s", osk_text());
		cov_edited();
	}
}

static void go_screen(int s)
{
	/*
	  Leaving More Settings puts an unsaved font back, and it is done here rather than in
	  the B handler because B is not the only way off that screen: the menu button jumps
	  straight to the menu bar from anywhere (see the KEY_MENU case), which walks past every
	  confirmation the screen has. Every other edit on that screen is only staged, so
	  abandoning it costs nothing; the font is the one that was applied as it was chosen, and
	  left behind it would stay on screen until the next reboot with nothing saying why.
	*/
	if (screen == SCR_SET && s != SCR_SET && font_dirty())
	{
		font_sel = font_was;
		font_apply(font_sel);
		font_note[0] = 0;
	}

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
	/*
	  The disc dialog's buttons sit side by side, so this is the axis that walks them -
	  and the core chooser, which is a column, has nothing on it.

	  Clamped rather than wrapping, like every other short list here: two entries that
	  wrap make left and right the same key.
	*/
	case SCR_DISC:
	{
		if (disc_picking) { nudge(); return; }

		disc_dlg d;
		disc_dlg_get(&d);
		disc_build_btns(&d);

		int n = disc_btn + dir;
		if (n < 0 || n >= disc_nbtn) { nudge(); return; }
		disc_btn = n;
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
		// Menu Layout, which is row 4 now that Online Covers sits under Cover Art. The
		// row indices in this file are the panel's, so inserting a row moves them.
		else if (opt_row == 4)
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
	  Online Covers: the switch is the only row with anything on this axis, and it refuses
	  on a build with no application credential.

	  That refusal is the point rather than tidiness. ss_enabled() would hold the line
	  anyway - it asks ss_available() first, so no request can be made whatever this is set
	  to - but a screen that let a player switch on something that provably cannot work,
	  and then said "On", would be the front-end lying about its own state. The row says
	  "Not Available", the footer says why, and the key does nothing.
	*/
	case SCR_COVERS:
		if (cov_row != COV_ON) { nudge(); return; }
		if (!ss_available()) { nudge(); return; }

		cov_on = cov_on ? 0 : 1;
		cov_edited();
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

		co_news_clear();

		// The core recomputes which options apply, so re-read rather than assume.
		core_opts_scan();
		if (co_row >= co_rows()) co_row = co_rows() - 1;
		break;
	}

	case SCR_SET:
		// The font wraps where a number stops, because a ring of file names has no ends
		// worth defending - the same rule opt_step_by() applies to a list.
		if (set_row == SET_ROW_FONT)
		{
			if (font_n < 2) { nudge(); return; }
			set_font_step(dir);
			return;
		}
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

		/*
		  The slide and not the world, which is what makes a held scroll cheap: the cards
		  move, the pips and the position line move with them, and all three are inside the
		  band. The title block and the prompts do not move at all until the selection
		  commits - and when it does, sel_commit() marks the full frame itself, so a tap
		  still gets one. slot_idx is the suspend strip's cursor, and the strip is a
		  different screen.
		*/
		mark_slide();
		return;
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
		if (dir < 0 && disc_or_rip_present()) { go_screen(SCR_DISCBAR); }
		else if (dir < 0) { mb_idx = 0; go_screen(SCR_MENUBAR); }
		else
		{
			// Down here is about the card under the cursor, whatever else is running.
			susp_is_disc = 0;

			chome_item *it = susp_target();
			if (!it) { nudge(); return; }     // folders have no suspend points
			lib_refresh_slots(it);
			slot_idx = 0;
			go_screen(SCR_SUSPEND);
		}
		break;

	case SCR_MENUBAR:
		if (dir > 0) go_screen(disc_or_rip_present() ? SCR_DISCBAR : SCR_HOME);
		else nudge();
		break;

	case SCR_DISCBAR:
		// Between the two: up carries on to the menu bar, down returns to the shelf.
		if (dir < 0) { mb_idx = 0; go_screen(SCR_MENUBAR); }
		else go_screen(SCR_HOME);
		break;

	case SCR_SUSPEND:
		// Back where the strip was opened from, which for a disc is its dialog and not
		// the shelf - the shelf is not what the player was looking at.
		if (dir < 0) go_screen(susp_is_disc ? SCR_DISC : SCR_HOME);
		else
		{
			// Down on a slot locks or unlocks it. Locking is ours to track:
			// there is no lock concept in MiSTer's savestate files.
			chome_item *it = susp_target();
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

	/*
	  Moving off disarms, which on this screen is not housekeeping but the point of it. The
	  timer used to go on running while the cursor was somewhere else, so a press, a look
	  away and a press back inside three seconds closed the game on what the player had
	  counted as the first of two presses. Cleared on any movement rather than only on
	  leaving row 0, because arriving back on the armed row is exactly the case that went
	  wrong and the screen must be found disarmed.
	*/
	case SCR_CLOSE:
		cls_row = (cls_row + dir + CLS_ROWS) % CLS_ROWS;
		ig_close_until = 0;
		mark_dirty();
		break;

	case SCR_SORT:
		sort_idx = (sort_idx + dir + SORT_COUNT) % SORT_COUNT;
		mark_dirty();
		break;

	case SCR_DISC:
	{
		if (disc_picking)
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

		/*
		  The buttons are a row, so up and down leave the dialog rather than walking it.

		  Down goes to the disc's save states, which is what Down does on a shelf card and
		  the reason this dialog exists at all in a running game: a disc has no card to
		  press Down on. Offered over a disc that is merely sitting in the drive as well,
		  but only where the name those states are filed under can be worked out before the
		  mount publishes one - which is a PlayStation disc and its serial, and nothing else.
		  See disc_susp_bind(); a disc whose key cannot be derived has no Down at all rather
		  than a Down onto slots belonging to something else.

		  Here and nowhere else, which is the point of it being here: this dialog is the
		  disc's one entry point, so the item that Down is about is reachable only from this
		  press and is in no view the shelf can scroll.

		  Up goes back to the badge it was opened from, when there is a badge; in a game
		  there is not, because the drive is the core's.
		*/
		if (dir > 0)
		{
			disc_dlg d;
			disc_dlg_get(&d);
			if (!d.susp) { nudge(); break; }

			susp_is_disc = 1;
			lib_refresh_slots(d.susp);
			slot_idx = 0;
			go_screen(SCR_SUSPEND);
			break;
		}

		if (disc_or_rip_present()) go_screen(SCR_DISCBAR);
		else nudge();
		break;
	}

	case SCR_DISPLAY:
		nudge();               // one row of tiles: nothing above or below
		break;

	case SCR_OPTIONS:
		{
			int n = ig_active ? OPT_ROWS_GAME : OPT_ROWS_MENU;
			opt_row = (opt_row + dir + n) % n;

			/*
			  Moving off disarms, as it does on More Settings and Online Covers:
			  reaching for another row means the player has stopped meaning to close
			  the game. The timer used to go on running while the cursor was elsewhere,
			  so a press, a look down the list, and a press back on the row inside three
			  seconds closed the game on what the player had counted as the first of two
			  presses. The row does say "Again To Confirm" when it is returned to, but a
			  player who has been somewhere else in between is not reading it - and now
			  that the list scrolls, going somewhere else and coming back is what walking
			  to the bottom of it feels like.
			*/
			ig_close_until = 0;
		}
		mark_dirty();
		break;

	case SCR_SET:
	{
		int n = set_nrows();
		set_row = (set_row + dir + n) % n;

		// Moving off disarms, as everywhere else here: reaching for another row means
		// the player has stopped meaning to do the thing this one offered.
		set_arm = 0;
		set_quit_arm = 0;
		mark_dirty();
		break;
	}

	case SCR_COVERS:
		cov_row = (cov_row + dir + COV_ROWS) % COV_ROWS;
		// Disarmed on the way past, for the reason above. The refusal note goes too: it
		// was about the row the player has just left.
		cov_arm = 0;
		cov_quit_arm = 0;
		cov_note[0] = 0;
		mark_dirty();
		break;

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
		case MB_OPTIONS:  opt_row = 0; opt_top = 0; go_screen(SCR_OPTIONS); break;
		case MB_POWER:    pwr_row = 0; pwr_arm = -1; go_screen(SCR_POWER); break;

		/*
		  Opened disarmed and on the first row, the way Power is. A screen that arrived
		  already armed - because the player had been here two seconds ago and backed out -
		  would close the game on the first press they made on it, which is the single-press
		  close this screen exists to prevent.
		*/
		case MB_CLOSE:    cls_row = 0; ig_close_until = 0; go_screen(SCR_CLOSE); break;
		case MB_CORE:
			core_opts_scan();
			co_news_clear();
			co_tier = CO_TIER_PICTURE;
			// Land on a page that has something, so an empty Picture list is not the
			// first thing a player meets on a core whose options are all elsewhere.
			if (!core_opts_tier_count(co_tier)) co_tier = CO_TIER_SYSTEM;
			if (!core_opts_tier_count(co_tier)) co_tier = CO_TIER_RISKY;
			co_row = 0;
			co_top = 0;
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
		co_top = 0;
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

		/*
		  Online Covers, directly under Cover Art because it is where the row above gets
		  somewhere to fetch from. Opened even on a build with no application credential:
		  the screen is what explains that state, and a row that refused to open would
		  leave the player nothing to read.
		*/
		case 1:
			cov_refresh();
			go_screen(SCR_COVERS);
			break;

		// gl_forget() as well: a rescan is also how a player says "I have re-scraped",
		// and the parsed gamelists would otherwise still be the ones from before.
		case 2: lib_rescan(); gl_forget(); art_shutdown(); art_init(theme_get()->sel_w, theme_get()->sel_h); view_rebuild(0); break;
		case 3: vp_install(); mark_dirty(); break;
		case 4: nudge(); break;                       // Layout changes with left/right
		case 5:
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

		case 6:
			wifi_row = 0;
			wifi_top = 0;
			go_screen(SCR_WIFI);
			if (net_present() && !net_count()) net_scan_start();
			break;

		case 7:
			ini_refresh();
			go_screen(SCR_INI);
			break;

		case 8:
			set_refresh();
			go_screen(SCR_SET);
			break;

		case 9:
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
			  closed menu - which is what Dinofly saw as "Core Settings just goes back to
			  the game". The comment above eat_menu_release has said the menu opens on the
			  release since the day it was written; I simply had not applied it here.
			*/
			printf("ClassicUI: handing the screen to the core's own options\n");
			osd_handoff = OSDH_WAITING;
			osd_handoff_until = GetTimer(4000);
			ig_close(1);
			menu_key_set(KEY_F12 | UPSTROKE);
			break;

		/*
		  About, which is the same row on both lists - the one place in this panel where
		  the shelf and a running game agree, because what it has to say does not depend on
		  whether anything is loaded.

		  This row used to be Close Game, in a game only, and the shelf's list stopped at
		  nine. Both are eleven now and this is the eleventh of each.
		*/
		case 10:
			go_screen(SCR_ABOUT);
			break;
		}
		break;

	case SCR_DISCBAR:
		disc_open_screen();
		break;

	case SCR_DISC:
	{
		disc_dlg d;
		disc_dlg_get(&d);

		/*
		  A rip owns this screen while it runs, and A is the only press on it.

		  Stopping takes two, like every other press in this front-end that throws work
		  away: the drive has been turning for minutes and the alternative to asking twice
		  is a thumb on the pad undoing all of it. The arming expires, so a stray press does
		  not leave the console one press from cancelling for ever.
		*/
		if (rip_showing())
		{
			if (!rip_busy())
			{
				/*
				  Acknowledging a finished rip, which is also where the shelf finds out.
				  lib_rescan() rather than nothing: the folder appeared under a games
				  directory the scanner has already walked, so without this the card the rip
				  just created is not on the shelf until something else rescans.
				*/
				int done = (rip_state()->state == RIP_DONE);
				rip_ack();
				rip_stop_arm = 0;
				if (done) lib_rescan();
				go_screen(disc_state() != DISC_ABSENT ? SCR_DISC : SCR_HOME);
				mark_dirty();
				break;
			}

			if (rip_stop_arm && !CheckTimer(rip_stop_until))
			{
				rip_stop_arm = 0;
				rip_cancel();
				mark_dirty();
				break;
			}

			rip_stop_arm = 1;
			rip_stop_until = GetTimer(3000);
			mark_dirty();
			break;
		}

		if (disc_picking)
		{
			disc_build_rows();
			if (disc_row < 0 || disc_row >= disc_nrows) { nudge(); break; }

			/*
			  Copy it to the card.

			  Two presses when there is already a folder of that name, and the row says so
			  in between - the shape the suspend strip's delete established. Nothing is
			  destroyed by the confirmation itself either: rip_perform() builds the new copy
			  in a hidden staging folder and only replaces the old one once every byte is
			  written, so a rip that is cancelled or that fails halfway leaves the folder
			  that was already there exactly as it was.
			*/
			if (disc_rowact[disc_row] == DACT_RIP)
			{
				int armed = (rip_over_arm && !CheckTimer(rip_over_until));
				rip_over_arm = 0;
				disc_rip_begin(&d, disc_rowsys[disc_row], armed);
				break;
			}

			/*
			  A "(not yet)" row refuses, and records nothing: remembering a choice that
			  cannot launch would re-offer the refusal every time the dialog opens. The
			  row already says why; the log says it in full.
			*/
			if (disc_rowact[disc_row] == DACT_NONE)
			{
				const chome_sys *sc = (disc_rowsys[disc_row] >= 0) ? lib_sys(disc_rowsys[disc_row]) : 0;
				printf("ClassicUI: disc -> %s (%s), not launched: that core's daemon does not read from the drive yet\n",
					sc ? sc->name : "?", disc_display_name());
				nudge();
				break;
			}

			/*
			  A core was chosen. Remembered so re-opening the dialog shows the decision
			  rather than starting from the guess. Rows only offer what disc_playables
			  can launch, so this launches; disc_launch() keeps its own guard for the
			  day the two disagree.
			*/
			disc_chosen_sys = disc_rowsys[disc_row];
			disc_picking = 0;
			disc_row = 0;
			disc_launch(disc_chosen_sys);
			break;
		}

		disc_build_btns(&d);
		if (disc_btn < 0 || disc_btn >= disc_nbtn) { nudge(); break; }

		if (disc_btnact[disc_btn] == DBTN_OPTS)
		{
			disc_picking = 1;
			disc_row = 0;
			disc_build_rows();
			mark_dirty();
			break;
		}

		// A over the running disc goes back to it, exactly as A on the running game's
		// own card does - see the SCR_HOME case above.
		if (d.running) { ig_close(1); break; }

		/*
		  And a dim Play refuses in the same words its label and the line under the title
		  already say. Nothing is remembered: a choice that cannot launch would re-offer
		  the refusal every time the dialog opens.
		*/
		if (!disc_can_play(&d))
		{
			const chome_sys *sc = (d.sysidx >= 0) ? lib_sys(d.sysidx) : 0;
			printf("ClassicUI: disc -> %s (%s), not launched: %s\n",
				sc ? sc->name : "no core for this disc", disc_display_name(),
				sc ? "that core's daemon does not read from the drive yet"
				: "nothing here claims it - use Options to pick a core");
			nudge();
			break;
		}

		disc_chosen_sys = d.sysidx;
		disc_launch(disc_chosen_sys);
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

	case SCR_CLOSE:
		// The second row is the way back into the game, and costs nothing, so it acts at once.
		if (cls_row != 0) { ig_close(1); break; }

		/*
		  And the first loses whatever has not been saved, so it takes two presses - the
		  same three-second arm the Options row carried before this screen existed, moved
		  rather than reinvented. quit_to_home(1) takes a suspend point on the way out where
		  the core can, which is what the 1 is.
		*/
		if (!CheckTimer(ig_close_until))
		{
			quit_to_home(1);
			break;
		}

		ig_close_until = GetTimer(3000);
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

		// On the font, A steps it forward too, for the same reason: a player who only
		// presses A can still get all the way round the list.
		if (set_row == SET_ROW_FONT)
		{
			if (font_n < 2) { nudge(); break; }
			set_font_step(1);
			break;
		}

		if (!set_pending()) { nudge(); break; }

		// Rewriting the player's own ini takes two presses, as everything here that
		// touches a real file does.
		if (set_arm && !CheckTimer(set_arm_until))
		{
			set_arm = 0;

			/*
			  The font goes in the same write as the options. It is the one key on this
			  screen the option table cannot hold - it is a path, not a number in a range -
			  so it is handed to opt_apply() as an extra rather than written afterwards,
			  which would take a second backup and lose the pre-edit file. Built-in writes
			  the key empty, which is exactly what user_io.cpp tests for.
			*/
			ini_set fset[1];
			int nf = 0;
			if (font_dirty())
			{
				fset[0].key = "font";
				fset[0].value = font_rel[font_sel];
				nf = 1;
			}

			int w = opt_apply(ini_path(), fset, nf);

			if (w < 0) { set_failed = 1; set_wrote = -1; }
			else
			{
				set_wrote = w;
				set_failed = 0;

				// cfg is what the running firmware reads, and set_summary() below reads
				// cfg.font back to work out which row the font list is on. Writing the
				// file without this would put the cursor back on the previous font.
				if (nf) snprintf(cfg.font, sizeof(cfg.font), "%s", font_rel[font_sel]);

				/*
				  classicui_overscan is in the set and every layout metric is derived
				  from it, so the theme has to be recomputed even though the canvas is
				  exactly the size it was - which is the one case theme_update() skips.
				  classicui_tracking and classicui_caps need the same repaint for a
				  simpler reason: they change the size and shape of every string drawn.
				*/
				theme_invalidate();
				theme_update(gfx_w(), gfx_h(), cfg.classicui_profile);
				art_init(theme_get()->sel_w, theme_get()->sel_h);
				gfx_damage_all();
			}

			set_summary();
			if (set_row > SET_ROW_SAVE) set_row = SET_ROW_SAVE;
			mark_dirty();
			break;
		}

		set_arm = 1;
		set_arm_until = GetTimer(3000);
		mark_dirty();
		break;
	}

	/*
	  Online Covers. Every row is refused on a build with no application credential, and
	  the account rows are refused first: the point of the screen in that state is to say
	  so, and letting somebody spell a password out on a pad for a request that provably
	  cannot be made is the specific unkindness this avoids. The footer says why.
	*/
	case SCR_COVERS:
		if (!ss_available()) { nudge(); break; }

		if (cov_row == COV_ON) { cov_on = cov_on ? 0 : 1; cov_edited(); break; }
		if (cov_row == COV_USER) { cov_open_user(); mark_dirty(); break; }
		if (cov_row == COV_PASS) { cov_open_pass(); mark_dirty(); break; }

		if (!cov_dirty()) { nudge(); break; }

		// Two presses, because this rewrites the player's own MiSTer.ini - the same rule
		// Best Settings and More Settings follow.
		if (cov_arm && !CheckTimer(cov_arm_until))
		{
			cov_arm = 0;
			int w = cov_apply();

			if (w < 0) { cov_failed = 1; cov_wrote = -1; }
			else { cov_wrote = w; cov_failed = 0; }

			mark_dirty();
			break;
		}

		cov_arm = 1;
		cov_arm_until = GetTimer(3000);
		mark_dirty();
		break;

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

	// A dismisses it, the same as B - and back to Options, which is where it is chosen from now.
	case SCR_ABOUT:
		go_screen(SCR_OPTIONS);
		break;

	case SCR_SUSPEND:
	{
		chome_item *it = susp_target();
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

		/*
		  The disc in the drive takes the same arming and a different hand-over: Play plus a
		  starting state, not a second launch path. Not through SCR_LAUNCH like a card, and
		  the reason is what that curtain ends in - launch_selected(), which launches the
		  entry under the shelf cursor. A disc has no entry there, which is the whole reason
		  this dialog exists, so the curtain would arm the resume and then start whatever the
		  cursor happened to be parked on. The disc's own Play button hands over immediately
		  for the same reason; this is that button with the record already written.
		*/
		if (it == disc_susp_item_get())
		{
			disc_launch(it->sysidx);
			break;
		}

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
			disc_dlg_enter();
			mark_dirty();
			return;
		}

		/*
		  Back to the disc it belongs to, which is where the dialog was opened from -
		  falling through to the generic back dropped the player onto the shelf.

		  There is no badge to go back to in a game, though: the drive belongs to the core,
		  so nothing is drawn in the corner and SCR_DISCBAR would be a state with nothing
		  on screen and no way to tell it from this one.
		*/
		go_screen(disc_or_rip_present() ? SCR_DISCBAR : SCR_HOME);
		return;

	case SCR_SUSPEND:
		// As Up does: back where the strip was opened from.
		go_screen(susp_is_disc ? SCR_DISC : SCR_HOME);
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
		/*
		  sel_commit() because this is a jump and not a scroll. B is one press with no
		  repeat behind it, and the shelf can be hundreds of entries from home - the ease is
		  exponential in the distance, so waiting for it to settle would leave the title
		  naming the card the player just left for the better part of half a second. Nothing
		  is being held, so there is nothing to defer.
		*/
		if (sel > 0) { sel = 0; slot_idx = 0; sel_commit(); mark_dirty(); }
		else nudge();
		break;

	case SCR_SORT:
	case SCR_DISPLAY:
	case SCR_OPTIONS:
		go_screen(SCR_MENUBAR);
		break;

	// About is a row of Options now rather than an entry on the bar, so back from it is
	// back to the list it was chosen from - the same way Best Settings and More Settings go.
	case SCR_ABOUT:
		go_screen(SCR_OPTIONS);
		break;

	case SCR_POWER:
		if (pwr_arm >= 0) { pwr_arm = -1; mark_dirty(); break; }   // first B cancels
		go_screen(SCR_MENUBAR);
		break;

	case SCR_CLOSE:
		// First B cancels, as on Power: backing out of an armed close must not also be the
		// press that leaves the screen, or a player stopping themselves overshoots.
		if (!CheckTimer(ig_close_until)) { ig_close_until = 0; mark_dirty(); break; }
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
		if (set_pending() && !(set_quit_arm && !CheckTimer(set_quit_until)))
		{
			set_quit_arm = 1;
			set_quit_until = GetTimer(3000);
			mark_dirty();
			break;
		}

		// The font, which is the one edit here that was applied as it was made, is put back
		// by go_screen() - see there for why it is not done on this line.
		set_quit_arm = 0;
		go_screen(SCR_OPTIONS);
		break;

	case SCR_COVERS:
		if (cov_arm) { cov_arm = 0; mark_dirty(); break; }          // first B cancels

		// And the same question More Settings asks, for the same reason: a login that was
		// typed and never saved is worth one press to confirm losing.
		if (cov_dirty() && !(cov_quit_arm && !CheckTimer(cov_quit_until)))
		{
			cov_quit_arm = 1;
			cov_quit_until = GetTimer(3000);
			mark_dirty();
			break;
		}

		/*
		  And the staging buffers go, password included. Not tidiness: cov_pass is a copy
		  of somebody's password in a static, and the screen is the only thing that has any
		  use for it. cfg keeps the one the firmware needs.
		*/
		cov_quit_arm = 0;
		memset(cov_pass, 0, sizeof(cov_pass));
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
  what Dinofly hit on PSX with Destruction Derby.

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
  front-end throwing you out for pressing Save. SMS is the first core Dinofly has that pauses,
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
	  is how Dinofly found it. Every panel already respects safe_y; this did not.
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
	int room = gfx_text_cols(p->w - 8 * s, s);
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
	unlink(PHYSICAL_DISC_IDENT_FILE);   // nothing is mounted after this, so nobody is playing
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

/*
  Who the disc in the drive is, as the mount named it - see PHYSICAL_DISC_IDENT_FILE.

  Read rather than derived. The front-end has no drive of its own here: the detection
  helper has been stopped and the core owns the hardware, so this is the only way to
  learn the name, and it is deliberately the same name the core's save files use.
*/
static int disc_ident_read(char *key, int klen, char *title, int tlen)
{
	FILE *f = fopen(PHYSICAL_DISC_IDENT_FILE, "rt");
	if (!f) return 0;

	char k[128] = {}, t[128] = {};
	int ok = (fgets(k, sizeof(k), f) != 0);
	int have_t = (fgets(t, sizeof(t), f) != 0);
	fclose(f);
	if (!ok) return 0;

	for (char *q = k; *q; q++) if (*q == '\n') { *q = 0; break; }
	for (char *q = t; *q; q++) if (*q == '\n') { *q = 0; break; }
	if (!k[0]) return 0;

	snprintf(key, klen, "%s", k);
	if (title && tlen) snprintf(title, tlen, "%s", have_t ? t : "");
	return 1;
}

/*
  What is running, by the name the rest of the firmware knows it by.

  The sentinel is what got *mounted*, not who is playing, and everything keyed on a
  game's path needs the latter: savestate slots, per-game core options, the suspend
  record. Substituting it in one place is the point - susp_write() stores this name and
  resume_poll() compares against it, so a disc whose two sides disagreed by a single
  byte would suspend and then silently never resume.

  When the name is missing the sentinel is kept rather than the record dropped: a core
  that predates the ident file still deserves a captioned menu, and it degrades to
  exactly the behaviour that shipped.
*/
static int cur_read_ident(char *sysid, int syslen, char *path, int pathlen,
	char *title, int tlen, int *is_disc)
{
	if (is_disc) *is_disc = 0;
	if (title && tlen) title[0] = 0;

	if (!cur_read(sysid, syslen, path, pathlen)) return 0;
	if (strcmp(path, PHYSICAL_DISC_SENTINEL)) return 1;

	if (is_disc) *is_disc = 1;
	disc_ident_read(path, pathlen, title, tlen);
	return 1;
}

static int ig_load_item()
{
	ig_have_item = 0;
	ig_is_disc = 0;

	char sysid[64] = {}, rompath[CH_PATH_LEN] = {}, disc_title[CH_PATH_LEN] = {};
	if (!cur_read_ident(sysid, sizeof(sysid), rompath, sizeof(rompath),
		disc_title, sizeof(disc_title), &ig_is_disc)) return 0;

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

	// A disc's own label reads better than its serial, when it has one.
	if (disc_title[0]) snprintf(ig_item.title, sizeof(ig_item.title), "%s", disc_title);

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

  And the *canvas* pixels are not square either, which is the half of this that was missed.
  A 15 kHz TV canvas arrives as 640x240 (640x288 with menu_pal=1) whenever the framebuffer
  takeover is not held - vga_scaler=1 or direct_video - and the scaler stretches it over the
  same screen a 320x240 canvas fills, so those pixels are twice as tall as they are wide.
  The sweep that gave chome_profile its px did the profile, the text scales, the card, the
  slot tiles and the bottom margin, and did not do this: fitting 4:3 into 640x240 by raw
  pixel count gave a 320-pixel-wide picture in a 640-pixel-wide canvas, so the still of the
  game occupied the middle half of the screen with 160 black columns down each side.

  Which is exactly where a full-width panel leaves the background showing. Over a physical
  disc the in-game menu opens on the disc's own dialog, and that dialog is the width of the
  canvas less the inset - so the strips it leaves were both inside those black bars, and the
  game the player was left looking at came to 640 pixels of 153600, all of them in the sliver
  of rows below the panel. The menu was reported as having a black background instead of a
  still of the game, and the capture had worked perfectly every time: the still was there the
  whole while, in the middle, under the dialog. Measured in assert_ingame_still().
*/
/*
  Fits SHOT_AR_W:SHOT_AR_H inside w x h, centred, and reports where it landed. px is the
  canvas pixels in one square unit - chome_profile::px - because the ratio is a ratio of
  what the screen shows, not of what the framebuffer counts.
*/
static void shot_fit(int w, int h, int px, int *fw, int *fh, int *ox, int *oy)
{
	if (px < 1) px = 1;
	int ew = w / px;                  // the width in square units

	int aw, ah;
	if ((long long)SHOT_AR_W * h > (long long)SHOT_AR_H * ew)
	{
		aw = ew;
		ah = (int)((long long)ew * SHOT_AR_H / SHOT_AR_W);
	}
	else
	{
		ah = h;
		aw = (int)((long long)h * SHOT_AR_W / SHOT_AR_H);
	}
	if (aw < 1) aw = 1;
	if (ah < 1) ah = 1;

	// Back into canvas pixels, and clamped: a rounding down in ew above must not come
	// back as a rectangle one pixel wider than the canvas it is drawn into.
	aw *= px;
	if (aw > w) aw = w;

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

/*
  Capture scaled to the canvas and dimmed, built once when the menu opens.

  How much of the paused game shows behind a menu, settled here because this is where the
  arithmetic that decides it lives.

  There are two treatments and there always were: this dim, and the checkerboard scrim
  compose() lays over the whole canvas on every panel screen. They do not compose the way
  two dims should. A checkerboard replaces half the pixels with its colour outright, so with
  COL_BGDARK it is a *floor* rather than a dim - luma 31 where the game was darker than
  that, luma 31 averaged in where it was brighter. Measured over a paused PlayStation core on
  the device: outside the dialog the framebuffer held exactly two values, 0 and 31, because
  the game was mid-fade and every pixel of it was below the floor. A dark scene came out flat
  grey-black and a bright one - Destruction Derby's title screen, which did show - merely
  dimmed. That is not the capture failing; it is the two treatments fighting.

  So the scrim over a still is black, which multiplies instead of flooring: half the pixels
  to zero keeps every ratio in the picture and leaves black black. It stays COL_BGDARK over
  the front-end's own grid background, where nothing is darker than COL_BGDARK anyway and the
  colour is the point - the shelf's bottom band is the same one.

  And the dim moves 5/16 -> 6/16 to pay for the difference, measured on white:

      treatment                     panel screen (scrim)     shelf (no scrim)
      5/16 then COL_BGDARK          79 and 29, mean 54       79
      6/16 then black               95 and 0,  mean 48       95

  Nothing behind a panel is therefore brighter than it was - 48 against 54, so no text over it
  reads worse than it does today - while the range the picture has to work in goes from 15..54
  to 0..48 and a dark scene is dark rather than lifted. The shelf-over-a-game screen, which
  has no scrim, is the one thing that gets brighter: 95 against 79 on white, and the text over
  it there is drawn with COL_SHADOW behind it.

  A pixel value is not a brightness the eye judges, so both halves of that table are asserted
  in assert_ingame_dim() rather than looked at.
*/
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
	shot_fit(p->w, p->h, p->px, &fw, &fh, 0, 0);

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
			// Dim to three eighths: the game stays recognisable, the menu readable. Kept in
			// sixteenths because the scrim halves this again on a panel screen, and the pair
			// of numbers in the table above is what was actually chosen.
			uint32_t r = ((c >> 16) & 0xff) * 6 / 16;
			uint32_t g = ((c >> 8) & 0xff) * 6 / 16;
			uint32_t b = (c & 0xff) * 6 / 16;
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
	if (restore_video)
	{
		video_fb_enable(0);

		/*
		  After the disable, so the resize reprograms nothing that is showing. Only on
		  this branch: a core switch re-execs the whole process, and resizing a
		  framebuffer that is still on screen with our last frame in it would reinterpret
		  those pixels at the new stride - a torn screen for exactly the moment the load
		  takes.
		*/
		video_fb_size_request(0);
	}

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
	sel_shown = at;                   // placed, not moved - see view_rebuild()
	ig_selected_running = 1;
	mark_dirty();
}

// Returns 1 when the menu took over, 0 when the core cannot host it.
static int ig_open()
{
	const chome_profile *p;

	// Before anything measures: the still is scaled to the canvas below, and building
	// it at full size only for the loop to halve the canvas on the next pass would
	// throw the scale away and draw the menu over the grid instead of over the game.
	fb_size_sync();

	theme_update(video_menu_fb_width(), video_menu_fb_height(), cfg.classicui_profile);
	p = theme_get();
	if (p->w < 8 || p->h < 8) { video_fb_size_request(0); return 0; }

	// Grab the running frame before we take the screen away from the core.
	int max_px = 2048 * 1024;
	free(ig_shot);
	ig_shot = (uint32_t*)malloc((size_t)max_px * 4);

	/*
	  The buffer is eight megabytes, so a refusal here is a real answer and not a formality -
	  and it has to be kept separately, because screenshot_grab_why() is not asked when the
	  grab is never reached and would then be answering about a previous open.
	*/
	const char *why = "no room for a frame of that size";
	if (ig_shot)
	{
		if (!screenshot_grab(ig_shot, max_px, &ig_shot_w, &ig_shot_h))
		{
			free(ig_shot);
			ig_shot = 0;
			ig_shot_w = ig_shot_h = 0;
		}
		why = screenshot_grab_why();
	}

	/*
	  Said out loud, once per open, because the two ways this ends up wrong are the same
	  picture on a television: no still because the grab came back with nothing, and no
	  still because the grab worked and the screen the menu opened on has none of it
	  showing. That ambiguity is the whole reason this line exists - a black background over
	  a physical disc was chased as a failed capture for a day, and the capture was fine.
	*/
	printf("ClassicUI: the still of the game: %s, %dx%d\n", why, ig_shot_w, ig_shot_h);

	/*
	  Opaque, once, here. The scaler hands back ARGB with nothing meaningful in the alpha
	  byte, and the drawing paths each worked around that by or-ing 0xff000000 in as they
	  read - so the menu background looked right while ss_write_thumb, which passes the
	  buffer straight to imlib2, wrote a fully transparent image. That is the black tile
	  Dinofly found on a PSX suspend point. Fixing it at the source means every consumer gets
	  a valid frame instead of each having to remember.
	*/
	if (ig_shot)
	{
		size_t n = (size_t)ig_shot_w * ig_shot_h;
		for (size_t i = 0; i < n; i++) ig_shot[i] |= 0xff000000u;
	}

	// The failed opens release the request: past here the core keeps the screen, and
	// the request must only ever stand while this front-end is what the player sees.
	if (!gfx_begin()) { video_fb_size_request(0); free(ig_shot); ig_shot = 0; return 0; }

	// Take the framebuffer. A core without support leaves us nothing to draw on.
	if (!video_menu_fb_present(ig_fb))
	{
		printf("ClassicUI: core has no HPS framebuffer, leaving the OSD to it\n");
		video_fb_size_request(0);
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
		video_fb_size_request(0);
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
	  And what came of it, against the canvas it has to match to be drawn at all -
	  draw_background() blits it only while ig_bg_w/h are the profile's w/h.

	  The rectangle is the second half of the same question the grab line above asks. A
	  still that is there, is canvas-sized, and is still not on screen is being covered by
	  what is drawn on top of it, and that is a layout question rather than a capture one:
	  the disc's dialog is the width of the canvas less the inset, so over a disc there is
	  almost nothing of it left to see whatever the numbers here say.

	  px is here because the picture inside that rectangle is fitted to the display aspect
	  and not to the pixel count - see shot_fit(). On a 640x240 canvas (px=2, which is
	  vga_scaler=1 or direct_video) the fit used to land in the middle half of the width with
	  black columns down each side, and those columns were the only background a full-width
	  dialog left showing.
	*/
	{
		int fw = 0, fh = 0, ox = 0, oy = 0;
		if (ig_bg) shot_fit(ig_bg_w, ig_bg_h, p->px, &fw, &fh, &ox, &oy);
		printf("ClassicUI: the menu background: %s, canvas %dx%d px=%d, picture %dx%d at %d,%d\n",
			ig_bg ? "built" : "none", p->w, p->h, p->px, fw, fh, ox, oy);
	}

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
	susp_is_disc = 0;
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

	/*
	  Over a disc the menu opens on the disc's own dialog, not on the shelf.

	  The shelf is where a file-launched game lives, and opening there lands the player on
	  the card they came from. A disc has no card - nothing scanned it, so ig_select_running()
	  finds nothing to park on and the shelf shows whatever it was last showing, which after
	  a disc launch is a folder. So the menu opened on a screen that had nothing to do with
	  what was playing, and getting back to the disc meant knowing it was up the menu bar.

	  The dialog is set up directly rather than through disc_open_screen(), for the same
	  reason `screen` is assigned above: go_screen() belongs to navigation between screens
	  and this is the first screen of the session.
	*/
	if (ig_running_disc())
	{
		disc_dlg_enter();
		screen = SCR_DISC;
	}

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

	// Only for the game this core actually booted - by the same name susp_write() used.
	char cur_sys[64] = {}, cur_path[CH_PATH_LEN] = {};
	if (!cur_read_ident(cur_sys, sizeof(cur_sys), cur_path, sizeof(cur_path), 0, 0, 0)) return;
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

	/*
	  By the disc's name too. The sentinel contains '*', which exFAT will not hold, so a
	  disc's reference frame could never be written - and this runs once per core session
	  with no retry, so it was one lost capture per disc, not a delayed one.
	*/
	char sysid[64] = {}, rompath[CH_PATH_LEN] = {};
	if (!cur_read_ident(sysid, sizeof(sysid), rompath, sizeof(rompath), 0, 0, 0)) return;

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
		/*
		  Not for a disc. This list is MiSTer's own file browser's, and every entry in it
		  is meant to be openable from there; a disc is not a file, so the best that could
		  be written is a path to something that is not on the card. Picking it would fail.
		*/
		if (cur_read(sysid, sizeof(sysid), rel, sizeof(rel)) && strcmp(rel, PHYSICAL_DISC_SENTINEL))
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
int chome_sel_index() { return sel; }

// See chome.h, and the comment on cov_state_of() for why the availability is an argument.
const char *chome_covers_state(int available, int on, const char *user, int has_pass)
{
	return cov_state_of(available, on, user, has_pass);
}

/*
  1 when the machine is not doing anything the scheduler needs to be prompt about, so it
  may sleep a little between passes instead of spinning.

  True whenever the front-end owns the screen: its shelf in the menu core, or its in-game
  menu over any core at all - paused, frozen, or still running.

  The still-running case is included, and the reason is that the player can neither see
  nor hear that game. ig_mute_engage() is called unconditionally when the in-game menu
  opens, before any pause or freeze decision, so every core is silenced; and the HPS
  framebuffer *replaces* the scaler's input rather than blending over it, so the game is
  not on screen either. An earlier version of this excluded still-running cores on the
  grounds that they were "still making sound", which was simply wrong.

  The residual risk, stated rather than hidden: a core actively streaming sectors - a CD
  or floppy game left running behind the menu - does still want prompt service. One
  millisecond is small next to a 13ms CD sector at 1x, so this should be invisible, but if
  a streaming core ever misbehaves with the menu open then excluding those specific cores
  is the first thing to try.
*/
int chome_core_idle()
{
	if (!cfg.classicui) return 0;

	if (is_menu()) return chome_active();

	return ig_active;
}

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

	// And the half-resolution request with it, before the wallpaper repaint below
	// draws into the framebuffer: the classic menu's screen is not ours to shrink.
	video_fb_size_request(0);

	lib_state_save();
	net_watch(0);

	/*
	  A rip in flight goes with us, and it has to: the classic menu can load a core, which
	  re-execs this process, and a helper holding /dev/sr0 through that would be a rip nobody
	  can see and nothing will ever reap - reading the drive while whatever the player loads
	  next tries to. rip_forget() cancels it and takes the staging folder with it, so the
	  card is left as it was rather than with a hidden half-copy on it.
	*/
	rip_forget();
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
	// The half-resolution request first, for the same reason - both change the canvas.
	fb_size_sync();
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

/*
  How often to advance the disc: 60fps when only the disc will be repainted, the old
  rate when the whole frame will be.

  These are the two cases the dispatch at the bottom of chome_handle() distinguishes, and
  asking the same questions here is what keeps the rate honest - a 16ms tick that then
  took the full-repaint branch would be 31% of the loop spent redrawing a whole screen to
  move a disc by a third of a position.

  Both answers describe the frame that was last drawn rather than the one about to be:
  ui_busy() can turn on between this and the render, and the disc's rectangle is where it
  was. That is the same window the dispatch already lives with - the worst case is one
  tick at the wrong rate, which is invisible.
*/
static unsigned long disc_spin_ms()
{
	int x, y, w, h;
	if (ui_busy()) return GFX_DISC_MS;
	if (!disc_spin_rect(&x, &y, &w, &h)) return GFX_DISC_MS;
	return GFX_DISC_PART_MS;
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

		/*
		  And the snap onto the rest position, in the same pass as the step that earned it
		  rather than on the next one. Both halves of that matter and the bug they replace was
		  a real one, found by the byte-identity check below in the harness.

		  It used to sit in an else branch, so the sequence was: step to within a thousandth
		  of a position and draw *that*, then snap on the following pass with nothing asking
		  for a repaint. Two consequences. The shelf came to rest with the selected card a
		  pixel short of its full height and a pixel off centre, because the frame at the rest
		  position was never drawn - nobody had noticed, since there was nothing to compare it
		  against until something else asked for a frame. And, worse for what this file now
		  relies on, it made animate() depend on how many times it had been called rather than
		  on how much time had passed: composing one instant twice snapped the shelf between
		  the two composes, so a partial frame and a full repaint of the same moment differed
		  by that pixel. The poll loop calls this many times per millisecond on the device, so
		  "how many times" is not a quantity anything should depend on.

		  Done here, selF is only ever at rest or a real distance from it when a pass ends, and
		  a pass with no time in it changes nothing at all: the step above multiplies by zero
		  and this leaves a distance that is still too big to snap.
		*/
		double r = sel - selF;
		if (r > -0.003 && r < 0.003) selF = sel;

		/*
		  The ease is the one animation in this front-end that moves nothing but the cards,
		  and it was asking for the whole screen because asking for the whole screen is all
		  that existed. Twelve frames of a tap's slide, of which the first was the only one
		  that had anything outside the card row to say.
		*/
		mark_slide();
	}

	/*
	  And the shelf coming to rest is one of the two moments the chrome commits (the other is
	  the release, in chome_handle). Placed after the ease so it sees this frame's selF: the
	  title is then never left disagreeing with a stationary shelf, however the shelf came to
	  stop - the end of the list under a held arrow, a repeat rate slower than the ease, or a
	  release that never arrived at all.
	*/
	if (selF == sel) sel_commit();

	double bt = (screen == SCR_MENUBAR || screen == SCR_SORT || screen == SCR_DISPLAY ||
		screen == SCR_OPTIONS || screen == SCR_ABOUT) ? 1 : 0;
	if (bar_y != bt)
	{
		bar_y += (bt - bar_y) * (k * 2.5 > 1 ? 1 : k * 2.5);
		if (bar_y > 0.998) bar_y = 1;
		if (bar_y < 0.002) bar_y = 0;
		mark_anim();
	}

	double st = (screen == SCR_SUSPEND) ? 1 : 0;
	if (strip_y != st)
	{
		strip_y += (st - strip_y) * (k * 2.5 > 1 ? 1 : k * 2.5);
		if (strip_y > 0.998) strip_y = 1;
		if (strip_y < 0.002) strip_y = 0;
		mark_anim();
	}

	if (screen == SCR_LAUNCH)
	{
		curtain += k * 0.6;
		if (curtain > 1) curtain = 1;
		mark_anim();
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

	if (!CheckTimer(nudge_until)) mark_anim();
	if (!CheckTimer(ig_close_until)) mark_anim();

	/*
	  A confirmation with a timer on it needs a repaint while it is up and one *more* when it
	  runs out. The second is the one that is easy to miss: painting only while the timer runs
	  leaves the last painted frame the one that still says it, so the message stays on screen
	  until the player happens to press something and stops reading as news at all. Clearing
	  it here is what makes the next repaint the one without it, and since the clear is the
	  condition it can only happen once.
	*/
	if (co_promoted[0])
	{
		if (!CheckTimer(co_promoted_until)) mark_anim();
		else { co_news_clear(); mark_dirty(); }
	}

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
		if (ph != anim_seen) { anim_seen = ph; mark_anim(); }
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
		// fight over the video mode - and the half-resolution request goes with it,
		// or the console somebody pressed F9 for comes up at 640x360.
		if (video_fb_state())
		{
			active = 0;
			video_menu_fb_analog(0);
			video_fb_size_request(0);
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
			chome_item *it = susp_target();

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

			/*
			  Y gives a core setting to the whole system while a game is loaded.

			  This is the half of per-game core options that did not exist: left and right
			  change a value *for this game*, which is the right default and stays the
			  default, and Y is the deliberate second act that says "for every game on this
			  core". It writes only this option's bits into <CORE>.CFG - see
			  core_opt_promote_to_core(), where the whole difficulty is - so the other
			  settings this game keeps to itself do not go with it.

			  Nothing changes on the core: the value on screen is already the one being
			  shared. What changes is where it is stored, and the row losing its star is
			  ambiguous on its own - X produces the same disappearance - so the footer says
			  which of the two just happened.
			*/
			if (screen == SCR_CORE)
			{
				if (co_row >= core_opts_tier_count(co_tier)) { nudge(); break; }

				const core_opt *o = core_opt_tier_at(co_tier, co_row);
				if (!o) { nudge(); break; }

				// Copied before the call: the option table is rescanned below and o then
				// points at whatever the rescan put in that slot.
				char nm[CO_NAME_LEN];
				snprintf(nm, sizeof(nm), "%s", o->name);

				if (!core_opt_promote_to_core(o)) { nudge(); break; }

				snprintf(co_promoted, sizeof(co_promoted), "%s", nm);
				co_promoted_until = GetTimer(3000);

				// Same reason as changing a value: the core recomputes its own mask.
				core_opts_scan();
				if (co_row >= co_rows()) co_row = co_rows() - 1;
				mark_dirty();
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

				co_news_clear();

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
				// On the font, the value to go back to is the one the file names rather
				// than a recommendation - this menu has no favourite font. Which is also
				// the only undo for a row whose change is already on screen.
				if (set_row == SET_ROW_FONT)
				{
					if (!font_dirty()) { nudge(); break; }
					font_sel = font_was;
					font_apply(font_sel);
					font_note[0] = 0;
					set_edited();
					break;
				}
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

			chome_item *it = susp_target();
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
			// The shoulders jump the shelf, so they belong to the shelf. With a panel up
			// they were still moving it behind the dialog.
			if (screen != SCR_HOME) { nudge(); break; }

			/*
			  By first letter, not by page.

			  A page is three or five cards depending on the profile, so on a shelf of a
			  thousand games paging is barely faster than walking - it took fifty presses
			  to cross the letter M on a real card, which is how this came up. A letter is
			  the unit somebody actually holds in their head when they are looking for
			  Metal Gear Solid.

			  Right goes to the first entry of the next letter. Left goes to the first
			  entry of *this* letter unless it is already there, and only then to the
			  previous letter's first entry - the same behaviour a music player's
			  track-back button has, and it means a mistimed press costs one press rather
			  than a whole letter.
			*/
			int n = lib_view_count();
			if (n < 2) { nudge(); break; }

			int next = sel;
			char cur = jump_initial(sel);

			if (k == KEY_EQUAL)
			{
				int i = sel;
				while (i + 1 < n && jump_initial(i + 1) == cur) i++;

				/*
				  Past the last letter there is no next letter, so the press lands on the
				  last entry instead of doing nothing. Standing in the middle of Z and
				  pressing right used to refuse, which reads as a dead button rather than
				  as the end of the alphabet - and the end of the list is plainly what was
				  being asked for. Only once actually on the final entry does it decline.

				  Left already does the mirror of this without being asked: walking back
				  from the first letter settles on entry 0, because the step-into-the-
				  previous-letter branch is guarded by i > 0.
				*/
				next = (i + 1 < n) ? i + 1 : (n - 1);
			}
			else
			{
				int i = sel;
				while (i > 0 && jump_initial(i - 1) == cur) i--;
				if (i == sel && i > 0)
				{
					// Already at the head of this letter, so step into the one before it.
					i--;
					char prev = jump_initial(i);
					while (i > 0 && jump_initial(i - 1) == prev) i--;
				}
				next = i;
			}

			if (next != sel) { sel = next; sel_commit(); mark_dirty(); } else nudge();
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

		/*
		  Letting go of a key that scrolls the shelf commits the chrome to wherever it
		  scrolled to. This is what makes a tap feel instant: a tap is a press and a release
		  a few tens of milliseconds apart, so the title follows the cards almost at once,
		  and it is only a key held down long enough to repeat that ever defers anything.

		  The release and not the idle frame, which is the trap. menu_key_get() delivers a
		  held key as a press every REPEATRATE and *nothing at all* in between, so chome_handle
		  is called with key == 0 on most frames of a hold - anything keyed off "no key this
		  frame" would commit between every pair of repeats and defer nothing. (That is also
		  why key_run cannot answer "is a key held": the line above resets it on those same
		  idle frames. The screenful jump it feeds is a separate matter, and not this one.)

		  Named keys rather than any release, so that a Y pressed and let go mid-scroll does
		  not commit a title the player is still scrolling past - and so that the harness can
		  force a full repaint mid-hold without changing what the frame should contain.
		*/
		if ((key & UPSTROKE) && (k == KEY_LEFT || k == KEY_RIGHT || k == KEY_MINUS || k == KEY_EQUAL))
		{
			sel_commit();
		}
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

	  Two tests, because neither covers the other. The latch is the only thing that knows
	  during the launch itself, before the core is loaded and while this process is still
	  the one that handed the drive over. core_holds_disc() is the only thing that knows
	  afterwards, in the re-exec'd process where the latch is 0 again - which is where the
	  helper was found alive under a playing game.
	*/
	if (disc_handed_to_core && is_menu()) disc_handed_to_core = 0;

	/*
	  And a third owner, which is the rip: its helper holds the drive for as long as the copy
	  takes, so the detection helper has to stay stopped for the same reason it stays stopped
	  under a playing core. Without this test disc_poll() would find nothing watching and
	  fork a fresh helper straight onto the drive being read - the same fault that was
	  measured under Metal Gear Solid, only over a 700 MB sequential read rather than a game.

	  rip_poll() runs regardless, and must: it is what notices the copy finishing, and the
	  screen showing the progress is drawn from what it reads. It is a read of one short line
	  in tmpfs and touches no device.
	*/
	if (rip_poll())
	{
		/*
		  A full repaint only when what is *written on the screen* changes, and not on every
		  line the child publishes.

		  The two are far apart. The child publishes four times a second for however many
		  minutes the disc takes, and the percentage under the disc changes at most a hundred
		  times in the whole rip - so marking dirty on every line would spend a full repaint
		  of the canvas four times a second, for the length of an operation whose whole point
		  is that the drive is the bottleneck. Folding what the screen actually shows into one
		  number and comparing that is the same trick the Wi-Fi and controller screens use a
		  few lines below, for the same reason.

		  The pie is not in this number on purpose: it moves with the sectors rather than with
		  the percentage, and it is inside the disc's rectangle, so the spin's partial repaint
		  already carries it at 60fps for the price of a blit. What is in it is everything
		  drawn *outside* that rectangle - the state, the percentage and the count of
		  unreadable sectors, which are the three things disc_dlg_from_rip() writes into the
		  line under the disc. Leave one out and that line goes stale behind a moving pie,
		  which is how this was found.
		*/
		const rip_status *rs = rip_state();
		static unsigned rip_shown = 0;

		unsigned sig = rip_showing()
			? (unsigned)(rip_percent(rs) | (rs->state << 8) | ((rs->bad & 0xffff) << 12))
			: 0;

		if (sig != rip_shown) { rip_shown = sig; mark_dirty(); }
	}

	int drive_is_ours = !disc_handed_to_core && !core_holds_disc() && !rip_busy();

	if (drive_is_ours) disc_poll();
	if (drive_is_ours && disc_take_dirty())
	{
		/*
		  mark_dirty() is the point of the dirty flag, and leaving it off is how the
		  badge came and went without the screen ever repainting - the same omission
		  that made the core options cursor look stuck. Nothing about a disc arriving
		  comes through a keypress, so if this does not ask for a repaint, nothing will.
		*/
		mark_dirty();

		/*
		  The remembered core choice is per disc - that is its whole meaning - so any
		  change of what is in the drive forgets it. Left standing, the next disc
		  inherited the previous one's core: play a PlayStation disc, insert a Mega CD
		  one, and the prompt offered "Play on PlayStation" for it.
		*/
		disc_chosen_sys = -1;

		/*
		  Taken out while we were looking at it. Both disc screens describe a disc that
		  is no longer there, so they have to be left rather than sitting there
		  offering to play nothing.

		  The suspend strip counts as a third, when it was opened by Down in the disc
		  dialog: it is showing that disc's slots under that disc's name, and A on one of
		  them would hand a drive with nothing in it to a core. disc_susp_item_get() has
		  already stopped answering by now, so staying here would also mean the strip
		  falling back to the shelf's own selection mid-screen.
		*/
		/*
		  Not while a rip is on that screen, though. A copy running is a disc the front-end
		  is still about even though the drive reports none - the helper has it - so leaving
		  here would drop the player off the progress screen the instant it appeared and
		  leave the copy running with nothing on screen to say so. disc_or_rip_present().
		*/
		if (!disc_or_rip_present() && (screen == SCR_DISC || screen == SCR_DISCBAR
			|| (screen == SCR_SUSPEND && susp_is_disc && !ig_running_disc())))
		{
			go_screen(SCR_HOME);
		}

		/*
		  The remembered core choice is "for this disc" - see disc_chosen_sys - and
		  this is where that promise is kept. Every disc change passes through ABSENT,
		  so forgetting here is what stops a choice outliving its disc: play a
		  PlayStation disc, put a Neo Geo CD in afterwards, and a remembered PSX pick
		  would caption the new disc's prompt "Play on PlayStation" - and launch it.
		*/
		if (disc_state() == DISC_ABSENT) disc_chosen_sys = -1;

		printf("ClassicUI: disc state=%d type=%s name=\"%s\"\n",
			disc_state(), disc_type_name(disc_type()), disc_display_name());

		/*
		  And the disc's scan, asked for here because this is where the disc becoming
		  identified happens - his instruction is that the picture should be fetched and
		  sized before the player opens the dialog, not when they do. Last in this branch
		  so that a disc that has just gone leaves the screens it was about first: the
		  helper reports an eject as a state change too, and disc_art_prefetch() has
		  nothing to do with one.
		*/
		disc_art_prefetch();
	}

	/*
	  And a repaint while it is spinning, for the same reason the Wi-Fi screen repaints
	  while it scans: the badge animates, and this UI only draws when something says it
	  must. Rate-limited to the animation step so a spinning disc does not mean a full
	  repaint every pass of this loop.
	*/
	/*
	  Or while the dialog is up over a disc that is already playing, which is the one case
	  where a disc is on screen and the drive says there is none: it is the core's, so
	  disc_poll() is not running and disc_state() has been ABSENT since the launch. Without
	  this arm the in-game dialog drew its disc once and it sat there, stopped.
	*/
	/*
	  Or while a rip is copying, which is the other case where a disc is on screen and the
	  drive says there is none: the helper has it. This arm is also what keeps the disc
	  turning through a stalled read - the spin follows rip_busy() and not the progress, so a
	  drive that has gone away for a few seconds shows a disc still turning and a percentage
	  that has stopped, which is the honest pair of facts.
	*/
	if (disc_or_rip_present() || (screen == SCR_DISC && !disc_picking && ig_running_disc()))
	{
		/*
		  Faster than the other animations, because the disc travels further per frame -
		  see GFX_DISC_MS. Only while a disc is in the drive, so a machine with an empty
		  one repaints exactly as often as it did before any of this existed.

		  Not mark_dirty(): the disc turning is the one change on screen, so it asks for
		  the partial path - see the dispatch at the bottom of this function - and a
		  full repaint every 50ms was most of what a spinning disc cost.

		  The rate follows which path that dispatch will take; disc_spin_ms() asks the
		  same two questions it does.
		*/
		static unsigned long disc_next_spin = 0;
		if (CheckTimer(disc_next_spin))
		{
			disc_next_spin = GetTimer(disc_spin_ms());
			disc_spin_due = 1;
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
	// The half-resolution request rides the same idiom: free when unchanged, and
	// re-asserting is what makes the More Settings toggle land - the resize branch
	// below picks the new canvas up like any other.
	fb_size_sync();
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

		/*
		  The still of the game, rebuilt at the new size. This is the hole
		  draw_background() reports when it falls through to the grid: ig_bg is
		  allocated at the canvas size, and until now nothing rebuilt it when the
		  canvas changed under an open menu - the analog takeover could always do
		  that, and the half-resolution toggle in More Settings now does it on
		  purpose. ig_shot is kept for exactly as long as the menu is open, so the
		  rebuild is a rescale of what was already grabbed, not a second grab.
		*/
		if (ig_active) ig_build_background(theme_get());

		gfx_damage_all();
		dirty = 1;
	}

	sync_sel_slots();
	request_visible_art(theme_get());

	/*
	  A cover that finished decoding this frame.

	  mark_slide(), because on the shelf a decoded cover can only have changed the face of a
	  card, and cards are the band. This one matters as much as the ease does: art_step()
	  decodes one image per frame by design, so scrolling into a stretch of the shelf nobody
	  has visited yet lands a new cover on most frames of the scroll - and asking for the
	  whole screen each time would have handed most of the saving straight back, on exactly
	  the pass where it was worth most.

	  The one place that acts on mark_slide() upgrades it to a full repaint off the shelf,
	  which is what covers the other consumer of the same cache: the suspend strip draws
	  thumbnails from it, and those are nowhere near the card row.
	*/
	int before = art_cache_count();
	art_step();
	if (art_cache_count() != before) mark_slide();

	/*
	  A disc scan that has just been written, which the disc on screen has to become.

	  Immediately after art_step(), because art_step() is what reaps the download and
	  scales it - so the picture that landed this frame is on screen this frame rather
	  than on the next one.

	  One repaint, and only for a scan that actually landed. Everything else is already in
	  place: disc_art_scale() drops the thumbnail cache's copy of that path through
	  art_forget(), art_thumb() then decodes the new file, and disc_rot() keys its rotated
	  buffer on the path and the source pointer - so a scan appearing is a cache miss in
	  both and the dialog draws the photograph rather than the generated face. The one
	  thing missing was anybody asking for a frame at all. Nothing about a download
	  finishing arrives on a keypress, and this UI paints when it is told to and not
	  otherwise: the picture was landing correctly and staying invisible until the player
	  happened to press something. The same omission made the disc badge appear and vanish
	  without a repaint, and it is why disc_take_dirty() above is followed by mark_dirty().

	  Not a per-frame test of the file, deliberately - that would be a stat of the card
	  sixty times a second for an event that happens at most once per disc per session.
	*/
	if (disc_art_take_ready()) mark_dirty();

	animate();

	/*
	  And the scrolling text, which asks for a frame at the one instant its window changes.

	  Not a rate: gfx_marquee() said when it would next look different and marq_arm() turned
	  that into this deadline, so there is no tick to throw away and no frame painted
	  byte-identical to the one before it. The disc arrives at the same place from the other
	  direction - it asks sixty times a second and skips the ticks on which its angle has not
	  moved - and the reason the marquee can do better is that its motion is a pure function
	  of the clock with no accumulator in it.

	  marq_rc.on is the last composed frame's answer to "is anything scrolling", so a screen
	  with nothing long on it never reaches CheckTimer at all.
	*/
	if (marq_rc.on && CheckTimer(marq_next)) marq_due = 1;

	if (dirty)
	{
		dirty = 0;
		slide_due = 0;                 // a full repaint repaints the cards too
		disc_spin_due = 0;             // a full repaint repaints the disc too
		marq_due = 0;                  // and the scrolling text with them
		render();
	}
	/*
	  Before the disc, because a sliding shelf is the more urgent of the two and because the
	  two regions are nowhere near each other: the badge is in the top corner and the cards
	  are at the bottom, so a rectangle covering both would be most of the screen and worth
	  nothing. The spin is left pending instead of being cleared - the badge holds its
	  rotation for the fifth of a second a slide lasts and picks up the current one on the
	  first frame after it, which is the same thing a busy screen already does to it.
	*/
	else if (slide_due)
	{
		slide_due = 0;

		/*
		  When in doubt, the full repaint - the disc path's rule, and the reasons are the
		  same two.

		  ui_busy() means a progress track may be sweeping the screen, and gfx_track's sweep
		  is continuous in milliseconds: it would advance inside the band and not outside it,
		  and the seam would sit there until something else asked for a full frame.

		  Off the shelf, because the invariant this path needs is "nothing outside the band
		  changed", and the only screen this front-end can promise that for is the one whose
		  every other element is either static or committed. With a panel up the shelf is
		  behind a scrim anyway, and its slide is worth nothing to clip.

		  And no band at all means the last composed frame drew no cards - the browser
		  returns out of compose() before the shelf - so there is nothing to clip to and the
		  cards have to be drawn the only other way there is.
		*/
		int y0, y1;
		if (ui_busy() || screen != SCR_HOME || !slide_band(&y0, &y1)) render();
		else
		{
			render_region(0, y0, gfx_w(), y1 - y0 + 1);

			/*
			  A band frame replays the badge's draw under a clip that excludes it, so the
			  signature it recorded describes pixels that never reached the screen. Left
			  standing, the spin tick after the slide would compare equal and skip, and
			  the badge would hold a stale angle for up to a whole position after the
			  shelf came to rest. Unknown forces the next tick to paint, which is exactly
			  what the pending spin did before the skip existed.
			*/
			disc_drawn_sig = -1;
		}
	}
	/*
	  Before the disc, and this one is not a matter of taste - it is what stops the marquee
	  starving. The spin tick fires every GFX_DISC_PART_MS and the chain is an else-if, so a
	  marquee arm sitting below a spinning disc would never be served at all while a disc was
	  in the drive: the text would freeze on the shelf exactly when a disc was on it, which
	  reads as the front-end having stopped. The other way round costs the disc five of its
	  sixty ticks a second, and the tick it loses is one it would very likely have skipped -
	  the badge's angle only moves sixteen times a second at the slow rate.

	  The spin is left pending rather than cleared, the same as the slide leaves it: the badge
	  picks up the current angle on the next tick, a fraction of a position later.
	*/
	else if (marq_due)
	{
		marq_due = 0;

		/*
		  The same two escapes as the slide and the disc, for the same two reasons.

		  ui_busy() means gfx_track's sweep may be crossing the screen, and that sweep is
		  continuous in milliseconds: it would advance inside the band and not outside it, and
		  the seam would sit there until something asked for a whole frame.

		  And no band means the last composed frame scrolled nothing - which the arm above
		  already tested, but the frame can change between the arm and here, and a
		  render_region() of a rectangle nobody recorded is a repaint of whatever y0 and y1
		  last happened to hold.
		*/
		int y0, y1;
		if (ui_busy() || !marq_band(&y0, &y1)) render();
		else
		{
			render_region(0, y0, gfx_w(), y1 - y0 + 1);

			/*
			  And the badge's signature is stale for the same reason it is after a slide: this
			  frame replayed its draw under a clip that excluded it, so what it recorded never
			  reached the screen. See the note in the slide arm above.
			*/
			disc_drawn_sig = -1;
		}
	}
	else if (disc_spin_due)
	{
		disc_spin_due = 0;

		/*
		  When in doubt, the full repaint. ui_busy() means a progress track may be on
		  screen, and gfx_track's sweep is continuous in milliseconds - a track crossing
		  the disc's rectangle would advance inside the clip and not outside it, and the
		  seam would sit there until the next full frame. While something is busy this
		  UI is repainting fully every GFX_SPIN_MS anyway, so a full frame at the disc
		  rate is what shipped before this path existed.
		*/
		int x, y, w, h;
		if (ui_busy()) render();
		else if (disc_spin_rect(&x, &y, &w, &h))
		{
			/*
			  Only when the disc will actually look different. The tick fires every
			  GFX_DISC_PART_MS; the angle moves every 62.5ms at the slow rate, so most
			  ticks would repaint the rectangle byte-identical - see disc_spin_sig().
			  The focused badge is exempt: its breath and pulse ride the clock, not
			  the angle, so on the tier every tick is a real frame.
			*/
			if (screen == SCR_DISCBAR || disc_spin_sig() != disc_drawn_sig)
				render_region(x, y, w, h);
		}
		// Otherwise the last frame drew no disc (the browser, say): nothing on screen
		// is turning, so nothing needs painting at all.
	}

	return 1;
}
