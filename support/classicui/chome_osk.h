/*
  Classic Home - on-screen keyboard.

  Text entry from the pad, because the machine this runs on usually has no keyboard
  attached and a Wi-Fi password has to come from somewhere. Modal: while it is open
  the front-end routes every key here and draws it over whatever was on screen.

  Navigation is geometric, not index-based - moving up or down picks the key whose
  centre is nearest. The bottom row's keys are wider than the letters above them, so
  index arithmetic across rows of unequal length lands somewhere other than where the
  player is looking; nearest-centre lands where they pointed.
*/

#ifndef CHOME_OSK_H
#define CHOME_OSK_H

#include "chome_theme.h"

#define OSK_MAX 64

/*
  title goes in the panel header, prompt is the line of explanation under it, and
  initial pre-fills the field.

  mask means "this is a password": it adds a HIDE key. It starts out *visible* - a
  password typed one letter at a time on a pad, with no feedback, is the most common
  way this screen fails, and a living room is not a shoulder-surfing threat model.
  Hiding is there for the player who wants it, not imposed on the one who doesn't.
*/
void osk_open(const char *title, const char *prompt, const char *initial, int mask);
void osk_close();

int  osk_active();

// 1 accepted, -1 cancelled, 0 while still open or after the result was taken.
int  osk_result();
void osk_clear_result();
const char *osk_text();

void osk_draw(const chome_profile *p, int pad);

/*
  Feeds one keypress; returns 1 when it was consumed, which is always while open.
  pad distinguishes a pad-mapped key from a real keyboard's: on a keyboard the
  printable keys type themselves instead of driving the cursor.
*/
int  osk_key(int k, int pad);

#endif
