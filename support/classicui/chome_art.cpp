#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>

#include "chome_art.h"
#include "chome_lib.h"
#include "../../file_io.h"
#include "../../cfg.h"
#include "../../lib/imlib2/Imlib2.h"

#define CACHE_MAX_BYTES (24 * 1024 * 1024)
#define QUEUE_MAX 32

struct art_slot
{
	int item;
	int state;
	int w, h;
	uint32_t *data;
	uint32_t stamp;      // for LRU
	int tried_fetch;
};

static art_slot *slots = 0;    // one per item, sparse: data only for loaded ones
static int nslots = 0;

static int card_w = 0, card_h = 0;
static uint32_t clock_tick = 0;
static int cache_bytes = 0;
static int cache_count = 0;

// decode queue, sorted by priority
static int queue[QUEUE_MAX];
static int qprio[QUEUE_MAX];
static int nqueue = 0;

// one outstanding fetch
static pid_t fetch_pid = -1;
static int fetch_item = -1;
static char fetch_tmp[1024];
static char fetch_dst[1024];

void art_init(int cw, int chh)
{
	if (cw != card_w || chh != card_h)
	{
		art_shutdown();
		card_w = cw;
		card_h = chh;
	}

	if (!slots)
	{
		nslots = lib_item_count();
		if (nslots < 1) nslots = 1;
		slots = (art_slot*)calloc(nslots, sizeof(art_slot));
		if (slots) for (int i = 0; i < nslots; i++) slots[i].item = i;
	}
	else if (lib_item_count() > nslots)
	{
		// The background scan added items: grow.
		int want = lib_item_count();
		art_slot *ns = (art_slot*)calloc(want, sizeof(art_slot));
		if (ns)
		{
			memcpy(ns, slots, sizeof(art_slot) * nslots);
			free(slots);
			slots = ns;
			for (int i = nslots; i < want; i++) slots[i].item = i;
			nslots = want;
		}
	}
}

void art_shutdown()
{
	if (slots)
	{
		for (int i = 0; i < nslots; i++) free(slots[i].data);
		free(slots);
		slots = 0;
	}
	nslots = 0;
	nqueue = 0;
	cache_bytes = 0;
	cache_count = 0;
}

int art_cache_count() { return cache_count; }
int art_cache_bytes() { return cache_bytes; }
int art_fetch_active() { return fetch_pid > 0; }

int art_state(int item)
{
	if (!slots || item < 0 || item >= nslots) return ART_NONE;
	return slots[item].state;
}

const uint32_t *art_get(int item, int *w, int *h)
{
	if (!slots || item < 0 || item >= nslots) return 0;
	art_slot *s = &slots[item];
	if (s->state != ART_READY || !s->data) return 0;
	s->stamp = ++clock_tick;
	if (w) *w = s->w;
	if (h) *h = s->h;
	return s->data;
}

/* ---------------------------------------------------------------- paths --- */

// libretro replaces these with '_' in thumbnail names.
static void sanitize(const char *in, char *out, int len)
{
	int o = 0;
	for (int i = 0; in[i] && o < len - 1; i++)
	{
		char c = in[i];
		if (c == '&' || c == '*' || c == '/' || c == ':' || c == '`' ||
			c == '<' || c == '>' || c == '?' || c == '\\' || c == '|') c = '_';
		out[o++] = c;
	}
	out[o] = 0;
}

static void rom_base(const chome_item *it, char *out, int len)
{
	const char *fn = strrchr(it->path, '/');
	fn = fn ? fn + 1 : it->path;
	snprintf(out, len, "%s", fn);
	char *dot = strrchr(out, '.');
	if (dot) *dot = 0;
}

static int file_exists_abs(const char *p)
{
	struct stat st;
	return (!stat(p, &st) && S_ISREG(st.st_mode)) ? 1 : 0;
}

// Where the libretro-layout art for this game should live locally.
static int art_cache_path(const chome_item *it, char *out, int len)
{
	const chome_sys *s = lib_sys(it->sysidx);
	if (!s || !s->lr[0]) return 0;

	char base[CH_PATH_LEN];
	rom_base(it, base, sizeof(base));

	char safe[CH_PATH_LEN];
	sanitize(base, safe, sizeof(safe));

	const char *dir = cfg.classicui_artdir[0] ? cfg.classicui_artdir : "boxart";
	snprintf(out, len, "%s/%s/%s/Named_Boxarts/%s.png", getRootDir(), dir, s->lr, safe);
	return 1;
}

// Finds a local file for this game, trying the community layouts in order.
static int find_local_art(const chome_item *it, char *out, int len)
{
	const chome_sys *s = lib_sys(it->sysidx);
	if (!s) return 0;

	const char *dir = cfg.classicui_artdir[0] ? cfg.classicui_artdir : "boxart";

	char base[CH_PATH_LEN];
	rom_base(it, base, sizeof(base));
	char safe[CH_PATH_LEN];
	sanitize(base, safe, sizeof(safe));

	const char *roots[2] = { getRootDir(), "/media/usb0" };
	const char *exts[2] = { "png", "jpg" };

	for (int r = 0; r < 2; r++)
	{
		if (!roots[r]) continue;

		// 1. libretro layout: <artdir>/<System Name>/Named_Boxarts/<ROM name>.png
		if (s->lr[0])
		{
			for (int e = 0; e < 2; e++)
			{
				snprintf(out, len, "%s/%s/%s/Named_Boxarts/%s.%s", roots[r], dir, s->lr, safe, exts[e]);
				if (file_exists_abs(out)) return 1;
			}
		}

		// 2. flat per-system folder: <artdir>/<games dir>/<ROM name>.png
		for (int e = 0; e < 2; e++)
		{
			snprintf(out, len, "%s/%s/%s/%s.%s", roots[r], dir, s->dir, safe, exts[e]);
			if (file_exists_abs(out)) return 1;
		}

		// 3. cleaned display title, for hand-made packs
		for (int e = 0; e < 2; e++)
		{
			snprintf(out, len, "%s/%s/%s/%s.%s", roots[r], dir, s->dir, it->title, exts[e]);
			if (file_exists_abs(out)) return 1;
		}
	}

	// 4. next to the ROM itself
	char gd[1024];
	if (lib_sys_games_dir(it->sysidx, gd, sizeof(gd)))
	{
		char rel[CH_PATH_LEN];
		snprintf(rel, sizeof(rel), "%s", it->path);
		char *slash = strrchr(rel, '/');
		if (slash) *slash = 0; else rel[0] = 0;

		for (int e = 0; e < 2; e++)
		{
			if (rel[0]) snprintf(out, len, "%s/%s/%s.%s", gd, rel, base, exts[e]);
			else snprintf(out, len, "%s/%s.%s", gd, base, exts[e]);
			if (file_exists_abs(out)) return 1;
		}
	}

	return 0;
}

/* --------------------------------------------------------------- decode --- */

static void cache_evict(int need)
{
	while (cache_bytes + need > CACHE_MAX_BYTES)
	{
		art_slot *victim = 0;
		for (int i = 0; i < nslots; i++)
		{
			if (slots[i].state != ART_READY || !slots[i].data) continue;
			if (!victim || slots[i].stamp < victim->stamp) victim = &slots[i];
		}
		if (!victim) return;

		cache_bytes -= victim->w * victim->h * 4;
		cache_count--;
		free(victim->data);
		victim->data = 0;
		victim->state = ART_NONE;   // may be reloaded later
	}
}

static int decode_into(const char *path, art_slot *s, uint32_t plate)
{
	Imlib_Load_Error err = IMLIB_LOAD_ERROR_NONE;
	Imlib_Image img = imlib_load_image_with_error_return(path, &err);
	if (!img)
	{
		printf("ClassicUI: art load failed (%d) %s\n", (int)err, path);
		return 0;
	}

	imlib_context_set_image(img);
	int sw = imlib_image_get_width();
	int sh = imlib_image_get_height();
	if (sw < 1 || sh < 1)
	{
		imlib_free_image();
		return 0;
	}

	int dw = card_w, dh = card_h;

	// Fit, do not stretch: cover art aspect varies wildly between systems (SNES
	// boxes are wide, Mega Drive tall). The remainder shows the system plate.
	int fw, fh;
	if ((long long)sw * dh > (long long)sh * dw)
	{
		fw = dw;
		fh = (int)((long long)dw * sh / sw);
	}
	else
	{
		fh = dh;
		fw = (int)((long long)dh * sw / sh);
	}
	if (fw < 1) fw = 1;
	if (fh < 1) fh = 1;
	if (fw > dw) fw = dw;
	if (fh > dh) fh = dh;

	Imlib_Image scaled = imlib_create_cropped_scaled_image(0, 0, sw, sh, fw, fh);
	imlib_context_set_image(img);
	imlib_free_image();

	if (!scaled) return 0;

	imlib_context_set_image(scaled);
	uint32_t *src = (uint32_t*)imlib_image_get_data_for_reading_only();
	if (!src)
	{
		imlib_free_image();
		return 0;
	}

	int bytes = dw * dh * 4;
	cache_evict(bytes);

	uint32_t *dst = (uint32_t*)malloc(bytes);
	if (dst)
	{
		for (int i = 0; i < dw * dh; i++) dst[i] = plate;

		int ox = (dw - fw) / 2;
		int oy = (dh - fh) / 2;
		for (int y = 0; y < fh; y++)
		{
			memcpy(dst + (size_t)(oy + y) * dw + ox, src + (size_t)y * fw, (size_t)fw * 4);
		}

		free(s->data);
		s->data = dst;
		s->w = dw;
		s->h = dh;
		s->state = ART_READY;
		cache_bytes += bytes;
		cache_count++;
	}

	imlib_context_set_image(scaled);
	imlib_free_image();

	return dst ? 1 : 0;
}

/* ------------------------------------------------------------ thumbnails -- */

#define THUMB_CACHE 4

struct thumb_slot
{
	char path[512];
	int w, h;
	int failed;
	uint32_t stamp;
	uint32_t *data;
};

static thumb_slot thumbs[THUMB_CACHE];
static uint32_t thumb_clock = 0;

const uint32_t *art_thumb(const char *fullpath, int w, int h)
{
	if (!fullpath || !*fullpath || w < 1 || h < 1) return 0;

	thumb_slot *victim = &thumbs[0];
	for (int i = 0; i < THUMB_CACHE; i++)
	{
		thumb_slot *t = &thumbs[i];
		if (!strcmp(t->path, fullpath) && t->w == w && t->h == h)
		{
			t->stamp = ++thumb_clock;
			return t->failed ? 0 : t->data;
		}
		if (t->stamp < victim->stamp) victim = t;
	}

	// Miss: take over the least recently used slot.
	free(victim->data);
	memset(victim, 0, sizeof(*victim));
	snprintf(victim->path, sizeof(victim->path), "%s", fullpath);
	victim->w = w;
	victim->h = h;
	victim->stamp = ++thumb_clock;

	if (!file_exists_abs(fullpath))
	{
		victim->failed = 1;
		return 0;
	}

	art_slot tmp = {};
	int save_w = card_w, save_h = card_h;
	card_w = w;
	card_h = h;
	int ok = decode_into(fullpath, &tmp, 0xff000000u);
	card_w = save_w;
	card_h = save_h;

	if (!ok)
	{
		victim->failed = 1;
		return 0;
	}

	// decode_into accounted this against the cover-art budget; move it over.
	cache_bytes -= tmp.w * tmp.h * 4;
	cache_count--;

	victim->data = tmp.data;
	return victim->data;
}

/* ---------------------------------------------------------------- fetch --- */

static void url_encode(const char *in, char *out, int len)
{
	static const char *hex = "0123456789ABCDEF";
	int o = 0;
	for (int i = 0; in[i] && o < len - 4; i++)
	{
		unsigned char c = (unsigned char)in[i];
		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
			c == '-' || c == '_' || c == '.' || c == '~' || c == '/')
		{
			out[o++] = (char)c;
		}
		else
		{
			out[o++] = '%';
			out[o++] = hex[c >> 4];
			out[o++] = hex[c & 15];
		}
	}
	out[o] = 0;
}

static void mkdirs(const char *path)
{
	char tmp[1024];
	snprintf(tmp, sizeof(tmp), "%s", path);

	char *slash = strrchr(tmp, '/');
	if (!slash) return;
	*slash = 0;

	for (char *p = tmp + 1; *p; p++)
	{
		if (*p != '/') continue;
		*p = 0;
		mkdir(tmp, 0777);
		*p = '/';
	}
	mkdir(tmp, 0777);
}

static int fetch_start(int item)
{
	if (fetch_pid > 0) return 0;
	if (!cfg.classicui_artfetch) return 0;

	chome_item *it = lib_item(item);
	if (!it) return 0;

	const chome_sys *s = lib_sys(it->sysidx);
	if (!s || !s->lr[0]) return 0;

	if (!art_cache_path(it, fetch_dst, sizeof(fetch_dst))) return 0;

	char base[CH_PATH_LEN];
	rom_base(it, base, sizeof(base));
	char safe[CH_PATH_LEN];
	sanitize(base, safe, sizeof(safe));

	char encsys[256], encname[512];
	url_encode(s->lr, encsys, sizeof(encsys));
	url_encode(safe, encname, sizeof(encname));

	const char *root = cfg.classicui_arturl[0] ? cfg.classicui_arturl : "https://thumbnails.libretro.com";

	char url[1400];
	snprintf(url, sizeof(url), "%s/%s/Named_Boxarts/%s.png", root, encsys, encname);

	snprintf(fetch_tmp, sizeof(fetch_tmp), "/tmp/classicui_art_%d.png", item);
	mkdirs(fetch_dst);

	pid_t pid = fork();
	if (pid < 0) return 0;

	if (!pid)
	{
		/*
		  Child: quiet, fail on HTTP errors, follow redirects, hard timeout.

		  The CA bundle is named explicitly. This rootfs carries a curl built for a
		  default bundle path that does not exist on it, so every https fetch dies
		  with "unable to get local issuer certificate" while a perfectly good trust
		  store sits next to it unused. Pass whichever bundle is really there; if
		  none is, let curl fall back to its own default rather than give up
		  verification, since an unverified download is not worth a cover picture.
		*/
		static const char *const ca[] =
		{
			"/etc/ssl/cert.pem",
			"/etc/ssl/certs/cacert.pem",
			"/etc/ssl/certs/ca-certificates.crt",
		};

		const char *bundle = 0;
		for (size_t i = 0; !bundle && i < sizeof(ca) / sizeof(ca[0]); i++)
		{
			if (file_exists_abs(ca[i])) bundle = ca[i];
		}

		if (bundle) execlp("curl", "curl", "-sfL", "-m", "20", "--retry", "0",
			"--cacert", bundle, "-o", fetch_tmp, url, (char*)NULL);

		execlp("curl", "curl", "-sfL", "-m", "20", "--retry", "0",
			"-o", fetch_tmp, url, (char*)NULL);
		_exit(127);
	}

	fetch_pid = pid;
	fetch_item = item;
	if (slots && item < nslots) slots[item].state = ART_FETCHING;
	printf("ClassicUI: fetching art for %s\n", it->title);
	return 1;
}

static void fetch_poll()
{
	if (fetch_pid <= 0) return;

	int status = 0;
	pid_t r = waitpid(fetch_pid, &status, WNOHANG);
	if (!r) return;

	int ok = (r > 0) && WIFEXITED(status) && !WEXITSTATUS(status);
	fetch_pid = -1;

	art_slot *s = (slots && fetch_item >= 0 && fetch_item < nslots) ? &slots[fetch_item] : 0;

	if (ok && file_exists_abs(fetch_tmp))
	{
		if (rename(fetch_tmp, fetch_dst))
		{
			// /tmp and the SD card are different filesystems: copy instead.
			FILE *in = fopen(fetch_tmp, "rb");
			FILE *out = in ? fopen(fetch_dst, "wb") : 0;
			if (in && out)
			{
				char buf[8192];
				size_t n;
				while ((n = fread(buf, 1, sizeof(buf), in)) > 0) fwrite(buf, 1, n, out);
			}
			if (out) fclose(out);
			if (in) fclose(in);
			unlink(fetch_tmp);
		}

		if (s)
		{
			s->state = ART_PENDING;    // decode it on a later step
			if (nqueue < QUEUE_MAX)
			{
				queue[nqueue] = fetch_item;
				qprio[nqueue] = 0;
				nqueue++;
			}
		}
	}
	else
	{
		unlink(fetch_tmp);
		if (s) s->state = ART_MISSING;
	}

	fetch_item = -1;
}

/* ---------------------------------------------------------------- queue --- */

void art_request(int item, int prio)
{
	if (!slots || item < 0 || item >= nslots) return;

	art_slot *s = &slots[item];
	if (s->state == ART_READY) { s->stamp = ++clock_tick; return; }
	if (s->state == ART_MISSING || s->state == ART_FETCHING) return;

	for (int i = 0; i < nqueue; i++)
	{
		if (queue[i] == item)
		{
			if (prio < qprio[i]) qprio[i] = prio;
			return;
		}
	}

	if (nqueue >= QUEUE_MAX)
	{
		// Replace the worst queued entry if this one matters more.
		int worst = 0;
		for (int i = 1; i < nqueue; i++) if (qprio[i] > qprio[worst]) worst = i;
		if (qprio[worst] <= prio) return;
		queue[worst] = item;
		qprio[worst] = prio;
	}
	else
	{
		queue[nqueue] = item;
		qprio[nqueue] = prio;
		nqueue++;
	}

	s->state = ART_PENDING;
}

static int queue_pop()
{
	if (!nqueue) return -1;

	int best = 0;
	for (int i = 1; i < nqueue; i++) if (qprio[i] < qprio[best]) best = i;

	int item = queue[best];
	queue[best] = queue[nqueue - 1];
	qprio[best] = qprio[nqueue - 1];
	nqueue--;
	return item;
}

void art_step()
{
	fetch_poll();
	if (!slots) return;

	int item = queue_pop();
	if (item < 0) return;
	if (item >= nslots) return;

	art_slot *s = &slots[item];
	if (s->state == ART_READY) return;

	chome_item *it = lib_item(item);
	if (!it) { s->state = ART_MISSING; return; }

	const chome_sys *sys = lib_sys(it->sysidx);
	uint32_t plate = sys ? sys->tint : 0xff4a4c58u;

	char path[1024];
	if (find_local_art(it, path, sizeof(path)))
	{
		if (decode_into(path, s, plate)) return;
		s->state = ART_MISSING;
		return;
	}

	// Nothing local. Try the network once per session, if enabled.
	if (cfg.classicui_artfetch && !s->tried_fetch && fetch_pid <= 0)
	{
		s->tried_fetch = 1;
		if (fetch_start(item)) return;
	}

	s->state = ART_MISSING;
}
