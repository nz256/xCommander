#define _GNU_SOURCE
#include <regex.h>
#include <ncurses.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <wchar.h>
#include <wctype.h>
#include "xc.h"
#include "ui.h"
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
	FILE *f = fopen(ed->filepath, "wb");
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
	int name_space = width - rlen - 4;
	if (name_space > 0) {
		mvprintw(0, 1, "%s%.*s", ed->modified ? "*" : "", name_space, base);
	}
	if (width > rlen) {
		mvprintw(0, width - rlen, "%s", right_info);
	}

	const char *labels[10] = {
		"Help", "Save", "Mark", ed->syntax_enabled ? "NoSyn" : "Syntax", ed->show_whitespace ? "NoSpc" : "Spaces",
		"Move", "Search", "Delete", ed->auto_indent ? "Ind:On" : "Ind:Of", "Quit"
	};
	if (g_mod_ctrl && !g_mod_alt && !g_mod_shift) {
		labels[6] = "Replac";
	} else if (g_mod_shift && !g_mod_ctrl && !g_mod_alt) {
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
					*y1 = ed->cur_y; *x1 = q_start;
					*y2 = ed->cur_y; *x2 = x;
					return;
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
	return;

found_open:
	*y1 = found_y;
	*x1 = found_x;
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
			if (depth == 0) { *y2 = y; *x2 = x; return; }
			x++;
			steps++;
		}
		y++;
		x = 0;
	}
	*y1 = *x1 = -1;
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
			int tok_attr = toks ? ed_token_attr(toks[i]) : COLOR_PAIR(1);
			if (is_sel) attrset(COLOR_PAIR(2));
			else if (is_pair) attrset(m_unmatched ? (COLOR_PAIR(17) | A_BOLD) : (COLOR_PAIR(4) | A_BOLD));
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

static void ed_unescape_tabs(const char *src, char *dst, size_t dst_sz) {
	size_t j = 0;
	for (size_t i = 0; src[i] != '\0' && j + 1 < dst_sz; i++) {
		if (src[i] == '\\' && src[i + 1] == 't') {
			dst[j++] = '\t';
			i++;
		} else {
			dst[j++] = src[i];
		}
	}
	dst[j] = '\0';
}

static int ed_line_to_utf8(const EditorLine *ln, char **out_utf, int **out_b2w) {
	int wlen = ln->len;
	size_t cap = (size_t)wlen * 4 + 8;
	char *utf = malloc(cap);
	int *b2w = malloc(cap * sizeof(int));
	if (!utf || !b2w) { free(utf); free(b2w); return -1; }
	size_t bpos = 0;
	for (int i = 0; i < wlen; i++) {
		char mb[8];
		int bl = wctomb(mb, ln->text[i]);
		if (bl <= 0) { mb[0] = '?'; bl = 1; }
		for (int k = 0; k < bl; k++) {
			utf[bpos] = mb[k];
			b2w[bpos] = i;
			bpos++;
		}
	}
	utf[bpos] = '\0';
	b2w[bpos] = wlen;
	*out_utf = utf;
	*out_b2w = b2w;
	return (int)bpos;
}

static int ed_widx_to_byte(const int *b2w, int blen, int widx) {
	for (int b = 0; b <= blen; b++) {
		if (b2w[b] >= widx) return b;
	}
	return blen;
}

static void ed_build_regex_repl(const char *repl_tmpl, const char *line_utf, const regmatch_t *pm, int nmatch, char *out, size_t out_sz) {
	size_t j = 0;
	for (size_t i = 0; repl_tmpl[i] != '\0' && j + 1 < out_sz; i++) {
		if (repl_tmpl[i] == '\\' && repl_tmpl[i + 1] != '\0') {
			char nx = repl_tmpl[i + 1];
			if (nx == 't') { out[j++] = '\t'; i++; continue; }
			if (nx == 'n') { out[j++] = '\n'; i++; continue; }
			if (nx == '\\') { out[j++] = '\\'; i++; continue; }
			if (nx >= '0' && nx <= '9') {
				int grp = nx - '0';
				if (grp < nmatch && pm[grp].rm_so >= 0 && pm[grp].rm_eo >= pm[grp].rm_so) {
					size_t gl = (size_t)(pm[grp].rm_eo - pm[grp].rm_so);
					if (j + gl >= out_sz) gl = out_sz - 1 - j;
					memcpy(out + j, line_utf + pm[grp].rm_so, gl);
					j += gl;
				}
				i++;
				continue;
			}
		}
		out[j++] = repl_tmpl[i];
	}
	out[j] = '\0';
}

static int ed_find(EditorState *ed, int start_y, int start_x) {
	if (ed->search_query[0] == '\0') return 0;

	if (ed->search_use_regex) {
		char pat[256];
		ed_unescape_tabs(ed->search_query, pat, sizeof(pat));
		regex_t re;
		int cflags = REG_EXTENDED | (ed->search_case_sens ? 0 : REG_ICASE);
		int rc = regcomp(&re, pat, cflags);
		if (rc != 0) {
			char err[128];
			regerror(rc, &re, err, sizeof(err));
			ui_message("RegExp Error", err);
			return -1;
		}
		for (int y = start_y; y < ed->line_count; y++) {
			const EditorLine *ln = &ed->lines[y];
			int x0 = (y == start_y) ? start_x : 0;
			if (x0 > ln->len) continue;
			char *utf = NULL; int *b2w = NULL;
			int blen = ed_line_to_utf8(ln, &utf, &b2w);
			if (blen < 0) continue;
			int boff = ed_widx_to_byte(b2w, blen, x0);
			regmatch_t pm[1];
			int eflags = (boff > 0) ? REG_NOTBOL : 0;
			if (regexec(&re, utf + boff, 1, pm, eflags) == 0) {
				int wx1 = b2w[boff + pm[0].rm_so];
				int wx2 = b2w[boff + pm[0].rm_eo];
				free(utf); free(b2w); regfree(&re);
				ed->cur_y = y;
				ed->cur_x = wx1;
				ed->sel_active = 1;
				ed->sel_start_y = y; ed->sel_start_x = wx1;
				ed->sel_end_y = y;   ed->sel_end_x = wx2;
				return 1;
			}
			free(utf); free(b2w);
		}
		regfree(&re);
		return 0;
	}

	wchar_t wq[128];
	size_t qlen = mbstowcs(wq, ed->search_query, 127);
	if (qlen == (size_t)-1 || qlen == 0) return 0;
	wq[qlen] = L'\0';

	for (int y = start_y; y < ed->line_count; y++) {
		const EditorLine *ln = &ed->lines[y];
		int x0 = (y == start_y) ? start_x : 0;
		for (int x = x0; x + (int)qlen <= ln->len; x++) {
			int eq = ed->search_case_sens
			         ? (wcsncmp(ln->text + x, wq, qlen) == 0)
			         : (wcsncasecmp(ln->text + x, wq, qlen) == 0);
			if (eq) {
				ed->cur_y = y;
				ed->cur_x = x;
				ed->sel_active = 1;
				ed->sel_start_y = y; ed->sel_start_x = x;
				ed->sel_end_y = y;   ed->sel_end_x = x + (int)qlen;
				return 1;
			}
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
	ed->cur_y = y;
	ed->cur_x = x + rlen;
	ed->modified = 1;
}

static int ed_interactive_replace(EditorState *ed) {
	if (ed->search_query[0] == '\0') return 0;

	if (ed->search_use_regex) {
		char pat[256];
		ed_unescape_tabs(ed->search_query, pat, sizeof(pat));
		regex_t re;
		int cflags = REG_EXTENDED | (ed->search_case_sens ? 0 : REG_ICASE);
		int rc = regcomp(&re, pat, cflags);
		if (rc != 0) {
			char err[128];
			regerror(rc, &re, err, sizeof(err));
			ui_message("RegExp Error", err);
			return 0;
		}
		int count = 0, replace_all = 0, saved_undo = 0;
		int y = ed->cur_y, x = ed->cur_x;
		while (y < ed->line_count) {
			EditorLine *ln = &ed->lines[y];
			if (x > ln->len) { y++; x = 0; continue; }
			char *utf = NULL; int *b2w = NULL;
			int blen = ed_line_to_utf8(ln, &utf, &b2w);
			if (blen < 0) { y++; x = 0; continue; }
			int boff = ed_widx_to_byte(b2w, blen, x);
			regmatch_t pm[10];
			int eflags = (boff > 0) ? REG_NOTBOL : 0;
			if (regexec(&re, utf + boff, 10, pm, eflags) != 0) {
				free(utf); free(b2w);
				y++; x = 0;
				continue;
			}
			for (int g = 0; g < 10; g++) {
				if (pm[g].rm_so >= 0) { pm[g].rm_so += boff; pm[g].rm_eo += boff; }
			}
			int wx1 = b2w[pm[0].rm_so];
			int wx2 = b2w[pm[0].rm_eo];
			int mlen = wx2 - wx1;

			char repl_utf[1024];
			ed_build_regex_repl(ed->replace_query, utf, pm, 10, repl_utf, sizeof(repl_utf));
			free(utf); free(b2w);

			if (!replace_all) {
				ed->cur_y = y; ed->cur_x = wx1;
				ed->sel_active = 1;
				ed->sel_start_y = y; ed->sel_start_x = wx1;
				ed->sel_end_y = y;   ed->sel_end_x = wx2;
				int sy = 1, sx = 0;
				ed_draw(ed, &sy, &sx);
				refresh();
				int act = ui_replace_prompt(ed->search_query, repl_utf, sy);
				ed_clear_sel(ed);
				if (act == 0) break;
				if (act == 3) { x = wx1 + (mlen > 0 ? 1 : 1); continue; }
				if (act == 2) replace_all = 1;
			}

			wchar_t wr[1024];
			size_t rlen = mbstowcs(wr, repl_utf, 1023);
			if (rlen == (size_t)-1) rlen = 0;
			wr[rlen] = L'\0';

			if (!saved_undo) { ed_push_undo(ed); saved_undo = 1; }
			ed_replace_match(ed, y, wx1, mlen, wr, (int)rlen);
			count++;
			if (mlen == 0 && rlen == 0) x = wx1 + 1;
			else if (mlen == 0) { x = wx1 + (int)rlen + 1; }
			else x = wx1 + (int)rlen;
		}
		regfree(&re);
		ed_clear_sel(ed);
		return count;
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

void ui_edit_file_ex(const char *filepath, int start_line, int start_col, int *out_line, int *out_col) {
	EditorState ed;
	memset(&ed, 0, sizeof(ed));
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
		refresh();

		InputEvent ev = input_read();
		if (ev.action == ACT_NONE) continue;
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
				int col_w = width / 10; if (col_w < 3) col_w = 3;
				int idx = ev.mouse_x / col_w; if (idx > 9) idx = 9;
				const KeyAction fk[10] = {
					ACT_F1, ACT_F2, ACT_F3, ACT_F4, ACT_F5,
					ACT_F6, ACT_F7, ACT_F8, ACT_F9, ACT_F10
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
				ed.cur_x = cx;
				ed_clear_sel(&ed);
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

		if (ev.action == ACT_F1) {
			curs_set(0);
			ui_editor_help();
		} else if (ev.action == ACT_F2) {
			ed_save_file(&ed);
		} else if (ev.action == ACT_F4) {
			if (!ed.syntax) ed.syntax = &SYNTAX_LANGS[0];
			ed.syntax_enabled = !ed.syntax_enabled;
		} else if (ev.action == ACT_F3) {
			if (!ed.sel_active) {
				ed.sel_active = 1;
				ed.sel_start_y = ed.cur_y; ed.sel_start_x = ed.cur_x;
				ed.sel_end_y = ed.cur_y;   ed.sel_end_x = ed.cur_x;
			} else {
				ed_clear_sel(&ed);
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
		} else if (ev.action == ACT_F6 || ev.action == ACT_CTRL_X) {
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
		} else if (ev.action == ACT_F8) {
			ed_push_undo(&ed);
			if (ed.sel_active) ed_delete_selection(&ed);
			else ed_delete_char(&ed);
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
				ed.sel_active = 1;
				int forward = (m_y1 < m_y2 || (m_y1 == m_y2 && m_x1 <= m_x2));
				if (forward) {
					ed.sel_start_y = m_y1; ed.sel_start_x = m_x1;
					ed.sel_end_y = m_y2;   ed.sel_end_x = m_x2 + 1;
					ed.cur_y = m_y2;       ed.cur_x = m_x2 + 1;
				} else {
					ed.sel_start_y = m_y2; ed.sel_start_x = m_x2;
					ed.sel_end_y = m_y1;   ed.sel_end_x = m_x1 + 1;
					ed.cur_y = m_y1;       ed.cur_x = m_x1 + 1;
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
			ed_clear_sel(&ed);
			if (ed.cur_y > 0) ed.cur_y--;
		} else if (ev.action == ACT_DOWN) {
			ed_clear_sel(&ed);
			if (ed.cur_y + 1 < ed.line_count) ed.cur_y++;
		} else if (ev.action == ACT_LEFT) {
			ed_clear_sel(&ed);
			if (ed.cur_x > 0) ed.cur_x--;
			else if (ed.cur_y > 0) { ed.cur_y--; ed.cur_x = ed.lines[ed.cur_y].len; }
		} else if (ev.action == ACT_RIGHT) {
			ed_clear_sel(&ed);
			if (ed.cur_x < ed.lines[ed.cur_y].len) ed.cur_x++;
			else if (ed.cur_y + 1 < ed.line_count) { ed.cur_y++; ed.cur_x = 0; }
		} else if (ev.action == ACT_CTRL_LEFT) {
			ed_clear_sel(&ed);
			ed_move_word_left(&ed);
		} else if (ev.action == ACT_CTRL_RIGHT) {
			ed_clear_sel(&ed);
			ed_move_word_right(&ed);
		} else if (ev.action == ACT_HOME) {
			ed_clear_sel(&ed);
			ed.cur_x = 0;
		} else if (ev.action == ACT_END) {
			ed_clear_sel(&ed);
			ed.cur_x = ed.lines[ed.cur_y].len;
		} else if (ev.action == ACT_CTRL_HOME) {
			ed_clear_sel(&ed);
			ed.cur_y = 0;
			ed.cur_x = 0;
		} else if (ev.action == ACT_CTRL_END) {
			ed_clear_sel(&ed);
			ed.cur_y = ed.line_count - 1;
			ed.cur_x = ed.lines[ed.cur_y].len;
		} else if (ev.action == ACT_PGUP) {
			ed_clear_sel(&ed);
			ed.cur_y -= vis_rows; if (ed.cur_y < 0) ed.cur_y = 0;
		} else if (ev.action == ACT_PGDN) {
			ed_clear_sel(&ed);
			ed.cur_y += vis_rows; if (ed.cur_y >= ed.line_count) ed.cur_y = ed.line_count - 1;
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
	curs_set(0);
	ed_free(&ed);
	touchwin(stdscr);
}
