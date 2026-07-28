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

void harness_advance(unsigned long ms) { vclock += ms; }
unsigned long harness_now() { return vclock; }

unsigned long GetTimer(unsigned long offset) { return vclock + offset; }
unsigned long CheckTimer(unsigned long t) { return (!t) || (vclock >= t); }

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

void video_menu_fb_present(int n)
{
	presented = n;
	present_count++;
}

int video_fb_state() { return 0; }

static int scaler_visible = 1;
void harness_set_scaler_visible(int v) { scaler_visible = v; }
int video_hdmi_connected() { return scaler_visible; }
int video_scaler_is_visible() { return scaler_visible; }
void video_menu_bg(int, int) {}

static char last_preset[1024] = {};
const char *harness_last_preset() { return last_preset; }

void video_loadPreset(char *name, bool save)
{
	snprintf(last_preset, sizeof(last_preset), "%s", name ? name : "");
	printf("  [stub] video_loadPreset(\"%s\", save=%d)\n", last_preset, save ? 1 : 0);
}

/* -------------------------------------------------------------- fake io --- */

char is_menu() { return 1; }
uint32_t user_io_status_get(const char *, int) { return 0; }

void OsdEnable(unsigned char) {}
void OsdDisable() {}
void OsdMenuCtl(int) {}

void open_joystick_setup() { printf("  [stub] open_joystick_setup()\n"); }

// The real one reads the FPGA scaler buffer; the harness has no core running.
int screenshot_thumbnail(const char *fullpath, int max_w)
{
	printf("  [stub] screenshot_thumbnail(\"%s\", %d)\n", fullpath ? fullpath : "", max_w);
	return 0;
}

static char last_launch[1024] = {};

const char *harness_last_launch() { return last_launch; }
void harness_clear_launch() { last_launch[0] = 0; }

int xml_load(const char *xml)
{
	snprintf(last_launch, sizeof(last_launch), "%s", xml ? xml : "");
	printf("  [stub] xml_load(\"%s\")\n", last_launch);
	return 0;
}
