#define _GNU_SOURCE
#include <errno.h>
#include <unistd.h>
#define _GNU_SOURCE
#include <ncurses.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <string.h>
#include <stdio.h>
#include <wchar.h>
#include <wctype.h>
#include "xc.h"
#include "ui.h"
#include "sudo_helper.h"
#include "widgets.h"
#include "exec.h"
#include "input.h"
#include "editor.h"

typedef struct {
	wchar_t *text;
	int len;
	int cap;
} EditorLine;

typedef struct {
	EditorLine *lines;
	int line_count;
	int cur_y;
	int cur_x;
	int modified;
} EditorSnap;

#define MAX_UNDO 64

typedef enum {
	TOK_NORMAL = 0,
	TOK_KEYWORD,
	TOK_STRING,
	TOK_NUMBER,
	TOK_COMMENT,
	TOK_PREPROC
} SyntaxToken;

typedef struct {
	const char *name;
	const char *exts[8];
	const char *line_comment;
	int c_block_comments;
	int hash_preproc;
	const char * const *keywords;
} SyntaxLang;

static const char * const KW_C[] = {
	"auto","break","case","char","const","continue","default","do","double","else",
	"enum","extern","float","for","goto","if","inline","int","long","register",
	"restrict","return","short","signed","sizeof","static","struct","switch",
	"typedef","union","unsigned","void","volatile","while","bool","true","false",
	"NULL","size_t","ssize_t","off_t","mode_t","wchar_t","FILE", NULL
};

static const char * const KW_PY[] = {
	"and","as","assert","async","await","break","class","continue","def","del",
	"elif","else","except","False","finally","for","from","global","if","import",
	"in","is","lambda","None","nonlocal","not","or","pass","raise","return",
	"True","try","while","with","yield","self","print","len","range", NULL
};

static const char * const KW_SH[] = {
	"if","then","else","elif","fi","case","esac","for","select","while","until",
	"do","done","in","function","time","export","local","readonly","unset","return",
	"exit","echo","printf","read","cd","pwd","source","set","shift","trap", NULL
};

static const char * const KW_GEN[] = {
	"TODO","FIXME","NOTE","ERROR","WARN","WARNING","INFO","DEBUG","OK","FAIL","FAILED",
	"TRUE","FALSE","YES","NO","ON","OFF","NULL","NONE", NULL
};

static const SyntaxLang SYNTAX_LANGS[] = {
	{ "C/C++",   { ".c", ".h", ".cpp", ".hpp", ".cc", NULL }, "//", 1, 1, KW_C },
	{ "Python",  { ".py", ".pyw", NULL }, "#", 0, 0, KW_PY },
	{ "Shell",   { ".sh", ".bash", ".zsh", NULL }, "#", 0, 0, KW_SH },
	{ "Config",  { ".conf", ".ini", ".cfg", ".toml", ".yaml", ".yml", ".env", NULL }, "#", 0, 0, KW_GEN },
	{ "Generic", { NULL }, "#", 0, 0, KW_GEN }
};

static const SyntaxLang *ed_detect_syntax(const char *filepath) {
	const char *dot = strrchr(filepath, '.');
	if (!dot) return NULL;
	int lang_cnt = (int)(sizeof(SYNTAX_LANGS) / sizeof(SYNTAX_LANGS[0]));
	for (int i = 0; i < lang_cnt; i++) {
		for (int j = 0; SYNTAX_LANGS[i].exts[j] != NULL; j++) {
			if (strcasecmp(dot, SYNTAX_LANGS[i].exts[j]) == 0) return &SYNTAX_LANGS[i];
		}
	}
	return &SYNTAX_LANGS[4];
}

static int ed_is_kw(const SyntaxLang *lang, const wchar_t *w, int len) {
	if (!lang || !lang->keywords || len <= 0 || len > 31) return 0;
	char buf[32];
	for (int i = 0; i < len; i++) {
		if (w[i] >= 128) return 0;
		buf[i] = (char)w[i];
	}
	buf[len] = '\0';
	for (int i = 0; lang->keywords[i] != NULL; i++) {
		if (strcmp(buf, lang->keywords[i]) == 0) return 1;
	}
	return 0;
}

static void ed_tokenize_line(const SyntaxLang *lang, const EditorLine *ln, unsigned char *toks, int *in_block_cmt) {
	if (!ln || ln->len <= 0) return;
	memset(toks, TOK_NORMAL, (size_t)ln->len);
	if (!lang) return;

	int i = 0;
	while (i < ln->len && (ln->text[i] == L' ' || ln->text[i] == L'\t')) i++;
	if (!*in_block_cmt && lang->hash_preproc && i < ln->len && ln->text[i] == L'#') {
		memset(toks + i, TOK_PREPROC, (size_t)(ln->len - i));
		return;
	}

	while (i < ln->len) {
		if (*in_block_cmt) {
			toks[i] = TOK_COMMENT;
			if (i + 1 < ln->len && ln->text[i] == L'*' && ln->text[i + 1] == L'/') {
				toks[i + 1] = TOK_COMMENT;
				*in_block_cmt = 0;
				i += 2;
				continue;
			}
			i++;
			continue;
		}
		if (lang->c_block_comments && i + 1 < ln->len && ln->text[i] == L'/' && ln->text[i + 1] == L'*') {
			toks[i] = TOK_COMMENT;
			toks[i + 1] = TOK_COMMENT;
			*in_block_cmt = 1;
			i += 2;
			continue;
		}
		if (lang->line_comment) {
			int clen = (int)strlen(lang->line_comment);
			int match = (i + clen <= ln->len);
			for (int k = 0; match && k < clen; k++) {
				if (ln->text[i + k] != (wchar_t)lang->line_comment[k]) match = 0;
			}
			if (match) {
				memset(toks + i, TOK_COMMENT, (size_t)(ln->len - i));
				return;
			}
		}
		wchar_t wc = ln->text[i];
		if (wc == L'"' || wc == L'\'') {
			wchar_t q = wc;
			toks[i++] = TOK_STRING;
			while (i < ln->len) {
				toks[i] = TOK_STRING;
				if (ln->text[i] == L'\\' && i + 1 < ln->len) {
					toks[i + 1] = TOK_STRING;
					i += 2;
					continue;
				}
				if (ln->text[i] == q) { i++; break; }
				i++;
			}
			continue;
		}
		if (iswdigit(wc) && (i == 0 || (!iswalnum(ln->text[i - 1]) && ln->text[i - 1] != L'_'))) {
			while (i < ln->len && (iswalnum(ln->text[i]) || ln->text[i] == L'.')) {
				toks[i++] = TOK_NUMBER;
			}
			continue;
		}
		if (iswalpha(wc) || wc == L'_') {
			int start = i;
			while (i < ln->len && (iswalnum(ln->text[i]) || ln->text[i] == L'_')) i++;
			if (ed_is_kw(lang, ln->text + start, i - start)) {
				memset(toks + start, TOK_KEYWORD, (size_t)(i - start));
			}
			continue;
		}
		i++;
	}
}

static int ed_token_attr(unsigned char tok) {
	if (tok == TOK_KEYWORD) return COLOR_PAIR(13) | A_BOLD;
	if (tok == TOK_STRING)  return COLOR_PAIR(14);
	if (tok == TOK_NUMBER)  return COLOR_PAIR(15);
	if (tok == TOK_PREPROC) return COLOR_PAIR(15);
	if (tok == TOK_COMMENT) return COLOR_PAIR(16);
	return COLOR_PAIR(1);
}

typedef struct {
	char filepath[MAX_PATH * 2];
	EditorLine *lines;
	int line_count;
	int line_cap;
	int cur_y;
	int cur_x;
	int top_line;
	int left_col;
	int modified;
	int overwrite_mode;
	int auto_indent;
	int show_whitespace;
	int syntax_enabled;
	const SyntaxLang *syntax;
	int sel_active;
	int mark_mode;
	int sel_start_y, sel_start_x;
	int sel_end_y, sel_end_x;
	char search_query[128];
	char replace_query[128];
	int search_case_sens;
	int search_use_regex;
	EditorSnap undo_stack[MAX_UNDO];
	int undo_count;
	EditorSnap redo_stack[MAX_UNDO];
	int redo_count;
} EditorState;

static void ed_line_init(EditorLine *ln, int cap) {
	if (cap < 16) cap = 16;
	ln->text = malloc((size_t)cap * sizeof(wchar_t));
	if (ln->text) ln->text[0] = L'\0';
	ln->len = 0;
	ln->cap = ln->text ? cap : 0;
}

static void ed_line_ensure(EditorLine *ln, int need) {
	if (need + 1 <= ln->cap) return;
	int ncap = ln->cap ? ln->cap * 2 : 32;
	while (ncap < need + 1) ncap *= 2;
	wchar_t *tmp = realloc(ln->text, (size_t)ncap * sizeof(wchar_t));
	if (!tmp) return;
	ln->text = tmp;
	ln->cap = ncap;
}

static void ed_insert_line(EditorState *ed, int idx, const wchar_t *src, int len) {
	if (idx < 0) idx = 0;
	if (idx > ed->line_count) idx = ed->line_count;
	if (ed->line_count >= ed->line_cap) {
		int ncap = ed->line_cap ? ed->line_cap * 2 : 128;
		EditorLine *tmp = realloc(ed->lines, (size_t)ncap * sizeof(EditorLine));
		if (!tmp) return;
		ed->lines = tmp;
		ed->line_cap = ncap;
	}
	if (idx < ed->line_count) {
		memmove(&ed->lines[idx + 1], &ed->lines[idx], (size_t)(ed->line_count - idx) * sizeof(EditorLine));
	}
	ed_line_init(&ed->lines[idx], len + 16);
	if (src && len > 0 && ed->lines[idx].text) {
		wmemcpy(ed->lines[idx].text, src, (size_t)len);
		ed->lines[idx].text[len] = L'\0';
		ed->lines[idx].len = len;
	}
	ed->line_count++;
}

static void ed_snap_free(EditorSnap *sp) {
	if (!sp || !sp->lines) return;
	for (int i = 0; i < sp->line_count; i++) free(sp->lines[i].text);
	free(sp->lines);
	sp->lines = NULL;
	sp->line_count = 0;
}

static void ed_clear_redo(EditorState *ed) {
	for (int i = 0; i < ed->redo_count; i++) ed_snap_free(&ed->redo_stack[i]);
	ed->redo_count = 0;
}

static void ed_snap_capture(const EditorState *ed, EditorSnap *out) {
	out->line_count = ed->line_count;
	out->cur_y = ed->cur_y;
	out->cur_x = ed->cur_x;
	out->modified = ed->modified;
	out->lines = malloc((size_t)(ed->line_count > 0 ? ed->line_count : 1) * sizeof(EditorLine));
	if (!out->lines) { out->line_count = 0; return; }
	for (int i = 0; i < ed->line_count; i++) {
		const EditorLine *src = &ed->lines[i];
		ed_line_init(&out->lines[i], src->len + 1);
		if (src->len > 0 && src->text && out->lines[i].text) {
			wmemcpy(out->lines[i].text, src->text, (size_t)src->len);
			out->lines[i].text[src->len] = L'\0';
			out->lines[i].len = src->len;
		}
	}
}

static void ed_snap_restore(EditorState *ed, EditorSnap *sp) {
	if (!sp || !sp->lines) return;
	for (int i = 0; i < ed->line_count; i++) free(ed->lines[i].text);
	free(ed->lines);
	ed->lines = sp->lines;
	ed->line_count = sp->line_count;
	ed->line_cap = sp->line_count;
	ed->cur_y = sp->cur_y;
	ed->cur_x = sp->cur_x;
	ed->modified = sp->modified;
	sp->lines = NULL;
	sp->line_count = 0;
	ed->sel_active = 0;
}

static void ed_push_undo(EditorState *ed) {
	ed_clear_redo(ed);
	if (ed->undo_count >= MAX_UNDO) {
		ed_snap_free(&ed->undo_stack[0]);
		memmove(&ed->undo_stack[0], &ed->undo_stack[1], sizeof(EditorSnap) * (MAX_UNDO - 1));
		ed->undo_count = MAX_UNDO - 1;
	}
	ed_snap_capture(ed, &ed->undo_stack[ed->undo_count++]);
}

static void ed_undo(EditorState *ed) {
	if (ed->undo_count <= 0) return;
	if (ed->redo_count < MAX_UNDO) {
		ed_snap_capture(ed, &ed->redo_stack[ed->redo_count++]);
	}
	ed->undo_count--;
	ed_snap_restore(ed, &ed->undo_stack[ed->undo_count]);
}

static void ed_redo(EditorState *ed) {
	if (ed->redo_count <= 0) return;
	if (ed->undo_count < MAX_UNDO) {
		ed_snap_capture(ed, &ed->undo_stack[ed->undo_count++]);
	}
	ed->redo_count--;
	ed_snap_restore(ed, &ed->redo_stack[ed->redo_count]);
}

static void ed_free(EditorState *ed) {
	for (int i = 0; i < ed->line_count; i++) {
		free(ed->lines[i].text);
	}
	free(ed->lines);
	ed->lines = NULL;
	ed->line_count = 0;
	ed->line_cap = 0;
	for (int i = 0; i < ed->undo_count; i++) ed_snap_free(&ed->undo_stack[i]);
	ed->undo_count = 0;
	ed_clear_redo(ed);
}

static void ed_load_file(EditorState *ed, const char *filepath) {
	snprintf(ed->filepath, sizeof(ed->filepath), "%s", filepath);
	ed->syntax = ed_detect_syntax(filepath);
	ed->syntax_enabled = (ed->syntax != NULL) ? 1 : 0;
	ed->auto_indent = 1;
	FILE *f = fopen(filepath, "rb");
	if (!f) {
		ed_insert_line(ed, 0, L"", 0);
		return;
	}
	fseek(f, 0, SEEK_END);
	long fsz = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (fsz <= 0) {
		fclose(f);
		ed_insert_line(ed, 0, L"", 0);
		return;
	}
	char *raw = malloc((size_t)fsz + 1);
	if (!raw) {
		fclose(f);
		ed_insert_line(ed, 0, L"", 0);
		return;
	}
	size_t n = fread(raw, 1, (size_t)fsz, f);
	fclose(f);
	raw[n] = '\0';

	size_t start = 0;
	for (size_t i = 0; i <= n; i++) {
		if (i == n || raw[i] == '\n') {
			size_t end = i;
			if (end > start && raw[end - 1] == '\r') end--;
			size_t blen = end - start;
			wchar_t *wbuf = malloc((blen + 1) * sizeof(wchar_t));
			if (wbuf) {
				char saved = raw[end];
				raw[end] = '\0';
				size_t wlen = mbstowcs(wbuf, raw + start, blen);
				raw[end] = saved;
				if (wlen == (size_t)-1) {
					for (size_t k = 0; k < blen; k++) {
						unsigned char c = (unsigned char)raw[start + k];
						wbuf[k] = (c >= 32 || c == '\t') ? (wchar_t)c : L'.';
					}
					wlen = blen;
				}
				wbuf[wlen] = L'\0';
				ed_insert_line(ed, ed->line_count, wbuf, (int)wlen);
				free(wbuf);
			}
			start = i + 1;
		}
	}
	free(raw);
	if (ed->line_count == 0) ed_insert_line(ed, 0, L"", 0);
	if (ed->syntax == &SYNTAX_LANGS[4] && ed->line_count > 0 && ed->lines[0].len > 2 &&
	    ed->lines[0].text[0] == L'#' && ed->lines[0].text[1] == L'!') {
		if (wcsstr(ed->lines[0].text, L"python")) ed->syntax = &SYNTAX_LANGS[1];
		else if (wcsstr(ed->lines[0].text, L"sh") || wcsstr(ed->lines[0].text, L"bash") || wcsstr(ed->lines[0].text, L"zsh")) ed->syntax = &SYNTAX_LANGS[2];
	}
}

static int ed_save_file(EditorState *ed) {
	char tmp_path[64] = "";
	int use_sudo = 0;
	FILE *f = fopen(ed->filepath, "wb");
	if (!f && (errno == EACCES || errno == EPERM)) {
		snprintf(tmp_path, sizeof(tmp_path), "/tmp/xc_sudo_edit_XXXXXX");
		int tfd = mkstemp(tmp_path);
		if (tfd >= 0) { f = fdopen(tfd, "wb"); if (f) use_sudo = 1; else { close(tfd); unlink(tmp_path); } }
	}
	if (!f) {
		ui_message("Save Error", "Cannot open file for writing");
		return 0;
	}
	for (int i = 0; i < ed->line_count; i++) {
		EditorLine *ln = &ed->lines[i];
		if (ln->len > 0 && ln->text) {
			size_t max_b = (size_t)ln->len * 4 + 4;
			char *mb = malloc(max_b);
			if (mb) {
				size_t written = wcstombs(mb, ln->text, max_b - 1);
				if (written != (size_t)-1) fwrite(mb, 1, written, f);
				free(mb);
			}
		}
		if (i + 1 < ed->line_count) fputc('\n', f);
	}
	fclose(f);
	if (use_sudo) {
		int sres = sudo_write_from_temp(tmp_path, ed->filepath);
		unlink(tmp_path);
		if (sres != 0) return 0;
	}
	ed->modified = 0;
	return 1;
}

static void ed_norm_sel(const EditorState *ed, int *sy, int *sx, int *ey, int *ex) {
	if (ed->sel_start_y < ed->sel_end_y ||
	    (ed->sel_start_y == ed->sel_end_y && ed->sel_start_x <= ed->sel_end_x)) {
		*sy = ed->sel_start_y; *sx = ed->sel_start_x;
		*ey = ed->sel_end_y;   *ex = ed->sel_end_x;
	} else {
		*sy = ed->sel_end_y;   *sx = ed->sel_end_x;
		*ey = ed->sel_start_y; *ex = ed->sel_start_x;
	}
}

static int ed_is_selected(const EditorState *ed, int y, int x) {
	if (!ed->sel_active) return 0;
	int sy, sx, ey, ex;
	ed_norm_sel(ed, &sy, &sx, &ey, &ex);
	if (sy == ey && sx == ex) return 0;
	if (y < sy || y > ey) return 0;
	if (y == sy && x < sx) return 0;
	if (y == ey && x >= ex) return 0;
	return 1;
}

#define ED_MACRO_MAX 32
#define ED_MACRO_LEN 512
typedef struct {
	int used;
	KeyAction key_action;
	wchar_t key_ch;
	InputEvent events[ED_MACRO_LEN];
	int count;
} EdMacro;

static EdMacro g_ed_macros[ED_MACRO_MAX];
static int g_ed_macro_rec = 0;
static InputEvent g_ed_rec_buf[ED_MACRO_LEN];
static int g_ed_rec_len = 0;

static int g_ed_macros_loaded = 0;

static void ed_macros_get_path(char *out, size_t sz) {
	const char *home = getenv("HOME");
	if (!home || !home[0]) home = "/tmp";
	char dir[MAX_PATH];
	snprintf(dir, sizeof(dir), "%s/.config", home);
	mkdir(dir, 0755);
	snprintf(dir, sizeof(dir), "%s/.config/xcommander", home);
	mkdir(dir, 0755);
	snprintf(out, sz, "%s/.config/xcommander/editor_macros.conf", home);
}

static void ed_macros_save(void) {
	char path[MAX_PATH];
	ed_macros_get_path(path, sizeof(path));
	FILE *f = fopen(path, "w");
	if (!f) return;
	for (int i = 0; i < ED_MACRO_MAX; i++) {
		if (!g_ed_macros[i].used || g_ed_macros[i].count <= 0) continue;
		fprintf(f, "MACRO %d %u %d\n", (int)g_ed_macros[i].key_action,
			(unsigned int)g_ed_macros[i].key_ch, g_ed_macros[i].count);
		for (int j = 0; j < g_ed_macros[i].count; j++) {
			fprintf(f, "EV %d %u\n", (int)g_ed_macros[i].events[j].action,
				(unsigned int)g_ed_macros[i].events[j].ch);
		}
	}
	fclose(f);
}

static void ed_macros_load(void) {
	if (g_ed_macros_loaded) return;
	g_ed_macros_loaded = 1;
	char path[MAX_PATH];
	ed_macros_get_path(path, sizeof(path));
	FILE *f = fopen(path, "r");
	if (!f) return;
	char line[128];
	int cur = -1, exp = 0, got = 0;
	while (fgets(line, sizeof(line), f)) {
		int act = 0, cnt = 0;
		unsigned int ch = 0;
		if (sscanf(line, "MACRO %d %u %d", &act, &ch, &cnt) == 3) {
			if (cur >= 0 && got == 0) g_ed_macros[cur].used = 0;
			cur = -1;
			for (int i = 0; i < ED_MACRO_MAX; i++) {
				if (!g_ed_macros[i].used) { cur = i; break; }
			}
			if (cur >= 0 && cnt > 0 && cnt <= ED_MACRO_LEN) {
				memset(&g_ed_macros[cur], 0, sizeof(EdMacro));
				g_ed_macros[cur].used = 1;
				g_ed_macros[cur].key_action = (KeyAction)act;
				g_ed_macros[cur].key_ch = (wchar_t)ch;
				exp = cnt;
				got = 0;
			} else {
				cur = -1;
			}
		} else if (cur >= 0 && got < exp && sscanf(line, "EV %d %u", &act, &ch) == 2) {
			g_ed_macros[cur].events[got].action = (KeyAction)act;
			g_ed_macros[cur].events[got].ch = (wchar_t)ch;
			got++;
			g_ed_macros[cur].count = got;
		}
	}
	fclose(f);
}

static int ed_macro_is_recordable(KeyAction a) {
	if (a == ACT_NONE || a == ACT_MOD_CHANGE || a == ACT_RESIZE || a == ACT_MACRO_REC) return 0;
	if (a == ACT_MOUSE_LEFT || a == ACT_MOUSE_DBL || a == ACT_MOUSE_RIGHT ||
	    a == ACT_MOUSE_WHEEL_UP || a == ACT_MOUSE_WHEEL_DOWN) return 0;
	return 1;
}

static KeyAction ed_macro_prompt_key(void) {
	int h = 5, w = 52;
	int y = (LINES - h) / 2, x = (COLS - w) / 2;
	if (y < 0) y = 0;
	if (x < 0) x = 0;
	WINDOW *win = newwin(h, w, y, x);
	wbkgd(win, COLOR_PAIR(4));
	box(win, 0, 0);
	mvwprintw(win, 1, 2, "Save Macro (%d steps)", g_ed_rec_len);
	mvwprintw(win, 2, 2, "Press hotkey to assign (Esc = cancel)...");
	wrefresh(win);
	curs_set(0);
	InputEvent kev;
	while (1) {
		kev = input_read();
		if (kev.action == ACT_NONE || kev.action == ACT_MOD_CHANGE || kev.action == ACT_RESIZE) continue;
		if (kev.action == ACT_MOUSE_LEFT || kev.action == ACT_MOUSE_DBL || kev.action == ACT_MOUSE_RIGHT ||
		    kev.action == ACT_MOUSE_WHEEL_UP || kev.action == ACT_MOUSE_WHEEL_DOWN) continue;
		break;
	}
	delwin(win);
	touchwin(stdscr);
	if (kev.action == ACT_ESC || kev.action == ACT_MACRO_REC) return ACT_NONE;
	int slot = -1;
	for (int i = 0; i < ED_MACRO_MAX; i++) {
		if (g_ed_macros[i].used && g_ed_macros[i].key_action == kev.action && g_ed_macros[i].key_ch == kev.ch) {
			slot = i; break;
		}
	}
	if (slot < 0) {
		for (int i = 0; i < ED_MACRO_MAX; i++) {
			if (!g_ed_macros[i].used) { slot = i; break; }
		}
	}
	if (slot < 0) slot = 0;
	g_ed_macros[slot].used = 1;
	g_ed_macros[slot].key_action = kev.action;
	g_ed_macros[slot].key_ch = kev.ch;
	g_ed_macros[slot].count = g_ed_rec_len;
	memcpy(g_ed_macros[slot].events, g_ed_rec_buf, sizeof(InputEvent) * (size_t)g_ed_rec_len);
	ed_macros_save();
	return kev.action;
}

static void ed_macro_key_name(KeyAction a, wchar_t ch, char *out, size_t sz) {
	if (a == ACT_CTRL_CHAR) snprintf(out, sz, "Ctrl+%lc", (wint_t)towupper((wint_t)ch));
	else if (a == ACT_ALT_CHAR) snprintf(out, sz, "Alt+%lc", (wint_t)towupper((wint_t)ch));
	else if (a == ACT_CHAR) snprintf(out, sz, "'%lc'", (wint_t)ch);
	else if (a == ACT_CTRL_A) snprintf(out, sz, "Ctrl+A");
	else if (a == ACT_CTRL_B) snprintf(out, sz, "Ctrl+B");
	else if (a == ACT_CTRL_E) snprintf(out, sz, "Ctrl+E");
	else if (a == ACT_CTRL_G) snprintf(out, sz, "Ctrl+G");
	else if (a == ACT_CTRL_H) snprintf(out, sz, "Ctrl+H");
	else if (a == ACT_CTRL_L) snprintf(out, sz, "Ctrl+L");
	else if (a == ACT_CTRL_O) snprintf(out, sz, "Ctrl+O");
	else if (a == ACT_CTRL_U) snprintf(out, sz, "Ctrl+U");
	else if (a == ACT_F11) snprintf(out, sz, "F11");
	else if (a == ACT_F12) snprintf(out, sz, "F12");
	else snprintf(out, sz, "Key#%d", (int)a);
}

static void ed_macro_preview(const EdMacro *m, char *out, size_t sz) {
	size_t pos = 0;
	out[0] = '\0';
	for (int i = 0; i < m->count && pos + 20 < sz; i++) {
		KeyAction a = m->events[i].action;
		wchar_t c = m->events[i].ch;
		const char *tag = NULL;
		if (a == ACT_CHAR && c >= 32) {
			pos += (size_t)snprintf(out + pos, sz - pos, "%lc", (wint_t)c);
			continue;
		}
		switch (a) {
			case ACT_ENTER: tag = "<Enter>"; break;
			case ACT_TAB: tag = "<Tab>"; break;
			case ACT_SHIFT_TAB: tag = "<ShTab>"; break;
			case ACT_BACKSPACE: tag = "<BS>"; break;
			case ACT_ALT_BACKSPACE: tag = "<Alt-BS>"; break;
			case ACT_DELETE: tag = "<Del>"; break;
			case ACT_UP: tag = "<Up>"; break;
			case ACT_DOWN: tag = "<Down>"; break;
			case ACT_LEFT: tag = "<Left>"; break;
			case ACT_RIGHT: tag = "<Right>"; break;
			case ACT_CTRL_LEFT: tag = "<C-Left>"; break;
			case ACT_CTRL_RIGHT: tag = "<C-Right>"; break;
			case ACT_SHIFT_UP: tag = "<Sel-Up>"; break;
			case ACT_SHIFT_DOWN: tag = "<Sel-Down>"; break;
			case ACT_SHIFT_LEFT: tag = "<Sel-Left>"; break;
			case ACT_SHIFT_RIGHT: tag = "<Sel-Right>"; break;
			case ACT_WORD_SEL_LEFT: tag = "<Sel-Word-L>"; break;
			case ACT_WORD_SEL_RIGHT: tag = "<Sel-Word-R>"; break;
			case ACT_SHIFT_HOME: tag = "<Sel-Home>"; break;
			case ACT_SHIFT_END: tag = "<Sel-End>"; break;
			case ACT_HOME: tag = "<Home>"; break;
			case ACT_END: tag = "<End>"; break;
			case ACT_CTRL_HOME: tag = "<C-Home>"; break;
			case ACT_CTRL_SHIFT_HOME: tag = "<CS-Home>"; break;
			case ACT_CTRL_SHIFT_END: tag = "<CS-End>"; break;
			case ACT_CTRL_END: tag = "<C-End>"; break;
			case ACT_PGUP: tag = "<PgUp>"; break;
			case ACT_PGDN: tag = "<PgDn>"; break;
			case ACT_CTRL_A: tag = "<C-A>"; break;
			case ACT_CTRL_C: tag = "<C-C>"; break;
			case ACT_CTRL_D: tag = "<C-D>"; break;
			case ACT_CTRL_SHIFT_D: tag = "<Dup>"; break;
			case ACT_CTRL_R: tag = "<Redo>"; break;
			case ACT_CTRL_V: tag = "<C-V>"; break;
			case ACT_CTRL_X: tag = "<C-X>"; break;
			case ACT_CTRL_Y: tag = "<DelLine>"; break;
			case ACT_CTRL_Z: tag = "<Undo>"; break;
			case ACT_TOGGLE_COMMENT: tag = "<Comment>"; break;
			case ACT_SELECT_MATCHING_BLOCK: tag = "<SelBlock>"; break;
			case ACT_MOVE_LINES_UP: tag = "<MoveUp>"; break;
			case ACT_MOVE_LINES_DOWN: tag = "<MoveDn>"; break;
			default: break;
		}
		if (tag) pos += (size_t)snprintf(out + pos, sz - pos, "%s", tag);
		else pos += (size_t)snprintf(out + pos, sz - pos, "<#%d>", (int)a);
	}
}

static int ed_macro_manager(void) {
	int idx[ED_MACRO_MAX];
	int cnt = 0;
	for (int i = 0; i < ED_MACRO_MAX; i++) {
		if (g_ed_macros[i].used && g_ed_macros[i].count > 0) idx[cnt++] = i;
	}
	if (cnt == 0) {
		ui_message("Editor Macros", "No macros saved yet. Press Ctrl+. to record.");
		return -1;
	}
	int h, w;
	getmaxyx(stdscr, h, w);
	int bw = (w - 6 > 92) ? 92 : (w - 6);
	if (bw < 54) bw = w - 2;
	int bh = (h - 4 > 22) ? 22 : (h - 4);
	if (bh < 14) bh = h - 2;
	int sy = (h - bh) / 2, sx = (w - bw) / 2;

	curs_set(0);
	WINDOW *win = newwin(bh, bw, sy, sx);
	wbkgd(win, COLOR_PAIR(5));

	const char *btns[3] = {"[ Run ]", "[ Delete ]", "[ Close ]"};
	int total_bw = 7 + 2 + 10 + 2 + 9;
	int sel = 0, top = 0, btn = 0, run_slot = -1;

	while (1) {
		int prev_rows = 5;
		int lh = bh - (prev_rows + 4);
		if (lh < 3) lh = 3;
		if (sel >= cnt) sel = cnt - 1;
		if (sel < 0) sel = 0;
		if (sel < top) top = sel;
		if (sel >= top + lh) top = sel - lh + 1;

		werase(win);
		box(win, 0, 0);
		mvwprintw(win, 0, 2, " Editor Macros (%d/%d) ", cnt, ED_MACRO_MAX);

		for (int r = 0; r < lh && top + r < cnt; r++) {
			int mi = idx[top + r];
			char kname[24], prev[256];
			ed_macro_key_name(g_ed_macros[mi].key_action, g_ed_macros[mi].key_ch, kname, sizeof(kname));
			ed_macro_preview(&g_ed_macros[mi], prev, sizeof(prev));
			int max_p = bw - 28;
			if (max_p < 8) max_p = 8;
			wattron(win, (top + r == sel) ? COLOR_PAIR(2) : COLOR_PAIR(5));
			mvwhline(win, 1 + r, 1, ' ', bw - 2);
			mvwprintw(win, 1 + r, 2, "%-10s (%3d steps)  %.*s", kname, g_ed_macros[mi].count, max_p, prev);
			wattroff(win, (top + r == sel) ? COLOR_PAIR(2) : COLOR_PAIR(5));
		}

		int sep_y = 1 + lh;
		mvwaddstr(win, sep_y, 0, "├");
		for (int x = 1; x < bw - 1; x++) mvwaddstr(win, sep_y, x, "─");
		mvwaddstr(win, sep_y, bw - 1, "┤");

		char full_prev[2048];
		wchar_t wprev[1024];
		ed_macro_preview(&g_ed_macros[idx[sel]], full_prev, sizeof(full_prev));
		int wlen = (int)mbstowcs(wprev, full_prev, 1023);
		if (wlen < 0) wlen = 0;
		wprev[wlen] = L'\0';
		int inner_w = bw - 4;
		for (int line = 0; line < prev_rows; line++) {
			int off = line * inner_w;
			if (off >= wlen) break;
			mvwprintw(win, sep_y + 1 + line, 2, "%.*ls", inner_w, wprev + off);
		}

		int bx0 = (bw - total_bw < 2) ? 2 : (bw - total_bw) / 2;
		UiButtonRow brow = { btns, 3, bh - 2, bx0, 2, btn };
		ui_btns_draw(&brow, win);
		wrefresh(win);

		InputEvent ev = input_read();
		if (ev.action == ACT_MOUSE_LEFT || ev.action == ACT_MOUSE_DBL) {
			int wy = ev.mouse_y - sy, wx = ev.mouse_x - sx;
			if (wy < 0 || wy >= bh || wx < 0 || wx >= bw) break;
			if (wy >= 1 && wy < 1 + lh && (top + wy - 1) < cnt) {
				sel = top + wy - 1;
				if (ev.action == ACT_MOUSE_DBL) { run_slot = idx[sel]; break; }
				continue;
			}
			int hit = ui_btns_hit_test(&brow, wy, wx);
			if (hit >= 0) {
				btn = hit;
				if (hit == 0) { run_slot = idx[sel]; break; }
				if (hit == 2) break;
				if (hit == 1) ev.action = ACT_F8;
			}
		}
		if (ev.action == ACT_ESC || ev.action == ACT_F10 || ev.action == ACT_CTRL_Q) break;
		if (ev.action == ACT_UP || ev.action == ACT_MOUSE_WHEEL_UP) { if (sel > 0) sel--; }
		else if (ev.action == ACT_DOWN || ev.action == ACT_MOUSE_WHEEL_DOWN) { if (sel + 1 < cnt) sel++; }
		else if (ev.action == ACT_LEFT || ev.action == ACT_SHIFT_TAB) { btn = (btn + 2) % 3; }
		else if (ev.action == ACT_RIGHT || ev.action == ACT_TAB) { btn = (btn + 1) % 3; }
		else if (ev.action == ACT_ENTER) {
			if (btn == 0) { run_slot = idx[sel]; break; }
			if (btn == 2) break;
			if (btn == 1) ev.action = ACT_F8;
		}
		if (ev.action == ACT_F8 || ev.action == ACT_DELETE) {
			g_ed_macros[idx[sel]].used = 0;
			ed_macros_save();
			cnt = 0;
			for (int i = 0; i < ED_MACRO_MAX; i++) {
				if (g_ed_macros[i].used && g_ed_macros[i].count > 0) idx[cnt++] = i;
			}
			if (cnt == 0) break;
		}
	}
	delwin(win);
	touchwin(stdscr);
	return run_slot;
}

static void ed_draw_bars(const EditorState *ed, int height, int width) {
	attrset(COLOR_PAIR(2));
	mvhline(0, 0, ' ', width);

	const char *base = strrchr(ed->filepath, '/');
	base = base ? (base + 1) : ed->filepath;

	wchar_t cur_ch = 0;
	if (ed->cur_y >= 0 && ed->cur_y < ed->line_count) {
		const EditorLine *ln = &ed->lines[ed->cur_y];
		if (ed->cur_x >= 0 && ed->cur_x < ln->len) cur_ch = ln->text[ed->cur_x];
	}

	char right_info[128];
	snprintf(right_info, sizeof(right_info), "[%s]  Ln %d/%d  Col %d  U+%04X  %s ", (ed->syntax && ed->syntax_enabled) ? ed->syntax->name : "Plain",
		ed->cur_y + 1, ed->line_count, ed->cur_x + 1,
		(unsigned int)cur_ch, ed->overwrite_mode ? "OVR" : "INS");

	int rlen = (int)strlen(right_info);
	int rec_w = g_ed_macro_rec ? 6 : 0;
	int mrk_w = ed->mark_mode ? 7 : 0;
	int name_space = width - rlen - rec_w - mrk_w - 4;
	if (name_space > 0) {
		mvprintw(0, 1, "%s%.*s", ed->modified ? "*" : "", name_space, base);
	}
	if (ed->mark_mode && width > rlen + rec_w + mrk_w) {
		attrset(COLOR_PAIR(4) | A_BOLD);
		mvprintw(0, width - rlen - rec_w - mrk_w, " MARK ");
		attrset(COLOR_PAIR(2));
	}
	if (g_ed_macro_rec && width > rlen + rec_w) {
		attrset(COLOR_PAIR(17) | A_BOLD);
		mvprintw(0, width - rlen - rec_w, " REC ");
		attrset(COLOR_PAIR(2));
	}
	if (width > rlen) {
		mvprintw(0, width - rlen, "%s", right_info);
	}

	const char *labels[12] = {
		"Help", "Save", ed->mark_mode ? "Unmark" : "Mark", ed->syntax_enabled ? "NoSyn" : "Syntax", ed->show_whitespace ? "NoSpc" : "Spaces",
		"", "Search", "", ed->auto_indent ? "Ind:On" : "Ind:Of", "Quit", "", ""
	};
	if (g_mod_ctrl && !g_mod_alt && !g_mod_shift) {
		for (int i = 0; i < 12; i++) labels[i] = "";
		labels[6] = "Replac";
	} else if (g_mod_shift && !g_mod_ctrl && !g_mod_alt) {
		for (int i = 0; i < 12; i++) labels[i] = "";
		labels[1] = "SaveAs";
		labels[6] = "Next";
	} else if (g_mod_alt && !g_mod_ctrl && !g_mod_shift) {
		for (int i = 0; i < 12; i++) labels[i] = "";
		labels[7] = "Goto";
	}
	attrset(COLOR_PAIR(6));
	mvhline(height - 1, 0, ' ', width);
	int col_w = width / 12;
	if (col_w < 3) col_w = 3;
	for (int i = 0; i < 12; i++) {
		int x = i * col_w;
		if (x >= width) break;
		int item_w = (i == 11) ? (width - x) : col_w;
		attrset(COLOR_PAIR(6));
		mvprintw(height - 1, x, "%d", i + 1);
		int num_len = (i >= 9) ? 2 : 1;
		int lspace = item_w - num_len;
		if (lspace > 0) {
			attrset(A_NORMAL | COLOR_PAIR(2));
			for (int k = 0; k < lspace; k++) mvaddch(height - 1, x + num_len + k, ' ');
			mvprintw(height - 1, x + num_len, "%.*s", lspace - (lspace > 1 ? 1 : 0), labels[i]);
		}
	}
	attrset(A_NORMAL);
}

static void ed_find_matching_pair(const EditorState *ed, int *y1, int *x1, int *y2, int *x2, int *unmatched) {
	*y1 = *x1 = *y2 = *x2 = -1;
	*unmatched = 0;
	if (ed->cur_y < 0 || ed->cur_y >= ed->line_count) return;
	const EditorLine *ln = &ed->lines[ed->cur_y];

	int check_x = -1;
	wchar_t ch = 0;
	const wchar_t *pairs = L"()[]{}<>\"\'`";
	if (ed->cur_x < ln->len && wcschr(pairs, ln->text[ed->cur_x])) {
		check_x = ed->cur_x;
		ch = ln->text[check_x];
	} else if (ed->cur_x > 0 && ed->cur_x <= ln->len && wcschr(pairs, ln->text[ed->cur_x - 1])) {
		check_x = ed->cur_x - 1;
		ch = ln->text[check_x];
	}
	if (check_x < 0) return;

	*y1 = ed->cur_y;
	*x1 = check_x;

	/* Парные кавычки в пределах текущей строки */
	if (ch == L'"' || ch == L'\'' || ch == L'`') {
		int q_idx = 0, target_idx = -1;
		for (int x = 0; x < ln->len; x++) {
			if (ln->text[x] == L'\\') { x++; continue; }
			if (ln->text[x] == ch) {
				if (x == check_x) target_idx = q_idx;
				q_idx++;
			}
		}
		int pair_idx = (target_idx % 2 == 0) ? (target_idx + 1) : (target_idx - 1);
		q_idx = 0;
		for (int x = 0; x < ln->len; x++) {
			if (ln->text[x] == L'\\') { x++; continue; }
			if (ln->text[x] == ch) {
				if (q_idx == pair_idx) { *y2 = ed->cur_y; *x2 = x; return; }
				q_idx++;
			}
		}
		*unmatched = 1;
		return;
	}

	/* Парные скобки с учётом вложенности по всему файлу */
	wchar_t open_c = 0, close_c = 0;
	int dir = 0;
	if (ch == L'(') { open_c = L'('; close_c = L')'; dir = 1; }
	else if (ch == L')') { open_c = L'('; close_c = L')'; dir = -1; }
	else if (ch == L'[') { open_c = L'['; close_c = L']'; dir = 1; }
	else if (ch == L']') { open_c = L'['; close_c = L']'; dir = -1; }
	else if (ch == L'{') { open_c = L'{'; close_c = L'}'; dir = 1; }
	else if (ch == L'}') { open_c = L'{'; close_c = L'}'; dir = -1; }
	else if (ch == L'<') { open_c = L'<'; close_c = L'>'; dir = 1; }
	else if (ch == L'>') { open_c = L'<'; close_c = L'>'; dir = -1; }

	int depth = 0;
	int y = ed->cur_y;
	int x = check_x;
	int steps = 0;
	while (y >= 0 && y < ed->line_count && steps < 200000) {
		const EditorLine *cl = &ed->lines[y];
		while (x >= 0 && x < cl->len) {
			wchar_t c = cl->text[x];
			if (c == open_c) depth += dir;
			else if (c == close_c) depth -= dir;
			if (depth == 0) { *y2 = y; *x2 = x; return; }
			x += dir;
			steps++;
		}
		y += dir;
		if (y >= 0 && y < ed->line_count) {
			x = (dir > 0) ? 0 : (ed->lines[y].len - 1);
		}
	}
	*unmatched = 1;
}

static void ed_find_enclosing_pair(const EditorState *ed, int *y1, int *x1, int *y2, int *x2) {
	int unm = 0;
	ed_find_matching_pair(ed, y1, x1, y2, x2, &unm);
	if (*y1 >= 0 && *y2 >= 0) return;

	*y1 = *x1 = *y2 = *x2 = -1;
	if (ed->cur_y < 0 || ed->cur_y >= ed->line_count) return;

	/* 1. Проверяем, находится ли курсор внутри кавычек в текущей строке */
	int qy1 = -1, qx1 = -1, qy2 = -1, qx2 = -1;
	const EditorLine *cur_ln = &ed->lines[ed->cur_y];
	wchar_t active_q = 0;
	int q_start = -1;
	for (int x = 0; x < cur_ln->len; x++) {
		wchar_t c = cur_ln->text[x];
		if (active_q && c == L'\\') { x++; continue; }
		if (c == L'"' || c == L'\'' || c == L'`') {
			if (!active_q) {
				active_q = c;
				q_start = x;
			} else if (active_q == c) {
				if (ed->cur_x > q_start && ed->cur_x <= x) {
					qy1 = ed->cur_y; qx1 = q_start;
					qy2 = ed->cur_y; qx2 = x;
					break;
				}
				active_q = 0;
				q_start = -1;
			}
		}
	}

	int d_paren = 0, d_brack = 0, d_brace = 0;
	int y = ed->cur_y;
	int x = ed->cur_x - 1;
	if (x >= ed->lines[y].len) x = ed->lines[y].len - 1;

	int found_y = -1, found_x = -1;
	wchar_t open_c = 0, close_c = 0;
	int steps = 0;

	while (y >= 0 && steps < 200000) {
		const EditorLine *ln = &ed->lines[y];
		while (x >= 0) {
			wchar_t c = ln->text[x];
			if (c == L')') d_paren++;
			else if (c == L'(') {
				if (d_paren == 0) { found_y = y; found_x = x; open_c = L'('; close_c = L')'; goto found_open; }
				d_paren--;
			} else if (c == L']') d_brack++;
			else if (c == L'[') {
				if (d_brack == 0) { found_y = y; found_x = x; open_c = L'['; close_c = L']'; goto found_open; }
				d_brack--;
			} else if (c == L'}') d_brace++;
			else if (c == L'{') {
				if (d_brace == 0) { found_y = y; found_x = x; open_c = L'{'; close_c = L'}'; goto found_open; }
				d_brace--;
			}
			x--;
			steps++;
		}
		y--;
		if (y >= 0) x = ed->lines[y].len - 1;
	}
	goto pick_best;

found_open:
	{
		int by1 = found_y, bx1 = found_x, by2 = -1, bx2 = -1;
		int depth = 0;
		y = found_y;
		x = found_x;
		steps = 0;
		while (y < ed->line_count && steps < 200000) {
			const EditorLine *ln = &ed->lines[y];
			while (x < ln->len) {
				wchar_t c = ln->text[x];
				if (c == open_c) depth++;
				else if (c == close_c) depth--;
				if (depth == 0) { by2 = y; bx2 = x; goto done_brack; }
				x++;
				steps++;
			}
			y++;
			x = 0;
		}
done_brack:
		if (by2 >= 0) {
			int brack_inner = (qy1 < 0) || ((by1 > qy1 || (by1 == qy1 && bx1 > qx1)) && (by2 < qy2 || (by2 == qy2 && bx2 < qx2)));
			if (brack_inner) { *y1 = by1; *x1 = bx1; *y2 = by2; *x2 = bx2; return; }
		}
	}

pick_best:
	if (qy1 >= 0 && qy2 >= 0) { *y1 = qy1; *x1 = qx1; *y2 = qy2; *x2 = qx2; }
}

static void ed_draw(EditorState *ed, int *out_scr_y, int *out_scr_x) {
	int height, width;
	getmaxyx(stdscr, height, width);
	int vis_rows = height - 2;
	if (vis_rows < 1) vis_rows = 1;

	if (ed->cur_y < ed->top_line) ed->top_line = ed->cur_y;
	if (ed->cur_y >= ed->top_line + vis_rows) ed->top_line = ed->cur_y - vis_rows + 1;

	int cursor_vcol = 0;
	if (ed->cur_y >= 0 && ed->cur_y < ed->line_count) {
		const EditorLine *cln = &ed->lines[ed->cur_y];
		for (int i = 0; i < ed->cur_x && i < cln->len; i++) {
			cursor_vcol += (cln->text[i] == L'\t') ? (4 - (cursor_vcol % 4)) : 1;
		}
	}
	if (cursor_vcol < ed->left_col) ed->left_col = cursor_vcol;
	if (cursor_vcol >= ed->left_col + width) ed->left_col = cursor_vcol - width + 1;

	if (ed->sel_active && ed->sel_start_y == ed->cur_y && ed->sel_end_y == ed->cur_y && ed->sel_end_x > ed->cur_x) {
		const EditorLine *cln = &ed->lines[ed->cur_y];
		int end_vcol = 0;
		for (int i = 0; i < ed->sel_end_x && i < cln->len; i++) {
			end_vcol += (cln->text[i] == L'\t') ? (4 - (end_vcol % 4)) : 1;
		}
		if (end_vcol + 2 > ed->left_col + width) {
			int shift = (end_vcol + 2) - width;
			if (shift > cursor_vcol) shift = cursor_vcol;
			if (shift > ed->left_col) ed->left_col = shift;
		}
	}

	int m_y1 = -1, m_x1 = -1, m_y2 = -1, m_x2 = -1, m_unmatched = 0;
	ed_find_matching_pair(ed, &m_y1, &m_x1, &m_y2, &m_x2, &m_unmatched);

	int in_block_cmt = 0;
	const SyntaxLang *active_syn = ed->syntax_enabled ? ed->syntax : NULL;
	if (active_syn && active_syn->c_block_comments) {
		for (int pre = 0; pre < ed->top_line && pre < ed->line_count; pre++) {
			unsigned char dummy[512];
			if (ed->lines[pre].len <= (int)sizeof(dummy)) ed_tokenize_line(active_syn, &ed->lines[pre], dummy, &in_block_cmt);
		}
	}

	for (int r = 0; r < vis_rows; r++) {
		int line_idx = ed->top_line + r;
		int y = 1 + r;
		attrset(COLOR_PAIR(1));
		mvhline(y, 0, ' ', width);
		if (line_idx >= ed->line_count) continue;

		const EditorLine *ln = &ed->lines[line_idx];
		unsigned char *toks = (active_syn && ln->len > 0) ? malloc((size_t)ln->len) : NULL;
		if (toks) ed_tokenize_line(active_syn, ln, toks, &in_block_cmt);

		int vcol = 0;
		for (int i = 0; i < ln->len; i++) {
			wchar_t wc = ln->text[i];
			int cw = (wc == L'\t') ? (4 - (vcol % 4)) : 1;
			int is_sel = ed_is_selected(ed, line_idx, i);
			int is_pair = ((line_idx == m_y1 && i == m_x1) || (line_idx == m_y2 && i == m_x2));
			attr_t tok_attr = toks ? (attr_t)ed_token_attr(toks[i]) : (attr_t)COLOR_PAIR(1);
			if (is_sel) attrset(COLOR_PAIR(2));
			else if (is_pair) attrset(m_unmatched ? (COLOR_PAIR(17) | A_BOLD) : (COLOR_PAIR(12) | A_BOLD));
			else attrset(tok_attr);

			for (int k = 0; k < cw; k++) {
				int sx = vcol - ed->left_col;
				if (sx >= 0 && sx < width) {
					if (wc == L'\t') {
						if (ed->show_whitespace) mvaddstr(y, sx, k == 0 ? "→" : "─");
						else mvaddch(y, sx, ' ');
					} else if (wc == L' ') {
						if (ed->show_whitespace) mvaddstr(y, sx, "·");
						else mvaddch(y, sx, ' ');
					} else if (wc >= 32 && k == 0) {
						char mb[8] = {0};
						int bl = wctomb(mb, wc);
						if (bl > 0) mvaddstr(y, sx, mb);
					} else if (k == 0) {
						mvaddch(y, sx, '.');
					}
				}
				vcol++;
			}
		}
		free(toks);
		{
			int sx = vcol - ed->left_col;
			int is_eol_sel = ed_is_selected(ed, line_idx, ln->len);
			if (sx >= 0 && sx < width && (is_eol_sel || ed->show_whitespace)) {
				attrset(is_eol_sel ? COLOR_PAIR(2) : COLOR_PAIR(1));
				if (ed->show_whitespace && line_idx + 1 < ed->line_count) mvaddstr(y, sx, "¶");
				else mvaddch(y, sx, ' ');
			}
		}
	}

	ed_draw_bars(ed, height, width);
	*out_scr_y = 1 + (ed->cur_y - ed->top_line);
	*out_scr_x = cursor_vcol - ed->left_col;
	if (*out_scr_x < 0) *out_scr_x = 0;
	if (*out_scr_x >= width) *out_scr_x = width - 1;
}

static void ed_delete_line(EditorState *ed, int idx) {
	if (idx < 0 || idx >= ed->line_count) return;
	free(ed->lines[idx].text);
	if (idx + 1 < ed->line_count) {
		memmove(&ed->lines[idx], &ed->lines[idx + 1], (size_t)(ed->line_count - idx - 1) * sizeof(EditorLine));
	}
	ed->line_count--;
	if (ed->line_count == 0) ed_insert_line(ed, 0, L"", 0);
}

static void ed_insert_char(EditorState *ed, wchar_t ch) {
	if (ed->cur_y < 0 || ed->cur_y >= ed->line_count) return;
	EditorLine *ln = &ed->lines[ed->cur_y];
	if (ed->cur_x > ln->len) ed->cur_x = ln->len;

	if (ed->overwrite_mode && ed->cur_x < ln->len) {
		ln->text[ed->cur_x++] = ch;
		ed->modified = 1;
		return;
	}
	ed_line_ensure(ln, ln->len + 1);
	if (!ln->text) return;
	wmemmove(ln->text + ed->cur_x + 1, ln->text + ed->cur_x, (size_t)(ln->len - ed->cur_x + 1));
	ln->text[ed->cur_x++] = ch;
	ln->len++;
	ed->modified = 1;
}

static void ed_insert_newline(EditorState *ed) {
	if (ed->cur_y < 0 || ed->cur_y >= ed->line_count) return;
	EditorLine *ln = &ed->lines[ed->cur_y];
	if (ed->cur_x > ln->len) ed->cur_x = ln->len;

	int indent = 0, extra_tab = 0, split_brace = 0;
	if (ed->auto_indent) {
		while (indent < ed->cur_x && (ln->text[indent] == L' ' || ln->text[indent] == L'\t')) indent++;
		int prev = ed->cur_x - 1;
		while (prev >= indent && (ln->text[prev] == L' ' || ln->text[prev] == L'\t')) prev--;
		if (prev >= indent && (ln->text[prev] == L'{' || ln->text[prev] == L':')) {
			extra_tab = 1;
			int next = ed->cur_x;
			while (next < ln->len && (ln->text[next] == L' ' || ln->text[next] == L'\t')) next++;
			if (ln->text[prev] == L'{' && next < ln->len && ln->text[next] == L'}') {
				split_brace = 1;
			}
		}
	}

	int tail_len = ln->len - ed->cur_x;
	int new_ind = indent + extra_tab;
	wchar_t *new_buf = malloc((size_t)(new_ind + (split_brace ? 0 : tail_len) + 16) * sizeof(wchar_t));
	if (!new_buf) return;
	if (indent > 0) wmemcpy(new_buf, ln->text, (size_t)indent);
	if (extra_tab) new_buf[indent] = L'\t';
	if (!split_brace && tail_len > 0) wmemcpy(new_buf + new_ind, ln->text + ed->cur_x, (size_t)tail_len);
	new_buf[new_ind + (split_brace ? 0 : tail_len)] = L'\0';

	wchar_t *brace_buf = NULL;
	if (split_brace) {
		brace_buf = malloc((size_t)(indent + tail_len + 16) * sizeof(wchar_t));
		if (brace_buf) {
			if (indent > 0) wmemcpy(brace_buf, ln->text, (size_t)indent);
			if (tail_len > 0) wmemcpy(brace_buf + indent, ln->text + ed->cur_x, (size_t)tail_len);
			brace_buf[indent + tail_len] = L'\0';
		}
	}

	ln->text[ed->cur_x] = L'\0';
	ln->len = ed->cur_x;

	ed_insert_line(ed, ed->cur_y + 1, new_buf, new_ind + (split_brace ? 0 : tail_len));
	free(new_buf);
	if (split_brace && brace_buf) {
		ed_insert_line(ed, ed->cur_y + 2, brace_buf, indent + tail_len);
		free(brace_buf);
	}

	ed->cur_y++;
	ed->cur_x = new_ind;
	ed->modified = 1;
}

static void ed_backspace(EditorState *ed) {
	if (ed->cur_y < 0 || ed->cur_y >= ed->line_count) return;
	EditorLine *ln = &ed->lines[ed->cur_y];
	if (ed->cur_x > 0) {
		wmemmove(ln->text + ed->cur_x - 1, ln->text + ed->cur_x, (size_t)(ln->len - ed->cur_x + 1));
		ed->cur_x--;
		ln->len--;
		ed->modified = 1;
	} else if (ed->cur_y > 0) {
		EditorLine *prev = &ed->lines[ed->cur_y - 1];
		int join_x = prev->len;
		ed_line_ensure(prev, prev->len + ln->len);
		if (ln->len > 0 && prev->text && ln->text) {
			wmemcpy(prev->text + prev->len, ln->text, (size_t)ln->len);
			prev->len += ln->len;
			prev->text[prev->len] = L'\0';
		}
		ed_delete_line(ed, ed->cur_y);
		ed->cur_y--;
		ed->cur_x = join_x;
		ed->modified = 1;
	}
}

static void ed_delete_char(EditorState *ed) {
	if (ed->cur_y < 0 || ed->cur_y >= ed->line_count) return;
	EditorLine *ln = &ed->lines[ed->cur_y];
	if (ed->cur_x < ln->len) {
		wmemmove(ln->text + ed->cur_x, ln->text + ed->cur_x + 1, (size_t)(ln->len - ed->cur_x));
		ln->len--;
		ed->modified = 1;
	} else if (ed->cur_y + 1 < ed->line_count) {
		EditorLine *next = &ed->lines[ed->cur_y + 1];
		ed_line_ensure(ln, ln->len + next->len);
		if (next->len > 0 && ln->text && next->text) {
			wmemcpy(ln->text + ln->len, next->text, (size_t)next->len);
			ln->len += next->len;
			ln->text[ln->len] = L'\0';
		}
		ed_delete_line(ed, ed->cur_y + 1);
		ed->modified = 1;
	}
}

static void ed_clear_sel(EditorState *ed) {
	ed->sel_active = 0;
	ed->mark_mode = 0;
}

static void ed_update_sel(EditorState *ed, int old_y, int old_x) {
	if (!ed->sel_active) {
		ed->sel_active = 1;
		ed->sel_start_y = old_y;
		ed->sel_start_x = old_x;
	}
	ed->sel_end_y = ed->cur_y;
	ed->sel_end_x = ed->cur_x;
	if (ed->sel_start_y == ed->sel_end_y && ed->sel_start_x == ed->sel_end_x) {
		ed->sel_active = 0;
	}
}

static void ed_indent_lines(EditorState *ed, int dir) {
	if (!ed || ed->line_count <= 0) return;
	int first_line = ed->cur_y, last_line = ed->cur_y;
	if (ed->sel_active) {
		int sy, sx, ey, ex;
		ed_norm_sel(ed, &sy, &sx, &ey, &ex);
		first_line = sy;
		last_line = (ey > sy && ex == 0) ? (ey - 1) : ey;
	}
	if (first_line < 0) first_line = 0;
	if (last_line >= ed->line_count) last_line = ed->line_count - 1;

	ed_push_undo(ed);
	for (int y = first_line; y <= last_line; y++) {
		EditorLine *ln = &ed->lines[y];
		if (dir > 0) {
			if (ln->len == 0 && first_line != last_line) continue;
			ed_line_ensure(ln, ln->len + 1);
			wmemmove(ln->text + 1, ln->text, (size_t)(ln->len + 1));
			ln->text[0] = L'\t';
			ln->len++;
			if (y == ed->cur_y) ed->cur_x++;
			if (ed->sel_active && y == ed->sel_start_y && ed->sel_start_x > 0) ed->sel_start_x++;
			if (ed->sel_active && y == ed->sel_end_y && ed->sel_end_x > 0) ed->sel_end_x++;
		} else {
			int rm = 0;
			if (ln->len > 0 && ln->text[0] == L'\t') rm = 1;
			else {
				while (rm < 4 && rm < ln->len && ln->text[rm] == L' ') rm++;
			}
			if (rm > 0) {
				wmemmove(ln->text, ln->text + rm, (size_t)(ln->len - rm + 1));
				ln->len -= rm;
				if (y == ed->cur_y) ed->cur_x = (ed->cur_x > rm) ? (ed->cur_x - rm) : 0;
				if (ed->sel_active && y == ed->sel_start_y) ed->sel_start_x = (ed->sel_start_x > rm) ? (ed->sel_start_x - rm) : 0;
				if (ed->sel_active && y == ed->sel_end_y) ed->sel_end_x = (ed->sel_end_x > rm) ? (ed->sel_end_x - rm) : 0;
			}
		}
	}
	ed->modified = 1;
}

static void ed_toggle_comment(EditorState *ed) {
	if (!ed || ed->line_count <= 0) return;
	const char *cmt_utf = (ed->syntax && ed->syntax->line_comment && ed->syntax->line_comment[0])
	                      ? ed->syntax->line_comment : "//";
	wchar_t wcmt[16] = {0};
	int clen = (int)mbstowcs(wcmt, cmt_utf, 15);
	if (clen <= 0) { wcscpy(wcmt, L"//"); clen = 2; }

	int first_line = ed->cur_y, last_line = ed->cur_y;
	if (ed->sel_active) {
		int sy, sx, ey, ex;
		ed_norm_sel(ed, &sy, &sx, &ey, &ex);
		first_line = sy;
		last_line = (ey > sy && ex == 0) ? (ey - 1) : ey;
	}
	if (first_line < 0) first_line = 0;
	if (last_line >= ed->line_count) last_line = ed->line_count - 1;

	int non_blank = 0, all_commented = 1, min_indent = -1;
	for (int y = first_line; y <= last_line; y++) {
		const EditorLine *ln = &ed->lines[y];
		int ind = 0;
		while (ind < ln->len && (ln->text[ind] == L' ' || ln->text[ind] == L'\t')) ind++;
		if (ind >= ln->len) continue;
		non_blank++;
		if (min_indent < 0 || ind < min_indent) min_indent = ind;
		if (ln->len - ind < clen || wcsncmp(ln->text + ind, wcmt, (size_t)clen) != 0) {
			all_commented = 0;
		}
	}

	ed_push_undo(ed);
	if (non_blank > 0 && all_commented) {
		for (int y = first_line; y <= last_line; y++) {
			EditorLine *ln = &ed->lines[y];
			int ind = 0;
			while (ind < ln->len && (ln->text[ind] == L' ' || ln->text[ind] == L'\t')) ind++;
			if (ind >= ln->len) continue;
			if (ln->len - ind >= clen && wcsncmp(ln->text + ind, wcmt, (size_t)clen) == 0) {
				int rm = clen;
				if (ind + rm < ln->len && ln->text[ind + rm] == L' ') rm++;
				wmemmove(ln->text + ind, ln->text + ind + rm, (size_t)(ln->len - (ind + rm) + 1));
				ln->len -= rm;
				if (y == ed->cur_y && ed->cur_x > ind) {
					ed->cur_x = (ed->cur_x - rm >= ind) ? (ed->cur_x - rm) : ind;
				}
			}
		}
	} else {
		if (min_indent < 0) min_indent = 0;
		for (int y = first_line; y <= last_line; y++) {
			EditorLine *ln = &ed->lines[y];
			int ind = 0;
			while (ind < ln->len && (ln->text[ind] == L' ' || ln->text[ind] == L'\t')) ind++;
			if (ind >= ln->len && non_blank > 0) continue;
			int ins_pos = (non_blank > 0) ? min_indent : ind;
			if (ins_pos > ln->len) ins_pos = ln->len;
			int add = clen + 1;
			ed_line_ensure(ln, ln->len + add);
			wmemmove(ln->text + ins_pos + add, ln->text + ins_pos, (size_t)(ln->len - ins_pos + 1));
			wmemcpy(ln->text + ins_pos, wcmt, (size_t)clen);
			ln->text[ins_pos + clen] = L' ';
			ln->len += add;
			if (y == ed->cur_y && ed->cur_x >= ins_pos) ed->cur_x += add;
		}
	}
	ed->modified = 1;
}

static void ed_move_lines(EditorState *ed, int dir) {
	if (!ed || ed->line_count <= 1) return;
	int first_line = ed->cur_y, last_line = ed->cur_y;
	if (ed->sel_active) {
		int sy, sx, ey, ex;
		ed_norm_sel(ed, &sy, &sx, &ey, &ex);
		first_line = sy;
		last_line = (ey > sy && ex == 0) ? (ey - 1) : ey;
	}
	if (dir < 0 && first_line <= 0) return;
	if (dir > 0 && last_line >= ed->line_count - 1) return;

	ed_push_undo(ed);
	if (dir < 0) {
		EditorLine tmp = ed->lines[first_line - 1];
		memmove(&ed->lines[first_line - 1], &ed->lines[first_line],
		        (size_t)(last_line - first_line + 1) * sizeof(EditorLine));
		ed->lines[last_line] = tmp;
		ed->cur_y--;
		if (ed->sel_active) { ed->sel_start_y--; ed->sel_end_y--; }
	} else {
		EditorLine tmp = ed->lines[last_line + 1];
		memmove(&ed->lines[first_line + 1], &ed->lines[first_line],
		        (size_t)(last_line - first_line + 1) * sizeof(EditorLine));
		ed->lines[first_line] = tmp;
		ed->cur_y++;
		if (ed->sel_active) { ed->sel_start_y++; ed->sel_end_y++; }
	}
	ed->modified = 1;
}

static void ed_duplicate_line_or_sel(EditorState *ed) {
	if (!ed || ed->line_count <= 0) return;
	ed_push_undo(ed);

	if (!ed->sel_active) {
		EditorLine *cur = &ed->lines[ed->cur_y];
		int clen = cur->len;
		wchar_t *copy = malloc((size_t)(clen + 1) * sizeof(wchar_t));
		if (!copy) return;
		if (clen > 0 && cur->text) wmemcpy(copy, cur->text, (size_t)clen);
		copy[clen] = L'\0';
		ed_insert_line(ed, ed->cur_y + 1, copy, clen);
		free(copy);
		ed->cur_y++;
		if (ed->cur_x > ed->lines[ed->cur_y].len) ed->cur_x = ed->lines[ed->cur_y].len;
		ed->modified = 1;
		return;
	}

	int sy, sx, ey, ex;
	ed_norm_sel(ed, &sy, &sx, &ey, &ex);
	if (sy == ey && sx == ex) {
		ed->sel_active = 0;
		return;
	}

	if (sy == ey) {
		EditorLine *ln = &ed->lines[sy];
		if (sx < 0) sx = 0;
		if (ex > ln->len) ex = ln->len;
		int flen = ex - sx;
		if (flen <= 0) return;
		wchar_t *frag = malloc((size_t)flen * sizeof(wchar_t));
		if (!frag) return;
		wmemcpy(frag, ln->text + sx, (size_t)flen);
		ed_line_ensure(ln, ln->len + flen);
		wmemmove(ln->text + ex + flen, ln->text + ex, (size_t)(ln->len - ex + 1));
		wmemcpy(ln->text + ex, frag, (size_t)flen);
		ln->len += flen;
		free(frag);
		ed->sel_start_y = sy; ed->sel_start_x = ex;
		ed->sel_end_y = sy;   ed->sel_end_x = ex + flen;
		ed->cur_y = sy;       ed->cur_x = ex + flen;
		ed->modified = 1;
		return;
	}

	/* Многострочное выделение (sy < ey) */
	int span = ey - sy;
	EditorLine *first = &ed->lines[sy];
	EditorLine *last = &ed->lines[ey];
	if (sx > first->len) sx = first->len;
	if (ex > last->len) ex = last->len;

	int head_len = first->len - sx;
	wchar_t *head_copy = malloc((size_t)(head_len + 1) * sizeof(wchar_t));
	if (!head_copy) return;
	if (head_len > 0 && first->text) wmemcpy(head_copy, first->text + sx, (size_t)head_len);
	head_copy[head_len] = L'\0';

	wchar_t **mid_copies = calloc((size_t)span, sizeof(wchar_t *));
	int *mid_lens = calloc((size_t)span, sizeof(int));
	if (!mid_copies || !mid_lens) { free(head_copy); free(mid_copies); free(mid_lens); return; }
	for (int k = 1; k < span; k++) {
		EditorLine *ml = &ed->lines[sy + k];
		mid_lens[k] = ml->len;
		mid_copies[k] = malloc((size_t)(ml->len + 1) * sizeof(wchar_t));
		if (mid_copies[k]) {
			if (ml->len > 0 && ml->text) wmemcpy(mid_copies[k], ml->text, (size_t)ml->len);
			mid_copies[k][ml->len] = L'\0';
		}
	}

	int tail_after_len = last->len - ex;
	wchar_t *tail_after = malloc((size_t)(tail_after_len + 1) * sizeof(wchar_t));
	if (!tail_after) {
		free(head_copy);
		for (int k = 1; k < span; k++) free(mid_copies[k]);
		free(mid_copies); free(mid_lens);
		return;
	}
	if (tail_after_len > 0 && last->text) wmemcpy(tail_after, last->text + ex, (size_t)tail_after_len);
	tail_after[tail_after_len] = L'\0';

	/* В точке (ey, ex) дописываем начало первой выделенной строки */
	ed_line_ensure(&ed->lines[ey], ex + head_len);
	if (head_len > 0) wmemcpy(ed->lines[ey].text + ex, head_copy, (size_t)head_len);
	ed->lines[ey].len = ex + head_len;
	ed->lines[ey].text[ed->lines[ey].len] = L'\0';
	free(head_copy);

	/* Вставляем промежуточные строки */
	for (int k = 1; k < span; k++) {
		ed_insert_line(ed, ey + k, mid_copies[k] ? mid_copies[k] : L"", mid_lens[k]);
		free(mid_copies[k]);
	}
	free(mid_copies); free(mid_lens);

	/* Вставляем конечную строку выделения + остаток оригинальной строки */
	int new_ey = ey + span;
	wchar_t *final_buf = malloc((size_t)(ex + tail_after_len + 1) * sizeof(wchar_t));
	if (final_buf) {
		if (ex > 0 && ed->lines[ey].text) wmemcpy(final_buf, ed->lines[ey].text, (size_t)ex);
		if (tail_after_len > 0) wmemcpy(final_buf + ex, tail_after, (size_t)tail_after_len);
		final_buf[ex + tail_after_len] = L'\0';
		ed_insert_line(ed, new_ey, final_buf, ex + tail_after_len);
		free(final_buf);
	}
	free(tail_after);

	ed->sel_start_y = ey;     ed->sel_start_x = ex;
	ed->sel_end_y = new_ey;   ed->sel_end_x = ex;
	ed->cur_y = new_ey;       ed->cur_x = ex;
	ed->modified = 1;
}

static void ed_delete_selection(EditorState *ed) {
	if (!ed->sel_active) return;
	int sy, sx, ey, ex;
	ed_norm_sel(ed, &sy, &sx, &ey, &ex);
	ed->sel_active = 0;
	if (sy == ey && sx == ex) return;

	if (sy == ey) {
		EditorLine *ln = &ed->lines[sy];
		if (ex > ln->len) ex = ln->len;
		if (sx < ex) {
			wmemmove(ln->text + sx, ln->text + ex, (size_t)(ln->len - ex + 1));
			ln->len -= (ex - sx);
		}
	} else {
		EditorLine *first = &ed->lines[sy];
		EditorLine *last = &ed->lines[ey];
		if (sx > first->len) sx = first->len;
		if (ex > last->len) ex = last->len;
		int tail_len = last->len - ex;
		ed_line_ensure(first, sx + tail_len);
		if (tail_len > 0 && first->text && last->text) {
			wmemcpy(first->text + sx, last->text + ex, (size_t)tail_len);
		}
		first->len = sx + tail_len;
		if (first->text) first->text[first->len] = L'\0';
		for (int y = ey; y > sy; y--) {
			ed_delete_line(ed, y);
		}
	}
	ed->cur_y = sy;
	ed->cur_x = sx;
	ed->modified = 1;
}

static void ed_copy_selection(const EditorState *ed) {
	if (!ed->sel_active) return;
	int sy, sx, ey, ex;
	ed_norm_sel(ed, &sy, &sx, &ey, &ex);
	if (sy == ey && sx == ex) return;

	size_t cap = 4096, len = 0;
	char *out = malloc(cap);
	if (!out) return;
	out[0] = '\0';

	for (int y = sy; y <= ey && y < ed->line_count; y++) {
		const EditorLine *ln = &ed->lines[y];
		int c1 = (y == sy) ? sx : 0;
		int c2 = (y == ey) ? ex : ln->len;
		if (c1 < 0) c1 = 0;
		if (c2 > ln->len) c2 = ln->len;
		if (c2 > c1) {
			int wcnt = c2 - c1;
			wchar_t *wsub = malloc((size_t)(wcnt + 1) * sizeof(wchar_t));
			char *mb = malloc((size_t)wcnt * 4 + 4);
			if (wsub && mb) {
				wmemcpy(wsub, ln->text + c1, (size_t)wcnt);
				wsub[wcnt] = L'\0';
				size_t bl = wcstombs(mb, wsub, (size_t)wcnt * 4 + 3);
				if (bl != (size_t)-1) {
					while (len + bl + 4 >= cap) { cap *= 2; out = realloc(out, cap); }
					if (out) { memcpy(out + len, mb, bl); len += bl; out[len] = '\0'; }
				}
			}
			free(wsub);
			free(mb);
		}
		if (y < ey && out) {
			while (len + 4 >= cap) { cap *= 2; out = realloc(out, cap); }
			if (out) { out[len++] = '\n'; out[len] = '\0'; }
		}
	}
	if (out && len > 0) clipboard_copy(out);
	free(out);
}

static void ed_copy_current_line(const EditorState *ed) {
	if (!ed || ed->cur_y < 0 || ed->cur_y >= ed->line_count) return;
	const EditorLine *ln = &ed->lines[ed->cur_y];
	size_t mbsz = (size_t)ln->len * 4 + 4;
	char *mb = malloc(mbsz);
	if (!mb) return;
	size_t bl = 0;
	if (ln->len > 0 && ln->text) {
		size_t r = wcstombs(mb, ln->text, mbsz - 2);
		if (r != (size_t)-1) bl = r;
	}
	mb[bl++] = '\n';
	mb[bl] = '\0';
	clipboard_copy(mb);
	free(mb);
}

static void ed_cut_current_line(EditorState *ed) {
	if (!ed || ed->cur_y < 0 || ed->cur_y >= ed->line_count) return;
	ed_push_undo(ed);
	ed_copy_current_line(ed);
	if (ed->line_count > 1) {
		ed_delete_line(ed, ed->cur_y);
		if (ed->cur_y >= ed->line_count) ed->cur_y = ed->line_count - 1;
		if (ed->cur_x > ed->lines[ed->cur_y].len) ed->cur_x = ed->lines[ed->cur_y].len;
	} else {
		EditorLine *ln = &ed->lines[0];
		if (ln->text) ln->text[0] = L'\0';
		ln->len = 0;
		ed->cur_x = 0;
	}
	ed->modified = 1;
}

static void ed_paste_clipboard(EditorState *ed) {
	char *raw = malloc(65536);
	if (!raw) return;
	size_t n = clipboard_paste_raw(raw, 65536);
	if (n == 0) { free(raw); return; }

	if (ed->sel_active) ed_delete_selection(ed);

	wchar_t *wbuf = malloc((n + 1) * sizeof(wchar_t));
	if (!wbuf) { free(raw); return; }
	size_t wlen = mbstowcs(wbuf, raw, n);
	free(raw);
	if (wlen == (size_t)-1) { free(wbuf); return; }
	wbuf[wlen] = L'\0';

	int saved_indent = ed->auto_indent;
	ed->auto_indent = 0;
	for (size_t i = 0; i < wlen; i++) {
		if (wbuf[i] == L'\r') continue;
		if (wbuf[i] == L'\n') ed_insert_newline(ed);
		else ed_insert_char(ed, wbuf[i]);
	}
	ed->auto_indent = saved_indent;
	free(wbuf);
}

typedef enum { PAT_LIT, PAT_ANY, PAT_DIGIT, PAT_NDIGIT, PAT_WORD, PAT_NWORD, PAT_SPACE, PAT_NSPACE, PAT_BOL, PAT_EOL, PAT_GO, PAT_GC } PatOp;
typedef struct { PatOp op; wchar_t ch; int min_r, max_r, lazy, gid; } PatNode;
typedef struct { PatNode n[256]; int cnt, gcnt, cs; } XcPat;
typedef struct { int s, e; } XcCap;

static int xc_pat_compile(XcPat *p, const wchar_t *w, int cs, char *err, size_t esz) {
	memset(p, 0, sizeof(*p)); p->cs = cs;
	int gs[16], sp = 0;
	for (size_t i = 0; w[i]; i++) {
		if (p->cnt >= 250) { snprintf(err, esz, "Too long"); return 0; }
		wchar_t c = w[i]; PatNode nd = { PAT_LIT, c, 1, 1, 0, 0 };
		if (c == L'\\') {
			wchar_t x = w[++i];
			if (!x) { snprintf(err, esz, "Trailing \\"); return 0; }
			if (x==L'n') nd.ch=L'\n'; else if (x==L't') nd.ch=L'\t'; else if (x==L'r') nd.ch=L'\r';
			else if (x==L'd') nd.op=PAT_DIGIT; else if (x==L'D') nd.op=PAT_NDIGIT;
			else if (x==L'w') nd.op=PAT_WORD; else if (x==L'W') nd.op=PAT_NWORD;
			else if (x==L's') nd.op=PAT_SPACE; else if (x==L'S') nd.op=PAT_NSPACE;
			else nd.ch = x;
		} else if (c==L'.') nd.op=PAT_ANY;
		else if (c==L'^') nd.op=PAT_BOL;
		else if (c==L'$') nd.op=PAT_EOL;
		else if (c==L'(') { if (p->gcnt>=9||sp>=16) { snprintf(err, esz, "Max 9 groups"); return 0; } nd.op=PAT_GO; nd.gid=++p->gcnt; gs[sp++]=nd.gid; p->n[p->cnt++]=nd; continue; }
		else if (c==L')') { if (sp<=0) { snprintf(err, esz, "Unmatched )"); return 0; } nd.op=PAT_GC; nd.gid=gs[--sp]; p->n[p->cnt++]=nd; continue; }
		else if (c==L'*'||c==L'+'||c==L'?') { snprintf(err, esz, "Bad quantifier"); return 0; }
		wchar_t q = w[i+1];
		if (q==L'*'||q==L'+'||q==L'?') {
			i++; nd.min_r = (q==L'+')?1:0; nd.max_r = (q==L'?')?1:-1;
			if (w[i+1]==L'?') { nd.lazy=1; i++; }
		}
		p->n[p->cnt++] = nd;
	}
	if (sp) { snprintf(err, esz, "Unclosed ("); return 0; }
	return 1;
}

static int xc_mch(const XcPat *p, const PatNode *nd, wchar_t c) {
	switch (nd->op) {
		case PAT_LIT: return p->cs ? (c==nd->ch) : (towlower((wint_t)c)==towlower((wint_t)nd->ch));
		case PAT_ANY: return c != L'\n';
		case PAT_DIGIT: return c>=L'0' && c<=L'9';
		case PAT_NDIGIT: return c!=L'\n' && !(c>=L'0' && c<=L'9');
		case PAT_WORD: return iswalnum((wint_t)c) || c==L'_';
		case PAT_NWORD: return c!=L'\n' && !(iswalnum((wint_t)c) || c==L'_');
		case PAT_SPACE: return c==L' ' || c==L'\t';
		case PAT_NSPACE: return c!=L'\n' && c!=L' ' && c!=L'\t';
		default: return 0;
	}
}

static int xc_match(const XcPat *p, int ni, const wchar_t *b, int len, int pos, XcCap *cap, int *endp) {
	if (ni >= p->cnt) { *endp = pos; return 1; }
	const PatNode *nd = &p->n[ni];
	if (nd->op == PAT_BOL) return (pos==0 || b[pos-1]==L'\n') ? xc_match(p,ni+1,b,len,pos,cap,endp) : 0;
	if (nd->op == PAT_EOL) return (pos==len || b[pos]==L'\n') ? xc_match(p,ni+1,b,len,pos,cap,endp) : 0;
	if (nd->op == PAT_GO) { int o=cap[nd->gid].s; cap[nd->gid].s=pos; if (xc_match(p,ni+1,b,len,pos,cap,endp)) return 1; cap[nd->gid].s=o; return 0; }
	if (nd->op == PAT_GC) { int o=cap[nd->gid].e; cap[nd->gid].e=pos; if (xc_match(p,ni+1,b,len,pos,cap,endp)) return 1; cap[nd->gid].e=o; return 0; }
	int mx = 0;
	while (pos+mx < len && (nd->max_r < 0 || mx < nd->max_r) && xc_mch(p, nd, b[pos+mx])) mx++;
	if (mx < nd->min_r) return 0;
	if (nd->lazy) { for (int k=nd->min_r; k<=mx; k++) if (xc_match(p,ni+1,b,len,pos+k,cap,endp)) return 1; }
	else { for (int k=mx; k>=nd->min_r; k--) if (xc_match(p,ni+1,b,len,pos+k,cap,endp)) return 1; }
	return 0;
}

static wchar_t *ed_flat(const EditorState *ed, int *olen, int **ost) {
	int tot = 0, *st = malloc((size_t)(ed->line_count + 1) * sizeof(int));
	if (!st) return NULL;
	for (int y = 0; y < ed->line_count; y++) { st[y] = tot; tot += ed->lines[y].len + (y + 1 < ed->line_count ? 1 : 0); }
	st[ed->line_count] = tot;
	wchar_t *b = malloc((size_t)(tot + 1) * sizeof(wchar_t));
	if (!b) { free(st); return NULL; }
	int k = 0;
	for (int y = 0; y < ed->line_count; y++) {
		if (ed->lines[y].len > 0) { wmemcpy(b + k, ed->lines[y].text, (size_t)ed->lines[y].len); k += ed->lines[y].len; }
		if (y + 1 < ed->line_count) b[k++] = L'\n';
	}
	b[k] = L'\0'; *olen = tot; *ost = st; return b;
}

static void ed_yx(const int *st, int lc, int pos, int *oy, int *ox) {
	int lo = 0, hi = lc - 1, a = 0;
	while (lo <= hi) { int m = (lo + hi) / 2; if (st[m] <= pos) { a = m; lo = m + 1; } else hi = m - 1; }
	*oy = a; *ox = pos - st[a];
}

static void ed_unflat(EditorState *ed, const wchar_t *b, int len) {
	for (int i = 0; i < ed->line_count; i++) free(ed->lines[i].text);
	free(ed->lines); ed->lines = NULL; ed->line_count = 0; ed->line_cap = 0;
	int st = 0;
	for (int i = 0; i <= len; i++) {
		if (i == len || b[i] == L'\n') {
			int l = i - st; if (l > 0 && b[st + l - 1] == L'\r') l--;
			ed_insert_line(ed, ed->line_count, b + st, l); st = i + 1;
		}
	}
	if (ed->line_count == 0) ed_insert_line(ed, 0, L"", 0);
}

static wchar_t *ed_brepl(const wchar_t *wr, const wchar_t *b, const XcCap *cap, int *olen) {
	size_t c = wcslen(wr) * 2 + 256, j = 0;
	wchar_t *o = malloc(c * sizeof(wchar_t));
	if (!o) return NULL;
	for (size_t i = 0; wr[i]; i++) {
		if (j + 16 >= c) { c *= 2; wchar_t *nb = realloc(o, c * sizeof(wchar_t)); if (!nb) { free(o); return NULL; } o = nb; }
		if (wr[i] == L'\\' && wr[i + 1]) {
			wchar_t x = wr[++i];
			if (x == L'n') { o[j++] = L'\n'; continue; }
			if (x == L't') { o[j++] = L'\t'; continue; }
			if (x == L'r') { o[j++] = L'\r'; continue; }
			if (x == L'\\') { o[j++] = L'\\'; continue; }
			if (x >= L'0' && x <= L'9') {
				int g = x - L'0';
				if (cap[g].s >= 0 && cap[g].e >= cap[g].s) {
					size_t gl = (size_t)(cap[g].e - cap[g].s);
					if (j + gl + 16 >= c) { c = j + gl + 256; wchar_t *nb = realloc(o, c * sizeof(wchar_t)); if (!nb) { free(o); return NULL; } o = nb; }
					if (gl > 0) wmemcpy(o + j, b + cap[g].s, gl);
					j += gl;
				}
				continue;
			}
		}
		o[j++] = wr[i];
	}
	o[j] = L'\0'; *olen = (int)j; return o;
}

static int ed_find(EditorState *ed, int start_y, int start_x) {
	if (ed->search_query[0] == '\0') return 0;
	if (ed->search_use_regex) {
		wchar_t wp[512]; size_t pl = mbstowcs(wp, ed->search_query, 511);
		if (pl == (size_t)-1 || pl == 0) return 0;
		wp[pl] = L'\0';
		XcPat pat; char err[128];
		if (!xc_pat_compile(&pat, wp, ed->search_case_sens, err, sizeof(err))) { ui_message("RegExp Error", err); return -1; }
		int fl = 0, *st = NULL; wchar_t *fb = ed_flat(ed, &fl, &st);
		if (!fb) return 0;
		int sp = (start_y < ed->line_count) ? (st[start_y] + start_x) : fl;
		if (sp < 0) sp = 0;
		for (int pos = sp; pos <= fl; pos++) {
			XcCap cap[10]; for (int g = 0; g < 10; g++) cap[g].s = cap[g].e = -1;
			int ep = pos;
			if (xc_match(&pat, 0, fb, fl, pos, cap, &ep)) {
				int y1, x1, y2, x2; ed_yx(st, ed->line_count, pos, &y1, &x1); ed_yx(st, ed->line_count, ep, &y2, &x2);
				free(fb); free(st);
				ed->cur_y = y1; ed->cur_x = x1; ed->sel_active = 1;
				ed->sel_start_y = y1; ed->sel_start_x = x1; ed->sel_end_y = y2; ed->sel_end_x = x2;
				return 1;
			}
		}
		free(fb); free(st); return 0;
	}
	wchar_t wq[128]; size_t qlen = mbstowcs(wq, ed->search_query, 127);
	if (qlen == (size_t)-1 || qlen == 0) return 0;
	wq[qlen] = L'\0';
	for (int y = start_y; y < ed->line_count; y++) {
		const EditorLine *ln = &ed->lines[y];
		int x0 = (y == start_y) ? start_x : 0;
		for (int x = x0; x + (int)qlen <= ln->len; x++) {
			int eq = ed->search_case_sens ? (wcsncmp(ln->text + x, wq, qlen) == 0) : (wcsncasecmp(ln->text + x, wq, qlen) == 0);
			if (eq) { ed->cur_y = y; ed->cur_x = x; ed->sel_active = 1; ed->sel_start_y = y; ed->sel_start_x = x; ed->sel_end_y = y; ed->sel_end_x = x + (int)qlen; return 1; }
		}
	}
	return 0;
}

static void ed_replace_match(EditorState *ed, int y, int x, int qlen, const wchar_t *wr, int rlen) {
	EditorLine *ln = &ed->lines[y];
	int diff = rlen - qlen;
	if (diff > 0) ed_line_ensure(ln, ln->len + diff);
	if (ln->text) {
		wmemmove(ln->text + x + rlen, ln->text + x + qlen, (size_t)(ln->len - (x + qlen) + 1));
		if (rlen > 0) wmemcpy(ln->text + x, wr, (size_t)rlen);
		ln->len += diff;
	}
	ed->cur_y = y; ed->cur_x = x + rlen; ed->modified = 1;
}

static int ed_interactive_replace(EditorState *ed) {
	if (ed->search_query[0] == '\0') return 0;
	if (ed->search_use_regex) {
		wchar_t wp[512], wrp[512];
		size_t pl = mbstowcs(wp, ed->search_query, 511), rl = mbstowcs(wrp, ed->replace_query, 511);
		if (pl == (size_t)-1 || pl == 0 || rl == (size_t)-1) return 0;
		wp[pl] = L'\0'; wrp[rl] = L'\0';
		XcPat pat; char err[128];
		if (!xc_pat_compile(&pat, wp, ed->search_case_sens, err, sizeof(err))) { ui_message("RegExp Error", err); return 0; }
		int fl = 0, *st = NULL; wchar_t *fb = ed_flat(ed, &fl, &st);
		if (!fb) return 0;
		size_t fcap = (size_t)(fl + 1);
		int pos = (ed->cur_y < ed->line_count) ? (st[ed->cur_y] + ed->cur_x) : fl;
		if (pos < 0) pos = 0;
		int cnt = 0, rall = 0, undo_ok = 0;
		while (pos <= fl) {
			XcCap cap[10]; for (int g = 0; g < 10; g++) cap[g].s = cap[g].e = -1;
			int ep = pos;
			if (!xc_match(&pat, 0, fb, fl, pos, cap, &ep)) { pos++; continue; }
			cap[0].s = pos; cap[0].e = ep;
			int mlen = ep - pos, rlen = 0;
			wchar_t *wr = ed_brepl(wrp, fb, cap, &rlen);
			if (!wr) break;
			if (!rall) {
				int y1, x1, y2, x2; ed_yx(st, ed->line_count, pos, &y1, &x1); ed_yx(st, ed->line_count, ep, &y2, &x2);
				ed->cur_y = y1; ed->cur_x = x1; ed->sel_active = 1;
				ed->sel_start_y = y1; ed->sel_start_x = x1; ed->sel_end_y = y2; ed->sel_end_x = x2;
				int sy = 1, sx = 0; ed_draw(ed, &sy, &sx); refresh();
				char pv[256]; size_t pvl = wcstombs(pv, wr, 255); pv[pvl == (size_t)-1 ? 0 : pvl] = '\0';
				int act = ui_replace_prompt(ed->search_query, pv, sy);
				ed_clear_sel(ed);
				if (act == 0) { free(wr); break; }
				if (act == 3) { free(wr); pos++; continue; }
				if (act == 2) rall = 1;
			}
			if (!undo_ok) { ed_push_undo(ed); undo_ok = 1; }
			int diff = rlen - mlen;
			if ((size_t)(fl + diff + 2) > fcap) { fcap = (size_t)(fl + diff + 1024); wchar_t *nb = realloc(fb, fcap * sizeof(wchar_t)); if (!nb) { free(wr); break; } fb = nb; }
			wmemmove(fb + pos + rlen, fb + ep, (size_t)(fl - ep + 1));
			if (rlen > 0) wmemcpy(fb + pos, wr, (size_t)rlen);
			fl += diff; free(wr); cnt++;
			if (!rall) {
				ed_unflat(ed, fb, fl); free(st); st = malloc((size_t)(ed->line_count + 1) * sizeof(int)); if (!st) break;
				int k = 0; for (int y = 0; y < ed->line_count; y++) { st[y] = k; k += ed->lines[y].len + (y + 1 < ed->line_count ? 1 : 0); } st[ed->line_count] = k;
			}
			pos += (mlen == 0 && rlen == 0) ? 1 : (mlen == 0 ? rlen + 1 : rlen);
		}
		if (undo_ok) {
			ed_unflat(ed, fb, fl); free(st); st = malloc((size_t)(ed->line_count + 1) * sizeof(int));
			if (st) { int k = 0; for (int y = 0; y < ed->line_count; y++) { st[y] = k; k += ed->lines[y].len + (y + 1 < ed->line_count ? 1 : 0); } st[ed->line_count] = k; ed_yx(st, ed->line_count, pos > fl ? fl : pos, &ed->cur_y, &ed->cur_x); }
			ed->modified = 1;
		}
		free(fb); free(st); ed_clear_sel(ed); return cnt;
	}
	wchar_t wq[128], wr[128];
	size_t qlen = mbstowcs(wq, ed->search_query, 127);
	size_t rlen = mbstowcs(wr, ed->replace_query, 127);
	if (qlen == (size_t)-1 || qlen == 0 || rlen == (size_t)-1) return 0;
	wq[qlen] = L'\0';
	wr[rlen] = L'\0';

	int count = 0;
	int replace_all = 0;
	int saved_undo = 0;
	int y = ed->cur_y;
	int x = ed->cur_x;

	while (y < ed->line_count) {
		EditorLine *ln = &ed->lines[y];
		int found = 0;
		while (x + (int)qlen <= ln->len) {
			int eq = ed->search_case_sens
			         ? (wcsncmp(ln->text + x, wq, qlen) == 0)
			         : (wcsncasecmp(ln->text + x, wq, qlen) == 0);
			if (eq) {
				found = 1;
				break;
			}
			x++;
		}
		if (!found) {
			y++;
			x = 0;
			continue;
		}

		if (!replace_all) {
			ed->cur_y = y;
			ed->cur_x = x;
			ed->sel_active = 1;
			ed->sel_start_y = y; ed->sel_start_x = x;
			ed->sel_end_y = y;   ed->sel_end_x = x + (int)qlen;

			int sy = 1, sx = 0;
			ed_draw(ed, &sy, &sx);
			refresh();

			int act = ui_replace_prompt(ed->search_query, ed->replace_query, sy);
			ed_clear_sel(ed);
			if (act == 0) break;
			if (act == 3) {
				x++;
				continue;
			}
			if (act == 2) replace_all = 1;
		}

		if (!saved_undo) {
			ed_push_undo(ed);
			saved_undo = 1;
		}
		ed_replace_match(ed, y, x, (int)qlen, wr, (int)rlen);
		x += (int)rlen;
		count++;
	}

	ed_clear_sel(ed);
	return count;
}

static int ed_replace_all(EditorState *ed) {
	if (ed->search_query[0] == '\0') return 0;
	wchar_t wq[128], wr[128];
	size_t qlen = mbstowcs(wq, ed->search_query, 127);
	size_t rlen = mbstowcs(wr, ed->replace_query, 127);
	if (qlen == (size_t)-1 || qlen == 0 || rlen == (size_t)-1) return 0;
	wq[qlen] = L'\0';
	wr[rlen] = L'\0';

	int count = 0;
	for (int y = 0; y < ed->line_count; y++) {
		EditorLine *ln = &ed->lines[y];
		int x = 0;
		while (x + (int)qlen <= ln->len) {
			if (wcsncasecmp(ln->text + x, wq, qlen) == 0) {
				int diff = (int)rlen - (int)qlen;
				if (diff > 0) ed_line_ensure(ln, ln->len + diff);
				if (ln->text) {
					wmemmove(ln->text + x + rlen, ln->text + x + qlen, (size_t)(ln->len - (x + (int)qlen) + 1));
					if (rlen > 0) wmemcpy(ln->text + x, wr, rlen);
					ln->len += diff;
				}
				x += (int)rlen;
				count++;
				ed->modified = 1;
			} else {
				x++;
			}
		}
	}
	ed_clear_sel(ed);
	return count;
}

static int ed_is_word_ch(wchar_t ch) {
	return iswalnum(ch) || ch == L'_';
}

static void ed_move_word_left(EditorState *ed) {
	if (ed->cur_x == 0) {
		if (ed->cur_y > 0) {
			ed->cur_y--;
			ed->cur_x = ed->lines[ed->cur_y].len;
		}
		return;
	}
	const EditorLine *ln = &ed->lines[ed->cur_y];
	while (ed->cur_x > 0 && !ed_is_word_ch(ln->text[ed->cur_x - 1])) ed->cur_x--;
	while (ed->cur_x > 0 && ed_is_word_ch(ln->text[ed->cur_x - 1])) ed->cur_x--;
}

static void ed_move_word_right(EditorState *ed) {
	const EditorLine *ln = &ed->lines[ed->cur_y];
	if (ed->cur_x >= ln->len) {
		if (ed->cur_y + 1 < ed->line_count) {
			ed->cur_y++;
			ed->cur_x = 0;
		}
		return;
	}
	while (ed->cur_x < ln->len && ed_is_word_ch(ln->text[ed->cur_x])) ed->cur_x++;
	while (ed->cur_x < ln->len && !ed_is_word_ch(ln->text[ed->cur_x])) ed->cur_x++;
}

void ui_edit_file(const char *filepath) {
	ui_edit_file_at(filepath, 1, 1);
}

void ui_edit_file_at(const char *filepath, int start_line, int start_col) {
	ui_edit_file_ex(filepath, start_line, start_col, NULL, NULL);
}

char g_ed_last_search[128] = "";
char g_ed_last_replace[128] = "";
int g_ed_last_case = 0;
int g_ed_last_regex = 0;

void ui_edit_file_ex(const char *filepath, int start_line, int start_col, int *out_line, int *out_col) {
	ed_macros_load();
	EditorState ed;
	memset(&ed, 0, sizeof(ed));
	snprintf(ed.search_query, sizeof(ed.search_query), "%s", g_ed_last_search);
	snprintf(ed.replace_query, sizeof(ed.replace_query), "%s", g_ed_last_replace);
	ed.search_case_sens = g_ed_last_case;
	ed.search_use_regex = g_ed_last_regex;
	ed_load_file(&ed, filepath);
	if (ed.line_count > 0) {
		if (start_line > 0) {
			int ty = start_line - 1;
			if (ty >= ed.line_count) ty = ed.line_count - 1;
			ed.cur_y = ty;
		}
		if (start_col > 0) {
			int tx = start_col - 1;
			if (tx > ed.lines[ed.cur_y].len) tx = ed.lines[ed.cur_y].len;
			ed.cur_x = tx;
		}
	}

	int play_macro = -1, play_pos = 0;
	while (1) {
		int height, width;
		getmaxyx(stdscr, height, width);
		int vis_rows = height - 2;
		if (vis_rows < 1) vis_rows = 1;

		if (ed.cur_y < 0) ed.cur_y = 0;
		if (ed.cur_y >= ed.line_count) ed.cur_y = ed.line_count - 1;
		if (ed.cur_x < 0) ed.cur_x = 0;
		if (ed.cur_x > ed.lines[ed.cur_y].len) ed.cur_x = ed.lines[ed.cur_y].len;

		int sy = 1, sx = 0;
		ed_draw(&ed, &sy, &sx);
		move(sy, sx);
		curs_set(1);
		printf("\033[?2026h"); fflush(stdout);
		wnoutrefresh(stdscr); doupdate();
		printf("\033[?2026l"); fflush(stdout);

		InputEvent ev;
		int from_macro = 0;
		if (play_macro >= 0) {
			if (play_pos < g_ed_macros[play_macro].count) {
				ev = g_ed_macros[play_macro].events[play_pos++];
				from_macro = 1;
			} else {
				play_macro = -1;
				play_pos = 0;
				continue;
			}
		} else {
			ev = input_read();
		}
		if (ev.action == ACT_NONE) continue;
		if (!from_macro && ev.action == ACT_MACRO_REC) {
			if (!g_ed_macro_rec) {
				g_ed_macro_rec = 1;
				g_ed_rec_len = 0;
			} else {
				g_ed_macro_rec = 0;
				if (g_ed_rec_len > 0) {
					ed_macro_prompt_key();
				}
			}
			continue;
		}
		if (!from_macro && g_ed_macro_rec && ed_macro_is_recordable(ev.action)) {
			if (g_ed_rec_len < ED_MACRO_LEN) {
				g_ed_rec_buf[g_ed_rec_len++] = ev;
			}
		} else if (!from_macro && !g_ed_macro_rec && ev.action == ACT_CTRL_CHAR && ev.ch == L'm') {
			int mslot = ed_macro_manager();
			if (mslot >= 0) { play_macro = mslot; play_pos = 0; }
			continue;
		} else if (!from_macro && !g_ed_macro_rec && ed_macro_is_recordable(ev.action)) {
			int found = -1;
			for (int i = 0; i < ED_MACRO_MAX; i++) {
				if (g_ed_macros[i].used && g_ed_macros[i].key_action == ev.action && g_ed_macros[i].key_ch == ev.ch) {
					found = i; break;
				}
			}
			if (found >= 0 && g_ed_macros[found].count > 0) {
				play_macro = found;
				play_pos = 0;
				continue;
			}
		}
		if (ev.action == ACT_RESIZE) { endwin(); refresh(); clearok(stdscr, TRUE); continue; }

		if (ev.action == ACT_MOUSE_WHEEL_UP) {
			ed.top_line -= 3; if (ed.top_line < 0) ed.top_line = 0;
			ed.cur_y -= 3; if (ed.cur_y < 0) ed.cur_y = 0;
			continue;
		}
		if (ev.action == ACT_MOUSE_WHEEL_DOWN) {
			ed.top_line += 3; if (ed.top_line >= ed.line_count) ed.top_line = ed.line_count - 1;
			ed.cur_y += 3; if (ed.cur_y >= ed.line_count) ed.cur_y = ed.line_count - 1;
			continue;
		}
		if (ev.action == ACT_MOUSE_LEFT) {
			if (ev.mouse_y == height - 1) {
				int col_w = width / 12; if (col_w < 3) col_w = 3;
				int idx = ev.mouse_x / col_w; if (idx > 11) idx = 11;
				const KeyAction fk[12] = {
					ACT_F1, g_mod_shift ? ACT_SHIFT_F2 : ACT_F2, ACT_F3, ACT_F4, ACT_F5,
					ACT_F6, ACT_F7, g_mod_alt ? ACT_ALT_F8 : ACT_F8, ACT_F9, ACT_F10, ACT_F11, ACT_F12
				};
				ev.action = fk[idx];
			} else if (ev.mouse_y >= 1 && ev.mouse_y < height - 1) {
				int target_y = ed.top_line + (ev.mouse_y - 1);
				if (target_y >= ed.line_count) target_y = ed.line_count - 1;
				ed.cur_y = target_y;
				const EditorLine *ln = &ed.lines[ed.cur_y];
				int target_vcol = ed.left_col + ev.mouse_x;
				int vcol = 0, cx = 0;
				while (cx < ln->len && vcol < target_vcol) {
					vcol += (ln->text[cx] == L'\t') ? (4 - (vcol % 4)) : 1;
					cx++;
				}
				int my0 = ed.cur_y, mx0 = ed.cur_x;
				ed.cur_x = cx;
				if (ed.mark_mode) ed_update_sel(&ed, my0, mx0); else ed_clear_sel(&ed);
				continue;
			}
		}

		if (ev.action == ACT_ESC || ev.action == ACT_F10 || ev.action == ACT_CTRL_Q) {
			if (ed.modified) {
				curs_set(0);
				int ans = ui_confirm_cancel("Save changes", "File was modified. Save before exit?");
				if (ans < 0) continue;
				if (ans == 1 && !ed_save_file(&ed)) continue;
			}
			break;
		}

		int old_y = ed.cur_y, old_x = ed.cur_x;

		if (ev.action == ACT_CTRL_O) {
			def_prog_mode();
			endwin();
			input_enable_raw();
			input_set_mouse(0);
			while (1) {
				InputEvent tev = input_read();
				if (tev.action == ACT_NONE || tev.action == ACT_MOD_CHANGE || tev.action == ACT_RESIZE) continue;
				break;
			}
			reset_prog_mode();
			input_enable_raw();
			input_set_mouse(1);
			clearok(stdscr, TRUE);
			touchwin(stdscr);
			refresh();
			continue;
		}
		if (ev.action == ACT_F1) {
			curs_set(0);
			ui_editor_help();
		} else if (ev.action == ACT_F2) {
			ed_save_file(&ed);
		} else if (ev.action == ACT_SHIFT_F2) {
			curs_set(0);
			char new_path[MAX_PATH];
			snprintf(new_path, sizeof(new_path), "%s", ed.filepath);
			if (ui_input("Save As", "Enter new file name or path:", new_path, sizeof(new_path)) && new_path[0] != '\0') {
				char resolved[MAX_PATH];
				if (new_path[0] != '/' && active_panel && active_panel->current_path[0] != '\0') {
					snprintf(resolved, sizeof(resolved), "%s/%s", active_panel->current_path, new_path);
				} else {
					snprintf(resolved, sizeof(resolved), "%s", new_path);
				}
				char old_path[MAX_PATH];
				snprintf(old_path, sizeof(old_path), "%s", ed.filepath);
				snprintf(ed.filepath, sizeof(ed.filepath), "%s", resolved);
				if (ed_save_file(&ed)) {
					ed.syntax = ed_detect_syntax(ed.filepath);
					ed.syntax_enabled = (ed.syntax != NULL) ? 1 : 0;
				} else {
					snprintf(ed.filepath, sizeof(ed.filepath), "%s", old_path);
				}
			}
		} else if (ev.action == ACT_F4) {
			if (!ed.syntax) ed.syntax = &SYNTAX_LANGS[0];
			ed.syntax_enabled = !ed.syntax_enabled;
		} else if (ev.action == ACT_F3) {
			if (!ed.mark_mode) {
				ed.mark_mode = 1;
				ed.sel_active = 1;
				ed.sel_start_y = ed.cur_y; ed.sel_start_x = ed.cur_x;
				ed.sel_end_y = ed.cur_y;   ed.sel_end_x = ed.cur_x;
			} else {
				ed.mark_mode = 0;
				if (ed.sel_start_y == ed.sel_end_y && ed.sel_start_x == ed.sel_end_x) ed.sel_active = 0;
			}
		} else if (ev.action == ACT_CTRL_F7) {
			curs_set(0);
			if (ui_replace_dialog(ed.search_query, sizeof(ed.search_query), ed.replace_query, sizeof(ed.replace_query), &ed.search_case_sens, &ed.search_use_regex)) {
				int cnt = ed_interactive_replace(&ed);
				(void)ed_replace_all;
				char rmsg[64];
				snprintf(rmsg, sizeof(rmsg), "Replaced %d occurrence(s)", cnt);
				ui_message("Replace", rmsg);
			}
		} else if (ev.action == ACT_F5) {
			ed.show_whitespace = !ed.show_whitespace;
		} else if (ev.action == ACT_CTRL_C) {
			if (ed.sel_active) ed_copy_selection(&ed);
			else ed_copy_current_line(&ed);
		} else if (ev.action == ACT_CTRL_X) {
			if (ed.sel_active) {
				ed_push_undo(&ed);
				ed_copy_selection(&ed);
				ed_delete_selection(&ed);
			} else {
				ed_cut_current_line(&ed);
			}
		} else if (ev.action == ACT_CTRL_V || ev.action == ACT_SHIFT_INSERT) {
			ed_push_undo(&ed);
			ed_paste_clipboard(&ed);
		} else if (ev.action == ACT_F7) {
			curs_set(0);
			if (ui_search_dialog(ed.search_query, sizeof(ed.search_query), &ed.search_case_sens, &ed.search_use_regex)) {
				int fr = ed_find(&ed, ed.cur_y, ed.cur_x);
				if (fr == 0) fr = ed_find(&ed, 0, 0);
				if (fr == 0) {
					ui_message("Search", "Text not found");
				}
			}
		} else if (ev.action == ACT_SHIFT_F7 && ed.search_query[0] != '\0') {
			int fr = ed_find(&ed, ed.cur_y, ed.cur_x + 1);
			if (fr == 0) {
				curs_set(0);
				ui_message("Search", "No more matches found");
			}
		} else if (ev.action == ACT_ALT_F8) {
			char gbuf[32], gprompt[64];
			snprintf(gbuf, sizeof(gbuf), "%d", ed.cur_y + 1);
			snprintf(gprompt, sizeof(gprompt), "Line[:Col] (1..%d):", ed.line_count);
			curs_set(0);
			if (ui_input_narrow("Go to Line (Alt+F8)", gprompt, gbuf, sizeof(gbuf)) && gbuf[0] != '\0') {
				int t_line = 0, t_col = 1;
				if (sscanf(gbuf, "%d:%d", &t_line, &t_col) >= 1 && t_line >= 1) {
					if (t_line > ed.line_count) t_line = ed.line_count;
					ed.cur_y = t_line - 1;
					if (t_col < 1) t_col = 1;
					int max_c = ed.lines[ed.cur_y].len;
					ed.cur_x = (t_col - 1 <= max_c) ? (t_col - 1) : max_c;
					if (ed.mark_mode) ed_update_sel(&ed, old_y, old_x);
					else ed_clear_sel(&ed);
					int center_top = ed.cur_y - vis_rows / 2;
					if (center_top < 0) center_top = 0;
					ed.top_line = center_top;
				}
			}
		} else if (ev.action == ACT_F9) {
			ed.auto_indent = !ed.auto_indent;
		} else if (ev.action == ACT_CTRL_A) {
			ed.sel_active = 1;
			ed.sel_start_y = 0; ed.sel_start_x = 0;
			ed.sel_end_y = ed.line_count - 1;
			ed.sel_end_x = ed.lines[ed.line_count - 1].len;
			ed.cur_y = ed.sel_end_y;
			ed.cur_x = ed.sel_end_x;
		} else if (ev.action == ACT_CTRL_Z) {
			ed_undo(&ed);
		} else if (ev.action == ACT_CTRL_R) {
			ed_redo(&ed);
		} else if (ev.action == ACT_INSERT_RIGHT_PATH) {
			int m_y1 = -1, m_x1 = -1, m_y2 = -1, m_x2 = -1;
			ed_find_enclosing_pair(&ed, &m_y1, &m_x1, &m_y2, &m_x2);
			if (m_y2 >= 0 && m_x2 >= 0) {
				ed_clear_sel(&ed);
				ed.cur_y = m_y2;
				ed.cur_x = m_x2;
			}
		} else if (ev.action == ACT_SELECT_MATCHING_BLOCK) {
			int m_y1 = -1, m_x1 = -1, m_y2 = -1, m_x2 = -1;
			ed_find_enclosing_pair(&ed, &m_y1, &m_x1, &m_y2, &m_x2);
			if (m_y1 >= 0 && m_y2 >= 0) {
				int forward = (m_y1 < m_y2 || (m_y1 == m_y2 && m_x1 <= m_x2));
				int oy = forward ? m_y1 : m_y2, ox = forward ? m_x1 : m_x2;
				int cy = forward ? m_y2 : m_y1, cx = forward ? m_x2 : m_x1;
				int has_inner = (oy < cy || (oy == cy && ox + 1 < cx));
				int sy, sx, ey, ex;
				ed_norm_sel(&ed, &sy, &sx, &ey, &ex);
				int inner_already = (ed.sel_active && sy == oy && sx == ox + 1 && ey == cy && ex == cx);
				ed.sel_active = 1;
				if (has_inner && !inner_already) {
					ed.sel_start_y = oy; ed.sel_start_x = ox + 1;
					ed.sel_end_y = cy;   ed.sel_end_x = cx;
					ed.cur_y = cy;       ed.cur_x = cx;
				} else {
					ed.sel_start_y = oy; ed.sel_start_x = ox;
					ed.sel_end_y = cy;   ed.sel_end_x = cx + 1;
					ed.cur_y = cy;       ed.cur_x = cx + 1;
				}
			}
		} else if (ev.action == ACT_CTRL_Y) {
			ed_push_undo(&ed);
			ed_clear_sel(&ed);
			ed_delete_line(&ed, ed.cur_y);
			if (ed.cur_y >= ed.line_count) ed.cur_y = ed.line_count - 1;
			ed.cur_x = 0;
			ed.modified = 1;
		} else if (ev.action == ACT_INSERT) {
			ed.overwrite_mode = !ed.overwrite_mode;
		} else if (ev.action == ACT_UP) {
			if (ed.cur_y > 0) ed.cur_y--;
			if (ed.mark_mode) ed_update_sel(&ed, old_y, old_x); else ed_clear_sel(&ed);
		} else if (ev.action == ACT_DOWN) {
			if (ed.cur_y + 1 < ed.line_count) ed.cur_y++;
			if (ed.mark_mode) ed_update_sel(&ed, old_y, old_x); else ed_clear_sel(&ed);
		} else if (ev.action == ACT_LEFT) {
			if (ed.cur_x > 0) ed.cur_x--;
			else if (ed.cur_y > 0) { ed.cur_y--; ed.cur_x = ed.lines[ed.cur_y].len; }
			if (ed.mark_mode) ed_update_sel(&ed, old_y, old_x); else ed_clear_sel(&ed);
		} else if (ev.action == ACT_RIGHT) {
			if (ed.cur_x < ed.lines[ed.cur_y].len) ed.cur_x++;
			else if (ed.cur_y + 1 < ed.line_count) { ed.cur_y++; ed.cur_x = 0; }
			if (ed.mark_mode) ed_update_sel(&ed, old_y, old_x); else ed_clear_sel(&ed);
		} else if (ev.action == ACT_CTRL_LEFT) {
			ed_move_word_left(&ed);
			if (ed.mark_mode) ed_update_sel(&ed, old_y, old_x); else ed_clear_sel(&ed);
		} else if (ev.action == ACT_CTRL_RIGHT) {
			ed_move_word_right(&ed);
			if (ed.mark_mode) ed_update_sel(&ed, old_y, old_x); else ed_clear_sel(&ed);
		} else if (ev.action == ACT_HOME) {
			ed.cur_x = 0;
			if (ed.mark_mode) ed_update_sel(&ed, old_y, old_x); else ed_clear_sel(&ed);
		} else if (ev.action == ACT_END) {
			ed.cur_x = ed.lines[ed.cur_y].len;
			if (ed.mark_mode) ed_update_sel(&ed, old_y, old_x); else ed_clear_sel(&ed);
		} else if (ev.action == ACT_CTRL_HOME) {
			ed.cur_y = 0;
			ed.cur_x = 0;
			if (ed.mark_mode) ed_update_sel(&ed, old_y, old_x); else ed_clear_sel(&ed);
		} else if (ev.action == ACT_CTRL_END) {
			ed.cur_y = ed.line_count - 1;
			ed.cur_x = ed.lines[ed.cur_y].len;
			if (ed.mark_mode) ed_update_sel(&ed, old_y, old_x); else ed_clear_sel(&ed);
		} else if (ev.action == ACT_PGUP) {
			ed.cur_y -= vis_rows; if (ed.cur_y < 0) ed.cur_y = 0;
			if (ed.mark_mode) ed_update_sel(&ed, old_y, old_x); else ed_clear_sel(&ed);
		} else if (ev.action == ACT_PGDN) {
			ed.cur_y += vis_rows; if (ed.cur_y >= ed.line_count) ed.cur_y = ed.line_count - 1;
			if (ed.mark_mode) ed_update_sel(&ed, old_y, old_x); else ed_clear_sel(&ed);
		} else if (ev.action == ACT_SHIFT_UP) {
			if (ed.cur_y > 0) ed.cur_y--;
			ed_update_sel(&ed, old_y, old_x);
		} else if (ev.action == ACT_SHIFT_DOWN) {
			if (ed.cur_y + 1 < ed.line_count) ed.cur_y++;
			ed_update_sel(&ed, old_y, old_x);
		} else if (ev.action == ACT_SHIFT_LEFT) {
			if (ed.cur_x > 0) ed.cur_x--;
			else if (ed.cur_y > 0) { ed.cur_y--; ed.cur_x = ed.lines[ed.cur_y].len; }
			ed_update_sel(&ed, old_y, old_x);
		} else if (ev.action == ACT_SHIFT_RIGHT) {
			if (ed.cur_x < ed.lines[ed.cur_y].len) ed.cur_x++;
			else if (ed.cur_y + 1 < ed.line_count) { ed.cur_y++; ed.cur_x = 0; }
			ed_update_sel(&ed, old_y, old_x);
		} else if (ev.action == ACT_WORD_SEL_LEFT) {
			ed_move_word_left(&ed);
			ed_update_sel(&ed, old_y, old_x);
		} else if (ev.action == ACT_WORD_SEL_RIGHT) {
			ed_move_word_right(&ed);
			ed_update_sel(&ed, old_y, old_x);
		} else if (ev.action == ACT_SHIFT_HOME) {
			ed.cur_x = 0;
			ed_update_sel(&ed, old_y, old_x);
		} else if (ev.action == ACT_SHIFT_END) {
			ed.cur_x = ed.lines[ed.cur_y].len;
			ed_update_sel(&ed, old_y, old_x);
		} else if (ev.action == ACT_CTRL_SHIFT_HOME) {
			ed.cur_y = 0;
			ed.cur_x = 0;
			ed_update_sel(&ed, old_y, old_x);
		} else if (ev.action == ACT_CTRL_SHIFT_END) {
			ed.cur_y = ed.line_count - 1;
			ed.cur_x = ed.lines[ed.cur_y].len;
			ed_update_sel(&ed, old_y, old_x);
		} else if (ev.action == ACT_TOGGLE_COMMENT) {
			ed_toggle_comment(&ed);
		} else if (ev.action == ACT_MOVE_LINES_UP) {
			ed_move_lines(&ed, -1);
		} else if (ev.action == ACT_MOVE_LINES_DOWN) {
			ed_move_lines(&ed, 1);
		} else if (ev.action == ACT_CTRL_SHIFT_D) {
			ed_duplicate_line_or_sel(&ed);
		} else if (ev.action == ACT_ENTER) {
			ed_push_undo(&ed);
			if (ed.sel_active) ed_delete_selection(&ed);
			ed_insert_newline(&ed);
		} else if (ev.action == ACT_BACKSPACE) {
			ed_push_undo(&ed);
			if (ed.sel_active) ed_delete_selection(&ed);
			else ed_backspace(&ed);
		} else if (ev.action == ACT_DELETE) {
			ed_push_undo(&ed);
			if (ed.sel_active) ed_delete_selection(&ed);
			else ed_delete_char(&ed);
		} else if (ev.action == ACT_TAB) {
			if (ed.sel_active) {
				ed_indent_lines(&ed, 1);
			} else {
				ed_push_undo(&ed);
				ed_insert_char(&ed, L'\t');
			}
		} else if (ev.action == ACT_SHIFT_TAB) {
			ed_indent_lines(&ed, -1);
		} else if (ev.action == ACT_CHAR && iswprint(ev.ch)) {
			ed_push_undo(&ed);
			if (ed.sel_active) ed_delete_selection(&ed);
			ed_insert_char(&ed, ev.ch);
		}
	}

	if (out_line) *out_line = ed.cur_y + 1;
	if (out_col) *out_col = ed.cur_x + 1;
	snprintf(g_ed_last_search, sizeof(g_ed_last_search), "%s", ed.search_query);
	snprintf(g_ed_last_replace, sizeof(g_ed_last_replace), "%s", ed.replace_query);
	g_ed_last_case = ed.search_case_sens;
	g_ed_last_regex = ed.search_use_regex;
	curs_set(0);
	ed_free(&ed);
	touchwin(stdscr);
}
