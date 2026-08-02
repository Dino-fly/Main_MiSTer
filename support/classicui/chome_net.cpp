#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "chome_net.h"

#include "../../hardware.h"

#define CONF "/media/fat/linux/wpa_supplicant.conf"
#define BAK  "/media/fat/linux/wpa_supplicant.conf.bak"

#define SCAN_OUT "/tmp/chome_scan.txt"
#define LINK_OUT "/tmp/chome_link.txt"

/*
  The join child's progress, one digit written after each step it completes. A file
  rather than a pipe because the parent is a frame loop that must never block or poll
  a descriptor it might have to drain, and because a whole-file rewrite of one byte is
  the one write a reader cannot catch half-finished.
*/
#define JOIN_OUT "/tmp/chome_join.txt"

#define K_NONE 0
#define K_SCAN 1
#define K_LINK 2
#define K_JOIN 3

static char iface[24];
static int iface_known = 0;
static unsigned long iface_next = 0;      // when to look again, while there is nothing

/*
  Where the wireless interfaces are. A variable only so the harness can point it at a
  directory it controls; nothing outside a test ever changes it.
*/
static const char *net_dir = "/sys/class/net";
void net_set_sysdir(const char *d) { net_dir = d ? d : "/sys/class/net"; iface_known = 0; iface_next = 0; }

static net_ap aps[NET_MAX];
static int nap = 0;

static net_link link_now;

static pid_t child = -1;
static int kind = K_NONE;

static int scanning = 0;
static int join_state = JOIN_IDLE;
static char join_detail[96];
static int join_phase = 0;

static int watching = 0;
static unsigned long link_due = 0;
static unsigned long child_kill = 0;    // when to give up on a read-only child

/* --------------------------------------------------------------- interface --- */

/*
  A wireless interface is one with a "wireless" directory in sysfs. Looking for the
  name wlan0 would work on almost every MiSTer and then not work on somebody's, for
  no reason they could discover.
*/
/*
  Finding the wireless interface, and NOT remembering that there was none.

  The adapter is a USB dongle whose driver may not have created its interface by the time
  this front-end first asks - so the first look can legitimately find nothing on a machine
  that has perfectly good Wi-Fi. Caching that answer meant the Options row read "No adapter"
  and the Wi-Fi screen stayed empty for the rest of the session, on a MiSTer that was at
  that moment reachable over the very interface it was denying. Found on Dinofly's device,
  where /sys/class/net/wlan0/wireless existed and the front-end had been told otherwise
  since boot.

  So a hit is cached for good and a miss is retried, at most once a second: this is called
  from per-frame paths and opendir() on every frame for the lifetime of the process is not
  a trade worth making.
*/
static void find_iface()
{
	iface[0] = 0;

	DIR *d = opendir(net_dir);
	if (!d) return;

	struct dirent *e;
	while ((e = readdir(d)))
	{
		if (e->d_name[0] == '.') continue;

		char p[256];
		snprintf(p, sizeof(p), "%s/%s/wireless", net_dir, e->d_name);

		struct stat st;
		if (!stat(p, &st) && S_ISDIR(st.st_mode))
		{
			snprintf(iface, sizeof(iface), "%s", e->d_name);
			iface_known = 1;                 // only a hit is worth remembering
			break;
		}
	}
	closedir(d);
}

const char *net_iface()
{
	if (!iface_known && CheckTimer(iface_next))
	{
		iface_next = GetTimer(1000);
		find_iface();
	}
	return iface;
}

static int forced_present = -1;

void net_force_present(int on) { forced_present = on; }

int net_present()
{
	if (forced_present >= 0) return forced_present ? 1 : 0;
	return net_iface()[0] ? 1 : 0;
}

/* ------------------------------------------------------------------- exec --- */

// Runs argv to completion with its output in path. Only ever called where blocking
// is fine: inside a child of our own.
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

static int run_quiet(const char *const argv[])
{
	return run_to(argv, "/dev/null");
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

/* ---------------------------------------------------------------- parsing --- */

static int is_printable_ssid(const char *s)
{
	if (!*s) return 0;

	// iw prints unprintable bytes as \xNN. A name like that is not something to put
	// in a list a person is meant to choose from - those entries are hidden
	// networks and beacons padded with nulls.
	if (strstr(s, "\\x")) return 0;

	for (const char *q = s; *q; q++) if ((unsigned char)*q < 0x20) return 0;
	return 1;
}

static void trim_end(char *s)
{
	size_t n = strlen(s);
	while (n && (s[n - 1] == ' ' || s[n - 1] == '\r' || s[n - 1] == '\t')) s[--n] = 0;
}

int net_parse_scan(const char *text, net_ap *out, int max)
{
	int n = 0;
	if (!text || max < 1) return 0;

	net_ap cur;
	int have = 0;

	const char *p = text;
	while (*p)
	{
		const char *eol = strchr(p, '\n');
		size_t len = eol ? (size_t)(eol - p) : strlen(p);

		char line[512];
		if (len >= sizeof(line)) len = sizeof(line) - 1;
		memcpy(line, p, len);
		line[len] = 0;
		trim_end(line);

		if (!strncmp(line, "BSS ", 4))
		{
			if (have && is_printable_ssid(cur.ssid) && n < max) out[n++] = cur;

			memset(&cur, 0, sizeof(cur));
			cur.signal = -100;
			cur.current = strstr(line, "-- associated") ? 1 : 0;
			have = 1;
		}
		else if (have)
		{
			const char *q = line;
			while (*q == '\t' || *q == ' ') q++;

			if (!strncmp(q, "SSID: ", 6))
			{
				// A BSS can carry the name twice, in the beacon and the probe
				// response. The first one that is usable wins.
				if (!is_printable_ssid(cur.ssid))
					snprintf(cur.ssid, sizeof(cur.ssid), "%s", q + 6);
			}
			else if (!strncmp(q, "signal: ", 8))
			{
				cur.signal = (int)strtod(q + 8, 0);
			}
			else if (!strncmp(q, "RSN:", 4) || !strncmp(q, "WPA:", 4))
			{
				cur.secure = 1;
			}
			else if (!strncmp(q, "capability: ", 12) && strstr(q, "Privacy"))
			{
				// WEP, or WPA whose RSN element we have not reached yet. Either way
				// it wants a password.
				cur.secure = 1;
			}
		}

		if (!eol) break;
		p = eol + 1;
	}

	if (have && is_printable_ssid(cur.ssid) && n < max) out[n++] = cur;

	/*
	  One row per network, not one per radio: a house with a dual-band router shows
	  the same name two or three times otherwise. Strongest of each wins, and the
	  associated one keeps its mark.
	*/
	for (int i = 0; i < n; i++)
	{
		for (int j = i + 1; j < n; j++)
		{
			if (strcmp(out[i].ssid, out[j].ssid)) continue;

			if (out[j].signal > out[i].signal) out[i].signal = out[j].signal;
			if (out[j].current) out[i].current = 1;
			if (out[j].secure) out[i].secure = 1;

			memmove(&out[j], &out[j + 1], sizeof(net_ap) * (size_t)(n - j - 1));
			n--;
			j--;
		}
	}

	// Strongest first, with whatever we are on now at the top: the two things a
	// person is looking for are "mine" and "the one that will work".
	for (int i = 0; i < n; i++)
	{
		for (int j = i + 1; j < n; j++)
		{
			int better = 0;
			if (out[j].current && !out[i].current) better = 1;
			else if (out[j].current == out[i].current && out[j].signal > out[i].signal) better = 1;

			if (better)
			{
				net_ap t = out[i];
				out[i] = out[j];
				out[j] = t;
			}
		}
	}

	return n;
}

int net_parse_link(const char *text, net_link *out)
{
	memset(out, 0, sizeof(*out));
	if (!text) return 0;

	// "Not connected." is what iw says when there is no association.
	if (strstr(text, "Not connected")) return 0;
	if (!strstr(text, "Connected to")) return 0;

	out->up = 1;
	out->signal = -100;

	const char *p = text;
	while (*p)
	{
		const char *eol = strchr(p, '\n');
		size_t len = eol ? (size_t)(eol - p) : strlen(p);

		char line[512];
		if (len >= sizeof(line)) len = sizeof(line) - 1;
		memcpy(line, p, len);
		line[len] = 0;
		trim_end(line);

		const char *q = line;
		while (*q == '\t' || *q == ' ') q++;

		if (!strncmp(q, "SSID: ", 6)) snprintf(out->ssid, sizeof(out->ssid), "%s", q + 6);
		else if (!strncmp(q, "signal: ", 8)) out->signal = (int)strtod(q + 8, 0);

		if (!eol) break;
		p = eol + 1;
	}

	return 1;
}

int net_conf_country(const char *text, char *out, size_t len)
{
	out[0] = 0;
	if (!text) return 0;

	const char *p = text;
	while (*p)
	{
		if ((p == text || p[-1] == '\n') && !strncmp(p, "country=", 8))
		{
			const char *v = p + 8;
			size_t i = 0;
			while (v[i] && v[i] != '\n' && v[i] != '\r' && i < len - 1) { out[i] = v[i]; i++; }
			out[i] = 0;
			trim_end(out);
			return out[0] ? 1 : 0;
		}
		p++;
	}
	return 0;
}

// wpa_supplicant's quoted strings take a backslash escape; anything else in there
// would end the value early and change which network we joined.
static int quote_into(char *dst, size_t len, const char *src)
{
	size_t o = 0;
	for (const char *q = src; *q; q++)
	{
		if ((unsigned char)*q < 0x20) return 0;

		if (*q == '"' || *q == '\\')
		{
			if (o + 2 >= len) return 0;
			dst[o++] = '\\';
		}
		else if (o + 1 >= len) return 0;

		dst[o++] = *q;
	}
	dst[o] = 0;
	return 1;
}

int net_conf_build(char *buf, size_t len, const char *country,
	const char *ssid, const char *psk, int secure)
{
	if (!ssid || !*ssid) return -1;

	char qs[128], qp[192];
	if (!quote_into(qs, sizeof(qs), ssid)) return -1;

	if (secure)
	{
		size_t pl = psk ? strlen(psk) : 0;
		// WPA's own limits. Saying so first is much kinder than a rollback three
		// quarters of a minute later.
		if (pl < 8 || pl > 63) return -1;
		if (!quote_into(qp, sizeof(qp), psk)) return -1;
	}

	// country is the two-letter code, the way net_conf_country() hands it back - the
	// line around it belongs here, with the rest of the file's syntax.
	char cl[48] = "";
	if (country && *country)
	{
		char qc[32];
		if (!quote_into(qc, sizeof(qc), country)) return -1;
		snprintf(cl, sizeof(cl), "country=%s\n", qc);
	}

	int n;
	if (secure)
	{
		n = snprintf(buf, len,
			"%snetwork={\n\tssid=\"%s\"\n\tpsk=\"%s\"\n}\n", cl, qs, qp);
	}
	else
	{
		n = snprintf(buf, len,
			"%snetwork={\n\tssid=\"%s\"\n\tkey_mgmt=NONE\n}\n", cl, qs);
	}

	if (n < 0 || (size_t)n >= len) return -1;
	return n;
}

/* ------------------------------------------------------------------ status --- */

// The address, straight from the kernel: no process to spawn, so this can be read
// as often as we like.
static void read_ip(net_link *l)
{
	l->ip[0] = 0;

	struct ifaddrs *ifa = 0;
	if (getifaddrs(&ifa)) return;

	for (struct ifaddrs *i = ifa; i; i = i->ifa_next)
	{
		if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET) continue;
		if (!i->ifa_name || strcmp(i->ifa_name, net_iface())) continue;

		struct sockaddr_in *sa = (struct sockaddr_in*)i->ifa_addr;
		inet_ntop(AF_INET, &sa->sin_addr, l->ip, sizeof(l->ip));
		break;
	}
	freeifaddrs(ifa);
}

const net_link *net_link_now()
{
	return &link_now;
}

void net_ingest_scan(const char *text)
{
	nap = net_parse_scan(text, aps, NET_MAX);
	scanning = 0;
}

void net_ingest_link(const char *text)
{
	net_link l;
	net_parse_link(text, &l);
	read_ip(&l);
	link_now = l;
}

/* ------------------------------------------------------------------- scan --- */

/*
  Trigger, wait, read the table - not `iw scan`, which does all three and blocks
  until the driver feels like answering. On this hardware, associated, that call sat
  there for a minute with nothing to show, which on screen is a front-end stuck on
  "looking for networks" forever.

  `scan trigger` returns as soon as the driver has accepted the request (or refuses,
  if one is already running, which is just as good for us), and `scan dump` reads the
  table the kernel keeps. Bounded by construction.
*/
static pid_t spawn_scan()
{
	pid_t p = fork();
	if (p) return p;

	const char *tr[] = { "iw", "dev", net_iface(), "scan", "trigger", 0 };
	const char *dp[] = { "iw", "dev", net_iface(), "scan", "dump", 0 };

	run_quiet(tr);
	sleep(4);                       // long enough for a pass over the 2.4 GHz channels
	run_to(dp, SCAN_OUT);
	_exit(0);
}

void net_scan_start()
{
	if (!net_present() || child > 0) return;

	scanning = 1;
	child = spawn_scan();
	kind = K_SCAN;
	child_kill = GetTimer(20000);
	if (child < 0) { child = -1; scanning = 0; kind = K_NONE; }
}

int net_scanning() { return scanning; }
int net_count() { return nap; }

void net_force_scanning(int on) { scanning = on ? 1 : 0; }

void net_force_join(int state, const char *detail)
{
	join_state = state;
	snprintf(join_detail, sizeof(join_detail), "%s", detail ? detail : "");

	// And the phase with it, exactly as net_join() does: the phase only moves forward,
	// so a fresh join that inherited the last one's would open nearly complete.
	join_phase = 0;
}

const net_ap *net_at(int i)
{
	if (i < 0 || i >= nap) return 0;
	return &aps[i];
}

/* ------------------------------------------------------------------- join --- */

static char join_ssid[NET_SSID];
static char join_psk[80];
static int join_secure = 0;

/*
  Called by the child only, after each step it finishes. Written whole and short so a
  parent that reads it mid-write gets either the old digit or the new one.
*/
static void join_report(int phase)
{
	FILE *f = fopen(JOIN_OUT, "wb");
	if (!f) return;
	fprintf(f, "%d\n", phase);
	fclose(f);
}

void net_ingest_join_phase(const char *text)
{
	if (!text) return;

	while (*text == ' ' || *text == '\t' || *text == '\n') text++;
	if (*text < '0' || *text > '9') return;

	int v = *text - '0';
	if (v == JOIN_ROLLBACK) { join_phase = JOIN_ROLLBACK; return; }
	if (v < 0 || v > JOIN_STEPS) return;

	// Forward only. The child writes in order, but a stale file from the previous
	// join is exactly what a fresh one would read on its first frame.
	if (join_phase != JOIN_ROLLBACK && v > join_phase) join_phase = v;
}

int net_join_phase() { return join_phase; }

const char *net_join_phase_name(int phase)
{
	// What is being waited for, in the words of somebody standing in the room, not
	// the names of the commands. "ifup" means nothing to the audience for this menu.
	switch (phase)
	{
	case 0: return "Saving it";
	case 1: return "Restarting Wi-Fi";
	case 2: return "Finding the network";
	case 3: return "Getting an address";
	case JOIN_ROLLBACK: return "Putting your old network back";
	}
	return "Connected";
}

/*
  The child half of a join. It may block for as long as it likes.

  Order matters: the old file is copied aside *before* anything is written, and put
  back by this same process if the new one does not come up. Nothing here is left to
  the UI to finish, because the UI might be a core switch away from not existing.
*/
static void join_child()
{
	char *old = slurp(CONF);

	if (old)
	{
		FILE *f = fopen(BAK, "wb");
		if (f) { fwrite(old, 1, strlen(old), f); fclose(f); }
	}

	char country[32] = "";
	if (old) net_conf_country(old, country, sizeof(country));

	char conf[1024];
	int n = net_conf_build(conf, sizeof(conf), country, join_ssid, join_psk, join_secure);
	if (n < 0) _exit(1);

	FILE *f = fopen(CONF, "wb");
	if (!f) _exit(1);
	fwrite(conf, 1, (size_t)n, f);
	fclose(f);
	sync();
	join_report(1);

	const char *dn[] = { "ifdown", net_iface(), 0 };
	const char *up[] = { "ifup", net_iface(), 0 };

	run_quiet(dn);
	run_quiet(up);
	join_report(2);

	/*
	  Associated is not enough: a wrong password associates and then falls off, and
	  no address means no network as far as anybody using it is concerned. So wait
	  for both, and keep waiting a little after association for DHCP.
	*/
	int ok = 0;
	for (int i = 0; i < 30 && !ok; i++)
	{
		sleep(1);

		const char *lk[] = { "iw", "dev", net_iface(), "link", 0 };
		run_to(lk, LINK_OUT);

		char *t = slurp(LINK_OUT);
		if (!t) continue;

		net_link l;
		int up_now = net_parse_link(t, &l);
		free(t);

		if (!up_now) continue;
		if (strcmp(l.ssid, join_ssid)) continue;   // associated, but not to this one

		// Associated. Reported here rather than after the loop because this is where
		// the wait changes character: the password was right and DHCP is the hold-up.
		join_report(3);

		read_ip(&l);
		if (l.ip[0]) ok = 1;
	}

	if (ok)
	{
		join_report(JOIN_STEPS);
		free(old);
		_exit(0);
	}

	join_report(JOIN_ROLLBACK);

	// Put it back exactly as it was.
	if (old)
	{
		f = fopen(CONF, "wb");
		if (f) { fwrite(old, 1, strlen(old), f); fclose(f); }
		free(old);
		sync();
	}
	else remove(CONF);

	run_quiet(dn);
	run_quiet(up);

	for (int i = 0; i < 25; i++)
	{
		sleep(1);

		net_link l;
		memset(&l, 0, sizeof(l));
		read_ip(&l);
		if (l.ip[0]) _exit(1);          // failed, and back where we started
	}

	_exit(2);                           // failed, and the old settings did not come back
}

void net_join(const char *ssid, const char *psk, int secure)
{
	if (!net_present() || child > 0) return;

	snprintf(join_ssid, sizeof(join_ssid), "%s", ssid ? ssid : "");
	snprintf(join_psk, sizeof(join_psk), "%s", psk ? psk : "");
	join_secure = secure ? 1 : 0;

	// Refuse what cannot work before taking the interface down for it.
	char probe[1024];
	if (net_conf_build(probe, sizeof(probe), "", join_ssid, join_psk, join_secure) < 0)
	{
		join_state = JOIN_FAIL;
		snprintf(join_detail, sizeof(join_detail),
			join_secure ? "A Wi-Fi password is 8 to 63 characters" : "That network name cannot be used");
		return;
	}

	join_state = JOIN_WORK;
	snprintf(join_detail, sizeof(join_detail), "%s", join_ssid);

	/*
	  Before the fork, and the stale file with it: the phase only ever moves forward,
	  so a leftover "3" from the last attempt would show this one as nearly done from
	  its first frame.
	*/
	join_phase = 0;
	remove(JOIN_OUT);

	pid_t p = fork();
	if (!p) join_child();

	if (p < 0)
	{
		join_state = JOIN_FAIL;
		snprintf(join_detail, sizeof(join_detail), "Could not start");
		return;
	}

	child = p;
	kind = K_JOIN;
	/*
	  No deadline on a join. It is the one child that must be allowed to finish: it
	  is holding the only copy of the old configuration and is the thing that puts it
	  back. Killing it half way is how a machine ends up off the network for good. Its
	  own waits bound it to about a minute.
	*/
	child_kill = 0;
	printf("ClassicUI: joining %s\n", join_ssid);
}

int net_join_state() { return join_state; }
const char *net_join_detail() { return join_detail; }

void net_join_ack()
{
	if (join_state == JOIN_OK || join_state == JOIN_FAIL || join_state == JOIN_LOST)
		join_state = JOIN_IDLE;
}

/* ------------------------------------------------------------------- poll --- */

void net_watch(int on)
{
	watching = on ? 1 : 0;
	if (on) link_due = 0;
}

static void link_refresh_start()
{
	pid_t p = fork();
	if (p < 0) return;

	if (!p)
	{
		const char *lk[] = { "iw", "dev", net_iface(), "link", 0 };
		run_to(lk, LINK_OUT);
		_exit(0);
	}

	child = p;
	kind = K_LINK;
	child_kill = GetTimer(10000);
}

void net_poll()
{
	if (!net_present()) return;

	/*
	  Give up on a child that is taking too long - but only on the read-only ones. A
	  driver that will not answer a scan should not leave the screen waiting for it.
	*/
	if (child > 0 && child_kill && CheckTimer(child_kill))
	{
		printf("ClassicUI: %s took too long, dropping it\n", kind == K_SCAN ? "scan" : "link read");
		kill(child, SIGKILL);
		child_kill = 0;
	}

	/*
	  A join reports where it has got to as it goes, and unlike everything else here
	  that means reading from a child that is still running. Once a frame is plenty:
	  the steps are seconds apart.
	*/
	if (child > 0 && kind == K_JOIN)
	{
		char *t = slurp(JOIN_OUT);
		if (t) { net_ingest_join_phase(t); free(t); }
	}

	if (child > 0)
	{
		int st = 0;
		pid_t r = waitpid(child, &st, WNOHANG);
		if (r == child || (r < 0 && errno == ECHILD))
		{
			/*
			  Nothing else in the firmware reaps children, so ECHILD should not
			  happen - but if it ever does the exit status is not ours to read, and
			  claiming a join worked on the strength of an uninitialised status is
			  the one outcome worth ruling out. Say nothing and let the status line,
			  which is refreshed from the interface itself, tell the truth.
			*/
			int lost = (r != child);
			int code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
			int was = kind;

			child = -1;
			kind = K_NONE;
			child_kill = 0;

			if (was == K_SCAN)
			{
				char *t = slurp(SCAN_OUT);
				if (t) { net_ingest_scan(t); free(t); }
				scanning = 0;
				printf("ClassicUI: %d networks\n", nap);
			}
			else if (was == K_LINK)
			{
				char *t = slurp(LINK_OUT);
				if (t) { net_ingest_link(t); free(t); }
			}
			else if (was == K_JOIN)
			{
				if (lost)
				{
					join_state = JOIN_IDLE;
					link_due = 0;
					return;
				}

				join_state = (code == 0) ? JOIN_OK : (code == 2) ? JOIN_LOST : JOIN_FAIL;

				// The last report the child managed to write may have been lost to the
				// race with its own exit; a join that returned 0 is finished whatever
				// the file says. A failed one keeps whatever step it reached.
				if (join_state == JOIN_OK) join_phase = JOIN_STEPS;

				if (join_state == JOIN_OK)
					snprintf(join_detail, sizeof(join_detail), "%s", join_ssid);
				else if (join_state == JOIN_LOST)
					snprintf(join_detail, sizeof(join_detail), "The old settings did not come back");
				else
					snprintf(join_detail, sizeof(join_detail), "Check the password and try again");

				printf("ClassicUI: join %s (%d)\n", join_state == JOIN_OK ? "ok" : "failed", code);
				link_due = 0;
			}
		}
	}

	if (!watching || child > 0) return;

	if (!link_due || CheckTimer(link_due))
	{
		link_due = GetTimer(3000);
		link_refresh_start();
	}
}
