#ifndef UI_H
#define UI_H

#include "xc.h"

void ui_init(void);
void draw_panel(Panel *p, int start_y, int start_x, int width, int height, int is_active);
void draw_fkey_bar(int y, int width);
void draw_screen(void);
int ui_confirm(const char *title, const char *msg);
int ui_multiline_paste(const char *text);
void ui_message(const char *title, const char *msg);
int ui_input(const char *title, const char *prompt, char *buf, int max_len);
void ui_help(void);
void ui_user_menu(void);

#endif
