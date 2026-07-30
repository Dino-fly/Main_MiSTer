#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include "chome_osk.h"
#include "chome_gfx.h"
#include "chome_theme.h"

#include "../../input.h"

#define A_CHAR  0
#define A_CAPS  1
#define A_PAGE  2
#define A_SPACE 3
#define A_DEL   4
#define A_DONE  5
#define A_MASK  6

#define MAXKEYS 64

struct oskey
{
	short x, y, w, h;
	short row;
	char ch;
	unsigned char act;
	const char *label;
};

static int active = 0;
static int result = 0;

static char text[OSK_MAX + 1];
static char title[64];
static char prompt[128];

static int masked = 0;                 // this is a password
static int hidden = 0;                 // ...and the player asked us to hide it
static int caps = 0;
static int page = 0;

static oskey keys[MAXKEYS];
static int nkeys = 0;
static int sel = 0;
static int laid_w = 0, laid_h = 0;     // canvas the current layout was built for

/*
  Page 0 is QWERTY, not alphabetical. A pad player finds a letter faster on an
  alphabetical grid, but almost nobody types their Wi-Fi password by spelling it -
  they remember it as a shape their fingers make on a phone. Keeping the familiar
  arrangement means that memory still works here.
*/
static const char *const pages[2][4] =
{
	{ "1234567890", "qwertyuiop", "asdfghjkl",  "zxcvbnm"   },
	{ "@#$%&*-_+=", "!?,.:;'\"/\\", "()[]{}<>~", "^|`"      },
};

/* ------------------------------------------------------------------ state --- */

void osk_open(const char *t, const char *pr, const char *initial, int mask)
{
	snprintf(title, sizeof(title), "%s", t ? t : "");
	snprintf(prompt, sizeof(prompt), "%s", pr ? pr : "");
	snprintf(text, sizeof(text), "%s", initial ? initial : "");

	masked = mask ? 1 : 0;
	hidden = 0;
	caps = 0;
	page = 0;
	result = 0;
	active = 1;
	laid_w = laid_h = 0;
	sel = 0;

	printf("ClassicUI: keyboard open for %s\n", title);
}

void osk_close()
{
	active = 0;
}

int osk_active() { return active; }
int osk_result() { return result; }
void osk_clear_result() { result = 0; }
const char *osk_text() { return text; }

/* ----------------------------------------------------------------- layout --- */

static void add(int x, int y, int w, int h, int row, char ch, int act, const char *label)
{
	if (nkeys >= MAXKEYS) return;

	oskey *k = &keys[nkeys++];
	k->x = (short)x; k->y = (short)y; k->w = (short)w; k->h = (short)h;
	k->row = (short)row;
	k->ch = ch;
	k->act = (unsigned char)act;
	k->label = label;
}

// Geometry of the whole panel, recomputed with the key table.
static int px, py, pw, ph, phdr, pkh, pad_s, scale;

static void layout(const chome_profile *p)
{
	scale = p->ts_ui;
	int s = scale;

	int kw = (p->w - 2 * p->inset) / 10;
	// Cap it: on a 1280-wide canvas a tenth of the screen per letter is a keyboard
	// you have to walk across. Big targets, not comic ones.
	if (kw > 44 * s) kw = 44 * s;

	int kh = kw * 3 / 5;
	if (kh < 11 * s) kh = 11 * s;       // the label has to fit

	int gw = kw * 10;
	pad_s = 4 * s;
	phdr = 10 * s + 6;
	pkh = kh;

	int line = 10 * s;                  // prompt, and the button hint
	int fh = 14 * s;                    // the text field

	pw = gw + 2 * pad_s;
	ph = phdr + pad_s + line + pad_s + fh + pad_s + 4 * kh + kh + pad_s + line + pad_s;
	px = (p->w - pw) / 2;
	py = (p->h - ph) / 2;
	if (py < p->safe_y) py = p->safe_y;

	int gx = px + pad_s;
	int y = py + phdr + pad_s + line + pad_s + fh + pad_s;

	nkeys = 0;

	for (int r = 0; r < 4; r++)
	{
		const char *row = pages[page][r];
		int n = (int)strlen(row);
		int rx = gx + (gw - n * kw) / 2;     // short rows sit centred under the long ones

		for (int i = 0; i < n; i++)
		{
			char c = row[i];
			if (caps && !page) c = (char)toupper((unsigned char)c);
			add(rx + i * kw, y + r * kh, kw, kh, r, c, A_CHAR, 0);
		}
	}

	// The function row divides the same width evenly: SPACE is not made wider
	// because X already types a space, and an even row is easier to aim along.
	int fn = masked ? 6 : 5;
	int fw = gw / fn;
	int fx = gx + (gw - fw * fn) / 2;
	int fy = y + 4 * kh;
	int i = 0;

	add(fx + (i++) * fw, fy, fw, kh, 4, 0, A_CAPS, caps ? "caps" : "CAPS");
	add(fx + (i++) * fw, fy, fw, kh, 4, 0, A_PAGE, page ? "ABC" : "!@#");
	add(fx + (i++) * fw, fy, fw, kh, 4, 0, A_SPACE, "SPACE");
	add(fx + (i++) * fw, fy, fw, kh, 4, 0, A_DEL, "DEL");
	if (masked) add(fx + (i++) * fw, fy, fw, kh, 4, 0, A_MASK, hidden ? "SHOW" : "HIDE");
	add(fx + (i++) * fw, fy, fw, kh, 4, 0, A_DONE, "DONE");

	if (sel >= nkeys) sel = nkeys - 1;
	laid_w = p->w;
	laid_h = p->h;
}

// Rebuilding the table moves the keys under the cursor, so keep pointing at the
// same place on screen rather than at the same table index.
static void relayout(const chome_profile *p)
{
	int cx = 0, cy = 0;
	if (nkeys && sel >= 0 && sel < nkeys)
	{
		cx = keys[sel].x + keys[sel].w / 2;
		cy = keys[sel].y + keys[sel].h / 2;
	}

	layout(p);

	if (!cx && !cy) return;

	int best = sel, bd = -1;
	for (int i = 0; i < nkeys; i++)
	{
		int dx = keys[i].x + keys[i].w / 2 - cx;
		int dy = keys[i].y + keys[i].h / 2 - cy;
		int d = dx * dx + dy * dy;
		if (bd < 0 || d < bd) { bd = d; best = i; }
	}
	sel = best;
}

/* ------------------------------------------------------------------- draw --- */

static void draw_key(const oskey *k, int on)
{
	int s = scale;

	uint32_t face = COL_PANELHI;
	uint32_t ink = COL_INK;

	if (k->act == A_DONE) { face = COL_GREEN; ink = COL_WHITE; }
	if (on) { face = COL_BLUE; ink = COL_WHITE; }

	int gap = (s > 1) ? s : 1;
	int x = k->x + gap, y = k->y + gap, w = k->w - 2 * gap, h = k->h - 2 * gap;

	gfx_fill(x, y, w, h, face);
	gfx_frame_rect(x, y, w, h, on ? COL_WHITE : COL_PANELLO, on ? 2 : 1);

	char lab[16];
	if (k->act == A_CHAR) { lab[0] = k->ch; lab[1] = 0; }
	else snprintf(lab, sizeof(lab), "%s", k->label);

	int ls = s;
	while (ls > 1 && gfx_text_w(lab, ls) > w - 2 * s) ls--;

	gfx_text_c(lab, x + w / 2, y + (h - 8 * ls) / 2, ls, ink, 0);
}

void osk_draw(const chome_profile *p, int pad)
{
	if (!active) return;

	if (p->w != laid_w || p->h != laid_h) layout(p);

	int s = scale;
	int line = 10 * s;
	int fh = 14 * s;

	// The screen behind is dimmed rather than covered: it stays clear that this is
	// a thing on top of the menu, not a new place the player has been taken to.
	gfx_scrim(0, 0, p->w, p->h, COL_BLACK, 2);

	gfx_fill(px + 4, py + 4, pw, ph, COL_SHADOW);
	gfx_fill(px, py, pw, ph, COL_PANEL);
	gfx_frame_rect(px, py, pw, ph, COL_PANELLO, 2);
	gfx_fill(px, py, pw, phdr, COL_INK);

	char up[64];
	snprintf(up, sizeof(up), "%s", title);
	for (char *q = up; *q; q++) *q = (char)toupper((unsigned char)*q);
	gfx_text(up, px + 6 * s, py + 4, s, COL_PANELHI, 0);

	int y = py + phdr + pad_s;
	// Ink, not a grey: this line is the only thing that says which network the
	// password is for, and grey-on-panel is the first thing a CRT loses.
	gfx_text(gfx_clip(prompt, s, pw - 12 * s), px + 6 * s, y, s, COL_INK, 0);
	y += line + pad_s;

	/* --- the field --- */
	int fx = px + pad_s, fw = pw - 2 * pad_s;
	gfx_fill(fx, y, fw, fh, COL_WHITE);
	gfx_frame_rect(fx, y, fw, fh, COL_INK, s);

	char shown[OSK_MAX + 1];
	if (masked && hidden)
	{
		size_t n = strlen(text);
		memset(shown, '*', n);
		shown[n] = 0;
	}
	else snprintf(shown, sizeof(shown), "%s", text);

	int caret = 2 * s;
	int room = fw - 4 * s - caret;
	const char *vis = shown;
	// Show the tail once it overruns: what was just typed matters more than the start.
	while (gfx_text_w(vis, s) > room && *vis) vis++;

	int tw = gfx_text_w(vis, s);
	gfx_text(vis, fx + 2 * s, y + (fh - 8 * s) / 2, s, COL_INK, 0);
	// A solid caret, not a blinking one: the screen is only redrawn when something
	// happens, so a blink would need a timer waking the whole front-end up.
	gfx_fill(fx + 2 * s + tw, y + 2 * s, caret, fh - 4 * s, COL_BLUE);

	if (!*text)
	{
		const char *ph2 = "(empty)";
		gfx_text(ph2, fx + 4 * s + caret, y + (fh - 8 * s) / 2, s, COL_PANELLO, 0);
	}

	y += fh + pad_s;

	for (int i = 0; i < nkeys; i++) draw_key(&keys[i], i == sel);

	/* --- what the buttons do --- */
	// On a bar of its own, matching the header: the same text on the panel face was
	// unreadable at 240p, and there is no mid grey in the palette that would fix it.
	int hf = line + 2 * pad_s;
	int hy = py + ph - hf + pad_s;
	gfx_fill(px, py + ph - hf, pw, hf, COL_INK);

	char hint[128];
	if (pad)
	{
		snprintf(hint, sizeof(hint), "A TYPE   B BACK   X SPACE   Y DELETE");
	}
	else
	{
		snprintf(hint, sizeof(hint), "JUST TYPE - OR PICK WITH THE ARROWS");
	}
	gfx_text_c(gfx_clip(hint, s, pw - 8 * s), px + pw / 2, hy, s, COL_PANELHI, 0);
}

/* ------------------------------------------------------------------ input --- */

static void insert(char c)
{
	size_t n = strlen(text);
	if (n >= OSK_MAX) return;

	text[n] = c;
	text[n + 1] = 0;
}

static void backspace()
{
	size_t n = strlen(text);
	if (n) text[n - 1] = 0;
}

static void move(const chome_profile *p, int dx, int dy)
{
	if (!nkeys) return;

	if (dx)
	{
		// Along a row, wrapping at the ends: the alternative is a cursor that
		// stops dead against an invisible wall.
		int row = keys[sel].row;
		int first = sel, last = sel;
		while (first > 0 && keys[first - 1].row == row) first--;
		while (last < nkeys - 1 && keys[last + 1].row == row) last++;

		sel += dx;
		if (sel < first) sel = last;
		else if (sel > last) sel = first;
		return;
	}

	if (!dy) return;

	int row = keys[sel].row + dy;
	int rows = 5;
	if (row < 0) row = rows - 1;
	if (row >= rows) row = 0;

	int cx = keys[sel].x + keys[sel].w / 2;
	int best = -1, bd = -1;
	for (int i = 0; i < nkeys; i++)
	{
		if (keys[i].row != row) continue;

		int d = keys[i].x + keys[i].w / 2 - cx;
		if (d < 0) d = -d;
		if (bd < 0 || d < bd) { bd = d; best = i; }
	}
	if (best >= 0) sel = best;
	(void)p;
}

/*
  Linux keycode to the character it prints, for the player who has a real keyboard
  plugged in. Only the unshifted main block: shift state is not tracked here, so
  CAPS on this keyboard is what selects capitals either way.
*/
static char kb_char(int k)
{
	static const struct { int from, to; const char *chars; } map[] =
	{
		{ KEY_1, KEY_EQUAL,        "1234567890-=" },
		{ KEY_Q, KEY_RIGHTBRACE,   "qwertyuiop[]" },
		{ KEY_A, KEY_APOSTROPHE,   "asdfghjkl;'"  },
		{ KEY_Z, KEY_SLASH,        "zxcvbnm,./"   },
	};

	for (unsigned i = 0; i < sizeof(map) / sizeof(map[0]); i++)
	{
		if (k < map[i].from || k > map[i].to) continue;
		return map[i].chars[k - map[i].from];
	}

	if (k == KEY_GRAVE) return '`';
	if (k == KEY_BACKSLASH) return '\\';
	if (k == KEY_SPACE) return ' ';
	return 0;
}

static void activate(const chome_profile *p)
{
	const oskey *k = &keys[sel];

	switch (k->act)
	{
	case A_CHAR:  insert(k->ch); break;
	case A_SPACE: insert(' '); break;
	case A_DEL:   backspace(); break;
	case A_CAPS:  caps = !caps; relayout(p); break;
	case A_PAGE:  page = !page; relayout(p); break;
	case A_MASK:  hidden = !hidden; relayout(p); break;

	case A_DONE:
		result = 1;
		active = 0;
		printf("ClassicUI: keyboard done, %d characters\n", (int)strlen(text));
		break;

	default: break;
	}
}

int osk_key(int k, int pad)
{
	if (!active) return 0;

	const chome_profile *p = theme_get();
	if (p->w != laid_w || p->h != laid_h) layout(p);

	// A real keyboard types; the cursor keys still work for the odd character the
	// player cannot find. Checked first so that ` and the bracket keys type
	// themselves instead of being read as pad buttons.
	if (!pad)
	{
		char c = kb_char(k);
		if (c)
		{
			if (caps) c = (char)toupper((unsigned char)c);
			insert(c);
			return 1;
		}
	}

	switch (k)
	{
	case KEY_LEFT:  move(p, -1, 0); break;
	case KEY_RIGHT: move(p, 1, 0); break;
	case KEY_UP:    move(p, 0, -1); break;
	case KEY_DOWN:  move(p, 0, 1); break;

	case KEY_ENTER:
	case KEY_KPENTER:
		/*
		  On a keyboard Enter finishes the entry rather than pressing the key under
		  the cursor. Nothing is lost by that: every key in the grid is also a key on
		  their keyboard, so they never needed to point at one - the grid is for the
		  pad. Having Enter type a "1" after they have carefully typed a password is
		  just baffling.
		*/
		if (!pad)
		{
			result = 1;
			active = 0;
			printf("ClassicUI: keyboard done, %d characters\n", (int)strlen(text));
			break;
		}
		activate(p);
		break;

	case KEY_SPACE:                 // some pads map A here; X is the literal space
		activate(p);
		break;

	case KEY_BACKSPACE:             // pad Y, and the keyboard's own backspace
		backspace();
		break;

	case KEY_TAB:                   // pad X
		insert(' ');
		break;

	case KEY_GRAVE:                 // pad Select
		caps = !caps;
		relayout(p);
		break;

	case KEY_ESC:
	case KEY_BACK:
	case KEY_MENU:
	case KEY_F12:
		result = -1;
		active = 0;
		printf("ClassicUI: keyboard cancelled\n");
		break;

	default:
		break;
	}

	return 1;
}
