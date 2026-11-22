#define _GNU_SOURCE
#include <errno.h>
#include <ncurses.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "input.h"
#include "sudo_helper.h"
#include "ui.h"

static char g_sudo_pass[256] = "";
static int g_sudo_has_pass = 0;

void sudo_clear_cache(void) {
	volatile char *p = (volatile char *)g_sudo_pass;
	for (size_t i = 0; i < sizeof(g_sudo_pass); i++) p[i] = 0;
	g_sudo_has_pass = 0;
}

static int prompt_sudo_password(char *out, size_t cap) {
	int h, w; getmaxyx(stdscr, h, w);
	int bw = (w - 4 < 54) ? (w - 4) : 54, bh = 6;
	if (bw < 36) bw = 36;
	int sy = (h > bh) ? (h - bh) / 2 : 0, sx = (w > bw) ? (w - bw) / 2 : 0;

	WINDOW *win = newwin(bh, bw, sy, sx);
	wbkgd(win, COLOR_PAIR(17));
	size_t len = 0; out[0] = '\0';
	curs_set(0);

	while (1) {
		werase(win);
		wattrset(win, COLOR_PAIR(17));
		box(win, 0, 0);
		mvwprintw(win, 0, 2, " Operation requires privileges elevation ");
		mvwprintw(win, 1, 2, "Enter sudo password:");

		wattrset(win, COLOR_PAIR(6));
		mvwhline(win, 2, 2, ' ', bw - 4);
		int vis = (int)len;
		if (vis > (bw - 6) / 2) vis = (bw - 6) / 2;
		for (int i = 0; i < vis; i++) mvwaddstr(win, 2, 3 + i * 2, "■");

		wattrset(win, COLOR_PAIR(17));
		const char *hint = "Confirm by <Enter>  Cancel by <Esc>";
		mvwprintw(win, 4, (bw - (int)strlen(hint)) / 2, "%s", hint);
		wrefresh(win);

		InputEvent ev = input_read();
		if (ev.action == ACT_ESC || ev.action == ACT_F10) {
			delwin(win); draw_screen(); refresh(); return 0;
		}
		if (ev.action == ACT_ENTER) {
			delwin(win); draw_screen(); refresh(); return 1;
		}
		if (ev.action == ACT_BACKSPACE) {
			if (len > 0) out[--len] = '\0';
		} else if (ev.action == ACT_CHAR && ev.ch >= 32 && ev.ch < 127) {
			if (len + 1 < cap) { out[len++] = (char)ev.ch; out[len] = '\0'; }
		}
	}
}

static int exec_sudo_pipe(const char *const argv[], const char *pass) {
	int pfd[2];
	if (pipe(pfd) != 0) return -1;
	pid_t pid = fork();
	if (pid < 0) { close(pfd[0]); close(pfd[1]); return -1; }
	if (pid == 0) {
		close(pfd[1]);
		dup2(pfd[0], STDIN_FILENO);
		close(pfd[0]);
		freopen("/dev/null", "w", stdout);
		freopen("/dev/null", "w", stderr);
		int argc = 0; while (argv[argc]) argc++;
		const char **sargv = malloc((size_t)(argc + 6) * sizeof(char *));
		if (!sargv) _exit(127);
		sargv[0] = "sudo";
		sargv[1] = pass ? "-S" : "-n";
		int k = 2;
		if (pass) { sargv[k++] = "-p"; sargv[k++] = ""; }
		sargv[k++] = "--";
		for (int i = 0; i < argc; i++) sargv[k++] = argv[i];
		sargv[k] = NULL;
		execvp("sudo", (char *const *)sargv);
		_exit(127);
	}
	close(pfd[0]);
	if (pass) {
		(void)!write(pfd[1], pass, strlen(pass));
		(void)!write(pfd[1], "\n", 1);
	}
	close(pfd[1]);
	int st = 0;
	waitpid(pid, &st, 0);
	return (WIFEXITED(st) && WEXITSTATUS(st) == 0) ? 0 : -1;
}

int sudo_run_argv(const char *const argv[]) {
	if (!argv || !argv[0]) return -1;
	if (exec_sudo_pipe(argv, NULL) == 0) return 0;
	if (g_sudo_has_pass) {
		if (exec_sudo_pipe(argv, g_sudo_pass) == 0) return 0;
		sudo_clear_cache();
	}
	for (int attempt = 0; attempt < 3; attempt++) {
		char pwd[256];
		if (!prompt_sudo_password(pwd, sizeof(pwd))) return -1;
		const char *chk[2] = { "true", NULL };
		if (exec_sudo_pipe(chk, pwd) == 0) {
			snprintf(g_sudo_pass, sizeof(g_sudo_pass), "%s", pwd);
			g_sudo_has_pass = 1;
			memset(pwd, 0, sizeof(pwd));
			return exec_sudo_pipe(argv, g_sudo_pass);
		}
		memset(pwd, 0, sizeof(pwd));
		ui_message("Authentication Failed", "Incorrect sudo password");
	}
	return -1;
}

int sudo_write_from_temp(const char *tmp_path, const char *dst_path) {
	struct stat st;
	int had_st = (stat(dst_path, &st) == 0);
	const char *dd_argv[5] = { "dd", NULL, NULL, "status=none", NULL };
	char if_arg[MAX_PATH + 8], of_arg[MAX_PATH + 8];
	snprintf(if_arg, sizeof(if_arg), "if=%s", tmp_path);
	snprintf(of_arg, sizeof(of_arg), "of=%s", dst_path);
	dd_argv[1] = if_arg; dd_argv[2] = of_arg;
	if (sudo_run_argv(dd_argv) != 0) return -1;
	if (!had_st) {
		const char *ch_argv[4] = { "chmod", "0644", dst_path, NULL };
		(void)sudo_run_argv(ch_argv);
	}
	return 0;
}
