#define _GNU_SOURCE
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <strings.h>
#include <sys/stat.h>
#include "panel.h"

static int compare_entries(const void *a, const void *b) {
	const FileEntry *fa = (const FileEntry *)a;
	const FileEntry *fb = (const FileEntry *)b;

	if (strcmp(fa->name, "..") == 0) return -1;
	if (strcmp(fb->name, "..") == 0) return 1;

	if (fa->is_dir != fb->is_dir) {
		return fb->is_dir - fa->is_dir;
	}

	int res = strcasecmp(fa->name, fb->name);
	if (res != 0) return res;
	return strcmp(fa->name, fb->name);
}

void load_directory(Panel *p) {
	char resolved_path[MAX_PATH];
	if (realpath(p->current_path, resolved_path) != NULL) {
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
		if (stat(full_path, &st) == 0) {
			if (S_ISDIR(st.st_mode)) {
				p->files[p->count].is_dir = 1;
			} else if (st.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) {
				p->files[p->count].is_exec = 1;
			}
		}
		p->count++;
	}
	closedir(d);

	qsort(p->files, p->count, sizeof(FileEntry), compare_entries);

	p->selected = 0;
	p->top_line = 0;

	if (p->parent_name[0] != '\0') {
		for (int i = 0; i < p->count; i++) {
			if (strcmp(p->files[i].name, p->parent_name) == 0) {
				p->selected = i;
				break;
			}
		}
		p->parent_name[0] = '\0';
	}
}

void change_directory(Panel *p, const char *new_dir) {
	char new_path[MAX_PATH];

	if (strcmp(new_dir, "..") == 0) {
		char *last_slash = strrchr(p->current_path, '/');
		if (last_slash != NULL) {
			snprintf(p->parent_name, MAX_FILENAME, "%s", last_slash + 1);

			if (last_slash == p->current_path) {
				strcpy(new_path, "/");
			} else {
				size_t len = (size_t)(last_slash - p->current_path);
				if (len >= MAX_PATH) len = MAX_PATH - 1;
				memcpy(new_path, p->current_path, len);
				new_path[len] = '\0';
			}
		} else {
			strcpy(new_path, "/");
		}
	} else {
		if (strcmp(p->current_path, "/") == 0) {
			snprintf(new_path, sizeof(new_path), "/%s", new_dir);
		} else {
			snprintf(new_path, sizeof(new_path), "%s/%s", p->current_path, new_dir);
		}
	}

	snprintf(p->current_path, MAX_PATH, "%s", new_path);
	load_directory(p);
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
