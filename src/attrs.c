#define _GNU_SOURCE
#include <dirent.h>
#include <ctype.h>
#include <errno.h>
#include <grp.h>
#include <pwd.h>
#include <ncurses.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <unistd.h>
#include "input.h"
#include "panel.h"
#include "ui.h"
#include "widgets.h"

static const mode_t k_bits[12] = {
	S_IRUSR, S_IWUSR, S_IXUSR,
	S_IRGRP, S_IWGRP, S_IXGRP,
	S_IROTH, S_IWOTH, S_IXOTH,
	S_ISUID, S_ISGID, S_ISVTX
};

static int resolve_uid(const char *s, uid_t *out) {
	if (!s || !*s) return 0;
	struct passwd *pw = getpwnam(s);
	if (pw) { *out = pw->pw_uid; return 1; }
	char *end = NULL;
	unsigned long v = strtoul(s, &end, 10);
	if (end && *end == '\0') { *out = (uid_t)v; return 1; }
	return -1;
}

static int resolve_gid(const char *s, gid_t *out) {
	if (!s || !*s) return 0;
	struct group *gr = getgrnam(s);
	if (gr) { *out = gr->gr_gid; return 1; }
	char *end = NULL;
	unsigned long v = strtoul(s, &end, 10);
	if (end && *end == '\0') { *out = (gid_t)v; return 1; }
	return -1;
}

static void apply_attrs_recursive(const char *path, mode_t set_mask, mode_t clr_mask,
                                  int ch_uid, uid_t uid, int ch_gid, gid_t gid,
                                  int rec, int *errs) {
	struct stat st;
	if (lstat(path, &st) != 0) { (*errs)++; return; }
	if (ch_uid || ch_gid) {
		if (lchown(path, ch_uid ? uid : (uid_t)-1, ch_gid ? gid : (gid_t)-1) != 0) (*errs)++;
	}
	if (!S_ISLNK(st.st_mode)) {
		mode_t nm = (st.st_mode & 07777 & ~clr_mask) | set_mask;
		if (chmod(path, nm) != 0) (*errs)++;
	}
	if (rec && S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode)) {
		DIR *d = opendir(path);
		if (!d) { (*errs)++; return; }
		struct dirent *de;
		while ((de = readdir(d)) != NULL) {
			if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
			char sub[MAX_PATH * 2];
			snprintf(sub, sizeof(sub), "%s/%s", path, de->d_name);
			apply_attrs_recursive(sub, set_mask, clr_mask, ch_uid, uid, ch_gid, gid, 1, errs);
		}
		closedir(d);
	}
}

int ui_file_attributes(Panel *panel) {
	if (!panel || panel->count == 0 || panel->archive_file[0] != '\0') return 0;

	char paths[MAX_FILES][MAX_PATH];
	int pcnt = 0, has_dir = 0, marked = panel_marked_count(panel);
	for (int i = 0; i < panel->count && pcnt < MAX_FILES; i++) {
		if (strcmp(panel->files[i].name, "..") == 0) continue;
		if ((marked > 0 && panel->files[i].is_selected) || (marked == 0 && i == panel->selected)) {
			panel_entry_full_path(panel, i, paths[pcnt], MAX_PATH);
			if (panel->files[i].is_dir) has_dir = 1;
			pcnt++;
		}
	}
	if (pcnt == 0) return 0;

	int state[12], init_state[12];
	for (int b = 0; b < 12; b++) {
		int on = 0, off = 0;
		for (int i = 0; i < pcnt; i++) {
			struct stat st;
			if (lstat(paths[i], &st) == 0) { if (st.st_mode & k_bits[b]) on = 1; else off = 1; }
		}
		state[b] = init_state[b] = (on && off) ? 2 : (on ? 1 : 0);
	}

	char owner_buf[64] = "", group_buf[64] = "", init_owner[64] = "", init_group[64] = "";
	uid_t f_uid = 0; gid_t f_gid = 0;
	int s_uid = 1, s_gid = 1, got = 0;
	for (int i = 0; i < pcnt; i++) {
		struct stat st;
		if (lstat(paths[i], &st) == 0) {
			if (!got) { f_uid = st.st_uid; f_gid = st.st_gid; got = 1; }
			else { if (st.st_uid != f_uid) s_uid = 0; if (st.st_gid != f_gid) s_gid = 0; }
		}
	}
	if (got && s_uid) {
		struct passwd *pw = getpwuid(f_uid);
		snprintf(owner_buf, sizeof(owner_buf), pw ? "%s" : "%u", pw ? pw->pw_name : (char *)(uintptr_t)f_uid);
		if (!pw) snprintf(owner_buf, sizeof(owner_buf), "%u", (unsigned)f_uid);
		snprintf(init_owner, sizeof(init_owner), "%s", owner_buf);
	}
	if (got && s_gid) {
		struct group *gr = getgrgid(f_gid);
		if (gr) snprintf(group_buf, sizeof(group_buf), "%s", gr->gr_name);
		else snprintf(group_buf, sizeof(group_buf), "%u", (unsigned)f_gid);
		snprintf(init_group, sizeof(init_group), "%s", group_buf);
	}

	int h, w; getmaxyx(stdscr, h, w);
	int bw = (w - 2 < 54) ? (w - 2) : 54, bh = 18;
	int sy = (h > bh) ? (h - bh) / 2 : 0, sx = (w > bw) ? (w - bw) / 2 : 0;
	WINDOW *win = newwin(bh, bw, sy, sx); wbkgd(win, COLOR_PAIR(5));

	const char *rlbl[4] = {"Owner:", "Group:", "Others:", "Special:"};
	const char *clbl[4][3] = { {"Read","Write","Exec"}, {"Read","Write","Exec"}, {"Read","Write","Exec"}, {"SUID","SGID","Sticky"} };
	const char *btns[2] = {"[ Set ]", "[ Cancel ]"};
	int foc = 0, rec = 0, applied = 0;

	while (1) {
		werase(win); box(win, 0, 0);
		mvwprintw(win, 0, 2, " File Attributes (Ctrl+A) ");
		if (pcnt == 1) {
			const char *bn = strrchr(paths[0], '/');
			mvwprintw(win, 1, 2, "Target: %.*s", bw - 12, bn ? bn + 1 : paths[0]);
		} else mvwprintw(win, 1, 2, "Target: %d selected items", pcnt);

		char oct[8] = "0";
		for (int g = 0; g < 4; g++) {
			int b0 = (g == 3) ? 9 : g * 3;
			if (state[b0] == 2 || state[b0+1] == 2 || state[b0+2] == 2) oct[g+1] = '?';
			else oct[g+1] = (char)('0' + (state[b0]?4:0) + (state[b0+1]?2:0) + (state[b0+2]?1:0));
		}
		mvwprintw(win, 2, 2, "Mode:   %s", oct);
		mvwhline(win, 3, 1, ACS_HLINE, bw - 2);
		for (int r = 0; r < 4; r++) {
			mvwprintw(win, 4 + r, 2, "%-9s", rlbl[r]);
			for (int c = 0; c < 3; c++) {
				int idx = r * 3 + c;
				char mk = (state[idx] == 1) ? 'x' : ((state[idx] == 2) ? '?' : ' ');
				wattrset(win, (foc == idx) ? COLOR_PAIR(2) : COLOR_PAIR(5));
				mvwprintw(win, 4 + r, 12 + c * 13, "[%c] %-7s", mk, clbl[r][c]);
			}
		}
		wattrset(win, COLOR_PAIR(5));
		mvwhline(win, 8, 1, ACS_HLINE, bw - 2);
		mvwprintw(win, 8, (bw - 11) / 2, " Ownership ");

		int fw = (bw - 16 < 10) ? 10 : bw - 16;
		mvwprintw(win, 9, 2, "Owner:");
		wattrset(win, (foc == 12) ? COLOR_PAIR(2) : COLOR_PAIR(4));
		mvwprintw(win, 9, 12, "%-*.*s", fw, fw, owner_buf);
		wattrset(win, COLOR_PAIR(5));
		mvwprintw(win, 10, 2, "Group:");
		wattrset(win, (foc == 13) ? COLOR_PAIR(2) : COLOR_PAIR(4));
		mvwprintw(win, 10, 12, "%-*.*s", fw, fw, group_buf);

		wattrset(win, COLOR_PAIR(5));
		mvwhline(win, 11, 1, ACS_HLINE, bw - 2);
		wattrset(win, (foc == 14) ? COLOR_PAIR(2) : COLOR_PAIR(5));
		mvwprintw(win, 12, 2, "[%c] Process subfolders (recursive)%s", rec ? 'x' : ' ', has_dir ? "" : " (no dirs)");
		wattrset(win, COLOR_PAIR(5));
		mvwhline(win, 13, 1, ACS_HLINE, bw - 2);

		UiButtonRow brow = { btns, 2, 15, (bw - 19) / 2, 2, (foc >= 15) ? (foc - 15) : -1 };
		ui_btns_draw(&brow, win);
		if (foc == 12 || foc == 13) { curs_set(1); wmove(win, (foc == 12) ? 9 : 10, 12 + (int)strlen((foc == 12) ? owner_buf : group_buf)); }
		else curs_set(0);
		wrefresh(win);

		InputEvent ev = input_read();
		if (ev.action == ACT_ESC || ev.action == ACT_F10) break;
		if (ev.action == ACT_MOUSE_LEFT) {
			int wy = ev.mouse_y - sy, wx = ev.mouse_x - sx;
			if (wy < 0 || wy >= bh || wx < 0 || wx >= bw) break;
			if (wy >= 4 && wy <= 7 && wx >= 12 && (wx - 12) / 13 < 3) { foc = (wy - 4) * 3 + (wx - 12) / 13; ev.action = ACT_CHAR; ev.ch = L' '; }
			else if (wy == 9) { foc = 12; continue; }
			else if (wy == 10) { foc = 13; continue; }
			else if (wy == 12) { foc = 14; ev.action = ACT_CHAR; ev.ch = L' '; }
			else { int hit = ui_btns_hit_test(&brow, wy, wx); if (hit == 0) { foc = 15; ev.action = ACT_ENTER; } else if (hit == 1) break; }
		}
		if ((foc == 12 || foc == 13) && ev.action == ACT_BACKSPACE) {
			char *b = (foc == 12) ? owner_buf : group_buf; size_t l = strlen(b); if (l > 0) b[l - 1] = '\0'; continue;
		}
		if ((foc == 12 || foc == 13) && ev.action == ACT_CHAR && ev.ch >= 33 && ev.ch < 127) {
			char *b = (foc == 12) ? owner_buf : group_buf; size_t l = strlen(b); if (l + 1 < 63) { b[l] = (char)ev.ch; b[l + 1] = '\0'; } continue;
		}
		if (ev.action == ACT_TAB) foc = (foc + 1) % 17;
		else if (ev.action == ACT_SHIFT_TAB) foc = (foc + 16) % 17;
		else if (ev.action == ACT_UP) foc = (foc >= 3 && foc <= 11) ? foc - 3 : (foc == 12 ? 9 : (foc == 13 ? 12 : (foc == 14 ? 13 : (foc >= 15 ? 14 : foc))));
		else if (ev.action == ACT_DOWN) foc = (foc <= 8) ? foc + 3 : (foc <= 11 ? 12 : (foc < 15 ? foc + 1 : foc));
		else if (ev.action == ACT_LEFT) { if (foc <= 11 && foc % 3 > 0) foc--; else if (foc == 16) foc = 15; }
		else if (ev.action == ACT_RIGHT) { if (foc <= 11 && foc % 3 < 2) foc++; else if (foc == 15) foc = 16; }
		else if (ev.action == ACT_CHAR && ev.ch == L' ') {
			if (foc <= 11) state[foc] = (init_state[foc] == 2) ? (state[foc] + 1) % 3 : !state[foc];
			else if (foc == 14) rec = !rec;
			else if (foc == 15) ev.action = ACT_ENTER;
			else if (foc == 16) break;
		}
		if (ev.action == ACT_ENTER) {
			if (foc == 16) break;
			uid_t n_uid = (uid_t)-1; gid_t n_gid = (gid_t)-1; int c_uid = 0, c_gid = 0;
			if (owner_buf[0] && strcmp(owner_buf, init_owner) != 0) {
				int r = resolve_uid(owner_buf, &n_uid);
				if (r < 0) { ui_message("Attributes Error", "Unknown owner user"); continue; }
				c_uid = (r == 1);
			}
			if (group_buf[0] && strcmp(group_buf, init_group) != 0) {
				int r = resolve_gid(group_buf, &n_gid);
				if (r < 0) { ui_message("Attributes Error", "Unknown group"); continue; }
				c_gid = (r == 1);
			}
			mode_t sm = 0, cm = 0;
			for (int b = 0; b < 12; b++) { if (state[b] == 1) sm |= k_bits[b]; else if (state[b] == 0) cm |= k_bits[b]; }
			int errs = 0;
			for (int i = 0; i < pcnt; i++) apply_attrs_recursive(paths[i], sm, cm, c_uid, n_uid, c_gid, n_gid, rec, &errs);
			if (errs > 0) { char em[96]; snprintf(em, sizeof(em), "Failed to update attributes on %d item(s)", errs); ui_message("Attributes Warning", em); }
			applied = 1; break;
		}
	}
	curs_set(0); delwin(win); touchwin(stdscr); return applied;
}

static off_t calc_dir_walk(const char *path, int *steps, int *cancelled) {
	if (*cancelled) return 0;
	if ((++(*steps) & 255) == 0) {
		fd_set fds; FD_ZERO(&fds); FD_SET(STDIN_FILENO, &fds);
		struct timeval tv = {0, 0};
		if (select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) > 0) {
			char ch = 0;
			if (read(STDIN_FILENO, &ch, 1) == 1 && ch == 27) { *cancelled = 1; return 0; }
		}
	}
	DIR *d = opendir(path);
	if (!d) return 0;
	off_t total = 0;
	struct dirent *de;
	while ((de = readdir(d)) != NULL && !*cancelled) {
		if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
		char sub[MAX_PATH * 2];
		snprintf(sub, sizeof(sub), "%s/%s", path, de->d_name);
		struct stat st;
		if (lstat(sub, &st) != 0) continue;
		if (S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode)) total += calc_dir_walk(sub, steps, cancelled);
		else total += st.st_size;
	}
	closedir(d);
	return total;
}

void panel_calc_dir_sizes(Panel *panel) {
	if (!panel || panel->count == 0 || panel->archive_file[0] != '\0') return;
	int marked_dirs = 0;
	for (int i = 0; i < panel->count; i++) {
		if (panel->files[i].is_selected && panel->files[i].is_dir && strcmp(panel->files[i].name, "..") != 0) marked_dirs++;
	}
	int on_up = (strcmp(panel->files[panel->selected].name, "..") == 0);
	int all_mode = (marked_dirs == 0 && on_up);

	int h, w; getmaxyx(stdscr, h, w);
	int bw = (w - 4 < 48) ? (w - 4) : 48, bh = 5;
	WINDOW *win = newwin(bh, bw, (h - bh) / 2, (w - bw) / 2);
	wbkgd(win, COLOR_PAIR(5)); box(win, 0, 0);
	mvwprintw(win, 1, 2, "Calculating folder size...");
	mvwprintw(win, 3, 2, "Press Esc to cancel");
	wrefresh(win);

	int steps = 0, cancelled = 0;
	off_t total_all = 0;
	for (int i = 0; i < panel->count && !cancelled; i++) {
		FileEntry *fe = &panel->files[i];
		if (strcmp(fe->name, "..") == 0) continue;
		if (!fe->is_dir) {
			if (all_mode) total_all += fe->size;
			continue;
		}
		int target = all_mode ? 1 : (marked_dirs > 0 ? fe->is_selected : (i == panel->selected));
		if (!target) continue;
		char fp[MAX_PATH * 2];
		panel_entry_full_path(panel, i, fp, sizeof(fp));
		off_t sz = calc_dir_walk(fp, &steps, &cancelled);
		if (!cancelled) { fe->size = sz; fe->dir_size_known = 1; total_all += sz; }
	}
	if (!cancelled && all_mode && on_up) {
		panel->files[panel->selected].size = total_all;
		panel->files[panel->selected].dir_size_known = 1;
	}
	delwin(win); touchwin(stdscr);
}
