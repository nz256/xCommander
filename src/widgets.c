#define _XOPEN_SOURCE 700
#include <stdlib.h>
#include "widgets.h"
#include "exec.h"
#include <string.h>
#include <wctype.h>

UiTheme g_ui_theme;

void ui_theme_init_default(void) {
	g_ui_theme.win_bg         = COLOR_PAIR(5);
	g_ui_theme.win_border     = COLOR_PAIR(5);
	g_ui_theme.win_title      = COLOR_PAIR(5);
	g_ui_theme.input_attr     = COLOR_PAIR(2);
	g_ui_theme.input_sel_attr = COLOR_PAIR(6);
	g_ui_theme.btn_normal     = COLOR_PAIR(5);
	g_ui_theme.btn_focus      = COLOR_PAIR(2);
	g_ui_theme.list_sel       = COLOR_PAIR(3);
}

static int tf_is_word_sep(wchar_t ch) {
	return iswspace(ch) || ch == L'/' || ch == L'-' || ch == L'_' || ch == L'.' || ch == L';';
}

static int tf_word_left(const wchar_t *buf, int pos) {
	while (pos > 0 && tf_is_word_sep(buf[pos - 1])) pos--;
	while (pos > 0 && !tf_is_word_sep(buf[pos - 1])) pos--;
	return pos;
}

static int tf_word_right(const wchar_t *buf, int pos, int len) {
	while (pos < len && !tf_is_word_sep(buf[pos])) pos++;
	while (pos < len && tf_is_word_sep(buf[pos])) pos++;
	return pos;
}

static void tf_delete_selection(UiTextField *tf) {
	if (tf->anchor == -1 || tf->anchor == tf->pos) return;
	int s1 = (tf->anchor < tf->pos) ? tf->anchor : tf->pos;
	int s2 = (tf->anchor > tf->pos) ? tf->anchor : tf->pos;
	wmemmove(tf->buf + s1, tf->buf + s2, (size_t)(tf->len - s2 + 1));
	tf->len -= (s2 - s1);
	tf->pos = s1;
	tf->anchor = -1;
}

void ui_tf_init(UiTextField *tf, wchar_t *buf, int cap, int select_all) {
	tf->buf = buf;
	tf->cap = cap;
	tf->len = (int)wcslen(buf);
	tf->pos = tf->len;
	tf->anchor = (select_all && tf->len > 0) ? 0 : -1;
	tf->y = 0;
	tf->x = 0;
	tf->width = 10;
}

void ui_tf_set_bounds(UiTextField *tf, int y, int x, int width) {
	tf->y = y;
	tf->x = x;
	tf->width = (width < 1) ? 1 : width;
}

void ui_tf_draw(const UiTextField *tf, WINDOW *win, int active) {
	int off = (tf->pos >= tf->width) ? (tf->pos - tf->width + 1) : 0;
	wattron(win, g_ui_theme.input_attr);
	mvwhline(win, tf->y, tf->x, ' ', tf->width);
	mvwaddnwstr(win, tf->y, tf->x, tf->buf + off, tf->width);
	wattroff(win, g_ui_theme.input_attr);

	if (active && tf->anchor != -1 && tf->anchor != tf->pos) {
		int s1 = (tf->anchor < tf->pos) ? tf->anchor : tf->pos;
		int s2 = (tf->anchor > tf->pos) ? tf->anchor : tf->pos;
		if (s1 < off) s1 = off;
		if (s2 > off + tf->width) s2 = off + tf->width;
		if (s2 > s1) {
			wattron(win, g_ui_theme.input_sel_attr);
			mvwaddnwstr(win, tf->y, tf->x + (s1 - off), tf->buf + s1, s2 - s1);
			wattroff(win, g_ui_theme.input_sel_attr);
		}
	}
}

void ui_tf_place_cursor(const UiTextField *tf, WINDOW *win) {
	int off = (tf->pos >= tf->width) ? (tf->pos - tf->width + 1) : 0;
	curs_set(1);
	wmove(win, tf->y, tf->x + (tf->pos - off));
}

int ui_tf_handle_mouse(UiTextField *tf, int wy, int wx) {
	if (wy != tf->y || wx < tf->x || wx >= tf->x + tf->width) return 0;
	int off = (tf->pos >= tf->width) ? (tf->pos - tf->width + 1) : 0;
	int np = off + (wx - tf->x);
	tf->pos = (np > tf->len) ? tf->len : np;
	tf->anchor = -1;
	return 1;
}

int ui_tf_handle_event(UiTextField *tf, const InputEvent *ev) {
	if (ev->action == ACT_WORD_SEL_LEFT) {
		if (tf->anchor == -1) tf->anchor = tf->pos;
		tf->pos = tf_word_left(tf->buf, tf->pos);
		return 1;
	}
	if (ev->action == ACT_WORD_SEL_RIGHT) {
		if (tf->anchor == -1) tf->anchor = tf->pos;
		tf->pos = tf_word_right(tf->buf, tf->pos, tf->len);
		return 1;
	}
	if (ev->action == ACT_SHIFT_LEFT) {
		if (tf->anchor == -1) tf->anchor = tf->pos;
		if (tf->pos > 0) tf->pos--;
		return 1;
	}
	if (ev->action == ACT_SHIFT_RIGHT) {
		if (tf->anchor == -1) tf->anchor = tf->pos;
		if (tf->pos < tf->len) tf->pos++;
		return 1;
	}
	if (ev->action == ACT_SHIFT_HOME) {
		if (tf->anchor == -1) tf->anchor = tf->pos;
		tf->pos = 0;
		return 1;
	}
	if (ev->action == ACT_SHIFT_END) {
		if (tf->anchor == -1) tf->anchor = tf->pos;
		tf->pos = tf->len;
		return 1;
	}
	if (ev->action == ACT_CTRL_LEFT) { tf->anchor = -1; tf->pos = tf_word_left(tf->buf, tf->pos); return 1; }
	if (ev->action == ACT_CTRL_RIGHT) { tf->anchor = -1; tf->pos = tf_word_right(tf->buf, tf->pos, tf->len); return 1; }
	if (ev->action == ACT_LEFT) { tf->anchor = -1; if (tf->pos > 0) tf->pos--; return 1; }
	if (ev->action == ACT_RIGHT) { tf->anchor = -1; if (tf->pos < tf->len) tf->pos++; return 1; }
	if (ev->action == ACT_HOME || ev->action == ACT_CTRL_A) { tf->anchor = -1; tf->pos = 0; return 1; }
	if (ev->action == ACT_END) { tf->anchor = -1; tf->pos = tf->len; return 1; }
	if (ev->action == ACT_CTRL_U) { tf->buf[0] = L'\0'; tf->len = 0; tf->pos = 0; tf->anchor = -1; return 1; }

	if (ev->action == ACT_BACKSPACE) {
		if (tf->anchor != -1 && tf->anchor != tf->pos) tf_delete_selection(tf);
		else if (tf->pos > 0) {
			wmemmove(tf->buf + tf->pos - 1, tf->buf + tf->pos, (size_t)(tf->len - tf->pos + 1));
			tf->pos--; tf->len--;
		}
		return 1;
	}
	if (ev->action == ACT_DELETE) {
		if (tf->anchor != -1 && tf->anchor != tf->pos) tf_delete_selection(tf);
		else if (tf->pos < tf->len) {
			wmemmove(tf->buf + tf->pos, tf->buf + tf->pos + 1, (size_t)(tf->len - tf->pos));
			tf->len--;
		}
		return 1;
	}
	if (ev->action == ACT_CTRL_C || ev->action == ACT_CTRL_Y || ev->action == ACT_CTRL_X) {
		int has_sel = (tf->anchor != -1 && tf->anchor != tf->pos);
		int s1 = has_sel ? ((tf->anchor < tf->pos) ? tf->anchor : tf->pos) : 0;
		int s2 = has_sel ? ((tf->anchor > tf->pos) ? tf->anchor : tf->pos) : tf->len;
		if (s2 > s1) {
			wchar_t wsub[1024];
			int n = (s2 - s1 < 1023) ? (s2 - s1) : 1023;
			wmemcpy(wsub, tf->buf + s1, (size_t)n);
			wsub[n] = L'\0';
			char utf[2048];
			if (wcstombs(utf, wsub, sizeof(utf)) != (size_t)-1) {
				utf[sizeof(utf) - 1] = '\0';
				clipboard_copy(utf);
			}
			if (ev->action == ACT_CTRL_X && has_sel) tf_delete_selection(tf);
		}
		return 1;
	}
	if (ev->action == ACT_CTRL_V || ev->action == ACT_SHIFT_INSERT) {
		tf_delete_selection(tf);
		char raw[1024] = {0};
		if (clipboard_paste(raw, sizeof(raw)) > 0) {
			wchar_t wadd[1024];
			size_t alen = mbstowcs(wadd, raw, 1023);
			if (alen != (size_t)-1 && alen > 0) {
				if (tf->len + (int)alen > tf->cap) alen = (size_t)(tf->cap - tf->len);
				if (alen > 0) {
					wmemmove(tf->buf + tf->pos + alen, tf->buf + tf->pos, (size_t)(tf->len - tf->pos + 1));
					wmemcpy(tf->buf + tf->pos, wadd, alen);
					tf->pos += (int)alen;
					tf->len += (int)alen;
				}
			}
		}
		return 1;
	}
	if (ev->action == ACT_CHAR && iswprint(ev->ch)) {
		tf_delete_selection(tf);
		if (tf->len < tf->cap) {
			wmemmove(tf->buf + tf->pos + 1, tf->buf + tf->pos, (size_t)(tf->len - tf->pos + 1));
			tf->buf[tf->pos] = ev->ch;
			tf->pos++;
			tf->len++;
		}
		return 1;
	}
	return 0;
}

void ui_btns_draw(const UiButtonRow *br, WINDOW *win) {
	int x = br->start_x;
	for (int i = 0; i < br->count; i++) {
		int attr = (br->focus_idx == i) ? g_ui_theme.btn_focus : g_ui_theme.btn_normal;
		wattron(win, attr);
		mvwprintw(win, br->y, x, "%s", br->labels[i]);
		wattroff(win, attr);
		x += (int)strlen(br->labels[i]) + br->gap;
	}
}

int ui_btns_hit_test(const UiButtonRow *br, int wy, int wx) {
	if (wy != br->y) return -1;
	int x = br->start_x;
	for (int i = 0; i < br->count; i++) {
		int w = (int)strlen(br->labels[i]);
		if (wx >= x && wx < x + w) return i;
		x += w + br->gap;
	}
	return -1;
}

void ui_cb_draw(const UiCheckbox *cb, WINDOW *win, int focused) {
	int attr = focused ? g_ui_theme.btn_focus : g_ui_theme.btn_normal;
	wattron(win, attr);
	mvwprintw(win, cb->y, cb->x, "[%c] %s", (cb->checked && *cb->checked) ? 'x' : ' ', cb->label);
	wattroff(win, attr);
}

int ui_cb_hit_test(const UiCheckbox *cb, int wy, int wx) {
	if (wy != cb->y) return 0;
	int w = 4 + (int)strlen(cb->label);
	return (wx >= cb->x && wx < cb->x + w);
}
