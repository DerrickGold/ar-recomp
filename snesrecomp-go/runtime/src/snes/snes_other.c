#include "rom.h"

#include "snes.h"
#include "cart.h"

#include <limits.h>
#include <stdio.h>

bool snes_loadRom(Snes *snes, const uint8_t *data, int length) {
    if (snes == NULL || snes->cart == NULL || data == NULL || length < 0) {
        return false;
    }

    SrRomImage image;
    const SrRomStatus status = sr_rom_prepare(data, (size_t)length, &image);
    if (status != SR_ROM_OK) {
        fprintf(stderr, "Failed to load ROM: %s\n", sr_rom_status_string(status));
        return false;
    }

    cart_load(snes->cart, (int)image.info.mapping, image.data,
              (int)image.size, (int)image.info.ram_size);
    const bool loaded = snes->cart->rom != NULL &&
                        snes->cart->romSize == (uint32_t)image.size;
    sr_rom_release(&image);
    return loaded;
}
