/*
  Host-side stubs for the Classic Home test harness.

  The five chome_*.cpp files are compiled UNMODIFIED against the real project
  headers and linked against these fakes, so what runs here is the same code that
  runs on the DE10-Nano - only the FPGA, SD card and clock are simulated.

  What this can prove: layout at every profile, index/scan/sort/view logic, art
  lookup and decode, savestate slot detection, the exact MGL written at launch,
  and that nothing crashes or reads out of bounds.

  What it cannot prove: anything about the real framebuffer's timing or the
  uncached-memory cost, SPI behaviour, or whether a core accepts the MGL.
*/

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>

#include "../../../lib/imlib2/Imlib2.h"

#include "../../../cfg.h"
#include "../../../video.h"
#include "../../../scaler.h"
#include "../../../osd.h"
#include "../../../hardware.h"
#include "../../../menu.h"
#include "../../../input.h"
#include "../../../user_io.h"
#include "../../../spi.h"
#include "../../arcade/mra_loader.h"
#include "../../../file_io.h"

#include "harness.h"

cfg_t cfg;

/* ------------------------------------------------------------ fake clock -- */

static unsigned long vclock = 100000;
static int real_clock = 0;

void harness_advance(unsigned long ms) { vclock += ms; }
void harness_use_real_clock(int on) { real_clock = on; }

// Deterministic virtual time for the tests, wall time for the interactive viewer.
static unsigned long now_ms()
{
	if (!real_clock) return vclock;

	struct timespec tp;
	clock_gettime(CLOCK_MONOTONIC, &tp);
	return (unsigned long)(tp.tv_sec * 1000ul + tp.tv_nsec / 1000000ul);
}

unsigned long harness_now() { return now_ms(); }

unsigned long GetTimer(unsigned long offset) { return now_ms() + offset; }
unsigned long CheckTimer(unsigned long t) { return (!t) || (now_ms() >= t); }

/* ------------------------------------------------------- fake filesystem -- */

static char fake_root[512] = "/tmp/chome_sd";

void harness_set_root(const char *r) { snprintf(fake_root, sizeof(fake_root), "%s", r); }

const char *getRootDir() { return fake_root; }

const char *getFullPath(const char *name)
{
	static char p[1024];
	if (name && name[0] == '/') snprintf(p, sizeof(p), "%s", name);
	else snprintf(p, sizeof(p), "%s/%s", fake_root, name ? name : "");
	return p;
}

int FileExists(const char *name, int)
{
	struct stat st;
	const char *p = getFullPath(name);
	return (!stat(p, &st) && S_ISREG(st.st_mode)) ? 1 : 0;
}

/*
  Answers *relative to the root*, as the real one does - it returns "games/SNES" and
  leaves prepending to the caller. Modelling that matters: while this returned absolute
  paths the harness could not see that the front-end was handing a root-relative path to
  opendir(), which made every ROM system empty on a real boot.
*/
int findGamesDir(char *dir, size_t dir_len)
{
	char probe[1024];
	struct stat st;

	snprintf(probe, sizeof(probe), "%s/games/%s", fake_root, dir);
	if (!stat(probe, &st) && S_ISDIR(st.st_mode))
	{
		char rel[1024];
		snprintf(rel, sizeof(rel), "games/%s", dir);
		snprintf(dir, dir_len, "%s", rel);
		return 1;
	}

	snprintf(probe, sizeof(probe), "%s/%s", fake_root, dir);
	if (!stat(probe, &st) && S_ISDIR(st.st_mode)) return 1;   // already the relative form

	return 0;
}

int FileSaveConfig(const char *name, void *buf, int size)
{
	char p[1024];
	snprintf(p, sizeof(p), "%s/config/%s", fake_root, name);
	FILE *f = fopen(p, "wb");
	if (!f) return 0;
	fwrite(buf, 1, size, f);
	fclose(f);
	return 1;
}

int FileLoadConfig(const char *name, void *buf, int size)
{
	char p[1024];
	snprintf(p, sizeof(p), "%s/config/%s", fake_root, name);
	FILE *f = fopen(p, "rb");
	if (!f) return 0;
	int n = (int)fread(buf, 1, size, f);
	fclose(f);
	return n;
}

int FileLoad(const char *name, void *buf, int size)
{
	if (!buf) return 0;
	char p[1024];
	snprintf(p, sizeof(p), "%s/%s", fake_root, name);
	FILE *f = fopen(p, "rb");
	if (!f) return 0;
	int n = (int)fread(buf, 1, size, f);
	fclose(f);
	return n;
}

/*
  Which MiSTer.ini is in use. The real cfg_get_name() scans the card for MiSTer_*.ini
  and offers them as alternates; nothing in the harness exercises that, and the
  Best Settings screen only needs a name to hang off getRootDir().
*/
uint16_t altcfg(int) { return 0; }
const char *cfg_get_name(uint8_t) { return "MiSTer.ini"; }

/*
  cfg.cpp's option table, as much of it as the configuration check asks about.

  The check reads the real table through these three accessors precisely so that it
  never keeps a list of option names of its own - see cfg.h. The harness has no cfg.cpp
  to read, so this is a fixture standing in for it, and it is deliberately a *short*
  one: only the classicui rows and debug, because those are the only rows the report
  prints and the only ones "is this a real option?" is ever asked about.

  What it therefore does not prove is that the shipped list is complete - that is
  structural, because in the firmware these functions are the table rather than a copy
  of it. What it does prove is the analysis: that a name in the table is accepted, a
  name outside it is called a typo, and every resolved value is printed.
*/
static const struct { const char *name; char kind; void *var; } stub_vars[] =
{
	{ "DEBUG",                   'u', &cfg.debug },
	{ "CLASSICUI",               'u', &cfg.classicui },
	{ "CLASSICUI_PROFILE",       'u', &cfg.classicui_profile },
	{ "CLASSICUI_OVERSCAN",      'u', &cfg.classicui_overscan },
	{ "CLASSICUI_HALFRES",       'u', &cfg.classicui_halfres },
	{ "CLASSICUI_TRACKING",      'i', &cfg.classicui_tracking },
	{ "CLASSICUI_CAPS",          'u', &cfg.classicui_caps },
	{ "CLASSICUI_ARTDIR",        's', cfg.classicui_artdir },
	{ "CLASSICUI_ARTFETCH",      'u', &cfg.classicui_artfetch },
	{ "CLASSICUI_ARTFILL",       'u', &cfg.classicui_artfill },
	{ "CLASSICUI_GAMELIST",      'u', &cfg.classicui_gamelist },
	{ "CLASSICUI_FREEZE",        'u', &cfg.classicui_freeze },
	{ "CLASSICUI_ARTURL",        's', cfg.classicui_arturl },
	{ "CLASSICUI_SCREENSCRAPER", 'u', &cfg.classicui_screenscraper },
	{ "CLASSICUI_DISC",          'u', &cfg.classicui_disc },
	{ "CLASSICUI_SS_USER",       's', cfg.classicui_ss_user },
	{ "CLASSICUI_SS_PASS",       's', cfg.classicui_ss_pass },
	{ "CLASSICUI_SS_REPLACE_PACK", 'u', &cfg.classicui_ss_replace_pack },
};

int cfg_var_count() { return (int)(sizeof(stub_vars) / sizeof(stub_vars[0])); }

const char *cfg_var_name(int i)
{
	if (i < 0 || i >= cfg_var_count()) return "";
	return stub_vars[i].name;
}

const char *cfg_var_text(int i, char *out, int max)
{
	if (max > 0) out[0] = 0;
	if (i < 0 || i >= cfg_var_count() || max <= 0) return out;

	switch (stub_vars[i].kind)
	{
	case 'u': snprintf(out, max, "%u", *(uint8_t*)stub_vars[i].var); break;
	case 'i': snprintf(out, max, "%d", *(int8_t*)stub_vars[i].var); break;
	default:  snprintf(out, max, "%s", (char*)stub_vars[i].var); break;
	}
	return out;
}

/* ---------------------------------------------------- fake framebuffers --- */

static int fb_supported = 1;
void harness_set_fb_supported(int v) { fb_supported = v; }

static uint32_t *fb[3] = {};
static int fbw = 1280, fbh = 720;
static int presented = 1;
static int present_count = 0;

/*
  The half-resolution request, modelled the way video_fb_config() applies it: the size
  harness_set_fb() states is the *display mode*, and the canvas the front-end sees is
  that divided by the request - unless the division would leave less than 320x240, in
  which case it is refused, exactly as the firmware refuses it for the analog takeover's
  TV modes. A model rather than the real code, because video.cpp does not compile in
  this harness; the division and the floor are the two behaviours the front-end depends
  on, and anything richer than that belongs on the device.
*/
static int fb_native_w = 1280, fb_native_h = 720;
static int fb_req = 0;
static int fb_div = 1;

static void fb_apply()
{
	int div = 1;
	if (fb_req > 1 && fb_native_w / fb_req >= 320 && fb_native_h / fb_req >= 240)
		div = fb_req;

	fb_div = div;
	int w = fb_native_w / div, h = fb_native_h / div;
	if (w == fbw && h == fbh && fb[1]) return;

	for (int i = 1; i <= 2; i++)
	{
		free(fb[i]);
		fb[i] = (uint32_t*)calloc((size_t)w * h, 4);
	}
	fbw = w;
	fbh = h;
}

void video_fb_size_request(int div)
{
	div = (div == 2 || div == 4) ? div : 0;
	if (div == fb_req) return;
	fb_req = div;
	fb_apply();
	printf("  [stub] fb size request %d: canvas now %dx%d (div %d)\n", div, fbw, fbh, fb_div);
}

int video_menu_fb_div() { return fb_div; }

void harness_set_fb(int w, int h)
{
	fb_native_w = w;
	fb_native_h = h;

	// Through the same gate as the request, so a section that pins the canvas while
	// the option is on gets the same answer the device would.
	fb_div = -1;                    // force the realloc even at an equal size
	for (int i = 1; i <= 2; i++) { free(fb[i]); fb[i] = 0; }
	fb_apply();
}

uint32_t *harness_fb_shown() { return fb[presented]; }

/*
  A hash of some rows of the frame that was last shown. Used to tell "the legend
  changed" from "the legend did not", which is the only way to check a prompt from
  outside: there is no way to read the strings back out of the front-end. Rows rather
  than the whole frame, because the shelf is still easing things about.
*/
unsigned long harness_fb_hash(int y0, int y1)
{
	return harness_fb_hash_box(0, y0, fbw, y1);
}

/*
  The same, bounded horizontally. A full-width band takes in the shelf either side of a
  centred panel, and the shelf is not the subject when a panel is: a card finishing its
  decode would read as the panel having changed.
*/
unsigned long harness_fb_hash_box(int x0, int y0, int x1, int y1)
{
	const uint32_t *p = fb[presented];
	if (!p) return 0;

	if (y0 < 0) y0 = 0;
	if (y1 > fbh) y1 = fbh;
	if (x0 < 0) x0 = 0;
	if (x1 > fbw) x1 = fbw;

	unsigned long h = 1469598103934665603UL;
	for (int y = y0; y < y1; y++)
	{
		for (int x = x0; x < x1; x++)
		{
			h ^= p[(size_t)y * fbw + x];
			h *= 1099511628211UL;
		}
	}
	return h;
}

/*
  The same, with a rectangle cut out of it.

  For hashing something that holds a moving part. The disc in the dialog turns, and its angle
  at any moment is not a function of the clock: disc_step() carries a phase and a smoothstep
  ramp, so the angle depends on the whole history of advances before it. A hash over the disc
  is therefore a hash over "how many frames every earlier section happened to run", and it
  changes when an unrelated section is added - which is exactly what it did, and the pixels
  that moved were the disc's 96x96 box and nothing else.

  So cut the moving part out and hash what is meant to hold still. The disc's own size and
  position are asserted separately, a few lines above, which is the part of it worth pinning.
*/
unsigned long harness_fb_hash_box_except(int x0, int y0, int x1, int y1,
	int ex0, int ey0, int ex1, int ey1)
{
	const uint32_t *p = fb[presented];
	if (!p) return 0;

	if (y0 < 0) y0 = 0;
	if (y1 > fbh) y1 = fbh;
	if (x0 < 0) x0 = 0;
	if (x1 > fbw) x1 = fbw;

	unsigned long h = 1469598103934665603UL;
	for (int y = y0; y < y1; y++)
	{
		for (int x = x0; x < x1; x++)
		{
			if (x >= ex0 && x < ex1 && y >= ey0 && y < ey1) continue;
			h ^= p[(size_t)y * fbw + x];
			h *= 1099511628211UL;
		}
	}
	return h;
}
int harness_present_count() { return present_count; }

uint32_t *video_menu_fb(int n) { return (n >= 1 && n <= 2) ? fb[n] : 0; }
int video_menu_fb_width() { return fbw; }
int video_menu_fb_height() { return fbh; }

int video_menu_fb_present(int n)
{
	if (!fb_supported) return 0;
	presented = n;
	present_count++;
	return 1;
}

void video_fb_enable(int, int) {}
void video_loadPreset(char *, bool);

int video_fb_state() { return 0; }

static int scaler_visible = 1;
void harness_set_scaler_visible(int v) { scaler_visible = v; }

/*
  Whether an HDMI sink is attached, which on hardware is one i2c byte and is NOT the
  same question as "does the scaler output reach a screen". The two agree on the
  ordinary machines - HDMI attached, or a CRT on the takeover - and part company on
  exactly the setup the LCD looks had to be fixed for: vga_scaler=1 with no HDMI,
  where the scaler is in the path and pointing straight at a tube.

  So it follows scaler_visible by default (HDMI_FOLLOW), which leaves every test
  written before this one saying what it always said, and can be pinned on its own
  to model that third machine.
*/
#define HDMI_FOLLOW (-2)
static int hdmi_connected = HDMI_FOLLOW;
void harness_set_hdmi_connected(int v) { hdmi_connected = v; }

int video_hdmi_connected() { return (hdmi_connected == HDMI_FOLLOW) ? scaler_visible : hdmi_connected; }
int video_scaler_is_visible() { return scaler_visible; }
void video_menu_bg(int, int) {}

/*
  Neo Geo romset naming. The real pair reads romsets.xml, and the contract the
  front-end has to honour is the return value: the title when the set is known,
  NULL when it is not - the shelf then shows the board name - and (char*)-1 when
  the file marks the set as one to hide.

  The fake table stands in for the xml so the harness does not need the loader.
*/
static int neo_scanned = 0;
int harness_neogeo_scanned() { return neo_scanned; }

int neogeo_scan_xml(char *path)
{
	printf("  [stub] neogeo_scan_xml(%s)\n", path);
	neo_scanned++;
	return 2;
}

char *neogeo_get_altname(char *path, char *name, char *altname)
{
	static const struct { const char *set, *title; } known[] =
	{
		{ "mslug",  "Metal Slug" },
		{ "kof98",  "The King of Fighters '98" },
	};

	if (!strcasecmp(altname, "neogeo")) return (char*)-1;   // BIOS set: hidden

	for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++)
	{
		if (!strcasecmp(altname, known[i].set)) return (char*)known[i].title;
	}
	return 0;
}

/*
  Models video_menu_fb_analog(). The part the UI has to cope with is that taking
  the scaler resizes the framebuffer to the TV mode, so the canvas shrinks under it
  mid-session; where the scaler output already reaches the screen it is a no-op.
*/
static int fb_analog = 0;
static int fb_analog_w = 0, fb_analog_h = 0;
int harness_fb_analog() { return fb_analog; }

/*
  Every *claim*, not the resulting state. The front-end re-asserts this every frame while
  it owns the screen, so on a setup where the scaler is already pointed at the TV the call
  is a no-op and the state tells you nothing - which is exactly the case where "the menu
  closed but the front-end carried on drawing" hides.
*/
static int fb_analog_claims = 0;
int harness_analog_claims() { return fb_analog_claims; }
void harness_reset_analog_claims() { fb_analog_claims = 0; }

void video_menu_fb_analog(int on)
{
	on = on ? 1 : 0;
	if (on) fb_analog_claims++;
	if (scaler_visible) return;
	if (on == fb_analog) return;

	fb_analog = on;
	if (on)
	{
		// The *mode*, not the canvas: with a half-resolution request in force the two
		// differ, and restoring the canvas as the mode would shrink the display by
		// the divisor every time the takeover bounced.
		fb_analog_w = fb_native_w;
		fb_analog_h = fb_native_h;
		harness_set_fb(320, 240);
		printf("  [stub] analog takeover: canvas now %dx%d\n", fbw, fbh);
	}
	else if (fb_analog_w)
	{
		harness_set_fb(fb_analog_w, fb_analog_h);
		printf("  [stub] analog released: canvas back to %dx%d\n", fb_analog_w, fb_analog_h);
	}
}

static char last_preset[1024] = {};
const char *harness_last_preset() { return last_preset; }
void harness_reset_preset() { last_preset[0] = 0; }

void video_loadPreset(char *name, bool save)
{
	snprintf(last_preset, sizeof(last_preset), "%s", name ? name : "");
	printf("  [stub] video_loadPreset(\"%s\", save=%d)\n", last_preset, save ? 1 : 0);
}

/* -------------------------------------------------------------- fake io --- */

/*
  The launch's file feed. 0 (idle) is also what the ARM answers after a restart
  into an already-loaded core, so idle is the right default here; a test that
  wants to model a launch in flight raises it and the look apply must then wait.
*/
static int mgl_busy = 0;
void harness_set_mgl_busy(int v) { mgl_busy = v; }
int menu_mgl_busy(void) { return mgl_busy; }

static char core_name[64] = "GAMEBOY";
void harness_set_core_name(const char *n) { snprintf(core_name, sizeof(core_name), "%s", n ? n : ""); }
char *user_io_get_core_name(int) { return core_name; }

static int in_menu_core = 1;
void harness_set_menu_core(int v) { in_menu_core = v; }
char is_menu() { return (char)in_menu_core; }

/*
  CONF_STR taken verbatim from the real Gameboy core on the hardware, because the
  idealised version this used to hold was why the harness passed while savestates
  and pause failed on the device. The awkward parts are all real: an "h3" hide
  prefix on the momentary entries, a "P3" *page* prefix on the pause option, the
  pause option being the OSD-watching preference rather than a command, the slot
  option living in the extended status word ("o01"), and a "Savestates to SDCard"
  option whose first value - the default - is On.
*/
static const char *fake_confstr[] =
{
	"GAMEBOY",
	"FS1,GBCGB BIN,Load ROM",
	"OEF,System,Auto,Gameboy,Gameboy Color,MegaDuck",
	"-",
	"OV,Savestates to SDCard,On,Off",
	"o01,Savestate Slot,1,2,3,4",
	"h3RS,Save state (Alt-F1)",
	"h3RT,Restore state (F1)",
	"-",
	"P3OQ,Pause when OSD is open,Off,On",
	"R0,Reset",
	0
};
/*
  The same core with its pause option removed - a NES or SNES, in other words. Those
  are the cores the front-end has to hold still with a state instead of pausing
  (freeze_engage), and so the only ones a copy-save can happen on. The table above
  pauses, so nothing using it reaches that path.
*/
static const char *fake_confstr_nopause[] =
{
	"GAMEBOY",
	"FS1,GBCGB BIN,Load ROM",
	"OEF,System,Auto,Gameboy,Gameboy Color,MegaDuck",
	"-",
	"OV,Savestates to SDCard,On,Off",
	"o01,Savestate Slot,1,2,3,4",
	"h3RS,Save state (Alt-F1)",
	"h3RT,Restore state (F1)",
	"-",
	"R0,Reset",
	0
};

/*
  A core that means something else by "Slot". MSX lists cartridge slots and Apple II
  expansion slots, and both were being adopted as the savestate selector - measured on the
  device. This one has savestates *as well*, and lists its unrelated slot option first, so
  a scanner that takes the first "slot" it sees picks the wrong one.
*/
static const char *fake_confstr_slotty[] =
{
	"SLOTTY",
	"FS1,ROM,Load ROM",
	"OGH,Cartridge Slot,Empty,Cart A,Cart B",
	"-",
	"OV,Savestates to SDCard,On,Off",
	"o01,Savestate Slot,1,2,3,4",
	"h3RS,Save state (Alt-F1)",
	"h3RT,Restore state (F1)",
	"R0,Reset",
	0
};

/*
  A core that pauses for real - the label says "Pause", not "Pause when OSD is open", so
  it is honoured whatever the OSD is doing. SMS is the first one Dinofly owns. Such a core is
  never frozen with a state (there is nothing to hold still), which is what makes saving
  into a slot a different path: the core has to be asked, and asked while it runs.
*/
static const char *fake_confstr_realpause[] =
{
	"REALPAUSE",
	"FS1,BIN,Load ROM",
	"-",
	"OH,Pause,Off,On",
	"o01,Savestate Slot,1,2,3,4",
	"h3RS,Save state (Alt-F1)",
	"h3RT,Restore state (F1)",
	"R0,Reset",
	0
};

/*
  A core with only TWO savestate slots - PSX, GBA and WonderSwan are all like this, where
  most cores offer four. The last slot is reserved to hold the game still, so such a core
  leaves the player exactly one.
*/
/*
  A core with a real options menu, shaped like the ones on the card rather than invented.

  Every awkwardness in here was taken from a real CONF_STR: pages defined separately from
  the options that reference them, a mask *before* the page prefix (which is how the N64
  publishes its VI group, and what a parser that assumes an order silently drops), the two
  bit-spec forms in one core, a value the core marks (U), and options the front-end owns
  and must not offer twice.
*/
static const char *fake_confstr_opts[] =
{
	"OPTCORE",
	"FS1,BIN,Load ROM",
	"P1,Audio & Video;",
	"P2,Debug settings;",
	"-",
	"O[38:37],Savestate Slot,1,2,3,4",          // ours: never offered
	"P1O[33:32],Aspect ratio,Original,Full Screen",  // ours: overlaps Display looks
	"P1OFH,Palette,Kitrinx,Smooth,Wavebeam",    // picture, legacy two-char spec
	"P1O[54:53],Widescreen Hack,Off,3:2,16:9",  // picture
	"D1P1O[35],VI Deblur,Original,On",          // picture, mask BEFORE page
	"D1P1O[36],VI Antialias,Original,Off",      // picture, mask before page
	"O[40:39],System Type,Auto,NTSC,PAL",       // system
	"O[80:79],Turbo(Cheats Off),Off,Low(U),High(U)",  // risky: the core says so
	"P2O[27:24],Cache Delay,0,1,2,3",           // debug page: never offered
	"T[0],Reset",                                // a trigger, not a setting
	"V,v1",
	0
};

/*
  The same core after an update that inserted a value into one of its lists.

  Cores do this - PSX's Widescreen Hack grew from two entries to four - and it moves
  every value after the insertion point by one. A per-game override stored as an index
  would come back as the wrong setting; stored as a name it still means what the
  player chose. Everything else here is identical to fake_confstr_opts so a test can
  swap one for the other and nothing else about the screen changes.
*/
static const char *fake_confstr_opts_v2[] =
{
	"OPTCORE",
	"FS1,BIN,Load ROM",
	"P1,Audio & Video;",
	"P2,Debug settings;",
	"-",
	"O[38:37],Savestate Slot,1,2,3,4",
	"P1O[33:32],Aspect ratio,Original,Full Screen",
	"P1OFH,Palette,Kitrinx,Smooth,Wavebeam",
	"P1O[54:53],Widescreen Hack,Off,3:2,5:3,16:9",   // 5:3 is the new one: 16:9 moved
	"D1P1O[35],VI Deblur,Original,On",
	"D1P1O[36],VI Antialias,Original,Off",
	"O[40:39],System Type,Auto,NTSC,PAL",
	"O[80:79],Turbo(Cheats Off),Off,Low(U),High(U)",
	"P2O[27:24],Cache Delay,0,1,2,3",
	"T[0],Reset",
	"V,v2",
	0
};

static const char *fake_confstr_twoslot[] =
{
	"TWOSLOT",
	"FS1,BIN,Load ROM",
	"-",
	"OV,Savestates to SDCard,On,Off",
	"o01,Savestate Slot,1,2",
	"h3RS,Save state (Alt-F1)",
	"h3RT,Restore state (F1)",
	"R0,Reset",
	0
};

/*
  A core that publishes options in BOTH bit-spec letter forms, which is the only shape
  that can catch a reader that drops the "O"/"o" distinction.

  Real cores do this constantly - the SMS lists Z80 Speed as "H8o8" and SMS BIOS as
  "H8oBC" while its other settings are "O..." - and the two forms address different
  status words: "o8" is bit 40, "O8" is bit 8. With only one form present the wrong
  bits are still self-consistent, so every read matches every write and the screen
  looks right; it is the collision between the two that shows the defect.

  Note the deliberate letter reuse: "O8" and "o8" share a spec string and differ only
  in ex, and "[41:40]" names in brackets the same bits "o8" reaches by ex - bracket
  specs are absolute and take no ex, which is why they were never affected.
*/
static const char *fake_confstr_optsex[] =
{
	"EXCORE",
	"FS1,BIN,Load ROM",
	"-",
	"O[9:8],System Type,Auto,NTSC,PAL",   // lower word, bracket form
	"O8,Region Lock,Off,On",              // lower word, letter form, bit 8
	"o8,Z80 Speed,Normal,Turbo",          // UPPER word, same letter, bit 40
	"o79,Mapper,Auto,Codemasters,Korea",  // upper word, multi-bit
	0
};

/*
  A core with more options than a 240p panel can hold, taken from the real PSX CONF_STR.

  This is the fixture the row-drop guard needs to mean anything. Every other core modelled
  here is short enough to fit at every profile, so a list that silently dropped its tail
  would pass the whole suite - which is exactly what happened: the PSX's System page is 27
  rows against roughly fifteen that fit, and every row past the fold was selectable and
  never drawn, on hardware, unnoticed by any test in this file.

  Trimmed to the settings rows: the file selectors, triggers and separators are not offered.

  Deliberately all on ONE page. The first attempt at this fixture used the PSX's real
  option *names*, and the curation in chome_core.cpp promptly sorted them into three tiers
  of about ten - so no single page overflowed and the fixture proved nothing. What makes the
  real PSX System page 27 rows is that most of its settings are named in system_tier[] or in
  no list at all, and both land in CO_TIER_SYSTEM. These do.
*/
static const char *fake_confstr_long[] =
{
	"PSXLONG",
	"FS1,CUECHD,Load CD",
	"O[40:39],System Type,Auto,NTSC-U,NTSC-J,PAL",
	/*
	  The SNAC rows, drawn rather than merely scanned.
	
	  Their footer help is the only copy of ours chosen by a *value* rather than by a screen,
	  and assert_no_clipped_copy() had never seen it: no fixture put one of these rows on the
	  options screen while it was being drawn, so three sentences could sit over the panel
	  width at 720p and 480p unnoticed. Keeping them here means the clip sweep measures them.
	*/
	"O[48:45],Pad1,Dualshock,Off,Digital,Analog,GunCon,NeGcon,Wheel-NegCon,"
		"Wheel-Analog,Mouse,Justifier,SNAC-port1,Analog Joystick,Popn",
	"O[52:49],Pad2,Dualshock,Off,Digital,Analog,GunCon,NeGcon,Wheel-NegCon,"
		"Wheel-Analog,Mouse,Justifier,SNAC-port2,Analog Joystick,Popn",
	"O[66],SNAC MemCard,Virtual,Real",
	"O[1],Video Region,Auto,NTSC,PAL",
	"O[2],TV System,Auto,NTSC,PAL",
	"O[3],Auto Region,Off,On",
	"O[4],Priority,Normal,High",
	"O[5],TMSS,Off,On",
	"O[7],Mapper,Auto,Codemasters,Korea",
	"O[8],SMS BIOS,Off,On",
	"O[10],GG BIOS,Off,On",
	"O[11],ROM Header,Auto,Ignore",
	"O[12],RAM Clear,Off,On",
	"O[13],PPU Reset Behavior,Off,On",
	"O[14],Initial WRAM,Zero,Random",
	"O[15],Initial ARAM,Zero,Random",
	"O[16],Audio Clock,Auto,NTSC,PAL",
	"O[17],Audio mode,Stereo,Mono",
	"O[18],Audio Enable,On,Off",
	"O[19],Audio Filter,On,Off",
	"O[20],FM Chip,YM2612,YM3438",
	"O[21],Stereo Mix,None,25%,50%",
	"O[22],Save Type,Auto,Off",
	"O[23],RTC,Off,On",
	"O[24],Fastboot,Off,On",
	"O[25],Sync core to video,Off,On",
	"O[26],Fixed Video Blanks,Off,On",
	"O[28],Sync 480i for HDMI,Off,On",
	"O[29],480i to 480p Hack,Off,On",
	"O[30],Disk Speed,Normal,Fast",
	0
};


/*
  The SMS's own shape, taken off the device: 6 rows on Picture, 16 on System & Sound, 1 on
  Risky, with a game running.

  fake_confstr_long is 28 rows on one page and scrolls correctly, which is exactly why it
  did not catch this: a list far longer than the panel takes the window past the cursor on
  the first press and everything works. The bug lives at a *particular* length - one where
  list_fit()'s answer and the number of rows the panel can really draw differ by one - and
  16+1 on a 240p panel is that length. Reported from hardware with a screenshot pair.
*/
static const char *fake_confstr_sms[] =
{
	"SMS",
	"FS1,BIN,Load ROM",
	"P1,Video & Sound;",
	"P2,System & Sound;",
	/* --- 6 that land on Picture --- */
	"P1O[1],Orientation,Normal,Rotate",
	"P1O[2],Flip Screen,Off,On",
	"P1O[3],Vertical Crop,Off,On",
	"P1O[4],Crop Offset,0,1,2",
	"P1O[5],Border,Off,On",
	"P1O[6],Masked Left Column,Off,On",
	/* --- 16 that land on System & Sound --- */
	"P2O[7],TV System,NTSC,PAL",
	"P2O[8],Region,US/EU,Japan",
	"P2O[9],SMS BIOS,Disable,Enable",
	"P2O[10],GG BIOS,Ext. File,Disable",
	"P2O[11],Mapper,Auto,Sega",
	"P2O[12],SMS FM Sound,Enable,Disable",
	"P2O[13],Swap Joysticks,No,Yes",
	"P2O[14],Multitap,Disabled,Port1",
	"P2O[15],Pause Btn Combo,Yes,No",
	"P2O[16],Gun Control,Disabled,Enabled",
	"P2O[17],Gun Fire,Joy,Mouse",
	"P2O[18],Gun Port,Port1,Port2",
	"P2O[19],Cross,Small,Big",
	"P2O[20],Paddle Control,Disabled,Enabled",
	"P2O[21],SK-1100,Off,On",
	"P2O[22],SC-3000,Off,On",
	/* --- 1 that lands on Risky --- */
	"P2O[23],Z80 Speed,Normal,Turbo",
	0
};

static int confstr_on = 1;

/*
  A table supplied by the test instead of one of the fixtures above.

  The SNAC arbitration has to be right for six real cores that spell the same thing two
  different ways, and adding six numbered fixtures to the ladder below - each used once -
  would bury the interesting part. Cleared by harness_set_confstr().
*/
static const char **confstr_custom = 0;
void harness_set_confstr_table(const char **tbl) { confstr_custom = tbl; }
void harness_set_confstr(int v) { confstr_on = v; confstr_custom = 0; }

/*
  An EMPTY entry returns 0, not "".

  That is what the firmware does - user_io.cpp:3094, `if (!len) return NULL;` - and the
  stub used to hand back the empty string instead. The difference is not academic: the
  scanner in chome_core.cpp treats 0 as "end of the config string" and "" as "nothing on
  this line, keep going", so a core that leaves an entry empty behaved one way on the
  device and the opposite way here. The Saturn core leaves index 1 empty, its options were
  therefore invisible in the front-end, and no test could fail because the stub never
  produced the condition. A fake that is kinder than the real thing hides exactly the bugs
  worth finding.
*/
char *user_io_get_confstr(int index)
{
	if (confstr_custom)
	{
		int n = 0;
		while (confstr_custom[n]) n++;
		if (index < 0 || index >= n) return 0;
		if (!confstr_custom[index][0]) return 0;
		return (char *)confstr_custom[index];
	}

	if (!confstr_on) return 0;

	const char **tbl = (confstr_on == 2) ? fake_confstr_nopause
		: (confstr_on == 3) ? fake_confstr_slotty
		: (confstr_on == 4) ? fake_confstr_realpause
		: (confstr_on == 5) ? fake_confstr_twoslot
		: (confstr_on == 6) ? fake_confstr_opts
		: (confstr_on == 7) ? fake_confstr_opts_v2
		: (confstr_on == 8) ? fake_confstr_optsex
		: (confstr_on == 9) ? fake_confstr_long
		: (confstr_on == 10) ? fake_confstr_sms : fake_confstr;
	int n = 0;
	while (tbl[n]) n++;

	if (index < 0 || index >= n) return 0;
	return (char *)tbl[index];
}

int substrcpy(char *d, const char *s, char idx)
{
	int field = 0;
	const char *p = s;
	while (*p && field < idx) { if (*p == ',') field++; p++; }
	if (field != idx) { d[0] = 0; return 0; }

	int i = 0;
	while (p[i] && p[i] != ',' && p[i] != ';' && i < 127) { d[i] = p[i]; i++; }
	d[i] = 0;
	return i;
}

/*
  Options round-trip through a little map rather than one hardcoded name, so a test
  can watch any of them. This used to key on "[40]" from an invented CONF_STR; the
  real core's pause option is "Q" behind a P3 page prefix, and its savestate-to-card
  option is "V", so hardcoding one name hid both.
*/
/*
  Big enough for the longest core modelled here, with room to spare.

  It was 24, and the 28-row fixture core walked straight past it - after which opt_slot()
  returned 0 for every new option, so writes were dropped and every read answered the
  default. That surfaced three sections later as the SNAC arbitration "failing" for all six
  cores, which is a long way from the cause. A silent cap on a fixture is the same class of
  defect as a silent cap on a list: harness_optmap_full() is asserted at the end of the run
  so the next one to hit it is told, rather than debugged.
*/
#define OPTMAP_MAX 96
/*
  Keyed on the spec AND on ex, because those two together are what identify an option.

  "o8" and "O8" are different bits - 40 and 8 - and arrive here as the same string with
  different ex, since the caller strips the letter that told them apart. A map keyed on
  the string alone answers both from one slot, which makes a reader that drops ex look
  perfectly consistent: it writes and reads the same wrong place. That is exactly the
  bug this fixture exists to catch, so the key has to carry ex.
*/
static struct { char opt[32]; int ex; uint32_t val; } optmap[OPTMAP_MAX];
static int noptmap = 0;
static int optmap_full = 0;

int harness_optmap_full() { return optmap_full; }

static uint32_t *opt_slot(const char *opt, int ex)
{
	if (!opt || !opt[0]) return 0;
	for (int i = 0; i < noptmap; i++)
		if (optmap[i].ex == !!ex && !strcmp(optmap[i].opt, opt)) return &optmap[i].val;
	if (noptmap >= OPTMAP_MAX) { optmap_full = 1; return 0; }
	snprintf(optmap[noptmap].opt, sizeof(optmap[noptmap].opt), "%s", opt);
	optmap[noptmap].ex = !!ex;
	optmap[noptmap].val = 0;
	return &optmap[noptmap++].val;
}

// The pause option of the modelled core (P3OQ) - "O" form, so ex is 0.
uint32_t harness_pause_val() { uint32_t *v = opt_slot("Q", 0); return v ? *v : 0; }
uint32_t harness_opt_val(const char *opt, int ex) { uint32_t *v = opt_slot(opt, ex); return v ? *v : 0; }
void harness_set_opt(const char *opt, uint32_t v, int ex) { uint32_t *p = opt_slot(opt, ex); if (p) *p = v; }

static char last_pulse_opt[64] = {};
const char *harness_last_pulse_opt() { return last_pulse_opt; }

/*
  Pulses counted per option. "the last thing pulsed" is not enough now that saving
  legitimately touches several options around the save itself - it takes pause off,
  pulses save, puts pause back - so the last write is not the interesting one.
*/
static struct { char opt[32]; int n; } pulses[OPTMAP_MAX];
static int npulses = 0;

int harness_pulses_on(const char *opt)
{
	for (int i = 0; i < npulses; i++) if (!strcmp(pulses[i].opt, opt)) return pulses[i].n;
	return 0;
}

static void note_pulse(const char *opt)
{
	for (int i = 0; i < npulses; i++)
	{
		if (!strcmp(pulses[i].opt, opt)) { pulses[i].n++; return; }
	}
	if (npulses >= OPTMAP_MAX) return;
	snprintf(pulses[npulses].opt, sizeof(pulses[npulses].opt), "%s", opt);
	pulses[npulses++].n = 1;
}

static char last_status_opt[64] = {};
static uint32_t last_status_val = 0;
static int status_pulses = 0;

const char *harness_last_status_opt() { return last_status_opt; }
int harness_status_pulses() { return status_pulses; }
void harness_reset_status() { last_status_opt[0] = 0; last_pulse_opt[0] = 0; status_pulses = 0; npulses = 0; }

/*
  The Display looks push a .gbp palette at the core's file slot. The harness
  only needs to know it happened - and that it never happens on a core without
  a palette slot, which a test asserts through harness_last_file_tx().
*/
static char last_file_tx[512] = {};
static int last_file_tx_idx = -1;
const char *harness_last_file_tx() { return last_file_tx; }
int harness_last_file_tx_idx() { return last_file_tx_idx; }
void harness_reset_file_tx() { last_file_tx[0] = 0; last_file_tx_idx = -1; }

int user_io_file_tx(const char *name, unsigned char index, char, char, char, uint32_t)
{
	snprintf(last_file_tx, sizeof(last_file_tx), "%s", name ? name : "");
	last_file_tx_idx = index;
	printf("  [stub] user_io_file_tx(\"%s\", %u)\n", last_file_tx, index);
	return 1;
}

/*
  The liveness poll the firmware sends the running core mid-computation. On the
  device it is one SPI word that keeps the PSX's savestate machine from deciding
  the HPS has wandered off; here there is no core, so it counts calls - a long
  computation that forgets to pump it is a bug this can see.
*/
static int alive_polls = 0;
void user_io_core_alive_poll() { alive_polls++; }
int harness_alive_polls() { return alive_polls; }
void harness_reset_alive_polls() { alive_polls = 0; }

void user_io_status_set(const char *opt, uint32_t value, int ex)
{
	snprintf(last_status_opt, sizeof(last_status_opt), "%s", opt ? opt : "");
	last_status_val = value;
	uint32_t *slot = opt_slot(opt, ex);
	if (slot) *slot = value;
	if (value)
	{
		status_pulses++;
		snprintf(last_pulse_opt, sizeof(last_pulse_opt), "%s", opt ? opt : "");
		if (opt) note_pulse(opt);
	}
	printf("  [stub] user_io_status_set(\"%s\", %u)\n", last_status_opt, value);
}

/*
  A game is not a still. Every grab has to differ from the last, or a whole class of
  bug is invisible here: two saves into one slot wrote *identical pixels*, so a tile
  that never updated its picture looked exactly like one that did. The counter is what
  makes "the moment" mean something - it stands in for the game having moved on.
*/
static unsigned grab_seq = 0;
void harness_reset_grab_seq() { grab_seq = 0; }

// See harness_set_grab() in harness.h for what a refusing grab is for here.
static int grab_ok = 1;
void harness_set_grab(int ok) { grab_ok = ok; }

/*
  A still of one flat colour instead of the drawn scene - see harness_set_grab_flat().

  Deliberately identical between grabs, which is the opposite of what the picture below is
  for: this one is not asking whether a frame moved, it is asking which pixels on screen
  came out of the still at all, and that needs a colour nothing else in the front-end draws.
*/
static uint32_t grab_flat = 0;
void harness_set_grab_flat(uint32_t argb) { grab_flat = argb; }

static const char *grab_why = "not attempted";
/*
  The scaler's header, which the grid generator reads to learn how many output
  pixels each source pixel is getting. There is no fabric here, so it answers
  "nothing running" and the generator falls back to its fixed gutter - the
  behaviour every look had before the scale-aware grid, which is what the
  fingerprinted screens in this suite were drawn with.

  harness_set_scale() lets a test say otherwise, so the scale-aware shape can be
  asserted without a device.
*/
static int scaler_src_h = 0, scaler_out_h = 0;

void harness_set_scale(int src_h, int out_h) { scaler_src_h = src_h; scaler_out_h = out_h; }

mister_scaler *mister_scaler_init()
{
	if (scaler_src_h < 1) return 0;

	static mister_scaler ms;
	memset(&ms, 0, sizeof(ms));
	ms.height = scaler_src_h;
	ms.output_height = scaler_out_h;
	ms.width = scaler_src_h * 4 / 3;
	ms.output_width = scaler_out_h * 4 / 3;
	return &ms;
}

int mister_scaler_read(mister_scaler *, unsigned char *, mister_scaler_format_t) { return -1; }
void mister_scaler_free(mister_scaler *) {}

const char *screenshot_grab_why(void) { return grab_why; }

// Attempts, not successes: the blank-frame retry in ig_open() is visible only
// as the same call being made again.
static int grab_calls = 0;
int harness_grab_calls() { return grab_calls; }
void harness_reset_grab_calls() { grab_calls = 0; }

int screenshot_grab(uint32_t *dst, int max_px, int *out_w, int *out_h)
{
	grab_calls++;
	int w = 320, h = 240;
	if (!grab_ok) { grab_why = "the harness was told to refuse"; return 0; }
	if (w * h > max_px) { grab_why = "the frame is larger than the buffer offered"; return 0; }
	grab_why = "ok";

	if (grab_flat)
	{
		for (int i = 0; i < w * h; i++) dst[i] = grab_flat;
		*out_w = w; *out_h = h;
		return 1;
	}

	// Colour derived from whatever is "running", so different games look different.
	uint32_t seed = 0x9e3779b9u;
	{
		FILE *f = fopen("/tmp/classicui_current", "rt");
		char buf[512] = {};
		if (f) { if (fread(buf, 1, sizeof(buf) - 1, f)) {} fclose(f); }
		for (const char *p = buf; *p; p++) { seed ^= (unsigned char)*p; seed *= 16777619u; }
	}
	/*
	  What moves between two grabs is a sprite, not the palette. That matters: the same
	  scene in the same colours a few pixels apart compresses to the same number of bytes,
	  which is exactly the case the picture cache cannot tell from a stat. Recolouring the
	  sky instead would change the file size and let a broken cache look fixed.
	*/
	++grab_seq;
	int sx = (int)(grab_seq * 17u % 60u);

	uint32_t sky = 0xff000000u | ((seed >> 8) & 0x3f3f7f);
	uint32_t ground = 0xff000000u | ((seed >> 16) & 0x2f5f2f);

	for (int y = 0; y < h; y++)
	{
		for (int x = 0; x < w; x++)
		{
			uint32_t c = sky;
			if (y > h * 2 / 3) c = ground;
			else if (((x / 16) + (y / 16)) % 7 == 0) c |= 0x202020;
			if (x > w / 3 + sx && x < w / 2 + sx && y > h / 3 && y < h * 2 / 3) c = 0xffe08040;
			dst[y * w + x] = c;
		}
	}

	*out_w = w; *out_h = h;
	return 1;
}

void fpga_load_rbf_stub_marker() {}
static char last_rbf[256] = {};
const char *harness_last_rbf() { return last_rbf; }

int fpga_load_rbf(const char *name, const char *, const char *)
{
	snprintf(last_rbf, sizeof(last_rbf), "%s", name ? name : "");
	printf("  [stub] fpga_load_rbf(\"%s\")\n", last_rbf);

	if (real_clock && strstr(last_rbf, "menu.rbf"))
	{
		in_menu_core = 1;
		printf("  [viewer] back in the menu core\n");
	}
	return 0;
}
uint32_t user_io_status_get(const char *opt, int ex)
{
	uint32_t *slot = opt_slot(opt, ex);
	return slot ? *slot : 0;
}
/*
  The real bit-spec parser, copied from user_io.cpp rather than faked.

  It used to answer "bit 1, one bit wide" to everything, which was enough while nothing here
  cared where an option lived. Handing one option to the whole system does care: it edits
  <CORE>.CFG in place, and the only thing that says which bits of that file belong to the
  option is this function. With the old stub every option would have written bit 1 and the
  test would have proved nothing about the placement - or worse, would have passed while the
  device wrote the wrong setting.

  Both spec forms are here because the fixture core publishes both, as real cores do:
  "[54:53]" for Widescreen Hack and the legacy pair "FH" for Palette.
*/
int user_io_status_bits(const char *opt, int *s, int *e, int ex, int single)
{
	uint32_t start = 0, end = 0;
	if (opt[0] == '[')
	{
		if (!single && sscanf(opt, "[%u:%u]", &end, &start) == 2)
		{
			if (start > 127 || end > 127 || end <= start) return 0;
		}
		else if (sscanf(opt, "[%u]", &start) == 1)
		{
			if (start > 127) return 0;
			end = start;
		}
		else return 0;
	}
	else
	{
		if ((opt[0] >= '0') && (opt[0] <= '9')) start = opt[0] - '0';
		else if ((opt[0] >= 'A') && (opt[0] <= 'V')) start = opt[0] - 'A' + 10;
		else return 0;

		if (!single && (opt[1] >= '0') && (opt[1] <= '9')) end = opt[1] - '0';
		else if (!single && (opt[1] >= 'A') && (opt[1] <= 'V')) end = opt[1] - 'A' + 10;
		else
		{
			single = 1;
			end = start;
		}

		if (ex)
		{
			start += 32;
			end += 32;
		}

		if (start > 127 || end > 127 || (!single && end <= start)) return 0;
	}

	if (end - start > 8) return 0;

	if (s) *s = (int)start;
	if (e) *e = (int)end;
	return 1 + end - start;
}
uint32_t user_io_status_mask(const char *) { return 3; }

void OsdEnable(unsigned char) {}
void OsdDisable() {}
void OsdMenuCtl(int) {}

void open_joystick_setup() { printf("  [stub] open_joystick_setup()\n"); }

// Which device the harness/viewer is pretending to be.
static int from_pad = 1;
void harness_set_input_pad(int v) { from_pad = v; }
int input_menu_key_from_pad() { return from_pad; }

/*
  The controllers MiSTer would have assigned players to. Settable, because the point of
  the Controllers screen is that it shows wired pads as well as wireless ones, and a
  container has neither.
*/
static pad_info fake_pads[8];
static int n_fake_pads = 0;

void harness_clear_pads() { n_fake_pads = 0; }

void harness_add_pad(int player, int kind, uint16_t vid, uint16_t pid, const char *name, const char *mac)
{
	if (n_fake_pads >= (int)(sizeof(fake_pads) / sizeof(fake_pads[0]))) return;

	pad_info *p = &fake_pads[n_fake_pads++];
	memset(p, 0, sizeof(*p));
	p->player = player;
	p->kind = kind;
	p->vid = (uint16_t)vid;
	p->pid = (uint16_t)pid;
	snprintf(p->name, sizeof(p->name), "%s", name ? name : "");
	snprintf(p->mac, sizeof(p->mac), "%s", mac ? mac : "");
}

int input_pad_list(pad_info *out, int max)
{
	int n = 0;
	for (int i = 0; i < n_fake_pads && n < max; i++) out[n++] = fake_pads[i];
	return n;
}

/*
  What the real one reads off a live device. There is no device here, so the harness
  writes the state a pad would be in and the tester is asked to draw it - which is the
  only part of the tester that can be checked without a hand on a controller.
*/
static pad_state fake_state[8];
static int fake_state_on[8];

void harness_clear_pad_state()
{
	memset(fake_state, 0, sizeof(fake_state));
	memset(fake_state_on, 0, sizeof(fake_state_on));
}

void harness_set_pad_state(int player, uint32_t held, const uint16_t *codes, int sticks, int lx, int ly)
{
	if (player < 1 || player > 8) return;

	pad_state *st = &fake_state[player - 1];
	memset(st, 0, sizeof(*st));
	st->held = held;
	st->sticks = sticks;
	st->lx = lx;
	st->ly = ly;
	if (codes) for (int i = 0; i < PAD_STATE_BTNS; i++) st->code[i] = codes[i];

	fake_state_on[player - 1] = 1;
}

int input_pad_state(int player, pad_state *out)
{
	if (!out) return 0;
	memset(out, 0, sizeof(*out));
	if (player < 1 || player > 8 || !fake_state_on[player - 1]) return 0;

	*out = fake_state[player - 1];
	return 1;
}

/*
  The real one copies one savestate slot's DDR buffer onto another. There is no DDR here,
  so it records the pair and succeeds - what the tests care about is that the front-end
  asks, because copying only the file leaves a slot that loads nothing.
*/
static int ss_copy_from = -1, ss_copy_to = -1;

int harness_ss_copy_from() { return ss_copy_from; }
int harness_ss_copy_to()   { return ss_copy_to; }

// A second copy has to be told from the one before it, or a test that saves twice
// passes on the first save's record.
void harness_reset_ss_copy() { ss_copy_from = ss_copy_to = -1; }

int user_io_ss_copy_slot(int from, int to)
{
	printf("  [stub] user_io_ss_copy_slot(%d -> %d)\n", from, to);
	ss_copy_from = from;
	ss_copy_to = to;
	return 1;
}

// The real one reads the FPGA scaler buffer; the harness has no core running.
int screenshot_thumbnail(const char *fullpath, int max_w)
{
	printf("  [stub] screenshot_thumbnail(\"%s\", %d)\n", fullpath ? fullpath : "", max_w);
	return 0;
}

/*
  This one is handed the pixels rather than reading the scaler, so the harness does the
  real thing: a PNG of those pixels, scaled as asked. It writes a decodable image rather
  than a marker file because the front-end reads these back through art_thumb() - a slot's
  picture is one of these - so a test can compare what a tile shows against what was
  written, not merely that something was.
*/
bool write_screenshot(const char *filename, const uint8_t *argb,
	int width, int height, int output_width, int output_height)
{
	printf("  [stub] write_screenshot(\"%s\", %p, %dx%d -> %dx%d)\n",
		filename ? filename : "", (const void *)argb, width, height, output_width, output_height);

	if (!filename || !argb || width < 1 || height < 1) return false;

	// imlib works on its own copy: the caller's buffer is a live screen grab.
	uint32_t *copy = (uint32_t*)malloc((size_t)width * height * 4);
	if (!copy) return false;
	memcpy(copy, argb, (size_t)width * height * 4);
	for (int i = 0; i < width * height; i++) copy[i] |= 0xff000000u;

	Imlib_Image im = imlib_create_image_using_data(width, height, (DATA32*)copy);
	if (!im) { free(copy); return false; }

	Imlib_Image out = im;
	if (output_width > 0 && output_height > 0)
	{
		imlib_context_set_image(im);
		out = imlib_create_cropped_scaled_image(0, 0, width, height, output_width, output_height);
		if (!out) out = im;
	}

	// Asked for the error rather than stat'ing afterwards: these are written over a file
	// that is usually already there, so "a file exists" says nothing about this write.
	Imlib_Load_Error err = IMLIB_LOAD_ERROR_NONE;
	imlib_context_set_image(out);
	imlib_image_set_has_alpha(0);
	imlib_image_set_format("png");
	imlib_save_image_with_error_return(filename, &err);
	if (out != im) imlib_free_image();

	imlib_context_set_image(im);
	imlib_free_image();
	free(copy);

	return (err == IMLIB_LOAD_ERROR_NONE);
}

static char last_launch[1024] = {};

const char *harness_last_launch() { return last_launch; }
void harness_clear_launch() { last_launch[0] = 0; }

/*
  Launching is simulated rather than refused, so the viewer can walk the whole
  flow: pick a game, "run" it, then open the menu over it. CURRENT_FILE is what the
  real code writes before a core switch, so what the in-game path reads back here
  is exactly the identity it would find on hardware.
*/
static int sim_launch = 0;
int harness_sim_launched() { return sim_launch; }

int xml_load(const char *xml)
{
	snprintf(last_launch, sizeof(last_launch), "%s", xml ? xml : "");
	printf("  [stub] xml_load(\"%s\")\n", last_launch);

	if (real_clock)
	{
		in_menu_core = 0;
		sim_launch++;
		printf("  [viewer] now pretending to run that game - press M for the menu\n");
	}
	return 0;
}

/* ------------------------------------------------------------------ audio ---
  The volume register, only as far as mute: enough to tell whether the menu
  silenced the core and gave the sound back afterwards.
*/
static int muted = 0;
static int mute_changes = 0;

void audio_mute(int on)
{
	if (muted != !!on) mute_changes++;
	muted = !!on;
}

int audio_is_muted() { return muted; }

/* ------------------------------------------------------- the input device ---
  Which controller the last menu key came from, and what it has mapped to the menu
  buttons. Enough of it to check that the prompts follow the controller.
*/
static char pad_name[128] = "Generic USB Gamepad";
static uint16_t pad_mmap[12] = { 0, 0, 0, 0, 0x131, 0x130, 0x133, 0x134, 0x136, 0x137, 0x13A, 0x13B };

/*
  The classic OSD, as far as the front-end can see it. Core Settings closes our menu and
  asks for the OSD; the harness only needs to agree about who owns the menu button, so
  a flag is the whole model - the OSD itself is not drawn here.
*/
static int osd_visible = 0;
static unsigned int last_menu_key = 0;
char user_io_osd_is_visible() { return (char)osd_visible; }

/*
  MiSTer's own recents list. The harness records the last thing offered to it, which is
  enough to assert that a launch from this front-end reaches the firmware's list at all -
  the point of the interop. What the firmware then does with it is recent.cpp's business.
*/
static char last_recent[512] = {};
static int recent_calls = 0;
void recent_update(char *dir, char *path, char *label, int)
{
	snprintf(last_recent, sizeof(last_recent), "%s|%s|%s",
		dir ? dir : "", path ? path : "", label ? label : "");
	recent_calls++;
}
const char *harness_last_recent() { return last_recent; }
int harness_recent_calls() { return recent_calls; }

/*
  Holding OSD_STATUS into the core with no overlay on screen. The harness records only what
  the front-end asked for, which is enough to assert that a core with an OSD-tied pause is
  now used rather than written off.

  What it cannot check, and no test here can: that the SPI sequence in OsdStatusHold()
  really keeps osd_status high. That depends on sys_top wiring only vga_osd's status to the
  core and on EnableOsd_on() selecting the instance - both read in the fabric source, and
  neither reachable from a stub. Replacing that sequence with the old OSD_ALL pair leaves
  every check here passing. It has to be confirmed on hardware: a paused game visibly
  stops, and nothing in a log will say so.
*/
static int osd_hold = 0;
void OsdStatusHold(int on) { osd_hold = on ? 1 : 0; }
int harness_osd_status_held() { return osd_hold; }

/*
  Whether the classic menu is on screen. Distinct from user_io_osd_is_visible(), which is
  about key handling and which the front-end sets itself - the two were confused once, and
  it stopped the in-game menu opening on real hardware.
*/
int menu_present() { return osd_visible; }
/*
  Queues a key for the classic menu - and deliberately does NOT make the menu appear.

  It used to set osd_visible here, which flattered the front-end: the gap between asking for
  the OSD and the OSD actually being on screen vanished, and that gap is exactly where the
  Core Settings handoff went wrong on hardware. A test written against the old stub could
  not tell a correct two-step handoff from a broken one-step one - I checked, by sabotaging
  the fix and watching every check still pass. Tests raise osd_visible themselves now, when
  they mean it.
*/
void menu_key_set(unsigned int c) { last_menu_key = c; }
void harness_set_osd_visible(int v) { osd_visible = v ? 1 : 0; }
unsigned int harness_last_menu_key() { return last_menu_key; }

/*
  The core side of an options screen: a hide/disable mask, the config file it would be
  written to, and the status words. The mask is what makes the conditional half testable -
  a core recomputes it as options change, and the front-end has to re-read rather than
  cache.
*/
static uint16_t osd_mask = 0;
static int cfg_saves = 0;
void harness_set_osd_mask(uint16_t m) { osd_mask = m; }
int harness_cfg_saves() { return cfg_saves; }

uint16_t spi_uio_cmd16(uint8_t cmd, uint16_t)
{
	if (cmd == UIO_GET_OSDMASK) return osd_mask;
	return 0;
}

/* ------------------------------------------------- the SNAC pad reader ---- */

/*
  Enough of psx_snac_pad.sv for snacpad.cpp to talk to.

  It answers the eight-word conversation the poll actually has: a magic word, then a
  presence/id word and three data words per port. Only the shape matters here - what is
  being tested is the firmware's arbitration and its ID check, not the fabric's protocol -
  so the pad state is whatever a test set, and the buttons arrive already de-inverted
  because the RTL does that inversion before the firmware sees them.

  snac_reader_present models a core built from an older sys with no reader at all, which
  answers without the magic and must not be mistaken for a port with nothing on it.
*/
static int snac_reader_present = 1;
static int snac_port_present[2] = { 0, 0 };
static uint8_t snac_port_id[2] = { 0x41, 0x41 };
static uint16_t snac_port_btns[2] = { 0, 0 };
static int snac_last_want = -1;

void harness_set_snac_reader(int present) { snac_reader_present = present; }
void harness_set_snac_pad(int port, int present, uint8_t id, uint16_t btns)
{
	if (port < 0 || port > 1) return;
	snac_port_present[port] = present;
	snac_port_id[port] = id;
	snac_port_btns[port] = btns;
}
int harness_snac_last_want() { return snac_last_want; }
void harness_reset_snac()
{
	snac_reader_present = 1;
	snac_port_present[0] = snac_port_present[1] = 0;
	snac_port_id[0] = snac_port_id[1] = 0x41;
	snac_port_btns[0] = snac_port_btns[1] = 0;
	snac_last_want = -1;
}

#define SNAC_MAGIC_STUB 0x4A
static int snac_word = -1;

uint16_t spi_uio_cmd_cont(uint16_t cmd)
{
	if (cmd == UIO_SNAC_PAD)
	{
		snac_word = 0;
		return snac_reader_present ? (uint16_t)(SNAC_MAGIC_STUB << 8) : 0;
	}
	snac_word = -1;
	return 0;
}

/*
  fpga_spi() rather than spi_w(), which is inline in spi.h and only forwards to this.
  Stubbing the lower one keeps the real spi_w in the tested path.
*/
uint16_t fpga_spi(uint16_t v)
{
	if (snac_word < 0) return 0;

	int w = snac_word++;

	// Word 0 carries the enable bit down and the port-1 presence/id back.
	if (w == 0) snac_last_want = (v & 1) ? 1 : 0;

	int port = (w < 4) ? 0 : 1;
	switch (w & 3)
	{
	case 0:
		return (uint16_t)((snac_port_present[port] ? 0x8000 : 0) | snac_port_id[port]);
	case 1: return snac_port_btns[port];
	default: return 0x8080;   // sticks centred
	}
}

void DisableIO() { snac_word = -1; }

uint32_t user_io_hd_mask(const char *opt)
{
	int start = 0;
	if (!user_io_status_bits(opt, &start, 0, 0, 1)) return 0;
	return start;
}

char *user_io_create_config_name(int)
{
	static char n[32];
	snprintf(n, sizeof(n), "%s.CFG", core_name);
	return n;
}

int user_io_status_save(const char *)
{
	cfg_saves++;
	return 1;
}

const char *input_menu_key_devname() { return pad_name; }

/*
  Zero unless a test sets one. A pad with no USB identity is the case where the name
  has to carry the layout, which is what most of these tests are about - so the
  vendor lookup must fall through by default or it would mask them.
*/
static uint32_t pad_vidpid = 0;
uint32_t input_menu_key_vidpid() { return pad_vidpid; }
void harness_set_pad_vidpid(uint32_t v) { pad_vidpid = v; }

uint16_t input_menu_key_btn(int sys_btn)
{
	if (sys_btn < 0 || sys_btn >= (int)(sizeof(pad_mmap) / sizeof(pad_mmap[0]))) return 0;
	return pad_mmap[sys_btn];
}

void harness_set_pad_name(const char *n) { snprintf(pad_name, sizeof(pad_name), "%s", n ? n : ""); }

void harness_swap_pad_faces()
{
	// A pad the player has remapped: circle and cross the other way round.
	uint16_t t = pad_mmap[4];
	pad_mmap[4] = pad_mmap[5];
	pad_mmap[5] = t;
}

int harness_muted() { return muted; }
int harness_mute_changes() { return mute_changes; }
void harness_set_muted(int v) { muted = !!v; mute_changes = 0; }

/*
  fileTYPE's three members, which live in file_io.cpp and are the only thing in that whole
  file this harness needs.

  It needs them because cd_track_t carries a fileTYPE and toc_t carries a hundred of them,
  so declaring a table of contents on the stack - which the rip tests do, to drive the
  ripper off a disc nobody owns - instantiates them. Nothing else here has ever built one.

  Linking file_io.cpp instead was the alternative and is the wrong trade: it pulls in the
  zip reader, the SD-card path resolution and the firmware's whole notion of a root
  directory, none of which the harness wants and all of which it already fakes elsewhere.
  These three are copies of the real ones bar the FileClose() in the destructor, which
  cannot run here and has nothing to close: a toc_t built by a test has no open files in it,
  because the ripper never opens one - it reads through an injected reader and writes with
  stdio.
*/
fileTYPE::fileTYPE()
{
	filp = 0;
	mode = 0;
	type = 0;
	zip = 0;
	size = 0;
	offset = 0;
}

fileTYPE::~fileTYPE() {}

int fileTYPE::opened() { return filp || zip; }
