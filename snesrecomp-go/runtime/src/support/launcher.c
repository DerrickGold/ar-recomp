#include "snesrecomp/host/launcher.h"

#include "snesrecomp/support/crc32.h"
#include "sha256.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#include <direct.h>
#define sr_chdir _chdir
#define sr_getcwd _getcwd
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <unistd.h>
#define sr_chdir chdir
#define sr_getcwd getcwd
#else
#include <unistd.h>
#define sr_chdir chdir
#define sr_getcwd getcwd
#endif

enum { kPathCapacity = 4096 };

static int copy_path(char *output, size_t capacity, const char *source) {
    size_t length;
    if (output == NULL || source == NULL || capacity == 0u) {
        return 0;
    }
    length = strlen(source);
    if (length >= capacity) {
        return 0;
    }
    memcpy(output, source, length + 1u);
    return 1;
}

static int executable_path(char *output, size_t capacity) {
    if (output == NULL || capacity < 2u) {
        return 0;
    }
#if defined(_WIN32)
    {
        DWORD length;
        if (capacity > (size_t)UINT32_MAX) {
            capacity = UINT32_MAX;
        }
        length = GetModuleFileNameA(NULL, output, (DWORD)capacity);
        return length != 0u && length < capacity;
    }
#elif defined(__APPLE__)
    {
        uint32_t length = capacity > UINT32_MAX ? UINT32_MAX : (uint32_t)capacity;
        return _NSGetExecutablePath(output, &length) == 0;
    }
#elif defined(__linux__)
    {
        const char *app_image = getenv("APPIMAGE");
        ssize_t length;
        if (app_image != NULL && app_image[0] != '\0') {
            return copy_path(output, capacity, app_image);
        }
        length = readlink("/proc/self/exe", output, capacity - 1u);
        if (length <= 0 || (size_t)length >= capacity) {
            return 0;
        }
        output[length] = '\0';
        return 1;
    }
#else
    (void)output;
    (void)capacity;
    return 0;
#endif
}

static int executable_directory(char *output, size_t capacity) {
    char *path = (char *)malloc(kPathCapacity);
    char *separator = NULL;
    char *cursor;
    int success;
    if (path == NULL || !executable_path(path, kPathCapacity)) {
        free(path);
        return 0;
    }
    for (cursor = path; *cursor != '\0'; ++cursor) {
        if (*cursor == '/' || *cursor == '\\') {
            separator = cursor;
        }
    }
    if (separator == NULL) {
        free(path);
        return 0;
    }
    separator[1] = '\0';
    success = copy_path(output, capacity, path);
    free(path);
    return success;
}

int snesrecomp_abspath(const char *path, char *output, size_t capacity) {
    if (path == NULL || path[0] == '\0' || output == NULL || capacity == 0u) {
        return 0;
    }
#if defined(_WIN32)
    {
        char *resolved = (char *)malloc(kPathCapacity);
        int success;
        if (resolved == NULL ||
            _fullpath(resolved, path, kPathCapacity) == NULL) {
            free(resolved);
            return 0;
        }
        success = copy_path(output, capacity, resolved);
        free(resolved);
        return success;
    }
#else
    if (path[0] == '/') {
        return copy_path(output, capacity, path);
    }
    {
        char *working_directory = (char *)malloc(kPathCapacity);
        int length;
        if (working_directory == NULL ||
            sr_getcwd(working_directory, kPathCapacity) == NULL) {
            free(working_directory);
            return 0;
        }
        length = snprintf(output, capacity, "%s/%s", working_directory, path);
        free(working_directory);
        return length >= 0 && (size_t)length < capacity;
    }
#endif
}

int snesrecomp_exe_dir_path(const char *leaf, char *output, size_t capacity) {
    char *directory = (char *)malloc(kPathCapacity);
    int length;
    if (directory == NULL || leaf == NULL || output == NULL || capacity == 0u ||
        !executable_directory(directory, kPathCapacity)) {
        free(directory);
        return 0;
    }
    length = snprintf(output, capacity, "%s%s", directory, leaf);
    free(directory);
    return length >= 0 && (size_t)length < capacity;
}

static int directory_is_writable(const char *directory) {
    char *probe = (char *)malloc(kPathCapacity);
    FILE *file;
    int length;
    if (probe == NULL) return 0;
    length = snprintf(probe, kPathCapacity,
                      "%s.snesrecomp-write-probe", directory);
    if (length < 0 || length >= kPathCapacity) {
        free(probe);
        return 0;
    }
    file = fopen(probe, "wb");
    if (file == NULL) {
        free(probe);
        return 0;
    }
    if (fclose(file) != 0) {
        remove(probe);
        free(probe);
        return 0;
    }
    if (remove(probe) != 0) {
        free(probe);
        return 0;
    }
    free(probe);
    return 1;
}

int snesrecomp_anchor_to_exe_dir(void) {
    char *directory = (char *)malloc(kPathCapacity);
    if (directory == NULL ||
        !executable_directory(directory, kPathCapacity)) {
        free(directory);
        return 0;
    }
    if (!directory_is_writable(directory)) {
        fprintf(stderr, "[Launcher] Executable directory is not writable: %s\n",
                directory);
        free(directory);
        return 0;
    }
    if (sr_chdir(directory) != 0) {
        fprintf(stderr, "[Launcher] Could not use executable directory: %s\n",
                directory);
        free(directory);
        return 0;
    }
    fprintf(stderr, "[Launcher] Config/saves anchored to '%s'.\n", directory);
    free(directory);
    return 1;
}

static int rom_config_path(char *output, size_t capacity) {
    if (snesrecomp_exe_dir_path("rom.cfg", output, capacity)) {
        return 1;
    }
    return copy_path(output, capacity, "rom.cfg");
}

static void read_cached_rom(char *output, size_t capacity) {
    char *path = (char *)malloc(kPathCapacity);
    FILE *file;
    size_t length;
    output[0] = '\0';
    if (path == NULL || !rom_config_path(path, kPathCapacity) ||
        (file = fopen(path, "rb")) == NULL) {
        free(path);
        return;
    }
    free(path);
    if (fgets(output, (int)capacity, file) == NULL) {
        output[0] = '\0';
    }
    fclose(file);
    length = strlen(output);
    while (length != 0u &&
           (output[length - 1u] == '\n' || output[length - 1u] == '\r')) {
        output[--length] = '\0';
    }
}

static void cache_rom(const char *rom_path) {
    char *path = (char *)malloc(kPathCapacity);
    FILE *file;
    if (path == NULL || !rom_config_path(path, kPathCapacity) ||
        (file = fopen(path, "wb")) == NULL) {
        free(path);
        return;
    }
    free(path);
    if (fputs(rom_path, file) >= 0) {
        fputc('\n', file);
    }
    fclose(file);
}

static uint8_t *read_rom_payload(const char *path, size_t *size) {
    FILE *file;
    long end;
    size_t bytes;
    size_t header;
    uint8_t *storage;
    uint8_t *payload;
    *size = 0u;
    file = fopen(path, "rb");
    if (file == NULL || fseek(file, 0, SEEK_END) != 0 ||
        (end = ftell(file)) <= 0 || fseek(file, 0, SEEK_SET) != 0) {
        if (file != NULL) {
            fclose(file);
        }
        return NULL;
    }
    bytes = (size_t)end;
    storage = (uint8_t *)malloc(bytes);
    if (storage == NULL) {
        fclose(file);
        return NULL;
    }
    if (fread(storage, 1u, bytes, file) != bytes || fclose(file) != 0) {
        free(storage);
        return NULL;
    }
    header = bytes % 1024u == 512u ? 512u : 0u;
    if (header == 0u) {
        *size = bytes;
        return storage;
    }
    payload = (uint8_t *)malloc(bytes - header);
    if (payload == NULL) {
        free(storage);
        return NULL;
    }
    memcpy(payload, storage + header, bytes - header);
    free(storage);
    *size = bytes - header;
    return payload;
}

static int verify_crc(const char *path, uint32_t expected) {
    uint8_t *payload;
    size_t size;
    uint32_t actual;
    if (expected == 0u) {
        return 1;
    }
    payload = read_rom_payload(path, &size);
    if (payload == NULL) {
        return 0;
    }
    actual = crc32_compute(payload, size);
    free(payload);
    if (actual != expected) {
        fprintf(stderr, "[Launcher] ROM CRC32 mismatch: expected %08X, got %08X.\n",
                expected, actual);
        return 0;
    }
    return 1;
}

static int compute_sha256(const char *path, uint8_t digest[32]) {
    uint8_t *payload;
    size_t size;
    payload = read_rom_payload(path, &size);
    if (payload == NULL) {
        return 0;
    }
    sha256_compute(payload, size, digest);
    free(payload);
    return 1;
}

static int verify_sha256(const char *path, const uint8_t *expected) {
    uint8_t actual[32];
    if (expected == NULL) {
        return 1;
    }
    if (!compute_sha256(path, actual)) {
        return 0;
    }
    if (memcmp(actual, expected, sizeof(actual)) != 0) {
        fprintf(stderr, "[Launcher] ROM SHA-256 mismatch.\n");
        return 0;
    }
    return 1;
}

static int matching_sha256(const char *path, const uint8_t (*hashes)[32],
                           size_t hash_count) {
    uint8_t actual[32];
    size_t index;
    if (!compute_sha256(path, actual)) {
        return -1;
    }
    for (index = 0u; index < hash_count; ++index) {
        if (memcmp(actual, hashes[index], sizeof(actual)) == 0) {
            return (int)index;
        }
    }
    return -1;
}

#if !defined(_WIN32)
static int command_picker(const char *command, char *output, size_t capacity) {
    FILE *pipe = popen(command, "r");
    int status;
    size_t length;
    if (pipe == NULL) {
        return 0;
    }
    if (output == NULL || capacity == 0u) {
        (void)pclose(pipe);
        return 0;
    }
    if (capacity > (size_t)INT_MAX) capacity = (size_t)INT_MAX;
    output[0] = '\0';
    if (fgets(output, (int)capacity, pipe) == NULL) {
        output[0] = '\0';
    }
    status = pclose(pipe);
    length = strlen(output);
    while (length != 0u &&
           (output[length - 1u] == '\n' || output[length - 1u] == '\r')) {
        output[--length] = '\0';
    }
    return status == 0 && output[0] != '\0';
}
#endif

static int pick_rom(char *output, size_t capacity) {
    const char *environment = getenv("SNESRECOMP_ROM");
    if (environment != NULL && environment[0] != '\0') {
        return copy_path(output, capacity, environment);
    }
#if defined(_WIN32)
    {
        OPENFILENAMEA dialog;
        if (capacity > UINT32_MAX) {
            capacity = UINT32_MAX;
        }
        memset(&dialog, 0, sizeof(dialog));
        output[0] = '\0';
        dialog.lStructSize = sizeof(dialog);
        dialog.lpstrFilter = "SNES ROMs (*.sfc;*.smc)\0*.sfc;*.smc\0All Files (*.*)\0*.*\0";
        dialog.lpstrFile = output;
        dialog.nMaxFile = (DWORD)capacity;
        dialog.lpstrTitle = "Select SNES ROM";
        dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST |
                       OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
        return GetOpenFileNameA(&dialog) != 0;
    }
#elif defined(__APPLE__)
    return command_picker(
        "osascript -e 'POSIX path of (choose file with prompt \"Select SNES ROM\")' 2>/dev/null",
        output, capacity);
#elif defined(__linux__)
    if (command_picker(
            "command -v zenity >/dev/null 2>&1 && zenity --file-selection --title='Select SNES ROM' --file-filter='SNES ROMs | *.sfc *.smc *.SFC *.SMC' 2>/dev/null",
            output, capacity)) {
        return 1;
    }
    return command_picker(
        "command -v kdialog >/dev/null 2>&1 && kdialog --getopenfilename / '*.sfc *.smc *.SFC *.SMC|SNES ROMs' 2>/dev/null",
        output, capacity);
#else
    (void)output;
    (void)capacity;
    return 0;
#endif
}

typedef int (*RomVerifier)(const char *path, const void *expected);

static int crc_verifier(const char *path, const void *expected) {
    return verify_crc(path, *(const uint32_t *)expected);
}

static int sha_verifier(const char *path, const void *expected) {
    return verify_sha256(path, (const uint8_t *)expected);
}

static int resolve_strict(int argc, char **argv, char *output, size_t capacity,
                          RomVerifier verifier, const void *expected,
                          const char *kind) {
    int command_line;
    if (output == NULL || capacity == 0u) {
        return 0;
    }
    output[0] = '\0';
    command_line = argc >= 2 && argv != NULL && argv[1] != NULL &&
                   argv[1][0] != '\0' && argv[1][0] != '-';
    if (command_line) {
        if (!snesrecomp_abspath(argv[1], output, capacity) &&
            !copy_path(output, capacity, argv[1])) {
            return 0;
        }
        if (!verifier(output, expected)) {
            fprintf(stderr, "[Launcher] Warning: %s mismatch for '%s'; continuing.\n",
                    kind, output);
        }
        cache_rom(output);
        return 1;
    }
    read_cached_rom(output, capacity);
    for (;;) {
        if (output[0] == '\0' && !pick_rom(output, capacity)) {
            return 0;
        }
        if (verifier(output, expected)) {
            cache_rom(output);
            return 1;
        }
        output[0] = '\0';
    }
}

int snesrecomp_launcher_resolve_rom(int argc, char **argv,
                                    char *output, size_t capacity,
                                    uint32_t expected_crc) {
    return resolve_strict(argc, argv, output, capacity, crc_verifier,
                          &expected_crc, "CRC32");
}

int snesrecomp_launcher_resolve_rom_sha256(int argc, char **argv,
                                           char *output, size_t capacity,
                                           const uint8_t *expected_sha256) {
    return resolve_strict(argc, argv, output, capacity, sha_verifier,
                          expected_sha256, "SHA-256");
}

int snesrecomp_launcher_resolve_rom_sha256_multi(int argc, char **argv,
                                                 char *output, size_t capacity,
                                                 const uint8_t (*hashes)[32],
                                                 size_t hash_count) {
    int command_line;
    if (output == NULL || capacity == 0u || (hash_count != 0u && hashes == NULL)) {
        return 0;
    }
    output[0] = '\0';
    command_line = argc >= 2 && argv != NULL && argv[1] != NULL &&
                   argv[1][0] != '\0' && argv[1][0] != '-';
    if (command_line) {
        if (!snesrecomp_abspath(argv[1], output, capacity) &&
            !copy_path(output, capacity, argv[1])) {
            return 0;
        }
    } else {
        read_cached_rom(output, capacity);
        if (output[0] == '\0' && !pick_rom(output, capacity)) {
            return 0;
        }
    }
    if (hash_count != 0u && matching_sha256(output, hashes, hash_count) < 0) {
        fprintf(stderr, "[Launcher] Warning: unrecognized ROM '%s'; continuing.\n",
                output);
    }
    cache_rom(output);
    return 1;
}
