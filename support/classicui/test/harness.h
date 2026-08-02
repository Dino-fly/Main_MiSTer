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
uint32_t harness_opt_val(const char *opt);
void harness_set_opt(const char *opt, uint32_t v);
void harness_reset_status();

uint32_t *harness_fb_shown();
unsigned long harness_fb_hash(int y0, int y1);
unsigned long harness_fb_hash_box(int x0, int y0, int x1, int y1);
void harness_set_pad_name(const char *n);
void harness_set_pad_vidpid(uint32_t v);
void harness_swap_pad_faces();

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
