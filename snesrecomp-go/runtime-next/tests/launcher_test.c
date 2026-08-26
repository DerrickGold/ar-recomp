#include "crc32.h"
#include "launcher.h"
#include "sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef LAUNCHER_TEST_ROM
#define LAUNCHER_TEST_ROM "runtime-next-launcher.sfc"
#endif

static int failures;

static void check(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "runtime-next launcher failed: %s\n", message);
        ++failures;
    }
}

static int make_fixture(void) {
    FILE *file = fopen(LAUNCHER_TEST_ROM, "wb");
    unsigned index;
    if (file == NULL) {
        return 0;
    }
    for (index = 0u; index < 1024u; ++index) {
        fputc((int)(index * 37u + 11u) & 0xff, file);
    }
    return fclose(file) == 0;
}

static int load_fixture(uint8_t data[1024]) {
    FILE *file = fopen(LAUNCHER_TEST_ROM, "rb");
    int okay = file != NULL && fread(data, 1u, 1024u, file) == 1024u;
    if (file != NULL) {
        fclose(file);
    }
    return okay;
}

int main(void) {
    char *arguments[] = {(char *)"launcher-test", (char *)LAUNCHER_TEST_ROM};
    char absolute[4096];
    char resolved[4096];
    char beside_executable[4096];
    uint8_t contents[1024];
    uint8_t digest[32];
    uint8_t hashes[2][32] = {{0}};
    uint32_t crc;
    check(make_fixture() && load_fixture(contents), "fixture creation");
    crc = crc32_compute(contents, sizeof(contents));
    sha256_compute(contents, sizeof(contents), digest);
    memcpy(hashes[1], digest, sizeof(digest));
    check(snesrecomp_abspath(LAUNCHER_TEST_ROM, absolute, sizeof(absolute)) &&
              absolute[0] != '\0',
          "absolute path resolution");
    check(snesrecomp_launcher_resolve_rom(2, arguments, resolved,
                                          sizeof(resolved), crc) &&
              strcmp(resolved, absolute) == 0,
          "CRC command-line resolution");
    check(snesrecomp_launcher_resolve_rom_sha256(2, arguments, resolved,
                                                 sizeof(resolved), digest) &&
              strcmp(resolved, absolute) == 0,
          "SHA-256 command-line resolution");
    check(snesrecomp_launcher_resolve_rom_sha256_multi(
              2, arguments, resolved, sizeof(resolved), hashes, 2u) &&
              strcmp(resolved, absolute) == 0,
          "multi-hash command-line resolution");
    check(snesrecomp_exe_dir_path("asset.bin", beside_executable,
                                  sizeof(beside_executable)) &&
              strstr(beside_executable, "asset.bin") != NULL,
          "executable-relative path");
    remove(LAUNCHER_TEST_ROM);
    return failures == 0 ? 0 : 1;
}
