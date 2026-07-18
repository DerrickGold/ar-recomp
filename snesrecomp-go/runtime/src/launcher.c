/*
 * launcher.c — ROM discovery + CRC32 verification + cached path.
 *
 * Public API: snesrecomp_launcher_resolve_rom() in launcher.h.
 *
 * Persists the user's chosen ROM path to <exe_dir>/rom.cfg so that
 * subsequent runs skip the file picker. Designed to be called from
 * the per-game runner's main() before any ROM byte is loaded.
 */
#include "launcher.h"
#include "crc32.h"
#include "sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <commdlg.h>
#  include <direct.h>
#  define snesrecomp_chdir _chdir
#  define snesrecomp_getcwd _getcwd
#  pragma comment(lib, "comdlg32.lib")
#else
#  include <unistd.h>
#  define snesrecomp_chdir chdir
#  define snesrecomp_getcwd getcwd
#endif
#ifdef __APPLE__
#  include <mach-o/dyld.h>
#endif

/* ---- exe-dir helpers ---- */

/* Full path of the running executable. Returns 1 on success. Platforms
 * without a query mechanism (e.g. Switch homebrew) return 0 and callers
 * fall back to cwd-relative behavior. */
static int get_exe_path(char *out, size_t max_len) {
#if defined(_WIN32)
    DWORD n = GetModuleFileNameA(NULL, out, (DWORD)max_len);
    return (n > 0 && n < max_len) ? 1 : 0;
#elif defined(__APPLE__)
    uint32_t size = (uint32_t)max_len;
    return _NSGetExecutablePath(out, &size) == 0 ? 1 : 0;
#elif defined(__linux__)
    /* Inside an AppImage, /proc/self/exe resolves to the binary in the
     * read-only squashfs mount (/tmp/.mount_XXXXXX/usr/bin/<game>), NOT
     * to where the user's .AppImage and config.ini live. The AppImage
     * runtime exports $APPIMAGE = absolute path of the .AppImage file;
     * use it so config.ini / rom.cfg / saves anchor next to the .AppImage,
     * exactly as they anchor next to the .exe on Windows. Without this the
     * exe dir is the read-only mount: anchoring declines (not writable),
     * the config path resolves inside the mount, and the user's config.ini
     * beside the .AppImage is never read — so e.g. Widescreen stays off on
     * Linux no matter what the file says. */
    const char *appimg = getenv("APPIMAGE");
    if (appimg && appimg[0]) {
        size_t len = strlen(appimg);
        if (len >= max_len) return 0;
        memcpy(out, appimg, len + 1);
        return 1;
    }
    ssize_t n = readlink("/proc/self/exe", out, max_len - 1);
    if (n <= 0) return 0;
    out[n] = '\0';
    return 1;
#else
    (void)out; (void)max_len;
    return 0;
#endif
}

/* Directory containing the executable, with trailing separator.
 * Falls back to "./" when the exe path can't be determined. */
static void get_exe_dir(char *out, size_t max_len) {
    char exe_path[1024];
    if (get_exe_path(exe_path, sizeof(exe_path))) {
        char *last_sep = NULL;
        for (char *p = exe_path; *p; p++)
            if (*p == '/' || *p == '\\') last_sep = p;
        if (last_sep) {
            *(last_sep + 1) = '\0';
            snprintf(out, max_len, "%s", exe_path);
            return;
        }
    }
    snprintf(out, max_len, "./");
}

int snesrecomp_abspath(const char *path, char *out, size_t max_len) {
    if (!path || !*path || !out || max_len == 0) return 0;
#ifdef _WIN32
    char tmp[1024];
    if (!_fullpath(tmp, path, sizeof(tmp))) return 0;
    if (strlen(tmp) >= max_len) return 0;
    strcpy(out, tmp);
    return 1;
#else
    if (path[0] == '/') {
        if (strlen(path) >= max_len) return 0;
        strcpy(out, path);
        return 1;
    }
    char cwd[1024];
    if (!snesrecomp_getcwd(cwd, sizeof(cwd))) return 0;
    if (snprintf(out, max_len, "%s/%s", cwd, path) >= (int)max_len) return 0;
    return 1;
#endif
}

int snesrecomp_exe_dir_path(const char *leaf, char *out, size_t max_len) {
    if (!leaf || !out || max_len == 0) return 0;
    char dir[1024];
    get_exe_dir(dir, sizeof(dir));
    /* get_exe_dir falls back to "./" when the exe path is unavailable; in that
     * case the caller is no better off than a bare relative name, so report
     * failure and let it keep its CWD-relative default. */
    if (dir[0] == '.' && (dir[1] == '/' || dir[1] == '\\' || dir[1] == '\0'))
        return 0;
    if (snprintf(out, max_len, "%s%s", dir, leaf) >= (int)max_len) return 0;
    return 1;
}

/* Can we create files in `dir`? Probed by actually creating one —
 * access(W_OK) lies on Windows and inside sandboxed mounts. */
static int dir_is_writable(const char *dir) {
    char probe[1024];
    if (snprintf(probe, sizeof(probe), "%s.snesrecomp_write_probe",
                 dir) >= (int)sizeof(probe))
        return 0;
    FILE *f = fopen(probe, "wb");
    if (!f) return 0;
    fclose(f);
    remove(probe);
    return 1;
}

int snesrecomp_anchor_to_exe_dir(void) {
    char dir[1024];
    get_exe_dir(dir, sizeof(dir));
    if (dir[0] == '.' && (dir[1] == '/' || dir[1] == '\0')) {
        /* Exe path unavailable on this platform — cwd stays authoritative. */
        return 0;
    }
    if (!dir_is_writable(dir)) {
        fprintf(stderr,
                "[Launcher] Executable directory '%s' is not writable; "
                "config and saves stay in the current directory.\n", dir);
        return 0;
    }
    if (snesrecomp_chdir(dir) != 0) {
        fprintf(stderr, "[Launcher] Could not change directory to '%s'.\n", dir);
        return 0;
    }
    /* Make the anchor visible: this is where config.ini / keybinds.ini /
     * rom.cfg / saves are read and written. On an AppImage this prints the
     * folder containing the .AppImage (via $APPIMAGE), which is the single
     * most common confusion ("I edited config.ini but nothing changed"). */
    fprintf(stderr, "[Launcher] Config/saves anchored to '%s'.\n", dir);
    return 1;
}

static void get_rom_cfg_path(char *out, size_t max_len) {
    char dir[512];
    get_exe_dir(dir, sizeof(dir));
    snprintf(out, max_len, "%srom.cfg", dir);
}

/* ---- rom.cfg persistence ---- */

static void rom_cfg_read(char *path_out, size_t max_len) {
    char cfg_path[512];
    get_rom_cfg_path(cfg_path, sizeof(cfg_path));
    FILE *f = fopen(cfg_path, "r");
    if (!f) { path_out[0] = '\0'; return; }
    if (!fgets(path_out, (int)max_len, f)) path_out[0] = '\0';
    fclose(f);
    size_t len = strlen(path_out);
    while (len > 0 && (path_out[len-1] == '\n' || path_out[len-1] == '\r'))
        path_out[--len] = '\0';
}

static void rom_cfg_write(const char *rom_path) {
    char cfg_path[512];
    get_rom_cfg_path(cfg_path, sizeof(cfg_path));
    FILE *f = fopen(cfg_path, "w");
    if (!f) return;
    fprintf(f, "%s\n", rom_path);
    fclose(f);
}

/* ---- File picker ---- */

#ifndef _WIN32
/* Run one shell-wrapped native chooser, read the selected path from its
 * stdout. The command is expected to print a single absolute path and exit
 * 0 on selection, or exit non-zero / print nothing on cancel or when the
 * tool is absent (each command is gated on `command -v <tool>`). Returns 1
 * and fills `out` only on a real selection. */
static int run_picker_cmd(const char *cmd, char *out, size_t max_len) {
    FILE *p = popen(cmd, "r");
    if (!p) return 0;
    char buf[1024];
    buf[0] = '\0';
    char *got = fgets(buf, sizeof(buf), p);
    int rc = pclose(p);
    if (!got) return 0;                 /* nothing printed: cancel / tool absent */
    size_t n = strlen(buf);
    while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r')) buf[--n] = '\0';
    if (rc != 0 || n == 0 || n >= max_len) return 0;
    memcpy(out, buf, n + 1);
    return 1;
}
#endif

static int pick_rom_file(char *out, size_t max_len) {
#ifdef _WIN32
    OPENFILENAMEA ofn;
    memset(&ofn, 0, sizeof(ofn));
    out[0] = '\0';
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = NULL;
    ofn.lpstrFilter = "SNES ROMs (*.sfc;*.smc)\0*.sfc;*.smc\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile   = out;
    ofn.nMaxFile    = (DWORD)max_len;
    ofn.lpstrTitle  = "Select SNES ROM";
    /* OFN_NOCHANGEDIR: the common dialog otherwise changes the process CWD to
     * the browsed folder, which defeats snesrecomp_anchor_to_exe_dir() and makes
     * later CWD-relative writes (config.ini, saves/) land next to the picked ROM
     * instead of next to the exe. */
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY
                    | OFN_NOCHANGEDIR;
    return GetOpenFileNameA(&ofn) ? 1 : 0;
#else
    /* Native graphical chooser, in preference order. Each entry is gated on
     * `command -v <tool>` so an absent tool falls through to the next, and
     * stderr is muted so "command not found" never reaches the user. The
     * caller still has rom.cfg (cached path) and the positional ROM arg as
     * fallbacks when none of these exist (e.g. Steam Deck Gaming Mode, or a
     * headless box). No new link-time dependencies — all via popen. */
    out[0] = '\0';
    static const char *const pickers[] = {
        /* zenity — GTK / GNOME, also the most common standalone tool. */
        "command -v zenity >/dev/null 2>&1 && "
        "zenity --file-selection --title='Select SNES ROM' "
        "--file-filter='SNES ROMs (.sfc .smc) | *.sfc *.smc *.SFC *.SMC' "
        "--file-filter='All files | *' 2>/dev/null",
        /* kdialog — KDE / Steam Deck Desktop Mode. */
        "command -v kdialog >/dev/null 2>&1 && "
        "kdialog --getopenfilename \"${HOME:-/}\" "
        "'*.sfc *.smc *.SFC *.SMC|SNES ROMs' 2>/dev/null",
        /* qarma — Qt drop-in for zenity. */
        "command -v qarma >/dev/null 2>&1 && "
        "qarma --file-selection --title='Select SNES ROM' 2>/dev/null",
        /* macOS — AppleScript chooser returning a POSIX path. */
        "command -v osascript >/dev/null 2>&1 && "
        "osascript -e 'POSIX path of (choose file with prompt \"Select SNES ROM\")' "
        "2>/dev/null",
    };
    for (size_t i = 0; i < sizeof(pickers) / sizeof(pickers[0]); i++)
        if (run_picker_cmd(pickers[i], out, max_len))
            return 1;
    fprintf(stderr,
            "[Launcher] No ROM specified and no graphical file chooser found "
            "(install zenity or kdialog), and no cached rom.cfg.\n"
            "Pass the ROM path as the first argument.\n");
    return 0;
#endif
}

/* ---- CRC32 verification ---- */

/* Returns 1 if the ROM at `path` matches expected_crc (or expected_crc==0).
 * If the file is 512 bytes longer than a multiple of 32KB, treat the first
 * 512 bytes as an SMC copier header and skip them for CRC purposes. */
static int verify_rom(const char *path, uint32_t expected_crc) {
    if (expected_crc == 0) return 1;

    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "[Launcher] Cannot open '%s'\n", path);
        return 0;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if (sz <= 0) { fclose(f); return 0; }

    uint8_t *data = (uint8_t *)malloc((size_t)sz);
    if (!data) { fclose(f); return 0; }
    size_t read = fread(data, 1, (size_t)sz, f);
    fclose(f);
    if (read != (size_t)sz) { free(data); return 0; }

    /* Strip 512-byte SMC header if present. */
    size_t hdr = ((size_t)sz % 1024 == 512) ? 512 : 0;
    uint32_t actual = crc32_compute(data + hdr, (size_t)sz - hdr);
    free(data);

    if (actual != expected_crc) {
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "ROM CRC32 mismatch.\n\nExpected: %08X\nGot:      %08X\n\n"
                 "Please select the correct ROM file.",
                 expected_crc, actual);
        fprintf(stderr, "[Launcher] %s\n", msg);
#ifdef _WIN32
        MessageBoxA(NULL, msg, "Wrong ROM", MB_ICONWARNING | MB_OK);
#endif
        return 0;
    }
    return 1;
}

/* ---- SHA-256 verification ---- */

/* Returns 1 if the ROM at `path` matches expected_sha256 (or NULL).
 * Same SMC-header-strip rule as CRC32. */
static int verify_rom_sha256(const char *path, const uint8_t *expected_sha256) {
    if (!expected_sha256) return 1;

    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "[Launcher] Cannot open '%s'\n", path);
        return 0;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if (sz <= 0) { fclose(f); return 0; }

    uint8_t *data = (uint8_t *)malloc((size_t)sz);
    if (!data) { fclose(f); return 0; }
    size_t read = fread(data, 1, (size_t)sz, f);
    fclose(f);
    if (read != (size_t)sz) { free(data); return 0; }

    size_t hdr = ((size_t)sz % 1024 == 512) ? 512 : 0;
    uint8_t actual[32];
    sha256_compute(data + hdr, (size_t)sz - hdr, actual);
    free(data);

    if (memcmp(actual, expected_sha256, 32) != 0) {
        char exp_hex[65], act_hex[65];
        for (int i = 0; i < 32; i++) {
            snprintf(exp_hex + i*2, 3, "%02x", expected_sha256[i]);
            snprintf(act_hex + i*2, 3, "%02x", actual[i]);
        }
        char msg[512];
        snprintf(msg, sizeof(msg),
                 "ROM SHA-256 mismatch.\n\nExpected:\n%s\n\nGot:\n%s\n\n"
                 "Please select the correct ROM file.",
                 exp_hex, act_hex);
        fprintf(stderr, "[Launcher] %s\n", msg);
#ifdef _WIN32
        MessageBoxA(NULL, msg, "Wrong ROM", MB_ICONWARNING | MB_OK);
#endif
        return 0;
    }
    return 1;
}

/* Compute the ROM's SHA-256 (auto-stripping a 512-byte copier header) and
 * return the index of the first matching entry in `hashes`, or -1 if none
 * match. Quiet (no dialog) — callers decide how to treat a non-match. On
 * non-match it logs the computed hash so it can be added if intended. */
static int rom_sha256_match(const char *path,
                            const uint8_t (*hashes)[32], size_t n) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "[Launcher] Cannot open '%s'\n", path); return -1; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if (sz <= 0) { fclose(f); return -1; }
    uint8_t *data = (uint8_t *)malloc((size_t)sz);
    if (!data) { fclose(f); return -1; }
    size_t rd = fread(data, 1, (size_t)sz, f);
    fclose(f);
    if (rd != (size_t)sz) { free(data); return -1; }

    size_t hdr = ((size_t)sz % 1024 == 512) ? 512 : 0;
    uint8_t actual[32];
    sha256_compute(data + hdr, (size_t)sz - hdr, actual);
    free(data);

    for (size_t i = 0; i < n; i++)
        if (memcmp(actual, hashes[i], 32) == 0) return (int)i;

    char hex[65];
    for (int i = 0; i < 32; i++) snprintf(hex + i*2, 3, "%02x", actual[i]);
    fprintf(stderr, "[Launcher] ROM SHA-256 %s matches no known hash.\n", hex);
    return -1;
}

/* ---- Public ---- */

int snesrecomp_launcher_resolve_rom(int argc, char **argv,
                                    char *out_path, size_t max_len,
                                    uint32_t expected_crc) {
    out_path[0] = '\0';

    /* (1) argv[1] override (back-compat with command-line invocation).
     * Absolutized so the rom.cfg cache stays valid however the next
     * launch's cwd differs from this one's. */
    if (argc >= 2 && argv[1] && argv[1][0] != '-' && argv[1][0] != '\0') {
        if (!snesrecomp_abspath(argv[1], out_path, max_len)) {
            strncpy(out_path, argv[1], max_len - 1);
            out_path[max_len - 1] = '\0';
        }
        if (expected_crc != 0 && !verify_rom(out_path, expected_crc)) {
            fprintf(stderr, "[Launcher] Warning: CRC mismatch for '%s' — continuing anyway\n", out_path);
        }
        rom_cfg_write(out_path);
        printf("[Launcher] ROM: %s\n", out_path);
        return 1;
    }

    /* (2) Cached path from rom.cfg. */
    rom_cfg_read(out_path, max_len);

    /* (3) File picker loop until user provides a valid (or skip-CRC) ROM. */
    for (;;) {
        if (out_path[0] == '\0') {
            if (!pick_rom_file(out_path, max_len)) {
                fprintf(stderr, "[Launcher] No ROM selected — exiting.\n");
                out_path[0] = '\0';
                return 0;
            }
        }
        if (verify_rom(out_path, expected_crc)) {
            rom_cfg_write(out_path);
            printf("[Launcher] ROM: %s\n", out_path);
            return 1;
        }
        /* Wrong ROM — clear and re-prompt. */
        out_path[0] = '\0';
    }
}

int snesrecomp_launcher_resolve_rom_sha256(int argc, char **argv,
                                           char *out_path, size_t max_len,
                                           const uint8_t *expected_sha256) {
    out_path[0] = '\0';

    /* (1) argv[1] override (back-compat with command-line invocation).
     * Absolutized so the rom.cfg cache stays valid however the next
     * launch's cwd differs from this one's. */
    if (argc >= 2 && argv[1] && argv[1][0] != '-' && argv[1][0] != '\0') {
        if (!snesrecomp_abspath(argv[1], out_path, max_len)) {
            strncpy(out_path, argv[1], max_len - 1);
            out_path[max_len - 1] = '\0';
        }
        if (expected_sha256 && !verify_rom_sha256(out_path, expected_sha256)) {
            fprintf(stderr, "[Launcher] Warning: SHA-256 mismatch for '%s' — continuing anyway\n", out_path);
        }
        rom_cfg_write(out_path);
        printf("[Launcher] ROM: %s\n", out_path);
        return 1;
    }

    /* (2) Cached path from rom.cfg. */
    rom_cfg_read(out_path, max_len);

    /* (3) File picker loop until user provides a valid (or skip-hash) ROM. */
    for (;;) {
        if (out_path[0] == '\0') {
            if (!pick_rom_file(out_path, max_len)) {
                fprintf(stderr, "[Launcher] No ROM selected — exiting.\n");
                out_path[0] = '\0';
                return 0;
            }
        }
        if (verify_rom_sha256(out_path, expected_sha256)) {
            rom_cfg_write(out_path);
            printf("[Launcher] ROM: %s\n", out_path);
            return 1;
        }
        /* Wrong ROM — clear and re-prompt. */
        out_path[0] = '\0';
    }
}

/* Like resolve_rom_sha256 but permissive: a ROM whose hash is in `hashes`
 * loads silently; ANY other readable ROM also loads, but with a warning
 * (so romhacks / other regions still work, just flagged). Only a missing/
 * unreadable file or a cancelled picker re-prompts. `hashes` is an array of
 * n_hashes 32-byte digests; pass n_hashes==0 to accept anything silently. */
int snesrecomp_launcher_resolve_rom_sha256_multi(int argc, char **argv,
                                                 char *out_path, size_t max_len,
                                                 const uint8_t (*hashes)[32],
                                                 size_t n_hashes) {
    out_path[0] = '\0';

    /* (1) argv[1] override. */
    if (argc >= 2 && argv[1] && argv[1][0] != '-' && argv[1][0] != '\0') {
        if (!snesrecomp_abspath(argv[1], out_path, max_len)) {
            strncpy(out_path, argv[1], max_len - 1);
            out_path[max_len - 1] = '\0';
        }
        if (n_hashes && rom_sha256_match(out_path, hashes, n_hashes) < 0)
            fprintf(stderr, "[Launcher] Warning: '%s' is not a recognized ROM "
                            "for this build - loading anyway; the game may "
                            "misbehave.\n", out_path);
        rom_cfg_write(out_path);
        printf("[Launcher] ROM: %s\n", out_path);
        return 1;
    }

    /* (2) Cached path from rom.cfg. */
    rom_cfg_read(out_path, max_len);

    /* (3) Picker. Accept any readable ROM; warn if unrecognized. Only a
     * cancelled picker (or an unreadable cached path) re-prompts. */
    for (;;) {
        if (out_path[0] == '\0') {
            if (!pick_rom_file(out_path, max_len)) {
                fprintf(stderr, "[Launcher] No ROM selected — exiting.\n");
                out_path[0] = '\0';
                return 0;
            }
        }
        FILE *probe = fopen(out_path, "rb");
        if (!probe) {                 /* stale cache / vanished file */
            fprintf(stderr, "[Launcher] '%s' is not readable — pick again.\n", out_path);
            out_path[0] = '\0';
            continue;
        }
        fclose(probe);
        if (n_hashes && rom_sha256_match(out_path, hashes, n_hashes) < 0)
            fprintf(stderr, "[Launcher] Warning: '%s' is not a recognized ROM "
                            "for this build - loading anyway; the game may "
                            "misbehave.\n", out_path);
        rom_cfg_write(out_path);
        printf("[Launcher] ROM: %s\n", out_path);
        return 1;
    }
}
