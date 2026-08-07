#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <time.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include "chome_rip.h"

#ifndef CHOME_HOST_TEST
#include <signal.h>
#include <sys/wait.h>
#include <fcntl.h>
#endif

/*
  See chome_rip.h for what this is, what the format is and why the drive is not opened
  here. Everything down to "the helper" is pure and testable; below it is a forked
  process and physical_disc, which the harness compiles out.
*/

static unsigned long rip_now_ms()
{
	struct timespec ts;
	if (clock_gettime(CLOCK_MONOTONIC, &ts)) return 0;
	return (unsigned long)ts.tv_sec * 1000UL + (unsigned long)(ts.tv_nsec / 1000000L);
}

/* ------------------------------------------------------------ the arithmetic --- */

void rip_msf(int sectors, int *m, int *s, int *f)
{
	if (sectors < 0) sectors = 0;
	*m = sectors / (75 * 60);
	*s = (sectors / 75) % 60;
	*f = sectors % 75;
}

int rip_percent(const rip_status *st)
{
	if (!st || st->total <= 0) return 0;

	if (st->state == RIP_DONE) return 100;

	long long pc = (long long)st->done * 100 / st->total;
	if (pc < 0) pc = 0;

	// 99 is the ceiling while it is still working: the last track's close and the sheet's
	// write happen after the final sector, and a bar reading 100% through them is a bar
	// that has finished over a screen that has not.
	if (pc > 99) pc = 99;
	return (int)pc;
}

long long rip_bytes_needed(const rip_plan *p)
{
	if (!p) return 0;

	long long n = 0;
	for (int i = 0; i < p->n; i++) n += (long long)p->t[i].sectors * PHYSICAL_DISC_RAW;

	/*
	  Plus the sheet, and one cluster's worth of slack per file rather than the exact byte
	  count. A rip that fits to the last byte on paper does not fit on exFAT, where a
	  cluster is routinely 128 KB - so a forty-track audio CD loses megabytes to rounding
	  that an exact sum would never have asked for.
	*/
	n += (long long)(p->n + 1) * 128 * 1024;
	return n;
}

int rip_space_ok(long long need, long long avail)
{
	// Nothing known about the filesystem: allowed through. Refusing every rip because
	// statvfs did not recognise the card would be a worse failure than the one this
	// guards against, and a copy that runs out of room fails safely - the write returns
	// short, the rip aborts and the staging folder goes.
	if (avail <= 0) return 1;

	return avail >= need + (long long)RIP_SPARE_MB * 1024 * 1024;
}

long long rip_free_bytes(const char *dir)
{
	struct statvfs vfs;
	if (!dir || statvfs(dir, &vfs)) return 0;

	// f_frsize, not f_bsize: the first is the fragment size the block counts are in, the
	// second is only a hint about efficient I/O. They differ on some filesystems and using
	// the wrong one overstates the free space by a whole factor.
	unsigned long unit = vfs.f_frsize ? vfs.f_frsize : vfs.f_bsize;
	return (long long)vfs.f_bavail * (long long)unit;
}

int rip_folder_name(const char *title, char *out, int outsz)
{
	if (!out || outsz < 2) return 0;
	out[0] = 0;
	if (!title) return 0;

	int n = 0;
	int pending_us = 0;                       // an underscore owed, not yet written

	for (const char *q = title; *q && n < outsz - 1; q++)
	{
		unsigned char c = (unsigned char)*q;

		int ok = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
			|| c == '-' || c == ' ' || c == '(' || c == ')' || c == '.' || c == '\'';

		// A run of anything else collapses to one underscore rather than one each, so
		// "Wing Commander III: The Heart of the Tiger" does not come out with a hole in it.
		if (!ok) { pending_us = 1; continue; }

		if (pending_us && n) { out[n++] = '_'; pending_us = 0; }
		if (n < outsz - 1) out[n++] = (char)c;
	}
	out[n] = 0;

	// Trailing spaces and dots are legal in the string and not in a FAT directory entry.
	while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == '.' || out[n - 1] == '_')) out[--n] = 0;

	// Leading spaces likewise, and a name that reduced to nothing at all is not a name.
	int lead = 0;
	while (out[lead] == ' ') lead++;
	if (lead) memmove(out, out + lead, strlen(out + lead) + 1);

	if (!out[0]) return 0;
	return (int)strlen(out);
}

/* ------------------------------------------------------------------ the plan --- */

/*
  Which mode a data track's sectors are in, read off the disc rather than assumed.

  A raw sector starts with a twelve-byte sync pattern, three bytes of address and then the
  mode: 1 for MODE1, 2 for MODE2. The drive's table of contents cannot say - a TOC entry
  has one data bit and no mode - so physical_disc marks every data track TT_MODE1, and a
  sheet that copied that would claim MODE1/2352 for most of the PlayStation library. The
  answer is in the sector, so this reads one.

  Tries INDEX 01 first and then a couple of sectors further in: the first sector of a
  track is the one most likely to be marginal on a scratched disc, and a mode taken from a
  read that failed would be a guess dressed up as a measurement.
*/
static int rip_track_mode(int index01, int (*rd)(int, uint8_t*, void*), void *ctx)
{
	static const uint8_t sync[12] =
		{ 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00 };

	if (!rd) return TT_MODE1;

	static const int probe[] = { 0, 1, 16 };
	uint8_t raw[PHYSICAL_DISC_RAW];

	for (unsigned i = 0; i < sizeof(probe) / sizeof(probe[0]); i++)
	{
		if (rd(index01 + probe[i], raw, ctx)) continue;
		if (memcmp(raw, sync, sizeof(sync))) continue;

		if (raw[15] == 2) return TT_MODE2;
		if (raw[15] == 1) return TT_MODE1;
	}

	/*
	  Unreadable, or a sector with no sync pattern where one should be. MODE1 is the safer
	  wrong answer: a MODE2 sector described as MODE1/2352 still has all 2352 of its bytes
	  in the file so nothing is lost, whereas the reverse invites a reader to look for a
	  subheader that is not there - and three of the five parsers in this tree reject a
	  MODE2 token outright.
	*/
	return TT_MODE1;
}

int rip_plan_build(const toc_t *toc, rip_plan *out, int (*rd)(int, uint8_t*, void*), void *ctx)
{
	if (!toc || !out) return 0;

	memset(out, 0, sizeof(*out));
	if (toc->last < 1) return 0;

	for (int i = 0; i < toc->last && i < RIP_TRACK_MAX; i++)
	{
		const cd_track_t *s = &toc->tracks[i];

		/*
		  `start` is INDEX 00 where physical_disc_psx_enrich_toc() found a pregap and INDEX
		  01 where it did not, and indexes[1] is that pregap's length in the first case and
		  0 in the second. `end` is exclusive - load_toc() sets each track's end to the next
		  one's start and the last one's to the leadout - so the length is a subtraction and
		  not a subtraction minus one, which is the classic way to lose the last sector of
		  every track on the disc.
		*/
		int len = s->end - s->start;
		if (len <= 0) continue;               // a zero-length track is not writable

		rip_track *t = &out->t[out->n];
		t->num = i + 1;
		t->start = s->start;
		t->sectors = len;
		t->pregap = (s->indexes[1] > 0 && s->indexes[1] < len) ? s->indexes[1] : 0;

		t->type = (s->type == TT_CDDA) ? TT_CDDA
			: rip_track_mode(s->start + t->pregap, rd, ctx);

		out->sectors += len;
		out->n++;
	}

	return out->n;
}

/*
  The token, for the reader that is going to see it.

  `mode1_only` is Mega CD, Neo Geo CD and PC Engine CD, whose parsers understand no MODE2
  token at all; see the format notes in the header. A measured MODE2 track written for one
  of those is reported in the log, because that combination means the disc is not what the
  console it is being ripped for expects and the sheet is the wrong place to find that out.
*/
static const char *rip_mode_token(int type, int mode1_only)
{
	if (type == TT_CDDA) return "AUDIO";
	if (type == TT_MODE2 && !mode1_only) return "MODE2/2352";
	return "MODE1/2352";
}

void rip_track_file(int num, char *out, int outsz)
{
	// "Track 01.bin". The word, the space and the two digits are not cosmetic: that is the
	// shape commit 73b0f71 taught the scanner to read as a part of a game rather than as a
	// game, and title_is_part_only() matches on the word followed by digits.
	snprintf(out, outsz, "Track %02d.bin", num);
}

int rip_cue_text(const rip_plan *p, int mode1_only, char *out, int outsz)
{
	if (!out || outsz < 2) return 0;
	out[0] = 0;
	if (!p || p->n < 1) return 0;

	int n = 0;

	// One line at a time through a fixed buffer, then appended if it fits, so a sheet that
	// would overflow is refused rather than silently truncated into an unparseable one.
	for (int i = 0; i < p->n; i++)
	{
		const rip_track *t = &p->t[i];

		char file[64];
		rip_track_file(t->num, file, sizeof(file));

		char blk[256];
		int b = 0;

		/*
		  Indented with spaces and never with tabs, and the keywords in capitals. Every cue
		  reader in this tree skips leading 0x20 and only 0x20 before matching, and compares
		  with memcmp and strstr against uppercase - so a tab-indented or lower-cased sheet
		  parses as no keywords at all, and megacdd and pcecdd then fall back to opening the
		  cue file itself as track data.
		*/
		b += snprintf(blk + b, sizeof(blk) - b, "FILE \"%s\" BINARY\n", file);
		b += snprintf(blk + b, sizeof(blk) - b, "  TRACK %02d %s\n",
			t->num, rip_mode_token(t->type, mode1_only));

		/*
		  INDEX 00 only where there is a pregap to point at, and always 00:00:00: the
		  pregap sectors are at the head of this track's own file, so the file's start *is*
		  INDEX 00. The multi-FILE readers ignore its position and take the pregap's length
		  from INDEX 01, which is the next line.
		*/
		if (t->pregap > 0) b += snprintf(blk + b, sizeof(blk) - b, "    INDEX 00 00:00:00\n");

		int m, s, f;
		rip_msf(t->pregap, &m, &s, &f);
		b += snprintf(blk + b, sizeof(blk) - b, "    INDEX 01 %02d:%02d:%02d\n", m, s, f);

		if (b < 0 || b >= (int)sizeof(blk)) return 0;
		if (n + b >= outsz) { out[0] = 0; return 0; }

		memcpy(out + n, blk, b);
		n += b;
		out[n] = 0;
	}

	return n;
}

/* ------------------------------------------------------------------ the copy --- */

void rip_stage_path(const char *games_dir, const char *name, char *out, int outsz)
{
	snprintf(out, outsz, "%s/%s%s%s", games_dir, RIP_STAGE_PREFIX, name, RIP_STAGE_SUFFIX);
}

int rip_folder_exists(const char *games_dir, const char *name)
{
	char path[1024];
	snprintf(path, sizeof(path), "%s/%s", games_dir, name);

	struct stat st;
	return !stat(path, &st);
}

void rip_rmdir_flat(const char *dir)
{
	DIR *d = opendir(dir);
	if (!d) return;

	struct dirent *de;
	while ((de = readdir(d)))
	{
		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;

		char path[1024];
		snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
		unlink(path);
	}
	closedir(d);

	rmdir(dir);
}

/*
  One sector, insisted upon.

  Two failures are being told apart here without being able to see which is which. A
  scratch fails every attempt instantly; a bus reset - the dock's drive does this, and a
  rip is long enough that it will - fails every attempt for a few seconds and then starts
  working again, because support/physical_disc re-attaches the device on its own. So one
  loop covers both: retry, and if the sector is still refused, keep coming back to it for
  up to RIP_STALL_MS before giving up on it.

  Progress is left where it is while that happens, and it happens by construction rather
  than by a flag: `done` cannot advance until this returns, so no note is published and
  the percentage the player is looking at stops moving. The disc keeps turning, because
  the spin is armed off rip_busy() and not off progress - which is the honest pair of
  facts to show. A bar that crept on through a stall would be lying about the one thing
  it is for.
*/
static int rip_read_insist(const rip_io *io, int lba, uint8_t *dst)
{
	unsigned long t0 = 0;

	for (;;)
	{
		for (int try_n = 0; try_n < RIP_READ_TRIES; try_n++)
		{
			if (!io->read(lba, dst, io->read_ctx)) return 0;
		}

		if (io->cancelled && io->cancelled(io->cancel_ctx)) return -2;

		unsigned long now = rip_now_ms();
		if (!t0)
		{
			t0 = now ? now : 1;
			printf("ClassicUI: rip: lba %d will not read, waiting for the drive\n", lba);
		}
		else if (now - t0 >= RIP_STALL_MS) return -1;

		/*
		  A tenth of a second between rounds. Not zero: a tight loop against a device that
		  is not there is the one way a rip could starve the rest of the machine, and the
		  recovery it is waiting on takes seconds rather than microseconds.
		*/
		struct timespec ts = { 0, 100 * 1000 * 1000 };
		nanosleep(&ts, 0);
	}
}

int rip_run(const rip_plan *p, const char *dir, const char *base, int mode1_only,
	const rip_io *io, int *bad)
{
	if (bad) *bad = 0;
	if (!p || !dir || !base || !io || !io->read) return RIP_FAILED;

	int done = 0;
	int nbad = 0;

	// What did not read, recorded as it happens rather than reconstructed afterwards, and
	// left in the folder. See RIP_BADFILE.
	char badpath[1024];
	snprintf(badpath, sizeof(badpath), "%s/%s", dir, RIP_BADFILE);
	FILE *bf = 0;

	for (int i = 0; i < p->n; i++)
	{
		const rip_track *t = &p->t[i];

		char file[64], path[1024];
		rip_track_file(t->num, file, sizeof(file));
		snprintf(path, sizeof(path), "%s/%s", dir, file);

		FILE *f = fopen(path, "wb");
		if (!f) { if (bf) fclose(bf); return RIP_FAILED; }

		for (int k = 0; k < t->sectors; k++)
		{
			if (io->cancelled && io->cancelled(io->cancel_ctx))
			{
				fclose(f);
				if (bf) fclose(bf);
				return RIP_CANCELLED;
			}

			uint8_t sec[PHYSICAL_DISC_RAW];
			int r = rip_read_insist(io, t->start + k, sec);

			if (r == -2) { fclose(f); if (bf) fclose(bf); return RIP_CANCELLED; }

			if (r)
			{
				// Given up on. Zero-filled so the file stays exactly the length the sheet
				// says, and listed so the player is told the copy is imperfect.
				memset(sec, 0, sizeof(sec));
				nbad++;

				if (!bf && (bf = fopen(badpath, "wb")))
				{
					fprintf(bf, "# Sectors of this disc that would not read, after %d attempts each.\n",
						RIP_READ_TRIES);
					fprintf(bf, "# They are in the track files as 2352 zero bytes, so every track is\n");
					fprintf(bf, "# the length the cue sheet says it is. A zeroed audio frame is a click;\n");
					fprintf(bf, "# a zeroed data sector may stop the game loading.\n");
					fprintf(bf, "# track  type   lba\n");
				}
				if (bf) fprintf(bf, "%7d  %-5s  %d\n", t->num,
					(t->type == TT_CDDA) ? "audio" : "data", t->start + k);
			}

			if (fwrite(sec, 1, sizeof(sec), f) != sizeof(sec))
			{
				// The card filled up or went away mid-rip. Not survivable and not worth
				// pretending about: the staging folder goes and the player is told.
				fclose(f);
				if (bf) fclose(bf);
				return RIP_FAILED;
			}

			done++;

			// Every 64 sectors and not every one: a note ends in a rename in the firmware,
			// and 333,000 renames is real work in the middle of an I/O-bound job.
			if (io->note && !(done & 63)) io->note(done, p->sectors, nbad, io->note_ctx);
		}

		if (fclose(f)) { if (bf) fclose(bf); return RIP_FAILED; }
		if (io->note) io->note(done, p->sectors, nbad, io->note_ctx);
	}

	if (bf) fclose(bf);

	/*
	  And the sheet last, deliberately.

	  The scanner treats a folder holding a .cue as a game whose parts are beside it, so a
	  sheet written first would describe tracks that do not exist yet for the length of the
	  rip. Writing it last makes the presence of the sheet the same fact as the rip being
	  complete - which is the second guard behind the staging folder, and the one that
	  still holds if somebody moves a staging folder into place by hand.
	*/
	char cue[16 * 1024];
	if (!rip_cue_text(p, mode1_only, cue, sizeof(cue))) return RIP_FAILED;

	char cuepath[1024];
	snprintf(cuepath, sizeof(cuepath), "%s/%s.cue", dir, base);

	size_t len = strlen(cue);
	FILE *cf = fopen(cuepath, "wb");
	if (!cf) return RIP_FAILED;
	if (fwrite(cue, 1, len, cf) != len) { fclose(cf); return RIP_FAILED; }
	if (fclose(cf)) return RIP_FAILED;

	if (bad) *bad = nbad;
	return RIP_DONE;
}

int rip_perform(const rip_plan *p, const char *games_dir, const char *name, int mode1_only,
	int overwrite, const rip_io *io, int *bad)
{
	if (bad) *bad = 0;
	if (!p || !games_dir || !name || !name[0] || p->n < 1) return RIP_FAILED;

	// Never over the top of a finished rip without having been told to. The confirmation is
	// the front-end's; this is the guard behind it, and it also covers the case where the
	// folder turned up between the player being asked and the child getting here.
	if (!overwrite && rip_folder_exists(games_dir, name)) return RIP_EXISTS;

	if (!rip_space_ok(rip_bytes_needed(p), rip_free_bytes(games_dir))) return RIP_NOSPACE;

	char stage[1024];
	rip_stage_path(games_dir, name, stage, sizeof(stage));

	/*
	  A staging folder left by an earlier attempt that did not get to clean up - a power
	  cut, or a helper killed while wedged. Nothing in it is worth keeping and there is no
	  way to know how far it got: the sheet is written last, so an abandoned staging folder
	  has no sheet and nothing that says which sectors in it are real.
	*/
	rip_rmdir_flat(stage);

	if (mkdir(stage, 0777) && errno != EEXIST) return RIP_FAILED;

	int st = rip_run(p, stage, name, mode1_only, io, bad);
	if (st != RIP_DONE) { rip_rmdir_flat(stage); return st; }

	char final_dir[1024];
	snprintf(final_dir, sizeof(final_dir), "%s/%s", games_dir, name);

	/*
	  The old copy goes now and not earlier, which is the whole value of the staging folder.
	  Up to this line a cancelled or failed overwrite has left what the player already had
	  untouched; from here there is a finished replacement ready to take its place.

	  Flat, and deliberately not recursive: a rip's folder is a sheet and its tracks and
	  nothing else, so a folder with a subdirectory in it is not one this wrote. rmdir then
	  refuses it, which fails the rename below and leaves both copies rather than deleting
	  something this has no business deleting.
	*/
	if (overwrite) rip_rmdir_flat(final_dir);

	/*
	  And into place in one step. rename(2) within a directory is atomic, so there is no
	  instant at which the shelf can see a partly built folder: the name the scanner looks
	  at either does not exist or holds a complete rip.
	*/
	if (rename(stage, final_dir))
	{
		rip_rmdir_flat(stage);
		return RIP_FAILED;
	}

	return RIP_DONE;
}

/* ------------------------------------------------------------------ the helper --- */

static rip_status rst;

const rip_status *rip_state() { return &rst; }

#ifdef CHOME_HOST_TEST

/*
  No child and no drive in the harness. The copy above is what has anything true to say
  about a rip; forking a process that opens /dev/sr0 is not something a host test can assert
  about, which is the same split chome_disc.cpp draws in the same place.

  What is kept is the *state*, because that is what the front-end draws from. In the
  firmware the parent knows about a rip through exactly these fields and nothing else, so a
  harness that sets them is driving the real screens through the real seam - see
  rip_test_set() in the header.
*/
static int rip_test_state = RIP_IDLE;
static int rip_test_nstarts = 0;
static char rip_test_dir[1024];
static char rip_test_name[96];
static int rip_test_mode1, rip_test_over;

int rip_start(const char *games_dir, const char *name, const char *title,
	int mode1_only, int overwrite)
{
	(void)title;
	if (!games_dir || !name || !name[0]) return 0;

	snprintf(rip_test_dir, sizeof(rip_test_dir), "%s", games_dir);
	snprintf(rip_test_name, sizeof(rip_test_name), "%s", name);
	rip_test_mode1 = mode1_only;
	rip_test_over = overwrite;
	rip_test_nstarts++;

	// As the firmware's child does before it has read a table of contents: running, with no
	// total yet, which is the state the "Reading the disc" line exists for.
	memset(&rst, 0, sizeof(rst));
	rst.state = RIP_RUNNING;
	snprintf(rst.name, sizeof(rst.name), "%s", name);
	rip_test_state = RIP_RUNNING;
	return 1;
}

int  rip_poll() { return 0; }
int  rip_busy() { return rip_test_state == RIP_RUNNING; }
int  rip_reportable() { return rip_test_state != RIP_IDLE && rip_test_state != RIP_RUNNING; }

void rip_ack() { memset(&rst, 0, sizeof(rst)); rip_test_state = RIP_IDLE; }

void rip_cancel()
{
	if (rip_test_state != RIP_RUNNING) return;
	rst.state = RIP_CANCELLED;
	rip_test_state = RIP_CANCELLED;
}

void rip_forget() { memset(&rst, 0, sizeof(rst)); rip_test_state = RIP_IDLE; }

void rip_test_set(int state, int done, int total, int bad)
{
	rst.state = state;
	rst.done = done;
	rst.total = total;
	rst.bad = bad;
	rip_test_state = state;

	// The two numbers the refusal's wording needs, derived from the disc rather than passed
	// in: what a rip of this many sectors would want, and a card with half of that free.
	// Only RIP_NOSPACE ever shows them, and only as text.
	rst.need_mb = (int)((long long)total * PHYSICAL_DISC_RAW / (1024 * 1024));
	rst.free_mb = rst.need_mb / 2;
}

void rip_test_reset()
{
	memset(&rst, 0, sizeof(rst));
	rip_test_state = RIP_IDLE;
	rip_test_nstarts = 0;
	rip_test_dir[0] = 0;
	rip_test_name[0] = 0;
	rip_test_mode1 = rip_test_over = 0;
}

int  rip_test_starts() { return rip_test_nstarts; }
const char *rip_test_last_dir() { return rip_test_dir; }
const char *rip_test_last_name() { return rip_test_name; }
int  rip_test_last_mode1() { return rip_test_mode1; }
int  rip_test_last_overwrite() { return rip_test_over; }

#else

static pid_t rip_pid = -1;
static char rip_games_dir[1024];
static char rip_last_line[256];

// ------------------------------------------------------------------- the child

struct child_ctx
{
	int done;
	unsigned long last_ms;
};

static char child_name[96];
static int child_need_mb, child_free_mb;

static void child_publish(int state, int done, int total, int nbad, int tracks)
{
	char tmp[128];
	snprintf(tmp, sizeof(tmp), "%s.new", RIP_PROGRESS_FILE);

	FILE *f = fopen(tmp, "w");
	if (!f) return;
	fprintf(f, "%d|%d|%d|%d|%d|%d|%d|%s\n", state, done, total, nbad, tracks,
		child_need_mb, child_free_mb, child_name);
	fclose(f);

	// Renamed into place so the parent never reads a half-written line, exactly the way
	// the disc state file and the ident file are published.
	rename(tmp, RIP_PROGRESS_FILE);
}

static int child_read(int lba, uint8_t *dst, void *ctx)
{
	(void)ctx;
	// No subchannel asked for: a rip writes 2352-byte tracks, nothing in this tree reads a
	// separate .sub that this would have written, and it would be a second thing to fail
	// per sector on a disc where the sectors are the point.
	return physical_disc_read_sector(lba, dst, 0);
}

static void child_note(int done, int total, int nbad, void *ctx)
{
	child_ctx *c = (child_ctx*)ctx;
	c->done = done;

	/*
	  Rate-limited here rather than in rip_run(), because this is where the cost is: a note
	  is a write and a rename in tmpfs and the copy calls it every 64 sectors, which on a
	  full CD is five thousand times. Four a second is more than the eye needs on a bar
	  that takes minutes to cross, and the last one is never dropped.
	*/
	unsigned long now = rip_now_ms();
	if (c->last_ms && now - c->last_ms < 250 && done < total) return;
	c->last_ms = now ? now : 1;

	child_publish(RIP_RUNNING, done, total, nbad, 0);
}

static int child_cancelled(void *ctx)
{
	(void)ctx;

	/*
	  A file rather than a signal. A signal would arrive while the child is inside an ioctl
	  on a drive that is not answering, where it can neither be handled nor unwind the
	  staging folder; a file is looked at between two sectors, which is a moment when there
	  is something sensible to do about it.
	*/
	struct stat st;
	return !stat(RIP_CANCEL_FILE, &st);
}

static void child_main(const char *games_dir, const char *name, int mode1_only, int overwrite)
{
	snprintf(child_name, sizeof(child_name), "%s", name);
	child_publish(RIP_RUNNING, 0, 0, 0, 0);

	if (physical_disc_open(0))
	{
		printf("ClassicUI: rip: no drive\n");
		child_publish(RIP_FAILED, 0, 0, 0, 0);
		_exit(1);
	}

	/*
	  Off with the speed cap.

	  The playback path holds the drive to CAPPED_SPEED_NX because a core wants sectors in
	  real time, and a drive spinning up to its full rate makes the audio glitch and the
	  seeks audible. A rip has no such constraint: nothing is listening and the sectors go
	  into a file. On a 700 MB disc the difference is tens of minutes, so native speed, and
	  asked for before the table of contents is read because that is where the cap is
	  applied.

	  It is also the one thing here that a real drive could disagree about. If a drive
	  turns out to read *less* reliably uncapped - more retries, more zero-filled sectors -
	  this is the single line to turn round, and the bad-sector count is the measurement
	  that would say so.
	*/
	physical_disc_native_speed(1);

	toc_t toc;
	if (physical_disc_load_toc(&toc))
	{
		printf("ClassicUI: rip: cannot read the table of contents\n");
		child_publish(RIP_FAILED, 0, 0, 0, 0);
		physical_disc_close();
		_exit(1);
	}

	/*
	  And the pregaps, where the drive can give them.

	  This needs the raw Q subchannel, which only the SCSI path returns - on a drive
	  answering through the kernel ioctls it declines and says so, and the basic table of
	  contents stands. That is not a failure: a sheet with no INDEX 00 lines is what a cue
	  without pregaps has always meant and every track still starts where the drive says it
	  does. What it costs is the two seconds of silence before a track that had a real
	  pregap, which is audible on a Mega CD soundtrack and on nothing at all on a data disc.
	*/
	physical_disc_psx_enrich_toc(&toc);

	rip_plan plan;
	if (!rip_plan_build(&toc, &plan, child_read, 0))
	{
		printf("ClassicUI: rip: nothing on this disc to write\n");
		child_publish(RIP_FAILED, 0, 0, 0, 0);
		physical_disc_close();
		_exit(1);
	}

	long long need = rip_bytes_needed(&plan);
	long long avail = rip_free_bytes(games_dir);
	child_need_mb = (int)(need / (1024 * 1024));
	child_free_mb = (int)(avail / (1024 * 1024));

	printf("ClassicUI: rip: %d tracks, %d sectors, %d MB into %s/%s (%d MB free)\n",
		plan.n, plan.sectors, child_need_mb, games_dir, name, child_free_mb);
	for (int i = 0; i < plan.n; i++)
	{
		printf("ClassicUI: rip: track %02d %s lba %d + %d sectors, pregap %d\n",
			plan.t[i].num, rip_mode_token(plan.t[i].type, mode1_only),
			plan.t[i].start, plan.t[i].sectors, plan.t[i].pregap);

		// The one case where the measurement and the token disagree, said out loud rather
		// than written into the sheet where nobody would look for it.
		if (mode1_only && plan.t[i].type == TT_MODE2)
			printf("ClassicUI: rip: track %02d measured MODE2 but this core's cue parser "
				"knows no MODE2 token, so the sheet says MODE1/2352\n", plan.t[i].num);
	}

	child_publish(RIP_RUNNING, 0, plan.sectors, 0, plan.n);

	child_ctx cc = { 0, 0 };

	rip_io io;
	memset(&io, 0, sizeof(io));
	io.read = child_read;
	io.note = child_note;
	io.note_ctx = &cc;
	io.cancelled = child_cancelled;

	int nbad = 0;
	int st = rip_perform(&plan, games_dir, name, mode1_only, overwrite, &io, &nbad);

	printf("ClassicUI: rip: finished state %d, %d unreadable sectors\n", st, nbad);
	child_publish(st, (st == RIP_DONE) ? plan.sectors : cc.done, plan.sectors, nbad, plan.n);

	physical_disc_close();
	_exit(0);
}

// ------------------------------------------------------------------ the parent

int rip_start(const char *games_dir, const char *name, const char *title,
	int mode1_only, int overwrite)
{
	if (rip_pid > 0) return 0;
	if (!games_dir || !name || !name[0]) return 0;

	snprintf(rip_games_dir, sizeof(rip_games_dir), "%s", games_dir);

	unlink(RIP_PROGRESS_FILE);
	unlink(RIP_CANCEL_FILE);

	// So that the first line of this rip cannot be mistaken for the last line of the one
	// before it, which is a real collision: both are "1|0|0|0|0|..." for the same folder.
	rip_last_line[0] = 0;

	memset(&rst, 0, sizeof(rst));
	rst.state = RIP_RUNNING;
	snprintf(rst.name, sizeof(rst.name), "%s", name);

	pid_t pid = fork();
	if (pid < 0) { rst.state = RIP_FAILED; return 0; }

	if (!pid)
	{
		child_main(games_dir, name, mode1_only, overwrite);
		_exit(0);
	}

	rip_pid = pid;
	printf("ClassicUI: ripping \"%s\" to %s/%s, helper pid %d\n",
		title ? title : name, games_dir, name, (int)pid);
	return 1;
}

int rip_busy() { return rip_pid > 0; }

int rip_reportable()
{
	return !rip_busy() && rst.state != RIP_IDLE && rst.state != RIP_RUNNING;
}

void rip_ack()
{
	memset(&rst, 0, sizeof(rst));
	unlink(RIP_PROGRESS_FILE);
}

int rip_poll()
{
	if (rip_pid <= 0) return 0;

	int changed = 0;

	/*
	  The line first and the child's exit second, in that order on purpose: the child
	  publishes its verdict and then exits, so reaping first would drop the final line on
	  the one pass where both happen and leave the screen at 99% for ever.
	*/
	FILE *f = fopen(RIP_PROGRESS_FILE, "r");
	if (f)
	{
		char line[256] = {};
		if (fgets(line, sizeof(line), f) && strcmp(line, rip_last_line))
		{
			snprintf(rip_last_line, sizeof(rip_last_line), "%s", line);

			int st = 0, done = 0, total = 0, nbad = 0, tracks = 0, need = 0, freemb = 0;
			char nm[96] = {};
			if (sscanf(line, "%d|%d|%d|%d|%d|%d|%d|%95[^\n]",
				&st, &done, &total, &nbad, &tracks, &need, &freemb, nm) >= 7)
			{
				rst.state = st;
				rst.done = done;
				rst.total = total;
				rst.bad = nbad;
				rst.tracks = tracks;
				rst.need_mb = need;
				rst.free_mb = freemb;
				if (nm[0]) snprintf(rst.name, sizeof(rst.name), "%s", nm);
				changed = 1;
			}
		}
		fclose(f);
	}

	int status = 0;
	pid_t r = waitpid(rip_pid, &status, WNOHANG);
	if (r == rip_pid || (r < 0 && errno == ECHILD))
	{
		rip_pid = -1;
		unlink(RIP_CANCEL_FILE);

		/*
		  A child that died without publishing a verdict - killed, or crashed - leaves the
		  line saying RUNNING, which on screen is a progress face that never finishes. And
		  the staging folder is removed from here rather than trusted to a process that is
		  no longer there to do it.
		*/
		if (rst.state == RIP_RUNNING || rst.state == RIP_IDLE) rst.state = RIP_FAILED;

		if (rst.state != RIP_DONE && rst.name[0])
		{
			char stage[1024];
			rip_stage_path(rip_games_dir, rst.name, stage, sizeof(stage));
			rip_rmdir_flat(stage);
		}

		changed = 1;
	}

	return changed;
}

void rip_cancel()
{
	if (rip_pid <= 0) return;

	// The polite ask first: the child sees the file between two sectors, stops, and its
	// staging folder goes with rip_perform()'s own cleanup.
	FILE *f = fopen(RIP_CANCEL_FILE, "w");
	if (f) { fputs("1\n", f); fclose(f); }

	rst.state = RIP_CANCELLED;

	/*
	  And then the kill, without waiting to be answered.

	  The child may be inside an uninterruptible ioctl on a drive that has stopped
	  responding - which is the whole reason this is a process - and in that case it will
	  never see the file. So it is killed, and this side removes the staging folder,
	  because after a SIGKILL there is nobody else left who could.
	*/
	kill(rip_pid, SIGKILL);
	waitpid(rip_pid, 0, WNOHANG);            // never blocking: it may be stuck in an ioctl
	rip_pid = -1;

	if (rst.name[0])
	{
		char stage[1024];
		rip_stage_path(rip_games_dir, rst.name, stage, sizeof(stage));
		rip_rmdir_flat(stage);
	}

	unlink(RIP_CANCEL_FILE);
	printf("ClassicUI: rip cancelled, nothing left behind\n");
}

void rip_forget()
{
	if (rip_pid > 0) rip_cancel();
	memset(&rst, 0, sizeof(rst));
	rip_last_line[0] = 0;
	unlink(RIP_PROGRESS_FILE);
	unlink(RIP_CANCEL_FILE);
}

#endif
