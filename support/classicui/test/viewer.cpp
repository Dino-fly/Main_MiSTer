/*
  Classic Home interactive viewer.

  Runs the real front-end against a fake SD card and serves it to a browser, so the
  UI can be driven by hand on a laptop with no MiSTer attached. The five
  chome_*.cpp files are the same ones that build for the DE10-Nano; only the
  platform is faked (see stubs.cpp).

  Launching a game is simulated rather than refused: the viewer flips itself into
  "game core" mode with a synthetic game picture, so the in-game menu, save states
  and Close Game can all be exercised end to end.

  Start it with support/classicui/test/play.sh, then open http://localhost:8080.

  What it cannot tell you: real redraw cost on uncached memory, SPI behaviour, or
  whether a core accepts an MGL.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <poll.h>
#include <linux/input.h>

#include "../../../cfg.h"
#include "../../../input.h"
#include "../chome.h"
#include "../chome_lib.h"
#include "../chome_art.h"
#include "../chome_theme.h"
#include "../chome_gfx.h"
#include "../chome_video.h"
#include "../../../lib/imlib2/Imlib2.h"

#include "harness.h"

#define ROOT "/tmp/chome_play"
#define PORT 8080

/* ------------------------------------------------------------- fake SD ---- */

static void mkpath(const char *p)
{
	char tmp[1024];
	snprintf(tmp, sizeof(tmp), "%s", p);
	for (char *q = tmp + 1; *q; q++)
	{
		if (*q != '/') continue;
		*q = 0;
		mkdir(tmp, 0777);
		*q = '/';
	}
	mkdir(tmp, 0777);
}

static void touch(const char *dir, const char *name)
{
	char p[1024];
	snprintf(p, sizeof(p), "%s/%s", dir, name);
	FILE *f = fopen(p, "wb");
	if (!f) return;
	for (int i = 0; i < 1024; i++) fputc(i & 0xff, f);
	fclose(f);
}

// A stand-in cover: plate, a title band, and a couple of shapes.
static void make_cover(const char *path, int w, int h, uint32_t col)
{
	Imlib_Image im = imlib_create_image(w, h);
	if (!im) return;

	imlib_context_set_image(im);
	imlib_image_set_has_alpha(0);
	uint32_t *d = (uint32_t*)imlib_image_get_data();

	for (int y = 0; y < h; y++)
	{
		for (int x = 0; x < w; x++)
		{
			uint32_t c = col;
			if (y > h * 78 / 100) c = 0xff141419;
			else if (y > h / 2 && ((x / (w / 8)) & 1)) c = (col & 0xfefefe) >> 1;
			int cx = x - w / 2, cy = y - h / 3;
			if (cx * cx + cy * cy < (w / 7) * (w / 7)) c = 0xfff0e0c0;
			d[y * w + x] = 0xff000000u | c;
		}
	}

	imlib_image_put_back_data((DATA32*)d);
	imlib_image_set_format("png");
	imlib_save_image(path);
	imlib_free_image();
}

struct fake_game { const char *dir; const char *file; const char *lr; uint32_t col; };

static const fake_game games[] =
{
	{ "SNES",       "Super Metroid (Europe).sfc",                          "Nintendo - Super Nintendo Entertainment System", 0xff5b4b8a },
	{ "SNES",       "Super Mario World (Europe).sfc",                      "Nintendo - Super Nintendo Entertainment System", 0xff3fa7e0 },
	{ "SNES",       "The Legend of Zelda - A Link to the Past (Europe).sfc","Nintendo - Super Nintendo Entertainment System", 0xff2e6e3a },
	{ "SNES",       "Super Castlevania IV (Europe).sfc",                   "Nintendo - Super Nintendo Entertainment System", 0xff6a2a3a },
	{ "SNES",       "F-Zero (Europe).sfc",                                 0,                                               0 },
	{ "Genesis",    "Sonic The Hedgehog 2 (Europe).md",                    "Sega - Mega Drive - Genesis", 0xff2b4c7e },
	{ "Genesis",    "Streets of Rage 2 (Europe).bin",                      "Sega - Mega Drive - Genesis", 0xff7a3a2a },
	{ "Genesis",    "Gunstar Heroes (Europe).md",                          0, 0 },
	{ "NES",        "Super Mario Bros 3 (Europe).nes",                     "Nintendo - Nintendo Entertainment System", 0xff8a4b2b },
	{ "NES",        "Metroid (Europe).nes",                                0, 0 },
	{ "TGFX16",     "Bonk's Adventure (USA).pce",                          "NEC - PC Engine - TurboGrafx 16", 0xff8a6e2b },
	{ "GAMEBOY",    "Tetris (World).gb",                                   "Nintendo - Game Boy", 0xff70a030 },
	{ "GAMEBOY",    "Super Mario Land (World).gb",                         0, 0 },
	{ "GAMEBOY",    "Zelda - Oracle of Ages (Europe).gbc",                 0, 0 },
	{ "GBA",        "Metroid Fusion (Europe).gba",                         "Nintendo - Game Boy Advance", 0xff4a3c8a },
	{ "GBA",        "Advance Wars (Europe).gba",                           0, 0 },
	{ "SMS",        "Sonic The Hedgehog (Europe) (GG).gg",                 0, 0 },
	{ "AtariLynx",  "Chip's Challenge (USA).lnx",                          0, 0 },
	{ "WonderSwan", "Gunpey (Japan).ws",                                   0, 0 },
	{ "Amiga",      "Turrican II.adf",                                     0, 0 },
	{ "Amiga",      "Lemmings.adf",                                        0, 0 },
};

static void build_sd()
{
	printf("Building a fake SD card at %s\n", ROOT);
	if (system("rm -rf " ROOT)) {}

	mkpath(ROOT "/config");

	for (size_t i = 0; i < sizeof(games) / sizeof(games[0]); i++)
	{
		char dir[1024];
		snprintf(dir, sizeof(dir), "%s/games/%s", ROOT, games[i].dir);
		mkpath(dir);
		touch(dir, games[i].file);

		// Cover art for some of them, so both real covers and the generated
		// fallback card are on screen at once.
		if (!games[i].lr) continue;

		char art[1024];
		snprintf(art, sizeof(art), "%s/boxart/%s/Named_Boxarts", ROOT, games[i].lr);
		mkpath(art);

		char base[512];
		snprintf(base, sizeof(base), "%s", games[i].file);
		char *dot = strrchr(base, '.');
		if (dot) *dot = 0;

		char path[1600];
		snprintf(path, sizeof(path), "%s/%s.png", art, base);
		make_cover(path, 500, 700, games[i].col);
	}

	mkpath(ROOT "/_Arcade");
	touch(ROOT "/_Arcade", "Street Fighter II.mra");
	touch(ROOT "/_Arcade", "Bubble Bobble.mra");
	touch(ROOT "/_Arcade", "Metal Slug.mra");

	// A couple of suspend points, one with a thumbnail.
	mkpath(ROOT "/savestates/SNES");
	touch(ROOT "/savestates/SNES", "Super Metroid (Europe)_1.ss");
	touch(ROOT "/savestates/SNES", "Super Metroid (Europe)_3.ss");
	make_cover(ROOT "/savestates/SNES/Super Metroid (Europe)_1.png", 320, 240, 0xff1e6fa8);

	mkpath(ROOT "/savestates/Gameboy");
	touch(ROOT "/savestates/Gameboy", "Tetris (World)_1.ss");
	make_cover(ROOT "/savestates/Gameboy/Tetris (World)_1.png", 320, 288, 0xff70a030);
}

/* ---------------------------------------------------------------- http ---- */

static int listen_fd = -1;

static int http_open()
{
	listen_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (listen_fd < 0) return 0;

	int on = 1;
	setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

	struct sockaddr_in a = {};
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(INADDR_ANY);
	a.sin_port = htons(PORT);

	if (bind(listen_fd, (struct sockaddr*)&a, sizeof(a)) || listen(listen_fd, 8))
	{
		printf("cannot listen on %d: %s\n", PORT, strerror(errno));
		close(listen_fd);
		listen_fd = -1;
		return 0;
	}
	return 1;
}

static void send_all(int fd, const char *buf, size_t len)
{
	while (len)
	{
		ssize_t n = write(fd, buf, len);
		if (n <= 0) return;
		buf += n;
		len -= (size_t)n;
	}
}

static void send_head(int fd, const char *status, const char *type, size_t len)
{
	char h[512];
	int n = snprintf(h, sizeof(h),
		"HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
		"Cache-Control: no-store\r\nConnection: close\r\n\r\n", status, type, len);
	send_all(fd, h, (size_t)n);
}

static const char *page();

// Key names the page sends, mapped here so the codes are right by construction.
static int key_from_name(const char *n)
{
	if (!strcmp(n, "up")) return KEY_UP;
	if (!strcmp(n, "down")) return KEY_DOWN;
	if (!strcmp(n, "left")) return KEY_LEFT;
	if (!strcmp(n, "right")) return KEY_RIGHT;
	if (!strcmp(n, "a")) return KEY_ENTER;
	if (!strcmp(n, "b")) return KEY_ESC;
	if (!strcmp(n, "x")) return KEY_TAB;
	if (!strcmp(n, "y")) return KEY_BACKSPACE;
	if (!strcmp(n, "select")) return KEY_GRAVE;
	if (!strcmp(n, "l")) return KEY_MINUS;
	if (!strcmp(n, "r")) return KEY_EQUAL;
	if (!strcmp(n, "menu")) return KEY_MENU;
	return 0;
}

static uint32_t frame_gen = 1;
static unsigned char *rgba = 0;
static int rgba_px = 0;

// The compose buffer is 0xAARRGGBB; the canvas wants R,G,B,A bytes.
static void build_rgba(const uint32_t *src, int w, int h)
{
	if (rgba_px < w * h)
	{
		free(rgba);
		rgba = (unsigned char*)malloc((size_t)w * h * 4);
		rgba_px = w * h;
	}
	if (!rgba) return;

	for (int i = 0; i < w * h; i++)
	{
		uint32_t c = src[i];
		rgba[i * 4 + 0] = (unsigned char)((c >> 16) & 0xff);
		rgba[i * 4 + 1] = (unsigned char)((c >> 8) & 0xff);
		rgba[i * 4 + 2] = (unsigned char)(c & 0xff);
		rgba[i * 4 + 3] = 0xff;
	}
}

static int pending_key = 0;

static void http_service()
{
	struct pollfd pf = { listen_fd, POLLIN, 0 };
	if (poll(&pf, 1, 8) <= 0) return;

	int fd = accept(listen_fd, 0, 0);
	if (fd < 0) return;

	char req[2048] = {};
	ssize_t n = read(fd, req, sizeof(req) - 1);
	if (n <= 0) { close(fd); return; }

	char path[512] = {};
	if (sscanf(req, "GET %511s", path) != 1) { close(fd); return; }

	if (!strcmp(path, "/") || !strncmp(path, "/index", 6))
	{
		const char *p = page();
		send_head(fd, "200 OK", "text/html; charset=utf-8", strlen(p));
		send_all(fd, p, strlen(p));
	}
	else if (!strncmp(path, "/key?k=", 7))
	{
		char name[32] = {};
		snprintf(name, sizeof(name), "%s", path + 7);
		for (char *q = name; *q; q++) if (*q == '&') { *q = 0; break; }

		int k = key_from_name(name);
		if (k) pending_key = k;

		send_head(fd, "204 No Content", "text/plain", 0);
	}
	else if (!strncmp(path, "/frame", 6))
	{
		uint32_t since = 0;
		const char *q = strstr(path, "since=");
		if (q) since = (uint32_t)strtoul(q + 6, 0, 10);

		int w = gfx_w(), h = gfx_h();

		if (since == frame_gen || w < 1 || h < 1)
		{
			send_head(fd, "204 No Content", "text/plain", 0);
		}
		else
		{
			uint32_t *src = harness_fb_shown();
			if (!src) send_head(fd, "204 No Content", "text/plain", 0);
			else
			{
				build_rgba(src, w, h);

				char head[256];
				int hn = snprintf(head, sizeof(head),
					"HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n"
					"X-W: %d\r\nX-H: %d\r\nX-Gen: %u\r\n"
					"Content-Length: %d\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n",
					w, h, frame_gen, w * h * 4);
				send_all(fd, head, (size_t)hn);
				send_all(fd, (const char*)rgba, (size_t)w * h * 4);
			}
		}
	}
	else
	{
		send_head(fd, "404 Not Found", "text/plain", 0);
	}

	close(fd);
}

/* ---------------------------------------------------------------- main ---- */

int main(int argc, char **argv)
{
	int w = 1280, h = 720, profile = 1;

	for (int i = 1; i < argc; i++)
	{
		if (!strcmp(argv[i], "--sd")) { w = 640; h = 480; profile = 2; }
		else if (!strcmp(argv[i], "--lo")) { w = 320; h = 240; profile = 3; }
	}

	// docker logs captures a pipe, where stdout would otherwise block-buffer and
	// hide everything the modules print.
	setvbuf(stdout, 0, _IOLBF, 0);

	build_sd();
	harness_set_root(ROOT);
	harness_use_real_clock(1);

	cfg.classicui = 1;
	cfg.classicui_artfetch = 0;
	cfg.classicui_profile = (uint8_t)profile;
	snprintf(cfg.classicui_artdir, sizeof(cfg.classicui_artdir), "boxart");

	harness_set_fb(w, h);
	theme_update(w, h, profile);

	if (!http_open()) return 1;

	printf("\n  Classic Home is live: http://localhost:%d   (%dx%d)\n", PORT, w, h);
	printf("  Ctrl-C to stop.\n\n");

	uint32_t last_gen_src = 0;

	while (1)
	{
		http_service();

		int k = pending_key;
		pending_key = 0;

		if (k)
		{
			chome_handle((uint32_t)k);
			chome_handle((uint32_t)k | UPSTROKE);
		}
		else
		{
			chome_handle(0);
		}

		// A new frame is anything the UI drew: presents are the signal.
		uint32_t p = (uint32_t)harness_present_count();
		if (p != last_gen_src)
		{
			last_gen_src = p;
			frame_gen++;
		}
	}

	return 0;
}

/* ---------------------------------------------------------------- page ---- */

static const char *page()
{
	return
"<!doctype html><meta charset=utf-8><title>Classic Home</title>\n"
"<style>\n"
"html,body{margin:0;background:#0d0e12;color:#9ea1b4;font:13px/1.5 ui-monospace,Menlo,monospace}\n"
"main{max-width:1320px;margin:0 auto;padding:18px}\n"
"canvas{display:block;width:100%;image-rendering:pixelated;background:#000;border:1px solid #2a2c36}\n"
"h1{font-size:13px;letter-spacing:.14em;text-transform:uppercase;color:#e8b22b;margin:0 0 12px}\n"
"table{border-collapse:collapse;margin-top:14px;font-size:12px}\n"
"td{padding:2px 14px 2px 0}\n"
"td:first-child{color:#d6d8e4}\n"
"b{color:#d6d8e4}\n"
"#s{margin-top:10px;color:#63667a;font-size:12px}\n"
"</style>\n"
"<main>\n"
"<h1>Classic Home &mdash; live</h1>\n"
"<canvas id=c></canvas>\n"
"<div id=s>connecting&hellip;</div>\n"
"<table>\n"
"<tr><td>&larr; &rarr; &uarr; &darr;</td><td>move &middot; up opens the menu bar &middot; down opens suspend points</td></tr>\n"
"<tr><td>Enter</td><td>A &mdash; start a game, confirm</td></tr>\n"
"<tr><td>Esc</td><td>B &mdash; back, resume</td></tr>\n"
"<tr><td>Tab</td><td>X &mdash; delete a suspend point</td></tr>\n"
"<tr><td>Backspace</td><td>Y &mdash; favourite, or save into a slot in-game</td></tr>\n"
"<tr><td>`</td><td>Select &mdash; sort</td></tr>\n"
"<tr><td>- =</td><td>L / R &mdash; jump a screenful</td></tr>\n"
"<tr><td>M</td><td>the OSD/menu button &mdash; opens the UI over a running game</td></tr>\n"
"</table>\n"
"<div id=s2 style=\"margin-top:12px;color:#63667a;font-size:12px\">Launching a game is simulated: the viewer switches to a synthetic game picture, so pressing <b>M</b> then exercises the in-game menu, its save states and Close Game.</div>\n"
"</main>\n"
"<script>\n"
"const c=document.getElementById('c'),g=c.getContext('2d'),s=document.getElementById('s');\n"
"let gen=0,img=null,busy=false,frames=0,t0=performance.now();\n"
"const KEYS={ArrowUp:'up',ArrowDown:'down',ArrowLeft:'left',ArrowRight:'right',\n"
"  Enter:'a',Escape:'b',Tab:'x',Backspace:'y','`':'select','-':'l','=':'r',m:'menu',M:'menu'};\n"
"addEventListener('keydown',e=>{const k=KEYS[e.key];if(!k)return;e.preventDefault();\n"
"  fetch('/key?k='+k).catch(()=>{});});\n"
"async function tick(){\n"
"  if(!busy){busy=true;\n"
"    try{\n"
"      const r=await fetch('/frame?since='+gen);\n"
"      if(r.status===200){\n"
"        const w=+r.headers.get('X-W'),h=+r.headers.get('X-H');\n"
"        gen=+r.headers.get('X-Gen');\n"
"        const buf=new Uint8ClampedArray(await r.arrayBuffer());\n"
"        if(c.width!==w||c.height!==h){c.width=w;c.height=h;img=null;}\n"
"        if(!img||img.width!==w)img=new ImageData(w,h);\n"
"        img.data.set(buf);g.putImageData(img,0,0);\n"
"        frames++;\n"
"      }\n"
"      const dt=(performance.now()-t0)/1000;\n"
"      s.textContent=c.width+'x'+c.height+'  \\u00b7  '+frames+' frames  \\u00b7  '+(frames/dt).toFixed(1)+' fps received';\n"
"    }catch(e){s.textContent='disconnected \\u2014 is play.sh still running?';}\n"
"    busy=false;\n"
"  }\n"
"  setTimeout(tick,40);\n"
"}\n"
"tick();\n"
"</script>\n";
}
