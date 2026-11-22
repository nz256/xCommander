#define _GNU_SOURCE
#include <ncurses.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <wchar.h>
#include <wctype.h>
#include "xc.h"
#include "ui.h"
#include "input.h"
#include "viewer.h"

typedef struct {
	unsigned char *data;
	size_t size;
	size_t *line_offsets;
	int line_count;
	int top_line;
	int left_col;
	int wrap_mode;
	int hex_mode;
	char search_query[128];
	size_t match_offset;
	size_t match_len;
} ViewerState;

static void viewer_index_lines(ViewerState *vs) {
	free(vs->line_offsets);
	vs->line_offsets = NULL;
	vs->line_count = 0;
	if (!vs->data || vs->size == 0) return;

	int cap = 1024;
	vs->line_offsets = malloc((size_t)cap * sizeof(size_t));
	if (!vs->line_offsets) return;

	vs->line_offsets[vs->line_count++] = 0;
	for (size_t i = 0; i < vs->size; i++) {
		if (vs->data[i] == '\n' && i + 1 < vs->size) {
			if (vs->line_count >= cap) {
				cap *= 2;
				size_t *tmp = realloc(vs->line_offsets, (size_t)cap * sizeof(size_t));
				if (!tmp) break;
				vs->line_offsets = tmp;
			}
			vs->line_offsets[vs->line_count++] = i + 1;
		}
	}
}

static void viewer_draw_bars(const ViewerState *vs, const char *filepath, int height, int width) {
	attrset(COLOR_PAIR(2));
	mvhline(0, 0, ' ', width);

	const char *base = strrchr(filepath, '/');
	base = base ? (base + 1) : filepath;

	int total_rows = vs->hex_mode ? (int)((vs->size + 15) / 16) : vs->line_count;
	if (total_rows < 1) total_rows = 1;
	int pct = ((vs->top_line + 1) * 100) / total_rows;
	if (pct > 100) pct = 100;
	if (vs->size == 0) pct = 100;

	char right_info[96];
	snprintf(right_info, sizeof(right_info), "%s | %s | %zu B | %d%% ",
		vs->hex_mode ? "HEX" : "TEXT",
		vs->wrap_mode ? "Wrap" : "NoWrap",
		vs->size, pct);

	int rlen = (int)strlen(right_info);
	int name_space = width - rlen - 2;
	if (name_space > 0) {
		mvprintw(0, 1, "%.*s", name_space, base);
	}
	if (width > rlen) {
		mvprintw(0, width - rlen, "%s", right_info);
	}

	const char *labels[10] = {
		"Help", vs->wrap_mode ? "Unwrap" : "Wrap", "Quit",
		vs->hex_mode ? "Text" : "Hex", "", "", "Search", "", "", "Quit"
	};
	if (g_mod_shift && !g_mod_ctrl && !g_mod_alt) {
		labels[6] = "Next";
	}
	attrset(COLOR_PAIR(6));
	mvhline(height - 1, 0, ' ', width);
	int col_w = width / 10;
	if (col_w < 3) col_w = 3;
	for (int i = 0; i < 10; i++) {
		int x = i * col_w;
		if (x >= width) break;
		int item_w = (i == 9) ? (width - x) : col_w;
		attrset(COLOR_PAIR(6));
		mvprintw(height - 1, x, "%d", i + 1);
		int num_len = (i == 9) ? 2 : 1;
		int lspace = item_w - num_len;
		if (lspace > 0) {
			attrset(COLOR_PAIR(2));
			for (int k = 0; k < lspace; k++) mvaddch(height - 1, x + num_len + k, ' ');
			mvprintw(height - 1, x + num_len, "%.*s", lspace - (lspace > 1 ? 1 : 0), labels[i]);
		}
	}
	attrset(A_NORMAL);
}

static void viewer_draw_hex(const ViewerState *vs, int height, int width) {
	int vis_rows = height - 2;
	int total_rows = (int)((vs->size + 15) / 16);
	for (int r = 0; r < vis_rows; r++) {
		int row_idx = vs->top_line + r;
		int y = 1 + r;
		attrset(COLOR_PAIR(1));
		mvhline(y, 0, ' ', width);
		if (row_idx >= total_rows) continue;

		size_t base_off = (size_t)row_idx * 16;
		char addr[16];
		snprintf(addr, sizeof(addr), "%08zX: ", base_off);
		attrset(COLOR_PAIR(7));
		mvprintw(y, 0, "%s", addr);

		for (int b = 0; b < 16; b++) {
			size_t off = base_off + (size_t)b;
			int hx = 10 + b * 3 + (b >= 8 ? 1 : 0);
			int ax = 60 + b;
			if (off < vs->size) {
				unsigned char byte = vs->data[off];
				int is_match = (vs->match_len > 0 && off >= vs->match_offset && off < vs->match_offset + vs->match_len);
				attrset(is_match ? COLOR_PAIR(2) : COLOR_PAIR(1));
				if (hx + 2 < width) mvprintw(y, hx, "%02X", byte);
				if (ax < width) {
					char ch = (byte >= 32 && byte <= 126) ? (char)byte : '.';
					mvaddch(y, ax, ch);
				}
			}
		}
	}
}

static void viewer_draw_text(const ViewerState *vs, int height, int width) {
	int vis_rows = height - 2;
	int screen_y = 1;
	int line_idx = vs->top_line;

	attrset(COLOR_PAIR(1));
	for (int y = 1; y <= vis_rows; y++) mvhline(y, 0, ' ', width);

	while (screen_y <= vis_rows && line_idx < vs->line_count) {
		size_t start = vs->line_offsets[line_idx];
		size_t end = (line_idx + 1 < vs->line_count) ? vs->line_offsets[line_idx + 1] : vs->size;
		while (end > start && (vs->data[end - 1] == '\n' || vs->data[end - 1] == '\r')) end--;

		size_t pos = start;
		int col = 0;
		int skip = vs->wrap_mode ? 0 : vs->left_col;

		while (pos < end && screen_y <= vis_rows) {
			wchar_t wc = L'?';
			int step = mbtowc(&wc, (const char *)(vs->data + pos), end - pos);
			if (step <= 0 || (wc < 32 && wc != L'\t') || (wc >= 0x80 && wc <= 0x9F) || !iswprint(wc)) {
				unsigned char b = vs->data[pos];
				wc = (b >= 32 && b <= 126) ? (wchar_t)b : L'.';
				step = 1;
			}
			int cw = (wc == L'\t') ? (4 - (col % 4)) : wcwidth(wc);
			if (cw < 1) { wc = L'.'; cw = 1; step = 1; }

			int is_match = (vs->match_len > 0 && pos >= vs->match_offset && pos < vs->match_offset + vs->match_len);
			attrset(is_match ? COLOR_PAIR(2) : COLOR_PAIR(1));

			if (vs->wrap_mode) {
				if (col + cw > width) {
					col = 0;
					screen_y++;
					if (screen_y > vis_rows) break;
				}
				if (wc == L'\t') {
					for (int k = 0; k < cw && col < width; k++) mvaddch(screen_y, col++, ' ');
				} else {
					char mb[8] = {0};
					int bl = wctomb(mb, wc);
					if (bl > 0) mvaddstr(screen_y, col, mb);
					col += cw;
				}
			} else {
				if (wc == L'\t') {
					for (int k = 0; k < cw; k++) {
						if (col >= skip && (col - skip) < width) mvaddch(screen_y, col - skip, ' ');
						col++;
					}
				} else {
					if (col >= skip && (col - skip + cw) <= width) {
						char mb[8] = {0};
						int bl = wctomb(mb, wc);
						if (bl > 0) mvaddstr(screen_y, col - skip, mb);
					}
					col += cw;
				}
			}
			pos += (size_t)step;
		}
		screen_y++;
		line_idx++;
	}
}

static int viewer_search(ViewerState *vs, size_t start_off) {
	size_t qlen = strlen(vs->search_query);
	if (qlen == 0 || !vs->data || vs->size < qlen) return 0;

	for (size_t i = start_off; i + qlen <= vs->size; i++) {
		if (strncasecmp((const char *)(vs->data + i), vs->search_query, qlen) == 0) {
			vs->match_offset = i;
			vs->match_len = qlen;
			if (vs->hex_mode) {
				vs->top_line = (int)(i / 16);
			} else {
				for (int l = 0; l < vs->line_count; l++) {
					size_t next_off = (l + 1 < vs->line_count) ? vs->line_offsets[l + 1] : vs->size;
					if (i >= vs->line_offsets[l] && i < next_off) {
						vs->top_line = l;
						break;
					}
				}
			}
			return 1;
		}
	}
	return 0;
}

void ui_view_file(const char *filepath) {
	FILE *f = fopen(filepath, "rb");
	if (!f) {
		ui_message("Error", "Cannot open file for viewing");
		return;
	}
	fseek(f, 0, SEEK_END);
	long fsz = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (fsz < 0) fsz = 0;

	ViewerState vs;
	memset(&vs, 0, sizeof(vs));
	vs.size = (size_t)fsz;
	vs.wrap_mode = 1;

	if (vs.size > 0) {
		vs.data = malloc(vs.size + 1);
		if (!vs.data) {
			fclose(f);
			ui_message("Error", "Out of memory");
			return;
		}
		vs.size = fread(vs.data, 1, vs.size, f);
		vs.data[vs.size] = '\0';
	}
	fclose(f);

	for (size_t i = 0; i < vs.size && i < 512; i++) {
		if (vs.data[i] == 0) { vs.hex_mode = 1; break; }
	}
	viewer_index_lines(&vs);
	curs_set(0);

	while (1) {
		int height, width;
		getmaxyx(stdscr, height, width);
		int vis_rows = height - 2;
		if (vis_rows < 1) vis_rows = 1;

		int total_rows = vs.hex_mode ? (int)((vs.size + 15) / 16) : vs.line_count;
		if (total_rows < 1) total_rows = 1;
		if (vs.top_line >= total_rows) vs.top_line = total_rows - 1;
		if (vs.top_line < 0) vs.top_line = 0;

		bkgd(COLOR_PAIR(1));
		werase(stdscr);
		if (vs.hex_mode) viewer_draw_hex(&vs, height, width);
		else viewer_draw_text(&vs, height, width);
		viewer_draw_bars(&vs, filepath, height, width);
		printf("\033[?2026h"); fflush(stdout);
		wnoutrefresh(stdscr); doupdate();
		printf("\033[?2026l"); fflush(stdout);

		InputEvent ev = input_read();
		if (ev.action == ACT_MOUSE_WHEEL_UP) { vs.top_line -= 3; if (vs.top_line < 0) vs.top_line = 0; continue; }
		if (ev.action == ACT_MOUSE_WHEEL_DOWN) { vs.top_line += 3; if (vs.top_line >= total_rows) vs.top_line = total_rows - 1; continue; }
		if (ev.action == ACT_MOUSE_LEFT && ev.mouse_y == height - 1) {
			int col_w = width / 10; if (col_w < 3) col_w = 3;
			int idx = ev.mouse_x / col_w; if (idx > 9) idx = 9;
			const KeyAction fk[10] = { ACT_F1, ACT_F2, ACT_F3, ACT_F4, ACT_NONE, ACT_NONE, ACT_F7, ACT_NONE, ACT_NONE, ACT_F10 };
			ev.action = fk[idx];
		}
		if (ev.action == ACT_NONE) continue;
		if (ev.action == ACT_RESIZE) { endwin(); refresh(); clearok(stdscr, TRUE); continue; }
		if (ev.action == ACT_ESC || ev.action == ACT_F3 || ev.action == ACT_F10 || ev.action == ACT_CTRL_Q) break;

		if (ev.action == ACT_F1) {
			ui_help();
		} else if (ev.action == ACT_F2) {
			vs.wrap_mode = !vs.wrap_mode;
			vs.left_col = 0;
		} else if (ev.action == ACT_F4) {
			size_t cur_byte = 0;
			if (!vs.hex_mode && vs.line_offsets && vs.top_line < vs.line_count) cur_byte = vs.line_offsets[vs.top_line];
			else if (vs.hex_mode) cur_byte = (size_t)vs.top_line * 16;

			vs.hex_mode = !vs.hex_mode;
			if (vs.hex_mode) {
				vs.top_line = (int)(cur_byte / 16);
			} else {
				vs.top_line = 0;
				for (int l = 0; l < vs.line_count; l++) {
					if (vs.line_offsets[l] <= cur_byte) vs.top_line = l;
					else break;
				}
			}
		} else if (ev.action == ACT_F7) {
			if (ui_input("Search (F7)", "Enter text to find:", vs.search_query, sizeof(vs.search_query))) {
				size_t start_off = (!vs.hex_mode && vs.line_offsets && vs.top_line < vs.line_count)
					? vs.line_offsets[vs.top_line] : (size_t)vs.top_line * 16;
				if (!viewer_search(&vs, start_off) && !viewer_search(&vs, 0)) {
					vs.match_len = 0;
					ui_message("Search", "Text not found");
				}
			}
		} else if ((ev.action == ACT_SHIFT_F7 || ev.action == ACT_ENTER) && vs.match_len > 0) {
			if (!viewer_search(&vs, vs.match_offset + 1)) {
				ui_message("Search", "No more matches found");
			}
		} else if (ev.action == ACT_UP) {
			if (vs.top_line > 0) vs.top_line--;
		} else if (ev.action == ACT_DOWN) {
			if (vs.top_line + 1 < total_rows) vs.top_line++;
		} else if (ev.action == ACT_PGUP) {
			vs.top_line -= vis_rows;
			if (vs.top_line < 0) vs.top_line = 0;
		} else if (ev.action == ACT_PGDN) {
			vs.top_line += vis_rows;
			if (vs.top_line >= total_rows) vs.top_line = total_rows - 1;
		} else if (ev.action == ACT_HOME) {
			vs.top_line = 0;
			vs.left_col = 0;
		} else if (ev.action == ACT_END) {
			vs.top_line = (total_rows > vis_rows) ? (total_rows - vis_rows) : 0;
		} else if (ev.action == ACT_LEFT && !vs.hex_mode && !vs.wrap_mode) {
			if (vs.left_col > 0) vs.left_col--;
		} else if (ev.action == ACT_RIGHT && !vs.hex_mode && !vs.wrap_mode) {
			vs.left_col++;
		}
	}

	free(vs.line_offsets);
	free(vs.data);
	clearok(stdscr, TRUE);
	touchwin(stdscr);
}
