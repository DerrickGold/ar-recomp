/* UTF-8 host filesystem paths, independent of the Windows ANSI/CRT code page.
 * Header-only so small ROM-free tools can use the same boundary as the runner.
 * Do not redefine libc names: callers opt in explicitly. */
#ifndef SNESRECOMP_SUPPORT_UTF8_FS_H
#define SNESRECOMP_SUPPORT_UTF8_FS_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <sys/stat.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <direct.h>
#include <io.h>
#include <process.h>
#include <wchar.h>

static inline wchar_t *sr_utf8_to_wide(const char *text) {
    if (!text) { errno = EINVAL; return NULL; }
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, NULL, 0);
    if (!count) { errno = EILSEQ; return NULL; }
    wchar_t *wide = (wchar_t *)malloc((size_t)count * sizeof(wchar_t));
    if (!wide) { errno = ENOMEM; return NULL; }
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, wide, count)) {
        free(wide); errno = EILSEQ; return NULL;
    }
    return wide;
}

static inline int sr_wide_to_utf8(const wchar_t *wide, char *out, size_t capacity) {
    if (!wide || !out || !capacity || capacity > INT_MAX) { errno = EINVAL; return 0; }
    out[0] = '\0';
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1, out, (int)capacity, NULL, NULL)) {
        out[0] = '\0'; errno = EILSEQ; return 0;
    }
    return 1;
}

// Normalize before adding the extended-path prefix; relative paths and '/' do
// not have normal Win32 semantics after that prefix. Never silently truncate.
static inline wchar_t *sr_win_path(const char *path) {
    if (!path || !*path) { errno = ENOENT; return NULL; }
    wchar_t *wide = sr_utf8_to_wide(path);
    if (!wide) return NULL;
    if (!wcsncmp(wide, L"\\\\?\\", 4)) return wide;
    DWORD count = GetFullPathNameW(wide, 0, NULL, NULL);
    if (!count) { free(wide); errno = ENOENT; return NULL; }
    wchar_t *full = (wchar_t *)malloc(((size_t)count + 8) * sizeof(wchar_t));
    if (!full) { free(wide); errno = ENOMEM; return NULL; }
    DWORD length = GetFullPathNameW(wide, count, full + 8, NULL);
    free(wide);
    if (!length || length >= count) { free(full); errno = ENAMETOOLONG; return NULL; }
    for (wchar_t *p = full + 8; *p; ++p) if (*p == L'/') *p = L'\\';
    if (full[8] == L'\\' && full[9] == L'\\') {
        memmove(full + 8, full + 10, (wcslen(full + 10) + 1) * sizeof(wchar_t));
        memcpy(full, L"\\\\?\\UNC\\", 8 * sizeof(wchar_t));
    } else {
        memmove(full + 4, full + 8, (wcslen(full + 8) + 1) * sizeof(wchar_t));
        memcpy(full, L"\\\\?\\", 4 * sizeof(wchar_t));
    }
    return full;
}

static inline FILE *sr_fopen(const char *path, const char *mode) {
    wchar_t *wide = sr_win_path(path), *wide_mode = sr_utf8_to_wide(mode);
    FILE *file = wide && wide_mode ? _wfopen(wide, wide_mode) : NULL;
    free(wide); free(wide_mode);
    return file;
}

static inline int sr_remove(const char *path) {
    wchar_t *wide = sr_win_path(path);
    int result = wide ? _wremove(wide) : -1;
    free(wide); return result;
}

static inline int sr_mkdir(const char *path) {
    wchar_t *wide = sr_win_path(path);
    int result = wide ? _wmkdir(wide) : -1;
    free(wide); return result;
}

static inline int sr_access(const char *path, int mode) {
    wchar_t *wide = sr_win_path(path);
    int result = wide ? _waccess(wide, mode) : -1;
    free(wide); return result;
}

static inline int sr_utf8_chdir(const char *path) {
    wchar_t *wide = sr_win_path(path);
    int result = wide ? _wchdir(wide) : -1;
    free(wide); return result;
}

static inline DWORD sr_path_attributes(const char *path) {
    wchar_t *wide = sr_win_path(path);
    DWORD result = wide ? GetFileAttributesW(wide) : INVALID_FILE_ATTRIBUTES;
    free(wide); return result;
}

static inline int sr_path_exists(const char *path) {
    return sr_path_attributes(path) != INVALID_FILE_ATTRIBUTES;
}

static inline int sr_path_is_directory(const char *path) {
    DWORD attrs = sr_path_attributes(path);
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

static inline int sr_replace_file(const char *source, const char *destination) {
    wchar_t *from = sr_win_path(source), *to = sr_win_path(destination);
    int result = from && to && MoveFileExW(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    free(from); free(to); return result;
}

static inline int sr_execvp(const char *path, const char *const *argv) {
    size_t count = 0;
    while (argv[count]) ++count;
    wchar_t **wide_args = (wchar_t **)calloc(count + 1, sizeof(wchar_t *));
    wchar_t *wide_path = sr_utf8_to_wide(path);
    if (!wide_args || !wide_path) { free(wide_args); free(wide_path); return -1; }
    size_t i;
    for (i = 0; i < count; ++i) {
        wide_args[i] = sr_utf8_to_wide(argv[i]);
        if (!wide_args[i]) break;
    }
    if (i == count) _wexecvp(wide_path, (const wchar_t *const *)wide_args);
    int saved_errno = errno;
    for (i = 0; i < count; ++i) free(wide_args[i]);
    free(wide_args); free(wide_path);
    errno = saved_errno;
    return -1;
}

#else
#include <unistd.h>
#define sr_fopen fopen
#define sr_remove remove
#define sr_access access
#define sr_utf8_chdir chdir
static inline int sr_mkdir(const char *path) { return mkdir(path, 0755); }
static inline int sr_path_exists(const char *path) {
    struct stat info; return stat(path, &info) == 0;
}
static inline int sr_path_is_directory(const char *path) {
    struct stat info; return stat(path, &info) == 0 && S_ISDIR(info.st_mode);
}
static inline int sr_replace_file(const char *from, const char *to) { return rename(from, to) == 0; }
#endif
#endif
