#ifndef UI_H
#define UI_H

#include "xc.h"

void ui_init(void);
void print_term_prompt_ex(const wchar_t *wcmd, int with_header);
void draw_panel(Panel *p, int start_y, int start_x, int width, int height, int is_active);
void draw_fkey_bar(int y, int width);
void draw_screen(void);
int ui_confirm(const char *title, const char *msg);
int ui_confirm_cancel(const char *title, const char *msg);
int ui_multiline_paste(const char *text);
void ml_history_add(const char *dir, const char *script);
int ui_multiline_history(char *out_script, size_t out_size);
int ui_cmd_history(CmdHistoryEntry *hist, int *hist_count, char *out_cmd, size_t out_size, char *out_dir, size_t dir_size);
int ui_dir_history(DirHistoryEntry *hist, int *hist_count, char *out_dir, size_t dir_size);
int ui_file_history(FileHistoryEntry *hist, int *hist_count, char *out_path, size_t path_size, int *out_line, int *out_col);
void ui_message(const char *title, const char *msg);
int ui_input(const char *title, const char *prompt, char *buf, int max_len);
int ui_input_narrow(const char *title, const char *prompt, char *buf, int max_len);
int ui_search_dialog(char *search_buf, int s_max, int *case_sens, int *use_regex);
int ui_replace_dialog(char *search_buf, int s_max, char *replace_buf, int r_max, int *case_sens, int *use_regex);
int ui_replace_prompt(const char *search_str, const char *replace_str, int avoid_y);
int ui_overwrite_prompt(const char *filename, off_t src_size, time_t src_mtime, off_t dst_size, time_t dst_mtime);
void ui_help(void);
void ui_editor_help(void);
void ui_user_menu(void);
void ui_drive_menu(Panel *target_panel, int is_right);
int ui_file_attributes(Panel *panel);
void panel_calc_dir_sizes(Panel *panel);


#define CMD_SUGGEST_MAX 6
extern CmdHistoryEntry cmd_history[MAX_HISTORY];
extern int history_count;
extern int cmd_suggest_idx[CMD_SUGGEST_MAX];
extern int cmd_suggest_count;
extern int cmd_suggest_sel;
extern int cmd_suggest_dismissed;
extern int cmd_suggest_y, cmd_suggest_x, cmd_suggest_w, cmd_suggest_h;
void cmd_suggest_reset(void);

#endif

extern int g_auto_colorize;
void ui_toggle_auto_colorize(void);
