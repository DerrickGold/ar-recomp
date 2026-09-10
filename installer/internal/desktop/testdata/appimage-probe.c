// ROM-free substitute for the game. Exercise real SDL/font dependencies and
// report every loaded library so an installed host copy cannot mask an omission.
#define _GNU_SOURCE
#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>
#include <link.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/vfs.h>
#include <unistd.h>

extern int middle(void);

static int report_library(struct dl_phdr_info *info, size_t size, void *output) {
    (void)size;
    if (info->dlpi_name[0]) fprintf(output, "library=%s\n", info->dlpi_name);
    return 0;
}

int main(int argc, char **argv) {
    char *cwd = getcwd(NULL, 0);
    const char *data = getenv("AR_USER_DATA_DIR");
    const char *libraries = getenv("LD_LIBRARY_PATH");
    if (!cwd || !data || strcmp(cwd, data) || !libraries || middle() != 42) return 10;
    if (argc != 4 || argv[1][0] != '/' || strcmp(argv[2], "--config")) return 11;
    char *config = NULL;
    if (asprintf(&config, "%s/config.ini", cwd) < 0 || strcmp(config, argv[3])) return 12;
    free(config);
    FILE *rom = fopen(argv[1], "rb");
    char content[14] = {0};
    if (!rom || fread(content, 1, 13, rom) != 13 || strcmp(content, "synthetic ROM")) return 13;
    fclose(rom);
    if (!SDL_Init(0) || !TTF_Init()) {
        fprintf(stderr, "initialize SDL/font library: %s\n", SDL_GetError());
        return 14;
    }
    TTF_Font *font = TTF_OpenFont("game-assets/fonts/noto/NotoSans-SemiCondensedExtraBold.ttf", 20);
    if (!font) {
        fprintf(stderr, "open packaged font: %s\n", SDL_GetError());
        return 15;
    }
    FILE *report = fopen("appimage-probe.txt", "w");
    if (!report) return 16;
    fprintf(report, "cwd=%s\nrom=%s\nprivate=%.*s\n", cwd, argv[1],
            (int)strcspn(libraries, ":"), libraries);
    struct statfs rom_fs;
    if (statfs(argv[1], &rom_fs)) return 18;
    fprintf(report, "filesystem=%lx\n", (unsigned long)rom_fs.f_type);
    dl_iterate_phdr(report_library, report);
    if (fclose(report)) return 17;
    TTF_CloseFont(font);
    TTF_Quit();
    SDL_Quit();
    free(cwd);
    return 0;
}
