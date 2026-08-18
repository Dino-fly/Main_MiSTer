#ifndef VIDEO_H
#define VIDEO_H

#define VFILTER_HORZ  0
#define VFILTER_VERT  1
#define VFILTER_SCAN  2
#define VFILTER_ILACE 3

struct VideoInfo
{
	uint32_t width;
	uint32_t height;
	uint32_t htime;
	uint32_t vtime;
	uint32_t ptime;
	uint32_t ctime;
	uint32_t vtimeh;
	uint32_t frame_clocks;
	uint32_t arx;
	uint32_t ary;
	uint32_t arxy;
	uint32_t fb_en;
	uint32_t fb_fmt;
	uint32_t fb_width;
	uint32_t fb_height;
	uint32_t pixrep;
	uint32_t de_h;
	uint32_t de_v;

	bool interlaced;
	bool rotated;
};

// expose video timings for timerfd-based frame timer
extern VideoInfo current_video_info;

void  video_init();
void  video_poll();

int   video_get_edid(uint8_t **buf, int *size);
void  video_hdmi_power(int on);

int   video_get_scaler_flt(int type);
void  video_set_scaler_flt(int type, int n);
char* video_get_scaler_coeff(int type, int only_name = 1);
void  video_set_scaler_coeff(int type, const char *name);

int   video_get_gamma_en();
void  video_set_gamma_en(int n);
char* video_get_gamma_curve(int only_name = 1);
void  video_set_gamma_curve(const char *name);

int   video_get_shadow_mask_mode();
void  video_set_shadow_mask_mode(int n);
char* video_get_shadow_mask(int only_name = 1);
void  video_set_shadow_mask(const char *name);
void  video_loadPreset(char *name, bool save);

int   video_get_rotated();

void video_cfg_reset();

void  video_mode_adjust(bool force = false);

int   hasAPI1_5();

void video_fb_enable(int enable, int n = 0);
int video_fb_state();

// True only while the console framebuffer (buffer 0) is what is on screen - see video.cpp.
int video_fb_terminal();
void video_menu_bg(int n, int idle = 0);

// 1 when an HDMI sink is attached, 0 when not, -1 when unknown.
int video_hdmi_connected();

// 1 when the scaler's output is what the user sees, so filters/masks/gamma apply.
int video_scaler_is_visible();

// Menu-core framebuffer access for alternative front-ends (support/classicui).
// n is 1 or 2: the menu background double-buffer pair.
uint32_t* video_menu_fb(int n);
int video_menu_fb_width();
int video_menu_fb_height();

// Ask for the menu framebuffer at 1/div of the display mode (2 or 4; 0 releases). May
// be refused - see video.cpp. Release it before handing the framebuffer to anyone else.
void video_fb_size_request(int div);
// The divisor actually in force between the display mode and the framebuffer, >= 1.
int video_menu_fb_div();

/*
  Ask for the menu framebuffer to be scanned out as RGB565 rather than 32-bit.

  Halves what the fabric has to read every frame - 8.1MB down to 4.1MB at 1080p, which
  is half a gigabyte a second of DDR3 the running core is also using - and halves what
  the front-end has to write. The caller is responsible for putting 565 pixels there;
  see gfx_end() in support/classicui/chome_gfx.cpp.
*/
void video_menu_fb_16bpp(int on);
int video_menu_fb_bpp();
// Returns 1 when the core accepted the framebuffer, 0 when it has no support for
// it (in which case a front-end must fall back to the OSD).
int video_menu_fb_present(int n);

// Ask for the framebuffer to be shown on the analog output, for setups where the
// scaler is otherwise bypassed (no vga_scaler, no direct_video) and the front-end
// would draw to something nothing displays. A no-op when the scaler output
// already reaches the screen. Safe to call every frame; releasing restores the
// video mode. Resizes the framebuffer to the TV mode, so re-read
// video_menu_fb_width/height() afterwards.
void video_menu_fb_analog(int on);

int video_bg_has_picture();
int video_chvt(int num);
void video_cmd(char *cmd);

/*
  Set the mode from anything MiSTer.ini's video_mode= accepts - a predefined number, a
  modeline, or a calculated width,height,refresh. video_mode_restore() puts back whatever
  the configuration says, which is the undo a front-end offering a mode has to have: see
  the notes on both in video.cpp.
*/
void video_mode_cmd(char *cmd);
void video_mode_restore();

void video_core_description(char *str, size_t len);
void video_scaler_description(char *str, size_t len);
char* video_get_core_mode_name(int with_vrefresh = 1);

void dbg_draw_cursor(int x, int y);

#endif // VIDEO_H
