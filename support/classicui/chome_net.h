/*
  Classic Home - wireless networking.

  Everything here is about one job: getting a console onto a home network without a
  text editor and an SD card reader, which is how MiSTer's Wi-Fi is set up today
  (hand-write /media/fat/linux/wpa_supplicant.conf and reboot).

  Two rules shape the implementation.

  The front-end runs inside HandleUI at frame rate, so nothing here may block: a
  scan takes seconds and bringing an interface up takes tens of seconds. Both run in
  a forked child that the UI polls.

  Joining a network can take the machine off the network - including the network the
  person setting it up is using to reach it. So a join keeps the old configuration,
  proves the new one actually associated and got an address, and puts the old one
  back if it did not. "Wrong password" has to end with the machine where it started,
  not offline.
*/

#ifndef CHOME_NET_H
#define CHOME_NET_H

#include <stddef.h>

#define NET_MAX 24
#define NET_SSID 33

struct net_ap
{
	char ssid[NET_SSID];
	int signal;                     // dBm, as reported
	int secure;
	int current;                    // the one we are associated with
};

struct net_link
{
	int up;                         // associated
	char ssid[NET_SSID];
	char ip[24];                    // empty until DHCP has finished
	int signal;
};

// 1 when this machine has a wireless interface at all.
int net_present();
const char *net_iface();
void net_set_sysdir(const char *d);   // tests only - see find_iface()

/*
  Called once a frame while a network screen is open. Reaps finished children and
  keeps the link status fresh; does nothing at all when nothing is watching.
*/
void net_watch(int on);
void net_poll();

const net_link *net_link_now();

void net_scan_start();
int  net_scanning();
int  net_count();
const net_ap *net_at(int i);

#define JOIN_IDLE 0
#define JOIN_WORK 1
#define JOIN_OK   2
#define JOIN_FAIL 3
#define JOIN_LOST 4                 // it failed and the old settings would not come back

void net_join(const char *ssid, const char *psk, int secure);
int  net_join_state();
const char *net_join_detail();
void net_join_ack();

/*
  How far a running join has got, 0..JOIN_STEPS, so the screen can show progress
  rather than a word that does not change for a minute.

  It is the child's own report, not a timer: the join happens in a forked process that
  may block for as long as it likes, and it writes the step it has reached to a file
  after each one. A stalled join therefore stops advancing, which is exactly what the
  player needs to be able to see.

  JOIN_ROLLBACK is past the end on purpose. Putting the old network back is not step
  five of getting onto the new one - it is what happens instead, and the screen says
  so rather than filling the track as though something had succeeded.
*/
#define JOIN_STEPS    4             // saving -> restarting -> associating -> address
#define JOIN_ROLLBACK 9

int net_join_phase();
const char *net_join_phase_name(int phase);

/*
  Makes `text` the phase, as read from the child's file. Exposed for the same reason
  net_ingest_scan() is: it is the only part of a join a machine with no radio can
  drive.
*/
void net_ingest_join_phase(const char *text);

/* ----------------------------------------------------------------------------
  Pure parts, exposed because they are the parts a harness without a radio can
  actually check: real `iw` output goes in, the table comes out.
*/

/*
  Takes the output of `iw dev X scan` or `iw dev X link` and makes it the current
  state. net_poll() calls these when the child finishes; they are declared here
  because that is also how a harness with no radio supplies its own.
*/
void net_ingest_scan(const char *text);
void net_ingest_link(const char *text);

int net_parse_scan(const char *text, net_ap *out, int max);
int net_parse_link(const char *text, net_link *out);

/*
  Builds the wpa_supplicant.conf a join would write. Returns its length, or -1 when
  the network cannot be expressed - an empty name, or a password of a length WPA
  does not allow, which is worth saying before taking the machine off the air.
*/
int net_conf_build(char *buf, size_t len, const char *country,
	const char *ssid, const char *psk, int secure);

// Reads country=XX out of an existing config, so rewriting the file does not throw
// away the setting that decides which channels are legal here.
int net_conf_country(const char *text, char *out, size_t len);

/*
  Three seams for a machine with no radio, the same bargain bt_force_present() makes.

  net_present() stats a directory under /sys, and net_scanning() and net_join_state()
  are set by children running `iw` and `ifup`. None of the three can be produced in a
  container, and they are the entire input to the parts of the Wi-Fi screen where the
  player is *waiting* - which is the half of that screen worth checking, since it is
  the half with an animation in it that has to stop when the work does.

  What is faked is only the answer, not the screen: the screen reads exactly these
  functions and nothing else, the same way it reads the AP list net_ingest_scan()
  supplies. Pass -1 to net_force_present() to get the real answer back.
*/
void net_force_present(int on);
void net_force_scanning(int on);

// Puts a join into `state` without a child, and resets the phase the way net_join()
// does, so a forced join starts from nothing behind it like a real one.
void net_force_join(int state, const char *detail);

#endif
