#ifndef PROGRESS_H
#define PROGRESS_H

#include <ncurses.h>
#include <sys/types.h>
#include <time.h>

typedef struct {
	WINDOW *win;
	char title[64];
	int two_bars;
	int cancelled;
	off_t total_bytes;
	off_t done_bytes;
	long total_files;
	long done_files;
	struct timespec t_start;
	struct timespec t_last_draw;
	int scan_steps;
} UiProgress;

void progress_open(UiProgress *p, const char *title, int two_bars);
void progress_scan_path(UiProgress *p, const char *path);
int  progress_check_cancel(UiProgress *p);
int  progress_update(UiProgress *p, const char *src, const char *dst, off_t cur_done, off_t cur_total, int force);
void progress_close(UiProgress *p);

#endif
