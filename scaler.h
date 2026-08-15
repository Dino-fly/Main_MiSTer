/*
Copyright 2019 alanswx
with help from the MiSTer contributors including Grabulosaure
*/

#ifndef SCALER_H
#define SCALER_H

typedef enum {
    RGB,
    BGR,
    BGRA,
    RGBA,
    ARGB32, // respect endianness
    YUV,
} mister_scaler_format_t;

typedef struct {
   int header;
   int width;
   int height;
   int line;
   int output_width;
   int output_height;

   char *map;
   int num_bytes;
   int map_off;
} mister_scaler;

#define MISTER_SCALER_BASEADDR     0x20000000
#define MISTER_SCALER_BUFFERSIZE   2048*3*1024

/*
  Silence mister_scaler_init()'s two per-call diagnostics. For callers that read
  the header on a timer rather than because a player asked for something - see the
  note at the top of scaler.cpp. Off by default.
*/
void mister_scaler_quiet(int on);

mister_scaler *mister_scaler_init();
int mister_scaler_read(mister_scaler *,unsigned char *buffer, mister_scaler_format_t format = ARGB32);
void mister_scaler_free(mister_scaler *);

void request_screenshot(char *cmd, int scaled = 0);
void screenshot_cb(void);

// Captures the current scaler output straight to an absolute path, scaled down to
// max_w with the source aspect preserved. Synchronous, intended for small
// thumbnails (Classic Home's suspend points). Returns 1 on success, 0 if the
// scaler is unavailable or a normal screenshot is already in flight.
int screenshot_thumbnail(const char *fullpath, int max_w);

/*
  Writes an ARGB buffer straight to disk, format taken from the filename extension,
  scaled to output_width/height when those are non-zero. Unlike screenshot_thumbnail()
  this does not read the scaler, so it works when the caller already holds the pixels -
  which is the only way to capture core video while the HPS framebuffer owns the output.
*/
bool write_screenshot(const char *filename, const uint8_t *argb,
                      int width, int height, int output_width, int output_height);

// Grabs the current core frame straight into a caller-owned ARGB buffer (0xAARRGGBB),
// for drawing rather than saving. max_px bounds the buffer; returns 0 if the scaler
// is unavailable, busy, or the frame does not fit.
int screenshot_grab(uint32_t *dst, int max_px, int *out_w, int *out_h);

/*
  Why the last screenshot_grab() answered as it did, as a phrase fit for a log line.
  "ok" after a successful grab, "not attempted" before the first one.

  This exists because a failed grab and a successful one cannot be told apart from the
  screen. Classic Home draws its in-game menu over a still of the game, and over a
  physical disc that still was reported black - which is what a grab returning nothing
  looks like, and also what a grab working perfectly looks like when the screen it is
  drawn on has nothing of it showing. Guessing between those two cost a day. The caller
  prints this instead, and one line from the device says which it was.

  There are five distinct ways screenshot_grab() returns 0 and they need entirely
  different repairs - a stuck screenshot save, a core with no scaler output, a frame too
  big for the buffer - so the phrase names the one that happened rather than saying it
  failed.
*/
const char *screenshot_grab_why(void);

#endif
