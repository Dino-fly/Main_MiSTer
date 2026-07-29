#include <stdio.h>
#include "chome_theme.h"
#include "../../cfg.h"

static chome_profile P;

static int pct(int v, double f)
{
	return (int)(v * f + 0.5);
}

const chome_profile *theme_get()
{
	return &P;
}

void theme_update(int w, int h, int force)
{
	static int last_force = -1;

	// Recompute when the canvas changes or when the forced profile changes;
	// the Display panel relies on the latter.
	if (w == P.w && h == P.h && P.name && force == last_force) return;
	last_force = force;

	int id;
	if (force == 1) id = PROF_HD;
	else if (force == 2) id = PROF_SD;
	else if (force == 3) id = PROF_LO;
	else if (w >= 900) id = PROF_HD;
	else if (w >= 480) id = PROF_SD;
	else id = PROF_LO;

	P.id = id;
	P.w = w;
	P.h = h;

	switch (id)
	{
	case PROF_HD: P.name = "hd"; P.visible = 5; break;
	case PROF_SD: P.name = "sd"; P.visible = 3; break;
	default:      P.name = "lo"; P.visible = 3; break;
	}

	// Text scales. Integer only: the ROM font is never resampled.
	if (w >= 900)      { P.ts_title = 3; P.ts_ui = 2; P.ts_tiny = 2; }
	else if (w >= 480) { P.ts_title = 2; P.ts_ui = 1; P.ts_tiny = 1; }
	else               { P.ts_title = 1; P.ts_ui = 1; P.ts_tiny = 1; }

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
	P.card_h = (P.card_w * 167) / 228;
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
	P.y_legend = h - (P.inset > P.safe_y ? P.inset : P.safe_y) - 8 * P.ts_ui;

	// Keep the shelf clear of the title block on short canvases.
	int need = P.y_meta + 12 * P.ts_ui + P.sel_h;
	if (P.y_shelf < need) P.y_shelf = need;
	if (P.y_shelf > P.y_legend - 10 * P.ts_ui) P.y_shelf = P.y_legend - 10 * P.ts_ui;
	if (P.y_pips < P.y_shelf + 4) P.y_pips = P.y_shelf + 4;
	if (P.y_pos < P.y_pips + 10 * P.ts_tiny) P.y_pos = P.y_pips + 10 * P.ts_tiny;

	P.thumb_w = pct(w, 0.156);
	P.thumb_h = (P.thumb_w * 3) / 4;
	P.thumb_gap = pct(w, 0.019);
	P.strip_h = pct(h, (id == PROF_LO) ? 0.50 : 0.47);

	P.panel_w = pct(w, (id == PROF_LO) ? 0.78 : 0.46);
	P.panel_h = pct(h, (id == PROF_LO) ? 0.62 : 0.56);
	P.row_h = 12 * P.ts_ui;

	printf("ClassicUI: profile %s, canvas %dx%d, card %dx%d, pitch %d, text %dx/%dx\n",
		P.name, P.w, P.h, P.card_w, P.card_h, P.pitch, P.ts_title, P.ts_ui);
}
