#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include "chome_bt.h"

#include "../../hardware.h"

#define PAIR_OUT   "/tmp/classicui_btpair.out"
#define LIST_OUT   "/tmp/classicui_btlist.out"
#define ADAPTER    "/sys/class/bluetooth/hci0"

/* --------------------------------------------------------------- present --- */

/*
  menu.cpp asks libbluetooth (hci_get_route), which means linking it and opening a
  socket. The adapter's presence is a directory in sysfs, which is a stat - and this
  is asked on every frame the screen is open.
*/
static int present_forced = -1;

void bt_force_present(int on) { present_forced = on; }

int bt_present()
{
	if (present_forced >= 0) return present_forced;

	struct stat st;
	return (!stat(ADAPTER, &st) && S_ISDIR(st.st_mode)) ? 1 : 0;
}

/* ----------------------------------------------------------------- naming --- */

/*
  Only pads whose own name is unhelpful. Sony's is the reason this exists; Microsoft's
  varies by revision and none of them say which; Nintendo and 8BitDo already name
  themselves properly and are deliberately absent.
*/
static const struct { uint16_t vid, pid; const char *label; } pad_names[] =
{
	{ 0x054C, 0x0268, "PlayStation 3 Controller" },
	{ 0x054C, 0x05C4, "PlayStation 4 Controller" },
	{ 0x054C, 0x09CC, "PlayStation 4 Controller" },
	{ 0x054C, 0x0BA0, "PlayStation 4 Controller" },     // the USB dongle
	{ 0x054C, 0x0CE6, "PlayStation 5 Controller" },
	{ 0x054C, 0x0DF2, "PlayStation 5 Controller" },
	{ 0x045E, 0x028E, "Xbox 360 Controller" },
	{ 0x045E, 0x02E0, "Xbox Controller" },
	{ 0x045E, 0x02FD, "Xbox Controller" },
	{ 0x045E, 0x0B13, "Xbox Controller" },
	{ 0x045E, 0x0B20, "Xbox Controller" },
};

const char *bt_pad_label(uint16_t vid, uint16_t pid, const char *reported)
{
	for (size_t i = 0; i < sizeof(pad_names) / sizeof(pad_names[0]); i++)
	{
		if (pad_names[i].vid == vid && pad_names[i].pid == pid) return pad_names[i].label;
	}
	return (reported && *reported) ? reported : "Controller";
}

/* ------------------------------------------------------------------ exec --- */

static int run_to(const char *const argv[], const char *path)
{
	pid_t p = fork();
	if (p < 0) return -1;

	if (!p)
	{
		int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (fd >= 0)
		{
			dup2(fd, 1);
			dup2(fd, 2);
			close(fd);
		}
		execvp(argv[0], (char *const *)argv);
		_exit(127);
	}

	int st = 0;
	if (waitpid(p, &st, 0) < 0) return -1;
	return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static char *slurp(const char *path)
{
	FILE *f = fopen(path, "rb");
	if (!f) return 0;

	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (n < 0 || n > 1 << 20) { fclose(f); return 0; }

	char *b = (char*)malloc((size_t)n + 1);
	if (!b) { fclose(f); return 0; }

	size_t got = fread(b, 1, (size_t)n, f);
	b[got] = 0;
	fclose(f);
	return b;
}

/* --------------------------------------------------------------- parsing --- */

static void trim_end(char *s)
{
	size_t n = strlen(s);
	while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '\t')) s[--n] = 0;
}

// "AA:BB:CC:DD:EE:FF" and nothing else.
static int is_mac(const char *s)
{
	for (int i = 0; i < 17; i++)
	{
		if ((i % 3) == 2) { if (s[i] != ':') return 0; }
		else if (!isxdigit((unsigned char)s[i])) return 0;
	}
	return s[17] == 0 || s[17] == ' ' || s[17] == '\n' || s[17] == '\r';
}

static bt_dev *find_mac(bt_dev *out, int n, const char *mac)
{
	for (int i = 0; i < n; i++) if (!strcasecmp(out[i].mac, mac)) return &out[i];
	return 0;
}

/*
  Is this "name" just the address again? bluetoothctl names a device it has never had
  a name from after its own address, but written with dashes rather than the colons it
  uses everywhere else - so comparing the two strings directly says they differ. Only
  the hex digits are compared, which is true of either spelling.
*/
static int same_as_mac(const char *name, const char *mac)
{
	const char *a = name, *b = mac;
	while (*a && *b)
	{
		while (*a == ':' || *a == '-') a++;
		while (*b == ':' || *b == '-') b++;
		if (!*a || !*b) break;
		if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
		a++;
		b++;
	}

	while (*a == ':' || *a == '-') a++;
	while (*b == ':' || *b == '-') b++;
	return (!*a && !*b);
}

/*
  Two sections, both bluetoothctl's own output:

      Device AA:BB:CC:DD:EE:FF Wireless Controller
      == AA:BB:CC:DD:EE:FF
          Connected: yes

  The device lines come from `paired-devices`; each `==` marker is written by the
  refresh child before that device's `info`, because 5.61 has no way to ask for the
  connected ones as a list. A device with no marker is simply not connected.
*/
int bt_parse_paired(const char *text, bt_dev *out, int max)
{
	if (!text || !out || max < 1) return 0;

	int n = 0;
	char cur[18] = {};

	const char *p = text;
	while (*p)
	{
		char line[256];
		const char *nl = strchr(p, '\n');
		size_t len = nl ? (size_t)(nl - p) : strlen(p);
		if (len >= sizeof(line)) len = sizeof(line) - 1;
		memcpy(line, p, len);
		line[len] = 0;
		p = nl ? nl + 1 : p + strlen(p);

		trim_end(line);

		char *s = line;
		while (*s == ' ' || *s == '\t') s++;

		if (!strncmp(s, "Device ", 7))
		{
			char *mac = s + 7;
			if (!is_mac(mac)) continue;

			if (n >= max) continue;
			if (find_mac(out, n, mac)) continue;      // bluetoothctl can repeat a device

			bt_dev *d = &out[n];
			memset(d, 0, sizeof(*d));
			memcpy(d->mac, mac, 17);
			d->mac[17] = 0;

			const char *name = (strlen(mac) > 18) ? mac + 18 : "";
			while (*name == ' ') name++;
			/*
			  A device bluetoothctl has never seen a name for is listed by its address.
			  Showing that twice says nothing, so leave the name empty and let the UI
			  decide what to call it.
			*/
			if (*name && !same_as_mac(name, d->mac)) snprintf(d->name, sizeof(d->name), "%s", name);
			n++;
			continue;
		}

		if (!strncmp(s, "== ", 3))
		{
			cur[0] = 0;
			if (is_mac(s + 3)) { memcpy(cur, s + 3, 17); cur[17] = 0; }
			continue;
		}

		if (cur[0] && !strncmp(s, "Connected:", 10))
		{
			bt_dev *d = find_mac(out, n, cur);
			if (d && strcasestr(s + 10, "yes")) d->connected = 1;
			continue;
		}
	}

	return n;
}

/* -------------------------------------------------------------- progress --- */

/*
  btctl's vocabulary, which is the whole reason this is a state machine and not a
  return code. It prints, per device it finds:

      NAME: Wireless Controller
      MAC:  AA:BB:CC:DD:EE:FF
      Skipping: non-input device        <- and back to looking
      Removing existing pair...
      Searching...
      Pairing...
      Trusting...
      Connecting...
      Done.                             <- paired, and it loops for the next one
      Failed!  /  Pair error!  /  Timed out.
      Type 0000 and <Enter>             <- wants a PIN, which a pad cannot be given here

  Everything after a "Done." or a failure belongs to the *next* controller, because
  `btctl pair` never stops on its own.
*/
static int  pg_state = BTP_IDLE;
static char pg_name[BT_NAME] = {};
static char pg_mac[18] = {};
static char pg_detail[96] = {};
static int  pg_done = 0;

// Checking that a pairing turned into a working link, and nudging it if it did not.
static unsigned long pg_verify = 0;
static int pg_tries = 0;

void bt_progress_reset()
{
	pg_state = BTP_LOOKING;
	pg_name[0] = 0;
	pg_mac[0] = 0;
	pg_detail[0] = 0;
	pg_done = 0;
	pg_verify = 0;
	pg_tries = 0;
}

static void pg_say(const char *s) { snprintf(pg_detail, sizeof(pg_detail), "%s", s); }

int bt_ingest_progress(const char *line)
{
	if (!line) return pg_state;

	char s[256];
	snprintf(s, sizeof(s), "%s", line);
	trim_end(s);

	char *t = s;
	while (*t == ' ' || *t == '\t') t++;
	if (!*t) return pg_state;

	if (!strncmp(t, "NAME:", 5))
	{
		const char *v = t + 5;
		while (*v == ' ') v++;
		snprintf(pg_name, sizeof(pg_name), "%s", v);
		pg_state = BTP_WORKING;
		pg_say("Found it");
		return pg_state;
	}

	/*
	  Kept, though never shown - the address means nothing to a player, but it is the
	  only handle for connecting the pad afterwards, which is not something the pad can
	  be relied on to do itself.
	*/
	if (!strncmp(t, "MAC:", 4))
	{
		const char *v = t + 4;
		while (*v == ' ') v++;
		if (is_mac(v)) { memcpy(pg_mac, v, 17); pg_mac[17] = 0; }
		return pg_state;
	}

	if (!strncmp(t, "Skipping:", 9))
	{
		// Not a controller. Say so, because the player is holding a button down and
		// deserves to know it was noticed and rejected.
		pg_state = BTP_LOOKING;
		pg_say("That is not a controller - still looking");
		pg_name[0] = 0;
		return pg_state;
	}

	if (!strncmp(t, "Type ", 5))
	{
		/*
		  A pad asking for a PIN cannot be answered from here: btctl replies 0000 and
		  that is either right or the pairing fails. Saying so is better than a silent
		  stall, and it is rare enough not to be worth a keypad.
		*/
		pg_state = BTP_PIN;
		pg_say("This controller wants a code - trying 0000");
		return pg_state;
	}

	if (!strcmp(t, "Removing existing pair...")) { pg_state = BTP_WORKING; pg_say("Forgetting the old pairing"); return pg_state; }
	if (!strcmp(t, "Searching..."))              { pg_state = BTP_WORKING; pg_say("Looking again");             return pg_state; }
	if (!strcmp(t, "Pairing..."))                { pg_state = BTP_WORKING; pg_say("Pairing");                   return pg_state; }
	if (!strcmp(t, "Trusting..."))               { pg_state = BTP_WORKING; pg_say("Almost there");              return pg_state; }
	if (!strcmp(t, "Connecting..."))             { pg_state = BTP_WORKING; pg_say("Connecting");                return pg_state; }

	if (!strcmp(t, "Done."))
	{
		/*
		  btctl says "Done." once its Connect() has returned, which is true but does not
		  stay true. A DS4 keeps its console's registration, and the first press of its
		  home button reconnects it there instead - which is how a pad that had just
		  paired ended up waking a PS4 in the next room. So this is "paired", not "ready",
		  and whether it is really usable is checked rather than announced.
		*/
		pg_state = BTP_OK;
		pg_done++;
		pg_say("Paired - checking it is awake");
		pg_verify = GetTimer(1500);
		pg_tries = 0;
		return pg_state;
	}

	if (!strcmp(t, "Failed!") || strstr(t, "Pair error!") || !strncmp(t, "Timed out.", 10)
		|| !strncmp(t, "Cancelling pairing.", 19))
	{
		pg_state = BTP_FAIL;
		pg_say("That did not work - try holding the buttons again");
		return pg_state;
	}

	return pg_state;
}

void bt_pair_ack()
{
	// Anything but idle, not just the three finished states: pairing mode now stops
	// itself, so leaving any other state uncleared would strand the panel on screen.
	if (pg_state == BTP_IDLE) return;

	pg_state = BTP_IDLE;
	pg_detail[0] = 0;
	pg_name[0] = 0;
	pg_verify = 0;
}

int bt_pair_state() { return pg_state; }
int bt_pair_done() { return pg_done; }
const char *bt_pair_name() { return pg_name; }
const char *bt_pair_detail() { return pg_detail; }

/* ----------------------------------------------------------------- state --- */

static bt_dev devs[BT_MAX];
static int ndev = 0;

static int watching = 0;

static pid_t list_child = -1;       // the paired-list refresh
static unsigned long list_kill = 0;
static unsigned long list_due = 0;

static pid_t pair_child = -1;       // `btctl pair`, long-lived
static long  pair_off = 0;          // how much of its log has been read
static int   pairing = 0;

int bt_count() { return ndev; }

static const bt_dev *dev_by_mac(const char *mac)
{
	if (!mac || !*mac) return 0;
	for (int i = 0; i < ndev; i++) if (!strcasecmp(devs[i].mac, mac)) return &devs[i];
	return 0;
}

const bt_dev *bt_at(int i)
{
	if (i < 0 || i >= ndev) return 0;
	return &devs[i];
}

int bt_pairing() { return pairing; }

/* --------------------------------------------------------------- refresh --- */

/*
  One child for the whole list, because a fork per device would be a fork per device
  per refresh. bluetoothctl 5.61 cannot be asked for "the connected ones" - `devices
  Paired` is rejected as too many arguments - so the connected state comes from an
  `info` per device, and the shell does that loop inside the one child.
*/
static void list_start()
{
	if (!bt_present() || list_child > 0) return;

	pid_t p = fork();
	if (p < 0) return;

	if (!p)
	{
		const char *sh[] = { "/bin/sh", "-c",
			"bluetoothctl paired-devices 2>/dev/null\n"
			"bluetoothctl paired-devices 2>/dev/null | while read -r _d mac _rest; do\n"
			"  [ -n \"$mac\" ] || continue\n"
			"  echo \"== $mac\"\n"
			"  bluetoothctl info \"$mac\" 2>/dev/null | grep 'Connected:'\n"
			"done\n", 0 };
		run_to(sh, LIST_OUT);
		_exit(0);
	}

	list_child = p;
	list_kill = GetTimer(15000);
}

void bt_refresh() { list_due = 0; }

void bt_ingest_paired(const char *text)
{
	ndev = text ? bt_parse_paired(text, devs, BT_MAX) : 0;
}

static void list_finish()
{
	char *txt = slurp(LIST_OUT);
	if (!txt) { ndev = 0; return; }

	bt_ingest_paired(txt);
	free(txt);
}

/* ---------------------------------------------------------------- pairing --- */

void bt_pair_start()
{
	if (!bt_present() || pairing) return;

	// Start from an empty log: the tail reader works from an offset, and last time's
	// "Done." must not be read as this time's.
	unlink(PAIR_OUT);
	pair_off = 0;
	bt_progress_reset();

	pid_t p = fork();
	if (p < 0) { pg_state = BTP_IDLE; return; }

	if (!p)
	{
		/*
		  btctl is exec'd directly rather than through run_to(), so the pid held here is
		  btctl itself and a signal reaches the thing that has to be stopped instead of
		  a process waiting on it. Its own group, so stopping it cannot touch anything
		  else - not the firmware, and not the other module's children.
		*/
		setpgid(0, 0);

		int fd = open(PAIR_OUT, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (fd >= 0)
		{
			dup2(fd, 1);
			dup2(fd, 2);
			close(fd);
		}

		const char *argv[] = { "btctl", "pair", 0 };
		execvp(argv[0], (char *const *)argv);
		_exit(127);
	}

	pair_child = p;
	pairing = 1;
	printf("ClassicUI: pairing mode on (btctl pair, pid %d)\n", (int)p);
}

/*
  Stops the discovery without touching what the screen is saying.

  Split out because stopping is no longer only something the player asks for: pairing
  mode has to end the moment a controller is working, and for a reason worth spelling
  out. `btctl pair` loops, and on re-discovering a device it has already paired it calls
  RemoveDevice to pair it again from scratch. If that second attempt then fails - which
  it will for a pad that has gone back to its console - the pairing it had just made is
  destroyed. Leaving discovery running after a success is therefore a way to lose it.
*/
static void pair_child_stop()
{
	if (!pairing) return;

	/*
	  SIGINT, not SIGTERM: btctl catches KeyboardInterrupt and stops discovery on the
	  way out, and a discovery left running keeps the adapter busy and the radio awake.
	  This is also the signal MiSTer's own pairing entry sends (menu.cpp passes
	  "-SIGINT btpair btctl" to its script runner).
	*/
	if (pair_child > 0) kill(-pair_child, SIGINT);

	pairing = 0;
	printf("ClassicUI: pairing mode off\n");

	// Whatever paired is worth showing straight away.
	bt_refresh();
}

// What the player asks for: stop, and clear the panel back to the list.
void bt_pair_stop()
{
	pair_child_stop();
	pg_state = BTP_IDLE;
	pg_detail[0] = 0;
	pg_name[0] = 0;
	pg_verify = 0;
}

// Reads whatever btctl has written since last time and advances the conversation.
static void pair_drain()
{
	int fd = open(PAIR_OUT, O_RDONLY);
	if (fd < 0) return;

	if (lseek(fd, pair_off, SEEK_SET) < 0) { close(fd); return; }

	static char buf[4096];
	ssize_t got = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (got <= 0) return;

	buf[got] = 0;

	/*
	  Only whole lines: a half-written one would be parsed as something it is not, and
	  the rest of it read again next frame. btctl flushes after every message, so a
	  partial line is a matter of a frame or two.
	*/
	char *last = strrchr(buf, '\n');
	if (!last)
	{
		// A full buffer with no line in it would otherwise be re-read forever. btctl
		// writes short lines, so this only guards against something else in that file.
		if ((size_t)got >= sizeof(buf) - 1) pair_off += got;
		return;
	}

	*last = 0;
	pair_off += (last - buf) + 1;

	int was_done = pg_done;

	char *save = 0;
	for (char *line = strtok_r(buf, "\n", &save); line; line = strtok_r(0, "\n", &save))
	{
		bt_ingest_progress(line);
	}

	// A controller that just paired should appear in the list without waiting for the
	// periodic refresh.
	if (pg_done != was_done) bt_refresh();
}

/* ------------------------------------------------------------------ forget --- */

/*
  Asks the adapter to bring the link up. bluetoothctl rather than btctl, which has no
  connect; harmless when the link is already up, so it is never conditional.

  This exists because a paired pad cannot be relied on to connect itself. A DS4 that is
  also registered to a console reconnects *there* when its home button is pressed, so
  waiting for the player to wake it is waiting for the wrong host to answer.
*/
void bt_connect(const char *mac)
{
	if (!bt_present() || !mac || !*mac) return;

	pid_t p = fork();
	if (p < 0) return;

	if (!p)
	{
		if (fork()) _exit(0);            // orphaned, so init reaps it - see bt_forget

		const char *argv[] = { "bluetoothctl", "connect", mac, 0 };
		run_to(argv, "/dev/null");
		_exit(0);
	}

	int st = 0;
	waitpid(p, &st, 0);
	printf("ClassicUI: asking %s to connect\n", mac);
}

void bt_forget(const char *mac)
{
	if (!bt_present() || !mac || !*mac) return;

	pid_t p = fork();
	if (p < 0) return;

	if (!p)
	{
		/*
		  Double-forked on purpose: the middle process exits at once, so btctl is
		  reparented to init and reaped there. Nothing in the firmware reaps children,
		  and a forget the UI does not need to wait for should not leave a zombie for
		  the rest of the session.
		*/
		if (fork()) _exit(0);

		const char *argv[] = { "btctl", "remove", mac, 0 };
		run_to(argv, "/dev/null");
		_exit(0);
	}

	int st = 0;
	waitpid(p, &st, 0);                 // the middle process, which returns immediately

	printf("ClassicUI: forgetting controller %s\n", mac);
	bt_refresh();
}

/* -------------------------------------------------------------------- poll --- */

void bt_watch(int on)
{
	on = on ? 1 : 0;
	if (watching == on) return;

	watching = on;
	if (on) list_due = 0;             // refresh as soon as the screen opens

	/*
	  Leaving the screen ends pairing mode. Discovery is not something to leave running
	  behind the player's back: it keeps the adapter busy, and a pad put into pairing
	  mode an hour later should not silently attach itself.
	*/
	if (!on && pairing) bt_pair_stop();
}

/*
  Did the pairing actually leave a usable controller? Runs whether or not pairing mode is
  still on, because the player may well have pressed Done the moment it said "Paired".
*/
static void verify_link()
{
	if (!pg_verify || !CheckTimer(pg_verify)) return;
	pg_verify = 0;

	const bt_dev *d = dev_by_mac(pg_mac);
	if (d && d->connected)
	{
		pg_say("Ready to play");
		pair_child_stop();               // before the loop can un-pair it
		return;
	}

	// Two attempts: enough for a pad that is merely slow, few enough that a pad which
	// has gone back to its console does not leave the screen trying forever.
	if (pg_mac[0] && pg_tries < 2)
	{
		pg_tries++;
		bt_connect(pg_mac);
		pg_say("Waking it up");
		pg_verify = GetTimer(5000);
		bt_refresh();
		return;
	}

	/*
	  Said plainly, because the reason is not the player's fault and not guessable: a pad
	  still registered to a console goes back to it.
	*/
	pg_say("Paired, but it went elsewhere - turn the console off and try Add again");
	pair_child_stop();
}

void bt_poll()
{
	if (!watching || !bt_present()) return;

	verify_link();

	if (pairing)
	{
		pair_drain();

		// btctl loops forever, so its exit means something went wrong - a missing
		// D-Bus, no adapter, a Python that will not import dbus.
		if (pair_child > 0)
		{
			int st = 0;
			pid_t r = waitpid(pair_child, &st, WNOHANG);
			if (r == pair_child || (r < 0 && errno == ECHILD))
			{
				pair_child = -1;
				pairing = 0;
				if (pg_state != BTP_OK)
				{
					pg_state = BTP_FAIL;
					pg_say("Pairing is not available on this machine");
				}
				printf("ClassicUI: btctl pair exited\n");
			}
		}
	}

	if (list_child > 0)
	{
		if (list_kill && CheckTimer(list_kill))
		{
			printf("ClassicUI: the controller list took too long, dropping it\n");
			kill(list_child, SIGKILL);
			list_kill = 0;
		}

		int st = 0;
		pid_t r = waitpid(list_child, &st, WNOHANG);
		if (r == list_child || (r < 0 && errno == ECHILD))
		{
			// Only a child we actually reaped wrote a list worth reading; on ECHILD the
			// file is whatever was there before, so the old one stays on screen.
			int reaped = (r == list_child);

			list_child = -1;
			list_kill = 0;
			if (reaped) list_finish();

			/*
			  Idle refreshes are cheap to ask for and not cheap to serve - two
			  bluetoothctl runs plus one per paired device, on an ARM - so they are
			  infrequent. Anything that actually changes the list (a pairing completing,
			  a forget) calls bt_refresh() and does not wait for this.
			*/
			list_due = GetTimer(10000);
		}
		return;
	}

	if (!list_due || CheckTimer(list_due)) list_start();
}
