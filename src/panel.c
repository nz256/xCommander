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
#include "archive.h"

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

static char g_temp_paths[MAX_FILES][MAX_PATH];
static int g_temp_count = 0;

static void temp_panel_load_entries(Panel *p) {
	p->count = 0;
	strcpy(p->files[0].name, "..");
	p->files[0].is_dir = 1;
	p->files[0].is_exec = 0;
	p->files[0].is_selected = 0;
	p->files[0].size = 0;
	p->files[0].mode = 0;
	p->files[0].mtime = 0;
	p->files[0].full_path[0] = '\0';
	p->count = 1;

	int w = 0;
	for (int i = 0; i < g_temp_count && p->count < MAX_FILES; i++) {
		struct stat st;
		if (lstat(g_temp_paths[i], &st) != 0) continue;
		if (w != i) snprintf(g_temp_paths[w], MAX_PATH, "%s", g_temp_paths[i]);
		const char *base = strrchr(g_temp_paths[w], '/');
		base = (base && base[1]) ? (base + 1) : g_temp_paths[w];
		snprintf(p->files[p->count].name, MAX_FILENAME, "%s", base);
		snprintf(p->files[p->count].full_path, MAX_PATH, "%s", g_temp_paths[w]);
		p->files[p->count].is_selected = 0;
		p->files[p->count].is_dir = S_ISDIR(st.st_mode) ? 1 : 0;
		p->files[p->count].is_exec = (!S_ISDIR(st.st_mode) && (st.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH))) ? 1 : 0;
		p->files[p->count].size = st.st_size;
		p->files[p->count].mode = st.st_mode;
		p->files[p->count].mtime = st.st_mtime;
		p->count++;
		w++;
	}
	g_temp_count = w;

	current_sort_mode = p->sort_mode;
	current_sort_desc = p->sort_desc;
	qsort(p->files, (size_t)p->count, sizeof(FileEntry), compare_entries);
	if (p->selected >= p->count) p->selected = (p->count > 0) ? (p->count - 1) : 0;
	if (p->selected < 0) p->selected = 0;
}

void temp_panel_open(Panel *p) {
	if (!p) return;
	p->archive_file[0] = '\0';
	p->archive_dir[0] = '\0';
	p->is_temp = 1;
	p->selected = 0;
	p->top_line = 0;
	temp_panel_load_entries(p);
}

int temp_panel_add(const char *full_path) {
	if (!full_path || !full_path[0] || g_temp_count >= MAX_FILES - 1) return 0;
	for (int i = 0; i < g_temp_count; i++) {
		if (strcmp(g_temp_paths[i], full_path) == 0) return 0;
	}
	snprintf(g_temp_paths[g_temp_count++], MAX_PATH, "%s", full_path);
	return 1;
}

void temp_panel_clear(void) {
	g_temp_count = 0;
}

int temp_panel_remove_selected(Panel *p) {
	if (!p || !p->is_temp || p->count == 0) return 0;
	int marked = panel_marked_count(p);
	int changed = 0;
	for (int i = 0; i < p->count; i++) {
		if (strcmp(p->files[i].name, "..") == 0) continue;
		if ((marked > 0 && p->files[i].is_selected) || (marked == 0 && i == p->selected)) {
			for (int j = 0; j < g_temp_count; j++) {
				if (strcmp(g_temp_paths[j], p->files[i].full_path) == 0) {
					memmove(&g_temp_paths[j], &g_temp_paths[j + 1], (size_t)(g_temp_count - j - 1) * MAX_PATH);
					g_temp_count--;
					changed = 1;
					break;
				}
			}
		}
	}
	if (changed) temp_panel_load_entries(p);
	return changed;
}

void panel_entry_full_path(const Panel *p, int index, char *out, size_t sz) {
	if (!p || index < 0 || index >= p->count || !out || sz == 0) return;
	if (p->is_temp && p->files[index].full_path[0] != '\0') {
		snprintf(out, sz, "%s", p->files[index].full_path);
	} else if (strcmp(p->current_path, "/") == 0) {
		snprintf(out, sz, "/%s", p->files[index].name);
	} else {
		snprintf(out, sz, "%s/%s", p->current_path, p->files[index].name);
	}
}

void load_directory(Panel *p) {
	if (p && p->is_temp) {
		temp_panel_load_entries(p);
		return;
	}
	if (p && p->archive_file[0] != '\0') {
		if (access(p->archive_file, R_OK) == 0 && archive_load_dir(p)) return;
		p->archive_file[0] = '\0';
		p->archive_dir[0] = '\0';
	}
	static Panel *tracked_p[4] = {0};
	static char tracked_dir[4][MAX_PATH] = {{0}};
	int slot = -1;
	for (int k = 0; k < 4; k++) {
		if (tracked_p[k] == p) { slot = k; break; }
		if (slot < 0 && tracked_p[k] == NULL) slot = k;
	}
	int same_dir = 0;
	if (slot >= 0 && tracked_p[slot] == p && strcmp(tracked_dir[slot], p->current_path) == 0) {
		same_dir = 1;
	}
	if (slot >= 0) {
		tracked_p[slot] = p;
		snprintf(tracked_dir[slot], MAX_PATH, "%s", p->current_path);
	}

	char marked_names[MAX_FILES][MAX_FILENAME];
	int marked_cnt = 0;
	if (same_dir) {
		for (int i = 0; i < p->count && marked_cnt < MAX_FILES; i++) {
			if (p->files[i].is_selected && strcmp(p->files[i].name, "..") != 0) {
				snprintf(marked_names[marked_cnt++], MAX_FILENAME, "%s", p->files[i].name);
			}
		}
	}
	char sized_names[MAX_FILES][MAX_FILENAME];
	off_t sized_vals[MAX_FILES];
	int sized_cnt = 0;
	if (same_dir) {
		for (int i = 0; i < p->count && sized_cnt < MAX_FILES; i++) {
			if (p->files[i].is_dir && p->files[i].dir_size_known && strcmp(p->files[i].name, "..") != 0) {
				snprintf(sized_names[sized_cnt], MAX_FILENAME, "%s", p->files[i].name);
				sized_vals[sized_cnt] = p->files[i].size;
				sized_cnt++;
			}
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
		p->files[p->count].full_path[0] = '\0';
		p->files[p->count].is_selected = 0;

		struct stat st;
		char full_path[MAX_PATH * 2];
		snprintf(full_path, sizeof(full_path), "%s/%s", p->current_path, dir->d_name);

		p->files[p->count].is_dir = 0;
		p->files[p->count].dir_size_known = 0;
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
	if (sized_cnt > 0) {
		for (int i = 0; i < p->count; i++) {
			if (!p->files[i].is_dir) continue;
			for (int m = 0; m < sized_cnt; m++) {
				if (strcmp(p->files[i].name, sized_names[m]) == 0) {
					p->files[i].size = sized_vals[m];
					p->files[i].dir_size_known = 1;
					break;
				}
			}
		}
	}

	panel_watch_update(p);
}

void change_directory(Panel *p, const char *dir) {
	if (!p || !dir || !*dir) return;

	if (p->is_temp) {
		if (strcmp(dir, "..") == 0) {
			p->is_temp = 0;
			load_directory(p);
			return;
		}
		for (int i = 0; i < p->count; i++) {
			if (strcmp(p->files[i].name, dir) == 0 && p->files[i].full_path[0] != '\0') {
				p->is_temp = 0;
				snprintf(p->current_path, MAX_PATH, "%s", p->files[i].full_path);
				p->parent_name[0] = '\0';
				load_directory(p);
				return;
			}
		}
		return;
	}

	if (p->archive_file[0] != '\0') {
		if (strcmp(dir, "..") == 0) {
			if (p->archive_dir[0] == '\0') {
				const char *abase = strrchr(p->archive_file, '/');
				abase = abase ? (abase + 1) : p->archive_file;
				snprintf(p->parent_name, MAX_FILENAME, "%s", abase);
				p->archive_file[0] = '\0';
				p->archive_dir[0] = '\0';
				load_directory(p);
			} else {
				char *sl = strrchr(p->archive_dir, '/');
				if (sl) {
					snprintf(p->parent_name, MAX_FILENAME, "%s", sl + 1);
					*sl = '\0';
				} else {
					snprintf(p->parent_name, MAX_FILENAME, "%s", p->archive_dir);
					p->archive_dir[0] = '\0';
				}
				archive_load_dir(p);
			}
			return;
		}
		char next_sub[MAX_PATH];
		if (p->archive_dir[0] != '\0') {
			snprintf(next_sub, sizeof(next_sub), "%s/%s", p->archive_dir, dir);
		} else {
			snprintf(next_sub, sizeof(next_sub), "%s", dir);
		}
		snprintf(p->archive_dir, MAX_PATH, "%s", next_sub);
		p->parent_name[0] = '\0';
		archive_load_dir(p);
		return;
	}

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
