#define _GNU_SOURCE
#include <dirent.h>
#include <grp.h>
#include <ncurses.h>
#include <poll.h>
#include <pwd.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <stdlib.h>
#include <unistd.h>
#include <wchar.h>
#include "info_panel.h"
#include "ui.h"

int g_info_panel = 0;

static char g_cache_path[MAX_PATH * 2] = "";
static off_t g_cache_bytes = 0;
static long g_cache_files = 0, g_cache_dirs = 0, g_cache_hid = 0;
static int g_cache_trunc = 0;

static void fmt_sz(off_t b, char *out, size_t sz) {
	if (b >= (off_t)1024 * 1024 * 1024) snprintf(out, sz, "%.2f GB (%lld B)", (double)b / (1024.0 * 1024.0 * 1024.0), (long long)b);
	else if (b >= 1024 * 1024) snprintf(out, sz, "%.2f MB (%lld B)", (double)b / (1024.0 * 1024.0), (long long)b);
	else if (b >= 1024) snprintf(out, sz, "%.1f KB (%lld B)", (double)b / 1024.0, (long long)b);
	else snprintf(out, sz, "%lld B", (long long)b);
}

static long long now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

static int check_esc_pressed(void) {
	struct pollfd pfd = { STDIN_FILENO, POLLIN, 0 };
	if (poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN)) {
		char buf[128];
		ssize_t n = read(STDIN_FILENO, buf, sizeof(buf) - 1);
		if (n <= 0) return 0;
		buf[n] = '\0';
		if (n == 1 && (buf[0] == 27 || buf[0] == 3)) return 1;
		if (strstr(buf, "\033[27u") || strstr(buf, "\033[27;1u") || strstr(buf, "\033[<0;")) return 1;
	}
	return 0;
}

typedef struct {
	WINDOW *win;
	long long start_ms;
	long long last_ms;
	const char *root;
} ScanUi;

static void scan_ui_tick(ScanUi *u) {
	if (g_cache_trunc) return;
	long long t = now_ms();
	if (t - u->last_ms < 100) return;
	u->last_ms = t;
	if (check_esc_pressed()) { g_cache_trunc = 1; return; }
	if (!u->win && (t - u->start_ms) >= 40) {
		int h, w; getmaxyx(stdscr, h, w);
		int bw = (w - 4 < 48) ? (w - 4) : 48, bh = 7;
		u->win = newwin(bh, bw, (h - bh) / 2, (w - bw) / 2);
		wbkgd(u->win, COLOR_PAIR(5));
	}
	if (u->win) {
		int bw = getmaxx(u->win), iw = bw - 4;
		werase(u->win); box(u->win, 0, 0);
		mvwprintw(u->win, 0, 2, " Info Panel ");
		mvwprintw(u->win, 1, (bw - 19) / 2, "Scanning the folder");
		int rl = (int)strlen(u->root);
		if (rl > iw && iw > 4) mvwprintw(u->win, 2, 2, "...%.*s", iw - 3, u->root + rl - iw + 3);
		else mvwprintw(u->win, 2, (bw - rl) / 2, "%.*s", iw, u->root);
		char ln[96];
		double gb = (double)g_cache_bytes / (1024.0 * 1024.0 * 1024.0);
		if (gb >= 0.1) snprintf(ln, sizeof(ln), "%.2f GB (%ld files)", gb, g_cache_files);
		else snprintf(ln, sizeof(ln), "%.1f MB (%ld files)", (double)g_cache_bytes / (1024.0 * 1024.0), g_cache_files);
		mvwprintw(u->win, 3, (bw - (int)strlen(ln)) / 2, "%s", ln);
		mvwprintw(u->win, 5, (bw - 19) / 2, "[ Esc - Interrupt ]");
		wrefresh(u->win);
	}
}

static void walk_dir_info(const char *path, ScanUi *u) {
	if (g_cache_trunc) return;
	DIR *d = opendir(path);
	if (!d) return;
	struct dirent *de;
	while ((de = readdir(d)) != NULL && !g_cache_trunc) {
		if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
		if (de->d_name[0] == '.') g_cache_hid++;
		char sub[MAX_PATH * 2];
		snprintf(sub, sizeof(sub), "%s/%s", path, de->d_name);
		struct stat st;
		if (lstat(sub, &st) != 0) continue;
		if (S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode)) {
			g_cache_dirs++; scan_ui_tick(u); walk_dir_info(sub, u);
		} else {
			g_cache_files++; g_cache_bytes += st.st_size;
			if ((g_cache_files & 255) == 0) scan_ui_tick(u);
		}
	}
	closedir(d);
}

static void find_mount_info(const char *path, char *mnt, size_t msz, char *fstype, size_t fsz) {
	snprintf(mnt, msz, "/"); snprintf(fstype, fsz, "unknown");
	FILE *f = fopen("/proc/mounts", "r");
	if (!f) return;
	char dev[128], mp[256], fs[64];
	size_t best = 0;
	while (fscanf(f, "%127s %255s %63s %*[^\n]\n", dev, mp, fs) == 3) {
		size_t l = strlen(mp);
		if (strncmp(path, mp, l) == 0 && (l == 1 || path[l] == '/' || path[l] == '\0') && l >= best) {
			best = l; snprintf(mnt, msz, "%s", mp); snprintf(fstype, fsz, "%s", fs);
		}
	}
	fclose(f);
}

static int info_print_wrapped(int y, int max_y, int x, int max_cols, const char *label, const char *text, int val_pair) {
	if (y >= max_y || max_cols < 8) return y;
	wchar_t wbuf[MAX_PATH * 2];
	size_t wl = mbstowcs(wbuf, text ? text : "", MAX_PATH * 2 - 1);
	if (wl == (size_t)-1) {
		attrset(COLOR_PAIR(1)); mvprintw(y, x, "%s", label);
		attrset(COLOR_PAIR(val_pair)); mvprintw(y, x + 8, "%.*s", max_cols - 8, text ? text : "");
		return y + 1;
	}
	wbuf[wl] = L'\0';
	attrset(COLOR_PAIR(1)); mvprintw(y, x, "%s", label);
	attrset(COLOR_PAIR(val_pair));
	size_t pos = 0;
	int lines = 0;
	while (pos < wl && y < max_y && lines < 4) {
		int cx = (lines == 0) ? (x + 8) : (x + 2);
		int avail = (lines == 0) ? (max_cols - 8) : (max_cols - 2);
		if (avail < 4) break;
		int used = 0;
		size_t start = pos;
		while (pos < wl) {
			int cw = wcwidth(wbuf[pos]); if (cw < 1) cw = 1;
			if (used + cw > avail) break;
			used += cw; pos++;
		}
		if (pos == start) pos++;
		mvaddnwstr(y, cx, wbuf + start, (int)(pos - start));
		y++; lines++;
	}
	if (lines == 0) y++;
	return y;
}

void draw_info_panel(const Panel *src, int sy, int sx, int w, int h) {
	if (!src || src->count == 0) return;
	char target[MAX_PATH * 2];
	const FileEntry *cur = &src->files[src->selected];
	if (strcmp(cur->name, "..") == 0) snprintf(target, sizeof(target), "%s", src->current_path);
	else panel_entry_full_path(src, src->selected, target, sizeof(target));

	struct stat st;
	int st_ok = (lstat(target, &st) == 0);
	if (st_ok && S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode)) {
		if (strcmp(g_cache_path, target) != 0) {
			snprintf(g_cache_path, sizeof(g_cache_path), "%s", target);
			g_cache_bytes = 0; g_cache_files = 0; g_cache_dirs = 0; g_cache_hid = 0; g_cache_trunc = 0;
			wnoutrefresh(stdscr); doupdate();
			ScanUi u = { NULL, now_ms(), 0, target };
			walk_dir_info(target, &u);
			if (u.win) { delwin(u.win); touchwin(stdscr); }
		}
	}
	attrset(COLOR_PAIR(1));
	for (int r = 0; r < h; r++) mvhline(sy + r, sx, ' ', w);
	for (int i = 1; i < w - 1; i++) { mvaddstr(sy, sx + i, "─"); mvaddstr(sy + h - 1, sx + i, "─"); }
	for (int i = 1; i < h - 1; i++) { mvaddstr(sy + i, sx, "│"); mvaddstr(sy + i, sx + w - 1, "│"); }
	mvaddstr(sy, sx, "┌"); mvaddstr(sy, sx + w - 1, "┐");
	mvaddstr(sy + h - 1, sx, "└"); mvaddstr(sy + h - 1, sx + w - 1, "┘");

	const char *bn = strrchr(target, '/'); bn = (bn && bn[1]) ? (bn + 1) : target;
	char dir_only[MAX_PATH * 2];
	snprintf(dir_only, sizeof(dir_only), "%s", target);
	char *dsl = strrchr(dir_only, '/');
	if (dsl) { if (dsl == dir_only) dir_only[1] = '\0'; else *dsl = '\0'; }
	else snprintf(dir_only, sizeof(dir_only), ".");

	attrset(COLOR_PAIR(2));
	if (w > 12) {
		wchar_t wbn[MAX_FILENAME];
		size_t bl = mbstowcs(wbn, bn, MAX_FILENAME - 1);
		mvaddstr(sy, sx + 2, " Info: ");
		int cur_c = sx + 9, lim_c = sx + w - 4;
		if (bl != (size_t)-1) {
			for (size_t k = 0; k < bl; k++) {
				int cw = wcwidth(wbn[k]); if (cw < 1) cw = 1;
				if (cur_c + cw > lim_c) { mvaddstr(sy, cur_c, "..."); cur_c += 3; break; }
				mvaddnwstr(sy, cur_c, wbn + k, 1); cur_c += cw;
			}
		}
		mvaddch(sy, cur_c, ' ');
	}
	if (!st_ok) { attrset(COLOR_PAIR(1)); mvprintw(sy + 2, sx + 2, "Cannot stat target"); return; }

	int iw = w - 4, y = sy + 1, max_y = sy + h - 1;
	y = info_print_wrapped(y, max_y, sx + 2, iw, "Dir:    ", dir_only, 7);
	y = info_print_wrapped(y, max_y, sx + 2, iw, "Name:   ", bn, 8);

	char tline[128] = "Regular file";
	if (S_ISLNK(st.st_mode)) {
		char lnk[MAX_PATH] = ""; ssize_t rl = readlink(target, lnk, sizeof(lnk) - 1);
		if (rl > 0) lnk[rl] = '\0';
		snprintf(tline, sizeof(tline), "Symlink -> %s", lnk);
	} else if (S_ISDIR(st.st_mode)) {
		char ghead[MAX_PATH * 2 + 16], br[64] = "";
		snprintf(ghead, sizeof(ghead), "%s/.git/HEAD", target);
		FILE *gf = fopen(ghead, "r");
		if (gf) { if (fgets(br, sizeof(br), gf)) { br[strcspn(br, "\r\n")] = '\0'; } fclose(gf); }
		const char *bname = strrchr(br, '/');
		if (bname) snprintf(tline, sizeof(tline), "Directory (Git: %s)", bname + 1);
		else snprintf(tline, sizeof(tline), "Directory");
	}
	y = info_print_wrapped(y, max_y, sx + 2, iw, "Type:   ", tline, 8);

	char szs[64];
	if (S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode)) {
		fmt_sz(g_cache_bytes, szs, sizeof(szs));
		if (y < sy + h - 1) { attrset(COLOR_PAIR(1)); mvprintw(y++, sx + 2, "Size:   %s%s", szs, g_cache_trunc ? " (interrupted)" : ""); }
		if (y < sy + h - 1) { mvprintw(y++, sx + 2, "Items:  %ld files, %ld dirs (%ld hid)%s", g_cache_files, g_cache_dirs, g_cache_hid, g_cache_trunc ? "+" : ""); }
	} else {
		fmt_sz(st.st_size, szs, sizeof(szs));
		if (y < sy + h - 1) { attrset(COLOR_PAIR(1)); mvprintw(y++, sx + 2, "Size:   %s", szs); }
	}

	if (y < sy + h - 1) { mvaddstr(y, sx, "├"); mvhline(y, sx + 1, ACS_HLINE, w - 2); mvaddstr(y++, sx + w - 1, "┤"); }
	char perm[11] = "---------";
	const char *rwx = "rwxrwxrwx";
	for (int b = 0; b < 9; b++) if (st.st_mode & (1 << (8 - b))) perm[b] = rwx[b];
	struct passwd *pw = getpwuid(st.st_uid); struct group *gr = getgrgid(st.st_gid);
	if (y < sy + h - 1) mvprintw(y++, sx + 2, "Mode:   %s (%04o)", perm, (unsigned)(st.st_mode & 07777));
	if (y < sy + h - 1) mvprintw(y++, sx + 2, "Owner:  %s:%s (%u:%u)", pw ? pw->pw_name : "?", gr ? gr->gr_name : "?", (unsigned)st.st_uid, (unsigned)st.st_gid);

	char tb[32]; struct tm *tmv;
	if (y < sy + h - 1 && (tmv = localtime(&st.st_mtime))) { strftime(tb, sizeof(tb), "%Y-%m-%d %H:%M:%S", tmv); mvprintw(y++, sx + 2, "Modif:  %s", tb); }
	if (y < sy + h - 1 && (tmv = localtime(&st.st_atime))) { strftime(tb, sizeof(tb), "%Y-%m-%d %H:%M:%S", tmv); mvprintw(y++, sx + 2, "Access: %s", tb); }
	if (y < sy + h - 1 && (tmv = localtime(&st.st_ctime))) { strftime(tb, sizeof(tb), "%Y-%m-%d %H:%M:%S", tmv); mvprintw(y++, sx + 2, "Change: %s", tb); }

	struct statvfs vfs;
	if (statvfs(target, &vfs) == 0 && y + 3 < sy + h - 1) {
		mvaddstr(y, sx, "├"); mvhline(y, sx + 1, ACS_HLINE, w - 2); mvaddstr(y++, sx + w - 1, "┤");
		char mnt[256], fst[64], tot[48], fre[48];
		find_mount_info(target, mnt, sizeof(mnt), fst, sizeof(fst));
		off_t tb_all = (off_t)vfs.f_blocks * vfs.f_frsize;
		off_t tb_avl = (off_t)vfs.f_bavail * vfs.f_frsize;
		fmt_sz(tb_all, tot, sizeof(tot)); fmt_sz(tb_avl, fre, sizeof(fre));
		int used_pct = (tb_all > 0) ? (int)(((tb_all - tb_avl) * 100) / tb_all) : 0;
		mvprintw(y++, sx + 2, "Mount:  %.*s (%s)", iw - 12, mnt, fst);
		mvprintw(y++, sx + 2, "Disk:   %s total", tot);
		mvprintw(y++, sx + 2, "Free:   %s (%d%% used)", fre, used_pct);
	}
}
