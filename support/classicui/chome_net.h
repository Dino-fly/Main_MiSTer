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

#endif
