#ifndef XC_H
#define XC_H

#define MAX_FILES 1024
#define MAX_FILENAME 256
#define MAX_PATH 4096
#define MAX_CMD 1024

typedef struct {
	char name[MAX_FILENAME];
	int is_dir;
	int is_exec;
	int is_selected;
} FileEntry;

typedef struct {
	FileEntry files[MAX_FILES];
	int count;
	int selected;
	int top_line;
	char current_path[MAX_PATH];
	char parent_name[MAX_FILENAME];
} Panel;

extern Panel left_panel;
extern Panel right_panel;
extern Panel *active_panel;

extern char cmd_buffer[MAX_CMD];
extern int cmd_pos;
extern int cmd_sel_start;
extern int cmd_sel_end;
extern int terminal_mode;

#endif
