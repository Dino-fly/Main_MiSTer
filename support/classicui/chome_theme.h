/*
  Classic Home - layout profiles and palette.

  Metrics are derived proportionally from the canvas so that any framebuffer size
  works, not just the three nominal ones. The canvas is output resolution divided
  by cfg.fb_size (video.cpp:3481), so a 1080p output with fb_size=2 lands on a
  960x540 canvas and must still lay out correctly.

  The four accent colours are the PAL pad's face buttons and are only ever used
  to encode state: green saved, yellow locked, blue focus, red destructive.
*/

#ifndef CHOME_THEME_H
#define CHOME_THEME_H

#include <inttypes.h>

#define PROF_HD 0
#define PROF_SD 1
#define PROF_LO 2

struct chome_profile
{
	int id;
	const char *name;

	int w, h;
	int inset;

	/*
	  Overscan margin. A TV does not show the whole picture - a few percent of every
	  edge sits behind the bezel - so anything anchored to an edge stops here rather
	  than at the edge itself: the menu bar sliding down from the top, the legend and
	  the save-state strip at the bottom. Zero on HD, where the signal is assumed to
	  reach an HDMI display 1:1. From cfg.classicui_overscan.
	*/
	int safe_x, safe_y;

	int card_w, card_h;
	int sel_w, sel_h;
	int pitch;
	int gap;
	int visible;

	int ts_title, ts_ui, ts_tiny;

	int y_title, y_meta, y_shelf, y_pips, y_pos, y_legend;
	int bar_h;

	int thumb_w, thumb_h, thumb_gap;
	int strip_h;

	int panel_w, panel_h;
	int row_h;
};

// Recompute for the current framebuffer size. force selects a profile
// (cfg.classicui_profile: 0 auto, 1 hd, 2 sd, 3 lo).
const chome_profile *theme_get();
void theme_update(int w, int h, int force);

#define COL_BG       0xff2a2c36u
#define COL_BGDARK   0xff1d1f26u
#define COL_GRID     0xff32343fu
#define COL_PANEL    0xffb9bac2u
#define COL_PANELHI  0xffd6d7ddu
#define COL_PANELLO  0xff8e8f99u
#define COL_INK      0xff20212au
#define COL_WHITE    0xfff2f2f5u
#define COL_DIM      0xff7a7c8cu
#define COL_SHADOW   0xff101118u
#define COL_RED      0xffc4353cu
#define COL_YELLOW   0xffe8b22bu
#define COL_BLUE     0xff2e6fb8u
#define COL_GREEN    0xff4a9e4eu
#define COL_FOCUS    0xfff2f2f5u
#define COL_BLACK    0xff000000u

/*
  Face-button colours, for the legend's button prompts.

  A PlayStation player reads the shapes by colour before they read the shape, and a Super
  Famicom pad's four buttons are colour-coded the same way - so a prompt that says which
  button in the pad's own colours is quicker to act on than a grey glyph. Keyed to the
  shape (or the letter), not to what the button is mapped to: a circle is red on every
  PlayStation pad ever made whatever the firmware has bound it to.

  Brighter than the panel palette above because these sit on a dark chip and have to hold
  up at 8 pixels on a CRT.
*/
#define COL_BTN_TRIANGLE 0xff5cc46bu
#define COL_BTN_CIRCLE   0xffe8544fu
#define COL_BTN_SQUARE   0xfff2a3bdu
#define COL_BTN_CROSS    0xff7b7bf0u

#define COL_BTN_A        0xffe8544fu
#define COL_BTN_B        0xffe8b22bu
#define COL_BTN_X        0xff5a8fe0u
#define COL_BTN_Y        0xff5cc46bu

// The chip they sit on: near-black, as on the pad itself.
#define COL_BTN_CHIP     0xff16171cu

#endif
