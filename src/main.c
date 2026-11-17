#define _GNU_SOURCE
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
#include "ui.h"
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

#define MAX_HISTORY 64
static char cmd_history[MAX_HISTORY][MAX_CMD];
static int history_count = 0;
static int history_idx = 0;

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

static void history_apply(void) {
	if (history_idx < 0) history_idx = 0;
	if (history_idx >= history_count) {
		history_idx = history_count;
		cmdline_clear();
		return;
	}
	cmdline_set_text(cmd_history[history_idx]);
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
		} else if (g_mod_shift && !g_mod_ctrl && !g_mod_alt) {
			if (idx == 3) return ACT_SHIFT_F4;
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
} StartupOpts;

static void print_cli_version(void) {
	printf("xCommander v%s [%06d]\n", XC_VERSION, XC_BUILD);
}

static void print_cli_help(const char *prog) {
	printf("Usage: %s [OPTIONS] [LEFT_DIR] [RIGHT_DIR]\n\n", prog);
	printf("Options:\n");
	printf("  -e, --edit FILE  Open FILE directly in built-in editor\n");
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
		if (strcmp(argv[i], "-e") == 0 || strcmp(argv[i], "--edit") == 0) {
			if (i + 1 >= argc) {
				fprintf(stderr, "Option '%s' requires a file path.\n", argv[i]);
				return -1;
			}
			opts->edit_file = argv[++i];
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

	if (opts.edit_file && opts.edit_file[0] != '\0') {
		ui_edit_file(opts.edit_file);
		input_restore_sane();
		endwin();
		input_restore_sane();
		return 0;
	}

	cmdline_init();
	static char last_prompt_dir[MAX_PATH] = "";
	while (1) {
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
				if (clipboard_paste_files(active_panel) > 0) { reload_panels_keep_cursor(); }
				draw_screen(); refresh();
			} else {
				char raw_clip[65536] = {0};
				if (clipboard_paste_raw(raw_clip, sizeof(raw_clip)) > 0 && strchr(raw_clip, '\n') != NULL) {
					int was_term = terminal_mode;
					if (was_term) { reset_prog_mode(); refresh(); }
					int ok = ui_multiline_paste(raw_clip);
					if (was_term) { endwin(); input_enable_raw(); }
					if (ok == 1) {
						run_multiline_script(raw_clip, was_term);
					} else if (ok == 2) {
						if (was_term) { reset_prog_mode(); refresh(); }
						char picked[65536] = {0};
						int h_ok = ui_multiline_history(picked, sizeof(picked));
						if (was_term) { endwin(); input_enable_raw(); }
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
				def_prog_mode(); endwin(); input_enable_raw();
				if (strcmp(last_prompt_dir, active_panel->current_path) != 0) {
					snprintf(last_prompt_dir, sizeof(last_prompt_dir), "%s", active_panel->current_path);
					print_term_prompt_ex(cmd_wbuf, 1);
				} else if (cmd_len > 0) {
					print_term_prompt_ex(cmd_wbuf, 0);
				}
			} else {
				reset_prog_mode(); input_enable_raw(); clearok(stdscr, TRUE);
				reload_panels_keep_cursor(); refresh(); draw_screen(); refresh();
			}
			continue;
		}
		if (ev.action == ACT_INSERT_LEFT_PATH) { cmdline_insert_path(&left_panel); if (!terminal_mode) { draw_screen(); refresh(); } else print_term_prompt(cmd_wbuf); continue; }
		if (ev.action == ACT_INSERT_RIGHT_PATH) { cmdline_insert_path(&right_panel); if (!terminal_mode) { draw_screen(); refresh(); } else print_term_prompt(cmd_wbuf); continue; }
		if (ev.action == ACT_ALT_F8 || ev.action == ACT_CTRL_H) {
			char picked_cmd[MAX_CMD] = "";
			int prev_cnt = history_count;
			int hres = ui_cmd_history(cmd_history, &history_count, picked_cmd, sizeof(picked_cmd));
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
			} else {
				if (!terminal_mode) { draw_screen(); refresh(); } else print_term_prompt(cmd_wbuf);
			}
			continue;
		}
		if (ev.action == ACT_ALT_CHAR && (ev.ch == L'h' || ev.ch == L'H' || ev.ch == L'р' || ev.ch == L'Р')) {
			int was_term = terminal_mode;
			if (was_term) { reset_prog_mode(); refresh(); }
			char picked[65536] = {0};
			int h_ok = ui_multiline_history(picked, sizeof(picked));
			if (was_term) { endwin(); input_enable_raw(); }
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
		int ed_res = cmdline_handle_edit_key(ev, terminal_mode);
		if (ed_res > 0) {
			if (ed_res == 2) history_idx = history_count;
			if (!terminal_mode) { draw_screen(); refresh(); } else print_term_prompt(cmd_wbuf);
			continue;
		}
		if (terminal_mode) {
			continue;
		}
		if (ev.action == ACT_TAB) active_panel = (active_panel == &left_panel) ? &right_panel : &left_panel;
		else if (ev.action == ACT_CTRL_R) { reload_panels_keep_cursor(); }
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
			if (!cur->is_dir && strcmp(cur->name, "..") != 0) { char fp[MAX_PATH * 2]; snprintf(fp, sizeof(fp), "%s/%s", active_panel->current_path, cur->name); ui_view_file(fp); }
		} else if (ev.action == ACT_F4 && active_panel->count > 0) {
			FileEntry *cur = &active_panel->files[active_panel->selected];
			if (!cur->is_dir && strcmp(cur->name, "..") != 0) {
				char fp[MAX_PATH * 2];
				snprintf(fp, sizeof(fp), "%s/%s", active_panel->current_path, cur->name);
				snprintf(active_panel->parent_name, MAX_FILENAME, "%s", cur->name);
				ui_edit_file(fp);
				load_directory(&left_panel);
				load_directory(&right_panel);
			}
		} else if (ev.action == ACT_SHIFT_F4) { if (create_and_edit_file(active_panel) > 0) { reload_panels_keep_cursor(); } }
		else if (ev.action == ACT_F5) {
			Panel *oth = (active_panel == &left_panel) ? &right_panel : &left_panel;
			char keep_name[MAX_FILENAME] = "";
			if (active_panel->count > 0) snprintf(keep_name, sizeof(keep_name), "%s", active_panel->files[active_panel->selected].name);
			if (copy_items(active_panel, oth) > 0) {
				snprintf(active_panel->parent_name, MAX_FILENAME, "%s", keep_name);
				load_directory(&left_panel);
				load_directory(&right_panel);
			}
		}
		else if (ev.action == ACT_F6) { Panel *oth = (active_panel == &left_panel) ? &right_panel : &left_panel; int ps = active_panel->selected; if (move_items(active_panel, oth) > 0) { reload_panels_keep_cursor(); if (ps >= active_panel->count) ps = active_panel->count - 1; if (ps >= 0) active_panel->selected = ps; } }
		else if (ev.action == ACT_F7) { if (make_directory(active_panel) > 0) { reload_panels_keep_cursor(); } }
		else if (ev.action == ACT_F8) { int ps = active_panel->selected; if (delete_items(active_panel) > 0) { reload_panels_keep_cursor(); if (ps >= active_panel->count) ps = active_panel->count - 1; if (ps >= 0) active_panel->selected = ps; } }
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
			else {
				char fp[MAX_PATH * 2];
				if (strcmp(active_panel->current_path, "/") == 0) snprintf(fp, sizeof(fp), "/%s", cur->name);
				else snprintf(fp, sizeof(fp), "%s/%s", active_panel->current_path, cur->name);
				if (access(fp, X_OK) == 0) { char rc[MAX_FILENAME + 16]; snprintf(rc, sizeof(rc), "./\"%s\"", cur->name); execute_system_command(rc); reload_panels_keep_cursor(); }
			}
		} else if (ev.action == ACT_CTRL_PGUP) change_directory(active_panel, "..");
		else if (ev.action == ACT_CTRL_PGDN && active_panel->count > 0) { FileEntry *cur = &active_panel->files[active_panel->selected]; if (cur->is_dir) change_directory(active_panel, cur->name); }
		if (!terminal_mode) { draw_screen(); refresh(); }
	}
	history_save();
	state_save();
	input_restore_sane();
	endwin();
	input_restore_sane();
	return 0;
}
