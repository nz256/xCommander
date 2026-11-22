#ifndef EXEC_H
#define EXEC_H

#include "xc.h"

void execute_system_command(const char *cmd);
void view_file(const char *filename);
void edit_file(const char *filename);
int create_and_edit_file(Panel *panel);
int copy_items(const Panel *src_panel, const Panel *dst_panel);
int move_items(const Panel *src_panel, const Panel *dst_panel);
int copy_items_inplace(const Panel *src_panel, const Panel *dst_panel);
int move_items_inplace(const Panel *src_panel, const Panel *dst_panel);
int make_directory(Panel *panel);
int trash_items(const Panel *panel);
int delete_items(const Panel *panel);
int apply_command_to_items(const Panel *panel);
int apply_command_to_selected(Panel *panel);

void clipboard_copy(const char *text);
size_t clipboard_paste(char *out, size_t out_size);
size_t clipboard_paste_raw(char *out, size_t out_size);

void clipboard_stage_files(const Panel *panel, int is_cut);
int clipboard_has_files(void);
int clipboard_paste_files(Panel *dst_panel);

#endif
