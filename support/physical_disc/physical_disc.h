/*
  Streaming sector reader for a physical CD in the drive.

  ---------------------------------------------------------------------------
  Origin. This file and physical_disc.cpp are taken from

      https://github.com/Anime0t4ku/Main_MiSTer_Physical_Disc

  a fork of Main_MiSTer by Anime0t4ku, GPLv3 as this tree is. The ring buffer,
  the prefetch worker, the speed cap, the drive-loss recovery and the udev rule
  are that author's work and are kept here as they were written, because every
  one of them is subtle and a smaller paraphrase of them cost two frozen
  consoles. Adaptations for this tree are marked "adaptation" in the .cpp and
  are listed at the top of it.
  ---------------------------------------------------------------------------

  What this is for. Detection and identification of a disc - what console it is
  for, its label, its serial - is ClassicUI's job and lives in
  support/classicui/chome_disc.h, which runs in a helper *process* because every
  ioctl on /dev/sr0 serialises behind whatever the drive is doing. This file is
  the other half: once a disc is to be *played*, a CD core's firmware-side daemon
  needs sectors in real time, so the drive is opened in this process, a worker
  thread reads ahead of the core, and the daemon's per-sector calls are served
  from a ring buffer.

  The two must not run at once. The front-end stops its detection helper before
  handing a disc to a core; see the disc launch in chome_ui.cpp.

  The detection half of this API - physical_disc_watch_start(),
  physical_disc_identify(), physical_disc_menu_status() and friends - is upstream
  code that this tree does not call, because chome_disc.cpp already does that job
  off the drawing thread. It is kept so the file stays whole and so a future merge
  has nothing to re-derive. Do not call it from the thread that draws.
*/

#ifndef MISTER_PHYSICAL_DISC_INCLUDED
#define MISTER_PHYSICAL_DISC_INCLUDED

#include <stdint.h>
#include "../../cd.h"

/*
  The filename a core is "mounted" with to mean "the disc in the drive". It is not
  a path and must never be resolved against a games folder - see the sentinel case
  in menu.cpp's MENU_GENERIC_IMAGE_SELECTED.
*/
#define PHYSICAL_DISC_SENTINEL "*PHYSICAL_DISC*"
#define PHYSICAL_DISC_RAW  2352
#define PHYSICAL_DISC_SUB  96

/*
  Who the disc in the drive *is*, published by the mount for the front-end to read.

  The sentinel above says "the disc" and nothing more, so everything a front-end keys
  on a game - savestate paths, per-game core options, suspend and resume - had no disc
  to key on and either collided across every disc or pointed at a filename containing
  '*', which cannot exist on exFAT.

  physical_disc_save_name() already answers "who is this disc" for the save files, and
  it is the only thing that does: the serial for a PlayStation disc, the header product
  id for Saturn and Mega CD, else the volume label, else a hash of the table of
  contents. Nobody else can recompute it - the front-end has no drive of its own, and
  the detection helper reads the disc with its own reader and never builds this name.
  Reimplementing the choice elsewhere would drift from the save files the moment a disc
  had no serial, so the mount writes its answer here instead, once, and the front-end
  reads it rather than guessing.

  Two lines: the name (a legal filename), then a human title for a caption, which may
  be empty. Lives in tmpfs and is removed when the game is left.
*/
#define PHYSICAL_DISC_IDENT_FILE "/tmp/classicui_disc_ident"

// How long a core holds its disc-swap door open after a swap is seen.
#define PHYSICAL_DISC_SWAP_DWELL_MS 500

/*
  Installs a udev rule that stops the persistent-storage rules probing sr[0-9] on
  insert, so udev is not reading the disc while a core is. Writes /etc and runs
  udevadm; this tree does not call it yet.
*/
void physical_disc_prepare_environment(void);

void physical_disc_set_device(const char *dev);

/*
  Open the drive (NULL: scan /dev/sr0../dev/sr7) and start the prefetch worker.
  0 on success.
*/
int physical_disc_open(const char *dev);
void physical_disc_native_speed(int enable);

int physical_disc_disc_present();
int physical_disc_media_changed();
int physical_disc_drive_busy();

// Disc swapping, for the cores that support it. Unused for PC Engine CD.
void physical_disc_swap_enable(int enable);
int physical_disc_swap_consume(void);
int physical_disc_swap_ejected(void);
int physical_disc_swap_happened(void);

/*
  Read the table of contents off the disc into a toc_t the CD daemons already
  understand, and set toc->phys so every read in the daemon can branch on it.
  Also arms the prefetch worker on the first track. 0 on success.
*/
int physical_disc_load_toc(toc_t *toc);

// The TOC last loaded, without touching the drive again.
int physical_disc_current_toc(toc_t *toc);
int physical_disc_toc_audio_only(const toc_t *toc);
int physical_disc_psx_enrich_toc(toc_t *toc);

/*
  A raw 2352-byte sector, and optionally its 96 bytes of subchannel. Served from
  the ring when the worker got there first, read synchronously when it did not -
  which is why the caller must be a thread that can afford to wait for a drive.
*/
int physical_disc_read_sector(int lba, uint8_t *dst, uint8_t *sub96);

// As above but does not move the prefetch window; for probing a sector.
int physical_disc_probe_sector(int lba, uint8_t *dst);

// 1 when the subchannel returned is real, 0 when it is zeros.
int physical_disc_read_sector_sub(int lba, uint8_t *dst, uint8_t *sub96);

// The 2048-byte user area, mode 1 or mode 2 form 1.
int physical_disc_read_data2048(int lba, uint8_t *dst);

/*
  Get the drive spinning and the ring populated before the core asks for anything.
  Blocks for up to ~8 seconds. Called once, from the daemon's load path.
*/
void physical_disc_prewarm_blocking(void);

// Tell the prefetch worker where the core is about to read.
void physical_disc_seek_hint(int lba);

typedef enum {
	PHYSICAL_DISC_DISC_NONE = 0,
	PHYSICAL_DISC_DISC_MEGACD,
	PHYSICAL_DISC_DISC_SATURN,
	PHYSICAL_DISC_DISC_PSX,
	PHYSICAL_DISC_DISC_PCECD,
	PHYSICAL_DISC_DISC_NEOGEO,
	PHYSICAL_DISC_DISC_3DO,
	PHYSICAL_DISC_DISC_CDI,
	PHYSICAL_DISC_DISC_MDPLUS,
	PHYSICAL_DISC_DISC_SNES,
	PHYSICAL_DISC_DISC_AUDIO,
	PHYSICAL_DISC_DISC_UNKNOWN,
} physical_disc_disc_t;

physical_disc_disc_t physical_disc_identify();
const char *physical_disc_disc_name(physical_disc_disc_t t);

typedef enum {
	PHYSICAL_DISC_REGION_UNKNOWN = 0,
	PHYSICAL_DISC_REGION_JP,
	PHYSICAL_DISC_REGION_US,
	PHYSICAL_DISC_REGION_EU,
} physical_disc_region_t;

physical_disc_region_t physical_disc_region();
physical_disc_region_t physical_disc_region_from_md_header(const uint8_t *hdr, int len);
const char *physical_disc_region_name(physical_disc_region_t r);

typedef enum {
	PHYSICAL_DISC_EV_NONE = 0,
	PHYSICAL_DISC_EV_DISC_IN,
	PHYSICAL_DISC_EV_DISC_OUT,
} physical_disc_event_t;

int physical_disc_watch_start(void);
void physical_disc_watch_stop(void);
int physical_disc_watching(void);

physical_disc_event_t physical_disc_poll_event(physical_disc_disc_t *type, physical_disc_region_t *region, int *initial);

void physical_disc_forget_disc(void);

int physical_disc_disc_label(char *out, int outsz);
int physical_disc_disc_serial(char *out, int outsz);

/*
  A stable filename for this disc's save file: the PlayStation serial, the Saturn
  or Mega CD header id, the volume label, or failing all of those a uuid derived
  from the table of contents. Never a path, and never the sentinel.
*/
int physical_disc_save_name(physical_disc_disc_t type, char *out, int outsz);

const char *physical_disc_console_name(physical_disc_disc_t t);

int physical_disc_menu_status(char *name, int namesz, physical_disc_disc_t *type);
int physical_disc_menu_dirty(void);

// Stops the worker, frees the ring and closes the drive.
void physical_disc_close();

#endif
