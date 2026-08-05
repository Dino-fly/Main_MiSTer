#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

#include "chome_ss.h"
#include "../../cfg.h"

#include "../../sxmlc.h"          // has its own extern "C" guard

/*
  See chome_ss.h for why none of this is live, and for the credential problem.

  One thing to settle against the first real reply, and it is written here rather
  than buried in a commit message because it is the one weak spot in this file:

  The *JSON* shape of a jeuInfos reply is confirmed - response.jeu, a flat medias
  array whose entries carry type/region/format/url, and response.ssuser carrying
  the quota counters. The *XML* shape of the same reply is not confirmed. API v1
  nested media per region and per kind into element names
  (medias/media_boxs/media_boxs3d/media_box3d_eu); v2 flattened it, but whether
  the flattened form puts type/region/url in attributes, in child elements, or the
  URL in the element text is not something to take on trust from second-hand
  descriptions.

  So the parser below reads all three placements and does not care about nesting
  depth: it takes any <media> element anywhere, an attribute if there is one, the
  element text if there is not. That is deliberately loose, and it means the first
  live reply must be dumped to a file and read by a human before this is trusted.
  If it turns out the XML is awkward, only ss_parse_file() changes - the URL
  builder, the classifier and the picker are independent of the wire format.
*/

/* --------------------------------------------------------------- the gate --- */

/*
  CLASSICUI_SS_DEVID / CLASSICUI_SS_DEVPASS are build-time only, and undefined in
  every build we ship. The test build defines them with dummy values so everything
  below stays covered.

  On a machine that has real credentials they arrive through this header, written by
  support/classicui/tools/ss_creds.sh from a file outside the repository and left in
  bin/ where nothing tracks it. Deliberately not a -D: that would put the password in
  every build log and in `ps` while the compiler ran. __has_include rather than a
  makefile conditional so a tree without the header still compiles on its own.

  These identify the *application*. The player's own ScreenScraper account is a
  separate pair, read from MiSTer.ini at runtime (classicui_ss_user / _ss_pass), and
  that is the one that earns a player their own request quota rather than sharing the
  guest pool.
*/
/*
  Two guards, both learned the hard way when this was added.

  CLASSICUI_SS_DEVID already being set means a -D wins over the file: the main harness
  passes dummy credentials that way and would otherwise collide with a real header on
  the developer's own machine, and a build that says "testdev" on the command line must
  mean it.

  CLASSICUI_SS_NO_CREDS is how the gate binary says "compile me the way we ship". Its
  whole purpose is to prove that the shipped configuration cannot reach the network, and
  it compiles this same file with no devid - so picking up a local credentials header
  made it fail the moment the developer had credentials, which is precisely when that
  reassurance is worth having. The shipped configuration is a property of the build, not
  of whose machine it is on, so the gate states it rather than inferring it.
*/
#if !defined(CLASSICUI_SS_DEVID) && !defined(CLASSICUI_SS_NO_CREDS) && defined(__has_include)
#if __has_include("bin/ss_credentials.h")
#include "bin/ss_credentials.h"
#endif
#endif

#ifdef CLASSICUI_SS_DEVID
#define SS_HAVE_DEV 1
#else
#define SS_HAVE_DEV 0
#define CLASSICUI_SS_DEVID   ""
#define CLASSICUI_SS_DEVPASS ""
#endif

#ifndef CLASSICUI_SS_SOFTNAME
#define CLASSICUI_SS_SOFTNAME "classichome"
#endif

#define SS_API_BASE "https://www.screenscraper.fr/api2/jeuInfos.php"

int ss_available()
{
	return SS_HAVE_DEV;
}

int ss_enabled()
{
	if (!ss_available()) return 0;
	if (!cfg.classicui_screenscraper) return 0;

	// Anonymous use of API v2 is not a thing: without an account the server answers
	// every request with a login error, so "on but no account" is off, not degraded.
	if (!cfg.classicui_ss_user[0]) return 0;

	return 1;
}

/* ------------------------------------------------------------- system ids --- */

/*
  Taken from a client that works against the live API rather than from memory, and
  cross-checked against its own source: muldjord/skyscraper, getPlatformId().

  The gaps are gaps on purpose. A wrong systemeid does not error - it matches a
  real game on the wrong platform and puts the wrong cover on the shelf, which is
  worse than no cover and much harder to notice. Anything not listed here waits
  either for systemesListe.php or for a line in the override file.
*/
struct ss_sysmap
{
	const char *sysid;
	const char *ssid;
};

static const ss_sysmap builtin[] =
{
	{ "nes",    "3"   },
	{ "snes",   "4"   },
	{ "md",     "1"   },
	{ "sms",    "2"   },
	{ "gb",     "9"   },
	{ "gba",    "12"  },
	{ "n64",    "14"  },
	{ "psx",    "57"  },
	{ "tg16",   "31"  },
	{ "neogeo", "142" },
	{ "arcade", "75"  },
};

// A .gbc in the Game Boy shelf is a different platform to the API than a .gb.
#define SS_ID_GBC "10"

#define SS_OVERRIDE_MAX 64

struct ss_override
{
	char sysid[16];
	char ssid[12];
};

static ss_override overrides[SS_OVERRIDE_MAX];
static int noverrides = 0;

void ss_systems_forget()
{
	noverrides = 0;
}

int ss_systems_load(const char *path)
{
	ss_systems_forget();

	FILE *f = fopen(path, "r");
	if (!f) return 0;

	char line[128];
	while (noverrides < SS_OVERRIDE_MAX && fgets(line, sizeof(line), f))
	{
		char *p = line;
		while (*p == ' ' || *p == '\t') p++;
		if (*p == '#' || *p == ';' || *p == '\n' || *p == '\r' || !*p) continue;

		char *eq = strchr(p, '=');
		if (!eq) continue;
		*eq = 0;

		char *key = p;
		char *val = eq + 1;

		// Trim both sides. The file is hand-edited, and a trailing \r from an editor
		// on Windows would otherwise become part of the number.
		for (char *e = key + strlen(key); e > key && (e[-1] == ' ' || e[-1] == '\t'); e--) e[-1] = 0;
		while (*val == ' ' || *val == '\t') val++;
		for (char *e = val + strlen(val); e > val &&
			(e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t'); e--) e[-1] = 0;

		if (!*key || !*val) continue;

		// Numbers only: this string goes straight into a URL.
		int ok = 1;
		for (const char *d = val; *d; d++) if (!isdigit((unsigned char)*d)) ok = 0;
		if (!ok) continue;

		ss_override *o = &overrides[noverrides++];
		snprintf(o->sysid, sizeof(o->sysid), "%s", key);
		snprintf(o->ssid, sizeof(o->ssid), "%s", val);
	}

	fclose(f);
	return noverrides;
}

static const char *ext_of(const char *name)
{
	if (!name) return 0;
	const char *dot = strrchr(name, '.');
	return dot ? dot + 1 : 0;
}

const char *ss_system_id(const char *sysid, const char *romnom)
{
	if (!sysid || !sysid[0]) return 0;

	// The override file wins, so a machine can correct or extend the table without
	// a rebuild - including correcting one of the built-ins if it ever goes stale.
	for (int i = 0; i < noverrides; i++)
	{
		if (!strcasecmp(overrides[i].sysid, sysid)) return overrides[i].ssid;
	}

	const char *ext = ext_of(romnom);

	if (!strcasecmp(sysid, "gb") && ext && !strcasecmp(ext, "gbc")) return SS_ID_GBC;

	/*
	  Game Gear rides in the Master System shelf as .gg, and it is a distinct
	  platform to the API - but its systemeid is not one of the values verified
	  above, so this returns nothing rather than scraping .gg games as Master
	  System and quietly filling the shelf with the wrong covers.
	*/
	if (!strcasecmp(sysid, "sms") && ext && !strcasecmp(ext, "gg")) return 0;

	for (size_t i = 0; i < sizeof(builtin) / sizeof(builtin[0]); i++)
	{
		if (!strcasecmp(builtin[i].sysid, sysid)) return builtin[i].ssid;
	}

	return 0;
}

/* --------------------------------------------------------------- the URL ---- */

static void ss_urlenc(const char *in, char *out, int len)
{
	static const char *hex = "0123456789ABCDEF";
	int o = 0;
	for (int i = 0; in && in[i] && o < len - 4; i++)
	{
		unsigned char c = (unsigned char)in[i];
		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
			c == '-' || c == '_' || c == '.' || c == '~')
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
	if (len > 0) out[o] = 0;
}

int ss_build_url(const ss_query *q, int redact, char *out, int len)
{
	if (!out || len <= 0) return 0;
	out[0] = 0;

	if (!SS_HAVE_DEV) return 0;
	if (!q || !q->systemeid || !q->systemeid[0]) return 0;
	if (!q->romnom || !q->romnom[0]) return 0;

	const char *devpass = redact ? "***" : CLASSICUI_SS_DEVPASS;
	const char *userpass = redact ? "***" : cfg.classicui_ss_pass;

	char e_dev[128], e_devpass[128], e_soft[64];
	char e_user[192], e_userpass[192];
	char e_rom[512];

	ss_urlenc(CLASSICUI_SS_DEVID, e_dev, sizeof(e_dev));
	// The redaction marker is written through rather than encoded: percent-encoded
	// asterisks (%2A%2A%2A) in a log line are noise, and the point of the marker is
	// that a human reading the log sees at a glance that something was removed.
	if (redact) snprintf(e_devpass, sizeof(e_devpass), "***");
	else ss_urlenc(devpass, e_devpass, sizeof(e_devpass));
	ss_urlenc(CLASSICUI_SS_SOFTNAME, e_soft, sizeof(e_soft));
	ss_urlenc(cfg.classicui_ss_user, e_user, sizeof(e_user));
	if (redact) snprintf(e_userpass, sizeof(e_userpass), "***");
	else ss_urlenc(userpass, e_userpass, sizeof(e_userpass));
	ss_urlenc(q->romnom, e_rom, sizeof(e_rom));

	int n = snprintf(out, len,
		SS_API_BASE "?devid=%s&devpassword=%s&softname=%s&output=xml"
		"&ssid=%s&sspassword=%s&systemeid=%s&romtype=rom&romnom=%s",
		e_dev, e_devpass, e_soft, e_user, e_userpass, q->systemeid, e_rom);

	if (n < 0 || n >= len) { out[0] = 0; return 0; }

	// Everything past here is optional and only narrows the match, so each one is
	// appended only if it fits - a truncated URL would be a request that fails for
	// a reason nobody could see.
	char tail[256];

	if (q->romtaille > 0)
	{
		int t = snprintf(tail, sizeof(tail), "&romtaille=%lld", q->romtaille);
		if (t > 0 && n + t < len) { memcpy(out + n, tail, t + 1); n += t; }
	}

	struct { const char *key; const char *val; } hashes[] =
	{
		{ "md5",  q->md5  },
		{ "sha1", q->sha1 },
		{ "crc",  q->crc  },
	};

	for (size_t i = 0; i < sizeof(hashes) / sizeof(hashes[0]); i++)
	{
		if (!hashes[i].val || !hashes[i].val[0]) continue;
		char e[128];
		ss_urlenc(hashes[i].val, e, sizeof(e));
		int t = snprintf(tail, sizeof(tail), "&%s=%s", hashes[i].key, e);
		if (t > 0 && n + t < len) { memcpy(out + n, tail, t + 1); n += t; }
	}

	return n;
}

/* ------------------------------------------------------------ classifying --- */

int ss_http_class(int http_code)
{
	switch (http_code)
	{
	case 200: return SS_OK;
	case 400: return SS_ERR_MALFORMED;      // our parameters are wrong, not the server's fault
	case 401: return SS_ERR_CLOSED;         // API shut off while the server is saturated
	case 403: return SS_ERR_CREDENTIALS;
	case 404: return SS_ERR_NOTFOUND;
	case 429: return SS_ERR_THREADS;
	case 430: return SS_ERR_QUOTA;

	/*
	  431 is "too many ROMs the server could not recognise". It is a throttle aimed
	  at clients that fling nonsense at the database, not a permanent ban - but the
	  right response is the same as a ban: stop, and let a human look at why our
	  match rate is bad.
	*/
	case 431: return SS_ERR_BLACKLISTED;

	case 423: return SS_ERR_CLOSED;
	}

	if (http_code >= 500) return SS_ERR_TRANSPORT;
	if (http_code == 0)   return SS_ERR_TRANSPORT;

	return SS_OK;
}

// Case-insensitive substring. strcasestr is a GNU extension and this has to build
// against the test host as well as the device.
static int ci_has(const char *hay, const char *needle)
{
	if (!hay || !needle || !*needle) return 0;

	size_t nl = strlen(needle);
	for (const char *p = hay; *p; p++)
	{
		if (!strncasecmp(p, needle, nl)) return 1;
	}
	return 0;
}

/*
  The API says most of this twice: an HTTP status and a French sentence in the
  body. Neither alone covers everything - a 200 can carry "quota de scrape" - so
  both are checked and the body is trusted over a 200.

  Matched on accent-free fragments deliberately: the body is UTF-8 and the site is
  French, so "trouvée" and "fermé" carry multi-byte characters whose exact encoding
  is not worth depending on. "non trouv" is as distinctive and cannot mismatch.
*/
int ss_body_class(const char *body)
{
	if (!body) return SS_OK;

	if (ci_has(body, "blacklist"))            return SS_ERR_BLACKLISTED;
	if (ci_has(body, "quota de scrape"))      return SS_ERR_QUOTA;
	if (ci_has(body, "totalement ferm"))      return SS_ERR_CLOSED;
	if (ci_has(body, "non trouv"))            return SS_ERR_NOTFOUND;
	if (ci_has(body, "erreur de login"))      return SS_ERR_CREDENTIALS;
	if (ci_has(body, "verifiez vos identifiants")) return SS_ERR_CREDENTIALS;
	if (ci_has(body, "identifiant"))          return SS_ERR_CREDENTIALS;

	/*
	  There is deliberately no body match for the thread limit.

	  The obvious one - the word "threads" - is in *every successful reply*, because
	  the ssuser block carries <maxthreads>. Matching it classified a perfectly good
	  game reply as a thread error and threw the game away; the fixture in the
	  harness is what caught it. The exact French wording of the real message is not
	  something we know, and guessing a longer fragment would be the same mistake
	  with a smaller blast radius, so the thread limit is recognised by HTTP 429
	  alone, which is documented.
	*/

	return SS_OK;
}

/* --------------------------------------------------------------- parsing ---- */

struct ss_ctx
{
	ss_result *r;
	int in_ssuser;
	int field;                 // which ssuser counter the text belongs to, -1 for none
	int in_media;
	ss_media cur;
	char text[SS_URL_LEN];
	int text_len;
	int saw_jeu;
};

#define SSF_NONE      -1
#define SSF_REQTODAY   0
#define SSF_MAXDAY     1
#define SSF_MAXTHREADS 2

static int ssuser_field(const char *tag)
{
	if (!strcasecmp(tag, "requeststoday"))     return SSF_REQTODAY;
	if (!strcasecmp(tag, "maxrequestsperday")) return SSF_MAXDAY;
	if (!strcasecmp(tag, "maxthreads"))        return SSF_MAXTHREADS;
	return SSF_NONE;
}

static const char *attr_of(const XMLNode *node, const char *name)
{
	if (!node) return 0;
	for (int i = 0; i < node->n_attributes; i++)
	{
		if (!node->attributes[i].name) continue;
		if (!strcasecmp(node->attributes[i].name, name)) return node->attributes[i].value;
	}
	return 0;
}

static void media_commit(ss_ctx *c)
{
	if (c->r->nmedia >= SS_MAX_MEDIA) return;
	if (!c->cur.url[0]) return;                      // a media with no URL is no use
	if (strncasecmp(c->cur.url, "http", 4)) return;  // and nor is one that is not a URL

	c->r->media[c->r->nmedia++] = c->cur;
}

static int sax(XMLEvent evt, const XMLNode *node, SXML_CHAR *text, const int n, SAX_Data *sd)
{
	(void)n;
	ss_ctx *c = (ss_ctx*)sd->user;

	switch (evt)
	{
	case XML_EVENT_START_NODE:
		if (!node || !node->tag) break;

		c->text_len = 0;
		c->text[0] = 0;
		c->field = SSF_NONE;

		if (!strcasecmp(node->tag, "ssuser")) { c->in_ssuser = 1; break; }

		if (!strcasecmp(node->tag, "jeu"))
		{
			c->saw_jeu = 1;
			const char *id = attr_of(node, "id");
			if (id) snprintf(c->r->gameid, sizeof(c->r->gameid), "%s", id);
			break;
		}

		if (!strcasecmp(node->tag, "media"))
		{
			c->in_media = 1;
			memset(&c->cur, 0, sizeof(c->cur));

			// Attributes if they are there; text and child elements are picked up
			// below for the placements we have not been able to confirm.
			const char *t = attr_of(node, "type");
			const char *rg = attr_of(node, "region");
			const char *fm = attr_of(node, "format");
			const char *u = attr_of(node, "url");

			if (t)  snprintf(c->cur.type,   sizeof(c->cur.type),   "%s", t);
			if (rg) snprintf(c->cur.region, sizeof(c->cur.region), "%s", rg);
			if (fm) snprintf(c->cur.format, sizeof(c->cur.format), "%s", fm);
			if (u)  snprintf(c->cur.url,    sizeof(c->cur.url),    "%s", u);

			// A self-closing <media .../> never gets an END_NODE, so commit now.
			if (node->tag_type == TAG_SELF) { media_commit(c); c->in_media = 0; }
			break;
		}

		if (c->in_ssuser) c->field = ssuser_field(node->tag);
		break;

	case XML_EVENT_TEXT:
		// Appended rather than assigned: sxmlc stops at every '>', so text
		// containing one arrives in pieces. Same reason as chome_gamelist.cpp.
		if (!text) break;
		{
			int len = (int)strlen(text);
			if (len > (int)sizeof(c->text) - 1 - c->text_len) len = (int)sizeof(c->text) - 1 - c->text_len;
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

		if (!strcasecmp(node->tag, "ssuser")) { c->in_ssuser = 0; c->field = SSF_NONE; break; }

		if (!strcasecmp(node->tag, "media"))
		{
			/*
			  Guarded on in_media, which the self-closing case has already cleared.
			  sxmlc reports a <media/> as both a start and an end node, so committing
			  unconditionally here stored every self-closing media twice - the
			  "all five media are taken" fixture is what caught it.
			*/
			if (c->in_media)
			{
				// The URL as element text, which is the placement we could not confirm
				// from the documentation. Only used when there was no url attribute.
				if (!c->cur.url[0] && c->text_len) snprintf(c->cur.url, sizeof(c->cur.url), "%s", c->text);
				media_commit(c);
				c->in_media = 0;
			}
			break;
		}

		// type/region/format as child elements of <media>, the other unconfirmed
		// placement.
		if (c->in_media)
		{
			if (!c->cur.type[0]   && !strcasecmp(node->tag, "type"))   snprintf(c->cur.type,   sizeof(c->cur.type),   "%s", c->text);
			if (!c->cur.region[0] && !strcasecmp(node->tag, "region")) snprintf(c->cur.region, sizeof(c->cur.region), "%s", c->text);
			if (!c->cur.format[0] && !strcasecmp(node->tag, "format")) snprintf(c->cur.format, sizeof(c->cur.format), "%s", c->text);
			if (!c->cur.url[0]    && !strcasecmp(node->tag, "url"))    snprintf(c->cur.url,    sizeof(c->cur.url),    "%s", c->text);
			c->text_len = 0;
			c->text[0] = 0;
			break;
		}

		if (c->in_ssuser && c->field != SSF_NONE)
		{
			int v = atoi(c->text);
			if (c->field == SSF_REQTODAY)   c->r->requests_today = v;
			if (c->field == SSF_MAXDAY)     c->r->max_requests_day = v;
			if (c->field == SSF_MAXTHREADS) c->r->max_threads = v;
			c->field = SSF_NONE;
		}

		c->text_len = 0;
		c->text[0] = 0;
		break;

	default:
		break;
	}

	return true;
}

int ss_parse_file(const char *path, ss_result *out)
{
	if (!out) return SS_ERR_MALFORMED;

	memset(out, 0, sizeof(*out));
	out->requests_today = -1;
	out->max_requests_day = -1;
	out->max_threads = -1;
	out->err = SS_ERR_MALFORMED;

	if (!path) return out->err;

	/*
	  The body is read once as text first, so the French error sentences can be
	  classified even when the reply is not well-formed XML - which is exactly what
	  an error reply from this API sometimes is. Bounded, because this is a network
	  response: only the head of it is needed to recognise an error.
	*/
	FILE *f = fopen(path, "rb");
	if (!f) return (out->err = SS_ERR_TRANSPORT);

	char head[4096];
	size_t got = fread(head, 1, sizeof(head) - 1, f);
	head[got] = 0;
	fclose(f);

	if (!got) return (out->err = SS_ERR_TRANSPORT);

	/*
	  Only classify the body when it is not a data reply.

	  The error sentences are matched on short fragments, and a reply describing a
	  real game can contain one of them by coincidence - a French title, a publisher
	  name. A reply that carries a <jeu> element is an answer, so it is never read
	  as an error however its text reads. The opening tag arrives well inside the
	  first block: only the ssuser counters precede it.
	*/
	if (!ci_has(head, "<jeu"))
	{
		int body = ss_body_class(head);
		if (body != SS_OK) return (out->err = body);
	}

	ss_ctx c;
	memset(&c, 0, sizeof(c));
	c.r = out;
	c.field = SSF_NONE;

	SAX_Callbacks cb;
	SAX_Callbacks_init(&cb);
	cb.all_event = sax;

	// Third argument is the user pointer that reaches sax() as sd->user.
	if (!XMLDoc_parse_file_SAX(path, &cb, &c)) return (out->err = SS_ERR_MALFORMED);

	// Parsed cleanly but describes no game: that is an answer, not a failure, and
	// the caller should remember it rather than ask again tomorrow.
	if (!c.saw_jeu) return (out->err = SS_ERR_NOTFOUND);

	out->err = SS_OK;
	return out->err;
}

/* ---------------------------------------------------------------- picking --- */

/*
  Type before region, which is the opposite of what Skyscraper does.

  Its reason for region-first is that a user wants their own local box. Ours is
  that the shelf card is a flat rectangle: a box-3D render dropped into it is a
  photographed box at an angle, and it looks wrong next to flat covers in a way a
  Japanese cover next to an American one does not. So the best *kind* is chosen
  first, and region decides between the ones of that kind.
*/
static const char *const cover_types[]  = { "box-2D", "box-3D", "mixrbv2", "mixrbv1", "ss", 0 };
static const char *const screen_types[] = { "ss", "sstitle", "mixrbv1", 0 };
static const char *const wheel_types[]  = { "wheel", "wheel-hd", "screenmarquee", 0 };

const ss_media *ss_pick(const ss_result *r, int kind, const char *const *regions)
{
	if (!r || r->err != SS_OK) return 0;

	const char *const *types =
		(kind == SS_KIND_SCREEN) ? screen_types :
		(kind == SS_KIND_WHEEL)  ? wheel_types  : cover_types;

	for (int t = 0; types[t]; t++)
	{
		if (regions)
		{
			for (int g = 0; regions[g]; g++)
			{
				for (int i = 0; i < r->nmedia; i++)
				{
					if (strcasecmp(r->media[i].type, types[t])) continue;
					if (strcasecmp(r->media[i].region, regions[g])) continue;
					return &r->media[i];
				}
			}
		}

		/*
		  Then anything of this kind whatever its region, which also covers the
		  entries that carry no region at all. Taken last rather than dropped: a
		  cover with no region is still the right cover.
		*/
		for (int i = 0; i < r->nmedia; i++)
		{
			if (!strcasecmp(r->media[i].type, types[t])) return &r->media[i];
		}
	}

	return 0;
}
