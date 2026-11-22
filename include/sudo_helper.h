#ifndef SUDO_HELPER_H
#define SUDO_HELPER_H

int sudo_run_argv(const char *const argv[]);
int sudo_write_from_temp(const char *tmp_path, const char *dst_path);
void sudo_clear_cache(void);

#endif
