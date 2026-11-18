#ifndef EDITOR_H
#define EDITOR_H

void ui_edit_file(const char *filepath);
void ui_edit_file_at(const char *filepath, int start_line, int start_col);
void ui_edit_file_ex(const char *filepath, int start_line, int start_col, int *out_line, int *out_col);

#endif
