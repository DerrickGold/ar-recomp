#ifndef SNESRECOMP_INPUT_H
#define SNESRECOMP_INPUT_H
#include <stdbool.h>
#include <stdint.h>
typedef struct Snes Snes;
typedef struct SaveLoadInfo SaveLoadInfo;

typedef struct SnesInputPort {
    uint32_t packet;
    int32_t pending_x, pending_y;
    uint16_t auto_result;
    uint8_t device, buttons, sensitivity, cursor, sign_x, sign_y;
} SnesInputPort;

void snes_input_reset(Snes *snes);
void snes_input_submit(Snes *snes, unsigned port, unsigned device,
                       unsigned buttons, int32_t dx, int32_t dy);
void snes_input_latch(Snes *snes, bool high);
uint8_t snes_input_read(Snes *snes, unsigned port);
void snes_input_auto_read(Snes *snes);
uint16_t snes_input_auto_result(const Snes *snes, unsigned port);
void snes_input_saveload(Snes *snes, SaveLoadInfo *info);
#endif
