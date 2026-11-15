#define _GNU_SOURCE
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

Panel left_panel, right_panel;
Panel *active_panel = NULL;

char cmd_buffer[MAX_CMD] = "";
int cmd_pos = 0;
int cmd_len = 0;
int cmd_sel_start = -1;
int cmd_sel_end = -1;
static int cmd_sel_anchor = -1;
int terminal_mode = 0;

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


static void print_term_prompt(const wchar_t *wcmd) {
	const char *path = (active_panel && *active_panel->current_path) ? active_panel->current_path : ".";
	printf("\r\033[K%s$ ", path);
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

int main(int argc, char **argv) {
	if (!isatty(STDIN_FILENO)) {
		setenv("TERM", "xterm-256color", 1);
		char self[MAX_PATH];
		ssize_t len = readlink("/proc/self/exe", self, sizeof(self) - 1);
		if (len > 0) {
			self[len] = '\0';
			const char *terms[] = {"xterm", "kitty", "alacritty", "konsole", "xfce4-terminal", "gnome-terminal", NULL};
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
	if (argc > 1 && argv[1][0] != '\0') {
		snprintf(left_panel.current_path, MAX_PATH, "%s", argv[1]);
		snprintf(right_panel.current_path, MAX_PATH, "%s", argv[1]);
	} else if (getcwd(cwd, sizeof(cwd)) != NULL) {
		strcpy(left_panel.current_path, cwd);
		strcpy(right_panel.current_path, cwd);
	} else {
		strcpy(left_panel.current_path, ".");
		strcpy(right_panel.current_path, ".");
	}

	left_panel.parent_name[0] = '\0';
	right_panel.parent_name[0] = '\0';

	load_directory(&left_panel);
	load_directory(&right_panel);
	active_panel = &left_panel;

	clear();
	refresh();
	draw_screen();
	refresh();

	wchar_t wcmd[MAX_CMD] = L"";
	while (1) {
		InputEvent ev = input_read();
		if (ev.action == ACT_NONE) continue;
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
				clipboard_paste_to_cmd(wcmd);
				if (!terminal_mode) { draw_screen(); refresh(); } else { print_term_prompt(wcmd); }
			}
			continue;
		}
		if (ev.action == ACT_CTRL_O) {
			terminal_mode = !terminal_mode;
			if (terminal_mode) { def_prog_mode(); endwin(); input_enable_raw(); print_term_prompt(wcmd); }
			else { reset_prog_mode(); input_enable_raw(); clearok(stdscr, TRUE); load_directory(&left_panel); load_directory(&right_panel); refresh(); draw_screen(); refresh(); }
			continue;
		}
		if (ev.action == ACT_INSERT_LEFT_PATH) { insert_panel_path_to_cmd(wcmd, &left_panel); if (!terminal_mode) { draw_screen(); refresh(); } else print_term_prompt(wcmd); continue; }
		if (ev.action == ACT_INSERT_RIGHT_PATH) { insert_panel_path_to_cmd(wcmd, &right_panel); if (!terminal_mode) { draw_screen(); refresh(); } else print_term_prompt(wcmd); continue; }
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
				if (terminal_mode) { printf("\n"); fflush(stdout); }
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
			if (!terminal_mode) { draw_screen(); refresh(); } else print_term_prompt(wcmd);
			continue;
		}
		if (terminal_mode) {
			if (ev.action == ACT_ENTER) { printf("\n"); print_term_prompt(wcmd); }
			continue;
		}
		if (ev.action == ACT_TAB) active_panel = (active_panel == &left_panel) ? &right_panel : &left_panel;
		else if (ev.action == ACT_F1) ui_help();
		else if (ev.action == ACT_F2) ui_user_menu();
		else if (ev.action == ACT_F3 && active_panel->count > 0) {
			FileEntry *cur = &active_panel->files[active_panel->selected];
			if (!cur->is_dir && strcmp(cur->name, "..") != 0) { char fp[MAX_PATH * 2]; snprintf(fp, sizeof(fp), "%s/%s", active_panel->current_path, cur->name); view_file(fp); }
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
		else if (ev.action == ACT_LEFT || ev.action == ACT_PGUP) { int h, w; getmaxyx(stdscr, h, w); (void)w; int vis = h - 4; if (vis < 1) vis = 1; active_panel->selected -= vis; if (active_panel->selected < 0) active_panel->selected = 0; }
		else if (ev.action == ACT_RIGHT || ev.action == ACT_PGDN) { int h, w; getmaxyx(stdscr, h, w); (void)w; int vis = h - 4; if (vis < 1) vis = 1; active_panel->selected += vis; if (active_panel->selected >= active_panel->count) active_panel->selected = (active_panel->count > 0) ? (active_panel->count - 1) : 0; }
		else if (ev.action == ACT_ENTER && active_panel->count > 0) {
			FileEntry *cur = &active_panel->files[active_panel->selected];
			if (cur->is_dir) change_directory(active_panel, cur->name);
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
	input_restore_sane();
	endwin();
	return 0;
}
