#define _GNU_SOURCE
#include <ctype.h>
#include <ncurses.h>
#include <string.h>
#include <wctype.h>
#include "input.h"
#include "menu_bar.h"
#include "panel.h"
#include "ui.h"

typedef struct { const char *lbl; char hk; wchar_t hk_ru; const char *key; MenuBarAction act; } MItem;

static int match_hk(wint_t ch, char hk, wchar_t hk_ru) {
	if (!hk) return 0;
	wint_t lc = towlower(ch);
	if (lc == (wint_t)tolower((unsigned char)hk)) return 1;
	if (hk_ru && lc == (wint_t)hk_ru) return 1;
	return 0;
}

static void draw_hk_label(WINDOW *w, int y, int x, const char *s, char hk, int base_attr, int hk_attr, int pad_w) {
	wmove(w, y, x);
	wattrset(w, base_attr);
	int done = 0, col = 0;
	for (const char *p = s; *p && col < pad_w; p++, col++) {
		if (!done && hk && tolower((unsigned char)*p) == tolower((unsigned char)hk)) {
			wattrset(w, hk_attr);
			waddch(w, (unsigned char)*p);
			wattrset(w, base_attr);
			done = 1;
		} else {
			waddch(w, (unsigned char)*p);
		}
	}
	while (col++ < pad_w) waddch(w, ' ');
}

MenuBarAction ui_top_menu(void) {
	const char *cats[5]    = { "Left", "Files", "Commands", "Options", "Right" };
	const char cat_hk[5]   = { 'L', 'F', 'C', 'O', 'R' };
	const wchar_t cat_ru[5]= { L'д', L'а', L'с', L'щ', L'к' };
	const int cat_x[5]     = { 2, 10, 19, 31, 42 };
	const char *col_lbl = g_auto_colorize ? "[x] Auto colorization" : "[ ] Auto colorization";
	const char *hid_lbl = g_show_hidden   ? "[x] Show hidden files" : "[ ] Show hidden files";

	MItem items[5][6] = {
		{ {"Sort by Name", 'N', L'т', "Ctrl+F3", MB_L_NAME}, {"Sort by Ext", 'E', L'у', "Ctrl+F4", MB_L_EXT}, {"Sort by Time", 'T', L'е', "Ctrl+F5", MB_L_TIME}, {"Sort by Size", 'S', L'ы', "Ctrl+F6", MB_L_SIZE}, {"Drive / Bookmarks", 'D', L'в', "Alt+F1", MB_L_DRIVE}, {NULL,0,0,NULL,0} },
		{ {"Attributes / Time", 'A', L'ф', "Ctrl+A", MB_F_ATTRS}, {"Folder Size", 'F', L'а', "F3", MB_F_DIRSIZE}, {"Find Files...", 'I', L'ш', "Alt+F7", MB_F_FIND}, {"Pack Archive...", 'P', L'з', "Shift+F1", MB_F_PACK}, {NULL,0,0,NULL,0}, {NULL,0,0,NULL,0} },
		{ {"Compare Dirs", 'C', L'с', "", MB_C_COMPARE}, {"Temporary Panel", 'T', L'е', "Alt+F1/0", MB_C_TMPPANEL}, {"Swap Panels", 'S', L'ы', "Ctrl+U", MB_C_SWAP}, {"User Menu", 'U', L'г', "F2", MB_C_USERMENU}, {"File associations", 'A', L'ф', "", MB_C_ASSOC}, {NULL,0,0,NULL,0} },
		{ {col_lbl, 'C', L'с', "Alt+F12", MB_O_COLORIZE}, {hid_lbl, 'H', L'р', "", MB_O_HIDDEN}, {NULL,0,0,NULL,0}, {NULL,0,0,NULL,0}, {NULL,0,0,NULL,0}, {NULL,0,0,NULL,0} },
		{ {"Sort by Name", 'N', L'т', "Ctrl+F3", MB_R_NAME}, {"Sort by Ext", 'E', L'у', "Ctrl+F4", MB_R_EXT}, {"Sort by Time", 'T', L'е', "Ctrl+F5", MB_R_TIME}, {"Sort by Size", 'S', L'ы', "Ctrl+F6", MB_R_SIZE}, {"Drive / Bookmarks", 'D', L'в', "Alt+F2", MB_R_DRIVE}, {NULL,0,0,NULL,0} }
	};

	int h, w; getmaxyx(stdscr, h, w); (void)h;
	int cat = 0, sel = 0;
	curs_set(0);

	while (1) {
		touchwin(stdscr);
		attrset(COLOR_PAIR(2));
		mvhline(0, 0, ' ', w);
		for (int c = 0; c < 5; c++) {
			int b_at = (c == cat) ? COLOR_PAIR(6) : COLOR_PAIR(2);
			int h_at = (c == cat) ? COLOR_PAIR(15) : COLOR_PAIR(4);
			attrset(b_at); mvaddch(0, cat_x[c], ' ');
			draw_hk_label(stdscr, 0, cat_x[c] + 1, cats[c], cat_hk[c], b_at, h_at, (int)strlen(cats[c]) + 1);
		}

		int cnt = 0; while (cnt < 6 && items[cat][cnt].lbl) cnt++;
		int dw = 34, dh = cnt + 2, dx = cat_x[cat];
		if (dx + dw > w) dx = (w > dw) ? (w - dw) : 0;

		WINDOW *drop = newwin(dh, dw, 1, dx);
		wbkgd(drop, COLOR_PAIR(2));
		wattrset(drop, COLOR_PAIR(9));
		box(drop, 0, 0);
		for (int i = 0; i < cnt; i++) {
			int b_at = (i == sel) ? COLOR_PAIR(6) : COLOR_PAIR(9);
			int h_at = (i == sel) ? COLOR_PAIR(15) : COLOR_PAIR(4);
			wattrset(drop, b_at);
			mvwaddch(drop, 1 + i, 1, ' ');
			draw_hk_label(drop, 1 + i, 2, items[cat][i].lbl, items[cat][i].hk, b_at, h_at, 21);
			wattrset(drop, b_at);
			mvwprintw(drop, 1 + i, 23, "%9s ", items[cat][i].key);
		}
		wnoutrefresh(stdscr); wnoutrefresh(drop); doupdate();

		InputEvent ev = input_read();
		delwin(drop);
		if (ev.action == ACT_ESC || ev.action == ACT_F9 || ev.action == ACT_F10) { curs_set(1); return MB_NONE; }
		if (ev.action == ACT_LEFT)  { cat = (cat + 4) % 5; sel = 0; }
		else if (ev.action == ACT_RIGHT) { cat = (cat + 1) % 5; sel = 0; }
		else if (ev.action == ACT_UP)    { sel = (sel + cnt - 1) % cnt; }
		else if (ev.action == ACT_DOWN)  { sel = (sel + 1) % cnt; }
		else if (ev.action == ACT_ENTER) { curs_set(1); return items[cat][sel].act; }
		else if (ev.action == ACT_CHAR && ev.ch > 0) {
			for (int i = 0; i < cnt; i++) {
				if (match_hk(ev.ch, items[cat][i].hk, items[cat][i].hk_ru)) { curs_set(1); return items[cat][i].act; }
			}
			for (int c = 0; c < 5; c++) {
				if (match_hk(ev.ch, cat_hk[c], cat_ru[c])) { cat = c; sel = 0; break; }
			}
		} else if (ev.action == ACT_MOUSE_LEFT) {
			if (ev.mouse_y == 0) {
				for (int c = 0; c < 5; c++) {
					if (ev.mouse_x >= cat_x[c] && ev.mouse_x < cat_x[c] + (int)strlen(cats[c]) + 2) { cat = c; sel = 0; break; }
				}
			} else if (ev.mouse_y >= 2 && ev.mouse_y < 1 + dh - 1 && ev.mouse_x > dx && ev.mouse_x < dx + dw - 1) {
				{ curs_set(1); return items[cat][ev.mouse_y - 2].act; }
			} else { curs_set(1); return MB_NONE; }
		}
	}
}
