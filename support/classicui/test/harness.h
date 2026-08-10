#ifndef CHOME_HARNESS_H
#define CHOME_HARNESS_H

#include <inttypes.h>

void harness_advance(unsigned long ms);
void harness_use_real_clock(int on);
int  harness_sim_launched();
unsigned long harness_now();

void harness_set_root(const char *r);
void harness_set_fb(int w, int h);
void harness_set_scaler_visible(int v);
void harness_set_menu_core(int v);
void harness_set_core_name(const char *n);
void harness_set_input_pad(int v);
void harness_clear_pads();
void harness_add_pad(int player, int kind, uint16_t vid, uint16_t pid, const char *name, const char *mac);
void harness_clear_pad_state();
void harness_set_pad_state(int player, uint32_t held, const uint16_t *codes, int sticks, int lx, int ly);
void harness_set_fb_supported(int v);
void harness_set_confstr(int v);
const char *harness_last_status_opt();
const char *harness_last_pulse_opt();
const char *harness_last_rbf();
int harness_status_pulses();
int harness_pulses_on(const char *opt);
uint32_t harness_pause_val();
/*
  ex defaults to 0, which is right for every "O"-form and bracket-form spec - almost all
  of them. Pass 1 for an option the core wrote as "o<letters>": those live 32 bits higher
  and are a different slot here, exactly as they are a different slot on the device.
*/
uint32_t harness_opt_val(const char *opt, int ex = 0);
void harness_set_opt(const char *opt, uint32_t v, int ex = 0);
void harness_reset_status();

uint32_t *harness_fb_shown();
unsigned long harness_fb_hash(int y0, int y1);
unsigned long harness_fb_hash_box(int x0, int y0, int x1, int y1);
unsigned long harness_fb_hash_box_except(int x0, int y0, int x1, int y1,
	int ex0, int ey0, int ex1, int ey1);
void harness_set_pad_name(const char *n);
void harness_set_pad_vidpid(uint32_t v);
void harness_set_osd_visible(int v);
void harness_set_osd_mask(uint16_t m);
int harness_cfg_saves();
int harness_osd_status_held();
const char *harness_last_recent();
int harness_recent_calls();
unsigned int harness_last_menu_key();
void harness_swap_pad_faces();

/*
  Make screenshot_grab() refuse, the way it does on a core with no scaler output.

  Not to test the refusal itself - the front-end already survives it - but to measure what
  the still of the game contributes to a frame, by composing the same screen twice and
  differencing. That is the only way to answer "is this screen showing the game or not"
  without an eye on a television, and it is the question a black in-game background over a
  physical disc turned out to hinge on.
*/
void harness_set_grab(int ok);

/*
  Make the grab hand back one flat colour instead of the drawn scene. 0 puts the scene back.

  This is how "is the game on screen here" is answered by counting rather than by looking: a
  colour nothing in the front-end draws goes in, and every pixel of it that comes out the
  other end is a pixel of the still that survived the dim, the fit and everything drawn on
  top. Zero of them is the black background this was written for.
*/
void harness_set_grab_flat(uint32_t argb);

int harness_muted();
int harness_mute_changes();
void harness_set_muted(int v);

int harness_present_count();
int harness_fb_analog();
int harness_analog_claims();
void harness_reset_analog_claims();
int harness_neogeo_scanned();
int harness_ss_copy_from();
int harness_ss_copy_to();
void harness_reset_ss_copy();

const char *harness_last_launch();
const char *harness_last_preset();
void harness_clear_launch();

#endif
