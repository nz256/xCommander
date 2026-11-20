#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <dirent.h>
#include <fnmatch.h>
#include <unistd.h>
#include <sys/stat.h>
#include <ncurses.h>
#include <wchar.h>
#include "find.h"
#include "xc.h"
#include "input.h"
#include "ui.h"
#include <wctype.h>
#include "exec.h"

static int find_is_word_sep(wchar_t ch) {
	return iswspace(ch) || ch == L'/' || ch == L'-' || ch == L'_' || ch == L'.' || ch == L';';
}

static int find_word_left(const wchar_t *buf, int pos) {
	while (pos > 0 && find_is_word_sep(buf[pos - 1])) pos--;
	while (pos > 0 && !find_is_word_sep(buf[pos - 1])) pos--;
	return pos;
}

static int find_word_right(const wchar_t *buf, int pos, int len) {
	while (pos < len && !find_is_word_sep(buf[pos])) pos++;
	while (pos < len && find_is_word_sep(buf[pos])) pos++;
	return pos;
}

static void find_draw_input(WINDOW *win, int y, int x, int w, const wchar_t *buf, int pos, int anchor, int active) {
	wattron(win, COLOR_PAIR(2));
	mvwhline(win, y, x - 1, ' ', w + 2);
	int off = (pos >= w) ? (pos - w + 1) : 0;
	mvwaddnwstr(win, y, x, buf + off, w);
	wattroff(win, COLOR_PAIR(2));

	if (active && anchor != -1 && anchor != pos) {
		int s1 = (anchor < pos) ? anchor : pos;
		int s2 = (anchor > pos) ? anchor : pos;
		if (s1 < off) s1 = off;
		if (s2 > off + w) s2 = off + w;
		if (s2 > s1) {
			wattron(win, COLOR_PAIR(6));
			mvwaddnwstr(win, y, x + (s1 - off), buf + s1, s2 - s1);
			wattroff(win, COLOR_PAIR(6));
		}
	}
}

#include "viewer.h"
#include "editor.h"

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
} FindCtx;

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

static int search_file_content(const char *path, const char *needle, int case_sens, int *out_line) {
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
	if (ctx->count >= MAX_FIND_RESULTS) return;
	DIR *d = opendir(abs_dir);
	if (!d) return;
	struct dirent *de;
	while ((de = readdir(d)) != NULL) {
		if (ctx->count >= MAX_FIND_RESULTS) break;
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
				if (search_file_content(full, ctx->text, ctx->case_sens, &mline)) {
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

static int __attribute__((unused)) wline_cols(const wchar_t *ws, int count) {
	int cols = 0;
	for (int i = 0; i < count && ws[i]; i++) {
		int cw = wcwidth(ws[i]);
		cols += (cw > 0) ? cw : 1;
	}
	return cols;
}

static int ui_find_prompt(FindCtx *ctx) {
	int h, w; getmaxyx(stdscr, h, w);
	int bw = (w - 10 > 48) ? 48 : (w - 4); if (bw < 38) bw = w - 2;
	int bh = 11, sy = (h - bh) / 2, sx = (w - bw) / 2;
	WINDOW *win = newwin(bh, bw, sy, sx); wbkgd(win, COLOR_PAIR(5));

	wchar_t wmask[128] = {0}, wtext[256] = {0};
	mbstowcs(wmask, ctx->mask, 127);
	mbstowcs(wtext, ctx->text, 255);
	int field = 0, mpos = (int)wcslen(wmask), tpos = (int)wcslen(wtext);
	int manchor = (mpos > 0) ? 0 : -1, tanchor = (tpos > 0) ? 0 : -1;
	int field_w = (bw - 6 < 8) ? 8 : (bw - 6);

	while (1) {
		werase(win); box(win, 0, 0);
		mvwprintw(win, 0, 2, " Find Files (Alt+F7) ");
		mvwprintw(win, 1, 2, "File mask(s) (e.g. *.c;*.h):");
		find_draw_input(win, 2, 3, field_w, wmask, mpos, manchor, field == 0);
		mvwprintw(win, 3, 2, "Containing text (optional):");
		find_draw_input(win, 4, 3, field_w, wtext, tpos, tanchor, field == 1);

		if (field == 2) wattron(win, COLOR_PAIR(3));
		mvwprintw(win, 5, 2, "[%c] Recursive subdirectories", ctx->recursive ? 'x' : ' ');
		if (field == 2) wattroff(win, COLOR_PAIR(3));
		if (field == 3) wattron(win, COLOR_PAIR(3));
		mvwprintw(win, 6, 2, "[%c] Case sensitive text", ctx->case_sens ? 'x' : ' ');
		if (field == 3) wattroff(win, COLOR_PAIR(3));
		if (field == 4) wattron(win, COLOR_PAIR(3));
		mvwprintw(win, 7, 2, "[%c] Skip hidden files/dirs", ctx->skip_hidden ? 'x' : ' ');
		if (field == 4) wattroff(win, COLOR_PAIR(3));

		mvwhline(win, 8, 1, ACS_HLINE, bw - 2);
		int btn_x = (bw - 22 < 2) ? 2 : (bw - 22) / 2;
		wattron(win, (field == 5) ? COLOR_PAIR(2) : COLOR_PAIR(5));
		mvwprintw(win, 9, btn_x, "[ Search ]");
		wattroff(win, (field == 5) ? COLOR_PAIR(2) : COLOR_PAIR(5));
		wattron(win, (field == 6) ? COLOR_PAIR(2) : COLOR_PAIR(5));
		mvwprintw(win, 9, btn_x + 12, "[ Cancel ]");
		wattroff(win, (field == 6) ? COLOR_PAIR(2) : COLOR_PAIR(5));

		if (field == 0 || field == 1) {
			curs_set(1);
			int cur_p = (field == 0) ? mpos : tpos;
			int off = (cur_p >= field_w) ? (cur_p - field_w + 1) : 0;
			wmove(win, (field == 0) ? 2 : 4, 3 + (cur_p - off));
		} else curs_set(0);

		printf("\033[?2026h"); fflush(stdout); wnoutrefresh(win); doupdate(); printf("\033[?2026l"); fflush(stdout);
		InputEvent ev = input_read();
		if (ev.action == ACT_MOUSE_LEFT) {
			int wy = ev.mouse_y - sy, wx = ev.mouse_x - sx;
			if (wy < 0 || wy >= bh || wx < 0 || wx >= bw) { curs_set(0); delwin(win); touchwin(stdscr); return 0; }
			if ((wy == 2 || wy == 4) && wx >= 2 && wx < bw - 2) {
				field = (wy == 2) ? 0 : 1;
				int *pp = (field == 0) ? &mpos : &tpos, *ap = (field == 0) ? &manchor : &tanchor;
				int blen = (int)wcslen((field == 0) ? wmask : wtext);
				int off = (*pp >= field_w) ? (*pp - field_w + 1) : 0;
				int np = off + (wx - 3); if (np < 0) np = 0;
				*pp = (np > blen) ? blen : np; *ap = -1; continue;
			}
			if (wy == 5) { field = 2; ctx->recursive = !ctx->recursive; continue; }
			if (wy == 6) { field = 3; ctx->case_sens = !ctx->case_sens; continue; }
			if (wy == 7) { field = 4; ctx->skip_hidden = !ctx->skip_hidden; continue; }
			if (wy == 9 && wx >= btn_x && wx < btn_x + 10) { field = 5; ev.action = ACT_ENTER; }
			else if (wy == 9 && wx >= btn_x + 12 && wx < btn_x + 22) { curs_set(0); delwin(win); touchwin(stdscr); return 0; }
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
		if (ev.action == ACT_UP) { field = (field + 6) % 7; continue; }
		if ((field == 5 || field == 6) && (ev.action == ACT_LEFT || ev.action == ACT_RIGHT)) { field = (field == 5) ? 6 : 5; continue; }
		if (field >= 2 && field <= 4 && ev.action == ACT_CHAR && ev.ch == ' ') {
			if (field == 2) ctx->recursive = !ctx->recursive;
			else if (field == 3) ctx->case_sens = !ctx->case_sens;
			else if (field == 4) ctx->skip_hidden = !ctx->skip_hidden;
			continue;
		}
		if (field == 0 || field == 1) {
			wchar_t *buf = (field == 0) ? wmask : wtext;
			int max_c = (field == 0) ? 127 : 255, *pos = (field == 0) ? &mpos : &tpos, *anc = (field == 0) ? &manchor : &tanchor, len = (int)wcslen(buf);
			if (ev.action == ACT_WORD_SEL_LEFT) { if (*anc == -1) *anc = *pos; *pos = find_word_left(buf, *pos); }
			else if (ev.action == ACT_WORD_SEL_RIGHT) { if (*anc == -1) *anc = *pos; *pos = find_word_right(buf, *pos, len); }
			else if (ev.action == ACT_SHIFT_LEFT) { if (*anc == -1) *anc = *pos; if (*pos > 0) (*pos)--; }
			else if (ev.action == ACT_SHIFT_RIGHT) { if (*anc == -1) *anc = *pos; if (*pos < len) (*pos)++; }
			else if (ev.action == ACT_SHIFT_HOME) { if (*anc == -1) *anc = *pos; *pos = 0; }
			else if (ev.action == ACT_SHIFT_END) { if (*anc == -1) *anc = *pos; *pos = len; }
			else if (ev.action == ACT_CTRL_LEFT) { *anc = -1; *pos = find_word_left(buf, *pos); }
			else if (ev.action == ACT_CTRL_RIGHT) { *anc = -1; *pos = find_word_right(buf, *pos, len); }
			else if (ev.action == ACT_LEFT) { *anc = -1; if (*pos > 0) (*pos)--; }
			else if (ev.action == ACT_RIGHT) { *anc = -1; if (*pos < len) (*pos)++; }
			else if (ev.action == ACT_HOME || ev.action == ACT_CTRL_A) { *anc = -1; *pos = 0; }
			else if (ev.action == ACT_END) { *anc = -1; *pos = len; }
			else if (ev.action == ACT_CTRL_U) { buf[0] = L'\0'; *pos = 0; *anc = -1; }
			else if (ev.action == ACT_BACKSPACE || ev.action == ACT_DELETE) {
				if (*anc != -1 && *anc != *pos) {
					int s1 = (*anc < *pos) ? *anc : *pos, s2 = (*anc > *pos) ? *anc : *pos;
					wmemmove(buf + s1, buf + s2, (size_t)(len - s2 + 1)); *pos = s1; *anc = -1;
				} else if (ev.action == ACT_BACKSPACE && *pos > 0) { wmemmove(buf + *pos - 1, buf + *pos, (size_t)(len - *pos + 1)); (*pos)--; }
				else if (ev.action == ACT_DELETE && *pos < len) { wmemmove(buf + *pos, buf + *pos + 1, (size_t)(len - *pos)); }
			} else if (ev.action == ACT_CTRL_C || ev.action == ACT_CTRL_Y || ev.action == ACT_CTRL_X) {
				int s1 = (*anc != -1 && *anc != *pos) ? ((*anc < *pos) ? *anc : *pos) : 0;
				int s2 = (*anc != -1 && *anc != *pos) ? ((*anc > *pos) ? *anc : *pos) : len;
				if (s2 > s1) {
					wchar_t wsub[256]; wmemcpy(wsub, buf + s1, (size_t)(s2 - s1)); wsub[s2 - s1] = L'\0';
					char utf_sub[512];
					if (wcstombs(utf_sub, wsub, sizeof(utf_sub)) != (size_t)-1) { utf_sub[sizeof(utf_sub) - 1] = '\0'; clipboard_copy(utf_sub); }
					if (ev.action == ACT_CTRL_X && *anc != -1 && *anc != *pos) { wmemmove(buf + s1, buf + s2, (size_t)(len - s2 + 1)); *pos = s1; *anc = -1; }
				}
			} else if (ev.action == ACT_CTRL_V || ev.action == ACT_SHIFT_INSERT) {
				if (*anc != -1 && *anc != *pos) { int s1 = (*anc < *pos) ? *anc : *pos, s2 = (*anc > *pos) ? *anc : *pos; wmemmove(buf + s1, buf + s2, (size_t)(len - s2 + 1)); len -= (s2 - s1); *pos = s1; *anc = -1; }
				char raw[256] = {0};
				if (clipboard_paste(raw, sizeof(raw)) > 0) {
					wchar_t wadd[256]; size_t alen = mbstowcs(wadd, raw, 255);
					if (alen != (size_t)-1 && alen > 0) { if (len + (int)alen > max_c) alen = (size_t)(max_c - len); wmemmove(buf + *pos + alen, buf + *pos, (size_t)(len - *pos + 1)); wmemcpy(buf + *pos, wadd, alen); *pos += (int)alen; }
				}
			} else if (ev.action == ACT_CHAR && iswprint(ev.ch)) {
				if (*anc != -1 && *anc != *pos) { int s1 = (*anc < *pos) ? *anc : *pos, s2 = (*anc > *pos) ? *anc : *pos; wmemmove(buf + s1, buf + s2, (size_t)(len - s2 + 1)); len -= (s2 - s1); *pos = s1; *anc = -1; }
				if (len < max_c) { wmemmove(buf + *pos + 1, buf + *pos, (size_t)(len - *pos + 1)); buf[*pos] = ev.ch; (*pos)++; }
			}
		}
	}
}

int ui_find_files(const char *start_dir, char *out_path, size_t out_sz) {
	if (!start_dir || !*start_dir) return 0;
	static char last_mask[128] = "*", last_text[256] = "";
	static int last_rec = 1, last_cs = 0, last_skip = 1;

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

	find_walk_dir(&ctx, ctx.base_dir, "");
	if (ctx.count == 0) { ui_message("Find Files", "No matching files found"); free(ctx.items); return 0; }

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

		const char *blbls[5] = { "[ Go to ]", "[ View (F3) ]", "[ Edit (F4) ]", "[ Insert ]", "[ Close ]" };
		int blens[5] = { 9, 13, 13, 10, 9 };
		int total_bw = 9 + 2 + 13 + 2 + 13 + 2 + 10 + 2 + 9;
		int bx = (bw - total_bw) / 2;
		if (bx < 2) bx = 2;
		int bpos[5], cur_x = bx;
		for (int b = 0; b < 5; b++) {
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
		if (ev.action == ACT_TAB || ev.action == ACT_RIGHT) { btn_focus = (btn_focus + 1) % 5; continue; }
		if (ev.action == ACT_SHIFT_TAB || ev.action == ACT_LEFT) { btn_focus = (btn_focus + 4) % 5; continue; }
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
				for (int b = 0; b < 5; b++) {
					if (wx >= bpos[b] && wx < bpos[b] + blens[b]) { hit = b; break; }
				}
				if (hit == -1) continue;
				btn_focus = hit;
				if (hit == 0) ev.action = ACT_ENTER;
				else if (hit == 1) ev.action = ACT_F3;
				else if (hit == 2) ev.action = ACT_F4;
				else if (hit == 3) ev.action = ACT_INSERT_NAME;
				else break;
			} else continue;
		}
		if (ev.action == ACT_CHAR && ev.ch == ' ') {
			if (btn_focus == 0) ev.action = ACT_ENTER;
			else if (btn_focus == 1) ev.action = ACT_F3;
			else if (btn_focus == 2) ev.action = ACT_F4;
			else if (btn_focus == 3) ev.action = ACT_INSERT_NAME;
			else break;
		}
		if (ev.action == ACT_ENTER) {
			if (btn_focus == 1) ev.action = ACT_F3;
			else if (btn_focus == 2) ev.action = ACT_F4;
			else if (btn_focus == 3) ev.action = ACT_INSERT_NAME;
			else if (btn_focus == 4) break;
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
