#define _GNU_SOURCE
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <unistd.h>
#include "input.h"
#include "progress.h"
#include "ui.h"

static void fmt_bytes(off_t b, char *out, size_t sz) {
	if (b >= (off_t)1024 * 1024 * 1024) snprintf(out, sz, "%.2f GB", (double)b / (1024.0 * 1024.0 * 1024.0));
	else if (b >= 1024 * 1024) snprintf(out, sz, "%.1f MB", (double)b / (1024.0 * 1024.0));
	else if (b >= 1024) snprintf(out, sz, "%.1f KB", (double)b / 1024.0);
	else snprintf(out, sz, "%lld B", (long long)b);
}

static void draw_bar(WINDOW *w, int y, int x, int bar_w, int pct) {
	if (pct < 0) pct = 0;
	if (pct > 100) pct = 100;
	int fill = (bar_w * pct) / 100;
	wattrset(w, COLOR_PAIR(5));
	mvwaddch(w, y, x, '[');
	wattrset(w, COLOR_PAIR(2));
	for (int i = 0; i < fill; i++) waddch(w, ' ');
	wattrset(w, COLOR_PAIR(5));
	for (int i = fill; i < bar_w; i++) waddch(w, '.');
	mvwprintw(w, y, x + 1 + bar_w, "] %3d%%", pct);
}

void progress_open(UiProgress *p, const char *title, int two_bars) {
	memset(p, 0, sizeof(*p));
	snprintf(p->title, sizeof(p->title), "%s", title ? title : "Progress");
	p->two_bars = two_bars;
	clock_gettime(CLOCK_MONOTONIC, &p->t_start);

	int h, w;
	getmaxyx(stdscr, h, w);
	int bw = (w - 6 > 64) ? 64 : (w - 4);
	if (bw < 42) bw = w - 2;
	int bh = two_bars ? 11 : 8;
	p->win = newwin(bh, bw, (h - bh) / 2, (w - bw) / 2);
	wbkgd(p->win, COLOR_PAIR(5));
	box(p->win, 0, 0);
	mvwprintw(p->win, 0, 2, " %s ", p->title);
	mvwprintw(p->win, 2, 2, "Scanning files...");
	mvwprintw(p->win, bh - 2, (bw - 18) / 2, "[ Esc - Cancel ]");
	wrefresh(p->win);
}

int progress_check_cancel(UiProgress *p) {
	if (!p || p->cancelled) return 1;
	fd_set fds;
	FD_ZERO(&fds);
	FD_SET(STDIN_FILENO, &fds);
	struct timeval tv = {0, 0};
	while (select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) > 0) {
		InputEvent ev = input_read();
		if (ev.action == ACT_ESC || ev.action == ACT_MOUSE_LEFT) {
			if (ui_confirm("Cancel", "Abort current operation?")) {
				p->cancelled = 1;
				break;
			}
			if (p->win) { touchwin(p->win); wrefresh(p->win); }
		}
		FD_ZERO(&fds);
		FD_SET(STDIN_FILENO, &fds);
	}
	return p->cancelled;
}

void progress_scan_path(UiProgress *p, const char *path) {
	if (!p || p->cancelled) return;
	if ((++p->scan_steps & 127) == 0) {
		progress_check_cancel(p);
		if (p->win) {
			mvwprintw(p->win, 2, 2, "Scanning: %ld item(s)...        ", p->total_files);
			wrefresh(p->win);
		}
	}
	struct stat st;
	if (lstat(path, &st) != 0) return;
	p->total_files++;
	if (S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode)) {
		DIR *d = opendir(path);
		if (!d) return;
		struct dirent *de;
		while ((de = readdir(d)) != NULL && !p->cancelled) {
			if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
			char sub[4096];
			snprintf(sub, sizeof(sub), "%s/%s", path, de->d_name);
			progress_scan_path(p, sub);
		}
		closedir(d);
	} else {
		p->total_bytes += st.st_size;
	}
}

int progress_update(UiProgress *p, const char *src, const char *dst, off_t cur_done, off_t cur_total, int force) {
	if (!p || !p->win) return 0;
	if (progress_check_cancel(p)) return 1;

	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	long ms = (now.tv_sec - p->t_last_draw.tv_sec) * 1000L + (now.tv_nsec - p->t_last_draw.tv_nsec) / 1000000L;
	if (!force && ms < 55) return 0;
	p->t_last_draw = now;

	int bh, bw;
	getmaxyx(p->win, bh, bw);
	int inner = bw - 4;
	int bar_w = bw - 10;
	if (bar_w < 10) bar_w = 10;

	werase(p->win);
	box(p->win, 0, 0);
	mvwprintw(p->win, 0, 2, " %s ", p->title);

	const char *bn = src ? strrchr(src, '/') : NULL;
	bn = bn ? (bn + 1) : (src ? src : "");
	mvwprintw(p->win, 1, 2, "Item: %.*s", inner - 6, bn);
	if (dst && *dst) mvwprintw(p->win, 2, 2, "To:   %.*s", inner - 6, dst);

	if (p->two_bars) {
		int cpct = (cur_total > 0) ? (int)((cur_done * 100) / cur_total) : 100;
		draw_bar(p->win, 3, 2, bar_w, cpct);

		char db[24], tb[24];
		fmt_bytes(p->done_bytes, db, sizeof(db));
		fmt_bytes(p->total_bytes, tb, sizeof(tb));
		mvwprintw(p->win, 5, 2, "Total: %s / %s (%ld / %ld)", db, tb, p->done_files, p->total_files);
		int tpct = (p->total_bytes > 0) ? (int)((p->done_bytes * 100) / p->total_bytes)
		         : (p->total_files > 0 ? (int)((p->done_files * 100) / p->total_files) : 100);
		draw_bar(p->win, 6, 2, bar_w, tpct);

		double el = (now.tv_sec - p->t_start.tv_sec) + (now.tv_nsec - p->t_start.tv_nsec) / 1e9;
		if (el > 0.15 && p->done_bytes > 0) {
			double spd = (double)p->done_bytes / el;
			char sb[24];
			fmt_bytes((off_t)spd, sb, sizeof(sb));
			long eta = (spd > 1.0 && p->total_bytes > p->done_bytes)
			         ? (long)((p->total_bytes - p->done_bytes) / spd) : 0;
			mvwprintw(p->win, 8, 2, "Speed: %s/s   ETA: %02ld:%02ld", sb, eta / 60, eta % 60);
		}
	} else {
		mvwprintw(p->win, 3, 2, "Progress: %ld / %ld items", p->done_files, p->total_files);
		int tpct = (p->total_files > 0) ? (int)((p->done_files * 100) / p->total_files) : 100;
		draw_bar(p->win, 4, 2, bar_w, tpct);
	}

	mvwprintw(p->win, bh - 2, (bw - 18) / 2, "[ Esc - Cancel ]");
	wrefresh(p->win);
	return p->cancelled;
}

void progress_close(UiProgress *p) {
	if (p && p->win) {
		delwin(p->win);
		p->win = NULL;
		touchwin(stdscr);
	}
}
