
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include "../../file_io.h"
#include "../../user_io.h"
#include "../../spi.h"
#include "../../hardware.h"
#include "../../menu.h"
#include "../../cheats.h"
#include "../megacd/megacd.h"
#include "../physical_disc/physical_disc.h"
#include "neogeocd.h"
#include "neogeo_loader.h"

static int need_reset=0;
static uint8_t has_command = 0;
static uint8_t neo_cd_en = 0;
static uint32_t poll_timer = 0;
static uint8_t cd_speed = 0;

#define CRC_START 5

#define NEOCD_GET_CMD        0
#define NEOCD_GET_SEND_DATA  1

void neocd_poll()
{
	static uint8_t last_req = 255;
	static uint32_t swap_close_at = 0;

	/*
	  Disc swapping, physical drive only. The reader's worker thread watches the tray
	  while a disc is mounted; once it has seen that disc leave and a new table of
	  contents read back, adopt the new disc and act out the tray for the core - OPEN
	  for a moment, then STOP (NO_DISC if the new disc turned out unreadable) - so the
	  BIOS sees a swap rather than the old disc's geometry continuing underneath it.

	  The dwell is what makes the swap visible at all. Status only reaches the core on
	  a poll and the CDD state machine on the FPGA side samples it on its own
	  schedule, so flipping to OPEN and back inside one poll interval is a transition
	  the fabric can miss entirely - and then the BIOS never learns the disc changed.
	  latency = 0 is part of the same trick: cdd_t::Update() returns early while a
	  latency is counting down in the STOP/TRAY/OPEN branch, so a leftover latency
	  would eat the dwell instead of holding OPEN through it.

	  This is mcd_poll()'s block rather than a paraphrase of it, deliberately: Neo Geo
	  CD has no daemon of its own, the `cdd` here *is* Mega CD's cdd_t, so the two
	  cores must drive it the same way or one of them is wrong. In particular isData
	  = 1 next to CD_STAT_OPEN is that daemon's own convention for a tray event - see
	  CD_COMM_TRAY_OPEN in megacdd.cpp, which sets exactly this pair. Deriving isData
	  from the new disc the way neocd_set_image() does for a cold mount was tried and
	  dropped: a cold mount is a machine powering up with a disc already in it, a swap
	  is a tray cycle, and Update() puts isData back per track as soon as the BIOS
	  plays anything anyway.
	*/
	if (cdd.is_phys() && physical_disc_swap_consume() && cdd.SwapPhys())
	{
		cdd.isData = 1;
		cdd.status = CD_STAT_OPEN;
		cdd.latency = 0;
		swap_close_at = GetTimer(PHYSICAL_DISC_SWAP_DWELL_MS);
	}
	if (cdd.is_phys() && swap_close_at && CheckTimer(swap_close_at))
	{
		swap_close_at = 0;
		cdd.status = cdd.loaded ? CD_STAT_STOP : CD_STAT_NO_DISC;
		cdd.latency = 10;
	}

	if (!poll_timer || CheckTimer(poll_timer))
	{

		set_poll_timer();

		if (has_command) {
			spi_uio_cmd_cont(UIO_CD_SET);
			uint64_t s = cdd.GetStatus(CRC_START);
			spi_w((s >> 0) & 0xFFFF);
			spi_w((s >> 16) & 0xFFFF);
			spi_w((s >> 32) & 0x00FF);
			DisableIO();

			has_command = 0;

			//printf("\x1b[32mNEOCD: Send status, status = %04X%04X%04X \n\x1b[0m", (uint16_t)((s >> 32) & 0x00FF), (uint16_t)((s >> 16) & 0xFFFF), (uint16_t)((s >> 0) & 0xFFFF));
		}

		cdd.Update();
	}


	uint8_t req = spi_uio_cmd_cont(UIO_CD_GET);

	if (req != last_req)
	{
		last_req = req;

		spi_w(NEOCD_GET_CMD);

		uint16_t data_in[4];
		data_in[0] = spi_w(0);
		data_in[1] = spi_w(0);
		data_in[2] = spi_w(0);
		DisableIO();

		if (need_reset || data_in[0] == 0xFF) {
			printf("NEOCD: request to reset\n");
			need_reset = 0;
			cdd.Reset();
		}

		cd_speed = (data_in[2] >> 8) & 3;

		uint64_t c = *((uint64_t*)(data_in));
		cdd.SetCommand(c, CRC_START);
		cdd.CommandExec();
		has_command = 1;

		//printf("\x1b[32mNEOCD: Get command, command = %04X%04X%04X, has_command = %u\n\x1b[0m", data_in[2], data_in[1], data_in[0], has_command);
	}
	else
		DisableIO();
}

void set_poll_timer()
{
	int speed = cd_speed;
	int interval = 10; // Slightly faster so the buffers stay filled when playing

	if (!cdd.isData || cdd.status != CD_STAT_PLAY || cdd.latency != 0)
	{
		speed = 0;
	}

	if (speed == 1)
	{
		interval = 5;
	}
	else if (speed == 2)
	{
		interval = 4;
	}
	else if (speed == 3)
	{
		interval = 2;
	}

	poll_timer = GetTimer(interval);
}

int neocd_set_image(const char *filename)
{
	int bios_ok = 0;
	int phys = !strcmp(filename, PHYSICAL_DISC_SENTINEL);

	cdd.Unload();
	physical_disc_swap_enable(0);
	cdd.status = CD_STAT_OPEN;

	/*
	  Open the drive here, not down in cdd.Load() where the sentinel is normally
	  recognised, because neogeo_romset_tx() runs first and mounts the CD backup RAM.
	  For a disc that save's name has to come off the disc itself - see
	  physical_disc_save_name() - and every route to a name there reads the table of
	  contents, which needs an open drive. With the drive still closed the lookup
	  failed silently, and it fails by emptying the caller's buffer rather than by
	  leaving the "physical_disc" fallback standing, so the save path came out as
	  saves/NEOGEO/.sav: one hidden file that every disc shared and every disc
	  overwrote. A core launch re-execs the firmware, so there was never an earlier
	  open to inherit - this path was reached with the drive shut every single time.
	  physical_disc_open() is idempotent, so cdd.Load() still owns the TOC and still
	  re-arms the read-ahead worker on it; all this call changes is that the drive is
	  already spinning by then, which is what mcd_set_image() gets out of opening
	  early for the region.
	*/
	if (phys) physical_disc_open(NULL);

	if (*filename)
	{
		/*
		  The loader takes a mutable name (the cart path strips an extension in
		  place); this path only ever hands it a CD name, which it does not touch,
		  but it still gets a copy rather than a cast. Whether it is the sentinel or
		  a cue path changes nothing here: the CD BIOS comes from the NeoGeo-CD home
		  dir either way, and that is the BIOS a physical disc boots through too.
		*/
		char nm[1024];
		snprintf(nm, sizeof(nm), "%s", filename);
		bios_ok = neogeo_romset_tx(nm, 1);

		if (cdd.Load(filename) > 0)
		{
			/*
			  An audio-only disc has no track the BIOS could boot; telling the core
			  it holds data would have it try. Only a physical disc can be in that
			  state here - a cue/chd was chosen by name, a disc is whatever was in
			  the drive.
			*/
			toc_t disc_toc = {};
			int audio_only = phys && !physical_disc_current_toc(&disc_toc) && physical_disc_toc_audio_only(&disc_toc);
			cdd.isData = audio_only ? 0 : 1;
			cdd.status = cdd.loaded ? CD_STAT_STOP : CD_STAT_NO_DISC;
			cdd.latency = 10;
			cdd.SendData = neocd_send_data;
			cdd.CanSendData = neocd_can_send_data;

			/*
			  Only now: arming the watch before the TOC is loaded would have the
			  worker thread poll a tray it has no geometry for, and the watch itself
			  waits for a leadout before it will believe anything. Armed last, so an
			  image mount or a failed disc leaves it disarmed.
			*/
			if (phys) physical_disc_swap_enable(1);
		}
		else
		{
			cdd.status = CD_STAT_NO_DISC;
		}
	}

	neocd_reset();

	// Loaded *and* booted through a BIOS that exists - a CD without its system ROM
	// is a black screen, and the caller deserves to know which of the two failed.
	return bios_ok && cdd.loaded;
}

void neocd_reset() {
	need_reset = 1;
}

int neocd_send_data(uint8_t* buf, int len, uint8_t index) {
	// set index byte
	user_io_set_index(index);

	user_io_set_download(1);
	user_io_file_tx_data(buf, len);
	user_io_set_download(0);
	return 1;
}

int neocd_is_en() {
	return (neo_cd_en == 1);
}

void neocd_set_en(int enable) {
	neo_cd_en = enable;
}

int neocd_can_send_data(uint8_t type) {
	// Ask the FPGA if it is ready to receive a sector
	spi_uio_cmd_cont(UIO_CD_GET);
	spi_w(NEOCD_GET_SEND_DATA | (type << 2));

	uint16_t data = spi_w(0);
	DisableIO();

	return (data == 1);
}
