#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <dirent.h>
#include <fnmatch.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>
#include <ncurses.h>
#include <wchar.h>
#include "find.h"
#include "xc.h"
#include "input.h"
#include "ui.h"
#include "panel.h"
#include "widgets.h"
#include "editor.h"
#include "viewer.h"
#include <wctype.h>
#include "exec.h"

#define MAX_FIND_RESULTS 2048

typedef struct {
	char rel_path[MAX_PATH];
	char full_path[MAX_PATH * 2];
	off_t size;
	int is_dir;
	int match_line;
} FindResult;

typedef struct {
	FindResult *items;
	int count;
	int cap;
	char base_dir[MAX_PATH];
	char mask[128];
	char text[256];
	int recursive;
	int case_sens;
	int skip_hidden;
	long scanned;
	int aborted;
	long last_draw_ms;
	WINDOW *prog_win;
	int prog_w;
	char cur_dir[MAX_PATH];
} FindCtx;

static long find_now_ms(void) {
	struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long)(ts.tv_sec * 1000L + ts.tv_nsec / 1000000L);
}

static void find_draw_progress(FindCtx *ctx, int force) {
	if (!ctx->prog_win) return;
	long now = find_now_ms();
	if (!force && (now - ctx->last_draw_ms) < 45) return;
	ctx->last_draw_ms = now;
	if (input_check_esc()) { ctx->aborted = 1; return; }

	int bw = ctx->prog_w;
	const char spin[4] = { '|', '/', '-', '\\' };
	char sp = spin[(now / 90) % 4];
	werase(ctx->prog_win); box(ctx->prog_win, 0, 0);
	mvwprintw(ctx->prog_win, 0, 2, " Searching (%c) [Esc: Stop] ", sp);
	mvwprintw(ctx->prog_win, 1, 2, "Found: %-6d   Scanned: %-9ld", ctx->count, ctx->scanned);

	int bar_w = bw - 4; if (bar_w < 10) bar_w = 10;
	int pos = (int)((now / 60) % (bar_w - 4));
	mvwhline(ctx->prog_win, 2, 2, ACS_HLINE, bar_w);
	mvwaddstr(ctx->prog_win, 2, 2 + pos, "<==>");

	int dmax = bw - 4; if (dmax < 8) dmax = 8;
	int dlen = (int)strlen(ctx->cur_dir);
	if (dlen <= dmax) mvwprintw(ctx->prog_win, 3, 2, "%-*s", dmax, ctx->cur_dir);
	else mvwprintw(ctx->prog_win, 3, 2, "...%s", ctx->cur_dir + (dlen - (dmax - 3)));
	wrefresh(ctx->prog_win);
}

static int match_multi_mask(const char *name, const char *mask_list) {
	if (!mask_list || !*mask_list || strcmp(mask_list, "*") == 0) return 1;
	char buf[256];
	snprintf(buf, sizeof(buf), "%s", mask_list);
	char *save = NULL;
	for (char *tok = strtok_r(buf, ";,", &save); tok; tok = strtok_r(NULL, ";,", &save)) {
		while (*tok == ' ') tok++;
		size_t tl = strlen(tok);
		while (tl > 0 && tok[tl - 1] == ' ') tok[--tl] = '\0';
		if (!*tok) continue;
		if (fnmatch(tok, name, FNM_CASEFOLD) == 0) return 1;
	}
	return 0;
}

static int search_file_content(FindCtx *ctx, const char *path, const char *needle, int case_sens, int *out_line) {
	if (!needle || !*needle) { if (out_line) *out_line = 1; return 1; }
	FILE *f = fopen(path, "rb");
	if (!f) return 0;

	char probe[2048];
	size_t pn = fread(probe, 1, sizeof(probe), f);
	for (size_t i = 0; i < pn; i++) {
		if (probe[i] == '\0') { fclose(f); return 0; }
	}
	rewind(f);

	char line[8192];
	int ln = 0;
	while (fgets(line, sizeof(line), f)) {
		ln++;
		if ((ln & 511) == 0 && ctx) { find_draw_progress(ctx, 0); if (ctx->aborted) { fclose(f); return 0; } }
		const char *hit = case_sens ? strstr(line, needle) : strcasestr(line, needle);
		if (hit) {
			if (out_line) *out_line = ln;
			fclose(f);
			return 1;
		}
	}
	fclose(f);
	return 0;
}

static void find_add_result(FindCtx *ctx, const char *rel, const char *full, off_t sz, int is_dir, int mline) {
	if (ctx->count >= MAX_FIND_RESULTS) return;
	if (ctx->count >= ctx->cap) {
		int nc = ctx->cap ? ctx->cap * 2 : 128;
		FindResult *ni = realloc(ctx->items, (size_t)nc * sizeof(FindResult));
		if (!ni) return;
		ctx->items = ni;
		ctx->cap = nc;
	}
	FindResult *r = &ctx->items[ctx->count++];
	snprintf(r->rel_path, sizeof(r->rel_path), "%s", rel);
	snprintf(r->full_path, sizeof(r->full_path), "%s", full);
	r->size = sz;
	r->is_dir = is_dir;
	r->match_line = mline;
}

static void find_walk_dir(FindCtx *ctx, const char *abs_dir, const char *rel_prefix) {
	if (ctx->count >= MAX_FIND_RESULTS || ctx->aborted) return;
	if (!strcmp(abs_dir, "/proc") || !strcmp(abs_dir, "/sys") || !strcmp(abs_dir, "/dev")) return;
	snprintf(ctx->cur_dir, sizeof(ctx->cur_dir), "%s", abs_dir);
	find_draw_progress(ctx, 0);
	DIR *d = opendir(abs_dir);
	if (!d) return;
	struct dirent *de;
	while ((de = readdir(d)) != NULL) {
		if (ctx->count >= MAX_FIND_RESULTS || ctx->aborted) break;
		ctx->scanned++;
		if ((ctx->scanned & 31) == 0) find_draw_progress(ctx, 0);
		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
		if (ctx->skip_hidden && de->d_name[0] == '.') continue;
		if (!strcmp(de->d_name, ".git")) continue;

		char full[MAX_PATH * 2], rel[MAX_PATH];
		if (!strcmp(abs_dir, "/")) snprintf(full, sizeof(full), "/%s", de->d_name);
		else snprintf(full, sizeof(full), "%s/%s", abs_dir, de->d_name);

		if (rel_prefix && *rel_prefix) snprintf(rel, sizeof(rel), "%s/%s", rel_prefix, de->d_name);
		else snprintf(rel, sizeof(rel), "%s", de->d_name);

		struct stat st;
		if (lstat(full, &st) != 0) continue;
		int is_d = S_ISDIR(st.st_mode);

		if (match_multi_mask(de->d_name, ctx->mask)) {
			if (ctx->text[0] == '\0') {
				find_add_result(ctx, rel, full, is_d ? 0 : st.st_size, is_d, 1);
			} else if (!is_d && S_ISREG(st.st_mode)) {
				int mline = 1;
				if (search_file_content(ctx, full, ctx->text, ctx->case_sens, &mline)) {
					find_add_result(ctx, rel, full, st.st_size, 0, mline);
				}
			}
		}

		if (is_d && ctx->recursive && !S_ISLNK(st.st_mode)) {
			find_walk_dir(ctx, full, rel);
		}
	}
	closedir(d);
}

static int ui_find_prompt(FindCtx *ctx) {
	int h, w; getmaxyx(stdscr, h, w);
	int bw = (w - 10 > 48) ? 48 : (w - 4); if (bw < 38) bw = w - 2;
	int bh = 11, sy = (h - bh) / 2, sx = (w - bw) / 2;
	WINDOW *win = newwin(bh, bw, sy, sx); wbkgd(win, g_ui_theme.win_bg);

	wchar_t wmask[128] = {0}, wtext[256] = {0};
	mbstowcs(wmask, ctx->mask, 127);
	mbstowcs(wtext, ctx->text, 255);

	int field_w = (bw - 6 < 8) ? 8 : (bw - 6);
	UiTextField tf_mask, tf_text;
	ui_tf_init(&tf_mask, wmask, 127, 1);
	ui_tf_set_bounds(&tf_mask, 2, 3, field_w);
	ui_tf_init(&tf_text, wtext, 255, 1);
	ui_tf_set_bounds(&tf_text, 4, 3, field_w);

	const char *btn_labels[2] = { "[ Search ]", "[ Cancel ]" };
	int btn_x = (bw - 22 < 2) ? 2 : (bw - 22) / 2;
	UiButtonRow btns = { btn_labels, 2, 9, btn_x, 2, -1 };

	int field = 0;

	while (1) {
		werase(win); box(win, 0, 0);
		mvwprintw(win, 0, 2, " Find Files (Alt+F7) ");
		mvwprintw(win, 1, 2, "File mask(s) (e.g. *.c;*.h):");
		ui_tf_draw(&tf_mask, win, field == 0);
		mvwprintw(win, 3, 2, "Containing text (optional):");
		ui_tf_draw(&tf_text, win, field == 1);

		UiCheckbox cbs[3] = {
			{ "Recursive subdirectories", &ctx->recursive, 5, 2 },
			{ "Case sensitive text", &ctx->case_sens, 6, 2 },
			{ "Skip hidden files/dirs", &ctx->skip_hidden, 7, 2 }
		};
		for (int c = 0; c < 3; c++) ui_cb_draw(&cbs[c], win, field == c + 2);

		mvwhline(win, 8, 1, ACS_HLINE, bw - 2);
		btns.focus_idx = (field == 5) ? 0 : ((field == 6) ? 1 : -1);
		ui_btns_draw(&btns, win);

		if (field == 0) ui_tf_place_cursor(&tf_mask, win);
		else if (field == 1) ui_tf_place_cursor(&tf_text, win);
		else curs_set(0);

		printf("\033[?2026h"); fflush(stdout); wnoutrefresh(win); doupdate(); printf("\033[?2026l"); fflush(stdout);
		InputEvent ev = input_read();
		if (ev.action == ACT_MOUSE_LEFT) {
			int wy = ev.mouse_y - sy, wx = ev.mouse_x - sx;
			if (wy < 0 || wy >= bh || wx < 0 || wx >= bw) { curs_set(0); delwin(win); touchwin(stdscr); return 0; }
			if (ui_tf_handle_mouse(&tf_mask, wy, wx)) { field = 0; continue; }
			if (ui_tf_handle_mouse(&tf_text, wy, wx)) { field = 1; continue; }
			if (wy == 5) { field = 2; ctx->recursive = !ctx->recursive; continue; }
			if (wy == 6) { field = 3; ctx->case_sens = !ctx->case_sens; continue; }
			if (wy == 7) { field = 4; ctx->skip_hidden = !ctx->skip_hidden; continue; }
			int hit = ui_btns_hit_test(&btns, wy, wx);
			if (hit == 0) { field = 5; ev.action = ACT_ENTER; }
			else if (hit == 1) { curs_set(0); delwin(win); touchwin(stdscr); return 0; }
			else continue;
		}
		if (ev.action == ACT_ESC) { curs_set(0); delwin(win); touchwin(stdscr); return 0; }
		if (ev.action == ACT_ENTER || ((field == 5 || field == 6) && ev.action == ACT_CHAR && ev.ch == ' ')) {
			if (field == 6) { curs_set(0); delwin(win); touchwin(stdscr); return 0; }
			wcstombs(ctx->mask, wmask, sizeof(ctx->mask) - 1); ctx->mask[sizeof(ctx->mask) - 1] = '\0';
			wcstombs(ctx->text, wtext, sizeof(ctx->text) - 1); ctx->text[sizeof(ctx->text) - 1] = '\0';
			curs_set(0); delwin(win); touchwin(stdscr); return 1;
		}
		if (ev.action == ACT_TAB || ev.action == ACT_DOWN) { field = (field + 1) % 7; continue; }
		if (ev.action == ACT_UP || ev.action == ACT_SHIFT_TAB) { field = (field + 6) % 7; continue; }
		if ((field == 5 || field == 6) && (ev.action == ACT_LEFT || ev.action == ACT_RIGHT)) { field = (field == 5) ? 6 : 5; continue; }
		if (field >= 2 && field <= 4 && ev.action == ACT_CHAR && ev.ch == ' ') {
			if (field == 2) ctx->recursive = !ctx->recursive;
			else if (field == 3) ctx->case_sens = !ctx->case_sens;
			else if (field == 4) ctx->skip_hidden = !ctx->skip_hidden;
			continue;
		}
		if (field == 0) ui_tf_handle_event(&tf_mask, &ev);
		else if (field == 1) ui_tf_handle_event(&tf_text, &ev);
	}
}

int ui_find_files(const char *start_dir, char *out_path, size_t out_sz) {
	if (!start_dir || !*start_dir) return 0;
	static char last_mask[128] = "*", last_text[256] = "";
	static int last_rec = 1, last_cs = 0, last_skip = 0;

	FindCtx ctx; memset(&ctx, 0, sizeof(ctx));
	snprintf(ctx.base_dir, sizeof(ctx.base_dir), "%s", start_dir);
	snprintf(ctx.mask, sizeof(ctx.mask), "%s", last_mask);
	snprintf(ctx.text, sizeof(ctx.text), "%s", last_text);
	ctx.recursive = last_rec; ctx.case_sens = last_cs; ctx.skip_hidden = last_skip;

	if (!ui_find_prompt(&ctx)) return 0;
	snprintf(last_mask, sizeof(last_mask), "%s", ctx.mask[0] ? ctx.mask : "*");
	snprintf(last_text, sizeof(last_text), "%s", ctx.text);
	last_rec = ctx.recursive; last_cs = ctx.case_sens; last_skip = ctx.skip_hidden;
	if (ctx.mask[0] == '\0') strcpy(ctx.mask, "*");

	touchwin(stdscr); refresh();
	int sh, sw; getmaxyx(stdscr, sh, sw);
	int pw = (sw - 10 > 62) ? 62 : (sw - 4); if (pw < 34) pw = sw - 2;
	int ph = 5, psy = (sh - ph) / 2, psx = (sw - pw) / 2;
	ctx.prog_w = pw;
	ctx.prog_win = newwin(ph, pw, psy, psx);
	if (ctx.prog_win) { wbkgd(ctx.prog_win, g_ui_theme.win_bg); }
	snprintf(ctx.cur_dir, sizeof(ctx.cur_dir), "%s", ctx.base_dir);
	find_draw_progress(&ctx, 1);

	find_walk_dir(&ctx, ctx.base_dir, "");

	if (ctx.prog_win) { delwin(ctx.prog_win); ctx.prog_win = NULL; touchwin(stdscr); refresh(); }
	if (ctx.count == 0) {
		ui_message("Find Files", ctx.aborted ? "Search stopped (0 matches)" : "No matching files found");
		free(ctx.items);
		return 0;
	}

	int h, w; getmaxyx(stdscr, h, w);
	int bw = (w - 6 > 96) ? 96 : (w - 4); if (bw < 52) bw = w - 2;
	int bh = (h - 4 > 30) ? 30 : (h - 2); if (bh < 12) bh = h - 2;
	int sy = (h - bh) / 2, sx = (w - bw) / 2;
	WINDOW *win = newwin(bh, bw, sy, sx); wbkgd(win, COLOR_PAIR(5));

	int sel = 0, top = 0, ret_code = 0, btn_focus = 0;
	while (1) {
		int vis = bh - 5; if (vis < 1) vis = 1;
		if (sel < 0) sel = 0;
		if (sel >= ctx.count) sel = ctx.count - 1;
		if (sel < top) top = sel;
		if (sel >= top + vis) top = sel - vis + 1;

		werase(win); box(win, 0, 0);
		char hdr[192];
		snprintf(hdr, sizeof(hdr), " Found %d%s item(s) [%s] ", ctx.count, (ctx.count >= MAX_FIND_RESULTS) ? "+" : "", ctx.mask);
		mvwaddnstr(win, 0, 2, hdr, bw - 4);

		for (int i = 0; i < vis; i++) {
			int idx = top + i; mvwhline(win, 1 + i, 2, ' ', bw - 4);
			if (idx >= ctx.count) continue;
			const FindResult *r = &ctx.items[idx];
			char row[MAX_PATH + 64];
			if (r->is_dir) snprintf(row, sizeof(row), "[DIR]  %s/", r->rel_path);
			else if (ctx.text[0] != '\0') snprintf(row, sizeof(row), "L:%-4d %s", r->match_line, r->rel_path);
			else snprintf(row, sizeof(row), "%8lld  %s", (long long)r->size, r->rel_path);
			if (idx == sel) wattron(win, COLOR_PAIR(3));
			mvwaddnstr(win, 1 + i, 2, row, bw - 4);
			if (idx == sel) wattroff(win, COLOR_PAIR(3));
		}

		mvwhline(win, bh - 4, 1, ACS_HLINE, bw - 2);
		mvwaddnstr(win, bh - 3, 2, ctx.items[sel].full_path, bw - 4);

		const char *blbls[6] = { "[ Go to ]", "[ View (F3) ]", "[ Edit (F4) ]", "[ Insert ]", "[ Panel ]", "[ Close ]" };
		int blens[6] = { 9, 13, 13, 10, 9, 9 };
		int total_bw = 9 + 2 + 13 + 2 + 13 + 2 + 10 + 2 + 9 + 2 + 9;
		int bx = (bw - total_bw) / 2;
		if (bx < 2) bx = 2;
		int bpos[6], cur_x = bx;
		for (int b = 0; b < 6; b++) {
			bpos[b] = cur_x;
			wattron(win, (btn_focus == b) ? COLOR_PAIR(2) : COLOR_PAIR(5));
			mvwprintw(win, bh - 2, cur_x, "%s", blbls[b]);
			wattroff(win, (btn_focus == b) ? COLOR_PAIR(2) : COLOR_PAIR(5));
			cur_x += blens[b] + 2;
		}

		printf("\033[?2026h"); fflush(stdout); wnoutrefresh(win); doupdate(); printf("\033[?2026l"); fflush(stdout);
		InputEvent ev = input_read();
		if (ev.action == ACT_ESC || ev.action == ACT_F10) break;
		if (ev.action == ACT_UP) { sel--; continue; }
		if (ev.action == ACT_DOWN) { sel++; continue; }
		if (ev.action == ACT_PGUP) { sel -= vis; continue; }
		if (ev.action == ACT_PGDN) { sel += vis; continue; }
		if (ev.action == ACT_HOME) { sel = 0; continue; }
		if (ev.action == ACT_END) { sel = ctx.count - 1; continue; }
		if (ev.action == ACT_TAB || ev.action == ACT_RIGHT) { btn_focus = (btn_focus + 1) % 6; continue; }
		if (ev.action == ACT_SHIFT_TAB || ev.action == ACT_LEFT) { btn_focus = (btn_focus + 5) % 6; continue; }
		if (ev.action == ACT_MOUSE_WHEEL_UP) { sel -= 3; continue; }
		if (ev.action == ACT_MOUSE_WHEEL_DOWN) { sel += 3; continue; }
		if (ev.action == ACT_MOUSE_LEFT || ev.action == ACT_MOUSE_DBL) {
			int wy = ev.mouse_y - sy, wx = ev.mouse_x - sx;
			if (wy < 0 || wy >= bh || wx < 0 || wx >= bw) break;
			if (wy >= 1 && wy <= vis) {
				int clicked = top + (wy - 1);
				if (clicked >= 0 && clicked < ctx.count) {
					int same = (clicked == sel);
					sel = clicked; btn_focus = 0;
					if (ev.action == ACT_MOUSE_DBL || same) ev.action = ACT_ENTER;
					else continue;
				} else continue;
			} else if (wy == bh - 2) {
				int hit = -1;
				for (int b = 0; b < 6; b++) {
					if (wx >= bpos[b] && wx < bpos[b] + blens[b]) { hit = b; break; }
				}
				if (hit == -1) continue;
				btn_focus = hit;
				if (hit == 0) ev.action = ACT_ENTER;
				else if (hit == 1) ev.action = ACT_F3;
				else if (hit == 2) ev.action = ACT_F4;
				else if (hit == 3) ev.action = ACT_INSERT_NAME;
				else if (hit == 4) {
					temp_panel_clear();
					for (int k = 0; k < ctx.count; k++) temp_panel_add(ctx.items[k].full_path);
					ret_code = 3; break;
				}
				else break;
			} else continue;
		}
		if (ev.action == ACT_CHAR && ev.ch == ' ') {
			if (btn_focus == 0) ev.action = ACT_ENTER;
			else if (btn_focus == 1) ev.action = ACT_F3;
			else if (btn_focus == 2) ev.action = ACT_F4;
			else if (btn_focus == 3) ev.action = ACT_INSERT_NAME;
			else if (btn_focus == 4) {
				temp_panel_clear();
				for (int k = 0; k < ctx.count; k++) temp_panel_add(ctx.items[k].full_path);
				ret_code = 3; break;
			}
			else break;
		}
		if (ev.action == ACT_ENTER) {
			if (btn_focus == 1) ev.action = ACT_F3;
			else if (btn_focus == 2) ev.action = ACT_F4;
			else if (btn_focus == 3) ev.action = ACT_INSERT_NAME;
			else if (btn_focus == 4) {
				temp_panel_clear();
				for (int k = 0; k < ctx.count; k++) temp_panel_add(ctx.items[k].full_path);
				ret_code = 3; break;
			}
			else if (btn_focus == 5) break;
		}

		const FindResult *cur = &ctx.items[sel];
		if (ev.action == ACT_ENTER) { if (out_path && out_sz > 0) snprintf(out_path, out_sz, "%s", cur->full_path); ret_code = 1; break; }
		if (ev.action == ACT_INSERT_NAME) { if (out_path && out_sz > 0) snprintf(out_path, out_sz, "%s", cur->full_path); ret_code = 2; break; }
		if (ev.action == ACT_F3 && !cur->is_dir) { ui_view_file(cur->full_path); touchwin(win); }
		if (ev.action == ACT_F4 && !cur->is_dir) { ui_edit_file_at(cur->full_path, cur->match_line, 1); touchwin(win); }
	}
	delwin(win); touchwin(stdscr); free(ctx.items);
	return ret_code;
}
