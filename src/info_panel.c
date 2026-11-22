#define _GNU_SOURCE
#include <dirent.h>
#include <grp.h>
#include <ncurses.h>
#include <pwd.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <stdlib.h>
#include <unistd.h>
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

static void walk_dir_info(const char *path, int *steps) {
	if (*steps > 15000) { g_cache_trunc = 1; return; }
	DIR *d = opendir(path);
	if (!d) return;
	struct dirent *de;
	while ((de = readdir(d)) != NULL) {
		if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
		if (++(*steps) > 15000) { g_cache_trunc = 1; break; }
		if (de->d_name[0] == '.') g_cache_hid++;
		char sub[MAX_PATH * 2];
		snprintf(sub, sizeof(sub), "%s/%s", path, de->d_name);
		struct stat st;
		if (lstat(sub, &st) != 0) continue;
		if (S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode)) { g_cache_dirs++; walk_dir_info(sub, steps); }
		else { g_cache_files++; g_cache_bytes += st.st_size; }
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

static void print_fit_utf8(int y, int x, int max_w, const char *str) {
	if (max_w <= 0 || !str) return;
	wchar_t wb[MAX_PATH * 2];
	size_t n = mbstowcs(wb, str, MAX_PATH * 2 - 1);
	if (n == (size_t)-1) { mvprintw(y, x, "%.*s", max_w, str); return; }
	wb[n] = L'\0';
	mvaddnwstr(y, x, wb, max_w);
}

void draw_info_panel(const Panel *src, int sy, int sx, int w, int h) {
	attrset(COLOR_PAIR(1));
	for (int r = 0; r < h; r++) mvhline(sy + r, sx, ' ', w);
	for (int i = 1; i < w - 1; i++) { mvaddstr(sy, sx + i, "─"); mvaddstr(sy + h - 1, sx + i, "─"); }
	for (int i = 1; i < h - 1; i++) { mvaddstr(sy + i, sx, "│"); mvaddstr(sy + i, sx + w - 1, "│"); }
	mvaddstr(sy, sx, "┌"); mvaddstr(sy, sx + w - 1, "┐");
	mvaddstr(sy + h - 1, sx, "└"); mvaddstr(sy + h - 1, sx + w - 1, "┘");

	if (!src || src->count == 0) return;
	char target[MAX_PATH * 2];
	const FileEntry *cur = &src->files[src->selected];
	if (strcmp(cur->name, "..") == 0) snprintf(target, sizeof(target), "%s", src->current_path);
	else panel_entry_full_path(src, src->selected, target, sizeof(target));

	const char *bn = strrchr(target, '/'); bn = (bn && bn[1]) ? (bn + 1) : target;
	attrset(COLOR_PAIR(2));
	mvprintw(sy, sx + 2, " Info: %.*s ", w - 11, bn);

	struct stat st;
	if (lstat(target, &st) != 0) { attrset(COLOR_PAIR(1)); mvprintw(sy + 2, sx + 2, "Cannot stat target"); return; }

	int iw = w - 4, y = sy + 1;
	attrset(COLOR_PAIR(7)); print_fit_utf8(y++, sx + 2, iw, target);

	char tline[128] = "Regular file";
	if (S_ISLNK(st.st_mode)) {
		char lnk[MAX_PATH] = ""; ssize_t rl = readlink(target, lnk, sizeof(lnk) - 1);
		if (rl > 0) lnk[rl] = '\0';
		snprintf(tline, sizeof(tline), "Symlink -> %s", lnk);
	} else if (S_ISDIR(st.st_mode)) {
		char ghead[MAX_PATH * 2], br[64] = "";
		snprintf(ghead, sizeof(ghead), "%s/.git/HEAD", target);
		FILE *gf = fopen(ghead, "r");
		if (gf) { if (fgets(br, sizeof(br), gf)) { br[strcspn(br, "\r\n")] = '\0'; } fclose(gf); }
		const char *bname = strrchr(br, '/');
		if (bname) snprintf(tline, sizeof(tline), "Directory (Git: %s)", bname + 1);
		else snprintf(tline, sizeof(tline), "Directory");
	}
	attrset(COLOR_PAIR(1));
	if (y < sy + h - 1) { mvprintw(y, sx + 2, "Type:   "); attrset(COLOR_PAIR(8)); print_fit_utf8(y++, sx + 10, iw - 8, tline); }

	char szs[64];
	if (S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode)) {
		if (strcmp(g_cache_path, target) != 0) {
			snprintf(g_cache_path, sizeof(g_cache_path), "%s", target);
			g_cache_bytes = 0; g_cache_files = 0; g_cache_dirs = 0; g_cache_hid = 0; g_cache_trunc = 0;
			int steps = 0; walk_dir_info(target, &steps);
		}
		fmt_sz(g_cache_bytes, szs, sizeof(szs));
		if (y < sy + h - 1) { attrset(COLOR_PAIR(1)); mvprintw(y++, sx + 2, "Size:   %s%s", szs, g_cache_trunc ? "+" : ""); }
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
		char mnt[128], fst[32], tot[48], fre[48];
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
