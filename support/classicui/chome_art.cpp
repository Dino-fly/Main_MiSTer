#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <errno.h>

#include "chome_art.h"
#include "chome_lib.h"
#include "chome_gamelist.h"
#include "chome_ss.h"
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

/*
  Name to look art up by. For a zipped ROM the path is "Game (USA).zip/Game.sfc",
  and the archive is the part named to the convention the art packs follow - the
  member inside is often abbreviated or differently cased - so the archive name
  wins whenever the path runs through one.
*/
static void rom_base(const chome_item *it, char *out, int len)
{
	char work[CH_PATH_LEN];
	snprintf(work, sizeof(work), "%s", it->path);

	char *zip = (char*)strcasestr(work, ".zip/");
	if (zip) zip[4] = 0;                    // cut the member off, keep "....zip"

	const char *fn = strrchr(work, '/');
	fn = fn ? fn + 1 : work;
	snprintf(out, len, "%s", fn);

	char *dot = strrchr(out, '.');
	if (dot) *dot = 0;
}

/*
  The game's path relative to its games dir, with an archive member cut off the way
  rom_base() does it: gamelist.xml and every scraper name the archive, not what is
  inside it, so "Game (USA).zip/Game.sfc" has to be looked up as "Game (USA).zip".
*/
static void rom_relpath(const chome_item *it, char *out, int len)
{
	snprintf(out, len, "%s", it->path);

	char *zip = (char*)strcasestr(out, ".zip/");
	if (zip) zip[4] = 0;
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

/*
  The media folders the PC scrapers write beside the ROMs, relative to the system's
  games dir, best first. Named after the ROM file, as every one of them does:
  <games dir>/media/box2d/Sonic The Hedgehog 2 (Europe).png

  "media/box2d" and its siblings are Skraper's romset layout - which is what most
  people who have scraped with ScreenScraper end up with - and "images"/"boxart"
  are what Batocera's own scraper writes into the ROM folder. Box art first,
  screenshots last: any of them beats a blank plate, but a box is what the card is
  shaped for.
*/
static const char *const scraper_dirs[] =
{
	"media/box2d",
	"boxart",
	"images",
	"media/images",
	"media/mixed",
	"media/screenshot",
	"screenshots",
};
#define SCRAPER_DIR_COUNT ((int)(sizeof(scraper_dirs) / sizeof(scraper_dirs[0])))

/*
  Finds a local file for this game, trying the layouts in order:

    0. whatever gamelist.xml names, when the player has scraped with anything else
    1. the scraper media folders beside the ROMs
    2. our own <artdir>, in the three shapes the community art packs come in
    3. next to the ROM itself

  gamelist.xml goes first on purpose. It is the one layer where the player has said
  which file belongs to which game rather than us guessing from a name, and it is
  the output of a deliberate scrape with a tool they chose; our <artdir> is a
  convention we invented, and its third shape matches on a cleaned title, which is
  the loosest match here. A gamelist entry that names a file which is not on the
  card is not allowed to win, though - gl_art() only answers with a file that
  exists - so a stale scrape leaves the later layers to do their job instead of
  producing a blank card.
*/
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

	char gd[1024];
	int have_gd = lib_sys_games_dir(it->sysidx, gd, sizeof(gd));

	// 0. what the player's own scrape says, read from gamelist.xml.
	{
		char rel[CH_PATH_LEN];
		rom_relpath(it, rel, sizeof(rel));
		if (gl_art(it->sysidx, rel, out, len)) return 1;
	}

	// 1. the scraper media folders beside the ROMs, for a card scraped without a
	//    gamelist.xml or whose gamelist names no pictures (ES-DE writes none).
	if (have_gd)
	{
		for (int m = 0; m < SCRAPER_DIR_COUNT; m++)
		{
			for (int e = 0; e < 2; e++)
			{
				snprintf(out, len, "%s/%s/%s.%s", gd, scraper_dirs[m], base, exts[e]);
				if (file_exists_abs(out)) return 1;
			}
		}
	}

	for (int r = 0; r < 2; r++)
	{
		if (!roots[r]) continue;

		// 2a. libretro layout: <artdir>/<System Name>/Named_Boxarts/<ROM name>.png
		if (s->lr[0])
		{
			for (int e = 0; e < 2; e++)
			{
				snprintf(out, len, "%s/%s/%s/Named_Boxarts/%s.%s", roots[r], dir, s->lr, safe, exts[e]);
				if (file_exists_abs(out)) return 1;
			}
		}

		// 2b. flat per-system folder: <artdir>/<games dir>/<ROM name>.png
		for (int e = 0; e < 2; e++)
		{
			snprintf(out, len, "%s/%s/%s/%s.%s", roots[r], dir, s->dir, safe, exts[e]);
			if (file_exists_abs(out)) return 1;
		}

		// 2c. cleaned display title, for hand-made packs
		for (int e = 0; e < 2; e++)
		{
			snprintf(out, len, "%s/%s/%s/%s.%s", roots[r], dir, s->dir, it->title, exts[e]);
			if (file_exists_abs(out)) return 1;
		}
	}

	// 3. next to the ROM itself
	if (have_gd)
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

/*
  imlib2 keeps its own cache of everything it loads, and decides whether an entry is still
  good from the file's mtime. A savestate slot's picture is rewritten in place whenever its
  state is, and a second write inside the same timestamp is invisible to that test - the card
  is FAT, whose mtimes are granular to two seconds - so it hands back the picture from before
  the save. Measured: the file grew from 1899 to 2355 bytes and imlib2 still returned the old
  image. Every load below is therefore decached: the pixels are copied into our own cache, so
  imlib2's second copy of them is only memory we pay for twice and a way to be wrong.
*/
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
		imlib_free_image_and_decache();
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
	imlib_free_image_and_decache();

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
	unsigned long mtime;              // of the file that was decoded, so a rewrite is seen
	long long size;                   // -1 when there was no file
	uint32_t *data;
};

static thumb_slot thumbs[THUMB_CACHE];
static uint32_t thumb_clock = 0;

/*
  Unlike cover art, these pictures are rewritten under us while their path stays the same:
  a savestate slot's picture is replaced every time its state is, and saving over a suspend
  point does exactly that. So a hit is checked against the file rather than trusted - without
  it the tile kept showing the moment that had just been overwritten, which reads as the save
  having done nothing at all, and it is what he reported as "overwriting a state does not
  work". A miss on a file that has not changed is only the same decode again.

  mtime alone will not do: the card is FAT, whose timestamps are granular to two seconds, and
  the picture is rewritten a moment after the one before it. So the size goes with it.

  Even the pair is a guess, and it guesses wrong on the case that matters most. Two frames of
  one game compress to the same number of bytes often enough - they are the same scene, the
  same palette, a sprite or two apart - and inside one FAT timestamp that is a hit on a file
  whose contents changed. He saw it: save over a suspend point twice and the tile still shows
  the first moment.

  Whoever rewrites one of these pictures knows they did, so they say so through art_forget()
  and no guessing is involved. The stat check stays for pictures that change without us -
  cover art dropped onto the card while the shelf is up.
*/
// Drops every decode of this file, whatever size it was asked for.
/*
  Where the scan of a physical disc lives, whether or not it has been fetched yet.

  Keyed on the disc's identity rather than on a filename, because a disc has no
  filename - see PHYSICAL_DISC_IDENT_FILE. The same key names the savestates and the
  per-game options, so a disc that has art has it under the name everything else
  already knows it by.

  Deliberately a path rather than a bitmap: the file is fetched and scaled elsewhere,
  and whoever draws it wants art_thumb(), which already caches decodes by path and
  size. sanitize() is applied because a key can come from a volume label, which is
  free text and occasionally contains a slash.
*/
int disc_art_path(const char *key, char *out, int len)
{
	if (!key || !*key || !out || len <= 0) return 0;

	char safe[128];
	sanitize(key, safe, sizeof(safe));
	if (!safe[0]) return 0;

	snprintf(out, len, "%s/classicui/discart/%s.png", getRootDir(), safe);
	return 1;
}

void art_forget(const char *fullpath)
{
	if (!fullpath || !*fullpath) return;

	for (int i = 0; i < THUMB_CACHE; i++)
	{
		thumb_slot *t = &thumbs[i];
		if (strcmp(t->path, fullpath)) continue;

		free(t->data);
		memset(t, 0, sizeof(*t));
	}
}

const uint32_t *art_thumb(const char *fullpath, int w, int h)
{
	if (!fullpath || !*fullpath || w < 1 || h < 1) return 0;

	struct stat st;
	int have = (!stat(fullpath, &st) && S_ISREG(st.st_mode));
	unsigned long mtime = have ? (unsigned long)st.st_mtime : 0;
	long long size = have ? (long long)st.st_size : -1;

	thumb_slot *victim = &thumbs[0];
	for (int i = 0; i < THUMB_CACHE; i++)
	{
		thumb_slot *t = &thumbs[i];
		if (!strcmp(t->path, fullpath) && t->w == w && t->h == h)
		{
			if (t->mtime == mtime && t->size == size)
			{
				t->stamp = ++thumb_clock;
				return t->failed ? 0 : t->data;
			}

			victim = t;                   // same picture, different contents: read it again
			break;
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
	victim->mtime = mtime;
	victim->size = size;

	if (!have)
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

// The trust store. Named explicitly because this rootfs carries a curl built for a
// default bundle path that does not exist on it, so every https fetch dies with "unable
// to get local issuer certificate" while a perfectly good trust store sits next to it
// unused. Pass whichever is really there; if none is, let curl fall back to its own
// default rather than give up verification, since an unverified download is not worth a
// cover picture.
static const char *curl_ca_bundle()
{
	static const char *const ca[] =
	{
		"/etc/ssl/cert.pem",
		"/etc/ssl/certs/cacert.pem",
		"/etc/ssl/certs/ca-certificates.crt",
	};

	for (size_t i = 0; i < sizeof(ca) / sizeof(ca[0]); i++)
	{
		if (file_exists_abs(ca[i])) return ca[i];
	}
	return 0;
}

// A value for a curl config file, which is double-quoted and backslash-escaped. A
// percent-encoded URL contains neither character, so this never fires in practice - it
// is here so that it cannot be made to fire by a media URL we did not write.
static void curl_cfg_quote(const char *in, char *out, int len)
{
	int o = 0;
	for (int i = 0; in && in[i] && o < len - 2; i++)
	{
		if (in[i] == '"' || in[i] == '\\') out[o++] = '\\';
		out[o++] = in[i];
	}
	if (len > 0) out[o] = 0;
}

/*
  Fork a curl for one URL into one file, or -1.

  Factored out of fetch_start() when the disc scan needed downloading too: two callers
  wanted the same three things - do not block the draw loop, verify the certificate, give
  up rather than hang - and the CA bundle problem above is exactly the sort of thing that
  gets fixed in one copy and not the other.

  `secret` is about where the URL is visible, and it is the reason this is not simply one
  execlp.

  A URL passed on argv is readable by anything on the box, for as long as the child lives:
  /proc/<pid>/cmdline, and `ps` reads it for you. That is harmless for cover art, whose
  URL is a public libretro thumbnail path - so that caller passes 0 and its command line
  is byte for byte what it always was, on a path that has been validated on hardware and
  is not worth disturbing.

  It is not harmless for the disc scan. Both of its URLs - the jeuInfos request and the
  media URL out of the reply - carry devid, devpassword, ssid and sspassword, so that
  caller passes 1 and the URL goes down a pipe into curl -K - instead, where nothing else
  can read it. Same discipline as the credentials header rather than a -D: keep the secret
  out of anything another process can see.

  The URL is never printed here either, whichever way it goes.
*/
static pid_t curl_spawn(const char *url, const char *dst, int secret)
{
	int pfd[2] = { -1, -1 };
	if (secret && pipe(pfd)) return -1;

	pid_t pid = fork();
	if (pid < 0)
	{
		if (pfd[0] >= 0) { close(pfd[0]); close(pfd[1]); }
		return -1;
	}

	if (pid > 0)
	{
		if (secret)
		{
			close(pfd[0]);

			char eurl[SS_URL_LEN * 2], edst[1200];
			curl_cfg_quote(url, eurl, sizeof(eurl));
			curl_cfg_quote(dst, edst, sizeof(edst));

			char conf[SS_URL_LEN * 2 + 1400];
			int n = snprintf(conf, sizeof(conf), "url = \"%s\"\noutput = \"%s\"\n", eurl, edst);

			// One write of well under a pipe buffer, so it cannot block the draw loop
			// waiting for a child that has not got to reading yet.
			if (n > 0 && n < (int)sizeof(conf))
			{
				ssize_t wrote = write(pfd[1], conf, (size_t)n);
				(void)wrote;
			}
			close(pfd[1]);
		}
		return pid;
	}

	/*
	  Child: quiet, fail on HTTP errors, follow redirects, hard timeout.

	  -g, --globoff, because a ScreenScraper media URL contains brackets. The one this
	  was found with ends `media=support-2D(eu)[1]`, and curl reads [ ] as a range to
	  expand - it refuses the whole URL with exit 3, "URL malformed", having never made a
	  request. Measured on the device: the query succeeded and picked support-2D/eu, and
	  then the download of that very picture failed with exit 3 and nothing to show for
	  it. Anything that passes a URL from a reply to curl needs this; a URL we composed
	  ourselves happens not to, which is exactly why it went unnoticed until a real reply
	  arrived.
	*/
	const char *bundle = curl_ca_bundle();

	if (secret)
	{
		close(pfd[1]);
		dup2(pfd[0], STDIN_FILENO);
		close(pfd[0]);

		if (bundle) execlp("curl", "curl", "-sfLg", "-m", "20", "--retry", "0",
			"--cacert", bundle, "-K", "-", (char*)NULL);

		execlp("curl", "curl", "-sfLg", "-m", "20", "--retry", "0",
			"-K", "-", (char*)NULL);
		_exit(127);
	}

	if (bundle) execlp("curl", "curl", "-sfLg", "-m", "20", "--retry", "0",
		"--cacert", bundle, "-o", dst, url, (char*)NULL);

	execlp("curl", "curl", "-sfLg", "-m", "20", "--retry", "0",
		"-o", dst, url, (char*)NULL);
	_exit(127);
}

static int fetch_start(int item)
{
	if (fetch_pid > 0) return 0;
	if (disc_art_active()) return 0;      // one download at a time, whoever wants it
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

	// 0: a public libretro thumbnail URL, safe on argv. See curl_spawn().
	pid_t pid = curl_spawn(url, fetch_tmp, 0);
	if (pid < 0) return 0;

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

/* ------------------------------------------------------------ disc scan --- */

/*
  The picture of the disc itself, for the physical-disc dialog.

  Two downloads, one frame apart, because there is no URL to fetch until the database
  has answered: first the jeuInfos reply, then the support-2D media that ss_pick()
  chooses out of it. Both go through curl_spawn(), both are reaped from art_step() with
  waitpid(WNOHANG), and neither ever runs while a cover fetch is in flight - the drive,
  the network and the one-request-at-a-time rule the API imposes are all shared.

  Where the files go matters more here than anywhere else in this file:

    the reply    lands in /tmp, which is tmpfs, and is unlinked the instant it is
                 parsed. It contains one URL per media - 133 of them in the reply this
                 was written against - and every single one carries devid, devpassword,
                 ssid and sspassword. A reply left on the SD card is the account
                 published, to anyone who ever borrows the card.
    the original lands in /tmp too and is unlinked after scaling. 417 KB for a picture
                 drawn at 144 px is not something to leave on somebody's card.
    the sprite   is the only thing that survives, at disc_art_path().

  Nothing here is reachable in a build we ship: it needs ss_enabled(), which needs
  ss_available(), which is compile-time false. It also needs classicui_artfetch, which
  is what keeps the test harness off the network.
*/
#define DA_IDLE   0
#define DA_QUERY  1      // waiting for the jeuInfos reply
#define DA_IMAGE  2      // waiting for the picture the reply named

static int   da_state = DA_IDLE;
static pid_t da_pid = -1;
static char  da_key[128];
static char  da_dst[1024];
static char  da_reply[256];
static char  da_tmp[256];

// One attempt per disc per session, the same rule cover art follows. A disc the
// database has never heard of must not be asked for on every frame of the dialog.
#define DA_TRIED_MAX 8
static char da_tried[DA_TRIED_MAX][128];
static int  da_ntried = 0;

int disc_art_active()
{
	return da_pid > 0;
}

// Raised where the picture is finished and handed over once. See disc_art_take_ready().
static int da_ready = 0;

int disc_art_take_ready()
{
	int r = da_ready;
	da_ready = 0;
	return r;
}

static unsigned da_asks = 0;

unsigned disc_art_asks()
{
	return da_asks;
}

/*
  What became of a curl, in words.

  The whole reason this exists: on the device the request went out - the log said so - and
  then nothing else was ever printed, because every failure below this point returned
  silently. "It asked and no picture appeared" describes six different faults, and the one
  thing that tells the first two apart is the exit status of a child nobody was reporting.

  The codes named are the ones this fetch can actually produce; curl's own manual page has
  the rest. 127 is not curl's at all - it is curl_spawn()'s child failing to exec one.
*/
static const char *da_curl_why(pid_t r, int status, char *buf, int len)
{
	if (r < 0) snprintf(buf, len, "could not be reaped: %s", strerror(errno));
	else if (WIFSIGNALED(status)) snprintf(buf, len, "killed by signal %d", WTERMSIG(status));
	else if (!WIFEXITED(status)) snprintf(buf, len, "did not exit normally");
	else
	{
		int e = WEXITSTATUS(status);
		const char *what =
			(e == 6)   ? " (host would not resolve: no DNS, or no network at all)" :
			(e == 7)   ? " (could not connect)" :
			(e == 22)  ? " (HTTP error: -f threw the body away, so the reason is in the status)" :
			(e == 28)  ? " (timed out: -m 20)" :
			(e == 35)  ? " (TLS handshake failed)" :
			(e == 60)  ? " (certificate not verified: the CA bundle - see curl_ca_bundle())" :
			(e == 127) ? " (no curl on this box: the exec failed)" : "";
		snprintf(buf, len, "exit %d%s", e, what);
	}
	return buf;
}

// Which refusal the database gave, in the words chome_ss.h uses for them - a number here
// would send whoever reads the log back to the header to decode it.
static const char *da_ss_why(int e)
{
	switch (e)
	{
	case SS_OK:              return "ok";
	case SS_ERR_NOTFOUND:    return "the database has no such game";
	case SS_ERR_CREDENTIALS: return "our devid pair or the account was refused";
	case SS_ERR_CLOSED:      return "the API is closed under load";
	case SS_ERR_BLACKLISTED: return "our softname is banned";
	case SS_ERR_QUOTA:       return "the account is out of requests for today";
	case SS_ERR_THREADS:     return "too many requests at once for this account";
	case SS_ERR_MALFORMED:   return "the reply did not parse";
	case SS_ERR_TRANSPORT:   return "the request never completed";
	}
	return "an answer this build does not know";
}

// Bytes on disk, or -1. Only ever for a log line, so a missing file is not an error.
static long long da_file_size(const char *path)
{
	struct stat st;
	if (!path || !path[0] || stat(path, &st)) return -1;
	return (long long)st.st_size;
}

static int da_already_tried(const char *key)
{
	for (int i = 0; i < da_ntried; i++) if (!strcmp(da_tried[i], key)) return 1;
	return 0;
}

static void da_mark_tried(const char *key)
{
	if (da_already_tried(key)) return;

	// Full: forget the oldest. A dialog opened over eight different discs in one
	// session may re-ask for the first of them, which is a request, not a bug.
	if (da_ntried >= DA_TRIED_MAX)
	{
		for (int i = 1; i < DA_TRIED_MAX; i++) memcpy(da_tried[i - 1], da_tried[i], sizeof(da_tried[0]));
		da_ntried = DA_TRIED_MAX - 1;
	}

	snprintf(da_tried[da_ntried++], sizeof(da_tried[0]), "%s", key);
}

static void da_reset()
{
	if (da_reply[0]) unlink(da_reply);
	if (da_tmp[0]) unlink(da_tmp);
	da_state = DA_IDLE;
	da_pid = -1;
	da_key[0] = 0;
	da_dst[0] = 0;
	da_reply[0] = 0;
	da_tmp[0] = 0;
}

int disc_art_request(const char *key, const char *sysid, const char *romnom)
{
	if (!key || !key[0]) return 0;

	/*
	  Counted here, above every refusal below, because "was this asked for at all" is a
	  different question from "did it get anywhere" - and the first one is the one that
	  says whether the disc turning up set this in motion or whether the dialog is still
	  the only thing that ever does. Deliberately not printed: the dialog calls this on
	  every pass of its draw, so a line here would be a line per frame.
	*/
	da_asks++;

	/*
	  A local, not da_dst. da_dst belongs to whatever fetch is in flight, and writing this
	  disc's path into it while another disc's picture was still downloading would land that
	  picture under this disc's name - the dialog would then show the wrong disc, which is
	  the one failure mode nobody would read as a bug in the fetcher.
	*/
	char want[1024];
	if (!disc_art_path(key, want, sizeof(want))) return 0;

	// Already on the card: the dialog can draw it, and nothing else has to happen.
	if (file_exists_abs(want)) return 1;

	// One in flight. Answer honestly about whose it is, so a dialog that has switched
	// discs does not sit waiting for a picture of the one before.
	if (da_pid > 0) return !strcmp(da_key, key);

	if (!cfg.classicui_artfetch) return 0;
	if (!ss_enabled()) return 0;
	if (fetch_pid > 0) return 0;
	if (da_already_tried(key)) return 0;

	const char *systemeid = ss_system_id(sysid, 0);
	if (!systemeid) return 0;

	/*
	  What to ask the database for.

	  This is the weak link and it is worth saying so out loud. jeuInfos.php matches on a
	  rom name, a hash or a game id, and a pressed disc has none of the three: there is no
	  file, hashing 700 MB off a spinning drive is not something to do while a dialog is
	  open, and the serial is not a jeuInfos key. So the caller passes a name - the disc
	  title if the title database knew it, else the serial - and it is matched as if it
	  were a filename, which will hit for the well-known discs and miss for the rest.

	  The proper answer is jeuRecherche.php, or the serial search the API grew later.
	  Neither can be tried until there is a credential to try it with, and guessing at a
	  second endpoint we cannot test would be two unknowns instead of one.
	*/
	char name[256];
	snprintf(name, sizeof(name), "%s", (romnom && romnom[0]) ? romnom : key);

	ss_query q;
	memset(&q, 0, sizeof(q));
	q.systemeid = systemeid;
	q.romnom = name;

	char url[1400];
	if (!ss_build_url(&q, 0, url, sizeof(url))) return 0;

	snprintf(da_dst, sizeof(da_dst), "%s", want);
	snprintf(da_reply, sizeof(da_reply), "/tmp/classicui_discart.xml");
	snprintf(da_key, sizeof(da_key), "%s", key);
	da_tmp[0] = 0;

	/*
	  Create the reply 0600 before curl is handed it, and remove anything left there first.

	  curl -o keeps the permissions of a file that already exists and otherwise creates one
	  at the mercy of the umask, which on this rootfs means 0644. The reply is a list of
	  URLs with our devid, our devpassword and the player's ScreenScraper password in every
	  one of them - 133 copies of them in the reply this was written against - so for the
	  couple of frames it exists it should not be readable by anything else on the box.

	  The unlink matters for the same reason from the other direction: a firmware process
	  killed mid-fetch leaves one behind, and a core change restarts the firmware without
	  clearing tmpfs, so the next request would otherwise inherit a stale credential dump
	  and its permissions.
	*/
	unlink(da_reply);
	{
		int fd = open(da_reply, O_CREAT | O_WRONLY | O_TRUNC, 0600);
		if (fd >= 0) close(fd);
	}

	// 1: this URL carries devid, devpassword, ssid and sspassword. Down a pipe, not argv.
	da_pid = curl_spawn(url, da_reply, 1);
	if (da_pid < 0) { da_reset(); return 0; }

	da_state = DA_QUERY;
	da_mark_tried(key);

	/*
	  The key and the systemeid, never the URL: the URL has devid, devpassword, ssid and
	  sspassword in it. The systemeid is in here because it is the other half of what was
	  asked - a scan that never arrives for a disc the database certainly holds is a
	  different fault if we asked the wrong platform for it, and it is the only part of
	  the query that is guessed from a table rather than read off the disc.
	*/
	printf("ClassicUI: asking for a disc scan for %s (system %s, as \"%s\")\n",
		da_key, systemeid, name);
	return 1;
}

/*
  Box-average a scan down, premultiplying by alpha.

  The naive version of this is a plain RGBA average, and it is wrong in a way that only
  shows on the thing it is for. These scans store the transparent pixels as RGB (0,0,0) -
  the corners outside the disc, and the hub hole, which is genuinely transparent out to
  15% of the disc radius - so averaging the colour channels without regard to alpha mixes
  those zeros into every pixel that straddles an edge. The result: a muddy grey rim all
  the way round the disc and a dirty ring round the hub, on a sprite whose whole appeal
  is that it is a clean circle over the dialog's background.

  So each source colour is weighted by its own alpha, the sums are divided by the sum of
  the alphas rather than by the pixel count, and alpha itself is the plain mean. A
  destination pixel with no coverage at all stays fully transparent black.
*/
static void da_box_average(const uint32_t *sp, int sw, int sh, uint32_t *dp, int dw, int dh)
{
	for (int y = 0; y < dh; y++)
	{
		int y0 = (int)((long long)y * sh / dh);
		int y1 = (int)((long long)(y + 1) * sh / dh);
		if (y1 <= y0) y1 = y0 + 1;
		if (y1 > sh) y1 = sh;

		for (int x = 0; x < dw; x++)
		{
			int x0 = (int)((long long)x * sw / dw);
			int x1 = (int)((long long)(x + 1) * sw / dw);
			if (x1 <= x0) x1 = x0 + 1;
			if (x1 > sw) x1 = sw;

			unsigned long long sa = 0, sr = 0, sg = 0, sb = 0;
			int n = 0;

			for (int yy = y0; yy < y1; yy++)
			{
				const uint32_t *row = sp + (size_t)yy * sw;
				for (int xx = x0; xx < x1; xx++)
				{
					uint32_t p = row[xx];
					unsigned long long a = (p >> 24) & 0xff;
					sa += a;
					sr += (unsigned long long)((p >> 16) & 0xff) * a;
					sg += (unsigned long long)((p >> 8) & 0xff) * a;
					sb += (unsigned long long)(p & 0xff) * a;
					n++;
				}
			}

			uint32_t v = 0;
			if (n > 0 && sa > 0)
			{
				unsigned a = (unsigned)((sa + n / 2) / (unsigned)n);
				unsigned r = (unsigned)((sr + sa / 2) / sa);
				unsigned g = (unsigned)((sg + sa / 2) / sa);
				unsigned b = (unsigned)((sb + sa / 2) / sa);
				if (a > 255) a = 255;
				if (r > 255) r = 255;
				if (g > 255) g = 255;
				if (b > 255) b = 255;
				v = (a << 24) | (r << 16) | (g << 8) | b;
			}

			dp[(size_t)y * dw + x] = v;
		}
	}
}

int disc_art_scale(const char *src_png, const char *dst_png)
{
	if (!src_png || !dst_png) return 0;

	Imlib_Load_Error err = IMLIB_LOAD_ERROR_NONE;
	Imlib_Image img = imlib_load_image_with_error_return(src_png, &err);
	if (!img)
	{
		printf("ClassicUI: disc scan load failed (%d)\n", (int)err);
		return 0;
	}

	imlib_context_set_image(img);
	int sw = imlib_image_get_width();
	int sh = imlib_image_get_height();
	const uint32_t *sp = (const uint32_t*)imlib_image_get_data_for_reading_only();

	if (sw < 1 || sh < 1 || !sp)
	{
		imlib_free_image_and_decache();
		return 0;
	}

	// Long side to DISC_ART_PX, aspect kept. The measured scan is square, but a scan
	// that is not must not come out as an oval disc.
	int dw, dh;
	if (sw >= sh)
	{
		dw = DISC_ART_PX;
		dh = (int)((long long)DISC_ART_PX * sh / sw);
	}
	else
	{
		dh = DISC_ART_PX;
		dw = (int)((long long)DISC_ART_PX * sw / sh);
	}
	if (dw < 1) dw = 1;
	if (dh < 1) dh = 1;

	// Never upscale: a scan smaller than the sprite is stored as it is rather than
	// blown up into a soft one.
	if (dw > sw) dw = sw;
	if (dh > sh) dh = sh;

	uint32_t *dp = (uint32_t*)malloc((size_t)dw * dh * 4);
	if (!dp)
	{
		imlib_free_image_and_decache();
		return 0;
	}

	da_box_average(sp, sw, sh, dp, dw, dh);
	imlib_free_image_and_decache();

	Imlib_Image out = imlib_create_image_using_copied_data(dw, dh, (DATA32*)dp);
	free(dp);
	if (!out) return 0;

	imlib_context_set_image(out);

	// Without this imlib2 writes the PNG with the alpha channel flattened away, and the
	// corners and the hub come out black instead of transparent - which on the dialog is
	// a black square with a disc printed on it.
	imlib_image_set_has_alpha(1);
	imlib_image_set_format("png");

	mkdirs(dst_png);

	/*
	  Whether that worked, said out loud.

	  mkdirs() returns nothing and complains about nothing, so a card mounted read-only, a
	  full one, or a plain file sitting where classicui/discart should be all end here as an
	  imlib save error with no hint of which - and the discart directory not existing on the
	  card afterwards is exactly the symptom being diagnosed. So the directory is checked
	  before the write and named if it is not there.
	*/
	{
		char dir[1024];
		snprintf(dir, sizeof(dir), "%s", dst_png);
		char *slash = strrchr(dir, '/');
		if (slash) *slash = 0;

		struct stat st;
		if (slash && dir[0] && (stat(dir, &st) || !S_ISDIR(st.st_mode)))
		{
			printf("ClassicUI: no directory for the disc scan at %s (%s)\n", dir, strerror(errno));
			imlib_free_image();
			return 0;
		}
	}

	Imlib_Load_Error serr = IMLIB_LOAD_ERROR_NONE;
	imlib_save_image_with_error_return(dst_png, &serr);
	imlib_free_image();

	if (serr != IMLIB_LOAD_ERROR_NONE)
	{
		printf("ClassicUI: disc scan save failed (%d) %s\n", (int)serr, dst_png);
		return 0;
	}

	// Whoever rewrote a picture says so, rather than leaving the thumbnail cache to
	// guess from an mtime the card is too coarse to resolve. See art_forget().
	art_forget(dst_png);

	/*
	  And whoever is drawing has to be told to draw again, which is the other half of the
	  same thought. Here rather than in the poll below because this is the moment the
	  picture becomes a picture - anything that writes one of these sprites, now or later,
	  wants the screen repainted, and a signal raised in the caller would be a signal the
	  next caller forgets. See disc_art_take_ready().
	*/
	da_ready = 1;
	return 1;
}

/*
  The reply, then the picture, and every way either of them can go wrong said out loud.

  This path used to be silent from end to end. On the device with real credentials the
  request went out - "asking for a disc scan for SLES-01506" is in the log - and then
  nothing: no classicui/discart directory, no sprite, and not one further line. Six
  distinct faults look identical from outside, and none of them was reported:

    curl never ran, or ran and failed          the exit status says which
    the reply parsed to a refusal              the database says which refusal
    the reply named no media at all            the game is not in there
    it named media but no support-2D           the game is in there without a disc scan
    the picture downloaded and would not scale disc_art_scale() says why
    there was nowhere to write it              also disc_art_scale()

  So each of them prints, and each print distinguishes itself from the others rather than
  saying "the disc scan failed" six times. The key is in every line, because two discs in
  one session otherwise leave a log nobody can attribute; the URL is in none of them,
  because every media URL in that reply carries devid, devpassword, ssid and sspassword -
  ss_redact_url() is there for the case where one genuinely has to be shown, and this is
  not one of those cases.
*/
static void disc_art_poll()
{
	if (da_pid <= 0) return;

	int status = 0;
	pid_t r = waitpid(da_pid, &status, WNOHANG);
	if (!r) return;

	int ok = (r > 0) && WIFEXITED(status) && !WEXITSTATUS(status);
	da_pid = -1;

	char why[128];

	if (da_state == DA_QUERY)
	{
		if (!ok)
		{
			printf("ClassicUI: the disc scan query for %s failed, curl %s\n",
				da_key, da_curl_why(r, status, why, sizeof(why)));
			da_reset();
			return;
		}

		long long got = da_file_size(da_reply);
		if (got <= 0)
		{
			// curl -f discards the body of an HTTP error, so a zero-byte reply and a
			// missing one are the same fault from here: it answered, with nothing.
			printf("ClassicUI: the disc scan query for %s came back with no reply (%lld bytes)\n",
				da_key, got);
			da_reset();
			return;
		}

		/*
		  Static, not a local. ss_result holds SS_MAX_MEDIA url[512] buffers - ~53 KB -
		  and this runs on the thread that draws, whose stack is not the place for it.
		*/
		static ss_result res;
		int e = ss_parse_file(da_reply, &res);

		// Parsed or not, the reply goes now. It is a list of URLs with our devid and the
		// player's password in every one of them, sitting in a file.
		unlink(da_reply);
		da_reply[0] = 0;

		if (e != SS_OK)
		{
			printf("ClassicUI: the disc scan reply for %s (%lld bytes) says: %s\n",
				da_key, got, da_ss_why(e));
			da_reset();
			return;
		}

		if (!res.nmedia)
		{
			printf("ClassicUI: the disc scan reply for %s names no media at all\n", da_key);
			da_reset();
			return;
		}

		/*
		  Region from the disc, which for a PlayStation disc is in the serial that is also
		  its identity key. Nothing derivable means no preference at all rather than a
		  house default, which lands ss_pick() on the first support-2D in reply order.
		*/
		const char *regs[4];
		int nr = ss_regions_for_serial(da_key, regs, 4);

		const ss_media *m = ss_pick(&res, SS_KIND_DISC, nr ? regs : 0);
		if (!m)
		{
			/*
			  The game is in the database and has pictures, just not this kind. Worth
			  distinguishing from "no media at all": that one is a bad match on the name
			  and would be fixed by asking differently, this one is a gap in the database
			  and no amount of asking will fill it. The types that were there are listed
			  because it is also how a change in what the API calls a disc scan would
			  show - see the top of chome_ss.cpp on that being the soft spot.
			*/
			printf("ClassicUI: the disc scan reply for %s has %d media but no support-2D:",
				da_key, res.nmedia);
			for (int i = 0; i < res.nmedia && i < 12; i++) printf(" %s", res.media[i].type);
			printf("%s\n", res.nmedia > 12 ? " ..." : "");
			da_reset();
			return;
		}

		snprintf(da_tmp, sizeof(da_tmp), "/tmp/classicui_discart_src");

		// 1 again: the media URL out of the reply carries the same four credentials.
		da_pid = curl_spawn(m->url, da_tmp, 1);
		if (da_pid < 0)
		{
			printf("ClassicUI: could not fork a curl for the disc scan for %s (%s)\n",
				da_key, strerror(errno));
			da_reset();
			return;
		}

		da_state = DA_IMAGE;

		// Type and region, never the URL - see ss_redact_url() for what is in one.
		printf("ClassicUI: disc scan for %s is %s/%s\n", da_key, m->type,
			m->region[0] ? m->region : "no region");
		return;
	}

	if (da_state == DA_IMAGE)
	{
		long long got = da_file_size(da_tmp);

		if (!ok)
		{
			printf("ClassicUI: the disc scan download for %s failed, curl %s\n",
				da_key, da_curl_why(r, status, why, sizeof(why)));
		}
		else if (got <= 0)
		{
			printf("ClassicUI: the disc scan for %s downloaded nothing (%lld bytes)\n",
				da_key, got);
		}
		else if (!disc_art_scale(da_tmp, da_dst))
		{
			// disc_art_scale() has already said which of load, directory or save it was.
			printf("ClassicUI: the disc scan for %s downloaded (%lld bytes) but would not"
				" scale into %s\n", da_key, got, da_dst);
		}
		else
		{
			// The file, named: it is the one thing on the card afterwards, and "it is not
			// there" was half of what there was to go on when this was silent.
			printf("ClassicUI: disc scan stored for %s: %s (%lld bytes from %lld)\n",
				da_key, da_dst, da_file_size(da_dst), got);
		}

		// The 417 KB original goes either way. da_reset() unlinks it.
		da_reset();
		return;
	}

	da_reset();
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

	/*
	  Before the early return below, deliberately. The disc dialog is drawn over a running
	  core, and the shelf's slots may not exist at all there - art_init() runs off a
	  library that a disc-only session never builds. Reaping after that check would leave a
	  started fetch unreaped until the player went back to the shelf, which is a zombie and
	  a picture that turns up minutes late.
	*/
	disc_art_poll();

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
