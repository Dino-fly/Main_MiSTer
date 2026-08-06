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

  The XML shape is now settled, which it was not when this was written.

  What was known then was the *JSON* shape - response.jeu, a flat medias array whose
  entries carry type/region/format/url, and response.ssuser carrying the quota
  counters. The XML shape of the same reply was a guess: API v1 nested media per region
  and per kind into element names (medias/media_boxs/media_boxs3d/media_box3d_eu), v2
  flattened it, and whether the flattened form put type/region/url in attributes, in
  child elements, or the URL in the element text was not something to take on trust
  from second-hand descriptions. So the parser was written to read all three
  placements, and this comment said the first live reply had to be read by a human.

  One was, on 2026-08-05, for a PlayStation game. The answer:

    type, region, format, crc, md5, sha1 and size are ATTRIBUTES, and the URL is the
    element TEXT. There is no url attribute.

  Which is one of the three placements the parser already handled, so nothing here had
  to change for it - the loose reading paid for itself. Two things about that reply did
  matter, and both are dealt with rather than described:

    - it carried 133 <media> elements against a store of 64. See the cap and the filter
      in chome_ss.h; the short version is that the overflow discarded exactly the types
      the picker wanted least often and needed most.
    - every media URL in it embeds devid, devpassword, ssid and sspassword. See
      ss_redact_url(), and do not save a reply anywhere but tmpfs.

  The remaining soft spot is smaller and different: one reply, one system. The nesting
  and the placement are confirmed; the region spellings for other consoles, and whether
  every system's medias block looks the same, are not.
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

const char *ss_why(int err)
{
	switch (err)
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

/* ------------------------------------------------- holding the module off --- */

/*
  Whether the module is allowed to ask, and the reason it is not.

  This is small, and it is the piece that decides whether a bad afternoon costs a player
  their covers for one session or for good. The rule it enforces is the one at the top of
  the header: a refusal holds the *module*, a verdict is remembered against the *game*,
  and the two states never touch. Nothing here knows what a game is, which is the point -
  it cannot record a miss even by accident.
*/
static int ss_hold = SS_OK;

int ss_hold_reason()
{
	return ss_hold;
}

int ss_may_request()
{
	if (!ss_enabled()) return 0;
	if (ss_hold != SS_OK) return 0;
	return 1;
}

void ss_forget_state()
{
	ss_hold = SS_OK;
}

int ss_verdict(int err)
{
	return (err == SS_OK || err == SS_ERR_NOTFOUND) ? 1 : 0;
}

// One place to raise the hold, so that every reason for it prints in the same shape and
// the first reason wins. First rather than last because the reasons are not equal: a
// blacklisting arriving after a quota must not read as though the quota were the story.
static void ss_hold_off(int why, const char *detail)
{
	if (ss_hold != SS_OK) return;

	ss_hold = why;
	printf("ClassicUI: ScreenScraper stands down for this session - %s%s%s\n",
		ss_why(why), detail ? ": " : "", detail ? detail : "");
}

void ss_note_result(int err, const ss_result *r)
{
	/*
	  The counters first, and from every reply rather than only from a refusal.

	  This is the half that costs nothing. ssuser rides along with the game data, so a
	  successful reply that happens to be the last one the allowance covers says so
	  itself - and standing down on it means the next request is never made, rather than
	  being made, refused, and counted against us. Reacting only to the refusal would
	  spend a request to learn something the previous reply already contained.

	  Both limits are checked for being positive before being compared against. They
	  initialise to -1 for "the reply did not carry them", and a reply from a server that
	  had stopped sending them would otherwise read as an allowance of -1 already
	  exceeded, and hold the module off over nothing.
	*/
	if (r)
	{
		if (r->max_requests_day > 0 && r->requests_today >= r->max_requests_day)
		{
			char d[96];
			snprintf(d, sizeof(d), "%d of %d requests used today",
				r->requests_today, r->max_requests_day);
			ss_hold_off(SS_ERR_QUOTA, d);
		}

		/*
		  And the ko allowance, which is the one worth standing down on early. Every
		  request for a game the database cannot match counts against it, a shelf of
		  homebrew and hacks can spend it without a single successful scrape, and the
		  server's answer past it is 431 - which this client is obliged to treat as a
		  ban needing a human. Stopping here is stopping one step before that.
		*/
		if (r->max_requests_ko_day > 0 && r->requests_ko_today >= r->max_requests_ko_day)
		{
			char d[96];
			snprintf(d, sizeof(d), "%d of %d unmatched requests used today",
				r->requests_ko_today, r->max_requests_ko_day);
			ss_hold_off(SS_ERR_QUOTA, d);
		}
	}

	switch (err)
	{
	/*
	  A verdict about the game. Nothing to hold: the account is in good standing and the
	  next game is worth asking about. Whoever called this owns the game side of it.
	*/
	case SS_OK:
	case SS_ERR_NOTFOUND:
		break;

	/*
	  Out of allowance, or asking too fast. Both temporary, and both stop the asking.

	  SS_ERR_THREADS says the same account is being scraped from somewhere else - a PC
	  running Skyscraper against it, most likely - and chome_ss.h's note on it says to
	  back off rather than stop. With no clock here, backing off for the session is what
	  that amounts to, and it is the right side to err on: the other client is the one
	  the player is watching, and a front-end quietly losing the race for their one
	  thread is better than two clients fighting over it.
	*/
	case SS_ERR_QUOTA:
	case SS_ERR_THREADS:
	case SS_ERR_CLOSED:
		ss_hold_off(err, 0);
		break;

	/*
	  Nothing further can succeed. Held for the same reason as the temporary ones rather
	  than a different one, because the effect wanted is identical - stop asking - and a
	  second mechanism for "stop harder" would be a second thing to get wrong.
	*/
	case SS_ERR_CREDENTIALS:
	case SS_ERR_BLACKLISTED:
		ss_hold_off(err, 0);
		break;

	/*
	  And these two are deliberately not a hold.

	  A reply that did not arrive or did not parse is one attempt failing, not the account
	  refusing us. Holding on it would let a single dropped packet cost every remaining
	  cover in the session; not holding costs at worst one failed curl per game, which is
	  a DNS lookup that fails in milliseconds when there is no network at all. The game
	  side is still left unmarked either way - ss_verdict() is false for both - so nothing
	  is remembered as missing over a wire fault.
	*/
	case SS_ERR_TRANSPORT:
	case SS_ERR_MALFORMED:
		break;

	default:
		break;
	}
}

/* ------------------------------------------------------- what we want ------- */

/*
  Type before region, which is the opposite of what Skyscraper does.

  Its reason for region-first is that a user wants their own local box. Ours is
  that the shelf card is a flat rectangle: a box-3D render dropped into it is a
  photographed box at an angle, and it looks wrong next to flat covers in a way a
  Japanese cover next to an American one does not. So the best *kind* is chosen
  first, and region decides between the ones of that kind.

  These live up here, above the parser rather than beside the picker where they used
  to, because the parser now filters on them. That is the point: the set of types
  worth storing is *derived* from the set the picker can ask for, so the two cannot
  drift. The bug that motivated the filter was the other kind of drift - the cap
  discarding types the picker listed - and a second hand-written list of "wanted"
  types would have been the same mistake in a new place.

  support-2D is the scan of the disc face. It has no kind list of its own beyond
  SS_KIND_DISC because there is nothing to fall back to: a disc either has a scan or
  it does not, and a box cover is not a substitute for a picture of a disc.
*/
static const char *const cover_types[]  = { "box-2D", "box-3D", "mixrbv2", "mixrbv1", "ss", 0 };
static const char *const screen_types[] = { "ss", "sstitle", "mixrbv1", 0 };
static const char *const wheel_types[]  = { "wheel", "wheel-hd", "screenmarquee", 0 };
static const char *const disc_types[]   = { "support-2D", 0 };

static const char *const *const all_types[] =
{
	cover_types, screen_types, wheel_types, disc_types
};

int ss_type_wanted(const char *type)
{
	if (!type || !type[0]) return 0;

	for (size_t k = 0; k < sizeof(all_types) / sizeof(all_types[0]); k++)
	{
		for (int t = 0; all_types[k][t]; t++)
		{
			if (!strcasecmp(all_types[k][t], type)) return 1;
		}
	}
	return 0;
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
#define SSF_KOTODAY    3
#define SSF_MAXKODAY   4

static int ssuser_field(const char *tag)
{
	if (!strcasecmp(tag, "requeststoday"))       return SSF_REQTODAY;
	if (!strcasecmp(tag, "maxrequestsperday"))   return SSF_MAXDAY;
	if (!strcasecmp(tag, "maxthreads"))          return SSF_MAXTHREADS;

	// The other quota. Compared before "requeststoday" would be, since neither name is
	// a prefix of the other - but they are one letter apart in the middle and it is
	// worth being explicit that these are two different allowances.
	if (!strcasecmp(tag, "requestskotoday"))     return SSF_KOTODAY;
	if (!strcasecmp(tag, "maxrequestskoperday")) return SSF_MAXKODAY;

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

/*
  Whether this media is worth one of the store's entries, and the reason it is asked
  before the cap rather than after.

  A real reply carried 133 media for one PlayStation game against a store of 64, and
  media_commit() returned early past the cap - so the last 69 were dropped, and because
  the server groups by type the drop landed squarely on mixrbv1 (first at index 89),
  mixrbv2 (97) and support-2D (past 64). Three of the five types the cover picker lists
  and the only type the disc dialog wants, thrown away to make room for bezels, pictos,
  figurines and box textures that nothing in this file can ask for. The fallbacks in
  ss_pick() had never been reachable.

  Three refusals, in the order that matters:

    1. the type is not one ss_type_wanted() recognises. This is the one that does the
       work - it is what turns 133 media into something a small store holds.
    2. the same type and region has already been stored. ss_pick() returns the first
       match, so a second one is unreachable by construction and costs 552 bytes.
    3. this type already holds SS_MAX_PER_TYPE. Without it a type carrying a dozen
       regions could still eat the store before a later type was reached, which is the
       original bug with a smaller blast radius. With it, ten wanted types times nine
       is 90 in a store of 96 and no type can be starved by the ones before it.
*/
static int media_keep(const ss_ctx *c)
{
	if (!c->cur.url[0]) return 0;                      // a media with no URL is no use
	if (strncasecmp(c->cur.url, "http", 4)) return 0;  // and nor is one that is not a URL

	if (!ss_type_wanted(c->cur.type)) return 0;

	int of_type = 0;
	for (int i = 0; i < c->r->nmedia; i++)
	{
		if (strcasecmp(c->r->media[i].type, c->cur.type)) continue;
		if (!strcasecmp(c->r->media[i].region, c->cur.region)) return 0;
		of_type++;
	}

	return of_type < SS_MAX_PER_TYPE;
}

static void media_commit(ss_ctx *c)
{
	if (c->r->nmedia >= SS_MAX_MEDIA) return;
	if (!media_keep(c)) return;

	c->r->media[c->r->nmedia++] = c->cur;
}

/*
  Undo XML entity escaping, in place, on a URL taken from element text.

  Needed because sxmlc decodes entities in *attribute values* and deliberately does not
  decode them in text - "no str_unescape(line)" at the SAX text callback in sxmlc.c - and
  the real reply puts the URL in the text. So a URL the server wrote as

      ...jeuInfos.php?devid=x&amp;devpassword=y

  arrived here as that literal string, and handing it to curl would have sent one
  parameter called "devid" whose value was "x&amp;devpassword=y". The credential half of
  every media URL would silently have gone missing, and the fetch would have come back
  401 with nothing in the log to say why.

  Only applied to the text placement, and that precision matters: an attribute value has
  already been through html2str() inside sxmlc, so running this over one as well would
  decode it twice and turn a legitimate "&amp;amp;" into "&".

  Deliberately short. The five predefined entities plus numeric references is the whole
  of what a URL can contain, and anything else is left alone rather than guessed at.
*/
static void url_unescape(char *s)
{
	char *w = s;

	for (char *p = s; *p; )
	{
		if (*p != '&') { *w++ = *p++; continue; }

		static const struct { const char *ent; char ch; } named[] =
		{
			{ "&amp;",  '&' },
			{ "&lt;",   '<' },
			{ "&gt;",   '>' },
			{ "&quot;", '"' },
			{ "&apos;", '\'' },
		};

		const char *hit = 0;
		char ch = 0;
		for (size_t i = 0; !hit && i < sizeof(named) / sizeof(named[0]); i++)
		{
			if (!strncmp(p, named[i].ent, strlen(named[i].ent))) { hit = named[i].ent; ch = named[i].ch; }
		}

		if (hit) { *w++ = ch; p += strlen(hit); continue; }

		if (p[1] == '#')
		{
			int base = (p[2] == 'x' || p[2] == 'X') ? 16 : 10;
			char *d = p + ((base == 16) ? 3 : 2);
			long v = 0;
			char *q = d;
			while (*q && *q != ';')
			{
				int dig = isdigit((unsigned char)*q) ? *q - '0' :
					(base == 16 && isxdigit((unsigned char)*q)) ? (tolower((unsigned char)*q) - 'a' + 10) : -1;
				if (dig < 0 || dig >= base) break;
				v = v * base + dig;
				q++;
			}

			// Only a plain one-byte reference, and only when it really was terminated.
			if (q > d && *q == ';' && v > 0 && v < 128)
			{
				*w++ = (char)v;
				p = q + 1;
				continue;
			}
		}

		*w++ = *p++;
	}

	*w = 0;
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
				// The URL as element text, which is the placement the real reply uses.
				// Only used when there was no url attribute - and there never is one.
				if (!c->cur.url[0] && c->text_len)
				{
					snprintf(c->cur.url, sizeof(c->cur.url), "%s", c->text);
					url_unescape(c->cur.url);
				}
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
			if (!c->cur.url[0]    && !strcasecmp(node->tag, "url"))
			{
				snprintf(c->cur.url, sizeof(c->cur.url), "%s", c->text);
				url_unescape(c->cur.url);      // element text, so sxmlc left the entities alone
			}
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
			if (c->field == SSF_KOTODAY)    c->r->requests_ko_today = v;
			if (c->field == SSF_MAXKODAY)   c->r->max_requests_ko_day = v;
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
	out->requests_ko_today = -1;
	out->max_requests_ko_day = -1;
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

// The type lists themselves are up above the parser, which filters on them.

const ss_media *ss_pick(const ss_result *r, int kind, const char *const *regions)
{
	if (!r || r->err != SS_OK) return 0;

	const char *const *types =
		(kind == SS_KIND_SCREEN) ? screen_types :
		(kind == SS_KIND_WHEEL)  ? wheel_types  :
		(kind == SS_KIND_DISC)   ? disc_types   : cover_types;

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
		  Then the first entry of this kind in reply order, whatever its region - which
		  also covers the entries that carry no region at all. Taken last rather than
		  dropped: a cover with no region is still the right cover, and for a disc scan
		  the wrong pressing is still a picture of the disc while nothing at all is a
		  hole in the dialog. Derek's rule, and it is deliberately reply order rather
		  than any preference of ours: the server lists its own best first.
		*/
		for (int i = 0; i < r->nmedia; i++)
		{
			if (!strcasecmp(r->media[i].type, types[t])) return &r->media[i];
		}
	}

	return 0;
}

/* ---------------------------------------------------------------- regions --- */

/*
  Sony wrote the region into the publisher prefix, which is the only reason a pressed
  disc can be asked for the right art at all: there is no filename to read a "(Europe)"
  out of, and the identity key for a PlayStation disc is exactly this serial.

  The prefixes are the ones disc_serial_at() already looks for; SCPM is left out here
  on purpose rather than guessed at, since a wrong region is quietly the wrong picture
  and no region is honestly no region.
*/
const char *ss_region_from_serial(const char *serial)
{
	if (!serial) return 0;

	// Only the four-letter prefix carries the region. Everything after it - the dash,
	// the underscore, the space, the dot, the number - varies by who wrote the string.
	static const struct { const char *prefix; const char *region; } psx[] =
	{
		{ "SLES", "eu" }, { "SCES", "eu" },
		{ "SLUS", "us" }, { "SCUS", "us" },
		{ "SLPS", "jp" }, { "SLPM", "jp" }, { "SCPS", "jp" },
	};

	for (size_t i = 0; i < sizeof(psx) / sizeof(psx[0]); i++)
	{
		if (!strncasecmp(serial, psx[i].prefix, 4)) return psx[i].region;
	}

	return 0;
}

int ss_regions_for_serial(const char *serial, const char **out, int max)
{
	if (!out || max < 1) return 0;
	out[0] = 0;

	const char *r = ss_region_from_serial(serial);
	if (!r) return 0;
	if (max < 2) return 0;                 // no room for the terminator: say nothing

	out[0] = r;
	out[1] = 0;
	return 1;
}

/* -------------------------------------------------------------- redaction --- */

/*
  The reply's own URLs are credentials, which was not obvious until a real reply was
  read: every media URL carries devid, devpassword, ssid and sspassword in its query
  string, because the media endpoint authenticates the same way jeuInfos.php does.

  ss_build_url() has had a redact flag from the start for the request URL. This is the
  same discipline for the other direction, and it is a separate function rather than a
  flag because these URLs are not built here - they are read off the wire, in whatever
  order and spelling the server chose, so they have to be rewritten rather than
  formatted.

  Rewritten by key, not by position: the four keys are found wherever they appear and
  their values replaced with "***", and anything else - the media type, the game id,
  the file name, the size - is copied through, because a log line with those removed
  could not be read against the reply it came from.
*/
int ss_redact_url(const char *in, char *out, int len)
{
	if (!out || len <= 0) return 0;
	out[0] = 0;
	if (!in) return 0;

	static const char *const secret[] = { "devid", "devpassword", "ssid", "sspassword", 0 };

	int o = 0;
	const char *p = in;

	while (*p)
	{
		/*
		  A key starts at the beginning of the query or just after a separator. Checked
		  here rather than with a plain substring search because "ssid" is a substring of
		  "sspassword" would-be values and of nothing useful: matching mid-token would
		  redact halfway through somebody's game title.
		*/
		int at_key = (p == in) || p[-1] == '?' || p[-1] == '&' || p[-1] == ';';

		const char *hit = 0;
		int klen = 0;

		if (at_key)
		{
			for (int s = 0; secret[s]; s++)
			{
				int l = (int)strlen(secret[s]);
				if (strncasecmp(p, secret[s], l)) continue;
				if (p[l] != '=') continue;
				hit = secret[s];
				klen = l;
				break;
			}
		}

		if (!hit)
		{
			if (o >= len - 1) { out[0] = 0; return 0; }
			out[o++] = *p++;
			continue;
		}

		// key=***, then skip the real value up to the next separator.
		if (o + klen + 4 >= len - 1) { out[0] = 0; return 0; }
		memcpy(out + o, p, (size_t)klen);
		o += klen;
		out[o++] = '=';
		out[o++] = '*';
		out[o++] = '*';
		out[o++] = '*';

		p += klen + 1;
		while (*p && *p != '&' && *p != ';') p++;
	}

	out[o] = 0;
	return o;
}
