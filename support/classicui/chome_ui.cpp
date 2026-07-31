#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>

#include "chome.h"
#include "chome_gfx.h"
#include "chome_theme.h"
#include "chome_lib.h"
#include "chome_art.h"
#include "chome_video.h"
#include "chome_icons32.h"
#include "chome_icons16.h"
#include "chome_osk.h"
#include "chome_net.h"

#include "../../cfg.h"
#include "../../user_io.h"
#include "../../input.h"
#include "../../osd.h"
#include "../../video.h"
#include "../../hardware.h"
#include "../../file_io.h"
#include "../../menu.h"
#include "../arcade/mra_loader.h"
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
static int ig_selected_running = 0;          // shelf parked on the running game
static unsigned long ig_close_until = 0;     // "press A again to close the game"
static int wifi_row = 0;                     // which network is picked
static int wifi_top = 0;                     // first one on screen
static char wifi_pick[NET_SSID];             // ...and its name, kept across the keyboard
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

// Rows on the Options panel. Several places step over them.
#define OPT_ROWS    7

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
#define CH_SLOTS      4
#define CH_SLOTS_USER 3

#define MB_DISPLAY  0
#define MB_OPTIONS  1
#define MB_ABOUT    2
#define MB_COUNT    3

/*
  Language and Manuals are gone. The first opened a panel with nothing behind it,
  and the second only handed the screen to the classic OSD - which is exactly what
  the front-end is not supposed to do on its own.
*/
static const char *mb_label[MB_COUNT] = { "Display", "Options", "About" };

/*
  Every Display option lives in the scaler - filters, shadow mask, gamma - so the
  whole entry is dropped when the scaler's output is not what reaches the screen:
  direct_video, or an analog-only setup without vga_scaler. Showing a CRT filter
  picker to somebody already looking at a real CRT would be daft.
*/
static int mb_visible(int i)
{
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

	if (r.sel_key)
	{
		for (int i = 0; i < n; i++)
		{
			const chome_entry *e = lib_view_entry(i);
			if (!e || e->kind != ENT_GAME) continue;
			chome_item *it = lib_item(e->game);
			if (it && it->key == r.sel_key) { sel = i; break; }
		}
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

static double bar_y = 0, strip_y = 0, curtain = 0;
static unsigned long launch_at = 0;
static unsigned long nudge_until = 0;
static unsigned long last_ms = 0;

static int dirty = 1;

static uint32_t last_key = 0;
static int key_run = 0;

// Set once the user hands off to the classic menu, so we do not immediately
// grab the screen back. The OSD/menu button brings us back.
static int handed_off = 0;

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
static void ig_select_running();
static int ss_can_save();
static int susp_matches(const chome_item *it);
static int ig_load_item();
static void quit_to_home(int suspend);
static int ss_can_load();
static int ss_do_save(int slot);
static int ss_do_load(int slot);
static void ss_pause_release(int engaged);

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

// Keeps the shelf pips honest: re-stat the selected game's savestate slots
// whenever the selection moves. Four stat() calls, only on change.
static void sync_sel_slots()
{
	static int last_sel = -1;
	static int last_view = -1;

	if (sel == last_sel && view == last_view) return;
	last_sel = sel;
	last_view = view;

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
		const char *row = d->rows[oy * ICON32 / box];
		for (int ox = 0; ox < box; ox++)
		{
			if (row[ox * ICON32 / box] == '#') gfx_fill(x + ox, y + oy, 1, 1, col);
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
}

static void draw_pips(const chome_profile *p)
{
	chome_item *it = cur_game();
	if (!it) return;

	int s = (p->id == PROF_HD) ? 2 : 1;
	int d = 6 * s, gap = 5 * s, n = CH_SLOTS_USER;
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

  Which shape goes with which menu button is looked up, not assumed: SYS_BTN_A is
  whatever button that pad has mapped to it, and the PSX pad reports its faces as the
  four positional codes (snacpad.cpp: Triangle north, Circle east, Cross south,
  Square west). With MiSTer's default map that lands on circle to confirm and cross
  to go back - the Japanese convention, and the one the physical layout implies,
  since A sits where the circle does. Remap the pad and this follows.

  The cross comes from the OSD font's own X: every drawn cross we tried breaks into
  dots at the eight pixels a 240p legend has for it, and an X is what people type for
  that button anyway.
*/
#define BTN_CODE_SOUTH 0x130
#define BTN_CODE_EAST  0x131
#define BTN_CODE_NORTH 0x133
#define BTN_CODE_WEST  0x134

static int pad_is_psx()
{
	const char *n = input_menu_key_devname();
	if (!n || !*n) return 0;

	// A SNAC pad is a uinput device snacpad.cpp names; a USB Sony pad says so too,
	// and the same labels are right for it.
	return strcasestr(n, "SNAC") || strcasestr(n, "PlayStation")
		|| strcasestr(n, "DualShock") || strcasestr(n, "DualSense")
		|| strcasestr(n, "Sony") ? 1 : 0;
}

struct prompt { const char *text; const char *pic; };

static prompt btn_prompt(int which)
{
	prompt out = { btn(which), 0 };
	if (which < 0 || which >= LBL_COUNT || !using_pad || !pad_is_psx()) return out;

	static const int sysbtn[LBL_COUNT] =
		{ SYS_BTN_A, SYS_BTN_B, SYS_BTN_X, SYS_BTN_Y, SYS_BTN_SELECT };

	switch (input_menu_key_btn(sysbtn[which]))
	{
	case BTN_CODE_EAST:  out.pic = "psx_circle";   break;
	case BTN_CODE_NORTH: out.pic = "psx_triangle"; break;
	case BTN_CODE_WEST:  out.pic = "psx_square";   break;
	case BTN_CODE_SOUTH: out.text = "X";           break;
	default: break;                                // Select keeps its own name
	}
	return out;
}

struct legend_pair { const char *key; const char *pic; const char *label; const char *shortl; };

// A prompt for one of the face buttons, whatever it is called on this controller.
static legend_pair lp(int which, const char *label, const char *shortl)
{
	prompt pr = btn_prompt(which);
	return { pr.text, pr.pic, label, shortl };
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
		if (here && ss_can_load() && n < max) { out[n++] = lp(LBL_A, "Load", "Load"); }
		else if (n < max) { out[n++] = lp(LBL_A, "Resume", "Play"); }
		if (here && ss_can_save() && n < max) { out[n++] = lp(LBL_Y, "Save", "Save"); }
		else if (n < max) { out[n++] = { CH_DOWN, 0, "Lock", "Lock" }; }
		if (n < max) { out[n++] = lp(LBL_X, "Delete", "Del"); }
		if (n < max) { out[n++] = lp(LBL_B, "Back", "Back"); }
		break;
	}
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
	case SCR_DISPLAY:
		if (n < max) { out[n++] = { CH_LEFT CH_RIGHT, 0, "Choose", "Sel" }; }
		if (n < max) { out[n++] = lp(LBL_A, "Apply", "OK"); }
		if (n < max) { out[n++] = lp(LBL_B, "Back", "Back"); }
		break;
	case SCR_OPTIONS:
		if (n < max) { out[n++] = { CH_LEFT CH_RIGHT, 0, "Change", "Chg" }; }
		if (n < max) { out[n++] = lp(LBL_A, "Select", "OK"); }
		if (n < max) { out[n++] = lp(LBL_B, "Back", "Back"); }
		break;
	case SCR_ABOUT:
		if (n < max) { out[n++] = lp(LBL_B, "Back", "Back"); }
		break;
	case SCR_MENUBAR:
		if (n < max) { out[n++] = lp(LBL_A, "Open", "Open"); }
		if (n < max) { out[n++] = { CH_LEFT CH_RIGHT, 0, "Move", "Move" }; }
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
			if (n < max) { out[n++] = { CH_UP, 0, "Menu", "Menu" }; }
			if (n < max) { out[n++] = lp(LBL_SELECT, "Sort", "Sort"); }
		}
		else
		{
			int running = ig_is_running(cur_game()) || susp_matches(cur_game());
			if (n < max) { out[n++] = lp(LBL_A, running ? "Resume" : "Start", running ? "Play" : "Start"); }
			/*
			  Order is priority: the 240p legend keeps only the first three, and
			  favouriting a game is worth more there than re-sorting the shelf. The
			  action itself was always here - it just never appeared on a CRT.
			*/
			if (n < max) { out[n++] = { CH_DOWN, 0, "Suspend Points", "Saves" }; }
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
		// A pictogram occupies one glyph cell, so the row measures the same either way.
		widths[i] = (pairs[i].pic ? 8 * s : gfx_text_w(pairs[i].key, s))
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
		int kw = pairs[i].pic ? 8 * s : gfx_text_w(pairs[i].key, s);
		gfx_fill(x - 2 * s, p->y_legend - 2 * s, kw + 4 * s, 8 * s + 4 * s, COL_PANEL);

		if (pairs[i].pic) picto(pairs[i].pic, x, p->y_legend, 8 * s, COL_INK);
		else gfx_text(pairs[i].key, x, p->y_legend, s, COL_INK, 0);

		char up[64];
		snprintf(up, sizeof(up), "%s", labels[i]);
		for (char *q = up; *q; q++) *q = (char)toupper((unsigned char)*q);
		gfx_text(up, x + kw + 5 * s, p->y_legend, s, COL_PANELHI, 0);

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
		snprintf(up, sizeof(up), "%s", mb_label[i]);
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
	return n + 1;
}

static void draw_rows(const panel_box *b, const char *const *rows, const char *const *vals, int n, int idx)
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
			gfx_text(v, b->x + b->w - 6 * b->s - vw, y, b->s, on ? COL_WHITE : COL_PANELLO, 0);
		}
	}
}

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
	if (armed) snprintf(hdr, sizeof(hdr), "DELETE SLOT %d? PRESS X AGAIN", del_arm_slot + 1);
	else snprintf(hdr, sizeof(hdr), "%s - SUSPEND POINTS", it ? it->title : "");
	for (char *q = hdr; *q; q++) *q = (char)toupper((unsigned char)*q);
	gfx_text(gfx_clip(hdr, s, p->w - p->inset * 2), p->inset, y + 6 * s, s,
		armed ? COL_RED : COL_PANELHI, 0);

	int n = CH_SLOTS_USER, tw = p->thumb_w, th = p->thumb_h, gap = p->thumb_gap;
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

		if (!st)
		{
			gfx_fill(x, ty, tw, th, COL_BG);
			gfx_frame_rect(x, ty, tw, th, COL_DIM, 1);
			gfx_text_c("EMPTY", x + tw / 2, ty + th / 2 - 4 * p->ts_tiny, p->ts_tiny, COL_DIM, 0);
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
		if (ref_shot_for(it, rp, sizeof(rp))) ref = art_thumb(rp, tile_w, tile_h);
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

	static const char *rows_menu[] = { "Cover Art", "Rescan Library", "Reinstall Looks", "Menu Layout", "Controllers", "Wi-Fi", "Advanced Settings" };
	static const char *rows_game[] = { "Cover Art", "Rescan Library", "Reinstall Looks", "Menu Layout", "Controllers", "Wi-Fi", "Close Game" };
	const char *const *rows = ig_active ? rows_game : rows_menu;
	char v1[32];
	if (lib_scanning()) snprintf(v1, sizeof(v1), "%d...", lib_scan_progress());
	else snprintf(v1, sizeof(v1), "%d games", lib_item_count());

	int closing = (ig_active && opt_row == OPT_ROWS - 1 && !CheckTimer(ig_close_until));

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

	const char *vals[] = {
		cfg.classicui_artfetch ? "Fetch Missing" : "Local Only",
		v1,
		"Write Files",
		cfg.classicui_profile == 0 ? "Auto" : theme_get()->name,
		"Classic Menu >",
		v2,
		ig_active ? (closing ? "PRESS A AGAIN" : "Back To Menu") : "Classic Menu >"
	};

	draw_rows(&b, rows, vals, OPT_ROWS, opt_row);

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

#define WIFI_VIS 6                           // rows on screen; the panel is sized for them

static void draw_wifi(const chome_profile *p)
{
	int s = p->ts_ui;
	int rowh = 14 * s;                       // a list aimed at with a pad, not a mouse

	/*
	  Sized for a fixed number of rows rather than for the screen or for however many
	  networks turned up. Filling four fifths of a 720p canvas to list three networks
	  looks broken, and a panel that changes size as more are found is worse.
	*/
	int w = p->w - 2 * p->inset;
	if (w > 44 * 8 * s) w = 44 * 8 * s;

	int h = (10 * s + 6) + 16 * s + WIFI_VIS * rowh + 6 * s;
	if (h > p->h - 2 * p->safe_y) h = p->h - 2 * p->safe_y;

	panel_box b = draw_panel_ex(p, w, h, "Wi-Fi");

	const net_link *l = net_link_now();
	char st[96];
	if (l->up && l->ip[0]) snprintf(st, sizeof(st), "On %s   %s", l->ssid, l->ip);
	else if (l->up) snprintf(st, sizeof(st), "On %s   getting an address", l->ssid);
	else if (!net_present()) snprintf(st, sizeof(st), "No Wi-Fi adapter is plugged in");
	else snprintf(st, sizeof(st), "Not connected");
	gfx_text(gfx_clip(st, s, b.w - 12 * s), b.x + 6 * s, b.y + 3 * s, s, COL_INK, 0);
	gfx_fill(b.x + 6 * s, b.y + 13 * s, b.w - 12 * s, s, COL_PANELLO);

	int y0 = b.y + 18 * s;
	int foot = 12 * s;
	int vis = (b.y + b.h - 4 * s - y0) / rowh;
	if (vis > WIFI_VIS) vis = WIFI_VIS;
	if (vis < 1) vis = 1;

	// While a join is running, or as soon as it has finished, that is the only thing
	// worth saying: the list underneath is about to be wrong either way.
	int js = net_join_state();
	if (js != JOIN_IDLE)
	{
		const char *head;
		uint32_t col = COL_INK;

		if (js == JOIN_WORK) head = "Connecting";
		else if (js == JOIN_OK) { head = "Connected"; col = COL_GREEN; }
		else { head = "Could not connect"; col = COL_RED; }

		char body[128];
		if (js == JOIN_WORK)
			snprintf(body, sizeof(body), "Joining %s. This can take a minute.", net_join_detail());
		else if (js == JOIN_OK)
			snprintf(body, sizeof(body), "This MiSTer is on %s.", net_join_detail());
		else if (js == JOIN_LOST)
			snprintf(body, sizeof(body), "%s", net_join_detail());
		else
			snprintf(body, sizeof(body), "%s Your old network was put back.", net_join_detail());

		int cy = b.y + b.h / 2 - 12 * s;
		gfx_text_c(head, b.x + b.w / 2, cy, s, col, 0);

		char lines[4][64];
		int nl = wrap_text(body, (b.w - 16 * s) / (8 * s), lines, 3);
		for (int i = 0; i < nl; i++)
			gfx_text_c(lines[i], b.x + b.w / 2, cy + (i + 2) * 10 * s, s, COL_INK, 0);

		if (js != JOIN_WORK)
			gfx_text_c(using_pad ? "A - OK" : "ENTER - OK", b.x + b.w / 2,
				b.y + b.h - foot, s, COL_PANELLO, 0);
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

	int n = net_count();
	if (!n)
	{
		gfx_text_c(net_scanning() ? "Looking for networks" : "No networks found",
			b.x + b.w / 2, b.y + b.h / 2, s, COL_INK, 0);
		if (!net_scanning())
			gfx_text_c(using_pad ? "X - LOOK AGAIN" : "TAB - LOOK AGAIN", b.x + b.w / 2,
				b.y + b.h - foot, s, COL_PANELLO, 0);
		return;
	}

	if (wifi_row >= n) wifi_row = n - 1;
	if (wifi_row < 0) wifi_row = 0;
	if (wifi_row < wifi_top) wifi_top = wifi_row;
	if (wifi_row >= wifi_top + vis) wifi_top = wifi_row - vis + 1;

	int bx = b.x + b.w - 8 * s - 12 * s;         // the bars
	int lx = bx - 11 * s;                        // the padlock

	for (int i = 0; i < vis && wifi_top + i < n; i++)
	{
		const net_ap *a = net_at(wifi_top + i);
		if (!a) break;

		int on = (wifi_top + i == wifi_row);
		int y = y0 + i * rowh;

		if (on) gfx_fill(b.x + 4 * s, y - 3 * s, b.w - 8 * s, rowh - 2 * s, COL_BLUE);

		uint32_t ink = on ? COL_WHITE : COL_INK;

		// A dot marks the one we are on, rather than the word "connected" competing
		// with the name for the same row.
		if (a->current) gfx_fill(b.x + 8 * s, y + 2 * s, 4 * s, 4 * s, ink);

		gfx_text(gfx_clip(a->ssid, s, lx - (b.x + 16 * s) - 2 * s), b.x + 16 * s, y, s, ink, 0);

		if (a->secure) gfx_text(CH_LOCK, lx, y, s, ink, 0);
		draw_bars(bx, y, s, a->signal, ink, on ? COL_PANELLO : COL_PANELHI);
	}

	if (net_scanning())
		gfx_text_c("Looking for more", b.x + b.w / 2, b.y + b.h - foot, s, COL_PANELLO, 0);
	else if (n > vis)
	{
		char more[48];
		snprintf(more, sizeof(more), "%d of %d", wifi_row + 1, n);
		gfx_text_c(more, b.x + b.w / 2, b.y + b.h - foot, s, COL_PANELLO, 0);
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

static void launch_write_mgl(const chome_sys *s, const char *relpath)
{
	FILE *f = fopen("/tmp/classicui_launch.mgl", "wt");
	if (!f) return;

	fprintf(f, "<mistergamedescription>\n");
	fprintf(f, "\t<rbf>%s</rbf>\n", s->rbf);
	fprintf(f, "\t<file delay=\"%d\" type=\"%c\" index=\"%d\" path=\"%s\"/>\n",
		s->delay ? s->delay : 2, s->type == 's' ? 's' : 'f', s->index, relpath);
	fprintf(f, "</mistergamedescription>\n");
	fclose(f);
}

static void do_launch(int sysidx, const char *relpath, chome_item *it)
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

	if (!s->rbf[0]) { nudge(); return; }

	launch_write_mgl(s, relpath);
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
		screen == SCR_ABOUT || screen == SCR_WIFI);
	if (overlay) gfx_scrim(0, 0, p->w, p->h, COL_BGDARK, 2);

	draw_suspend(p);
	draw_legend(p);
	draw_menubar(p, screen == SCR_MENUBAR || overlay);

	switch (screen)
	{
	case SCR_SORT:    draw_sort_panel(p); break;
	case SCR_DISPLAY: draw_display_screen(p); break;
	case SCR_OPTIONS: draw_options_panel(p); break;
	case SCR_ABOUT:   draw_about_panel(p); break;
	case SCR_WIFI:    draw_wifi(p); break;
	case SCR_LAUNCH:  draw_launch(p); break;
	default: break;
	}

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
		if (n < 0 || n >= CH_SLOTS_USER) { nudge(); return; }
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
	case SCR_BROWSE:
		nudge();
		return;
	default:
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
	}
	mark_dirty();
}

static void move_v(int dir)
{
	switch (screen)
	{
	case SCR_HOME:
		if (dir < 0) { mb_idx = 0; go_screen(SCR_MENUBAR); }
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
		if (dir > 0) go_screen(SCR_HOME);
		else nudge();
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

	case SCR_SORT:
		sort_idx = (sort_idx + dir + SORT_COUNT) % SORT_COUNT;
		mark_dirty();
		break;

	case SCR_DISPLAY:
		nudge();               // one row of tiles: nothing above or below
		break;

	case SCR_OPTIONS:
		opt_row = (opt_row + dir + OPT_ROWS) % OPT_ROWS;
		mark_dirty();
		break;

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
		case MB_ABOUT:    go_screen(SCR_ABOUT); break;
		}
		break;

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
		case 1: lib_rescan(); art_shutdown(); art_init(theme_get()->sel_w, theme_get()->sel_h); view_rebuild(0); break;
		case 2: vp_install(); mark_dirty(); break;
		case 3: nudge(); break;                       // Layout changes with left/right
		case 4:
			if (ig_active) { ig_close(1); open_joystick_setup(); }
			else { chome_leave(); open_joystick_setup(); }
			break;

		case 5:
			wifi_row = 0;
			wifi_top = 0;
			go_screen(SCR_WIFI);
			if (net_present() && !net_count()) net_scan_start();
			break;

		case 6:
			if (!ig_active) { chome_leave(); break; }

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

		// Otherwise it means launching the game; the core picks its slot up.
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
	case SCR_HOME:
		if (nav_pop()) break;
		if (ig_active) ig_close(1);             // nothing to go back to but the game
		else nudge();
		break;

	case SCR_SORT:
	case SCR_DISPLAY:
	case SCR_OPTIONS:
	case SCR_ABOUT:
		go_screen(SCR_MENUBAR);
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
			ss_copy_opt(spec, ss_hk.slot_opt, sizeof(ss_hk.slot_opt));
			ss_hk.slot_ex = ex;

			// How many values the option offers, so we never select a missing slot.
			int n = 0;
			char v[64];
			while (n < 8 && substrcpy(v, p, (char)(2 + n)) && v[0]) n++;
			ss_hk.slot_count = n ? n : 4;
			ss_hk.found_slot = 1;
		}
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
	  A pause the core only honours while the OSD is on screen is no use here: the
	  OSD draws over this UI, so holding it open to win the pause would put stale
	  menu rows on top of everything. Setting the option alone changes nothing,
	  since OSD_STATUS stays low - so say so rather than claim a pause that did not
	  happen.
	*/
	if (h->pause_needs_osd)
	{
		printf("ClassicUI: this core only pauses while the OSD is open, so it keeps running\n");
		return 0;
	}

	if (h->pause_is_option)
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

static void ss_pause_release(int engaged)
{
	const ss_hooks *h = ss_get();
	if (!engaged || !h->found_pause) return;

	if (h->pause_is_option) user_io_status_set(h->pause_opt, ss_pause_prev, h->pause_ex);
	else ss_pulse(h->pause_opt, h->pause_ex);

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
	if (slot == susp_slot()) ss_quiet_until = GetTimer(SS_QUIET_MS);

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

	if (slot == susp_slot()) ss_quiet_until = GetTimer(SS_QUIET_MS);

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

	for (int y = 0; y < h; y++)
	{
		int sy = (y * ig_shot_h) / h;
		const uint32_t *srow = ig_shot + (size_t)sy * ig_shot_w;
		uint32_t *drow = buf + (size_t)y * w;
		for (int x = 0; x < w; x++) drow[x] = srow[(x * ig_shot_w) / w] | 0xff000000u;
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
	if ((long long)ig_shot_w * p->h > (long long)ig_shot_h * p->w)
	{
		fw = p->w;
		fh = (int)((long long)p->w * ig_shot_h / ig_shot_w);
	}
	else
	{
		fh = p->h;
		fw = (int)((long long)p->h * ig_shot_w / ig_shot_h);
	}
	if (fw < 1) fw = 1;
	if (fh < 1) fh = 1;

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

	int n = lib_view_count();
	for (int i = 0; i < n; i++)
	{
		const chome_entry *e = lib_view_entry(i);
		if (!e || e->kind != ENT_GAME) continue;

		chome_item *it = lib_item(e->game);
		if (!it) continue;
		if (it->sysidx != ig_item.sysidx || strcmp(it->path, ig_item.path)) continue;

		sel = i;
		selF = i;
		ig_selected_running = 1;
		mark_dirty();
		return;
	}
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

	if (!gfx_begin()) { free(ig_shot); ig_shot = 0; return 0; }

	// Take the framebuffer. A core without support leaves us nothing to draw on.
	if (!video_menu_fb_present(ig_fb))
	{
		printf("ClassicUI: core has no HPS framebuffer, leaving the OSD to it\n");
		free(ig_shot);
		ig_shot = 0;
		return 0;
	}

	// As in the menu core: input keeps arriving and the overlay ends up off, because
	// it draws over this UI rather than under it.
	OsdEnable(DISABLE_KEYBOARD);
	OsdMenuCtl(0);

	ig_mute_engage();             // before the freeze state, which takes a moment to write

	ss_hk_valid = 0;              // re-read CONF_STR: it may not have been ready before
	ig_load_item();
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
	view_rebuild(1);
	art_init(theme_get()->sel_w, theme_get()->sel_h);

	ig_paused = ss_pause_engage();
	ig_frozen = freeze_engage();

	ig_active = 1;
	screen = SCR_HOME;
	slot_idx = 0;
	del_arm_slot = -1;
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

	lib_state_save();
	net_watch(0);
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
	OsdEnable(DISABLE_KEYBOARD);
	OsdMenuCtl(0);

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

	if (eat_menu_release && igmenu && (key & UPSTROKE))
	{
		eat_menu_release = 0;
		return 1;
	}

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
			if (igpress && igmenu)
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
		if (video_fb_state())
		{
			active = 0;
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
				/*
				  The core writes the state itself and process_ss() notices on its
				  next poll, so resume immediately: it needs to run those frames.
				  The slot and its thumbnail are there next time the menu opens.
				*/
				if (ss_do_save(slot_idx)) { ig_frozen = 0; ig_close(1); }
				else nudge();
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
			// Only in a game core: the handoff in the menu core wants the classic
			// menu, and there the release is what asks for it.
			if (ig_active) { eat_menu_release = 1; ig_close(1); }
			else chome_leave();
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
	  Networking. Cheap unless something is in flight: it reaps the child that runs
	  iw or ifup, and asks for the link again every few seconds while a screen is
	  showing it. The state has to be picked up even after leaving the screen,
	  because the child is still out there either way.
	*/
	net_poll();

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

	// Background work: one scan slice and one art decode per frame.
	if (lib_scanning())
	{
		lib_scan_step();
		art_init(theme_get()->sel_w, theme_get()->sel_h);
		view_rebuild(1);
		if (ig_active) ig_select_running();     // findable once its system is in
	}

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
