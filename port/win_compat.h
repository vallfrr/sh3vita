/* Windows (MinGW-w64) host-bench compatibility shims. Force-included with -include for the Windows build only. */
#ifndef SH3_WIN_COMPAT_H
#define SH3_WIN_COMPAT_H
#ifdef _WIN32
#include <io.h>
#include <direct.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <windows.h>
#ifndef O_NOFOLLOW
#define O_NOFOLLOW _O_BINARY /* binary mode: no CRLF/Ctrl-Z translation */
#endif
#define lstat stat
static inline int setenv(const char *name, const char *value, int overwrite) {
    if (!overwrite && getenv(name)) return 0;
    return _putenv_s(name, value);
}
static inline char *realpath(const char *path, char *resolved) {
    struct stat st_;
    if (!_fullpath(resolved, path, 4096)) return NULL;
    for (char *q = resolved; *q; q++) if (*q == 0x5c) *q = '/';
    if (stat(resolved, &st_)) { errno = ENOENT; return NULL; }
    return resolved;
}
static inline struct tm *localtime_r(const time_t *t, struct tm *out) { return localtime_s(out, t) ? NULL : out; }
static inline struct tm *gmtime_r(const time_t *t, struct tm *out) { return gmtime_s(out, t) ? NULL : out; }
/* Positional read without moving the descriptor's own offset semantics being relied upon: the callers seek afterwards. */
static inline ssize_t pread(int fd, void *buf, size_t n, off_t off) {
    HANDLE h = (HANDLE)_get_osfhandle(fd); OVERLAPPED ov = {0}; DWORD got = 0;
    ov.Offset = (DWORD)(off & 0xffffffffu); ov.OffsetHigh = (DWORD)((unsigned long long)off >> 32);
    if (!ReadFile(h, buf, (DWORD)n, &got, &ov)) { if (GetLastError() == ERROR_HANDLE_EOF) return 0; errno = EIO; return -1; }
    return (ssize_t)got;
}
#endif
#endif
