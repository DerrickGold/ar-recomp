#include "keybinds.h"

#include <stdio.h>
#include <string.h>

#ifndef KEYBINDS_TEST_EXE
#define KEYBINDS_TEST_EXE "runtime-test-runner"
#endif
#ifndef KEYBINDS_TEST_INI
#define KEYBINDS_TEST_INI "keybinds.ini"
#endif

static int failures;

static void check(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "runtime keybinds failed: %s\n", message);
        ++failures;
    }
}

int main(void) {
    uint8_t keyboard[512] = {0};
    FILE *file = fopen(KEYBINDS_TEST_INI, "wb");
    const KeyBinds *bindings;
    if (file == NULL) {
        return 1;
    }
    fputs("[player1]\nA = Q\nup = None\nstart = F12\n"
          "[player2]\nb = 1\nright = Left Shift\n",
          file);
    fclose(file);
    keybinds_init(KEYBINDS_TEST_EXE);
    bindings = keybinds_get();
    check(bindings->p1.a == 20 && bindings->p1.up == 0 &&
              bindings->p1.start == 69,
          "INI scancode parsing");
    check(bindings->p2.b == 30 && bindings->p2.right == 225,
          "second-player parsing");
    keyboard[20] = 1u;
    keyboard[69] = 1u;
    check(keybinds_read_player(keyboard, 1) == ((1u << 3) | (1u << 8)),
          "player-one SNES mask");
    memset(keyboard, 0, sizeof(keyboard));
    keyboard[30] = 1u;
    keyboard[225] = 1u;
    check(keybinds_read_player(keyboard, 2) == ((1u << 11) | (1u << 4)),
          "player-two SNES mask");
    check(keybinds_read_player(keyboard, 0) == 0u &&
              keybinds_read_player(NULL, 1) == 0u,
          "invalid input rejection");
    remove(KEYBINDS_TEST_INI);
    keybinds_init(KEYBINDS_TEST_EXE);
    bindings = keybinds_get();
    check(bindings->p1.a == 27 && bindings->p1.b == 29 &&
              bindings->p2.a == 0,
          "default bindings and reset");
    remove(KEYBINDS_TEST_INI);
    return failures == 0 ? 0 : 1;
}
