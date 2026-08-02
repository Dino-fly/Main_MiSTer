/*
  Classic Home - pixel drawing layer.

  Everything is composed into a cached RAM buffer and only the damaged rows are
  copied into the menu framebuffer, which is an uncached /dev/mem mapping
  (shmem.cpp) shared with the FPGA scaler. Drawing straight into it a pixel at a
  time would be far too slow; see docs/CLASSIC_UI_PLAN.md section 7.3.

  Colours are 0xAARRGGBB, matching Imlib2's DATA32 convention, which is what the
  existing wallpaper code already writes into these buffers.
*/

#ifndef CHOME_GFX_H
#define CHOME_GFX_H

#include <inttypes.h>

// Acquire the framebuffer and compose buffer for this frame.
// Returns 0 when the framebuffer is unavailable (core without HPS fb support).
int  gfx_begin();

// Copy damaged rows to the back buffer and page-flip.
void gfx_end();

void gfx_shutdown();

int  gfx_w();
int  gfx_h();

// Damage tracking. gfx_damage_all() is implied by a resolution change.
void gfx_damage(int x, int y, int w, int h);
void gfx_damage_all();

// Height in rows of the damage submitted this frame, for telemetry.
int  gfx_damage_rows();

void gfx_fill(int x, int y, int w, int h, uint32_t col);
void gfx_frame_rect(int x, int y, int w, int h, uint32_t col, int t);
void gfx_blend(int x, int y, int w, int h, uint32_t col, int alpha);

// 50% checkerboard scrim: dims a region without introducing a new colour.
void gfx_scrim(int x, int y, int w, int h, uint32_t col, int step);

/*
  Activity indicators. Both are driven by a millisecond clock, not by a frame count -
  frames here are only drawn when something changed, so a frame counter would make the
  same animation run at a different speed depending on what else was moving.

  GFX_SPIN_MS is also the rate at which a screen showing one has to repaint, and it is
  a tenth of a second rather than a frame time on purpose: the ring has eight
  positions, so painting faster than it moves is work with nothing to show for it, and
  a full repaint of this UI is not free.
*/
#define GFX_SPIN_DOTS 8
#define GFX_SPIN_MS   100UL
#define GFX_SWEEP_MS  900UL

// A ring of dots with a bright head and a fading tail: something is happening and has
// not stopped happening. Drawn around whatever sits at cx,cy - an icon, usually.
void gfx_spinner(int cx, int cy, int r, int dot, unsigned long ms, uint32_t hot, uint32_t cold);

// Named steps as a row of boxes: `done` behind us, the one being worked on sweeping,
// the rest empty. See the comment in chome_gfx.cpp for why the active one sweeps
// rather than creeping forward, and why `live` has to be told rather than assumed.
void gfx_track(int x, int y, int w, int h, int nseg, int done, int live, unsigned long ms,
	uint32_t fill, uint32_t track, uint32_t glow);

// Text, in MiSTer's 8x8 OSD ROM font (charrom.cpp), at integer scales only.
// Codes 1..4 are up/down/left/right arrows, which the ROM font lacks.
#define CH_UP    "\x01"
#define CH_DOWN  "\x02"
#define CH_LEFT  "\x03"
#define CH_RIGHT "\x04"

#define GLYPH_W 8

void gfx_text(const char *s, int x, int y, int scale, uint32_t col, uint32_t shadow);
void gfx_text_c(const char *s, int cx, int y, int scale, uint32_t col, uint32_t shadow);
int  gfx_text_w(const char *s, int scale);

// Truncate to fit maxpx, appending '>' when clipped. Returns a static buffer.
const char *gfx_clip(const char *s, int scale, int maxpx);

// Nearest-neighbour blit of an ARGB source. No filtering: at these scales
// integer-ish nearest keeps pixel art crisp and costs almost nothing.
void gfx_blit(const uint32_t *src, int sw, int sh, int dx, int dy, int dw, int dh);

#endif
