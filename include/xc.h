#include <sys/types.h>
#ifndef XC_H
#define XC_H

#define MAX_FILES 1024
#define MAX_FILENAME 256
#define MAX_PATH 4096
#define MAX_CMD 1024

typedef enum {
	SORT_NAME = 0,
	SORT_EXT,
	SORT_SIZE,
	SORT_TIME,
	SORT_NONE
} SortMode;

typedef struct {
	char name[MAX_FILENAME];
	int is_dir;
	int is_exec;
	int is_selected;
	off_t size;
	mode_t mode;
	time_t mtime;
} FileEntry;

typedef struct {
	FileEntry files[MAX_FILES];
	int count;
	int selected;
	int top_line;
	char current_path[MAX_PATH];
	char parent_name[MAX_FILENAME];
	SortMode sort_mode;
	int sort_desc;
	int watch_fd;
} Panel;

extern Panel left_panel;
extern Panel right_panel;
extern Panel *active_panel;

extern char cmd_buffer[MAX_CMD];
extern int cmd_pos;
extern int cmd_sel_start;
extern int cmd_sel_end;
extern int terminal_mode;
extern char quick_search[MAX_FILENAME];
extern int quick_search_active;
extern int quick_search_nomatch;

#endif
