#include "spc.h"

#include "apu.h"
#include "saveload.h"

#include <stddef.h>
#include <stdlib.h>

enum {
    F_C = 1u << 0, F_Z = 1u << 1, F_I = 1u << 2, F_H = 1u << 3,
    F_B = 1u << 4, F_P = 1u << 5, F_V = 1u << 6, F_N = 1u << 7
};

static const uint8_t k_opcode_cycles[256] = {
    2,8,4,5,3,4,3,6,2,6,5,4,5,4,6,8, 2,8,4,5,4,5,5,6,5,5,6,5,2,2,4,6,
    2,8,4,5,3,4,3,6,2,6,5,4,5,4,5,4, 2,8,4,5,4,5,5,6,5,5,6,5,2,2,3,8,
    2,8,4,5,3,4,3,6,2,6,4,4,5,4,6,6, 2,8,4,5,4,5,5,6,5,5,5,5,2,2,4,3,
    2,8,4,5,3,4,3,6,2,6,5,4,5,4,6,5, 2,8,4,5,4,5,5,6,5,5,5,5,2,2,3,6,
    2,8,4,5,3,4,3,6,2,6,5,4,5,2,4,5, 2,8,4,5,4,5,5,6,5,5,5,5,2,2,12,5,
    3,8,4,5,3,4,3,6,2,6,4,4,5,2,4,4, 2,8,4,5,4,5,5,6,5,5,5,5,2,2,3,4,
    3,8,4,5,4,5,4,7,2,5,6,4,5,2,4,9, 2,8,4,5,5,6,6,7,4,5,5,5,2,2,6,3,
    2,8,4,5,3,4,3,6,2,4,5,3,4,3,4,3, 2,8,4,5,4,5,5,6,3,4,5,4,2,2,4,3,
};

void (*g_spc_opcode_trace_hook)(Spc *, uint16_t);
void (*g_spc_opcode_patch_hook)(Spc *, uint16_t);
int (*g_spc_opcode_cycle_hook)(Spc *, uint16_t, int);

static uint8_t read8(Spc *spc, uint16_t address) {
    return apu_cpuRead(spc->apu, address);
}
static void write8(Spc *spc, uint16_t address, uint8_t value) {
    apu_cpuWrite(spc->apu, address, value);
}
static uint8_t fetch8(Spc *spc) { return read8(spc, spc->pc++); }
static uint16_t fetch16(Spc *spc) {
    const uint16_t low = fetch8(spc);
    return (uint16_t)(low | ((uint16_t)fetch8(spc) << 8));
}
static uint16_t direct(Spc *spc, uint8_t value) {
    return (uint16_t)(value | (spc->p ? 0x100u : 0u));
}
static uint16_t direct_next(Spc *spc, uint8_t value) {
    return direct(spc, (uint8_t)(value + 1u));
}
static uint16_t direct_pointer(Spc *spc, uint8_t value) {
    return (uint16_t)(read8(spc, direct(spc, value)) |
        ((uint16_t)read8(spc, direct_next(spc, value)) << 8));
}
static void push(Spc *spc, uint8_t value) {
    write8(spc, (uint16_t)(0x100u | spc->sp), value);
    --spc->sp;
}
static uint8_t pop(Spc *spc) {
    ++spc->sp;
    return read8(spc, (uint16_t)(0x100u | spc->sp));
}
static void push_pc(Spc *spc) {
    push(spc, (uint8_t)(spc->pc >> 8));
    push(spc, (uint8_t)spc->pc);
}
static void pop_pc(Spc *spc) {
    spc->pc = (uint16_t)(pop(spc) | ((uint16_t)pop(spc) << 8));
}
static uint8_t flags(const Spc *spc) {
    return (uint8_t)((spc->c ? F_C : 0u) | (spc->z ? F_Z : 0u) |
        (spc->i ? F_I : 0u) | (spc->h ? F_H : 0u) |
        (spc->b ? F_B : 0u) | (spc->p ? F_P : 0u) |
        (spc->v ? F_V : 0u) | (spc->n ? F_N : 0u));
}
static void set_flags(Spc *spc, uint8_t value) {
    spc->c = (value & F_C) != 0u; spc->z = (value & F_Z) != 0u;
    spc->i = (value & F_I) != 0u; spc->h = (value & F_H) != 0u;
    spc->b = (value & F_B) != 0u; spc->p = (value & F_P) != 0u;
    spc->v = (value & F_V) != 0u; spc->n = (value & F_N) != 0u;
}
static uint8_t nz8(Spc *spc, uint8_t value) {
    spc->z = value == 0u; spc->n = (value & 0x80u) != 0u; return value;
}
static uint16_t nz16(Spc *spc, uint16_t value) {
    spc->z = value == 0u; spc->n = (value & 0x8000u) != 0u; return value;
}
static uint8_t adc8(Spc *spc, uint8_t left, uint8_t right) {
    const int carry = spc->c ? 1 : 0;
    const int result = left + right + carry;
    spc->c = result > 0xff;
    spc->h = (left & 0x0f) + (right & 0x0f) + carry > 0x0f;
    spc->v = ((~(left ^ right) & (left ^ result) & 0x80) != 0);
    return nz8(spc, (uint8_t)result);
}
static uint8_t sbc8(Spc *spc, uint8_t left, uint8_t right) {
    const int borrow = spc->c ? 0 : 1;
    const int result = left - right - borrow;
    spc->c = result >= 0;
    spc->h = (int)(left & 0x0f) - (int)(right & 0x0f) - borrow >= 0;
    spc->v = (((left ^ right) & (left ^ result) & 0x80) != 0);
    return nz8(spc, (uint8_t)result);
}
static void compare8(Spc *spc, uint8_t left, uint8_t right) {
    const int result = left - right;
    spc->c = result >= 0; (void)nz8(spc, (uint8_t)result);
}
static uint8_t alu(Spc *spc, unsigned operation, uint8_t left, uint8_t right) {
    switch (operation) {
        case 0: return nz8(spc, (uint8_t)(left | right));
        case 1: return nz8(spc, (uint8_t)(left & right));
        case 2: return nz8(spc, (uint8_t)(left ^ right));
        case 3: compare8(spc, left, right); return left;
        case 4: return adc8(spc, left, right);
        default: return sbc8(spc, left, right);
    }
}
static int branch(Spc *spc, bool condition) {
    const int8_t offset = (int8_t)fetch8(spc);
    if (!condition) return 0;
    spc->pc = (uint16_t)(spc->pc + offset);
    return 2;
}
static uint8_t memory_bit(Spc *spc, uint16_t *address) {
    const uint16_t operand = fetch16(spc);
    *address = (uint16_t)(operand & 0x1fffu);
    return (uint8_t)(1u << (operand >> 13));
}
static uint8_t shift_value(Spc *spc, uint8_t value, unsigned kind) {
    uint8_t result;
    if (kind == 0u) {
        spc->c = (value & 0x80u) != 0u; result = (uint8_t)(value << 1);
    } else if (kind == 1u) {
        const uint8_t carry = spc->c ? 1u : 0u;
        spc->c = (value & 0x80u) != 0u; result = (uint8_t)((value << 1) | carry);
    } else if (kind == 2u) {
        spc->c = (value & 1u) != 0u; result = (uint8_t)(value >> 1);
    } else {
        const uint8_t carry = spc->c ? 0x80u : 0u;
        spc->c = (value & 1u) != 0u; result = (uint8_t)((value >> 1) | carry);
    }
    return nz8(spc, result);
}
static void shift_memory(Spc *spc, uint16_t address, unsigned kind) {
    write8(spc, address, shift_value(spc, read8(spc, address), kind));
}

static bool regular_alu(Spc *spc, uint8_t opcode) {
    const unsigned operation = opcode >> 5;
    const uint8_t mode = (uint8_t)(opcode & 0x1fu);
    uint16_t address = 0u;
    uint8_t right = 0u;
    if (operation > 5u) return false;
    switch (mode) {
        case 0x04: address = direct(spc, fetch8(spc)); right = read8(spc, address); break;
        case 0x05: address = fetch16(spc); right = read8(spc, address); break;
        case 0x06: right = read8(spc, direct(spc, spc->x)); break;
        case 0x07: address = direct_pointer(spc, (uint8_t)(fetch8(spc) + spc->x)); right = read8(spc, address); break;
        case 0x08: right = fetch8(spc); break;
        case 0x14: address = direct(spc, (uint8_t)(fetch8(spc) + spc->x)); right = read8(spc, address); break;
        case 0x15: address = (uint16_t)(fetch16(spc) + spc->x); right = read8(spc, address); break;
        case 0x16: address = (uint16_t)(fetch16(spc) + spc->y); right = read8(spc, address); break;
        case 0x17: address = (uint16_t)(direct_pointer(spc, fetch8(spc)) + spc->y); right = read8(spc, address); break;
        case 0x09: {
            const uint8_t source = read8(spc, direct(spc, fetch8(spc)));
            const uint16_t destination = direct(spc, fetch8(spc));
            const uint8_t value = alu(spc, operation, read8(spc, destination), source);
            if (operation != 3u) write8(spc, destination, value);
            return true;
        }
        case 0x18: {
            const uint8_t immediate = fetch8(spc);
            const uint16_t destination = direct(spc, fetch8(spc));
            const uint8_t value = alu(spc, operation, read8(spc, destination), immediate);
            if (operation != 3u) write8(spc, destination, value);
            return true;
        }
        case 0x19: {
            const uint16_t destination = direct(spc, spc->x);
            const uint8_t value = alu(spc, operation, read8(spc, destination),
                                      read8(spc, direct(spc, spc->y)));
            if (operation != 3u) write8(spc, destination, value);
            return true;
        }
        default: return false;
    }
    spc->a = alu(spc, operation, spc->a, right);
    return true;
}

Spc *spc_init(Apu *apu) {
    Spc *spc = (Spc *)calloc(1u, sizeof(*spc));
    if (spc != NULL) spc->apu = apu;
    return spc;
}
void spc_free(Spc *spc) { free(spc); }
void spc_reset(Spc *spc) {
    if (spc == NULL) return;
    spc->a = spc->x = spc->y = spc->sp = 0u;
    spc->c = spc->z = spc->v = spc->n = false;
    spc->i = spc->h = spc->p = spc->b = false;
    spc->stopped = false; spc->cyclesUsed = 0u;
    spc->pc = (uint16_t)(read8(spc, 0xfffeu) | ((uint16_t)read8(spc, 0xffffu) << 8));
}
void spc_saveload(Spc *spc, SaveLoadInfo *info) {
    if (spc == NULL || info == NULL || info->func == NULL) return;
    if (!info->portable) {
        info->func(info, &spc->a, offsetof(Spc, cyclesUsed) - offsetof(Spc, a));
        return;
    }
    saveload_u8(info, &spc->a);
    saveload_u8(info, &spc->x);
    saveload_u8(info, &spc->y);
    saveload_u8(info, &spc->sp);
    saveload_u16(info, &spc->pc);
    saveload_bool(info, &spc->c);
    saveload_bool(info, &spc->z);
    saveload_bool(info, &spc->v);
    saveload_bool(info, &spc->n);
    saveload_bool(info, &spc->i);
    saveload_bool(info, &spc->h);
    saveload_bool(info, &spc->p);
    saveload_bool(info, &spc->b);
    saveload_bool(info, &spc->stopped);
}

int spc_runOpcode(Spc *spc) {
    if (spc == NULL) return 1;
    spc->cyclesUsed = 0u;
    if (spc->stopped) return 1;
    const uint16_t opcode_pc = spc->pc;
    if (g_spc_opcode_trace_hook != NULL) g_spc_opcode_trace_hook(spc, opcode_pc);
    if (g_spc_opcode_patch_hook != NULL) g_spc_opcode_patch_hook(spc, opcode_pc);
    const uint8_t opcode = fetch8(spc);
    int extra = 0;
    if (!regular_alu(spc, opcode)) {
        uint16_t address, word, right, left;
        uint8_t value, bit, dp;
        switch (opcode) {
            case 0x00: break;
            case 0x01: case 0x11: case 0x21: case 0x31: case 0x41: case 0x51:
            case 0x61: case 0x71: case 0x81: case 0x91: case 0xa1: case 0xb1:
            case 0xc1: case 0xd1: case 0xe1: case 0xf1:
                push_pc(spc); address = (uint16_t)(0xffdeu - ((opcode >> 4) * 2u));
                spc->pc = (uint16_t)(read8(spc, address) | ((uint16_t)read8(spc, (uint16_t)(address + 1u)) << 8)); break;
            case 0x02: case 0x22: case 0x42: case 0x62: case 0x82: case 0xa2: case 0xc2: case 0xe2:
                address = direct(spc, fetch8(spc)); write8(spc, address, (uint8_t)(read8(spc, address) | (1u << (opcode >> 5)))); break;
            case 0x12: case 0x32: case 0x52: case 0x72: case 0x92: case 0xb2: case 0xd2: case 0xf2:
                address = direct(spc, fetch8(spc)); write8(spc, address, (uint8_t)(read8(spc, address) & ~(1u << (opcode >> 5)))); break;
            case 0x03: case 0x23: case 0x43: case 0x63: case 0x83: case 0xa3: case 0xc3: case 0xe3:
                value = read8(spc, direct(spc, fetch8(spc))); extra = branch(spc, (value & (1u << (opcode >> 5))) != 0u); break;
            case 0x13: case 0x33: case 0x53: case 0x73: case 0x93: case 0xb3: case 0xd3: case 0xf3:
                value = read8(spc, direct(spc, fetch8(spc))); extra = branch(spc, (value & (1u << (opcode >> 5))) == 0u); break;
            case 0x0a: address = 0; bit = memory_bit(spc, &address); spc->c = spc->c || (read8(spc, address) & bit) != 0u; break;
            case 0x2a: address = 0; bit = memory_bit(spc, &address); spc->c = spc->c || (read8(spc, address) & bit) == 0u; break;
            case 0x4a: address = 0; bit = memory_bit(spc, &address); spc->c = spc->c && (read8(spc, address) & bit) != 0u; break;
            case 0x6a: address = 0; bit = memory_bit(spc, &address); spc->c = spc->c && (read8(spc, address) & bit) == 0u; break;
            case 0x8a: address = 0; bit = memory_bit(spc, &address); spc->c = spc->c != ((read8(spc, address) & bit) != 0u); break;
            case 0xaa: address = 0; bit = memory_bit(spc, &address); spc->c = (read8(spc, address) & bit) != 0u; break;
            case 0xca: address = 0; bit = memory_bit(spc, &address); value = read8(spc, address); value = spc->c ? (uint8_t)(value | bit) : (uint8_t)(value & ~bit); write8(spc, address, value); break;
            case 0xea: address = 0; bit = memory_bit(spc, &address); write8(spc, address, (uint8_t)(read8(spc, address) ^ bit)); break;
            case 0x0b: case 0x2b: case 0x4b: case 0x6b: shift_memory(spc, direct(spc, fetch8(spc)), opcode >> 5); break;
            case 0x0c: case 0x2c: case 0x4c: case 0x6c: shift_memory(spc, fetch16(spc), opcode >> 5); break;
            case 0x1b: case 0x3b: case 0x5b: case 0x7b: shift_memory(spc, direct(spc, (uint8_t)(fetch8(spc) + spc->x)), opcode >> 5); break;
            case 0x1c: case 0x3c: case 0x5c: case 0x7c: spc->a = shift_value(spc, spc->a, opcode >> 5); break;
            case 0x0d: push(spc, flags(spc)); break; case 0x2d: push(spc, spc->a); break;
            case 0x4d: push(spc, spc->x); break; case 0x6d: push(spc, spc->y); break;
            case 0x8e: set_flags(spc, pop(spc)); break; case 0xae: spc->a = pop(spc); break;
            case 0xce: spc->x = pop(spc); break; case 0xee: spc->y = pop(spc); break;
            case 0x0e: case 0x4e:
                address = fetch16(spc); value = read8(spc, address); (void)nz8(spc, (uint8_t)(spc->a - value));
                write8(spc, address, opcode == 0x0e ? (uint8_t)(value | spc->a) : (uint8_t)(value & ~spc->a)); break;
            case 0x0f: push_pc(spc); push(spc, (uint8_t)(flags(spc) | F_B)); spc->b = true; spc->i = true;
                spc->pc = (uint16_t)(read8(spc, 0xffdeu) | ((uint16_t)read8(spc, 0xffdfu) << 8)); break;
            case 0x10: extra = branch(spc, !spc->n); break; case 0x30: extra = branch(spc, spc->n); break;
            case 0x50: extra = branch(spc, !spc->v); break; case 0x70: extra = branch(spc, spc->v); break;
            case 0x90: extra = branch(spc, !spc->c); break; case 0xb0: extra = branch(spc, spc->c); break;
            case 0xd0: extra = branch(spc, !spc->z); break; case 0xf0: extra = branch(spc, spc->z); break;
            case 0x1a: case 0x3a:
                dp = fetch8(spc); word = (uint16_t)(read8(spc, direct(spc, dp)) | ((uint16_t)read8(spc, direct_next(spc, dp)) << 8));
                word = opcode == 0x1a ? (uint16_t)(word - 1u) : (uint16_t)(word + 1u);
                write8(spc, direct(spc, dp), (uint8_t)word); write8(spc, direct_next(spc, dp), (uint8_t)(word >> 8)); (void)nz16(spc, word); break;
            case 0x1d: spc->x = nz8(spc, (uint8_t)(spc->x - 1u)); break; case 0x3d: spc->x = nz8(spc, (uint8_t)(spc->x + 1u)); break;
            case 0xdc: spc->y = nz8(spc, (uint8_t)(spc->y - 1u)); break; case 0xfc: spc->y = nz8(spc, (uint8_t)(spc->y + 1u)); break;
            case 0x9c: spc->a = nz8(spc, (uint8_t)(spc->a - 1u)); break; case 0xbc: spc->a = nz8(spc, (uint8_t)(spc->a + 1u)); break;
            case 0x8b: case 0xab: address = direct(spc, fetch8(spc)); value = read8(spc, address); value = opcode == 0x8b ? (uint8_t)(value - 1u) : (uint8_t)(value + 1u); write8(spc, address, nz8(spc, value)); break;
            case 0x8c: case 0xac: address = fetch16(spc); value = read8(spc, address); value = opcode == 0x8c ? (uint8_t)(value - 1u) : (uint8_t)(value + 1u); write8(spc, address, nz8(spc, value)); break;
            case 0x9b: case 0xbb: address = direct(spc, (uint8_t)(fetch8(spc) + spc->x)); value = read8(spc, address); value = opcode == 0x9b ? (uint8_t)(value - 1u) : (uint8_t)(value + 1u); write8(spc, address, nz8(spc, value)); break;
            case 0x1e: compare8(spc, spc->x, read8(spc, fetch16(spc))); break; case 0x3e: compare8(spc, spc->x, read8(spc, direct(spc, fetch8(spc)))); break;
            case 0x5e: compare8(spc, spc->y, read8(spc, fetch16(spc))); break; case 0x7e: compare8(spc, spc->y, read8(spc, direct(spc, fetch8(spc)))); break;
            case 0xc8: compare8(spc, spc->x, fetch8(spc)); break; case 0xad: compare8(spc, spc->y, fetch8(spc)); break;
            case 0x1f: address = (uint16_t)(fetch16(spc) + spc->x); spc->pc = (uint16_t)(read8(spc, address) | ((uint16_t)read8(spc, (uint16_t)(address + 1u)) << 8)); break;
            case 0x2f: extra = branch(spc, true); break;
            case 0x3f: address = fetch16(spc); push_pc(spc); spc->pc = address; break;
            case 0x4f: address = (uint16_t)(0xff00u | fetch8(spc)); push_pc(spc); spc->pc = address; break;
            case 0x5f: spc->pc = fetch16(spc); break; case 0x6f: pop_pc(spc); break;
            case 0x7f: set_flags(spc, pop(spc)); pop_pc(spc); break;
            case 0x20: spc->p = false; break; case 0x40: spc->p = true; break;
            case 0x60: spc->c = false; break; case 0x80: spc->c = true; break;
            case 0xa0: spc->i = true; break; case 0xc0: spc->i = false; break;
            case 0xe0: spc->v = false; spc->h = false; break; case 0xed: spc->c = !spc->c; break;
            case 0x2e: case 0xde: dp = fetch8(spc); address = direct(spc, opcode == 0xde ? (uint8_t)(dp + spc->x) : dp); extra = branch(spc, spc->a != read8(spc, address)); break;
            case 0x6e: address = direct(spc, fetch8(spc)); value = (uint8_t)(read8(spc, address) - 1u); write8(spc, address, value); extra = branch(spc, value != 0u); break;
            case 0xfe: --spc->y; extra = branch(spc, spc->y != 0u); break;
            case 0x5a: case 0x7a: case 0x9a: case 0xba:
                dp = fetch8(spc); right = (uint16_t)(read8(spc, direct(spc, dp)) | ((uint16_t)read8(spc, direct_next(spc, dp)) << 8)); left = (uint16_t)(spc->a | ((uint16_t)spc->y << 8));
                if (opcode == 0x5a) { const int result = (int)left - (int)right; spc->c = result >= 0; (void)nz16(spc, (uint16_t)result); }
                else if (opcode == 0x7a) { const uint32_t result = (uint32_t)left + right; spc->c = result > 0xffffu; spc->h = (left & 0x0fffu) + (right & 0x0fffu) > 0x0fffu; spc->v = ((~(left ^ right) & (left ^ result) & 0x8000u) != 0u); word = nz16(spc, (uint16_t)result); spc->a = (uint8_t)word; spc->y = (uint8_t)(word >> 8); }
                else if (opcode == 0x9a) { const int32_t result = (int32_t)left - right; spc->c = result >= 0; spc->h = (int)(left & 0x0fffu) - (int)(right & 0x0fffu) >= 0; spc->v = (((left ^ right) & (left ^ result) & 0x8000u) != 0u); word = nz16(spc, (uint16_t)result); spc->a = (uint8_t)word; spc->y = (uint8_t)(word >> 8); }
                else { spc->a = (uint8_t)right; spc->y = (uint8_t)(right >> 8); (void)nz16(spc, right); } break;
            case 0xda: dp = fetch8(spc); write8(spc, direct(spc, dp), spc->a); write8(spc, direct_next(spc, dp), spc->y); break;
            case 0x5d: spc->x = nz8(spc, spc->a); break; case 0x7d: spc->a = nz8(spc, spc->x); break;
            case 0x9d: spc->x = nz8(spc, spc->sp); break; case 0xbd: spc->sp = spc->x; break;
            case 0xdd: spc->a = nz8(spc, spc->y); break; case 0xfd: spc->y = nz8(spc, spc->a); break;
            case 0x8d: spc->y = nz8(spc, fetch8(spc)); break; case 0xcd: spc->x = nz8(spc, fetch8(spc)); break; case 0xe8: spc->a = nz8(spc, fetch8(spc)); break;
            case 0xc4: write8(spc, direct(spc, fetch8(spc)), spc->a); break; case 0xc5: write8(spc, fetch16(spc), spc->a); break; case 0xc6: write8(spc, direct(spc, spc->x), spc->a); break;
            case 0xc7: write8(spc, direct_pointer(spc, (uint8_t)(fetch8(spc) + spc->x)), spc->a); break; case 0xd4: write8(spc, direct(spc, (uint8_t)(fetch8(spc) + spc->x)), spc->a); break;
            case 0xd5: write8(spc, (uint16_t)(fetch16(spc) + spc->x), spc->a); break; case 0xd6: write8(spc, (uint16_t)(fetch16(spc) + spc->y), spc->a); break;
            case 0xd7: write8(spc, (uint16_t)(direct_pointer(spc, fetch8(spc)) + spc->y), spc->a); break;
            case 0xd8: write8(spc, direct(spc, fetch8(spc)), spc->x); break; case 0xd9: write8(spc, direct(spc, (uint8_t)(fetch8(spc) + spc->y)), spc->x); break;
            case 0xcb: write8(spc, direct(spc, fetch8(spc)), spc->y); break; case 0xdb: write8(spc, direct(spc, (uint8_t)(fetch8(spc) + spc->x)), spc->y); break;
            case 0xc9: write8(spc, fetch16(spc), spc->x); break; case 0xcc: write8(spc, fetch16(spc), spc->y); break;
            case 0xe4: spc->a = nz8(spc, read8(spc, direct(spc, fetch8(spc)))); break; case 0xe5: spc->a = nz8(spc, read8(spc, fetch16(spc))); break; case 0xe6: spc->a = nz8(spc, read8(spc, direct(spc, spc->x))); break;
            case 0xe7: spc->a = nz8(spc, read8(spc, direct_pointer(spc, (uint8_t)(fetch8(spc) + spc->x)))); break; case 0xf4: spc->a = nz8(spc, read8(spc, direct(spc, (uint8_t)(fetch8(spc) + spc->x)))); break;
            case 0xf5: spc->a = nz8(spc, read8(spc, (uint16_t)(fetch16(spc) + spc->x))); break; case 0xf6: spc->a = nz8(spc, read8(spc, (uint16_t)(fetch16(spc) + spc->y))); break;
            case 0xf7: spc->a = nz8(spc, read8(spc, (uint16_t)(direct_pointer(spc, fetch8(spc)) + spc->y))); break;
            case 0xe9: spc->x = nz8(spc, read8(spc, fetch16(spc))); break; case 0xf8: spc->x = nz8(spc, read8(spc, direct(spc, fetch8(spc)))); break;
            case 0xf9: spc->x = nz8(spc, read8(spc, direct(spc, (uint8_t)(fetch8(spc) + spc->y)))); break; case 0xeb: spc->y = nz8(spc, read8(spc, direct(spc, fetch8(spc)))); break;
            case 0xec: spc->y = nz8(spc, read8(spc, fetch16(spc))); break; case 0xfb: spc->y = nz8(spc, read8(spc, direct(spc, (uint8_t)(fetch8(spc) + spc->x)))); break;
            case 0x8f: value = fetch8(spc); write8(spc, direct(spc, fetch8(spc)), value); break;
            case 0xfa: value = read8(spc, direct(spc, fetch8(spc))); write8(spc, direct(spc, fetch8(spc)), value); break;
            case 0xaf: write8(spc, direct(spc, spc->x), spc->a); ++spc->x; break; case 0xbf: spc->a = nz8(spc, read8(spc, direct(spc, spc->x))); ++spc->x; break;
            case 0x9e: {
                const uint16_t ya = (uint16_t)(spc->a | ((uint16_t)spc->y << 8)); const uint16_t divisor = spc->x;
                spc->h = (spc->y & 0x0fu) >= (spc->x & 0x0fu); spc->v = spc->y >= divisor;
                if (divisor == 0u) { spc->a = 0xffu; spc->y = (uint8_t)ya; }
                else if (spc->y < divisor * 2u) { spc->a = (uint8_t)(ya / divisor); spc->y = (uint8_t)(ya % divisor); }
                else { const uint16_t denominator = (uint16_t)(256u - divisor); const uint16_t numerator = (uint16_t)(ya - divisor * 256u); spc->a = (uint8_t)(255u - numerator / denominator); spc->y = (uint8_t)(divisor + numerator % denominator); }
                (void)nz8(spc, spc->a); break;
            }
            case 0x9f: spc->a = nz8(spc, (uint8_t)((spc->a << 4) | (spc->a >> 4))); break;
            case 0xbe: if (!spc->c || spc->a > 0x99u) { spc->a = (uint8_t)(spc->a - 0x60u); spc->c = false; } if (!spc->h || (spc->a & 0x0fu) > 9u) spc->a = (uint8_t)(spc->a - 6u); (void)nz8(spc, spc->a); break;
            case 0xdf: if (spc->c || spc->a > 0x99u) { spc->a = (uint8_t)(spc->a + 0x60u); spc->c = true; } if (spc->h || (spc->a & 0x0fu) > 9u) spc->a = (uint8_t)(spc->a + 6u); (void)nz8(spc, spc->a); break;
            case 0xcf: word = (uint16_t)(spc->a * spc->y); spc->a = (uint8_t)word; spc->y = nz8(spc, (uint8_t)(word >> 8)); break;
            case 0xef: case 0xff: spc->stopped = true; break;
            default: break;
        }
    }
    int cycles = (int)k_opcode_cycles[opcode] + extra;
    if (g_spc_opcode_cycle_hook != NULL) cycles = g_spc_opcode_cycle_hook(spc, opcode_pc, cycles);
    if (cycles < 0) cycles = 0;
    spc->cyclesUsed = (uint8_t)cycles;
    return cycles;
}
