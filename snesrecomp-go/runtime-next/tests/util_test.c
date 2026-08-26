#include "crc32.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

static void check(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "runtime-next util failed: %s\n", message);
        ++failures;
    }
}

static void put_le24(uint8_t *data, uint32_t value) {
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8);
    data[2] = (uint8_t)(value >> 16);
}

static void put_le32(uint8_t *data, uint32_t value) {
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8);
    data[2] = (uint8_t)(value >> 16);
    data[3] = (uint8_t)(value >> 24);
}

static void test_strings(void) {
    char delimited[] = "  first,second";
    char *delim_cursor = delimited;
    char lines[] = "  alpha  # note\r\n beta\t\n";
    char *line_cursor = lines;
    char words[] = " \"hello world\" tail";
    char *word_cursor = words;
    char pair[] = "key \t=  value";
    char *owned = NULL;
    char *path;
    bool parsed = false;
    check(strcmp(NextDelim(&delim_cursor, ','), "first") == 0 &&
              strcmp(NextDelim(&delim_cursor, ','), "second") == 0,
          "delimiter parsing");
    check(strcmp(NextLineStripComments(&line_cursor), "alpha") == 0 &&
              strcmp(NextLineStripComments(&line_cursor), "beta") == 0,
          "line and comment parsing");
    check(strcmp(NextPossiblyQuotedString(&word_cursor), "hello world") == 0 &&
              strcmp(NextPossiblyQuotedString(&word_cursor), "tail") == 0,
          "quoted token parsing");
    check(strcmp(SplitKeyValue(pair), "value") == 0 && strcmp(pair, "key") == 0,
          "key/value parsing");
    check(StringEqualsNoCase("TrUe", "true") &&
              strcmp(StringStartsWithNoCase("AssetPath", "asset"), "Path") == 0,
          "ASCII case comparison");
    check(ParseBool("YES", &parsed) && parsed && ParseBool("off", &parsed) &&
              !parsed && !ParseBool("maybe", &parsed),
          "boolean parsing");
    check(strcmp(SkipPrefix("prefix-rest", "prefix-"), "rest") == 0 &&
              SkipPrefix("short", "longer") == NULL,
          "prefix parsing");
    StrSet(&owned, "asset");
    check(owned != NULL && strcmp(owned, "asset") == 0, "owned string set");
    StrSet(&owned, NULL);
    check(owned == NULL, "owned string clear");
    path = ReplaceFilenameWithNewPath("dir/sub/file.sfc", "track.wav");
    check(path != NULL && strcmp(path, "dir/sub/track.wav") == 0,
          "path replacement");
    free(path);
}

static void test_packed_tables(void) {
    /* Two 16-bit offsets, three payload slices, then descriptor count=2. */
    static const uint8_t indexed[] = {
        2u, 0u, 5u, 0u, 'a', 'b', 'c', 'd', 'e', 'f', 2u, 0x80u,
    };
    uint8_t addressed[32] = {0};
    MemBlk item;
    put_le24(addressed + 2u, 0x1000u);
    put_le24(addressed + 5u, 0x2000u);
    put_le24(addressed + 8u, 14u);
    put_le24(addressed + 11u, 18u);
    addressed[14] = 0xA0u;
    addressed[15] = 0xA1u;
    addressed[18] = 0xB0u;
    addressed[19] = 0xB1u;
    addressed[0] = 2u;
    item = FindIndexInMemblk((MemBlk){indexed, sizeof(indexed)}, 1u);
    check(item.size == 3u && memcmp(item.ptr, "cde", 3u) == 0,
          "indexed packed-table lookup");
    check(FindIndexInMemblk((MemBlk){indexed, sizeof(indexed)}, 3u).ptr == NULL,
          "indexed packed-table range rejection");
    check(FindAddrInMemblk((MemBlk){addressed, sizeof(addressed)}, 0x1001u) ==
              addressed + 15u,
          "address packed-table lookup");
    check(FindAddrInMemblk((MemBlk){addressed, sizeof(addressed)}, 0x0fffu) == NULL,
          "address packed-table lower-bound rejection");
}

static void test_bps(void) {
    static const uint8_t source[] = {'a', 'b', 'c'};
    uint8_t patch[23] = {
        'B', 'P', 'S', '1', 0x83u, 0x83u, 0x80u,
        0x80u, 0x81u, 'x', 0x80u,
    };
    size_t output_size = 99u;
    uint8_t *output;
    put_le32(patch + 11u, crc32_compute(source, sizeof(source)));
    put_le32(patch + 15u, crc32_compute((const uint8_t *)"axc", 3u));
    put_le32(patch + 19u, crc32_compute(patch, 19u));
    output = ApplyBps(source, sizeof(source), patch, sizeof(patch), &output_size);
    check(output != NULL && output_size == 3u && memcmp(output, "axc", 3u) == 0,
          "BPS source/target read patch");
    free(output);
    patch[9] = 'y';
    output_size = 99u;
    check(ApplyBps(source, sizeof(source), patch, sizeof(patch), &output_size) == NULL &&
              output_size == 0u,
          "BPS checksum rejection");
}

int main(void) {
    test_strings();
    test_packed_tables();
    test_bps();
    if (failures != 0) {
        fprintf(stderr, "runtime-next util: %d failure(s)\n", failures);
        return 1;
    }
    puts("runtime-next util: PASS");
    return 0;
}
