#define _GNU_SOURCE
#include <fnmatch.h>
#include <sys/inotify.h>
#include <unistd.h>
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <strings.h>
#include <sys/stat.h>
#include "panel.h"

static SortMode current_sort_mode = SORT_NAME;
static int current_sort_desc = 0;

static int compare_entries(const void *a, const void *b) {
	const FileEntry *fa = (const FileEntry *)a;
	const FileEntry *fb = (const FileEntry *)b;

	// Родительская директория ".." всегда выше всех
	if (strcmp(fa->name, "..") == 0) return -1;
	if (strcmp(fb->name, "..") == 0) return 1;

	// Директории всегда сверху файлов
	if (fa->is_dir && !fb->is_dir) return -1;
	if (!fa->is_dir && fb->is_dir) return 1;

	int res = 0;

	switch (current_sort_mode) {
		case SORT_NAME: {
			res = strcasecmp(fa->name, fb->name);
			if (res == 0) res = strcmp(fa->name, fb->name);
			break;
		}
		case SORT_EXT: {
			const char *ext_a = strrchr(fa->name, '.');
			const char *ext_b = strrchr(fb->name, '.');
			ext_a = ext_a ? ext_a + 1 : "";
			ext_b = ext_b ? ext_b + 1 : "";
			res = strcasecmp(ext_a, ext_b);
			if (res == 0) {
				res = strcasecmp(fa->name, fb->name);
			}
			break;
		}
		case SORT_SIZE: {
			if (fa->size < fb->size) res = -1;
			else if (fa->size > fb->size) res = 1;
			else res = strcasecmp(fa->name, fb->name);
			break;
		}
		case SORT_TIME: {
			if (fa->mtime < fb->mtime) res = -1;
			else if (fa->mtime > fb->mtime) res = 1;
			else res = strcasecmp(fa->name, fb->name);
			break;
		}
		case SORT_NONE:
		default:
			res = 0;
			break;
	}

	return current_sort_desc ? -res : res;
}int g_inotify_fd = -1;

static void panel_watch_update(Panel *p) {
	if (!p) return;
	if (g_inotify_fd < 0) {
		g_inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	}
	if (g_inotify_fd < 0) return;
	if (p->watch_fd >= 0) {
		inotify_rm_watch(g_inotify_fd, p->watch_fd);
		p->watch_fd = -1;
	}
	uint32_t mask = IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO |
	                IN_ATTRIB | IN_MODIFY | IN_DELETE_SELF | IN_MOVE_SELF;
	p->watch_fd = inotify_add_watch(g_inotify_fd, p->current_path, mask);
}

void load_directory(Panel *p) {
	char marked_names[MAX_FILES][MAX_FILENAME];
	int marked_cnt = 0;
	for (int i = 0; i < p->count && marked_cnt < MAX_FILES; i++) {
		if (p->files[i].is_selected && strcmp(p->files[i].name, "..") != 0) {
			snprintf(marked_names[marked_cnt++], MAX_FILENAME, "%s", p->files[i].name);
		}
	}

	char resolved_path[MAX_PATH];
	if (access(p->current_path, F_OK) != 0 && realpath(p->current_path, resolved_path) != NULL) {
		snprintf(p->current_path, MAX_PATH, "%s", resolved_path);
	}

	DIR *d = opendir(p->current_path);
	if (!d) {
		strcpy(p->current_path, "/");
		d = opendir("/");
		if (!d) return;
	}

	struct dirent *dir;
	p->count = 0;

	if (strcmp(p->current_path, "/") != 0) {
		strcpy(p->files[p->count].name, "..");
		p->files[p->count].is_dir = 1;
		p->files[p->count].is_exec = 0;
		p->files[p->count].is_selected = 0;
		p->count++;
	}

	while ((dir = readdir(d)) != NULL && p->count < MAX_FILES) {
		if (strcmp(dir->d_name, ".") == 0 || strcmp(dir->d_name, "..") == 0) continue;

		snprintf(p->files[p->count].name, MAX_FILENAME, "%s", dir->d_name);
		p->files[p->count].is_selected = 0;

		struct stat st;
		char full_path[MAX_PATH * 2];
		snprintf(full_path, sizeof(full_path), "%s/%s", p->current_path, dir->d_name);

		p->files[p->count].is_dir = 0;
		p->files[p->count].is_exec = 0;
		p->files[p->count].size = 0;
		p->files[p->count].mode = 0;
		p->files[p->count].mtime = 0;
		if (stat(full_path, &st) == 0) {
			p->files[p->count].size = st.st_size;
			p->files[p->count].mode = st.st_mode;
			p->files[p->count].mtime = st.st_mtime;
			if (S_ISDIR(st.st_mode)) {
				p->files[p->count].is_dir = 1;
			} else if (st.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) {
				p->files[p->count].is_exec = 1;
			}
		}
		p->count++;
	}
	closedir(d);

		current_sort_mode = p->sort_mode;
	current_sort_desc = p->sort_desc;
	qsort(p->files, p->count, sizeof(FileEntry), compare_entries);

	int prev_sel = p->selected;
	p->selected = 0;
	p->top_line = 0;

	if (p->parent_name[0] != '\0') {
		int found = 0;
		for (int i = 0; i < p->count; i++) {
			if (strcmp(p->files[i].name, p->parent_name) == 0) {
				p->selected = i;
				found = 1;
				break;
			}
		}
		if (!found && p->count > 0) {
			p->selected = (prev_sel < p->count) ? prev_sel : (p->count - 1);
		}
		p->parent_name[0] = '\0';
	}

	if (marked_cnt > 0) {
		for (int i = 0; i < p->count; i++) {
			for (int m = 0; m < marked_cnt; m++) {
				if (strcmp(p->files[i].name, marked_names[m]) == 0) {
					p->files[i].is_selected = 1;
					break;
				}
			}
		}
	}

	panel_watch_update(p);
}

void change_directory(Panel *p, const char *dir) {
	if (!p || !dir || !*dir) return;

	char next_path[MAX_PATH];
	if (strcmp(dir, "..") == 0) {
		if (strcmp(p->current_path, "/") == 0) return;
		char tmp[MAX_PATH];
		snprintf(tmp, sizeof(tmp), "%s", p->current_path);
		size_t len = strlen(tmp);
		while (len > 1 && tmp[len - 1] == '/') tmp[--len] = '\0';
		char *sl = strrchr(tmp, '/');
		if (!sl) return;
		snprintf(p->parent_name, MAX_FILENAME, "%s", sl + 1);
		if (sl == tmp) {
			snprintf(next_path, sizeof(next_path), "/");
		} else {
			*sl = '\0';
			snprintf(next_path, sizeof(next_path), "%s", tmp);
		}
	} else if (dir[0] == '/') {
		snprintf(next_path, sizeof(next_path), "%s", dir);
		p->parent_name[0] = '\0';
	} else {
		if (strcmp(p->current_path, "/") == 0) {
			snprintf(next_path, sizeof(next_path), "/%s", dir);
		} else {
			snprintf(next_path, sizeof(next_path), "%s/%s", p->current_path, dir);
		}
		p->parent_name[0] = '\0';
	}

	struct stat st;
	if (stat(next_path, &st) == 0 && S_ISDIR(st.st_mode) && access(next_path, R_OK | X_OK) == 0) {
		snprintf(p->current_path, MAX_PATH, "%s", next_path);
		load_directory(p);
	}
}

int panel_marked_count(const Panel *p) {
	int marked = 0;
	if (!p) return 0;
	for (int i = 0; i < p->count; i++) {
		if (p->files[i].is_selected && strcmp(p->files[i].name, "..") != 0) {
			marked++;
		}
	}
	return marked;
}

void panel_toggle_mark(Panel *p, int index) {
	if (!p || index < 0 || index >= p->count) return;
	if (strcmp(p->files[index].name, "..") == 0) return;
	p->files[index].is_selected = !p->files[index].is_selected;
}

void panel_select_mask(Panel *p, const char *mask, int select_mode) {
	if (!p || !mask || !*mask) return;
	for (int i = 0; i < p->count; i++) {
		if (strcmp(p->files[i].name, "..") == 0) continue;
		if (fnmatch(mask, p->files[i].name, FNM_CASEFOLD) == 0) {
			p->files[i].is_selected = select_mode ? 1 : 0;
		}
	}
}

void panel_invert_selection(Panel *p) {
	if (!p) return;
	for (int i = 0; i < p->count; i++) {
		if (strcmp(p->files[i].name, "..") == 0) continue;
		p->files[i].is_selected = !p->files[i].is_selected;
	}
}
