#pragma once
#include <stddef.h>
/* Maps only the SH3 data tree; returns 0 on success, -1 for unsafe paths. */
int sh3_vita_data_path(const char *windows_path, char *out, size_t capacity);
