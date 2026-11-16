#define _GNU_SOURCE
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
#include "ui.h"
#include "exec.h"
#include "input.h"
#include "viewer.h"

Panel left_panel, right_panel;
Panel *active_panel = NULL;

char cmd_buffer[MAX_CMD] = "";
int cmd_pos = 0;
int cmd_len = 0;
int cmd_sel_start = -1;
int cmd_sel_end = -1;
static int cmd_sel_anchor = -1;
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

#define MAX_HISTORY 64
static char cmd_history[MAX_HISTORY][MAX_CMD];
static int history_count = 0;
static int history_idx = 0;
static void insert_current_name_to_cmd(wchar_t *wcmd) {
	if (!active_panel || active_panel->count == 0) return;
	const char *name = active_panel->files[active_panel->selected].name;
	if (!name || strcmp(name, "..") == 0) return;

	char formatted[MAX_FILENAME + 8];
	if (strchr(name, ' ') != NULL) {
		snprintf(formatted, sizeof(formatted), "\"%s\" ", name);
	} else {
		snprintf(formatted, sizeof(formatted), "%s ", name);
	}

	wchar_t wadd[MAX_FILENAME + 8];
	size_t add_len = mbstowcs(wadd, formatted, MAX_FILENAME + 7);
	if (add_len == (size_t)-1) return;
	wadd[add_len] = L'\0';

	if (cmd_pos > 0 && wcmd[cmd_pos - 1] != L' ' && cmd_len < MAX_CMD - 2) {
		wmemmove(wcmd + cmd_pos + 1, wcmd + cmd_pos, (size_t)(cmd_len - cmd_pos + 1));
		wcmd[cmd_pos++] = L' ';
		cmd_len++;
	}

	if (cmd_len + (int)add_len < MAX_CMD - 1) {
		wmemmove(wcmd + cmd_pos + add_len, wcmd + cmd_pos, (size_t)(cmd_len - cmd_pos + 1));
		wmemcpy(wcmd + cmd_pos, wadd, add_len);
		cmd_pos += (int)add_len;
		cmd_len += (int)add_len;
	}

	wcstombs(cmd_buffer, wcmd, sizeof(cmd_buffer));
	cmd_buffer[sizeof(cmd_buffer) - 1] = '\0';
}

static void history_add(const char *cmd) {
	if (!cmd || *cmd == '\0') return;
	if (history_count > 0 && strcmp(cmd_history[history_count - 1], cmd) == 0) {
		history_idx = history_count;
		return;
	}
	if (history_count < MAX_HISTORY) {
		snprintf(cmd_history[history_count], MAX_CMD, "%s", cmd);
		history_count++;
	} else {
		memmove(cmd_history, cmd_history + 1, sizeof(cmd_history[0]) * (MAX_HISTORY - 1));
		snprintf(cmd_history[MAX_HISTORY - 1], MAX_CMD, "%s", cmd);
	}
	history_idx = history_count;
}

static void history_apply(wchar_t *wcmd) {
	if (history_idx < 0) history_idx = 0;
	if (history_idx >= history_count) {
		history_idx = history_count;
		*wcmd = L'\0';
		*cmd_buffer = '\0';
		cmd_pos = 0;
		cmd_len = 0;
		return;
	}
	snprintf(cmd_buffer, sizeof(cmd_buffer), "%s", cmd_history[history_idx]);
	size_t wlen = mbstowcs(wcmd, cmd_buffer, MAX_CMD - 1);
	if (wlen == (size_t)-1) {
		*wcmd = L'\0';
		cmd_pos = 0;
		cmd_len = 0;
	} else {
		wcmd[wlen] = L'\0';
		cmd_pos = (int)wlen;
		cmd_len = (int)wlen;
	}
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

	char line[MAX_CMD];
	while (fgets(line, sizeof(line), f)) {
		size_t len = strlen(line);
		while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
			line[--len] = '\0';
		}
		if (len > 0) {
			history_add(line);
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
		fprintf(f, "%s\n", cmd_history[i]);
	}
	fclose(f);
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
	fprintf(f, "%s\n%s\n%d\n",
		left_panel.current_path,
		right_panel.current_path,
		(active_panel == &right_panel) ? 1 : 0);
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
	fclose(f);
	size_t l1 = strlen(b1); while (l1 > 0 && (b1[l1 - 1] == '\n' || b1[l1 - 1] == '\r')) b1[--l1] = '\0';
	size_t l2 = strlen(b2); while (l2 > 0 && (b2[l2 - 1] == '\n' || b2[l2 - 1] == '\r')) b2[--l2] = '\0';
	if (l1 > 0) snprintf(lpath, lsize, "%s", b1);
	if (l2 > 0) snprintf(rpath, rsize, "%s", b2);
	return (l1 > 0 && l2 > 0);
}

static void print_term_prompt_ex(const wchar_t *wcmd, int with_header) {
	const char *path = (active_panel && *active_panel->current_path) ? active_panel->current_path : ".";
	char prompt_str[MAX_PATH + 256];
	format_custom_prompt(path, prompt_str, sizeof(prompt_str));
	char *last_nl = strrchr(prompt_str, '\n');
	if (last_nl) {
		*last_nl = '\0';
		if (with_header) {
			const char *hdr = prompt_str;
			while (*hdr == '\n') { printf("\r\n"); hdr++; }
			printf("\r\033[K%s\r\n", hdr);
		}
		printf("\r\033[K%s", last_nl + 1);
	} else {
		printf("\r\033[K%s", prompt_str);
	}
	if (wcmd && cmd_sel_start >= 0 && cmd_sel_end > cmd_sel_start && cmd_sel_end <= cmd_len) {
		wchar_t part[MAX_CMD];
		char utf[MAX_CMD];

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
		printf("%s", cmd_buffer);
	}
	if (cmd_len > cmd_pos) {
		printf("\033[%dD", cmd_len - cmd_pos);
	}
	fflush(stdout);
}

static void print_term_prompt(const wchar_t *wcmd) {
	print_term_prompt_ex(wcmd, 0);
}

static int confirm_exit(const wchar_t *wcmd) {
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
		print_term_prompt(wcmd);
	}
	return res;
}

static int is_word_sep(wchar_t ch) {
	return iswspace(ch) || ch == L'/' || ch == L'-' || ch == L'_' || ch == L'.';
}

static int word_jump_left(const wchar_t *buf, int pos) {
	while (pos > 0 && is_word_sep(buf[pos - 1])) pos--;
	while (pos > 0 && !is_word_sep(buf[pos - 1])) pos--;
	return pos;
}

static int word_jump_right(const wchar_t *buf, int pos, int len) {
	while (pos < len && !is_word_sep(buf[pos])) pos++;
	while (pos < len && is_word_sep(buf[pos])) pos++;
	return pos;
}

static void cmd_update_sel(void) {
	if (cmd_sel_anchor >= 0 && cmd_sel_anchor != cmd_pos) {
		cmd_sel_start = (cmd_sel_anchor < cmd_pos) ? cmd_sel_anchor : cmd_pos;
		cmd_sel_end = (cmd_sel_anchor > cmd_pos) ? cmd_sel_anchor : cmd_pos;
	} else {
		cmd_sel_start = -1;
		cmd_sel_end = -1;
	}
}

static void cmd_delete_sel(wchar_t *wcmd) {
	if (cmd_sel_start >= 0 && cmd_sel_end > cmd_sel_start) {
		wmemmove(wcmd + cmd_sel_start, wcmd + cmd_sel_end, (size_t)(cmd_len - cmd_sel_end + 1));
		cmd_len -= (cmd_sel_end - cmd_sel_start);
		cmd_pos = cmd_sel_start;
	}
	cmd_sel_anchor = -1;
	cmd_sel_start = -1;
	cmd_sel_end = -1;
}

static void clipboard_paste_to_cmd(wchar_t *wcmd) {
	char raw[MAX_CMD] = {0};
	if (clipboard_paste(raw, sizeof(raw)) == 0) return;

	wchar_t wadd[MAX_CMD];
	size_t add_len = mbstowcs(wadd, raw, MAX_CMD - 1);
	if (add_len == (size_t)-1 || add_len == 0) return;
	wadd[add_len] = L'\0';

	if (cmd_sel_start >= 0) cmd_delete_sel(wcmd);

	if (cmd_len + (int)add_len >= MAX_CMD - 1) {
		add_len = (size_t)(MAX_CMD - 1 - cmd_len);
	}
	if (add_len == 0) return;

	wmemmove(wcmd + cmd_pos + add_len, wcmd + cmd_pos, (size_t)(cmd_len - cmd_pos + 1));
	wmemcpy(wcmd + cmd_pos, wadd, add_len);
	cmd_pos += (int)add_len;
	cmd_len += (int)add_len;

	wcstombs(cmd_buffer, wcmd, sizeof(cmd_buffer));
	cmd_buffer[sizeof(cmd_buffer) - 1] = '\0';
}

static void clipboard_copy_from_cmd(const wchar_t *wcmd) {
	if (wcmd && cmd_sel_start >= 0 && cmd_sel_end > cmd_sel_start) {
		wchar_t wsub[MAX_CMD];
		int slen = cmd_sel_end - cmd_sel_start;
		wmemcpy(wsub, wcmd + cmd_sel_start, (size_t)slen);
		wsub[slen] = L'\0';
		char utf_sub[MAX_CMD];
		if (wcstombs(utf_sub, wsub, sizeof(utf_sub)) != (size_t)-1) {
			utf_sub[sizeof(utf_sub) - 1] = '\0';
			clipboard_copy(utf_sub);
		}
	} else if (cmd_len > 0) {
		clipboard_copy(cmd_buffer);
	} else if (!terminal_mode && active_panel && active_panel->count > 0) {
		const char *name = active_panel->files[active_panel->selected].name;
		char full[MAX_PATH * 2];
		if (strcmp(name, "..") == 0) {
			snprintf(full, sizeof(full), "%s", active_panel->current_path);
		} else if (strcmp(active_panel->current_path, "/") == 0) {
			snprintf(full, sizeof(full), "/%s", name);
		} else {
			snprintf(full, sizeof(full), "%s/%s", active_panel->current_path, name);
		}
		clipboard_copy(full);
	}
}

static void insert_panel_path_to_cmd(wchar_t *wcmd, const Panel *p) {
	if (!p || !*p->current_path) return;
	char formatted[MAX_PATH + 8];
	if (strchr(p->current_path, ' ') != NULL) {
		snprintf(formatted, sizeof(formatted), "\"%s\" ", p->current_path);
	} else if (strcmp(p->current_path, "/") == 0) {
		snprintf(formatted, sizeof(formatted), "/");
	} else {
		snprintf(formatted, sizeof(formatted), "%s/", p->current_path);
	}

	wchar_t wadd[MAX_PATH + 8];
	size_t add_len = mbstowcs(wadd, formatted, MAX_PATH + 7);
	if (add_len == (size_t)-1 || add_len == 0) return;
	wadd[add_len] = L'\0';

	if (cmd_sel_start >= 0) cmd_delete_sel(wcmd);

	if (cmd_pos > 0 && wcmd[cmd_pos - 1] != L' ' && cmd_len < MAX_CMD - 2) {
		wmemmove(wcmd + cmd_pos + 1, wcmd + cmd_pos, (size_t)(cmd_len - cmd_pos + 1));
		wcmd[cmd_pos++] = L' ';
		cmd_len++;
	}

	if (cmd_len + (int)add_len < MAX_CMD - 1) {
		wmemmove(wcmd + cmd_pos + add_len, wcmd + cmd_pos, (size_t)(cmd_len - cmd_pos + 1));
		wmemcpy(wcmd + cmd_pos, wadd, add_len);
		cmd_pos += (int)add_len;
		cmd_len += (int)add_len;
	}

	wcstombs(cmd_buffer, wcmd, sizeof(cmd_buffer));
	cmd_buffer[sizeof(cmd_buffer) - 1] = '\0';
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

int main(int argc, char **argv) {
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
					if (strcmp(terms[i], "gnome-terminal") == 0) {
						execlp(terms[i], terms[i], "--", self, (argc > 1 ? argv[1] : NULL), (char *)NULL);
					} else {
						execlp(terms[i], terms[i], "-e", self, (argc > 1 ? argv[1] : NULL), (char *)NULL);
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

	char cwd[MAX_PATH];
	if (getcwd(cwd, sizeof(cwd)) == NULL) strcpy(cwd, ".");
	char saved_l[MAX_PATH] = {0}, saved_r[MAX_PATH] = {0};
	int act_right = 0;
	int has_state = state_load(saved_l, sizeof(saved_l), saved_r, sizeof(saved_r), &act_right);

	if (argc > 1 && argv[1][0] != '\0') {
		snprintf(left_panel.current_path, MAX_PATH, "%s", argv[1]);
		if (has_state && access(saved_r, R_OK | X_OK) == 0) snprintf(right_panel.current_path, MAX_PATH, "%s", saved_r);
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

	load_directory(&left_panel);
	load_directory(&right_panel);
	active_panel = act_right ? &right_panel : &left_panel;

	clear();
	refresh();
	draw_screen();
	refresh();
	input_enable_raw();

	wchar_t wcmd[MAX_CMD] = L"";
	while (1) {
		InputEvent ev = input_read();
		if (ev.action == ACT_NONE) continue;
		if (ev.action >= ACT_MOUSE_LEFT && ev.action <= ACT_MOUSE_WHEEL_DOWN) {
			ev.action = handle_mouse_event(ev);
			if (ev.action == ACT_NONE) continue;
		}
		if (ev.action == ACT_RESIZE) {
			if (!terminal_mode) { endwin(); refresh(); clearok(stdscr, TRUE); draw_screen(); refresh(); }
			continue;
		}
		if (ev.action == ACT_CTRL_Q || ev.action == ACT_F10) {
			if (confirm_exit(wcmd)) break;
			if (!terminal_mode) { draw_screen(); refresh(); }
			continue;
		}
		if (ev.action == ACT_CTRL_C || ev.action == ACT_CTRL_Y) {
			if (!terminal_mode && cmd_len == 0 && cmd_sel_start < 0) clipboard_stage_files(active_panel, 0);
			else clipboard_copy_from_cmd(wcmd);
			continue;
		}
		if (ev.action == ACT_CTRL_X && (cmd_sel_start >= 0 || (!terminal_mode && cmd_len == 0))) {
			if (cmd_sel_start >= 0) {
				clipboard_copy_from_cmd(wcmd); cmd_delete_sel(wcmd);
				wcstombs(cmd_buffer, wcmd, sizeof(cmd_buffer)); cmd_buffer[sizeof(cmd_buffer) - 1] = '\0';
				if (!terminal_mode) { draw_screen(); refresh(); } else { print_term_prompt(wcmd); }
			} else clipboard_stage_files(active_panel, 1);
			continue;
		}
		if (ev.action == ACT_CTRL_V || ev.action == ACT_SHIFT_INSERT) {
			if (!terminal_mode && cmd_len == 0 && clipboard_has_files()) {
				if (clipboard_paste_files(active_panel) > 0) { load_directory(&left_panel); load_directory(&right_panel); }
				draw_screen(); refresh();
			} else {
				char raw_clip[65536] = {0};
				if (clipboard_paste_raw(raw_clip, sizeof(raw_clip)) > 0 && strchr(raw_clip, '\n') != NULL) {
					int was_term = terminal_mode;
					if (was_term) { reset_prog_mode(); refresh(); }
					int ok = ui_multiline_paste(raw_clip);
					if (was_term) { endwin(); input_enable_raw(); }
					if (ok) {
						char tmp_sh[128];
						snprintf(tmp_sh, sizeof(tmp_sh), "/tmp/xc_paste_%d.sh", (int)getpid());
						FILE *sf = fopen(tmp_sh, "w");
						if (sf) {
							fputs("printf \"\\n\\033[1;36m%s\\033[0m\\n\" \"$(printf '═%.0s' $(seq 1 ${COLUMNS:-80}))\"\n", sf);
							fputs("printf \"\\033[1;33m▶ [ $(date +%H:%M:%S) | $PWD ]\\033[0m\\n\"\n", sf);
							fputs(raw_clip, sf);
							fputs("\nrc=$?\nif [ $rc -ne 0 ]; then\n  printf \"\\n\\033[1;31m--- Error (exit: $rc). Press any key... ---\\033[0m\"\n  read -n 1 -s -r\nfi\n", sf);
							fclose(sf);
							char run_sh[160];
							snprintf(run_sh, sizeof(run_sh), ". \"%s\"", tmp_sh);
							if (was_term) printf("\r\n");
							execute_system_command(run_sh);
							unlink(tmp_sh);
							load_directory(&left_panel);
							load_directory(&right_panel);
						}
					}
					if (!was_term) { draw_screen(); refresh(); }
					else { print_term_prompt_ex(wcmd, 1); }
				} else {
					clipboard_paste_to_cmd(wcmd);
					if (!terminal_mode) { draw_screen(); refresh(); } else { print_term_prompt(wcmd); }
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
			if (terminal_mode) { def_prog_mode(); endwin(); input_enable_raw(); print_term_prompt_ex(wcmd, 1); }
			else { reset_prog_mode(); input_enable_raw(); clearok(stdscr, TRUE); load_directory(&left_panel); load_directory(&right_panel); refresh(); draw_screen(); refresh(); }
			continue;
		}
		if (ev.action == ACT_INSERT_LEFT_PATH) { insert_panel_path_to_cmd(wcmd, &left_panel); if (!terminal_mode) { draw_screen(); refresh(); } else print_term_prompt(wcmd); continue; }
		if (ev.action == ACT_INSERT_RIGHT_PATH) { insert_panel_path_to_cmd(wcmd, &right_panel); if (!terminal_mode) { draw_screen(); refresh(); } else print_term_prompt(wcmd); continue; }
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
		if (ev.action == ACT_INSERT_NAME) { insert_current_name_to_cmd(wcmd); if (!terminal_mode) { draw_screen(); refresh(); } else print_term_prompt(wcmd); continue; }
		int is_hist_prev = (ev.action == ACT_CTRL_E) || (terminal_mode && ev.action == ACT_UP);
		int is_hist_next = (ev.action == ACT_CTRL_X) || (terminal_mode && ev.action == ACT_DOWN);
		if (is_hist_prev || is_hist_next) {
			if (history_count > 0) {
				if (is_hist_prev && history_idx > 0) history_idx--;
				else if (is_hist_next && history_idx < history_count) history_idx++;
				cmd_sel_anchor = -1; cmd_update_sel(); history_apply(wcmd);
				if (!terminal_mode) { draw_screen(); refresh(); } else print_term_prompt(wcmd);
			}
			continue;
		}
		int is_printable = (ev.action == ACT_CHAR && iswprint(ev.ch) && !(cmd_len == 0 && ev.ch == L' '));
		int is_cursor_move = (cmd_len > 0 && (ev.action == ACT_HOME || ev.action == ACT_END || ev.action == ACT_CTRL_A || ev.action == ACT_SHIFT_LEFT || ev.action == ACT_SHIFT_RIGHT || ev.action == ACT_CTRL_LEFT || ev.action == ACT_CTRL_RIGHT || ev.action == ACT_WORD_SEL_LEFT || ev.action == ACT_WORD_SEL_RIGHT || ev.action == ACT_CTRL_B || ev.action == ACT_CTRL_F || (terminal_mode && (ev.action == ACT_LEFT || ev.action == ACT_RIGHT))));
		int is_editing = (cmd_len > 0 && (ev.action == ACT_BACKSPACE || ev.action == ACT_DELETE || ev.action == ACT_ENTER || ev.action == ACT_CTRL_U || ev.action == ACT_ESC));
		if (is_printable || is_cursor_move || is_editing) {
			if (is_cursor_move) {
				if (ev.action == ACT_HOME || ev.action == ACT_CTRL_A) { cmd_sel_anchor = -1; cmd_update_sel(); cmd_pos = 0; }
				else if (ev.action == ACT_END) { cmd_sel_anchor = -1; cmd_update_sel(); cmd_pos = cmd_len; }
				else if (ev.action == ACT_SHIFT_LEFT) { if (cmd_sel_anchor == -1) cmd_sel_anchor = cmd_pos; if (cmd_pos > 0) cmd_pos--; cmd_update_sel(); }
				else if (ev.action == ACT_SHIFT_RIGHT) { if (cmd_sel_anchor == -1) cmd_sel_anchor = cmd_pos; if (cmd_pos < cmd_len) cmd_pos++; cmd_update_sel(); }
				else if (ev.action == ACT_WORD_SEL_LEFT) { if (cmd_sel_anchor == -1) cmd_sel_anchor = cmd_pos; cmd_pos = word_jump_left(wcmd, cmd_pos); cmd_update_sel(); }
				else if (ev.action == ACT_WORD_SEL_RIGHT) { if (cmd_sel_anchor == -1) cmd_sel_anchor = cmd_pos; cmd_pos = word_jump_right(wcmd, cmd_pos, cmd_len); cmd_update_sel(); }
				else if (ev.action == ACT_CTRL_LEFT) { cmd_sel_anchor = -1; cmd_update_sel(); cmd_pos = word_jump_left(wcmd, cmd_pos); }
				else if (ev.action == ACT_CTRL_RIGHT) { cmd_sel_anchor = -1; cmd_update_sel(); cmd_pos = word_jump_right(wcmd, cmd_pos, cmd_len); }
				else if (ev.action == ACT_LEFT || ev.action == ACT_CTRL_B) { cmd_sel_anchor = -1; cmd_update_sel(); if (cmd_pos > 0) cmd_pos--; }
				else if (ev.action == ACT_RIGHT || ev.action == ACT_CTRL_F) { cmd_sel_anchor = -1; cmd_update_sel(); if (cmd_pos < cmd_len) cmd_pos++; }
			} else if (ev.action == ACT_BACKSPACE) {
				if (cmd_sel_start >= 0) cmd_delete_sel(wcmd);
				else if (cmd_pos > 0) { wmemmove(wcmd + cmd_pos - 1, wcmd + cmd_pos, (size_t)(cmd_len - cmd_pos + 1)); cmd_pos--; cmd_len--; }
			} else if (ev.action == ACT_DELETE) {
				if (cmd_sel_start >= 0) cmd_delete_sel(wcmd);
				else if (cmd_pos < cmd_len) { wmemmove(wcmd + cmd_pos, wcmd + cmd_pos + 1, (size_t)(cmd_len - cmd_pos)); cmd_len--; }
			} else if (ev.action == ACT_CTRL_U || ev.action == ACT_ESC) {
				cmd_sel_anchor = -1; cmd_update_sel(); cmd_pos = 0; cmd_len = 0; *wcmd = L'\0'; *cmd_buffer = '\0'; history_idx = history_count;
			} else if (ev.action == ACT_ENTER) {
				if (terminal_mode) { printf("\r\n"); fflush(stdout); }
				if (cmd_len > 0) {
					wcstombs(cmd_buffer, wcmd, sizeof(cmd_buffer)); cmd_buffer[sizeof(cmd_buffer) - 1] = '\0';
					if (strcmp(cmd_buffer, "exit") == 0 || strcmp(cmd_buffer, "quit") == 0) {
						if (confirm_exit(wcmd)) break;
						cmd_pos = 0; cmd_len = 0; *wcmd = L'\0'; *cmd_buffer = '\0';
						if (!terminal_mode) { draw_screen(); refresh(); } else print_term_prompt(wcmd);
						continue;
					}
					history_add(cmd_buffer); history_save(); execute_system_command(cmd_buffer);
					if (!terminal_mode) { load_directory(&left_panel); load_directory(&right_panel); }
				}
				cmd_pos = 0; cmd_len = 0; *wcmd = L'\0'; *cmd_buffer = '\0';
			} else if (is_printable && cmd_len < MAX_CMD - 1) {
				if (cmd_sel_start >= 0) cmd_delete_sel(wcmd);
				wmemmove(wcmd + cmd_pos + 1, wcmd + cmd_pos, (size_t)(cmd_len - cmd_pos + 1));
				wcmd[cmd_pos++] = ev.ch; cmd_len++;
			}
			wcstombs(cmd_buffer, wcmd, sizeof(cmd_buffer)); cmd_buffer[sizeof(cmd_buffer) - 1] = '\0';
			if (!terminal_mode) { draw_screen(); refresh(); } else print_term_prompt_ex(wcmd, ev.action == ACT_ENTER ? 1 : 0);
			continue;
		}
		if (terminal_mode) {
			continue;
		}
		if (ev.action == ACT_TAB) active_panel = (active_panel == &left_panel) ? &right_panel : &left_panel;
		else if (ev.action == ACT_F1) ui_help();
		else if (ev.action == ACT_ALT_F1) { quick_search_reset(); ui_drive_menu(&left_panel, 0); }
		else if (ev.action == ACT_F2) ui_user_menu();
		else if (ev.action == ACT_ALT_F2) { quick_search_reset(); ui_drive_menu(&right_panel, 1); }
		else if (ev.action == ACT_F3 && active_panel->count > 0) {
			FileEntry *cur = &active_panel->files[active_panel->selected];
			if (!cur->is_dir && strcmp(cur->name, "..") != 0) { char fp[MAX_PATH * 2]; snprintf(fp, sizeof(fp), "%s/%s", active_panel->current_path, cur->name); ui_view_file(fp); }
		} else if (ev.action == ACT_F4 && active_panel->count > 0) {
			FileEntry *cur = &active_panel->files[active_panel->selected];
			if (!cur->is_dir && strcmp(cur->name, "..") != 0) { char fp[MAX_PATH * 2]; snprintf(fp, sizeof(fp), "%s/%s", active_panel->current_path, cur->name); edit_file(fp); load_directory(&left_panel); load_directory(&right_panel); }
		} else if (ev.action == ACT_SHIFT_F4) { if (create_and_edit_file(active_panel) > 0) { load_directory(&left_panel); load_directory(&right_panel); } }
		else if (ev.action == ACT_F5) { Panel *oth = (active_panel == &left_panel) ? &right_panel : &left_panel; if (copy_items(active_panel, oth) > 0) { load_directory(&left_panel); load_directory(&right_panel); } }
		else if (ev.action == ACT_F6) { Panel *oth = (active_panel == &left_panel) ? &right_panel : &left_panel; int ps = active_panel->selected; if (move_items(active_panel, oth) > 0) { load_directory(&left_panel); load_directory(&right_panel); if (ps >= active_panel->count) ps = active_panel->count - 1; if (ps >= 0) active_panel->selected = ps; } }
		else if (ev.action == ACT_F7) { if (make_directory(active_panel) > 0) { load_directory(&left_panel); load_directory(&right_panel); } }
		else if (ev.action == ACT_F8) { int ps = active_panel->selected; if (delete_items(active_panel) > 0) { load_directory(&left_panel); load_directory(&right_panel); if (ps >= active_panel->count) ps = active_panel->count - 1; if (ps >= 0) active_panel->selected = ps; } }
		else if (ev.action == ACT_SHIFT_UP) { panel_toggle_mark(active_panel, active_panel->selected); if (active_panel->selected > 0) active_panel->selected--; }
		else if (ev.action == ACT_SHIFT_DOWN || ev.action == ACT_INSERT) { panel_toggle_mark(active_panel, active_panel->selected); if (active_panel->selected < active_panel->count - 1) active_panel->selected++; }
		else if (ev.action == ACT_UP && active_panel->selected > 0) active_panel->selected--;
		else if (ev.action == ACT_DOWN && active_panel->selected < active_panel->count - 1) active_panel->selected++;
		else if (ev.action == ACT_LEFT || ev.action == ACT_PGUP) { int h, w; getmaxyx(stdscr, h, w); (void)w; int vis = h - 6; if (vis < 1) vis = 1; active_panel->selected -= vis; if (active_panel->selected < 0) active_panel->selected = 0; }
		else if (ev.action == ACT_RIGHT || ev.action == ACT_PGDN) { int h, w; getmaxyx(stdscr, h, w); (void)w; int vis = h - 6; if (vis < 1) vis = 1; active_panel->selected += vis; if (active_panel->selected >= active_panel->count) active_panel->selected = (active_panel->count > 0) ? (active_panel->count - 1) : 0; }
		else if (ev.action == ACT_ENTER && active_panel->count > 0) {
			FileEntry *cur = &active_panel->files[active_panel->selected];
			if (cur->is_dir) { quick_search_reset(); change_directory(active_panel, cur->name); }
			else {
				char fp[MAX_PATH * 2];
				if (strcmp(active_panel->current_path, "/") == 0) snprintf(fp, sizeof(fp), "/%s", cur->name);
				else snprintf(fp, sizeof(fp), "%s/%s", active_panel->current_path, cur->name);
				if (access(fp, X_OK) == 0) { char rc[MAX_FILENAME + 16]; snprintf(rc, sizeof(rc), "./\"%s\"", cur->name); execute_system_command(rc); load_directory(&left_panel); load_directory(&right_panel); }
			}
		} else if (ev.action == ACT_CTRL_PGUP) change_directory(active_panel, "..");
		else if (ev.action == ACT_CTRL_PGDN && active_panel->count > 0) { FileEntry *cur = &active_panel->files[active_panel->selected]; if (cur->is_dir) change_directory(active_panel, cur->name); }
		if (!terminal_mode) { draw_screen(); refresh(); }
	}
	history_save();
	state_save();
	input_restore_sane();
	endwin();
	return 0;
}
