#include "sha256.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int check_vector(const char *message, const uint8_t *data, size_t length,
                        const char *expected_hex) {
    static const char digits[] = "0123456789abcdef";
    uint8_t digest[32];
    char actual_hex[65];
    sha256_compute(data, length, digest);
    for (size_t index = 0; index < sizeof(digest); ++index) {
        actual_hex[index * 2u] = digits[digest[index] >> 4u];
        actual_hex[index * 2u + 1u] = digits[digest[index] & 0x0Fu];
    }
    actual_hex[64] = '\0';
    if (strcmp(actual_hex, expected_hex) == 0) {
        return 0;
    }
    fprintf(stderr, "runtime SHA-256 failed for %s:\n  got  %s\n  want %s\n",
            message, actual_hex, expected_hex);
    return 1;
}

static int check_incremental(void) {
    static const uint8_t value[] = "abc";
    static const uint8_t expected[32] = {
        0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea,
        0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
        0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
        0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad,
    };
    Sha256Context context;
    uint8_t digest[32];
    sha256_init(&context);
    sha256_update(&context, value, 1u);
    sha256_update(&context, value + 1u, 1u);
    sha256_update(&context, value + 2u, 1u);
    sha256_final(&context, digest);
    if (memcmp(digest, expected, sizeof(digest)) == 0) return 0;
    fprintf(stderr, "runtime SHA-256 failed for incremental updates\n");
    return 1;
}

int main(void) {
    static const uint8_t abc[] = "abc";
    static const uint8_t two_blocks[] =
        "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    int failed = 0;
    failed |= check_vector(
        "empty", NULL, 0u,
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    failed |= check_vector(
        "abc", abc, sizeof(abc) - 1u,
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    failed |= check_vector(
        "two-block padding", two_blocks, sizeof(two_blocks) - 1u,
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    failed |= check_incremental();
    return failed;
}
