#ifndef CHEATS_H
#define CHEATS_H

void cheats_init(const char *rom_path, uint32_t romcrc);
int cheats_available();
void cheats_scan(int mode);
void cheats_scroll_name();
void cheats_print();
void cheats_toggle();
int cheats_loaded();

void cheats_init_arcade(int unit_size, int max_active);
void cheats_add_arcade(const char *name, const char *cheatData, int cheatSize);
void cheats_finalize_arcade();

/*
  Enumeration, for a front-end that is not the OSD.

  Everything above is written around one cursor and one 32-column buffer:
  cheats_print() draws into the OSD, cheats_scan() moves that cursor and
  cheats_toggle() acts on wherever it happens to be. None of that can be reused
  by a menu with its own layout, so these read the store by index and leave the
  cursor where they found it. Additive only - the OSD path is untouched.

  cheats_set_enabled() can be refused: a cheat is a whole number of cheat_unit_size
  lines and the core takes cheat_max_lines() of them, so enabling one when the
  budget is spent does nothing. It returns what the store says afterwards rather
  than what was asked for, so a caller can say so instead of appearing to ignore
  the press. See support/classicui/UPSTREAM.md.
*/
const char *cheats_name(int idx);
int cheats_is_enabled(int idx);
int cheats_set_enabled(int idx, int on);
int cheats_active();
int cheats_max_lines();

#endif
