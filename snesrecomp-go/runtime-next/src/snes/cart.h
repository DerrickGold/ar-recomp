#ifndef CART_H
#define CART_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Snes Snes;
typedef struct Cart Cart;
typedef struct SaveLoadInfo SaveLoadInfo;

/* Public layout retained for generated/runtime ABI compatibility. */
struct Cart {
    Snes *snes;
    uint8_t type;
    uint8_t *rom;
    uint32_t romSize;
    uint8_t *ram;
    uint32_t ramSize;
};

Cart *cart_init(Snes *snes);
void cart_free(Cart *cart);
void cart_reset(Cart *cart);
void cart_load(Cart *cart, int type, uint8_t *rom, int rom_size, int ram_size);
uint8_t cart_read(Cart *cart, uint8_t bank, uint16_t address);
void cart_write(Cart *cart, uint8_t bank, uint16_t address, uint8_t value);
void cart_saveload(Cart *cart, SaveLoadInfo *info);

#ifdef __cplusplus
}
#endif

#endif
