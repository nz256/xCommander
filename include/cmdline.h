#ifndef CMDLINE_H
#define CMDLINE_H

#include <wchar.h>
#include "input.h"
#include "xc.h"

#define CMD_MAX_LEN 16384

extern wchar_t cmd_wbuf[CMD_MAX_LEN];
extern int cmd_len;
extern int cmd_pos;
extern int cmd_scroll;
extern int cmd_sel_start;
extern int cmd_sel_end;

void cmdline_init(void);
void cmdline_clear(void);
void cmdline_set_text(const char *utf8);
void cmdline_get_utf8(char *out, size_t out_size);
void cmdline_insert_wstr(const wchar_t *wstr);
void cmdline_insert_path(const Panel *p);
void cmdline_insert_filename(const Panel *p);
void cmdline_copy(const Panel *p, int terminal_mode);
void cmdline_cut(void);
void cmdline_paste(void);
int  cmdline_handle_edit_key(InputEvent ev, int terminal_mode);

#endif
