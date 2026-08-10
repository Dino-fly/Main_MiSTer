// cfg.h
// 2015, rok.krajnc@gmail.com
// 2017+, Sorgelig

#ifndef __CFG_H__
#define __CFG_H__

#include <inttypes.h>

//// type definitions ////
typedef struct {
	uint32_t keyrah_mode;
	uint8_t forced_scandoubler;
	uint8_t key_menu_as_rgui;
	uint8_t reset_combo;
	uint8_t csync;
	uint8_t vga_scaler;
	uint8_t vga_sog;
	uint8_t hdmi_audio_96k;
	uint8_t dvi_mode;
	uint8_t hdmi_limited;
	uint8_t hdmi_cec;
	uint8_t hdmi_cec_sleep;
	uint8_t hdmi_cec_wake;
	uint8_t hdmi_cec_input_mode;
	uint8_t hdmi_cec_power_on;
	float hdmi_cec_clock;
	uint8_t direct_video;
	uint8_t video_info;
	float refresh_min;
	float refresh_max;
	uint8_t controller_info;
	uint8_t menu_player;
	uint8_t vsync_adjust;
	uint8_t kbd_nomouse;
	uint8_t mouse_throttle;
	uint8_t bootscreen;
	uint8_t vscale_mode;
	uint16_t vscale_border;
	uint8_t rbf_hide_datecode;
	uint8_t menu_pal;
	int16_t bootcore_timeout;
	uint8_t fb_size;
	uint8_t fb_terminal;
	uint8_t fb_terminal_vga;
	uint8_t osd_rotate;
	uint16_t osd_timeout;
	uint8_t gamepad_defaults;
	uint8_t recents;
	uint16_t jamma_vid;
	uint16_t jamma_pid;
	uint16_t jamma2_vid;
	uint16_t jamma2_pid;
	uint16_t no_merge_vid;
	uint16_t no_merge_pid;
	uint32_t no_merge_vidpid[256];
	uint16_t spinner_vid;
	uint16_t spinner_pid;
	int spinner_throttle;
	uint8_t spinner_axis;
	uint8_t sniper_mode;
	uint8_t browse_expand;
	uint8_t logo;
	uint8_t log_file_entry;
	uint8_t shmask_mode_default;
	int bt_auto_disconnect;
	int bt_reset_before_pair;
	char bootcore[256];
	char video_conf[1024];
	char video_conf_pal[1024];
	char video_conf_ntsc[1024];
	char font[1024];
	char shared_folder[1024];
	char waitmount[1024];
	char custom_aspect_ratio[2][16];
	char afilter_default[1023];
	char vfilter_default[1023];
	char vfilter_vertical_default[1023];
	char vfilter_scanlines_default[1023];
	char shmask_default[1023];
	char preset_default[1023];
	char player_controller[6][8][256];
	char controller_deadzone[32][256];
	uint8_t rumble;
	uint8_t snac_pad;
	uint8_t snac_psx;
	uint8_t snac_psx_fallback;
	uint8_t snac_psx_memcard;
	uint8_t wheel_force;
	uint16_t wheel_range;
	uint8_t hdmi_game_mode;
	uint8_t vrr_mode;
	uint8_t vrr_vesa_framerate;
	uint16_t video_off;
	uint8_t video_off_logo;
	uint8_t disable_autofire;
	uint8_t video_brightness;
	uint8_t video_contrast;
	uint8_t video_saturation;
	uint16_t video_hue;
	char video_gain_offset[256];
	uint8_t hdr;
	uint16_t hdr_max_nits;
	uint16_t hdr_avg_nits;
	char vga_mode[16];
	char vga_mode_int;
	char ntsc_mode;
	uint32_t controller_unique_mapping[256];
	char osd_lock[25];
	uint16_t osd_lock_time;
	char debug;
	uint8_t lookahead;
	char main[1024];
	char vfilter_interlace_default[1023];
	char autofire_rates[3072];
	uint8_t autofire_on_directions;
	char screenshot_image_format[16];
	uint16_t xbe2_shift;
	uint8_t spd_quirk;
	uint16_t hdmi_off;
	uint32_t keyboard_as_joystick[256];
	uint8_t classicui;
	uint8_t classicui_artfetch;
	uint8_t classicui_gamelist;
	uint8_t classicui_freeze;
	uint8_t classicui_profile;
	uint8_t classicui_overscan;
	/*
	  Letter spacing, in font pixels, added to the 8-pixel advance of the OSD ROM font.
	  Signed, so it is an int8_t and not a uint8_t like everything else here - which is
	  why the front-end's option table needed a signed pointer of its own rather than
	  reusing the uint8_t one; see the comment on opt_def in support/classicui/chome_opt.h.
	*/
	int8_t classicui_tracking;
	// 1 (default) - titles and labels are shouted in capitals, as they always were.
	uint8_t classicui_caps;
	/*
	  1 (default) - the front-end asks for the menu framebuffer at half the display
	  mode on each axis, a quarter of the pixels to compose and copy, and the scaler
	  upscales it. See video_fb_size_request() in video.cpp for what can refuse the
	  request, and theme_update() in support/classicui/chome_theme.cpp for how the
	  layout keeps the display's profile rather than the shrunken canvas's.
	*/
	uint8_t classicui_halfres;
	// Inert until this build carries a ScreenScraper devid; see chome_ss.h.
	uint8_t classicui_screenscraper;

	/*
	  Retry ScreenScraper for the covers this front-end downloaded from the libretro
	  thumbnail pack, once each. OFF, and only ever set by hand.

	  For the player who entered their own account after their card had already been filled
	  from the pack, and who would otherwise keep that pack art for ever - a fetched cover is
	  found on the card at rung one and never looked for again. It applies to nothing else:
	  no gamelist.xml cover, no scrape made with another tool, no pack installed by hand. See
	  art_pack_marked() in support/classicui/chome_art.h for where the provenance is kept and
	  why this is a setting rather than something that happens by itself.
	*/
	uint8_t classicui_ss_replace_pack;
	// OFF by default: see support/classicui/chome_disc.h.
	uint8_t classicui_disc;
	char classicui_artdir[256];
	char classicui_arturl[512];
	char classicui_ss_user[64];
	char classicui_ss_pass[64];
} cfg_t;

extern cfg_t cfg;

//// functions ////
void cfg_parse();
void cfg_print();
const char* cfg_get_name(uint8_t alt);
const char* cfg_get_label(uint8_t alt);
bool cfg_has_video_sections();

void cfg_error(const char *fmt, ...);
bool cfg_check_errors(char *msg, size_t max_len);

/*
  Read-only access to the option table itself, so that code outside this file can ask
  "is this a real option?" and "what did it end up as?" without keeping a second list
  of option names beside cfg.cpp's. The configuration check in
  support/classicui/chome_cfgrec.h needs both, and a hand-copied list there would be
  wrong the day an option is added - reporting a brand new setting as a typo.

  cfg_var_count() is the length of the table; cfg_var_name(i) is the name as the table
  spells it (upper case, matched case-insensitively against the file);
  cfg_var_text(i, ...) formats the value the variable currently holds, always - unlike
  cfg_print(), which suppresses empty strings and empty arrays. A report has to be able
  to say that classicui_ss_user is empty, so it cannot use a printer that says nothing
  in that case. Returns out.
*/
int cfg_var_count();
const char *cfg_var_name(int i);
const char *cfg_var_text(int i, char *out, int max);

struct yc_mode
{
	char key[64];
	int64_t phase_inc;
};

void yc_parse(yc_mode *yc_table, int max);

#endif // __CFG_H__
