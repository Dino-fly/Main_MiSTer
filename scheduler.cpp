#include "scheduler.h"
#include <unistd.h>
#include <stdio.h>
#include "libco.h"
#include "menu.h"
#include "support/classicui/chome.h"
#include "user_io.h"
#include "input.h"
#include "frame_timer.h"
#include "fpga_io.h"
#include "osd.h"
#include "profiling.h"
#include "video.h"

static cothread_t co_scheduler = nullptr;
static cothread_t co_poll = nullptr;
static cothread_t co_ui = nullptr;
static cothread_t co_last = nullptr;

static void scheduler_wait_fpga_ready(void)
{
	while (!is_fpga_ready(1))
	{
		fpga_wait_to_reset();
	}
}

static void scheduler_co_poll(void)
{
	for (;;)
	{
		scheduler_wait_fpga_ready();

		{
			SPIKE_SCOPE("co_poll", 1000);
			user_io_poll();
			frame_timer();
			input_poll(0);
			video_poll();
		}

		scheduler_yield();
	}
}

static void scheduler_co_ui(void)
{
	for (;;)
	{
		{
			SPIKE_SCOPE("co_ui", 1000);
			HandleUI();
			OsdUpdate();
		}

		scheduler_yield();
	}
}

static void scheduler_schedule(void)
{
	if (co_last == co_poll)
	{
		co_last = co_ui;
		co_switch(co_ui);
	}
	else
	{
		co_last = co_poll;
		co_switch(co_poll);
	}
}

void scheduler_init(void)
{
	const unsigned int co_stack_size = 262144 * sizeof(void*);

	co_poll = co_create(co_stack_size, scheduler_co_poll);
	co_ui = co_create(co_stack_size, scheduler_co_ui);
}

void scheduler_run(void)
{
	co_scheduler = co_active();

	for (;;)
	{
		scheduler_schedule();

		/*
		  Give the core back when nothing needs us to be quick.

		  This scheduler never sleeps, by design: the HPS talks to the FPGA over SPI with
		  nothing to wake on, so a core asking for a ROM chunk or a disk sector is served
		  only when co_poll next runs. That is why the firmware sits at 100% of one core -
		  measured on hardware: cpu1 100%, cpu0 0.8%, load average exactly 1.00.

		  But while the front-end owns the screen the game is muted and not composited, so
		  there is nothing to be late for. A millisecond here costs nothing and takes the
		  core back from a spin that only makes heat.

		  Here rather than inside either coroutine because this is the one point where both
		  have yielded and neither is mid-work. Note the sleep must NOT go in main.cpp's
		  while(1): that loop is behind #else on USE_SCHEDULER and is dead code, which is
		  where the first attempt at this went and why it appeared to do nothing.

		  menu_mgl_busy() excludes the launch window, when a ROM is being pushed across and
		  every chunk is driven from user_io_poll().
		*/
		if (chome_core_idle() && !menu_mgl_busy()) usleep(1000);
	}

	co_delete(co_ui);
	co_delete(co_poll);
	co_delete(co_scheduler);
}

void scheduler_yield(void)
{
	co_switch(co_scheduler);
}
