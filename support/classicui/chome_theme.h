/*
  Classic Home - layout profiles and palette.

  Metrics are derived proportionally from the canvas so that any framebuffer size
  works, not just the three nominal ones. The canvas is output resolution divided
  by cfg.fb_size (video_fb_config() in video.cpp), so a 1080p output with fb_size=2
  lands on a 960x540 canvas and must still lay out correctly. A 15 kHz TV canvas is
  not square-pixelled at all - see px below.

  The four accent colours are the PAL pad's face buttons and are only ever used
  to encode state: green saved, yellow locked, blue focus, red destructive.
*/

#ifndef CHOME_THEME_H
#define CHOME_THEME_H

#include <inttypes.h>

#define PROF_HD 0
#define PROF_SD 1
#define PROF_LO 2

/*
  How many suspend slots the strip lays out a row of. The same number as CH_SLOTS_USER in
  chome_ui.cpp, which is what draws them, and tied to it by a static_assert there: the tile
  size is derived from a full row fitting between the insets, and a profile cannot ask
  chome_ui.cpp how wide a row is. A core offering fewer slots draws the same tiles in a
  narrower row rather than bigger ones - a suspend point is looked at at one size, whatever
  the core it came from happens to keep.
*/
#define CHOME_STRIP_SLOTS 3

struct chome_profile
{
	int id;
	const char *name;

	int w, h;
	int inset;

	/*
	  How many canvas pixels wide one square unit is: 1 on a canvas whose pixels are
	  square, 2 on a 15 kHz TV canvas whose pixels are twice as tall as they are wide.

	  The scaler stretches the framebuffer across the mode's whole active area, so a
	  640x240 canvas and a 320x240 one fill exactly the same screen - the 640 one just
	  has half-width pixels. video_fb_config() normally hands the front-end the 320 form,
	  but only while it holds the analog output itself; with vga_scaler=1 or
	  direct_video the halving does not happen and a 640x240 (or 640x288) canvas
	  arrives instead. Every shape that has to keep a ratio divides by this, and the
	  profile is chosen from w / px rather than from w - see theme_update().
	*/
	int px;

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

	/*
	  The suspend strip: the panel across the bottom, and the row of slot tiles in it.

	  strip_h is the strip's height above the overscan margin. The panel is drawn
	  strip_h + safe_y tall from h - safe_y - strip_h, so it reaches the bottom edge of
	  the canvas whatever the margin is, and the strip keeps the same room inside it.

	  thumb_y is the top of the tile row measured from the panel's own top edge, so the
	  draw site and the metrics below cannot disagree about where the row starts. The
	  tiles are always 4:3, whatever room is left over: what goes in one is a frame of
	  the game, written 4:3 by ss_write_thumb(), and it is blitted to the tile without
	  letterboxing - so a tile of another shape is a stretched screenshot.
	*/
	int thumb_w, thumb_h, thumb_gap, thumb_y;
	int strip_h;

	int panel_w, panel_h;
	int row_h;
};

// Recompute for the current framebuffer size. force selects a profile
// (cfg.classicui_profile: 0 auto, 1 hd, 2 sd, 3 lo).
const chome_profile *theme_get();
void theme_update(int w, int h, int force);

/*
  Make the next theme_update() recompute even though nothing it is passed has changed.
  For an ini setting the metrics are derived from - classicui_overscan - which the
  settings screen can move under a canvas that is staying exactly the same size.
*/
void theme_invalidate();

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

/*
  The lettered pads, one palette each. Named for the family rather than shared, because the
  two disagree about which letter is which colour and a single COL_BTN_A cannot be both.

  Xbox reuses three of the Super Famicom's four hues in a different order, and that is not a
  coincidence worth collapsing: its diamond is Nintendo's with A/B and X/Y swapped over, so
  the colour standing in a given corner swaps with the letter. Spelled out twice rather than
  derived from one another, so tuning one pad's red cannot silently move the other's.
*/
#define COL_SNES_A       0xffe8544fu   // red
#define COL_SNES_B       0xffe8b22bu   // amber
#define COL_SNES_X       0xff5a8fe0u   // blue
#define COL_SNES_Y       0xff5cc46bu   // green

#define COL_XBOX_A       0xff5cc46bu   // green
#define COL_XBOX_B       0xffe8544fu   // red
#define COL_XBOX_X       0xff5a8fe0u   // blue
#define COL_XBOX_Y       0xffe8b22bu   // amber

/*
  And a pad whose family we could not work out. The letters still say which button, because
  those come from the code the pad reports; the colour would be an invention, so there is
  not one - all four discs are the same light grey. A player holding something we do not
  recognise is better served by a legend that declines to guess than by one that paints a
  Nintendo palette onto a pad that does not have it.
*/
#define COL_BTN_PLAIN    0xffc3c5d0u

// The chip they sit on: near-black, as on the pad itself.
#define COL_BTN_CHIP     0xff16171cu

#endif
