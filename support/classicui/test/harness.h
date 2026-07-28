#ifndef CHOME_HARNESS_H
#define CHOME_HARNESS_H

#include <inttypes.h>

void harness_advance(unsigned long ms);
unsigned long harness_now();

void harness_set_root(const char *r);
void harness_set_fb(int w, int h);
void harness_set_scaler_visible(int v);

uint32_t *harness_fb_shown();
int harness_present_count();

const char *harness_last_launch();
const char *harness_last_preset();
void harness_clear_launch();

#endif
