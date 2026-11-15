#define _GNU_SOURCE
#include <ncurses.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>
#include "ui.h"
#include "exec.h"
#include "panel.h"

static int build_cmd_prompt(char *out, int out_size, int max_cols) {
	const char *path = (active_panel && *active_panel->current_path) ? active_panel->current_path : ".";
	wchar_t wpath[MAX_PATH];
	size_t wlen = mbstowcs(wpath, path, MAX_PATH - 1);

	if (wlen == (size_t)-1) {
		snprintf(out, (size_t)out_size, "> ");
		return 2;
	}
	wpath[wlen] = L'\0';

	/* Оставляем место минимум под 15 символов самой команды и "> " */
	int limit = max_cols - 18;
	if (limit < 6) limit = 6;

	if ((int)wlen > limit) {
		wchar_t wshort[MAX_PATH];
		swprintf(wshort, MAX_PATH, L"...%ls", wpath + (wlen - (size_t)(limit - 3)));
		wcstombs(out, wshort, (size_t)out_size);
		out[out_size - 1] = '\0';
		strcat(out, "> ");
		return limit + 2;
	} else {
		snprintf(out, (size_t)out_size, "%s> ", path);
		return (int)wlen + 2;
	}
}


static void draw_win_box(WINDOW *win) {
	int h, w;
	getmaxyx(win, h, w);

	for (int i = 1; i < w - 1; i++) {
		mvwaddstr(win, 0, i, "─");
		mvwaddstr(win, h - 1, i, "─");
	}
	for (int i = 1; i < h - 1; i++) {
		mvwaddstr(win, i, 0, "│");
		mvwaddstr(win, i, w - 1, "│");
	}
	mvwaddstr(win, 0, 0, "┌");
	mvwaddstr(win, 0, w - 1, "┐");
	mvwaddstr(win, h - 1, 0, "└");
	mvwaddstr(win, h - 1, w - 1, "┘");
}
static void mvwprint_utf8(WINDOW *win, int y, int x, int max_cols, const char *str) {
	if (max_cols <= 0 || !str) return;
	wchar_t wbuf[MAX_PATH];
	size_t len = mbstowcs(wbuf, str, MAX_PATH - 1);
	if (len == (size_t)-1) {
		mvwprintw(win, y, x, "%.*s", max_cols, str);
		return;
	}
	if ((int)len > max_cols) {
		len = (size_t)max_cols;
	}
	wbuf[len] = L'\0';

	char utf8_out[MAX_PATH * 4];
	size_t bytes = wcstombs(utf8_out, wbuf, sizeof(utf8_out) - 1);
	if (bytes != (size_t)-1) {
		utf8_out[bytes] = '\0';
		mvwaddstr(win, y, x, utf8_out);
	}
}

void ui_init(void) {
	initscr();
	start_color();
	raw();
	system("stty lnext undef");
	noecho();
	keypad(stdscr, TRUE);
	curs_set(0);
	set_escdelay(25);

	init_pair(1, COLOR_CYAN, COLOR_BLUE);     /* Рамки и обычные файлы (ярко-голубой с A_BOLD) */
	init_pair(2, COLOR_BLACK, COLOR_CYAN);    /* Чёрный текст на бирюзовом (курсор обычных файлов, F-клавиши) */
	init_pair(3, COLOR_YELLOW, COLOR_BLUE);   /* Выделенные файлы (ярко-жёлтый с A_BOLD) */
	init_pair(4, COLOR_YELLOW, COLOR_CYAN);   /* Выделенные файлы под курсором */
	init_pair(5, COLOR_BLACK, COLOR_WHITE);   /* Модальные окна */
	init_pair(6, COLOR_WHITE, COLOR_BLACK);   /* Командная строка и цифры F-клавиш */
	init_pair(7, COLOR_WHITE, COLOR_BLUE);    /* Каталоги (ярко-белый с A_BOLD) */
	init_pair(8, COLOR_GREEN, COLOR_BLUE);    /* Исполняемые файлы (ярко-зелёный с A_BOLD) */
	init_pair(9, COLOR_WHITE, COLOR_CYAN);    /* Каталоги под курсором */
	init_pair(10, COLOR_GREEN, COLOR_CYAN);   /* Исполняемые файлы под курсором */
	init_pair(11, COLOR_CYAN, COLOR_CYAN);    /* Обычные файлы под курсором (ярко-голубой) */

	bkgd(COLOR_PAIR(1));
}

void draw_fkey_bar(int y, int width) {
	const char *labels[] = {
		"Help", "UserMn", "View", "Edit", "Copy",
		"RenMov", "MkFold", "Delete", "ConfMn", "Quit"
	};

	attrset(COLOR_PAIR(6));
	mvhline(y, 0, ' ', width);

	int col_w = width / 10;
	if (col_w < 3) col_w = 3;

	for (int i = 0; i < 10; i++) {
		int x = i * col_w;
		if (x >= width) break;

		int item_w = (i == 9) ? (width - x) : col_w;

		attrset(COLOR_PAIR(6));
		mvprintw(y, x, "%d", i + 1);

		int num_len = (i == 9) ? 2 : 1;
		int label_space = item_w - num_len;
		if (label_space > 0) {
			attrset(A_NORMAL | COLOR_PAIR(2));
			for (int k = 0; k < label_space; k++) mvaddch(y, x + num_len + k, ' ');
			mvprintw(y, x + num_len, "%.*s", label_space - (label_space > 1 ? 1 : 0), labels[i]);
		}
	}
	attrset(A_NORMAL);
}

void draw_panel(Panel *p, int start_y, int start_x, int width, int height, int is_active) {
	int normal_attr = COLOR_PAIR(1) | A_BOLD;
	int dir_attr = COLOR_PAIR(7) | A_BOLD;
	int exec_attr = COLOR_PAIR(8) | A_BOLD;
	int mark_attr = COLOR_PAIR(3) | A_BOLD;

	int cur_normal_attr = COLOR_PAIR(11) | A_BOLD;
	int cur_dir_attr = COLOR_PAIR(9) | A_BOLD;
	int cur_exec_attr = COLOR_PAIR(10) | A_BOLD;
	int cur_mark_attr = COLOR_PAIR(4) | A_BOLD;

	attrset(normal_attr);

	int inner_w = width - 2;
	int col1_w = inner_w / 2;
	int col2_w = inner_w - col1_w - 1;
	int mid_x = start_x + 1 + col1_w;

	for (int i = 1; i < width - 1; i++) {
		mvaddstr(start_y, start_x + i, "─");
		mvaddstr(start_y + height - 1, start_x + i, "─");
	}
	mvaddstr(start_y, mid_x, "┬");
	mvaddstr(start_y + height - 1, mid_x, "┴");

	for (int i = 1; i < height - 1; i++) {
		mvaddstr(start_y + i, start_x, "│");
		mvaddstr(start_y + i, mid_x, "│");
		mvaddstr(start_y + i, start_x + width - 1, "│");
	}

	mvaddstr(start_y, start_x, "┌");
	mvaddstr(start_y, start_x + width - 1, "┐");
	mvaddstr(start_y + height - 1, start_x, "└");
	mvaddstr(start_y + height - 1, start_x + width - 1, "┘");

	/* Заголовок пути в верхней рамке */
	int max_path_len = width - 6;
	if (max_path_len < 1) max_path_len = 1;
	char short_path[MAX_PATH];
	snprintf(short_path, (size_t)max_path_len + 1, "%s", p->current_path);

	int path_len = (int)mbstowcs(NULL, short_path, 0);
	if (path_len < 0) path_len = (int)strlen(short_path);

	int title_w = path_len + 2;
	int title_x = start_x + (width - title_w) / 2;
	if (title_x < start_x + 1) title_x = start_x + 1;

	attrset(is_active ? (A_NORMAL | COLOR_PAIR(2)) : (attr_t)normal_attr);
	mvprintw(start_y, title_x, " %s ", short_path);
	attrset(normal_attr);

	int visible_lines = height - 2;
	if (visible_lines < 1) visible_lines = 1;
	int page_capacity = visible_lines * 2;

	if (p->selected < p->top_line) {
		p->top_line = p->selected;
	}
	if (p->selected >= p->top_line + page_capacity) {
		p->top_line = p->selected - page_capacity + 1;
	}

	for (int col = 0; col < 2; col++) {
		int col_x = (col == 0) ? (start_x + 1) : (mid_x + 1);
		int col_w = (col == 0) ? col1_w : col2_w;
		if (col_w < 1) continue;

		for (int row = 0; row < visible_lines; row++) {
			int file_idx = p->top_line + col * visible_lines + row;
			int y = start_y + 1 + row;
			int is_cursor = (is_active && file_idx == p->selected && file_idx < p->count);
			int is_marked = (file_idx < p->count && p->files[file_idx].is_selected);
			int is_dir = (file_idx < p->count && p->files[file_idx].is_dir);
			int is_exec = (file_idx < p->count && p->files[file_idx].is_exec);
			int item_attr = normal_attr;

			if (is_cursor) {
				if (is_marked) item_attr = cur_mark_attr;
				else if (is_dir) item_attr = cur_dir_attr;
				else if (is_exec) item_attr = cur_exec_attr;
				else item_attr = cur_normal_attr;
			} else {
				if (is_marked) item_attr = mark_attr;
				else if (is_dir) item_attr = dir_attr;
				else if (is_exec) item_attr = exec_attr;
				else item_attr = normal_attr;
			}

			attrset(item_attr);
			for (int k = 0; k < col_w; k++) {
				mvaddch(y, col_x + k, ' ');
			}

			if (file_idx < p->count && col_w > 1) {
				mvwprint_utf8(stdscr, y, col_x + 1, col_w - 1, p->files[file_idx].name);
			}
		}
	}

	attrset(A_NORMAL);
}

void draw_screen(void) {
	erase();
	int height, width;
	getmaxyx(stdscr, height, width);

	int panel_width = width / 2;
	int panel_height = height - 2;
	if (panel_height < 3) panel_height = 3;

	draw_panel(&left_panel, 0, 0, panel_width, panel_height, active_panel == &left_panel);
	draw_panel(&right_panel, 0, panel_width, width - panel_width, panel_height, active_panel == &right_panel);

	char prompt[MAX_PATH + 16];
	int prompt_cols = build_cmd_prompt(prompt, sizeof(prompt), width);

	attrset(COLOR_PAIR(6));
	mvhline(height - 2, 0, ' ', width);
	mvprintw(height - 2, 0, "%s%s", prompt, cmd_buffer);
	attrset(A_NORMAL);

	if (cmd_sel_start >= 0 && cmd_sel_end > cmd_sel_start) {
		wchar_t wtmp[MAX_CMD];
		size_t wl = mbstowcs(wtmp, cmd_buffer, MAX_CMD - 1);
		if (wl != (size_t)-1 && cmd_sel_end <= (int)wl) {
			wtmp[cmd_sel_end] = L'\0';
			char sub_utf[MAX_CMD];
			if (wcstombs(sub_utf, wtmp + cmd_sel_start, sizeof(sub_utf)) != (size_t)-1) {
				attron(COLOR_PAIR(2));
				mvprintw(height - 2, prompt_cols + cmd_sel_start, "%s", sub_utf);
				attroff(COLOR_PAIR(2));
			}
		}
	}

	draw_fkey_bar(height - 1, width);

	int cursor_x = prompt_cols + cmd_pos;
	if (cursor_x >= width) cursor_x = width - 1;
	move(height - 2, cursor_x);
	curs_set(1);
}

int ui_confirm(const char *title, const char *msg) {
	int height, width;
	getmaxyx(stdscr, height, width);

	int msg_cols = (int)mbstowcs(NULL, msg, 0);
	if (msg_cols < 0) msg_cols = (int)strlen(msg);
	int box_w = msg_cols + 6;
	if (box_w < 26) box_w = 26;
	if (box_w > width - 4) box_w = width - 4;
	int box_h = 5;

	int start_y = (height - box_h) / 2;
	int start_x = (width - box_w) / 2;

	curs_set(0);
	WINDOW *win = newwin(box_h, box_w, start_y, start_x);
	keypad(win, TRUE);
	wbkgd(win, COLOR_PAIR(5));

	int selected = 1;
	int res = 0;

	while (1) {
		werase(win);
		draw_win_box(win);
		mvwprintw(win, 0, 2, " %s ", title);

		int msg_x = (box_w - msg_cols) / 2;
		if (msg_x < 2) msg_x = 2;
		mvwprint_utf8(win, 1, msg_x, box_w - 4, msg);

		const char *btn_yes = "[ Yes ]";
		const char *btn_no  = "[ No ]";
		int total_btn_w = 7 + 3 + 6;
		int btn_x = (box_w - total_btn_w) / 2;
		if (btn_x < 2) btn_x = 2;

		wattron(win, selected == 1 ? COLOR_PAIR(2) : COLOR_PAIR(5));
		mvwprintw(win, 3, btn_x, "%s", btn_yes);
		wattroff(win, selected == 1 ? COLOR_PAIR(2) : COLOR_PAIR(5));

		wattron(win, selected == 0 ? COLOR_PAIR(2) : COLOR_PAIR(5));
		mvwprintw(win, 3, btn_x + 10, "%s", btn_no);
		wattroff(win, selected == 0 ? COLOR_PAIR(2) : COLOR_PAIR(5));

		wrefresh(win);

		int ch = wgetch(win);
		if (ch == KEY_LEFT || ch == KEY_RIGHT || ch == '\t' || ch == KEY_BTAB) {
			selected = 1 - selected;
		} else if (ch == '\n' || ch == KEY_ENTER || ch == '\r' || ch == ' ') {
			res = selected;
			break;
		} else if (ch == 27 || ch == 'n' || ch == 'N') {
			res = 0;
			break;
		} else if (ch == 'y' || ch == 'Y') {
			res = 1;
			break;
		}
	}

	delwin(win);
	touchwin(stdscr);
	return res;
}

void ui_message(const char *title, const char *msg) {
	int height, width;
	getmaxyx(stdscr, height, width);

	int msg_cols = (int)mbstowcs(NULL, msg, 0);
	if (msg_cols < 0) msg_cols = (int)strlen(msg);
	int box_w = msg_cols + 6;
	if (box_w < 26) box_w = 26;
	if (box_w > width - 4) box_w = width - 4;
	int box_h = 5;

	int start_y = (height - box_h) / 2;
	int start_x = (width - box_w) / 2;

	curs_set(0);
	WINDOW *win = newwin(box_h, box_w, start_y, start_x);
	wbkgd(win, COLOR_PAIR(5));
	draw_win_box(win);

	mvwprintw(win, 0, 2, " %s ", title);

	int msg_x = (box_w - msg_cols) / 2;
	if (msg_x < 2) msg_x = 2;
	mvwprint_utf8(win, 1, msg_x, box_w - 4, msg);

	const char *btn = "[ Enter ]";
	int btn_x = (box_w - (int)strlen(btn)) / 2;
	wattron(win, COLOR_PAIR(2));
	mvwprintw(win, 3, btn_x, "%s", btn);
	wattroff(win, COLOR_PAIR(2));
	wrefresh(win);

	while (1) {
		int ch = wgetch(win);
		if (ch == '\n' || ch == KEY_ENTER || ch == '\r' || ch == 27 || ch == ' ') {
			break;
		}
	}

	delwin(win);
	touchwin(stdscr);
}

static int ui_is_word_sep(wchar_t ch) {
	return iswspace(ch) || ch == L'/' || ch == L'-' || ch == L'_' || ch == L'.';
}

static int ui_word_left(const wchar_t *buf, int pos) {
	while (pos > 0 && ui_is_word_sep(buf[pos - 1])) pos--;
	while (pos > 0 && !ui_is_word_sep(buf[pos - 1])) pos--;
	return pos;
}

static int ui_word_right(const wchar_t *buf, int pos, int len) {
	while (pos < len && !ui_is_word_sep(buf[pos])) pos++;
	while (pos < len && ui_is_word_sep(buf[pos])) pos++;
	return pos;
}

int ui_input(const char *title, const char *prompt, char *buf, int max_len) {
	int height, width;
	getmaxyx(stdscr, height, width);

	int box_w = width - 10;
	if (box_w > 70) box_w = 70;
	if (box_w < 36) box_w = 36;
	int box_h = 6;

	int start_y = (height - box_h) / 2;
	int start_x = (width - box_w) / 2;

	WINDOW *win = newwin(box_h, box_w, start_y, start_x);
	keypad(win, TRUE);
	wbkgd(win, COLOR_PAIR(5));

	wchar_t wbuf[MAX_PATH];
	size_t conv = mbstowcs(wbuf, buf, MAX_PATH - 1);
	int len = (conv == (size_t)-1) ? 0 : (int)conv;
	wbuf[len] = L'\0';
	int pos = len;
	int sel_anchor = -1;

	int field_w = box_w - 6;
	int res = 0;
	int focus = 0;

	while (1) {
		werase(win);
		draw_win_box(win);
		mvwprintw(win, 0, 2, " %s ", title);
		mvwprint_utf8(win, 1, 3, field_w, prompt);

		wattron(win, COLOR_PAIR(2));
		mvwhline(win, 2, 3, ' ', field_w);

		int offset = 0;
		if (pos >= field_w) {
			offset = pos - field_w + 1;
		}
		mvwaddnwstr(win, 2, 3, wbuf + offset, field_w);
		wattroff(win, COLOR_PAIR(2));

		if (sel_anchor != -1 && sel_anchor != pos) {
			int s1 = (sel_anchor < pos) ? sel_anchor : pos;
			int s2 = (sel_anchor > pos) ? sel_anchor : pos;
			if (s1 < offset) s1 = offset;
			if (s2 > offset + field_w) s2 = offset + field_w;
			if (s2 > s1) {
				wattron(win, COLOR_PAIR(6));
				mvwaddnwstr(win, 2, 3 + (s1 - offset), wbuf + s1, s2 - s1);
				wattroff(win, COLOR_PAIR(6));
			}
		}

		const char *btn_ok  = "[ OK ]";
		const char *btn_can = "[ Cancel ]";
		int total_btn_w = 6 + 4 + 10;
		int btn_x = (box_w - total_btn_w) / 2;
		if (btn_x < 2) btn_x = 2;

		wattron(win, focus == 1 ? COLOR_PAIR(2) : COLOR_PAIR(5));
		mvwprintw(win, 4, btn_x, "%s", btn_ok);
		wattroff(win, focus == 1 ? COLOR_PAIR(2) : COLOR_PAIR(5));

		wattron(win, focus == 2 ? COLOR_PAIR(2) : COLOR_PAIR(5));
		mvwprintw(win, 4, btn_x + 10, "%s", btn_can);
		wattroff(win, focus == 2 ? COLOR_PAIR(2) : COLOR_PAIR(5));

		if (focus == 0) {
			curs_set(1);
			wmove(win, 2, 3 + (pos - offset));
		} else {
			curs_set(0);
		}
		wrefresh(win);

		wint_t wch;
		int status = wget_wch(win, &wch);
		if (status == ERR) continue;

		if (status == KEY_CODE_YES) {
			const char *kn = keyname((int)wch);
			if (wch == KEY_DOWN) {
				focus = (focus == 0) ? 1 : 0;
			} else if (wch == KEY_UP) {
				focus = 0;
			} else if (strcmp(kn, "kLFT6") == 0) {
				focus = 0;
				if (sel_anchor == -1) sel_anchor = pos;
				pos = ui_word_left(wbuf, pos);
			} else if (strcmp(kn, "kRIT6") == 0) {
				focus = 0;
				if (sel_anchor == -1) sel_anchor = pos;
				pos = ui_word_right(wbuf, pos, len);
			} else if (wch == KEY_SLEFT) {
				focus = 0;
				if (sel_anchor == -1) sel_anchor = pos;
				if (pos > 0) pos--;
			} else if (wch == KEY_SRIGHT) {
				focus = 0;
				if (sel_anchor == -1) sel_anchor = pos;
				if (pos < len) pos++;
			} else if (strcmp(kn, "kLFT5") == 0) {
				focus = 0;
				sel_anchor = -1;
				pos = ui_word_left(wbuf, pos);
			} else if (strcmp(kn, "kRIT5") == 0) {
				focus = 0;
				sel_anchor = -1;
				pos = ui_word_right(wbuf, pos, len);
			} else if (wch == KEY_LEFT) {
				sel_anchor = -1;
				if (focus == 0 && pos > 0) pos--;
				else if (focus == 2) focus = 1;
				else if (focus == 1) focus = 2;
			} else if (wch == KEY_RIGHT) {
				sel_anchor = -1;
				if (focus == 0 && pos < len) pos++;
				else if (focus == 1) focus = 2;
				else if (focus == 2) focus = 1;
			} else if (wch == KEY_HOME) {
				focus = 0;
				sel_anchor = -1;
				pos = 0;
			} else if (wch == KEY_END) {
				focus = 0;
				sel_anchor = -1;
				pos = len;
			} else if (wch == KEY_BACKSPACE && focus == 0) {
				if (sel_anchor != -1 && sel_anchor != pos) {
					int s1 = (sel_anchor < pos) ? sel_anchor : pos;
					int s2 = (sel_anchor > pos) ? sel_anchor : pos;
					wmemmove(wbuf + s1, wbuf + s2, (size_t)(len - s2 + 1));
					len -= (s2 - s1);
					pos = s1;
					sel_anchor = -1;
				} else if (pos > 0) {
					wmemmove(wbuf + pos - 1, wbuf + pos, (size_t)(len - pos + 1));
					pos--;
					len--;
				}
			} else if (wch == KEY_DC && focus == 0) {
				if (sel_anchor != -1 && sel_anchor != pos) {
					int s1 = (sel_anchor < pos) ? sel_anchor : pos;
					int s2 = (sel_anchor > pos) ? sel_anchor : pos;
					wmemmove(wbuf + s1, wbuf + s2, (size_t)(len - s2 + 1));
					len -= (s2 - s1);
					pos = s1;
					sel_anchor = -1;
				} else if (pos < len) {
					wmemmove(wbuf + pos, wbuf + pos + 1, (size_t)(len - pos));
					len--;
				}
			}
		} else {
			if (wch == 27) {
				res = 0;
				break;
			} else if (wch == L'\t') {
				focus = (focus + 1) % 3;
			} else if (wch == L'\n' || wch == L'\r') {
				if (focus == 2) {
					res = 0;
				} else {
					wcstombs(buf, wbuf, (size_t)max_len);
					buf[max_len - 1] = '\0';
					res = 1;
				}
				break;
			} else if (wch == 127 || wch == 8) {
				focus = 0;
				if (sel_anchor != -1 && sel_anchor != pos) {
					int s1 = (sel_anchor < pos) ? sel_anchor : pos;
					int s2 = (sel_anchor > pos) ? sel_anchor : pos;
					wmemmove(wbuf + s1, wbuf + s2, (size_t)(len - s2 + 1));
					len -= (s2 - s1);
					pos = s1;
					sel_anchor = -1;
				} else if (pos > 0) {
					wmemmove(wbuf + pos - 1, wbuf + pos, (size_t)(len - pos + 1));
					pos--;
					len--;
				}
			} else if (wch == 3 || wch == 25 || wch == 24) {
				int s1 = (sel_anchor != -1 && sel_anchor != pos) ? ((sel_anchor < pos) ? sel_anchor : pos) : 0;
				int s2 = (sel_anchor != -1 && sel_anchor != pos) ? ((sel_anchor > pos) ? sel_anchor : pos) : len;
				if (s2 > s1) {
					wchar_t wsub[MAX_PATH];
					wmemcpy(wsub, wbuf + s1, (size_t)(s2 - s1));
					wsub[s2 - s1] = L'\0';
					char utf_sub[MAX_PATH];
					if (wcstombs(utf_sub, wsub, sizeof(utf_sub)) != (size_t)-1) {
						utf_sub[sizeof(utf_sub) - 1] = '\0';
						clipboard_copy(utf_sub);
					}
					if (wch == 24 && sel_anchor != -1 && sel_anchor != pos) {
						wmemmove(wbuf + s1, wbuf + s2, (size_t)(len - s2 + 1));
						len -= (s2 - s1);
						pos = s1;
						sel_anchor = -1;
					}
				}
			} else if (wch == 22) {
				focus = 0;
				if (sel_anchor != -1 && sel_anchor != pos) {
					int s1 = (sel_anchor < pos) ? sel_anchor : pos;
					int s2 = (sel_anchor > pos) ? sel_anchor : pos;
					wmemmove(wbuf + s1, wbuf + s2, (size_t)(len - s2 + 1));
					len -= (s2 - s1);
					pos = s1;
					sel_anchor = -1;
				}
				char raw[MAX_PATH] = {0};
				if (clipboard_paste(raw, sizeof(raw)) > 0) {
					wchar_t wadd[MAX_PATH];
					size_t alen = mbstowcs(wadd, raw, MAX_PATH - 1);
					if (alen != (size_t)-1 && alen > 0) {
						if (len + (int)alen >= MAX_PATH - 2) alen = (size_t)(MAX_PATH - 2 - len);
						wmemmove(wbuf + pos + alen, wbuf + pos, (size_t)(len - pos + 1));
						wmemcpy(wbuf + pos, wadd, alen);
						pos += (int)alen;
						len += (int)alen;
					}
				}
			} else if (wch == 21) {
				focus = 0;
				len = 0;
				pos = 0;
				sel_anchor = -1;
				wbuf[0] = L'\0';
			} else if (iswprint(wch) && len < MAX_PATH - 2) {
				focus = 0;
				if (sel_anchor != -1 && sel_anchor != pos) {
					int s1 = (sel_anchor < pos) ? sel_anchor : pos;
					int s2 = (sel_anchor > pos) ? sel_anchor : pos;
					wmemmove(wbuf + s1, wbuf + s2, (size_t)(len - s2 + 1));
					len -= (s2 - s1);
					pos = s1;
					sel_anchor = -1;
				}
				wmemmove(wbuf + pos + 1, wbuf + pos, (size_t)(len - pos + 1));
				wbuf[pos++] = (wchar_t)wch;
				len++;
			}
		}
	}

	curs_set(0);
	delwin(win);
	touchwin(stdscr);
	return res;
}

void ui_help(void) {
	int height, width;
	getmaxyx(stdscr, height, width);

	int box_w = 64;
	if (box_w > width - 4) box_w = width - 4;
	int box_h = 25;
	if (box_h > height - 2) box_h = height - 2;

	int start_y = (height - box_h) / 2;
	int start_x = (width - box_w) / 2;

	curs_set(0);
	WINDOW *win = newwin(box_h, box_w, start_y, start_x);
	keypad(win, TRUE);
	wbkgd(win, COLOR_PAIR(5));
	draw_win_box(win);

#ifndef XC_VERSION
#define XC_VERSION "0.1"
#endif
#ifndef XC_BUILD
#define XC_BUILD 1
#endif
	mvwprintw(win, 0, 2, " xCommander [%06d] - Help (F1) ", XC_BUILD);

	const char *lines[] = {
		"Tab            Switch between panels",
		"Arrows         Navigate list (Left/Right by columns)",
		"Enter          Open directory or run executable",
		"Ctrl+PageUp    Go to parent directory (..)",
		"Ctrl+PageDown  Enter selected directory",
		"Ctrl+J         Insert current name to command line",
		"Ctrl+E / X     Command history (Up/Down in terminal)",
		"Esc / Ctrl+U   Clear command line",
		"Shift+Arrows   Select/unselect item and move cursor",
		"Insert         Select or unselect current item",
		"F1             Show this help",
		"F3             View file (less)",
		"F4             Edit file ($EDITOR / vi)",
		"Shift+F4       Create new file and open in editor",
		"F5             Copy file or selected items",
		"F6             Move / rename",
		"F7             Create new directory",
		"F8             Delete file, directory or selection",
		"Ctrl+O         Toggle panels (terminal mode)",
		"F10 / Ctrl+Q   Quit xCommander (or type exit)"
	};
	int total = (int)(sizeof(lines) / sizeof(lines[0]));

	for (int i = 0; i < total && (i + 2) < (box_h - 3); i++) {
		mvwprint_utf8(win, i + 2, 3, box_w - 6, lines[i]);
	}

	const char *btn = "[ Close ]";
	int btn_x = (box_w - (int)strlen(btn)) / 2;
	wattron(win, COLOR_PAIR(2));
	mvwprintw(win, box_h - 2, btn_x, "%s", btn);
	wattroff(win, COLOR_PAIR(2));
	wrefresh(win);

	while (1) {
		int ch = wgetch(win);
		if (ch == 27 || ch == '\n' || ch == KEY_ENTER || ch == '\r' || ch == ' ' || ch == KEY_F(1) || ch == KEY_F(10)) {
			break;
		}
	}

	delwin(win);
	touchwin(stdscr);
}

#include <sys/stat.h>

static void get_user_menu_path(char *out, size_t out_size) {
	const char *cfg = getenv("XDG_CONFIG_HOME");
	char dir[MAX_PATH];
	if (cfg && *cfg) {
		snprintf(dir, sizeof(dir), "%s/xcommander", cfg);
	} else {
		const char *home = getenv("HOME");
		if (!home || !*home) home = "/tmp";
		char cdir[MAX_PATH];
		snprintf(cdir, sizeof(cdir), "%s/.config", home);
		mkdir(cdir, 0755);
		snprintf(dir, sizeof(dir), "%s/.config/xcommander", home);
	}
	mkdir(dir, 0755);
	snprintf(out, out_size, "%s/menu", dir);
}

static void ensure_default_user_menu(const char *path) {
	struct stat st;
	if (stat(path, &st) == 0) return;
	FILE *f = fopen(path, "w");
	if (!f) return;
	fprintf(f, "# xCommander User Menu (F2)\n");
	fprintf(f, "# Format: Title | command\n");
	fprintf(f, "# Macros: %%f = current item, %%p = active path, %%o = other panel path\n\n");
	fprintf(f, "Pack current item to .tar.gz | tar -czvf \"%%f.tar.gz\" \"%%f\"\n");
	fprintf(f, "Pack current item to .zip    | zip -r \"%%f.zip\" \"%%f\"\n");
	fprintf(f, "Extract archive here         | tar -xvf \"%%f\" || unzip \"%%f\"\n");
	fprintf(f, "Disk usage of current dir    | du -sh . && read -n 1 -s -r -p \"Press any key...\"\n");
	fprintf(f, "Git status                   | git status && read -n 1 -s -r -p \"Press any key...\"\n");
	fclose(f);
}

static void expand_menu_cmd(const char *tmpl, char *out, size_t out_size) {
	const char *cur_name = (active_panel && active_panel->count > 0) ? active_panel->files[active_panel->selected].name : "";
	const char *act_path = (active_panel) ? active_panel->current_path : ".";
	Panel *other = (active_panel == &left_panel) ? &right_panel : &left_panel;
	const char *oth_path = (other) ? other->current_path : ".";

	size_t j = 0;
	for (size_t i = 0; tmpl[i] != '\0' && j + 1 < out_size; i++) {
		if (tmpl[i] == '%' && tmpl[i + 1] != '\0') {
			const char *rep = NULL;
			if (tmpl[i + 1] == 'f') rep = cur_name;
			else if (tmpl[i + 1] == 'p') rep = act_path;
			else if (tmpl[i + 1] == 'o') rep = oth_path;
			if (rep) {
				size_t rlen = strlen(rep);
				if (j + rlen >= out_size) rlen = out_size - 1 - j;
				memcpy(out + j, rep, rlen);
				j += rlen;
				i++;
				continue;
			}
		}
		out[j++] = tmpl[i];
	}
	out[j] = '\0';
}

void ui_user_menu(void) {
	char menu_path[MAX_PATH + 32];
	get_user_menu_path(menu_path, sizeof(menu_path));
	ensure_default_user_menu(menu_path);

	char titles[64][128];
	char cmds[64][MAX_CMD];
	int count = 0;
	int sel = 0;
	int reload = 1;

	int height, width;
	getmaxyx(stdscr, height, width);

	int box_w = 58;
	if (box_w > width - 4) box_w = width - 4;
	int box_h = 16;
	if (box_h > height - 2) box_h = height - 2;

	int start_y = (height - box_h) / 2;
	int start_x = (width - box_w) / 2;

	curs_set(0);
	WINDOW *win = newwin(box_h, box_w, start_y, start_x);
	keypad(win, TRUE);
	wbkgd(win, COLOR_PAIR(5));

	while (1) {
		if (reload) {
			count = 0;
			FILE *f = fopen(menu_path, "r");
			if (f) {
				char line[MAX_CMD + 160];
				while (fgets(line, sizeof(line), f) && count < 64) {
					size_t l = strlen(line);
					while (l > 0 && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = '\0';
					char *p = line;
					while (*p == ' ' || *p == '\t') p++;
					if (*p == '\0' || *p == '#') continue;

					char *sep = strchr(p, '|');
					if (!sep) continue;
					*sep = '\0';

					char *t_end = sep - 1;
					while (t_end > p && (*t_end == ' ' || *t_end == '\t')) *t_end-- = '\0';

					char *c = sep + 1;
					while (*c == ' ' || *c == '\t') c++;

					snprintf(titles[count], sizeof(titles[0]), "%s", p);
					snprintf(cmds[count], sizeof(cmds[0]), "%s", c);
					count++;
				}
				fclose(f);
			}
			if (sel >= count) sel = (count > 0) ? (count - 1) : 0;
			reload = 0;
		}

		werase(win);
		draw_win_box(win);
		mvwprintw(win, 0, 2, " User Menu (F2) ");

		int vis = box_h - 4;
		if (vis < 1) vis = 1;
		int top = 0;
		if (sel >= vis) top = sel - vis + 1;

		if (count == 0) {
			mvwprint_utf8(win, 2, 3, box_w - 6, "Menu is empty. Press F4 to edit.");
		} else {
			for (int i = 0; i < vis && (top + i) < count; i++) {
				int idx = top + i;
				wattron(win, idx == sel ? COLOR_PAIR(2) : COLOR_PAIR(5));
				mvwhline(win, 2 + i, 2, ' ', box_w - 4);
				mvwprint_utf8(win, 2 + i, 3, box_w - 6, titles[idx]);
				wattroff(win, idx == sel ? COLOR_PAIR(2) : COLOR_PAIR(5));
			}
		}

		const char *hint = "[ Enter: Run | F4: Edit Menu | Esc: Close ]";
		int hx = (box_w - (int)strlen(hint)) / 2;
		if (hx < 2) hx = 2;
		mvwprintw(win, box_h - 1, hx, "%s", hint);
		wrefresh(win);

		int ch = wgetch(win);
		if (ch == 27 || ch == KEY_F(2) || ch == KEY_F(10)) {
			break;
		} else if (ch == KEY_UP && count > 0) {
			sel = (sel > 0) ? (sel - 1) : (count - 1);
		} else if (ch == KEY_DOWN && count > 0) {
			sel = (sel + 1) % count;
		} else if (ch == KEY_F(4)) {
			edit_file(menu_path);
			clearok(stdscr, TRUE);
			refresh();
			draw_screen();
			refresh();
			reload = 1;
		} else if ((ch == '\n' || ch == KEY_ENTER || ch == '\r') && count > 0) {
			char final_cmd[MAX_CMD];
			expand_menu_cmd(cmds[sel], final_cmd, sizeof(final_cmd));
			delwin(win);
			touchwin(stdscr);
			execute_system_command(final_cmd);
			load_directory(&left_panel);
			load_directory(&right_panel);
			return;
		}
	}

	delwin(win);
	touchwin(stdscr);
}
