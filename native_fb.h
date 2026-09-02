/*
  Native analog framebuffer for the Console Mode menu core (Retro-Remake/menu_ConsoleMode).

  That core carries a DDR3 frame reader (rtl/native_video_reader.sv) that scans a
  32-bit frame out on the core video path, so it goes through sys_top's Y/C encoder and
  subcarrier gate exactly like a game does. The HPS framebuffer path Classic Home uses
  by default is muxed in as vgas_o and never enters the encoder, which is why the shelf
  is black and white on S-Video and composite while games are in colour.

  Read out of Retro-Remake/menu_ConsoleMode on branch `consolemode`
  (rtl/native_video_reader.sv, rtl/native_video_top.sv, menu.sv) and checked field by
  field against the status decode in menu.sv rather than taken on trust.

  Worth knowing before anyone is told they must choose: that core already carries the
  SNAC controller. menu.sv instantiates ps1_snac_controller with snac_enable tied high
  and wires hps_io's snac_buttons/snac_debug, so a player moving to it for colour keeps
  a PSX pad in the front-end. Its own comment ("status[9] is the native-FB mode not
  SNAC") is about a bit that used to mean something else, not about a choice between
  the two.

  Protocol, from the RTL:
    ctrl word   @ 0x3A000000  (uint32) = (frame_counter << 2) | active_buffer(bit0)
    buffer 0    @ 0x3A000100  pixels, uint32 0x00RRGGBB (Linux fb byte order B,G,R,X)
    buffer 1    = buffer 0 + line_words*8 * total_lines
    NTSC 240p   : 320 px x 240 lines, stride 1280 bytes
    PAL  288p   : 352 px x 288 lines, stride 1408 bytes
    status[9]   = 1 turns the reader on, status[24:22] = mode (0 NTSC 240p, 2 PAL 288p)
*/
#ifndef NATIVE_FB_H
#define NATIVE_FB_H

#include <stdint.h>

// 1 if the loaded core is the Console Mode menu (its CONF_STR carries the Video Mode row).
int  native_fb_available();

// Take or release the analog output. Maps DDR on first enable.
void native_fb_enable(int on);
int  native_fb_active();

// Copy a w x h 0x00RRGGBB canvas into buffer (n & 1) and flip. Returns 1 when shown.
int  native_fb_present(const uint32_t *src, int w, int h, int n);

// Re-send the H/V centering status bits from cfg. No-op when inactive.
void native_fb_apply_offsets();

// Switch raster mode to cfg.classicui_native_mode. No-op when inactive.
void native_fb_set_mode();

#endif
