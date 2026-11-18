#ifndef ARCHIVE_H
#define ARCHIVE_H

#include "xc.h"

int archive_is_supported(const char *filename);
int archive_load_dir(Panel *p);
int archive_extract_one(const char *archive_file, const char *inner_path, char *out_tmp_path, size_t out_sz);
int archive_extract_items(Panel *src, Panel *dst);
int archive_update_one(const char *archive_file, const char *inner_path, const char *src_tmp_file);
int archive_delete_items(Panel *src);
int archive_create_from_panel(Panel *src);

#endif
