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

#include "../../../cfg.h"
#include "../../../video.h"
#include "../../../osd.h"
#include "../../../hardware.h"
#include "../../../menu.h"
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

int findGamesDir(char *dir, size_t dir_len)
{
	char probe[1024];
	struct stat st;

	snprintf(probe, sizeof(probe), "%s/games/%s", fake_root, dir);
	if (!stat(probe, &st) && S_ISDIR(st.st_mode)) { snprintf(dir, dir_len, "%s", probe); return 1; }

	snprintf(probe, sizeof(probe), "%s/%s", fake_root, dir);
	if (!stat(probe, &st) && S_ISDIR(st.st_mode)) { snprintf(dir, dir_len, "%s", probe); return 1; }

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
static int confstr_on = 1;
void harness_set_confstr(int v) { confstr_on = v; }

char *user_io_get_confstr(int index)
{
	if (!confstr_on) return 0;
	int n = (int)(sizeof(fake_confstr) / sizeof(fake_confstr[0])) - 1;
	if (index < 0 || index >= n) return 0;
	return (char *)fake_confstr[index];
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

	uint32_t sky = 0xff000000u | ((seed >> 8) & 0x3f3f7f);
	uint32_t ground = 0xff000000u | ((seed >> 16) & 0x2f5f2f);

	for (int y = 0; y < h; y++)
	{
		for (int x = 0; x < w; x++)
		{
			uint32_t c = sky;
			if (y > h * 2 / 3) c = ground;
			else if (((x / 16) + (y / 16)) % 7 == 0) c |= 0x202020;
			if (x > w / 3 && x < w / 2 && y > h / 3 && y < h * 2 / 3) c = 0xffe08040;
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

// The real one reads the FPGA scaler buffer; the harness has no core running.
int screenshot_thumbnail(const char *fullpath, int max_w)
{
	printf("  [stub] screenshot_thumbnail(\"%s\", %d)\n", fullpath ? fullpath : "", max_w);
	return 0;
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
