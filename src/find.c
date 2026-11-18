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

static int wline_cols(const wchar_t *ws, int count) {
	int cols = 0;
	for (int i = 0; i < count && ws[i]; i++) {
		int cw = wcwidth(ws[i]);
		cols += (cw > 0) ? cw : 1;
	}
	return cols;
}

static int ui_find_prompt(FindCtx *ctx) {
	int h, w; getmaxyx(stdscr, h, w);
	int bw = (w - 10 > 68) ? 68 : (w - 4); if (bw < 44) bw = w - 2;
	int bh = 13, sy = (h - bh) / 2, sx = (w - bw) / 2;
	WINDOW *win = newwin(bh, bw, sy, sx); wbkgd(win, COLOR_PAIR(5));

	wchar_t wmask[128] = {0}, wtext[256] = {0};
	mbstowcs(wmask, ctx->mask, 127);
	mbstowcs(wtext, ctx->text, 255);
	int field = 0;
	int mpos = (int)wcslen(wmask);
	int tpos = (int)wcslen(wtext);

	while (1) {
		werase(win); box(win, 0, 0);
		mvwprintw(win, 0, 2, " Find Files (Alt+F7) ");

		mvwprintw(win, 2, 3, "File mask(s) (e.g. *.c;*.h):");
		wattron(win, (field == 0) ? COLOR_PAIR(3) : COLOR_PAIR(2));
		mvwhline(win, 3, 3, ' ', bw - 6);
		mvwaddnwstr(win, 3, 4, wmask, bw - 8);
		wattroff(win, (field == 0) ? COLOR_PAIR(3) : COLOR_PAIR(2));

		mvwprintw(win, 5, 3, "Containing text (optional):");
		wattron(win, (field == 1) ? COLOR_PAIR(3) : COLOR_PAIR(2));
		mvwhline(win, 6, 3, ' ', bw - 6);
		mvwaddnwstr(win, 6, 4, wtext, bw - 8);
		wattroff(win, (field == 1) ? COLOR_PAIR(3) : COLOR_PAIR(2));

		if (field == 2) wattron(win, COLOR_PAIR(3));
		mvwprintw(win, 8, 3, "[%c] Recursive subdirectories", ctx->recursive ? 'x' : ' ');
		if (field == 2) wattroff(win, COLOR_PAIR(3));
		if (field == 3) wattron(win, COLOR_PAIR(3));
		mvwprintw(win, 9, 3, "[%c] Case sensitive text", ctx->case_sens ? 'x' : ' ');
		if (field == 3) wattroff(win, COLOR_PAIR(3));
		if (field == 4) wattron(win, COLOR_PAIR(3));
		mvwprintw(win, 10, 3, "[%c] Skip hidden files/dirs", ctx->skip_hidden ? 'x' : ' ');
		if (field == 4) wattroff(win, COLOR_PAIR(3));

		const char *hint = " Enter: Search | Tab/Arrows: Field | Space: Toggle | Esc: Cancel ";
		if (bw > (int)strlen(hint) + 4) mvwprintw(win, bh - 1, (bw - (int)strlen(hint)) / 2, "%s", hint);

		if (field == 0 || field == 1) {
			curs_set(1);
			int cx = (field == 0) ? wline_cols(wmask, mpos) : wline_cols(wtext, tpos);
			if (cx > bw - 8) cx = bw - 8;
			wmove(win, (field == 0) ? 3 : 6, 4 + cx);
		} else {
			curs_set(0);
		}

		printf("\033[?2026h"); fflush(stdout); wnoutrefresh(win); doupdate(); printf("\033[?2026l"); fflush(stdout);
		InputEvent ev = input_read();
		if (ev.action == ACT_ESC) { curs_set(0); delwin(win); touchwin(stdscr); return 0; }
		if (ev.action == ACT_ENTER) {
			wcstombs(ctx->mask, wmask, sizeof(ctx->mask) - 1);
			ctx->mask[sizeof(ctx->mask) - 1] = '\0';
			wcstombs(ctx->text, wtext, sizeof(ctx->text) - 1);
			ctx->text[sizeof(ctx->text) - 1] = '\0';
			curs_set(0); delwin(win); touchwin(stdscr); return 1;
		}
		if (ev.action == ACT_TAB || ev.action == ACT_DOWN) { field = (field + 1) % 5; continue; }
		if (ev.action == ACT_UP) { field = (field + 4) % 5; continue; }
		if (field >= 2 && ev.action == ACT_CHAR && ev.ch == ' ') {
			if (field == 2) ctx->recursive = !ctx->recursive;
			else if (field == 3) ctx->case_sens = !ctx->case_sens;
			else if (field == 4) ctx->skip_hidden = !ctx->skip_hidden;
			continue;
		}
		if (field == 0 || field == 1) {
			wchar_t *buf = (field == 0) ? wmask : wtext;
			int max_c = (field == 0) ? 127 : 255;
			int *pos = (field == 0) ? &mpos : &tpos;
			int len = (int)wcslen(buf);
			if (ev.action == ACT_LEFT && *pos > 0) (*pos)--;
			else if (ev.action == ACT_RIGHT && *pos < len) (*pos)++;
			else if (ev.action == ACT_HOME) *pos = 0;
			else if (ev.action == ACT_END) *pos = len;
			else if (ev.action == ACT_CTRL_U) { buf[0] = L'\0'; *pos = 0; }
			else if (ev.action == ACT_BACKSPACE && *pos > 0) {
				wmemmove(buf + *pos - 1, buf + *pos, (size_t)(len - *pos + 1));
				(*pos)--;
			} else if (ev.action == ACT_DELETE && *pos < len) {
				wmemmove(buf + *pos, buf + *pos + 1, (size_t)(len - *pos));
			} else if (ev.action == ACT_CHAR && ev.ch >= 32 && len < max_c) {
				wmemmove(buf + *pos + 1, buf + *pos, (size_t)(len - *pos + 1));
				buf[*pos] = ev.ch;
				(*pos)++;
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

	int sel = 0, top = 0, ret_code = 0;
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
		const char *btns = "[ Enter: Go to ]  [ F3: View ]  [ F4: Edit ]  [ Ctrl+J: Insert ]  [ Esc: Close ]";
		wattron(win, COLOR_PAIR(2)); mvwaddnstr(win, bh - 2, 2, btns, bw - 4); wattroff(win, COLOR_PAIR(2));

		printf("\033[?2026h"); fflush(stdout); wnoutrefresh(win); doupdate(); printf("\033[?2026l"); fflush(stdout);
		InputEvent ev = input_read();
		if (ev.action == ACT_ESC || ev.action == ACT_F10) break;
		if (ev.action == ACT_UP) { sel--; continue; }
		if (ev.action == ACT_DOWN) { sel++; continue; }
		if (ev.action == ACT_PGUP) { sel -= vis; continue; }
		if (ev.action == ACT_PGDN) { sel += vis; continue; }
		if (ev.action == ACT_HOME) { sel = 0; continue; }
		if (ev.action == ACT_END) { sel = ctx.count - 1; continue; }
		if (ev.action == ACT_MOUSE_WHEEL_UP) { sel -= 3; continue; }
		if (ev.action == ACT_MOUSE_WHEEL_DOWN) { sel += 3; continue; }

		const FindResult *cur = &ctx.items[sel];
		if (ev.action == ACT_ENTER) { if (out_path && out_sz > 0) snprintf(out_path, out_sz, "%s", cur->full_path); ret_code = 1; break; }
		if (ev.action == ACT_INSERT_NAME) { if (out_path && out_sz > 0) snprintf(out_path, out_sz, "%s", cur->full_path); ret_code = 2; break; }
		if (ev.action == ACT_F3 && !cur->is_dir) { ui_view_file(cur->full_path); touchwin(win); }
		if (ev.action == ACT_F4 && !cur->is_dir) { ui_edit_file_at(cur->full_path, cur->match_line, 1); touchwin(win); }
	}
	delwin(win); touchwin(stdscr); free(ctx.items);
	return ret_code;
}
