/*
  Classic Home - ScreenScraper, for the covers nothing on the card can supply.

  This is the last layer of the art chain, below the pack, gamelist.xml and the
  scraper folders that chome_art.cpp already looks in: a game whose cover is on
  none of those has to be asked for, or it stays a plain plate forever.

      https://www.screenscraper.fr/webapi2.php

  ---------------------------------------------------------------------------
  NOTHING HERE IS LIVE. ss_available() is compile-time false in this tree.
  ---------------------------------------------------------------------------

  API v2 is not open. Every request carries a devid/devpassword pair issued to a
  *named application* by ScreenScraper staff on request in their API forum, tied to
  a `softname`; there is no self-service registration, no anonymous tier, and
  borrowing another client's pair is what gets an application blacklisted (the API
  returns "blacklisté" for exactly that). So the credential cannot be invented
  here, and none is in this tree.

  Which is why the whole module is gated on CLASSICUI_SS_DEVID being defined at
  build time. Undefined - the state of every build we ship today - and
  ss_available() returns 0, ss_build_url() refuses, and no call site can reach the
  network. The test build defines it with a dummy value so the URL builder and the
  parser are still covered.

  When a real credential exists, one decision is still open and should not be made
  by default: a devid in a string literal is greppable out of the binary in
  seconds. Skyscraper keeps its pair obfuscated and decrypts at use. That is
  security theatre against anyone determined, but it is also the accepted norm, and
  shipping ours in clear would be a gift to whoever wants to burn it. Decide before
  the first public build, not after.

  ---------------------------------------------------------------------------

  Why XML and not JSON. The API answers in xml, json or ini, and this asks for xml
  purely because sxmlc is already in the tree and already trusted for two other
  file formats (.mra, romsets.xml) plus gamelist.xml next door. Adding a JSON
  parser to read six fields would be new attack surface for no gain.

  What gets asked for, and what does not. jeuInfos.php only - one request for one
  game, and only for a game with no cover from any local layer. Not
  systemesListe.php on every boot, not jeuRecherche.php fuzzy matching, and no
  parallel requests: `maxthreads` is 1 for an ordinary account and going over it is
  the fastest way to a 429.

  Quota comes back inside the game response itself, under response/ssuser, so it is
  read from every reply and never costs a request of its own. When requeststoday
  reaches maxrequestsperday the module stops asking for the rest of the day - the
  server would refuse anyway, and hammering a refusal is what quotas are counted
  against.

  Hashing. ScreenScraper matches far better on a hash than a filename, but this
  runs on a 800 MHz ARM reading a FAT card, and md5 of a 700 MB .chd is not
  something to do while somebody is scrolling a shelf. So a file up to
  SS_HASH_MAX_BYTES is hashed and matched properly; anything above it is matched on
  name and size alone, which is what the API's romnom/romtaille pair is for.

  ---------------------------------------------------------------------------

  Every media URL in a reply is a credential.

  Measured against a real jeuInfos reply: each of the URLs the server hands back
  embeds devid, devpassword, ssid *and* sspassword in its query string, because the
  media endpoint authenticates the same way the API does. So the redaction rule that
  ss_build_url() applies to the *request* URL applies just as hard to every URL that
  comes back in the *reply*, and to the reply file itself. Saving one of those replies
  next to the card's other caches would be publishing the account.

  Two consequences, both load-bearing:

    - ss_redact_url() exists, and anything that logs, asserts on or reports a media
      URL has to go through it. A raw one in /tmp/debug.txt is a password in
      /tmp/debug.txt.
    - a downloaded reply lives in tmpfs and is unlinked as soon as it is parsed. It
      never reaches the SD card.
*/

#ifndef CHOME_SS_H
#define CHOME_SS_H

#include <stdint.h>

// Above this a file is matched by name and size instead of by hash. Covers every
// cartridge system outright; disc images are the ones that fall through.
#define SS_HASH_MAX_BYTES  (32 * 1024 * 1024)

/*
  The media store, and why it is filtered rather than simply made bigger.

  Measured, from one real jeuInfos reply for one PlayStation game: 133 <media>
  elements. The store was 64 and media_commit() returned early past it, so 69 of them
  were thrown away - and because the server groups the list by type, the ones thrown
  away were the ones at the end. First occurrence in that reply: sstitle 0, ss 1,
  wheel 6, box-2D 17, box-3D 47, mixrbv1 89, mixrbv2 97, support-2D past 64. So the
  cap discarded mixrbv1, mixrbv2 and support-2D outright: three of the five entries in
  cover_types, the last entry of screen_types, and the disc scan. The mixrbv fallbacks
  in the picker could never once have fired.

  Raising the cap to cover 133 was the wrong fix. ss_media is ~552 bytes, dominated by
  url[512], so 160 entries is ~88 KB - and the overwhelming majority of what it would
  buy is bezels, pictos, figurines, box textures and support-texture variants that no
  kind list below can ever ask for. So the parser filters instead: a media whose type
  is not one ss_type_wanted() recognises is dropped before the cap is even consulted.

  Ten types survive the filter (the three kind lists plus support-2D). Two further
  rules make 96 enough for them:

    - a second entry with the same type *and* region is dropped, because ss_pick()
      returns the first match and can never reach it. Pure dead weight.
    - no single type may take more than SS_MAX_PER_TYPE of the store, so an early
      type with many regions cannot starve a late one. This is the rule that
      protects support-2D, which arrives last in the reply and is the one thing the
      disc dialog needs.

  10 types x 9 per type is 90, which fits in 96 with room to spare, so the cap can no
  longer drop a type entirely however the server orders the list. 96 x 552 is ~53 KB
  against ~88 KB for the naive fix.

  Nine regions per type is generous against what was measured - support media in that
  reply carried de, eu, uk, jp and sp, five of the twelve-odd region codes the API
  uses - but it is a ceiling, not a guarantee: a type that really did carry more than
  nine regions would keep the first nine, and a preference for a region beyond those
  falls back to the first entry of the type, which is the same answer as for a region
  the reply never carried at all.
*/
#define SS_MAX_MEDIA    96
#define SS_MAX_PER_TYPE 9
#define SS_URL_LEN      512
#define SS_TYPE_LEN     24
#define SS_REGION_LEN   8

// What we would ever want off the API. The shelf uses covers; the others are here
// because the suspend strip and the header could use them later, and because
// picking one out of a media list is the same code either way. DISC is the scan of
// the disc face itself, which is what the physical-disc dialog draws.
#define SS_KIND_COVER   0
#define SS_KIND_SCREEN  1
#define SS_KIND_WHEEL   2
#define SS_KIND_DISC    3
#define SS_KIND_COUNT   4

/*
  How a request ended. Split this finely because the responses want different
  handling and lumping them together is how a client ends up retrying something
  that will never succeed:

    SS_ERR_NOTFOUND     this game is not in the database. Cache the miss - asking
                        again tomorrow gets the same answer and costs a request.
    SS_ERR_CREDENTIALS  our devid pair or the user's account was refused. Stop
                        entirely; every further request fails identically.
    SS_ERR_CLOSED       the server has shut the API off under load. Try later.
    SS_ERR_BLACKLISTED  our softname is banned. Stop, and it needs a human.
    SS_ERR_QUOTA        the user's daily allowance is gone. Stop until tomorrow.
    SS_ERR_THREADS      too many at once. Ours is one at a time, so this means
                        the same account is scraping on a PC - back off, do not stop.
    SS_ERR_MALFORMED    a reply we could not parse. Treat as transport, not as a
                        verdict on the game.
    SS_ERR_TRANSPORT    curl failed, no network, timeout.
*/
enum
{
	SS_OK = 0,
	SS_ERR_NOTFOUND,
	SS_ERR_CREDENTIALS,
	SS_ERR_CLOSED,
	SS_ERR_BLACKLISTED,
	SS_ERR_QUOTA,
	SS_ERR_THREADS,
	SS_ERR_MALFORMED,
	SS_ERR_TRANSPORT
};

struct ss_media
{
	char type[SS_TYPE_LEN];      // "box-2D", "ss", "wheel", ...
	char region[SS_REGION_LEN];  // "wor", "us", "eu", "jp", or empty
	char format[8];              // "png", "jpg"
	char url[SS_URL_LEN];
};

struct ss_result
{
	int err;
	int nmedia;
	ss_media media[SS_MAX_MEDIA];

	// From response/ssuser. -1 when the reply did not carry them.
	int requests_today;
	int max_requests_day;
	int max_threads;

	char gameid[16];
};

/*
  1 when this build carries a devid at all. Compile-time, and false everywhere we
  ship today - so a call site guarded by this is provably dead code rather than
  code that happens not to run.
*/
int ss_available();

/*
  1 when a request would actually be made: available, the user turned the option
  on, and there is a ScreenScraper account to make it under. The API is unusable
  anonymously, so a missing account is off, not degraded.
*/
int ss_enabled();

/*
  MiSTer system id ("nes", "psx", ...) to the API's systemeid, as a string because
  that is how it goes into the URL. 0 when we do not know it.

  Deliberately incomplete. The values that are here were taken from a working
  client rather than guessed, and the systems that are missing are missing because
  a wrong systemeid does not fail - it silently matches a different platform and
  writes somebody else's box art onto the shelf. Fill them from systemesListe.php
  once there is a credential to call it with, or per-machine from the override file
  below.

  Two systems ride in another core's shelf and are told apart by extension, which
  the API does not do for us: .gbc is a different systemeid from .gb, and .gg is a
  different one from .sms. Pass the ROM name so that can be handled; pass 0 for it
  to take the core's own platform.
*/
const char *ss_system_id(const char *sysid, const char *romnom);

/*
  Reads /media/fat/config/classicui_ss_systems.cfg if it is there: lines of
  `<sysid>=<number>`, `#` comments. Lets the gaps above be filled without a
  rebuild, and is where a cached systemesListe.php would be written. Returns how
  many mappings it took.
*/
int ss_systems_load(const char *path);
void ss_systems_forget();

struct ss_query
{
	const char *systemeid;
	const char *romnom;          // file name only, not a path
	long long   romtaille;       // bytes, 0 to leave it out
	const char *md5;             // 0 when the file was too big to hash
	const char *crc;             // 0 when not computed
	const char *sha1;            // 0 when not computed
};

/*
  Builds the jeuInfos.php URL. Returns the length written, or 0 if it refused -
  no devid in this build, no systemeid, no rom name, or the buffer is too small.

  redact replaces devpassword and the user's password with "***". Everything that
  logs or asserts on a URL uses that form; the live form exists only long enough
  to hand to curl. A credential in /tmp/debug.txt is a credential published.
*/
int ss_build_url(const ss_query *q, int redact, char *out, int len);

/*
  Parses a reply already on disk. Fills `out` and returns its err. A reply that
  parses but describes no game is SS_ERR_NOTFOUND, not SS_ERR_MALFORMED.
*/
int ss_parse_file(const char *path, ss_result *out);

/*
  Classify without parsing. The API signals most failures twice - an HTTP status
  and a French sentence in the body - and neither alone covers everything, so both
  are checked. Return SS_OK when nothing is recognised.
*/
int ss_http_class(int http_code);
int ss_body_class(const char *body);

/*
  1 when a media of this type is one some kind list below can ask for. The parser
  drops everything else before SS_MAX_MEDIA is consulted - see the comment on it.
  Exposed because the filter is the interesting half of the parse and a test that
  could only see it through a 133-element fixture would be testing the fixture.
*/
int ss_type_wanted(const char *type);

/*
  Best media of this kind, or 0. `regions` is a 0-terminated preference list such
  as {"wor","us","eu","jp",0}; a media with no region at all is taken last rather
  than dropped, since plenty of entries carry none.

  Pass 0 for `regions` to mean "no preference", which lands on the first entry of the
  best available type in reply order. That is also what happens when a preference list
  is given and the reply carries none of the regions in it: Dinofly's rule, and the right
  one, because a disc scan in the wrong region is a picture of the game and no scan at
  all is a blank hole.
*/
const ss_media *ss_pick(const ss_result *r, int kind, const char *const *regions);

/*
  The region a PlayStation serial implies, spelled the way the API spells regions.
  Sony's publisher prefix carries it: SLES/SCES are Europe, SLUS/SCUS are the US,
  SLPS/SLPM/SCPS are Japan. Returns "eu", "us", "jp", or 0 when the prefix is not one
  of those - which includes every non-PlayStation disc, since their identity keys are
  header ids and volume labels rather than serials.

  Accepts the serial in any of the forms this tree passes around: SLES-01506,
  SLES_015.06, "SLPS 01204", SLES01506. Only the first four characters are read.
*/
const char *ss_region_from_serial(const char *serial);

/*
  A region preference list for a disc known by its identity key, written into `out`
  as a 0-terminated list and returned as a count.

  0 means nothing could be derived from the key, and the caller should hand ss_pick()
  a null `regions` rather than a list of plausible defaults: preferring "wor" over
  whatever the reply happens to lead with would be this code inventing a rule instead
  of admitting it does not know.

  Deliberately only the one derived region, for the same reason. The European variants
  the API also uses - uk, de, sp, and the rest - are not appended: the one reply we
  have measured carried a plain "eu" alongside them, so widening the list would be
  guessing on no evidence, and the first-in-reply fallback already covers a disc whose
  scan exists only as "uk".
*/
int ss_regions_for_serial(const char *serial, const char **out, int max);

/*
  Blanks the credential values out of a URL, for logging.

  Written for the URLs that arrive in a *reply*: every one of them carries devid,
  devpassword, ssid and sspassword in its query string, so a media URL in a log or a
  cache file is the account published. Replaces the value of each of those four keys
  with "***" and leaves everything else - the media type, the game id, the file name -
  intact, since a log line that hid those would be no use.

  Returns the length written. Refuses (0, and an empty buffer) rather than truncate,
  because half a URL in a log is worse than none: the half that survives could be the
  half with the password in it.
*/
int ss_redact_url(const char *in, char *out, int len);

#endif
