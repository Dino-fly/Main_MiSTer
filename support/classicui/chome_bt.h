/*
  Classic Home - pairing a wireless controller.

  The job is the one thing a console has to be able to do without a keyboard: get a
  controller connected. MiSTer can already do it, but only through the classic OSD's
  Bluetooth pairing entry, which drops the player into a script's console output.

  Three constraints shape this, the first two the same as chome_net's.

  The front-end runs inside HandleUI at frame rate, so nothing here may block. A
  discovery pass takes tens of seconds and pairing can take a minute, so the work runs
  in a forked child that the UI polls.

  Pairing is not a request-response: it is a conversation that reports progress. So
  unlike the network children, the pairing child is long-lived and its output is read
  *while it runs* - the parent re-reads the tail of its log each frame and feeds the
  lines to a state machine.

  And it delegates rather than reimplementing. /usr/sbin/btctl is MiSTer's own D-Bus
  agent: `btctl pair` registers a pairing agent, starts discovery, and pairs, trusts
  and connects any *input* device that appears, looping so several pads can be done in
  one go. Reimplementing that against org.bluez in C++ would be a lot of code to
  arrive at the same behaviour, and it would drift from what the rest of MiSTer does.
  It also already filters to input devices, so a phone or a pair of headphones in
  pairing mode nearby is skipped rather than adopted.
*/

#ifndef CHOME_BT_H
#define CHOME_BT_H

#include <inttypes.h>

#define BT_MAX  12
#define BT_NAME 48

struct bt_dev
{
	char mac[18];
	char name[BT_NAME];
	int  connected;
};

// 1 when this machine has a Bluetooth adapter at all. Cheap: no fork, no D-Bus.
int bt_present();

/*
  A name a player will recognise, from the pad's USB ids rather than from what it calls
  itself: a DualShock 4 broadcasts "Wireless Controller", which is true of nearly
  everything in the room and tells nobody which of their pads this is. Falls back to the
  reported name for anything not in the table, which is most things and is fine - a
  Wii U Pro Controller already says what it is.

  Keyed on ids because they are the same over USB and over the air, so a pad reads the
  same however it is plugged in.
*/
const char *bt_pad_label(uint16_t vid, uint16_t pid, const char *reported);

/*
  Called once a frame while a controller screen is open. Reaps finished children,
  keeps the paired list fresh and advances the pairing conversation; does nothing at
  all when nothing is watching.
*/
void bt_watch(int on);
void bt_poll();

// The paired controllers, as of the last refresh.
int bt_count();
const bt_dev *bt_at(int i);
void bt_refresh();

/*
  Pairing mode. Discovery and an agent run until it is stopped, so the player can put
  one pad after another into pairing mode without coming back to the menu.
*/
#define BTP_IDLE    0
#define BTP_LOOKING 1               // discovery is up, nothing found yet
#define BTP_WORKING 2               // pairing, trusting or connecting a named device
#define BTP_OK      3               // that one is done; discovery continues
#define BTP_FAIL    4               // that one did not pair; discovery continues
#define BTP_PIN     5               // the pad wants a PIN typed, which we cannot do

void bt_pair_start();
void bt_pair_stop();
int  bt_pairing();                  // pairing mode is on
int  bt_pair_state();
int  bt_pair_done();                // how many have paired since it started

/*
  How far along the current controller is, 0..BTP_STEPS, for the progress track on the
  pairing screen. The five states above say what kind of thing is happening; this says
  how much of it is behind us, which is the question somebody holding two buttons down
  is actually asking.

  It is a reading of the same btctl lines the state machine already parses, not a
  timer: a pairing that stalls stops advancing, which is the whole point of showing it.
  A failure leaves the step where it got to, so the screen can say how far it got.
*/
#define BTP_STEPS 4                 // looking -> found -> pairing -> connecting -> ready
int  bt_pair_step();
const char *bt_pair_step_name(int step);

/*
  The controller the conversation is about, and one line of plain language about it.
  Both are empty until something is found.
*/
const char *bt_pair_name();
const char *bt_pair_detail();

/*
  Clears a finished result back to idle, so the screen goes back to showing the list.
  The same shape as net_join_ack(): a result stays on screen until it is acknowledged,
  because it is the answer to something the player did.
*/
void bt_pair_ack();

/*
  Brings a paired controller's link up. Needed because a pad cannot be relied on to do
  it: one that is also registered to a console reconnects there when its own button is
  pressed, so the adapter has to ask.
*/
void bt_connect(const char *mac);

// Undoes a pairing. Destructive; the UI confirms first.
void bt_forget(const char *mac);

/* ----------------------------------------------------------------------------
  Pure parts, exposed because they are what a harness with no radio can check:
  real btctl and bluetoothctl output goes in, the state comes out.
*/

/*
  Parses the paired list the refresh child writes. Two sections: bluetoothctl's own
  `paired-devices` lines, then a per-device `== <mac>` marker followed by that
  device's `Connected:` line. Both come from bluetoothctl verbatim.
*/
int bt_parse_paired(const char *text, bt_dev *out, int max);

/*
  Makes that text the current list. bt_poll() calls this when the refresh child
  finishes; it is declared here because it is also how a harness with no adapter
  supplies its own, the same as net_ingest_scan().
*/
void bt_ingest_paired(const char *text);

/*
  Feeds one line of `btctl pair` output to the state machine and returns the state it
  leaves it in. Exposed so the conversation can be driven from a transcript.
*/
int bt_ingest_progress(const char *line);
void bt_progress_reset();

/*
  Answers bt_present() with `on` instead of asking sysfs; -1 gives the real answer back.

  The one seam here that is not a parser. bt_present() stats a directory under /sys,
  which a container cannot conjure up, and the parts of the Controllers screen that only
  exist when there is a radio - the entry that adds a controller, chief among them - are
  otherwise unreachable from a harness.
*/
void bt_force_present(int on);

#endif
