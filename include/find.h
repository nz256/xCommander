#ifndef FIND_H
#define FIND_H

#include <stddef.h>

/* Returns:
 * 0 = closed/cancelled
 * 1 = Go to file in active panel (out_path set)
 * 2 = Insert path into command line (out_path set)
 */
int ui_find_files(const char *start_dir, char *out_path, size_t out_sz);

#endif
