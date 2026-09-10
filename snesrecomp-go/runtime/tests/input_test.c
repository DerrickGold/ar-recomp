#include "snes/input.h"
#include "snes/snes.h"
#include "snes/saveload.h"
#include "snes/spc.h"
#include "snesrecomp/runner.h"
#include "snesrecomp/game/apu_sync.h"
#include "runner_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

static unsigned failures;
#define CHECK(test) do { if (!(test)) { fprintf(stderr,"input contract line %d: %s\n",__LINE__,#test); ++failures; } } while(0)
static uint32_t read_bits(Snes *snes, unsigned port, unsigned count) {
    uint32_t result = 0;
    for (unsigned i=0;i<count;++i) result = (result << 1) | (snes_read(snes,0x4016u+port)&1u);
    return result;
}
static void latch(Snes *snes) { snes_write(snes,0x804016u,1); snes_writeReg(snes,0x4016u,0); }
static void auto_read(Snes *snes) {
    (void)sr_runner_transition_game_timing(snes, SR_GAME_TIMING_BEGIN_FRAME_SLICE, 0);
}
typedef struct Buffer { SaveLoadInfo base; unsigned offset; uint8_t bytes[64]; } Buffer;
static void transfer(SaveLoadInfo *info, void *data, size_t size) {
    Buffer *b=(Buffer *)info;
    if (size>sizeof(b->bytes)-b->offset) { info->failed=true; return; }
    if(info->saving) memcpy(b->bytes+b->offset,data,size); else memcpy(data,b->bytes+b->offset,size);
    b->offset+=(unsigned)size;
}
void RtlApuLock(void) {}
void RtlApuUnlock(void) {}
typedef struct Snapshot {
    SaveLoadInfo base;
    size_t offset;
    uint8_t bytes[2u * 1024u * 1024u];
} Snapshot;
static void snapshot_transfer(SaveLoadInfo *base, void *data, size_t size) {
    Snapshot *snapshot = (Snapshot *)base;
    if (size > sizeof(snapshot->bytes) - snapshot->offset) {
        base->failed = true;
        return;
    }
    if (base->saving) memcpy(snapshot->bytes + snapshot->offset, data, size);
    else memcpy(data, snapshot->bytes + snapshot->offset, size);
    snapshot->offset += size;
}
static void test_snapshots(Snes *snes) {
    Snapshot *snapshot = calloc(1u, sizeof(*snapshot));
    CHECK(snapshot != NULL);
    if (!snapshot) return;
    size_t old_size = 0;
    for (unsigned version = 11u; version <= 13u; ++version) {
        snes_input_submit(snes, 0, SR_INPUT_DEVICE_NONE, 0, 0, 0);
        snes_input_submit(snes, 0, SR_INPUT_DEVICE_MOUSE, 1, -8, 6);
        latch(snes);
        (void)read_bits(snes, 0, 21);
        snes_input_submit(snes, 0, SR_INPUT_DEVICE_MOUSE, 3, 15, -17);
        SnesInputPort expected = snes->inputPorts[0];
        snes->apu->spc->pc = 0xffc8u;
        snes->apu->romReadable = true;
        snapshot->base = (SaveLoadInfo){.func = snapshot_transfer,
            .saving = true, .portable = version != 11u, .format_version = version};
        snapshot->offset = 0u;
        snes_saveload(snes, &snapshot->base);
        CHECK(!snapshot->base.failed);
        size_t saved_size = snapshot->offset;
        if (version == 12u) old_size = saved_size;
        if (version == 13u) CHECK(saved_size == old_size + 41u);
        snes_input_reset(snes);
        snapshot->base.saving = false;
        snapshot->offset = 0u;
        snes_saveload(snes, &snapshot->base);
        CHECK(!snapshot->base.failed && snapshot->offset == saved_size);
        if (version == 13u) {
            CHECK(memcmp(&expected, &snes->inputPorts[0], sizeof(expected)) == 0);
            CHECK(snes->apu->spc->pc == 0xffc8u);
        } else {
            CHECK(snes->inputPorts[0].device == SR_INPUT_DEVICE_MOUSE);
            CHECK(snes->inputPorts[0].cursor == 0 && snes->inputPorts[0].pending_x == 0);
            CHECK(snes->apu->spc->pc == 0xffc0u); /* old bootstrap migrates */
        }
        for (unsigned hidden = 0; hidden < 2; ++hidden) {
            snes->apu->romReadable = hidden == 0u;
            uint16_t pc = hidden ? 0xffc8u : 0x0500u;
            snes->apu->spc->pc = pc;
            snapshot->base.saving = true;
            snapshot->offset = 0u;
            snes_saveload(snes, &snapshot->base);
            snapshot->base.saving = false;
            snapshot->offset = 0u;
            snes_saveload(snes, &snapshot->base);
            CHECK(!snapshot->base.failed && snes->apu->spc->pc == pc);
        }
    }
    free(snapshot);
}
int main(void) {
    uint8_t *ram=calloc(0x20000u,1u);
    Snes *snes=snes_init(ram);
    CHECK(snes!=NULL);
    if(!snes) return 1;
    snes_reset(snes,true);
    snes->input1_currentState=0x0a55u;
    snes->input2_currentState=0x05aau;
    latch(snes);
    CHECK(read_bits(snes,0,16)==SwapInputBits(0xa55));
    CHECK(read_bits(snes,1,16)==SwapInputBits(0x5aa));
    CHECK(read_bits(snes,0,40)==UINT32_MAX);
    snes_input_submit(snes,0,SR_INPUT_DEVICE_MOUSE,3,5,-9);
    latch(snes);
    CHECK(read_bits(snes,0,32)==0x00c18905u);
    CHECK(read_bits(snes,0,33)==UINT32_MAX);
    latch(snes);
    CHECK(read_bits(snes,0,32)==0x00c18000u); /* zero repeats previous sign */
    snes_input_submit(snes,0,SR_INPUT_DEVICE_MOUSE,1,-4,3);
    snes_writeReg(snes,0x4200,1);
    auto_read(snes);
    CHECK(snes_readReg(snes,0x4218)==0x41u && snes_readReg(snes,0x4219)==0);
    CHECK(read_bits(snes,0,16)==0x0384u);
    CHECK(read_bits(snes,0,1)==1u);
    snes_input_submit(snes,1,SR_INPUT_DEVICE_MOUSE,2,2,4);
    auto_read(snes);
    CHECK(snes_readReg(snes,0x421a)==0x81 && snes_readReg(snes,0x421b)==0);
    CHECK(read_bits(snes,1,16)==0x0402u);
    snes_input_submit(snes,1,SR_INPUT_DEVICE_MOUSE,0,12,9);
    snes_beginVblank(snes); /* beam positioning / scanout is not another read */
    CHECK(snes->inputPorts[1].pending_x==12 && snes->inputPorts[1].cursor==32);
    for(unsigned sensitivity=1;sensitivity<=3;++sensitivity) {
        snes_writeReg(snes,0x4016,1);
        (void)snes_readReg(snes,0x4016);
        snes_input_submit(snes,0,SR_INPUT_DEVICE_MOUSE,0,4,7);
        snes_writeReg(snes,0x4016,0);
        static const uint32_t expected[]={0x00111508u,0x00211c0cu,0x00010704u};
        CHECK(read_bits(snes,0,32)==expected[sensitivity-1u]);
    }
    snes_input_submit(snes,0,SR_INPUT_DEVICE_MOUSE,0,INT32_MAX,INT32_MIN);
    snes_input_submit(snes,0,SR_INPUT_DEVICE_MOUSE,0,100,-100);
    CHECK(snes->inputPorts[0].pending_x==INT32_MAX && snes->inputPorts[0].pending_y==INT32_MIN);
    latch(snes);
    CHECK(read_bits(snes,0,32)==0x0001ff7fu);
    snes_input_submit(snes,0,SR_INPUT_DEVICE_NONE,0,0,0);
    latch(snes); CHECK(read_bits(snes,0,40)==0);
    snes_input_submit(snes,0,SR_INPUT_DEVICE_MOUSE,0,-7,6);
    latch(snes); (void)read_bits(snes,0,19);
    Buffer buffer={.base={.func=transfer,.saving=true,.portable=true}};
    snes_input_saveload(snes,&buffer.base);
    uint32_t remaining=read_bits(snes,0,13);
    snes_input_reset(snes);
    buffer.offset=0; buffer.base.saving=false;
    snes_input_saveload(snes,&buffer.base);
    CHECK(!buffer.base.failed && read_bits(snes,0,13)==remaining);
    const SnesRunnerApi *api=sr_runner_get_api(SR_RUNNER_ABI_VERSION);
    SrGenerationSnapshot gen={.struct_size=SR_GENERATION_SNAPSHOT_V2_SIZE};
    CHECK(api && (api->capabilities&SR_RUNNER_CAP_INPUT_DEVICES)!=0 && api->struct_size>=SNES_RUNNER_API_INPUT_DEVICES_SIZE);
    CHECK(api->query_generations(sr_runner_handle(snes),&gen)==SR_RESULT_OK);
    SrInputDeviceRequest req={.struct_size=SR_INPUT_DEVICE_REQUEST_V2_SIZE,.lifetime_generation=gen.lifetime_generation,.device=SR_INPUT_DEVICE_MOUSE,.delta_x=4};
    CHECK(api->submit_input_device(sr_runner_handle(snes),&req)==SR_RESULT_OK);
    req.port=2; CHECK(api->submit_input_device(sr_runner_handle(snes),&req)==SR_RESULT_INVALID_ARGUMENT);
    req.port=0; req.lifetime_generation++; CHECK(api->submit_input_device(sr_runner_handle(snes),&req)==SR_RESULT_STALE_VIEW);
    SnesInputPort prior = snes->inputPorts[0];
    req.lifetime_generation = gen.lifetime_generation;
    req.device = SR_INPUT_DEVICE_GAMEPAD;
    CHECK(api->submit_input_device(sr_runner_handle(snes), &req) == SR_RESULT_INVALID_ARGUMENT);
    req.device = SR_INPUT_DEVICE_MOUSE; req.reserved = 1;
    CHECK(api->submit_input_device(sr_runner_handle(snes), &req) == SR_RESULT_INVALID_ARGUMENT);
    req.reserved = 0; req.buttons = 4;
    CHECK(api->submit_input_device(sr_runner_handle(snes), &req) == SR_RESULT_INVALID_ARGUMENT);
    req.buttons = 0; req.struct_size = SR_INPUT_DEVICE_REQUEST_V2_SIZE - 1;
    CHECK(api->submit_input_device(sr_runner_handle(snes), &req) == SR_RESULT_INVALID_ARGUMENT);
    CHECK(memcmp(&prior, &snes->inputPorts[0], sizeof(prior)) == 0);
    test_snapshots(snes);
    snes_free(snes); free(ram); return failures?1:0;
}
