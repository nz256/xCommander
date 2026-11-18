#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <wchar.h>
#include <wctype.h>
#include "cmdline.h"
#include "exec.h"

wchar_t cmd_wbuf[CMD_MAX_LEN] = L"";
int cmd_len = 0;
int cmd_pos = 0;
int cmd_scroll = 0;
int cmd_sel_start = -1;
int cmd_sel_end = -1;
static int cmd_sel_anchor = -1;

static void sync_legacy_buf(void) {
	cmd_wbuf[cmd_len] = L'\0';
	wcstombs(cmd_buffer, cmd_wbuf, sizeof(cmd_buffer));
	cmd_buffer[sizeof(cmd_buffer) - 1] = '\0';
}

void cmdline_init(void) {
	cmdline_clear();
}

void cmdline_clear(void) {
	cmd_wbuf[0] = L'\0';
	cmd_len = 0;
	cmd_pos = 0;
	cmd_scroll = 0;
	cmd_sel_anchor = -1;
	cmd_sel_start = -1;
	cmd_sel_end = -1;
	cmd_buffer[0] = '\0';
}

void cmdline_set_text(const char *utf8) {
	cmdline_clear();
	if (!utf8 || !*utf8) return;
	size_t wlen = mbstowcs(cmd_wbuf, utf8, CMD_MAX_LEN - 1);
	if (wlen == (size_t)-1) {
		cmdline_clear();
		return;
	}
	cmd_len = (int)wlen;
	cmd_pos = cmd_len;
	sync_legacy_buf();
}

void cmdline_get_utf8(char *out, size_t out_size) {
	if (!out || out_size == 0) return;
	cmd_wbuf[cmd_len] = L'\0';
	size_t n = wcstombs(out, cmd_wbuf, out_size - 1);
	if (n == (size_t)-1) out[0] = '\0';
	else out[n] = '\0';
}

static void update_sel(void) {
	if (cmd_sel_anchor >= 0 && cmd_sel_anchor != cmd_pos) {
		cmd_sel_start = (cmd_sel_anchor < cmd_pos) ? cmd_sel_anchor : cmd_pos;
		cmd_sel_end = (cmd_sel_anchor > cmd_pos) ? cmd_sel_anchor : cmd_pos;
	} else {
		cmd_sel_start = -1;
		cmd_sel_end = -1;
	}
}

static void delete_sel(void) {
	if (cmd_sel_start >= 0 && cmd_sel_end > cmd_sel_start) {
		wmemmove(cmd_wbuf + cmd_sel_start, cmd_wbuf + cmd_sel_end, (size_t)(cmd_len - cmd_sel_end + 1));
		cmd_len -= (cmd_sel_end - cmd_sel_start);
		cmd_pos = cmd_sel_start;
	}
	cmd_sel_anchor = -1;
	cmd_sel_start = -1;
	cmd_sel_end = -1;
}

void cmdline_insert_wstr(const wchar_t *wstr) {
	if (!wstr || !*wstr) return;
	if (cmd_sel_start >= 0) delete_sel();
	int add = (int)wcslen(wstr);
	if (cmd_len + add >= CMD_MAX_LEN - 1) add = CMD_MAX_LEN - 1 - cmd_len;
	if (add <= 0) return;
	wmemmove(cmd_wbuf + cmd_pos + add, cmd_wbuf + cmd_pos, (size_t)(cmd_len - cmd_pos + 1));
	wmemcpy(cmd_wbuf + cmd_pos, wstr, (size_t)add);
	cmd_pos += add;
	cmd_len += add;
	sync_legacy_buf();
}

void cmdline_insert_path(const Panel *p) {
	if (!p || !*p->current_path) return;
	char fmt[MAX_PATH + 8];
	if (strchr(p->current_path, ' ') != NULL) snprintf(fmt, sizeof(fmt), "\"%s\" ", p->current_path);
	else if (strcmp(p->current_path, "/") == 0) snprintf(fmt, sizeof(fmt), "/");
	else snprintf(fmt, sizeof(fmt), "%s/", p->current_path);
	wchar_t wadd[MAX_PATH + 8];
	size_t n = mbstowcs(wadd, fmt, MAX_PATH + 7);
	if (n == (size_t)-1 || n == 0) return;
	wadd[n] = L'\0';
	if (cmd_sel_start >= 0) delete_sel();
	if (cmd_pos > 0 && cmd_wbuf[cmd_pos - 1] != L' ' && cmd_len < CMD_MAX_LEN - 2) {
		cmdline_insert_wstr(L" ");
	}
	cmdline_insert_wstr(wadd);
}

void cmdline_insert_filename(const Panel *p) {
	if (!p || p->count == 0) return;
	const char *name = p->files[p->selected].name;
	if (!name || strcmp(name, "..") == 0) return;
	char fmt[MAX_FILENAME + 8];
	if (strchr(name, ' ') != NULL) snprintf(fmt, sizeof(fmt), "\"%s\" ", name);
	else snprintf(fmt, sizeof(fmt), "%s ", name);
	wchar_t wadd[MAX_FILENAME + 8];
	size_t n = mbstowcs(wadd, fmt, MAX_FILENAME + 7);
	if (n == (size_t)-1 || n == 0) return;
	wadd[n] = L'\0';
	if (cmd_sel_start >= 0) delete_sel();
	if (cmd_pos > 0 && cmd_wbuf[cmd_pos - 1] != L' ' && cmd_len < CMD_MAX_LEN - 2) {
		cmdline_insert_wstr(L" ");
	}
	cmdline_insert_wstr(wadd);
}

void cmdline_copy(const Panel *p, int terminal_mode) {
	if (cmd_sel_start >= 0 && cmd_sel_end > cmd_sel_start) {
		int slen = cmd_sel_end - cmd_sel_start;
		wchar_t *wsub = malloc((size_t)(slen + 1) * sizeof(wchar_t));
		char *utf = malloc((size_t)slen * 4 + 4);
		if (wsub && utf) {
			wmemcpy(wsub, cmd_wbuf + cmd_sel_start, (size_t)slen);
			wsub[slen] = L'\0';
			if (wcstombs(utf, wsub, (size_t)slen * 4 + 3) != (size_t)-1) clipboard_copy(utf);
		}
		free(wsub);
		free(utf);
	} else if (cmd_len > 0) {
		char *utf = malloc((size_t)cmd_len * 4 + 4);
		if (utf) {
			cmdline_get_utf8(utf, (size_t)cmd_len * 4 + 4);
			clipboard_copy(utf);
			free(utf);
		}
	} else if (!terminal_mode && p && p->count > 0) {
		const char *name = p->files[p->selected].name;
		char full[MAX_PATH * 2];
		if (strcmp(name, "..") == 0) snprintf(full, sizeof(full), "%s", p->current_path);
		else if (strcmp(p->current_path, "/") == 0) snprintf(full, sizeof(full), "/%s", name);
		else snprintf(full, sizeof(full), "%s/%s", p->current_path, name);
		clipboard_copy(full);
	}
}

void cmdline_cut(void) {
	if (cmd_sel_start < 0 || cmd_sel_end <= cmd_sel_start) return;
	cmdline_copy(NULL, 1);
	delete_sel();
	sync_legacy_buf();
}

void cmdline_paste(void) {
	char raw[CMD_MAX_LEN] = {0};
	if (clipboard_paste(raw, sizeof(raw)) == 0) return;
	wchar_t *wadd = malloc(CMD_MAX_LEN * sizeof(wchar_t));
	if (!wadd) return;
	size_t n = mbstowcs(wadd, raw, CMD_MAX_LEN - 1);
	if (n != (size_t)-1 && n > 0) {
		wadd[n] = L'\0';
		cmdline_insert_wstr(wadd);
	}
	free(wadd);
}

static int is_sep(wchar_t ch) {
	return iswspace(ch) || ch == L'/' || ch == L'-' || ch == L'_' || ch == L'.';
}

static int jump_left(int pos) {
	while (pos > 0 && is_sep(cmd_wbuf[pos - 1])) pos--;
	while (pos > 0 && !is_sep(cmd_wbuf[pos - 1])) pos--;
	return pos;
}

static int jump_right(int pos) {
	while (pos < cmd_len && !is_sep(cmd_wbuf[pos])) pos++;
	while (pos < cmd_len && is_sep(cmd_wbuf[pos])) pos++;
	return pos;
}

int cmdline_handle_edit_key(InputEvent ev, int terminal_mode) {
	int is_print = (ev.action == ACT_CHAR && iswprint(ev.ch) && !(cmd_len == 0 && ev.ch == L' '));
	if (!terminal_mode && cmd_len == 0 && (ev.action == ACT_SHIFT_UP || ev.action == ACT_SHIFT_DOWN || ev.action == ACT_SHIFT_LEFT || ev.action == ACT_SHIFT_RIGHT)) return 0;
	int is_move = (cmd_len > 0 && (ev.action == ACT_HOME || ev.action == ACT_END || ev.action == ACT_CTRL_A ||
		ev.action == ACT_SHIFT_LEFT || ev.action == ACT_SHIFT_RIGHT || ev.action == ACT_CTRL_LEFT ||
		ev.action == ACT_CTRL_RIGHT || ev.action == ACT_WORD_SEL_LEFT || ev.action == ACT_WORD_SEL_RIGHT ||
		ev.action == ACT_CTRL_B || ev.action == ACT_CTRL_F ||
		(terminal_mode && (ev.action == ACT_LEFT || ev.action == ACT_RIGHT))));
	int is_edit = (cmd_len > 0 && (ev.action == ACT_BACKSPACE || ev.action == ACT_DELETE || ev.action == ACT_CTRL_U || ev.action == ACT_ESC));

	if (!is_print && !is_move && !is_edit) return 0;

	if (is_move) {
		if (ev.action == ACT_HOME || ev.action == ACT_CTRL_A) { cmd_sel_anchor = -1; update_sel(); cmd_pos = 0; }
		else if (ev.action == ACT_END) { cmd_sel_anchor = -1; update_sel(); cmd_pos = cmd_len; }
		else if (ev.action == ACT_SHIFT_LEFT) { if (cmd_sel_anchor == -1) cmd_sel_anchor = cmd_pos; if (cmd_pos > 0) cmd_pos--; update_sel(); }
		else if (ev.action == ACT_SHIFT_RIGHT) { if (cmd_sel_anchor == -1) cmd_sel_anchor = cmd_pos; if (cmd_pos < cmd_len) cmd_pos++; update_sel(); }
		else if (ev.action == ACT_WORD_SEL_LEFT) { if (cmd_sel_anchor == -1) cmd_sel_anchor = cmd_pos; cmd_pos = jump_left(cmd_pos); update_sel(); }
		else if (ev.action == ACT_WORD_SEL_RIGHT) { if (cmd_sel_anchor == -1) cmd_sel_anchor = cmd_pos; cmd_pos = jump_right(cmd_pos); update_sel(); }
		else if (ev.action == ACT_CTRL_LEFT) { cmd_sel_anchor = -1; update_sel(); cmd_pos = jump_left(cmd_pos); }
		else if (ev.action == ACT_CTRL_RIGHT) { cmd_sel_anchor = -1; update_sel(); cmd_pos = jump_right(cmd_pos); }
		else if (ev.action == ACT_LEFT || ev.action == ACT_CTRL_B) { cmd_sel_anchor = -1; update_sel(); if (cmd_pos > 0) cmd_pos--; }
		else if (ev.action == ACT_RIGHT || ev.action == ACT_CTRL_F) { cmd_sel_anchor = -1; update_sel(); if (cmd_pos < cmd_len) cmd_pos++; }
	} else if (ev.action == ACT_BACKSPACE) {
		if (cmd_sel_start >= 0) delete_sel();
		else if (cmd_pos > 0) { wmemmove(cmd_wbuf + cmd_pos - 1, cmd_wbuf + cmd_pos, (size_t)(cmd_len - cmd_pos + 1)); cmd_pos--; cmd_len--; }
	} else if (ev.action == ACT_DELETE) {
		if (cmd_sel_start >= 0) delete_sel();
		else if (cmd_pos < cmd_len) { wmemmove(cmd_wbuf + cmd_pos, cmd_wbuf + cmd_pos + 1, (size_t)(cmd_len - cmd_pos)); cmd_len--; }
	} else if (ev.action == ACT_CTRL_U || ev.action == ACT_ESC) {
		cmdline_clear();
		return 2;
	} else if (is_print && cmd_len < CMD_MAX_LEN - 1) {
		if (cmd_sel_start >= 0) delete_sel();
		wmemmove(cmd_wbuf + cmd_pos + 1, cmd_wbuf + cmd_pos, (size_t)(cmd_len - cmd_pos + 1));
		cmd_wbuf[cmd_pos++] = ev.ch;
		cmd_len++;
	}
	sync_legacy_buf();
	return 1;
}
