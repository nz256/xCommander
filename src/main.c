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
				raw();
				system("stty lnext undef");
	clearok(stdscr, TRUE);
	draw_screen();
	refresh();
	int res = ui_confirm("Quit", "Quit xCommander?");
	if (!res) {
		def_prog_mode();
		endwin();
		system("stty -icanon -echo -ixon -isig lnext undef");
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

int main(void) {
	setlocale(LC_ALL, "");
	ui_init();
	nonl();
	system("stty -ixon");
	printf("\033[>4;2m");
	fflush(stdout);
        history_load();

	char cwd[MAX_PATH];
	if (getcwd(cwd, sizeof(cwd)) != NULL) {
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
	doupdate();

	wchar_t wcmd[MAX_CMD] = L"";
	wint_t wch;
	int status;

	while (1) {
		status = get_wch(&wch);
		if (status == ERR) continue;

		/* Безусловный выход по Ctrl+Q (17) */
		if (status == OK && wch == 17) { if (confirm_exit(wcmd)) break; draw_screen(); doupdate(); continue; }

		/* Ctrl+C (3) или Ctrl+Y (25): копирование текста или файлов */
		if (status == OK && (wch == 3 || wch == 25)) {
			if (!terminal_mode && cmd_len == 0 && cmd_sel_start < 0) {
				clipboard_stage_files(active_panel, 0);
			} else {
				clipboard_copy_from_cmd(wcmd);
			}
			continue;
		}

		/* Ctrl+X (24): вырезать текст при выделении или файлы при пустой строке */
		if (status == OK && wch == 24 && (cmd_sel_start >= 0 || (!terminal_mode && cmd_len == 0))) {
			if (cmd_sel_start >= 0) {
				clipboard_copy_from_cmd(wcmd);
				cmd_delete_sel(wcmd);
				wcstombs(cmd_buffer, wcmd, sizeof(cmd_buffer));
				cmd_buffer[sizeof(cmd_buffer) - 1] = '\0';
				if (!terminal_mode) { draw_screen(); doupdate(); } else { print_term_prompt(wcmd); }
			} else {
				clipboard_stage_files(active_panel, 1);
			}
			continue;
		}

		/* Ctrl+V (22) или Shift+Insert: вставка файлов (если скопированы) или текста */
		if ((status == OK && wch == 22) || (status == KEY_CODE_YES && (strcmp(keyname((int)wch), "kIC") == 0 || strcmp(keyname((int)wch), "kIC2") == 0))) {
			if (!terminal_mode && cmd_len == 0 && clipboard_has_files()) {
				if (clipboard_paste_files(active_panel) > 0) {
					load_directory(&left_panel);
					load_directory(&right_panel);
				}
				draw_screen();
				doupdate();
			} else {
				clipboard_paste_to_cmd(wcmd);
				if (!terminal_mode) { draw_screen(); doupdate(); } else { print_term_prompt(wcmd); }
			}
			continue;
		}

		/* Ctrl+O: переключение режима терминала */
		if (status == OK && wch == 15) {
			terminal_mode = !terminal_mode;
			if (terminal_mode) {
				def_prog_mode();
				printf("\033[>4;0m");
				fflush(stdout);
				endwin();
				system("stty -icanon -echo -ixon -isig lnext undef");
				print_term_prompt(wcmd);
			} else {
				reset_prog_mode();
				raw();
				system("stty lnext undef");
				clearok(stdscr, TRUE);
				printf("\033[>4;2m");
				fflush(stdout);
				load_directory(&left_panel);
				load_directory(&right_panel);
				refresh();
				draw_screen();
				doupdate();
			}
			continue;
		}

		/* Листание истории: Ctrl+E / Ctrl+X всегда, а в режиме терминала также Стрелка Вверх / Вниз */
		int is_hist_prev = (status == OK && wch == 5) || (terminal_mode && status == KEY_CODE_YES && wch == KEY_UP);
		int is_hist_next = (status == OK && wch == 24) || (terminal_mode && status == KEY_CODE_YES && wch == KEY_DOWN);

		if (is_hist_prev || is_hist_next) {
			if (history_count > 0) {
				if (is_hist_prev && history_idx > 0) {
					history_idx--;
				} else if (is_hist_next && history_idx < history_count) {
					history_idx++;
				}
				cmd_sel_anchor = -1; cmd_update_sel();
				history_apply(wcmd);
				if (!terminal_mode) {
					draw_screen();
					doupdate();
				} else {
					print_term_prompt(wcmd);
				}
			}
			continue;
		}


		/* Ctrl+Enter (10) или Alt+Enter (27+13): вставка имени в командную строку */
		int is_insert_name = 0;
		if (!terminal_mode && status == OK && wch == 10) {
			is_insert_name = 1;
		} else if (!terminal_mode && status == OK && wch == 27) {
			nodelay(stdscr, TRUE);
			char seq[32] = {0};
			int slen = 0;
			wint_t nw;
			while (slen < 30 && get_wch(&nw) == OK) {
				seq[slen++] = (char)nw;
			}
			nodelay(stdscr, FALSE);
			if (strstr(seq, "27;5;99~") != NULL || strstr(seq, "99;5u") != NULL) {
				if (cmd_len == 0 && cmd_sel_start < 0) clipboard_stage_files(active_panel, 0);
				else clipboard_copy_from_cmd(wcmd);
				continue;
			}
			if (strstr(seq, "27;5;120~") != NULL || strstr(seq, "120;5u") != NULL) {
				if (cmd_sel_start >= 0) {
					clipboard_copy_from_cmd(wcmd);
					cmd_delete_sel(wcmd);
					wcstombs(cmd_buffer, wcmd, sizeof(cmd_buffer));
					cmd_buffer[sizeof(cmd_buffer) - 1] = '\0';
					draw_screen();
					doupdate();
					continue;
				}
			}
			if (strstr(seq, "27;5;118~") != NULL || strstr(seq, "118;5u") != NULL) {
				if (cmd_len == 0 && clipboard_has_files()) {
					if (clipboard_paste_files(active_panel) > 0) { load_directory(&left_panel); load_directory(&right_panel); }
				} else {
					clipboard_paste_to_cmd(wcmd);
				}
				draw_screen();
				doupdate();
				continue;
			}
			if ((slen == 1 && (seq[0] == 13 || seq[0] == 10)) ||
			    strstr(seq, "27;5;13~") != NULL ||
			    strstr(seq, "13;5u") != NULL) {
				is_insert_name = 1;
			}
		}
		if (is_insert_name) {
			insert_current_name_to_cmd(wcmd);
			draw_screen();
			doupdate();
			continue;
		}

				/* Ввод и редактирование текста в командной строке */
		int is_printable = (status == OK && iswprint(wch) && !(cmd_len == 0 && wch == L' '));
		int is_cursor_move = (cmd_len > 0 && (
			(status == KEY_CODE_YES && (wch == KEY_HOME || wch == KEY_END || wch == KEY_SLEFT || wch == KEY_SRIGHT)) ||
			(terminal_mode && status == KEY_CODE_YES && (wch == KEY_LEFT || wch == KEY_RIGHT)) ||
			(status == KEY_CODE_YES && (strcmp(keyname((int)wch), "kLFT5") == 0 || strcmp(keyname((int)wch), "kRIT5") == 0 || strcmp(keyname((int)wch), "kLFT6") == 0 || strcmp(keyname((int)wch), "kRIT6") == 0)) ||
			(status == OK && (wch == 1 || wch == 2 || wch == 6))
		));
		int is_editing = (cmd_len > 0 && (
			(status == KEY_CODE_YES && (wch == KEY_BACKSPACE || wch == KEY_DC || wch == KEY_ENTER)) ||
			(status == OK && (wch == 127 || wch == 8 || wch == L'\n' || wch == L'\r' || wch == 21 || wch == 27))
		));

		if (is_printable || is_cursor_move || is_editing) {
			if (is_cursor_move) {
				if ((status == KEY_CODE_YES && wch == KEY_HOME) || (status == OK && wch == 1)) {
					cmd_sel_anchor = -1; cmd_update_sel();
					cmd_pos = 0;
				} else if (status == KEY_CODE_YES && wch == KEY_END) {
					cmd_sel_anchor = -1; cmd_update_sel();
					cmd_pos = cmd_len;
				} else if (status == KEY_CODE_YES && wch == KEY_SLEFT) {
					if (cmd_sel_anchor == -1) cmd_sel_anchor = cmd_pos;
					if (cmd_pos > 0) cmd_pos--;
					cmd_update_sel();
				} else if (status == KEY_CODE_YES && wch == KEY_SRIGHT) {
					if (cmd_sel_anchor == -1) cmd_sel_anchor = cmd_pos;
					if (cmd_pos < cmd_len) cmd_pos++;
					cmd_update_sel();
				} else if (status == KEY_CODE_YES && strcmp(keyname((int)wch), "kLFT6") == 0) {
					if (cmd_sel_anchor == -1) cmd_sel_anchor = cmd_pos;
					cmd_pos = word_jump_left(wcmd, cmd_pos);
					cmd_update_sel();
				} else if (status == KEY_CODE_YES && strcmp(keyname((int)wch), "kRIT6") == 0) {
					if (cmd_sel_anchor == -1) cmd_sel_anchor = cmd_pos;
					cmd_pos = word_jump_right(wcmd, cmd_pos, cmd_len);
					cmd_update_sel();
				} else if (status == KEY_CODE_YES && strcmp(keyname((int)wch), "kLFT5") == 0) {
					cmd_sel_anchor = -1; cmd_update_sel();
					cmd_pos = word_jump_left(wcmd, cmd_pos);
				} else if (status == KEY_CODE_YES && strcmp(keyname((int)wch), "kRIT5") == 0) {
					cmd_sel_anchor = -1; cmd_update_sel();
					cmd_pos = word_jump_right(wcmd, cmd_pos, cmd_len);
				} else if ((status == KEY_CODE_YES && wch == KEY_LEFT) || (status == OK && wch == 2)) {
					cmd_sel_anchor = -1; cmd_update_sel();
					if (cmd_pos > 0) cmd_pos--;
				} else if ((status == KEY_CODE_YES && wch == KEY_RIGHT) || (status == OK && wch == 6)) {
					cmd_sel_anchor = -1; cmd_update_sel();
					if (cmd_pos < cmd_len) cmd_pos++;
				}
			} else if ((status == KEY_CODE_YES && wch == KEY_BACKSPACE) || (status == OK && (wch == 127 || wch == 8))) {
				if (cmd_sel_start >= 0) { cmd_delete_sel(wcmd); } else if (cmd_pos > 0) {
					wmemmove(wcmd + cmd_pos - 1, wcmd + cmd_pos, (size_t)(cmd_len - cmd_pos + 1));
					cmd_pos--;
					cmd_len--;
				}
			} else if (status == KEY_CODE_YES && wch == KEY_DC) {
				if (cmd_pos < cmd_len) {
					wmemmove(wcmd + cmd_pos, wcmd + cmd_pos + 1, (size_t)(cmd_len - cmd_pos));
					cmd_len--;
				}
			} else if (status == OK && (wch == 21 || wch == 27)) {
				cmd_sel_anchor = -1; cmd_update_sel();
				cmd_pos = 0;
				cmd_len = 0;
				*wcmd = L'\0';
				*cmd_buffer = '\0';
				history_idx = history_count;
			} else if ((status == KEY_CODE_YES && wch == KEY_ENTER) || (status == OK && (wch == L'\n' || wch == L'\r'))) {
				if (terminal_mode) {
					printf("\n");
					fflush(stdout);
				}
				if (cmd_len > 0) {
					wcstombs(cmd_buffer, wcmd, sizeof(cmd_buffer));
					cmd_buffer[sizeof(cmd_buffer) - 1] = '\0';
					if (strcmp(cmd_buffer, "exit") == 0 || strcmp(cmd_buffer, "quit") == 0) {
						if (confirm_exit(wcmd)) break;
						cmd_pos = 0;
						cmd_len = 0;
						*wcmd = L'\0';
						*cmd_buffer = '\0';
						if (!terminal_mode) { draw_screen(); doupdate(); } else { print_term_prompt(wcmd); }
						continue;
					}
					history_add(cmd_buffer);
					history_save();
					execute_system_command(cmd_buffer);
					if (!terminal_mode) {
						load_directory(&left_panel);
						load_directory(&right_panel);
					}
				}
				cmd_pos = 0;
				cmd_len = 0;
				*wcmd = L'\0';
				*cmd_buffer = '\0';
			} else if (is_printable && cmd_len < MAX_CMD - 1) {
				if (cmd_sel_start >= 0) cmd_delete_sel(wcmd);
				wmemmove(wcmd + cmd_pos + 1, wcmd + cmd_pos, (size_t)(cmd_len - cmd_pos + 1));
				wcmd[cmd_pos++] = (wchar_t)wch;
				cmd_len++;
			}

			wcstombs(cmd_buffer, wcmd, sizeof(cmd_buffer));
			cmd_buffer[sizeof(cmd_buffer) - 1] = '\0';

			if (!terminal_mode) {
				draw_screen();
				doupdate();
			} else {
				print_term_prompt(wcmd);
			}
			continue;
		}

		/* В режиме терминала блокируем навигацию по панелям, кроме Ctrl+O и F10 */
		if (terminal_mode && wch != KEY_F(10)) {
			if (wch == '\n' || wch == KEY_ENTER || wch == '\r') {
				printf("\n");
				print_term_prompt(wcmd);
			}
			continue;
		}

		if (wch == KEY_F(10)) { if (confirm_exit(wcmd)) break; }
		if (wch == KEY_RESIZE) {
			draw_screen();
			doupdate();
			continue;
		}

		if (wch == '\t' || wch == KEY_BTAB) {
			active_panel = (active_panel == &left_panel) ? &right_panel : &left_panel;
		} else if (wch == KEY_F(1)) {
			ui_help();
		} else if (wch == KEY_F(2)) {
			ui_user_menu();
		} else if (wch == KEY_F(3)) {
			if (active_panel->count > 0) {
				FileEntry *current = &active_panel->files[active_panel->selected];
				if (!current->is_dir && strcmp(current->name, "..") != 0) {
					char full_path[MAX_PATH * 2];
					snprintf(full_path, sizeof(full_path), "%s/%s", active_panel->current_path, current->name);
					view_file(full_path);
				}
			}
		} else if (wch == KEY_F(4)) {
			if (active_panel->count > 0) {
				FileEntry *current = &active_panel->files[active_panel->selected];
				if (!current->is_dir && strcmp(current->name, "..") != 0) {
					char full_path[MAX_PATH * 2];
					snprintf(full_path, sizeof(full_path), "%s/%s", active_panel->current_path, current->name);
					edit_file(full_path);
					load_directory(&left_panel);
					load_directory(&right_panel);
				}
			}
		} else if (wch == KEY_F(16)) { /* Shift+F4: создание нового файла */
			if (create_and_edit_file(active_panel) > 0) {
				load_directory(&left_panel);
				load_directory(&right_panel);
			}
		} else if (wch == KEY_F(5)) {
			Panel *other_panel = (active_panel == &left_panel) ? &right_panel : &left_panel;
			if (copy_items(active_panel, other_panel) > 0) {
				load_directory(&left_panel);
				load_directory(&right_panel);
			}
		} else if (wch == KEY_F(6)) {
			Panel *other_panel = (active_panel == &left_panel) ? &right_panel : &left_panel;
			int prev_sel = active_panel->selected;
			if (move_items(active_panel, other_panel) > 0) {
				load_directory(&left_panel);
				load_directory(&right_panel);
				if (prev_sel >= active_panel->count) prev_sel = active_panel->count - 1;
				if (prev_sel >= 0) active_panel->selected = prev_sel;
			}
		} else if (wch == KEY_F(7)) {
			if (make_directory(active_panel) > 0) {
				load_directory(&left_panel);
				load_directory(&right_panel);
			}
		} else if (wch == KEY_F(8)) {
			int prev_sel = active_panel->selected;
			if (delete_items(active_panel) > 0) {
				load_directory(&left_panel);
				load_directory(&right_panel);
				if (prev_sel >= active_panel->count) prev_sel = active_panel->count - 1;
				if (prev_sel >= 0) active_panel->selected = prev_sel;
			}
		} else if (wch == KEY_SR) { /* Shift+Вверх */
			panel_toggle_mark(active_panel, active_panel->selected);
			if (active_panel->selected > 0) active_panel->selected--;
		} else if (wch == KEY_SF) { /* Shift+Вниз */
			panel_toggle_mark(active_panel, active_panel->selected);
			if (active_panel->selected < active_panel->count - 1) active_panel->selected++;
		} else if (wch == KEY_IC) { /* Insert */
			panel_toggle_mark(active_panel, active_panel->selected);
			if (active_panel->selected < active_panel->count - 1) active_panel->selected++;
		} else if (wch == KEY_UP) {
			if (active_panel->selected > 0) active_panel->selected--;
		} else if (wch == KEY_DOWN) {
			if (active_panel->selected < active_panel->count - 1) active_panel->selected++;
				} else if (wch == KEY_LEFT) {
			int height, width;
			getmaxyx(stdscr, height, width);
			(void)width;
			int vis = height - 4;
			if (vis < 1) vis = 1;
			active_panel->selected -= vis;
			if (active_panel->selected < 0) active_panel->selected = 0;
		} else if (wch == KEY_RIGHT) {
			int height, width;
			getmaxyx(stdscr, height, width);
			(void)width;
			int vis = height - 4;
			if (vis < 1) vis = 1;
			active_panel->selected += vis;
			if (active_panel->selected >= active_panel->count) {
				active_panel->selected = (active_panel->count > 0) ? (active_panel->count - 1) : 0;
				}
				} else if (wch == '\n' || wch == KEY_ENTER || wch == '\r') {
			if (active_panel->count > 0) {
				FileEntry *current = &active_panel->files[active_panel->selected];
				if (current->is_dir) {
					change_directory(active_panel, current->name);
				} else {
					char full_path[MAX_PATH * 2];
					if (strcmp(active_panel->current_path, "/") == 0) {
						snprintf(full_path, sizeof(full_path), "/%s", current->name);
					} else {
						snprintf(full_path, sizeof(full_path), "%s/%s", active_panel->current_path, current->name);
					}

					if (access(full_path, X_OK) == 0) {
						char run_cmd[MAX_FILENAME + 16];
						snprintf(run_cmd, sizeof(run_cmd), "./\"%s\"", current->name);
						execute_system_command(run_cmd);
						load_directory(&left_panel);
						load_directory(&right_panel);
					}
				}
			}
		} else if (status == KEY_CODE_YES && strcmp(keyname((int)wch), "kPRV5") == 0) {
			change_directory(active_panel, "..");
		} else if (status == KEY_CODE_YES && strcmp(keyname((int)wch), "kNXT5") == 0) {
			if (active_panel->count > 0) {
				FileEntry *current = &active_panel->files[active_panel->selected];
				if (current->is_dir) {
					change_directory(active_panel, current->name);
				}
			}
		}

		if (!terminal_mode) {
			draw_screen();
			doupdate();
		}
	}
	history_save();
	endwin();
	return 0;
}
