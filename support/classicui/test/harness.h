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
void harness_set_fb_supported(int v);
void harness_set_confstr(int v);
const char *harness_last_status_opt();
const char *harness_last_pulse_opt();
const char *harness_last_rbf();
int harness_status_pulses();
uint32_t harness_pause_val();
void harness_reset_status();

uint32_t *harness_fb_shown();
int harness_present_count();

const char *harness_last_launch();
const char *harness_last_preset();
void harness_clear_launch();

#endif
