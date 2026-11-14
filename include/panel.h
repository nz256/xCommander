#ifndef PANEL_H
#define PANEL_H

#include "xc.h"

void load_directory(Panel *p);
void change_directory(Panel *p, const char *new_dir);
int panel_marked_count(const Panel *p);
void panel_toggle_mark(Panel *p, int index);

#endif
