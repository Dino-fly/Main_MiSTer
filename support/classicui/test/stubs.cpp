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
#include "../../../osd.h"
#include "../../../hardware.h"
#include "../../../menu.h"
#include "../../../input.h"
#include "../../../user_io.h"
#include "../../../spi.h"
#include "../../arcade/mra_loader.h"

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

/* ---------------------------------------------------- fake framebuffers --- */

static int fb_supported = 1;
void harness_set_fb_supported(int v) { fb_supported = v; }

static uint32_t *fb[3] = {};
static int fbw = 1280, fbh = 720;
static int presented = 1;
static int present_count = 0;

void harness_set_fb(int w, int h)
{
	for (int i = 1; i <= 2; i++)
	{
		free(fb[i]);
		fb[i] = (uint32_t*)calloc((size_t)w * h, 4);
	}
	fbw = w;
	fbh = h;
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
int video_hdmi_connected() { return scaler_visible; }
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
		fb_analog_w = fbw;
		fb_analog_h = fbh;
		harness_set_fb(320, 240);
		printf("  [stub] analog takeover: canvas now 320x240\n");
	}
	else if (fb_analog_w)
	{
		harness_set_fb(fb_analog_w, fb_analog_h);
		printf("  [stub] analog released: canvas back to %dx%d\n", fb_analog_w, fb_analog_h);
	}
}

static char last_preset[1024] = {};
const char *harness_last_preset() { return last_preset; }

void video_loadPreset(char *name, bool save)
{
	snprintf(last_preset, sizeof(last_preset), "%s", name ? name : "");
	printf("  [stub] video_loadPreset(\"%s\", save=%d)\n", last_preset, save ? 1 : 0);
}

/* -------------------------------------------------------------- fake io --- */

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

static int confstr_on = 1;
void harness_set_confstr(int v) { confstr_on = v; }

char *user_io_get_confstr(int index)
{
	if (!confstr_on) return 0;

	const char **tbl = (confstr_on == 2) ? fake_confstr_nopause
		: (confstr_on == 3) ? fake_confstr_slotty
		: (confstr_on == 4) ? fake_confstr_realpause
		: (confstr_on == 5) ? fake_confstr_twoslot
		: (confstr_on == 6) ? fake_confstr_opts
		: (confstr_on == 7) ? fake_confstr_opts_v2 : fake_confstr;
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
#define OPTMAP_MAX 16
static struct { char opt[32]; uint32_t val; } optmap[OPTMAP_MAX];
static int noptmap = 0;

static uint32_t *opt_slot(const char *opt)
{
	if (!opt || !opt[0]) return 0;
	for (int i = 0; i < noptmap; i++) if (!strcmp(optmap[i].opt, opt)) return &optmap[i].val;
	if (noptmap >= OPTMAP_MAX) return 0;
	snprintf(optmap[noptmap].opt, sizeof(optmap[noptmap].opt), "%s", opt);
	optmap[noptmap].val = 0;
	return &optmap[noptmap++].val;
}

// The pause option of the modelled core (P3OQ).
uint32_t harness_pause_val() { uint32_t *v = opt_slot("Q"); return v ? *v : 0; }
uint32_t harness_opt_val(const char *opt) { uint32_t *v = opt_slot(opt); return v ? *v : 0; }
void harness_set_opt(const char *opt, uint32_t v) { uint32_t *p = opt_slot(opt); if (p) *p = v; }

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

void user_io_status_set(const char *opt, uint32_t value, int)
{
	snprintf(last_status_opt, sizeof(last_status_opt), "%s", opt ? opt : "");
	last_status_val = value;
	uint32_t *slot = opt_slot(opt);
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

int screenshot_grab(uint32_t *dst, int max_px, int *out_w, int *out_h)
{
	int w = 320, h = 240;
	if (w * h > max_px) return 0;

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
uint32_t user_io_status_get(const char *opt, int)
{
	uint32_t *slot = opt_slot(opt);
	return slot ? *slot : 0;
}
int user_io_status_bits(const char *, int *st, int *, int, int) { if (st) *st = 1; return 1; }
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
