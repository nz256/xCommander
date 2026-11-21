#define _GNU_SOURCE
#include <time.h>
#include "cmdline.h"
#include <sys/time.h>
#include <ncurses.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <locale.h>
#include <wchar.h>
#include <wctype.h>
#include "xc.h"
#include "panel.h"
#include "archive.h"
#include "find.h"
#include "ui.h"
#include "widgets.h"
#include "exec.h"
#include "input.h"
#include "viewer.h"
#include "editor.h"

Panel left_panel, right_panel;
Panel *active_panel = NULL;

char cmd_buffer[MAX_CMD] = "";
int terminal_mode = 0;
char quick_search[MAX_FILENAME] = "";
int quick_search_active = 0;
int quick_search_nomatch = 0;
static wchar_t wsearch[MAX_FILENAME] = L"";
static int wsearch_len = 0;

static wchar_t alt_layout_char(wchar_t c) {
	int is_up = iswupper(c);
	wchar_t lc = towlower(c);
	const wchar_t *en = L"qwertyuiop[]asdfghjkl;'zxcvbnm,.`";
	const wchar_t *ru = L"йцукенгшщзхъфывапролджэячсмитьбюё";
	for (int i = 0; en[i] != L'\0'; i++) {
		if (lc == en[i]) return is_up ? towupper(ru[i]) : ru[i];
		if (lc == ru[i]) return is_up ? towupper(en[i]) : en[i];
	}
	return c;
}

static int match_prefix_mode(const char *name, const wchar_t *wpfx, int plen, int alt_mode) {
	wchar_t wname[MAX_FILENAME];
	size_t nlen = mbstowcs(wname, name, MAX_FILENAME - 1);
	if (nlen == (size_t)-1 || (int)nlen < plen) return 0;
	for (int i = 0; i < plen; i++) {
		wchar_t target = (alt_mode && wpfx[i] < 128) ? alt_layout_char(wpfx[i]) : wpfx[i];
		if (towlower(wname[i]) != towlower(target)) return 0;
	}
	return 1;
}

static int match_prefix_icase(const char *name, const wchar_t *wpfx, int plen) {
	wchar_t wname[MAX_FILENAME];
	size_t nlen = mbstowcs(wname, name, MAX_FILENAME - 1);
	if (nlen == (size_t)-1 || (int)nlen < plen) return 0;
	for (int i = 0; i < plen; i++) {
		if (towlower(wname[i]) != towlower(wpfx[i])) return 0;
	}
	return 1;
}

static int quick_search_jump(int start_from) {
	if (!active_panel || active_panel->count == 0 || wsearch_len <= 0) return 0;
	for (int alt_mode = 0; alt_mode <= 1; alt_mode++) {
		for (int off = 0; off < active_panel->count; off++) {
			int idx = (start_from + off) % active_panel->count;
			if (strcmp(active_panel->files[idx].name, "..") == 0) continue;
			if (match_prefix_mode(active_panel->files[idx].name, wsearch, wsearch_len, alt_mode)) {
				active_panel->selected = idx;
				wchar_t wdisp[MAX_FILENAME];
				for (int k = 0; k < wsearch_len; k++) {
					wdisp[k] = (alt_mode && wsearch[k] < 128) ? alt_layout_char(wsearch[k]) : wsearch[k];
				}
				wdisp[wsearch_len] = L'\0';
				if (alt_mode) wmemcpy(wsearch, wdisp, (size_t)(wsearch_len + 1));
				wcstombs(quick_search, wdisp, sizeof(quick_search));
				quick_search[sizeof(quick_search) - 1] = '\0';
				return 1;
			}
		}
	}
	return 0;
}

static void quick_search_reset(void) {
	quick_search_nomatch = 0;
	quick_search_active = 0;
	wsearch_len = 0;
	wsearch[0] = L'\0';
	quick_search[0] = '\0';
}

CmdHistoryEntry cmd_history[MAX_HISTORY];
int history_count = 0;
static int history_idx = 0;

static void history_add_entry(const char *ts, const char *dir, const char *cmd) {
	if (!cmd || *cmd == '\0') return;
	for (int i = 0; i < history_count; i++) {
		if (strcmp(cmd_history[i].cmd, cmd) == 0) {
			for (int k = i; k + 1 < history_count; k++) {
				cmd_history[k] = cmd_history[k + 1];
			}
			history_count--;
			break;
		}
	}
	if (history_count >= MAX_HISTORY) {
		memmove(cmd_history, cmd_history + 1, sizeof(cmd_history[0]) * (MAX_HISTORY - 1));
		history_count = MAX_HISTORY - 1;
	}
	CmdHistoryEntry *e = &cmd_history[history_count];
	if (ts && *ts) {
		snprintf(e->timestamp, sizeof(e->timestamp), "%s", ts);
	} else {
		time_t now = time(NULL);
		struct tm *tmi = localtime(&now);
		if (tmi) strftime(e->timestamp, sizeof(e->timestamp), "%Y-%m-%d %H:%M", tmi);
		else e->timestamp[0] = '\0';
	}
	if (dir && *dir) {
		snprintf(e->dir, sizeof(e->dir), "%s", dir);
	} else if (active_panel && active_panel->current_path[0] != '\0') {
		snprintf(e->dir, sizeof(e->dir), "%s", active_panel->current_path);
	} else {
		snprintf(e->dir, sizeof(e->dir), ".");
	}
	snprintf(e->cmd, sizeof(e->cmd), "%s", cmd);
	history_count++;
	history_idx = history_count;
}

static void history_add(const char *cmd) {
	history_add_entry(NULL, NULL, cmd);
}

static void history_apply(void) {
	if (history_idx < 0) history_idx = 0;
	if (history_idx >= history_count) {
		history_idx = history_count;
		cmdline_clear();
		return;
	}
	cmdline_set_text(cmd_history[history_idx].cmd);
}

#include <sys/stat.h>
#include <errno.h>

static void get_history_path(char *out, size_t out_size) {
	const char *state_home = getenv("XDG_STATE_HOME");
	char base_dir[MAX_PATH];

	if (state_home && *state_home) {
		snprintf(base_dir, sizeof(base_dir), "%s/xcommander", state_home);
	} else {
		const char *home = getenv("HOME");
		if (!home || !*home) home = "/tmp";
		char local_dir[MAX_PATH];
		snprintf(local_dir, sizeof(local_dir), "%s/.local", home);
		mkdir(local_dir, 0755);
		snprintf(local_dir, sizeof(local_dir), "%s/.local/state", home);
		mkdir(local_dir, 0755);
		snprintf(base_dir, sizeof(base_dir), "%s/.local/state/xcommander", home);
	}

	mkdir(base_dir, 0755);
	snprintf(out, out_size, "%s/history", base_dir);
}

static void history_load(void) {
	char path[MAX_PATH + 32];
	get_history_path(path, sizeof(path));

	FILE *f = fopen(path, "r");
	if (!f) return;

	char line[MAX_CMD + MAX_PATH + 64];
	while (fgets(line, sizeof(line), f)) {
		size_t len = strlen(line);
		while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
			line[--len] = '\0';
		}
		if (len == 0) continue;
		char *t1 = strchr(line, '\t');
		char *t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
		if (t1 && t2) {
			*t1 = '\0';
			*t2 = '\0';
			history_add_entry(line, t1 + 1, t2 + 1);
		} else {
			history_add_entry("", "", line);
		}
	}
	fclose(f);
}

static void history_save(void) {
	char path[MAX_PATH + 32];
	get_history_path(path, sizeof(path));

	FILE *f = fopen(path, "w");
	if (!f) return;

	for (int i = 0; i < history_count; i++) {
		fprintf(f, "%s\t%s\t%s\n",
		        cmd_history[i].timestamp,
		        cmd_history[i].dir,
		        cmd_history[i].cmd);
	}
	fclose(f);
}

static DirHistoryEntry dir_history[MAX_DIR_HISTORY];
static int dir_history_count = 0;
static int dir_history_pos = -1;
static int suppress_dir_track = 0;

static void dir_history_add_entry(const char *ts, const char *dir) {
	if (!dir || *dir == '\0') return;
	if (dir_history_count > 0 && strcmp(dir_history[dir_history_count - 1].dir, dir) == 0) {
		return;
	}
	if (dir_history_count >= MAX_DIR_HISTORY) {
		memmove(dir_history, dir_history + 1, sizeof(dir_history[0]) * (MAX_DIR_HISTORY - 1));
		dir_history_count = MAX_DIR_HISTORY - 1;
	}
	DirHistoryEntry *e = &dir_history[dir_history_count];
	if (ts && *ts) {
		snprintf(e->timestamp, sizeof(e->timestamp), "%s", ts);
	} else {
		time_t now = time(NULL);
		struct tm *tmi = localtime(&now);
		if (tmi) strftime(e->timestamp, sizeof(e->timestamp), "%Y-%m-%d %H:%M", tmi);
		else e->timestamp[0] = '\0';
	}
	snprintf(e->dir, sizeof(e->dir), "%s", dir);
	dir_history_count++;
	dir_history_pos = dir_history_count - 1;
}

static void get_dir_history_path(char *out, size_t out_size) {
	char hist_path[MAX_PATH + 32];
	get_history_path(hist_path, sizeof(hist_path));
	char *sl = strrchr(hist_path, '/');
	if (sl) *sl = '\0';
	snprintf(out, out_size, "%s/dir_history", hist_path);
}

static void dir_history_load(void) {
	char path[MAX_PATH + 32];
	get_dir_history_path(path, sizeof(path));
	FILE *f = fopen(path, "r");
	if (!f) return;
	char line[MAX_PATH + 64];
	while (fgets(line, sizeof(line), f)) {
		size_t len = strlen(line);
		while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = '\0';
		if (len == 0) continue;
		char *t1 = strchr(line, '\t');
		if (t1) {
			*t1 = '\0';
			dir_history_add_entry(line, t1 + 1);
		} else {
			dir_history_add_entry("", line);
		}
	}
	fclose(f);
}

static void dir_history_save(void) {
	char path[MAX_PATH + 32];
	get_dir_history_path(path, sizeof(path));
	FILE *f = fopen(path, "w");
	if (!f) return;
	for (int i = 0; i < dir_history_count; i++) {
		fprintf(f, "%s\t%s\n", dir_history[i].timestamp, dir_history[i].dir);
	}
	fclose(f);
}

static void track_panel_dirs(char *last_l, size_t lsz, char *last_r, size_t rsz) {
	if (suppress_dir_track) return;
	int changed = 0;
	if (left_panel.current_path[0] != '\0' && strcmp(last_l, left_panel.current_path) != 0) {
		snprintf(last_l, lsz, "%s", left_panel.current_path);
		dir_history_add_entry(NULL, left_panel.current_path);
		changed = 1;
	}
	if (right_panel.current_path[0] != '\0' && strcmp(last_r, right_panel.current_path) != 0) {
		snprintf(last_r, rsz, "%s", right_panel.current_path);
		dir_history_add_entry(NULL, right_panel.current_path);
		changed = 1;
	}
	if (changed) dir_history_save();
}

static FileHistoryEntry file_history[MAX_FILE_HISTORY];
static int file_history_count = 0;

static void file_history_add_entry(const char *ts, const char *mode, int line, int col, const char *path) {
	if (!path || *path == '\0') return;
	char abs_p[MAX_PATH];
	if (path[0] != '/' && realpath(path, abs_p) != NULL) {
		path = abs_p;
	}
	int prev_line = 1, prev_col = 1;
	for (int i = 0; i < file_history_count; i++) {
		if (strcmp(file_history[i].path, path) == 0) {
			if (file_history[i].line > 0) prev_line = file_history[i].line;
			if (file_history[i].col > 0) prev_col = file_history[i].col;
			for (int k = i; k + 1 < file_history_count; k++) {
				file_history[k] = file_history[k + 1];
			}
			file_history_count--;
			break;
		}
	}
	if (file_history_count >= MAX_FILE_HISTORY) {
		memmove(file_history, file_history + 1, sizeof(file_history[0]) * (MAX_FILE_HISTORY - 1));
		file_history_count = MAX_FILE_HISTORY - 1;
	}
	FileHistoryEntry *e = &file_history[file_history_count];
	if (ts && *ts) {
		snprintf(e->timestamp, sizeof(e->timestamp), "%s", ts);
	} else {
		time_t now = time(NULL);
		struct tm *tmi = localtime(&now);
		if (tmi) strftime(e->timestamp, sizeof(e->timestamp), "%Y-%m-%d %H:%M", tmi);
		else e->timestamp[0] = '\0';
	}
	snprintf(e->mode, sizeof(e->mode), "%s", (mode && *mode) ? mode : "Edit");
	e->line = (line > 0) ? line : prev_line;
	e->col = (col > 0) ? col : prev_col;
	snprintf(e->path, sizeof(e->path), "%s", path);
	file_history_count++;
}

static void file_history_get_pos(const char *path, int *out_line, int *out_col) {
	if (out_line) *out_line = 1;
	if (out_col) *out_col = 1;
	if (!path || *path == '\0') return;
	char abs_p[MAX_PATH];
	if (path[0] != '/' && realpath(path, abs_p) != NULL) path = abs_p;
	for (int i = file_history_count - 1; i >= 0; i--) {
		if (strcmp(file_history[i].path, path) == 0) {
			if (out_line) *out_line = (file_history[i].line > 0) ? file_history[i].line : 1;
			if (out_col) *out_col = (file_history[i].col > 0) ? file_history[i].col : 1;
			return;
		}
	}
}

static void get_file_history_path(char *out, size_t out_size) {
	char hist_path[MAX_PATH + 32];
	get_history_path(hist_path, sizeof(hist_path));
	char *sl = strrchr(hist_path, '/');
	if (sl) *sl = '\0';
	snprintf(out, out_size, "%s/file_history", hist_path);
}

static void file_history_load(void) {
	char path[MAX_PATH + 32];
	get_file_history_path(path, sizeof(path));
	FILE *f = fopen(path, "r");
	if (!f) return;
	char line[MAX_PATH + 128];
	while (fgets(line, sizeof(line), f)) {
		size_t len = strlen(line);
		while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = '\0';
		if (len == 0) continue;
		char *t1 = strchr(line, '\t');
		char *t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
		char *t3 = t2 ? strchr(t2 + 1, '\t') : NULL;
		char *t4 = t3 ? strchr(t3 + 1, '\t') : NULL;
		if (t1 && t2 && t3 && t4) {
			*t1 = '\0'; *t2 = '\0'; *t3 = '\0'; *t4 = '\0';
			file_history_add_entry(line, t1 + 1, atoi(t2 + 1), atoi(t3 + 1), t4 + 1);
		} else if (t1 && t2) {
			*t1 = '\0'; *t2 = '\0';
			file_history_add_entry(line, t1 + 1, 1, 1, t2 + 1);
		} else {
			file_history_add_entry("", "Edit", 1, 1, line);
		}
	}
	fclose(f);
}

static void file_history_save(void) {
	char path[MAX_PATH + 32];
	get_file_history_path(path, sizeof(path));
	FILE *f = fopen(path, "w");
	if (!f) return;
	for (int i = 0; i < file_history_count; i++) {
		fprintf(f, "%s\t%s\t%d\t%d\t%s\n",
		        file_history[i].timestamp,
		        file_history[i].mode,
		        file_history[i].line > 0 ? file_history[i].line : 1,
		        file_history[i].col > 0 ? file_history[i].col : 1,
		        file_history[i].path);
	}
	fclose(f);
}

static void get_state_path(char *out, size_t out_size) {
	char hist_path[MAX_PATH + 32];
	get_history_path(hist_path, sizeof(hist_path));
	char *sl = strrchr(hist_path, '/');
	if (sl) *sl = '\0';
	snprintf(out, out_size, "%s/state", hist_path);
}

static void state_save(void) {
	char path[MAX_PATH + 32];
	get_state_path(path, sizeof(path));
	FILE *f = fopen(path, "w");
	if (!f) return;
	fprintf(f, "%s\n%s\n%d\n%d\n%d\n%s\n%s\n",
		left_panel.current_path,
		right_panel.current_path,
		(active_panel == &right_panel) ? 1 : 0,
		g_ed_last_case,
		g_ed_last_regex,
		g_ed_last_search,
		g_ed_last_replace);
	fclose(f);
}

static int state_load(char *lpath, size_t lsize, char *rpath, size_t rsize, int *act_right) {
	char path[MAX_PATH + 32];
	get_state_path(path, sizeof(path));
	FILE *f = fopen(path, "r");
	if (!f) return 0;
	char b1[MAX_PATH] = {0}, b2[MAX_PATH] = {0}, b3[32] = {0};
	if (!fgets(b1, sizeof(b1), f) || !fgets(b2, sizeof(b2), f)) { fclose(f); return 0; }
	if (fgets(b3, sizeof(b3), f)) *act_right = (atoi(b3) == 1) ? 1 : 0;
	char b4[32] = {0}, b5[32] = {0}, b6[128] = {0}, b7[128] = {0};
	if (fgets(b4, sizeof(b4), f)) g_ed_last_case = (atoi(b4) == 1) ? 1 : 0;
	if (fgets(b5, sizeof(b5), f)) g_ed_last_regex = (atoi(b5) == 1) ? 1 : 0;
	if (fgets(b6, sizeof(b6), f)) {
		size_t l = strlen(b6); while (l > 0 && (b6[l - 1] == '\n' || b6[l - 1] == '\r')) b6[--l] = '\0';
		snprintf(g_ed_last_search, sizeof(g_ed_last_search), "%s", b6);
	}
	if (fgets(b7, sizeof(b7), f)) {
		size_t l = strlen(b7); while (l > 0 && (b7[l - 1] == '\n' || b7[l - 1] == '\r')) b7[--l] = '\0';
		snprintf(g_ed_last_replace, sizeof(g_ed_last_replace), "%s", b7);
	}
	fclose(f);
	size_t l1 = strlen(b1); while (l1 > 0 && (b1[l1 - 1] == '\n' || b1[l1 - 1] == '\r')) b1[--l1] = '\0';
	size_t l2 = strlen(b2); while (l2 > 0 && (b2[l2 - 1] == '\n' || b2[l2 - 1] == '\r')) b2[--l2] = '\0';
	if (l1 > 0) snprintf(lpath, lsize, "%s", b1);
	if (l2 > 0) snprintf(rpath, rsize, "%s", b2);
	return (l1 > 0 && l2 > 0);
}

static void print_term_prompt(const wchar_t *wcmd) {
	print_term_prompt_ex(wcmd, 0);
}

static int confirm_exit(void) {
	if (!terminal_mode) {
		return ui_confirm("Quit", "Quit xCommander?");
	}
	reset_prog_mode();
	input_enable_raw();
	clearok(stdscr, TRUE);
	draw_screen();
	refresh();
	int res = ui_confirm("Quit", "Quit xCommander?");
	if (!res) {
		def_prog_mode();
		endwin();
		input_enable_raw();
		print_term_prompt(cmd_wbuf);
	}
	return res;
}









static KeyAction handle_mouse_event(InputEvent ev) {
	if (terminal_mode) return ACT_NONE;
	int height, width;
	getmaxyx(stdscr, height, width);
	int mx = ev.mouse_x, my = ev.mouse_y;
	if (mx < 0 || my < 0 || mx >= width || my >= height) return ACT_NONE;

	if (my == height - 1 && ev.action == ACT_MOUSE_LEFT) {
		int col_w = width / 10;
		if (col_w < 3) col_w = 3;
		int idx = mx / col_w;
		if (idx > 9) idx = 9;
		if (g_mod_alt && !g_mod_ctrl && !g_mod_shift) {
			if (idx == 0) return ACT_ALT_F1;
			if (idx == 1) return ACT_ALT_F2;
			if (idx == 7) return ACT_ALT_F8;
			if (idx == 8) return ACT_ALT_F11;
			if (idx == 11) return ACT_ALT_F12;
		} else if (g_mod_shift && !g_mod_ctrl && !g_mod_alt) {
			if (idx == 3) return ACT_SHIFT_F4;
			if (idx == 4) return ACT_SHIFT_F5;
			if (idx == 5) return ACT_SHIFT_F6;
		} else if (g_mod_ctrl && !g_mod_alt && !g_mod_shift) {
			if (idx == 2) return ACT_CTRL_F3;
			if (idx == 3) return ACT_CTRL_F4;
			if (idx == 4) return ACT_CTRL_F5;
			if (idx == 5) return ACT_CTRL_F6;
		}
		const KeyAction fkeys[10] = {
			ACT_F1, ACT_F2, ACT_F3, ACT_F4, ACT_F5,
			ACT_F6, ACT_F7, ACT_F8, ACT_F9, ACT_F10
		};
		return fkeys[idx];
	}

	int panel_h = height - 2;
	if (my >= panel_h) return ACT_NONE;

	int panel_w = width / 2;
	Panel *target_p = (mx < panel_w) ? &left_panel : &right_panel;
	int p_start_x = (mx < panel_w) ? 0 : panel_w;
	int p_width = (mx < panel_w) ? panel_w : (width - panel_w);

	if (cmd_suggest_count > 0 && cmd_suggest_w > 0 && cmd_suggest_h > 0) {
		int in_box = (ev.mouse_y >= cmd_suggest_y && ev.mouse_y < cmd_suggest_y + cmd_suggest_h &&
		              ev.mouse_x >= cmd_suggest_x && ev.mouse_x < cmd_suggest_x + cmd_suggest_w);
		if (in_box) {
			if (ev.action == ACT_MOUSE_WHEEL_UP) {
				if (cmd_suggest_sel < 0) cmd_suggest_sel = (cmd_suggest_count > 1) ? (cmd_suggest_count - 2) : 0;
				else cmd_suggest_sel = (cmd_suggest_sel > 0) ? (cmd_suggest_sel - 1) : (cmd_suggest_count - 1);
				draw_screen(); refresh();
				return ACT_NONE;
			}
			if (ev.action == ACT_MOUSE_WHEEL_DOWN) {
				if (cmd_suggest_sel < 0) cmd_suggest_sel = 0;
				else cmd_suggest_sel = (cmd_suggest_sel + 1) % cmd_suggest_count;
				draw_screen(); refresh();
				return ACT_NONE;
			}
			if (ev.action == ACT_MOUSE_LEFT) {
				int row = ev.mouse_y - (cmd_suggest_y + 1);
				if (row >= 0 && row < cmd_suggest_count) {
					cmdline_set_text(cmd_history[cmd_suggest_idx[row]].cmd);
					cmd_suggest_reset();
					draw_screen(); refresh();
				}
				return ACT_NONE;
			}
		} else if (ev.action == ACT_MOUSE_LEFT && ev.mouse_y < cmd_suggest_y + cmd_suggest_h) {
			cmd_suggest_dismissed = 1;
			cmd_suggest_count = 0;
			cmd_suggest_sel = -1;
		}
	}

	if (ev.action == ACT_MOUSE_WHEEL_UP || ev.action == ACT_MOUSE_WHEEL_DOWN) {
		int step = 3;
		if (ev.action == ACT_MOUSE_WHEEL_UP) {
			target_p->selected -= step;
			if (target_p->selected < 0) target_p->selected = 0;
		} else if (target_p->count > 0) {
			target_p->selected += step;
			if (target_p->selected >= target_p->count) target_p->selected = target_p->count - 1;
		}
		active_panel = target_p;
		quick_search_reset();
		draw_screen(); refresh();
		return ACT_NONE;
	}

	active_panel = target_p;
	quick_search_reset();

	int visible_lines = panel_h - 4;
	if (visible_lines < 1) visible_lines = 1;
	int row = my - 1;
	if (row >= 0 && row < visible_lines) {
		int inner_w = p_width - 2;
		int col1_w = inner_w / 2;
		int mid_x = p_start_x + 1 + col1_w;
		int col = (mx > mid_x) ? 1 : 0;
		int file_idx = target_p->top_line + col * visible_lines + row;

		if (file_idx >= 0 && file_idx < target_p->count) {
			static long long last_click_ms = 0;
			static Panel *last_click_p = NULL;
			static int last_click_idx = -1;

			struct timeval tv;
			gettimeofday(&tv, NULL);
			long long now_ms = (long long)tv.tv_sec * 1000LL + tv.tv_usec / 1000;

			target_p->selected = file_idx;
			if (ev.action == ACT_MOUSE_RIGHT) {
				panel_toggle_mark(target_p, file_idx);
				draw_screen(); refresh();
				return ACT_NONE;
			}
			if (ev.action == ACT_MOUSE_LEFT) {
				int is_dbl = (last_click_p == target_p && last_click_idx == file_idx && (now_ms - last_click_ms) < 400);
				last_click_p = target_p;
				last_click_idx = file_idx;
				last_click_ms = now_ms;
				draw_screen(); refresh();
				if (is_dbl) {
					last_click_ms = 0;
					return ACT_ENTER;
				}
				return ACT_NONE;
			}
		}
	}

	draw_screen(); refresh();
	return ACT_NONE;
}

static void reload_panels_keep_cursor(void) {
	Panel *ps[2] = { &left_panel, &right_panel };
	for (int i = 0; i < 2; i++) {
		if (ps[i]->parent_name[0] == '\0' && ps[i]->count > 0 &&
		    ps[i]->selected >= 0 && ps[i]->selected < ps[i]->count) {
			snprintf(ps[i]->parent_name, MAX_FILENAME, "%s", ps[i]->files[ps[i]->selected].name);
		}
		load_directory(ps[i]);
	}
}

static void run_command_to_editor(const char *cmd) {
	if (!cmd || *cmd == '\0') return;
	char tmp_out[128] = "/tmp/xc_cmd_out_XXXXXX";
	int fd = mkstemp(tmp_out);
	if (fd < 0) return;
	close(fd);

	int was_term = terminal_mode;
	if (was_term) { reset_prog_mode(); input_set_mouse(1); refresh(); }
	else {
		int h, w; getmaxyx(stdscr, h, w);
		attrset(g_ui_theme.win_bg);
		mvprintw(h / 2, (w - 22) / 2, " Running command... ");
		refresh();
	}

	char sh_cmd[CMD_MAX_LEN + 256];
	snprintf(sh_cmd, sizeof(sh_cmd), "( %s ) > \"%s\" 2>&1", cmd, tmp_out);
	int chd = 0;
	char old_cwd[MAX_PATH] = "";
	if (active_panel && active_panel->current_path[0] && getcwd(old_cwd, sizeof(old_cwd))) {
		if (chdir(active_panel->current_path) == 0) chd = 1;
	}
	int rc = system(sh_cmd);
	(void)rc;
	if (chd && old_cwd[0]) { int r2 = chdir(old_cwd); (void)r2; }

	int end_ln = 1, end_cl = 1;
	ui_edit_file_ex(tmp_out, 1, 1, &end_ln, &end_cl);
	unlink(tmp_out);

	if (was_term) {
		endwin();
		input_enable_raw();
		input_set_mouse(0);
	} else {
		reload_panels_keep_cursor();
		draw_screen();
		refresh();
	}
}

static void run_multiline_script(const char *script, int was_term) {
	if (!script || !*script) return;
	ml_history_add(active_panel ? active_panel->current_path : ".", script);
	char tmp_sh[128];
	snprintf(tmp_sh, sizeof(tmp_sh), "/tmp/xc_paste_%d.sh", (int)getpid());
	FILE *sf = fopen(tmp_sh, "w");
	if (sf) {
		fputs("printf \"\\n\\033[1;36m%s\\033[0m\\n\" \"$(printf '═%.0s' $(seq 1 ${COLUMNS:-80}))\"\n", sf);
		fputs("printf \"\\033[1;33m▶ [ $(date +%H:%M:%S) | $PWD ]\\033[0m\\n\"\n", sf);
		fputs(script, sf);
		fputs("\nrc=$?\nif [ $rc -ne 0 ]; then\n  printf \"\\n\\033[1;31m--- Error (exit: $rc). Press any key... ---\\033[0m\"\n  read -n 1 -s -r\nfi\n", sf);
		fclose(sf);
		char run_sh[160];
		snprintf(run_sh, sizeof(run_sh), ". \"%s\"", tmp_sh);
		if (was_term) printf("\r\n");
		execute_system_command(run_sh);
		unlink(tmp_sh);
		reload_panels_keep_cursor();
	}
	if (!was_term) { draw_screen(); refresh(); }
	else { print_term_prompt_ex(cmd_wbuf, 1); }
}

#ifndef XC_VERSION
#define XC_VERSION "0.1"
#endif
#ifndef XC_BUILD
#define XC_BUILD 0
#endif

typedef struct {
	const char *left_dir;
	const char *right_dir;
	const char *edit_file;
	int edit_mode;
	int edit_line;
	int edit_col;
} StartupOpts;

static void print_cli_version(void) {
	printf("xCommander v%s [%06d]\n", XC_VERSION, XC_BUILD);
}

static void print_cli_help(const char *prog) {
	printf("Usage: %s [OPTIONS] [LEFT_DIR] [RIGHT_DIR]\n\n", prog);
	printf("Options:\n");
	printf("  -e[LINE[:POS]] [FILE], --edit  Open FILE in built-in editor at LINE:POS\n");
	printf("  -v, --version    Show version and build number\n");
	printf("  -h, --help       Show this help message\n");
}

static int parse_startup_args(int argc, char **argv, StartupOpts *opts) {
	memset(opts, 0, sizeof(*opts));
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
			print_cli_version();
			return 1;
		}
		if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
			print_cli_help(argv[0]);
			return 1;
		}
		if (strncmp(argv[i], "-e", 2) == 0 && (argv[i][2] == '\0' || (argv[i][2] >= '0' && argv[i][2] <= '9'))) {
			opts->edit_mode = 1;
			opts->edit_line = 1;
			opts->edit_col = 1;
			const char *spec = argv[i] + 2;
			if (*spec != '\0') {
				char *endp = NULL;
				long ln = strtol(spec, &endp, 10);
				if (endp == spec || ln < 1 || (*endp != '\0' && *endp != ':')) {
					fprintf(stderr, "Invalid editor position in '%s'. Expected -e[LINE[:POS]]\n", argv[i]);
					return -1;
				}
				opts->edit_line = (int)ln;
				if (*endp == ':') {
					const char *cspec = endp + 1;
					char *cendp = NULL;
					long cl = strtol(cspec, &cendp, 10);
					if (cendp == cspec || cl < 1 || *cendp != '\0') {
						fprintf(stderr, "Invalid column position in '%s'. Expected -e[LINE[:POS]]\n", argv[i]);
						return -1;
					}
					opts->edit_col = (int)cl;
				}
			}
			if (i + 1 < argc && argv[i + 1][0] != '-') {
				opts->edit_file = argv[++i];
			}
			continue;
		}
		if (strcmp(argv[i], "--edit") == 0) {
			opts->edit_mode = 1;
			opts->edit_line = 1;
			opts->edit_col = 1;
			if (i + 1 < argc && argv[i + 1][0] != '-') {
				opts->edit_file = argv[++i];
			}
			continue;
		}
		if (argv[i][0] == '-') {
			fprintf(stderr, "Unknown option: %s\nTry '%s --help' for more information.\n", argv[i], argv[0]);
			return -1;
		}
		if (!opts->left_dir) opts->left_dir = argv[i];
		else if (!opts->right_dir) opts->right_dir = argv[i];
	}
	return 0;
}

int main(int argc, char **argv) {
	StartupOpts opts;
	int arg_res = parse_startup_args(argc, argv, &opts);
	if (arg_res != 0) return (arg_res > 0) ? 0 : 1;

	if (!isatty(STDIN_FILENO)) {
		setenv("TERM", "xterm-256color", 1);
		char self[MAX_PATH];
		ssize_t len = readlink("/proc/self/exe", self, sizeof(self) - 1);
		if (len > 0) {
			self[len] = '\0';
			const char *terms[] = {"kitty", "xfce4-terminal", "konsole", "xterm", NULL};
			for (int i = 0; terms[i]; i++) {
				char check[128];
				snprintf(check, sizeof(check), "command -v %s >/dev/null 2>&1", terms[i]);
				if (system(check) == 0) {
					char **nargv = calloc((size_t)argc + 3, sizeof(char *));
					if (nargv) {
						int k = 0;
						nargv[k++] = (char *)terms[i];
						nargv[k++] = (strcmp(terms[i], "gnome-terminal") == 0) ? "--" : "-e";
						nargv[k++] = self;
						for (int a = 1; a < argc; a++) nargv[k++] = argv[a];
						nargv[k] = NULL;
						execvp(terms[i], nargv);
					}
				}
			}
		}
		return 1;
	}
	setlocale(LC_ALL, "");
	ui_init();
	nonl();
	
	
        history_load();
	dir_history_load();
	file_history_load();

	char cwd[MAX_PATH];
	if (getcwd(cwd, sizeof(cwd)) == NULL) strcpy(cwd, ".");
	char saved_l[MAX_PATH] = {0}, saved_r[MAX_PATH] = {0};
	int act_right = 0;
	int has_state = state_load(saved_l, sizeof(saved_l), saved_r, sizeof(saved_r), &act_right);

	if (opts.left_dir && opts.left_dir[0] != '\0') {
		snprintf(left_panel.current_path, MAX_PATH, "%s", opts.left_dir);
		if (opts.right_dir && opts.right_dir[0] != '\0') snprintf(right_panel.current_path, MAX_PATH, "%s", opts.right_dir);
		else if (has_state && access(saved_r, R_OK | X_OK) == 0) snprintf(right_panel.current_path, MAX_PATH, "%s", saved_r);
		else strcpy(right_panel.current_path, cwd);
		act_right = 0;
	} else if (has_state) {
		if (access(saved_l, R_OK | X_OK) == 0) snprintf(left_panel.current_path, MAX_PATH, "%s", saved_l);
		else strcpy(left_panel.current_path, cwd);
		if (access(saved_r, R_OK | X_OK) == 0) snprintf(right_panel.current_path, MAX_PATH, "%s", saved_r);
		else strcpy(right_panel.current_path, cwd);
	} else {
		strcpy(left_panel.current_path, cwd);
		strcpy(right_panel.current_path, cwd);
	}

	left_panel.parent_name[0] = '\0';
	right_panel.parent_name[0] = '\0';
	left_panel.watch_fd = -1;
	right_panel.watch_fd = -1;

	load_directory(&left_panel);
	load_directory(&right_panel);
	active_panel = act_right ? &right_panel : &left_panel;

	clear();
	refresh();
	draw_screen();
	refresh();
	input_enable_raw();

	if (opts.edit_mode) {
		const char *target_file = (opts.edit_file && opts.edit_file[0] != '\0') ? opts.edit_file : "untitled.txt";
		int end_ln = opts.edit_line, end_cl = opts.edit_col;
		ui_edit_file_ex(target_file, opts.edit_line, opts.edit_col, &end_ln, &end_cl);
		file_history_add_entry(NULL, "Edit", end_ln, end_cl, target_file);
		file_history_save();
		input_restore_sane();
		endwin();
		input_restore_sane();
		return 0;
	}

	cmdline_init();
	static char last_prompt_dir[MAX_PATH] = "";
	char tracked_l[MAX_PATH] = "", tracked_r[MAX_PATH] = "";
	track_panel_dirs(tracked_l, sizeof(tracked_l), tracked_r, sizeof(tracked_r));
	while (1) {
		track_panel_dirs(tracked_l, sizeof(tracked_l), tracked_r, sizeof(tracked_r));
		InputEvent ev = input_read();
		if (ev.action == ACT_NONE) continue;
		if (ev.action == ACT_DIR_CHANGED) {
			if (!terminal_mode) {
				reload_panels_keep_cursor();
				draw_screen();
				refresh();
			}
			continue;
		}
		if (ev.action != ACT_SHIFT_LEFT && ev.action != ACT_SHIFT_RIGHT && ev.action != ACT_NONE) { /* reset lr chain */ }
		if (ev.action == ACT_MOD_CHANGE) {
			if (!terminal_mode) { draw_screen(); refresh(); }
			continue;
		}
		if (ev.action >= ACT_MOUSE_LEFT && ev.action <= ACT_MOUSE_WHEEL_DOWN) {
			ev.action = handle_mouse_event(ev);
			if (ev.action == ACT_NONE) continue;
		}
		if (ev.action == ACT_RESIZE) {
			if (!terminal_mode) { endwin(); refresh(); clearok(stdscr, TRUE); draw_screen(); refresh(); }
			continue;
		}
		if (ev.action == ACT_CTRL_Q || ev.action == ACT_F10) {
			if (confirm_exit()) break;
			if (!terminal_mode) { draw_screen(); refresh(); }
			continue;
		}
		if (!terminal_mode && ev.action == ACT_ALT_SHIFT_N && active_panel && active_panel->count > 0) {
			int marked = panel_marked_count(active_panel);
			if (marked == 0) {
				const char *nm = active_panel->files[active_panel->selected].name;
				if (strcmp(nm, "..") != 0) clipboard_copy(nm);
			} else {
				size_t cap = (size_t)marked * (MAX_FILENAME + 2) + 8;
				char *txt = malloc(cap);
				if (txt) {
					size_t off = 0; txt[0] = '\0';
					for (int i = 0; i < active_panel->count; i++) {
						if (!active_panel->files[i].is_selected || strcmp(active_panel->files[i].name, "..") == 0) continue;
						off += (size_t)snprintf(txt + off, cap - off, "%s%s", off ? "\n" : "", active_panel->files[i].name);
					}
					if (off > 0) clipboard_copy(txt);
					free(txt);
				}
			}
			continue;
		}
		if (ev.action == ACT_CTRL_C || ev.action == ACT_CTRL_Y) {
			if (!terminal_mode && cmd_len == 0 && cmd_sel_start < 0) clipboard_stage_files(active_panel, 0);
			else cmdline_copy(active_panel, terminal_mode);
			continue;
		}
		if (ev.action == ACT_CTRL_X && (cmd_sel_start >= 0 || (!terminal_mode && cmd_len == 0))) {
			if (cmd_sel_start >= 0) {
				cmdline_cut();
				if (!terminal_mode) { draw_screen(); refresh(); } else { print_term_prompt(cmd_wbuf); }
			} else clipboard_stage_files(active_panel, 1);
			continue;
		}
		if (ev.action == ACT_CTRL_V || ev.action == ACT_SHIFT_INSERT) {
			if (!terminal_mode && cmd_len == 0 && clipboard_has_files()) {
				if (clipboard_paste_files(active_panel) > 0) {
					for (int i = 0; i < left_panel.count; i++) left_panel.files[i].is_selected = 0;
					for (int i = 0; i < right_panel.count; i++) right_panel.files[i].is_selected = 0;
					reload_panels_keep_cursor();
				}
				draw_screen(); refresh();
			} else {
				char raw_clip[65536] = {0};
				if (clipboard_paste_raw(raw_clip, sizeof(raw_clip)) > 0 && strchr(raw_clip, '\n') != NULL) {
					int was_term = terminal_mode;
					if (was_term) { reset_prog_mode(); input_set_mouse(1); refresh(); }
					int ok = ui_multiline_paste(raw_clip);
					if (was_term) { endwin(); input_enable_raw(); if (terminal_mode) input_set_mouse(0); }
					if (ok == 1) {
						run_multiline_script(raw_clip, was_term);
					} else if (ok == 2) {
						if (was_term) { reset_prog_mode(); input_set_mouse(1); refresh(); }
						char picked[65536] = {0};
						int h_ok = ui_multiline_history(picked, sizeof(picked));
						if (was_term) { endwin(); input_enable_raw(); if (terminal_mode) input_set_mouse(0); }
						if (h_ok) run_multiline_script(picked, was_term);
						else if (!was_term) { draw_screen(); refresh(); }
						else { print_term_prompt(cmd_wbuf); }
					} else {
						if (!was_term) { draw_screen(); refresh(); }
						else { print_term_prompt(cmd_wbuf); }
					}
				} else {
					cmdline_paste();
					if (!terminal_mode) { draw_screen(); refresh(); } else { print_term_prompt(cmd_wbuf); }
				}
			}
			continue;
		}
		if (!terminal_mode && cmd_len == 0 && ev.action == ACT_CTRL_U) {
			char tmp_path[MAX_PATH];
			snprintf(tmp_path, sizeof(tmp_path), "%s", left_panel.current_path);
			snprintf(left_panel.current_path, sizeof(left_panel.current_path), "%s", right_panel.current_path);
			snprintf(right_panel.current_path, sizeof(right_panel.current_path), "%s", tmp_path);
			int l_sel = left_panel.selected;
			int r_sel = right_panel.selected;
			left_panel.parent_name[0] = '\0';
			right_panel.parent_name[0] = '\0';
			quick_search_reset();
			load_directory(&left_panel);
			load_directory(&right_panel);
			if (r_sel < left_panel.count) left_panel.selected = r_sel;
			if (l_sel < right_panel.count) right_panel.selected = l_sel;
			active_panel = (active_panel == &left_panel) ? &right_panel : &left_panel;
			draw_screen();
			refresh();
			continue;
		}
		if (ev.action == ACT_CTRL_O) {
			terminal_mode = !terminal_mode;
			if (terminal_mode) {
				def_prog_mode(); endwin(); input_enable_raw(); if (terminal_mode) input_set_mouse(0); input_set_mouse(0);
				if (strcmp(last_prompt_dir, active_panel->current_path) != 0) {
					snprintf(last_prompt_dir, sizeof(last_prompt_dir), "%s", active_panel->current_path);
					print_term_prompt_ex(cmd_wbuf, 1);
				} else {
					print_term_prompt_ex(cmd_wbuf, 0);
				}
			} else {
				reset_prog_mode(); input_enable_raw(); input_set_mouse(1); clearok(stdscr, TRUE);
				reload_panels_keep_cursor(); refresh(); draw_screen(); refresh();
			}
			continue;
		}
		if (ev.action == ACT_INSERT_LEFT_PATH) { cmdline_insert_path(&left_panel); if (!terminal_mode) { draw_screen(); refresh(); } else print_term_prompt(cmd_wbuf); continue; }
		if (ev.action == ACT_INSERT_RIGHT_PATH) { cmdline_insert_path(&right_panel); if (!terminal_mode) { draw_screen(); refresh(); } else print_term_prompt(cmd_wbuf); continue; }
		if (!terminal_mode && cmd_len == 0 && ev.action == ACT_ALT_BACKSPACE) {
			if (dir_history_count > 1) {
				int start_i = (dir_history_pos >= 0 && dir_history_pos < dir_history_count) ? (dir_history_pos - 1) : (dir_history_count - 2);
				int found_i = -1;
				for (int i = start_i; i >= 0; i--) {
					if (strcmp(dir_history[i].dir, active_panel->current_path) != 0 &&
					    access(dir_history[i].dir, R_OK | X_OK) == 0) {
						found_i = i;
						break;
					}
				}
				if (found_i >= 0) {
					dir_history_pos = found_i;
					suppress_dir_track = 1;
					quick_search_reset();
					active_panel->parent_name[0] = '\0';
					snprintf(active_panel->current_path, MAX_PATH, "%s", dir_history[found_i].dir);
					load_directory(active_panel);
					if (active_panel == &left_panel) snprintf(tracked_l, sizeof(tracked_l), "%s", active_panel->current_path);
					else snprintf(tracked_r, sizeof(tracked_r), "%s", active_panel->current_path);
					suppress_dir_track = 0;
					draw_screen();
					refresh();
				}
			}
			continue;
		}
		if (!terminal_mode && cmd_len == 0 && ev.action == ACT_ALT_SHIFT_BACKSPACE) {
			if (dir_history_count > 1 && dir_history_pos >= 0 && dir_history_pos + 1 < dir_history_count) {
				int found_i = -1;
				for (int i = dir_history_pos + 1; i < dir_history_count; i++) {
					if (strcmp(dir_history[i].dir, active_panel->current_path) != 0 &&
					    access(dir_history[i].dir, R_OK | X_OK) == 0) {
						found_i = i;
						break;
					}
				}
				if (found_i >= 0) {
					dir_history_pos = found_i;
					suppress_dir_track = 1;
					quick_search_reset();
					active_panel->parent_name[0] = '\0';
					snprintf(active_panel->current_path, MAX_PATH, "%s", dir_history[found_i].dir);
					load_directory(active_panel);
					if (active_panel == &left_panel) snprintf(tracked_l, sizeof(tracked_l), "%s", active_panel->current_path);
					else snprintf(tracked_r, sizeof(tracked_r), "%s", active_panel->current_path);
					suppress_dir_track = 0;
					draw_screen();
					refresh();
				}
			}
			continue;
		}
		if (ev.action == ACT_ALT_F9 || ev.action == ACT_CTRL_G) {
			char picked_file[MAX_PATH] = "";
			int picked_line = 1, picked_col = 1;
			int prev_fcnt = file_history_count;
			int fres = ui_file_history(file_history, &file_history_count, picked_file, sizeof(picked_file), &picked_line, &picked_col);
			if (file_history_count != prev_fcnt) file_history_save();
			if ((fres == 1 || fres == 2) && picked_file[0] != '\0') {
				if (access(picked_file, F_OK) == 0) {
					if (fres == 1) {
						int end_ln = picked_line, end_cl = picked_col;
						ui_edit_file_ex(picked_file, picked_line, picked_col, &end_ln, &end_cl);
						file_history_add_entry(NULL, "Edit", end_ln, end_cl, picked_file);
						file_history_save();
						reload_panels_keep_cursor();
					} else {
						file_history_add_entry(NULL, "View", 0, 0, picked_file);
						file_history_save();
						ui_view_file(picked_file);
					}
				} else {
					ui_message("File History", "File no longer exists");
				}
			} else if (fres == 3 && picked_file[0] != '\0') {
				char dir_buf[MAX_PATH];
				snprintf(dir_buf, sizeof(dir_buf), "%s", picked_file);
				char *sl = strrchr(dir_buf, '/');
				if (sl) {
					const char *fname = sl + 1;
					if (sl == dir_buf) strcpy(dir_buf, "/");
					else *sl = '\0';
					if (access(dir_buf, R_OK | X_OK) == 0) {
						quick_search_reset();
						snprintf(active_panel->parent_name, MAX_FILENAME, "%s", fname);
						snprintf(active_panel->current_path, MAX_PATH, "%s", dir_buf);
						load_directory(active_panel);
					} else {
						ui_message("Jump", "Parent directory no longer exists");
					}
				}
			}
			if (!terminal_mode) { draw_screen(); refresh(); } else print_term_prompt(cmd_wbuf);
			continue;
		}
		if (ev.action == ACT_ALT_F10 || ev.action == ACT_CTRL_D) {
			char picked_dir[MAX_PATH] = "";
			int prev_dcnt = dir_history_count;
			int dres = ui_dir_history(dir_history, &dir_history_count, picked_dir, sizeof(picked_dir));
			if (dir_history_count != prev_dcnt) dir_history_save();
			if ((dres == 1 || dres == 2) && picked_dir[0] != '\0') {
				Panel *tp = (dres == 1) ? active_panel : ((active_panel == &left_panel) ? &right_panel : &left_panel);
				if (access(picked_dir, R_OK | X_OK) == 0) {
					quick_search_reset();
					tp->parent_name[0] = '\0';
					snprintf(tp->current_path, MAX_PATH, "%s", picked_dir);
					load_directory(tp);
					track_panel_dirs(tracked_l, sizeof(tracked_l), tracked_r, sizeof(tracked_r));
				} else {
					ui_message("Folder History", "Directory no longer exists");
				}
			} else if (dres == 3 && picked_dir[0] != '\0') {
				char qpath[MAX_PATH + 16];
				if (strchr(picked_dir, ' ')) snprintf(qpath, sizeof(qpath), "\"%s\" ", picked_dir);
				else snprintf(qpath, sizeof(qpath), "%s ", picked_dir);
				wchar_t wqpath[MAX_PATH + 16]; if (mbstowcs(wqpath, qpath, MAX_PATH + 15) != (size_t)-1) cmdline_insert_wstr(wqpath);
			}
			if (!terminal_mode) { draw_screen(); refresh(); } else print_term_prompt(cmd_wbuf);
			continue;
		}
		if (ev.action == ACT_ALT_F8 || ev.action == ACT_CTRL_H) {
			char picked_cmd[MAX_CMD] = "";
			char picked_dir[MAX_PATH] = "";
			int prev_cnt = history_count;
			int hres = ui_cmd_history(cmd_history, &history_count, picked_cmd, sizeof(picked_cmd), picked_dir, sizeof(picked_dir));
			if (history_count != prev_cnt) {
				history_idx = history_count;
				history_save();
			}
			if (hres == 1 && picked_cmd[0] != '\0') {
				if (terminal_mode) { printf("\r\n"); fflush(stdout); }
				history_add(picked_cmd);
				history_save();
				execute_system_command(picked_cmd);
				last_prompt_dir[0] = '\0';
				if (!terminal_mode) reload_panels_keep_cursor();
				cmdline_clear();
				if (!terminal_mode) { draw_screen(); refresh(); } else print_term_prompt_ex(cmd_wbuf, 1);
			} else if (hres == 2 && picked_cmd[0] != '\0') {
				cmdline_set_text(picked_cmd);
				if (!terminal_mode) { draw_screen(); refresh(); } else print_term_prompt(cmd_wbuf);
			} else if (hres == 3) {
				if (picked_dir[0] != '\0' && access(picked_dir, R_OK | X_OK) == 0) {
					quick_search_reset();
					active_panel->parent_name[0] = '\0';
					snprintf(active_panel->current_path, MAX_PATH, "%s", picked_dir);
					load_directory(active_panel);
				} else {
					ui_message("Jump", "Saved directory no longer exists");
				}
				if (!terminal_mode) { draw_screen(); refresh(); } else print_term_prompt_ex(cmd_wbuf, 1);
			} else {
				if (!terminal_mode) { draw_screen(); refresh(); } else print_term_prompt(cmd_wbuf);
			}
			continue;
		}
		if (ev.action == ACT_ALT_F11 || ev.action == ACT_CTRL_L) {
			int was_term = terminal_mode;
			if (was_term) { reset_prog_mode(); input_set_mouse(1); refresh(); }
			char picked[65536] = {0};
			int h_ok = ui_multiline_history(picked, sizeof(picked));
			if (was_term) { endwin(); input_enable_raw(); if (terminal_mode) input_set_mouse(0); }
			if (h_ok) run_multiline_script(picked, was_term);
			else if (!was_term) { draw_screen(); refresh(); }
			else { print_term_prompt(cmd_wbuf); }
			continue;
		}
		if (!terminal_mode && iswprint(ev.ch) &&
		    (ev.action == ACT_ALT_CHAR || (quick_search_active && ev.action == ACT_CHAR))) {
			if (quick_search_active && wsearch_len == 1 && towlower(wsearch[0]) == towlower(ev.ch)) {
				quick_search_jump((active_panel->selected + 1) % active_panel->count);
			} else if (wsearch_len < MAX_FILENAME - 2) {
				wsearch[wsearch_len] = ev.ch;
				wsearch[wsearch_len + 1] = L'\0';
				wsearch_len++;
				if (quick_search_jump(active_panel->selected)) {
					quick_search_active = 1;
					quick_search_nomatch = 0;
				} else {
					wsearch[--wsearch_len] = L'\0';
					if (quick_search_active) quick_search_nomatch = 1;
					beep();
				}
			}
			draw_screen(); refresh();
			continue;
		}
		if (!terminal_mode && quick_search_active) {
			if (ev.action == ACT_BACKSPACE) {
				if (wsearch_len > 0) {
					wsearch[--wsearch_len] = L'\0';
					wcstombs(quick_search, wsearch, sizeof(quick_search));
					quick_search[sizeof(quick_search) - 1] = '\0';
					if (wsearch_len == 0) quick_search_reset();
					else quick_search_jump(0);
				}
				draw_screen(); refresh();
				continue;
			} else if (ev.action == ACT_ESC) {
				quick_search_reset();
				draw_screen(); refresh();
				continue;
			} else if (ev.action != ACT_ENTER) {
				quick_search_reset();
			}
		}
		if (ev.action == ACT_INSERT_NAME) { cmdline_insert_filename(active_panel); if (!terminal_mode) { draw_screen(); refresh(); } else print_term_prompt(cmd_wbuf); continue; }
		int is_hist_prev = (ev.action == ACT_CTRL_E) || (terminal_mode && ev.action == ACT_UP);
		int is_hist_next = (ev.action == ACT_CTRL_X) || (terminal_mode && ev.action == ACT_DOWN);
		if (is_hist_prev || is_hist_next) {
			if (history_count > 0) {
				if (is_hist_prev && history_idx > 0) history_idx--;
				else if (is_hist_next && history_idx < history_count) history_idx++;
				history_apply();
				if (!terminal_mode) { draw_screen(); refresh(); } else print_term_prompt(cmd_wbuf);
			}
			continue;
		}
		int is_cmd_to_ed = (cmd_len > 0 && ev.action == ACT_SHIFT_ENTER);
		if (is_cmd_to_ed) {
			char full_cmd[CMD_MAX_LEN];
			cmdline_get_utf8(full_cmd, sizeof(full_cmd));
			history_add(full_cmd);
			history_idx = history_count;
			history_save();
			cmdline_clear();
			cmd_suggest_reset();
			run_command_to_editor(full_cmd);
			if (terminal_mode) print_term_prompt_ex(cmd_wbuf, 1);
			continue;
		}
		if (cmd_len > 0 && ev.action == ACT_ENTER) {
			if (terminal_mode) { printf("\r\n"); fflush(stdout); }
			char full_cmd[CMD_MAX_LEN];
			cmdline_get_utf8(full_cmd, sizeof(full_cmd));
			if (strcmp(full_cmd, "exit") == 0 || strcmp(full_cmd, "quit") == 0) {
				if (confirm_exit()) break;
				cmdline_clear();
				if (!terminal_mode) { draw_screen(); refresh(); } else print_term_prompt(cmd_wbuf);
				continue;
			}
			history_add(full_cmd);
			history_save();
			execute_system_command(full_cmd); last_prompt_dir[0] = '\0';
			if (!terminal_mode) reload_panels_keep_cursor();
			cmdline_clear();
			if (!terminal_mode) { draw_screen(); refresh(); } else print_term_prompt_ex(cmd_wbuf, 1);
			continue;
		}
		if (!terminal_mode && cmd_len == 0 && ev.action == ACT_CHAR &&
		    (ev.ch == L'+' || ev.ch == L'-' || ev.ch == L'*')) {
			static char last_mask[MAX_FILENAME] = "*";
			if (ev.ch == L'*') {
				panel_invert_selection(active_panel);
			} else {
				char mask_buf[MAX_FILENAME];
				snprintf(mask_buf, sizeof(mask_buf), "%s", last_mask);
				const char *title = (ev.ch == L'+') ? "Select (+)" : "Deselect (-)";
				if (ui_input(title, "Enter file mask (e.g. *.c, *test*):", mask_buf, sizeof(mask_buf)) && mask_buf[0] != '\0') {
					snprintf(last_mask, sizeof(last_mask), "%s", mask_buf);
					panel_select_mask(active_panel, mask_buf, ev.ch == L'+');
				}
			}
			draw_screen();
			refresh();
			continue;
		}
		if (!terminal_mode && cmd_suggest_count > 0) {
			if (ev.action == ACT_ESC) {
				cmd_suggest_dismissed = 1;
				cmd_suggest_count = 0;
				cmd_suggest_sel = -1;
				draw_screen(); refresh();
				continue;
			}
			if (ev.action == ACT_UP) {
				if (cmd_suggest_sel < 0) cmd_suggest_sel = (cmd_suggest_count > 1) ? (cmd_suggest_count - 2) : 0;
				else cmd_suggest_sel = (cmd_suggest_sel > 0) ? (cmd_suggest_sel - 1) : (cmd_suggest_count - 1);
				draw_screen(); refresh();
				continue;
			}
			if (ev.action == ACT_DOWN) {
				if (cmd_suggest_sel < 0) cmd_suggest_sel = 0;
				else cmd_suggest_sel = (cmd_suggest_sel + 1) % cmd_suggest_count;
				draw_screen(); refresh();
				continue;
			}
			if (ev.action == ACT_RIGHT && cmd_pos == cmd_len) {
				int pick = (cmd_suggest_sel >= 0 && cmd_suggest_sel < cmd_suggest_count) ? cmd_suggest_sel : (cmd_suggest_count - 1);
				cmdline_set_text(cmd_history[cmd_suggest_idx[pick]].cmd);
				cmd_suggest_reset();
				draw_screen(); refresh();
				continue;
			}
			if (ev.action == ACT_ENTER && cmd_suggest_sel >= 0 && cmd_suggest_sel < cmd_suggest_count) {
				cmdline_set_text(cmd_history[cmd_suggest_idx[cmd_suggest_sel]].cmd);
				cmd_suggest_reset();
			}
		}
		int ed_res = cmdline_handle_edit_key(ev, terminal_mode);
		if (ed_res > 0) {
			if (ed_res == 2) { history_idx = history_count; cmd_suggest_dismissed = 0; cmd_suggest_sel = -1; }
			if (cmd_len == 0) cmd_suggest_reset();
			if (!terminal_mode) { draw_screen(); refresh(); } else print_term_prompt(cmd_wbuf);
			continue;
		}
		if (terminal_mode) {
			continue;
		}
		if (ev.action == ACT_ALT_F12) { ui_toggle_auto_colorize(); draw_screen(); refresh(); continue; }
		if (ev.action == ACT_TAB) active_panel = (active_panel == &left_panel) ? &right_panel : &left_panel;
		else if (ev.action == ACT_CTRL_R) { reload_panels_keep_cursor(); clearok(stdscr, TRUE); }
				else if (ev.action == ACT_CTRL_F3 || ev.action == ACT_CTRL_F4 || ev.action == ACT_CTRL_F5 || ev.action == ACT_CTRL_F6) {
			SortMode new_mode = SORT_NAME;
			if (ev.action == ACT_CTRL_F3) new_mode = SORT_NAME;
			else if (ev.action == ACT_CTRL_F4) new_mode = SORT_EXT;
			else if (ev.action == ACT_CTRL_F5) new_mode = SORT_TIME;
			else if (ev.action == ACT_CTRL_F6) new_mode = SORT_SIZE;

			if (active_panel->sort_mode == new_mode) {
				active_panel->sort_desc = !active_panel->sort_desc;
			} else {
				active_panel->sort_mode = new_mode;
				active_panel->sort_desc = 0;
			}
			load_directory(active_panel);
		}
		else if (ev.action == ACT_F1) ui_help();
		else if (ev.action == ACT_ALT_F1) { quick_search_reset(); ui_drive_menu(&left_panel, 0); }
		else if (ev.action == ACT_F2) ui_user_menu();
		else if (ev.action == ACT_ALT_F2) { quick_search_reset(); ui_drive_menu(&right_panel, 1); }
		else if (ev.action == ACT_F3 && active_panel->count > 0) {
			FileEntry *cur = &active_panel->files[active_panel->selected];
			if (cur->is_dir) {
				panel_calc_dir_sizes(active_panel);
				if (active_panel->sort_mode == SORT_SIZE) {
					char keep[MAX_FILENAME];
					snprintf(keep, sizeof(keep), "%s", cur->name);
					snprintf(active_panel->parent_name, MAX_FILENAME, "%s", keep);
					load_directory(active_panel);
				}
				draw_screen(); refresh();
			} else if (strcmp(cur->name, "..") != 0) {
				if (active_panel->archive_file[0]) {
					char in[MAX_PATH*2], tfp[MAX_PATH];
					if (active_panel->archive_dir[0]) snprintf(in, sizeof(in), "%s/%s", active_panel->archive_dir, cur->name);
					else snprintf(in, sizeof(in), "%s", cur->name);
					if (archive_extract_one(active_panel->archive_file, in, tfp, sizeof(tfp))) {
						ui_view_file(tfp); unlink(tfp);
						char *sl = strrchr(tfp, '/'); if (sl) { *sl = '\0'; rmdir(tfp); }
					} else ui_message("Archive View", "Failed to extract file from archive");
				} else {
					char fp[MAX_PATH * 2];
					panel_entry_full_path(active_panel, active_panel->selected, fp, sizeof(fp));
					file_history_add_entry(NULL, "View", 0, 0, fp);
					file_history_save();
					ui_view_file(fp);
				}
			}
		} else if (ev.action == ACT_F4 && active_panel->count > 0) {
			FileEntry *cur = &active_panel->files[active_panel->selected];
			if (!cur->is_dir && strcmp(cur->name, "..") != 0) {
				if (active_panel->archive_file[0] != '\0') {
					char inner[MAX_PATH * 2], tfp[MAX_PATH];
					if (active_panel->archive_dir[0] != '\0') snprintf(inner, sizeof(inner), "%s/%s", active_panel->archive_dir, cur->name);
					else snprintf(inner, sizeof(inner), "%s", cur->name);
					if (archive_extract_one(active_panel->archive_file, inner, tfp, sizeof(tfp))) {
						struct stat st1, st2;
						int has_st1 = (stat(tfp, &st1) == 0);
						int end_ln = 1, end_cl = 1;
						ui_edit_file_ex(tfp, 1, 1, &end_ln, &end_cl);
						if (has_st1 && stat(tfp, &st2) == 0 && (st1.st_mtime != st2.st_mtime || st1.st_size != st2.st_size)) {
							char qmsg[MAX_FILENAME + 64];
							snprintf(qmsg, sizeof(qmsg), "Update \"%s\" in archive?", cur->name);
							if (ui_confirm("Archive Edit", qmsg)) {
								if (archive_update_one(active_panel->archive_file, inner, tfp)) {
									archive_load_dir(active_panel);
								} else {
									ui_message("Archive Edit", "Failed to update file in archive");
								}
							}
						}
						unlink(tfp);
						char *sl = strrchr(tfp, '/');
						if (sl) { *sl = '\0'; rmdir(tfp); }
					} else {
						ui_message("Archive Edit", "Failed to extract file from archive");
					}
					draw_screen(); refresh();
					continue;
				}
				char fp[MAX_PATH * 2];
				panel_entry_full_path(active_panel, active_panel->selected, fp, sizeof(fp));
				snprintf(active_panel->parent_name, MAX_FILENAME, "%s", cur->name);
				int start_ln = 1, start_cl = 1;
				file_history_get_pos(fp, &start_ln, &start_cl);
				int end_ln = start_ln, end_cl = start_cl;
				ui_edit_file_ex(fp, start_ln, start_cl, &end_ln, &end_cl);
				file_history_add_entry(NULL, "Edit", end_ln, end_cl, fp);
				file_history_save();
				load_directory(&left_panel);
				load_directory(&right_panel);
			}
		} else if (ev.action == ACT_SHIFT_F4) {
			if (create_and_edit_file(active_panel) > 0) {
				reload_panels_keep_cursor();
				if (active_panel->count > 0 && active_panel->selected >= 0 && active_panel->selected < active_panel->count) {
					FileEntry *ne = &active_panel->files[active_panel->selected];
					if (!ne->is_dir && strcmp(ne->name, "..") != 0) {
						char fp[MAX_PATH * 2];
						snprintf(fp, sizeof(fp), "%s/%s", active_panel->current_path, ne->name);
						file_history_add_entry(NULL, "Edit", 1, 1, fp);
						file_history_save();
					}
				}
			}
		}
		else if (ev.action == ACT_SHIFT_F5) {
			Panel *oth = (active_panel == &left_panel) ? &right_panel : &left_panel;
			char keep_name[MAX_FILENAME] = "";
			if (active_panel->count > 0) snprintf(keep_name, sizeof(keep_name), "%s", active_panel->files[active_panel->selected].name);
			if (copy_items_inplace(active_panel, oth) > 0) {
				for (int i = 0; i < active_panel->count; i++) active_panel->files[i].is_selected = 0;
				snprintf(active_panel->parent_name, MAX_FILENAME, "%s", keep_name);
				load_directory(&left_panel);
				load_directory(&right_panel);
			}
		}
		else if (ev.action == ACT_SHIFT_F6) {
			Panel *oth = (active_panel == &left_panel) ? &right_panel : &left_panel;
			int ps = active_panel->selected;
			if (move_items_inplace(active_panel, oth) > 0) {
				for (int i = 0; i < active_panel->count; i++) active_panel->files[i].is_selected = 0;
				load_directory(&left_panel);
				load_directory(&right_panel);
				if (active_panel->count > 0) active_panel->selected = (ps < active_panel->count) ? ps : (active_panel->count - 1);
			}
		}
		else if (ev.action == ACT_F5) {
			Panel *oth = (active_panel == &left_panel) ? &right_panel : &left_panel;
			if (oth->is_temp && !active_panel->is_temp && active_panel->archive_file[0] == '\0') {
				int marked = panel_marked_count(active_panel);
				int added = 0;
				for (int i = 0; i < active_panel->count; i++) {
					if (strcmp(active_panel->files[i].name, "..") == 0) continue;
					if ((marked > 0 && active_panel->files[i].is_selected) || (marked == 0 && i == active_panel->selected)) {
						char fp[MAX_PATH * 2];
						panel_entry_full_path(active_panel, i, fp, sizeof(fp));
						added += temp_panel_add(fp);
						active_panel->files[i].is_selected = 0;
					}
				}
				if (added > 0) load_directory(oth);
				draw_screen(); refresh();
				continue;
			}
			if (active_panel->archive_file[0]) {
				if (archive_extract_items(active_panel, oth) > 0) { load_directory(&left_panel); load_directory(&right_panel); }
				draw_screen(); refresh();
				continue;
			}
			char keep_name[MAX_FILENAME] = "";
			if (active_panel->count > 0) snprintf(keep_name, sizeof(keep_name), "%s", active_panel->files[active_panel->selected].name);
			if (copy_items(active_panel, oth) > 0) {
				for (int i = 0; i < active_panel->count; i++) active_panel->files[i].is_selected = 0;
				snprintf(active_panel->parent_name, MAX_FILENAME, "%s", keep_name);
				load_directory(&left_panel);
				load_directory(&right_panel);
			}
		}
		else if (ev.action == ACT_F6) { Panel *oth = (active_panel == &left_panel) ? &right_panel : &left_panel; int ps = active_panel->selected; if (move_items(active_panel, oth) > 0) { reload_panels_keep_cursor(); if (ps >= active_panel->count) ps = active_panel->count - 1; if (ps >= 0) active_panel->selected = ps; } }
		else if (ev.action == ACT_ALT_F7) {
			char found_fp[MAX_PATH * 2] = "";
			int fres = ui_find_files(active_panel->current_path, found_fp, sizeof(found_fp));
			if (fres == 1 && found_fp[0] != '\0') {
				char *sl = strrchr(found_fp, '/');
				if (sl) {
					const char *bn = sl + 1;
					if (sl == found_fp) strcpy(found_fp, "/");
					else *sl = '\0';
					active_panel->archive_file[0] = '\0';
					active_panel->archive_dir[0] = '\0';
					quick_search_reset();
					snprintf(active_panel->parent_name, MAX_FILENAME, "%s", bn);
					snprintf(active_panel->current_path, MAX_PATH, "%s", found_fp);
					load_directory(active_panel);
				}
			} else if (fres == 3) {
				temp_panel_open(active_panel);
			} else if (fres == 2 && found_fp[0] != '\0') {
				wchar_t wfp[MAX_PATH * 2];
				if (mbstowcs(wfp, found_fp, MAX_PATH * 2 - 1) != (size_t)-1) {
					wfp[MAX_PATH * 2 - 1] = L'\0';
					cmdline_insert_wstr(wfp);
				}
			}
			draw_screen(); refresh();
		}
		else if (ev.action == ACT_F7) { if (make_directory(active_panel) > 0) { reload_panels_keep_cursor(); } }
		else if (ev.action == ACT_SHIFT_F1 || ev.action == ACT_ALT_F5) {
			if (archive_create_from_panel(active_panel) > 0) {
				load_directory(&left_panel);
				load_directory(&right_panel);
			}
			draw_screen(); refresh();
		}
		else if (ev.action == ACT_CTRL_A) {
			if (ui_file_attributes(active_panel)) reload_panels_keep_cursor();
			draw_screen(); refresh();
		}
		else if (ev.action == ACT_F8) {
			if (active_panel->is_temp) {
				int ps = active_panel->selected;
				if (temp_panel_remove_selected(active_panel) > 0) {
					if (ps >= active_panel->count) ps = active_panel->count - 1;
					if (ps >= 0) active_panel->selected = ps;
				}
				draw_screen(); refresh();
				continue;
			}
			if (active_panel->archive_file[0] != '\0') {
				int ps = active_panel->selected;
				if (archive_delete_items(active_panel) > 0) {
					archive_load_dir(active_panel);
					if (ps >= active_panel->count) ps = active_panel->count - 1;
					if (ps >= 0) active_panel->selected = ps;
				}
				draw_screen(); refresh();
				continue;
			}
			int ps = active_panel->selected;
			if (trash_items(active_panel) > 0) { reload_panels_keep_cursor(); if (ps >= active_panel->count) ps = active_panel->count - 1; if (ps >= 0) active_panel->selected = ps; }
		}
		else if (ev.action == ACT_SHIFT_F8) {
			if (active_panel->archive_file[0] != '\0' || active_panel->is_temp) continue;
			int ps = active_panel->selected;
			if (delete_items(active_panel) > 0) { reload_panels_keep_cursor(); if (ps >= active_panel->count) ps = active_panel->count - 1; if (ps >= 0) active_panel->selected = ps; }
		}
		else if (ev.action == ACT_ALT_SHIFT_END && active_panel->count > 0) {
			for (int i = active_panel->selected; i < active_panel->count; i++) {
				if (strcmp(active_panel->files[i].name, "..") != 0) active_panel->files[i].is_selected = 1;
			}
			active_panel->selected = active_panel->count - 1;
		}
		else if (ev.action == ACT_ALT_SHIFT_HOME && active_panel->count > 0) {
			for (int i = active_panel->selected; i >= 0; i--) {
				if (strcmp(active_panel->files[i].name, "..") != 0) active_panel->files[i].is_selected = 1;
			}
			active_panel->selected = 0;
		}
		else if (ev.action == ACT_HOME && active_panel->count > 0) active_panel->selected = 0;
		else if (ev.action == ACT_END && active_panel->count > 0) active_panel->selected = active_panel->count - 1;
		else if (ev.action == ACT_SHIFT_UP) { panel_toggle_mark(active_panel, active_panel->selected); if (active_panel->selected > 0) active_panel->selected--; }
		else if (ev.action == ACT_SHIFT_DOWN || ev.action == ACT_INSERT) { panel_toggle_mark(active_panel, active_panel->selected); if (active_panel->selected < active_panel->count - 1) active_panel->selected++; }
		else if (ev.action == ACT_SHIFT_LEFT && active_panel->count > 0) {
			int h, w; getmaxyx(stdscr, h, w); (void)w;
			int vis = h - 6; if (vis < 1) vis = 1;
			int cur = active_panel->selected;
			int target = cur - vis;
			if (target < 0) target = 0;
			int stop = (target == 0) ? 0 : (target + 1);
			for (int i = cur; i >= stop; i--) panel_toggle_mark(active_panel, i);
			active_panel->selected = target;
		}
		else if (ev.action == ACT_SHIFT_RIGHT && active_panel->count > 0) {
			int h, w; getmaxyx(stdscr, h, w); (void)w;
			int vis = h - 6; if (vis < 1) vis = 1;
			int cur = active_panel->selected;
			int target = cur + vis;
			if (target >= active_panel->count) target = active_panel->count - 1;
			int stop = (target == active_panel->count - 1) ? target : (target - 1);
			for (int i = cur; i <= stop; i++) panel_toggle_mark(active_panel, i);
			active_panel->selected = target;
		}
		else if (ev.action == ACT_UP && active_panel->selected > 0) active_panel->selected--;
		else if (ev.action == ACT_DOWN && active_panel->selected < active_panel->count - 1) active_panel->selected++;
		else if (ev.action == ACT_LEFT || ev.action == ACT_PGUP) { int h, w; getmaxyx(stdscr, h, w); (void)w; int vis = h - 6; if (vis < 1) vis = 1; active_panel->selected -= vis; if (active_panel->selected < 0) active_panel->selected = 0; }
		else if (ev.action == ACT_RIGHT || ev.action == ACT_PGDN) { int h, w; getmaxyx(stdscr, h, w); (void)w; int vis = h - 6; if (vis < 1) vis = 1; active_panel->selected += vis; if (active_panel->selected >= active_panel->count) active_panel->selected = (active_panel->count > 0) ? (active_panel->count - 1) : 0; }
		else if (ev.action == ACT_SHIFT_ENTER && active_panel->count > 0) {
			FileEntry *cur = &active_panel->files[active_panel->selected];
			char target[MAX_PATH * 2];
			if (strcmp(cur->name, "..") == 0) snprintf(target, sizeof(target), "%s", active_panel->current_path);
			else if (strcmp(active_panel->current_path, "/") == 0) snprintf(target, sizeof(target), "/%s", cur->name);
			else snprintf(target, sizeof(target), "%s/%s", active_panel->current_path, cur->name);
			char ocmd[MAX_PATH * 2 + 64];
			snprintf(ocmd, sizeof(ocmd), "nohup xdg-open \"%s\" >/dev/null 2>&1 &", target);
			int rc = system(ocmd);
			(void)rc;
		}
		else if (ev.action == ACT_ENTER && active_panel->count > 0) {
			FileEntry *cur = &active_panel->files[active_panel->selected];
			if (cur->is_dir) { quick_search_reset(); change_directory(active_panel, cur->name); }
			else if (!active_panel->archive_file[0] && archive_is_supported(cur->name) && !(cur->mode & (S_IXUSR|S_IXGRP|S_IXOTH))) {
				quick_search_reset();
				if (!strcmp(active_panel->current_path, "/")) snprintf(active_panel->archive_file, MAX_PATH, "/%s", cur->name);
				else snprintf(active_panel->archive_file, MAX_PATH, "%s/%s", active_panel->current_path, cur->name);
				active_panel->archive_dir[0] = '\0'; active_panel->parent_name[0] = '\0';
				archive_load_dir(active_panel);
			}
			else {
				char fp[MAX_PATH * 2];
				if (strcmp(active_panel->current_path, "/") == 0) snprintf(fp, sizeof(fp), "/%s", cur->name);
				else snprintf(fp, sizeof(fp), "%s/%s", active_panel->current_path, cur->name);
				if (access(fp, X_OK) == 0) { char rc[MAX_FILENAME + 16]; snprintf(rc, sizeof(rc), "./\"%s\"", cur->name); execute_system_command(rc); reload_panels_keep_cursor(); }
			}
		} else if (ev.action == ACT_CTRL_PGUP) {
			if (active_panel->is_temp && active_panel->count > 0 && active_panel->selected > 0) {
				const FileEntry *te = &active_panel->files[active_panel->selected];
				if (te->full_path[0] != '\0') {
					char dir_buf[MAX_PATH];
					snprintf(dir_buf, sizeof(dir_buf), "%s", te->full_path);
					char *sl = strrchr(dir_buf, '/');
					if (sl) {
						const char *bn = sl + 1;
						if (sl == dir_buf) strcpy(dir_buf, "/");
						else *sl = '\0';
						active_panel->is_temp = 0;
						snprintf(active_panel->parent_name, MAX_FILENAME, "%s", bn);
						snprintf(active_panel->current_path, MAX_PATH, "%s", dir_buf);
						load_directory(active_panel);
						draw_screen(); refresh();
						continue;
					}
				}
			}
			change_directory(active_panel, "..");
		}
		else if (ev.action == ACT_CTRL_PGDN && active_panel->count > 0) {
			FileEntry *cur = &active_panel->files[active_panel->selected];
			if (cur->is_dir) { quick_search_reset(); change_directory(active_panel, cur->name); }
			else if (!active_panel->archive_file[0] && strcmp(cur->name, "..") != 0) {
				quick_search_reset();
				if (!strcmp(active_panel->current_path, "/")) snprintf(active_panel->archive_file, MAX_PATH, "/%s", cur->name);
				else snprintf(active_panel->archive_file, MAX_PATH, "%s/%s", active_panel->current_path, cur->name);
				active_panel->archive_dir[0] = '\0'; active_panel->parent_name[0] = '\0';
				archive_load_dir(active_panel);
			}
		}
		if (!terminal_mode) { draw_screen(); refresh(); }
	}
	history_save();
	dir_history_save();
	file_history_save();
	state_save();
	input_restore_sane();
	endwin();
	input_restore_sane();
	return 0;
}
