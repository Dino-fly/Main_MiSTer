/*
  Classic Home - the MiSTer.ini options the player is allowed to edit.

  cfg.cpp's ini_var[] already carries every option's name, type and range. What it does
  not carry is anything a person could act on: there are no labels, no defaults (those
  are assignments at the top of cfg_parse(), not data), no grouping, and no opinion
  about which options are safe to hand to somebody who has never heard of a scaler. So
  that metadata lives here, in a table of its own, and the two are kept in step by hand.
  A `def` that drifts from cfg_parse() makes the colour coding lie, which is the one
  thing this screen is for - the harness checks the pairs it can.

  Three rules decided what is in the set:

  1. Only what a player can see and judge. If knowing whether to change it requires
     knowing what a scaler, a sync signal or a colour space is, it is not here. That
     is the same rule that keeps the rest of the front-end free of technical panels.

  2. Nothing that can leave the machine with no picture and no way back. vga_scaler,
     direct_video, vga_mode, forced_scandoubler, video_mode, fb_terminal, bootcore and
     main all decide whether anything reaches a screen at all, or which program runs;
     a wrong value there is a black set and a card that has to come out and go into a
     PC. The audience for this front-end cannot ssh in to undo it. gamepad_defaults is
     excluded for the same reason in miniature: it silently moves every button in every
     core, so the pad the player is holding stops matching what is on screen.

  3. Every value is picked from a list or stepped within the range cfg.cpp declares, so
     nothing this screen writes can be rejected by the parser. An out-of-range value
     raises cfg_error(), and user_io.cpp shows those as an Info() panel over the game
     for five seconds on the next core load - a classic-OSD element, which is precisely
     what the front-end exists to prevent. Clamping here is not tidiness; it is what
     stops this screen from creating the thing it is meant to avoid.

  Should an option ever be added whose worst case is a machine the player cannot
  recover, it needs to say so on screen before it is set, the way closing a game and
  writing the ini already do. Nothing in the table today needs that.
*/

#ifndef CHOME_OPT_H
#define CHOME_OPT_H

#include <inttypes.h>
#include <limits.h>              // for OPT_NO_REC

#include "chome_ini.h"           // for ini_set: opt_apply() carries the screen's own keys

#define OPT_MAX 24               // the table, and the view built from it

// What kind of thing the value is, which is also how left and right behave on it.
#define OPT_LIST   0             // one of a short list of named values
#define OPT_NUMBER 1             // a number in a range, moved a step at a time

// Groups. Only for ordering and for the caption over the list - the list itself is
// flat, because a novice scrolling one list beats a novice choosing a category first.
#define OG_PICTURE 0
#define OG_PADS    1
#define OG_MENU    2

// When a written value starts being true.
#define OW_NOW   0               // this session is already living under it
#define OW_GAME  1               // the next core load, which re-reads the whole ini

// A row with no opinion about its own value. See `rec` in opt_def below.
#define OPT_NO_REC INT_MIN

struct opt_choice
{
	int val;
	const char *label;
};

struct opt_def
{
	const char *key;             // spelled as MiSTer.ini spells it
	const char *label;           // what the row says
	const char *help;            // one sentence, under the list, for the selected row
	uint8_t group;
	uint8_t kind;

	/*
	  Only meaningful when the scaler's output is what reaches the screen. The Display
	  entry in the menu bar disappears on the same test and for the same reason: filters,
	  colour and picture size all live in the scaler, and on direct_video or an analog-only
	  set they change nothing at all. Offering a player a Brightness that does nothing is
	  worse than not offering one.
	*/
	uint8_t scaler_only;

	int lo, hi;                  // the range cfg.cpp declares. Values are clamped to it
	int step;                    // OPT_NUMBER: what one press of left or right moves
	const char *unit;            // OPT_NUMBER: drawn after the number, or 0

	int def;                     // what the firmware uses when the key is absent

	/*
	  ...and what is right for this front-end. Usually def, and OPT_NO_REC when the
	  honest answer is "that is not for us to say".

	  Every other row on this screen is a preference, so "away from what we recommend"
	  is a useful thing to colour amber. snac_device is not a preference: it says what
	  is physically plugged into a port, and a player who told us they have an adapter
	  for another console gave the *correct* answer. Telling that person they are away
	  from the recommendation - "Usually PlayStation" - would be the screen arguing with
	  a fact about their own desk, and the one thing worse than no advice is advice that
	  is wrong for the person reading it.

	  A sentinel rather than -1, because -1 is a value a player can legitimately set:
	  classicui_tracking's range is -2..+2. INT_MIN is outside every range cfg.cpp
	  declares and will stay outside any that gets added.
	*/
	int rec;

	const opt_choice *choices;   // OPT_LIST
	uint8_t nchoices;

	/*
	  The cfg field to update as well, so this session agrees with the file it just
	  wrote. uint8_t only - every option in the set is one, and a wider cfg field needs
	  this widened rather than silently scribbling on the field after it.
	*/
	uint8_t *live;

	/*
	  ...and the same thing for a signed field, which is what widening it looks like.

	  classicui_tracking is an int8_t because -2 is a value a player can set, and pointing
	  `live` at it through a cast is precisely what the paragraph above rules out: -1 would
	  be written as 0xFF through a uint8_t* and read back correctly only because this
	  target happens to be two's complement with 8-bit chars. One extra pointer and one
	  extra branch in opt_apply() costs nothing and says what it means. Exactly one of the
	  two is set on any row.
	*/
	int8_t *live_s;
	uint8_t when;                // OW_NOW or OW_GAME, for what the screen says after writing
};

int opt_count();
const opt_def *opt_at(int i);
const char *opt_group_name(int g);

/*
  The options that apply to this machine, in table order. Anything scaler_only is left
  out when the scaler is not what reaches the screen. Returns how many were written.
*/
int opt_view(int *out, int max, int scaler_visible);

// Read the file into the model. Everything below reads from that snapshot, so the
// screen cannot change under the player while they are on it.
void opt_load(const char *path);

int opt_value(int i);            // what it is set to now, edits included
int opt_present(int i);          // the key was in the file at all
int opt_is_rec(int i);           // the value is the recommended one - what the colour says

// Clamped to [lo, hi] whatever is asked for. Returns the value that was actually set.
int opt_set(int i, int v);

/*
  One press of left or right. A list wraps, because a short ring of names has no ends
  worth defending; a number stops at its range, because running into the end of a
  number is information. Returns 0 when nothing moved, so the caller can nudge.
*/
int opt_step_by(int i, int dir);

// Back to the recommended value. 0 when it was already there.
int opt_reset(int i);
int opt_has_rec(int i);          // 0 on the row whose rec is OPT_NO_REC, which refuses X

// How the value reads on the row: a choice's label, or the number and its unit. A
// value no choice claims is shown as itself rather than as the nearest name - the
// player's own file said it, and rounding it off in the display would be a lie.
const char *opt_value_text(int i, char *buf, int max);

// The same, for the recommended value - which is what the screen has to name when it
// says a row is not at it.
const char *opt_rec_text(int i, char *buf, int max);

int opt_dirty();                 // edits not yet written

/*
  Write the edits. Only what changed is written: a settings screen that wrote all of
  its keys would plant a dozen lines in the player's ini for things they never touched,
  and freeze today's defaults into a file that would otherwise follow the firmware.
  Returns how many were written, 0 for nothing to do, -1 with opt_error() set.

  `extra` is for the keys the screen owns that this table cannot hold - today that is
  `font=`, which is a path and not a number in a range. They go through the same call
  rather than through a second ini_apply_set() afterwards for one concrete reason: each
  write takes a backup, so two writes would leave MiSTer.ini.bak holding the file as it
  was halfway through the save instead of as it was before the player touched anything.
  The caller owns telling cfg about them; this knows nothing about what they mean.
*/
int opt_apply(const char *path, const ini_set *extra, int nextra);
const char *opt_error();

// 1 when everything just written is already true for this session.
int opt_wrote_live();

#endif
