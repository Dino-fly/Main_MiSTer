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
#include <strings.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/uinput.h>

#include "hardware.h"
#include "spi.h"
#include "user_io.h"
#include "cfg.h"
#include "snacpad.h"
#include "native_fb.h"

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
static int supported = SNAC_UNPROBED;
static int enabled = 0;    // last enable bit sent to the core
static uint32_t poll_timer = 0;

/*
  The last answer to "should this reader be driving the port", kept so a screen can ask.

  A published copy of snacpad_poll()'s own `want` rather than a second derivation of it:
  two of its three terms are cfg fields anybody can read, but the third is
  core_owns_snac(), which is static here and has to be. A front-end that recomputed the
  two it can see would announce a missing reader on exactly the core that does not need one
  - the old PSX core reading the port natively - which is the one place the message would
  be a flat lie.
*/
static int wanted = 0;

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

/*
  Let go of everything the pad is holding, before the device stops existing.

  UI_DEV_DESTROY removes the node. It does not say what was held at the time, and there is
  nobody left to send a release afterwards - so whatever the consumer last saw pressed, it
  goes on believing is pressed. pad_reset_state() below does not help: it clears *our*
  struct, after the device it would have reported through is already gone.

  This is not theoretical, and the way it presents is worth writing down because it looks
  nothing like a stuck button. Setting the PSX core's Pad1 to SNAC-port1 is done by holding
  a direction on the very pad this reads - and landing on that value is exactly what makes
  the core claim the port, so core_owns_snac() flips and this device is destroyed on the
  same poll, with the direction still down. The options row then received a direction that
  never came up and cycled through its own values on its own, which is what a player sees
  and reports as "the menu keeps changing the setting". Found on hardware; the harness has
  no /dev/uinput and cannot see any of it.

  Zero every button, centre the hat and both sticks, sync, and only then destroy. Cheap,
  and it costs nothing on the path where nothing was held.
*/
static void pad_release_all(int idx)
{
	snac_pad_t *pad = &pads[idx];
	if (pad->fd < 0) return;

	for (int i = 0; i < 16; i++)
	{
		if (btn_codes[i]) emit(pad->fd, EV_KEY, btn_codes[i], 0);
	}

	emit(pad->fd, EV_ABS, ABS_HAT0X, 0);
	emit(pad->fd, EV_ABS, ABS_HAT0Y, 0);

	static const uint16_t sticks[4] = { ABS_X, ABS_Y, ABS_RX, ABS_RY };
	for (int i = 0; i < 4; i++) emit(pad->fd, EV_ABS, sticks[i], 0x80);

	emit(pad->fd, EV_SYN, SYN_REPORT, 0);
}

static void pad_destroy(int idx)
{
	if (pads[idx].fd >= 0)
	{
		pad_release_all(idx);
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

static int owned_logged = -1;   // last core_owns_snac() answer we printed

// One comma-separated field of a CONF_STR line. Cores do not quote.
static void confstr_field(const char *src, int idx, char *out, int len)
{
	out[0] = 0;
	int n = 0;
	const char *p = src;
	while (n < idx)
	{
		const char *c = strchr(p, ',');
		if (!c) return;
		p = c + 1;
		n++;
	}
	const char *e = strchr(p, ',');
	int l = e ? (int)(e - p) : (int)strlen(p);
	if (l > len - 1) l = len - 1;
	memcpy(out, p, l);
	out[l] = 0;
}

/*
  Does the running core drive the SNAC port itself?

  This is the whole of the arbitration, and it replaces a setting of ours. Exactly one
  reader may own the bus: the core's, or this one. The core's own options already say
  which the player wants, so asking them is both the honest answer and one less thing to
  configure - and it generalises past PSX, which a setting of ours never could. A SNES
  SNAC adapter with the SNES core's own SNAC switch on now takes the bus from us for the
  same reason a PSX pad in native mode does.

  Every SNAC-capable core spells it one of two ways, so there are two rules and no table
  of core names to keep in step:

    - the value the player picked names SNAC. PSX's Pad1 offers "SNAC-port1" among
      thirteen values, the N64's "Pad 1 Type" offers "SNAC", the SMS calls the option
      "USERIO" and the value "SNAC".
    - or the option itself IS the switch, and anything but its first value turns it on.
      The NES ("SNAC,Off,Controllers,Zapper,3D Glasses"), the Mega Drive ("Off,Port 1,
      Port 2,Port 3") and the SNES ("No,Yes") are all this shape.

  The name match is exact for the second rule on purpose: "SNAC MemCard" and "SNAC
  Compare" both begin with SNAC and neither of them hands over the port.

  Erring towards "the core owns it" is the safe direction. Get it wrong that way and
  nobody drives the port - the player loses a pad until they look at the option. Get it
  wrong the other way and two readers drive the same pins at once, which is the one
  outcome that can damage something.

  Every value here is read from cur_status[], the firmware's own shadow of the status
  word (user_io.cpp:546) - not from the core. So this is a handful of local memory reads
  and a string compare, cheap enough for the 2 ms poll, and it is already correct on the
  first poll because <CORE>.CFG is loaded into that shadow during user_io_init().
*/
static int core_owns_snac()
{
	if (is_menu()) return 0;

	for (int i = 1; i < 64; i++)
	{
		char *line = user_io_get_confstr(i);
		if (!line) break;
		if (!*line) continue;

		char spec[40];
		confstr_field(line, 0, spec, sizeof(spec));
		if (!spec[0]) continue;

		// A page definition ("P1,Audio & Video"), which is not an option.
		if (spec[0] == 'P' && spec[1] >= '0' && spec[1] <= '9' && !spec[2]) continue;

		/*
		  Strip the prefixes. They arrive in either order - "D1P1O[104]" and "P1O[3:1]"
		  are both real - so this loops rather than assuming a sequence, the same way
		  chome_core.cpp's split_prefix() does and for the same reason.
		*/
		const char *body = spec;
		while ((body[0] == 'H' || body[0] == 'h' || body[0] == 'D' || body[0] == 'd'
			|| body[0] == 'P') && body[1] && body[2] && body[2] != ',') body += 2;

		if (body[0] != 'O' && body[0] != 'o') continue;

		// "o" is the second status word. Dropping this reads a different option.
		int ex = (body[0] == 'o') ? 1 : 0;

		char name[40];
		confstr_field(line, 1, name, sizeof(name));
		if (!name[0]) continue;

		uint32_t v = user_io_status_get(body + 1, ex);

		char val[40];
		confstr_field(line, 2 + (int)v, val, sizeof(val));

		if (val[0] && strcasestr(val, "SNAC")) return 1;
		if (!strcasecmp(name, "SNAC") && v != 0) return 1;
	}

	return 0;
}

/*
  Which controller IDs this reader can decode.

  The reader in the fabric accepts anything that is not 0xFF and acks like a pad
  (psx_snac_pad.sv:178), and it never checks the 0x5A a real PSX pad sends as its second
  byte. That is deliberately loose there and has to be tightened somewhere, because of
  what the ID byte feeds: the buttons are delivered *inverted*, so a device that answers
  with zeroes arrives here as id 0x00 with every button and every direction held down
  for ever. On this front-end that pad drives the menu, so the shelf would scroll on its
  own and no press could stop it.

  A SNAC adapter for another console on the same port is the way that happens - the
  bypass switch on a SuperDock routes the bus to an extension port where anything can be
  plugged in. Whether such an adapter idles its lines high (in which case the reader
  already rejects it: the ID reads 0xFF and it fails cleanly) or low is an electrical
  question about hardware we do not control, so it is not worth predicting - it is worth
  refusing.

  0x41 digital, 0x73 analog and 0x53 analog-mode-2 are the three the reader can actually
  decode; see the note at the top of psx_snac_pad.sv. Anything else is refused and said
  out loud once, rather than silently, so that a real pad we have not met turns into a
  bug report with an ID in it instead of a player with a dead port.
*/
static int psx_id_known(uint8_t id)
{
	return (id == 0x41 || id == 0x73 || id == 0x53) ? 1 : 0;
}

/*
  A port's presence bit, with the ID sanity check applied and complained about once.

  Once per distinct ID rather than once per port: an adapter that answers differently as
  it is plugged in should say so each time it changes, and a poll running every 2 ms must
  not put the same line in the log five hundred times a second.
*/
static uint8_t bad_id_said[2] = { 0, 0 };
#ifdef CHOME_HOST_TEST
static int test_present[2] = { 0, 0 };
#endif

static int pad_present(int idx, uint16_t w)
{
	if (!(w >> 15)) { bad_id_said[idx] = 0; return 0; }

	uint8_t id = (uint8_t)(w & 0xFF);
	if (psx_id_known(id)) { bad_id_said[idx] = 0; return 1; }

	if (bad_id_said[idx] != id)
	{
		bad_id_said[idx] = id;
		printf("snacpad: port %d answered with id %02X, which is not a PSX pad this reader"
			" can decode - ignoring it. If a real PlayStation controller is plugged in"
			" here, please report this ID.\n", idx + 1, id);
	}
	return 0;
}

void snacpad_init()
{
	// core (re)loaded: the fabric side is back to disabled, ask again
	supported = SNAC_UNPROBED;
	enabled = 0;
	/*
	  And nothing is known about the new core's port yet. Cleared rather than left standing
	  because a screen asks these two together: with `wanted` inherited from the previous
	  core and `supported` reset, the pair reads as "we want the port and have not looked",
	  which is silent - but the inherited half would be describing a core that is gone.
	*/
	wanted = 0;
	owned_logged = -1;
	bad_id_said[0] = bad_id_said[1] = 0;
	poll_timer = 0;
}

/*
  The Console Mode menu core reads a PSX pad too, and reports it somewhere else.

  It carries its own ps1_snac_controller rather than the framework's psx_snac_pad, and
  publishes it through hps_io command 0x2E - the menu-mask read - in words 2 and 3.
  Nothing in that core implements UIO_SNAC_PAD at all, so the probe below finds no reader
  and the pad would simply be dead. Without this, the one core that can put the shelf on
  a television in colour (see native_fb.h) would be the one core where a PlayStation pad
  does not work, and a player would have to choose between the two.

  The button word needs no translation: the same sixteen bits in the same order, active
  high, because both readers decode the same protocol. What that core does not offer is
  the analog sticks or a second port, and it reports controller_valid where the framework
  reader reports the pad's ID - so the ID is given as 0x41. A digital pad is what this
  delivers, and claiming a DualShock would be a lie about the axes.

  Gated on the core being identified from its config string, never probed blind: words 2
  and 3 of 0x2E are undefined on every other core, and reading somebody else's undefined
  bytes as a gamepad is how a phantom pad gets invented.

  One asymmetry worth knowing. The framework reader is *told* whether to drive the port;
  this one has snac_enable tied high and polls regardless, so `want` here decides only
  whether a uinput device exists, not whether the core touches the pins. Handing the port
  to something else is therefore not in our gift on this core.
*/
static int cm_pad_poll(int want)
{
	if (!native_fb_available()) return 0;

	spi_uio_cmd_cont(UIO_GET_OSDMASK);
	spi_w(0);                     // byte_cnt 1: the menu mask, which is not ours
	uint16_t btns = spi_w(0);     // byte_cnt 2: snac_buttons, active high
	uint16_t dbg = spi_w(0);      // byte_cnt 3: snac_debug; bit 11 is controller_valid
	DisableIO();

	if (supported != SNAC_READER)
	{
		printf("snacpad: this core reads the SNAC port at 0x2E (Console Mode), using that\n");
		supported = SNAC_READER;
	}
	enabled = want;

	/*
	  Centred, not zero. This reader has no sticks, and 0,0 is hard up and to the left to
	  anything that reads the axes - which for a pad the whole input pipeline treats as
	  ordinary would be a stuck stick rather than an absent one.
	*/
	static const uint8_t centred[4] = { 0x80, 0x80, 0x80, 0x80 };

	const int present = want && ((dbg >> 11) & 1);
#ifdef CHOME_HOST_TEST
	test_present[0] = present;
	test_present[1] = 0;
#endif

	pad_update(0, present, 0x41, btns, centred);
	pad_update(1, 0, 0, 0, centred);      // one port only; releases it if it ever existed
	return 1;
}

void snacpad_poll()
{
	/*
	  Who owns the port, asked fresh every poll rather than settled once.

	  Three terms, and each is a different kind of statement:

	    snac_pad     - the player wants SNAC pads at all.
	    snac_device  - what is physically on the port. Not inferable: the bypass switch
	                   on a SuperDock reroutes the bus to an extension port that takes
	                   any console's adapter, and nothing readable changes when it moves.
	                   So it is asked once and believed. See cfg.h.
	    the core     - whether the running core has claimed the bus with its own option.

	  Re-derived every 2 ms on purpose. There is no settled state to unwind, so a player
	  who changes Pad1 while a game runs hands the port over within one poll and the loser
	  lets go; the cost is at most one corrupt pad frame during the switch. The old code
	  needed a probe window and five variables to hold that transition, and all of it
	  existed to serve a hand-over this no longer does.
	*/
	int claimed = core_owns_snac();
	int want = (cfg.snac_pad != 0) && (cfg.snac_device == 0) && !claimed;

	// Published here, above every early return below, so a screen asking snacpad_wanted()
	// gets this poll's answer even on the passes that do nothing else. The cheapest of
	// those - the feature switched off entirely - returns before touching SPI, and that
	// is exactly the case a front-end must stay quiet about, so it has to be recorded.
	wanted = want;

	if (owned_logged != claimed)
	{
		owned_logged = claimed;
		if (claimed) printf("snacpad: this core reads the SNAC port itself, standing back\n");
		else if (cfg.snac_pad != 0 && cfg.snac_device != 0)
			printf("snacpad: snac_device says the port is not a PSX pad, not touching it\n");
		else if (cfg.snac_pad != 0) printf("snacpad: reading the SNAC port for this core\n");
	}

	/*
	  With the feature off there is nothing to say to the core and no reason to touch
	  SPI at all. Handing the port back is different: the reader has to be *told* to let
	  go, rather than trusted to have come up idle, or two readers would drive the same
	  pins. So once `supported` is known the poll keeps running even when it wants
	  nothing - at SNAC_RETRY_MS, set at the bottom - which is also what lets a core
	  option changed mid-game take effect without a relaunch.
	*/
	if (!want && !enabled && supported < 0
		&& pads[0].fd < 0 && pads[1].fd < 0) return;

	if (poll_timer && !CheckTimer(poll_timer)) return;
	poll_timer = GetTimer(SNAC_POLL_MS);

	uint16_t status = spi_uio_cmd_cont(UIO_SNAC_PAD);
	if ((status >> 8) != SNAC_MAGIC)
	{
		DisableIO();

		// No framework reader - but see cm_pad_poll(): one core reads the port itself and
		// says so elsewhere, and on that core this is not a fault to report.
		if (cm_pad_poll(want)) return;

		if (supported != SNAC_NO_READER)
		{
			if (want) printf("snacpad: no SNAC pad reader in this core (sys update needed)\n");
			supported = SNAC_NO_READER;
			pad_destroy(0);
			pad_destroy(1);
		}
#ifdef CHOME_HOST_TEST
		test_present[0] = test_present[1] = 0;
#endif
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

	if (supported != SNAC_READER)
	{
		printf("snacpad: SNAC pad reader present, %s\n", want ? "enabled" : "disabled");
		supported = SNAC_READER;
	}
	enabled = want;

	if (!want)
	{
		pad_destroy(0);
		pad_destroy(1);
#ifdef CHOME_HOST_TEST
		test_present[0] = test_present[1] = 0;
#endif
		poll_timer = GetTimer(SNAC_RETRY_MS);
		return;
	}

	uint8_t ax1[4] = { (uint8_t)l1, (uint8_t)(l1 >> 8), (uint8_t)r1, (uint8_t)(r1 >> 8) };
	uint8_t ax2[4] = { (uint8_t)l2, (uint8_t)(l2 >> 8), (uint8_t)r2, (uint8_t)(r2 >> 8) };

	int p1 = pad_present(0, w0);
	int p2 = pad_present(1, w4);
#ifdef CHOME_HOST_TEST
	test_present[0] = p1;
	test_present[1] = p2;
#endif
	pad_update(0, p1, w0 & 0xFF, b1, ax1);
	pad_update(1, p2, w4 & 0xFF, b2, ax2);
}

// See the note in snacpad.h. Both are what the poll above recorded, nothing more.
int snacpad_reader() { return supported; }
int snacpad_wanted() { return wanted; }

#ifdef CHOME_HOST_TEST
/*
  Two answers the host harness needs and cannot get any other way.

  Not a back door into the logic - both are recorded by the code above as it runs, and
  neither changes what it does. They exist because the two things worth asserting here are
  invisible from outside: the enable bit is a value handed to SPI and then gone, and a pad
  that passes the ID check still creates no device on a host with no /dev/uinput, so
  "was it accepted" cannot be read off a uinput node that was never made.
*/
int snacpad_test_enabled() { return enabled; }
int snacpad_test_present(int idx) { return (idx >= 0 && idx < 2) ? test_present[idx] : 0; }
#endif
