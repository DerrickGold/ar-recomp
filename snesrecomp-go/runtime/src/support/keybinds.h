#ifndef SNESRECOMP_KEYBINDS_H
#define SNESRECOMP_KEYBINDS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* SDL scancodes use the USB HID usage values represented here as integers.
 * Keeping the persisted binding model independent of SDL makes it usable by
 * other host frontends without changing the game-facing ABI. */
typedef struct PlayerBinds {
    int a;
    int b;
    int x;
    int y;
    int l;
    int r;
    int start;
    int select;
    int up;
    int down;
    int left;
    int right;
} PlayerBinds;

typedef struct KeyBinds {
    PlayerBinds p1;
    PlayerBinds p2;
} KeyBinds;

void keybinds_init(const char *executable_path);
const KeyBinds *keybinds_get(void);
uint16_t keybinds_read_player(const uint8_t *keyboard_state, int player);

#ifdef __cplusplus
}
#endif

#endif
