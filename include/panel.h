#ifndef PANEL_H
#define PANEL_H

#include "xc.h"

void load_directory(Panel *p);
void change_directory(Panel *p, const char *new_dir);
int panel_marked_count(const Panel *p);
void panel_toggle_mark(Panel *p, int index);
void panel_select_mask(Panel *p, const char *mask, int select_mode);
void panel_invert_selection(Panel *p);
void temp_panel_open(Panel *p);
int temp_panel_add(const char *full_path);
void temp_panel_clear(void);
int temp_panel_remove_selected(Panel *p);
void panel_entry_full_path(const Panel *p, int index, char *out, size_t sz);

extern int g_show_hidden;
void panel_compare_dirs(Panel *left, Panel *right);

#endif
