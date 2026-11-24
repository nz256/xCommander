#define _GNU_SOURCE
#include <sys/ioctl.h>
#include "cmdline.h"
#include <unistd.h>
#include <fnmatch.h>
#include <locale.h>
#include <fcntl.h>
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
#include "info_panel.h"
#include "widgets.h"
#include "exec.h"
#include "panel.h"
#include "input.h"
#include "editor.h"

int g_auto_colorize = 0;

static void get_color_state_path(char *out, size_t out_sz) {
	const char *state_home = getenv("XDG_STATE_HOME");
	char base[MAX_PATH];
	if (state_home && *state_home) {
		snprintf(base, sizeof(base), "%s/xcommander", state_home);
	} else {
		const char *home = getenv("HOME");
		if (!home || !*home) home = "/tmp";
		char tmp[MAX_PATH];
		snprintf(tmp, sizeof(tmp), "%s/.local", home); mkdir(tmp, 0755);
		snprintf(tmp, sizeof(tmp), "%s/.local/state", home); mkdir(tmp, 0755);
		snprintf(base, sizeof(base), "%s/.local/state/xcommander", home);
	}
	mkdir(base, 0755);
	snprintf(out, out_sz, "%s/colors.state", base);
}

static void ui_load_color_state(void) {
	char path[MAX_PATH + 32];
	get_color_state_path(path, sizeof(path));
	FILE *f = fopen(path, "r");
	if (!f) return;
	int val = 0;
	if (fscanf(f, "auto_color=%d", &val) == 1) g_auto_colorize = val ? 1 : 0;
	fclose(f);
}

void ui_toggle_auto_colorize(void) {
	g_auto_colorize = !g_auto_colorize;
	char path[MAX_PATH + 32];
	get_color_state_path(path, sizeof(path));
	FILE *f = fopen(path, "w");
	if (f) {
		fprintf(f, "auto_color=%d\n", g_auto_colorize);
		fclose(f);
	}
}

static int auto_ext_color_attr(const char *name, int on_cursor) {
	if (!g_auto_colorize || !name || !*name) return 0;
	const char *dot = strrchr(name, '.');
	if (!dot || dot == name || *(dot + 1) == '\0') return 0;
	unsigned int h = 2166136261u;
	for (const char *p = dot + 1; *p; p++) {
		unsigned char c = (unsigned char)tolower((unsigned char)*p);
		h ^= c;
		h *= 16777619u;
	}
	int idx = (int)(h % 32u);
	return COLOR_PAIR((on_cursor ? 52 : 20) + idx);
}


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

static int g_dialog_overlay = 0;

static void ui_init_ex(int overlay) {
	setlocale(LC_ALL, "");
	g_dialog_overlay = overlay;
	if (overlay) { input_enable_raw(); return; }
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
		init_pair(14, 226, 18);  /* Синтаксис: строки в кавычках (ярко-жёлтый) */
		init_pair(15, 214, 18);  /* Синтаксис: числа (ярко-оранжевый) */
		init_pair(16, 117, 18);  /* Синтаксис: комментарии (яркий небесно-голубой) */
		init_pair(17, 231, 160); /* Непарная скобка (белый на красном) */
		init_pair(18, 213, 18);  /* Синтаксис: директивы и константы (ярко-пурпурный) */
		init_pair(19, 118, 18);  /* Синтаксис: операторы и скобки (ярко-салатовый) */
		init_pair(84, 67, 18);   /* Скрытые файлы/папки (приглушённый голубой на тёмно-синем) */
		init_pair(85, 18, 37);   /* Скрытые файлы/папки под курсором */
		static const short ext_fg[32] = {
			196,  46,  51, 201,  88,  22,  23,  53,
			208, 118,  45, 199, 130,  28,  24,  89,
			214,  84,  39, 135,  94,  29, 232,  54,
			202,  48,  75, 205, 124,  34,  30, 126
		};
		static const short neon_rgb[32][3] = {
			{1000,  150,  150}, { 250, 1000,  250}, {   0, 1000, 1000}, {1000,  150, 1000},
			{ 450,    0,    0}, {   0,  380,    0}, {   0,  360,  420}, { 420,    0,  450},
			{1000,  450,    0}, { 600, 1000,  150}, {   0,  750, 1000}, {1000,  200,  750},
			{ 520,  220,    0}, {   0,  550,  180}, {   0,  480,  620}, { 580,    0,  380},
			{1000,  650,  100}, { 200, 1000,  600}, { 250,  600, 1000}, { 750,  300, 1000},
			{ 360,  160,   40}, {   0,  420,  300}, {  40,   40,   40}, { 340,  120,  520},
			{1000,  300,  100}, {   0,  850,  450}, { 450,  750, 1000}, {1000,  400,  850},
			{ 680,   80,   80}, { 180,  720,  180}, {   0,  620,  680}, { 720,  120,  650}
		};
		for (int ci = 0; ci < 32; ci++) {
			short cidx = ext_fg[ci];
			if (can_change_color()) {
				cidx = (short)(100 + ci);
				init_color(cidx, neon_rgb[ci][0], neon_rgb[ci][1], neon_rgb[ci][2]);
			}
			init_pair(20 + ci, cidx, 18);
			init_pair(52 + ci, cidx, 37);
		}
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
		init_pair(18, COLOR_MAGENTA, COLOR_BLUE);
		init_pair(19, COLOR_GREEN, COLOR_BLUE);
		init_pair(84, COLOR_BLUE, COLOR_BLUE);
		init_pair(85, COLOR_BLACK, COLOR_CYAN);
		static const short ext_fg8[16] = {
			COLOR_MAGENTA, COLOR_YELLOW, COLOR_CYAN, COLOR_GREEN,
			COLOR_WHITE, COLOR_CYAN, COLOR_YELLOW, COLOR_RED,
			COLOR_YELLOW, COLOR_GREEN, COLOR_MAGENTA, COLOR_CYAN,
			COLOR_WHITE, COLOR_GREEN, COLOR_RED, COLOR_CYAN
		};
		for (int ci = 0; ci < 32; ci++) { init_pair(20 + ci, ext_fg8[ci % 16], COLOR_BLUE); init_pair(52 + ci, ext_fg8[ci % 16], COLOR_CYAN); }
		init_pair(12, COLOR_YELLOW, COLOR_WHITE);
	}

	ui_theme_init_default();
	ui_load_color_state();
	bkgd(COLOR_PAIR(1));
}

void ui_init(void) { ui_init_ex(0); }
void ui_init_overlay(void) { ui_init_ex(1); }

int ui_apply_bg_color(const char *spec) {
	if (!spec || !spec[0]) return -1;
	use_default_colors();
	int bg = -1;
	if (strcasecmp(spec, "default") == 0 || strcasecmp(spec, "none") == 0) bg = -1;
	else if (strcasecmp(spec, "black") == 0) bg = COLOR_BLACK;
	else if (strcasecmp(spec, "red") == 0) bg = COLOR_RED;
	else if (strcasecmp(spec, "green") == 0) bg = COLOR_GREEN;
	else if (strcasecmp(spec, "yellow") == 0) bg = COLOR_YELLOW;
	else if (strcasecmp(spec, "blue") == 0) bg = COLOR_BLUE;
	else if (strcasecmp(spec, "magenta") == 0) bg = COLOR_MAGENTA;
	else if (strcasecmp(spec, "cyan") == 0) bg = COLOR_CYAN;
	else if (strcasecmp(spec, "white") == 0) bg = COLOR_WHITE;
	else if (strcasecmp(spec, "gray") == 0 || strcasecmp(spec, "grey") == 0) bg = 8;
	else if (strcasecmp(spec, "darkblue") == 0) bg = 17;
	else if (strcasecmp(spec, "darkgreen") == 0) bg = 22;
	else if (strcasecmp(spec, "darkred") == 0) bg = 52;
	else if (strcasecmp(spec, "orange") == 0) bg = 208;
	else if (strcasecmp(spec, "purple") == 0) bg = 93;
	else {
		const char *p = spec;
		if (strncasecmp(p, "color", 5) == 0) p += 5;
		if (*p == '#') p++;
		char *endp = NULL;
		if (strlen(p) == 6 && strspn(p, "0123456789abcdefABCDEF") == 6 && (spec[0] == '#' || strpbrk(p, "abcdefABCDEF") != NULL)) {
			unsigned int rgb = (unsigned int)strtoul(p, NULL, 16);
			int r = (int)((rgb >> 16) & 0xFF), g = (int)((rgb >> 8) & 0xFF), b = (int)(rgb & 0xFF);
			if (can_change_color() && COLORS >= 256) {
				init_color(250, (short)(r * 1000 / 255), (short)(g * 1000 / 255), (short)(b * 1000 / 255));
				bg = 250;
			} else {
				int ri = (r * 5 + 127) / 255, gi = (g * 5 + 127) / 255, bi = (b * 5 + 127) / 255;
				bg = 16 + 36 * ri + 6 * gi + bi;
			}
		} else {
			long idx = strtol(p, &endp, 10);
			if (endp != p && *endp == '\0' && idx >= 0 && idx < COLORS) bg = (int)idx;
			else return -1;
		}
	}
	init_pair(14, COLOR_WHITE, bg);
	bkgd(COLOR_PAIR(14));
	refresh();
	return 0;
}

void draw_fkey_bar(int y, int width) {
	const char *labels[12] = {
		"Help", "UserMn", "View", "Edit", "Copy", "RenMov",
		"MkFold", "Delete", "ConfMn", "Quit", "", ""
	};
	if (g_mod_alt && !g_mod_ctrl && !g_mod_shift) {
		labels[0] = "Left";
		labels[1] = "Right";
		labels[2] = "";
		labels[3] = "";
		labels[4] = "Pack";
		labels[5] = "Link";
		labels[6] = "Find";
		labels[7] = "CmdH";
		labels[8] = "FileH";
		labels[9] = "DirH";
		labels[10] = "ScrH";
		labels[11] = "Color";
	} else if (g_mod_shift && !g_mod_ctrl && !g_mod_alt) {
		labels[0] = "Pack";
		labels[1] = "";
		labels[2] = "";
		labels[3] = "Create";
		labels[4] = "";
		labels[5] = "RenInP";
		labels[6] = "";
		labels[7] = "Erase!";
		labels[8] = "";
		labels[9] = "";
	} else if (g_mod_ctrl && !g_mod_alt && !g_mod_shift) {
		labels[0] = "";
		labels[1] = "";
		labels[2] = "Name";
		labels[3] = "Ext";
		labels[4] = "Time";
		labels[5] = "Size";
		labels[6] = "";
		labels[7] = "";
		labels[8] = "";
		labels[9] = "";
	}

	attrset(COLOR_PAIR(6));
	mvhline(y, 0, ' ', width);

	int col_w = width / 12;
	if (col_w < 3) col_w = 3;

	for (int i = 0; i < 12; i++) {
		int x = i * col_w;
		if (x >= width) break;

		int item_w = (i == 11) ? (width - x) : col_w;

		attrset(COLOR_PAIR(6));
		mvprintw(y, x, "%d", i + 1);

		int num_len = (i >= 9) ? 2 : 1;
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
	char disp_path[MAX_PATH * 2];
	if (p->is_temp) {
		int tcnt = (p->count > 0) ? (p->count - 1) : 0;
		snprintf(disp_path, sizeof(disp_path), "TmpPanel (%d item%s)", tcnt, tcnt == 1 ? "" : "s");
	} else if (p->archive_file[0] != '\0') {
		const char *ab = strrchr(p->archive_file, '/'); ab = ab ? (ab + 1) : p->archive_file;
		snprintf(disp_path, sizeof(disp_path), "%s:/%s", ab, p->archive_dir);
	} else snprintf(disp_path, sizeof(disp_path), "%s", p->current_path);
	int normal_attr = COLOR_PAIR(1);
	int dir_attr = COLOR_PAIR(7);
	int exec_attr = COLOR_PAIR(8);
	int mark_attr = COLOR_PAIR(3);

	int cur_normal_attr = COLOR_PAIR(11);
	int cur_dir_attr = COLOR_PAIR(9);
	int cur_exec_attr = COLOR_PAIR(10);
	int cur_mark_attr = COLOR_PAIR(4);
	int hidden_attr = (COLORS >= 256) ? COLOR_PAIR(84) : (COLOR_PAIR(84) | A_BOLD);
	int cur_hidden_attr = (COLORS >= 256) ? COLOR_PAIR(85) : (COLOR_PAIR(85) | A_BOLD);

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

	/* Заголовок в верхней рамке: тип сортировки максимально налево, путь сразу после него */
	const char *s_name = "Name";
	if (p->sort_mode == SORT_EXT) s_name = "Ext";
	else if (p->sort_mode == SORT_SIZE) s_name = "Size";
	else if (p->sort_mode == SORT_TIME) s_name = "Time";
	else if (p->sort_mode == SORT_NONE) s_name = "None";

	char sort_info[32];
	snprintf(sort_info, sizeof(sort_info), "[%s%s]%s", s_name, p->sort_desc ? "↓" : "↑", g_auto_colorize ? "[Clr]" : "");
	int sort_cols = (int)mbstowcs(NULL, sort_info, 0);
	if (sort_cols < 0) sort_cols = (int)strlen(sort_info);

	attrset((attr_t)normal_attr);
	mvprintw(start_y, start_x + 1, "%s", sort_info);

	int path_x = start_x + 1 + sort_cols;
	int max_path_cols = (start_x + width - 1) - path_x - 2;
	attrset(is_active ? (A_NORMAL | COLOR_PAIR(2)) : (attr_t)normal_attr);
	if (max_path_cols > 0) {
		wchar_t wpath[MAX_PATH * 2] = {0};
		size_t wlen = mbstowcs(wpath, disp_path, MAX_PATH * 2 - 1);
		if (wlen == (size_t)-1) {
			mvprintw(start_y, path_x, " %.*s ", max_path_cols, disp_path);
		} else if ((int)wlen <= max_path_cols) {
			mvprintw(start_y, path_x, " %ls ", wpath);
		} else if (max_path_cols > 3) {
			wchar_t *tail = wpath + ((int)wlen - (max_path_cols - 3));
			mvprintw(start_y, path_x, " ...%ls ", tail);
		} else {
			mvprintw(start_y, path_x, " %.*ls ", max_path_cols, wpath);
		}
	}
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
			int is_hidden = (file_idx < p->count && p->files[file_idx].name[0] == '.' && strcmp(p->files[file_idx].name, "..") != 0);
			int item_attr = normal_attr;

			if (is_cursor) {
				if (is_marked) item_attr = cur_mark_attr;
				else if (is_hidden) item_attr = cur_hidden_attr;
				else if (is_dir) item_attr = cur_dir_attr;
				else if (is_exec) item_attr = cur_exec_attr;
				else {
					int auto_attr = (file_idx < p->count) ? auto_ext_color_attr(p->files[file_idx].name, 1) : 0;
					item_attr = auto_attr ? auto_attr : cur_normal_attr;
				}
			} else {
				if (is_marked) item_attr = mark_attr;
				else if (is_hidden) item_attr = hidden_attr;
				else if (is_dir) item_attr = dir_attr;
				else if (is_exec) item_attr = exec_attr;
				else {
					int auto_attr = (file_idx < p->count) ? auto_ext_color_attr(p->files[file_idx].name, 0) : 0;
					item_attr = auto_attr ? auto_attr : normal_attr;
				}
			}

			attrset(item_attr);
			for (int k = 0; k < col_w; k++) {
				mvaddch(y, col_x + k, ' ');
			}

			if (file_idx < p->count && col_w > 1) {
				const char *disp_name = p->files[file_idx].name;
				char tmp_disp[MAX_PATH];
				if (p->is_temp && p->files[file_idx].full_path[0] != '\0') {
					const char *fp = p->files[file_idx].full_path;
					const char *sl1 = strrchr(fp, '/');
					if (sl1 && sl1 > fp) {
						const char *sl2 = sl1 - 1;
						while (sl2 > fp && *sl2 != '/') sl2--;
						const char *sub = (*sl2 == '/') ? (sl2 + 1) : sl2;
						snprintf(tmp_disp, sizeof(tmp_disp), "%s", sub);
						disp_name = tmp_disp;
					}
				}
				mvwprint_utf8(stdscr, y, col_x + 1, col_w - 1, disp_name);
			}
		}
	}

	int info_y = start_y + height - 2;
	attrset(normal_attr);
	for (int k = 0; k < inner_w; k++) mvaddch(info_y, start_x + 1 + k, ' ');

	int marked_cnt = 0, marked_files = 0, marked_dirs = 0, unknown_dirs = 0;
	off_t marked_bytes = 0;
	for (int i = 0; i < p->count; i++) {
		if (p->files[i].is_selected && strcmp(p->files[i].name, "..") != 0) {
			marked_cnt++;
			if (p->files[i].is_dir) {
				marked_dirs++;
				if (p->files[i].dir_size_known) marked_bytes += p->files[i].size;
				else unknown_dirs++;
			} else {
				marked_files++;
				marked_bytes += p->files[i].size;
			}
		}
	}

	if (is_active && quick_search_active && inner_w > 2) {
		char sline[MAX_FILENAME + 32];
		snprintf(sline, sizeof(sline), "Search: %s_", quick_search);
		attrset(quick_search_nomatch ? (COLOR_PAIR(4) | A_REVERSE) : COLOR_PAIR(2));
		mvwprint_utf8(stdscr, info_y, start_x + 2, inner_w - 2, sline);
	} else if (marked_cnt > 0 && inner_w > 2) {
		char sz[32], summary[160];
		format_size(marked_bytes, sz, sizeof(sz));
		if (unknown_dirs > 0) {
			if (marked_files > 0)
				snprintf(summary, sizeof(summary), "Selected: %d file(s), %d dir(s), %s + dirs", marked_files, marked_dirs, sz);
			else if (marked_bytes > 0)
				snprintf(summary, sizeof(summary), "Selected: %d dir(s), %s + dirs", marked_dirs, sz);
			else
				snprintf(summary, sizeof(summary), "Selected: %d dir(s), + dirs", marked_dirs);
		} else {
			snprintf(summary, sizeof(summary), "Selected: %d item(s), %s", marked_cnt, sz);
		}
		attrset(mark_attr);
		mvwprint_utf8(stdscr, info_y, start_x + 2, inner_w - 2, summary);
	} else if (p->count > 0 && p->selected >= 0 && p->selected < p->count && inner_w > 2) {
		const FileEntry *cur = &p->files[p->selected];
		char sz[32], perms[10], tbuf[32] = "", meta[96];
		if (strcmp(cur->name, "..") == 0 && !cur->dir_size_known) snprintf(sz, sizeof(sz), "<UP>");
		else if (cur->is_dir && !cur->dir_size_known) snprintf(sz, sizeof(sz), "<DIR>");
		else format_size(cur->size, sz, sizeof(sz));
		format_perms(cur->mode, perms);
		struct tm *tm_info = localtime(&cur->mtime);
		if (tm_info) strftime(tbuf, sizeof(tbuf), "%d.%m.%Y %H:%M", tm_info);
		snprintf(meta, sizeof(meta), "%s  %s  %s", sz, perms, tbuf);

		if (p->is_temp && cur->full_path[0] != '\0') {
			mvwprint_utf8(stdscr, info_y, start_x + 2, inner_w - 2, cur->full_path);
		} else {
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
	}

	attrset(A_NORMAL);
}

int cmd_suggest_idx[CMD_SUGGEST_MAX];
int cmd_suggest_count = 0;
int cmd_suggest_sel = -1;
int cmd_suggest_dismissed = 0;
int cmd_suggest_y = 0, cmd_suggest_x = 0, cmd_suggest_w = 0, cmd_suggest_h = 0;

void cmd_suggest_reset(void) {
	cmd_suggest_count = 0;
	cmd_suggest_sel = -1;
	cmd_suggest_dismissed = 0;
	cmd_suggest_y = cmd_suggest_x = cmd_suggest_w = cmd_suggest_h = 0;
}

static void ui_draw_cmd_suggest(int height, int width, int prompt_cols) {
	if (cmd_len <= 0 || cmd_suggest_dismissed || history_count <= 0 || height < 6 || width < 20) {
		cmd_suggest_count = 0;
		cmd_suggest_sel = -1;
		return;
	}
	size_t blen = strlen(cmd_buffer);
	if (blen == 0) { cmd_suggest_count = 0; return; }

	int found[CMD_SUGGEST_MAX];
	int fcnt = 0;
	for (int i = history_count - 1; i >= 0 && fcnt < CMD_SUGGEST_MAX; i--) {
		const char *c = cmd_history[i].cmd;
		if (strncmp(c, cmd_buffer, blen) == 0 && strcmp(c, cmd_buffer) != 0) {
			int dup = 0;
			for (int k = 0; k < fcnt; k++) {
				if (strcmp(cmd_history[found[k]].cmd, c) == 0) { dup = 1; break; }
			}
			if (!dup) found[fcnt++] = i;
		}
	}
	if (fcnt == 0) {
		cmd_suggest_count = 0;
		cmd_suggest_sel = -1;
		return;
	}

	/* Самый свежий вариант — в самом низу (ближе к командной строке) */
	for (int i = 0; i < fcnt; i++) cmd_suggest_idx[i] = found[fcnt - 1 - i];
	cmd_suggest_count = fcnt;
	if (cmd_suggest_sel >= fcnt) cmd_suggest_sel = fcnt - 1;

	int max_cols = 18;
	for (int i = 0; i < fcnt; i++) {
		int cols = (int)mbstowcs(NULL, cmd_history[cmd_suggest_idx[i]].cmd, 0);
		if (cols < 0) cols = (int)strlen(cmd_history[cmd_suggest_idx[i]].cmd);
		if (cols > max_cols) max_cols = cols;
	}

	int bw = max_cols + 4;
	if (bw > width - 2) bw = width - 2;
	if (bw < 16) bw = 16;
	int bh = fcnt + 2;
	int sx = prompt_cols;
	if (sx + bw > width) sx = width - bw;
	if (sx < 0) sx = 0;
	int sy = height - 2 - bh;
	if (sy < 1) sy = 1;

	cmd_suggest_y = sy; cmd_suggest_x = sx; cmd_suggest_w = bw; cmd_suggest_h = bh;

	attrset(g_ui_theme.win_bg);
	mvaddstr(sy, sx, "┌");
	for (int x = 1; x < bw - 1; x++) mvaddstr(sy, sx + x, "─");
	mvaddstr(sy, sx + bw - 1, "┐");
	const char *hdr = " History (Right: fill) ";
	int hlen = (int)strlen(hdr);
	if (bw > hlen + 3) mvprintw(sy, sx + 2, "%s", hdr);

	for (int i = 0; i < fcnt; i++) {
		int row_y = sy + 1 + i;
		attrset(g_ui_theme.win_bg);
		mvaddstr(row_y, sx, "│");
		mvaddstr(row_y, sx + bw - 1, "│");
		int is_hl = (cmd_suggest_sel >= 0) ? (i == cmd_suggest_sel) : (i == fcnt - 1);
		attrset(is_hl ? g_ui_theme.btn_focus : g_ui_theme.win_bg);
		mvhline(row_y, sx + 1, ' ', bw - 2);
		mvwprint_utf8(stdscr, row_y, sx + 2, bw - 4, cmd_history[cmd_suggest_idx[i]].cmd);
	}

	attrset(g_ui_theme.win_bg);
	mvaddstr(sy + bh - 1, sx, "└");
	for (int x = 1; x < bw - 1; x++) mvaddstr(sy + bh - 1, sx + x, "─");
	mvaddstr(sy + bh - 1, sx + bw - 1, "┘");
	attrset(A_NORMAL);
}

void draw_screen(void) {
	bkgd(COLOR_PAIR(1));
	attrset(COLOR_PAIR(1));
	int height, width;
	getmaxyx(stdscr, height, width);

	int panel_width = width / 2;
	int panel_height = height - 2;
	if (panel_height < 3) panel_height = 3;

	if (g_info_panel && active_panel == &right_panel)
		draw_info_panel(&right_panel, 0, 0, panel_width, panel_height);
	else
		draw_panel(&left_panel, 0, 0, panel_width, panel_height, active_panel == &left_panel);
	if (g_info_panel && active_panel == &left_panel)
		draw_info_panel(&left_panel, 0, panel_width, width - panel_width, panel_height);
	else
		draw_panel(&right_panel, 0, panel_width, width - panel_width, panel_height, active_panel == &right_panel);

	char prompt[MAX_PATH + 16];
	int prompt_cols = build_cmd_prompt(prompt, sizeof(prompt), width);
	int avail = width - prompt_cols;
	if (avail < 4) avail = 4;

	if (cmd_pos < cmd_scroll) cmd_scroll = cmd_pos;
	if (cmd_pos >= cmd_scroll + avail) cmd_scroll = cmd_pos - avail + 1;
	if (cmd_scroll < 0) cmd_scroll = 0;

	attrset(COLOR_PAIR(6));
	mvhline(height - 2, 0, ' ', width);
	mvprintw(height - 2, 0, "%s", prompt);
	if (cmd_len > cmd_scroll) {
		mvaddnwstr(height - 2, prompt_cols, cmd_wbuf + cmd_scroll, avail);
	}

	if (cmd_sel_start >= 0 && cmd_sel_end > cmd_sel_start) {
		int s1 = (cmd_sel_start < cmd_scroll) ? cmd_scroll : cmd_sel_start;
		int s2 = (cmd_sel_end > cmd_scroll + avail) ? (cmd_scroll + avail) : cmd_sel_end;
		if (s2 > s1) {
			attron(A_REVERSE);
			mvaddnwstr(height - 2, prompt_cols + (s1 - cmd_scroll), cmd_wbuf + s1, s2 - s1);
			attroff(A_REVERSE);
		}
	}
	attrset(A_NORMAL);

	draw_fkey_bar(height - 1, width);

	ui_draw_cmd_suggest(height, width, prompt_cols);

	int cursor_x = prompt_cols + (cmd_pos - cmd_scroll);
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
	wbkgd(win, g_ui_theme.win_bg);

	const char *blbls[2] = { "[ Yes ]", "[ No ]" };
	int btn_x = (box_w - 16 < 2) ? 2 : (box_w - 16) / 2;
	UiButtonRow btns = { blbls, 2, 3, btn_x, 3, 0 };

	int selected = 1, res = 0;
	while (1) {
		draw_win_box(win);
		mvwprintw(win, 0, 2, " %s ", title);
		int msg_x = (box_w - msg_cols < 4) ? 2 : (box_w - msg_cols) / 2;
		mvwprint_utf8(win, 1, msg_x, box_w - 4, msg);

		btns.focus_idx = selected ? 0 : 1;
		ui_btns_draw(&btns, win);

		printf("\033[?2026h"); fflush(stdout); wnoutrefresh(win); doupdate(); printf("\033[?2026l"); fflush(stdout);
		InputEvent ev = input_read();
		if (ev.action == ACT_MOUSE_LEFT) {
			int wy = ev.mouse_y - start_y, wx = ev.mouse_x - start_x;
			if (wy < 0 || wy >= box_h || wx < 0 || wx >= box_w) { res = 0; break; }
			int hit = ui_btns_hit_test(&btns, wy, wx);
			if (hit == 0) { res = 1; break; }
			if (hit == 1) { res = 0; break; }
			continue;
		}
		if (ev.action == ACT_LEFT || ev.action == ACT_RIGHT || ev.action == ACT_TAB || ev.action == ACT_SHIFT_TAB) selected = 1 - selected;
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
	idlok(win, FALSE); idcok(win, FALSE);
	wbkgd(win, g_ui_theme.win_bg);
	draw_win_box(win);

	mvwprintw(win, 0, 2, " %s ", title);
	int msg_x = (box_w - msg_cols < 4) ? 2 : (box_w - msg_cols) / 2;
	mvwprint_utf8(win, 1, msg_x, box_w - 4, msg);

	const char *blbls[1] = { "[ Enter ]" };
	int btn_x = (box_w - 9 < 2) ? 2 : (box_w - 9) / 2;
	UiButtonRow btns = { blbls, 1, 3, btn_x, 0, 0 };
	ui_btns_draw(&btns, win);
	wrefresh(win);

	while (1) {
		InputEvent ev = input_read();
		if (ev.action == ACT_MOUSE_LEFT || ev.action == ACT_ENTER || ev.action == ACT_ESC || (ev.action == ACT_CHAR && ev.ch == L' ')) break;
	}

	delwin(win);
	touchwin(stdscr);
}

void ui_dialog_box_overlay(const char *title, const char *text) {
	int prev = g_dialog_overlay;
	g_dialog_overlay = 1;
	(void)ui_dialog_show(0, title, text);
	g_dialog_overlay = prev;
}

void ui_dialog_box(const char *title, const char *text) {
	(void)ui_dialog_show(0, title, text);
}

int ui_dialog_show(int is_confirm, const char *title, const char *text) {
	if (!title || !title[0]) title = is_confirm ? "Confirm" : "Message";
	if (!text) text = "";

	int height = 24, width = 80;
	if (g_dialog_overlay) {
		FILE *tty_out = fopen("/dev/tty", "w");
		if (!tty_out) tty_out = stderr;
		struct winsize ws;
		if (ioctl(STDERR_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 && ws.ws_col > 0) { height = ws.ws_row; width = ws.ws_col; }
		if (tty_out != stderr) fclose(tty_out);
	} else {
		getmaxyx(stdscr, height, width);
	}
	int max_iw = (width - 8 < 24) ? 24 : (width - 8);
	if (max_iw > 96) max_iw = 96;

	int cap = 64, lcnt = 0;
	char **lines = malloc((size_t)cap * sizeof(char *));
	if (!lines) return 0;

	int max_cols = 0;
	const char *p = text;
	while (*p || lcnt == 0) {
		const char *nl = strchr(p, '\n');
		size_t raw_len = nl ? (size_t)(nl - p) : strlen(p);
		if (raw_len > 0 && p[raw_len - 1] == '\r') raw_len--;
		size_t off = 0;
		do {
			size_t take = raw_len - off;
			if ((int)take > max_iw * 2) take = (size_t)(max_iw * 2);
			char *seg = malloc(take + 1);
			if (!seg) break;
			memcpy(seg, p + off, take);
			seg[take] = '\0';
			while (take > 0) {
				int cols = (int)mbstowcs(NULL, seg, 0);
				if (cols >= 0 && cols <= max_iw) { if (cols > max_cols) max_cols = cols; break; }
				if (cols < 0 && (int)take <= max_iw) { if ((int)take > max_cols) max_cols = (int)take; break; }
				seg[--take] = '\0';
			}
			if (lcnt >= cap) {
				cap *= 2;
				char **nl_arr = realloc(lines, (size_t)cap * sizeof(char *));
				if (!nl_arr) { free(seg); break; }
				lines = nl_arr;
			}
			lines[lcnt++] = seg;
			off += take;
		} while (off < raw_len);
		if (!nl) break;
		p = nl + 1;
		if (*p == '\0') break;
	}

	int tcols = (int)mbstowcs(NULL, title, 0);
	if (tcols < 0) tcols = (int)strlen(title);
	int box_w = ((max_cols > tcols) ? max_cols : tcols) + 6;
	if (box_w < (is_confirm ? 32 : 30)) box_w = is_confirm ? 32 : 30;
	if (box_w > width - 4) box_w = width - 4;
	int max_vis = (height - 6 < 1) ? 1 : (height - 6);
	int vis_lines = (lcnt < max_vis) ? lcnt : max_vis;
	int box_h = vis_lines + 3;
	int start_y = (height - box_h) / 2, start_x = (width - box_w) / 2;
	int top = 0, sel = 0, res = 0;

	if (g_dialog_overlay) {
		FILE *tty_out = fopen("/dev/tty", "w");
		if (!tty_out) tty_out = stderr;
		fprintf(tty_out, "\033[s\033[?25l\033[?1000h\033[?1006h");
		while (1) {
			fprintf(tty_out, "\033[?2026h\033[30;48;5;252m\033[%d;%dH┌─ %.*s ", start_y + 1, start_x + 1, box_w - 6, title);
			int used = 4 + (tcols > box_w - 6 ? box_w - 6 : tcols);
			for (int c = used; c < box_w - 1; c++) fputs("─", tty_out);
			fputs("┐", tty_out);
			for (int r = 0; r < vis_lines; r++) {
				fprintf(tty_out, "\033[%d;%dH│ ", start_y + 2 + r, start_x + 1);
				int pcols = 0;
				if (top + r < lcnt) {
					const char *ln = lines[top + r];
					fputs(ln, tty_out);
					pcols = (int)mbstowcs(NULL, ln, 0);
					if (pcols < 0) pcols = (int)strlen(ln);
				}
				for (int sp = pcols; sp < box_w - 4; sp++) fputc(' ', tty_out);
				fputs(" │", tty_out);
			}
			fprintf(tty_out, "\033[%d;%dH│", start_y + 2 + vis_lines, start_x + 1);
			int b1_x = is_confirm ? (box_w - 16) / 2 : (box_w - 9) / 2, b2_x = b1_x + 10;
			for (int c = 1; c < b1_x; c++) fputc(' ', tty_out);
			if (!is_confirm) {
				fputs("\033[30;48;5;37m{ \033[1;93mE\033[22;30mnter }\033[30;48;5;252m", tty_out);
				for (int c = b1_x + 9; c < box_w - 1; c++) fputc(' ', tty_out);
				if (tty_out != stderr) fclose(tty_out);
	} else {
				if (sel == 0) fputs("\033[30;48;5;37m[ \033[93mY\033[30mes ]\033[30;48;5;252m", stdout);
				else fputs("[ \033[34mY\033[30mes ]", stdout);
				fputs("   ", stdout);
				if (sel == 1) fputs("\033[30;48;5;37m[ \033[93mN\033[30mo ]\033[30;48;5;252m", stdout);
				else fputs("[ \033[34mN\033[30mo ]", stdout);
				for (int c = b2_x + 6; c < box_w - 1; c++) fputc(' ', stdout);
			}
			fputs("│", stdout);
			printf("\033[%d;%dH└", start_y + box_h, start_x + 1);
			char ind[32] = "";
			if (lcnt > vis_lines) snprintf(ind, sizeof(ind), " %d/%d ", top + vis_lines, lcnt);
			int ilen = (int)strlen(ind);
			for (int c = 1; c < box_w - 1 - ilen; c++) fputs("─", stdout);
			if (ilen > 0) fputs(ind, stdout);
			fputs("┘\033[?2026l", stdout);
			fflush(stdout);
			InputEvent ev = input_read();
			if (ev.action == ACT_UP || ev.action == ACT_MOUSE_WHEEL_UP) { top -= 3; if (top < 0) top = 0; continue; }
			if (ev.action == ACT_DOWN || ev.action == ACT_MOUSE_WHEEL_DOWN) { top += 3; if (top + vis_lines > lcnt) top = (lcnt > vis_lines) ? (lcnt - vis_lines) : 0; continue; }
			if (ev.action == ACT_PGUP) { top -= vis_lines; if (top < 0) top = 0; continue; }
			if (ev.action == ACT_PGDN) { top += vis_lines; if (top + vis_lines > lcnt) top = lcnt - vis_lines; if (top < 0) top = 0; continue; }
			if (is_confirm && (ev.action == ACT_LEFT || ev.action == ACT_RIGHT || ev.action == ACT_TAB)) { sel = 1 - sel; continue; }
			if (!is_confirm) {
				if (ev.action == ACT_ENTER || ev.action == ACT_ESC || (ev.action == ACT_CHAR && (ev.ch == L' ' || ev.ch == L'e' || ev.ch == L'E'))) { res = 1; break; }
				if (ev.action == ACT_MOUSE_LEFT) { int wy = ev.mouse_y - start_y, wx = ev.mouse_x - start_x; if (wy == box_h - 2 && wx >= b1_x && wx < b1_x + 9) { res = 1; break; } }
			} else {
				if (ev.action == ACT_ESC || (ev.action == ACT_CHAR && (ev.ch == L'n' || ev.ch == L'N'))) { res = 0; break; }
				if (ev.action == ACT_CHAR && (ev.ch == L'y' || ev.ch == L'Y')) { res = 1; break; }
				if (ev.action == ACT_ENTER || (ev.action == ACT_CHAR && ev.ch == L' ')) { res = (sel == 0) ? 1 : 0; break; }
				if (ev.action == ACT_MOUSE_LEFT) {
					int wy = ev.mouse_y - start_y, wx = ev.mouse_x - start_x;
					if (wy == box_h - 2 && wx >= b1_x && wx < b1_x + 7) { res = 1; break; }
					if (wy == box_h - 2 && wx >= b2_x && wx < b2_x + 6) { res = 0; break; }
				}
			}
		}
		printf("\033[0m");
		for (int r = 0; r < box_h; r++) printf("\033[%d;%dH%*s", start_y + 1 + r, start_x + 1, box_w, "");
		printf("\033[?1006l\033[?1000l\033[?25h\033[u\033[0m");
		fflush(stdout);
	} else {
		curs_set(0);
		wnoutrefresh(stdscr);
		WINDOW *win = newwin(box_h, box_w, start_y, start_x);
		wbkgd(win, g_ui_theme.win_bg);
		const char *blbls1[1] = { "[ Enter ]" };
		const char *blbls2[2] = { "[ Yes ]", "[ No ]" };
		int btn_x = is_confirm ? ((box_w - 16 < 2) ? 2 : (box_w - 16) / 2) : ((box_w - 9 < 2) ? 2 : (box_w - 9) / 2);
		UiButtonRow btns = { is_confirm ? blbls2 : blbls1, is_confirm ? 2 : 1, box_h - 2, btn_x, 3, 0 };
		while (1) {
			btns.focus_idx = sel;
			draw_win_box(win);
			mvwprintw(win, 0, 2, " %.*s ", box_w - 6, title);
			for (int r = 0; r < vis_lines; r++) {
				mvwhline(win, 1 + r, 2, ' ', box_w - 4);
				if (top + r < lcnt) mvwprint_utf8(win, 1 + r, 2, box_w - 4, lines[top + r]);
			}
			if (lcnt > vis_lines) mvwprintw(win, box_h - 1, box_w - 11, " %d/%d ", top + vis_lines, lcnt);
			ui_btns_draw(&btns, win);
			printf("\033[?2026h"); fflush(stdout); wnoutrefresh(win); doupdate(); printf("\033[?2026l"); fflush(stdout);
			InputEvent ev = input_read();
			if (ev.action == ACT_UP || ev.action == ACT_MOUSE_WHEEL_UP) { top -= 3; if (top < 0) top = 0; continue; }
			if (ev.action == ACT_DOWN || ev.action == ACT_MOUSE_WHEEL_DOWN) { top += 3; if (top + vis_lines > lcnt) top = (lcnt > vis_lines) ? (lcnt - vis_lines) : 0; continue; }
			if (ev.action == ACT_PGUP) { top -= vis_lines; if (top < 0) top = 0; continue; }
			if (ev.action == ACT_PGDN) { top += vis_lines; if (top + vis_lines > lcnt) top = lcnt - vis_lines; if (top < 0) top = 0; continue; }
			if (is_confirm && (ev.action == ACT_LEFT || ev.action == ACT_RIGHT || ev.action == ACT_TAB)) { sel = 1 - sel; continue; }
			if (!is_confirm) {
				if (ev.action == ACT_ENTER || ev.action == ACT_ESC || (ev.action == ACT_CHAR && (ev.ch == L' ' || ev.ch == L'e' || ev.ch == L'E'))) { res = 1; break; }
				if (ev.action == ACT_MOUSE_LEFT) { int wy = ev.mouse_y - start_y, wx = ev.mouse_x - start_x; if (ui_btns_hit_test(&btns, wy, wx) == 0) { res = 1; break; } }
			} else {
				if (ev.action == ACT_ESC || (ev.action == ACT_CHAR && (ev.ch == L'n' || ev.ch == L'N'))) { res = 0; break; }
				if (ev.action == ACT_CHAR && (ev.ch == L'y' || ev.ch == L'Y')) { res = 1; break; }
				if (ev.action == ACT_ENTER || (ev.action == ACT_CHAR && ev.ch == L' ')) { res = (sel == 0) ? 1 : 0; break; }
				if (ev.action == ACT_MOUSE_LEFT) {
					int wy = ev.mouse_y - start_y, wx = ev.mouse_x - start_x;
					int hit = ui_btns_hit_test(&btns, wy, wx);
					if (hit == 0) { res = 1; break; }
					if (hit == 1) { res = 0; break; }
				}
			}
		}
		delwin(win);
		touchwin(stdscr);
	}
	for (int i = 0; i < lcnt; i++) free(lines[i]);
	free(lines);
	return res;
}

int ui_dialog_menu(const char *title, const char **items, int count, char *out_buf, size_t out_sz) {
	if (!items || count <= 0 || !out_buf || out_sz == 0) return 0;
	if (!title || !title[0]) title = "Select";

	int height = 24, width = 80;
	if (g_dialog_overlay) {
		struct winsize ws;
		int tfd = open("/dev/tty", O_RDWR);
		if (ioctl(tfd >= 0 ? tfd : STDERR_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 && ws.ws_col > 0) { height = ws.ws_row; width = ws.ws_col; }
		if (tfd >= 0) close(tfd);
	} else getmaxyx(stdscr, height, width);

	int max_c = (int)mbstowcs(NULL, title, 0);
	if (max_c < 0) max_c = (int)strlen(title);
	for (int i = 0; i < count; i++) {
		int c = (int)mbstowcs(NULL, items[i], 0);
		if (c < 0) c = (int)strlen(items[i]);
		if (c > max_c) max_c = c;
	}
	int box_w = max_c + 8;
	if (box_w < 30) box_w = 30;
	if (box_w > width - 4) box_w = width - 4;
	int iw = box_w - 4;
	int max_vis = (height - 5 < 1) ? 1 : (height - 5);
	int vis = (count < max_vis) ? count : max_vis;
	int box_h = vis + 2;
	int start_y = (height - box_h) / 2, start_x = (width - box_w) / 2;
	int sel = 0, top = 0, res = 0;

	if (g_dialog_overlay) {
		FILE *tty_out = fopen("/dev/tty", "w");
		if (!tty_out) tty_out = stderr;
		fprintf(tty_out, "\033[s\033[?25l\033[?1000h\033[?1006h");
		while (1) {
			if (sel < top) top = sel;
			if (sel >= top + vis) top = sel - vis + 1;
			fprintf(tty_out, "\033[?2026h\033[30;48;5;252m\033[%d;%dH┌─ ", start_y + 1, start_x + 1);
			wchar_t wt[256]; size_t wn = mbstowcs(wt, title, 255);
			int tcols = 0, max_tc = box_w - 6;
			if (wn != (size_t)-1) {
				for (size_t k = 0; k < wn && tcols < max_tc; k++) { char mb[8]; int l = wctomb(mb, wt[k]); if (l > 0) { fwrite(mb, 1, (size_t)l, tty_out); tcols++; } }
			} else { tcols = (int)strlen(title); if (tcols > max_tc) tcols = max_tc; fwrite(title, 1, (size_t)tcols, tty_out); }
			fputc(' ', tty_out);
			for (int c = 4 + tcols; c < box_w - 1; c++) fputs("─", tty_out);
			fputs("┐", tty_out);

			for (int r = 0; r < vis; r++) {
				int idx = top + r;
				fprintf(tty_out, "\033[%d;%dH│ ", start_y + 2 + r, start_x + 1);
				if (idx == sel) fputs("\033[30;48;5;37m", tty_out);
				const char *it = items[idx];
				int ic = (int)mbstowcs(NULL, it, 0); if (ic < 0) ic = (int)strlen(it);
				fprintf(tty_out, " %.*s", iw * 2, it);
				for (int sp = ic + 1; sp < iw; sp++) fputc(' ', tty_out);
				if (idx == sel) fputs("\033[30;48;5;252m", tty_out);
				fputs(" │", tty_out);
			}

			fprintf(tty_out, "\033[%d;%dH└", start_y + box_h, start_x + 1);
			char ind[32] = "";
			if (count > vis) snprintf(ind, sizeof(ind), " %d/%d ", sel + 1, count);
			int ilen = (int)strlen(ind);
			for (int c = 1; c < box_w - 1 - ilen; c++) fputs("─", tty_out);
			if (ilen > 0) fputs(ind, tty_out);
			fputs("┘\033[?2026l", tty_out);
			fflush(tty_out);
			InputEvent ev = input_read();
			if (ev.action == ACT_ESC) { res = 0; break; }
			if (ev.action == ACT_ENTER) { res = 1; break; }
			if (ev.action == ACT_UP || ev.action == ACT_MOUSE_WHEEL_UP) { if (sel > 0) sel--; continue; }
			if (ev.action == ACT_DOWN || ev.action == ACT_MOUSE_WHEEL_DOWN) { if (sel + 1 < count) sel++; continue; }
			if (ev.action == ACT_HOME) { sel = 0; continue; }
			if (ev.action == ACT_END) { sel = count - 1; continue; }
			if (ev.action == ACT_PGUP) { sel -= vis; if (sel < 0) sel = 0; continue; }
			if (ev.action == ACT_PGDN) { sel += vis; if (sel >= count) sel = count - 1; continue; }
			if (ev.action == ACT_MOUSE_LEFT) {
				int wy = ev.mouse_y - start_y, wx = ev.mouse_x - start_x;
				if (wy >= 1 && wy <= vis && wx >= 1 && wx < box_w - 1) {
					int hit = top + (wy - 1);
					if (hit >= 0 && hit < count) { sel = hit; res = 1; break; }
				}
			}
		}
		fprintf(tty_out, "\033[0m");
		for (int r = 0; r < box_h; r++) fprintf(tty_out, "\033[%d;%dH%*s", start_y + 1 + r, start_x + 1, box_w, "");
		fprintf(tty_out, "\033[?1006l\033[?1000l\033[?25h\033[u\033[0m");
		fflush(tty_out);
		if (tty_out != stderr) fclose(tty_out);
	} else {
		curs_set(0);
		wnoutrefresh(stdscr);
		WINDOW *win = newwin(box_h, box_w, start_y, start_x);
		wbkgd(win, g_ui_theme.win_bg);
		while (1) {
			if (sel < top) top = sel;
			if (sel >= top + vis) top = sel - vis + 1;
			draw_win_box(win);
			mvwprintw(win, 0, 2, " %.*s ", box_w - 6, title);
			for (int r = 0; r < vis; r++) {
				int idx = top + r;
				wattrset(win, (idx == sel) ? (chtype)COLOR_PAIR(2) : (chtype)g_ui_theme.win_bg);
				mvwhline(win, 1 + r, 2, ' ', iw);
				mvwprint_utf8(win, 1 + r, 3, iw - 2, items[idx]);
			}
			wattrset(win, g_ui_theme.win_bg);
			if (count > vis) mvwprintw(win, box_h - 1, box_w - 11, " %d/%d ", sel + 1, count);
			printf("\033[?2026h"); fflush(stdout); wnoutrefresh(win); doupdate(); printf("\033[?2026l"); fflush(stdout);
			InputEvent ev = input_read();
			if (ev.action == ACT_ESC) { res = 0; break; }
			if (ev.action == ACT_ENTER) { res = 1; break; }
			if (ev.action == ACT_UP || ev.action == ACT_MOUSE_WHEEL_UP) { if (sel > 0) sel--; continue; }
			if (ev.action == ACT_DOWN || ev.action == ACT_MOUSE_WHEEL_DOWN) { if (sel + 1 < count) sel++; continue; }
			if (ev.action == ACT_HOME) { sel = 0; continue; }
			if (ev.action == ACT_END) { sel = count - 1; continue; }
			if (ev.action == ACT_PGUP) { sel -= vis; if (sel < 0) sel = 0; continue; }
			if (ev.action == ACT_PGDN) { sel += vis; if (sel >= count) sel = count - 1; continue; }
			if (ev.action == ACT_MOUSE_LEFT) {
				int wy = ev.mouse_y - start_y, wx = ev.mouse_x - start_x;
				if (wy >= 1 && wy <= vis && wx >= 1 && wx < box_w - 1) {
					int hit = top + (wy - 1);
					if (hit >= 0 && hit < count) { sel = hit; res = 1; break; }
				}
			}
		}
		delwin(win);
		touchwin(stdscr);
	}
	if (res) snprintf(out_buf, out_sz, "%s", items[sel]);
	return res;
}

static int ui_input_ex(const char *title, const char *prompt, char *buf, int max_len, int max_w);

int ui_dialog_input(const char *title, const char *prompt, char *out_buf, size_t out_sz) {
	if (!g_dialog_overlay) return ui_input_ex(title ? title : "Input", prompt ? prompt : "Enter value:", out_buf, (int)out_sz, 68);
	if (!out_buf || out_sz == 0) return 0;
	if (!title || !title[0]) title = "Input";
	if (!prompt || !prompt[0]) prompt = "Enter value:";

	wchar_t wbuf[1024];
	size_t wlen = mbstowcs(wbuf, out_buf, 1023);
	if (wlen == (size_t)-1) wlen = 0;
	wbuf[wlen] = L'\0';
	int cur = (int)wlen, sel_anc = (wlen > 0) ? 0 : -1, scroll = 0, focus = 0, res = 0; /* focus: 0=input, 1=OK, 2=Cancel */

	int height = 24, width = 80;
	if (g_dialog_overlay) {
		struct winsize ws;
		if (ioctl(STDERR_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 && ws.ws_col > 0) { height = ws.ws_row; width = ws.ws_col; }
	} else getmaxyx(stdscr, height, width);

	int box_w = width - 8;
	if (box_w > 68) box_w = 68;
	if (box_w < 36) box_w = 36;
	if (box_w > width - 2) box_w = width - 2;
	int iw = box_w - 4;
	int box_h = 6;
	int start_y = (height - box_h) / 2, start_x = (width - box_w) / 2;

	if (g_dialog_overlay) {
		FILE *tty_out = fopen("/dev/tty", "w");
		if (!tty_out) tty_out = stderr;
		fprintf(tty_out, "\033[s\033[?25l\033[?1000h\033[?1006h");
		while (1) {
			if (cur < scroll) scroll = cur;
			if (cur > scroll + iw - 1) scroll = cur - iw + 1;
			if (scroll < 0) scroll = 0;
			fprintf(tty_out, "\033[?2026h\033[30;48;5;252m\033[%d;%dH┌─ ", start_y + 1, start_x + 1);
			wchar_t wt[256]; size_t wn = mbstowcs(wt, title, 255);
			int tcols = 0, max_tc = box_w - 6;
			if (wn != (size_t)-1) {
				for (size_t k = 0; k < wn && tcols < max_tc; k++) { char mb[8]; int l = wctomb(mb, wt[k]); if (l > 0) { fwrite(mb, 1, (size_t)l, tty_out); tcols++; } }
			} else { tcols = (int)strlen(title); if (tcols > max_tc) tcols = max_tc; fwrite(title, 1, (size_t)tcols, tty_out); }
			fputc(' ', tty_out);
			for (int c = 4 + tcols; c < box_w - 1; c++) fputs("─", tty_out);
			fputs("┐", tty_out);

			fprintf(tty_out, "\033[%d;%dH│ ", start_y + 2, start_x + 1);
			wchar_t wp[256]; size_t wpn = mbstowcs(wp, prompt, 255);
			int pcols = 0;
			if (wpn != (size_t)-1) {
				for (size_t k = 0; k < wpn && pcols < iw; k++) { char mb[8]; int l = wctomb(mb, wp[k]); if (l > 0) { fwrite(mb, 1, (size_t)l, tty_out); pcols++; } }
			} else { pcols = (int)strlen(prompt); if (pcols > iw) pcols = iw; fwrite(prompt, 1, (size_t)pcols, tty_out); }
			for (int sp = pcols; sp < iw; sp++) fputc(' ', tty_out);
			fputs(" │", tty_out);

			fprintf(tty_out, "\033[%d;%dH│ \033[30;48;5;37m", start_y + 3, start_x + 1);
			int drawn = 0;
			for (int idx = scroll; idx < (int)wlen && drawn < iw; idx++) {
				char mb[8]; int n = wctomb(mb, wbuf[idx]);
				if (n > 0) {
					int s0 = (sel_anc >= 0) ? (sel_anc < cur ? sel_anc : cur) : -1;
					int s1 = (sel_anc >= 0) ? (sel_anc > cur ? sel_anc : cur) : -1;
					int in_sel = (focus == 0 && s0 >= 0 && idx >= s0 && idx < s1);
					if (focus == 0 && idx == cur) fputs("\033[97;48;5;16m", tty_out);
					else if (in_sel) fputs("\033[97;48;5;16m", tty_out);
					fwrite(mb, 1, (size_t)n, tty_out);
					if ((focus == 0 && idx == cur) || in_sel) fputs("\033[30;48;5;37m", tty_out);
					drawn++;
				}
			}
			if (focus == 0 && cur == (int)wlen && drawn < iw) { fputs("\033[97;48;5;16m \033[30;48;5;37m", tty_out); drawn++; }
			for (; drawn < iw; drawn++) fputc(' ', tty_out);
			fputs("\033[30;48;5;252m │", tty_out);

			fprintf(tty_out, "\033[%d;%dH│", start_y + 4, start_x + 1);
			int b1_x = (box_w - 19) / 2, b2_x = b1_x + 9;
			for (int c = 1; c < b1_x; c++) fputc(' ', tty_out);
			if (focus == 1) fputs("\033[30;48;5;37m{ \033[1;93mO\033[22;30mK }\033[30;48;5;252m", tty_out);
			else fputs("[ \033[1;93mO\033[22;30mK ]", tty_out);
			fputs("   ", tty_out);
			if (focus == 2) fputs("\033[30;48;5;37m{ \033[1;93mC\033[22;30mancel }\033[30;48;5;252m", tty_out);
			else fputs("[ \033[1;93mC\033[22;30mancel ]", tty_out);
			for (int c = b2_x + 10; c < box_w - 1; c++) fputc(' ', tty_out);
			fputs("│", tty_out);

			fprintf(tty_out, "\033[%d;%dH└", start_y + 5, start_x + 1);
			for (int c = 1; c < box_w - 1; c++) fputs("─", tty_out);
			fputs("┘\033[?2026l", tty_out);
			fflush(tty_out);
			InputEvent ev = input_read();
			if (ev.action == ACT_ESC) { res = 0; break; }
			if (ev.action == ACT_TAB) { focus = (focus + 1) % 3; continue; }
			if (ev.action == ACT_UP) { focus = 0; continue; }
			if (ev.action == ACT_DOWN) { if (focus == 0) focus = 1; continue; }
			if (ev.action == ACT_ENTER) { res = (focus == 2) ? 0 : 1; break; }
			if (ev.action == ACT_MOUSE_LEFT) {
				int wy = ev.mouse_y - start_y, wx = ev.mouse_x - start_x;
				if (wy == 2 && wx >= 2 && wx < box_w - 2) { focus = 0; continue; }
				if (wy == 3 && wx >= b1_x && wx < b1_x + 6) { res = 1; break; }
				if (wy == 3 && wx >= b2_x && wx < b2_x + 10) { res = 0; break; }
			}
			if (focus != 0) {
				if (ev.action == ACT_LEFT || ev.action == ACT_RIGHT) { focus = (focus == 1) ? 2 : 1; continue; }
				if (ev.action == ACT_CHAR && (ev.ch == L'o' || ev.ch == L'O')) { res = 1; break; }
				if (ev.action == ACT_CHAR && (ev.ch == L'c' || ev.ch == L'C')) { res = 0; break; }
				continue;
			}
			if (ev.action == ACT_CTRL_A || (ev.action == ACT_CHAR && ev.ch == 1)) { sel_anc = 0; cur = (int)wlen; continue; }
			if ((ev.action == ACT_CTRL_C || ev.action == ACT_CTRL_X) && sel_anc >= 0 && sel_anc != cur) {
				int s0 = sel_anc < cur ? sel_anc : cur, s1 = sel_anc > cur ? sel_anc : cur;
				wchar_t tmpw[1024]; int tl = s1 - s0; if (tl > 1023) tl = 1023;
				wmemcpy(tmpw, wbuf + s0, (size_t)tl); tmpw[tl] = L'\0';
				char cb[4096]; size_t cn = wcstombs(cb, tmpw, sizeof(cb) - 1);
				if (cn != (size_t)-1) { cb[cn] = '\0'; FILE *pf = popen("xclip -selection clipboard 2>/dev/null || xsel -ib 2>/dev/null || wl-copy 2>/dev/null", "w"); if (pf) { fputs(cb, pf); pclose(pf); } }
				if (ev.action == ACT_CTRL_X) { wmemmove(wbuf + s0, wbuf + s1, wlen - (size_t)s1 + 1); wlen -= (size_t)(s1 - s0); cur = s0; sel_anc = -1; }
				continue;
			}
			if (ev.action == ACT_CTRL_V) {
				FILE *pf = popen("xclip -o -selection clipboard 2>/dev/null || xsel -ob 2>/dev/null || wl-paste -n 2>/dev/null", "r");
				if (pf) {
					char pb[2048] = ""; size_t pr = fread(pb, 1, sizeof(pb) - 1, pf); pb[pr] = '\0'; pclose(pf);
					pb[strcspn(pb, "\r\n")] = '\0';
					wchar_t pw[1024]; size_t pwl = mbstowcs(pw, pb, 1023);
					if (pwl != (size_t)-1 && pwl > 0) {
						if (sel_anc >= 0 && sel_anc != cur) { int s0 = sel_anc < cur ? sel_anc : cur, s1 = sel_anc > cur ? sel_anc : cur; wmemmove(wbuf + s0, wbuf + s1, wlen - (size_t)s1 + 1); wlen -= (size_t)(s1 - s0); cur = s0; sel_anc = -1; }
						if (wlen + pwl < 1020) { wmemmove(wbuf + cur + pwl, wbuf + cur, wlen - (size_t)cur + 1); wmemcpy(wbuf + cur, pw, pwl); wlen += pwl; cur += (int)pwl; }
					}
				}
				continue;
			}
			if (ev.action == ACT_SHIFT_LEFT) { if (sel_anc < 0) sel_anc = cur; if (cur > 0) cur--; continue; }
			if (ev.action == ACT_SHIFT_RIGHT) { if (sel_anc < 0) sel_anc = cur; if (cur < (int)wlen) cur++; continue; }
			if (ev.action == ACT_SHIFT_HOME) { if (sel_anc < 0) sel_anc = cur; cur = 0; continue; }
			if (ev.action == ACT_SHIFT_END) { if (sel_anc < 0) sel_anc = cur; cur = (int)wlen; continue; }
			if (ev.action == ACT_CTRL_LEFT) { sel_anc = -1; while (cur > 0 && wbuf[cur - 1] == L' ') cur--; while (cur > 0 && wbuf[cur - 1] != L' ') cur--; continue; }
			if (ev.action == ACT_CTRL_RIGHT) { sel_anc = -1; while (cur < (int)wlen && wbuf[cur] != L' ') cur++; while (cur < (int)wlen && wbuf[cur] == L' ') cur++; continue; }
			if (ev.action == ACT_LEFT) { sel_anc = -1; if (cur > 0) cur--; continue; }
			if (ev.action == ACT_RIGHT) { sel_anc = -1; if (cur < (int)wlen) cur++; continue; }
			if (ev.action == ACT_HOME) { sel_anc = -1; cur = 0; continue; }
			if (ev.action == ACT_END) { sel_anc = -1; cur = (int)wlen; continue; }
			if (ev.action == ACT_BACKSPACE) {
				if (sel_anc >= 0 && sel_anc != cur) { int s0 = sel_anc < cur ? sel_anc : cur, s1 = sel_anc > cur ? sel_anc : cur; wmemmove(wbuf + s0, wbuf + s1, wlen - (size_t)s1 + 1); wlen -= (size_t)(s1 - s0); cur = s0; sel_anc = -1; }
				else if (cur > 0) { memmove(&wbuf[cur - 1], &wbuf[cur], (wlen - (size_t)cur + 1) * sizeof(wchar_t)); cur--; wlen--; }
				continue;
			}
			if (ev.action == ACT_DELETE) {
				if (cur < (int)wlen) { memmove(&wbuf[cur], &wbuf[cur + 1], (wlen - (size_t)cur) * sizeof(wchar_t)); wlen--; }
				continue;
			}
			if (ev.action == ACT_CHAR && ev.ch >= 32 && wlen < 1020) {
				if (sel_anc >= 0 && sel_anc != cur) { int s0 = sel_anc < cur ? sel_anc : cur, s1 = sel_anc > cur ? sel_anc : cur; wmemmove(wbuf + s0, wbuf + s1, wlen - (size_t)s1 + 1); wlen -= (size_t)(s1 - s0); cur = s0; sel_anc = -1; }
				memmove(&wbuf[cur + 1], &wbuf[cur], (wlen - (size_t)cur + 1) * sizeof(wchar_t));
				wbuf[cur++] = ev.ch; wlen++;
			}
		}
		fprintf(tty_out, "\033[0m");
		for (int r = 0; r < box_h; r++) fprintf(tty_out, "\033[%d;%dH%*s", start_y + 1 + r, start_x + 1, box_w, "");
		fprintf(tty_out, "\033[?1006l\033[?1000l\033[?25h\033[u\033[0m");
		fflush(tty_out);
		if (tty_out != stderr) fclose(tty_out);
	} else {
		curs_set(0);
		wnoutrefresh(stdscr);
		WINDOW *win = newwin(box_h, box_w, start_y, start_x);
		wbkgd(win, g_ui_theme.win_bg);
		const char *blbls[2] = { "[ OK ]", "[ Cancel ]" };
		int b1_x = (box_w - 19) / 2;
		UiButtonRow btns = { blbls, 2, 3, b1_x, 3, -1 };
		while (1) {
			if (cur < scroll) scroll = cur;
			if (cur > scroll + iw - 1) scroll = cur - iw + 1;
			if (scroll < 0) scroll = 0;
			btns.focus_idx = (focus > 0) ? (focus - 1) : -1;
			draw_win_box(win);
			mvwprintw(win, 0, 2, " %.*s ", box_w - 6, title);
			mvwhline(win, 1, 2, ' ', iw);
			mvwprint_utf8(win, 1, 2, iw, prompt);
			wattron(win, g_ui_theme.input_attr);
			mvwhline(win, 2, 2, ' ', iw);
			int drawn = 0;
			for (int idx = scroll; idx < (int)wlen && drawn < iw; idx++) {
				char mb[8]; int n = wctomb(mb, wbuf[idx]);
				if (n > 0) {
					mb[n] = '\0';
					if (focus == 0 && idx == cur) wattron(win, A_REVERSE);
					mvwaddstr(win, 2, 2 + drawn, mb);
					if (focus == 0 && idx == cur) wattroff(win, A_REVERSE);
					drawn++;
				}
			}
			if (focus == 0 && cur == (int)wlen && drawn < iw) { wattron(win, A_REVERSE); mvwaddch(win, 2, 2 + drawn, ' '); wattroff(win, A_REVERSE); }
			wattroff(win, g_ui_theme.input_attr);
			ui_btns_draw(&btns, win);
			printf("\033[?2026h"); fflush(stdout); wnoutrefresh(win); doupdate(); printf("\033[?2026l"); fflush(stdout);
			InputEvent ev = input_read();
			if (ev.action == ACT_ESC) { res = 0; break; }
			if (ev.action == ACT_TAB) { focus = (focus + 1) % 3; continue; }
			if (ev.action == ACT_UP) { focus = 0; continue; }
			if (ev.action == ACT_DOWN) { if (focus == 0) focus = 1; continue; }
			if (ev.action == ACT_ENTER) { res = (focus == 2) ? 0 : 1; break; }
			if (ev.action == ACT_MOUSE_LEFT) {
				int wy = ev.mouse_y - start_y, wx = ev.mouse_x - start_x;
				if (wy == 2 && wx >= 2 && wx < box_w - 2) { focus = 0; continue; }
				int hit = ui_btns_hit_test(&btns, wy, wx);
				if (hit == 0) { res = 1; break; }
				if (hit == 1) { res = 0; break; }
			}
			if (focus != 0) {
				if (ev.action == ACT_LEFT || ev.action == ACT_RIGHT) { focus = (focus == 1) ? 2 : 1; continue; }
				if (ev.action == ACT_CHAR && (ev.ch == L'o' || ev.ch == L'O')) { res = 1; break; }
				if (ev.action == ACT_CHAR && (ev.ch == L'c' || ev.ch == L'C')) { res = 0; break; }
				continue;
			}
			if (ev.action == ACT_LEFT) { if (cur > 0) cur--; continue; }
			if (ev.action == ACT_RIGHT) { if (cur < (int)wlen) cur++; continue; }
			if (ev.action == ACT_HOME) { cur = 0; continue; }
			if (ev.action == ACT_END) { cur = (int)wlen; continue; }
			if (ev.action == ACT_BACKSPACE) { if (cur > 0) { memmove(&wbuf[cur - 1], &wbuf[cur], (wlen - (size_t)cur + 1) * sizeof(wchar_t)); cur--; wlen--; } continue; }
			if (ev.action == ACT_DELETE) { if (cur < (int)wlen) { memmove(&wbuf[cur], &wbuf[cur + 1], (wlen - (size_t)cur) * sizeof(wchar_t)); wlen--; } continue; }
			if (ev.action == ACT_CHAR && ev.ch >= 32 && wlen < 1020) { memmove(&wbuf[cur + 1], &wbuf[cur], (wlen - (size_t)cur + 1) * sizeof(wchar_t)); wbuf[cur++] = ev.ch; wlen++; }
		}
		delwin(win);
		touchwin(stdscr);
	}
	if (res) {
		wbuf[wlen] = L'\0';
		size_t n = wcstombs(out_buf, wbuf, out_sz - 1);
		if (n == (size_t)-1) out_buf[0] = '\0';
		else out_buf[n] = '\0';
	}
	return res;
}

int ui_dialog_run_form(const char *spec_text, int allow_bg) {
	int mode = 0; /* 0=msg, 1=confirm, 2=input, 3=menu */
	char title[256] = "", bg[64] = "", text[8192] = "", def_val[2048] = "", items_raw[4096] = "";
	size_t tlen = 0;
	const char *p = spec_text ? spec_text : "";
	while (*p) {
		const char *nl = strchr(p, '\n');
		size_t len = nl ? (size_t)(nl - p) : strlen(p);
		while (len > 0 && (p[len - 1] == '\r' || p[len - 1] == ' ' || p[len - 1] == '\t')) len--;
		size_t s = 0;
		while (s < len && (p[s] == ' ' || p[s] == '\t')) s++;
		if (s < len && p[s] != '#' && p[s] != ';') {
			const char *eq = memchr(p + s, '=', len - s);
			if (eq) {
				size_t klen = (size_t)(eq - (p + s));
				while (klen > 0 && (p[s + klen - 1] == ' ' || p[s + klen - 1] == '\t')) klen--;
				const char *v = eq + 1;
				size_t vlen = (size_t)((p + len) - v);
				while (vlen > 0 && (*v == ' ' || *v == '\t')) { v++; vlen--; }
				if (vlen >= 2 && ((*v == '"' && v[vlen - 1] == '"') || (*v == '\'' && v[vlen - 1] == '\''))) { v++; vlen -= 2; }
				if (klen == 4 && strncasecmp(p + s, "type", 4) == 0) {
					if (vlen == 7 && strncasecmp(v, "confirm", 7) == 0) mode = 1;
					else if (vlen == 5 && strncasecmp(v, "input", 5) == 0) mode = 2;
					else if (vlen == 4 && strncasecmp(v, "menu", 4) == 0) mode = 3;
				} else if (klen == 7 && strncasecmp(p + s, "default", 7) == 0) { size_t cp = (vlen < sizeof(def_val) - 1) ? vlen : sizeof(def_val) - 1; memcpy(def_val, v, cp); def_val[cp] = '\0'; }
				else if (klen == 5 && strncasecmp(p + s, "items", 5) == 0) { size_t cp = (vlen < sizeof(items_raw) - 1) ? vlen : sizeof(items_raw) - 1; memcpy(items_raw, v, cp); items_raw[cp] = '\0'; }
				else if (klen == 5 && strncasecmp(p + s, "title", 5) == 0) { size_t cp = (vlen < sizeof(title) - 1) ? vlen : sizeof(title) - 1; memcpy(title, v, cp); title[cp] = '\0'; }
				else if (klen == 2 && strncasecmp(p + s, "bg", 2) == 0) { size_t cp = (vlen < sizeof(bg) - 1) ? vlen : sizeof(bg) - 1; memcpy(bg, v, cp); bg[cp] = '\0'; }
				else if (klen == 4 && strncasecmp(p + s, "text", 4) == 0) {
					for (size_t k = 0; k < vlen && tlen + 2 < sizeof(text); k++) {
						if (v[k] == '\\' && k + 1 < vlen && v[k + 1] == 'n') { text[tlen++] = '\n'; k++; }
						else text[tlen++] = v[k];
					}
					text[tlen] = '\0';
				}
			}
		}
		if (!nl) break;
		p = nl + 1;
	}
	int has_bg = (allow_bg && bg[0] != '\0');
	if (has_bg) {
		ui_init(); nonl(); idlok(stdscr, FALSE); idcok(stdscr, FALSE); scrollok(stdscr, FALSE); input_enable_raw();
		if (ui_apply_bg_color(bg) != 0) { bkgd(COLOR_PAIR(1)); refresh(); }
	}
	int r = 0;
	if (mode == 2) {
		r = ui_dialog_input(title[0] ? title : NULL, text, def_val, sizeof(def_val));
		if (r && allow_bg) { printf("%s\n", def_val); fflush(stdout); }
	} else if (mode == 3) {
		const char *arr[128]; int icnt = 0; char *tok = strtok(items_raw, "|");
		while (tok && icnt < 128) { arr[icnt++] = tok; tok = strtok(NULL, "|"); }
		char out_m[2048] = "";
		r = ui_dialog_menu(title[0] ? title : NULL, arr, icnt, out_m, sizeof(out_m));
		if (r && allow_bg) { printf("%s\n", out_m); fflush(stdout); }
	} else {
		r = ui_dialog_show(mode == 1, title[0] ? title : NULL, text);
	}
	if (has_bg) { input_restore_sane(); endwin(); input_restore_sane(); }
	return r;
}

static int ui_exec_single_dialog(const char *cmd_part, char *out_val, size_t out_sz) {
	if (out_val && out_sz > 0) out_val[0] = '\0';
	const char *p = cmd_part;
	while (*p == ' ' || *p == '\t') p++;
	if (strncmp(p, ":dialog", 7) == 0 && (p[7] == '\0' || p[7] == ' ' || p[7] == '\t')) p += 7;
	else if (strncmp(p, ":D", 2) == 0 && (p[2] == '\0' || p[2] == ' ' || p[2] == '\t')) p += 2;
	else return -1;

	char args[64][1024];
	int argc_d = 0;
	while (*p && argc_d < 64) {
		while (*p == ' ' || *p == '\t') p++;
		if (!*p) break;
		char q = 0;
		if (*p == '"' || *p == '\'') q = *p++;
		size_t w = 0;
		while (*p) {
			if (q) { if (*p == q) { p++; break; } }
			else if (*p == ' ' || *p == '\t') break;
			if (*p == '\\' && p[1] == 'n') { if (w + 1 < sizeof(args[0])) args[argc_d][w++] = '\n'; p += 2; continue; }
			if (*p == '\\' && (p[1] == '"' || p[1] == '\'' || p[1] == '\\')) p++;
			if (w + 1 < sizeof(args[0])) args[argc_d][w++] = *p;
			p++;
		}
		args[argc_d][w] = '\0';
		argc_d++;
	}

	int mode = 0, off_a = 0;
	if (argc_d > 0) {
		if (strcmp(args[0], "msg") == 0) { mode = 0; off_a = 1; }
		else if (strcmp(args[0], "confirm") == 0) { mode = 1; off_a = 1; }
		else if (strcmp(args[0], "input") == 0) { mode = 2; off_a = 1; }
		else if (strcmp(args[0], "menu") == 0) { mode = 3; off_a = 1; }
		else if (strcmp(args[0], "form") == 0) { mode = 4; off_a = 1; }
	}
	int rem = argc_d - off_a;
	int prev_ov = g_dialog_overlay;
	if (terminal_mode) g_dialog_overlay = 1;
	int ok = 1;

	if (mode == 2) {
		const char *ititle = (rem >= 2) ? args[off_a] : "Input";
		const char *iprompt = (rem >= 2) ? args[off_a + 1] : (rem == 1 ? args[off_a] : "Enter value:");
		char buf[2048] = "";
		if (rem >= 3) snprintf(buf, sizeof(buf), "%s", args[off_a + 2]);
		ok = ui_dialog_input(ititle, iprompt, buf, sizeof(buf));
		if (ok) { setenv("XC_DIALOG_OUT", buf, 1); if (out_val && out_sz > 0) snprintf(out_val, out_sz, "%s", buf); }
	} else if (mode == 3) {
		const char *mtitle = (rem >= 1) ? args[off_a] : "Select";
		const char *mitems[64]; int mcnt = 0;
		for (int k = 1; k < rem && mcnt < 64; k++) mitems[mcnt++] = args[off_a + k];
		char out_m[1024] = "";
		ok = (mcnt > 0) ? ui_dialog_menu(mtitle, mitems, mcnt, out_m, sizeof(out_m)) : 0;
		if (ok) { setenv("XC_DIALOG_OUT", out_m, 1); if (out_val && out_sz > 0) snprintf(out_val, out_sz, "%s", out_m); }
	} else {
		const char *title = (rem >= 2) ? args[off_a] : (mode == 1 ? "Confirm" : "Message");
		const char *src = (rem >= 2) ? args[off_a + 1] : (rem == 1 ? args[off_a] : "");
		char *dyn_txt = NULL;
		if (src[0] != '\0') {
			char fpath[MAX_PATH * 2];
			if (src[0] != '/' && active_panel && active_panel->current_path[0] != '\0')
				snprintf(fpath, sizeof(fpath), "%s/%s", active_panel->current_path, src);
			else snprintf(fpath, sizeof(fpath), "%s", src);
			struct stat st;
			if (stat(fpath, &st) == 0 && S_ISREG(st.st_mode)) {
				if (rem == 1) { const char *bn = strrchr(fpath, '/'); title = (bn && bn[1]) ? (bn + 1) : fpath; }
				dyn_txt = calloc(1, 1);
				FILE *fp = fopen(fpath, "rb");
				if (fp && st.st_size > 0 && st.st_size < 1024 * 1024) {
					char *nb = realloc(dyn_txt, (size_t)st.st_size + 1);
					if (nb) { dyn_txt = nb; size_t rd = fread(dyn_txt, 1, (size_t)st.st_size, fp); dyn_txt[rd] = '\0'; }
				}
				if (fp) fclose(fp);
			}
		}
		if (mode == 4) ok = ui_dialog_run_form(dyn_txt ? dyn_txt : src, 0);
		else ok = ui_dialog_show(mode == 1, title, dyn_txt ? dyn_txt : src);
		free(dyn_txt);
	}
	g_dialog_overlay = prev_ov;
	return ok ? 1 : 0;
}

int ui_try_builtin_dialog_cmd(const char *raw_cmd) {
	if (!raw_cmd) return 0;
	/* Шаг A: развернуть все подстановки $(:dialog ...) и $(:D ...) в строке */
	char buf[MAX_CMD * 2];
	snprintf(buf, sizeof(buf), "%s", raw_cmd);
	while (1) {
		char *sub = strstr(buf, "$(:dialog");
		if (!sub) sub = strstr(buf, "$(:D");
		if (!sub) break;
		const char *after = sub + (strncmp(sub, "$(:dialog", 9) == 0 ? 9 : 4);
		if (*after != ' ' && *after != '\t' && *after != ')') break;
		char q = 0; char *endp = NULL;
		for (char *c = sub + 2; *c; c++) {
			if (q) { if (*c == '\\' && c[1]) { c++; continue; } if (*c == q) q = 0; }
			else { if (*c == '"' || *c == '\'') q = *c; else if (*c == ')') { endp = c; break; } }
		}
		if (!endp) break;
		*endp = '\0';
		char val[2048] = "";
		int ok = ui_exec_single_dialog(sub + 2, val, sizeof(val));
		if (!ok) return 1; /* Пользователь нажал Esc/Cancel — прерываем команду */
		char tmp[MAX_CMD * 2];
		*sub = '\0';
		snprintf(tmp, sizeof(tmp), "%s%s%s", buf, val, endp + 1);
		snprintf(buf, sizeof(buf), "%s", tmp);
	}

	/* Шаг B: проверить наличие :dialog или :D вне кавычек в строке */
	int has_dlg = 0; char q0 = 0;
	for (const char *c = buf; *c; c++) {
		if (q0) { if (*c == '\\' && c[1]) { c++; continue; } if (*c == q0) q0 = 0; continue; }
		if (*c == '"' || *c == '\'') { q0 = *c; continue; }
		if ((c == buf || c[-1] == ' ' || c[-1] == '\t' || c[-1] == '&' || c[-1] == ';') &&
		    ((strncmp(c, ":dialog", 7) == 0 && (c[7] == '\0' || c[7] == ' ' || c[7] == '\t')) ||
		     (strncmp(c, ":D", 2) == 0 && (c[2] == '\0' || c[2] == ' ' || c[2] == '\t')))) { has_dlg = 1; break; }
	}
	if (!has_dlg) {
		if (strcmp(buf, raw_cmd) != 0) { execute_system_command(buf); return 1; }
		return 0;
	}

	/* Выполняем цепочку команд через &&, сохраняя присваивания VAR=val для последующих :dialog */
	char chain[MAX_CMD * 2];
	snprintf(chain, sizeof(chain), "%s", buf);
	char *cur = chain;
	while (cur && *cur) {
		char q = 0; char *amp = NULL;
		for (char *c = cur; *c; c++) {
			if (q) { if (*c == '\\' && c[1]) { c++; continue; } if (*c == q) q = 0; }
			else { if (*c == '"' || *c == '\'') q = *c; else if (*c == '&' && c[1] == '&') { amp = c; break; } }
		}
		if (amp) *amp = '\0';
		while (*cur == ' ' || *cur == '\t') cur++;
		char *end_c = cur + strlen(cur);
		while (end_c > cur && (end_c[-1] == ' ' || end_c[-1] == '\t')) *(--end_c) = '\0';

		/* Подставляем $VAR и ${VAR} из переменных окружения (включая присвоенные в этой же цепочке) */
		char exp_seg[MAX_CMD * 2];
		size_t ew = 0;
		for (const char *s1 = cur; *s1 && ew + 2 < sizeof(exp_seg); ) {
			if (*s1 == '$') {
				const char *vn = s1 + 1; int br = 0;
				if (*vn == '{') { br = 1; vn++; }
				char vname[128]; size_t vl = 0;
				while ((isalnum((unsigned char)*vn) || *vn == '_') && vl + 1 < sizeof(vname)) vname[vl++] = *vn++;
				vname[vl] = '\0';
				if (vl > 0 && (!br || *vn == '}')) {
					if (br) vn++;
					const char *vv = getenv(vname);
					if (vv) { size_t vvl = strlen(vv); if (ew + vvl < sizeof(exp_seg)) { memcpy(exp_seg + ew, vv, vvl); ew += vvl; } }
					s1 = vn; continue;
				}
			}
			exp_seg[ew++] = *s1++;
		}
		exp_seg[ew] = '\0';

		/* Если сегмент вида VAR=VALUE без пробелов до '=' — сохраняем в окружение */
		char *eq = strchr(exp_seg, '=');
		int is_assign = 0;
		if (eq && eq > exp_seg) {
			is_assign = 1;
			for (char *k = exp_seg; k < eq; k++) if (!isalnum((unsigned char)*k) && *k != '_') { is_assign = 0; break; }
			if (is_assign) {
				*eq = '\0';
				char *valp = eq + 1;
				size_t vlen = strlen(valp);
				if (vlen >= 2 && ((valp[0] == '"' && valp[vlen - 1] == '"') || (valp[0] == '\'' && valp[vlen - 1] == '\''))) { valp[vlen - 1] = '\0'; valp++; }
				setenv(exp_seg, valp, 1);
			}
		}
		if (!is_assign) {
			int r = ui_exec_single_dialog(exp_seg, NULL, 0);
			if (r == -1) {
				if (amp) { *amp = '&'; execute_system_command(cur); break; }
				execute_system_command(exp_seg);
			} else if (r == 0) break;
		}
		cur = amp ? (amp + 2) : NULL;
	}
	return 1;
}

static int ui_input_ex(const char *title, const char *prompt, char *buf, int max_len, int max_w) {
	int height, width;
	getmaxyx(stdscr, height, width);

	int box_w = width - 4;
	if (box_w > max_w) box_w = max_w;
	if (box_w < 32) box_w = 32;
	int box_h = 6;

	int start_y = (height - box_h) / 2;
	int start_x = (width - box_w) / 2;

	WINDOW *win = newwin(box_h, box_w, start_y, start_x);
	keypad(win, TRUE);
	wbkgd(win, g_ui_theme.win_bg);

	wchar_t wbuf[MAX_PATH];
	size_t conv = mbstowcs(wbuf, buf, MAX_PATH - 1);
	int len = (conv == (size_t)-1) ? 0 : (int)conv;
	wbuf[len] = L'\0';

	int field_w = box_w - 6;
	UiTextField tf;
	ui_tf_init(&tf, wbuf, MAX_PATH - 2, 1);
	ui_tf_set_bounds(&tf, 2, 3, field_w);

	const char *btn_labels[2] = { "[ OK ]", "[ Cancel ]" };
	int total_btn_w = 6 + 4 + 10;
	int btn_x = (box_w - total_btn_w) / 2;
	if (btn_x < 2) btn_x = 2;

	UiButtonRow btns = { btn_labels, 2, 4, btn_x, 4, -1 };

	int res = 0;
	int focus = 0; /* 0 = input, 1 = OK, 2 = Cancel */

	while (1) {
		werase(win);
		draw_win_box(win);
		mvwprintw(win, 0, 2, " %s ", title);
		mvwprint_utf8(win, 1, 3, field_w, prompt);

		ui_tf_draw(&tf, win, focus == 0);
		btns.focus_idx = (focus == 1) ? 0 : ((focus == 2) ? 1 : -1);
		ui_btns_draw(&btns, win);

		if (focus == 0) ui_tf_place_cursor(&tf, win);
		else curs_set(0);
		wrefresh(win);

		InputEvent ev = input_read();
		if (ev.action == ACT_NONE) continue;
		if (ev.action == ACT_MOUSE_LEFT) {
			int wy = ev.mouse_y - start_y;
			int wx = ev.mouse_x - start_x;
			if (wy < 0 || wy >= box_h || wx < 0 || wx >= box_w) { res = 0; break; }
			if (ui_tf_handle_mouse(&tf, wy, wx)) { focus = 0; continue; }
			int hit = ui_btns_hit_test(&btns, wy, wx);
			if (hit == 0) { wcstombs(buf, wbuf, (size_t)max_len); buf[max_len - 1] = '\0'; res = 1; break; }
			if (hit == 1) { res = 0; break; }
			continue;
		}
		if (ev.action == ACT_ESC) { res = 0; break; }
		if (ev.action == ACT_TAB) { focus = (focus + 1) % 3; continue; }
		if (ev.action == ACT_SHIFT_TAB) { focus = (focus + 2) % 3; continue; }
		if (ev.action == ACT_DOWN) { focus = (focus == 0) ? 1 : 0; continue; }
		if (ev.action == ACT_UP) { focus = 0; continue; }
		if (ev.action == ACT_ENTER || ((focus == 1 || focus == 2) && ev.action == ACT_CHAR && ev.ch == L' ')) {
			if (focus == 2) res = 0;
			else { wcstombs(buf, wbuf, (size_t)max_len); buf[max_len - 1] = '\0'; res = 1; }
			break;
		}
		if (focus > 0 && (ev.action == ACT_LEFT || ev.action == ACT_RIGHT)) {
			focus = (focus == 1) ? 2 : 1;
			continue;
		}
		if (ui_tf_handle_event(&tf, &ev)) {
			focus = 0;
			continue;
		}
	}

	curs_set(0);
	delwin(win);
	touchwin(stdscr);
	return res;
}

int ui_input(const char *title, const char *prompt, char *buf, int max_len) {
	return ui_input_ex(title, prompt, buf, max_len, 70);
}

int ui_input_narrow(const char *title, const char *prompt, char *buf, int max_len) {
	return ui_input_ex(title, prompt, buf, max_len, 36);
}

static void ui_show_help_lines(const char *title, const char **lines, int total) {
	int top = 0;
	char filter[128] = "";
	int flen = 0;

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

	const char **f_lines = malloc(sizeof(const char *) * (size_t)(total > 0 ? total : 1));
	if (!f_lines) return;

	curs_set(0);
	WINDOW *win = newwin(box_h, box_w, start_y, start_x);
	wbkgd(win, g_ui_theme.win_bg);

	while (1) {
		int f_total = 0;
		if (flen == 0) {
			for (int i = 0; i < total; i++) f_lines[f_total++] = lines[i];
		} else {
			const char *pending_hdr = NULL;
			for (int i = 0; i < total; i++) {
				const char *ln = lines[i];
				if (strncmp(ln, "===", 3) == 0) {
					pending_hdr = ln;
					continue;
				}
				if (*ln == '\0') continue;
				if (strcasestr(ln, filter) || (pending_hdr && strcasestr(pending_hdr, filter))) {
					if (pending_hdr) {
						if (f_total > 0) f_lines[f_total++] = "";
						f_lines[f_total++] = pending_hdr;
						pending_hdr = NULL;
					}
					f_lines[f_total++] = ln;
				}
			}
		}

		draw_win_box(win);
		mvwprintw(win, 0, 2, " %s ", title);

		char pos_str[32];
		if (f_total > 0) {
			int vis_tmp = box_h - 5;
			if (vis_tmp < 1) vis_tmp = 1;
			snprintf(pos_str, sizeof(pos_str), " %d-%d/%d ", top + 1, (top + vis_tmp < f_total ? top + vis_tmp : f_total), f_total);
		} else {
			snprintf(pos_str, sizeof(pos_str), " 0/0 ");
		}
		if (box_w > (int)strlen(pos_str) + 4) {
			mvwprintw(win, 0, box_w - (int)strlen(pos_str) - 2, "%s", pos_str);
		}

		wattron(win, g_ui_theme.input_attr);
		mvwhline(win, 1, 2, ' ', box_w - 4);
		char f_disp[256];
		snprintf(f_disp, sizeof(f_disp), "Filter: %s", flen > 0 ? filter : "(type to filter)");
		mvwprint_utf8(win, 1, 3, box_w - 6, f_disp);
		wattroff(win, g_ui_theme.input_attr);

		const char *blbls[1] = { flen > 0 ? "[ Clear (Esc) ]" : "[ Close (Esc) ]" };
		int btn_x = (box_w - (int)strlen(blbls[0]) < 2) ? 2 : (box_w - (int)strlen(blbls[0])) / 2;
		UiButtonRow brow = { blbls, 1, box_h - 2, btn_x, 0, 0 };
		ui_btns_draw(&brow, win);
		const char *nav = " Type: Filter | Up/Down: Scroll ";
		if (box_w > (int)strlen(nav) + 4) mvwprintw(win, box_h - 1, box_w - (int)strlen(nav) - 2, "%s", nav);

		int vis = box_h - 5;
		if (vis < 1) vis = 1;
		if (top > f_total - vis) top = (f_total > vis) ? (f_total - vis) : 0;
		if (top < 0) top = 0;

		for (int i = 0; i < vis; i++) {
			mvwhline(win, 2 + i, 2, ' ', box_w - 4);
			if (top + i >= f_total) continue;
			const char *ln = f_lines[top + i];
			int is_hdr = (strncmp(ln, "===", 3) == 0);
			if (is_hdr) wattron(win, COLOR_PAIR(5) | A_UNDERLINE);
			mvwprint_utf8(win, 2 + i, 2, box_w - 4, ln);
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
			if (wy < 0 || wy >= box_h || wx < 0 || wx >= box_w) break;
			if (ui_btns_hit_test(&brow, wy, wx) == 0) {
				if (flen > 0) { flen = 0; filter[0] = '\0'; top = 0; continue; }
				break;
			}
			continue;
		}
		if (ev.action == ACT_ESC) {
			if (flen > 0) { flen = 0; filter[0] = '\0'; top = 0; continue; }
			break;
		}
		if (ev.action == ACT_ENTER || ev.action == ACT_F1 || ev.action == ACT_F10) break;
		else if (ev.action == ACT_BACKSPACE || ev.action == ACT_CTRL_U) {
			if (ev.action == ACT_CTRL_U) { flen = 0; filter[0] = '\0'; top = 0; }
			else if (flen > 0) {
				while (flen > 0 && ((unsigned char)filter[flen - 1] & 0xC0) == 0x80) flen--;
				if (flen > 0) flen--;
				filter[flen] = '\0';
				top = 0;
			}
		}
		else if (ev.action == ACT_CHAR && ev.ch >= 32) {
			char u8[8];
			int n = wctomb(u8, ev.ch);
			if (n > 0 && flen + n < (int)sizeof(filter) - 1) {
				memcpy(filter + flen, u8, (size_t)n);
				flen += n;
				filter[flen] = '\0';
				top = 0;
			}
		}
		else if (ev.action == ACT_UP && top > 0) top--;
		else if (ev.action == ACT_DOWN && top + vis < f_total) top++;
		else if (ev.action == ACT_PGUP) { top -= vis; if (top < 0) top = 0; }
		else if (ev.action == ACT_PGDN) { top += vis; }
		else if (ev.action == ACT_HOME) top = 0;
		else if (ev.action == ACT_END) top = f_total;
	}

	free(f_lines);
	delwin(win);
	touchwin(stdscr);
}

void ui_help(void) {
	const char *lines[] = {
		"=== 1. NAVIGATION & PANELS ===",
		"  Tab                Switch active panel (Left / Right)",
		"  Arrows / Home/End  Move cursor / jump to first or last item",
		"  PgUp / PgDn        Scroll one page up / down",
		"  Enter              Open directory or run executable",
		"  Shift+Enter        Open folder or file in system file manager",
		"  Ctrl+PgUp / PgDn   Go to parent (..) / enter directory",
		"  Ctrl+U             Swap left and right panels with cursor",
		"  Ctrl+R             Force reload both panels (auto-updates via inotify)",
		"  Ctrl+F3..F6        Sort active panel: Name, Ext, Time, Size (repeat: rev)",
		"  Alt+F1 / Alt+F2    Change drive / bookmarks / TmpPanel (0) menu",
		"                       Ins / + : Bookmark current dir with custom hotkey",
		"                       F8 / Del: Remove bookmark or safe-unmount USB",
		"  Insert / Shift+Arr Mark or unmark files & ranges (incl. Shift+Left/Right)",
		"  Alt+F10 / Ctrl+D   Folder History (512 steps; Go / Other / Insert)",
		"  Hold Alt / Ctrl    Dynamic 12-key F-bar (Alt: 8CmdH 9FileH 10DirH 11ScrH)",
		"  Alt+BkSp / Sh+BkSp Step Back / Forward through visited folder history",
		"  Alt+Shift+Home/End Select range from cursor to start / end of panel",
		"  + / - / *          Select by mask / Deselect by mask / Invert selection",
		"  Alt+<char>         Quick search by prefix (auto EN/RU layout)",
		"                     Continue typing without Alt; Esc to cancel",
		"",
		"=== 2. VIEWER, EDITOR & FILE OPERATIONS ===",
		"  F1                 Open this Help guide (type to live-filter)",
		"  F2 / F9            Open User Menu / Top Menu (Compare Dirs, File Assoc, Sort)",
		"  File Associations  ~/.config/xcommander/assoc (Enter / F3 / F4 rules; F9->Commands)",
		"  Ctrl+I             Toggle Info Panel (dir size, Git branch, perms, disk)",
		"  F3                 View file (Text/Hex) or calculate folder(s) size",
		"  F4                 Built-in TUI Editor (F1 help; Ctrl+. / Ctrl+M macros)",
		"  Shift+F4           Create new file and open in built-in Editor",
		"  F5 / F6            Copy / Move or rename (with progress bar & Esc cancel)",
		"  Shift+F5 / F6      Copy / Rename single file in-place (name only)",
		"  Alt+F6             Make symbolic (ln -s) or hard (ln) link",
		"  F7 / F8 / Sh+F8    MkDir / Trash / Permanent Delete (with progress bar)",
		"  Ctrl+A             File attributes, owner & timestamps (chmod/chown/time)",
		"  Auto sudo          Red password prompt on EACCES/EPERM (F2/F5-F8/Alt+F6/Ctrl+A)",
		"  Ctrl+G             Apply command to selected items (macros: !.! !~ !. !: !/)",
		"  Alt+F7             Find files by mask & text (Go to, View, Edit, Panel)",
		"  Ctrl+PgUp (TmpPnl) Jump to real directory of item in Temporary Panel",
		"  Enter / Ctrl+PgDn  Enter archive (.zip, .tar.*, .7z, .jar, .apk)",
		"                       Inside: F3 View, F4 Edit+Upd, F5 Extract, F8 Del",
		"  Shift+F1 / Alt+F5  Pack selected items to archive (.zip/.tar.*/.7z)",
		"",
		"=== 3. COMMAND LINE & SMART CLIPBOARD ===",
		"  Alt+[ / Alt+]      Insert Left / Right panel path into cmd",
		"  Ctrl+Ent/Alt+Ent   Insert filename into cmd (auto ./ for executables)",
		"  Shift+Enter        Run command and open output directly in F4 editor",
		"  Type in cmd        Live history popup from 1st char (Up/Dn, Right, Esc)",
		"  Ctrl+C / Ctrl+X    Copy / Cut file (URI-list) or selected text",
		"  Alt+Shift+N        Copy filename(s) as plain text to clipboard",
		"  Ctrl+V / Shift+Ins Paste file, text, or multiline script dialog",
		"  Home / End         Jump to start / end of command line (Ctrl+A in Ctrl+O)",
		"  Ctrl+Left / Right  Jump cursor by words in command line",
		"  Shift+Arr/Home/End Select chars / to start or end in cmd & dialogs",
		"  Ctrl+Shift+Arrows  Select whole words (also Alt+Shift+Arrows)",
		"  Alt+F11 / Ctrl+L   Multiline Script History (Enter:Run F4:Edit F8:Delete)",
		"  Alt+F12            Toggle 32-color auto file colorization by extension",
		"  Alt+F8 / Ctrl+H    Command History (512 items; filter, Run/Insert/Jump)",
		"  Alt+F9             File History (512 items + cursor pos; Edit/View/Jump)",
		"  Ctrl+E / Ctrl+X    Step through command history inline",
		"  Ctrl+U / Esc       Clear command line (when text is entered)",
		"",
		"=== 4. MOUSE, CLI & TERMINAL MODE ===",
		"  Mouse Left / Dbl   Focus panel & move cursor / Open folder or run",
		"  Mouse Right        Toggle file selection mark (like Insert)",
		"  Mouse Wheel        Scroll panels, menus, viewer, editor & help",
		"  F-Bar Click        Click any F1-F10 button on bottom bar",
		"  Ctrl+O             Toggle terminal mode (native mouse scroll/select)",
		"  F10 / Ctrl+Q       Save panel paths & history and quit",
		"  Built-in :dialog   :dialog / :D [msg|confirm|input|menu|form] (also $(:dialog ...))",
		"  CLI Options        xc -e [FILE|-] | -v [FILE|-] | -D [--bg C] msg|confirm|input|menu|form",
		"  Configs            ~/.config/xcommander/{menu,assoc,prompt,bookmarks}"
	};
#ifndef XC_BUILD
#define XC_BUILD 0
#endif
	char help_title[64];
	snprintf(help_title, sizeof(help_title), "xCommander Help (F1) [%06d]", XC_BUILD);
	ui_show_help_lines(help_title, lines, (int)(sizeof(lines) / sizeof(lines[0])));
}

void ui_editor_help(void) {
	const char *lines[] = {
		"=== 1. CURSOR MOVEMENT & SCROLLING ===",
		"  Arrows             Move cursor by character / line",
		"  Ctrl+Left / Right  Jump cursor by words",
		"  Home / End         Jump to start / end of current line",
		"  Ctrl+Home / End    Jump to start / end of entire file",
		"  Ctrl+]             Jump to matching bracket or quote",
		"  Alt+F8             Go to line[:col] number",
		"  PageUp / PageDown  Scroll one page up / down",
		"  Mouse Click        Place cursor at clicked line and column",
		"  Mouse Wheel        Scroll text up / down",
		"",
		"=== 2. BLOCK SELECTION & VIEW OPTIONS ===",
		"  Shift+Arrows       Select characters / lines",
		"  Shift+Home / End   Select from cursor to start / end of line",
		"  Ctrl+Sh+Home / End Select from cursor to start / end of entire file",
		"  Ctrl+Shift+Arrows  Select whole words (also Alt+Shift+Arrows)",
		"  F3                 Toggle MARK selection mode (move cursor to extend)",
		"  Alt+Shift+]        Select entire block between matching pair",
		"  Ctrl+A             Select entire file",
		"  Ctrl+C             Copy selected block to system clipboard",
		"  Ctrl+X             Cut selected block (or line) to clipboard",
		"  Ctrl+V / Shift+Ins Paste text (supports multiline paste)",
		"  Del / BkSp         Delete selected block (or char)",
		"  F4                 Toggle syntax highlighting on/off",
		"  F5                 Toggle visible whitespace (spaces, tabs, EOL)",
		"",
		"=== 3. EDITING, SEARCH & FILE CONTROL ===",
		"  Enter              Split line (with auto-indent if enabled)",
		"  Tab / Shift+Tab    Insert tab or indent / unindent selected block",
		"  Insert             Toggle Insert (INS) / Overwrite (OVR) mode",
		"  Ctrl+Z / Ctrl+R    Undo / Redo changes (up to 64 steps)",
		"  Ctrl+Y             Delete entire current line",
		"  Ctrl+Shift+D / /   Duplicate line/block / Toggle line or block comment",
		"  Ctrl+Shift+Up/Dn   Move current line or selected block up / down",
		"  F2 / Shift+F2      Save file (auto sudo prompt if needed) / Save As",
		"  F7                 Find text (Case sensitive & RegExp options)",
		"  Shift+F7           Find next match",
		"  Ctrl+F7            Interactive Replace (Case sensitive, RegExp, \\t, \\1..\\9)",
		"  Ctrl+. / Ctrl+M    Record/Stop macro / Manage & delete saved macros",
		"  Ctrl+O             Toggle terminal output screen (like FAR)",
		"  Hold Ctrl/Shift    Dynamic 12-key F1..F12 bar highlights active keys",
		"  F9                 Toggle Auto-Indent (Ind:On / Ind:Of)",
		"  CLI / Dialogs      xc -e [LINE[:COL]] [FILE|-]; .dialog keys: type/title/bg/text/default/items",
		"  F1                 Open this Editor Help",
		"  Esc / F10 / Ctrl+Q Exit editor (Yes / No / Cancel if modified)"
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
	char name_noext[MAX_FILENAME], ext_only[MAX_FILENAME], full_fp[MAX_PATH * 2], dir_sl[MAX_PATH + 2];
	snprintf(name_noext, sizeof(name_noext), "%s", cur_name);
	ext_only[0] = '\0';
	char *dot = strrchr(name_noext, '.');
	if (dot && dot != name_noext) { snprintf(ext_only, sizeof(ext_only), "%s", dot + 1); *dot = '\0'; }
	if (strcmp(act_path, "/") == 0) { snprintf(full_fp, sizeof(full_fp), "/%s", cur_name); snprintf(dir_sl, sizeof(dir_sl), "/"); }
	else { snprintf(full_fp, sizeof(full_fp), "%s/%s", act_path, cur_name); snprintf(dir_sl, sizeof(dir_sl), "%s/", act_path); }

	size_t j = 0;
	for (size_t i = 0; tmpl[i] != '\0' && j + 1 < out_size; i++) {
		if (tmpl[i] == '!' && tmpl[i + 1] != '\0') {
			const char *rep = NULL; size_t skip = 1;
			if (tmpl[i + 1] == '.' && tmpl[i + 2] == '!') { rep = cur_name; skip = 2; }
			else if (tmpl[i + 1] == '~') rep = name_noext;
			else if (tmpl[i + 1] == '.') rep = ext_only;
			else if (tmpl[i + 1] == ':') rep = full_fp;
			else if (tmpl[i + 1] == '/') rep = dir_sl;
			if (rep) { size_t rl = strlen(rep); if (j + rl >= out_size) rl = out_size - 1 - j; memcpy(out + j, rep, rl); j += rl; i += skip; continue; }
		}
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

static void get_assoc_path(char *out, size_t out_size) {
	const char *cfg = getenv("XDG_CONFIG_HOME");
	char dir[MAX_PATH];
	if (cfg && *cfg) snprintf(dir, sizeof(dir), "%s/xcommander", cfg);
	else {
		const char *home = getenv("HOME");
		if (!home || !*home) home = "/tmp";
		char cdir[MAX_PATH];
		snprintf(cdir, sizeof(cdir), "%s/.config", home); mkdir(cdir, 0755);
		snprintf(dir, sizeof(dir), "%s/.config/xcommander", home);
	}
	mkdir(dir, 0755);
	snprintf(out, out_size, "%s/assoc", dir);
}

static void ensure_default_assoc(const char *path) {
	struct stat st;
	if (stat(path, &st) == 0) return;
	FILE *f = fopen(path, "w");
	if (!f) return;
	fputs("# xCommander File Associations\n", f);
	fputs("# Format: mask1,mask2 | Description | Enter_cmd | F3_cmd (optional) | F4_cmd (optional)\n", f);
	fputs("# Macros: !.! (filename), !~ (name without ext), !. (ext), !: (active dir), !/ (passive dir), :dialog\n", f);
	fputs("*.jpg,*.jpeg,*.png,*.webp,*.gif | Images | xdg-open \"!.!\" >/dev/null 2>&1 & | |\n", f);
	fputs("*.pdf | PDF Documents | xdg-open \"!.!\" >/dev/null 2>&1 & | pdftotext \"!.!\" - |\n", f);
	fputs("*.mp4,*.mkv,*.avi,*.webm,*.mp3,*.flac | Media | xdg-open \"!.!\" >/dev/null 2>&1 & | |\n", f);
	fclose(f);
}

static int assoc_mask_match(const char *masks, const char *filename) {
	char buf[512];
	snprintf(buf, sizeof(buf), "%s", masks);
	char *save = NULL, *tok = strtok_r(buf, ",", &save);
	while (tok) {
		while (*tok == ' ' || *tok == '\t') tok++;
		size_t l = strlen(tok);
		while (l > 0 && (tok[l - 1] == ' ' || tok[l - 1] == '\t')) tok[--l] = '\0';
		if (l > 0 && fnmatch(tok, filename, FNM_CASEFOLD) == 0) return 1;
		tok = strtok_r(NULL, ",", &save);
	}
	return 0;
}

int ui_try_file_assoc(const char *filename, int key_mode) {
	if (!filename || !filename[0] || strcmp(filename, "..") == 0) return 0;
	char path[MAX_PATH + 32];
	get_assoc_path(path, sizeof(path));
	ensure_default_assoc(path);
	FILE *f = fopen(path, "r");
	if (!f) return 0;

	char descs[32][128], cmds[32][MAX_CMD];
	const char *menu_items[32];
	int mcnt = 0;
	char line[2048];
	while (fgets(line, sizeof(line), f) && mcnt < 32) {
		size_t len = strlen(line);
		while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = '\0';
		char *p = line;
		while (*p == ' ' || *p == '\t') p++;
		if (!*p || *p == '#' || *p == ';') continue;
		char *cols[5] = { p, NULL, NULL, NULL, NULL };
		int ncol = 1;
		for (char *c = p; *c && ncol < 5; c++) if (*c == '|') { *c = '\0'; cols[ncol++] = c + 1; }
		for (int k = 0; k < ncol; k++) {
			while (*cols[k] == ' ' || *cols[k] == '\t') cols[k]++;
			size_t cl = strlen(cols[k]);
			while (cl > 0 && (cols[k][cl - 1] == ' ' || cols[k][cl - 1] == '\t')) cols[k][--cl] = '\0';
		}
		int cmd_col = 2 + key_mode; /* 2=Enter, 3=F3, 4=F4 */
		if (ncol <= cmd_col || !cols[cmd_col] || !cols[cmd_col][0]) continue;
		if (!assoc_mask_match(cols[0], filename)) continue;
		snprintf(descs[mcnt], sizeof(descs[0]), "%s", (ncol > 1 && cols[1][0]) ? cols[1] : cols[cmd_col]);
		snprintf(cmds[mcnt], sizeof(cmds[0]), "%s", cols[cmd_col]);
		menu_items[mcnt] = descs[mcnt];
		mcnt++;
	}
	fclose(f);
	if (mcnt == 0) return 0;

	int pick = 0;
	if (mcnt > 1) {
		char chosen[128] = "";
		if (!ui_dialog_menu(filename, menu_items, mcnt, chosen, sizeof(chosen))) return 1;
		for (int i = 0; i < mcnt; i++) if (strcmp(descs[i], chosen) == 0) { pick = i; break; }
	}
	char final_cmd[MAX_CMD];
	expand_menu_cmd(cmds[pick], final_cmd, sizeof(final_cmd));
	if (!ui_try_builtin_dialog_cmd(final_cmd)) {
		char lkeep[MAX_FILENAME] = "", rkeep[MAX_FILENAME] = "";
		int lsel = left_panel.selected, rsel = right_panel.selected;
		if (left_panel.count > 0 && lsel < left_panel.count) snprintf(lkeep, sizeof(lkeep), "%s", left_panel.files[lsel].name);
		if (right_panel.count > 0 && rsel < right_panel.count) snprintf(rkeep, sizeof(rkeep), "%s", right_panel.files[rsel].name);
		execute_system_command(final_cmd);
		load_directory(&left_panel);
		load_directory(&right_panel);
		if (lkeep[0]) { int f = 0; for (int i = 0; i < left_panel.count; i++) if (strcmp(left_panel.files[i].name, lkeep) == 0) { left_panel.selected = i; f = 1; break; } if (!f && left_panel.count > 0) left_panel.selected = (lsel < left_panel.count) ? lsel : (left_panel.count - 1); }
		if (rkeep[0]) { int f = 0; for (int i = 0; i < right_panel.count; i++) if (strcmp(right_panel.files[i].name, rkeep) == 0) { right_panel.selected = i; f = 1; break; } if (!f && right_panel.count > 0) right_panel.selected = (rsel < right_panel.count) ? rsel : (right_panel.count - 1); }
	}
	return 1;
}

void ui_file_assoc_menu(void) {
	char path[MAX_PATH + 32];
	get_assoc_path(path, sizeof(path));
	ensure_default_assoc(path);
	if (ui_dialog_show(1, "File Associations", "Edit ~/.config/xcommander/assoc in built-in editor (F4)?")) {
		ui_edit_file(path);
	}
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
			ui_edit_file(menu_path);
			clearok(stdscr, TRUE); draw_screen(); refresh();
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
			int out_i = 0;
			for (int k = 0; k < len && out_i < (int)sizeof(line) - 5; k++) {
				if (p[k] == '\t') {
					for (int sp = 0; sp < 4; sp++) line[out_i++] = ' ';
				} else if ((unsigned char)p[k] >= 32) {
					line[out_i++] = p[k];
				}
			}
			line[out_i] = '\0';
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

static void get_bookmarks_path(char *out, size_t out_size) {
	const char *home = getenv("HOME");
	if (!home || !*home) home = "/tmp";
	char d1[MAX_PATH];
	snprintf(d1, sizeof(d1), "%s/.config", home); mkdir(d1, 0755);
	snprintf(d1, sizeof(d1), "%s/.config/xcommander", home); mkdir(d1, 0755);
	snprintf(out, out_size, "%s/.config/xcommander/bookmarks", home);
}

static void bookmark_add_entry(char hk, const char *dir_path) {
	if (!dir_path || !*dir_path) return;
	char bpath[MAX_PATH + 64];
	get_bookmarks_path(bpath, sizeof(bpath));

	char lines[64][MAX_PATH + 8];
	int cnt = 0;
	FILE *rf = fopen(bpath, "r");
	if (rf) {
		char buf[MAX_PATH + 8];
		while (fgets(buf, sizeof(buf), rf) && cnt < 63) {
			size_t l = strlen(buf);
			while (l > 0 && (buf[l - 1] == '\n' || buf[l - 1] == '\r')) buf[--l] = '\0';
			if (l == 0) continue;
			const char *p = (buf[0] != '\0' && buf[1] == ':' && buf[2] == '/') ? (buf + 2) : buf;
			if (strcmp(p, dir_path) == 0) continue;
			snprintf(lines[cnt++], sizeof(lines[0]), "%s", buf);
		}
		fclose(rf);
	}

	FILE *wf = fopen(bpath, "w");
	if (!wf) return;
	for (int i = 0; i < cnt; i++) fprintf(wf, "%s\n", lines[i]);
	if (hk) fprintf(wf, "%c:%s\n", hk, dir_path);
	else fprintf(wf, "%s\n", dir_path);
	fclose(wf);
}

static void bookmark_remove_entry(const char *dir_path) {
	if (!dir_path || !*dir_path) return;
	char bpath[MAX_PATH + 64];
	get_bookmarks_path(bpath, sizeof(bpath));

	char lines[64][MAX_PATH + 8];
	int cnt = 0;
	FILE *rf = fopen(bpath, "r");
	if (!rf) return;
	char buf[MAX_PATH + 8];
	while (fgets(buf, sizeof(buf), rf) && cnt < 63) {
		size_t l = strlen(buf);
		while (l > 0 && (buf[l - 1] == '\n' || buf[l - 1] == '\r')) buf[--l] = '\0';
		if (l == 0) continue;
		const char *p = (buf[0] != '\0' && buf[1] == ':' && buf[2] == '/') ? (buf + 2) : buf;
		if (strcmp(p, dir_path) == 0) continue;
		snprintf(lines[cnt++], sizeof(lines[0]), "%s", buf);
	}
	fclose(rf);

	FILE *wf = fopen(bpath, "w");
	if (!wf) return;
	for (int i = 0; i < cnt; i++) fprintf(wf, "%s\n", lines[i]);
	fclose(wf);
}

void ui_drive_menu(Panel *target_panel, int is_right) {
	if (!target_panel) return;

	while (1) {
		char paths[48][MAX_PATH];
		char devs[48][256];
		char labels[48][128];
		char custom_hk[48];
		int item_type[48];
		int count = 0;

		Panel *other_p = is_right ? &left_panel : &right_panel;
		if (other_p && *other_p->current_path && count < 48) {
			snprintf(paths[count], MAX_PATH, "%s", other_p->current_path);
			devs[count][0] = '\0';
			custom_hk[count] = '-';
			item_type[count] = 3;
			const char *ob = strrchr(other_p->current_path, '/');
			ob = (ob && *(ob + 1)) ? (ob + 1) : other_p->current_path;
			snprintf(labels[count], sizeof(labels[0]), "%s (Other)", ob);
			count++;
		}
		const char *home = getenv("HOME");
		if (home && *home) {
			snprintf(paths[count], MAX_PATH, "%s", home);
			devs[count][0] = '\0';
			custom_hk[count] = '~';
			item_type[count] = 0;
			snprintf(labels[count], sizeof(labels[0]), "~ (Home)");
			count++;
		}

		snprintf(paths[count], MAX_PATH, "/");
		devs[count][0] = '\0';
		custom_hk[count] = '\x01';
		item_type[count] = 0;
		snprintf(labels[count], sizeof(labels[0]), "/ (Root)");
		count++;

		snprintf(paths[count], MAX_PATH, "::TEMP::");
		devs[count][0] = '\0';
		custom_hk[count] = '0';
		item_type[count] = 0;
		snprintf(labels[count], sizeof(labels[0]), "TmpPanel (Temporary)");
		count++;

		FILE *mf = fopen("/proc/mounts", "r");
		if (mf) {
			char dev[256], mnt[MAX_PATH], fstype[64];
			while (fscanf(mf, "%255s %4095s %63s %*[^\n]\n", dev, mnt, fstype) == 3 && count < 36) {
				if (strcmp(mnt, "/") == 0) continue;
				int is_media = (strncmp(mnt, "/media/", 7) == 0 || strncmp(mnt, "/mnt/", 5) == 0 || strncmp(mnt, "/run/media/", 11) == 0);
				if (strncmp(dev, "/dev/", 5) != 0 && strcmp(fstype, "ntfs3") != 0 && strcmp(fstype, "nfs") != 0 && strcmp(fstype, "cifs") != 0 && !(is_media && strncmp(fstype, "fuse", 4) == 0)) continue;
				if (strncmp(mnt, "/boot", 5) == 0 || strcmp(mnt, "/efi") == 0 || strncmp(mnt, "/snap", 5) == 0) continue;
				int dup = 0;
				for (int i = 0; i < count; i++) { if (strcmp(paths[i], mnt) == 0) { dup = 1; break; } }
				if (dup) continue;
				snprintf(paths[count], MAX_PATH, "%s", mnt);
				snprintf(devs[count], sizeof(devs[0]), "%s", dev);
				custom_hk[count] = '\0';
				item_type[count] = (is_media && strncmp(dev, "/dev/", 5) == 0) ? 1 : 0;
				const char *bname = strrchr(mnt, '/');
				bname = (bname && *(bname + 1)) ? (bname + 1) : mnt;
				snprintf(labels[count], sizeof(labels[0]), "%s (%s)", bname, fstype);
				count++;
			}
			fclose(mf);
		}

		char bpath[MAX_PATH + 64];
		get_bookmarks_path(bpath, sizeof(bpath));
		FILE *bf = fopen(bpath, "r");
		if (bf) {
			char line[MAX_PATH + 8];
			while (fgets(line, sizeof(line), bf) && count < 47) {
				size_t l = strlen(line);
				while (l > 0 && (line[l - 1] == '\n' || line[l - 1] == '\r' || line[l - 1] == ' ')) line[--l] = '\0';
				if (line[0] == '\0' || line[0] == '#') continue;
				char hk = '\0';
				const char *p = line;
				if (line[0] != '\0' && line[1] == ':' && line[2] == '/') { hk = line[0]; p = line + 2; }
				int dup = 0;
				for (int i = 0; i < count; i++) { if (item_type[i] != 3 && strcmp(paths[i], p) == 0) { if (hk && !custom_hk[i]) custom_hk[i] = hk; dup = 1; break; } }
				if (dup) continue;
				snprintf(paths[count], MAX_PATH, "%s", p);
				devs[count][0] = '\0';
				custom_hk[count] = hk;
				item_type[count] = 2;
				const char *bname = strrchr(p, '/');
				bname = (bname && *(bname + 1)) ? (bname + 1) : p;
				snprintf(labels[count], sizeof(labels[0]), "* %s", bname);
				count++;
			}
			fclose(bf);
		}

		const char *auto_pool = "0123456789abcdefghijklmnopqrstuvwxyz";
		char item_hk[48];
		for (int i = 0; i < count; i++) item_hk[i] = custom_hk[i];
		for (int i = 0; i < count; i++) {
			if (item_hk[i] != '\0') continue;
			for (int k = 0; auto_pool[k] != '\0'; k++) {
				char cand = auto_pool[k];
				int used = 0;
				for (int j = 0; j < count; j++) { if (tolower((unsigned char)item_hk[j]) == cand) { used = 1; break; } }
				if (!used) { item_hk[i] = cand; break; }
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
			} else free_info[i][0] = '\0';
			if (item_type[i] != 3 && strcmp(paths[i], target_panel->current_path) == 0) sel = i;
		}

		int height, width;
		getmaxyx(stdscr, height, width);
		int panel_w = width / 2;
		int box_w = panel_w - 4;
		if (box_w > 56) box_w = 56;
		if (box_w < 42) box_w = 42;
		if (box_w > width - 2) box_w = width - 2;
		int sep_idx = -1;
		for (int i = 0; i < count; i++) { if (item_type[i] == 2) { sep_idx = i; break; } }
		int disp_total = count + (sep_idx >= 0 ? 1 : 0);
		int box_h = disp_total + 2;
		if (box_h > height - 4) box_h = height - 4;
		if (box_h < 5) box_h = 5;
		int panel_x = is_right ? panel_w : 0;
		int start_x = panel_x + (panel_w - box_w) / 2;
		if (start_x < 1) start_x = 1;
		if (start_x + box_w >= width) start_x = width - box_w - 1;
		int start_y = (height - 2 - box_h) / 2;
		if (start_y < 1) start_y = 1;

		curs_set(0);
		WINDOW *win = newwin(box_h, box_w, start_y, start_x);
		wbkgd(win, COLOR_PAIR(5));
		int chosen = -1, action_mode = 0;

		while (1) {
			int vis = box_h - 2;
			if (vis < 1) vis = 1;
			int sel_disp = sel + (sep_idx >= 0 && sel >= sep_idx ? 1 : 0);
			int top = (sel_disp >= vis) ? (sel_disp - vis + 1) : 0;
			werase(win);
			draw_win_box(win);
			mvwprintw(win, 0, (box_w - 16) / 2, is_right ? " Change Right " : " Change Left ");

			for (int i = 0; i < vis && (top + i) < disp_total; i++) {
				int dpos = top + i;
				if (sep_idx >= 0 && dpos == sep_idx) {
					wattron(win, COLOR_PAIR(5));
					mvwaddstr(win, 1 + i, 0, "├");
					for (int x = 1; x < box_w - 1; x++) mvwaddstr(win, 1 + i, x, "─");
					mvwaddstr(win, 1 + i, box_w - 1, "┤");
					wattroff(win, COLOR_PAIR(5));
					continue;
				}
				int idx = (sep_idx >= 0 && dpos > sep_idx) ? (dpos - 1) : dpos;
				char kch = (item_hk[idx] > 32) ? item_hk[idx] : ' ';
				int flen = (int)strlen(free_info[idx]);
				int left_w = box_w - 6 - flen;
				if (left_w < 8) left_w = 8;
				char left_str[160];
				snprintf(left_str, sizeof(left_str), "%c. %s", kch, labels[idx]);
				wattron(win, idx == sel ? COLOR_PAIR(2) : COLOR_PAIR(5));
				mvwhline(win, 1 + i, 1, ' ', box_w - 2);
				mvwprint_utf8(win, 1 + i, 2, left_w, left_str);
				if (flen > 0 && box_w - 3 - flen > 4) mvwprintw(win, 1 + i, box_w - 2 - flen, "%s", free_info[idx]);
				wattroff(win, idx == sel ? COLOR_PAIR(2) : COLOR_PAIR(5));
			}
			wrefresh(win);

			InputEvent ev = input_read();
			if (ev.action == ACT_ESC || ev.action == ACT_F10) break;
			if (ev.action == ACT_MOUSE_WHEEL_UP) { if (sel > 0) sel--; continue; }
			if (ev.action == ACT_MOUSE_WHEEL_DOWN) { if (sel + 1 < count) sel++; continue; }
			if (ev.action == ACT_MOUSE_LEFT) {
				int wy = ev.mouse_y - start_y, wx = ev.mouse_x - start_x;
				if (wy < 0 || wy >= box_h || wx < 0 || wx >= box_w) break;
				if (wy >= 1 && wy < 1 + vis && (top + wy - 1) < disp_total) {
					int dpos = top + (wy - 1);
					if (sep_idx >= 0 && dpos == sep_idx) continue;
					chosen = (sep_idx >= 0 && dpos > sep_idx) ? (dpos - 1) : dpos;
					break;
				}
				continue;
			}
			if (ev.action == ACT_INSERT || (ev.action == ACT_CHAR && (ev.ch == L'+' || ev.ch == L'='))) { action_mode = 2; break; }
			if (ev.action == ACT_F8 || ev.action == ACT_DELETE) { if (count > 0) { action_mode = 1; break; } }
			if (ev.action == ACT_UP && sel > 0) sel--;
			else if (ev.action == ACT_DOWN && sel + 1 < count) sel++;
			else if (ev.action == ACT_ENTER && count > 0) { chosen = sel; break; }
			else if (ev.action == ACT_CHAR) {
				wchar_t ch = towlower(ev.ch);
				if (ch == L'ё') ch = L'~';
				const wchar_t *ru = L"йцукенгшщзхъфывапролджэячсмитьбю";
				const char    *en = "qwertyuiop[]asdfghjkl;'zxcvbnm,.";
				for (int k = 0; ru[k] != L'\0'; k++) { if (ch == ru[k]) { ch = (wchar_t)en[k]; break; } }
				if (ch > 32) {
					for (int i = 0; i < count; i++) {
						if (item_hk[i] > 32 && (wchar_t)tolower((unsigned char)item_hk[i]) == ch) { chosen = i; break; }
					}
				}
				if (chosen >= 0) break;
			}
		}

		delwin(win);
		touchwin(stdscr);

		if (action_mode == 2) {
			char hk_buf[16] = "";
			char prompt[MAX_PATH + 64];
			snprintf(prompt, sizeof(prompt), "Hotkey (1 char, empty=auto) for %s:", target_panel->current_path);
			draw_screen(); refresh();
			if (ui_input("Add Bookmark (Ins)", prompt, hk_buf, sizeof(hk_buf))) {
				char chosen_hk = '\0';
				if (hk_buf[0] != '\0') {
					wchar_t whk = 0;
					if (mbtowc(&whk, hk_buf, strlen(hk_buf)) > 0) {
						whk = towlower(whk);
						const wchar_t *ru = L"йцукенгшщзхъфывапролджэячсмитьбю";
						const char    *en = "qwertyuiop[]asdfghjkl;'zxcvbnm,.";
						for (int k = 0; ru[k] != L'\0'; k++) { if (whk == ru[k]) { whk = (wchar_t)en[k]; break; } }
						if (whk >= 33 && whk < 127 && whk != L':' && whk != L'-' && whk != L'~') chosen_hk = (char)whk;
					}
				}
				bookmark_add_entry(chosen_hk, target_panel->current_path);
			}
			draw_screen(); refresh();
			continue;
		}

		if (action_mode == 1 && sel >= 0 && sel < count) {
			if (item_type[sel] == 2) {
				char qmsg[160];
				snprintf(qmsg, sizeof(qmsg), "Remove bookmark %s?", labels[sel]);
				draw_screen(); refresh();
				if (ui_confirm("Remove Bookmark (F8)", qmsg)) bookmark_remove_entry(paths[sel]);
				draw_screen(); refresh();
				continue;
			}
			if (item_type[sel] != 1 || devs[sel][0] == '\0') {
				draw_screen(); refresh();
				ui_message("Unmount", "Only removable drives or * bookmarks can be removed/unmounted");
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
			if (strncmp(left_panel.current_path, paths[sel], mlen) == 0 && (left_panel.current_path[mlen] == '/' || left_panel.current_path[mlen] == '\0')) {
				snprintf(left_panel.current_path, MAX_PATH, "%s", fallback);
				load_directory(&left_panel);
			}
			if (strncmp(right_panel.current_path, paths[sel], mlen) == 0 && (right_panel.current_path[mlen] == '/' || right_panel.current_path[mlen] == '\0')) {
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
			if (strcmp(paths[chosen], "::TEMP::") == 0) {
				temp_panel_open(target_panel);
			} else {
				target_panel->is_temp = 0;
				target_panel->archive_file[0] = '\0';
				target_panel->archive_dir[0] = '\0';
				snprintf(target_panel->current_path, MAX_PATH, "%s", paths[chosen]);
				target_panel->parent_name[0] = '\0';
				load_directory(target_panel);
			}
			active_panel = target_panel;
		}
		break;
	}
}

int ui_search_dialog(char *search_buf, int s_max, int *case_sens, int *use_regex) {
	int h, w; getmaxyx(stdscr, h, w);
	int bw = (w - 10 > 70) ? 70 : ((w - 10 < 44) ? 44 : w - 10), bh = 8;
	int sy = (h - bh) / 2, sx = (w - bw) / 2;
	WINDOW *win = newwin(bh, bw, sy, sx); keypad(win, TRUE); wbkgd(win, g_ui_theme.win_bg);

	wchar_t wb[MAX_PATH];
	size_t n = mbstowcs(wb, search_buf, MAX_PATH - 1);
	wb[(n == (size_t)-1) ? 0 : (int)n] = 0;

	int cs = (case_sens && *case_sens) ? 1 : 0;
	int rx = (use_regex && *use_regex) ? 1 : 0;
	int fw = bw - 6, foc = 0, res = 0, cx1 = 3, cx2 = 24;

	UiTextField tf;
	ui_tf_init(&tf, wb, MAX_PATH - 2, 1);
	ui_tf_set_bounds(&tf, 2, 3, fw);

	const char *blbls[2] = { "[ Search ]", "[ Cancel ]" };
	int bx = (bw - 24 < 2) ? 2 : (bw - 24) / 2;
	UiButtonRow btns = { blbls, 2, 6, bx, 4, -1 };

	while (1) {
		werase(win); draw_win_box(win); mvwprintw(win, 0, 2, " Find (F7) ");
		mvwprint_utf8(win, 1, 3, fw, "Search for:");
		ui_tf_draw(&tf, win, foc == 0);

		UiCheckbox cb_cs = { "Case sensitive", &cs, 4, cx1 };
		UiCheckbox cb_rx = { "RegExp", &rx, 4, cx2 };
		ui_cb_draw(&cb_cs, win, foc == 1);
		ui_cb_draw(&cb_rx, win, foc == 2);

		btns.focus_idx = (foc == 3) ? 0 : ((foc == 4) ? 1 : -1);
		ui_btns_draw(&btns, win);

		if (foc == 0) ui_tf_place_cursor(&tf, win);
		else curs_set(0);
		wrefresh(win);

		InputEvent ev = input_read();
		if (ev.action == ACT_MOUSE_LEFT) {
			int wy = ev.mouse_y - sy, wx = ev.mouse_x - sx;
			if (wy < 0 || wy >= bh || wx < 0 || wx >= bw) break;
			if (ui_tf_handle_mouse(&tf, wy, wx)) { foc = 0; continue; }
			if (wy == 4) {
				if (wx >= cx1 && wx < cx1 + 18) { foc = 1; cs = !cs; continue; }
				if (wx >= cx2 && wx < cx2 + 10) { foc = 2; rx = !rx; continue; }
			}
			int hit = ui_btns_hit_test(&btns, wy, wx);
			if (hit == 0) { foc = 3; ev.action = ACT_ENTER; }
			else if (hit == 1) break;
			else continue;
		}
		if (ev.action == ACT_ESC || ev.action == ACT_F10) break;
		if (ev.action == ACT_TAB) { foc = (foc + 1) % 5; continue; }
		if (ev.action == ACT_SHIFT_TAB) { foc = (foc + 4) % 5; continue; }
		if (ev.action == ACT_DOWN) { foc = (foc == 0) ? 1 : ((foc == 1) ? 3 : ((foc == 2) ? 4 : foc)); continue; }
		if (ev.action == ACT_UP) { foc = (foc == 1 || foc == 2) ? 0 : ((foc == 3) ? 1 : ((foc == 4) ? 2 : foc)); continue; }
		if ((foc == 1 || foc == 2) && ev.action == ACT_CHAR && ev.ch == L' ') { if (foc == 1) cs = !cs; else rx = !rx; continue; }
		if (ev.action == ACT_ENTER || ((foc == 3 || foc == 4) && ev.action == ACT_CHAR && ev.ch == L' ')) {
			if (foc == 4) break;
			wcstombs(search_buf, wb, (size_t)s_max); search_buf[s_max - 1] = '\0';
			if (case_sens) *case_sens = cs;
			if (use_regex) *use_regex = rx;
			res = 1; break;
		}
		if (foc == 1 || foc == 2) { if (ev.action == ACT_LEFT || ev.action == ACT_RIGHT) foc = 3 - foc; continue; }
		if (foc >= 3) { if (ev.action == ACT_LEFT || ev.action == ACT_RIGHT) foc = 7 - foc; continue; }
		if (ui_tf_handle_event(&tf, &ev)) { foc = 0; continue; }
	}
	curs_set(0); delwin(win); touchwin(stdscr); return res;
}

int ui_replace_dialog(char *search_buf, int s_max, char *replace_buf, int r_max, int *case_sens, int *use_regex) {
	int h, w; getmaxyx(stdscr, h, w);
	int bw = (w - 10 > 70) ? 70 : ((w - 10 < 44) ? 44 : w - 10), bh = 11;
	int sy = (h - bh) / 2, sx = (w - bw) / 2;
	WINDOW *win = newwin(bh, bw, sy, sx); keypad(win, TRUE); wbkgd(win, g_ui_theme.win_bg);

	wchar_t wb[2][MAX_PATH];
	for (int f = 0; f < 2; f++) {
		size_t n = mbstowcs(wb[f], f == 0 ? search_buf : replace_buf, MAX_PATH - 1);
		wb[f][(n == (size_t)-1) ? 0 : (int)n] = 0;
	}

	int cs = (case_sens && *case_sens) ? 1 : 0;
	int rx = (use_regex && *use_regex) ? 1 : 0;
	int fw = bw - 6, foc = 0, res = 0, cx1 = 3, cx2 = 24;

	UiTextField tf[2];
	ui_tf_init(&tf[0], wb[0], MAX_PATH - 2, 1);
	ui_tf_set_bounds(&tf[0], 2, 3, fw);
	ui_tf_init(&tf[1], wb[1], MAX_PATH - 2, 1);
	ui_tf_set_bounds(&tf[1], 5, 3, fw);

	const char *blbls[2] = { "[ Replace ]", "[ Cancel ]" };
	int bx = (bw - 25 < 2) ? 2 : (bw - 25) / 2;
	UiButtonRow btns = { blbls, 2, 9, bx, 4, -1 };

	while (1) {
		werase(win); draw_win_box(win); mvwprintw(win, 0, 2, " Replace (Ctrl+F7) ");
		mvwprint_utf8(win, 1, 3, fw, "Search for:");
		ui_tf_draw(&tf[0], win, foc == 0);
		mvwprint_utf8(win, 4, 3, fw, "Replace with:");
		ui_tf_draw(&tf[1], win, foc == 1);

		UiCheckbox cb_cs = { "Case sensitive", &cs, 7, cx1 };
		UiCheckbox cb_rx = { "RegExp", &rx, 7, cx2 };
		ui_cb_draw(&cb_cs, win, foc == 2);
		ui_cb_draw(&cb_rx, win, foc == 3);

		btns.focus_idx = (foc == 4) ? 0 : ((foc == 5) ? 1 : -1);
		ui_btns_draw(&btns, win);

		if (foc < 2) ui_tf_place_cursor(&tf[foc], win);
		else curs_set(0);
		wrefresh(win);

		InputEvent ev = input_read();
		if (ev.action == ACT_MOUSE_LEFT) {
			int wy = ev.mouse_y - sy, wx = ev.mouse_x - sx;
			if (wy < 0 || wy >= bh || wx < 0 || wx >= bw) break;
			if (ui_tf_handle_mouse(&tf[0], wy, wx)) { foc = 0; continue; }
			if (ui_tf_handle_mouse(&tf[1], wy, wx)) { foc = 1; continue; }
			if (wy == 7) {
				if (wx >= cx1 && wx < cx1 + 18) { foc = 2; cs = !cs; continue; }
				if (wx >= cx2 && wx < cx2 + 10) { foc = 3; rx = !rx; continue; }
			}
			int hit = ui_btns_hit_test(&btns, wy, wx);
			if (hit == 0) { foc = 4; ev.action = ACT_ENTER; }
			else if (hit == 1) break;
			else continue;
		}
		if (ev.action == ACT_ESC || ev.action == ACT_F10) break;
		if (ev.action == ACT_TAB) { foc = (foc + 1) % 6; continue; }
		if (ev.action == ACT_SHIFT_TAB) { foc = (foc + 5) % 6; continue; }
		if (ev.action == ACT_DOWN) { foc = (foc == 0) ? 1 : ((foc == 1) ? 2 : ((foc == 2) ? 4 : ((foc == 3) ? 5 : foc))); continue; }
		if (ev.action == ACT_UP) { foc = (foc == 1) ? 0 : ((foc == 2 || foc == 3) ? 1 : ((foc == 4) ? 2 : ((foc == 5) ? 3 : foc))); continue; }
		if ((foc == 2 || foc == 3) && ev.action == ACT_CHAR && ev.ch == L' ') { if (foc == 2) cs = !cs; else rx = !rx; continue; }
		if (ev.action == ACT_ENTER || ((foc == 4 || foc == 5) && ev.action == ACT_CHAR && ev.ch == L' ')) {
			if (foc == 5) break;
			wcstombs(search_buf, wb[0], (size_t)s_max); search_buf[s_max - 1] = '\0';
			wcstombs(replace_buf, wb[1], (size_t)r_max); replace_buf[r_max - 1] = '\0';
			if (case_sens) *case_sens = cs;
			if (use_regex) *use_regex = rx;
			res = 1; break;
		}
		if (foc == 2 || foc == 3) { if (ev.action == ACT_LEFT || ev.action == ACT_RIGHT) foc = 5 - foc; continue; }
		if (foc >= 4) { if (ev.action == ACT_LEFT || ev.action == ACT_RIGHT) foc = 9 - foc; continue; }
		if (foc < 2 && ui_tf_handle_event(&tf[foc], &ev)) continue;
	}
	curs_set(0); delwin(win); touchwin(stdscr); return res;
}

int ui_overwrite_prompt(const char *filename, off_t src_size, time_t src_mtime, off_t dst_size, time_t dst_mtime) {
	int h, w; getmaxyx(stdscr, h, w);
	int bw = (w - 4 < 56) ? (w - 4) : 56;
	if (bw < 44) bw = w - 2;
	int bh = 9, sy = (h - bh) / 2, sx = (w - bw) / 2;

	char s_sz[32], d_sz[32], s_tm[32] = "", d_tm[32] = "";
	format_size(src_size, s_sz, sizeof(s_sz));
	format_size(dst_size, d_sz, sizeof(d_sz));
	struct tm *tm1 = localtime(&src_mtime);
	if (tm1) strftime(s_tm, sizeof(s_tm), "%d.%m.%Y %H:%M", tm1);
	struct tm *tm2 = localtime(&dst_mtime);
	if (tm2) strftime(d_tm, sizeof(d_tm), "%d.%m.%Y %H:%M", tm2);

	char line_src[96], line_dst[96];
	snprintf(line_src, sizeof(line_src), "Source: %10s  %s", s_sz, s_tm);
	snprintf(line_dst, sizeof(line_dst), "Target: %10s  %s", d_sz, d_tm);

	curs_set(0);
	WINDOW *win = newwin(bh, bw, sy, sx);
	wbkgd(win, g_ui_theme.win_bg);

	const char *blbls[4] = { "[ Overwrite ]", "[ All ]", "[ Skip ]", "[ Cancel ]" };
	int bx0 = (bw - 41 < 2) ? 2 : (bw - 41) / 2;
	UiButtonRow btns = { blbls, 4, 7, bx0, 1, 0 };

	int sel = 0, res = 0;
	while (1) {
		werase(win);
		draw_win_box(win);
		mvwprintw(win, 0, 2, " Warning ");
		mvwprint_utf8(win, 1, 3, bw - 6, "Target file already exists:");
		mvwprint_utf8(win, 2, 3, bw - 6, filename);
		mvwprint_utf8(win, 4, 3, bw - 6, line_src);
		mvwprint_utf8(win, 5, 3, bw - 6, line_dst);

		btns.focus_idx = sel;
		ui_btns_draw(&btns, win);
		wrefresh(win);

		InputEvent ev = input_read();
		if (ev.action == ACT_MOUSE_LEFT) {
			int wy = ev.mouse_y - sy, wx = ev.mouse_x - sx;
			if (wy < 0 || wy >= bh || wx < 0 || wx >= bw) { res = 0; break; }
			int hit = ui_btns_hit_test(&btns, wy, wx);
			if (hit >= 0) { res = (hit == 3) ? 0 : (hit + 1); break; }
			continue;
		}
		if (ev.action == ACT_ESC || ev.action == ACT_F10) { res = 0; break; }
		if (ev.action == ACT_LEFT || ev.action == ACT_SHIFT_TAB) sel = (sel + 3) % 4;
		else if (ev.action == ACT_RIGHT || ev.action == ACT_TAB) sel = (sel + 1) % 4;
		else if (ev.action == ACT_ENTER || (ev.action == ACT_CHAR && ev.ch == L' ')) { res = (sel == 3) ? 0 : (sel + 1); break; }
		else if (ev.action == ACT_CHAR) {
			wchar_t ch = towlower(ev.ch);
			if (ch == L'o' || ch == L'y') { res = 1; break; }
			if (ch == L'a') { res = 2; break; }
			if (ch == L's' || ch == L'n') { res = 3; break; }
			if (ch == L'c') { res = 0; break; }
		}
	}
	delwin(win);
	touchwin(stdscr);
	return res;
}

int ui_replace_prompt(const char *search_str, const char *replace_str, int avoid_y) {
	int h, w; getmaxyx(stdscr, h, w);
	int bw = (w - 2 < 50) ? (w - 2) : 50, bh = 8, sx = (w - bw) / 2, sy = h - bh - 2;
	if (avoid_y >= sy - 1 && avoid_y <= sy + bh) sy = 2;
	if (sy < 1) sy = 1;
	WINDOW *win = newwin(bh, bw, sy, sx); wbkgd(win, g_ui_theme.win_bg);

	const char *blbls[4] = { "[ Replace ]", "[ All ]", "[ Skip ]", "[ Cancel ]" };
	int bx0 = (bw - 39 < 2) ? 2 : (bw - 39) / 2;
	UiButtonRow btns = { blbls, 4, 6, bx0, 1, 0 };

	int sel = 0, res = 0;
	while (1) {
		char s_disp[256], r_disp[256];
		const char *srcs[2] = { search_str, replace_str };
		char *dsts[2] = { s_disp, r_disp };
		for (int k = 0; k < 2; k++) {
			size_t j = 0;
			for (size_t i = 0; srcs[k][i] && j + 3 < 255; i++) {
				unsigned char c = (unsigned char)srcs[k][i];
				if (c == '\n') { dsts[k][j++] = '\\'; dsts[k][j++] = 'n'; }
				else if (c == '\t') { dsts[k][j++] = '\\'; dsts[k][j++] = 't'; }
				else if (c == '\r') { dsts[k][j++] = '\\'; dsts[k][j++] = 'r'; }
				else dsts[k][j++] = (char)c;
			}
			dsts[k][j] = '\0';
		}
		werase(win); draw_win_box(win); mvwprintw(win, 0, (bw - 9) / 2, " Replace ");
		mvwprintw(win, 1, (bw - 7) / 2, "Replace"); mvwprint_utf8(win, 2, 4, bw - 8, s_disp);
		mvwprintw(win, 3, (bw - 4) / 2, "with"); mvwprint_utf8(win, 4, 4, bw - 8, r_disp);

		btns.focus_idx = sel;
		ui_btns_draw(&btns, win);
		wrefresh(win);

		InputEvent ev = input_read();
		if (ev.action == ACT_MOUSE_LEFT) {
			int wy = ev.mouse_y - sy, wx = ev.mouse_x - sx;
			if (wy < 0 || wy >= bh || wx < 0 || wx >= bw) break;
			int hit = ui_btns_hit_test(&btns, wy, wx);
			if (hit >= 0) { res = (hit == 3) ? 0 : (hit + 1); break; }
			continue;
		}
		if (ev.action == ACT_ESC || ev.action == ACT_F10) break;
		if (ev.action == ACT_LEFT || ev.action == ACT_SHIFT_TAB) sel = (sel + 3) % 4;
		else if (ev.action == ACT_RIGHT || ev.action == ACT_TAB) sel = (sel + 1) % 4;
		else if (ev.action == ACT_ENTER || (ev.action == ACT_CHAR && ev.ch == L' ')) { res = (sel == 3) ? 0 : (sel + 1); break; }
		else if (ev.action == ACT_CHAR || ev.action == ACT_ALT_CHAR) {
			wchar_t ch = towlower(ev.ch);
			if (ch == L'r' || ch == L'y' || ch == L'к' || ch == L'н') { res = 1; break; }
			if (ch == L'a' || ch == L'ф') { res = 2; break; }
			if (ch == L's' || ch == L'n' || ch == L'ы' || ch == L'т') { res = 3; break; }
			if (ch == L'c' || ch == L'с') break;
		}
	}
	delwin(win); touchwin(stdscr); return res;
}

static void get_ml_path(char *out, size_t sz) {
	const char *h = getenv("HOME"); if (!h || !*h) h = "/tmp";
	char d[MAX_PATH]; snprintf(d, sizeof(d), "%s/.local/state/xcommander", h);
	snprintf(out, sz, "%s/multiline_history", d);
}
void ml_history_add(const char *dir, const char *script) {
	if (!script || !*script) return;
	char p[MAX_PATH + 48];
	get_ml_path(p, sizeof(p));

	char *hdr[64] = {0}, *bdy[64] = {0};
	int cnt = 0;

	FILE *rf = fopen(p, "rb");
	if (rf) {
		fseek(rf, 0, SEEK_END);
		long sz = ftell(rf);
		fseek(rf, 0, SEEK_SET);
		if (sz > 0 && sz < 1048576) {
			char *raw = malloc((size_t)sz + 1);
			if (raw) {
				size_t r = fread(raw, 1, (size_t)sz, rf);
				raw[r] = '\0';
				char *cur = raw;
				while (cnt < 63 && (cur = strstr(cur, "<<<XC_ML_ENTRY ")) != NULL) {
					cur += 15;
					char *he = strstr(cur, ">>>\n");
					if (!he) break;
					*he = '\0';
					char *bs = he + 4;
					char *be = strstr(bs, "\n<<<XC_ML_END>>>");
					if (!be) break;
					*be = '\0';
					if (strcmp(bs, script) != 0) {
						hdr[cnt] = strdup(cur);
						bdy[cnt] = strdup(bs);
						cnt++;
					}
					cur = be + 16;
				}
				free(raw);
			}
		}
		fclose(rf);
	}

	time_t now = time(NULL);
	struct tm *tm_i = localtime(&now);
	char ts[32] = "";
	if (tm_i) strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M", tm_i);

	FILE *wf = fopen(p, "wb");
	if (wf) {
		if (ts[0]) fprintf(wf, "<<<XC_ML_ENTRY [%s] %s>>>\n%s\n<<<XC_ML_END>>>\n", ts, dir ? dir : ".", script);
		else fprintf(wf, "<<<XC_ML_ENTRY %s>>>\n%s\n<<<XC_ML_END>>>\n", dir ? dir : ".", script);
		int limit = (cnt > 49) ? 49 : cnt;
		for (int i = 0; i < limit; i++) {
			fprintf(wf, "<<<XC_ML_ENTRY %s>>>\n%s\n<<<XC_ML_END>>>\n", hdr[i], bdy[i]);
		}
		fclose(wf);
	}
	for (int i = 0; i < cnt; i++) { free(hdr[i]); free(bdy[i]); }
}

static void ml_history_save_all(const char *path, char **hdr, char **bdy, int cnt) {
	FILE *wf = fopen(path, "wb");
	if (!wf) return;
	for (int i = 0; i < cnt; i++) {
		fprintf(wf, "<<<XC_ML_ENTRY %s>>>\n%s\n<<<XC_ML_END>>>\n", hdr[i], bdy[i]);
	}
	fclose(wf);
}

int ui_cmd_history(CmdHistoryEntry *hist, int *hist_count, char *out_cmd, size_t out_size, char *out_dir, size_t dir_size) {
	if (!hist || !hist_count || *hist_count <= 0) {
		ui_message("Command History", "Command history is empty");
		return 0;
	}

	int h, w;
	getmaxyx(stdscr, h, w);
	int bw = (w - 6 > 96) ? 96 : (w - 6);
	if (bw < 58) bw = w - 2;
	int bh = (h - 4 > 24) ? 24 : (h - 4);
	if (bh < 12) bh = h - 2;
	int sy = (h - bh) / 2, sx = (w - bw) / 2;

	curs_set(0);
	WINDOW *win = newwin(bh, bw, sy, sx);
	wbkgd(win, COLOR_PAIR(5));

	const char *btns[5] = {"[ Run ]", "[ Insert ]", "[ Jump ]", "[ Delete ]", "[ Cancel ]"};
	int total_bw = 7 + 1 + 10 + 1 + 8 + 1 + 10 + 1 + 10;

	int *map_idx = malloc((size_t)(*hist_count) * sizeof(int));
	if (!map_idx) { delwin(win); return 0; }

	char filter[128] = "";
	int flen = 0;
	int sel = 0, top = 0, btn = 0, res = 0, mcnt = 0;

	while (1) {
		mcnt = 0;
		for (int i = *hist_count - 1; i >= 0; i--) {
			if (flen == 0 ||
			    strcasestr(hist[i].cmd, filter) != NULL ||
			    strcasestr(hist[i].dir, filter) != NULL ||
			    strcasestr(hist[i].timestamp, filter) != NULL) {
				map_idx[mcnt++] = i;
			}
		}

		int lh = bh - 8;
		if (lh < 4) lh = 4;
		if (sel >= mcnt) sel = (mcnt > 0) ? (mcnt - 1) : 0;
		if (sel < 0) sel = 0;
		if (sel < top) top = sel;
		if (sel >= top + lh) top = sel - lh + 1;

		werase(win);
		draw_win_box(win);
		if (flen > 0) {
			mvwprintw(win, 0, 2, " Command History (Alt+F8) [%d/%d] Filter: %s ", mcnt, *hist_count, filter);
		} else {
			mvwprintw(win, 0, 2, " Command History (Alt+F8) [%d] (type to filter) ", *hist_count);
		}

		if (mcnt == 0) {
			mvwprint_utf8(win, 2, 3, bw - 6, "No matching commands in history");
		} else {
			for (int i = 0; i < lh && (top + i) < mcnt; i++) {
				int ridx = map_idx[top + i];
				char row_buf[MAX_CMD + 64];
				if (hist[ridx].timestamp[0] != '\0') {
					snprintf(row_buf, sizeof(row_buf), "[%s] %s", hist[ridx].timestamp, hist[ridx].cmd);
				} else {
					snprintf(row_buf, sizeof(row_buf), "%s", hist[ridx].cmd);
				}
				wattron(win, (top + i) == sel ? COLOR_PAIR(2) : COLOR_PAIR(5));
				mvwhline(win, 1 + i, 1, ' ', bw - 2);
				mvwprint_utf8(win, 1 + i, 2, bw - 4, row_buf);
				wattroff(win, (top + i) == sel ? COLOR_PAIR(2) : COLOR_PAIR(5));
			}
		}

		int sep_y = 1 + lh;
		mvwaddstr(win, sep_y, 0, "├");
		for (int x = 1; x < bw - 1; x++) mvwaddstr(win, sep_y, x, "─");
		mvwaddstr(win, sep_y, bw - 1, "┤");

		if (mcnt > 0) {
			const CmdHistoryEntry *cur = &hist[map_idx[sel]];
			char meta[MAX_PATH + 64];
			const char *ts = cur->timestamp[0] ? cur->timestamp : "---- -- -- --:--";
			const char *dr = cur->dir[0] ? cur->dir : ".";
			snprintf(meta, sizeof(meta), "%s  |  %s", ts, dr);
			wattron(win, A_BOLD);
			mvwprint_utf8(win, sep_y + 1, 2, bw - 4, meta);
			wattroff(win, A_BOLD);
			mvwprint_utf8(win, sep_y + 2, 2, bw - 4, cur->cmd);
		}

		int bx0 = (bw - total_bw < 2) ? 2 : (bw - total_bw) / 2;
		UiButtonRow brow = { btns, 5, bh - 2, bx0, 1, btn };
		ui_btns_draw(&brow, win);
		wrefresh(win);

		InputEvent ev = input_read();
		if (ev.action == ACT_MOUSE_LEFT) {
			int wy = ev.mouse_y - sy, wx = ev.mouse_x - sx;
			if (wy < 0 || wy >= bh || wx < 0 || wx >= bw) break;
			if (wy >= 1 && wy < 1 + lh && (top + wy - 1) < mcnt) { sel = top + wy - 1; continue; }
			int hit = ui_btns_hit_test(&brow, wy, wx);
			if (hit >= 0) {
				btn = hit;
				if (hit == 0 && mcnt > 0) { res = 1; goto done_ch; }
				if (hit == 1 && mcnt > 0) { res = 2; goto done_ch; }
				if (hit == 2 && mcnt > 0) { res = 3; goto done_ch; }
				if (hit == 3 && mcnt > 0) goto do_del_ch;
				goto done_ch;
			}
			continue;
		}
		if (ev.action == ACT_ESC) {
			if (flen > 0) { flen = 0; filter[0] = '\0'; sel = 0; top = 0; continue; }
			break;
		}
		if (ev.action == ACT_F10) break;
		if (ev.action == ACT_MOUSE_WHEEL_UP && sel > 0) sel--;
		else if (ev.action == ACT_MOUSE_WHEEL_DOWN && sel + 1 < mcnt) sel++;
		else if (ev.action == ACT_UP && sel > 0) sel--;
		else if (ev.action == ACT_DOWN && sel + 1 < mcnt) sel++;
		else if (ev.action == ACT_PGUP) { sel -= lh; if (sel < 0) sel = 0; }
		else if (ev.action == ACT_PGDN) { sel += lh; if (sel >= mcnt) sel = mcnt - 1; }
		else if (ev.action == ACT_LEFT) btn = (btn + 4) % 5;
		else if (ev.action == ACT_RIGHT || ev.action == ACT_TAB) btn = (btn + 1) % 5;
		else if (ev.action == ACT_F4 && mcnt > 0) { res = 2; break; }
		else if (ev.action == ACT_INSERT_NAME && mcnt > 0) { res = 3; break; }
		else if ((ev.action == ACT_F8 || ev.action == ACT_DELETE) && mcnt > 0) goto do_del_ch;
		else if (ev.action == ACT_BACKSPACE && flen > 0) {
			while (flen > 0 && ((unsigned char)filter[flen - 1] & 0xC0) == 0x80) flen--;
			if (flen > 0) flen--;
			filter[flen] = '\0';
			sel = 0; top = 0;
		} else if (ev.action == ACT_CHAR && iswprint(ev.ch) && !(ev.ch == L' ' && btn != 0 && flen == 0)) {
			char mb[8];
			int bl = wctomb(mb, ev.ch);
			if (bl > 0 && flen + bl < (int)sizeof(filter) - 1) {
				memcpy(filter + flen, mb, (size_t)bl);
				flen += bl;
				filter[flen] = '\0';
				sel = 0; top = 0;
			}
		} else if (ev.action == ACT_ENTER) {
			if (mcnt == 0 || btn == 4) break;
			if (btn == 0) { res = 1; break; }
			if (btn == 1) { res = 2; break; }
			if (btn == 2) { res = 3; break; }
			if (btn == 3) goto do_del_ch;
		}
		continue;

do_del_ch:
		if (mcnt > 0) {
			int del_i = map_idx[sel];
			for (int k = del_i; k + 1 < *hist_count; k++) {
				hist[k] = hist[k + 1];
			}
			(*hist_count)--;
			if (*hist_count == 0) break;
		}
	}

done_ch:
	if (res > 0 && mcnt > 0) {
		const CmdHistoryEntry *chosen = &hist[map_idx[sel]];
		if (out_cmd && out_size > 0) snprintf(out_cmd, out_size, "%s", chosen->cmd);
		if (out_dir && dir_size > 0) snprintf(out_dir, dir_size, "%s", chosen->dir);
	}
	delwin(win);
	free(map_idx);
	touchwin(stdscr);
	return res;
}

int ui_dir_history(DirHistoryEntry *hist, int *hist_count, char *out_dir, size_t dir_size) {
	if (!hist || !hist_count || *hist_count <= 0) {
		ui_message("Folder History", "Folder history is empty");
		return 0;
	}

	int h, w;
	getmaxyx(stdscr, h, w);
	int bw = (w - 6 > 96) ? 96 : (w - 6);
	if (bw < 58) bw = w - 2;
	int bh = (h - 4 > 24) ? 24 : (h - 4);
	if (bh < 12) bh = h - 2;
	int sy = (h - bh) / 2, sx = (w - bw) / 2;

	curs_set(0);
	WINDOW *win = newwin(bh, bw, sy, sx);
	wbkgd(win, COLOR_PAIR(5));

	const char *btns[5] = {"[ Go ]", "[ Other ]", "[ Insert ]", "[ Delete ]", "[ Cancel ]"};
	int total_bw = 6 + 1 + 9 + 1 + 10 + 1 + 10 + 1 + 10;

	int *map_idx = malloc((size_t)(*hist_count) * sizeof(int));
	if (!map_idx) { delwin(win); return 0; }

	char filter[128] = "";
	int flen = 0;
	int sel = 0, top = 0, btn = 0, res = 0, mcnt = 0;

	while (1) {
		mcnt = 0;
		for (int i = *hist_count - 1; i >= 0; i--) {
			if (flen == 0 || strcasestr(hist[i].dir, filter) != NULL) {
				map_idx[mcnt++] = i;
			}
		}

		int lh = bh - 7;
		if (lh < 4) lh = 4;
		if (sel >= mcnt) sel = (mcnt > 0) ? (mcnt - 1) : 0;
		if (sel < 0) sel = 0;
		if (sel < top) top = sel;
		if (sel >= top + lh) top = sel - lh + 1;

		werase(win);
		draw_win_box(win);
		if (flen > 0) {
			mvwprintw(win, 0, 2, " Folder History (Alt+F12 / Ctrl+D) [%d/%d] Filter: %s ", mcnt, *hist_count, filter);
		} else {
			mvwprintw(win, 0, 2, " Folder History (Alt+F12 / Ctrl+D) [%d] (type to filter) ", *hist_count);
		}

		if (mcnt == 0) {
			mvwprint_utf8(win, 2, 3, bw - 6, "No matching folders in history");
		} else {
			for (int i = 0; i < lh && (top + i) < mcnt; i++) {
				int ridx = map_idx[top + i];
				char row_buf[MAX_PATH + 64];
				if (hist[ridx].timestamp[0] != '\0') {
					snprintf(row_buf, sizeof(row_buf), "[%s] %s", hist[ridx].timestamp, hist[ridx].dir);
				} else {
					snprintf(row_buf, sizeof(row_buf), "%s", hist[ridx].dir);
				}
				wattron(win, (top + i) == sel ? COLOR_PAIR(2) : COLOR_PAIR(5));
				mvwhline(win, 1 + i, 1, ' ', bw - 2);
				mvwprint_utf8(win, 1 + i, 2, bw - 4, row_buf);
				wattroff(win, (top + i) == sel ? COLOR_PAIR(2) : COLOR_PAIR(5));
			}
		}

		int sep_y = 1 + lh;
		mvwaddstr(win, sep_y, 0, "├");
		for (int x = 1; x < bw - 1; x++) mvwaddstr(win, sep_y, x, "─");
		mvwaddstr(win, sep_y, bw - 1, "┤");

		if (mcnt > 0) {
			const DirHistoryEntry *cur = &hist[map_idx[sel]];
			wattron(win, A_BOLD);
			mvwprint_utf8(win, sep_y + 1, 2, bw - 4, cur->dir);
			wattroff(win, A_BOLD);
		}

		int bx0 = (bw - total_bw < 2) ? 2 : (bw - total_bw) / 2;
		UiButtonRow brow = { btns, 5, bh - 2, bx0, 1, btn };
		ui_btns_draw(&brow, win);
		wrefresh(win);

		InputEvent ev = input_read();
		if (ev.action == ACT_MOUSE_LEFT) {
			int wy = ev.mouse_y - sy, wx = ev.mouse_x - sx;
			if (wy < 0 || wy >= bh || wx < 0 || wx >= bw) break;
			if (wy >= 1 && wy < 1 + lh && (top + wy - 1) < mcnt) { sel = top + wy - 1; continue; }
			int hit = ui_btns_hit_test(&brow, wy, wx);
			if (hit >= 0) {
				btn = hit;
				if (hit == 0 && mcnt > 0) { res = 1; goto done_dh; }
				if (hit == 1 && mcnt > 0) { res = 2; goto done_dh; }
				if (hit == 2 && mcnt > 0) { res = 3; goto done_dh; }
				if (hit == 3 && mcnt > 0) goto do_del_dh;
				goto done_dh;
			}
			continue;
		}
		if (ev.action == ACT_ESC) {
			if (flen > 0) { flen = 0; filter[0] = '\0'; sel = 0; top = 0; continue; }
			break;
		}
		if (ev.action == ACT_F10) break;
		if (ev.action == ACT_MOUSE_WHEEL_UP && sel > 0) sel--;
		else if (ev.action == ACT_MOUSE_WHEEL_DOWN && sel + 1 < mcnt) sel++;
		else if (ev.action == ACT_UP && sel > 0) sel--;
		else if (ev.action == ACT_DOWN && sel + 1 < mcnt) sel++;
		else if (ev.action == ACT_PGUP) { sel -= lh; if (sel < 0) sel = 0; }
		else if (ev.action == ACT_PGDN) { sel += lh; if (sel >= mcnt) sel = mcnt - 1; }
		else if (ev.action == ACT_LEFT) btn = (btn + 4) % 5;
		else if (ev.action == ACT_RIGHT || ev.action == ACT_TAB) btn = (btn + 1) % 5;
		else if (ev.action == ACT_F4 && mcnt > 0) { res = 2; break; }
		else if (ev.action == ACT_INSERT_NAME && mcnt > 0) { res = 3; break; }
		else if ((ev.action == ACT_F8 || ev.action == ACT_DELETE) && mcnt > 0) goto do_del_dh;
		else if (ev.action == ACT_BACKSPACE && flen > 0) {
			while (flen > 0 && ((unsigned char)filter[flen - 1] & 0xC0) == 0x80) flen--;
			if (flen > 0) flen--;
			filter[flen] = '\0';
			sel = 0; top = 0;
		} else if (ev.action == ACT_CHAR && iswprint(ev.ch) && !(ev.ch == L' ' && btn != 0 && flen == 0)) {
			char mb[8];
			int bl = wctomb(mb, ev.ch);
			if (bl > 0 && flen + bl < (int)sizeof(filter) - 1) {
				memcpy(filter + flen, mb, (size_t)bl);
				flen += bl;
				filter[flen] = '\0';
				sel = 0; top = 0;
			}
		} else if (ev.action == ACT_ENTER) {
			if (mcnt == 0 || btn == 4) break;
			if (btn == 0) { res = 1; break; }
			if (btn == 1) { res = 2; break; }
			if (btn == 2) { res = 3; break; }
			if (btn == 3) goto do_del_dh;
		}
		continue;

do_del_dh:
		if (mcnt > 0) {
			int del_i = map_idx[sel];
			for (int k = del_i; k + 1 < *hist_count; k++) {
				hist[k] = hist[k + 1];
			}
			(*hist_count)--;
			if (*hist_count == 0) break;
		}
	}

done_dh:
	if (res > 0 && mcnt > 0 && out_dir && dir_size > 0) {
		snprintf(out_dir, dir_size, "%s", hist[map_idx[sel]].dir);
	}
	delwin(win);
	free(map_idx);
	touchwin(stdscr);
	return res;
}

int ui_file_history(FileHistoryEntry *hist, int *hist_count, char *out_path, size_t path_size, int *out_line, int *out_col) {
	if (!hist || !hist_count || *hist_count <= 0) {
		ui_message("File History", "File history is empty");
		return 0;
	}

	int h, w;
	getmaxyx(stdscr, h, w);
	int bw = (w - 6 > 96) ? 96 : (w - 6);
	if (bw < 58) bw = w - 2;
	int bh = (h - 4 > 24) ? 24 : (h - 4);
	if (bh < 12) bh = h - 2;
	int sy = (h - bh) / 2, sx = (w - bw) / 2;

	curs_set(0);
	WINDOW *win = newwin(bh, bw, sy, sx);
	wbkgd(win, COLOR_PAIR(5));

	const char *btns[5] = {"[ Edit ]", "[ View ]", "[ Jump ]", "[ Delete ]", "[ Cancel ]"};
	int total_bw = 8 + 1 + 8 + 1 + 8 + 1 + 10 + 1 + 10;

	int *map_idx = malloc((size_t)(*hist_count) * sizeof(int));
	if (!map_idx) { delwin(win); return 0; }

	char filter[128] = "";
	int flen = 0;
	int sel = 0, top = 0, btn = 0, res = 0, mcnt = 0;

	while (1) {
		mcnt = 0;
		for (int i = *hist_count - 1; i >= 0; i--) {
			if (flen == 0 || strcasestr(hist[i].path, filter) != NULL) {
				map_idx[mcnt++] = i;
			}
		}

		int lh = bh - 8;
		if (lh < 4) lh = 4;
		if (sel >= mcnt) sel = (mcnt > 0) ? (mcnt - 1) : 0;
		if (sel < 0) sel = 0;
		if (sel < top) top = sel;
		if (sel >= top + lh) top = sel - lh + 1;

		werase(win);
		draw_win_box(win);
		if (flen > 0) {
			mvwprintw(win, 0, 2, " File History (Alt+F11) [%d/%d] Filter: %s ", mcnt, *hist_count, filter);
		} else {
			mvwprintw(win, 0, 2, " File History (Alt+F11) [%d] (type to filter) ", *hist_count);
		}

		if (mcnt == 0) {
			mvwprint_utf8(win, 2, 3, bw - 6, "No matching files in history");
		} else {
			for (int i = 0; i < lh && (top + i) < mcnt; i++) {
				int ridx = map_idx[top + i];
				const FileHistoryEntry *fe = &hist[ridx];
				char row_buf[MAX_PATH + 64];
				const char *md = fe->mode[0] ? fe->mode : "Edit";
				char tag[32];
				if (strcmp(md, "Edit") == 0 && (fe->line > 1 || fe->col > 1)) {
					snprintf(tag, sizeof(tag), "Edit %d:%d", fe->line > 0 ? fe->line : 1, fe->col > 0 ? fe->col : 1);
				} else {
					snprintf(tag, sizeof(tag), "%s", md);
				}
				if (fe->timestamp[0] != '\0') {
					snprintf(row_buf, sizeof(row_buf), "[%s] [%s] %s", fe->timestamp, tag, fe->path);
				} else {
					snprintf(row_buf, sizeof(row_buf), "[%s] %s", tag, fe->path);
				}
				wattron(win, (top + i) == sel ? COLOR_PAIR(2) : COLOR_PAIR(5));
				mvwhline(win, 1 + i, 1, ' ', bw - 2);
				mvwprint_utf8(win, 1 + i, 2, bw - 4, row_buf);
				wattroff(win, (top + i) == sel ? COLOR_PAIR(2) : COLOR_PAIR(5));
			}
		}

		int sep_y = 1 + lh;
		mvwaddstr(win, sep_y, 0, "├");
		for (int x = 1; x < bw - 1; x++) mvwaddstr(win, sep_y, x, "─");
		mvwaddstr(win, sep_y, bw - 1, "┤");

		if (mcnt > 0) {
			const FileHistoryEntry *cur = &hist[map_idx[sel]];
			char meta[96];
			snprintf(meta, sizeof(meta), "%s  |  Mode: %s  |  Cursor: %d:%d",
			         cur->timestamp[0] ? cur->timestamp : "---- -- -- --:--",
			         cur->mode[0] ? cur->mode : "Edit",
			         cur->line > 0 ? cur->line : 1,
			         cur->col > 0 ? cur->col : 1);
			wattron(win, A_BOLD);
			mvwprint_utf8(win, sep_y + 1, 2, bw - 4, meta);
			wattroff(win, A_BOLD);
			mvwprint_utf8(win, sep_y + 2, 2, bw - 4, cur->path);
		}

		int bx0 = (bw - total_bw < 2) ? 2 : (bw - total_bw) / 2;
		UiButtonRow brow = { btns, 5, bh - 2, bx0, 1, btn };
		ui_btns_draw(&brow, win);
		wrefresh(win);

		InputEvent ev = input_read();
		if (ev.action == ACT_MOUSE_LEFT) {
			int wy = ev.mouse_y - sy, wx = ev.mouse_x - sx;
			if (wy < 0 || wy >= bh || wx < 0 || wx >= bw) break;
			if (wy >= 1 && wy < 1 + lh && (top + wy - 1) < mcnt) { sel = top + wy - 1; continue; }
			int hit = ui_btns_hit_test(&brow, wy, wx);
			if (hit >= 0) {
				btn = hit;
				if (hit == 0 && mcnt > 0) { res = 1; goto done_fh; }
				if (hit == 1 && mcnt > 0) { res = 2; goto done_fh; }
				if (hit == 2 && mcnt > 0) { res = 3; goto done_fh; }
				if (hit == 3 && mcnt > 0) goto do_del_fh;
				goto done_fh;
			}
			continue;
		}
		if (ev.action == ACT_ESC) {
			if (flen > 0) { flen = 0; filter[0] = '\0'; sel = 0; top = 0; continue; }
			break;
		}
		if (ev.action == ACT_F10) break;
		if (ev.action == ACT_MOUSE_WHEEL_UP && sel > 0) sel--;
		else if (ev.action == ACT_MOUSE_WHEEL_DOWN && sel + 1 < mcnt) sel++;
		else if (ev.action == ACT_UP && sel > 0) sel--;
		else if (ev.action == ACT_DOWN && sel + 1 < mcnt) sel++;
		else if (ev.action == ACT_PGUP) { sel -= lh; if (sel < 0) sel = 0; }
		else if (ev.action == ACT_PGDN) { sel += lh; if (sel >= mcnt) sel = mcnt - 1; }
		else if (ev.action == ACT_LEFT) btn = (btn + 4) % 5;
		else if (ev.action == ACT_RIGHT || ev.action == ACT_TAB) btn = (btn + 1) % 5;
		else if (ev.action == ACT_F4 && mcnt > 0) { res = 1; break; }
		else if (ev.action == ACT_F3 && mcnt > 0) { res = 2; break; }
		else if (ev.action == ACT_INSERT_NAME && mcnt > 0) { res = 3; break; }
		else if ((ev.action == ACT_F8 || ev.action == ACT_DELETE) && mcnt > 0) goto do_del_fh;
		else if (ev.action == ACT_BACKSPACE && flen > 0) {
			while (flen > 0 && ((unsigned char)filter[flen - 1] & 0xC0) == 0x80) flen--;
			if (flen > 0) flen--;
			filter[flen] = '\0';
			sel = 0; top = 0;
		} else if (ev.action == ACT_CHAR && iswprint(ev.ch) && !(ev.ch == L' ' && btn != 0 && flen == 0)) {
			char mb[8];
			int bl = wctomb(mb, ev.ch);
			if (bl > 0 && flen + bl < (int)sizeof(filter) - 1) {
				memcpy(filter + flen, mb, (size_t)bl);
				flen += bl;
				filter[flen] = '\0';
				sel = 0; top = 0;
			}
		} else if (ev.action == ACT_ENTER) {
			if (mcnt == 0 || btn == 4) break;
			if (btn == 0) { res = 1; break; }
			if (btn == 1) { res = 2; break; }
			if (btn == 2) { res = 3; break; }
			if (btn == 3) goto do_del_fh;
		}
		continue;

do_del_fh:
		if (mcnt > 0) {
			int del_i = map_idx[sel];
			for (int k = del_i; k + 1 < *hist_count; k++) {
				hist[k] = hist[k + 1];
			}
			(*hist_count)--;
			if (*hist_count == 0) break;
		}
	}

done_fh:
	if (res > 0 && mcnt > 0) {
		const FileHistoryEntry *chosen = &hist[map_idx[sel]];
		if (out_path && path_size > 0) snprintf(out_path, path_size, "%s", chosen->path);
		if (out_line) *out_line = (chosen->line > 0) ? chosen->line : 1;
		if (out_col) *out_col = (chosen->col > 0) ? chosen->col : 1;
	}
	delwin(win);
	free(map_idx);
	touchwin(stdscr);
	return res;
}

int ui_multiline_history(char *out_script, size_t out_size) {
	char p[MAX_PATH + 48];
	get_ml_path(p, sizeof(p));
	FILE *f = fopen(p, "rb");
	if (!f) { ui_message("History", "Multiline history is empty"); return 0; }
	fseek(f, 0, SEEK_END);
	long s = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (s <= 0) { fclose(f); ui_message("History", "Multiline history is empty"); return 0; }

	char *raw = malloc((size_t)s + 1);
	if (!raw) { fclose(f); return 0; }
	size_t n = fread(raw, 1, (size_t)s, f);
	fclose(f);
	raw[n] = '\0';

	char *hdr[64] = {0}, *bdy[64] = {0};
	int cnt = 0;
	char *cur = raw;
	while (cnt < 64 && (cur = strstr(cur, "<<<XC_ML_ENTRY ")) != NULL) {
		cur += 15;
		char *he = strstr(cur, ">>>\n");
		if (!he) break;
		*he = '\0';
		char *bs = he + 4;
		char *be = strstr(bs, "\n<<<XC_ML_END>>>");
		if (!be) break;
		*be = '\0';
		hdr[cnt] = strdup(cur);
		bdy[cnt] = strdup(bs);
		cnt++;
		cur = be + 16;
	}
	free(raw);
	if (cnt == 0) { ui_message("History", "Multiline history is empty"); return 0; }

	int h, w;
	getmaxyx(stdscr, h, w);
	int bw = (w - 6 > 88) ? 88 : (w - 6);
	if (bw < 50) bw = w - 2;
	int bh = (h - 4 > 26) ? 26 : (h - 4);
	if (bh < 12) bh = h - 2;
	int sy = (h - bh) / 2, sx = (w - bw) / 2;

	curs_set(0);
	WINDOW *win = newwin(bh, bw, sy, sx);
	wbkgd(win, COLOR_PAIR(5));

	const char *btns[4] = {"[ Run ]", "[ Edit ]", "[ Delete ]", "[ Cancel ]"};
	int total_bw = 7 + 1 + 8 + 1 + 10 + 1 + 10;

	int sel = 0, top = 0, btn = 0, chs = -1;
	while (1) {
		int lh = (bh - 5) / 2;
		if (lh < 3) lh = 3;
		if (sel >= cnt) sel = cnt - 1;
		if (sel < 0) sel = 0;
		if (sel < top) top = sel;
		if (sel >= top + lh) top = sel - lh + 1;

		werase(win);
		draw_win_box(win);
		mvwprintw(win, 0, 2, " Multiline History (Alt+H) [%d/%d] ", sel + 1, cnt);

		for (int i = 0; i < lh && (top + i) < cnt; i++) {
			int idx = top + i;
			wattron(win, idx == sel ? COLOR_PAIR(2) : COLOR_PAIR(5));
			mvwhline(win, 1 + i, 1, ' ', bw - 2);
			mvwprint_utf8(win, 1 + i, 2, bw - 4, hdr[idx]);
			wattroff(win, idx == sel ? COLOR_PAIR(2) : COLOR_PAIR(5));
		}

		int sep_y = 1 + lh;
		mvwaddstr(win, sep_y, 0, "├");
		for (int x = 1; x < bw - 1; x++) mvwaddstr(win, sep_y, x, "─");
		mvwaddstr(win, sep_y, bw - 1, "┤");

		int prev_rows = bh - sep_y - 3;
		const char *bp = bdy[sel];
		for (int r = 0; r < prev_rows && bp && *bp; r++) {
			const char *nl = strchr(bp, '\n');
			int len = nl ? (int)(nl - bp) : (int)strlen(bp);
			char line[512];
			if (len >= (int)sizeof(line)) len = (int)sizeof(line) - 1;
			memcpy(line, bp, (size_t)len);
			line[len] = '\0';
			mvwprint_utf8(win, sep_y + 1 + r, 2, bw - 4, line);
			if (!nl) break;
			bp = nl + 1;
		}

		int bx0 = (bw - total_bw < 2) ? 2 : (bw - total_bw) / 2;
		UiButtonRow brow = { btns, 4, bh - 2, bx0, 1, btn };
		ui_btns_draw(&brow, win);
		wrefresh(win);

		InputEvent ev = input_read();
		if (ev.action == ACT_MOUSE_LEFT) {
			int wy = ev.mouse_y - sy, wx = ev.mouse_x - sx;
			if (wy < 0 || wy >= bh || wx < 0 || wx >= bw) break;
			if (wy >= 1 && wy < 1 + lh && (top + wy - 1) < cnt) { sel = top + wy - 1; continue; }
			int hit = ui_btns_hit_test(&brow, wy, wx);
			if (hit >= 0) {
				btn = hit;
				if (hit == 0) { chs = sel; goto done_ml; }
				if (hit == 1) goto do_edit_ml;
				if (hit == 2) goto do_del_ml;
				goto done_ml;
			}
			continue;
		}
		if (ev.action == ACT_ESC || ev.action == ACT_F10) break;
		if (ev.action == ACT_MOUSE_WHEEL_UP && sel > 0) sel--;
		else if (ev.action == ACT_MOUSE_WHEEL_DOWN && sel + 1 < cnt) sel++;
		else if (ev.action == ACT_UP && sel > 0) sel--;
		else if (ev.action == ACT_DOWN && sel + 1 < cnt) sel++;
		else if (ev.action == ACT_PGUP) { sel -= lh; if (sel < 0) sel = 0; }
		else if (ev.action == ACT_PGDN) { sel += lh; if (sel >= cnt) sel = cnt - 1; }
		else if (ev.action == ACT_LEFT) btn = (btn + 3) % 4;
		else if (ev.action == ACT_RIGHT || ev.action == ACT_TAB) btn = (btn + 1) % 4;
		else if (ev.action == ACT_F8 || ev.action == ACT_DELETE) goto do_del_ml;
		else if (ev.action == ACT_F4) goto do_edit_ml;
		else if (ev.action == ACT_ENTER || (ev.action == ACT_CHAR && ev.ch == L' ')) {
			if (btn == 0) { chs = sel; break; }
			if (btn == 1) goto do_edit_ml;
			if (btn == 2) goto do_del_ml;
			break;
		}
		continue;

do_del_ml:
		if (ui_confirm("Delete Entry", "Remove selected script from history?")) {
			free(hdr[sel]); free(bdy[sel]);
			for (int k = sel; k + 1 < cnt; k++) { hdr[k] = hdr[k + 1]; bdy[k] = bdy[k + 1]; }
			cnt--;
			ml_history_save_all(p, hdr, bdy, cnt);
			if (cnt == 0) break;
		}
		continue;

do_edit_ml:
		{
			char tmp_ed[128];
			snprintf(tmp_ed, sizeof(tmp_ed), "/tmp/xc_ml_edit_%d.sh", (int)getpid());
			FILE *tf = fopen(tmp_ed, "wb");
			if (tf) {
				fwrite(bdy[sel], 1, strlen(bdy[sel]), tf);
				fclose(tf);
				ui_edit_file(tmp_ed);
				FILE *rf2 = fopen(tmp_ed, "rb");
				if (rf2) {
					fseek(rf2, 0, SEEK_END);
					long nsz = ftell(rf2);
					fseek(rf2, 0, SEEK_SET);
					if (nsz > 0 && nsz < 65536) {
						char *nb = malloc((size_t)nsz + 1);
						if (nb) {
							size_t rr = fread(nb, 1, (size_t)nsz, rf2);
							nb[rr] = '\0';
							free(bdy[sel]);
							bdy[sel] = nb;
							ml_history_save_all(p, hdr, bdy, cnt);
						}
					}
					fclose(rf2);
				}
				unlink(tmp_ed);
				clearok(stdscr, TRUE); draw_screen(); refresh();
			}
		}
	}

done_ml:
	if (chs >= 0) snprintf(out_script, out_size, "%s", bdy[chs]);
	for (int i = 0; i < cnt; i++) { free(hdr[i]); free(bdy[i]); }
	delwin(win);
	touchwin(stdscr);
	return chs >= 0;
}

int ui_confirm_cancel(const char *title, const char *msg) {
	int height, width;
	getmaxyx(stdscr, height, width);
	int msg_cols = (int)mbstowcs(NULL, msg, 0);
	if (msg_cols < 0) msg_cols = (int)strlen(msg);
	int box_w = msg_cols + 6;
	if (box_w < 36) box_w = 36;
	if (box_w > width - 4) box_w = width - 4;
	int box_h = 5;
	int start_y = (height - box_h) / 2, start_x = (width - box_w) / 2;

	curs_set(0);
	WINDOW *win = newwin(box_h, box_w, start_y, start_x);
	wbkgd(win, g_ui_theme.win_bg);

	const char *btns[3] = { "[ Yes ]", "[ No ]", "[ Cancel ]" };
	int bx0 = (box_w - 27 < 2) ? 2 : (box_w - 27) / 2;
	UiButtonRow brow = { btns, 3, 3, bx0, 2, 0 };

	int sel = 0; /* 0 = Yes, 1 = No, 2 = Cancel */
	int res = -1;

	while (1) {
		draw_win_box(win);
		mvwprintw(win, 0, 2, " %s ", title);
		int msg_x = (box_w - msg_cols < 4) ? 2 : (box_w - msg_cols) / 2;
		mvwprint_utf8(win, 1, msg_x, box_w - 4, msg);

		brow.focus_idx = sel;
		ui_btns_draw(&brow, win);
		wrefresh(win);

		InputEvent ev = input_read();
		if (ev.action == ACT_ESC || ev.action == ACT_F10) { res = -1; break; }
		if (ev.action == ACT_LEFT || ev.action == ACT_SHIFT_TAB) sel = (sel + 2) % 3;
		else if (ev.action == ACT_RIGHT || ev.action == ACT_TAB) sel = (sel + 1) % 3;
		else if (ev.action == ACT_MOUSE_LEFT) {
			int wy = ev.mouse_y - start_y, wx = ev.mouse_x - start_x;
			if (wy < 0 || wy >= box_h || wx < 0 || wx >= box_w) { res = -1; break; }
			int hit = ui_btns_hit_test(&brow, wy, wx);
			if (hit >= 0) { res = (hit == 0) ? 1 : (hit == 1 ? 0 : -1); break; }
		} else if (ev.action == ACT_ENTER || (ev.action == ACT_CHAR && ev.ch == L' ')) {
			res = (sel == 0) ? 1 : (sel == 1 ? 0 : -1);
			break;
		} else if (ev.action == ACT_CHAR) {
			wchar_t ch = towlower(ev.ch);
			if (ch == L'y' || ch == L'н') { res = 1; break; }
			if (ch == L'n' || ch == L'т') { res = 0; break; }
			if (ch == L'c' || ch == L'с') { res = -1; break; }
		}
	}
	delwin(win);
	touchwin(stdscr);
	return res;
}

static void get_git_branch(const char *dir, char *out, size_t out_size) {
	out[0] = '\0';
	char cur[MAX_PATH];
	snprintf(cur, sizeof(cur), "%s", dir);
	while (1) {
		char head[MAX_PATH + 16];
		snprintf(head, sizeof(head), "%s/.git/HEAD", cur);
		FILE *f = fopen(head, "r");
		if (f) {
			char line[256] = {0};
			if (fgets(line, sizeof(line), f)) {
				size_t l = strlen(line);
				while (l > 0 && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = '\0';
				const char *ref = strstr(line, "ref: refs/heads/");
				if (ref) snprintf(out, out_size, " (%s)", ref + 16);
				else if (l >= 7) snprintf(out, out_size, " (%.7s)", line);
			}
			fclose(f);
			return;
		}
		char *sl = strrchr(cur, '/');
		if (!sl || sl == cur) break;
		*sl = '\0';
	}
}

static void load_prompt_template(char *tmpl, size_t tmpl_size) {
	const char *home = getenv("HOME");
	if (!home || !*home) home = "/tmp";
	char dir[MAX_PATH], path[MAX_PATH + 16];
	snprintf(dir, sizeof(dir), "%s/.config", home);
	mkdir(dir, 0755);
	snprintf(dir, sizeof(dir), "%s/.config/xcommander", home);
	mkdir(dir, 0755);
	snprintf(path, sizeof(path), "%s/prompt", dir);

	FILE *f = fopen(path, "r");
	if (!f) {
		f = fopen(path, "w");
		if (f) {
			fprintf(f, "# xCommander Terminal Prompt (Ctrl+O)\n");
			fprintf(f, "# Macros: \\u=user, \\h=host, \\w=path(~), \\W=dir, \\g=git, \\$=$/#, \\e=ESC\n");
			fprintf(f, "\\e[1;32m\\u@\\h\\e[0m:\\e[1;34m\\w\\e[1;33m\\g\\e[0m\\$ \n");
			fclose(f);
		}
		snprintf(tmpl, tmpl_size, "\\e[1;32m\\u@\\h\\e[0m:\\e[1;34m\\w\\e[1;33m\\g\\e[0m\\$ ");
		return;
	}
	char line[512];
	while (fgets(line, sizeof(line), f)) {
		size_t l = strlen(line);
		while (l > 0 && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = '\0';
		if (line[0] == '\0' || line[0] == '#') continue;
		snprintf(tmpl, tmpl_size, "%s", line);
		fclose(f);
		return;
	}
	fclose(f);
	snprintf(tmpl, tmpl_size, "\\e[1;32m\\u@\\h\\e[0m:\\e[1;34m\\w\\e[1;33m\\g\\e[0m\\$ ");
}

static void format_custom_prompt(const char *path, char *out, size_t out_size) {
	char tmpl[512];
	load_prompt_template(tmpl, sizeof(tmpl));

	const char *user = getenv("USER");
	if (!user || !*user) user = "user";

	char host[128] = "linux";
	if (gethostname(host, sizeof(host) - 1) == 0) {
		char *dot = strchr(host, '.');
		if (dot) *dot = '\0';
	}

	char short_w[MAX_PATH];
	const char *home = getenv("HOME");
	if (home && *home && strncmp(path, home, strlen(home)) == 0 &&
	    (path[strlen(home)] == '/' || path[strlen(home)] == '\0')) {
		snprintf(short_w, sizeof(short_w), "~%s", path + strlen(home));
	} else {
		snprintf(short_w, sizeof(short_w), "%s", path);
	}

	const char *base_w = strrchr(short_w, '/');
	base_w = (base_w && *(base_w + 1)) ? (base_w + 1) : short_w;

	char git_br[128];
	get_git_branch(path, git_br, sizeof(git_br));

	size_t j = 0;
	for (size_t i = 0; tmpl[i] != '\0' && j + 1 < out_size; i++) {
		if (tmpl[i] == '\\' && tmpl[i + 1] != '\0') {
			char next = tmpl[i + 1];
			if (next == 'e') { out[j++] = '\033'; i++; continue; }
			if (next == 'n') { out[j++] = '\n'; i++; continue; }
			const char *rep = NULL;
			if (next == 'u') rep = user;
			else if (next == 'h') rep = host;
			else if (next == 'w') rep = short_w;
			else if (next == 'W') rep = base_w;
			else if (next == 'g') rep = git_br;
			else if (next == '$') rep = (geteuid() == 0) ? "#" : "$";
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


static int visible_cols_ansi(const char *str) {
	wchar_t wbuf[16384];
	char clean[16384];
	size_t j = 0;
	for (size_t i = 0; str[i] != '\0' && j + 1 < sizeof(clean); i++) {
		if (str[i] == '\033' && str[i + 1] == '[') {
			i += 2;
			while (str[i] != '\0' && !(str[i] >= '@' && str[i] <= '~')) i++;
			continue;
		}
		clean[j++] = str[i];
	}
	clean[j] = '\0';
	size_t wl = mbstowcs(wbuf, clean, 16383);
	if (wl == (size_t)-1) return (int)j;
	wbuf[wl] = L'\0';
	int cols = wcswidth(wbuf, wl);
	return (cols >= 0) ? cols : (int)wl;
}

void print_term_prompt_ex(const wchar_t *wcmd, int with_header) {
	static int prev_cur_row = 0;
	if (with_header) prev_cur_row = 0;

	struct winsize ws;
	int term_cols = 80;
	if (ioctl(STDERR_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
		term_cols = ws.ws_col;
	}

	if (!with_header && prev_cur_row > 0) {
		printf("\033[%dA", prev_cur_row);
	}

	const char *path = (active_panel && *active_panel->current_path) ? active_panel->current_path : ".";
	char prompt_str[MAX_PATH + 256];
	format_custom_prompt(path, prompt_str, sizeof(prompt_str));
	char *last_nl = strrchr(prompt_str, '\n');
	const char *prompt_line = prompt_str;
	if (last_nl) {
		*last_nl = '\0';
		if (with_header) {
			const char *hdr = prompt_str;
			while (*hdr == '\n') { printf("\r\n"); hdr++; }
			printf("\r\033[K%s\r\n", hdr);
		}
		prompt_line = last_nl + 1;
	}

	printf("\r\033[J%s", prompt_line);

	char full_utf[16384] = "";
	if (wcmd) {
		wcstombs(full_utf, wcmd, sizeof(full_utf) - 1);
		full_utf[sizeof(full_utf) - 1] = '\0';
	} else {
		snprintf(full_utf, sizeof(full_utf), "%s", cmd_buffer);
	}

	if (wcmd && cmd_sel_start >= 0 && cmd_sel_end > cmd_sel_start && cmd_sel_end <= cmd_len) {
		wchar_t part[16384];
		char utf[16384];
		if (cmd_sel_start > 0) {
			wmemcpy(part, wcmd, (size_t)cmd_sel_start);
			part[cmd_sel_start] = L'\0';
			if (wcstombs(utf, part, sizeof(utf)) != (size_t)-1) printf("%s", utf);
		}
		int slen = cmd_sel_end - cmd_sel_start;
		wmemcpy(part, wcmd + cmd_sel_start, (size_t)slen);
		part[slen] = L'\0';
		if (wcstombs(utf, part, sizeof(utf)) != (size_t)-1) printf("\033[7m%s\033[27m", utf);
		if (cmd_len > cmd_sel_end) {
			int rlen = cmd_len - cmd_sel_end;
			wmemcpy(part, wcmd + cmd_sel_end, (size_t)rlen);
			part[rlen] = L'\0';
			if (wcstombs(utf, part, sizeof(utf)) != (size_t)-1) printf("%s", utf);
		}
	} else {
		printf("%s", full_utf);
	}

	int pcols = visible_cols_ansi(prompt_line);
	int tcols = visible_cols_ansi(full_utf);
	int total_cols = pcols + tcols;
	int cur_cols = pcols + (wcmd ? wcswidth(wcmd, (size_t)cmd_pos) : cmd_pos);
	if (cur_cols < pcols) cur_cols = pcols;

	int end_row = (total_cols > 0) ? ((total_cols - 1) / term_cols) : 0;
	int cur_row = cur_cols / term_cols;
	int cur_col = cur_cols % term_cols;

	if (total_cols > 0 && (total_cols % term_cols) == 0) {
		printf(" \b");
	}
	if (end_row > cur_row) {
		printf("\033[%dA", end_row - cur_row);
	}
	printf("\r");
	if (cur_col > 0) {
		printf("\033[%dC", cur_col);
	}

	prev_cur_row = cur_row;
	fflush(stdout);
}

