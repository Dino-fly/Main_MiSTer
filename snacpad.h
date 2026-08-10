#ifndef SNACPAD_H
#define SNACPAD_H

// Framework PSX SNAC pad support: polls pad state gathered by the
// psx_snac_pad reader in the core's sys framework (UIO_SNAC_PAD) and
// exposes each connected pad as a virtual gamepad through uinput, so it
// behaves like any other input device (menu navigation, per-core mapping).

void snacpad_init();
void snacpad_poll();

/*
  The two facts a front-end needs to tell a player their SNAC pad cannot work here.

  Neither is a diagnostic hook: both are recorded by snacpad_poll() as it runs and neither
  changes what it does. They exist because the failure this reports has, until now, only
  ever been a line in the log ("no SNAC pad reader in this core") - and a player sitting in
  front of a television will never see a log. Their pad simply does nothing.

  snacpad_reader() is the probe's answer about the *core that is running now*, which is the
  only core it can be about: loading a core re-execs the firmware and snacpad_init() puts
  this back to SNAC_UNPROBED, so the value always describes whatever is on the FPGA at the
  moment it is read. That also means the menu core and a game core answer separately, which
  is correct - they are built from different sys trees and either one can be the old one.

    SNAC_UNPROBED  the poll has not looked yet, or has no reason to. NOT the same as "no
                   reader", and a caller that treats it as one will announce a fault during
                   the first milliseconds of every core's life and whenever the port
                   belongs to somebody else. Say nothing.
    SNAC_NO_READER looked, and this core's sys has no psx_snac_pad reader in it.
    SNAC_READER    looked, and it is there.

  snacpad_wanted() is whether this reader is the one that should be driving the port at
  all - cfg.snac_pad on, cfg.snac_device saying a PlayStation adapter is what is plugged
  in, and the running core not having claimed the bus with its own option. It is the same
  three-term test snacpad_poll() applies, published rather than recomputed, because the
  third term (core_owns_snac()) is not answerable from outside this file.

  A caller wanting "the player's SNAC pad is dead and this core is why" must ask both:
  snacpad_wanted() && snacpad_reader() == SNAC_NO_READER. Reader alone is not enough. An
  old PSX core carrying its own native SNAC support can hold no framework reader *and*
  work perfectly, because the core is reading the port itself - announcing a fault there
  would be telling somebody their working pad is broken.
*/
#define SNAC_UNPROBED  (-1)
#define SNAC_NO_READER 0
#define SNAC_READER    1

int snacpad_reader();
int snacpad_wanted();

#ifdef CHOME_HOST_TEST
// See the note at the foot of snacpad.cpp. Recorded as the poll runs; changes nothing.
int snacpad_test_enabled();
int snacpad_test_present(int idx);
#endif

#endif
