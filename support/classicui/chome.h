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

/*
  Which screen is up, as one of the SCR_* values in chome_ui.cpp.

  Exported for diagnostics and for the harness: navigation between animated screens
  cannot be tested by hashing pixels, because two rotation phases of the same screen
  differ and two different screens may not. Asserting the screen id is the only way to
  test an ordering like "up reaches the disc, then the menu bar".
*/
int chome_screen_id();

/*
  1 when nothing needs the poll loop to be prompt: the shelf, or the in-game menu over a
  game that is genuinely held still. See the comment on the definition for the case this
  deliberately excludes.
*/
int chome_core_idle();

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

/*
  The in-game pause menu. Opens on the OSD/menu button inside a game core, drawn
  over a still capture of the running game.

  It is not a true overlay: MiSTer's HPS framebuffer replaces the core's video
  rather than blending with it, so the "background" is a frame grabbed the moment
  the menu opened. The game keeps running behind it, exactly as it does behind the
  classic OSD. Cores without framebuffer support fall through to that OSD instead.
*/
int chome_ingame_active();

/*
  Opens the on-screen keyboard over whatever is on screen. The screens inside the
  front-end that need text call this; it is public because the harness drives it the
  same way, and because text entry is a front-end-wide facility rather than the
  property of one screen.

  mask marks the field as a password: see chome_osk.h for what that does and does
  not do.
*/
void chome_text_entry(const char *title, const char *prompt, const char *initial, int mask);

/*
  The suspend point's savestate slot, which is the front-end's plumbing and not a save
  the player made: it is how a game is held still while this menu is up, because the
  core cannot really be paused then.

  The firmware asks about it so it can keep it out of sight. chome_hidden_slot() gives
  the 0-based slot, or -1 when nothing is reserved, and chome_ss_quiet() is true for a
  moment around a write to it - long enough to cover the polls on which the core's own
  "Save to state N" message and the state's thumbnail would otherwise arrive.
*/
int chome_hidden_slot();
int chome_ss_quiet();

#endif
