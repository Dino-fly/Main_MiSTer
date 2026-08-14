#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "chome_gamelist.h"
#include "chome_lib.h"
#include "../../cfg.h"

#include "../../sxmlc.h"          // has its own extern "C" guard

/*
  The XML is read by sxmlc, the parser already in this tree: user_io.cpp reads .mra
  files with it and the Neo Geo loader reads romsets.xml with it. Writing another
  XML parser for a file format we do not own would be the wrong kind of confidence.

  XMLDoc_parse_file_SAX() streams - it reads up to the next '>' at a time rather
  than loading the file - so a 20 MB gamelist costs a tag's worth of memory, not
  20 MB. What does grow without bound is one stretch with no '>' in it: a giant
  comment, or a renamed binary that has none at all, is read into a single buffer.
  That is what the size check below really guards, and it is why it happens before
  the parser is handed anything.

  A UTF-8 BOM needs no handling. sxmlc only skips one in its Unicode build, which
  this is not, so the three bytes arrive as text before the first '<' - and text
  outside a field is dropped, which is exactly what should happen to them.
*/

/* ------------------------------------------------------------- storage ----- */

struct gl_ent
{
	uint64_t hpath;      // normalised path relative to the games dir
	uint64_t hbase;      // its last component only
	int32_t  off;        // art path, into the arena. -1 when the entry names none
	/*
	  <name>, into the arena, or -1.

	  Not metadata for its own sake - this shelf draws its own titles from the file name and
	  has no use for a scraper's. It is here because it is a *file name*: the layout Skraper
	  and the Recalbox/ES packs write names each picture after the game's <name> rather than
	  after the ROM, so media/box2d/<name>.png cannot be found without reading this. See
	  find_local_art() in chome_art.cpp for where it is used.
	*/
	int32_t  noff;
	int16_t  sysidx;
};

static gl_ent *ents = 0;
static int nents = 0;
static int ents_cap = 0;

static char *arena = 0;
static int arena_used = 0;
static int arena_cap = 0;

static uint8_t sys_loaded[CH_MAX_SYS];
static uint8_t sys_rejected[CH_MAX_SYS];
static int16_t sys_count[CH_MAX_SYS];

void gl_forget()
{
	free(ents);
	ents = 0;
	nents = 0;
	ents_cap = 0;

	free(arena);
	arena = 0;
	arena_used = 0;
	arena_cap = 0;

	memset(sys_loaded, 0, sizeof(sys_loaded));
	memset(sys_rejected, 0, sizeof(sys_rejected));
	memset(sys_count, 0, sizeof(sys_count));
}

static int arena_put(const char *s)
{
	int need = (int)strlen(s) + 1;
	if (arena_used + need > GL_ARENA_MAX) return -1;

	if (arena_used + need > arena_cap)
	{
		int want = arena_cap ? arena_cap : (16 * 1024);
		while (want < arena_used + need) want *= 2;
		if (want > GL_ARENA_MAX) want = GL_ARENA_MAX;

		char *n = (char*)realloc(arena, want);
		if (!n) return -1;
		arena = n;
		arena_cap = want;
	}

	int off = arena_used;
	memcpy(arena + off, s, need);
	arena_used += need;
	return off;
}

static gl_ent *ent_new()
{
	if (nents >= GL_MAX_ENTRIES) return 0;

	if (nents >= ents_cap)
	{
		int want = ents_cap ? ents_cap * 2 : 256;
		if (want > GL_MAX_ENTRIES) want = GL_MAX_ENTRIES;

		gl_ent *n = (gl_ent*)realloc(ents, sizeof(gl_ent) * want);
		if (!n) return 0;
		ents = n;
		ents_cap = want;
	}

	gl_ent *e = &ents[nents++];
	memset(e, 0, sizeof(*e));
	return e;
}

/* --------------------------------------------------------------- keys ----- */

/*
  Two files are the same game when their paths are, and paths from two programs
  agree only after the spelling differences are taken out: a leading "./" (ES
  writes one, other tools do not), Windows separators, and case, since the card is
  FAT and its own lookups ignore it.

  64-bit FNV-1a rather than the 32-bit hash chome_lib.cpp uses for item keys: a
  collision there costs a wrong play count, here it would put another game's cover
  on a card, and 4096 entries in 64 bits makes that not worth thinking about.
*/
static uint64_t fnv1a(const char *s, int len)
{
	uint64_t h = 14695981039346656037ull;
	for (int i = 0; i < len; i++)
	{
		unsigned char c = (unsigned char)s[i];
		if (c == '\\') c = '/';
		if (c >= 'A' && c <= 'Z') c += 32;
		h ^= c;
		h *= 1099511628211ull;
	}
	return h;
}

static const char *strip_dotslash(const char *p)
{
	while (p[0] == '.' && p[1] == '/') p += 2;
	while (p[0] == '/') p++;
	return p;
}

static void keys_of(const char *relpath, uint64_t *hpath, uint64_t *hbase)
{
	const char *p = strip_dotslash(relpath);
	int len = (int)strlen(p);
	while (len > 0 && (p[len - 1] == ' ' || p[len - 1] == '\r' || p[len - 1] == '\n')) len--;

	*hpath = fnv1a(p, len);

	int b = len;
	while (b > 0 && p[b - 1] != '/' && p[b - 1] != '\\') b--;
	*hbase = fnv1a(p + b, len - b);
}

/* ------------------------------------------------------------- parsing ---- */

/*
  Which tags name a picture, best first.

  <boxart> is Batocera's own field for the 2D box and says exactly what it holds,
  so it wins. <thumbnail> comes next because in EmulationStation's own vocabulary
  that is the box front while <image> is the "main" picture, which scrapers fill
  with a screenshot or a composite as often as with a box - and this shelf draws
  boxes. <image> is still third and does most of the work in practice, since a
  gamelist that names only one picture names it there.

  Everything else a gamelist can carry is left out on purpose: <marquee> and
  <wheel> are logos on transparency, <video>, <manual>, <magazine>, <map>,
  <bezel>, <cartridge> and <boxback> are not the front of a box, and putting any
  of them on a card would look like a bug.
*/
static const char *const art_tags[] =
{
	"boxart",
	"thumbnail",
	"image",
	"mix",
	"titleshot",
	"fanart",
};
#define ART_TAG_COUNT ((int)(sizeof(art_tags) / sizeof(art_tags[0])))

#define GL_TEXT_MAX 512

// The two fields that are not pictures, given numbers past the end of art_tags so that one
// `field` covers all of them and the rank comparison below can never mistake one for a
// picture.
#define GL_FIELD_PATH (ART_TAG_COUNT)
#define GL_FIELD_NAME (ART_TAG_COUNT + 1)

struct gl_ctx
{
	int sysidx;
	int in_game;
	int field;                  // index into art_tags, GL_FIELD_PATH/NAME, -1 none
	int text_len;
	char text[GL_TEXT_MAX];
	char path[GL_TEXT_MAX];
	char name[GL_TEXT_MAX];
	char art[GL_TEXT_MAX];
	int  art_rank;
	int  full;                  // hit a cap
	int  error;                 // malformed
};

// &amp; and friends, in place. pugixml - what ES and Batocera write with - escapes
// text rather than wrapping it in CDATA, so this is the only decoding needed.
static void unescape(char *s)
{
	char *o = s;
	for (char *p = s; *p; )
	{
		if (*p != '&') { *o++ = *p++; continue; }

		if (!strncmp(p, "&amp;", 5))       { *o++ = '&';  p += 5; }
		else if (!strncmp(p, "&lt;", 4))   { *o++ = '<';  p += 4; }
		else if (!strncmp(p, "&gt;", 4))   { *o++ = '>';  p += 4; }
		else if (!strncmp(p, "&quot;", 6)) { *o++ = '"';  p += 6; }
		else if (!strncmp(p, "&apos;", 6)) { *o++ = '\''; p += 6; }
		else if (p[1] == '#')
		{
			char *end = 0;
			long cp = (p[2] == 'x' || p[2] == 'X') ? strtol(p + 3, &end, 16) : strtol(p + 2, &end, 10);

			if (end && *end == ';' && cp > 0 && cp < 0x10000)
			{
				// UTF-8, which is what a filename on the card is.
				if (cp < 0x80) *o++ = (char)cp;
				else if (cp < 0x800)
				{
					*o++ = (char)(0xc0 | (cp >> 6));
					*o++ = (char)(0x80 | (cp & 0x3f));
				}
				else
				{
					*o++ = (char)(0xe0 | (cp >> 12));
					*o++ = (char)(0x80 | ((cp >> 6) & 0x3f));
					*o++ = (char)(0x80 | (cp & 0x3f));
				}
				p = end + 1;
			}
			else *o++ = *p++;              // not a reference after all: keep the '&'
		}
		else *o++ = *p++;                  // unknown entity: leave it alone
	}
	*o = 0;
}

static void trim_text(char *s)
{
	char *p = s;
	while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
	if (p != s) memmove(s, p, strlen(p) + 1);

	char *e = s + strlen(s);
	while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
}

static int tag_field(const char *tag)
{
	for (int i = 0; i < ART_TAG_COUNT; i++) if (!strcasecmp(tag, art_tags[i])) return i;
	if (!strcasecmp(tag, "path")) return GL_FIELD_PATH;
	if (!strcasecmp(tag, "name")) return GL_FIELD_NAME;
	return -1;
}

/*
  1 when this picture path is one we are willing to resolve.

  "~/..." is a path on the machine that did the scraping, and MiSTer has no home
  directory worth resolving it against. ".." could name anything on the card and a
  gamelist has no business doing so. Both are dropped rather than guessed at.
*/
static int art_usable(const char *art)
{
	if (!art[0]) return 0;
	if (art[0] == '~') return 0;
	if (strstr(art, "..")) return 0;
	return 1;
}

/*
  An entry is worth keeping when it names a picture *or* a name, which is a widening: it
  used to be pictures only.

  A <name> with no picture beside it is exactly the ES-DE and Skraper-romset case - the
  gamelist says what the game is called and the pictures are matched to that name on disk
  under media/box2d/ - so an entry thrown away for naming no <image> is the entry that
  would have found the cover. Entries that name neither are still dropped: they are a
  hash and two -1s that nothing can ever answer with.
*/
static void commit(gl_ctx *c)
{
	if (!c->path[0]) return;

	int usable = art_usable(c->art);
	if (!usable && !c->name[0]) return;

	int off = -1;
	if (usable)
	{
		off = arena_put(c->art);
		if (off < 0) { c->full = 1; return; }
	}

	int noff = -1;
	if (c->name[0])
	{
		noff = arena_put(c->name);
		if (noff < 0) { c->full = 1; return; }
	}

	if (off < 0 && noff < 0) return;

	gl_ent *e = ent_new();
	if (!e) { c->full = 1; return; }

	e->sysidx = (int16_t)c->sysidx;
	e->off = off;
	e->noff = noff;
	keys_of(c->path, &e->hpath, &e->hbase);
}

static int sax(XMLEvent evt, const XMLNode *node, SXML_CHAR *text, const int n, SAX_Data *sd)
{
	gl_ctx *c = (gl_ctx*)sd->user;

	switch (evt)
	{
	case XML_EVENT_START_NODE:
		if (!node || !node->tag) break;
		if (node->tag_type != TAG_FATHER && node->tag_type != TAG_SELF) break;

		if (!strcasecmp(node->tag, "game") || !strcasecmp(node->tag, "folder"))
		{
			c->in_game = strcasecmp(node->tag, "game") ? 0 : 1;
			c->path[0] = 0;
			c->name[0] = 0;
			c->art[0] = 0;
			c->art_rank = ART_TAG_COUNT;
			c->field = -1;
			break;
		}

		/*
		  Guarded on in_game, which a <folder> clears above: a folder carries <image>
		  too, and it names a directory the shelf has no card for, so nothing inside
		  one is read.
		*/
		if (c->in_game) c->field = tag_field(node->tag);
		c->text_len = 0;
		c->text[0] = 0;
		break;

	case XML_EVENT_TEXT:
		/*
		  Text can arrive in more than one piece - sxmlc stops at every '>', so a
		  '>' inside text splits it - hence appending rather than assigning.
		*/
		if (c->field < 0 || !text) break;
		{
			int len = (int)strlen(text);
			if (len > GL_TEXT_MAX - 1 - c->text_len) len = GL_TEXT_MAX - 1 - c->text_len;
			if (len > 0)
			{
				memcpy(c->text + c->text_len, text, len);
				c->text_len += len;
				c->text[c->text_len] = 0;
			}
		}
		break;

	case XML_EVENT_END_NODE:
		if (!node || !node->tag) break;

		if (!strcasecmp(node->tag, "game"))
		{
			if (c->in_game) commit(c);
			c->in_game = 0;
			c->field = -1;
			if (c->full) return false;         // caps reached: stop reading the file
			break;
		}
		if (!strcasecmp(node->tag, "folder"))
		{
			c->field = -1;
			break;
		}

		if (c->in_game && c->field >= 0)
		{
			unescape(c->text);
			trim_text(c->text);

			if (c->text[0])
			{
				if (c->field == GL_FIELD_PATH) snprintf(c->path, sizeof(c->path), "%s", c->text);
				else if (c->field == GL_FIELD_NAME) snprintf(c->name, sizeof(c->name), "%s", c->text);
				else if (c->field < c->art_rank)
				{
					snprintf(c->art, sizeof(c->art), "%s", c->text);
					c->art_rank = c->field;
				}
			}
		}
		c->field = -1;
		c->text_len = 0;
		c->text[0] = 0;
		break;

	case XML_EVENT_ERROR:
		/*
		  Malformed, and there is no telling how much of what came before was read
		  the way it was meant. Everything this file contributed is thrown away by
		  the caller; the shelf then behaves as though the file were not there.
		*/
		printf("ClassicUI: gamelist.xml: parse error %d at line %d, ignoring the file\n", n, sd ? sd->line_num : 0);
		c->error = 1;
		return false;

	default:
		break;
	}

	return true;
}

/* -------------------------------------------------------------- loading --- */

static void gl_load(int sysidx)
{
	sys_loaded[sysidx] = 1;

	char dir[1024];
	if (!lib_sys_games_dir(sysidx, dir, sizeof(dir))) return;

	char path[1152];
	snprintf(path, sizeof(path), "%s/gamelist.xml", dir);

	struct stat st;
	if (stat(path, &st) || !S_ISREG(st.st_mode) || !st.st_size) return;

	if (st.st_size > GL_MAX_BYTES)
	{
		printf("ClassicUI: %s is %lld bytes, too large to be a gamelist - ignoring\n",
			path, (long long)st.st_size);
		sys_rejected[sysidx] = 1;
		return;
	}

	int ents0 = nents;
	int arena0 = arena_used;

	gl_ctx c;
	memset(&c, 0, sizeof(c));
	c.sysidx = sysidx;
	c.field = -1;
	c.art_rank = ART_TAG_COUNT;

	SAX_Callbacks sax_cb;
	SAX_Callbacks_init(&sax_cb);
	sax_cb.all_event = sax;

	XMLDoc_parse_file_SAX(path, &sax_cb, &c);

	if (c.error)
	{
		// Roll the file back out. A half-read gamelist is not a smaller gamelist.
		nents = ents0;
		arena_used = arena0;
		sys_rejected[sysidx] = 1;
		sys_count[sysidx] = 0;
		return;
	}

	/*
	  A cap, unlike a parse error, does not make what was read wrong - it only makes
	  it short - so those entries stay and the rest of the file is not read.
	*/
	if (c.full)
	{
		printf("ClassicUI: gamelist.xml: %s is larger than the %d-entry table, the rest is not read\n",
			dir, GL_MAX_ENTRIES);
		sys_rejected[sysidx] = 1;
	}

	sys_count[sysidx] = (int16_t)(nents - ents0);
	if (sys_count[sysidx]) printf("ClassicUI: %s/gamelist.xml -> %d covers\n", dir, sys_count[sysidx]);
}

/* -------------------------------------------------------------- lookup ---- */

static int file_exists(const char *p)
{
	struct stat st;
	return (!stat(p, &st) && S_ISREG(st.st_mode)) ? 1 : 0;
}

static int resolve(int sysidx, const char *rel, char *out, int len)
{
	if (rel[0] == '/')
	{
		snprintf(out, len, "%s", rel);
		return file_exists(out);
	}

	char dir[1024];
	if (!lib_sys_games_dir(sysidx, dir, sizeof(dir))) return 0;

	snprintf(out, len, "%s/%s", dir, strip_dotslash(rel));
	return file_exists(out);
}

/*
  The entry for one game, or 0.

  `want_name` picks which of the two fields the caller needs, and entries that have not got
  it are passed over rather than matched and then found empty. That matters because the two
  fields are populated independently now: a gamelist can name a picture for one game, a
  <name> for another, and both for a third, and a lookup that stopped at the first entry
  with the right path would answer "no name" for a file whose name is in the entry the
  filename fallback would have found.

  The path is what identifies a game, and it is tried first. The last component on its own
  is a fallback for gamelists whose <path> is not relative to the games dir the way ES
  writes it - an absolute path from a PC is the case that turns up - and it is second
  because it cannot tell two same-named files in different folders apart, so a real path
  match must always beat it.
*/
static const gl_ent *gl_find(int sysidx, const char *relpath, int want_name)
{
	if (sysidx < 0 || sysidx >= CH_MAX_SYS || !relpath || !*relpath) return 0;
	if (!cfg.classicui_gamelist) return 0;

	if (!sys_loaded[sysidx]) gl_load(sysidx);
	if (!sys_count[sysidx]) return 0;

	uint64_t hpath, hbase;
	keys_of(relpath, &hpath, &hbase);

	const gl_ent *base_hit = 0;
	for (int i = 0; i < nents; i++)
	{
		if (ents[i].sysidx != sysidx) continue;
		if ((want_name ? ents[i].noff : ents[i].off) < 0) continue;

		if (ents[i].hpath == hpath) return &ents[i];
		if (!base_hit && ents[i].hbase == hbase) base_hit = &ents[i];
	}

	return base_hit;
}

int gl_art(int sysidx, const char *relpath, char *out, int len)
{
	const gl_ent *e = gl_find(sysidx, relpath, 0);
	if (!e) return 0;

	return resolve(sysidx, arena + e->off, out, len);
}

int gl_name(int sysidx, const char *relpath, char *out, int len)
{
	const gl_ent *e = gl_find(sysidx, relpath, 1);
	if (!e) return 0;

	snprintf(out, len, "%s", arena + e->noff);
	return out[0] ? 1 : 0;
}

int gl_loaded(int sysidx)
{
	if (sysidx < 0 || sysidx >= CH_MAX_SYS) return 0;
	return sys_loaded[sysidx];
}

int gl_count(int sysidx)
{
	if (sysidx < 0 || sysidx >= CH_MAX_SYS) return 0;
	return sys_count[sysidx];
}

int gl_rejected(int sysidx)
{
	if (sysidx < 0 || sysidx >= CH_MAX_SYS) return 0;
	return sys_rejected[sysidx];
}
