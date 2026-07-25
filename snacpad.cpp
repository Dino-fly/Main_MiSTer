// Framework PSX SNAC pad support.
//
// The sys framework of a core can include a PSX pad reader on the user port
// (psx_snac_pad.sv, served by sys_top over UIO_SNAC_PAD). This module polls
// that reader and mirrors each connected pad as a uinput gamepad with
// standard event codes, so the pads go through the regular input pipeline:
// they work in the OSD menu, get per-core mapping, and work with every core
// regardless of the core's own controller support.
//
// MiSTer.ini: snac_pad=1 enables it (2 = same but without the
// Select+Start -> OSD button synthesis). Can be set per-core.

#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/uinput.h>

#include "hardware.h"
#include "spi.h"
#include "user_io.h"
#include "cfg.h"
#include "snacpad.h"

#define SNAC_MAGIC     0x4A
#define SNAC_POLL_MS   2
#define SNAC_RETRY_MS  1000

#define SNAC_VID       0x4D53  // 'MS'
#define SNAC_PID       0x0F01  // +port

// PSX button bit order delivered by the reader (active high):
// 0:Select 1:L3 2:R3 3:Start 4:Up 5:Right 6:Down 7:Left
// 8:L2 9:R2 10:L1 11:R1 12:Triangle 13:Circle 14:Cross 15:Square
static const uint16_t btn_codes[16] =
{
	BTN_SELECT, BTN_THUMBL, BTN_THUMBR, BTN_START,
	0, 0, 0, 0, // d-pad is reported as HAT0X/HAT0Y
	BTN_TL2, BTN_TR2, BTN_TL, BTN_TR,
	BTN_NORTH, BTN_EAST, BTN_SOUTH, BTN_WEST
};

#define BTNS_DPAD      0x00F0
#define BTN_BIT_SELECT 0x0001
#define BTN_BIT_START  0x0008

struct snac_pad_t
{
	int      fd;
	int      connected;
	uint8_t  id;
	uint16_t buttons;
	uint8_t  axes[4]; // lx, ly, rx, ry
};

static snac_pad_t pads[2] =
{
	{ -1, 0, 0xFF, 0, { 0x80, 0x80, 0x80, 0x80 } },
	{ -1, 0, 0xFF, 0, { 0x80, 0x80, 0x80, 0x80 } },
};
static int supported = -1; // -1: not probed yet, 0: no (old sys), 1: yes
static int enabled = 0;    // last enable bit sent to the core
static uint32_t poll_timer = 0;

static void pad_reset_state(snac_pad_t *pad)
{
	pad->connected = 0;
	pad->id = 0xFF;
	pad->buttons = 0;
	memset(pad->axes, 0x80, sizeof(pad->axes));
}

static void emit(int fd, uint16_t type, uint16_t code, int32_t value)
{
	struct input_event ev = {};
	gettimeofday(&ev.time, NULL);
	ev.type = type;
	ev.code = code;
	ev.value = value;
	write(fd, &ev, sizeof(ev));
}

static int pad_create(int idx)
{
	int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0)
	{
		printf("snacpad: unable to open /dev/uinput\n");
		return -1;
	}

	struct uinput_user_dev uinp = {};
	snprintf(uinp.name, UINPUT_MAX_NAME_SIZE, "MiSTer SNAC Pad %d", idx + 1);
	uinp.id.bustype = BUS_USB;
	uinp.id.vendor = SNAC_VID;
	uinp.id.product = SNAC_PID + idx;
	uinp.id.version = 1;

	ioctl(fd, UI_SET_EVBIT, EV_KEY);
	for (int i = 0; i < 16; i++) if (btn_codes[i]) ioctl(fd, UI_SET_KEYBIT, btn_codes[i]);
	ioctl(fd, UI_SET_KEYBIT, BTN_MODE);

	ioctl(fd, UI_SET_EVBIT, EV_ABS);
	static const uint16_t sticks[4] = { ABS_X, ABS_Y, ABS_RX, ABS_RY };
	for (int i = 0; i < 4; i++)
	{
		ioctl(fd, UI_SET_ABSBIT, sticks[i]);
		uinp.absmin[sticks[i]] = 0;
		uinp.absmax[sticks[i]] = 255;
		uinp.absflat[sticks[i]] = 8;
	}
	ioctl(fd, UI_SET_ABSBIT, ABS_HAT0X);
	ioctl(fd, UI_SET_ABSBIT, ABS_HAT0Y);
	uinp.absmin[ABS_HAT0X] = -1;
	uinp.absmax[ABS_HAT0X] = 1;
	uinp.absmin[ABS_HAT0Y] = -1;
	uinp.absmax[ABS_HAT0Y] = 1;

	if (write(fd, &uinp, sizeof(uinp)) != sizeof(uinp) || ioctl(fd, UI_DEV_CREATE))
	{
		printf("snacpad: unable to create uinput device for pad %d\n", idx + 1);
		close(fd);
		return -1;
	}

	// initialize axes to center
	for (int i = 0; i < 4; i++) emit(fd, EV_ABS, sticks[i], 0x80);
	emit(fd, EV_SYN, SYN_REPORT, 0);

	return fd;
}

static void pad_destroy(int idx)
{
	if (pads[idx].fd >= 0)
	{
		ioctl(pads[idx].fd, UI_DEV_DESTROY);
		close(pads[idx].fd);
		pads[idx].fd = -1;
	}
	pad_reset_state(&pads[idx]);
}

static int hat_val(uint16_t buttons, int neg_bit, int pos_bit)
{
	if (buttons & (1 << neg_bit)) return -1;
	if (buttons & (1 << pos_bit)) return 1;
	return 0;
}

static void pad_update(int idx, int connected, uint8_t id, uint16_t buttons, const uint8_t *axes)
{
	snac_pad_t *pad = &pads[idx];

	if (!connected)
	{
		if (pad->fd >= 0)
		{
			printf("snacpad: pad %d disconnected\n", idx + 1);
			pad_destroy(idx);
		}
		return;
	}

	if (pad->fd < 0)
	{
		pad->fd = pad_create(idx);
		if (pad->fd < 0) return;
		pad_reset_state(pad);
		printf("snacpad: pad %d connected (id %02X)\n", idx + 1, id);
	}
	else if (id != pad->id)
	{
		printf("snacpad: pad %d mode change (id %02X)\n", idx + 1, id);
	}
	pad->id = id;
	pad->connected = 1;

	uint16_t changed = buttons ^ pad->buttons;
	int sent = 0;

	for (int i = 0; i < 16; i++)
	{
		if (btn_codes[i] && (changed & (1 << i)))
		{
			emit(pad->fd, EV_KEY, btn_codes[i], (buttons >> i) & 1);
			sent = 1;
		}
	}

	if (changed & BTNS_DPAD)
	{
		int hx = hat_val(buttons, 7, 5);      // left, right
		int hy = hat_val(buttons, 4, 6);      // up, down
		int ohx = hat_val(pad->buttons, 7, 5);
		int ohy = hat_val(pad->buttons, 4, 6);
		if (hx != ohx) { emit(pad->fd, EV_ABS, ABS_HAT0X, hx); sent = 1; }
		if (hy != ohy) { emit(pad->fd, EV_ABS, ABS_HAT0Y, hy); sent = 1; }
	}

	if (cfg.snac_pad < 2)
	{
		// pads have no dedicated menu button: Select+Start -> BTN_MODE
		int combo = ((buttons & (BTN_BIT_SELECT | BTN_BIT_START)) == (BTN_BIT_SELECT | BTN_BIT_START));
		int ocombo = ((pad->buttons & (BTN_BIT_SELECT | BTN_BIT_START)) == (BTN_BIT_SELECT | BTN_BIT_START));
		if (combo != ocombo)
		{
			emit(pad->fd, EV_KEY, BTN_MODE, combo);
			sent = 1;
		}
	}

	static const uint16_t sticks[4] = { ABS_X, ABS_Y, ABS_RX, ABS_RY };
	for (int i = 0; i < 4; i++)
	{
		if (axes[i] != pad->axes[i])
		{
			emit(pad->fd, EV_ABS, sticks[i], axes[i]);
			sent = 1;
		}
	}

	if (sent) emit(pad->fd, EV_SYN, SYN_REPORT, 0);

	pad->buttons = buttons;
	memcpy(pad->axes, axes, sizeof(pad->axes));
}

void snacpad_init()
{
	// core (re)loaded: the fabric side is back to disabled, probe again
	supported = -1;
	enabled = 0;
	poll_timer = 0;
}

void snacpad_poll()
{
	int want = (cfg.snac_pad != 0);
	if (!want && !enabled && supported < 0 && pads[0].fd < 0 && pads[1].fd < 0) return;

	if (poll_timer && !CheckTimer(poll_timer)) return;
	poll_timer = GetTimer(SNAC_POLL_MS);

	uint16_t status = spi_uio_cmd_cont(UIO_SNAC_PAD);
	if ((status >> 8) != SNAC_MAGIC)
	{
		DisableIO();
		if (supported != 0)
		{
			if (want) printf("snacpad: no SNAC pad reader in this core (sys update needed)\n");
			supported = 0;
			pad_destroy(0);
			pad_destroy(1);
		}
		poll_timer = GetTimer(SNAC_RETRY_MS);
		return;
	}

	uint16_t w0 = spi_w(want);  // control word; response is pad1 presence/id
	uint16_t b1 = spi_w(0);
	uint16_t l1 = spi_w(0);
	uint16_t r1 = spi_w(0);
	uint16_t w4 = spi_w(0);
	uint16_t b2 = spi_w(0);
	uint16_t l2 = spi_w(0);
	uint16_t r2 = spi_w(0);
	DisableIO();

	if (supported != 1)
	{
		printf("snacpad: SNAC pad reader present, %s\n", want ? "enabled" : "disabled");
		supported = 1;
	}
	enabled = want;

	if (!want)
	{
		pad_destroy(0);
		pad_destroy(1);
		poll_timer = GetTimer(SNAC_RETRY_MS);
		return;
	}

	uint8_t ax1[4] = { (uint8_t)l1, (uint8_t)(l1 >> 8), (uint8_t)r1, (uint8_t)(r1 >> 8) };
	uint8_t ax2[4] = { (uint8_t)l2, (uint8_t)(l2 >> 8), (uint8_t)r2, (uint8_t)(r2 >> 8) };
	pad_update(0, w0 >> 15, w0 & 0xFF, b1, ax1);
	pad_update(1, w4 >> 15, w4 & 0xFF, b2, ax2);
}
