/*
  Classic Home - a SNES-Classic-style front-end for the MiSTer menu core.

  Additive and flag-gated (CLASSICUI=1 in MiSTer.ini): the existing OSD menu is
  untouched and remains both the in-game menu and the escape hatch for advanced
  settings. See docs/CLASSIC_UI_PLAN.md.
*/

#ifndef CHOME_H
#define CHOME_H

#include <inttypes.h>

// True when the front-end is configured on and we are in the menu core.
int chome_enabled();

// True while it owns the screen. menu_key_get() also uses this to enable key
// repeat for the shelf.
int chome_active();

// Called from HandleUI() with the decoded key. Returns 1 when the key was
// consumed and the classic menu should not run this frame.
int chome_handle(uint32_t key);

// Hand the screen back to the classic OSD menu.
void chome_leave();

// Called from HandleUI() in every core: applies a video look armed at launch.
void chome_core_boot();

/*
  Called from HandleUI() in every core, every frame. In a game core it grabs one
  reference frame a little after launch, so the Display screen can preview the
  looks over the user's own game instead of a test pattern. Cheap: it does nothing
  at all once the shot exists or has been taken.
*/
void chome_core_poll();

#endif
