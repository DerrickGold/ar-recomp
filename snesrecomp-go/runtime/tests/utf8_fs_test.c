#include "snesrecomp/support/utf8_fs.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>

static void write_text(const char *path, const char *text) {
    FILE *file = sr_fopen(path, "wb");
    assert(file != NULL);
    assert(fwrite(text, 1, strlen(text), file) == strlen(text));
    assert(fclose(file) == 0);
}

static void expect_text(const char *path, const char *text) {
    char bytes[64] = {0};
    FILE *file = sr_fopen(path, "rb");
    assert(file != NULL);
    size_t count = fread(bytes, 1, sizeof(bytes) - 1, file);
    assert(!ferror(file));
    assert(fclose(file) == 0);
    assert(count == strlen(text) && !strcmp(bytes, text));
}

static void remove_directory(const char *path) {
#ifdef _WIN32
    wchar_t *wide = sr_win_path(path);
    assert(wide && _wrmdir(wide) == 0);
    free(wide);
#else
    assert(rmdir(path) == 0);
#endif
}

int main(void) {
    // All data is synthetic and belongs to this invocation. UTF-8 escapes keep
    // this test independent of the compiler's source-file locale too.
    const char *unicode = "\xe6\x97\xa5\xe6\x9c\xac-\xf0\x9f\x8e\xae";
    char root[256], deep[1024], file[1200], temporary[1200], missing[1200];
#ifdef _WIN32
    unsigned long pid = GetCurrentProcessId();
#else
    unsigned long pid = (unsigned long)getpid();
#endif
    snprintf(root, sizeof(root), "utf8-fs-%s-%lu", unicode, pid);
    assert(sr_mkdir(root) == 0);
    snprintf(deep, sizeof(deep), "%s", root);
    size_t ends[8];
    for (size_t i = 0; i < 8; ++i) {
        ends[i] = strlen(deep);
        strcat(deep, "/long-directory-with-spaces-and-more-than-forty-characters");
        assert(sr_mkdir(deep) == 0);
    }
    assert(strlen(deep) > 400 && sr_path_is_directory(deep));
    snprintf(file, sizeof(file), "%s/settings-%s.ini", deep, unicode);
    snprintf(temporary, sizeof(temporary), "%s/new-%s.tmp", deep, unicode);
    snprintf(missing, sizeof(missing), "%s/absent.tmp", deep);
    write_text(file, "previous");
    assert(sr_path_exists(file));
    assert(!sr_replace_file(missing, file));
    expect_text(file, "previous");
    write_text(temporary, "replacement");
    assert(sr_replace_file(temporary, file));
    assert(!sr_path_exists(temporary));
    expect_text(file, "replacement");
    assert(sr_remove(file) == 0);
    assert(!sr_path_exists(file));
    // A portable bundle chdirs beside its executable, then writes settings.ini
    // and saves/ by leaf name. On Windows that chdir once left a \\?\ working
    // directory, which sr_win_path rewrote to \\?\UNC\?\C:\..., so every leaf
    // open failed with ENOENT.
    assert(sr_utf8_chdir(root) == 0);
    write_text("settings.ini.tmp", "leaf");
    assert(sr_replace_file("settings.ini.tmp", "settings.ini"));
    expect_text("settings.ini", "leaf");
    assert(sr_remove("settings.ini") == 0);
    assert(sr_utf8_chdir("..") == 0);
#ifdef _WIN32
    // A working directory can still arrive verbatim from a parent process.
    wchar_t *home = _wgetcwd(NULL, 0);
    assert(home && sr_utf8_chdir(root) == 0);
    wchar_t *anchored = _wgetcwd(NULL, 0);
    assert(anchored && wcsncmp(anchored, L"\\\\?\\", 4) != 0);
    free(anchored);
    wchar_t *verbatim = sr_win_path(".");
    assert(verbatim && _wchdir(verbatim) == 0);
    free(verbatim);
    write_text("verbatim.tmp", "verbatim");
    expect_text("verbatim.tmp", "verbatim");
    assert(sr_remove("verbatim.tmp") == 0);
    assert(_wchdir(home) == 0);
    free(home);
#endif
#ifdef _WIN32
    assert(sr_utf8_to_wide("\xff") == NULL);
    wchar_t *wide = sr_utf8_to_wide(unicode);
    char roundtrip[128];
    assert(wide && sr_wide_to_utf8(wide, roundtrip, sizeof(roundtrip)));
    assert(!strcmp(roundtrip, unicode));
    free(wide);
#endif
    for (int i = 7; i >= 0; --i) {
        remove_directory(deep);
        deep[ends[i]] = '\0';
    }
    remove_directory(root);
    puts("PASS: UTF-8/deep paths and safe replacement");
    return 0;
}
