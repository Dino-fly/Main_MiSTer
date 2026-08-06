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

	/*
	  ScreenScraper looked for this game and had no cover for it, so the rung is done
	  with and the ladder drops to the libretro pack.

	  A separate field from tried_fetch above rather than a shared "we tried the network"
	  flag, and the separation is the whole point: this one may be set only when
	  ss_verdict() is true. A quota that ran out mid-scroll must leave every game it
	  touched as unasked as it found them - see art_ss_absent() - and a single flag
	  meaning "some network source has been tried" could not express that.
	*/
	int ss_absent;
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

  `status`, when given, is a file the HTTP status code is written into, and it is the only
  way this client can ever see one.

  ScreenScraper signals a rate limit as HTTP 429 and a spent quota as 430, and -f throws
  the body of an HTTP error away - so from a bare exit status those two are
  indistinguishable from a dropped connection, and ss_http_class() could never fire on a
  live reply however carefully it was written. That is not a cosmetic loss: 429 is the one
  refusal there is no other evidence for anywhere, and mistaking it for transport is
  mistaking "stop asking" for "try the next game", which is how a client earns a
  blacklisting. So curl is asked for %{http_code} and its stdout is pointed at a file.

  Only honoured for `secret`, which is both ScreenScraper paths. The libretro fetch passes
  0 and its command line stays byte for byte what it has always been - that path is
  validated on hardware and is not worth disturbing to learn a status nothing reads.
*/
static pid_t curl_spawn(const char *url, const char *dst, int secret, const char *status)
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

			/*
			  write-out is the config-file spelling of -w. In the config rather than on argv
			  because that is where the rest of this request already lives, and because
			  argv is the thing this whole branch exists to keep empty.
			*/
			char conf[SS_URL_LEN * 2 + 1400];
			int n = snprintf(conf, sizeof(conf), "url = \"%s\"\noutput = \"%s\"\n%s", eurl, edst,
				status ? "write-out = \"%{http_code}\"\n" : "");

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

		/*
		  The status file takes the child's stdout, which is where -w writes. Without this
		  the code would land in the firmware's own log, where nothing can read it and it
		  would sit in the middle of a line somebody else was writing.

		  0600 like the reply, and for a weaker version of the same reason: this file holds
		  three digits and no secret, but it is made by the same request and there is no
		  argument for it being more readable than the reply it describes.
		*/
		if (status)
		{
			int sfd = open(status, O_CREAT | O_WRONLY | O_TRUNC, 0600);
			if (sfd >= 0) { dup2(sfd, STDOUT_FILENO); close(sfd); }
		}

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

// Defined with the ScreenScraper fetch below. Declared here because the two share the one
// download slot, and this is the caller that has to ask about it first.
static int ss_fetch_active();

static int fetch_start(int item)
{
	if (fetch_pid > 0) return 0;
	// One download at a time, whoever wants it. ss_fetch_active() rather than
	// disc_art_active(): the latter answers only for a disc now, and a cover being fetched
	// from ScreenScraper holds the same slot for the same reasons.
	if (ss_fetch_active()) return 0;
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
	pid_t pid = curl_spawn(url, fetch_tmp, 0, 0);
	if (pid < 0) return 0;

	fetch_pid = pid;
	fetch_item = item;
	if (slots && item < nslots) slots[item].state = ART_FETCHING;
	printf("ClassicUI: fetching art for %s\n", it->title);
	return 1;
}

/*
  Move a downloaded picture from tmpfs onto the card, and return 1 if it got there.

  Factored out when ScreenScraper became a second source of covers, because both of them
  land a file in /tmp and both want it in the same place afterwards - and the rename that
  fails is the interesting part. /tmp is tmpfs and the artdir is on the SD card, so
  rename() across them fails with EXDEV every single time on the device; the copy is not a
  fallback for an unusual case, it is the path that always runs there. It only looks like
  a fallback because rename() does work in a host test, where both are on the same
  filesystem.

  The source is unlinked either way. Leaving it would fill tmpfs with covers over a long
  session, and tmpfs is RAM on this box.
*/
static int store_download(const char *src, const char *dst)
{
	if (!rename(src, dst)) return 1;

	FILE *in = fopen(src, "rb");
	FILE *out = in ? fopen(dst, "wb") : 0;

	int ok = 0;
	if (in && out)
	{
		char buf[8192];
		size_t n;
		ok = 1;
		while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
		{
			if (fwrite(buf, 1, n, out) != n) ok = 0;
		}
	}

	if (out && fclose(out)) ok = 0;
	if (in) fclose(in);
	unlink(src);

	if (!ok) unlink(dst);      // a half-written PNG on the card is worse than none
	return ok;
}

// Hand an item back to the queue so the ladder is walked again for it. ART_NONE rather
// than leaving it ART_FETCHING when the queue is full: ART_FETCHING is one of the two
// states art_request() refuses to re-queue, so an item left in it when there was no room
// would never be looked at again.
static void art_requeue(int item)
{
	if (!slots || item < 0 || item >= nslots) return;

	if (nqueue < QUEUE_MAX)
	{
		queue[nqueue] = item;
		qprio[nqueue] = 0;
		nqueue++;
		slots[item].state = ART_PENDING;
		return;
	}

	slots[item].state = ART_NONE;
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

	if (ok && file_exists_abs(fetch_tmp) && store_download(fetch_tmp, fetch_dst))
	{
		if (s) art_requeue(fetch_item);    // decode it on a later step
	}
	else
	{
		unlink(fetch_tmp);
		if (s) s->state = ART_MISSING;
	}

	fetch_item = -1;
}

/* --------------------------------------------------- the ScreenScraper fetch --- */

/*
  One fetcher, two media types: the scan of a physical disc's face, and a shelf cover.

  It was written for the disc alone and was called the disc fetch throughout - da_state,
  da_pid, da_key. When ScreenScraper became the first source for covers as well, the
  choice was to grow this or to write a second one beside it, and a second one was the
  wrong answer for a reason this tree has already paid for once: the -g that a media URL
  needs, the 0600 on the reply, the unlink of a credential dump, the one-request-at-a-time
  rule and the six distinct ways a fetch fails silently are all things that would have
  been fixed in one copy and not the other. So the prefix is ssf_ now, because a shared
  mechanism that still carries the name of its first caller is a mechanism the next reader
  gets wrong.

  What is genuinely per-kind stays per-kind, and it is only three things: which media type
  is picked out of the reply, where the picture ends up, and what "finished" means
  afterwards. Everything between the request and that point is one path.

  Two downloads, one frame apart, because there is no URL to fetch until the database has
  answered: first the jeuInfos reply, then the media that ss_pick() chooses out of it.
  Both go through curl_spawn(), both are reaped from art_step() with waitpid(WNOHANG), and
  neither ever runs while the libretro cover fetch is in flight - the drive, the network
  and the one-request-at-a-time rule the API imposes are all shared.

  Where the files go matters more here than anywhere else in this file:

    the reply    lands in /tmp, which is tmpfs, and is unlinked the instant it is
                 parsed. It contains one URL per media - 133 of them in the reply this
                 was written against - and every single one carries devid, devpassword,
                 ssid and sspassword. A reply left on the SD card is the account
                 published, to anyone who ever borrows the card.
    the original lands in /tmp too and is unlinked after scaling or storing. 417 KB for a
                 picture drawn at 144 px is not something to leave on somebody's card.
    the picture  is the only thing that survives: the sprite at disc_art_path() for a
                 disc, the libretro-layout PNG at art_cache_path() for a cover.

  Nothing here is reachable in a build we ship: it needs ss_enabled(), which needs
  ss_available(), which is compile-time false. It also needs classicui_artfetch, which
  is what keeps the test harness off the network.
*/
#define SSF_IDLE   0
#define SSF_QUERY  1      // waiting for the jeuInfos reply
#define SSF_IMAGE  2      // waiting for the picture the reply named

static int   ssf_state = SSF_IDLE;
static pid_t ssf_pid = -1;
static char  ssf_key[128];
static char  ssf_dst[1024];
static char  ssf_reply[256];
static char  ssf_tmp[256];

// Where curl writes the HTTP status of whichever of the two requests is in flight. See
// curl_spawn()'s `status`, and ssf_http_code() for what is done with it.
static char  ssf_status[256];

// Which of the two this fetch is for, and the shelf item it belongs to - SS_KIND_DISC and
// -1 for a disc, which has no shelf item to belong to.
static int   ssf_kind = SS_KIND_DISC;
static int   ssf_item = -1;

/*
  And that item's identity as well as its index, because the index alone is not a name.

  A background scan that finds more games, or Options > Rescan Library, renumbers the item
  array while a fetch is in flight - two round trips is a long time for that to happen in.
  The index would still be in range and would still resolve, to a different game: this
  fetch's cover would be stored under that game's name, or - worse, because it is silent
  and permanent for the session - that game would be marked as having no cover on the
  strength of a reply about something else. chome_item.key is a hash of the system and the
  path, so comparing it is how the index is checked for still meaning what it meant.
*/
static uint32_t ssf_item_key = 0;

// The media URL the reply named, kept from the settle to the download stage. Never
// printed: it carries devid, devpassword, ssid and sspassword. See ss_redact_url().
static char  ssf_media[SS_URL_LEN];

// One attempt per disc per session. A disc the database has never heard of must not be
// asked for on every frame of the dialog, and unlike a game there is no shelf slot to
// hang the answer on - so a small list of keys does the job the ss_absent flag does for a
// cover. See the note on it in disc_art_request().
#define DISC_TRIED_MAX 8
static char disc_tried[DISC_TRIED_MAX][128];
static int  disc_ntried = 0;

// Anything is in flight. The one-at-a-time rule is about the account and the network, so
// it does not care which kind is holding the slot.
static int ss_fetch_active()
{
	return ssf_pid > 0;
}

/*
  Whereas this one is about the disc dialog's spinner, so it does care: a cover being
  fetched for the shelf must not put a "fetching the disc scan" spinner over a dialog that
  is going to go on drawing the generated disc face.
*/
int disc_art_active()
{
	return (ssf_pid > 0 && ssf_kind == SS_KIND_DISC) ? 1 : 0;
}

// The shelf item this fetch is for, or -1 - which covers a disc fetch, no slots at all, and
// a library that has been renumbered under us. See ssf_item_key.
static int ssf_cover_item()
{
	if (ssf_kind != SS_KIND_COVER) return -1;
	if (!slots || ssf_item < 0 || ssf_item >= nslots) return -1;

	chome_item *it = lib_item(ssf_item);
	if (!it || it->key != ssf_item_key) return -1;

	return ssf_item;
}

// Raised where the picture is finished and handed over once. See disc_art_take_ready().
static int disc_ready = 0;

int disc_art_take_ready()
{
	int r = disc_ready;
	disc_ready = 0;
	return r;
}

static unsigned disc_asks = 0;

unsigned disc_art_asks()
{
	return disc_asks;
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
static const char *ssf_curl_why(pid_t r, int status, char *buf, int len)
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

// Which refusal the database gave, in words, is ss_why() in chome_ss.cpp. It used to be a
// private table here; the cover ladder wanted the same words, and a second copy of it
// would have drifted from this one the first time a code was added to the enum.

// Bytes on disk, or -1. Only ever for a log line, so a missing file is not an error.
static long long ssf_file_size(const char *path)
{
	struct stat st;
	if (!path || !path[0] || stat(path, &st)) return -1;
	return (long long)st.st_size;
}

static int disc_already_tried(const char *key)
{
	for (int i = 0; i < disc_ntried; i++) if (!strcmp(disc_tried[i], key)) return 1;
	return 0;
}

static void disc_mark_tried(const char *key)
{
	if (disc_already_tried(key)) return;

	// Full: forget the oldest. A dialog opened over eight different discs in one
	// session may re-ask for the first of them, which is a request, not a bug.
	if (disc_ntried >= DISC_TRIED_MAX)
	{
		for (int i = 1; i < DISC_TRIED_MAX; i++) memcpy(disc_tried[i - 1], disc_tried[i], sizeof(disc_tried[0]));
		disc_ntried = DISC_TRIED_MAX - 1;
	}

	snprintf(disc_tried[disc_ntried++], sizeof(disc_tried[0]), "%s", key);
}

/*
  The HTTP status of the request that just finished, or 0 when there is not one to be had.

  0 rather than a guess, and every caller treats 0 as "no information" rather than as any
  particular fault: an old curl that did not write the file, a fork that never got as far
  as exec, and a request that genuinely never reached a server all land here, and none of
  them is evidence about the account.
*/
static int ssf_http_code()
{
	if (!ssf_status[0]) return 0;

	FILE *f = fopen(ssf_status, "rb");
	if (!f) return 0;

	char buf[32];
	size_t n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);

	if (!n) return 0;
	buf[n] = 0;

	int code = atoi(buf);
	return (code >= 100 && code <= 599) ? code : 0;
}

static void ssf_reset()
{
	if (ssf_reply[0]) unlink(ssf_reply);
	if (ssf_tmp[0]) unlink(ssf_tmp);
	if (ssf_status[0]) unlink(ssf_status);
	ssf_state = SSF_IDLE;
	ssf_pid = -1;
	ssf_key[0] = 0;
	ssf_dst[0] = 0;
	ssf_reply[0] = 0;
	ssf_tmp[0] = 0;
	ssf_status[0] = 0;
	ssf_kind = SS_KIND_DISC;
	ssf_item = -1;
	ssf_item_key = 0;

	// The media URL goes with the rest of it. It is the one field here that is a
	// credential, and leaving a spent one in a static for the rest of the session would be
	// a password sitting in the process image for no reason at all.
	memset(ssf_media, 0, sizeof(ssf_media));
}

/*
  Create the reply file 0600 before curl is handed it, and remove anything left there
  first. Shared by both kinds, because the reason is the same for both and it is the sort
  of thing that gets done in one copy and forgotten in the other.

  curl -o keeps the permissions of a file that already exists and otherwise creates one at
  the mercy of the umask, which on this rootfs means 0644. The reply is a list of URLs with
  our devid, our devpassword and the player's ScreenScraper password in every one of them -
  133 copies of them in the reply this was written against - so for the couple of frames it
  exists it should not be readable by anything else on the box.

  The unlink matters for the same reason from the other direction: a firmware process
  killed mid-fetch leaves one behind, and a core change restarts the firmware without
  clearing tmpfs, so the next request would otherwise inherit a stale credential dump and
  its permissions.
*/
static void ssf_make_reply_private(const char *path)
{
	unlink(path);

	int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0600);
	if (fd >= 0) close(fd);
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
	disc_asks++;

	/*
	  A local, not ssf_dst. ssf_dst belongs to whatever fetch is in flight, and writing this
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
	if (ssf_pid > 0) return !strcmp(ssf_key, key);

	if (!cfg.classicui_artfetch) return 0;

	/*
	  ss_may_request() rather than ss_enabled(), and above disc_already_tried() rather than
	  below it, which is a small ordering with a real consequence.

	  The module stands down for the session when the quota runs out. If that check came
	  after the tried-list, a disc asked about while the allowance was gone would burn its
	  one attempt on a request that was never made and be unaskable for the rest of the
	  session; asking first means the disc keeps its attempt for whenever the hold is not
	  the reason any more. It also means the dialog's per-frame call costs nothing at all
	  while the hold stands - it returns here, before a fork.

	  Not the whole of the fix a cover gets. A cover distinguishes the two refusals per
	  game, because there is a shelf slot to record it in and thousands of games at stake;
	  a disc is marked tried on the attempt whatever the outcome, because the dialog asks on
	  every pass of its draw and an unconditional guard is the only thing that stops a
	  transport failure becoming a fork per frame. The asymmetry is deliberate, it is
	  bounded at eight keys, and a core change clears it.
	*/
	if (!ss_may_request()) return 0;
	if (fetch_pid > 0) return 0;
	if (disc_already_tried(key)) return 0;

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

	snprintf(ssf_dst, sizeof(ssf_dst), "%s", want);
	snprintf(ssf_reply, sizeof(ssf_reply), "/tmp/classicui_discart.xml");
	snprintf(ssf_status, sizeof(ssf_status), "/tmp/classicui_discart.status");
	snprintf(ssf_key, sizeof(ssf_key), "%s", key);
	ssf_tmp[0] = 0;
	ssf_kind = SS_KIND_DISC;
	ssf_item = -1;

	ssf_make_reply_private(ssf_reply);

	// 1: this URL carries devid, devpassword, ssid and sspassword. Down a pipe, not argv.
	ssf_pid = curl_spawn(url, ssf_reply, 1, ssf_status);
	if (ssf_pid < 0) { ssf_reset(); return 0; }

	ssf_state = SSF_QUERY;
	disc_mark_tried(key);

	/*
	  The key and the systemeid, never the URL: the URL has devid, devpassword, ssid and
	  sspassword in it. The systemeid is in here because it is the other half of what was
	  asked - a scan that never arrives for a disc the database certainly holds is a
	  different fault if we asked the wrong platform for it, and it is the only part of
	  the query that is guessed from a table rather than read off the disc.
	*/
	printf("ClassicUI: asking for a disc scan for %s (system %s, as \"%s\")\n",
		ssf_key, systemeid, name);
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
static void disc_box_average(const uint32_t *sp, int sw, int sh, uint32_t *dp, int dw, int dh)
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

	disc_box_average(sp, sw, sh, dp, dw, dh);
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
	disc_ready = 1;
	return 1;
}

/*
  The noun this fetch is about, and the media type it went looking for.

  Every line the fetch prints goes through these, because two kinds now share the path and
  a log that said "the fetch failed" for both would be a log nobody could attribute - the
  same reason the key is in every line.
*/
static const char *ssf_what()
{
	return (ssf_kind == SS_KIND_DISC) ? "disc scan" : "cover";
}

static const char *ssf_wanted()
{
	return (ssf_kind == SS_KIND_DISC) ? "support-2D" : "cover art";
}

/*
  What a landed reply means: parse it, fold it into the module's state, say so, and pick the
  media to download.

  Returns 1 when there is one, having left its URL in ssf_media.

  This is the half of the fetch that decides whether a game is written off, and it is the
  reason chome_ss.h has a whole section on the two refusals. The rule, in one place:

    the reply was a verdict and named nothing we can use   remember it against the game
    the reply was a refusal                                remember it against the module

  Getting that backwards is not a cosmetic bug. A quota exhausted halfway through a first
  boot would, under a single "we tried and got nothing" flag, mark every remaining game on
  the shelf as having no art - permanently, silently, and with no way back but a rescan the
  player has no reason to think of. ss_verdict() is the guard, and it is checked here rather
  than at the four call sites that could each have got it wrong.
*/
static int ssf_settle(const char *reply_path, long long got)
{
	/*
	  Static, not a local. ss_result holds SS_MAX_MEDIA url[512] buffers - ~53 KB - and
	  this runs on the thread that draws, whose stack is not the place for it.
	*/
	static ss_result res;
	int e = ss_parse_file(reply_path, &res);

	// Parsed or not, the reply goes now. It is a list of URLs with our devid and the
	// player's password in every one of them, sitting in a file.
	unlink(reply_path);

	/*
	  The counters go in whatever the outcome, and `res` is handed over even when the parse
	  failed: ss_parse_file() zeroes it and sets the counters to -1 first, so a reply that
	  carried none is safely silent, and a well-formed reply that merely named no game did
	  carry them. That is the free half of the quota - see ss_note_result().
	*/
	ss_note_result(e, &res);

	memset(ssf_media, 0, sizeof(ssf_media));

	const ss_media *m = 0;

	if (e == SS_OK && res.nmedia)
	{
		if (ssf_kind == SS_KIND_DISC)
		{
			/*
			  Region from the disc, which for a PlayStation disc is in the serial that is
			  also its identity key. Nothing derivable means no preference at all rather
			  than a house default, which lands ss_pick() on the first support-2D in reply
			  order.
			*/
			const char *regs[4];
			int nr = ss_regions_for_serial(ssf_key, regs, 4);
			m = ss_pick(&res, SS_KIND_DISC, nr ? regs : 0);
		}
		else
		{
			/*
			  And no preference at all for a cover, deliberately.

			  A region could be guessed from the No-Intro tag in the file name - "(Europe)",
			  "(USA)" - and it is tempting, because a European player looking at a Japanese
			  box is a visible wrong answer. It is not done here because it would be a
			  second guess stacked on a first: the tag would have to be mapped to the API's
			  region spellings, of which one reply has shown us seven, and a preference for
			  a region the reply does not carry lands back on reply order anyway. The server
			  lists its own best first - Dinofly's rule, and the same rule the disc scan falls
			  back to - so reply order is the answer until there is a real reply to check a
			  mapping against.
			*/
			m = ss_pick(&res, SS_KIND_COVER, 0);
		}
	}

	if (m) snprintf(ssf_media, sizeof(ssf_media), "%s", m->url);

	if (e != SS_OK)
	{
		printf("ClassicUI: the %s reply for %s (%lld bytes) says: %s\n",
			ssf_what(), ssf_key, got, ss_why(e));
	}
	else if (!res.nmedia)
	{
		printf("ClassicUI: the %s reply for %s names no media at all\n", ssf_what(), ssf_key);
	}
	else if (!m)
	{
		/*
		  The game is in the database and has pictures, just not this kind. Worth
		  distinguishing from "no media at all": that one is a bad match on the name and
		  would be fixed by asking differently, this one is a gap in the database and no
		  amount of asking will fill it. The types that were there are listed because it is
		  also how a change in what the API calls a disc scan - or a cover - would show; see
		  the top of chome_ss.cpp on that being the soft spot.
		*/
		printf("ClassicUI: the %s reply for %s has %d media but no %s:",
			ssf_what(), ssf_key, res.nmedia, ssf_wanted());
		for (int i = 0; i < res.nmedia && i < 12; i++) printf(" %s", res.media[i].type);
		printf("%s\n", res.nmedia > 12 ? " ..." : "");
	}
	else
	{
		// Type and region, never the URL - see ss_redact_url() for what is in one.
		printf("ClassicUI: %s for %s is %s/%s\n", ssf_what(), ssf_key, m->type,
			m->region[0] ? m->region : "no region");
	}

	/*
	  The allowance, printed from every reply that carried it. This is the number that says
	  whether a shelf that stopped filling up stopped because the database has nothing or
	  because the account has nothing left, and it is the one thing a live run needs in the
	  log that no amount of host testing can produce.
	*/
	if (res.max_requests_day > 0)
	{
		printf("ClassicUI: ScreenScraper allowance: %d of %d requests today",
			res.requests_today, res.max_requests_day);
		if (res.max_requests_ko_day > 0)
			printf(", %d of %d unmatched", res.requests_ko_today, res.max_requests_ko_day);
		printf("\n");
	}

	/*
	  And the one decision this function exists to make.

	  Only when the reply was the database answering about the game, and only for a cover -
	  a disc has no shelf slot to record it in and uses the tried-list instead, see
	  disc_art_request(). "Answered and named nothing we can use" covers both halves of a
	  real miss: the game is not in there at all, and the game is in there with media but
	  no cover among them. Neither will change by tomorrow, so neither is worth a second
	  request.

	  Everything else falls through here untouched, which is the entire point. A quota, a
	  429, a closed API, a dropped connection: the game is left exactly as unasked as it was
	  before, and ss_note_result() above has already held the module off so that the next
	  game does not spend a request discovering the same thing.
	*/
	int cover_item = ssf_cover_item();

	if (!m && ss_verdict(e) && cover_item >= 0)
	{
		slots[cover_item].ss_absent = 1;
		printf("ClassicUI: ScreenScraper has no cover for %s, falling back to the pack\n",
			ssf_key);
	}

	return m ? 1 : 0;
}

int art_ss_settle(int item, const char *reply_path)
{
	// Not while a fetch is in flight: this writes the fields that fetch is using, and a
	// caller that settled a reply mid-download would land the picture under another name.
	if (ss_fetch_active()) return 0;
	if (!reply_path || !reply_path[0]) return 0;

	chome_item *it = lib_item(item);

	ssf_kind = SS_KIND_COVER;
	ssf_item = item;
	ssf_item_key = it ? it->key : 0;
	snprintf(ssf_key, sizeof(ssf_key), "%s", it ? it->title : "?");

	return ssf_settle(reply_path, ssf_file_size(reply_path));
}

/*
  The end of a fetch, however it ended.

  A cover is handed back to the queue so that the ladder is walked again for it, and that
  one line is what makes the fallback work at all: on success find_local_art() now finds
  the file that was just written, and on any failure the rung below - the libretro pack -
  gets its turn on the very next pass. There is no separate "and now try the other source"
  path, because a second copy of the ladder is exactly what would drift from the first.
*/
static void ssf_finish()
{
	int item = ssf_cover_item();

	ssf_reset();

	if (item >= 0) art_requeue(item);
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
    it named media but not the kind wanted     it is in there without that picture
    the picture downloaded and would not store disc_art_scale() / store_download() say why
    there was nowhere to write it              also those two

  So each of them prints, and each print distinguishes itself from the others rather than
  saying "the fetch failed" six times. The key is in every line, because two fetches in one
  session otherwise leave a log nobody can attribute; the URL is in none of them, because
  every media URL in that reply carries devid, devpassword, ssid and sspassword -
  ss_redact_url() is there for the case where one genuinely has to be shown, and this is
  not one of those cases.
*/
static void ss_fetch_poll()
{
	if (ssf_pid <= 0) return;

	int status = 0;
	pid_t r = waitpid(ssf_pid, &status, WNOHANG);
	if (!r) return;

	int ok = (r > 0) && WIFEXITED(status) && !WEXITSTATUS(status);
	ssf_pid = -1;

	char why[128];

	/*
	  What the server said, before anything is concluded from the exit status alone.

	  This is the distinction the whole feature turns on, arriving at the one place it can
	  be made. curl -f exits 22 for every HTTP error and keeps none of the body, so from
	  here a 429 rate limit, a 430 spent quota, a 403 refusal and a server that hung up all
	  look identical - and the right response to the first three is to stop asking while the
	  right response to the fourth is not to. ss_http_class() knows which is which; this is
	  what finally gives it something to read.

	  0 means the status is unknown, and then the exit status is all there is: transport,
	  which holds nothing and writes off nobody.
	*/
	int http = ssf_http_code();
	int hclass = http ? ss_http_class(http) : SS_OK;
	int failed_as = (hclass != SS_OK) ? hclass : SS_ERR_TRANSPORT;

	if (ssf_state == SSF_QUERY)
	{
		if (!ok)
		{
			printf("ClassicUI: the %s query for %s failed, curl %s (HTTP %d: %s)\n",
				ssf_what(), ssf_key, ssf_curl_why(r, status, why, sizeof(why)),
				http, ss_why(failed_as));

			/*
			  Told to the module as whatever the status says it was, and nothing is written
			  off against the game either way: ss_verdict() is false for every one of these,
			  so a 429 in the middle of a shelf leaves every card it touched still worth
			  asking about. What differs between them is only whether the module keeps
			  asking - see ss_note_result().
			*/
			ss_note_result(failed_as, 0);
			ssf_finish();
			return;
		}

		long long got = ssf_file_size(ssf_reply);
		if (got <= 0)
		{
			// curl -f discards the body of an HTTP error, so a zero-byte reply and a
			// missing one are the same fault from here: it answered, with nothing.
			printf("ClassicUI: the %s query for %s came back with no reply (%lld bytes,"
				" HTTP %d)\n", ssf_what(), ssf_key, got, http);
			ss_note_result(failed_as, 0);
			ssf_finish();
			return;
		}

		// ssf_settle() unlinks it, so this field is cleared first: ssf_reset() would
		// otherwise unlink a path that has already gone, or worse, one reused since.
		char reply[sizeof(ssf_reply)];
		snprintf(reply, sizeof(reply), "%s", ssf_reply);
		ssf_reply[0] = 0;

		if (!ssf_settle(reply, got)) { ssf_finish(); return; }

		snprintf(ssf_tmp, sizeof(ssf_tmp), "%s", (ssf_kind == SS_KIND_DISC) ?
			"/tmp/classicui_discart_src" : "/tmp/classicui_cover_src");

		// 1 again: the media URL out of the reply carries the same four credentials.
		ssf_pid = curl_spawn(ssf_media, ssf_tmp, 1, ssf_status);
		if (ssf_pid < 0)
		{
			printf("ClassicUI: could not fork a curl for the %s for %s (%s)\n",
				ssf_what(), ssf_key, strerror(errno));
			ssf_finish();
			return;
		}

		ssf_state = SSF_IMAGE;
		return;
	}

	if (ssf_state == SSF_IMAGE)
	{
		long long got = ssf_file_size(ssf_tmp);

		if (!ok)
		{
			printf("ClassicUI: the %s download for %s failed, curl %s (HTTP %d)\n",
				ssf_what(), ssf_key, ssf_curl_why(r, status, why, sizeof(why)), http);

			// The media endpoint authenticates the same way and throttles the same way, so a
			// refusal here is the same news about the account as a refusal to the query. The
			// picture is lost either way; what this buys is not asking for the next one.
			ss_note_result(failed_as, 0);
		}
		else if (got <= 0)
		{
			printf("ClassicUI: the %s for %s downloaded nothing (%lld bytes)\n",
				ssf_what(), ssf_key, got);
		}
		else if (ssf_kind == SS_KIND_DISC)
		{
			if (!disc_art_scale(ssf_tmp, ssf_dst))
			{
				// disc_art_scale() has already said which of load, directory or save it was.
				printf("ClassicUI: the disc scan for %s downloaded (%lld bytes) but would not"
					" scale into %s\n", ssf_key, got, ssf_dst);
			}
			else
			{
				// The file, named: it is the one thing on the card afterwards, and "it is
				// not there" was half of what there was to go on when this was silent.
				printf("ClassicUI: disc scan stored for %s: %s (%lld bytes from %lld)\n",
					ssf_key, ssf_dst, ssf_file_size(ssf_dst), got);
			}
		}
		else
		{
			/*
			  A cover is stored as it arrives rather than scaled, and it goes to
			  art_cache_path() - the same libretro-layout file the pack fetch writes. So a
			  ScreenScraper cover populates the local pack exactly as a libretro one does,
			  the next boot finds it at rung one and needs no network, and decode_into()
			  resizes it to the card at decode time as it already does for every other
			  source. Nothing about the drawing path has to know where a cover came from.
			*/
			mkdirs(ssf_dst);

			if (!store_download(ssf_tmp, ssf_dst))
			{
				printf("ClassicUI: the cover for %s downloaded (%lld bytes) but would not"
					" store into %s\n", ssf_key, got, ssf_dst);
			}
			else
			{
				printf("ClassicUI: cover stored for %s: %s (%lld bytes)\n",
					ssf_key, ssf_dst, ssf_file_size(ssf_dst));
			}
		}

		// The original goes either way. ssf_reset() unlinks it.
		ssf_finish();
		return;
	}

	ssf_reset();
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

/* ---------------------------------------------------------------- ladder --- */

int art_ss_absent(int item)
{
	if (!slots || item < 0 || item >= nslots) return 0;
	return slots[item].ss_absent ? 1 : 0;
}

static unsigned ss_asks = 0;

unsigned art_ss_asks()
{
	return ss_asks;
}

/*
  The file name to ask the database for, which is not the one the art packs are named
  after.

  rom_base() strips the extension, because that is what a libretro thumbnail is called.
  jeuInfos.php wants the ROM file as it sits on the card, extension included - romnom is
  matched against the database's own file names - so this keeps it. It also feeds
  ss_system_id(), which reads the extension to tell a .gbc from a .gb and a .gg from a
  .sms: two systems that ride in another core's shelf and are a different platform to the
  API. Handing it a name with the extension cut off would silently scrape every Game Boy
  Color game as a Game Boy one.

  An archive is named rather than its member, the same way rom_relpath() does it, because
  that is the name the ROM is distributed and catalogued under.
*/
static void rom_filename(const chome_item *it, char *out, int len)
{
	char work[CH_PATH_LEN];
	rom_relpath(it, work, sizeof(work));

	const char *fn = strrchr(work, '/');
	snprintf(out, len, "%s", fn ? fn + 1 : work);
}

/*
  Whether a ScreenScraper query for this game can be built at all.

  Asked in the ladder rather than discovered inside the request, so that a game we cannot
  ask about drops straight to the libretro pack instead of being reported as a rung that
  then quietly refuses. Two things can stop it, and both are properties of the game rather
  than of the moment:

    no systemeid    the platform is not in ss_system_id()'s table, or is one it refuses to
                    guess at - a .gg in the Master System shelf. Asking anyway would scrape
                    somebody else's platform and put the wrong box on the card.
    no artdir path  art_cache_path() needs the system's libretro name to build one, and
                    without it a downloaded cover has nowhere to live. The libretro rung
                    needs the same thing, so such a system has no network art at all.
*/
static int ss_can_ask(const chome_item *it)
{
	const chome_sys *s = lib_sys(it->sysidx);
	if (!s) return 0;

	char dst[1024];
	if (!art_cache_path(it, dst, sizeof(dst))) return 0;

	char name[CH_PATH_LEN];
	rom_filename(it, name, sizeof(name));

	return ss_system_id(s->id, name) ? 1 : 0;
}

/*
  The ladder, in order, as one answer. See the top of chome_art.h for why the order is what
  it is; this is that order and nothing else, so that art_step() below and the harness are
  reading the same thing rather than two accounts of it.

  Pure: it stats the card looking for rung one, and does not fetch, fork or record
  anything.
*/
static int art_source_for(int item, char *local, int len)
{
	if (!slots || item < 0 || item >= nslots) return ART_SRC_NONE;

	chome_item *it = lib_item(item);
	if (!it) return ART_SRC_NONE;

	// 1. the card. Whatever is already there wins, whether the player scraped it or a
	//    previous run of one of the two rungs below wrote it.
	if (find_local_art(it, local, len)) return ART_SRC_LOCAL;

	if (!cfg.classicui_artfetch) return ART_SRC_NONE;

	const art_slot *s = &slots[item];

	/*
	  2. ScreenScraper, when the player has turned it on with an account and it has neither
	     answered about this game nor stood down.

	     ss_may_request() is the module-wide half of the two refusals and s->ss_absent is
	     the per-game half. Both have to be clear, and which of them is set is exactly what
	     decides whether this game comes back to this rung later: a game the database has no
	     cover for never returns, a game passed over while the quota was gone returns the
	     moment the hold does.
	*/
	if (!s->ss_absent && ss_may_request() && ss_can_ask(it)) return ART_SRC_SS;

	// 3. the libretro pack. Needs the system's libretro name, the same as rung two does.
	const chome_sys *sys = lib_sys(it->sysidx);
	if (!s->tried_fetch && sys && sys->lr[0]) return ART_SRC_LIBRETRO;

	return ART_SRC_NONE;
}

int art_next_source(int item)
{
	char local[1024];
	return art_source_for(item, local, sizeof(local));
}

/*
  Ask ScreenScraper for one game's cover. Returns 1 when a query is on its way.

  The shape is disc_art_request()'s, and the parts that are the same are the same code:
  ssf_make_reply_private() for the 0600 reply, curl_spawn(..., 1) for a URL that must not
  reach argv, and the one ssf_ state machine to reap it. What differs is only the query.
*/
static int cover_ss_start(int item)
{
	/*
	  Counted above every refusal below, for the reason disc_art_asks() is: "did the ladder
	  reach this rung" is a different question from "did it get anywhere", and the first one
	  is the shape of the feature. Not printed - the shelf can walk a lot of cards.
	*/
	ss_asks++;

	if (ss_fetch_active() || fetch_pid > 0) return 0;
	if (!cfg.classicui_artfetch) return 0;
	if (!ss_may_request()) return 0;

	chome_item *it = lib_item(item);
	if (!it) return 0;

	const chome_sys *sys = lib_sys(it->sysidx);
	if (!sys) return 0;

	char dst[1024];
	if (!art_cache_path(it, dst, sizeof(dst))) return 0;

	char name[CH_PATH_LEN];
	rom_filename(it, name, sizeof(name));

	const char *systemeid = ss_system_id(sys->id, name);
	if (!systemeid) return 0;

	/*
	  Name and size, and deliberately no hash.

	  chome_ss.h describes hashing anything under SS_HASH_MAX_BYTES and matching it
	  properly, which is what a PC scraper does and what gets the best match rate. It is not
	  done here, and the reason is the discipline this whole file is built on: one stage per
	  frame, nothing that blocks the draw loop. An md5 of even a 4 MB cartridge off a FAT
	  card on an 800 MHz ARM is tens of milliseconds inside the frame that draws the shelf,
	  and at SS_HASH_MAX_BYTES it is the better part of a second - a visible stall every time
	  the player scrolls onto an unscraped card. Doing it properly means hashing in slices
	  across frames, with the partial state held per item, and that is a piece of work of its
	  own rather than a line here.

	  So romnom and romtaille, which is the pair the API provides for exactly this case.
	*/
	long long size = 0;
	if (!strcasestr(it->path, ".zip/"))
	{
		/*
		  Not for a game inside an archive. What could be measured there is the size of the
		  zip around the ROM, and romtaille is matched against the size of the ROM - so
		  sending it would narrow the match to nothing at all rather than narrow it usefully.
		  A zipped game is matched on its name alone, which is what the packs name it by too.
		*/
		char gd[1024];
		if (lib_sys_games_dir(it->sysidx, gd, sizeof(gd)))
		{
			char full[1024];
			snprintf(full, sizeof(full), "%s/%s", gd, it->path);

			struct stat st;
			if (!stat(full, &st) && S_ISREG(st.st_mode)) size = (long long)st.st_size;
		}
	}

	ss_query q;
	memset(&q, 0, sizeof(q));
	q.systemeid = systemeid;
	q.romnom = name;
	q.romtaille = size;

	char url[1400];
	if (!ss_build_url(&q, 0, url, sizeof(url))) return 0;

	snprintf(ssf_dst, sizeof(ssf_dst), "%s", dst);
	snprintf(ssf_reply, sizeof(ssf_reply), "/tmp/classicui_cover.xml");
	snprintf(ssf_status, sizeof(ssf_status), "/tmp/classicui_cover.status");
	snprintf(ssf_key, sizeof(ssf_key), "%s", it->title);
	ssf_tmp[0] = 0;
	ssf_kind = SS_KIND_COVER;
	ssf_item = item;
	ssf_item_key = it->key;

	ssf_make_reply_private(ssf_reply);

	// 1: this URL carries devid, devpassword, ssid and sspassword. Down a pipe, not argv.
	ssf_pid = curl_spawn(url, ssf_reply, 1, ssf_status);
	if (ssf_pid < 0) { ssf_reset(); return 0; }

	ssf_state = SSF_QUERY;
	slots[item].state = ART_FETCHING;

	// The title, the systemeid and the name asked under; never the URL. The systemeid is
	// here for the same reason it is in the disc line: it is the one part of the query
	// guessed from a table, and a wrong one matches a real game on the wrong platform.
	printf("ClassicUI: asking ScreenScraper for a cover for %s (system %s, as \"%s\")\n",
		it->title, systemeid, name);
	return 1;
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
	ss_fetch_poll();

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
	int src = art_source_for(item, path, sizeof(path));

	if (src == ART_SRC_LOCAL)
	{
		if (decode_into(path, s, plate)) return;
		s->state = ART_MISSING;
		return;
	}

	if (src == ART_SRC_SS || src == ART_SRC_LIBRETRO)
	{
		/*
		  One download at a time, whoever wants it - and an item that arrives while the one
		  slot is busy is put back rather than given up on.

		  ART_NONE, not ART_MISSING: art_request() refuses to re-queue a missing item, so
		  the old code's ART_MISSING here meant that a card whose turn came up during
		  somebody else's download was blank for the rest of the session. That was already
		  wrong and rarely visible, because one fetch is quick and the queue is short. With
		  two network rungs a fetch now occupies the slot for two round trips instead of one,
		  which is long enough for it to have become the common case rather than the odd one.
		  ART_NONE is re-requestable, and the shelf asks again for every card it draws.
		*/
		if (ss_fetch_active() || fetch_pid > 0) { s->state = ART_NONE; return; }

		if (src == ART_SRC_SS)
		{
			if (cover_ss_start(item)) return;

			// It refused after the ladder said it would not - a fork that failed, in
			// practice. Put the item back rather than write it off: nothing has been learnt
			// about the game, and the rung below it has not had its turn.
			s->state = ART_NONE;
			return;
		}

		s->tried_fetch = 1;
		if (fetch_start(item)) return;
	}

	s->state = ART_MISSING;
}
