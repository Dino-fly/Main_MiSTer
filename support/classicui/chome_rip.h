/*
  Classic Home - copying the disc in the drive onto the card.

  ---------------------------------------------------------------------------
  This writes a .cue and its tracks. It does not read the drive itself, and it
  does not draw. Both of those belong to somebody else and the seams are why.
  ---------------------------------------------------------------------------

  What this is for. The disc dialog can hand a pressed disc to a core, which reads it in
  real time and needs the drive to keep up for as long as the game lasts. A rip is the
  other answer to the same disc: copy it once, slowly, and afterwards the game loads off
  the card like every other game on the shelf - no drive, no dock, no spin-up, and no
  disc to keep in the tray.

  Three collaborators, and none of them is this file's business:

    support/physical_disc      owns the drive. It has the table of contents, the raw
                               2352-byte sector reads, the speed cap and - the part that
                               matters most here - the drive-loss detection and the
                               re-attach after a USB reset. A rip is minutes long and a
                               bus that resets mid-rip is an ordinary event on this dock,
                               so this file must survive one rather than reimplement the
                               recovery. It does that by treating "cannot read that
                               sector" as something to sit on and come back to rather
                               than as a failure; see rip_read_insist().

    chome_disc.cpp             owns *detection*, in its own helper process. The drive has
                               exactly one owner, so the front-end stops that helper
                               before a rip starts, the same way it stops it before
                               handing a disc to a core. See disc_launch().

    chome_ui.cpp               owns the screen, the folder's name and which core the rip
                               is for. It already knows the disc by the best name
                               anything knows it by - the title table's answer, else the
                               volume label, else the serial - and that name is what the
                               card on the shelf will be called, so it is the name the
                               folder gets.

  Structure, and it is the same split chome_disc.cpp draws for the same reason:
  everything above "the helper" is pure and goes through an injected reader, so the
  harness drives a synthetic multi-track disc through the whole thing and asserts on the
  bytes that come out. Everything below it forks a process and talks to physical_disc,
  and is compiled out of the harness (CHOME_HOST_TEST) - a test that needs a disc in a
  drive is a test nobody runs.

  A process, not a thread, and not inline. Every ioctl on /dev/sr0 serialises behind
  whatever the drive is doing; a full CD is 700 MB and many minutes of them. Doing this
  on the thread that draws would stop the console for the length of a rip, which is the
  same freeze chome_disc.h documents twice, only much longer. A thread stuck in an
  uninterruptible ioctl cannot be killed, which is what Cancel has to be able to do. So:
  a child owns the drive, publishes a line into /tmp, and the parent reads that line and
  nothing else - exactly the shape the ident file established.

  ------------------------------------------------------------------ the format ---

  A rip is only worth having if the target core loads it, so the layout is not a choice
  this file gets to make freely. What it emits, for a disc whose serial we know:

    <games>/<Game>/<Disc>.cue             the sheet
    <games>/<Game>/<Disc> - Track 01.bin  one file per track, raw 2352-byte sectors
    <games>/<Game>/<Disc> - Track 02.bin
    ...

  where <Game> is "Metal Gear Solid (Europe)" and <Disc> is
  "Metal Gear Solid (Europe) (SLES-01506)". For a disc with no serial - PC Engine CD and
  Neo Geo CD carry none - both collapse to the bare title and the shape is what it always
  was: <games>/<Title>/<Title>.cue beside Track 01.bin.

  ------------------------------------------------------- one folder, every disc ---

  A game is a folder and a *disc* is a sheet inside it. That is not a filing preference;
  three separate mechanisms already require it and one forbids the alternative.

    psx.cpp:824 decides whether a mount is a disc swap or a new game by comparing the
    cue's parent directory against the last one, string against string. Same folder means
    same_game: no reset, and psx_mount_save() keeps the memory card that is already
    mounted. Two discs in two folders reset the console between them and give the game a
    different memory card per disc, which is the failure this layout exists to avoid.

    chome_lib.cpp's scanner emits one item per .cue and hides the tracks beside them -
    dir_has_playlist() is a per-directory boolean and ext_is_part() matches on the
    *extension*, so N sheets in one folder are N items and no track is ever a card.

    clean_title() strips everything from the first '(' and group_key() is
    system|extension|cleaned-title, so those N items collapse into ONE shelf card that X
    cycles through. The serial and region live entirely inside parentheses precisely so
    that the card still reads "Metal Gear Solid".

  And no .m3u. Nothing in this tree reads one: psx.cpp:400 loads .cue, .chd or the
  physical-disc sentinel and nothing else, and the scanner's only interest in the
  extension is as a second trigger for the same "this folder is a rip" boolean. A
  playlist here would be a file written for no reader, so none is written.

  ------------------------------------------------------ which disc this one is ---

  It is named by its serial and NOT by a disc number, and the distinction is deliberate.

  Nothing available at rip time says "this is disc 2". The disc's own filesystem carries
  a serial and no disc index; the shipped title table maps serial -> title and is
  explicitly not a ROM database - SLES-01506 and SLES-11506 both return the bare string
  "Metal Gear Solid", because tools/disctitles.py deletes everything from the first '('
  and Redump's "(Disc 1)" goes with it. And only one disc is in the drive, so there is
  nothing to count against.

  The serial-digit convention - SLES-01506, SLES-11506, SLES-21506 - is real for many PAL
  sets and does not hold generally: Metal Gear Solid's two USA discs are SLUS-00594 and
  SLUS-00776, which no arithmetic turns into 1 and 2. A number derived that way would be
  right often enough to be trusted and wrong often enough to matter, so it is not
  derived. The serial is a unique key and it is the truth, so it is what the name says.

  One thing does fall out for free: push_game_grouped() orders a card's variants by
  strcasecmp on the path, and serials within a set sort in disc order for every set
  checked (SLES-01506 < SLES-11506, SCES-00867 < SCES-10867 < SCES-20867, and
  SLUS-00594 < SLUS-00776). So the cycle tends to run disc 1, disc 2, disc 3 without
  anything having claimed to know which is which.

  Region comes from the serial's prefix, which is the one piece of metadata that IS
  derivable: SCES/SLES/SCED/SLED are Europe, SCUS/SLUS are USA, SCPS/SLPS/SLPM/SCPM/SIPS
  are Japan. The rest of Sony's prefixes are promo and demo codes this does not claim to
  place, and a disc it cannot place simply has no region in its name - the serial still
  makes it unique. That is what stops a PAL copy landing on top of an NTSC one.

  One FILE per track rather than one big .bin. Three reasons, in the order of how much
  they cost to get wrong:

    Every CD core in this tree carries its own copy of the same cue parser, and all six
    of them - psx.cpp, megacdd.cpp (which Neo Geo CD also uses), pcecdd.cpp,
    saturncdd.cpp, 3docdd.cpp, cdi.cpp - support one FILE per track, taking each track's
    *length* from the file's size rather than from arithmetic in the sheet. That is the
    shape least able to desynchronise, because there is no sum for the sheet to get
    wrong.

    A single .bin of a full disc is 700 MB, which is one file to lose rather than one
    track, and a rip of a two-disc set on a FAT32 card is close enough to the 4 GB
    ceiling to be worth not walking towards.

    And commit 73b0f71 taught the library scanner that a folder holding a .cue and its
    "Track NN.bin" parts is ONE game named after the folder, with the tracks not listed
    as games of their own. So a rip in exactly this shape appears on the shelf as a
    single properly-named card with no further work; any other shape either litters the
    shelf with fragments or does not appear at all.

  Raw 2352 for every track, audio and data alike. The drive hands over 2352 bytes
  whatever the track is (PHYSICAL_DISC_RAW); a 2048-byte cook throws away the sync,
  header and subheader that a MODE2 sector needs, and PSX XA audio lives in exactly
  those bytes. Every core here reads 2352-byte tracks and three of them - psx.cpp and
  cdi.cpp reject anything else outright - cannot be given less. There is no case where
  writing less is better and several where it is fatal.

  The mode token is *measured*, and then translated for the reader that will see it.

  The drive's table of contents says only "data" or "audio" - CDROMREADTOCENTRY has one
  bit and no mode - so physical_disc marks every data track TT_MODE1, and a sheet that
  copied that would claim MODE1/2352 for PlayStation discs that are mostly MODE2. So
  rip_plan_build() reads one sector per data track and takes the mode from byte 15 of
  its header, which is where it actually is.

  Writing that measurement into the sheet is a second question, because the parsers do
  not all understand the same tokens. Measured against the code:

    psx.cpp:250       MODE1/2352 and MODE2/2352, both -> 2352. Anything else is a hard
                      failure, so the true mode is safe and welcome here.
    cdi.cpp:288       MODE1/2352, MODE2/2352, CDI/2352. Same: the true mode.
    saturncdd.cpp:169 MODE1/2048, MODE1/2352, MODE2/2352, per track. The true mode.
    megacdd.cpp:149   ONLY MODE1/2048 and MODE1/2352, and only for track 1. A
                      MODE2/2352 token leaves sector_size and type at 0 and the loader
                      falls through to sniffing the first sixteen bytes of the file -
                      which happens to answer 2352 for a raw rip, but by accident.
    pcecdd.cpp:157    ONLY MODE1/2048 and MODE1/2352. No MODE2 at all.

  Hence rip_cue_text()'s `mode1_only`: for Mega CD, Neo Geo CD (which is megacdd's
  parser) and PC Engine CD the sheet says MODE1/2352 for a data track whatever was
  measured. That is not a lie being told to make it fit - those consoles' discs are Mode
  1, so the measurement and the token agree - but if a disc ever disagrees the log says
  so rather than the sheet quietly claiming something the parser cannot read.

  Pregaps are written into the track that owns them, INDEX 00 at the start of the file
  and INDEX 01 at the pregap's length. Both the parsers this matters for read it that
  way and neither reads INDEX 00's *position* in the multi-FILE shape:

    psx.cpp:311       indexes[1] = INDEX 01's value, taken as the pregap inside the file;
                      the track's LBA comes from the running total of file sizes.
    megacdd.cpp:210   start = (running total) + pregap, then start += INDEX 01's value,
                      with end computed before that shift - so the track's playable span
                      begins at INDEX 01 and the pregap sectors sit in front of it in the
                      same file, which is exactly what is written.

  So INDEX 00 is always 00:00:00 in what this emits: it documents that the pregap is in
  the file, for a human and for tools that do read it, and it is ignored - harmlessly -
  by the two readers above. Where the drive cannot report a pregap at all
  (physical_disc_psx_enrich_toc() needs the raw Q subchannel, which the kernel ioctl path
  cannot return) there is no INDEX 00 line and the track starts at its INDEX 01, which is
  what a cue sheet without pregaps has always meant.

  MSF times count from the start of the *file*, not of the disc. That is the multi-FILE
  convention and the only one that works when the reader derives LBAs from file sizes.
  rip_msf() is the one place that arithmetic happens.

  Two rules the parsers share that this has to keep and that are easy to break by
  accident: the keywords are compared with memcmp and strstr against UPPERCASE, and the
  indent is skipped with `while (*lptr == 0x20)`, which is a space and not a tab. A
  lower-cased or tab-indented sheet parses as no keywords at all, and on megacdd and
  pcecdd the fallback then opens the *cue file itself* as track data.
*/

#ifndef CHOME_RIP_H
#define CHOME_RIP_H

#include <stdint.h>
#include "../../cd.h"

/*
  For PHYSICAL_DISC_RAW, which is the sector size a rip writes. Taken from the reader
  rather than restated here: a second 2352 in this tree is a second thing to get out of
  step with the drive. This is a header of declarations only and pulls in no ioctl.
*/
#include "../physical_disc/physical_disc.h"

// A rip's own track table: what to write and how much of it. Not a toc_t - that carries
// an open fileTYPE per track and a chd handle, none of which a plan wants.
struct rip_track
{
	int num;            // 1-based, as the sheet numbers them
	int type;           // TT_CDDA, TT_MODE1 or TT_MODE2 - the *measured* mode
	int start;          // first LBA to read: INDEX 00 where there is a pregap
	int sectors;        // how many, up to but not including the next track's start
	int pregap;         // sectors of pregap at the head of this track's file, or 0
};

#define RIP_TRACK_MAX 99

struct rip_plan
{
	rip_track t[RIP_TRACK_MAX];
	int n;
	int sectors;        // the sum, which is what progress is a fraction of
};

/*
  How a rip reaches the disc and the outside world.

  Injected rather than called directly, and that is the whole testability of this file:
  the harness supplies a synthetic disc, counts the notes and cancels at a chosen sector,
  with no drive and no helper process in existence.
*/
struct rip_io
{
	// 2352 bytes at that LBA. 0 on success; anything else is "not right now", which is
	// retried rather than treated as fatal - see the read-error policy below.
	int (*read)(int lba, uint8_t *dst, void *ctx);
	void *read_ctx;

	// Called as sectors land, so the caller can publish progress. May be 0.
	void (*note)(int done, int total, int bad, void *ctx);
	void *note_ctx;

	// Non-zero to stop. May be 0, which means a rip that cannot be cancelled.
	int (*cancelled)(void *ctx);
	void *cancel_ctx;
};

/* ------------------------------------------------------------- the outcome --- */

#define RIP_IDLE      0
#define RIP_RUNNING   1
#define RIP_DONE      2
#define RIP_FAILED    3
#define RIP_CANCELLED 4
#define RIP_NOSPACE   5
#define RIP_EXISTS    6

/*
  How many times a sector is asked for before it is given up on, and how long the whole
  rip will sit on one sector before calling the drive lost.

  Two numbers rather than one because they are two different events. A scratch fails
  immediately and repeatably, and four attempts is enough to know: the drive's own
  retries are already inside each of them. A USB reset - which on this dock is an
  ordinary occurrence during a long read - fails every attempt for as long as the bus is
  down, and support/physical_disc re-attaches on its own within a few seconds. So the
  loop backs off and keeps coming back for RIP_STALL_MS before it concludes the drive is
  gone, and progress does *not* advance while it does. A rip that gave up on a bus reset
  would fail eight minutes in for a reason that had already fixed itself.
*/
#define RIP_READ_TRIES 4
#define RIP_STALL_MS   90000

/*
  What is written where a sector will not read: 2352 zero bytes, counted, listed, and
  said out loud.

  The alternative - abort - throws away a nearly complete 700 MB copy because of one
  scratched frame in an audio track, where the audible result is a click. So the rip
  finishes. What it must never do is finish *quietly*: rip_run() counts every one,
  writes their LBAs into RIP_BADFILE beside the tracks, and the front-end reports the
  count instead of "Done". An imperfect rip the player knows about is a usable thing; an
  imperfect rip they do not is a bug report about the core.

  Zero-filled rather than skipped, and that is the part that matters most. A short track
  file is a track whose every later sector is at the wrong offset, so one unreadable
  sector would desynchronise the audio from that point to the end of the disc. A zeroed
  one costs a single frame and keeps every other sector where the sheet says it is.

  A bad sector in a data track is worse than one in an audio track - it can stop the game
  loading rather than click - so the list records the track and its type. The wording the
  player sees is the same either way, because "some of this disc did not read" is the
  fact and which part of it is not this file's judgement to make.
*/
#define RIP_BADFILE "unreadable-sectors.txt"

/*
  Where a rip is assembled: a sibling of the finished folder, hidden, renamed into place
  only once every byte is written.

  This is "a cancelled rip must not leave a half-written folder that looks like a game"
  discharged by construction rather than by cleanup code. The library scanner skips any
  entry whose name starts with '.' (`if (de->d_name[0] == '.') continue;` in scan_dir),
  so a rip that is cancelled, that fails, whose helper is killed because the drive
  wedged, or that dies with the machine's power, leaves behind something the shelf cannot
  see and no core will ever be offered. There is no window in which a half-written folder
  looks like a game, because a half-written rip is never at the finished name. rename(2)
  within one directory is atomic, so the instant the folder appears it is complete.
*/
#define RIP_STAGE_PREFIX "."
#define RIP_STAGE_SUFFIX ".riptmp"

/* --------------------------------------------------------------- the parts --- */

/*
  Build a plan from the drive's table of contents.

  `rd` reads one sector per data track to establish MODE1 against MODE2; pass a working
  reader or every data track comes out MODE1, which is the assumption this function
  exists to stop making. Reads nothing for audio tracks.

  Returns the number of tracks planned, or 0 when the table of contents describes nothing
  that can be written.
*/
int rip_plan_build(const toc_t *toc, rip_plan *out, int (*rd)(int, uint8_t*, void*), void *ctx);

/*
  The sheet, as text, exactly as it will be written.

  `mode1_only` for the three parsers that understand no MODE2 token - Mega CD, Neo Geo CD
  and PC Engine CD. See the format notes at the top of this file.
*/
int rip_cue_text(const rip_plan *p, int mode1_only, char *out, int outsz);

// "Track 01.bin" for track 1. The shape commit 73b0f71 taught the scanner to read as a
// part of a game rather than as a game of its own.
void rip_track_file(int num, char *out, int outsz);

/*
  The same, qualified by the disc it belongs to: "<Disc> - Track 01.bin".

  Two discs in one folder both have a track 1, so the bare name can only belong to one of
  them; this is the half of the collision that would have overwritten bytes rather than
  merely offered to. An empty or null `base` gives the bare name, which is the single-disc
  case and what every rip written before this looked like.

  Still hidden from the shelf, and by the extension rather than the wording: the scanner
  suppresses bin/iso/wav/raw beside a .cue (ext_is_part), so the "Track NN" in the middle
  is for a human reading the folder rather than for dir_has_playlist().
*/
void rip_track_file_for(const char *base, int num, char *out, int outsz);

// What the finished folder will occupy, tracks and sheet together.
long long rip_bytes_needed(const rip_plan *p);

/*
  Whether that fits, given what the card says is free.

  Separate from the statvfs so the decision can be tested without a filesystem of a
  chosen size. The margin is inside here: a card filled to its last sector by a rip
  cannot then save a state, write an index or update a config, so a rip that would leave
  less than RIP_SPARE_MB behind refuses as though it did not fit. `avail` of 0 means
  "could not tell" and is allowed to proceed - refusing every rip on a filesystem
  statvfs does not recognise would be a worse failure than the one this guards, and a
  copy that runs out of room fails safely anyway.
*/
#define RIP_SPARE_MB 64
int rip_space_ok(long long need, long long avail);

// MSF as a cue sheet writes it, from a sector count. Exposed because it is the one piece
// of arithmetic in the format a test can pin exactly.
void rip_msf(int sectors, int *m, int *s, int *f);

/*
  A folder name from a title: everything exFAT will not take replaced, the result
  trimmed, and never empty.

  Deliberately the same rule as sanitize_name() in physical_disc.cpp, which is what the
  save files are named through. Not shared with it - that one is static and is about a
  filename rather than a directory - but kept the same so that a disc's folder and its
  savestates cannot end up spelled differently.
*/
int rip_folder_name(const char *title, char *out, int outsz);

/*
  The region a Sony serial places a disc in - "Europe", "USA", "Japan" - or "" for a
  prefix this does not claim to know and for a key that is not a serial at all.

  Returns the length, so 0 reads as "no region" at a call site that does not care why.
  The unplaced prefixes (SCZS and the PAPX/PCPX/PEPX/PUPX promo codes) are left blank on
  purpose rather than guessed at: a wrong region in a folder name is a wrong folder.
*/
int rip_region_of(const char *serial, char *out, int outsz);

/*
  The folder that holds every disc of one game: "<Title> (<Region>)", or the bare title
  where the region is unknown. Sanitised exactly as rip_folder_name() sanitises.
*/
int rip_game_folder(const char *title, const char *serial, char *out, int outsz);

/*
  The base name for one disc's sheet and tracks inside that folder:
  "<Title> (<Region>) (<SERIAL>)", or the bare title where there is no serial - which is
  the single-disc shape this wrote before multi-disc sets were handled at all.
*/
int rip_disc_base(const char *title, const char *serial, char *out, int outsz);

/*
  Which folder in `games_dir` this disc actually belongs in.

  The region-qualified name when that folder is already there, else a bare-title folder
  when THAT is there, else the region-qualified name. The middle case is what keeps a
  card ripped before this existed working: disc 1 sitting in "Metal Gear Solid/" is
  adopted, so disc 2 joins it rather than starting a rival folder that would defeat
  psx.cpp's same-directory swap. Nothing already on the card is ever renamed.
*/
int rip_target_folder(const char *games_dir, const char *title, const char *serial,
	char *out, int outsz);

/*
  Whether THIS disc - this exact base name - is already in that folder.

  This is the question the replace prompt is about, and it is deliberately not
  rip_folder_exists(): a folder holding disc 1 is a game to add disc 2 to, and offering
  to replace it is the data loss this pair of functions was split to stop.
*/
int rip_disc_present(const char *games_dir, const char *folder, const char *base);

/* ----------------------------------------------------------------- the work --- */

/*
  Copy the planned tracks into `dir`, which must already exist. Writes the sheet last, so
  a directory holding a .cue is a directory whose tracks are all there.

  Returns RIP_DONE, RIP_CANCELLED or RIP_FAILED and fills *bad with the number of sectors
  that had to be zero-filled. Does not clean up after itself: rip_perform() owns the
  staging directory and therefore owns removing it.
*/
int rip_run(const rip_plan *p, const char *dir, const char *base, int mode1_only,
	const rip_io *io, int *bad);

/*
  The whole thing: stage, copy, publish, or leave nothing behind.

  `games_dir` is the target system's games folder and `name` the folder to create in it.
  Refuses RIP_NOSPACE if the card has not the room, and RIP_EXISTS if that folder is
  already there and `overwrite` is 0.

  With `overwrite` set, the folder that is there survives until the new copy is finished:
  the rip is assembled in the staging folder as always, and only once every byte of it is
  written is the old one removed and the new one renamed into its place. So a confirmed
  overwrite that is then cancelled, or that fails on a bad card, costs the player nothing -
  which is what makes the confirmation a decision about the offer rather than a gamble.
*/
int rip_perform(const rip_plan *p, const char *games_dir, const char *name, int mode1_only,
	int overwrite, const rip_io *io, int *bad);

/*
  The same, for one disc of a game that may have others: `folder` is the game and `base`
  names this disc's sheet and tracks within it. rip_perform() is this with the two equal,
  which is the single-disc shape and byte for byte what it always wrote.

  RIP_EXISTS now means "this disc is already in that folder" - same base, so same serial -
  and not "that folder is not empty". A folder holding other discs is added to.

  Publishing differs between the two cases, and both keep the rule that a folder the shelf
  can see is a folder whose sheet is real:

    the folder does not exist yet - the staged copy is renamed into place whole, exactly as
    before, so there is no instant at which a partly-built game is visible.

    the folder is already there and holds other discs - it cannot be replaced by a rename
    without taking them with it, so the staged files are moved in one at a time and the
    SHEET GOES LAST. Until that final move the new disc is a set of .bin files with no cue
    naming them, which the scanner already ignores; the moment the cue lands, the disc is
    complete. An interrupted add leaves stray tracks and no card, never a broken one.
*/
int rip_perform_disc(const rip_plan *p, const char *games_dir, const char *folder,
	const char *base, int mode1_only, int overwrite, const rip_io *io, int *bad);

// Whether <games_dir>/<name> is already there, which is what the confirmation is about.
int rip_folder_exists(const char *games_dir, const char *name);

// Free bytes on the filesystem holding that directory, or 0 when it cannot be told.
long long rip_free_bytes(const char *dir);

// Remove a directory and the files directly in it. Used on the staging folder, which is
// flat by construction; it does not recurse and will not follow a link.
void rip_rmdir_flat(const char *dir);

// Where a rip is assembled, so the front-end can say so and a test can look there.
void rip_stage_path(const char *games_dir, const char *name, char *out, int outsz);

/* --------------------------------------------------------- from the front-end --- */

/*
  What the parent process knows about a rip, which is what the child last published.

  Deliberately the same shape as the disc state file: one short line in tmpfs, written to
  a sibling and renamed, and the reader compares the *content* rather than the mtime.
  That last part is not a preference - chome_disc.cpp's poll gated on st_mtime, which is
  seconds resolution on this filesystem, and lost every update that landed inside the
  same second as the one before it. A progress line changes several times a second by
  definition, so an mtime gate here would drop almost all of them.
*/
#define RIP_PROGRESS_FILE "/tmp/classicui_rip_progress"
#define RIP_CANCEL_FILE   "/tmp/classicui_rip_cancel"

struct rip_status
{
	int state;          // RIP_*
	int done;           // sectors written
	int total;          // sectors to write, 0 before the plan is built
	int bad;            // zero-filled so far
	int tracks;
	int need_mb;        // what the rip wanted, and...
	int free_mb;        // ...what the card had. Both only for the refusal's wording.
	char name[96];      // the game's folder, so the finished message can say where it went
	char base[160];     // this disc's sheet within it; equal to name for a single-disc rip
};

/*
  Where progress has got to, in hundredths, from the sectors actually written.

  Not from elapsed time and not from the track index. A disc's tracks are wildly uneven -
  a PlayStation disc is one 600 MB data track and eight two-minute audio ones - so a
  fraction counted in tracks would sit at 11% for twenty minutes and then sprint to the
  end. Sectors are the only quantity that advances at the rate the work does.

  Clamped to 99 while the rip is running, because a bar that reads 100% for the length of
  the last track's fclose and the sheet's write is a bar that has finished and a screen
  that has not.
*/
int rip_percent(const rip_status *st);

/*
  Start one. Forks the helper, which opens the drive, reads the table of contents, builds
  the plan and copies. The caller must have stopped the detection helper first: the drive
  has one owner.

  Returns 1 when the child was started. Everything after that is reported through
  rip_poll(), the refusals included - whether the card has room is a question only the
  child can answer, because answering it means stat-ing a filesystem the drive is on.
*/
int rip_start(const char *games_dir, const char *name, const char *base, const char *title,
	int mode1_only, int overwrite);

// Called from the idle loop. Reads the one line and nothing else, so it cannot block on a
// drive however wedged it is. 1 when anything the UI draws has changed.
int rip_poll();

const rip_status *rip_state();

// 1 while a child is out there working. That is what owns the drive, what keeps the
// detection helper stopped, and what makes the dialog draw the progress face.
int rip_busy();

// 1 while a finished rip has something to say that the player has not acknowledged.
int rip_reportable();
void rip_ack();

/*
  Ask it to stop.

  Writes the cancel file, which the child looks for between sectors, and kills it without
  waiting for an answer - it may be inside an uninterruptible ioctl on a drive that has
  stopped responding, which is the whole reason this is a process rather than a thread.
  Either way the staging folder goes, removed by whichever of the two is still alive to
  do it. Cancel has to be immediate from the player's side: the one thing worse than a
  rip that takes ten minutes is a Stop button that takes ten minutes.
*/
void rip_cancel();

// Give up on the child entirely. For chome_leave().
void rip_forget();

#ifdef CHOME_HOST_TEST
/*
  The harness's way in.

  There is no child and no drive here, so the thing the front-end draws from - "a rip is
  running and it is this far through it" - is set directly. That is not a shortcut around
  the seam: it *is* the seam. In the firmware the parent's entire knowledge of a rip is the
  line the child publishes, so a harness that writes the same fields writes exactly what the
  parent would have read, and every screen, every fraction and every button downstream of it
  is the real code.

  rip_start() here records what it was asked for instead of forking, so a test can assert
  the folder a press would have created and the mode the sheet would have been written in
  without a rip happening - which is the half of this that no synthetic disc can check.
*/
void rip_test_set(int state, int done, int total, int bad);
void rip_test_reset();
int  rip_test_starts();
const char *rip_test_last_dir();
const char *rip_test_last_name();
const char *rip_test_last_base();
int  rip_test_last_mode1();
int  rip_test_last_overwrite();
#endif

#endif
