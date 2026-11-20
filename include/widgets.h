#ifndef WIDGETS_H
#define WIDGETS_H

#include <ncurses.h>
#include <wchar.h>
#include "input.h"

typedef struct {
	int win_bg;
	int win_border;
	int win_title;
	int input_attr;
	int input_sel_attr;
	int btn_normal;
	int btn_focus;
	int list_sel;
} UiTheme;

extern UiTheme g_ui_theme;
void ui_theme_init_default(void);

typedef struct {
	wchar_t *buf;
	int cap;       /* максимальная длина в символах без '\0' */
	int len;
	int pos;
	int anchor;    /* -1 если выделения нет */
	int y;
	int x;
	int width;
} UiTextField;

void ui_tf_init(UiTextField *tf, wchar_t *buf, int cap, int select_all);
void ui_tf_set_bounds(UiTextField *tf, int y, int x, int width);
void ui_tf_draw(const UiTextField *tf, WINDOW *win, int active);
void ui_tf_place_cursor(const UiTextField *tf, WINDOW *win);
int  ui_tf_handle_mouse(UiTextField *tf, int wy, int wx);
int  ui_tf_handle_event(UiTextField *tf, const InputEvent *ev);

typedef struct {
	const char **labels;
	int count;
	int y;
	int start_x;
	int gap;
	int focus_idx; /* -1 если фокус не на кнопках */
} UiButtonRow;

void ui_btns_draw(const UiButtonRow *br, WINDOW *win);
int  ui_btns_hit_test(const UiButtonRow *br, int wy, int wx);

typedef struct {
	const char *label;
	int *checked;
	int y;
	int x;
} UiCheckbox;

void ui_cb_draw(const UiCheckbox *cb, WINDOW *win, int focused);
int  ui_cb_hit_test(const UiCheckbox *cb, int wy, int wx);

#endif
