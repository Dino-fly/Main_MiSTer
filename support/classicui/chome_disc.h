/*
  Classic Home - the disc in the drive.

  Derek's machine is a SuperStation One with a SuperDock attached, and the dock's
  slot-loading drive is an ordinary USB optical drive: it appears as /dev/sr0
  (/dev/cdrom -> sr0), an HL-DT-ST DVDRAM GUD1N behind the dock's internal hub. So
  none of this needs the dock specifically - any USB drive the board can power will
  do, on a DE10-Nano too.

  ---------------------------------------------------------------------------
  This detects and identifies a disc. It does not play one.
  OFF unless classicui_disc=1, and that default is not caution for its own sake.
  ---------------------------------------------------------------------------

  Two hardware findings shaped the whole design, both of which froze the front-end
  before they were understood:

    1. Identifying a disc inline on the drawing thread stops the console. It costs up
       to ~30 sequential SCSI reads, and against a drive that will not answer them
       the firmware sits in state D in `blk_execute_rq` for minutes.

    2. Moving only the *reads* into a child is not enough. With that child busy on the
       drive, the parent's own CDROM_DRIVE_STATUS ioctl blocked as well - state D in
       `sr_block_ioctl`. **Every ioctl on /dev/sr0 serialises behind whatever the
       drive is doing.** There is no cheap status poll while a disc is being read.

  So a helper *process* owns the drive and the front-end never touches it: the parent
  reads a small file out of /tmp and nothing more. A wedged drive costs a stuck helper
  instead of a stuck console. A process rather than a thread because a thread stuck in
  an uninterruptible ioctl cannot be killed and shares the UI's address space.

  It stays opt-in until it has been proven against a range of drives and discs,
  because the failure mode when this is wrong is the worst kind: the console stops.

  Playing a disc means feeding sectors to a core in real time, which lives in each
  CD core's firmware-side daemon (megacdd.cpp, pcecdd.cpp, saturncdd.cpp, ...) - the
  code that already streams a .cue or .chd to the core over the existing protocol.
  Teaching those to source from a drive instead of a file is the work in

      https://github.com/Anime0t4ku/Main_MiSTer_Physical_Disc   (GPLv3, as are we)

  and it is deliberately not merged yet: that fork also rewrites menu.cpp and
  user_io.cpp, the two files this front-end already hooks, and it is behind
  upstream. Detection and identification are separable, testable, and enough to
  build the whole user-facing side against - so that comes first, and the merge
  happens once this is proven on real discs.

  What is derived from that fork, with thanks, is the knowledge rather than the
  code: the SCSI READ CD command shape, the signatures each console's discs carry,
  and how a PlayStation serial is dug out of the filesystem. That work would have
  taken a long time to rediscover from specifications. Everything here is a fresh
  implementation of it, structured for this tree - in particular with the sector
  parsing split from the device I/O so the harness can identify a disc that does
  not exist.

  How a disc is recognised. Every one of these is a signature in the first few
  sectors of the disc's first data track, and the order matters because some discs
  answer to more than one test:

    Mega CD      "SEGADISCSYSTEM" at offset 16 of the first raw sector
    Saturn       "SEGA SEGASATURN" in the same place
    3DO          0x01 followed by five 0x5A bytes
    PlayStation  "PLAYSTATION" in the ISO primary volume descriptor
    Neo Geo CD   "NGCD" there, or an IPL.TXT in the root
    CD-i         "CD-I" instead of "CD001", or a CDI_APPL entry
    PC Engine CD "PC Engine CD-ROM SYSTEM" in the first two raw sectors
    Mega Drive+  an ISO carrying <name>.md and <name>.cue for the same <name>
    SNES (MSU-1) an ISO carrying a .sfc or .smc
    Audio CD     a table of contents with no data track at all

  A disc that answers none of them is UNKNOWN, which is a real answer: it is what
  makes the "choose a core yourself" prompt necessary rather than a fallback.
*/

#ifndef CHOME_DISC_H
#define CHOME_DISC_H

#include <stdint.h>

// A raw sector carries its 16-byte sync/header; a user sector is the payload only.
#define DISC_RAW_SIZE   2352
#define DISC_USER_SIZE  2048

#define DISC_READ_RAW   0
#define DISC_READ_USER  1

/*
  What the front-end draws. Deliberately a state and not a pair of booleans: the
  gap between "there is a disc" and "we know what it is" is the whole point of the
  spinning icon, and a caller that has to infer it from two flags will get it wrong
  in one of the two orders.

    DISC_ABSENT    no disc, or no drive at all
    DISC_SPINNING  a disc is in there and we are still working out what it is.
                   The drive is genuinely seeking during this - it is not a
                   decorative state
    DISC_READY     identified, and disc_type() says what it is
    DISC_UNKNOWN   identified as nothing we recognise. Distinct from SPINNING: we
                   have finished looking, so the UI must stop pretending to work
                   and ask the player instead
*/
#define DISC_ABSENT    0
#define DISC_SPINNING  1
#define DISC_READY     2
#define DISC_UNKNOWN   3

// Disc types. The order is the identification order, not a preference.
#define DISC_T_NONE     0
#define DISC_T_MEGACD   1
#define DISC_T_SATURN   2
#define DISC_T_PSX      3
#define DISC_T_PCECD    4
#define DISC_T_NEOGEO   5
#define DISC_T_3DO      6
#define DISC_T_CDI      7
#define DISC_T_MDPLUS   8
#define DISC_T_SNES     9
#define DISC_T_AUDIO    10
#define DISC_T_UNKNOWN  11

#define DISC_SERIAL_LEN 16
#define DISC_LABEL_LEN  40

/* ------------------------------------------------------------- the drive --- */

/*
  Start and stop watching. Cheap when there is no drive: the open fails once and is
  not retried on every poll.

  Returns 1 when a drive was found. Note that a drive with no disc in it is still a
  success - "no drive" and "no disc" are different states and only the first is a
  reason to stop looking.
*/
int  disc_watch_start();
void disc_watch_stop();
int  disc_watching();

/*
  Called from the front-end's idle loop, and cheap by construction: it stats one small
  file in /tmp and reads it only when the mtime moved. It does **not** touch the drive -
  see the top of this file for why that is the whole point.
*/
void disc_poll();

int  disc_state();
int  disc_type();

/*
  1 once since the last call if anything the UI draws has changed - state, type,
  serial or label. Lets the shelf redraw on a disc event without polling four
  getters every frame.
*/
int  disc_take_dirty();

const char *disc_type_name(int type);        // "PlayStation", "Mega CD", ...

/*
  The system id in chome_lib's table ("psx", "md", "tg16", ...) that can load this
  disc, or 0 when we have none for it. 0 for AUDIO and UNKNOWN, and also for types
  whose core this firmware has no shelf entry for - which is why the caller must
  handle "identified but unplayable here" rather than assuming a type implies a
  core.
*/
const char *disc_system_id(int type);

// Empty when the disc carries none, which is normal for anything but PlayStation.
const char *disc_serial();

// The ISO volume label, cleaned up. Empty when there is none worth showing.
const char *disc_label();

/*
  Best name to put under the icon: the label if there is one, else the serial, else
  the console name. Never empty while a disc is present.
*/
const char *disc_display_name();

/* -------------------------------------------------------------- the parts --- */

/*
  Every system in this firmware that could load a disc, as chome_lib system ids.

  Derived from disc_system_id() rather than listed separately, so the two can never
  disagree: it is exactly the set of systems some disc type maps to. Used for the
  "pick a core yourself" prompt, which is what an unidentified disc - or an identified
  one whose core cannot read the drive, like Saturn - has to fall back on. Saturn is a
  shelf system for images on the card; what it has not got is a disc_playables entry.

  Writes up to `max` pointers into `out` and returns how many. The pointers are
  static strings.
*/
int disc_capable_systems(const char **out, int max);

/*
  The sector reader. Production reads the real drive; the harness installs its own
  so that identification can be tested against discs nobody has to own.

  Must return 0 on success and fill `dst` with DISC_RAW_SIZE bytes for
  DISC_READ_RAW or DISC_USER_SIZE for DISC_READ_USER. Anything non-zero is "could
  not read that sector", which is normal and not an error - identification probes
  sectors that do not always exist.
*/
typedef int (*disc_reader_fn)(int lba, int mode, uint8_t *dst, void *ctx);
void disc_set_reader(disc_reader_fn fn, void *ctx);
void disc_reset_reader();

/*
  The state machine, separated from the drive so it can be driven without one.

    disc_ingest_present(1) on a new arrival   -> DISC_SPINNING, identify becomes due
    disc_ingest_present(0)                    -> DISC_ABSENT, everything forgotten
    disc_ingest_identify(lba0) when due       -> DISC_READY or DISC_UNKNOWN

  disc_ingest_identify() must not be called on the thread that draws, and the reason
  is measured rather than theoretical. Identification issues up to ~30 sequential
  SCSI reads; on a real drive that would not answer them, the firmware sat in state
  D in `blk_execute_rq` inside a single SG_IO ioctl and the whole front-end stopped
  for minutes. Deferring it by one poll - which is what the first version of this did
  - fixes nothing, because the cost is in the reads themselves and not in when they
  start.

  So in the firmware it runs in a forked child, the same shape chome_art.cpp uses for
  downloads: the child identifies and writes a one-line result, the parent reaps it
  with waitpid(WNOHANG). The harness calls these directly, which is the only place
  calling them inline is safe.
*/
void disc_ingest_present(int present);
int  disc_identify_due();
void disc_ingest_identify(int data_lba0);

/*
  Identify the disc whose first data track starts at `data_lba0`, or -1 when the
  table of contents found no data track at all (which is an audio CD). Pure: reads
  only through the installed reader.
*/
int disc_identify_at(int data_lba0);

/*
  A PlayStation serial such as "SLUS-00626", dug out of the filesystem by looking
  for a known publisher prefix followed by a version terminator. Returns the length
  written, or 0.

  Normalised the way Redump and ScreenScraper write them: the underscore becomes a
  dash and the dot goes, so SLUS_006.26 reads SLUS-00626. That matters because the
  serial is the only reliable handle on *which game* a physical disc is - there is
  no filename to match on - so it has to come out in the form the outside world
  uses.
*/
int disc_serial_at(int data_lba0, char *out, int outsz);

// The volume label out of an ISO primary volume descriptor.
int disc_label_at(int data_lba0, char *out, int outsz);

#endif
