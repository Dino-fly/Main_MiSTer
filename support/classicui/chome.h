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
  Which entry the shelf has selected, as an index into the current view.

  Exported for the same reason chome_screen_id() is: a navigation rule cannot be tested by
  hashing pixels. The shoulder buttons jump by first letter, and the property worth checking
  is "it landed on the first entry of the next letter" - a statement about the view, not
  about which card is drawn where. Reading it off the framebuffer would tie the test to the
  card layout and would still not distinguish the first entry of a letter from the second.
*/
int chome_sel_index();

/*
  What Options > Online Covers is in, in one phrase: "Not Available", "Off", "No Account",
  "No Password" or "On". The Options row and the screen itself are both drawn from this, so
  they cannot come to disagree about whether a cover would really be fetched.

  Exported for the same two reasons chome_screen_id() is, and the harness one is the
  sharper of them. `available` is whether this build carries a ScreenScraper application
  credential, and that is decided at compile time - see chome_ss.h. A build that can run
  the front-end at all is a build where ss_available() is a constant, and the test build
  necessarily has a dummy credential, so the state a player on a shipped build would see is
  the one state no test could otherwise reach. Passing it in is what makes all five
  checkable from one binary.
*/
const char *chome_covers_state(int available, int on, const char *user, int has_pass);

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

/*
  The icon a system actually draws, as a key into chome_icons32.h, or 0 when it has none
  and falls back to the folder.

  Usually the system's own id, because the id doubles as the icon key. Not always: three of
  the CD systems have no drawing of their own and borrow the machine they bolt onto, and the
  drawings can only ever be added by tools/icons32.py, which is generated from a licensed
  set. Exported so the harness can ask which systems fall back rather than working it out
  from the same table twice - the version that worked it out separately went on passing
  while a renamed id had already lost its icon on screen.
*/
const char *chome_sysicon_id(const char *sysid);

#ifdef CHOME_HOST_TEST
/*
  Levers into the dialog disc's rotation cache, compiled out of the firmware - the same
  arrangement as chome_rip.h's rip_test hooks and for the same reason: the property the
  harness has to prove is that the cache is invisible in the pixels, and a cache that can
  neither be dropped nor bypassed without moving the clock is one whose absence no test
  could ever compare against its presence.

  disc_test_rot_drop() forgets every cached frame without touching the clock or the
  allocations. disc_test_rot_direct(1) makes every ask a direct full-angle resample, which
  is the path a between-the-kept-angles miss takes; the harness holds its output equal,
  byte for byte, to the quadrant-composed frames the cache serves. disc_test_rot_info()
  reports what the budget allowed at the current diameter, so the memory claim in the
  report is read from the code rather than recomputed beside it. disc_test_shown_step()
  is the step the last rotated frame was built for, 0-255 - the observable the backoff
  checks quantisation against.
*/
void disc_test_rot_drop();
void disc_test_rot_direct(int on);
int  disc_test_rot_info(int *slots, int *stride, long *bytes);
int  disc_test_shown_step();
int  disc_test_rot_renders();

/*
  The disc dialog's two lines of text, straight out of the disc_dlg_get() the drawing
  uses. See the definition in chome_ui.cpp for why this one screen is checked as
  sentences rather than as pixels.
*/
void disc_test_dlg_text(char *title, int tsz, char *sub, int ssz);
#endif

#endif
