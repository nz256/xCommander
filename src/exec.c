#define _DEFAULT_SOURCE
#include <ncurses.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <termios.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include "xc.h"
#include "exec.h"
#include "panel.h"
#include "ui.h"
#include "widgets.h"
#include "sudo_helper.h"
#include "progress.h"
static UiProgress *g_prog = NULL;
#include "input.h"
#include "editor.h"

static void build_full_path(char *out, size_t out_size, const char *dir, const char *name) {
	if (strcmp(dir, "/") == 0) {
		snprintf(out, out_size, "/%s", name);
	} else {
		snprintf(out, out_size, "%s/%s", dir, name);
	}
}

static int g_overwrite_all = 0;

static int check_overwrite(const char *src_path, const char *dst_path) {
	struct stat dst_st;
	if (lstat(dst_path, &dst_st) != 0) return 1;
	if (S_ISDIR(dst_st.st_mode)) return 1;
	if (g_overwrite_all) return 1;

	struct stat src_st;
	memset(&src_st, 0, sizeof(src_st));
	lstat(src_path, &src_st);

	const char *base = strrchr(dst_path, '/');
	base = base ? (base + 1) : dst_path;

	int act = ui_overwrite_prompt(base, src_st.st_size, src_st.st_mtime, dst_st.st_size, dst_st.st_mtime);
	if (act == 2) {
		g_overwrite_all = 1;
		return 1;
	}
	if (act == 1) return 1;
	if (act == 3) return 0;
	return -1;
}

static int copy_single_file(const char *src_path, const char *dst_path, mode_t mode) {
	int ovr = check_overwrite(src_path, dst_path);
	if (ovr == 0) { if (g_prog) g_prog->done_files++; return 0; }
	if (ovr < 0) return -2;

	struct stat st;
	off_t fsize = (lstat(src_path, &st) == 0) ? st.st_size : 0;
	off_t cur_done = 0;
	if (g_prog && progress_update(g_prog, src_path, dst_path, 0, fsize, 1)) return -2;

	FILE *in = fopen(src_path, "rb");
	if (!in) return -1;

	FILE *out = fopen(dst_path, "wb");
	if (!out) {
		fclose(in);
		return -1;
	}

	char buf[65536];
	size_t n;
	int err = 0;

	while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
		if (fwrite(buf, 1, n, out) != n) {
			err = -1;
			break;
		}
		cur_done += (off_t)n;
		if (g_prog) {
			g_prog->done_bytes += (off_t)n;
			if (progress_update(g_prog, src_path, dst_path, cur_done, fsize, 0)) { err = -2; break; }
		}
	}

	if (err == 0 && ferror(in)) err = -1;

	fclose(in);
	if (fclose(out) != 0 && err == 0) err = -1;

	if (err == 0) {
		chmod(dst_path, mode);
		if (g_prog) g_prog->done_files++;
	} else if (err == -2) {
		unlink(dst_path);
	}
	return err;
}

static char g_copy_err[MAX_PATH + 128] = "";

static int copy_recursive(const char *src_path, const char *dst_path) {
	struct stat st;
	if (lstat(src_path, &st) != 0) {
		snprintf(g_copy_err, sizeof(g_copy_err), "%s: %s", src_path, strerror(errno));
		return -1;
	}

	if (S_ISLNK(st.st_mode)) {
		char lnk[MAX_PATH];
		ssize_t len = readlink(src_path, lnk, sizeof(lnk) - 1);
		if (len < 0) {
			snprintf(g_copy_err, sizeof(g_copy_err), "%s: %s", src_path, strerror(errno));
			return -1;
		}
		lnk[len] = '\0';
		unlink(dst_path);
		if (symlink(lnk, dst_path) != 0) {
			snprintf(g_copy_err, sizeof(g_copy_err), "%s: %s", dst_path, strerror(errno));
			return -1;
		}
		if (g_prog) {
			g_prog->done_files++;
			if (progress_update(g_prog, src_path, dst_path, 0, 0, 0)) return -2;
		}
		return 0;
	}

	if (S_ISDIR(st.st_mode)) {
		if (mkdir(dst_path, (st.st_mode & 0777) | 0700) != 0 && errno != EEXIST) {
			snprintf(g_copy_err, sizeof(g_copy_err), "%s: %s", dst_path, strerror(errno));
			return -1;
		}
		chmod(dst_path, (st.st_mode & 0777) | 0700);

		DIR *d = opendir(src_path);
		if (!d) {
			snprintf(g_copy_err, sizeof(g_copy_err), "%s: %s", src_path, strerror(errno));
			return -1;
		}

		struct dirent *entry;
		while ((entry = readdir(d)) != NULL) {
			if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;

			char next_src[MAX_PATH * 2];
			char next_dst[MAX_PATH * 2];
			snprintf(next_src, sizeof(next_src), "%s/%s", src_path, entry->d_name);
			snprintf(next_dst, sizeof(next_dst), "%s/%s", dst_path, entry->d_name);

			int rc = copy_recursive(next_src, next_dst);
			if (rc != 0) {
				closedir(d);
				return rc;
			}
		}
		closedir(d);
		chmod(dst_path, st.st_mode & 07777);
		if (g_prog) g_prog->done_files++;
		return 0;
	} else {
		int rc = copy_single_file(src_path, dst_path, st.st_mode & 0777);
		if (rc == -1 && g_copy_err[0] == '\0') {
			snprintf(g_copy_err, sizeof(g_copy_err), "%s: %s", src_path, strerror(errno));
		}
		return rc;
	}
}

static int delete_recursive(const char *target_path) {
	struct stat st;
	if (lstat(target_path, &st) != 0) return -1;

	if (S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode)) {
		DIR *d = opendir(target_path);
		if (!d) return -1;

		struct dirent *entry;
		while ((entry = readdir(d)) != NULL) {
			if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
				continue;
			}

			char child[MAX_PATH * 2];
			snprintf(child, sizeof(child), "%s/%s", target_path, entry->d_name);

			if (delete_recursive(child) != 0) {
				closedir(d);
				return -1;
			}
		}
		closedir(d);
		if (g_prog) {
			g_prog->done_files++;
			if (progress_update(g_prog, target_path, NULL, 0, 0, 0)) return -2;
		}
		return rmdir(target_path);
	} else {
		if (g_prog) {
			g_prog->done_files++;
			if (progress_update(g_prog, target_path, NULL, 0, 0, 0)) return -2;
		}
		return unlink(target_path);
	}
}

static int move_single_path(const char *src_full, const char *dst_full) {
	if (strcmp(src_full, dst_full) == 0) return 0;

	struct stat st_src;
	if (lstat(src_full, &st_src) == 0 && !S_ISDIR(st_src.st_mode)) {
		int ovr = check_overwrite(src_full, dst_full);
		if (ovr == 0) return 0;
		if (ovr < 0) return -2;
	}

	if (rename(src_full, dst_full) != 0) {
		if (errno == EXDEV) {
			if (copy_recursive(src_full, dst_full) != 0 || delete_recursive(src_full) != 0) {
				return -1;
			}
		} else {
			return -1;
		}
	}
	return 0;
}

int create_and_edit_file(Panel *panel) {
	if (!panel) return 0;

	char filename[MAX_FILENAME] = "";
	if (!ui_input_newfile("Create File (Shift+F4)", "Enter new file name:", filename, sizeof(filename))) {
		return 0;
	}
	if (strlen(filename) == 0) return 0;

	char full_path[MAX_PATH * 2];
	if (filename[0] == '/') {
		snprintf(full_path, sizeof(full_path), "%s", filename);
	} else {
		build_full_path(full_path, sizeof(full_path), panel->current_path, filename);
	}

	ui_edit_file(full_path);

	struct stat st;
	if (stat(full_path, &st) == 0) {
		const char *base = strrchr(filename, '/');
		base = base ? (base + 1) : filename;
		snprintf(panel->parent_name, MAX_FILENAME, "%s", base);
		return 1;
	}
	return 0;
}

static void resolve_dest_path(const Panel *src_panel, char *dest, size_t dest_sz) {
	if (!dest || dest[0] == '\0') return;
	char tmp[MAX_PATH * 2];
	if (dest[0] == '~' && (dest[1] == '/' || dest[1] == '\0')) {
		const char *home = getenv("HOME");
		if (home && *home) {
			snprintf(tmp, sizeof(tmp), "%s%s", home, dest + 1);
			snprintf(dest, dest_sz, "%s", tmp);
			return;
		}
	}
	if (dest[0] != '/' && src_panel) {
		build_full_path(tmp, sizeof(tmp), src_panel->current_path, dest);
		snprintf(dest, dest_sz, "%s", tmp);
	}
}

static int copy_items_ex(const Panel *src_panel, const Panel *dst_panel, int inplace) {
	if (!src_panel || !dst_panel || src_panel->count == 0) return 0;

	int marked = panel_marked_count(src_panel);
	if (marked == 0 || (inplace && marked == 1)) {
		int sel_idx = src_panel->selected;
		if (inplace && marked == 1) {
			for (int i = 0; i < src_panel->count; i++) {
				if (src_panel->files[i].is_selected && strcmp(src_panel->files[i].name, "..") != 0) { sel_idx = i; break; }
			}
		}
		const FileEntry *item = &src_panel->files[sel_idx];
		if (strcmp(item->name, "..") == 0) return 0;

		char dest_input[MAX_PATH * 2];
		if (inplace || strcmp(src_panel->current_path, dst_panel->current_path) == 0) {
			snprintf(dest_input, sizeof(dest_input), "%s", item->name);
		} else {
			build_full_path(dest_input, sizeof(dest_input), dst_panel->current_path, item->name);
		}

		char prompt[MAX_FILENAME + 64];
		snprintf(prompt, sizeof(prompt), "Copy \"%s\" to:", item->name);

		if (!ui_input_path(inplace ? "Copy (Shift+F5)" : "Copy (F5)", prompt, dest_input, sizeof(dest_input))) return 0;
		if (strlen(dest_input) == 0) return 0;

		char src_full[MAX_PATH * 2];
		char dst_full[MAX_PATH * 5];
		panel_entry_full_path(src_panel, sel_idx, src_full, sizeof(src_full));

		if (dest_input[0] == '/') {
			snprintf(dst_full, sizeof(dst_full), "%s", dest_input);
		} else {
			build_full_path(dst_full, sizeof(dst_full), src_panel->current_path, dest_input);
		}

		struct stat st;
		if (stat(dst_full, &st) == 0 && S_ISDIR(st.st_mode)) {
			char tmp[MAX_PATH * 5 + MAX_FILENAME + 2];
			build_full_path(tmp, sizeof(tmp), dst_full, item->name);
			size_t tlen = strlen(tmp);
			if (tlen < sizeof(dst_full)) {
				memcpy(dst_full, tmp, tlen + 1);
			}
		}

		if (strcmp(src_full, dst_full) == 0) {
			ui_message("Error", "Cannot copy item onto itself");
			return -1;
		}

		g_overwrite_all = 0;
		UiProgress prog; progress_open(&prog, "Copying (F5)", 1);
		progress_scan_path(&prog, src_full);
		g_prog = &prog;
		int cr = prog.cancelled ? -2 : copy_recursive(src_full, dst_full);
		g_prog = NULL; progress_close(&prog);
		if (cr == -2) return 1;
		if (cr != 0) {
			ui_message("Error", "Failed to copy item");
			return -1;
		}
		return 1;
	}

	char dest_dir[MAX_PATH * 2];
	snprintf(dest_dir, sizeof(dest_dir), "%s", dst_panel->current_path);

	char prompt[64];
	snprintf(prompt, sizeof(prompt), "Copy selected (%d items) to:", marked);

	if (!ui_input_path("Copy Group (F5)", prompt, dest_dir, sizeof(dest_dir))) return 0;
	if (strlen(dest_dir) == 0) return 0;
	resolve_dest_path(src_panel, dest_dir, sizeof(dest_dir));

	g_overwrite_all = 0;
	UiProgress prog; progress_open(&prog, "Copying Group (F5)", 1);
	for (int i = 0; i < src_panel->count && !prog.cancelled; i++) {
		if (!src_panel->files[i].is_selected || strcmp(src_panel->files[i].name, "..") == 0) continue;
		char sf[MAX_PATH * 2]; panel_entry_full_path(src_panel, i, sf, sizeof(sf));
		progress_scan_path(&prog, sf);
	}
	g_prog = &prog;
	for (int i = 0; i < src_panel->count && !prog.cancelled; i++) {
		if (!src_panel->files[i].is_selected) continue;
		if (strcmp(src_panel->files[i].name, "..") == 0) continue;

		char src_full[MAX_PATH * 2];
		char dst_full[MAX_PATH * 3];
		panel_entry_full_path(src_panel, i, src_full, sizeof(src_full));
		build_full_path(dst_full, sizeof(dst_full), dest_dir, src_panel->files[i].name);

		if (strcmp(src_full, dst_full) == 0) continue;

		int cr = copy_recursive(src_full, dst_full);
		if (cr == -2) break;
		if (cr != 0) {
			g_prog = NULL; progress_close(&prog);
			ui_message("Error", g_copy_err[0] ? g_copy_err : "Error copying file group");
			return -1;
		}
	}
	g_prog = NULL; progress_close(&prog);
	return 1;
}

int copy_items(const Panel *src_panel, const Panel *dst_panel) { return copy_items_ex(src_panel, dst_panel, 0); }
int copy_items_inplace(const Panel *src_panel, const Panel *dst_panel) { return copy_items_ex(src_panel, dst_panel, 1); }

static int move_items_ex(const Panel *src_panel, const Panel *dst_panel, int inplace) {
	if (!src_panel || !dst_panel || src_panel->count == 0) return 0;

	int marked = panel_marked_count(src_panel);
	if (marked == 0 || (inplace && marked == 1)) {
		int sel_idx = src_panel->selected;
		if (inplace && marked == 1) {
			for (int i = 0; i < src_panel->count; i++) {
				if (src_panel->files[i].is_selected && strcmp(src_panel->files[i].name, "..") != 0) { sel_idx = i; break; }
			}
		}
		const FileEntry *item = &src_panel->files[sel_idx];
		if (strcmp(item->name, "..") == 0) return 0;

		char dest_input[MAX_PATH * 2];
		if (inplace || strcmp(src_panel->current_path, dst_panel->current_path) == 0) {
			snprintf(dest_input, sizeof(dest_input), "%s", item->name);
		} else {
			build_full_path(dest_input, sizeof(dest_input), dst_panel->current_path, item->name);
		}

		char prompt[MAX_FILENAME + 64];
		snprintf(prompt, sizeof(prompt), "Move or rename \"%s\" to:", item->name);

		if (!ui_input_path(inplace ? "Rename / Move (Shift+F6)" : "Move / Rename (F6)", prompt, dest_input, sizeof(dest_input))) return 0;
		if (strlen(dest_input) == 0) return 0;

		char src_full[MAX_PATH * 2];
		char dst_full[MAX_PATH * 5];
		build_full_path(src_full, sizeof(src_full), src_panel->current_path, item->name);

		if (dest_input[0] == '/') {
			snprintf(dst_full, sizeof(dst_full), "%s", dest_input);
		} else {
			build_full_path(dst_full, sizeof(dst_full), src_panel->current_path, dest_input);
		}

		struct stat st;
		if (stat(dst_full, &st) == 0 && S_ISDIR(st.st_mode)) {
			char tmp[MAX_PATH * 5 + MAX_FILENAME + 2];
			build_full_path(tmp, sizeof(tmp), dst_full, item->name);
			size_t tlen = strlen(tmp);
			if (tlen < sizeof(dst_full)) {
				memcpy(dst_full, tmp, tlen + 1);
			}
		}

		g_overwrite_all = 0;
		int mr = move_single_path(src_full, dst_full);
		if (mr == -2) return 0;
		if (mr != 0) {
			ui_message("Error", "Failed to move or rename item");
			return -1;
		}
		return 1;
	}

	char dest_dir[MAX_PATH * 2];
	snprintf(dest_dir, sizeof(dest_dir), "%s", dst_panel->current_path);

	char prompt[64];
	snprintf(prompt, sizeof(prompt), "Move selected (%d items) to:", marked);

	if (!ui_input_path("Move Group (F6)", prompt, dest_dir, sizeof(dest_dir))) return 0;
	if (strlen(dest_dir) == 0) return 0;
	resolve_dest_path(src_panel, dest_dir, sizeof(dest_dir));

	UiProgress prog; progress_open(&prog, "Moving Group (F6)", 1);
	for (int i = 0; i < src_panel->count && !prog.cancelled; i++) {
		if (!src_panel->files[i].is_selected || strcmp(src_panel->files[i].name, "..") == 0) continue;
		char sf[MAX_PATH * 2]; build_full_path(sf, sizeof(sf), src_panel->current_path, src_panel->files[i].name);
		progress_scan_path(&prog, sf);
	}
	g_prog = &prog;
	for (int i = 0; i < src_panel->count && !prog.cancelled; i++) {
		if (!src_panel->files[i].is_selected) continue;
		if (strcmp(src_panel->files[i].name, "..") == 0) continue;

		char src_full[MAX_PATH * 2];
		char dst_full[MAX_PATH * 3];
		build_full_path(src_full, sizeof(src_full), src_panel->current_path, src_panel->files[i].name);
		build_full_path(dst_full, sizeof(dst_full), dest_dir, src_panel->files[i].name);

		int mr = move_single_path(src_full, dst_full);
		if (mr == -2) break;
		if (mr != 0) {
			g_prog = NULL; progress_close(&prog);
			ui_message("Error", "Error moving file group");
			return -1;
		}
	}
	g_prog = NULL; progress_close(&prog);
	return 1;
}

int make_directory(Panel *panel) {
	if (!panel) return 0;

	char dir_name[MAX_FILENAME] = "";
	if (!ui_input("Make Folder (F7)", "Enter new folder name:", dir_name, sizeof(dir_name))) {
		return 0;
	}
	if (strlen(dir_name) == 0) return 0;

	char full_path[MAX_PATH * 2];
	if (dir_name[0] == '/') {
		snprintf(full_path, sizeof(full_path), "%s", dir_name);
	} else {
		build_full_path(full_path, sizeof(full_path), panel->current_path, dir_name);
	}

	if (mkdir(full_path, 0755) != 0 && !((errno == EACCES || errno == EPERM) && sudo_run_argv((const char *const[]){"mkdir", "-p", "--", full_path, NULL}) == 0)) {
		ui_message("Error", "Failed to create folder");
		return -1;
	}

	const char *base = strrchr(dir_name, '/');
	base = base ? (base + 1) : dir_name;
	snprintf(panel->parent_name, MAX_FILENAME, "%s", base);
	return 1;
}

static int is_protected_path(const char *path) {
	char resolved[MAX_PATH];
	if (!realpath(path, resolved)) {
		snprintf(resolved, sizeof(resolved), "%s", path);
	}
	if (strcmp(resolved, "/") == 0 || strcmp(resolved, "/home") == 0 ||
	    strcmp(resolved, "/etc") == 0 || strcmp(resolved, "/usr") == 0 ||
	    strcmp(resolved, "/var") == 0 || strcmp(resolved, "/bin") == 0 ||
	    strcmp(resolved, "/boot") == 0 || strcmp(resolved, "/dev") == 0 ||
	    strcmp(resolved, "/proc") == 0 || strcmp(resolved, "/sys") == 0 ||
	    strcmp(resolved, "/media") == 0 || strcmp(resolved, "/mnt") == 0) {
		return 1;
	}
	const char *home = getenv("HOME");
	if (home && home[0] && strcmp(resolved, home) == 0) return 1;
	return 0;
}

static int move_one_to_trash(const char *full_path, const char *name) {
	if (is_protected_path(full_path)) return -1;
	char cmd[MAX_PATH * 3];
	char esc[MAX_PATH * 2 + 64];
	size_t j = 0;
	esc[j++] = '\'';
	for (size_t i = 0; full_path[i] && j + 6 < sizeof(esc); i++) {
		if (full_path[i] == '\'') {
			esc[j++] = '\''; esc[j++] = '\\'; esc[j++] = '\''; esc[j++] = '\'';
		} else {
			esc[j++] = full_path[i];
		}
	}
	esc[j++] = '\'';
	esc[j] = '\0';
	snprintf(cmd, sizeof(cmd), "gio trash -- %s >/dev/null 2>&1", esc);
	if (system(cmd) == 0) return 0;

	const char *home = getenv("HOME");
	if (!home || !home[0]) return -1;
	char tbase[MAX_PATH], tfiles[MAX_PATH], tinfo[MAX_PATH];
	snprintf(tbase, sizeof(tbase), "%s/.local/share/Trash", home);
	mkdir(tbase, 0700);
	snprintf(tfiles, sizeof(tfiles), "%s/files", tbase);
	mkdir(tfiles, 0700);
	snprintf(tinfo, sizeof(tinfo), "%s/info", tbase);
	mkdir(tinfo, 0700);

	char dst[MAX_PATH * 2], infop[MAX_PATH * 2];
	snprintf(dst, sizeof(dst), "%s/%s", tfiles, name);
	snprintf(infop, sizeof(infop), "%s/%s.trashinfo", tinfo, name);
	struct stat st;
	for (int n = 2; lstat(dst, &st) == 0 && n < 10000; n++) {
		snprintf(dst, sizeof(dst), "%s/%s.%d", tfiles, name, n);
		snprintf(infop, sizeof(infop), "%s/%s.%d.trashinfo", tinfo, name, n);
	}
	if (move_single_path(full_path, dst) != 0) return -1;
	FILE *f = fopen(infop, "w");
	if (f) {
		fprintf(f, "[Trash Info]\nPath=%s\n", full_path);
		fclose(f);
	}
	return 0;
}

int trash_items(const Panel *panel) {
	if (!panel || panel->count == 0) return 0;

	int marked = panel_marked_count(panel);
	if (marked == 0) {
		const FileEntry *item = &panel->files[panel->selected];
		if (strcmp(item->name, "..") == 0) return 0;

		char prompt[MAX_FILENAME + 64];
		snprintf(prompt, sizeof(prompt), "Move \"%s\" to Trash?", item->name);
		if (!ui_confirm("Move to Trash (F8)", prompt)) return 0;

		char full_path[MAX_PATH * 2];
		build_full_path(full_path, sizeof(full_path), panel->current_path, item->name);

		UiProgress prog; progress_open(&prog, "Moving to Trash (F8)", 0);
		prog.total_files = 1;
		progress_update(&prog, full_path, NULL, 0, 0, 1);
		int tr = move_one_to_trash(full_path, item->name);
		prog.done_files = 1;
		progress_close(&prog);
		if (tr != 0) {
			draw_screen();
			if ((errno == EACCES || errno == EPERM || access(panel->current_path, W_OK) != 0) &&
			    ui_confirm("Elevation Required", "No permission for Trash. Delete permanently with sudo?") &&
			    sudo_run_argv((const char *const[]){"rm", "-rf", "--", full_path, NULL}) == 0) {
				return 1;
			}
			ui_message("Error", "Failed to move item to Trash");
			return -1;
		}
		return 1;
	}

	char prompt[64];
	snprintf(prompt, sizeof(prompt), "Move selected items (%d items) to Trash?", marked);
	if (!ui_confirm("Move Group to Trash (F8)", prompt)) return 0;

	UiProgress prog; progress_open(&prog, "Moving Group to Trash (F8)", 0);
	prog.total_files = marked;
	for (int i = 0; i < panel->count && !prog.cancelled; i++) {
		if (!panel->files[i].is_selected) continue;
		if (strcmp(panel->files[i].name, "..") == 0) continue;

		char full_path[MAX_PATH * 2];
		build_full_path(full_path, sizeof(full_path), panel->current_path, panel->files[i].name);
		if (progress_update(&prog, full_path, NULL, 0, 0, 0)) break;

		if (move_one_to_trash(full_path, panel->files[i].name) != 0) {
			if ((errno == EACCES || errno == EPERM || access(panel->current_path, W_OK) != 0) &&
			    sudo_run_argv((const char *const[]){ "rm", "-rf", "--", full_path, NULL }) == 0) {
				prog.done_files++;
				continue;
			}
			progress_close(&prog);
			ui_message("Error", "Error moving selected items to Trash");
			return -1;
		}
		prog.done_files++;
	}
	progress_close(&prog);
	return 1;
}

int delete_items(const Panel *panel) {
	if (!panel || panel->count == 0) return 0;

	int marked = panel_marked_count(panel);
	if (marked == 0) {
		const FileEntry *item = &panel->files[panel->selected];
		if (strcmp(item->name, "..") == 0) return 0;

		char prompt[MAX_FILENAME + 96];
		snprintf(prompt, sizeof(prompt), "Permanently delete \"%s\"? (Cannot be undone)", item->name);
		if (!ui_confirm("Permanent Delete (Shift+F8)", prompt)) return 0;

		char full_path[MAX_PATH * 2];
		build_full_path(full_path, sizeof(full_path), panel->current_path, item->name);
		if (is_protected_path(full_path)) {
			ui_message("Protected", "Refusing to delete protected system/home path");
			return -1;
		}

		UiProgress prog; progress_open(&prog, "Deleting (Shift+F8)", 0);
		progress_scan_path(&prog, full_path);
		g_prog = &prog;
		int dr = prog.cancelled ? -2 : delete_recursive(full_path);
		g_prog = NULL; progress_close(&prog);
		if (dr == -2) return 1;
		if (dr != 0) {
			draw_screen();
			if ((errno == EACCES || errno == EPERM || access(panel->current_path, W_OK) != 0) &&
			    sudo_run_argv((const char *const[]){"rm", "-rf", "--", full_path, NULL}) == 0) {
				return 1;
			}
			ui_message("Error", "Failed to delete item");
			return -1;
		}
		return 1;
	}

	char prompt[96];
	snprintf(prompt, sizeof(prompt), "Permanently delete %d selected items? (Cannot be undone)", marked);
	if (!ui_confirm("Permanent Delete Group (Shift+F8)", prompt)) return 0;

	UiProgress prog; progress_open(&prog, "Deleting Group (Shift+F8)", 0);
	for (int i = 0; i < panel->count && !prog.cancelled; i++) {
		if (!panel->files[i].is_selected || strcmp(panel->files[i].name, "..") == 0) continue;
		char fp[MAX_PATH * 2]; build_full_path(fp, sizeof(fp), panel->current_path, panel->files[i].name);
		progress_scan_path(&prog, fp);
	}
	g_prog = &prog;
	for (int i = 0; i < panel->count && !prog.cancelled; i++) {
		if (!panel->files[i].is_selected) continue;
		if (strcmp(panel->files[i].name, "..") == 0) continue;

		char full_path[MAX_PATH * 2];
		build_full_path(full_path, sizeof(full_path), panel->current_path, panel->files[i].name);
		if (is_protected_path(full_path)) {
			g_prog = NULL; progress_close(&prog);
			ui_message("Protected", "Refusing to delete protected system/home path");
			return -1;
		}

		int dr = delete_recursive(full_path);
		if (dr == -2) break;
		if (dr != 0) {
			g_prog = NULL; progress_close(&prog);
			draw_screen();
			if ((errno == EACCES || errno == EPERM || access(panel->current_path, W_OK) != 0) &&
			    sudo_run_argv((const char *const[]){"rm", "-rf", "--", full_path, NULL}) == 0) {
				continue;
			}
			ui_message("Error", "Error deleting selected items");
			return -1;
		}
	}
	g_prog = NULL; progress_close(&prog);
	return 1;
}

static int build_run_script(char *out_path, size_t out_size, const char *workdir) {
	snprintf(out_path, out_size, "/tmp/xc_run_%d.sh", (int)getpid());
	FILE *out = fopen(out_path, "w");
	if (!out) { out_path[0] = '\0'; return 0; }

	fputs("shopt -s expand_aliases 2>/dev/null\n", out);
	fputs("PS1='$ '\n", out);
	fputs("bind() { :; }\n", out);

	const char *home = getenv("HOME");
	if (home && *home) {
		char src_rc[MAX_PATH];
		snprintf(src_rc, sizeof(src_rc), "%s/.bashrc", home);
		FILE *in = fopen(src_rc, "r");
		if (in) {
			char line[4096];
			int in_case_i = 0;
			while (fgets(line, sizeof(line), in)) {
				if (strstr(line, "case") && strstr(line, "$-")) {
					in_case_i = 1;
					continue;
				}
				if (in_case_i) {
					if (strstr(line, "esac")) in_case_i = 0;
					continue;
				}
				if ((strstr(line, "$-") || strstr(line, "PS1")) && strstr(line, "return")) {
					continue;
				}
				fputs(line, out);
			}
			fclose(in);
		}
	}

	fprintf(out, "\ncd \"%s\" || exit 1\n", workdir);
	fclose(out);
	return 1;
}

static void sanitize_bg_cmd(const char *cmd, char *out, size_t out_sz) {
	snprintf(out, out_sz, "%s", cmd);
	size_t len = strlen(out);
	while (len > 0 && (out[len - 1] == ' ' || out[len - 1] == '\t' || out[len - 1] == '\n' || out[len - 1] == '\r')) {
		out[--len] = '\0';
	}
	if (len >= 1 && out[len - 1] == '&') {
		if (len >= 2 && (out[len - 2] == '&' || out[len - 2] == '>' || out[len - 2] == '<')) return;
		char body[MAX_CMD];
		size_t blen = len - 1;
		while (blen > 0 && (out[blen - 1] == ' ' || out[blen - 1] == '\t')) blen--;
		if (blen == 0 || blen >= sizeof(body)) return;
		memcpy(body, out, blen);
		body[blen] = '\0';
		if (strchr(body, '>') == NULL) {
			snprintf(out, out_sz, "{ %s; } </dev/null >/dev/null 2>&1 &", body);
		}
	}
}

void execute_system_command(const char *cmd) {
	if (strlen(cmd) == 0) return;
	if (ui_try_builtin_dialog_cmd(cmd)) return;

	input_restore_sane();
	if (!terminal_mode) {
		def_prog_mode();
		nl();
		endwin();
		input_restore_sane();
	}
	if (!terminal_mode) {
		char saved_cmd[MAX_CMD];
		snprintf(saved_cmd, sizeof(saved_cmd), "%s", cmd_buffer);
		snprintf(cmd_buffer, sizeof(cmd_buffer), "%s", cmd);
		print_term_prompt_ex(NULL, 1);
		snprintf(cmd_buffer, sizeof(cmd_buffer), "%s", saved_cmd);
		printf("\r\n");
		fflush(stdout);
	}

	char pwd_file[128];
	char other_pwd_file[128];
	snprintf(pwd_file, sizeof(pwd_file), "/tmp/xc_pwd_%d", (int)getpid());
	snprintf(other_pwd_file, sizeof(other_pwd_file), "/tmp/xc_other_pwd_%d", (int)getpid());
	unlink(pwd_file);
	unlink(other_pwd_file);

	char full_cmd[MAX_CMD + MAX_PATH + 256];
	const char *shell = getenv("SHELL");
	if (!shell) shell = "/bin/bash";

	setenv("XC_PWD_FILE", pwd_file, 1);
	setenv("XC_OTHER_PWD_FILE", other_pwd_file, 1);
	char clean_rc[128] = "";
	char safe_cmd[MAX_CMD + 64];
	sanitize_bg_cmd(cmd, safe_cmd, sizeof(safe_cmd));
	setenv("XC_USER_CMD", safe_cmd, 1);
	if (build_run_script(clean_rc, sizeof(clean_rc), active_panel->current_path)) {
		snprintf(full_cmd, sizeof(full_cmd), "%s -c '. \"%s\"; eval \"$XC_USER_CMD\"\nxc_rc=$?; [ -s \"$XC_PWD_FILE\" ] || pwd > \"$XC_PWD_FILE\"; exit $xc_rc' bash", shell, clean_rc);
	} else {
		snprintf(full_cmd, sizeof(full_cmd), "%s -c 'cd \"%s\" && %s; [ -s \"$XC_PWD_FILE\" ] || pwd > \"$XC_PWD_FILE\"'", shell, active_panel->current_path, safe_cmd);
	}
	input_restore_sane();
	fflush(stdout);
	system(full_cmd);
	if (clean_rc[0] != '\0') unlink(clean_rc);
	unsetenv("XC_PWD_FILE");
	unsetenv("XC_OTHER_PWD_FILE");
	unsetenv("XC_USER_CMD");

	FILE *pf = fopen(pwd_file, "r");
	if (pf) {
		char new_dir[MAX_PATH];
		if (fgets(new_dir, sizeof(new_dir), pf) != NULL) {
			size_t len = strlen(new_dir);
			while (len > 0 && (new_dir[len - 1] == '\n' || new_dir[len - 1] == '\r')) {
				new_dir[--len] = '\0';
			}
			if (len > 0 && active_panel && strcmp(active_panel->current_path, new_dir) != 0) {
				snprintf(active_panel->current_path, MAX_PATH, "%s", new_dir);
				load_directory(active_panel);
			}
		}
		fclose(pf);
	}
	unlink(pwd_file);

	Panel *other_panel = (active_panel == &left_panel) ? &right_panel : &left_panel;
	FILE *opf = fopen(other_pwd_file, "r");
	if (opf) {
		char other_dir[MAX_PATH];
		if (fgets(other_dir, sizeof(other_dir), opf) != NULL) {
			size_t len = strlen(other_dir);
			while (len > 0 && (other_dir[len - 1] == '\n' || other_dir[len - 1] == '\r')) {
				other_dir[--len] = '\0';
			}
			if (len > 0 && other_panel && strcmp(other_panel->current_path, other_dir) != 0) {
				snprintf(other_panel->current_path, MAX_PATH, "%s", other_dir);
				load_directory(other_panel);
			}
		}
		fclose(opf);
	}
	unlink(other_pwd_file);

	if (!terminal_mode) {
		printf("\r\n");
		fflush(stdout);
		reset_prog_mode();
		nonl();
		clearok(stdscr, TRUE);
		refresh();
	}
	input_enable_raw();
}

void view_file(const char *filename) {
	char cmd[MAX_PATH * 2 + 64];
	snprintf(cmd, sizeof(cmd), "less -R -X \"%s\"", filename);
	execute_system_command(cmd);
}

void edit_file(const char *filename) {
	const char *editor = getenv("EDITOR");
	if (!editor) editor = "vi";
	char cmd[MAX_PATH * 2 + 64];
	snprintf(cmd, sizeof(cmd), "%s \"%s\"", editor, filename);
	execute_system_command(cmd);
}

static char clip_paths[MAX_FILES][MAX_PATH * 2];
static char clip_last_sys[MAX_PATH * 2] = "";
static int clip_count = 0;
static int clip_is_cut = 0;

static void clipboard_write_sys(const char *text) {
	if (!text || !*text) return;
	const char *cmd = "( [ -n \"$WAYLAND_DISPLAY\" ] && wl-copy ) 2>/dev/null || xclip -selection clipboard 2>/dev/null || xsel --clipboard --input 2>/dev/null";
	FILE *p = popen(cmd, "w");
	if (p) {
		fwrite(text, 1, strlen(text), p);
		pclose(p);
	}
}

void clipboard_copy(const char *text) {
	if (!text || !*text) return;
	clip_count = 0;
	clip_is_cut = 0;
	clip_last_sys[0] = '\0';
	clipboard_write_sys(text);
}

size_t clipboard_paste_raw(char *out, size_t out_size) {
	if (!out || out_size == 0) return 0;
	out[0] = '\0';
	const char *cmd = "( [ -n \"$WAYLAND_DISPLAY\" ] && wl-paste -n ) 2>/dev/null || xclip -o -selection clipboard 2>/dev/null || xsel --clipboard --output 2>/dev/null";
	FILE *p = popen(cmd, "r");
	if (!p) return 0;
	size_t n = 0;
	while (n + 1 < out_size) {
		size_t r = fread(out + n, 1, out_size - 1 - n, p);
		if (r == 0) break;
		n += r;
	}
	pclose(p);
	out[n] = '\0';
	while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r')) out[--n] = '\0';
	if (strncmp(out, "file://", 7) == 0) {
		n -= 7;
		memmove(out, out + 7, n + 1);
	}
	return n;
}

size_t clipboard_paste(char *out, size_t out_size) {
	size_t n = clipboard_paste_raw(out, out_size);
	for (size_t i = 0; i < n; i++) {
		if (out[i] == '\n' || out[i] == '\r') out[i] = ' ';
	}
	return n;
}

void clipboard_stage_files(const Panel *panel, int is_cut) {
	if (!panel || panel->count == 0) return;
	clip_count = 0;
	clip_is_cut = is_cut;
	clip_last_sys[0] = '\0';

	int marked = panel_marked_count(panel);
	if (marked > 0) {
		for (int i = 0; i < panel->count && clip_count < MAX_FILES; i++) {
			if (!panel->files[i].is_selected) continue;
			if (strcmp(panel->files[i].name, "..") == 0) continue;
			build_full_path(clip_paths[clip_count], sizeof(clip_paths[0]), panel->current_path, panel->files[i].name);
			clip_count++;
		}
	} else {
		const FileEntry *item = &panel->files[panel->selected];
		if (strcmp(item->name, "..") == 0) return;
		build_full_path(clip_paths[0], sizeof(clip_paths[0]), panel->current_path, item->name);
		clip_count = 1;
	}

	if (clip_count > 0) {
		snprintf(clip_last_sys, sizeof(clip_last_sys), "%s", clip_paths[0]);
		static char uri_buf[MAX_FILES * (MAX_PATH * 2 + 16)];
		size_t uoff = 0;
		for (int i = 0; i < clip_count && uoff + 32 < sizeof(uri_buf); i++) {
			int w = snprintf(uri_buf + uoff, sizeof(uri_buf) - uoff, "file://%s\r\n", clip_paths[i]);
			if (w > 0) uoff += (size_t)w;
		}
		const char *ucmd = "( [ -n \"$WAYLAND_DISPLAY\" ] && wl-copy -t text/uri-list ) 2>/dev/null || xclip -selection clipboard -t text/uri-list 2>/dev/null";
		FILE *up = popen(ucmd, "w");
		if (up) {
			fwrite(uri_buf, 1, uoff, up);
			pclose(up);
		} else {
			clipboard_write_sys(clip_last_sys);
		}
	}
}

int clipboard_has_files(void) {
	if (clip_count <= 0) return 0;
	char sys_buf[MAX_PATH * 2] = {0};
	if (clipboard_paste(sys_buf, sizeof(sys_buf)) > 0) {
		char *sp = strchr(sys_buf, ' ');
		if (sp) *sp = '\0';
		if (strcmp(sys_buf, clip_last_sys) != 0) {
			clip_count = 0;
			return 0;
		}
	}
	return 1;
}

int clipboard_paste_files(Panel *dst_panel) {
	if (!dst_panel || clip_count <= 0) return 0;

	char prompt[MAX_PATH + 64];
	if (clip_count == 1) {
		const char *base = strrchr(clip_paths[0], '/');
		base = base ? (base + 1) : clip_paths[0];
		snprintf(prompt, sizeof(prompt), "%s \"%s\" to %s?", clip_is_cut ? "Move" : "Paste", base, dst_panel->current_path);
	} else {
		snprintf(prompt, sizeof(prompt), "%s %d items to %s?", clip_is_cut ? "Move" : "Paste", clip_count, dst_panel->current_path);
	}

	if (!ui_confirm(clip_is_cut ? "Move (Ctrl+V)" : "Paste (Ctrl+V)", prompt)) return 0;

	int ok_count = 0;
	g_overwrite_all = 0;
	for (int i = 0; i < clip_count; i++) {
		const char *base = strrchr(clip_paths[i], '/');
		base = base ? (base + 1) : clip_paths[i];
		char dst_full[MAX_PATH * 2];
		build_full_path(dst_full, sizeof(dst_full), dst_panel->current_path, base);

		if (strcmp(clip_paths[i], dst_full) == 0) continue;

		int res = clip_is_cut ? move_single_path(clip_paths[i], dst_full) : copy_recursive(clip_paths[i], dst_full);
		if (res == -2) break;
		if (res != 0) {
			ui_message("Error", clip_is_cut ? "Failed to move item" : "Failed to paste item");
			return -1;
		}
		snprintf(dst_panel->parent_name, MAX_FILENAME, "%s", base);
		ok_count++;
	}

	if (clip_is_cut && ok_count > 0) {
		clip_count = 0;
		clip_is_cut = 0;
		clip_last_sys[0] = '\0';
	}
	return ok_count > 0 ? 1 : 0;
}

int move_items(const Panel *src_panel, const Panel *dst_panel) { return move_items_ex(src_panel, dst_panel, 0); }
int move_items_inplace(const Panel *src_panel, const Panel *dst_panel) { return move_items_ex(src_panel, dst_panel, 1); }

static void shell_quote_append(const char *s, char *out, size_t cap, size_t *pos) {
	if (*pos + 3 >= cap) return;
	out[(*pos)++] = '\'';
	for (size_t i = 0; s && s[i] && *pos + 6 < cap; i++) {
		if (s[i] == '\'') {
			out[(*pos)++] = '\''; out[(*pos)++] = '\\'; out[(*pos)++] = '\''; out[(*pos)++] = '\'';
		} else {
			out[(*pos)++] = s[i];
		}
	}
	out[(*pos)++] = '\'';
	out[*pos] = '\0';
}

static void expand_far_macros(const char *tpl, const char *dir, const char *name, const char *full, char *out, size_t cap) {
	char stem[MAX_FILENAME], ext[MAX_FILENAME] = "";
	snprintf(stem, sizeof(stem), "%s", name);
	char *dot = strrchr(stem, '.');
	if (dot && dot != stem) { snprintf(ext, sizeof(ext), "%s", dot + 1); *dot = '\0'; }

	size_t pos = 0; out[0] = '\0';
	int has_macro = 0;
	for (size_t i = 0; tpl[i] && pos + 8 < cap; i++) {
		if (tpl[i] == '!') {
			has_macro = 1;
			if (tpl[i + 1] == '.' && tpl[i + 2] == '!') { shell_quote_append(name, out, cap, &pos); i += 2; }
			else if (tpl[i + 1] == '~') { shell_quote_append(stem, out, cap, &pos); i += 1; }
			else if (tpl[i + 1] == '.') { shell_quote_append(ext, out, cap, &pos); i += 1; }
			else if (tpl[i + 1] == ':') { shell_quote_append(full, out, cap, &pos); i += 1; }
			else if (tpl[i + 1] == '/') {
				char dslash[MAX_PATH + 4];
				size_t dl = strlen(dir);
				snprintf(dslash, sizeof(dslash), "%s%s", dir, (dl > 0 && dir[dl - 1] == '/') ? "" : "/");
				shell_quote_append(dslash, out, cap, &pos); i += 1;
			} else {
				shell_quote_append(name, out, cap, &pos);
			}
		} else {
			out[pos++] = tpl[i];
			out[pos] = '\0';
		}
	}
	if (!has_macro && pos + 4 < cap) {
		out[pos++] = ' '; out[pos] = '\0';
		shell_quote_append(name, out, cap, &pos);
	}
}

int apply_command_to_items(const Panel *panel) {
	if (!panel || panel->count == 0) return 0;
	int *idxs = malloc((size_t)panel->count * sizeof(int)), cnt = 0, marked = panel_marked_count(panel);
	if (!idxs) return 0;
	for (int i = 0; i < panel->count; i++) {
		if (strcmp(panel->files[i].name, "..") == 0) continue;
		if ((marked > 0 && panel->files[i].is_selected) || (marked == 0 && i == panel->selected)) idxs[cnt++] = i;
	}
	if (cnt == 0) { free(idxs); return 0; }

	static char last_cmd[1024] = "";
	char cfg_path[MAX_PATH];
	const char *xdg = getenv("XDG_CONFIG_HOME");
	if (xdg && *xdg) snprintf(cfg_path, sizeof(cfg_path), "%s/xcommander/apply_cmd", xdg);
	else { const char *hm = getenv("HOME"); snprintf(cfg_path, sizeof(cfg_path), "%s/.config/xcommander/apply_cmd", hm ? hm : "/tmp"); }
	if (last_cmd[0] == '\0') {
		FILE *rf = fopen(cfg_path, "r");
		if (rf) {
			if (fgets(last_cmd, sizeof(last_cmd), rf)) last_cmd[strcspn(last_cmd, "\r\n")] = '\0';
			fclose(rf);
		}
	}
	char cmd[1024];
	snprintf(cmd, sizeof(cmd), "%s", last_cmd);
	char title[96];
	snprintf(title, sizeof(title), "Apply command (%d item%s)", cnt, cnt == 1 ? "" : "s");
	if (!ui_input(title, "Command (macros: !.! !~ !. !: !/):", cmd, sizeof(cmd))) { free(idxs); return 0; }
	if (cmd[0] == '\0') { free(idxs); return 0; }
	snprintf(last_cmd, sizeof(last_cmd), "%s", cmd);
	FILE *wf = fopen(cfg_path, "w");
	if (wf) { fprintf(wf, "%s\n", last_cmd); fclose(wf); }

	const char *cp = cmd;
	while (*cp == ' ' || *cp == '\t') cp++;
	if ((strncmp(cp, ":dialog", 7) == 0 && (cp[7] == '\0' || cp[7] == ' ' || cp[7] == '\t')) ||
	    (strncmp(cp, ":D", 2) == 0 && (cp[2] == '\0' || cp[2] == ' ' || cp[2] == '\t'))) {
		for (int k = 0; k < cnt; k++) {
			int idx = idxs[k];
			char full[MAX_PATH * 2], dir[MAX_PATH * 2], expanded[4096];
			panel_entry_full_path(panel, idx, full, sizeof(full));
			snprintf(dir, sizeof(dir), "%s", full);
			char *sl = strrchr(dir, '/');
			if (sl) { if (sl == dir) strcpy(dir, "/"); else *sl = '\0'; }
			else snprintf(dir, sizeof(dir), "%s", panel->current_path);
			expand_far_macros(cmd, dir, panel->files[idx].name, full, expanded, sizeof(expanded));
			ui_try_builtin_dialog_cmd(expanded);
		}
		free(idxs);
		return 1;
	}

	input_set_mouse(0);
	endwin();
	input_restore_sane();

	for (int k = 0; k < cnt; k++) {
		int idx = idxs[k];
		char full[MAX_PATH * 2], dir[MAX_PATH * 2], expanded[4096];
		panel_entry_full_path(panel, idx, full, sizeof(full));
		snprintf(dir, sizeof(dir), "%s", full);
		char *sl = strrchr(dir, '/');
		if (sl) { if (sl == dir) strcpy(dir, "/"); else *sl = '\0'; }
		else snprintf(dir, sizeof(dir), "%s", panel->current_path);

		expand_far_macros(cmd, dir, panel->files[idx].name, full, expanded, sizeof(expanded));
		printf("\033[1;36m==> [%d/%d] %s\033[0m\r\n", k + 1, cnt, expanded);
		fflush(stdout);
		if (chdir(dir) == 0) { int rc = system(expanded); (void)rc; }
	}
	printf("\r\n\033[1;33m--- Done (%d item%s). Press any key ---\033[0m", cnt, cnt == 1 ? "" : "s");
	fflush(stdout);
	struct termios orig_t, raw_t;
	if (tcgetattr(STDIN_FILENO, &orig_t) == 0) {
		raw_t = orig_t;
		raw_t.c_lflag &= ~(tcflag_t)(ICANON | ECHO);
		raw_t.c_cc[VMIN] = 1; raw_t.c_cc[VTIME] = 0;
		tcsetattr(STDIN_FILENO, TCSANOW, &raw_t);
		char ch; (void)!read(STDIN_FILENO, &ch, 1);
		tcsetattr(STDIN_FILENO, TCSANOW, &orig_t);
	} else {
		getchar();
	}

	reset_prog_mode();
	input_enable_raw();
	input_set_mouse(1);
	clearok(stdscr, TRUE);
	return 1;
}

static int create_one_link(const char *target, const char *link_path, int is_hard) {
	int rc = is_hard ? link(target, link_path) : symlink(target, link_path);
	if (rc == 0) return 0;
	if (errno == EACCES || errno == EPERM) {
		if (is_hard) return sudo_run_argv((const char *const[]){ "ln", "--", target, link_path, NULL });
		return sudo_run_argv((const char *const[]){ "ln", "-s", "--", target, link_path, NULL });
	}
	return -1;
}

int make_link_items(const Panel *src_panel, const Panel *dst_panel) {
	if (!src_panel || src_panel->count == 0 || src_panel->archive_file[0] != '\0') return 0;
	int marked = panel_marked_count(src_panel);
	const FileEntry *cur = &src_panel->files[src_panel->selected];
	if (marked == 0 && strcmp(cur->name, "..") == 0) return 0;

	const char *def_dir = (dst_panel && dst_panel->archive_file[0] == '\0' && !dst_panel->is_temp)
	                      ? dst_panel->current_path : src_panel->current_path;
	char link_buf[MAX_PATH], targ_buf[MAX_PATH];
	if (marked <= 1) {
		int idx = src_panel->selected;
		if (marked == 1) { for (int i = 0; i < src_panel->count; i++) if (src_panel->files[i].is_selected) { idx = i; break; } }
		panel_entry_full_path(src_panel, idx, targ_buf, sizeof(targ_buf));
		build_full_path(link_buf, sizeof(link_buf), def_dir, src_panel->files[idx].name);
	} else {
		snprintf(targ_buf, sizeof(targ_buf), "(%d selected items)", marked);
		snprintf(link_buf, sizeof(link_buf), "%s", def_dir);
	}

	int h, w; getmaxyx(stdscr, h, w);
	int bw = (w - 4 < 62) ? (w - 4) : 62, bh = 11;
	int sy = (h > bh) ? (h - bh) / 2 : 0, sx = (w > bw) ? (w - bw) / 2 : 0;
	WINDOW *win = newwin(bh, bw, sy, sx); wbkgd(win, COLOR_PAIR(5));

	int foc = 0, is_hard = 0, created = 0;
	size_t lpos = strlen(link_buf), tpos = strlen(targ_buf);
	const char *btns[2] = { "[ Create ]", "[ Cancel ]" };

	while (1) {
		werase(win); wattrset(win, COLOR_PAIR(5)); box(win, 0, 0);
		mvwprintw(win, 0, 2, " Make Link (Alt+F6) ");
		int fw = bw - 4;
		mvwprintw(win, 2, 2, "Link path%s:", marked > 1 ? " (directory)" : "");
		wattrset(win, (foc == 0) ? COLOR_PAIR(2) : COLOR_PAIR(4));
		mvwprintw(win, 3, 2, "%-*.*s", fw, fw, link_buf);

		wattrset(win, COLOR_PAIR(5));
		mvwprintw(win, 4, 2, "Target:");
		wattrset(win, (foc == 1 && marked <= 1) ? COLOR_PAIR(2) : COLOR_PAIR(4));
		mvwprintw(win, 5, 2, "%-*.*s", fw, fw, targ_buf);

		wattrset(win, (foc == 2) ? COLOR_PAIR(2) : COLOR_PAIR(5));
		mvwprintw(win, 7, 2, "(%c) Symbolic link   (%c) Hard link", is_hard ? ' ' : 'x', is_hard ? 'x' : ' ');

		UiButtonRow brow = { btns, 2, 9, (bw - 24) / 2, 2, (foc >= 3) ? (foc - 3) : -1 };
		ui_btns_draw(&brow, win);
		if (foc == 0) { curs_set(1); wmove(win, 3, 2 + ((int)lpos < fw ? (int)lpos : fw - 1)); }
		else if (foc == 1 && marked <= 1) { curs_set(1); wmove(win, 5, 2 + ((int)tpos < fw ? (int)tpos : fw - 1)); }
		else curs_set(0);
		wrefresh(win);

		InputEvent ev = input_read();
		if (ev.action == ACT_ESC || ev.action == ACT_F10) break;
		if (ev.action == ACT_TAB || ev.action == ACT_DOWN) { foc = (foc + 1) % 5; if (foc == 1 && marked > 1) foc = 2; continue; }
		if (ev.action == ACT_SHIFT_TAB || ev.action == ACT_UP) { foc = (foc + 4) % 5; if (foc == 1 && marked > 1) foc = 0; continue; }
		if (foc == 2 && (ev.action == ACT_LEFT || ev.action == ACT_RIGHT || (ev.action == ACT_CHAR && ev.ch == ' '))) { is_hard = !is_hard; continue; }
		if (foc >= 3 && ev.action == ACT_LEFT) { foc = 3; continue; }
		if (foc >= 3 && ev.action == ACT_RIGHT) { foc = 4; continue; }
		if (ev.action == ACT_MOUSE_LEFT) {
			int ry = ev.mouse_y - sy, rx = ev.mouse_x - sx;
			if (ry == 3 && rx >= 2 && rx < 2 + fw) { foc = 0; continue; }
			if (ry == 5 && rx >= 2 && rx < 2 + fw && marked <= 1) { foc = 1; continue; }
			if (ry == 7 && rx >= 2 && rx < bw - 2) { foc = 2; is_hard = (rx >= 20) ? 1 : 0; continue; }
			int bhit = ui_btns_hit_test(&brow, ry, rx);
			if (bhit == 1) break;
			if (bhit == 0) { foc = 3; ev.action = ACT_ENTER; }
		}
		if (foc == 0 || (foc == 1 && marked <= 1)) {
			char *b = (foc == 0) ? link_buf : targ_buf;
			size_t *p = (foc == 0) ? &lpos : &tpos;
			if (ev.action == ACT_BACKSPACE && *p > 0) { b[--(*p)] = '\0'; continue; }
			if (ev.action == ACT_CHAR && ev.ch >= 32 && *p + 5 < MAX_PATH) {
				char mb[8]; int ml = wctomb(mb, ev.ch);
				if (ml > 0 && *p + (size_t)ml < MAX_PATH) { memcpy(b + *p, mb, (size_t)ml); *p += (size_t)ml; b[*p] = '\0'; }
				continue;
			}
		}
		if (ev.action == ACT_ENTER) {
			if (foc == 4) break;
			curs_set(0); delwin(win); win = NULL; draw_screen();
			if (marked <= 1) {
				if (create_one_link(targ_buf, link_buf, is_hard) != 0) ui_message("Link Error", "Failed to create link");
				else created = 1;
			} else {
				int errs = 0;
				for (int i = 0; i < src_panel->count; i++) {
					if (!src_panel->files[i].is_selected || strcmp(src_panel->files[i].name, "..") == 0) continue;
					char tfp[MAX_PATH], lfp[MAX_PATH * 2];
					panel_entry_full_path(src_panel, i, tfp, sizeof(tfp));
					build_full_path(lfp, sizeof(lfp), link_buf, src_panel->files[i].name);
					if (create_one_link(tfp, lfp, is_hard) != 0) errs++; else created = 1;
				}
				if (errs > 0) { char em[64]; snprintf(em, sizeof(em), "Failed on %d item(s)", errs); ui_message("Link Error", em); }
			}
			break;
		}
	}
	curs_set(0); if (win) { delwin(win); touchwin(stdscr); }
	return created;
}
