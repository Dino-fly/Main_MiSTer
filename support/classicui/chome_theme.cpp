#include <stdio.h>
#include "chome_theme.h"
#include "../../cfg.h"
#include "../../video.h"

static chome_profile P;

static int pct(int v, double f)
{
	return (int)(v * f + 0.5);
}

const chome_profile *theme_get()
{
	return &P;
}

void theme_invalidate()
{
	P.name = 0;
}

void theme_update(int w, int h, int force)
{
	static int last_force = -1;
	static int last_div = -1;

	/*
	  How many pixels of glass one canvas pixel covers. With classicui_halfres on - or a
	  global fb_size=2 - the framebuffer is a fraction of the display mode and the scaler
	  upscales it, so the canvas alone no longer says how big anything is on the screen.
	  1 whenever the framebuffer is the mode 1:1, which makes everything below exactly
	  what it always was.
	*/
	int div = video_menu_fb_div();
	if (div < 1) div = 1;

	// Recompute when the canvas changes or when the forced profile changes;
	// the Display panel relies on the latter. The divisor is in the key for the one
	// case where it moves without the canvas moving: a refused request being granted
	// after a mode change to the same size.
	if (w == P.w && h == P.h && P.name && force == last_force && div == last_div) return;
	last_force = force;
	last_div = div;

	/*
	  Are the canvas pixels square?

	  Nothing MiSTer outputs is wider than 16:9, so a canvas at least twice as wide as
	  it is tall is not a wide screen - it is a 15 kHz TV canvas that arrived unhalved.
	  video_fb_config() hands the front-end 320x240 while it holds the analog output
	  itself, because a 15 kHz mode has pixels twice as tall as they are wide; but that
	  halving is conditional on the takeover, and with vga_scaler=1 or direct_video
	  there is no takeover, so the full 640x240 (or 640x288, with menu_pal=1) canvas
	  comes through instead.

	  That is the bug behind "the interface was smushed together with the buttons
	  overlapping" on a CRT with vga_scaler=1. At 640x240 the width alone put the
	  layout on the SD profile, and SD sizes a card at a quarter of the width - 160 px,
	  which through the 228x167 ratio is 117 lines, enlarged to 152 for the selected
	  one. On a 240-line canvas that is more than the whole shelf band, so both clamps
	  below saturated and the position line ended up two pixels *below* the button
	  legend, drawn over it.

	  So the shape of the surface is worked out first, and everything derived from a
	  ratio - the profile itself, the text scales, the card, the slot tiles, the margin
	  spent on lines - is measured in square units rather than in canvas pixels. At
	  640x240 that reproduces the 240p layout exactly, twice as wide, which is what the
	  screen shows anyway. The one thing it cannot fix is the 8x8 ROM font: the scale is
	  a single integer, so on a stretched canvas the glyphs come out half as wide as
	  they are tall. Thin text that fits beats correctly-shaped text drawn off the
	  bottom of the picture, and this canvas is not one the front-end ever asks for.
	*/
	int px = (w >= h * 2) ? 2 : 1;
	int ew = w / px;                  // the width in square units

	/*
	  The profile is chosen from the glass, not from the canvas.

	  The hd/sd/lo split answers "how far away is this screen and how much fits on it",
	  and that is a property of the display: a 720p television is the same television
	  whether the framebuffer behind it is 1280 or 640 wide. Chosen from the canvas, a
	  720p display with classicui_halfres on landed on the sd layout - three cards
	  instead of five, and a 6% overscan margin meant for a CRT's bezel spent on an HDMI
	  panel that shows every pixel - while a 1080p display halved to 960 stayed hd. The
	  same discontinuity was already reachable with a global fb_size=2, which is why this
	  reads the divisor that is actually in force rather than the front-end's own option.

	  Every metric below is a fraction of the canvas, so the same profile at half the
	  canvas is the same picture on the glass, upscaled.
	*/
	int eg = ew * div;                // the width in square units of glass

	int id;
	if (force == 1) id = PROF_HD;
	else if (force == 2) id = PROF_SD;
	else if (force == 3) id = PROF_LO;
	else if (eg >= 900) id = PROF_HD;
	else if (eg >= 480) id = PROF_SD;
	else id = PROF_LO;

	P.id = id;
	P.w = w;
	P.h = h;
	P.px = px;

	switch (id)
	{
	case PROF_HD: P.name = "hd"; P.visible = 5; break;
	case PROF_SD: P.name = "sd"; P.visible = 3; break;
	default:      P.name = "lo"; P.visible = 3; break;
	}

	/*
	  Text scales. Integer only: the ROM font is never resampled.

	  Chosen on the glass like the profile, then divided back to canvas pixels, so a
	  glyph covers the same screen area whether the canvas is halved or not: 720p gives
	  ts_ui 2 whole and 1 halved, which are the same 16 pixels of glass. The one that
	  cannot survive the division exactly is the title at half resolution - 3 canvas
	  pixels has no half, so it rounds up to 2, which is 32 pixels of glass against 24
	  native. A title a third bigger beats every string on the screen shrinking to
	  half height, which is what dividing 900 by the divisor and reading the old
	  thresholds off the canvas would have done.
	*/
	int tt, tu, ty;
	if (eg >= 900)      { tt = 3; tu = 2; ty = 2; }
	else if (eg >= 480) { tt = 2; tu = 1; ty = 1; }
	else                { tt = 1; tu = 1; ty = 1; }

	P.ts_title = (tt + div - 1) / div; if (P.ts_title < 1) P.ts_title = 1;
	P.ts_ui    = (tu + div - 1) / div; if (P.ts_ui < 1) P.ts_ui = 1;
	P.ts_tiny  = (ty + div - 1) / div; if (P.ts_tiny < 1) P.ts_tiny = 1;

	/*
	  Overscan. An analog canvas means a TV, and a TV keeps a few percent of every
	  edge behind its bezel - which is why the menu bar, sliding down to y=0, was
	  landing mostly off-screen on a CRT. HD is left alone: that canvas only arises
	  on an HDMI display, which shows the signal 1:1.

	  Horizontally this folds into inset, which every screen already indents by. The
	  vertical margin is applied by the elements anchored to an edge.
	*/
	int over = (id == PROF_HD) ? 0 : cfg.classicui_overscan;
	if (over > 15) over = 15;
	P.safe_x = pct(w, over / 100.0);
	P.safe_y = pct(h, over / 100.0);

	P.inset = pct(w, 0.025);
	if (P.inset < P.safe_x) P.inset = P.safe_x;

	/*
	  Card width as a fraction of the canvas, from the reviewed mockup metrics:
	  228/1280 on HD, 160/640 on SD, 84/320 at 240p. The fraction grows as the
	  canvas shrinks because fewer cards are on screen - a single proportional
	  constant makes 240p cards far too small. Height keeps the 228x167 ratio.
	*/
	double cf = (id == PROF_HD) ? 0.178 : (id == PROF_SD) ? 0.250 : 0.2625;
	P.card_w = pct(w, cf);
	P.card_h = (P.card_w * 167) / (228 * px);
	P.gap = pct(w, (id == PROF_HD) ? 0.019 : 0.022);

	double sel = (id == PROF_HD) ? 1.35 : (id == PROF_SD) ? 1.30 : 1.25;
	P.sel_w = pct(P.card_w, sel);
	P.sel_h = pct(P.card_h, sel);

	// Keeps the gap beside the enlarged card equal to every other gap.
	P.pitch = P.card_w + P.gap + (P.sel_w - P.card_w) / 2;

	P.bar_h = pct(h, 0.078);
	if (P.bar_h < 8 * P.ts_ui + 8) P.bar_h = 8 * P.ts_ui + 8;

	P.y_title  = pct(h, 0.244);
	P.y_meta   = pct(h, 0.297);
	P.y_shelf  = pct(h, 0.722);
	P.y_pips   = pct(h, 0.753);
	P.y_pos    = pct(h, 0.808);

	/*
	  The margin under whatever hangs off the bottom edge. The inset is a fraction of
	  the width, so on a stretched canvas spending it on lines spends twice as many as
	  the same fraction of the height would - which at 640x240 took 24 lines out of a
	  240-line picture. Converted back to square units first. At px == 1 this is
	  exactly the max() that has always been here.
	*/
	int vmargin = P.inset / px;
	if (vmargin < P.safe_y) vmargin = P.safe_y;
	P.y_legend = h - vmargin - 8 * P.ts_ui;

	/*
	  Keep the shelf clear of the title block on short canvases.

	  The block is three lines deep, not two: under the title and the system line sits the
	  file name of the selected game, drawn only on the cards where the title alone does
	  not say which file it is (draw_title_block). Reserved unconditionally, because a row
	  of cards that moved when the cursor reached such a card would be worse than the space
	  it costs - and at the three nominal canvases this changes no metric at all, the
	  clamp only ever bites on a canvas far shorter than it is wide.
	*/
	int need = P.y_meta + 12 * P.ts_ui + 10 * P.ts_tiny + P.sel_h;
	if (P.y_shelf < need) P.y_shelf = need;
	if (P.y_shelf > P.y_legend - 10 * P.ts_ui) P.y_shelf = P.y_legend - 10 * P.ts_ui;
	if (P.y_pips < P.y_shelf + 4) P.y_pips = P.y_shelf + 4;
	if (P.y_pos < P.y_pips + 10 * P.ts_tiny) P.y_pos = P.y_pips + 10 * P.ts_tiny;

	/*
	  The suspend strip, and the row of slot tiles inside it.

	  strip_h is the strip's own height, above the overscan margin: draw_suspend() rests the
	  panel at h - safe_y - strip_h and fills it strip_h + safe_y tall, so the panel reaches
	  the bottom edge of the canvas and the screen shows no seam under it. The strip keeps
	  its strip_h of room whatever the margin is - on a television that hides 15% of every
	  edge the panel simply starts higher up - and the tiles below are derived from whatever
	  room that leaves, so a bigger margin gives them a little more rather than less.

	  The tiles were pct(w, 0.156) regardless of what the strip had spare, which at 240p is
	  50x37 - a row of three spanning 168 px of the 276 the header spans, with about a third
	  of the panel below them empty. They are derived from the room instead: as wide as a
	  full row fits between the insets, as tall as fits between the header and the legend,
	  and the tighter of the two decides, because the tile keeps 4:3 (see chome_theme.h).
	  On a tall-enough canvas that is the width; at 240p and on a 540-line canvas it is
	  the height, and the width left over is the price of not stretching a screenshot.
	*/
	P.strip_h = pct(h, (id == PROF_LO) ? 0.50 : 0.47);
	P.thumb_gap = pct(w, 0.019);

	// Clears the header: armed, that line is a drawn button, which btn_chip() hangs 2*s
	// above its text row and 12*s tall - so 18*ts_ui is the first row below either form
	// of it, and the 6 is the gap under it.
	P.thumb_y = 18 * P.ts_ui + 6;

	/*
	  What the row has to itself. Horizontally both insets, less the 3 px the focus frame
	  is drawn outside the selected tile on each side; vertically from the top of the tile
	  row down to the top of the legend's own band (draw_legend fills from y_legend -
	  6*ts_ui), less the slot number under each tile and two rows so the caption does not
	  sit on the legend's rule.
	*/
	int strip_top = h - P.safe_y - P.strip_h;              // where draw_suspend() rests it
	int room_w = w - P.inset * 2 - 6 - (CHOME_STRIP_SLOTS - 1) * P.thumb_gap;
	int room_h = (P.y_legend - 8 * P.ts_ui) - (strip_top + P.thumb_y) - (5 + 8 * P.ts_tiny);

	P.thumb_w = room_w / CHOME_STRIP_SLOTS;
	P.thumb_h = (P.thumb_w * 3) / (4 * px);
	if (P.thumb_h > room_h)
	{
		P.thumb_h = room_h;
		P.thumb_w = (P.thumb_h * 4 * px) / 3;
	}

	// No profile is this cramped, but the metrics have to stay drawable on any
	// framebuffer: a zero or negative tile is a blit of garbage, not a small tile.
	if (P.thumb_w < 16) P.thumb_w = 16;
	if (P.thumb_h < 12) P.thumb_h = 12;

	P.panel_w = pct(w, (id == PROF_LO) ? 0.78 : 0.46);
	P.panel_h = pct(h, (id == PROF_LO) ? 0.62 : 0.56);
	P.row_h = 12 * P.ts_ui;

	char divs[24] = "";
	if (div > 1) snprintf(divs, sizeof(divs), " (1/%d of the display)", div);
	printf("ClassicUI: profile %s, canvas %dx%d%s%s, card %dx%d, pitch %d, text %dx/%dx\n",
		P.name, P.w, P.h, (px > 1) ? " (half-width pixels)" : "", divs,
		P.card_w, P.card_h, P.pitch, P.ts_title, P.ts_ui);
	printf("ClassicUI: strip %d tall at y=%d, slot tile %dx%d, margin %dx%d\n",
		P.strip_h, P.h - P.safe_y - P.strip_h, P.thumb_w, P.thumb_h, P.safe_x, P.safe_y);
}
