/*
  Minimal uinput keyboard, so a test can press buttons on the MiSTer.

  Usage: keyinj <code|name>[:hold_ms] ...   e.g.  keyinj f12 up up enter:60 down

  It registers a device carrying the whole keyboard range, waits for the firmware to
  notice it (MiSTer enumerates input devices on an inotify event and that is not
  instant - without the settle the first key lands in the void), then sends each key
  as a press/release pair with a gap between them.

  This proves nothing about a *pad*: it is a keyboard, and the pad path has its own
  mapping layer. It exists only to reach screens that otherwise need a human thumb.
  A fake keyboard has already once hidden a total failure of the pad input path, so a
  green run of this on its own is not evidence that a player could do the same thing.

  Build for the device with the tree's own cross-compiler, then copy it over:
    arm-none-linux-gnueabihf-gcc -O1 -static -o keyinj keyinj.c
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/uinput.h>

static int fd;

static void emit(int type, int code, int val)
{
	struct input_event ev;
	memset(&ev, 0, sizeof(ev));
	ev.type = type;
	ev.code = code;
	ev.value = val;
	if (write(fd, &ev, sizeof(ev)) != sizeof(ev)) perror("write");
}

static void key(int code, int hold_ms)
{
	emit(EV_KEY, code, 1);
	emit(EV_SYN, SYN_REPORT, 0);
	usleep(hold_ms * 1000);
	emit(EV_KEY, code, 0);
	emit(EV_SYN, SYN_REPORT, 0);
	usleep(120000);
}

struct { const char *name; int code; } names[] = {
	{ "f12", KEY_F12 }, { "esc", KEY_ESC }, { "enter", KEY_ENTER },
	{ "up", KEY_UP }, { "down", KEY_DOWN }, { "left", KEY_LEFT }, { "right", KEY_RIGHT },
	{ "menu", KEY_F12 }, { 0, 0 }
};

int main(int argc, char **argv)
{
	if (argc < 2) { fprintf(stderr, "usage: keyinj <key>[:hold_ms] ...\n"); return 2; }

	fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
	if (fd < 0) { perror("/dev/uinput"); return 1; }

	ioctl(fd, UI_SET_EVBIT, EV_KEY);
	for (int c = 1; c < 255; c++) ioctl(fd, UI_SET_KEYBIT, c);

	struct uinput_setup us;
	memset(&us, 0, sizeof(us));
	us.id.bustype = BUS_USB;
	us.id.vendor  = 0x1234;
	us.id.product = 0x5678;
	snprintf(us.name, sizeof(us.name), "classicui test keyboard");
	if (ioctl(fd, UI_DEV_SETUP, &us) < 0) { perror("UI_DEV_SETUP"); return 1; }
	if (ioctl(fd, UI_DEV_CREATE) < 0) { perror("UI_DEV_CREATE"); return 1; }

	// Let the firmware enumerate it before anything is sent.
	sleep(2);

	for (int i = 1; i < argc; i++)
	{
		char buf[64];
		snprintf(buf, sizeof(buf), "%s", argv[i]);

		int hold = 40;
		char *colon = strchr(buf, ':');
		if (colon) { *colon = 0; hold = atoi(colon + 1); }

		int code = -1;
		if (buf[0] >= '0' && buf[0] <= '9') code = atoi(buf);
		else for (int k = 0; names[k].name; k++) if (!strcasecmp(buf, names[k].name)) code = names[k].code;

		if (code < 0) { fprintf(stderr, "unknown key: %s\n", buf); continue; }

		printf("key %s -> %d (hold %dms)\n", buf, code, hold);
		fflush(stdout);
		key(code, hold);
	}

	usleep(300000);
	ioctl(fd, UI_DEV_DESTROY);
	close(fd);
	return 0;
}
