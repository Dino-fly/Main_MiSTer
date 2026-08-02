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
//
// The PSX core is the exception, and snac_psx chooses which reader owns the port
// there - see the note above psx_hand_over() below.

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

/*
  Who owns the SNAC port on the PSX core.

  Every other core has only one candidate: the reader in sys/, mirroring pads through
  uinput. The PSX core has two, because a PSX core reading a PSX port natively is the
  thing SNAC was built for in the first place, and both drive the same clock and
  command pins. They cannot share.

  Native is the default, and it is the better one for a PSX player: the core sees the
  real protocol, so GunCon and Justifier, NeGcon and the wheels, rumble, and the
  physical memory cards in a SuperStation One all work. Emulation reaches none of
  those - it presents a plain digital pad.

  What emulation buys is the menu. The Select+Start chord is synthesised in this file,
  so with the core reading the port directly there is no uinput device, no chord, and
  no way to open the front-end from that pad at all. A player with only a SNAC pad and
  no other controller wants snac_psx=1 for that reason alone.

  So: snac_psx=0 (default) native, snac_psx=1 emulated. It is read per-core like every
  other key, but only ever consulted on the PSX core.

  Why this cannot be chosen per port, which is the obvious thing to want. The adapter
  gives each port its own ATT but shares CLK, CMD, DAT and ACK - one bus, two chip
  selects. Whoever drives it drives both ports, so "native on port 1, emulated on
  port 2" would be two masters on the same wires. The choice is for the pair.

  What *is* per port is which of the core's own pads is pointed at SNAC: Pad1 and Pad2
  are separate options. So in native mode the ports are handled one at a time, and a
  port with nothing in it is left on the core's virtual pad, where a USB or Bluetooth
  controller can still be that player.

  Which needs knowing what is plugged in, so the reader is run first. It reports a
  connected flag per port, and it is the only thing that can: it is also the only
  reason to keep it enabled at all when nothing is connected to either port - a pad
  plugged in later then still appears, and can still open the menu. That is the
  fallback, and snac_psx_fallback=0 turns it off and commits to native regardless.

  Memory cards cannot take part in any of this, and it is worth saying why. The core
  offers one option for both slots - "SNAC MemCard,Virtual,Real", not one per card -
  so there is nothing per-slot to control. And this reader could not detect them
  anyway: it addresses the controller (0x01) and a memory card answers to 0x81, which
  it never sends. Detecting a card would be a change to psx_snac_pad.sv and a rebuild
  of every core. So snac_psx_memcard is a plain manual choice, virtual by default,
  because "Real" with no card in the slot means no memory card at all.
*/
#define PSX_PROBE_MS 400

static int psx_native = 0;      // the core owns the bus on this core
static int psx_phase = 0;       // 0 probing, 1 settled
static int psx_probe_ok = 0;    // the reader answered, so presence is knowable
static int psx_seen[2] = {};    // a pad was seen on that port during the probe
static unsigned long psx_until = 0;
static int psx_emulate = 0;     // probe found nothing, so the reader is kept

// Everything before the first comma, minus the D<n>/h<n> hide markers and the
// leading O, which is what user_io_status_bits() wants.
static int confstr_spec(const char *line, char *out, int len)
{
	const char *p = line;

	while (*p)
	{
		if ((*p == 'D' || *p == 'd' || *p == 'H' || *p == 'h') && p[1] >= '0' && p[1] <= '9')
		{
			p += 2;
			continue;
		}
		break;
	}

	if (*p != 'O' && *p != 'o') return 0;
	p++;

	int n = 0;
	while (*p && *p != ',' && n < len - 1) out[n++] = *p++;
	out[n] = 0;
	return (*p == ',') && n;
}

/*
  Set one of the core's own options by name, to the value with that name.

  By name in both directions on purpose: Pad1 has thirteen values today and a core is
  free to add more, so an index would be a guess with a wrong controller as the prize.
*/
static int confstr_set(const char *option, const char *value)
{
	for (int i = 1; i < 64; i++)
	{
		char *line = user_io_get_confstr(i);
		if (!line || !*line) break;

		char spec[32];
		if (!confstr_spec(line, spec, sizeof(spec))) continue;

		const char *name = strchr(line, ',');
		if (!name) continue;
		name++;

		size_t nlen = strlen(option);
		if (strncasecmp(name, option, nlen) || name[nlen] != ',') continue;

		const char *v = name + nlen + 1;
		for (int idx = 0; *v; idx++)
		{
			const char *end = strchr(v, ',');
			size_t n = end ? (size_t)(end - v) : strlen(v);

			if (n == strlen(value) && !strncasecmp(v, value, n))
			{
				user_io_status_set(spec, (uint32_t)idx);
				printf("snacpad: PSX %s = %s\n", option, value);
				return 1;
			}

			if (!end) break;
			v = end + 1;
		}

		printf("snacpad: PSX %s has no \"%s\", left alone\n", option, value);
		return 0;
	}
	return 0;
}

/*
  Hand the bus to the core, for the ports that have something on them.

  Without pointing the core's pads at SNAC, "native" would leave it on its default
  Dualshock - a virtual pad nothing is feeding - and the player would get no input at
  all, which is worse than either mode.

  Nothing is written to the core's config. This applies at core load and lasts as long
  as the core does, so a player who changes Pad1 by hand keeps their choice until the
  next load, and snac_psx=1 stops it happening at all.
*/
static void psx_hand_over()
{
	if (!psx_probe_ok)
	{
		// No reader in this core, so nothing is knowable and nothing is touched. The
		// core's own SNAC options still work; they are just set by hand, as always.
		printf("snacpad: PSX core has no reader, leaving its pad options alone\n");
		return;
	}

	int force = (cfg.snac_psx_fallback == 0);

	if (psx_seen[0] || force) confstr_set("Pad1", "SNAC-port1");
	else printf("snacpad: nothing on SNAC port 1, leaving Pad1 virtual\n");

	if (psx_seen[1] || force) confstr_set("Pad2", "SNAC-port2");
	else printf("snacpad: nothing on SNAC port 2, leaving Pad2 virtual\n");

	// One option for both slots, and undetectable - see the note above.
	if (cfg.snac_psx_memcard) confstr_set("SNAC MemCard", "Real");
}

void snacpad_init()
{
	// core (re)loaded: the fabric side is back to disabled, probe again
	supported = -1;
	enabled = 0;
	psx_native = 0;
	psx_phase = 0;
	psx_probe_ok = 0;
	psx_seen[0] = psx_seen[1] = 0;
	psx_until = 0;
	psx_emulate = 0;
	poll_timer = 0;
}

void snacpad_poll()
{
	/*
	  On PSX the bus belongs to the core unless the player asked otherwise - but which
	  ports to hand it for depends on what is plugged in, and the reader is the only
	  thing that can tell us. So it runs first, for PSX_PROBE_MS, and the hand-over
	  happens after. All of it here rather than at init: the CONF_STR is not readable
	  that early, and neither is a pad that has not been polled yet.
	*/
	int psx_want_native = (cfg.snac_pad != 0) && is_psx() && (cfg.snac_psx == 0);
	int probing = psx_want_native && (psx_phase == 0);

	psx_native = psx_want_native && !probing && !psx_emulate;

	int want = (cfg.snac_pad != 0) && (!psx_native || probing);

	/*
	  With the feature off there is nothing to say to the core and no reason to touch
	  SPI at all. PSX native mode is different: the reader has to be *told* to let go,
	  rather than trusted to have come up idle, or the two readers would both be
	  driving the port. So it runs the handshake even though it wants nothing back -
	  once a second, after the first one establishes that the core has a reader.
	*/
	if (!want && !enabled && !psx_native && supported < 0
		&& pads[0].fd < 0 && pads[1].fd < 0) return;

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
		if (probing)
		{
			psx_probe_ok = 0;
			psx_phase = 1;
			psx_hand_over();
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

	if (probing)
	{
		psx_probe_ok = 1;
		if (w0 >> 15) psx_seen[0] = 1;
		if (w4 >> 15) psx_seen[1] = 1;

		if (!psx_until) psx_until = GetTimer(PSX_PROBE_MS);
		else if (CheckTimer(psx_until))
		{
			psx_phase = 1;

			if (psx_seen[0] || psx_seen[1] || cfg.snac_psx_fallback == 0)
			{
				psx_hand_over();
			}
			else
			{
				/*
				  Nothing on either port. Handing the bus over would buy nothing and
				  cost the menu, so the reader keeps it: a pad plugged in later still
				  appears, and can still open the front-end.
				*/
				psx_emulate = 1;
				printf("snacpad: no SNAC pad on either port, staying emulated\n");
			}
		}
	}

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
