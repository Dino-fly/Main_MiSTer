#ifndef SNACPAD_H
#define SNACPAD_H

// Framework PSX SNAC pad support: polls pad state gathered by the
// psx_snac_pad reader in the core's sys framework (UIO_SNAC_PAD) and
// exposes each connected pad as a virtual gamepad through uinput, so it
// behaves like any other input device (menu navigation, per-core mapping).

void snacpad_init();
void snacpad_poll();

#endif
