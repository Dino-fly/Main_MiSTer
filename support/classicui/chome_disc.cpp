#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>

#include "chome_disc.h"
#include "chome_titles.h"

#ifndef CHOME_HOST_TEST
#include <fcntl.h>
#include <unistd.h>
// Before linux/cdrom.h, which defines CDSL_CURRENT as INT_MAX without including
// this itself. The fork this derives from carries the same include for the same
// reason; the harness never hits it because it stubs the whole drive out.
#include <climits>
#include <sys/ioctl.h>
#include <linux/cdrom.h>
#include <sys/wait.h>
#include <signal.h>
#include <sys/stat.h>
#include <time.h>
#include "../../cfg.h"
#endif

/*
  See chome_disc.h for what this is and what it deliberately is not.

  Structure: everything above "the drive" is pure and runs against the installed
  reader, so the harness tests it against synthetic discs. Everything below talks to
  /dev/sr0 and is compiled out of the harness entirely (CHOME_HOST_TEST), because a
  test that needs a real disc in a real drive is a test nobody runs.
*/

/* --------------------------------------------------------------- reading ---- */

static disc_reader_fn reader = 0;
static void *reader_ctx = 0;

void disc_set_reader(disc_reader_fn fn, void *ctx)
{
	reader = fn;
	reader_ctx = ctx;
}

static int read_raw(int lba, uint8_t *dst)
{
	if (!reader) return -1;
	return reader(lba, DISC_READ_RAW, dst, reader_ctx);
}

static int read_user(int lba, uint8_t *dst)
{
	if (!reader) return -1;
	return reader(lba, DISC_READ_USER, dst, reader_ctx);
}

// memmem is a GNU extension and this file is built for two toolchains; a short
// search over 2 KB is not worth an ifdef.
static const uint8_t *find_bytes(const uint8_t *hay, int haylen, const void *needle, int nlen)
{
	if (nlen <= 0 || haylen < nlen) return 0;
	for (int i = 0; i <= haylen - nlen; i++)
	{
		if (!memcmp(hay + i, needle, nlen)) return hay + i;
	}
	return 0;
}

/* ------------------------------------------------------------ identifying --- */

static uint32_t iso_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/*
  Walk the ISO root directory looking for two things that are only visible in the
  file list rather than in a signature:

    - a Mega Drive+ disc, which is an ISO carrying both <name>.md and <name>.cue for
      the same name. Neither file alone means anything; the pair is the format.
    - an MSU-1 SNES disc, which carries a .sfc or .smc.

  Bounded at 1 MB of directory data. A root directory is a few KB; anything
  claiming megabytes is either corrupt or hostile, and this runs before we know
  which.
*/
static int iso_root_features(int data_lba0, int *has_mdplus, int *has_snes)
{
	uint8_t pvd[DISC_USER_SIZE];

	*has_mdplus = 0;
	*has_snes = 0;

	if (read_user(data_lba0 + 16, pvd)) return 0;
	if (pvd[0] != 1 || memcmp(pvd + 1, "CD001", 5)) return 0;

	const uint8_t *root = pvd + 156;
	if (root[0] < 34) return 0;

	uint32_t extent = iso_le32(root + 2);
	uint32_t size = iso_le32(root + 10);
	if (!extent || !size) return 0;

	enum { NAMES_MAX = 64, NAME_LEN = 128 };
	char md[NAMES_MAX][NAME_LEN];
	char cue[NAMES_MAX][NAME_LEN];
	int nmd = 0, ncue = 0;

	for (uint32_t done = 0; done < size && done < 1024 * 1024; done += DISC_USER_SIZE)
	{
		uint8_t sec[DISC_USER_SIZE];
		if (read_user(data_lba0 + extent + done / DISC_USER_SIZE, sec)) break;

		int off = 0;
		while (off < DISC_USER_SIZE && done + off < size)
		{
			int len = sec[off];
			if (!len) break;                                  // rest of this sector is padding
			if (len < 34 || off + len > DISC_USER_SIZE) break; // malformed: stop trusting it

			int nlen = sec[off + 32];
			if (nlen > 0 && nlen < NAME_LEN - 8 && off + 33 + nlen <= DISC_USER_SIZE)
			{
				char name[NAME_LEN];
				memcpy(name, sec + off + 33, nlen);
				name[nlen] = 0;

				// ISO9660 names carry a ";1" version suffix.
				char *semi = strchr(name, ';');
				if (semi) *semi = 0;

				char *dot = strrchr(name, '.');
				if (dot)
				{
					if (!strcasecmp(dot, ".sfc") || !strcasecmp(dot, ".smc")) *has_snes = 1;
					else if (!strcasecmp(dot, ".md") && nmd < NAMES_MAX)
					{
						*dot = 0;
						snprintf(md[nmd++], NAME_LEN, "%s", name);
					}
					else if (!strcasecmp(dot, ".cue") && ncue < NAMES_MAX)
					{
						*dot = 0;
						snprintf(cue[ncue++], NAME_LEN, "%s", name);
					}
				}
			}
			off += len;
		}
	}

	for (int i = 0; i < nmd && !*has_mdplus; i++)
	{
		for (int j = 0; j < ncue; j++)
		{
			if (!strcasecmp(md[i], cue[j])) { *has_mdplus = 1; break; }
		}
	}

	return 1;
}

int disc_identify_at(int data_lba0)
{
	// No data track anywhere: that is an audio CD, and there is nothing to read.
	if (data_lba0 < 0) return DISC_T_AUDIO;

	uint8_t raw[DISC_RAW_SIZE * 2];

	/*
	  Mega Drive+ first. Such a disc is also a perfectly valid ISO and would be
	  caught by a later test as something else, so the more specific answer has to
	  come before the more general one.
	*/
	int has_mdplus = 0, has_snes = 0;
	iso_root_features(data_lba0, &has_mdplus, &has_snes);
	if (has_mdplus) return DISC_T_MDPLUS;

	// Signatures sitting in the first raw sector, past its 16-byte header.
	if (!read_raw(data_lba0, raw))
	{
		if (!memcmp(raw + 16, "SEGADISCSYSTEM", 14)) return DISC_T_MEGACD;
		if (!memcmp(raw + 16, "SEGA SEGASATURN", 15)) return DISC_T_SATURN;
		if (raw[16] == 0x01 && raw[17] == 0x5A && raw[18] == 0x5A &&
			raw[19] == 0x5A && raw[20] == 0x5A && raw[21] == 0x5A) return DISC_T_3DO;
	}

	/*
	  The ISO primary volume descriptor, 16 sectors in. Read raw because a CD-i
	  disc's descriptor sits at a different offset within the sector than a
	  CD-ROM's, so both places are tried.
	*/
	if (!read_raw(data_lba0 + 16, raw))
	{
		const uint8_t *iso = raw + 16;
		if (memcmp(iso + 1, "CD001", 5) && memcmp(iso + 1, "CD-I", 4)) iso = raw + 24;

		if (!memcmp(iso + 1, "CD001", 5))
		{
			if (!memcmp(iso + 8, "PLAYSTATION", 11)) return DISC_T_PSX;
			if (!memcmp(iso + 8, "NGCD", 4)) return DISC_T_NEOGEO;
		}
		if (!memcmp(iso + 1, "CD-I", 4)) return DISC_T_CDI;
	}

	// Discs whose only tell is a file in the root.
	for (int s = 16; s <= 40; s++)
	{
		uint8_t user[DISC_USER_SIZE];
		if (read_user(data_lba0 + s, user)) continue;
		if (find_bytes(user, sizeof(user), "IPL.TXT", 7)) return DISC_T_NEOGEO;
		if (find_bytes(user, sizeof(user), "CDI_APPL", 8)) return DISC_T_CDI;
	}

	/*
	  PC Engine CD puts its string somewhere in the first two sectors rather than at
	  a fixed offset, so this is a search rather than a compare.
	*/
	if (!read_raw(data_lba0, raw) && !read_raw(data_lba0 + 1, raw + DISC_RAW_SIZE))
	{
		if (find_bytes(raw, sizeof(raw), "PC Engine CD-ROM SYSTEM", 23)) return DISC_T_PCECD;
	}

	// Last, because a .sfc on a disc is weaker evidence than any signature above.
	if (has_snes) return DISC_T_SNES;

	return DISC_T_UNKNOWN;
}

int disc_label_at(int data_lba0, char *out, int outsz)
{
	if (!out || outsz < 2) return 0;
	out[0] = 0;
	if (data_lba0 < 0) return 0;

	uint8_t user[DISC_USER_SIZE];
	if (read_user(data_lba0 + 16, user)) return 0;
	if (user[0] != 1 || memcmp(user + 1, "CD001", 5)) return 0;

	char lbl[33];
	memcpy(lbl, user + 40, 32);
	lbl[32] = 0;

	int end = 32;
	while (end > 0 && (lbl[end - 1] == ' ' || !lbl[end - 1])) end--;
	lbl[end] = 0;

	// ISO labels use underscores for spaces, and there is no promise the rest is
	// printable - this ends up on screen.
	for (int i = 0; i < end; i++)
	{
		if (lbl[i] == '_') lbl[i] = ' ';
		else if (lbl[i] < 0x20 || (uint8_t)lbl[i] > 0x7E) lbl[i] = ' ';
	}

	// Trim again: the substitutions above can leave trailing spaces.
	end = (int)strlen(lbl);
	while (end > 0 && lbl[end - 1] == ' ') end--;
	lbl[end] = 0;

	/*
	  Every PlayStation disc is labelled "PLAYSTATION", which tells the player
	  nothing they cannot see from the icon. Refused so the caller falls through to
	  the serial, which at least identifies the game.
	*/
	if (!strcasecmp(lbl, "PLAYSTATION")) return 0;

	snprintf(out, outsz, "%s", lbl);
	return (int)strlen(out);
}

int disc_serial_at(int data_lba0, char *out, int outsz)
{
	// Sony's publisher prefixes. A disc's serial is written into its boot
	// configuration as e.g. "SLUS_006.26;1".
	static const char *const pfx[] =
	{
		"SCES", "SLES", "SCUS", "SLUS", "SCPS", "SLPS", "SLPM", "SCPM",
		"SIPS", "SCED", "SLED", "SCZS", "PAPX", "PCPX", "PEPX", "PUPX",
	};

	if (!out || outsz < 2) return 0;
	out[0] = 0;
	if (data_lba0 < 0) return 0;

	for (int s = 16; s <= 64; s++)
	{
		uint8_t user[DISC_USER_SIZE];
		if (read_user(data_lba0 + s, user)) continue;

		for (size_t p = 0; p < sizeof(pfx) / sizeof(pfx[0]); p++)
		{
			const uint8_t *m = find_bytes(user, sizeof(user), pfx[p], 4);
			if (!m) continue;

			int avail = (int)(sizeof(user) - (m - user));
			const uint8_t *semi = find_bytes(m, avail, ";", 1);
			if (!semi) continue;

			int len = (int)(semi - m);
			if (len < 8 || len > 11) continue;      // "SLUS_006.26" is 11

			char id[16];
			memcpy(id, m, len);
			id[len] = 0;

			// Redump/ScreenScraper form: SLUS_006.26 -> SLUS-00626.
			if (id[4] == '_') id[4] = '-';
			char *dot = strchr(id, '.');
			if (dot) memmove(dot, dot + 1, strlen(dot));

			snprintf(out, outsz, "%s", id);
			return (int)strlen(out);
		}
	}

	return 0;
}

/* ------------------------------------------------------------ names, cores --- */

const char *disc_type_name(int type)
{
	switch (type)
	{
	case DISC_T_MEGACD:  return "Mega CD";
	case DISC_T_SATURN:  return "Saturn";
	case DISC_T_PSX:     return "PlayStation";
	case DISC_T_PCECD:   return "PC Engine CD";
	case DISC_T_NEOGEO:  return "Neo Geo CD";
	case DISC_T_3DO:     return "3DO";
	case DISC_T_CDI:     return "CD-i";
	case DISC_T_MDPLUS:  return "Mega Drive+";
	case DISC_T_SNES:    return "SNES MSU-1";
	case DISC_T_AUDIO:   return "Audio CD";
	case DISC_T_UNKNOWN: return "Unknown disc";
	}
	return "";
}

/*
  Which of our shelf systems can load this - meaning the *pressed disc*, which is not the
  same question as which system reads an image of it off the card.

  Several deliberately answer nothing. 3DO and CD-i have no entry in the shelf's system
  table at all, so "we identified it" and "we can launch it" are different questions and
  the caller has to ask both - that is exactly the case where the player gets asked to pick
  a core instead. Audio CDs answer nothing because no core plays them; that is a job for
  the firmware, not a shelf card.

  Saturn answers nothing for a different reason and it is worth being precise about, since
  the obvious reading is now wrong: Saturn IS a shelf system, and a .cue or .chd in
  games/Saturn is a card that launches the Saturn core. What it has no entry in is
  disc_playables - saturncdd.cpp has not been taught to stream sectors from a drive, the
  way megacdd and pcecdd have - so there is no core to hand the *drive* to, and claiming
  one here would offer a Play that could only fail.

  Nor is this table what the shelf entries changed. Mega CD still answers "md", because
  the Mega Drive row is where a player looks for Sega and its launch already overrides the
  rbf to the MegaCD core; that route is hardware-verified and had no reason to move. Where
  a *copy* of the disc goes is a separate question with a separate answer - see
  rip_target::dest in chome_ui.cpp - because a folder of tracks in games/Genesis is not a
  Mega Drive game and never became a card.
*/
const char *disc_system_id(int type)
{
	switch (type)
	{
	case DISC_T_PSX:    return "psx";
	case DISC_T_MEGACD: return "md";      // the Mega Drive core loads Mega CD
	case DISC_T_PCECD:  return "tg16";
	case DISC_T_NEOGEO: return "neogeo";
	case DISC_T_MDPLUS: return "md";
	case DISC_T_SNES:   return "snes";
	}
	return 0;
}

int disc_capable_systems(const char **out, int max)
{
	if (!out || max <= 0) return 0;

	int n = 0;
	for (int t = DISC_T_NONE; t <= DISC_T_UNKNOWN; t++)
	{
		const char *id = disc_system_id(t);
		if (!id) continue;

		// Two disc types share the Mega Drive core, so de-duplicate.
		int seen = 0;
		for (int i = 0; i < n; i++) if (!strcmp(out[i], id)) seen = 1;
		if (seen) continue;

		out[n++] = id;
		if (n >= max) break;
	}

	return n;
}

/* ------------------------------------------------------------- the drive ---- */

static int dstate = DISC_ABSENT;
static int dtype = DISC_T_NONE;
static char dserial[DISC_SERIAL_LEN];
static char dlabel[DISC_LABEL_LEN];
static int ddirty = 0;
static int watching = 0;

int  disc_state() { return dstate; }
int  disc_type()  { return dtype; }
const char *disc_serial() { return dserial; }
const char *disc_label()  { return dlabel; }

int disc_take_dirty()
{
	int d = ddirty;
	ddirty = 0;
	return d;
}

/*
  What to put under the icon.

  The title table comes first, because it is the only layer that can produce a name a
  player recognises. A pressed disc has no filename, so the two things below it are
  the two things the disc itself carries: a volume label, which is whatever the
  mastering engineer typed and is sometimes the game and sometimes "PLAYSTATION", and
  a serial, which is exact and unreadable. "SLES-01506" is the right answer to the
  wrong question.

  Below the table, the order is unchanged - label, then serial, then the console's
  name - so a card with no table on it behaves exactly as it did before this existed.
  That is the whole contract: disc_title_for() returns 0 for a missing file, and 0
  falls straight through to what was here before.

  Both identifiers are offered to the table, serial first, because they are the only
  handle each system gives us: PlayStation discs carry a serial and PC Engine and Neo
  Geo discs do not, so for those the label *is* the key. Asking twice is free after
  the first frame - chome_titles.cpp caches both answers, misses included, which it
  has to because this function runs on every frame that draws the disc.
*/
const char *disc_display_name()
{
	const char *t = dserial[0] ? disc_title_for(dserial) : 0;
	if (!t && dlabel[0]) t = disc_title_for(dlabel);
	if (t) return t;

	if (dlabel[0]) return dlabel;
	if (dserial[0]) return dserial;
	return disc_type_name(dtype);
}

static int identify_pending = 0;

static void disc_forget()
{
	if (dstate != DISC_ABSENT || dtype != DISC_T_NONE) ddirty = 1;
	dstate = DISC_ABSENT;
	dtype = DISC_T_NONE;
	dserial[0] = 0;
	dlabel[0] = 0;
	identify_pending = 0;
}

/*
  The state machine, kept out of the ioctl path on purpose.

  The two-phase shape - "there is a disc" now, "it is a PlayStation disc" later - is
  the whole reason the spinning icon exists, so it is the last thing that should
  only be exercisable with a disc in a drive. disc_poll() below does nothing but
  read the drive and call these two; the harness calls them directly.
*/
void disc_ingest_present(int present)
{
	if (!present)
	{
		disc_forget();
		return;
	}

	if (dstate != DISC_ABSENT) return;      // already known about, not a new arrival

	/*
	  Deliberately does not identify here. The drive is almost certainly still
	  spinning up, and reading a sector now blocks the thread that draws for as long
	  as that takes - which is exactly the moment the player is waiting to see
	  something happen.
	*/
	dstate = DISC_SPINNING;
	dtype = DISC_T_NONE;
	dserial[0] = 0;
	dlabel[0] = 0;
	identify_pending = 1;
	ddirty = 1;
}

int disc_identify_due()
{
	return identify_pending;
}

void disc_ingest_identify(int lba0)
{
	if (!identify_pending) return;
	identify_pending = 0;

	dtype = disc_identify_at(lba0);
	dstate = (dtype == DISC_T_UNKNOWN) ? DISC_UNKNOWN : DISC_READY;

	disc_label_at(lba0, dlabel, sizeof(dlabel));
	disc_serial_at(lba0, dserial, sizeof(dserial));
	ddirty = 1;
}

/* --------------------------------------------------- probing and backoff ---- */

/*
  Two small decisions pulled out of disc_watch_start() and disc_poll() so they can be
  tested without a device node, a fork(), or a wall clock: whether it is time to look
  for a drive again, and whether it is time to fork a replacement helper. Both are
  pure - the same three ints always give the same answer - which is what lets the
  harness drive "no drive at boot, one appears a few seconds later" and "a helper
  that keeps dying gets backed off" as ordinary checks instead of something that
  needs real hardware and real time to pass.
*/

// Sentinel for "never looked yet", distinct from every real clock value.
#define DISC_NEVER_PROBED (-1)

// Seconds between retries once no drive has been found. The probe itself is three
// open() calls that fail immediately when nothing is at the path - there is no seek
// or spin-up to make a fast retry expensive - so this floor is for the log, not the
// drive: "no optical drive" printed every frame would drown out everything else on
// the console. A few seconds is short enough that a USB drive enumerating a moment
// after the menu comes up is found well before anyone would think to reboot over it.
#define DISC_PROBE_RETRY_S 5

/*
  Whether disc_watch_start() should try opening a drive again right now.

    found       a drive is already known and being watched - always false once this
                is true, so the parent never opens a second fd racing its own helper.
    last_probe  DISC_NEVER_PROBED before the first attempt, else the time (same
                clock as `now`) of the previous one.
    now         the current time, same clock as `last_probe`.
*/
int disc_probe_due(int found, int last_probe, int now)
{
	if (found) return 0;
	if (last_probe == DISC_NEVER_PROBED) return 1;
	return (now - last_probe) >= DISC_PROBE_RETRY_S;
}

// A helper that dies inside this many seconds of its own fork is "instant" - too
// fast to have done any real work, so it is almost certainly failing the same way it
// just failed rather than hitting a fresh problem.
#define DISC_HELPER_QUICK_DEATH_S 2

// Backoff once two helpers in a row have died instantly. Long enough that a helper
// which can never open the device - wrong permissions, a drive gone between the
// probe and the fork - does not turn into a fork() bomb: one CPU doing nothing but
// forking and dying, forever, behind a UI whose log never repeats itself and so
// looks healthy.
#define DISC_HELPER_BACKOFF_S 10

/*
  Whether disc_poll() should fork a replacement helper right now.

    quick_deaths  consecutive helpers that died within DISC_HELPER_QUICK_DEATH_S
                  seconds of their own fork. 0 means either none has died yet or the
                  last one ran a normal while before it did.
    last_fork     the time the most recent fork was attempted.
    now           the current time.

  The first quick death still reforks at once - one bad fork is not a pattern, and a
  drive that was just found a moment ago is worth trying again immediately. Only a
  *second* consecutive quick death - the replacement dying just as fast - switches to
  the backoff above.
*/
int disc_refork_due(int quick_deaths, int last_fork, int now)
{
	if (quick_deaths <= 1) return 1;
	return (now - last_fork) >= DISC_HELPER_BACKOFF_S;
}

#ifdef CHOME_HOST_TEST

/*
  The harness drives the state machine directly rather than through a drive. Only
  the identification above is under test; the ioctl plumbing below is not something
  a host test can say anything true about.
*/
int  disc_watch_start() { watching = 1; return 1; }
void disc_watch_stop()  { watching = 0; disc_forget(); }
int  disc_watching()    { return watching; }
void disc_poll() {}
void disc_reset_reader() { reader = 0; reader_ctx = 0; }

#else

/*
  The drive is owned by a helper child. The parent never touches it.

  Two hardware findings forced this, in order:

  1. Identification inline on the draw thread froze the front-end. It issues up to
     ~30 sequential SCSI reads with multi-second timeouts, and on a drive that would
     not answer, the firmware sat in state D in blk_execute_rq for minutes.

  2. Moving only the *reads* into a child was not enough. With the child busy on the
     drive, the parent's own CDROM_DRIVE_STATUS ioctl blocked too - state D in
     sr_block_ioctl. Every ioctl on /dev/sr0 serialises behind whatever the drive is
     doing, so there is no such thing as a cheap status poll while a disc is being
     read.

  So the parent's only contact with the disc is reading a small file out of /tmp. The
  helper owns the fd, polls the status, identifies, and rewrites that file. A drive
  that wedges costs a stuck helper; the console keeps drawing.

  The helper is deliberately a process and not a thread: a thread stuck in an
  uninterruptible ioctl cannot be killed, and it would hold the same address space as
  the UI. A process can be abandoned.
*/

#define DISC_STATE_FILE "/tmp/classicui_disc_state"

/*
  Poll intervals, in seconds. Both are status queries; neither reads the disc. See the
  comment in helper_main() for why the settled one is as long as it is.
*/
#define DISC_POLL_EMPTY_S    1
#define DISC_POLL_SETTLED_S  30

static pid_t helper_pid = -1;

// When the current (or most recent) helper was forked, and the path it was forked
// onto - kept so a refork after a death does not have to re-probe /dev/sr0 et al.,
// and so disc_refork_due() has a clock to measure against.
static int helper_fork_t = 0;
static char dev_path[32] = {};

// Consecutive helpers that died within DISC_HELPER_QUICK_DEATH_S of their own fork.
// See disc_refork_due().
static int quick_deaths = 0;

// disc_watch_start()'s own retry state: the last time it looked for a drive and
// found none, and whether "no optical drive" has already been said once. See
// disc_probe_due().
static int last_probe_t = DISC_NEVER_PROBED;
static int no_drive_logged = 0;

// ------------------------------------------------------------------ the helper

static int helper_fd = -1;

static void quiet_the_drive(const char *dev)
{
	const char *name = strrchr(dev, '/');
	name = name ? name + 1 : dev;

	char path[128];
	FILE *f;

	// Readahead is wasted on sectors asked for one at a time, and the kernel's own
	// media polling fights ours for the drive. Both best-effort.
	snprintf(path, sizeof(path), "/sys/block/%s/queue/read_ahead_kb", name);
	if ((f = fopen(path, "w"))) { fputs("0", f); fclose(f); }

	snprintf(path, sizeof(path), "/sys/block/%s/events_poll_msecs", name);
	if ((f = fopen(path, "w"))) { fputs("-1", f); fclose(f); }
}

/*
  Reading sectors, using the kernel's own paths rather than raw SCSI.

  The first version issued SCSI READ CD (0xBE) through SG_IO, which is what the fork
  does and what every ripping tool does. On this drive - an HL-DT-ST DVDRAM GUD1N
  over USB - it **wedges**: the request sits in `blk_execute_rq` indefinitely and the
  `io.timeout` never rescues it, because the command never reaches the drive to time
  out against. Measured, not guessed: the helper stayed in state D for minutes while
  the disc sat there perfectly readable.

  Perfectly readable by other means, that is. On the same drive and the same disc:

    - CDROMREADTOCHDR / CDROMREADTOCENTRY  work
    - CDROMREADRAW (one raw 2352-byte sector) works
    - an ordinary read() at lba * 2048     works

  So that is what this uses. The cost is that CDROMREADRAW is CD-only and one sector
  at a time, which is irrelevant here - identification reads a few dozen sectors once
  - and would matter only for streaming playback, which is not this file's job.

  Keep SG_IO in mind if a drive ever refuses CDROMREADRAW; the two are alternatives
  and the fork carries both for what is presumably this reason.
*/

// One raw 2352-byte sector, via the kernel rather than SG_IO.
static int ioctl_read_raw(int lba, uint8_t *dst)
{
	union
	{
		struct cdrom_msf msf;
		uint8_t raw[DISC_RAW_SIZE];
	} req;

	// CDROMREADRAW addresses by MSF, and MSF counts from the 2-second pregap.
	int f = lba + 150;
	memset(&req, 0, sizeof(req));
	req.msf.cdmsf_min0 = (uint8_t)(f / (75 * 60));
	req.msf.cdmsf_sec0 = (uint8_t)((f / 75) % 60);
	req.msf.cdmsf_frame0 = (uint8_t)(f % 75);

	if (ioctl(helper_fd, CDROMREADRAW, &req) < 0) return -1;
	memcpy(dst, req.raw, DISC_RAW_SIZE);
	return 0;
}

/*
  The 2048-byte user area, read as a block device. pread rather than lseek+read so
  the fd has no shared position - the helper is the only reader, but a stateless read
  is one less thing to reason about.
*/
static int block_read_user(int lba, uint8_t *dst)
{
	ssize_t n = pread(helper_fd, dst, DISC_USER_SIZE, (off_t)lba * DISC_USER_SIZE);
	return (n == DISC_USER_SIZE) ? 0 : -1;
}

static int helper_reader(int lba, int mode, uint8_t *dst, void *ctx)
{
	(void)ctx;
	if (helper_fd < 0) return -1;

	if (mode == DISC_READ_USER) return block_read_user(lba, dst);
	return ioctl_read_raw(lba, dst);
}

/*
  Where the first data track starts, or -1 when the table of contents is all audio -
  which is how an audio CD is recognised before a sector is read.
*/
static int find_data_track()
{
	struct cdrom_tochdr hdr;
	if (ioctl(helper_fd, CDROMREADTOCHDR, &hdr) < 0) return -1;

	for (int t = hdr.cdth_trk0; t <= hdr.cdth_trk1; t++)
	{
		struct cdrom_tocentry e;
		memset(&e, 0, sizeof(e));
		e.cdte_track = (uint8_t)t;
		e.cdte_format = CDROM_LBA;

		if (ioctl(helper_fd, CDROMREADTOCENTRY, &e) < 0) continue;
		if (e.cdte_ctrl & CDROM_DATA_TRACK) return e.cdte_addr.lba;
	}

	return -1;
}

static void helper_write(int state, int type, const char *serial, const char *label)
{
	char tmp[80];
	snprintf(tmp, sizeof(tmp), "%s.new", DISC_STATE_FILE);

	FILE *f = fopen(tmp, "w");
	if (!f) return;
	fprintf(f, "%d|%d|%s|%s\n", state, type, serial ? serial : "", label ? label : "");
	fclose(f);

	// Renamed into place so the parent never reads a half-written line.
	rename(tmp, DISC_STATE_FILE);
}

static void helper_main(const char *dev)
{
	helper_fd = open(dev, O_RDONLY | O_NONBLOCK);
	if (helper_fd < 0) _exit(1);

	quiet_the_drive(dev);
	disc_set_reader(helper_reader, 0);

	int last = -1;
	pid_t parent = getppid();

	for (;;)
	{
		// Leave with the front-end rather than outliving it holding the drive.
		if (getppid() != parent) _exit(0);

		int st = ioctl(helper_fd, CDROM_DRIVE_STATUS, CDSL_CURRENT);

		if (st != last)
		{
			last = st;

			if (st != CDS_DISC_OK)
			{
				helper_write(DISC_ABSENT, DISC_T_NONE, "", "");
			}
			else
			{
				// Say "there is a disc" before doing the slow part, so the front-end
				// can start its spinning icon while the drive is still seeking.
				helper_write(DISC_SPINNING, DISC_T_NONE, "", "");

				int lba0 = find_data_track();
				int t = disc_identify_at(lba0);

				char ser[DISC_SERIAL_LEN] = {};
				char lbl[DISC_LABEL_LEN] = {};
				disc_serial_at(lba0, ser, sizeof(ser));
				disc_label_at(lba0, lbl, sizeof(lbl));

				helper_write(t == DISC_T_UNKNOWN ? DISC_UNKNOWN : DISC_READY, t, ser, lbl);
			}
		}

		/*
		  How often to ask the drive again.

		  First, what this loop does *not* do, because I described it carelessly once and it
		  matters: the disc is read exactly once per insertion. Every sector access -
		  find_data_track, identify, serial, label - happens inside the state-change branch
		  above, which only runs when the status transitions. Once a disc is identified this
		  loop never touches its surface again. It does not re-spin the disc to keep the icon
		  turning; the icon is animation and knows nothing about the drive.

		  What remains is a status ioctl, which asks the drive's controller whether media is
		  present. That is not a disc read, but it is not free either: on some drives
		  TEST UNIT READY can provoke a spin-up to check the media, and there is no portable
		  way to know whether this drive is one of them.

		  Which leaves a genuine constraint rather than a bug: tray-open cannot be noticed
		  without somebody asking periodically. The kernel's own polling
		  (events_poll_msecs) is the same query on the same drive, just moved, and it is
		  disabled here anyway. So the choice is how often, and the honest position is "as
		  rarely as the interface tolerates":

		    disc present, identified   30s. Nothing is waiting on this. The only thing left
		                               to notice is the disc leaving, and a badge that
		                               lingers half a minute after an eject costs nothing -
		                               nothing acts on it, and re-identification happens on
		                               the transition back.
		    no disc                    1s. An insertion is something the player just did and
		                               is waiting to see acknowledged, and an empty drive
		                               has no disc to disturb.

		  If even the 30s query turns out to wake the drive on this hardware, the next step
		  is to stop entirely once identified and re-check only when the player opens the
		  disc prompt - at the cost of a badge that can be wrong until they look. That is a
		  product decision, not a technical one, and it is Derek's to make.
		*/
		int wait = (st == CDS_DISC_OK) ? DISC_POLL_SETTLED_S : DISC_POLL_EMPTY_S;

		sleep(wait);
	}
}

// ------------------------------------------------------------------ the parent

int disc_watching() { return watching; }

/*
  Fork a helper onto dev_path (already known to open) and record when, so
  disc_refork_due() has a clock to measure the next death against.

  A fork() failure is fed into the same quick_deaths counter as a helper that opens
  and immediately exits - to the caller both are "that attempt did not produce a
  running helper", and both should back off the same way rather than one of them
  retrying every frame forever.
*/
static int fork_helper()
{
	pid_t pid = fork();
	int now = (int)time(0);

	if (pid < 0)
	{
		quick_deaths++;
		helper_fork_t = now;
		helper_pid = -1;
		return 0;
	}

	if (!pid)
	{
		helper_main(dev_path);
		_exit(0);
	}

	helper_pid = pid;
	helper_fork_t = now;
	printf("ClassicUI: optical drive at %s, helper pid %d\n", dev_path, (int)pid);
	return 1;
}

int disc_watch_start()
{
	if (watching) return helper_pid > 0;
	if (!cfg.classicui_disc) return 0;

	int now = (int)time(0);
	if (!disc_probe_due(0, last_probe_t, now)) return 0;

	static const char *const paths[] = { "/dev/sr0", "/dev/cdrom", "/dev/sr1" };

	const char *dev = 0;
	for (size_t i = 0; !dev && i < sizeof(paths) / sizeof(paths[0]); i++)
	{
		// O_NONBLOCK: opening a drive with no disc in it otherwise hangs.
		int fd = open(paths[i], O_RDONLY | O_NONBLOCK);
		if (fd >= 0) { close(fd); dev = paths[i]; }
	}

	if (!dev)
	{
		// Looked, found nothing, and will look again in DISC_PROBE_RETRY_S rather than
		// never - see disc_probe_due(). watching stays 0 so disc_poll() keeps calling
		// back here every frame, but the retry timer - not this function being skipped
		// - is what keeps that cheap and the log quiet.
		last_probe_t = now;
		if (!no_drive_logged)
		{
			printf("ClassicUI: no optical drive\n");
			no_drive_logged = 1;
		}
		return 0;
	}

	unlink(DISC_STATE_FILE);
	snprintf(dev_path, sizeof(dev_path), "%s", dev);

	// The device node itself is the thing that was "found" - watching latches here
	// and stays latched even if the fork below fails or the helper dies later; see
	// fork_helper() and disc_refork_due() for how those get retried without
	// re-probing paths that are already known good.
	watching = 1;
	quick_deaths = 0;
	return fork_helper();
}

void disc_watch_stop()
{
	watching = 0;

	if (helper_pid > 0)
	{
		kill(helper_pid, SIGKILL);
		waitpid(helper_pid, 0, WNOHANG);   // never blocking: it may be stuck in an ioctl
		helper_pid = -1;
	}

	unlink(DISC_STATE_FILE);
	disc_reset_reader();
	disc_forget();
}

void disc_reset_reader()
{
	reader = 0;
	reader_ctx = 0;
}

/*
  The parent's whole involvement: has that little file changed, and if so what does it
  say. No device access, so this cannot block on the drive however wedged it is.

  Also where a dead helper is noticed and, subject to disc_refork_due()'s backoff,
  replaced. Neither is a device access either: waitpid(WNOHANG) asks the kernel about
  a process this one already owns, and fork() below re-runs the probe from the path
  already recorded in dev_path rather than re-opening /dev/sr0 et al.
*/
void disc_poll()
{
	if (!cfg.classicui_disc) return;

	if (!watching)
	{
		disc_watch_start();
	}
	else if (helper_pid > 0)
	{
		// Non-blocking for the same reason disc_watch_stop() reaps this way: a helper
		// stuck in an uninterruptible ioctl has not exited, so this never waits on one.
		int status = 0;
		if (waitpid(helper_pid, &status, WNOHANG) == helper_pid)
		{
			int now = (int)time(0);
			int ran = now - helper_fork_t;
			quick_deaths = (ran > DISC_HELPER_QUICK_DEATH_S) ? 0 : (quick_deaths + 1);
			helper_pid = -1;

			// The helper died with the drive; whatever it last wrote to the state file no
			// longer has anyone confirming it. Forget rather than leave a stale disc (or
			// worse, a stale identification) on screen with nothing watching to correct it.
			disc_forget();
		}
	}
	else if (disc_refork_due(quick_deaths, helper_fork_t, (int)time(0)))
	{
		fork_helper();
	}

	if (helper_pid <= 0) return;

	/*
	  Rate-limited, then compared by content - NOT by mtime.

	  This used to gate on st_mtime, which is **seconds** resolution on this filesystem.
	  The helper writes SPINNING and then READY, and when the disc is already spun up the
	  identification finishes inside the same second - so the second write had the same
	  mtime as the first and the front-end never saw it. The disc sat at SPINNING for
	  ever, which left the prompt with no Play row and made the disc unlaunchable. It
	  only ever worked when the drive was slow enough to push the two writes into
	  different seconds, which is why it looked intermittent.

	  Comparing the line itself cannot miss an update. The file is twenty bytes in tmpfs,
	  so the read is trivial; the counter is only there because this is called on every
	  pass of the draw loop and there is no point doing it thousands of times a second.
	*/
	static int skip = 0;
	if (++skip < 32) return;
	skip = 0;

	FILE *f = fopen(DISC_STATE_FILE, "r");
	if (!f) return;

	char line[160] = {};
	if (!fgets(line, sizeof(line), f)) { fclose(f); return; }
	fclose(f);

	static char last_line[160] = {};
	if (!strcmp(line, last_line)) return;
	snprintf(last_line, sizeof(last_line), "%s", line);

	char *nl = strchr(line, '\n');
	if (nl) *nl = 0;

	char *f1 = strchr(line, '|');
	if (!f1) return;
	*f1++ = 0;
	char *f2 = strchr(f1, '|');
	if (!f2) return;
	*f2++ = 0;
	char *f3 = strchr(f2, '|');
	if (f3) *f3++ = 0;

	int st = atoi(line);
	int t = atoi(f1);

	if (st < DISC_ABSENT || st > DISC_UNKNOWN) return;
	if (t < DISC_T_NONE || t > DISC_T_UNKNOWN) t = DISC_T_UNKNOWN;

	dstate = st;
	dtype = t;
	snprintf(dserial, sizeof(dserial), "%s", f2);
	snprintf(dlabel, sizeof(dlabel), "%s", f3 ? f3 : "");
	ddirty = 1;

	printf("ClassicUI: disc state=%d %s", dstate, disc_type_name(dtype));
	if (dlabel[0]) printf(" \"%s\"", dlabel);
	if (dserial[0]) printf(" %s", dserial);
	printf("\n");
}

#endif
