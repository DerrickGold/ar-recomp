#include "cpu.h"
#include "saveload.h"

#include <stddef.h>
#include <stdlib.h>

Cpu *cpu_init(void) {
    return (Cpu *)calloc(1u, sizeof(Cpu));
}

void cpu_free(Cpu *cpu) {
    free(cpu);
}

void cpu_reset(Cpu *cpu) {
    if (cpu == NULL) {
        return;
    }
    cpu->a = 0u;
    cpu->x = 0u;
    cpu->y = 0u;
    cpu->sp = 0x0100u;
    cpu->pc = 0u;
    cpu->dp = 0u;
    cpu->k = 0u;
    cpu->db = 0u;
    cpu->c = false;
    cpu->z = false;
    cpu->v = false;
    cpu->n = false;
    cpu->i = true;
    cpu->d = false;
    cpu->xf = true;
    cpu->mf = true;
    cpu->e = true;
}

uint8_t cpu_getFlags(Cpu *cpu) {
    if (cpu == NULL) {
        return 0u;
    }
    return (uint8_t)((cpu->n ? 0x80u : 0u) |
                     (cpu->v ? 0x40u : 0u) |
                     (cpu->mf ? 0x20u : 0u) |
                     (cpu->xf ? 0x10u : 0u) |
                     (cpu->d ? 0x08u : 0u) |
                     (cpu->i ? 0x04u : 0u) |
                     (cpu->z ? 0x02u : 0u) |
                     (cpu->c ? 0x01u : 0u));
}

void cpu_setFlags(Cpu *cpu, uint8_t value) {
    if (cpu == NULL) {
        return;
    }
    cpu->n = (value & 0x80u) != 0u;
    cpu->v = (value & 0x40u) != 0u;
    cpu->mf = (value & 0x20u) != 0u;
    cpu->xf = (value & 0x10u) != 0u;
    cpu->d = (value & 0x08u) != 0u;
    cpu->i = (value & 0x04u) != 0u;
    cpu->z = (value & 0x02u) != 0u;
    cpu->c = (value & 0x01u) != 0u;

    if (cpu->e) {
        cpu->mf = true;
        cpu->xf = true;
        cpu->sp = (uint16_t)((cpu->sp & 0x00FFu) | 0x0100u);
    }
    if (cpu->xf) {
        cpu->x &= 0x00FFu;
        cpu->y &= 0x00FFu;
    }
}

void cpu_saveload(Cpu *cpu, SaveLoadInfo *info) {
    if (cpu != NULL && info != NULL && info->func != NULL) {
        info->func(info, &cpu->a, sizeof(*cpu) - offsetof(Cpu, a));
    }
}
