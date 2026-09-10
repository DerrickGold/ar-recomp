/* Redistributable protocol fixture: no cartridge bytes or original IPL ROM. */
#include "snes/apu.h"
#include "snes/spc.h"
#include "snesrecomp/game/apu_sync.h"
#include <stdio.h>
#include <string.h>

static int failures;
static void check(int ok, const char *what) {
    if (!ok) { fprintf(stderr, "boot protocol: %s\n", what); ++failures; }
}
void RtlApuLock(void) {}
void RtlApuUnlock(void) {}
static void cycles(Apu *apu, unsigned count) {
    for (unsigned i = 0; i < count; ++i) apu_cycle(apu);
}
static int wait_port(Apu *apu, unsigned port, uint8_t value) {
    for (unsigned i = 0; i < 20000; ++i) {
        apu_cycle(apu);
        if (apu->outPorts[port] == value) return 1;
    }
    fprintf(stderr, "timeout: port %u=%02x wanted %02x PC=%04x\n",
            port, apu->outPorts[port], value, apu->spc->pc);
    return 0;
}
static void send(Apu *apu, unsigned port, uint8_t value) {
    apu_schedulePortWrite(apu, (uint8_t)port, value, apu->sampleClock);
    cycles(apu, 8);
}
static int command(Apu *apu, uint8_t token, uint16_t address, int transfer) {
    send(apu, 2, (uint8_t)address);
    send(apu, 3, (uint8_t)(address >> 8));
    send(apu, 1, (uint8_t)transfer);
    send(apu, 0, token);
    return wait_port(apu, 0, token);
}
static int upload(Apu *apu, const uint8_t *data, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        send(apu, 1, data[i]);
        send(apu, 0, (uint8_t)i);
        if (!wait_port(apu, 0, (uint8_t)i)) return 0;
    }
    return 1;
}
int main(void) {
    Apu *apu = apu_init();
    if (!apu) return 1;
    apu_reset(apu);
    apu->auditWritesEnabled = true;
    memset(apu->ram, 0xa5, 0xf0);
    check(wait_port(apu, 1, 0xbb) && apu->outPorts[0] == 0xaa,
          "ready signature");
    check(apu->spc->sp == 0xef, "initial stack");
    for (unsigned i = 0; i < 0xf0; ++i) check(apu->ram[i] == 0, "clear direct page");
    send(apu, 0, 0xcb);
    cycles(apu, 1000);
    check(apu->outPorts[0] == 0xaa, "reject non-CC initial token");
    uint8_t bytes[513];
    for (unsigned i = 0; i < sizeof(bytes); ++i) bytes[i] = (uint8_t)(i * 37u + 11u);
    check(command(apu, 0xcc, 0x027d, 1) && upload(apu, bytes, sizeof(bytes)),
          "upload crosses destination page and counter wrap twice");
    check(memcmp(apu->ram + 0x027d, bytes, sizeof(bytes)) == 0,
          "uploaded bytes exact");
    for (unsigned i = 0x027d; i < 0x027d + sizeof(bytes); ++i)
        check((apu->ramWritten[i >> 3] & (1u << (i & 7u))) != 0, "write provenance");
    /* Original tiny SPC program publishes a marker then idles. */
    const uint8_t program[] = {0x8f, 0x5a, 0xf5, 0x2f, 0xfe};
    check(command(apu, 3, 0x0700, 1) && upload(apu, program, sizeof(program)),
          "second transfer resets its byte counter");
    check(command(apu, 7, 0x0700, 0) && wait_port(apu, 1, 0x5a),
          "zero-length execute command runs uploaded SPC code");
    check(apu->spc->pc >= 0x0703 && apu->spc->pc <= 0x0705,
          "execution leaves IPL");
    /* Re-entering the owned firmware resets DP/SP and republishes readiness. */
    apu->spc->pc = 0xffc0; apu->spc->p = true;
    memset(apu->ram, 0x6b, 0xf0);
    check(wait_port(apu, 1, 0xbb), "return to bootstrap");
    for (unsigned i = 0; i < 0xf0; ++i) check(apu->ram[i] == 0, "reentry clears DP");
    /* Upload can target the MMIO DSP pair, not just flat ARAM. */
    const uint8_t dsp_pair[] = {0x0c, 0x42};
    check(command(apu, 0xcc, 0x00f2, 1) && upload(apu, dsp_pair, 2), "MMIO upload");
    check(apu_cpuRead(apu, 0xf3) == 0x42, "DSP write went through hardware");
    apu_free(apu);
    return failures ? 1 : 0;
}
