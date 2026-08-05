/*
Copyright 2005, 2006, 2007 Dennis van Weeren
Copyright 2008, 2009 Jakub Bednarski
Copyright 2012 Till Harbaum

This file is part of Minimig

Minimig is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 3 of the License, or
(at your option) any later version.

Minimig is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <sched.h>
#include <inttypes.h>
#include <ctype.h>
#include <string.h>
#include "menu.h"
#include "support/classicui/chome.h"
#include "user_io.h"
#include "input.h"
#include "frame_timer.h"
#include "fpga_io.h"
#include "scheduler.h"
#include "osd.h"
#include "offload.h"

const char *version = "$VER:" VDATE;

int main(int argc, char *argv[])
{
	// Always pin main worker process to core #1 as core #0 is the
	// hardware interrupt handler in Linux.  This reduces idle latency
	// in the main loop by about 6-7x.
	cpu_set_t set;
	CPU_ZERO(&set);
	CPU_SET(1, &set);
	sched_setaffinity(0, sizeof(set), &set);

	offload_start();

	fpga_io_init();

	DISKLED_OFF;

	printf("\nMinimig by Dennis van Weeren");
	printf("\nARM Controller by Jakub Bednarski");
	printf("\nMiSTer code by Sorgelig\n\n");

	printf("Version %s\n\n", version + 5);

	if (argc > 1) printf("Core path: %s\n", argv[1]);
	if (argc > 2) printf("XML path: %s\n", argv[2]);

	if (!is_fpga_ready(1))
	{
		printf("\nGPI[31]==1. FPGA is uninitialized or incompatible core loaded.\n");
		printf("Quitting. Bye bye...\n");
		exit(0);
	}

	FindStorage();
	user_io_init((argc > 1) ? argv[1] : "",(argc > 2) ? argv[2] : NULL);

#ifdef USE_SCHEDULER
	scheduler_init();
	scheduler_run();
#else
	while (1)
	{
		if (!is_fpga_ready(1))
		{
			fpga_wait_to_reset();
		}

		user_io_poll();
		frame_timer();
		input_poll(0);
		HandleUI();
		OsdUpdate();

		/*
		  Give the core back when nothing needs us to be quick.

		  This loop has no sleep in it, by design: the HPS talks to the FPGA over SPI with
		  nothing to wake on, so a core asking for a ROM chunk or a disk sector is served
		  only when we come round to ask. That is why the firmware sits at 100% of one core
		  - measured on hardware: cpu1 100%, cpu0 0.8%, load average exactly 1.00.

		  But when the front-end's menu is up over a game that is genuinely paused or frozen,
		  and when its shelf is up in the menu core, nothing is fetching anything. There is
		  no request to be late for, so a millisecond of sleep costs nothing and takes the
		  core back from a spin that is only generating heat.

		  Both guards matter. chome_core_idle() excludes an in-game menu over a core that is
		  *still running* - cores with neither pause nor save states, and classicui_freeze=0 -
		  because those are still fetching data and still making sound. menu_mgl_busy()
		  excludes the launch window, when a ROM is being pushed across and every chunk is
		  driven from user_io_poll() above.

		  A millisecond, not more: input still has to feel immediate, and the disc animation
		  wants tens of passes a second. At roughly 20us of work per pass this is still a
		  ~98% reduction while remaining imperceptible.
		*/
		if (chome_core_idle() && !menu_mgl_busy()) usleep(1000);
	}
#endif
	return 0;
}
