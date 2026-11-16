#define _GNU_SOURCE
#include <sys/statvfs.h>
#include <ctype.h>
#include <time.h>
#include <sys/stat.h>
#include <ncurses.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>
#include "ui.h"
#include "exec.h"
#include "panel.h"
#include "input.h"

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
	setenv("NCURSES_NO_HARD_SCROLL", "1", 1);
	const char *term = getenv("TERM");
	if (!term || !*term || strcmp(term, "dumb") == 0) {
		setenv("TERM", "xterm-256color", 1);
	}
	initscr();
	start_color();
	raw();
	input_enable_raw();
	noecho();
	keypad(stdscr, TRUE);
	idlok(stdscr, FALSE);
	scrollok(stdscr, FALSE);
	define_key("\033[1;2D", KEY_SLEFT);
	define_key("\033[1;2C", KEY_SRIGHT);
	define_key("\033[1;6D", KEY_MAX + 1);
	define_key("\033[1;6C", KEY_MAX + 2);
	define_key("\033[1;4D", KEY_MAX + 1);
	define_key("\033[1;4C", KEY_MAX + 2);
	define_key("\033[1;2D", KEY_SLEFT);
	define_key("\033[1;2C", KEY_SRIGHT);
	define_key("\033[1;6D", KEY_MAX + 1);
	define_key("\033[1;6C", KEY_MAX + 2);
	define_key("\033OP", KEY_F(1));
	define_key("\033OQ", KEY_F(2));
	define_key("\033OR", KEY_F(3));
	define_key("\033OS", KEY_F(4));
	define_key("\033[11~", KEY_F(1));
	define_key("\033[12~", KEY_F(2));
	define_key("\033[13~", KEY_F(3));
	define_key("\033[14~", KEY_F(4));
	define_key("\033[15~", KEY_F(5));
	define_key("\033[1;1P", KEY_F(1));
	define_key("\033[1;1Q", KEY_F(2));
	define_key("\033[1;1R", KEY_F(3));
	define_key("\033[1;1S", KEY_F(4));
	define_key("\033[15;1~", KEY_F(5));
	curs_set(0);
	set_escdelay(25);

	if (COLORS >= 256) {
		init_pair(12, 18, 252);
		init_pair(1, 51, 18);    /* Рамки и обычные файлы (ярко-голубой на тёмно-синем) */
		init_pair(2, 16, 37);    /* Чёрный текст на бирюзовом */
		init_pair(3, 226, 18);   /* Выделенные файлы (ярко-жёлтый на тёмно-синем) */
		init_pair(4, 226, 37);   /* Выделенные файлы под курсором */
		init_pair(5, 16, 252);   /* Модальные окна (чёрный на светло-сером) */
		init_pair(6, 231, 16);   /* Командная строка и цифры F-клавиш */
		init_pair(7, 231, 18);   /* Каталоги (ярко-белый на тёмно-синем) */
		init_pair(8, 46, 18);    /* Исполняемые файлы (ярко-зелёный на тёмно-синем) */
		init_pair(9, 231, 37);   /* Каталоги под курсором */
		init_pair(10, 46, 37);   /* Исполняемые файлы под курсором */
		init_pair(11, 51, 37);   /* Обычные файлы под курсором (ярко-голубой на бирюзовом) */
		init_pair(13, 231, 18);  /* Синтаксис: ключевые слова (ярко-белый) */
		init_pair(14, 120, 18);  /* Синтаксис: строки (мягкий зелёный) */
		init_pair(15, 228, 18);  /* Синтаксис: числа и директивы (светло-жёлтый) */
		init_pair(16, 103, 18);  /* Синтаксис: комментарии (серо-синий) */
		init_pair(17, 231, 160); /* Непарная скобка (белый на красном) */
	} else {
		init_pair(12, COLOR_BLUE, COLOR_WHITE);
		init_pair(1, COLOR_CYAN, COLOR_BLUE);
		init_pair(2, COLOR_BLACK, COLOR_CYAN);
		init_pair(3, COLOR_YELLOW, COLOR_BLUE);
		init_pair(4, COLOR_YELLOW, COLOR_CYAN);
		init_pair(5, COLOR_BLACK, COLOR_WHITE);
		init_pair(6, COLOR_WHITE, COLOR_BLACK);
		init_pair(7, COLOR_WHITE, COLOR_BLUE);
		init_pair(8, COLOR_GREEN, COLOR_BLUE);
		init_pair(9, COLOR_WHITE, COLOR_CYAN);
		init_pair(10, COLOR_GREEN, COLOR_CYAN);
		init_pair(11, COLOR_CYAN, COLOR_CYAN);
		init_pair(13, COLOR_WHITE, COLOR_BLUE);
		init_pair(14, COLOR_GREEN, COLOR_BLUE);
		init_pair(15, COLOR_YELLOW, COLOR_BLUE);
		init_pair(16, COLOR_CYAN, COLOR_BLUE);
		init_pair(17, COLOR_WHITE, COLOR_RED);
		init_pair(12, COLOR_YELLOW, COLOR_WHITE);
	}

	bkgd(COLOR_PAIR(1));
}

void draw_fkey_bar(int y, int width) {
	const char *labels[10] = {
		"Help", "UserMn", "View", "Edit", "Copy",
		"RenMov", "MkFold", "Delete", "ConfMn", "Quit"
	};
	if (g_mod_alt && !g_mod_ctrl && !g_mod_shift) {
		labels[0] = "Left";
		labels[1] = "Right";
	} else if (g_mod_shift && !g_mod_ctrl && !g_mod_alt) {
		labels[3] = "Create";
	}

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

static void format_size(off_t bytes, char *out, size_t out_size) {
	if (bytes < 1024) {
		snprintf(out, out_size, "%lld B", (long long)bytes);
	} else if (bytes < 1024 * 1024) {
		snprintf(out, out_size, "%.1f K", (double)bytes / 1024.0);
	} else if (bytes < 1024LL * 1024 * 1024) {
		snprintf(out, out_size, "%.1f M", (double)bytes / (1024.0 * 1024.0));
	} else {
		snprintf(out, out_size, "%.2f G", (double)bytes / (1024.0 * 1024.0 * 1024.0));
	}
}

static void format_perms(mode_t m, char out[10]) {
	out[0] = (m & S_IRUSR) ? 'r' : '-';
	out[1] = (m & S_IWUSR) ? 'w' : '-';
	out[2] = (m & S_IXUSR) ? 'x' : '-';
	out[3] = (m & S_IRGRP) ? 'r' : '-';
	out[4] = (m & S_IWGRP) ? 'w' : '-';
	out[5] = (m & S_IXGRP) ? 'x' : '-';
	out[6] = (m & S_IROTH) ? 'r' : '-';
	out[7] = (m & S_IWOTH) ? 'w' : '-';
	out[8] = (m & S_IXOTH) ? 'x' : '-';
	out[9] = '\0';
}

void draw_panel(Panel *p, int start_y, int start_x, int width, int height, int is_active) {
	int normal_attr = COLOR_PAIR(1);
	int dir_attr = COLOR_PAIR(7);
	int exec_attr = COLOR_PAIR(8);
	int mark_attr = COLOR_PAIR(3);

	int cur_normal_attr = COLOR_PAIR(11);
	int cur_dir_attr = COLOR_PAIR(9);
	int cur_exec_attr = COLOR_PAIR(10);
	int cur_mark_attr = COLOR_PAIR(4);

	attrset(normal_attr);

	int inner_w = width - 2;
	int col1_w = inner_w / 2;
	int col2_w = inner_w - col1_w - 1;
	int mid_x = start_x + 1 + col1_w;

	int info_sep_y = start_y + height - 3;
	for (int i = 1; i < width - 1; i++) {
		mvaddstr(start_y, start_x + i, "─");
		mvaddstr(info_sep_y, start_x + i, "─");
		mvaddstr(start_y + height - 1, start_x + i, "─");
	}
	mvaddstr(start_y, mid_x, "┬");
	mvaddstr(info_sep_y, mid_x, "┴");

	for (int i = 1; i < height - 1; i++) {
		mvaddstr(start_y + i, start_x, "│");
		if (start_y + i < info_sep_y) mvaddstr(start_y + i, mid_x, "│");
		mvaddstr(start_y + i, start_x + width - 1, "│");
	}
	mvaddstr(info_sep_y, start_x, "├");
	mvaddstr(info_sep_y, start_x + width - 1, "┤");

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

	int visible_lines = height - 4;
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

	int info_y = start_y + height - 2;
	attrset(normal_attr);
	for (int k = 0; k < inner_w; k++) mvaddch(info_y, start_x + 1 + k, ' ');

	int marked_cnt = 0;
	off_t marked_bytes = 0;
	for (int i = 0; i < p->count; i++) {
		if (p->files[i].is_selected && strcmp(p->files[i].name, "..") != 0) {
			marked_cnt++;
			if (!p->files[i].is_dir) marked_bytes += p->files[i].size;
		}
	}

	if (is_active && quick_search_active && inner_w > 2) {
		char sline[MAX_FILENAME + 32];
		snprintf(sline, sizeof(sline), "Search: %s_", quick_search);
		attrset(quick_search_nomatch ? (COLOR_PAIR(4) | A_REVERSE) : COLOR_PAIR(2));
		mvwprint_utf8(stdscr, info_y, start_x + 2, inner_w - 2, sline);
	} else if (marked_cnt > 0 && inner_w > 2) {
		char sz[32], summary[128];
		format_size(marked_bytes, sz, sizeof(sz));
		snprintf(summary, sizeof(summary), "Selected: %d item(s), %s", marked_cnt, sz);
		attrset(mark_attr);
		mvwprint_utf8(stdscr, info_y, start_x + 2, inner_w - 2, summary);
	} else if (p->count > 0 && p->selected >= 0 && p->selected < p->count && inner_w > 2) {
		const FileEntry *cur = &p->files[p->selected];
		char sz[32], perms[10], tbuf[32] = "", meta[96];
		if (strcmp(cur->name, "..") == 0) snprintf(sz, sizeof(sz), "<UP>");
		else if (cur->is_dir) snprintf(sz, sizeof(sz), "<DIR>");
		else format_size(cur->size, sz, sizeof(sz));
		format_perms(cur->mode, perms);
		struct tm *tm_info = localtime(&cur->mtime);
		if (tm_info) strftime(tbuf, sizeof(tbuf), "%d %b %H:%M", tm_info);
		snprintf(meta, sizeof(meta), "%s  %s  %s", sz, perms, tbuf);

		int meta_len = (int)strlen(meta);
		int left_avail = inner_w - 2 - meta_len - 2;
		if (left_avail > 0) {
			mvwprint_utf8(stdscr, info_y, start_x + 2, left_avail, cur->name);
		}
		if (inner_w - 2 >= meta_len) {
			mvprintw(info_y, start_x + width - 2 - meta_len, "%s", meta);
		} else if (inner_w > 2) {
			mvwprint_utf8(stdscr, info_y, start_x + 2, inner_w - 2, sz);
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
				attron(COLOR_PAIR(6) | A_REVERSE);
				mvprintw(height - 2, prompt_cols + cmd_sel_start, "%s", sub_utf);
				attroff(COLOR_PAIR(6) | A_REVERSE);
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
	idlok(win, FALSE);
	scrollok(win, FALSE);

	int selected = 1;
	int res = 0;

	while (1) {
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

		printf("\033[?2026h"); fflush(stdout);
		wnoutrefresh(win); doupdate();
		printf("\033[?2026l"); fflush(stdout);

		InputEvent ev = input_read();
		if (ev.action == ACT_MOUSE_LEFT) {
			int wy = ev.mouse_y - start_y, wx = ev.mouse_x - start_x;
			if (wy < 0 || wy >= box_h || wx < 0 || wx >= box_w) { res = 0; break; }
			if (wy == 3) {
				if (wx >= btn_x && wx < btn_x + 7) { res = 1; break; }
				if (wx >= btn_x + 10 && wx < btn_x + 16) { res = 0; break; }
			}
			continue;
		}
		if (ev.action == ACT_LEFT || ev.action == ACT_RIGHT || ev.action == ACT_TAB) selected = 1 - selected;
		else if (ev.action == ACT_ENTER || (ev.action == ACT_CHAR && ev.ch == L' ')) { res = selected; break; }
		else if (ev.action == ACT_ESC || (ev.action == ACT_CHAR && (ev.ch == L'n' || ev.ch == L'N' || ev.ch == L'т' || ev.ch == L'Т'))) { res = 0; break; }
		else if (ev.action == ACT_CHAR && (ev.ch == L'y' || ev.ch == L'Y' || ev.ch == L'н' || ev.ch == L'Н')) { res = 1; break; }
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
	wnoutrefresh(stdscr);
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
		InputEvent ev = input_read();
		if (ev.action == ACT_MOUSE_LEFT || ev.action == ACT_ENTER || ev.action == ACT_ESC || (ev.action == ACT_CHAR && ev.ch == L' ')) break;
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

		InputEvent ev = input_read();
		if (ev.action == ACT_NONE) continue;
		if (ev.action == ACT_DOWN) focus = (focus == 0) ? 1 : 0;
		else if (ev.action == ACT_UP) focus = 0;
		else if (ev.action == ACT_WORD_SEL_LEFT) { focus = 0; if (sel_anchor == -1) sel_anchor = pos; pos = ui_word_left(wbuf, pos); }
		else if (ev.action == ACT_WORD_SEL_RIGHT) { focus = 0; if (sel_anchor == -1) sel_anchor = pos; pos = ui_word_right(wbuf, pos, len); }
		else if (ev.action == ACT_SHIFT_LEFT) { focus = 0; if (sel_anchor == -1) sel_anchor = pos; if (pos > 0) pos--; }
		else if (ev.action == ACT_SHIFT_RIGHT) { focus = 0; if (sel_anchor == -1) sel_anchor = pos; if (pos < len) pos++; }
		else if (ev.action == ACT_CTRL_LEFT) { focus = 0; sel_anchor = -1; pos = ui_word_left(wbuf, pos); }
		else if (ev.action == ACT_CTRL_RIGHT) { focus = 0; sel_anchor = -1; pos = ui_word_right(wbuf, pos, len); }
		else if (ev.action == ACT_LEFT) { sel_anchor = -1; if (focus == 0 && pos > 0) pos--; else if (focus == 2) focus = 1; else if (focus == 1) focus = 2; }
		else if (ev.action == ACT_RIGHT) { sel_anchor = -1; if (focus == 0 && pos < len) pos++; else if (focus == 1) focus = 2; else if (focus == 2) focus = 1; }
		else if (ev.action == ACT_HOME || ev.action == ACT_CTRL_A) { focus = 0; sel_anchor = -1; pos = 0; }
		else if (ev.action == ACT_END) { focus = 0; sel_anchor = -1; pos = len; }
		else if (ev.action == ACT_BACKSPACE && focus == 0) {
			if (sel_anchor != -1 && sel_anchor != pos) {
				int s1 = (sel_anchor < pos) ? sel_anchor : pos, s2 = (sel_anchor > pos) ? sel_anchor : pos;
				wmemmove(wbuf + s1, wbuf + s2, (size_t)(len - s2 + 1)); len -= (s2 - s1); pos = s1; sel_anchor = -1;
			} else if (pos > 0) { wmemmove(wbuf + pos - 1, wbuf + pos, (size_t)(len - pos + 1)); pos--; len--; }
		} else if (ev.action == ACT_DELETE && focus == 0) {
			if (sel_anchor != -1 && sel_anchor != pos) {
				int s1 = (sel_anchor < pos) ? sel_anchor : pos, s2 = (sel_anchor > pos) ? sel_anchor : pos;
				wmemmove(wbuf + s1, wbuf + s2, (size_t)(len - s2 + 1)); len -= (s2 - s1); pos = s1; sel_anchor = -1;
			} else if (pos < len) { wmemmove(wbuf + pos, wbuf + pos + 1, (size_t)(len - pos)); len--; }
		} else if (ev.action == ACT_ESC) { res = 0; break; }
		else if (ev.action == ACT_TAB) { focus = (focus + 1) % 3; }
		else if (ev.action == ACT_ENTER) {
			if (focus == 2) res = 0; else { wcstombs(buf, wbuf, (size_t)max_len); buf[max_len - 1] = '\0'; res = 1; }
			break;
		} else if (ev.action == ACT_CTRL_C || ev.action == ACT_CTRL_Y || ev.action == ACT_CTRL_X) {
			int s1 = (sel_anchor != -1 && sel_anchor != pos) ? ((sel_anchor < pos) ? sel_anchor : pos) : 0;
			int s2 = (sel_anchor != -1 && sel_anchor != pos) ? ((sel_anchor > pos) ? sel_anchor : pos) : len;
			if (s2 > s1) {
				wchar_t wsub[MAX_PATH]; wmemcpy(wsub, wbuf + s1, (size_t)(s2 - s1)); wsub[s2 - s1] = L'\0';
				char utf_sub[MAX_PATH];
				if (wcstombs(utf_sub, wsub, sizeof(utf_sub)) != (size_t)-1) { utf_sub[sizeof(utf_sub) - 1] = '\0'; clipboard_copy(utf_sub); }
				if (ev.action == ACT_CTRL_X && sel_anchor != -1 && sel_anchor != pos) {
					wmemmove(wbuf + s1, wbuf + s2, (size_t)(len - s2 + 1)); len -= (s2 - s1); pos = s1; sel_anchor = -1;
				}
			}
		} else if (ev.action == ACT_CTRL_V || ev.action == ACT_SHIFT_INSERT) {
			focus = 0;
			if (sel_anchor != -1 && sel_anchor != pos) {
				int s1 = (sel_anchor < pos) ? sel_anchor : pos, s2 = (sel_anchor > pos) ? sel_anchor : pos;
				wmemmove(wbuf + s1, wbuf + s2, (size_t)(len - s2 + 1)); len -= (s2 - s1); pos = s1; sel_anchor = -1;
			}
			char raw[MAX_PATH] = {0};
			if (clipboard_paste(raw, sizeof(raw)) > 0) {
				wchar_t wadd[MAX_PATH]; size_t alen = mbstowcs(wadd, raw, MAX_PATH - 1);
				if (alen != (size_t)-1 && alen > 0) {
					if (len + (int)alen >= MAX_PATH - 2) alen = (size_t)(MAX_PATH - 2 - len);
					wmemmove(wbuf + pos + alen, wbuf + pos, (size_t)(len - pos + 1));
					wmemcpy(wbuf + pos, wadd, alen); pos += (int)alen; len += (int)alen;
				}
			}
		} else if (ev.action == ACT_CTRL_U) { focus = 0; len = 0; pos = 0; sel_anchor = -1; wbuf[0] = L'\0'; }
		else if (ev.action == ACT_CHAR && iswprint(ev.ch) && len < MAX_PATH - 2) {
			focus = 0;
			if (sel_anchor != -1 && sel_anchor != pos) {
				int s1 = (sel_anchor < pos) ? sel_anchor : pos, s2 = (sel_anchor > pos) ? sel_anchor : pos;
				wmemmove(wbuf + s1, wbuf + s2, (size_t)(len - s2 + 1)); len -= (s2 - s1); pos = s1; sel_anchor = -1;
			}
			wmemmove(wbuf + pos + 1, wbuf + pos, (size_t)(len - pos + 1)); wbuf[pos++] = ev.ch; len++;
		}
	}

	curs_set(0);
	delwin(win);
	touchwin(stdscr);
	return res;
}

static void ui_show_help_lines(const char *title, const char **lines, int total) {
	int top = 0;

	int height, width;
	getmaxyx(stdscr, height, width);

	int box_w = width - 8;
	if (box_w > 96) box_w = 96;
	if (box_w < 50) box_w = width - 4;
	int box_h = height - 4;
	if (box_h > 36) box_h = 36;
	if (box_h < 12) box_h = height - 2;

	int start_y = (height - box_h) / 2;
	int start_x = (width - box_w) / 2;

	curs_set(0);
	WINDOW *win = newwin(box_h, box_w, start_y, start_x);
	wbkgd(win, COLOR_PAIR(5));

	draw_win_box(win);
	mvwprintw(win, 0, 2, " %s ", title);
	const char *btn = "[ Close (Esc) ]";
	wattron(win, COLOR_PAIR(2));
	mvwprintw(win, box_h - 2, (box_w - (int)strlen(btn)) / 2, "%s", btn);
	wattroff(win, COLOR_PAIR(2));
	const char *nav = " Up/Down/Wheel: Scroll ";
	if (box_w > (int)strlen(nav) + 4) mvwprintw(win, box_h - 1, box_w - (int)strlen(nav) - 2, "%s", nav);

	while (1) {
		int vis = box_h - 4;
		if (vis < 1) vis = 1;
		if (top > total - vis) top = (total > vis) ? (total - vis) : 0;
		if (top < 0) top = 0;

		char pos_str[32];
		snprintf(pos_str, sizeof(pos_str), " %d-%d/%d ", top + 1, (top + vis < total ? top + vis : total), total);
		if (box_w > (int)strlen(pos_str) + 4) {
			mvwprintw(win, 0, box_w - (int)strlen(pos_str) - 2, "%s", pos_str);
		}

		for (int i = 0; i < vis; i++) {
			mvwhline(win, 1 + i, 2, ' ', box_w - 4);
			if (top + i >= total) continue;
			const char *ln = lines[top + i];
			int is_hdr = (strncmp(ln, "===", 3) == 0);
			if (is_hdr) wattron(win, COLOR_PAIR(5) | A_UNDERLINE);
			mvwprint_utf8(win, 1 + i, 2, box_w - 4, ln);
			if (is_hdr) wattroff(win, COLOR_PAIR(5) | A_UNDERLINE);
		}
		printf("\033[?2026h"); fflush(stdout);
		wnoutrefresh(win); doupdate();
		printf("\033[?2026l"); fflush(stdout);

		InputEvent ev = input_read();
		if (ev.action == ACT_MOUSE_WHEEL_UP) { top -= 3; if (top < 0) top = 0; continue; }
		if (ev.action == ACT_MOUSE_WHEEL_DOWN) { top += 3; continue; }
		if (ev.action == ACT_MOUSE_LEFT) {
			int wy = ev.mouse_y - start_y, wx = ev.mouse_x - start_x;
			if (wy < 0 || wy >= box_h || wx < 0 || wx >= box_w || wy == box_h - 2) break;
			continue;
		}
		if (ev.action == ACT_ESC || ev.action == ACT_ENTER || ev.action == ACT_F1 || ev.action == ACT_F10) break;
		else if (ev.action == ACT_UP && top > 0) top--;
		else if (ev.action == ACT_DOWN && top + vis < total) top++;
		else if (ev.action == ACT_PGUP) { top -= vis; if (top < 0) top = 0; }
		else if (ev.action == ACT_PGDN) { top += vis; }
		else if (ev.action == ACT_HOME) top = 0;
		else if (ev.action == ACT_END) top = total;
	}

	delwin(win);
	touchwin(stdscr);
}

void ui_help(void) {
	const char *lines[] = {
		"=== 1. NAVIGATION & PANELS ===",
		"  Tab                Switch active panel (Left / Right)",
		"  Arrows / PgUp/Dn   Move cursor / page scroll in panel",
		"  Enter              Open directory or run executable",
		"  Shift+Enter        Open folder or file in system file manager",
		"  Ctrl+PgUp / PgDn   Go to parent (..) / enter directory",
		"  Ctrl+U             Swap left and right panels with cursor",
		"  Alt+F1 / Alt+F2    Change drive / mount point (Left / Right)",
		"                       F8 / Delete in drive menu: Safe USB unmount",
		"  Insert / Shift+Arr Mark or unmark files and directories",
		"  Alt+<char>         Quick search by prefix (auto EN/RU layout)",
		"                     Continue typing without Alt; Esc to cancel",
		"",
		"=== 2. VIEWER, EDITOR & FILE OPERATIONS ===",
		"  F1                 Open this Help guide",
		"  F2                 Open User Menu (submenus { } & hotkeys)",
		"  F3                 Built-in Text / Hex viewer (F2:Wrap F4:Hex F7:Find)",
		"  F4                 Built-in TUI Editor (F1 inside editor for keys)",
		"  Shift+F4           Create new file and open in built-in Editor",
		"  F5 / F6            Copy / Move or rename selected items",
		"  F7 / F8            Create directory / Delete selected items",
		"",
		"=== 3. COMMAND LINE & SMART CLIPBOARD ===",
		"  Alt+[ / Alt+]      Insert Left / Right panel path into cmd",
		"  Alt+Enter          Insert current filename into cmd",
		"  Ctrl+C / Ctrl+X    Copy / Cut file (URI-list) or selected text",
		"  Ctrl+V / Shift+Ins Paste file, text, or multiline script dialog",
		"  Shift+Arrows       Select characters in command line",
		"  Alt+Shift+Arrows   Select whole words (also Ctrl+Shift+Arrows)",
		"  Ctrl+Left / Right  Jump cursor by words",
		"  Alt+H              Open Multiline Command History (or via Ctrl+V)",
		"  Ctrl+E / Ctrl+X    Browse persistent command history",
		"  Ctrl+U / Esc       Clear command line (when text is entered)",
		"",
		"=== 4. MOUSE & TERMINAL MODE ===",
		"  Mouse Left / Dbl   Focus panel & move cursor / Open folder or run",
		"  Mouse Right        Toggle file selection mark (like Insert)",
		"  Mouse Wheel        Scroll panels, menus, viewer, editor & help",
		"  F-Bar Click        Click any F1-F10 button on bottom bar",
		"  Ctrl+O             Toggle full terminal mode / panels",
		"  F10 / Ctrl+Q       Save panel paths & history and quit",
		"  Configs: ~/.config/xcommander/{menu,prompt,bookmarks}"
	};
	ui_show_help_lines("xCommander Help (F1)", lines, (int)(sizeof(lines) / sizeof(lines[0])));
}

void ui_editor_help(void) {
	const char *lines[] = {
		"=== 1. CURSOR MOVEMENT & SCROLLING ===",
		"  Arrows             Move cursor by character / line",
		"  Ctrl+Left / Right  Jump cursor by words",
		"  Home / End         Jump to start / end of current line",
		"  Ctrl+Home / End    Jump to start / end of entire file",
		"  Ctrl+]             Jump to matching bracket or quote",
		"  PageUp / PageDown  Scroll one page up / down",
		"  Mouse Click        Place cursor at clicked line and column",
		"  Mouse Wheel        Scroll text up / down",
		"",
		"=== 2. BLOCK SELECTION & CLIPBOARD ===",
		"  Shift+Arrows       Select characters / lines",
		"  Shift+Home / End   Select from cursor to start / end of line",
		"  Ctrl+Shift+Arrows  Select whole words (also Alt+Shift+Arrows)",
		"  F3                 Toggle block selection mark at cursor",
		"  Alt+Shift+]        Select entire block between matching pair",
		"  Ctrl+A             Select entire file",
		"  Ctrl+C             Copy selected block to system clipboard",
		"  F4                 Toggle syntax highlighting on/off",
		"  F5                 Toggle visible whitespace (spaces, tabs, EOL)",
		"  F6 / Ctrl+X        Cut selected block to system clipboard",
		"  Ctrl+V / Shift+Ins Paste text (supports multiline paste)",
		"  F8                 Delete selected block (or char under cursor)",
		"",
		"=== 3. EDITING, SEARCH & FILE CONTROL ===",
		"  Enter              Split line (with auto-indent if enabled)",
		"  Tab                Insert tabulation character",
		"  Insert             Toggle Insert (INS) / Overwrite (OVR) mode",
		"  Ctrl+Z / Ctrl+R    Undo / Redo changes (up to 64 steps)",
		"  Ctrl+Y             Delete entire current line",
		"  F2                 Save file to disk",
		"  F7                 Find text (case-insensitive)",
		"  Shift+F7           Find next match",
		"  Ctrl+F7            Search and replace all occurrences",
		"  F9                 Toggle Auto-Indent (Ind:On / Ind:Of)",
		"  F1                 Open this Editor Help",
		"  Esc / F10 / Ctrl+Q Exit editor (prompts to save if modified)"
	};
	ui_show_help_lines("Built-in Editor Help (F1)", lines, (int)(sizeof(lines) / sizeof(lines[0])));
}

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
	fprintf(f, "# Format: [Key:] Title | command\n");
	fprintf(f, "# Macros: %%f = current item, %%p = active path, %%o = other panel path\n\n");
	fprintf(f, "1: Pack current item to .tar.gz | tar -czvf \"%%f.tar.gz\" \"%%f\"\n");
	fprintf(f, "2: Pack current item to .zip    | zip -r \"%%f.zip\" \"%%f\"\n");
	fprintf(f, "x: Extract archive here         | tar -xvf \"%%f\" || unzip \"%%f\"\n");
	fprintf(f, "d: Disk usage of current dir    | du -sh . && read -n 1 -s -r -p \"Press any key...\"\n");
	fprintf(f, "g: Git status                   | git status && read -n 1 -s -r -p \"Press any key...\"\n");
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

typedef struct MenuNode {
	char hotkey;
	char title[128];
	char cmd[MAX_CMD];
	int is_submenu;
	struct MenuNode **children;
	int child_count;
	int child_cap;
	struct MenuNode *parent;
} MenuNode;

static MenuNode *menu_node_new(const char *title, const char *cmd, char hk, int is_sub, MenuNode *parent) {
	MenuNode *n = calloc(1, sizeof(MenuNode));
	if (!n) return NULL;
	n->hotkey = hk;
	n->is_submenu = is_sub;
	n->parent = parent;
	snprintf(n->title, sizeof(n->title), "%s", title);
	if (cmd) snprintf(n->cmd, sizeof(n->cmd), "%s", cmd);
	return n;
}

static void menu_node_add_child(MenuNode *parent, MenuNode *child) {
	if (!parent || !child) return;
	if (parent->child_count >= parent->child_cap) {
		int ncap = (parent->child_cap == 0) ? 8 : (parent->child_cap * 2);
		MenuNode **tmp = realloc(parent->children, (size_t)ncap * sizeof(MenuNode *));
		if (!tmp) return;
		parent->children = tmp;
		parent->child_cap = ncap;
	}
	parent->children[parent->child_count++] = child;
}

static void free_user_menu_tree(MenuNode *node) {
	if (!node) return;
	for (int i = 0; i < node->child_count; i++) free_user_menu_tree(node->children[i]);
	free(node->children);
	free(node);
}

static MenuNode *load_user_menu_tree(const char *path) {
	MenuNode *root = menu_node_new("User Menu (F2)", NULL, 0, 1, NULL);
	FILE *f = fopen(path, "r");
	if (!f) return root;

	MenuNode *cur = root;
	char line[MAX_CMD + 160];
	while (fgets(line, sizeof(line), f)) {
		size_t l = strlen(line);
		while (l > 0 && (line[l - 1] == '\n' || line[l - 1] == '\r' || line[l - 1] == ' ' || line[l - 1] == '\t')) line[--l] = '\0';
		char *p = line;
		while (*p == ' ' || *p == '\t') p++;
		if (*p == '\0' || *p == '#') continue;

		if (strcmp(p, "}") == 0) {
			if (cur->parent) cur = cur->parent;
			continue;
		}

		char hk = '\0';
		if (p[0] != '\0' && p[1] == ':') {
			hk = p[0];
			p += 2;
			while (*p == ' ' || *p == '\t') p++;
		}

		size_t plen = strlen(p);
		if (plen > 0 && p[plen - 1] == '{') {
			p[--plen] = '\0';
			while (plen > 0 && (p[plen - 1] == ' ' || p[plen - 1] == '\t')) p[--plen] = '\0';
			MenuNode *sub = menu_node_new(p, NULL, hk, 1, cur);
			menu_node_add_child(cur, sub);
			cur = sub;
			continue;
		}

		char *sep = strchr(p, '|');
		if (!sep) continue;
		*sep = '\0';
		char *tend = sep - 1;
		while (tend > p && (*tend == ' ' || *tend == '\t')) *tend-- = '\0';
		char *c = sep + 1;
		while (*c == ' ' || *c == '\t') c++;
		menu_node_add_child(cur, menu_node_new(p, c, hk, 0, cur));
	}
	fclose(f);
	return root;
}

void ui_user_menu(void) {
	char menu_path[MAX_PATH + 32];
	get_user_menu_path(menu_path, sizeof(menu_path));
	ensure_default_user_menu(menu_path);

	MenuNode *root = load_user_menu_tree(menu_path);
	MenuNode *cur_menu = root;
	int sel = 0;
	int btn_focus = 0;

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
	wbkgd(win, COLOR_PAIR(5));

	while (1) {
		int has_back = (cur_menu->parent != NULL) ? 1 : 0;
		int total_items = cur_menu->child_count + has_back;
		if (sel >= total_items) sel = (total_items > 0) ? (total_items - 1) : 0;
		if (sel < 0) sel = 0;

		werase(win);
		draw_win_box(win);
		if (cur_menu->parent == NULL) {
			mvwprintw(win, 0, 2, " User Menu (F2) ");
		} else {
			char hdr[160];
			snprintf(hdr, sizeof(hdr), " User Menu: %s ", cur_menu->title);
			mvwprint_utf8(win, 0, 2, box_w - 4, hdr);
		}

		int vis = box_h - 5;
		if (vis < 1) vis = 1;
		int top = 0;
		if (sel >= vis) top = sel - vis + 1;

		if (total_items == 0) {
			mvwprint_utf8(win, 2, 3, box_w - 6, "Menu is empty. Press F4 to edit.");
		} else {
			for (int i = 0; i < vis && (top + i) < total_items; i++) {
				int idx = top + i;
				char disp[180];
				if (has_back && idx == 0) {
					snprintf(disp, sizeof(disp), ".. (Back)");
				} else {
					MenuNode *node = cur_menu->children[idx - has_back];
					const char *mark = node->is_submenu ? " [>]" : "";
					if (node->hotkey) snprintf(disp, sizeof(disp), "%c. %s%s", node->hotkey, node->title, mark);
					else snprintf(disp, sizeof(disp), "   %s%s", node->title, mark);
				}
				wattron(win, idx == sel ? COLOR_PAIR(2) : COLOR_PAIR(5));
				mvwhline(win, 2 + i, 2, ' ', box_w - 4);
				mvwprint_utf8(win, 2 + i, 3, box_w - 6, disp);
				wattroff(win, idx == sel ? COLOR_PAIR(2) : COLOR_PAIR(5));
			}
		}

		const char *b_run  = "[ Run ]";
		const char *b_edit = "[ Edit (F4) ]";
		const char *b_cls  = "[ Close ]";
		int total_bw = 7 + 3 + 13 + 3 + 9;
		int bx = (box_w - total_bw) / 2;
		if (bx < 2) bx = 2;

		wattron(win, btn_focus == 0 ? COLOR_PAIR(2) : COLOR_PAIR(5));
		mvwprintw(win, box_h - 2, bx, "%s", b_run);
		wattroff(win, btn_focus == 0 ? COLOR_PAIR(2) : COLOR_PAIR(5));

		wattron(win, btn_focus == 1 ? COLOR_PAIR(2) : COLOR_PAIR(5));
		mvwprintw(win, box_h - 2, bx + 10, "%s", b_edit);
		wattroff(win, btn_focus == 1 ? COLOR_PAIR(2) : COLOR_PAIR(5));

		wattron(win, btn_focus == 2 ? COLOR_PAIR(2) : COLOR_PAIR(5));
		mvwprintw(win, box_h - 2, bx + 26, "%s", b_cls);
		wattroff(win, btn_focus == 2 ? COLOR_PAIR(2) : COLOR_PAIR(5));
		wrefresh(win);

		InputEvent ev = input_read();
		if (ev.action == ACT_MOUSE_WHEEL_UP && total_items > 0) {
			btn_focus = 0;
			if (sel > 0) sel--;
			continue;
		}
		if (ev.action == ACT_MOUSE_WHEEL_DOWN && total_items > 0) {
			btn_focus = 0;
			if (sel + 1 < total_items) sel++;
			continue;
		}
		if (ev.action == ACT_MOUSE_LEFT) {
			int wy = ev.mouse_y - start_y, wx = ev.mouse_x - start_x;
			if (wy < 0 || wy >= box_h || wx < 0 || wx >= box_w) break;
			if (wy == box_h - 2) {
				if (wx >= bx && wx < bx + 7) { btn_focus = 0; ev.action = ACT_ENTER; }
				else if (wx >= bx + 10 && wx < bx + 23) { ev.action = ACT_F4; }
				else if (wx >= bx + 26 && wx < bx + 35) break;
				else continue;
			} else if (wy >= 2 && wy < 2 + vis && (top + wy - 2) < total_items) {
				sel = top + (wy - 2);
				btn_focus = 0;
				ev.action = ACT_ENTER;
			} else {
				continue;
			}
		}
		if (ev.action == ACT_F2 || ev.action == ACT_F10) break;
		if (ev.action == ACT_ESC) {
			if (cur_menu->parent) { cur_menu = cur_menu->parent; sel = 0; btn_focus = 0; continue; }
			break;
		}
		if (ev.action == ACT_BACKSPACE && cur_menu->parent) {
			cur_menu = cur_menu->parent; sel = 0; btn_focus = 0; continue;
		}
		if (ev.action == ACT_TAB || ev.action == ACT_RIGHT) {
			btn_focus = (btn_focus + 1) % 3;
		} else if (ev.action == ACT_LEFT) {
			if (btn_focus > 0) btn_focus = (btn_focus + 2) % 3;
			else if (cur_menu->parent) { cur_menu = cur_menu->parent; sel = 0; }
			else btn_focus = 2;
		} else if (ev.action == ACT_UP && total_items > 0) {
			btn_focus = 0;
			sel = (sel > 0) ? (sel - 1) : (total_items - 1);
		} else if (ev.action == ACT_DOWN && total_items > 0) {
			btn_focus = 0;
			sel = (sel + 1) % total_items;
		} else if (ev.action == ACT_F4 || (ev.action == ACT_ENTER && btn_focus == 1)) {
			edit_file(menu_path);
			clearok(stdscr, TRUE); refresh(); draw_screen(); refresh();
			free_user_menu_tree(root);
			root = load_user_menu_tree(menu_path);
			cur_menu = root;
			sel = 0; btn_focus = 0;
		} else if (ev.action == ACT_ENTER && btn_focus == 2) {
			break;
		} else if (ev.action == ACT_CHAR && cur_menu->child_count > 0) {
			wchar_t ch = towlower(ev.ch);
			const wchar_t *ru = L"йцукенгшщзхъфывапролджэячсмитьбюё";
			const char *en = "qwertyuiop[]asdfghjkl;'zxcvbnm,.`";
			for (int k = 0; ru[k] != L'\0'; k++) {
				if (ch == ru[k]) { ch = (wchar_t)en[k]; break; }
			}
			MenuNode *hit = NULL;
			for (int i = 0; i < cur_menu->child_count; i++) {
				MenuNode *n = cur_menu->children[i];
				if (n->hotkey && (wchar_t)tolower((unsigned char)n->hotkey) == ch) { hit = n; break; }
			}
			if (hit) {
				if (hit->is_submenu) {
					cur_menu = hit; sel = 0; btn_focus = 0;
				} else {
					char final_cmd[MAX_CMD];
					expand_menu_cmd(hit->cmd, final_cmd, sizeof(final_cmd));
					free_user_menu_tree(root);
					delwin(win);
					touchwin(stdscr);
					execute_system_command(final_cmd);
					load_directory(&left_panel);
					load_directory(&right_panel);
					return;
				}
			}
		} else if (ev.action == ACT_ENTER && btn_focus == 0 && total_items > 0) {
			if (has_back && sel == 0) {
				cur_menu = cur_menu->parent; sel = 0;
			} else {
				MenuNode *node = cur_menu->children[sel - has_back];
				if (node->is_submenu) {
					cur_menu = node; sel = 0;
				} else {
					char final_cmd[MAX_CMD];
					expand_menu_cmd(node->cmd, final_cmd, sizeof(final_cmd));
					free_user_menu_tree(root);
					delwin(win);
					touchwin(stdscr);
					execute_system_command(final_cmd);
					load_directory(&left_panel);
					load_directory(&right_panel);
					return;
				}
			}
		}
	}

	free_user_menu_tree(root);
	delwin(win);
	touchwin(stdscr);
}

int ui_multiline_paste(const char *text) {
	int height, width;
	getmaxyx(stdscr, height, width);

	int box_w = width - 8;
	if (box_w > 84) box_w = 84;
	if (box_w < 40) box_w = 40;

	int max_lines = height - 8;
	if (max_lines > 14) max_lines = 14;
	if (max_lines < 3) max_lines = 3;

	int box_h = max_lines + 5;
	int start_y = (height - box_h) / 2;
	int start_x = (width - box_w) / 2;

	curs_set(0);
	WINDOW *win = newwin(box_h, box_w, start_y, start_x);
	wbkgd(win, COLOR_PAIR(5));

	int selected = 1;
	int res = 0;

	while (1) {
		werase(win);
		draw_win_box(win);
		mvwprintw(win, 0, (box_w - 17) / 2, " Multiline paste ");

		const char *p = text;
		for (int row = 0; row < max_lines && *p; row++) {
			const char *nl = strchr(p, '\n');
			int len = nl ? (int)(nl - p) : (int)strlen(p);
			if (len > 0 && p[len - 1] == '\r') len--;
			char line[256];
			if (len >= (int)sizeof(line)) len = (int)sizeof(line) - 1;
			memcpy(line, p, (size_t)len);
			line[len] = '\0';
			mvwprint_utf8(win, 1 + row, 2, box_w - 4, line);
			if (!nl) break;
			p = nl + 1;
		}

		const char *btn_exec = "[ Execute ]";
		const char *btn_can  = "[ Cancel ]";
		int total_w = 11 + 4 + 10;
		int btn_x = (box_w - total_w) / 2;
		if (btn_x < 2) btn_x = 2;

		wattron(win, selected == 1 ? COLOR_PAIR(2) : COLOR_PAIR(5));
		mvwprintw(win, box_h - 2, btn_x, "%s", btn_exec);
		wattroff(win, selected == 1 ? COLOR_PAIR(2) : COLOR_PAIR(5));

		wattron(win, selected == 0 ? COLOR_PAIR(2) : COLOR_PAIR(5));
		mvwprintw(win, box_h - 2, btn_x + 15, "%s", btn_can);
		wattroff(win, selected == 0 ? COLOR_PAIR(2) : COLOR_PAIR(5));

		wrefresh(win);

		InputEvent ev = input_read();
		if (ev.action == ACT_MOUSE_LEFT) {
			int wy = ev.mouse_y - start_y, wx = ev.mouse_x - start_x;
			if (wy < 0 || wy >= box_h || wx < 0 || wx >= box_w) { res = 0; break; }
			if (wy == box_h - 2) {
				if (wx >= btn_x && wx < btn_x + 11) { res = 1; break; }
				if (wx >= btn_x + 15 && wx < btn_x + 25) { res = 0; break; }
			}
			continue;
		}
		if (ev.action == ACT_LEFT || ev.action == ACT_RIGHT || ev.action == ACT_TAB) {
			selected = 1 - selected;
		} else if (ev.action == ACT_ENTER) {
			res = selected;
			break;
		} else if (ev.action == ACT_ESC) {
			res = 0;
			break;
		}
	}

	delwin(win);
	touchwin(stdscr);
	return res;
}

void ui_drive_menu(Panel *target_panel, int is_right) {
	if (!target_panel) return;

	while (1) {
		char paths[48][MAX_PATH];
		char devs[48][256];
		char labels[48][128];
		int can_unmount[48];
		int count = 0;

		const char *home = getenv("HOME");
		if (home && *home) {
			snprintf(paths[count], MAX_PATH, "%s", home);
			devs[count][0] = '\0';
			can_unmount[count] = 0;
			snprintf(labels[count], sizeof(labels[0]), "~ (Home)");
			count++;
		}

		snprintf(paths[count], MAX_PATH, "/");
		devs[count][0] = '\0';
		can_unmount[count] = 0;
		snprintf(labels[count], sizeof(labels[0]), "/ (Root)");
		count++;

		FILE *mf = fopen("/proc/mounts", "r");
		if (mf) {
			char dev[256], mnt[MAX_PATH], fstype[64];
			while (fscanf(mf, "%255s %4095s %63s %*[^\n]\n", dev, mnt, fstype) == 3 && count < 40) {
				if (strcmp(mnt, "/") == 0) continue;
				int is_media = (strncmp(mnt, "/media/", 7) == 0 || strncmp(mnt, "/mnt/", 5) == 0 || strncmp(mnt, "/run/media/", 11) == 0);
				if (strncmp(dev, "/dev/", 5) != 0 &&
				    strcmp(fstype, "ntfs3") != 0 &&
				    strcmp(fstype, "nfs") != 0 &&
				    strcmp(fstype, "cifs") != 0 &&
				    !(is_media && strncmp(fstype, "fuse", 4) == 0)) continue;
				if (strncmp(mnt, "/boot", 5) == 0 || strcmp(mnt, "/efi") == 0 || strncmp(mnt, "/snap", 5) == 0) continue;

				int dup = 0;
				for (int i = 0; i < count; i++) {
					if (strcmp(paths[i], mnt) == 0) { dup = 1; break; }
				}
				if (dup) continue;

				snprintf(paths[count], MAX_PATH, "%s", mnt);
				snprintf(devs[count], sizeof(devs[0]), "%s", dev);
				can_unmount[count] = (is_media && strncmp(dev, "/dev/", 5) == 0) ? 1 : 0;
				const char *bname = strrchr(mnt, '/');
				bname = (bname && *(bname + 1)) ? (bname + 1) : mnt;
				snprintf(labels[count], sizeof(labels[0]), "%s (%s)", bname, fstype);
				count++;
			}
			fclose(mf);
		}

		if (home && *home) {
			char bpath[MAX_PATH + 64];
			snprintf(bpath, sizeof(bpath), "%s/.config/xcommander/bookmarks", home);
			FILE *bf = fopen(bpath, "r");
			if (bf) {
				char line[MAX_PATH];
				while (fgets(line, sizeof(line), bf) && count < 48) {
					size_t l = strlen(line);
					while (l > 0 && (line[l - 1] == '\n' || line[l - 1] == '\r' || line[l - 1] == ' ')) line[--l] = '\0';
					if (line[0] == '\0' || line[0] == '#') continue;
					int dup = 0;
					for (int i = 0; i < count; i++) {
						if (strcmp(paths[i], line) == 0) { dup = 1; break; }
					}
					if (dup) continue;
					snprintf(paths[count], MAX_PATH, "%s", line);
					devs[count][0] = '\0';
					can_unmount[count] = 0;
					const char *bname = strrchr(line, '/');
					bname = (bname && *(bname + 1)) ? (bname + 1) : line;
					snprintf(labels[count], sizeof(labels[0]), "* %s", bname);
					count++;
				}
				fclose(bf);
			}
		}

		char free_info[48][32];
		int sel = 0;
		for (int i = 0; i < count; i++) {
			struct statvfs vfs;
			if (statvfs(paths[i], &vfs) == 0) {
				off_t avail = (off_t)vfs.f_bavail * (off_t)vfs.f_frsize;
				char sz[32];
				format_size(avail, sz, sizeof(sz));
				snprintf(free_info[i], sizeof(free_info[0]), "%s", sz);
			} else {
				free_info[i][0] = '\0';
			}
			if (strcmp(paths[i], target_panel->current_path) == 0) sel = i;
		}

		int height, width;
		getmaxyx(stdscr, height, width);

		int panel_w = width / 2;
		int box_w = panel_w - 4;
		if (box_w > 56) box_w = 56;
		if (box_w < 42) box_w = 42;
		if (box_w > width - 2) box_w = width - 2;

		int box_h = count + 5;
		if (box_h > height - 4) box_h = height - 4;
		if (box_h < 7) box_h = 7;

		int panel_x = is_right ? panel_w : 0;
		int start_x = panel_x + (panel_w - box_w) / 2;
		if (start_x < 1) start_x = 1;
		if (start_x + box_w >= width) start_x = width - box_w - 1;
		int start_y = (height - 2 - box_h) / 2;
		if (start_y < 1) start_y = 1;

		curs_set(0);
		WINDOW *win = newwin(box_h, box_w, start_y, start_x);
		wbkgd(win, COLOR_PAIR(5));

		const char *keys = "0123456789abcdefghijklmnopqrstuvwxyz";
		int chosen = -1;
		int do_unmount = 0;
		int btn_focus = 0;

		while (1) {
			int vis = box_h - 4;
			if (vis < 1) vis = 1;
			int top = 0;
			if (sel >= vis) top = sel - vis + 1;

			werase(win);
			draw_win_box(win);
			mvwprintw(win, 0, (box_w - 16) / 2, is_right ? " Change Right " : " Change Left ");

			for (int i = 0; i < vis && (top + i) < count; i++) {
				int idx = top + i;
				char kch = (idx < 36) ? keys[idx] : ' ';
				int flen = (int)strlen(free_info[idx]);
				int left_w = box_w - 6 - flen;
				if (left_w < 8) left_w = 8;

				char left_str[160];
				snprintf(left_str, sizeof(left_str), "%c. %s", kch, labels[idx]);

				wattron(win, idx == sel ? COLOR_PAIR(2) : COLOR_PAIR(5));
				mvwhline(win, 1 + i, 1, ' ', box_w - 2);
				mvwprint_utf8(win, 1 + i, 2, left_w, left_str);
				if (flen > 0 && box_w - 3 - flen > 4) {
					mvwprintw(win, 1 + i, box_w - 2 - flen, "%s", free_info[idx]);
				}
				wattroff(win, idx == sel ? COLOR_PAIR(2) : COLOR_PAIR(5));
			}

			const char *b_sel = "[ Select ]";
			const char *b_unm = "[ Unmount (F8) ]";
			const char *b_can = "[ Cancel ]";
			int total_bw = 10 + 2 + 16 + 2 + 10;
			int bx = (box_w - total_bw) / 2;
			if (bx < 2) bx = 2;

			wattron(win, btn_focus == 0 ? COLOR_PAIR(2) : COLOR_PAIR(5));
			mvwprintw(win, box_h - 2, bx, "%s", b_sel);
			wattroff(win, btn_focus == 0 ? COLOR_PAIR(2) : COLOR_PAIR(5));

			wattron(win, btn_focus == 1 ? COLOR_PAIR(2) : COLOR_PAIR(5));
			mvwprintw(win, box_h - 2, bx + 12, "%s", b_unm);
			wattroff(win, btn_focus == 1 ? COLOR_PAIR(2) : COLOR_PAIR(5));

			wattron(win, btn_focus == 2 ? COLOR_PAIR(2) : COLOR_PAIR(5));
			mvwprintw(win, box_h - 2, bx + 30, "%s", b_can);
			wattroff(win, btn_focus == 2 ? COLOR_PAIR(2) : COLOR_PAIR(5));
			wrefresh(win);

			InputEvent ev = input_read();
			if (ev.action == ACT_ESC || ev.action == ACT_F10) break;
			if (ev.action == ACT_TAB || ev.action == ACT_RIGHT) { btn_focus = (btn_focus + 1) % 3; continue; }
			if (ev.action == ACT_LEFT) { btn_focus = (btn_focus + 2) % 3; continue; }
			if (ev.action == ACT_MOUSE_WHEEL_UP) { btn_focus = 0; if (sel > 0) sel--; continue; }
			if (ev.action == ACT_MOUSE_WHEEL_DOWN) { btn_focus = 0; if (sel + 1 < count) sel++; continue; }
			if (ev.action == ACT_MOUSE_LEFT) {
				int wy = ev.mouse_y - start_y, wx = ev.mouse_x - start_x;
				if (wy < 0 || wy >= box_h || wx < 0 || wx >= box_w) break;
				if (wy == box_h - 2) {
					if (wx >= bx && wx < bx + 10 && count > 0) { chosen = sel; break; }
					if (wx >= bx + 12 && wx < bx + 28 && count > 0) { do_unmount = 1; break; }
					if (wx >= bx + 30 && wx < bx + 40) break;
					continue;
				}
				if (wy >= 1 && wy < 1 + vis && (top + wy - 1) < count) {
					chosen = top + (wy - 1);
					break;
				}
				continue;
			}
			if (ev.action == ACT_F8 || ev.action == ACT_DELETE) {
				if (count > 0) { do_unmount = 1; break; }
			}
			if (ev.action == ACT_UP && sel > 0) { btn_focus = 0; sel--; }
			else if (ev.action == ACT_DOWN && sel + 1 < count) { btn_focus = 0; sel++; }
			else if (ev.action == ACT_ENTER) {
				if (btn_focus == 2) break;
				if (btn_focus == 1 && count > 0) { do_unmount = 1; break; }
				if (count > 0) { chosen = sel; break; }
			} else if (ev.action == ACT_CHAR) {
				wchar_t ch = towlower(ev.ch);
				for (int i = 0; i < count && i < 36; i++) {
					if ((wchar_t)keys[i] == ch) { chosen = i; break; }
				}
				if (chosen >= 0) break;
			}
		}

		delwin(win);
		touchwin(stdscr);

		if (do_unmount && sel >= 0 && sel < count) {
			if (!can_unmount[sel] || devs[sel][0] == '\0') {
				draw_screen(); refresh();
				ui_message("Unmount", "Only removable / media drives can be unmounted");
				draw_screen(); refresh();
				continue;
			}
			char qmsg[160];
			snprintf(qmsg, sizeof(qmsg), "Unmount %s?", labels[sel]);
			draw_screen(); refresh();
			int ok_unm = ui_confirm("Unmount (F8)", qmsg);
			draw_screen(); refresh();
			if (!ok_unm) continue;
			const char *fallback = (home && *home) ? home : "/";
			size_t mlen = strlen(paths[sel]);
			if (strncmp(left_panel.current_path, paths[sel], mlen) == 0 &&
			    (left_panel.current_path[mlen] == '/' || left_panel.current_path[mlen] == '\0')) {
				snprintf(left_panel.current_path, MAX_PATH, "%s", fallback);
				load_directory(&left_panel);
			}
			if (strncmp(right_panel.current_path, paths[sel], mlen) == 0 &&
			    (right_panel.current_path[mlen] == '/' || right_panel.current_path[mlen] == '\0')) {
				snprintf(right_panel.current_path, MAX_PATH, "%s", fallback);
				load_directory(&right_panel);
			}
			draw_screen(); refresh();

			char ucmd[512];
			snprintf(ucmd, sizeof(ucmd), "udisksctl unmount -b \"%s\" 2>&1", devs[sel]);
			FILE *up = popen(ucmd, "r");
			char out_msg[256] = "Unmounted safely";
			int status = -1;
			if (up) {
				char buf[256] = {0};
				if (fgets(buf, sizeof(buf), up)) {
					size_t bl = strlen(buf);
					while (bl > 0 && (buf[bl - 1] == '\n' || buf[bl - 1] == '\r')) buf[--bl] = '\0';
					if (bl > 0) snprintf(out_msg, sizeof(out_msg), "%s", buf);
				}
				status = pclose(up);
			}
			draw_screen(); refresh();
			ui_message(status == 0 ? "Unmount" : "Unmount Error", out_msg);
			draw_screen(); refresh();
			continue;
		}

		if (chosen >= 0 && chosen < count) {
			snprintf(target_panel->current_path, MAX_PATH, "%s", paths[chosen]);
			target_panel->parent_name[0] = '\0';
			load_directory(target_panel);
			active_panel = target_panel;
		}
		break;
	}
}

int ui_replace_dialog(char *search_buf, int s_max, char *replace_buf, int r_max) {
	int height, width;
	getmaxyx(stdscr, height, width);

	int box_w = width - 10;
	if (box_w > 70) box_w = 70;
	if (box_w < 38) box_w = 38;
	int box_h = 9;

	int start_y = (height - box_h) / 2;
	int start_x = (width - box_w) / 2;

	WINDOW *win = newwin(box_h, box_w, start_y, start_x);
	keypad(win, TRUE);
	wbkgd(win, COLOR_PAIR(5));

	wchar_t wbuf[2][MAX_PATH];
	int len[2] = {0, 0};
	int pos[2] = {0, 0};
	int anchor[2] = {-1, -1};

	for (int f = 0; f < 2; f++) {
		const char *src = (f == 0) ? search_buf : replace_buf;
		size_t c = mbstowcs(wbuf[f], src, MAX_PATH - 1);
		len[f] = (c == (size_t)-1) ? 0 : (int)c;
		wbuf[f][len[f]] = L'\0';
		pos[f] = len[f];
	}

	int field_w = box_w - 6;
	int focus = 0; /* 0 = Search, 1 = Replace, 2 = [Replace], 3 = [Cancel] */
	int res = 0;

	const char *btn_ok  = "[ Replace ]";
	const char *btn_can = "[ Cancel ]";
	int total_btn_w = 11 + 4 + 10;
	int btn_x = (box_w - total_btn_w) / 2;
	if (btn_x < 2) btn_x = 2;

	while (1) {
		werase(win);
		draw_win_box(win);
		mvwprintw(win, 0, 2, " Replace (Ctrl+F7) ");
		mvwprint_utf8(win, 1, 3, field_w, "Search for:");
		mvwprint_utf8(win, 4, 3, field_w, "Replace with:");

		int offsets[2] = {0, 0};
		for (int f = 0; f < 2; f++) {
			int fy = (f == 0) ? 2 : 5;
			wattron(win, focus == f ? COLOR_PAIR(2) : COLOR_PAIR(6));
			mvwhline(win, fy, 3, ' ', field_w);
			if (pos[f] >= field_w) offsets[f] = pos[f] - field_w + 1;
			mvwaddnwstr(win, fy, 3, wbuf[f] + offsets[f], field_w);
			wattroff(win, focus == f ? COLOR_PAIR(2) : COLOR_PAIR(6));

			if (focus == f && anchor[f] != -1 && anchor[f] != pos[f]) {
				int s1 = (anchor[f] < pos[f]) ? anchor[f] : pos[f];
				int s2 = (anchor[f] > pos[f]) ? anchor[f] : pos[f];
				if (s1 < offsets[f]) s1 = offsets[f];
				if (s2 > offsets[f] + field_w) s2 = offsets[f] + field_w;
				if (s2 > s1) {
					wattron(win, COLOR_PAIR(5));
					mvwaddnwstr(win, fy, 3 + (s1 - offsets[f]), wbuf[f] + s1, s2 - s1);
					wattroff(win, COLOR_PAIR(5));
				}
			}
		}

		wattron(win, focus == 2 ? COLOR_PAIR(2) : COLOR_PAIR(5));
		mvwprintw(win, 7, btn_x, "%s", btn_ok);
		wattroff(win, focus == 2 ? COLOR_PAIR(2) : COLOR_PAIR(5));

		wattron(win, focus == 3 ? COLOR_PAIR(2) : COLOR_PAIR(5));
		mvwprintw(win, 7, btn_x + 15, "%s", btn_can);
		wattroff(win, focus == 3 ? COLOR_PAIR(2) : COLOR_PAIR(5));

		if (focus == 0 || focus == 1) {
			curs_set(1);
			int fy = (focus == 0) ? 2 : 5;
			wmove(win, fy, 3 + (pos[focus] - offsets[focus]));
		} else {
			curs_set(0);
		}
		wrefresh(win);

		InputEvent ev = input_read();
		if (ev.action == ACT_NONE || ev.action == ACT_MOD_CHANGE) continue;

		if (ev.action == ACT_MOUSE_LEFT) {
			int wy = ev.mouse_y - start_y, wx = ev.mouse_x - start_x;
			if (wy < 0 || wy >= box_h || wx < 0 || wx >= box_w) { res = 0; break; }
			if (wy == 2) { focus = 0; anchor[0] = -1; int np = offsets[0] + (wx - 3); if (np < 0) np = 0; if (np > len[0]) np = len[0]; pos[0] = np; continue; }
			if (wy == 5) { focus = 1; anchor[1] = -1; int np = offsets[1] + (wx - 3); if (np < 0) np = 0; if (np > len[1]) np = len[1]; pos[1] = np; continue; }
			if (wy == 7) {
				if (wx >= btn_x && wx < btn_x + 11) { ev.action = ACT_ENTER; focus = 2; }
				else if (wx >= btn_x + 15 && wx < btn_x + 25) { res = 0; break; }
				else continue;
			} else continue;
		}

		if (ev.action == ACT_ESC || ev.action == ACT_F10) { res = 0; break; }
		if (ev.action == ACT_TAB) { focus = (focus + 1) % 4; continue; }
		if (ev.action == ACT_DOWN) { if (focus < 2) focus++; continue; }
		if (ev.action == ACT_UP) { if (focus > 0) focus = (focus == 3) ? 1 : (focus - 1); continue; }
		if (ev.action == ACT_ENTER) {
			if (focus == 3) { res = 0; break; }
			wcstombs(search_buf, wbuf[0], (size_t)s_max); search_buf[s_max - 1] = '\0';
			wcstombs(replace_buf, wbuf[1], (size_t)r_max); replace_buf[r_max - 1] = '\0';
			res = 1;
			break;
		}

		if (focus == 2 || focus == 3) {
			if (ev.action == ACT_LEFT || ev.action == ACT_RIGHT) focus = (focus == 2) ? 3 : 2;
			continue;
		}

		int f = focus;
		if (ev.action == ACT_WORD_SEL_LEFT) { if (anchor[f] == -1) anchor[f] = pos[f]; pos[f] = ui_word_left(wbuf[f], pos[f]); }
		else if (ev.action == ACT_WORD_SEL_RIGHT) { if (anchor[f] == -1) anchor[f] = pos[f]; pos[f] = ui_word_right(wbuf[f], pos[f], len[f]); }
		else if (ev.action == ACT_SHIFT_LEFT) { if (anchor[f] == -1) anchor[f] = pos[f]; if (pos[f] > 0) pos[f]--; }
		else if (ev.action == ACT_SHIFT_RIGHT) { if (anchor[f] == -1) anchor[f] = pos[f]; if (pos[f] < len[f]) pos[f]++; }
		else if (ev.action == ACT_CTRL_LEFT) { anchor[f] = -1; pos[f] = ui_word_left(wbuf[f], pos[f]); }
		else if (ev.action == ACT_CTRL_RIGHT) { anchor[f] = -1; pos[f] = ui_word_right(wbuf[f], pos[f], len[f]); }
		else if (ev.action == ACT_LEFT) { anchor[f] = -1; if (pos[f] > 0) pos[f]--; }
		else if (ev.action == ACT_RIGHT) { anchor[f] = -1; if (pos[f] < len[f]) pos[f]++; }
		else if (ev.action == ACT_HOME || ev.action == ACT_CTRL_A) { anchor[f] = -1; pos[f] = 0; }
		else if (ev.action == ACT_END) { anchor[f] = -1; pos[f] = len[f]; }
		else if (ev.action == ACT_BACKSPACE) {
			if (anchor[f] != -1 && anchor[f] != pos[f]) {
				int s1 = (anchor[f] < pos[f]) ? anchor[f] : pos[f], s2 = (anchor[f] > pos[f]) ? anchor[f] : pos[f];
				wmemmove(wbuf[f] + s1, wbuf[f] + s2, (size_t)(len[f] - s2 + 1)); len[f] -= (s2 - s1); pos[f] = s1; anchor[f] = -1;
			} else if (pos[f] > 0) { wmemmove(wbuf[f] + pos[f] - 1, wbuf[f] + pos[f], (size_t)(len[f] - pos[f] + 1)); pos[f]--; len[f]--; }
		} else if (ev.action == ACT_DELETE) {
			if (anchor[f] != -1 && anchor[f] != pos[f]) {
				int s1 = (anchor[f] < pos[f]) ? anchor[f] : pos[f], s2 = (anchor[f] > pos[f]) ? anchor[f] : pos[f];
				wmemmove(wbuf[f] + s1, wbuf[f] + s2, (size_t)(len[f] - s2 + 1)); len[f] -= (s2 - s1); pos[f] = s1; anchor[f] = -1;
			} else if (pos[f] < len[f]) { wmemmove(wbuf[f] + pos[f], wbuf[f] + pos[f] + 1, (size_t)(len[f] - pos[f])); len[f]--; }
		} else if (ev.action == ACT_CTRL_C || ev.action == ACT_CTRL_Y || ev.action == ACT_CTRL_X) {
			int s1 = (anchor[f] != -1 && anchor[f] != pos[f]) ? ((anchor[f] < pos[f]) ? anchor[f] : pos[f]) : 0;
			int s2 = (anchor[f] != -1 && anchor[f] != pos[f]) ? ((anchor[f] > pos[f]) ? anchor[f] : pos[f]) : len[f];
			if (s2 > s1) {
				wchar_t wsub[MAX_PATH]; wmemcpy(wsub, wbuf[f] + s1, (size_t)(s2 - s1)); wsub[s2 - s1] = L'\0';
				char utf_sub[MAX_PATH];
				if (wcstombs(utf_sub, wsub, sizeof(utf_sub)) != (size_t)-1) { utf_sub[sizeof(utf_sub) - 1] = '\0'; clipboard_copy(utf_sub); }
				if (ev.action == ACT_CTRL_X && anchor[f] != -1 && anchor[f] != pos[f]) {
					wmemmove(wbuf[f] + s1, wbuf[f] + s2, (size_t)(len[f] - s2 + 1)); len[f] -= (s2 - s1); pos[f] = s1; anchor[f] = -1;
				}
			}
		} else if (ev.action == ACT_CTRL_V || ev.action == ACT_SHIFT_INSERT) {
			if (anchor[f] != -1 && anchor[f] != pos[f]) {
				int s1 = (anchor[f] < pos[f]) ? anchor[f] : pos[f], s2 = (anchor[f] > pos[f]) ? anchor[f] : pos[f];
				wmemmove(wbuf[f] + s1, wbuf[f] + s2, (size_t)(len[f] - s2 + 1)); len[f] -= (s2 - s1); pos[f] = s1; anchor[f] = -1;
			}
			char raw[MAX_PATH] = {0};
			if (clipboard_paste(raw, sizeof(raw)) > 0) {
				wchar_t wadd[MAX_PATH]; size_t alen = mbstowcs(wadd, raw, MAX_PATH - 1);
				if (alen != (size_t)-1 && alen > 0) {
					if (len[f] + (int)alen >= MAX_PATH - 2) alen = (size_t)(MAX_PATH - 2 - len[f]);
					wmemmove(wbuf[f] + pos[f] + alen, wbuf[f] + pos[f], (size_t)(len[f] - pos[f] + 1));
					wmemcpy(wbuf[f] + pos[f], wadd, alen); pos[f] += (int)alen; len[f] += (int)alen;
				}
			}
		} else if (ev.action == ACT_CTRL_U) {
			len[f] = 0; pos[f] = 0; anchor[f] = -1; wbuf[f][0] = L'\0';
		} else if (ev.action == ACT_CHAR && iswprint(ev.ch) && len[f] < MAX_PATH - 2) {
			if (anchor[f] != -1 && anchor[f] != pos[f]) {
				int s1 = (anchor[f] < pos[f]) ? anchor[f] : pos[f], s2 = (anchor[f] > pos[f]) ? anchor[f] : pos[f];
				wmemmove(wbuf[f] + s1, wbuf[f] + s2, (size_t)(len[f] - s2 + 1)); len[f] -= (s2 - s1); pos[f] = s1; anchor[f] = -1;
			}
			wmemmove(wbuf[f] + pos[f] + 1, wbuf[f] + pos[f], (size_t)(len[f] - pos[f] + 1)); wbuf[f][pos[f]++] = ev.ch; len[f]++;
		}
	}

	curs_set(0);
	delwin(win);
	touchwin(stdscr);
	return res;
}

/* Возвращает: 1 = Replace, 2 = All, 3 = Skip, 0 = Cancel */
int ui_replace_prompt(const char *search_str, const char *replace_str, int avoid_y) {
	int height, width;
	getmaxyx(stdscr, height, width);

	int box_w = 50;
	if (box_w > width - 2) box_w = width - 2;
	int box_h = 8;

	int start_x = (width - box_w) / 2;
	int start_y = height - box_h - 2;
	if (avoid_y >= start_y - 1 && avoid_y <= start_y + box_h) {
		start_y = 2;
	}
	if (start_y < 1) start_y = 1;

	curs_set(0);
	WINDOW *win = newwin(box_h, box_w, start_y, start_x);
	wbkgd(win, COLOR_PAIR(5));

	char line_s[160], line_r[160];
	snprintf(line_s, sizeof(line_s), "\"%s\"", search_str);
	snprintf(line_r, sizeof(line_r), "\"%s\"", replace_str);

	const char *btns[4] = { "[ Replace ]", "[ All ]", "[ Skip ]", "[ Cancel ]" };
	int bw[4] = { 11, 7, 8, 10 };
	int total_bw = 11 + 1 + 7 + 1 + 8 + 1 + 10;
	int bx0 = (box_w - total_bw) / 2;
	if (bx0 < 1) bx0 = 1;

	int sel = 0;
	int res = 0;

	while (1) {
		werase(win);
		draw_win_box(win);
		mvwprintw(win, 0, (box_w - 9) / 2, " Replace ");
		mvwprintw(win, 1, (box_w - 7) / 2, "Replace");

		int sl = (int)mbstowcs(NULL, line_s, 0); if (sl < 0) sl = (int)strlen(line_s);
		if (sl > box_w - 4) sl = box_w - 4;
		mvwprint_utf8(win, 2, (box_w - sl) / 2, box_w - 4, line_s);

		mvwprintw(win, 3, (box_w - 4) / 2, "with");

		int rl = (int)mbstowcs(NULL, line_r, 0); if (rl < 0) rl = (int)strlen(line_r);
		if (rl > box_w - 4) rl = box_w - 4;
		mvwprint_utf8(win, 4, (box_w - rl) / 2, box_w - 4, line_r);

		for (int i = 1; i < box_w - 1; i++) mvwaddstr(win, 5, i, "─");
		mvwaddstr(win, 5, 0, "├");
		mvwaddstr(win, 5, box_w - 1, "┤");

		int cx = bx0;
		for (int i = 0; i < 4; i++) {
			wattron(win, sel == i ? COLOR_PAIR(2) : COLOR_PAIR(5));
			mvwprintw(win, 6, cx, "%s", btns[i]);
			wattroff(win, sel == i ? COLOR_PAIR(2) : COLOR_PAIR(5));
			cx += bw[i] + 1;
		}
		wrefresh(win);

		InputEvent ev = input_read();
		if (ev.action == ACT_NONE || ev.action == ACT_MOD_CHANGE) continue;

		if (ev.action == ACT_MOUSE_LEFT) {
			int wy = ev.mouse_y - start_y, wx = ev.mouse_x - start_x;
			if (wy < 0 || wy >= box_h || wx < 0 || wx >= box_w) { res = 0; break; }
			if (wy == 6) {
				int px = bx0;
				for (int i = 0; i < 4; i++) {
					if (wx >= px && wx < px + bw[i]) {
						res = (i == 3) ? 0 : (i + 1);
						goto done_rp;
					}
					px += bw[i] + 1;
				}
			}
			continue;
		}

		if (ev.action == ACT_ESC || ev.action == ACT_F10) { res = 0; break; }
		if (ev.action == ACT_LEFT) { sel = (sel + 3) % 4; continue; }
		if (ev.action == ACT_RIGHT || ev.action == ACT_TAB) { sel = (sel + 1) % 4; continue; }
		if (ev.action == ACT_ENTER || (ev.action == ACT_CHAR && ev.ch == L' ')) {
			res = (sel == 3) ? 0 : (sel + 1);
			break;
		}
		if (ev.action == ACT_CHAR) {
			wchar_t c = towlower(ev.ch);
			if (c == L'r' || c == L'y' || c == L'к' || c == L'н') { res = 1; break; }
			if (c == L'a' || c == L'ф') { res = 2; break; }
			if (c == L's' || c == L'n' || c == L'ы' || c == L'т') { res = 3; break; }
			if (c == L'c' || c == L'с') { res = 0; break; }
		}
	}

done_rp:
	delwin(win);
	touchwin(stdscr);
	return res;
}

static void get_ml_history_path(char *out, size_t out_size) {
	const char *state_home = getenv("XDG_STATE_HOME");
	char base_dir[MAX_PATH];
	if (state_home && *state_home) {
		snprintf(base_dir, sizeof(base_dir), "%s/xcommander", state_home);
	} else {
		const char *home = getenv("HOME");
		if (!home || !*home) home = "/tmp";
		char d1[MAX_PATH];
		snprintf(d1, sizeof(d1), "%s/.local", home); mkdir(d1, 0755);
		snprintf(d1, sizeof(d1), "%s/.local/state", home); mkdir(d1, 0755);
		snprintf(base_dir, sizeof(base_dir), "%s/.local/state/xcommander", home);
	}
	mkdir(base_dir, 0755);
	snprintf(out, out_size, "%s/multiline_history", base_dir);
}

void ml_history_add(const char *dir, const char *script) {
	if (!script || !*script) return;
	char path[MAX_PATH + 48];
	get_ml_history_path(path, sizeof(path));

	time_t now = time(NULL);
	struct tm *tm_i = localtime(&now);
	char tstr[32] = "0000-00-00 00:00:00";
	if (tm_i) strftime(tstr, sizeof(tstr), "%Y-%m-%d %H:%M:%S", tm_i);

	char *old_data = NULL;
	FILE *rf = fopen(path, "rb");
	if (rf) {
		fseek(rf, 0, SEEK_END);
		long sz = ftell(rf);
		fseek(rf, 0, SEEK_SET);
		if (sz > 0 && sz < 512 * 1024) {
			old_data = malloc((size_t)sz + 1);
			if (old_data) {
				size_t rn = fread(old_data, 1, (size_t)sz, rf);
				old_data[rn] = '\0';
			}
		}
		fclose(rf);
	}

	FILE *wf = fopen(path, "wb");
	if (wf) {
		fprintf(wf, "<<<XC_ML_ENTRY %s | %s>>>\n%s\n<<<XC_ML_END>>>\n", tstr, dir ? dir : ".", script);
		if (old_data) fwrite(old_data, 1, strlen(old_data), wf);
		fclose(wf);
	}
	free(old_data);
}

int ui_multiline_history(char *out_script, size_t out_size) {
	char path[MAX_PATH + 48];
	get_ml_history_path(path, sizeof(path));

	FILE *f = fopen(path, "rb");
	if (!f) {
		ui_message("Multiline History (Alt+H)", "Multiline command history is empty");
		return 0;
	}
	fseek(f, 0, SEEK_END);
	long fsz = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (fsz <= 0) {
		fclose(f);
		ui_message("Multiline History (Alt+H)", "Multiline command history is empty");
		return 0;
	}
	char *raw = malloc((size_t)fsz + 1);
	if (!raw) { fclose(f); return 0; }
	size_t n = fread(raw, 1, (size_t)fsz, f);
	fclose(f);
	raw[n] = '\0';

	char *headers[32] = {0};
	char *bodies[32] = {0};
	int count = 0;

	char *p = raw;
	while (count < 32 && (p = strstr(p, "<<<XC_ML_ENTRY ")) != NULL) {
		p += 15;
		char *hend = strstr(p, ">>>\n");
		if (!hend) break;
		*hend = '\0';
		char *body_start = hend + 4;
		char *bend = strstr(body_start, "\n<<<XC_ML_END>>>");
		if (!bend) break;
		*bend = '\0';

		headers[count] = strdup(p);
		bodies[count] = strdup(body_start);
		count++;
		p = bend + 16;
	}
	free(raw);

	if (count == 0) {
		ui_message("Multiline History (Alt+H)", "Multiline command history is empty");
		return 0;
	}

	int height, width;
	getmaxyx(stdscr, height, width);
	int box_w = width - 6;
	if (box_w > 86) box_w = 86;
	if (box_w < 46) box_w = 46;
	int box_h = height - 4;
	if (box_h > 24) box_h = 24;
	if (box_h < 12) box_h = 12;

	int start_y = (height - box_h) / 2;
	int start_x = (width - box_w) / 2;

	curs_set(0);
	WINDOW *win = newwin(box_h, box_w, start_y, start_x);
	wbkgd(win, COLOR_PAIR(5));

	int sel = 0;
	int btn_focus = 0;
	int chosen = -1;

	while (1) {
		werase(win);
		draw_win_box(win);
		mvwprintw(win, 0, 2, " Multiline Command History (Alt+H) [%d/%d] ", sel + 1, count);

		int list_h = (box_h - 5) / 2;
		if (list_h < 3) list_h = 3;
		int prev_y = 1 + list_h;
		int prev_h = box_h - 3 - prev_y;

		int top = 0;
		if (sel >= list_h) top = sel - list_h + 1;

		for (int i = 0; i < list_h && (top + i) < count; i++) {
			int idx = top + i;
			char summary[256];
			const char *b = bodies[idx] ? bodies[idx] : "";
			while (*b == ' ' || *b == '\t' || *b == '\n') b++;
			char first_line[96] = {0};
			int k = 0;
			while (b[k] && b[k] != '\n' && b[k] != '\r' && k < 90) { first_line[k] = b[k]; k++; }
			first_line[k] = '\0';
			snprintf(summary, sizeof(summary), "%s | %s", headers[idx], first_line);

			wattron(win, idx == sel ? COLOR_PAIR(2) : COLOR_PAIR(5));
			mvwhline(win, 1 + i, 1, ' ', box_w - 2);
			mvwprint_utf8(win, 1 + i, 2, box_w - 4, summary);
			wattroff(win, idx == sel ? COLOR_PAIR(2) : COLOR_PAIR(5));
		}

		for (int i = 1; i < box_w - 1; i++) mvwaddstr(win, prev_y, i, "─");
		mvwaddstr(win, prev_y, 0, "├");
		mvwaddstr(win, prev_y, box_w - 1, "┤");
		mvwprintw(win, prev_y, 2, " Preview ");

		const char *cur_body = bodies[sel] ? bodies[sel] : "";
		const char *lp = cur_body;
		for (int r = 0; r < prev_h && *lp; r++) {
			const char *nl = strchr(lp, '\n');
			int llen = nl ? (int)(nl - lp) : (int)strlen(lp);
			char lbuf[512];
			if (llen >= (int)sizeof(lbuf)) llen = (int)sizeof(lbuf) - 1;
			memcpy(lbuf, lp, (size_t)llen);
			lbuf[llen] = '\0';
			mvwprint_utf8(win, prev_y + 1 + r, 2, box_w - 4, lbuf);
			if (!nl) break;
			lp = nl + 1;
		}

		const char *b_exec = "[ Execute ]";
		const char *b_canc = "[ Close ]";
		int bx = (box_w - 24) / 2;
		wattron(win, btn_focus == 0 ? COLOR_PAIR(2) : COLOR_PAIR(5));
		mvwprintw(win, box_h - 2, bx, "%s", b_exec);
		wattroff(win, btn_focus == 0 ? COLOR_PAIR(2) : COLOR_PAIR(5));
		wattron(win, btn_focus == 1 ? COLOR_PAIR(2) : COLOR_PAIR(5));
		mvwprintw(win, box_h - 2, bx + 15, "%s", b_canc);
		wattroff(win, btn_focus == 1 ? COLOR_PAIR(2) : COLOR_PAIR(5));
		wrefresh(win);

		InputEvent ev = input_read();
		if (ev.action == ACT_ESC || ev.action == ACT_F10) break;
		if (ev.action == ACT_TAB || ev.action == ACT_LEFT || ev.action == ACT_RIGHT) { btn_focus = 1 - btn_focus; continue; }
		if (ev.action == ACT_MOUSE_WHEEL_UP) { btn_focus = 0; if (sel > 0) sel--; continue; }
		if (ev.action == ACT_MOUSE_WHEEL_DOWN) { btn_focus = 0; if (sel + 1 < count) sel++; continue; }
		if (ev.action == ACT_MOUSE_LEFT) {
			int wy = ev.mouse_y - start_y, wx = ev.mouse_x - start_x;
			if (wy < 0 || wy >= box_h || wx < 0 || wx >= box_w) break;
			if (wy == box_h - 2) {
				if (wx >= bx && wx < bx + 11) { chosen = sel; break; }
				if (wx >= bx + 15 && wx < bx + 24) break;
				continue;
			}
			if (wy >= 1 && wy < 1 + list_h && (top + wy - 1) < count) {
				sel = top + (wy - 1);
				btn_focus = 0;
			}
			continue;
		}
		if (ev.action == ACT_UP && sel > 0) { btn_focus = 0; sel--; }
		else if (ev.action == ACT_DOWN && sel + 1 < count) { btn_focus = 0; sel++; }
		else if (ev.action == ACT_ENTER) {
			if (btn_focus == 1) break;
			chosen = sel;
			break;
		}
	}

	if (chosen >= 0 && chosen < count && bodies[chosen]) {
		snprintf(out_script, out_size, "%s", bodies[chosen]);
	}
	for (int i = 0; i < count; i++) {
		free(headers[i]);
		free(bodies[i]);
	}
	delwin(win);
	touchwin(stdscr);
	return (chosen >= 0) ? 1 : 0;
}
